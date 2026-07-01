#include "duckdb.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/file_open_flags.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/materialized_query_result.hpp"

#include "idoc_functions.hpp"
#include "idoc_format.hpp"
#include "idoc_dict_source.hpp"

#include <cctype>

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
		throw BinderException("read_idoc_segment: failed to read dictionary: " + result->GetError());
	}
	if (result->RowCount() == 0) {
		throw BinderException("read_idoc_segment: dictionary has no fields for segment '%s'", bind->segnam);
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
			throw BinderException("read_idoc_segment: field '%s' offset/length out of SDATA bounds", rule.name);
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

void RegisterIdocTypedReaderFunctions(ExtensionLoader &loader) {
	TableFunction f("read_idoc_segment", {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
	                ReadSegmentScan, ReadSegmentBind, ReadSegmentInitGlobal);
	f.named_parameters["framing"] = LogicalType::VARCHAR;
	f.named_parameters["lenient"] = LogicalType::BOOLEAN;
	f.named_parameters["encoding"] = LogicalType::VARCHAR;
	loader.RegisterFunction(f);
}

} // namespace duckdb
