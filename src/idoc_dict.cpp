#include "duckdb.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/materialized_query_result.hpp"
#include "duckdb/common/string_util.hpp"

#include "idoc_functions.hpp"
#include "idoc_format.hpp"
#include "idoc_dict_source.hpp"
#include "idoc_doc.hpp"

#include <algorithm>
#include <set>

namespace duckdb {

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------
static idx_t ReqCol(MaterializedQueryResult &r, const char *name) {
	for (idx_t i = 0; i < r.names.size(); i++) {
		if (StringUtil::CIEquals(r.names[i], name)) {
			return i;
		}
	}
	throw BinderException("dictionary is missing the required column '%s'", name);
}
static bool OptCol(MaterializedQueryResult &r, const char *name, idx_t &out) {
	for (idx_t i = 0; i < r.names.size(); i++) {
		if (StringUtil::CIEquals(r.names[i], name)) {
			out = i;
			return true;
		}
	}
	return false;
}
static int64_t ToI64(Value v) {
	return v.IsNull() ? 0 : v.DefaultCastAs(LogicalType::BIGINT).GetValue<int64_t>();
}
static std::string ToStr(const Value &v) {
	return v.IsNull() ? std::string() : v.ToString();
}

static unique_ptr<MaterializedQueryResult> QueryDict(ClientContext &context, const std::string &dict) {
	Connection con(*context.db);
	auto res = con.Query("SELECT * FROM " + DictSource(dict));
	if (res->HasError()) {
		throw BinderException("failed to read dictionary: " + res->GetError());
	}
	return res;
}

// ===========================================================================
// sap_idoc_dict_offsets(dict) — compute the 0-based SDATA offset of each field as the
// cumulative width of the preceding fields in the same segment (offline authoring:
// supply only field order + width). Input needs segnam, field_pos, field_name,
// length; datatype optional.
// ===========================================================================
struct DictRow {
	std::string segnam;
	int64_t field_pos;
	std::string field_name;
	int64_t offset;
	int64_t length;
	std::string datatype;
};

struct DictSourceBindData : public TableFunctionData {
	std::string dict;
};
struct DictRowsGlobalState : public GlobalTableFunctionState {
	vector<DictRow> rows;
	idx_t cursor = 0;
	idx_t MaxThreads() const override {
		return 1;
	}
};

static unique_ptr<FunctionData> DictSourceBind(ClientContext &, TableFunctionBindInput &input,
                                               vector<LogicalType> &return_types, vector<string> &names,
                                               bool with_problem) {
	auto bind = make_uniq<DictSourceBindData>();
	bind->dict = input.inputs[0].GetValue<string>();
	if (with_problem) {
		names = {"segnam", "field_name", "offset", "length", "problem"};
		return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::BIGINT, LogicalType::BIGINT,
		                LogicalType::VARCHAR};
	} else {
		names = {"segnam", "field_pos", "field_name", "offset", "length", "datatype"};
		return_types = {LogicalType::VARCHAR, LogicalType::BIGINT, LogicalType::VARCHAR,
		                LogicalType::BIGINT,  LogicalType::BIGINT, LogicalType::VARCHAR};
	}
	return std::move(bind);
}
static unique_ptr<FunctionData> OffsetsBind(ClientContext &c, TableFunctionBindInput &input,
                                            vector<LogicalType> &rt, vector<string> &n) {
	return DictSourceBind(c, input, rt, n, false);
}

static vector<DictRow> LoadRows(ClientContext &context, const std::string &dict, bool need_offset) {
	auto res = QueryDict(context, dict);
	idx_t c_seg = ReqCol(*res, "segnam"), c_pos = ReqCol(*res, "field_pos"), c_fn = ReqCol(*res, "field_name"),
	      c_len = ReqCol(*res, "length");
	idx_t c_dt, c_off;
	bool has_dt = OptCol(*res, "datatype", c_dt);
	bool has_off = OptCol(*res, "offset", c_off);
	if (need_offset && !has_off) {
		throw BinderException("dictionary is missing the required column 'offset'");
	}
	vector<DictRow> rows;
	for (idx_t i = 0; i < res->RowCount(); i++) {
		DictRow r;
		r.segnam = ToStr(res->GetValue(c_seg, i));
		r.field_pos = ToI64(res->GetValue(c_pos, i));
		r.field_name = ToStr(res->GetValue(c_fn, i));
		r.length = ToI64(res->GetValue(c_len, i));
		r.offset = has_off ? ToI64(res->GetValue(c_off, i)) : 0;
		r.datatype = has_dt ? ToStr(res->GetValue(c_dt, i)) : std::string();
		rows.push_back(std::move(r));
	}
	std::stable_sort(rows.begin(), rows.end(), [](const DictRow &a, const DictRow &b) {
		return a.segnam == b.segnam ? a.field_pos < b.field_pos : a.segnam < b.segnam;
	});
	return rows;
}

static unique_ptr<GlobalTableFunctionState> OffsetsInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<DictSourceBindData>();
	auto state = make_uniq<DictRowsGlobalState>();
	state->rows = LoadRows(context, bind.dict, /*need_offset=*/false);
	// cumulative offset per segment
	std::string cur;
	int64_t running = 0;
	for (auto &r : state->rows) {
		if (r.segnam != cur) {
			cur = r.segnam;
			running = 0;
		}
		r.offset = running;
		running += r.length;
	}
	return std::move(state);
}

static void OffsetsScan(ClientContext &, TableFunctionInput &data_p, DataChunk &output) {
	auto &gs = data_p.global_state->Cast<DictRowsGlobalState>();
	idx_t n = 0;
	while (gs.cursor < gs.rows.size() && n < STANDARD_VECTOR_SIZE) {
		auto &r = gs.rows[gs.cursor++];
		output.SetValue(0, n, Value(r.segnam));
		output.SetValue(1, n, Value::BIGINT(r.field_pos));
		output.SetValue(2, n, Value(r.field_name));
		output.SetValue(3, n, Value::BIGINT(r.offset));
		output.SetValue(4, n, Value::BIGINT(r.length));
		output.SetValue(5, n, r.datatype.empty() ? Value(LogicalType::VARCHAR) : Value(r.datatype));
		n++;
	}
	output.SetCardinality(n);
}

// ===========================================================================
// sap_idoc_dict_validate(dict) — one row per structural problem; empty = sound.
// ===========================================================================
struct ProblemRow {
	std::string segnam, field_name, problem;
	int64_t offset, length;
};
struct ValidateGlobalState : public GlobalTableFunctionState {
	vector<ProblemRow> problems;
	idx_t cursor = 0;
	idx_t MaxThreads() const override {
		return 1;
	}
};
static unique_ptr<FunctionData> ValidateBind(ClientContext &c, TableFunctionBindInput &input, vector<LogicalType> &rt,
                                             vector<string> &n) {
	return DictSourceBind(c, input, rt, n, true);
}
static unique_ptr<GlobalTableFunctionState> ValidateInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<DictSourceBindData>();
	auto rows = LoadRows(context, bind.dict, /*need_offset=*/true);
	auto state = make_uniq<ValidateGlobalState>();
	std::string cur;
	int64_t prev_end = -1;
	std::set<int64_t> seen_pos;
	for (auto &r : rows) {
		if (r.segnam != cur) {
			cur = r.segnam;
			prev_end = -1;
			seen_pos.clear();
		}
		auto add = [&](const std::string &p) {
			state->problems.push_back(ProblemRow{r.segnam, r.field_name, p, r.offset, r.length});
		};
		if (r.offset < 0 || r.length <= 0) {
			add("offset<0 or length<=0");
		}
		if (r.offset + r.length > static_cast<int64_t>(erpl_idoc::SDATA_LEN)) {
			add("field exceeds SDATA(1000)");
		}
		if (prev_end >= 0 && r.offset < prev_end) {
			add("overlaps the previous field");
		}
		if (!seen_pos.insert(r.field_pos).second) {
			add("duplicate field_pos in segment");
		}
		prev_end = r.offset + r.length;
	}
	return std::move(state);
}
static void ValidateScan(ClientContext &, TableFunctionInput &data_p, DataChunk &output) {
	auto &gs = data_p.global_state->Cast<ValidateGlobalState>();
	idx_t n = 0;
	while (gs.cursor < gs.problems.size() && n < STANDARD_VECTOR_SIZE) {
		auto &p = gs.problems[gs.cursor++];
		output.SetValue(0, n, Value(p.segnam));
		output.SetValue(1, n, Value(p.field_name));
		output.SetValue(2, n, Value::BIGINT(p.offset));
		output.SetValue(3, n, Value::BIGINT(p.length));
		output.SetValue(4, n, Value(p.problem));
		n++;
	}
	output.SetCardinality(n);
}

// ===========================================================================
// sap_idoc_dict_from_fields(fields, idoctyp, cimtyp, release) -> LIST<STRUCT(B4)>
// Normalize an IDOCTYPE_READ_COMPLETE PT_FIELDS list into B4 dictionary rows,
// without any RFC (it only transforms data erpl_rfc already returned).
// ===========================================================================
static const child_list_t<LogicalType> &B4Fields() {
	static const child_list_t<LogicalType> fields = {
	    {"idoctyp", LogicalType::VARCHAR},   {"cimtyp", LogicalType::VARCHAR},
	    {"release", LogicalType::VARCHAR},   {"segnam", LogicalType::VARCHAR},
	    {"segdef", LogicalType::VARCHAR},    {"field_pos", LogicalType::INTEGER},
	    {"field_name", LogicalType::VARCHAR},{"offset", LogicalType::INTEGER},
	    {"length", LogicalType::INTEGER},    {"datatype", LogicalType::VARCHAR},
	    {"data_element", LogicalType::VARCHAR}, {"description", LogicalType::VARCHAR},
	    {"mandatory", LogicalType::BOOLEAN}};
	return fields;
}

// case-insensitive struct field lookup; returns NULL Value if absent
static Value StructField(const Value &s, const char *name) {
	auto &types = StructType::GetChildTypes(s.type());
	auto &vals = StructValue::GetChildren(s);
	for (idx_t i = 0; i < types.size(); i++) {
		if (StringUtil::CIEquals(types[i].first, name)) {
			return vals[i];
		}
	}
	return Value();
}

static void DictFromFieldsFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto struct_type = LogicalType::STRUCT(B4Fields());
	for (idx_t row = 0; row < args.size(); row++) {
		auto fields = args.data[0].GetValue(row);
		auto idoctyp = ToStr(args.data[1].GetValue(row));
		auto cimtyp = ToStr(args.data[2].GetValue(row));
		auto release = ToStr(args.data[3].GetValue(row));
		vector<Value> out_rows;
		if (!fields.IsNull()) {
			for (auto &f : ListValue::GetChildren(fields)) {
				auto i32 = [&](const char *nm) {
					auto v = StructField(f, nm);
					return v.IsNull() ? Value(LogicalType::INTEGER)
					                  : Value::INTEGER((int32_t)ToI64(v));
				};
				auto vstr = [&](const char *nm) {
					auto v = StructField(f, nm);
					return v.IsNull() ? Value(LogicalType::VARCHAR) : Value(ToStr(v));
				};
				child_list_t<Value> b4 = {{"idoctyp", Value(idoctyp)},
				                          {"cimtyp", Value(cimtyp)},
				                          {"release", Value(release)},
				                          {"segnam", vstr("SEGMENTTYP")},
				                          {"segdef", Value(LogicalType::VARCHAR)},
				                          {"field_pos", i32("FIELD_POS")},
				                          {"field_name", vstr("FIELDNAME")},
				                          {"offset", i32("BYTE_FIRST")},
				                          {"length", i32("EXTLEN")},
				                          {"datatype", vstr("DATATYPE")},
				                          {"data_element", vstr("ROLLNAME")},
				                          {"description", vstr("DESCRP")},
				                          {"mandatory", Value::BOOLEAN(false)}};
				out_rows.push_back(Value::STRUCT(std::move(b4)));
			}
		}
		result.SetValue(row, Value::LIST(struct_type, std::move(out_rows)));
	}
	if (args.size() == 1) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

void RegisterIdocDictFunctions(ExtensionLoader &loader) {
	RegisterDocTableFunction(
	    loader, TableFunction("sap_idoc_dict_offsets", {LogicalType::VARCHAR}, OffsetsScan, OffsetsBind, OffsetsInit),
	    "Compute a segment dictionary's field offsets from lengths only: the 0-based SDATA offset of each "
	    "field is the cumulative width of the preceding fields in the same segment. Input needs "
	    "segnam, field_pos, field_name, length (datatype optional). 'src' is a .csv/.parquet path, a "
	    "table/view name, or a relation expression.",
	    {"SELECT * FROM sap_idoc_dict_offsets('my_fields.csv')"}, {"src"});

	RegisterDocTableFunction(
	    loader, TableFunction("sap_idoc_dict_validate", {LogicalType::VARCHAR}, ValidateScan, ValidateBind, ValidateInit),
	    "Validate a segment dictionary: returns one row per structural problem (offset<0/length<=0, field "
	    "exceeds SDATA(1000), overlaps the previous field, duplicate field_pos). An empty result means the "
	    "dictionary is structurally sound.",
	    {"SELECT * FROM sap_idoc_dict_validate('my.dict.parquet')"}, {"src"});

	RegisterDocScalarFunction(
	    loader,
	    ScalarFunction("sap_idoc_dict_from_fields",
	                   {LogicalType::LIST(LogicalType::ANY), LogicalType::VARCHAR, LogicalType::VARCHAR,
	                    LogicalType::VARCHAR},
	                   LogicalType::LIST(LogicalType::STRUCT(B4Fields())), DictFromFieldsFun),
	    "Normalize an IDOCTYPE_READ_COMPLETE PT_FIELDS list into the SPEC B4 dictionary schema (the "
	    "transform used internally by the sap_idoc_dictionary macro). Pure — it only reshapes data that "
	    "erpl_rfc already returned; no RFC is performed here.",
	    {"SELECT UNNEST(sap_idoc_dict_from_fields(r.PT_FIELDS,'MATMAS05','','620')) "
	     "FROM sap_rfc_invoke('IDOCTYPE_READ_COMPLETE', sap_idoc_params('MATMAS05')) r"},
	    {"fields", "idoctyp", "cimtyp", "release"});
}

} // namespace duckdb
