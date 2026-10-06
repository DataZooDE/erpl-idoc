#include "duckdb.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/materialized_query_result.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/date.hpp"

#include "idoc_functions.hpp"
#include "idoc_format.hpp"
#include "idoc_dict_source.hpp"
#include "idoc_multifile.hpp"
#include "idoc_doc.hpp"
#include "telemetry.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include "erpl_idoc_banner.hpp"

namespace duckdb {

using erpl_idoc::EDI_DD40_FIELDS;
using erpl_idoc::Framing;
using erpl_idoc::GetFieldRaw;
using erpl_idoc::ParsedIdoc;
using erpl_idoc::RTrim;

// One field's slicing rule taken from the dictionary.
struct TypedFieldRule {
	std::string name;
	int64_t offset;
	int64_t length;
	std::string datatype;
	int64_t field_pos = 0; // used by sap_idoc_read_fields (long form)
	// sap_idoc_read_segment(typed := true): SQL type this field decodes to.
	enum class Kind { TEXT, DATE, TIME, DECIMAL };
	Kind kind = Kind::TEXT;
	LogicalType type = LogicalType::VARCHAR;
	int64_t decimals = -1; // from the dictionary's optional `decimals` column; -1 = not supplied
};

static std::string UpperAscii(std::string s) {
	for (auto &c : s) {
		c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
	}
	return s;
}

// Map a SAP DDIC datatype to a SQL type. SAP's IDOCTYPE_READ_COMPLETE reports no decimals, so
// DEC/CURR/QUAN only become DECIMAL when the dictionary supplies them; otherwise they stay text.
// NUMC stays text on purpose: leading zeros are significant for keys.
static void ApplyTypedMapping(TypedFieldRule &rule) {
	auto dt = UpperAscii(rule.datatype);
	if (dt == "DATS") {
		rule.kind = TypedFieldRule::Kind::DATE;
		rule.type = LogicalType::DATE;
	} else if (dt == "TIMS") {
		rule.kind = TypedFieldRule::Kind::TIME;
		rule.type = LogicalType::TIME;
	} else if ((dt == "DEC" || dt == "CURR" || dt == "QUAN") && rule.decimals >= 0 && rule.decimals <= 38) {
		auto width = std::min<int64_t>(38, std::max<int64_t>(rule.length, rule.decimals + 1));
		rule.kind = TypedFieldRule::Kind::DECIMAL;
		rule.type = LogicalType::DECIMAL(static_cast<uint8_t>(width), static_cast<uint8_t>(rule.decimals));
	}
}

static bool AllDigits(const std::string &s) {
	return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

// Parse the (already decoded) text of one field. Returns false when the text is not a valid value
// of the field's type; blank text and the all-zero SAP date are a valid NULL.
static bool ParseTypedValue(const TypedFieldRule &f, const std::string &raw, Value &out) {
	auto text = RTrim(raw);
	size_t lead = text.find_first_not_of(' ');
	text = lead == std::string::npos ? std::string() : text.substr(lead);
	out = Value(f.type); // NULL of the column type
	if (text.empty()) {
		return true;
	}
	switch (f.kind) {
	case TypedFieldRule::Kind::DATE: {
		if (text == "00000000") {
			return true;
		}
		if (text.size() != 8 || !AllDigits(text)) {
			return false;
		}
		int32_t y = std::stoi(text.substr(0, 4)), m = std::stoi(text.substr(4, 2)), d = std::stoi(text.substr(6, 2));
		if (!Date::IsValid(y, m, d)) {
			return false;
		}
		out = Value::DATE(Date::FromDate(y, m, d));
		return true;
	}
	case TypedFieldRule::Kind::TIME: {
		if (text.size() != 6 || !AllDigits(text)) {
			return false;
		}
		int32_t h = std::stoi(text.substr(0, 2)), mi = std::stoi(text.substr(2, 2)), sec = std::stoi(text.substr(4, 2));
		if (!Time::IsValidTime(h, mi, sec, 0)) {
			return false;
		}
		out = Value::TIME(Time::FromTime(h, mi, sec, 0));
		return true;
	}
	case TypedFieldRule::Kind::DECIMAL: {
		// SAP writes a trailing minus ("5.000-"); a leading '+' is tolerated.
		if (text.back() == '-') {
			text = "-" + text.substr(0, text.size() - 1);
		} else if (text.front() == '+') {
			text.erase(0, 1);
		}
		if (text.find_first_not_of("-.0123456789") != std::string::npos) {
			return false;
		}
		// never round silently: more fractional digits than the dictionary's scale is invalid
		auto dot = text.find('.');
		if (dot != std::string::npos && static_cast<int64_t>(text.size() - dot - 1) > f.decimals) {
			return false;
		}
		string error;
		return Value(text).DefaultTryCastAs(f.type, out, &error, true);
	}
	default:
		return false;
	}
}

struct ReadSegmentBindData : public TableFunctionData {
	vector<std::string> files;
	std::string segnam;
	bool has_framing_override = false;
	Framing framing_override = Framing::FIXED;
	bool lenient = false;
	bool with_filename = false;
	erpl_idoc::TextEncoding encoding = erpl_idoc::TextEncoding::UTF8;
	bool typed = false;
	bool strict = true;
	vector<TypedFieldRule> fields;
};

static unique_ptr<FunctionData> ReadSegmentBind(ClientContext &context, TableFunctionBindInput &input,
                                                vector<LogicalType> &return_types, vector<string> &names) {
	auto bind = make_uniq<ReadSegmentBindData>();
	bind->files = IdocResolvePaths(context, input.inputs[0]);
	bind->segnam = input.inputs[1].GetValue<string>();
	auto dict = input.inputs[2].GetValue<string>();
	PostHogTelemetry::Instance().RecordFunctionCall("sap_idoc_read_segment");

	auto &np = input.named_parameters;
	if (np.count("framing") && !np["framing"].IsNull()) {
		bind->has_framing_override = true;
		bind->framing_override = erpl_idoc::FramingFromString(np["framing"].GetValue<string>());
	}
	if (np.count("lenient") && !np["lenient"].IsNull()) {
		bind->lenient = np["lenient"].GetValue<bool>();
	}
	if (np.count("encoding") && !np["encoding"].IsNull()) {
		bind->encoding = CheckedEncodingParam(np["encoding"]);
	}
	if (np.count("filename") && !np["filename"].IsNull()) {
		bind->with_filename = np["filename"].GetValue<bool>();
	}
	if (np.count("typed") && !np["typed"].IsNull()) {
		bind->typed = np["typed"].GetValue<bool>();
	}
	if (np.count("strict") && !np["strict"].IsNull()) {
		bind->strict = np["strict"].GetValue<bool>();
	}

	// Load the slicing rules for this segment from the dictionary relation. `decimals` is an optional
	// dictionary column (SAP's own field list does not carry it); it scales DEC/CURR/QUAN when typed.
	Connection con(*context.db);
	string decimals_expr = "NULL";
	auto probe = con.Query("SELECT * FROM " + DictSource(dict) + " LIMIT 0");
	if (!probe->HasError()) {
		for (auto &col : probe->names) {
			if (UpperAscii(col) == "DECIMALS") {
				decimals_expr = "\"" + col + "\"";
			}
		}
	}
	auto query = "SELECT field_name, \"offset\", length, datatype, " + decimals_expr + " FROM " + DictSource(dict) +
	             " WHERE segnam = '" + DictSqlEscape(bind->segnam) + "' ORDER BY field_pos";
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
		auto decimals = result->GetValue(4, i);
		rule.decimals = decimals.IsNull() ? -1 : decimals.GetValue<int64_t>();
		if (rule.offset < 0 || rule.length < 0 || rule.offset > static_cast<int64_t>(erpl_idoc::SDATA_LEN) ||
		    rule.length > static_cast<int64_t>(erpl_idoc::SDATA_LEN) - rule.offset) {
			throw BinderException("sap_idoc_read_segment: field '%s' offset/length out of SDATA bounds", rule.name);
		}
		if (bind->typed) {
			ApplyTypedMapping(rule);
		}
		// lower-case the column name (SAP dict names are upper-case)
		std::string lower;
		for (char c : rule.name) {
			lower += static_cast<char>(tolower(static_cast<unsigned char>(c)));
		}
		names.push_back(lower);
		return_types.push_back(rule.type); // VARCHAR unless typed := true maps this field's datatype
		bind->fields.push_back(std::move(rule));
	}
	if (bind->with_filename) {
		names.push_back("filename");
		return_types.push_back(LogicalType::VARCHAR);
	}
	return std::move(bind);
}

static unique_ptr<GlobalTableFunctionState> ReadSegmentInitGlobal(ClientContext &context,
                                                                  TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<ReadSegmentBindData>();
	return MakeIdocGlobal(context, bind.files, bind.lenient, bind.has_framing_override, bind.framing_override,
	                      /*allow_xml_control=*/false);
}
static unique_ptr<LocalTableFunctionState> TypedInitLocal(ExecutionContext &, TableFunctionInitInput &,
                                                          GlobalTableFunctionState *) {
	return MakeIdocLocal();
}

static void ReadSegmentScan(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind = data_p.bind_data->Cast<ReadSegmentBindData>();
	auto &g = data_p.global_state->Cast<IdocGlobalState>();
	auto &l = data_p.local_state->Cast<IdocLocalState>();
	idx_t out_row = 0;
	while (out_row < STANDARD_VECTOR_SIZE && IdocPeek(context, g, l)) {
		auto &rec = l.pending;
		if (rec.is_control || RTrim(GetFieldRaw(rec.bytes, EDI_DD40_FIELDS[0])) != bind.segnam) { // SEGNAM
			IdocConsume(l);
			continue;
		}
		auto sdata = GetFieldRaw(rec.bytes, EDI_DD40_FIELDS[6]); // 1000-byte SDATA
		output.SetValue(0, out_row, Value::BIGINT(rec.document_key));
		output.SetValue(1, out_row, Value(RTrim(GetFieldRaw(rec.bytes, EDI_DD40_FIELDS[3])))); // SEGNUM
		for (idx_t i = 0; i < bind.fields.size(); i++) {
			auto &f = bind.fields[i];
			std::string raw = sdata.substr(f.offset, f.length);
			// decode per the source codepage, then trim trailing pad spaces
			auto text = erpl_idoc::DecodeText(raw, bind.encoding);
			if (f.kind == TypedFieldRule::Kind::TEXT) {
				output.SetValue(2 + i, out_row, Value(RTrim(text)));
				continue;
			}
			Value value;
			if (!ParseTypedValue(f, text, value)) {
				if (bind.strict) {
					throw InvalidInputException(
					    "sap_idoc_read_segment: field '%s' (%s) has invalid value '%s' (document %d, segment %s); "
					    "pass strict := false to read it as NULL",
					    f.name, f.datatype, RTrim(text), static_cast<int64_t>(rec.document_key),
					    RTrim(GetFieldRaw(rec.bytes, EDI_DD40_FIELDS[3])));
				}
				value = Value(f.type);
			}
			output.SetValue(2 + i, out_row, value);
		}
		if (bind.with_filename) {
			output.SetValue(output.ColumnCount() - 1, out_row, Value(l.current_file));
		}
		IdocConsume(l);
		out_row++;
	}
	output.SetCardinality(out_row);
}

// --- sap_idoc_read_fields: decode ALL fields of ALL records in one call ---------
// Long form (one row per field), dictionary-driven, byte-correct — the same slicing
// as sap_idoc_read_segment, but across every segment at once and without per-segment
// calls or manual offset math.
struct ReadFieldsBindData : public TableFunctionData {
	vector<std::string> files;
	bool has_framing_override = false;
	Framing framing_override = Framing::FIXED;
	bool lenient = false;
	bool include_unknown = true;
	bool with_filename = false;
	erpl_idoc::TextEncoding encoding = erpl_idoc::TextEncoding::UTF8;
	std::map<std::string, vector<TypedFieldRule>> fields_by_seg;
};

static unique_ptr<FunctionData> ReadFieldsBind(ClientContext &context, TableFunctionBindInput &input,
                                               vector<LogicalType> &return_types, vector<string> &names) {
	auto bind = make_uniq<ReadFieldsBindData>();
	bind->files = IdocResolvePaths(context, input.inputs[0]);
	auto dict = input.inputs[1].GetValue<string>();
	PostHogTelemetry::Instance().RecordFunctionCall("sap_idoc_read_fields");

	auto &np = input.named_parameters;
	if (np.count("framing") && !np["framing"].IsNull()) {
		bind->has_framing_override = true;
		bind->framing_override = erpl_idoc::FramingFromString(np["framing"].GetValue<string>());
	}
	if (np.count("lenient") && !np["lenient"].IsNull()) {
		bind->lenient = np["lenient"].GetValue<bool>();
	}
	if (np.count("encoding") && !np["encoding"].IsNull()) {
		bind->encoding = CheckedEncodingParam(np["encoding"]);
	}
	if (np.count("include_unknown") && !np["include_unknown"].IsNull()) {
		bind->include_unknown = np["include_unknown"].GetValue<bool>();
	}
	if (np.count("filename") && !np["filename"].IsNull()) {
		bind->with_filename = np["filename"].GetValue<bool>();
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
	if (bind->with_filename) {
		names.push_back("filename");
		return_types.push_back(LogicalType::VARCHAR);
	}
	return std::move(bind);
}

static unique_ptr<GlobalTableFunctionState> ReadFieldsInitGlobal(ClientContext &context,
                                                                 TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<ReadFieldsBindData>();
	return MakeIdocGlobal(context, bind.files, bind.lenient, bind.has_framing_override, bind.framing_override,
	                      /*allow_xml_control=*/false);
}

static void ReadFieldsScan(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind = data_p.bind_data->Cast<ReadFieldsBindData>();
	auto &g = data_p.global_state->Cast<IdocGlobalState>();
	auto &l = data_p.local_state->Cast<IdocLocalState>();
	idx_t out_row = 0;
	while (out_row < STANDARD_VECTOR_SIZE && IdocPeek(context, g, l)) {
		auto &rec = l.pending;
		if (rec.is_control) {
			IdocConsume(l);
			continue;
		}
		auto segnam = RTrim(GetFieldRaw(rec.bytes, EDI_DD40_FIELDS[0]));
		auto it = bind.fields_by_seg.find(segnam);
		bool known = (it != bind.fields_by_seg.end());
		if (!known && !bind.include_unknown) {
			IdocConsume(l);
			continue;
		}
		auto sdata = GetFieldRaw(rec.bytes, EDI_DD40_FIELDS[6]);  // SDATA(1000)
		auto segnum = RTrim(GetFieldRaw(rec.bytes, EDI_DD40_FIELDS[3]));
		auto psgnum = RTrim(GetFieldRaw(rec.bytes, EDI_DD40_FIELDS[4]));
		auto hlevel = RTrim(GetFieldRaw(rec.bytes, EDI_DD40_FIELDS[5]));
		idx_t nfields = known ? it->second.size() : 1;
		while (l.field_cursor < nfields && out_row < STANDARD_VECTOR_SIZE) {
			output.SetValue(0, out_row, Value::BIGINT(rec.document_key));
			output.SetValue(1, out_row, Value(segnum));
			output.SetValue(2, out_row, Value(psgnum));
			output.SetValue(3, out_row, Value(hlevel));
			output.SetValue(4, out_row, Value(segnam));
			if (known) {
				auto &f = it->second[l.field_cursor];
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
			if (bind.with_filename) {
				output.SetValue(output.ColumnCount() - 1, out_row, Value(l.current_file));
			}
			l.field_cursor++;
			out_row++;
		}
		if (l.field_cursor >= nfields) {
			IdocConsume(l);
		}
	}
	output.SetCardinality(out_row);
}

void RegisterIdocTypedReaderFunctions(ExtensionLoader &loader) {
	// The first argument (the IDoc path) is either a VARCHAR path/glob or a
	// LIST(VARCHAR); the trailing fixed args (segnam, dict) are unchanged.
	{
		TableFunctionSet set("sap_idoc_read_segment");
		for (auto &first : vector<LogicalType> {LogicalType::VARCHAR, LogicalType::LIST(LogicalType::VARCHAR)}) {
			TableFunction f("sap_idoc_read_segment", {first, LogicalType::VARCHAR, LogicalType::VARCHAR},
			                DATAZOO_GUARD(ERPL_IDOC_BANNER, ReadSegmentScan),
	                DATAZOO_GUARD(ERPL_IDOC_BANNER, ReadSegmentBind), ReadSegmentInitGlobal, TypedInitLocal);
			f.named_parameters["framing"] = LogicalType::VARCHAR;
			f.named_parameters["lenient"] = LogicalType::BOOLEAN;
			f.named_parameters["encoding"] = LogicalType::VARCHAR;
			f.named_parameters["filename"] = LogicalType::BOOLEAN;
			f.named_parameters["typed"] = LogicalType::BOOLEAN;
			f.named_parameters["strict"] = LogicalType::BOOLEAN;
			set.AddFunction(std::move(f));
		}
		RegisterDocTableFunctionSet(
		    loader, std::move(set),
		    "Read one IDoc segment type: split each segment's SDATA into named columns using a segment "
		    "dictionary. Columns are VARCHAR by default; 'typed := true' maps DATS to DATE, TIMS to TIME and "
		    "DEC/CURR/QUAN to DECIMAL (only where the dictionary supplies a 'decimals' column; NUMC and CHAR stay "
		    "text). An invalid value raises an error; 'strict := false' reads it as NULL instead. Accepts a single "
		    "path, a glob, or a LIST of paths (+ 'filename := true'). 'dict' is the dictionary source — a "
		    ".csv/.parquet path, a table/view name, or a relation expression (SPEC B4 columns: segnam, field_pos, "
		    "field_name, offset, length, datatype [, decimals]).",
		    {"SELECT airlineid, flightdate FROM sap_idoc_read_segment('flight.idoc', 'E1BPSBONEW', 'flight_dict.csv')",
		     "SELECT flightdate FROM sap_idoc_read_segment('flight.idoc', 'E1BPSBONEW', 'flight_dict.csv', typed := true)",
		     "SELECT * FROM sap_idoc_read_segment('corpus/*.idoc', 'E1BPSBONEW', 'dict_view')"},
		    {"path", "segnam", "dict", "filename", "encoding", "lenient", "framing", "typed", "strict"});
	}
	{
		TableFunctionSet set("sap_idoc_read_fields");
		for (auto &first : vector<LogicalType> {LogicalType::VARCHAR, LogicalType::LIST(LogicalType::VARCHAR)}) {
			TableFunction f("sap_idoc_read_fields", {first, LogicalType::VARCHAR}, DATAZOO_GUARD(ERPL_IDOC_BANNER, ReadFieldsScan),
	                DATAZOO_GUARD(ERPL_IDOC_BANNER, ReadFieldsBind),
			                ReadFieldsInitGlobal, TypedInitLocal);
			f.named_parameters["framing"] = LogicalType::VARCHAR;
			f.named_parameters["lenient"] = LogicalType::BOOLEAN;
			f.named_parameters["encoding"] = LogicalType::VARCHAR;
			f.named_parameters["include_unknown"] = LogicalType::BOOLEAN;
			f.named_parameters["filename"] = LogicalType::BOOLEAN;
			set.AddFunction(std::move(f));
		}
		RegisterDocTableFunctionSet(
		    loader, std::move(set),
		    "Decode ALL fields of every record in an IDoc using a segment dictionary — one call, long form: one row "
		    "per (record, field) with document_key, segnum, psgnum, hlevel, segnam, field_pos, field_name, datatype, "
		    "value. SDATA is sliced byte-correctly (same decoding as sap_idoc_read_segment). Accepts a single path, a "
		    "glob, or a LIST of paths (+ 'filename := true'). Segments absent from the dictionary yield one row with "
		    "NULL field_name/field_pos and the raw trimmed SDATA (set include_unknown => false to drop them). 'dict' "
		    "is any dictionary source: a .csv/.parquet path, a table/view name, or a relation expression.",
		    {"SELECT segnam, field_name, value FROM sap_idoc_read_fields('flight.idoc', 'flight_dict.csv')",
		     "SELECT * FROM sap_idoc_read_fields('corpus/*.idoc', 'dict', filename=true) WHERE segnam = 'E1BPSBONEW'"},
		    {"path", "dict", "filename", "include_unknown", "encoding", "lenient", "framing"});
	}
}

} // namespace duckdb
