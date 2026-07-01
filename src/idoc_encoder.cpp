#include "duckdb.hpp"
#include "duckdb/function/scalar_function.hpp"

#include "idoc_functions.hpp"
#include "idoc_format.hpp"
#include "idoc_doc.hpp"

namespace duckdb {

using erpl_idoc::EncodeControl;
using erpl_idoc::EncodeDataRecord;
using erpl_idoc::EncodeSdata;

static std::string ValStr(const Value &v) {
	return v.IsNull() ? std::string() : v.ToString();
}
static int64_t ValI64(const Value &v) {
	return v.IsNull() ? 0 : v.GetValue<int64_t>();
}
static std::vector<std::string> ListStr(const Value &list) {
	std::vector<std::string> out;
	if (list.IsNull()) {
		return out;
	}
	for (auto &c : ListValue::GetChildren(list)) {
		out.push_back(ValStr(c));
	}
	return out;
}
static std::vector<int64_t> ListI64(const Value &list) {
	std::vector<int64_t> out;
	if (list.IsNull()) {
		return out;
	}
	for (auto &c : ListValue::GetChildren(list)) {
		out.push_back(ValI64(c));
	}
	return out;
}

// sap_idoc_encode_sdata(offsets LIST<INT>, lengths LIST<INT>, values LIST<VARCHAR>) -> VARCHAR(1000)
static void EncodeSdataFun(DataChunk &args, ExpressionState &state, Vector &result) {
	for (idx_t row = 0; row < args.size(); row++) {
		auto off = ListI64(args.data[0].GetValue(row));
		auto len = ListI64(args.data[1].GetValue(row));
		auto val = ListStr(args.data[2].GetValue(row));
		auto sdata = EncodeSdata(off, len, val);
		result.SetValue(row, Value(sdata));
	}
	if (args.size() == 1) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

// sap_idoc_encode_data_record(segnam, mandt, docnum, segnum, psgnum, hlevel, sdata) -> BLOB(1063)
static void EncodeDataRecordFun(DataChunk &args, ExpressionState &state, Vector &result) {
	for (idx_t row = 0; row < args.size(); row++) {
		auto rec = EncodeDataRecord(ValStr(args.data[0].GetValue(row)), ValStr(args.data[1].GetValue(row)),
		                            ValI64(args.data[2].GetValue(row)), ValI64(args.data[3].GetValue(row)),
		                            ValI64(args.data[4].GetValue(row)), ValI64(args.data[5].GetValue(row)),
		                            ValStr(args.data[6].GetValue(row)));
		result.SetValue(row, Value::BLOB_RAW(rec));
	}
	if (args.size() == 1) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

// sap_idoc_encode_control(values LIST<VARCHAR>) -> BLOB(524)
static void EncodeControlFun(DataChunk &args, ExpressionState &state, Vector &result) {
	for (idx_t row = 0; row < args.size(); row++) {
		auto rec = EncodeControl(ListStr(args.data[0].GetValue(row)));
		result.SetValue(row, Value::BLOB_RAW(rec));
	}
	if (args.size() == 1) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

void RegisterIdocEncoderFunctions(ExtensionLoader &loader) {
	RegisterDocScalarFunction(
	    loader,
	    ScalarFunction("sap_idoc_encode_sdata",
	                   {LogicalType::LIST(LogicalType::BIGINT), LogicalType::LIST(LogicalType::BIGINT),
	                    LogicalType::LIST(LogicalType::VARCHAR)},
	                   LogicalType::VARCHAR, EncodeSdataFun),
	    "Compose a 1000-byte IDoc SDATA payload by placing each value at its (offset, length) — the "
	    "parallel lists come from the segment dictionary. Values are space-padded/truncated to width.",
	    {"SELECT sap_idoc_encode_sdata([0,3], [3,4], ['LH','0400'])"}, {"offsets", "lengths", "values"});

	RegisterDocScalarFunction(
	    loader,
	    ScalarFunction("sap_idoc_encode_data_record",
	                   {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::BIGINT, LogicalType::BIGINT,
	                    LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::VARCHAR},
	                   LogicalType::BLOB, EncodeDataRecordFun),
	    "Compose a 1063-byte EDI_DD40 data record (BLOB). Numeric header fields (docnum, segnum, psgnum, "
	    "hlevel) are zero-padded to SAP widths; segnam/mandt/sdata are placed as-is.",
	    {"SELECT sap_idoc_encode_data_record('E1BPSBONEW','001',0,2,1,2, my_sdata)"},
	    {"segnam", "mandt", "docnum", "segnum", "psgnum", "hlevel", "sdata"});

	RegisterDocScalarFunction(
	    loader,
	    ScalarFunction("sap_idoc_encode_control", {LogicalType::LIST(LogicalType::VARCHAR)}, LogicalType::BLOB,
	                   EncodeControlFun),
	    "Compose a 524-byte EDI_DC40 control record (BLOB) from up to 36 field values given in EDI_DC40 "
	    "order (tabnam, mandt, docnum, docrel, …, serial); missing/short values are space-padded.",
	    {"SELECT sap_idoc_encode_control(['EDI_DC40','001', …])"}, {"values"});
}

} // namespace duckdb
