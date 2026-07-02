#include "duckdb.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/file_open_flags.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/materialized_query_result.hpp"

#include "idoc_functions.hpp"
#include "idoc_format.hpp"
#include "idoc_dict_source.hpp"
#include "idoc_doc.hpp"
#include "telemetry.hpp"

#include <cctype>
#include <map>

namespace duckdb {

using erpl_idoc::EDI_DD40_FIELDS;
using erpl_idoc::Framing;
using erpl_idoc::GetFieldRaw;
using erpl_idoc::ParsedIdoc;
using erpl_idoc::RTrim;

// Shared file read (kept local to avoid cross-TU coupling).
static std::string TypedReadWholeFile(ClientContext &context, const std::string &path) {
	auto &fs = FileSystem::GetFileSystem(context);
	auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ);
	auto size = static_cast<idx_t>(handle->GetFileSize());
	std::string buffer;
	buffer.resize(size);
	idx_t total = 0;
	while (total < size) {
		auto n = handle->Read(reinterpret_cast<void *>(&buffer[total]), size - total);
		if (n <= 0) {
			throw IOException("short read on '%s': got %llu of %llu bytes", path, (unsigned long long)total,
			                  (unsigned long long)size);
		}
		total += static_cast<idx_t>(n);
	}
	return buffer;
}

// One field's slicing rule taken from the dictionary.
struct TypedFieldRule {
	std::string name;
	int64_t offset;
	int64_t length;
	std::string datatype;
	int64_t field_pos = 0; // used by sap_idoc_read_fields (long form)
};

struct ReadSegmentBindData : public TableFunctionData {
	std::string path;
	std::string segnam;
	bool has_framing_override = false;
	Framing framing_override = Framing::FIXED;
	bool lenient = false;
	std::string encoding = "utf-8";
	vector<TypedFieldRule> fields;
};

struct ReadSegmentGlobalState : public GlobalTableFunctionState {
	ParsedIdoc parsed;
	idx_t cursor = 0;
	idx_t MaxThreads() const override {
		return 1;
	}
};

static unique_ptr<FunctionData> ReadSegmentBind(ClientContext &context, TableFunctionBindInput &input,
                                                vector<LogicalType> &return_types, vector<string> &names) {
	auto bind = make_uniq<ReadSegmentBindData>();
	bind->path = input.inputs[0].GetValue<string>();
	bind->segnam = input.inputs[1].GetValue<string>();
	auto dict = input.inputs[2].GetValue<string>();
	PostHogTelemetry::Instance().CaptureFunctionExecution("sap_idoc_read_segment");

	auto &np = input.named_parameters;
	if (np.count("framing") && !np["framing"].IsNull()) {
		bind->has_framing_override = true;
		bind->framing_override = erpl_idoc::FramingFromString(np["framing"].GetValue<string>());
	}
	if (np.count("lenient") && !np["lenient"].IsNull()) {
		bind->lenient = np["lenient"].GetValue<bool>();
	}
	if (np.count("encoding") && !np["encoding"].IsNull()) {
		bind->encoding = np["encoding"].GetValue<string>();
	}

	// Load the slicing rules for this segment from the dictionary relation.
	auto query = "SELECT field_name, \"offset\", length, datatype FROM " + DictSource(dict) +
	             " WHERE segnam = '" + DictSqlEscape(bind->segnam) + "' ORDER BY field_pos";
	Connection con(*context.db);
	auto result = con.Query(query);
	if (result->HasError()) {
		throw BinderException("sap_idoc_read_segment: failed to read dictionary: " + result->GetError());
	}
	if (result->RowCount() == 0) {
		throw BinderException("sap_idoc_read_segment: dictionary has no fields for segment '%s'", bind->segnam);
	}

	names = {"document_key", "segnum"};
	return_types = {LogicalType::BIGINT, LogicalType::VARCHAR};
	for (idx_t i = 0; i < result->RowCount(); i++) {
		TypedFieldRule rule;
		rule.name = result->GetValue(0, i).ToString();
		rule.offset = result->GetValue(1, i).GetValue<int64_t>();
		rule.length = result->GetValue(2, i).GetValue<int64_t>();
		auto dt = result->GetValue(3, i);
		rule.datatype = dt.IsNull() ? "" : dt.ToString();
		if (rule.offset < 0 || rule.length < 0 || rule.offset > static_cast<int64_t>(erpl_idoc::SDATA_LEN) ||
		    rule.length > static_cast<int64_t>(erpl_idoc::SDATA_LEN) - rule.offset) {
			throw BinderException("sap_idoc_read_segment: field '%s' offset/length out of SDATA bounds", rule.name);
		}
		// lower-case the column name (SAP dict names are upper-case)
		std::string lower;
		for (char c : rule.name) {
			lower += static_cast<char>(tolower(static_cast<unsigned char>(c)));
		}
		names.push_back(lower);
		return_types.push_back(LogicalType::VARCHAR); // typed values as strings (acceptance #3)
		bind->fields.push_back(std::move(rule));
	}
	return std::move(bind);
}

static unique_ptr<GlobalTableFunctionState> ReadSegmentInitGlobal(ClientContext &context,
                                                                  TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<ReadSegmentBindData>();
	auto data = TypedReadWholeFile(context, bind.path);
	auto state = make_uniq<ReadSegmentGlobalState>();
	if (bind.lenient) {
		state->parsed = erpl_idoc::ParseImageLenient(data);
	} else {
		auto framing = bind.has_framing_override ? bind.framing_override : erpl_idoc::DetectFraming(data);
		state->parsed = erpl_idoc::ParseImage(data, framing);
	}
	return std::move(state);
}

static void ReadSegmentScan(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind = data_p.bind_data->Cast<ReadSegmentBindData>();
	auto &gstate = data_p.global_state->Cast<ReadSegmentGlobalState>();
	auto &records = gstate.parsed.records;
	idx_t out_row = 0;
	while (gstate.cursor < records.size() && out_row < STANDARD_VECTOR_SIZE) {
		auto &rec = records[gstate.cursor++];
		if (rec.is_control) {
			continue;
		}
		if (RTrim(GetFieldRaw(rec.bytes, EDI_DD40_FIELDS[0])) != bind.segnam) { // SEGNAM
			continue;
		}
		auto sdata = GetFieldRaw(rec.bytes, EDI_DD40_FIELDS[6]); // 1000-byte SDATA
		output.SetValue(0, out_row, Value::BIGINT(rec.document_key));
		output.SetValue(1, out_row, Value(RTrim(GetFieldRaw(rec.bytes, EDI_DD40_FIELDS[3])))); // SEGNUM
		for (idx_t i = 0; i < bind.fields.size(); i++) {
			auto &f = bind.fields[i];
			std::string raw = sdata.substr(f.offset, f.length);
			// decode per the source codepage, then trim trailing pad spaces
			output.SetValue(2 + i, out_row, Value(RTrim(erpl_idoc::DecodeText(raw, bind.encoding))));
		}
		out_row++;
	}
	output.SetCardinality(out_row);
}

// --- sap_idoc_read_fields: decode ALL fields of ALL records in one call ---------
// Long form (one row per field), dictionary-driven, byte-correct — the same slicing
// as sap_idoc_read_segment, but across every segment at once and without per-segment
// calls or manual offset math.
struct ReadFieldsBindData : public TableFunctionData {
	std::string path;
	bool has_framing_override = false;
	Framing framing_override = Framing::FIXED;
	bool lenient = false;
	bool include_unknown = true;
	std::string encoding = "utf-8";
	std::map<std::string, vector<TypedFieldRule>> fields_by_seg;
};

struct ReadFieldsGlobalState : public GlobalTableFunctionState {
	ParsedIdoc parsed;
	idx_t cursor = 0;       // record index
	idx_t field_cursor = 0; // field index within the current record (spans chunks)
	idx_t MaxThreads() const override {
		return 1;
	}
};

static unique_ptr<FunctionData> ReadFieldsBind(ClientContext &context, TableFunctionBindInput &input,
                                               vector<LogicalType> &return_types, vector<string> &names) {
	auto bind = make_uniq<ReadFieldsBindData>();
	bind->path = input.inputs[0].GetValue<string>();
	auto dict = input.inputs[1].GetValue<string>();
	PostHogTelemetry::Instance().CaptureFunctionExecution("sap_idoc_read_fields");

	auto &np = input.named_parameters;
	if (np.count("framing") && !np["framing"].IsNull()) {
		bind->has_framing_override = true;
		bind->framing_override = erpl_idoc::FramingFromString(np["framing"].GetValue<string>());
	}
	if (np.count("lenient") && !np["lenient"].IsNull()) {
		bind->lenient = np["lenient"].GetValue<bool>();
	}
	if (np.count("encoding") && !np["encoding"].IsNull()) {
		bind->encoding = np["encoding"].GetValue<string>();
	}
	if (np.count("include_unknown") && !np["include_unknown"].IsNull()) {
		bind->include_unknown = np["include_unknown"].GetValue<bool>();
	}

	// Load slicing rules for ALL segments once, grouped by segment name.
	auto query = "SELECT segnam, field_pos, field_name, \"offset\", length, datatype FROM " + DictSource(dict) +
	             " ORDER BY segnam, field_pos";
	Connection con(*context.db);
	auto result = con.Query(query);
	if (result->HasError()) {
		throw BinderException("sap_idoc_read_fields: failed to read dictionary: " + result->GetError());
	}
	for (idx_t i = 0; i < result->RowCount(); i++) {
		auto seg = result->GetValue(0, i).ToString();
		TypedFieldRule rule;
		rule.field_pos = result->GetValue(1, i).IsNull() ? 0 : result->GetValue(1, i).GetValue<int64_t>();
		rule.name = result->GetValue(2, i).ToString();
		rule.offset = result->GetValue(3, i).GetValue<int64_t>();
		rule.length = result->GetValue(4, i).GetValue<int64_t>();
		auto dt = result->GetValue(5, i);
		rule.datatype = dt.IsNull() ? "" : dt.ToString();
		if (rule.offset < 0 || rule.length < 0 || rule.offset > static_cast<int64_t>(erpl_idoc::SDATA_LEN) ||
		    rule.length > static_cast<int64_t>(erpl_idoc::SDATA_LEN) - rule.offset) {
			throw BinderException("sap_idoc_read_fields: field '%s' (segment '%s') offset/length out of SDATA bounds",
			                      rule.name, seg);
		}
		bind->fields_by_seg[seg].push_back(std::move(rule));
	}

	names = {"document_key", "segnum", "psgnum", "hlevel", "segnam", "field_pos", "field_name", "datatype", "value"};
	return_types = {LogicalType::BIGINT,  LogicalType::VARCHAR, LogicalType::VARCHAR,
	                LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::INTEGER,
	                LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR};
	return std::move(bind);
}

static unique_ptr<GlobalTableFunctionState> ReadFieldsInitGlobal(ClientContext &context,
                                                                 TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<ReadFieldsBindData>();
	auto data = TypedReadWholeFile(context, bind.path);
	auto state = make_uniq<ReadFieldsGlobalState>();
	if (bind.lenient) {
		state->parsed = erpl_idoc::ParseImageLenient(data);
	} else {
		auto framing = bind.has_framing_override ? bind.framing_override : erpl_idoc::DetectFraming(data);
		state->parsed = erpl_idoc::ParseImage(data, framing);
	}
	return std::move(state);
}

static void ReadFieldsScan(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind = data_p.bind_data->Cast<ReadFieldsBindData>();
	auto &g = data_p.global_state->Cast<ReadFieldsGlobalState>();
	auto &records = g.parsed.records;
	idx_t out_row = 0;
	while (g.cursor < records.size() && out_row < STANDARD_VECTOR_SIZE) {
		auto &rec = records[g.cursor];
		if (rec.is_control) {
			g.cursor++;
			g.field_cursor = 0;
			continue;
		}
		auto segnam = RTrim(GetFieldRaw(rec.bytes, EDI_DD40_FIELDS[0]));
		auto it = bind.fields_by_seg.find(segnam);
		bool known = (it != bind.fields_by_seg.end());
		if (!known && !bind.include_unknown) {
			g.cursor++;
			g.field_cursor = 0;
			continue;
		}
		auto sdata = GetFieldRaw(rec.bytes, EDI_DD40_FIELDS[6]);  // SDATA(1000)
		auto segnum = RTrim(GetFieldRaw(rec.bytes, EDI_DD40_FIELDS[3]));
		auto psgnum = RTrim(GetFieldRaw(rec.bytes, EDI_DD40_FIELDS[4]));
		auto hlevel = RTrim(GetFieldRaw(rec.bytes, EDI_DD40_FIELDS[5]));
		idx_t nfields = known ? it->second.size() : 1;
		while (g.field_cursor < nfields && out_row < STANDARD_VECTOR_SIZE) {
			output.SetValue(0, out_row, Value::BIGINT(rec.document_key));
			output.SetValue(1, out_row, Value(segnum));
			output.SetValue(2, out_row, Value(psgnum));
			output.SetValue(3, out_row, Value(hlevel));
			output.SetValue(4, out_row, Value(segnam));
			if (known) {
				auto &f = it->second[g.field_cursor];
				output.SetValue(5, out_row, Value::INTEGER(static_cast<int32_t>(f.field_pos)));
				output.SetValue(6, out_row, Value(f.name));
				output.SetValue(7, out_row, f.datatype.empty() ? Value(LogicalType::VARCHAR) : Value(f.datatype));
				std::string raw = sdata.substr(f.offset, f.length);
				output.SetValue(8, out_row, Value(RTrim(erpl_idoc::DecodeText(raw, bind.encoding))));
			} else {
				// Segment not in the dictionary: one row with the raw trimmed SDATA.
				output.SetValue(5, out_row, Value(LogicalType::INTEGER));
				output.SetValue(6, out_row, Value(LogicalType::VARCHAR));
				output.SetValue(7, out_row, Value(LogicalType::VARCHAR));
				output.SetValue(8, out_row, Value(RTrim(erpl_idoc::DecodeText(sdata, bind.encoding))));
			}
			g.field_cursor++;
			out_row++;
		}
		if (g.field_cursor >= nfields) {
			g.cursor++;
			g.field_cursor = 0;
		}
	}
	output.SetCardinality(out_row);
}

void RegisterIdocTypedReaderFunctions(ExtensionLoader &loader) {
	TableFunction f("sap_idoc_read_segment", {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
	                ReadSegmentScan, ReadSegmentBind, ReadSegmentInitGlobal);
	f.named_parameters["framing"] = LogicalType::VARCHAR;
	f.named_parameters["lenient"] = LogicalType::BOOLEAN;
	f.named_parameters["encoding"] = LogicalType::VARCHAR;
	RegisterDocTableFunction(
	    loader, std::move(f),
	    "Typed read of one IDoc segment type: split each segment's SDATA into named, typed columns using a "
	    "segment dictionary. 'dict' is the dictionary source — a .csv/.parquet path, a table/view name, or a "
	    "relation expression (SPEC B4 columns: segnam, field_pos, field_name, offset, length, datatype). The "
	    "dictionary's origin (live erpl_rfc, persisted file, hand-authored) is irrelevant.",
	    {"SELECT airlineid, flightdate FROM sap_idoc_read_segment('flight.idoc', 'E1BPSBONEW', 'flight_dict.csv')",
	     "SELECT * FROM sap_idoc_read_segment('f.idoc', 'E1BPSBONEW', 'dict_view')"},
	    {"path", "segnam", "dict"});

	TableFunction ff("sap_idoc_read_fields", {LogicalType::VARCHAR, LogicalType::VARCHAR}, ReadFieldsScan,
	                 ReadFieldsBind, ReadFieldsInitGlobal);
	ff.named_parameters["framing"] = LogicalType::VARCHAR;
	ff.named_parameters["lenient"] = LogicalType::BOOLEAN;
	ff.named_parameters["encoding"] = LogicalType::VARCHAR;
	ff.named_parameters["include_unknown"] = LogicalType::BOOLEAN;
	RegisterDocTableFunction(
	    loader, std::move(ff),
	    "Decode ALL fields of every record in an IDoc using a segment dictionary — one call, long form: one row "
	    "per (record, field) with document_key, segnum, psgnum, hlevel, segnam, field_pos, field_name, datatype, "
	    "value. SDATA is sliced byte-correctly (same decoding as sap_idoc_read_segment). Segments absent from the "
	    "dictionary yield one row with NULL field_name/field_pos and the raw trimmed SDATA (set include_unknown => "
	    "false to drop them). 'dict' is any dictionary source: a .csv/.parquet path, a table/view name, or a "
	    "relation expression.",
	    {"SELECT segnam, field_name, value FROM sap_idoc_read_fields('flight.idoc', 'flight_dict.csv')",
	     "SELECT * FROM sap_idoc_read_fields('f.idoc', 'dict') WHERE segnam = 'E1BPSBONEW'"},
	    {"path", "dict"});
}

} // namespace duckdb
