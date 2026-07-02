#include "duckdb.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/file_open_flags.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/materialized_query_result.hpp"
#include "duckdb/common/string_util.hpp"

#include "idoc_functions.hpp"
#include "idoc_format.hpp"
#include "idoc_dict_source.hpp"
#include "idoc_doc.hpp"
#include "telemetry.hpp"
#include "idoc_xml.hpp"

#include <map>

namespace duckdb {

using erpl_idoc::EmitIdocXml;
using erpl_idoc::ParseIdocXml;
using erpl_idoc::XmlField;
using erpl_idoc::XmlFieldValue;
using erpl_idoc::XmlIdoc;
using erpl_idoc::XmlSegment;

static std::string XmlReadWholeFile(ClientContext &context, const std::string &path) {
	auto &fs = FileSystem::GetFileSystem(context);
	auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ);
	auto size = static_cast<idx_t>(handle->GetFileSize());
	std::string buf;
	buf.resize(size);
	idx_t total = 0;
	while (total < size) {
		auto n = handle->Read(reinterpret_cast<void *>(&buf[total]), size - total);
		if (n <= 0) {
			throw IOException("short read on '%s'", path);
		}
		total += static_cast<idx_t>(n);
	}
	return buf;
}

// One field slicing rule from the dictionary.
struct FieldRule {
	std::string field_name;
	int64_t offset;
	int64_t length;
};
// Load dictionary rules grouped by segment, ordered by field_pos.
static std::map<std::string, vector<FieldRule>> LoadDictRules(ClientContext &context, const std::string &dict) {
	Connection con(*context.db);
	auto res = con.Query("SELECT segnam, field_name, \"offset\", length FROM " + DictSource(dict) +
	                     " ORDER BY segnam, field_pos");
	if (res->HasError()) {
		throw BinderException("failed to read dictionary: " + res->GetError());
	}
	std::map<std::string, vector<FieldRule>> rules;
	for (idx_t i = 0; i < res->RowCount(); i++) {
		auto seg = res->GetValue(0, i).ToString();
		FieldRule r;
		r.field_name = res->GetValue(1, i).ToString();
		r.offset = res->GetValue(2, i).GetValue<int64_t>();
		r.length = res->GetValue(3, i).GetValue<int64_t>();
		rules[seg].push_back(std::move(r));
	}
	return rules;
}

// ===========================================================================
// sap_idoc_read_xml(path) — generic long: one row per field (control + segments)
// ===========================================================================
struct XmlPathBindData : public TableFunctionData {
	std::string path;
};
struct XmlLongRow {
	int64_t document_key;
	int64_t seq;
	std::string segnam;
	int32_t hlevel;
	std::string field_name;
	std::string value;
};
struct XmlLongGlobalState : public GlobalTableFunctionState {
	vector<XmlLongRow> rows;
	idx_t cursor = 0;
	idx_t MaxThreads() const override {
		return 1;
	}
};

static unique_ptr<FunctionData> ReadXmlBind(ClientContext &, TableFunctionBindInput &input,
                                            vector<LogicalType> &return_types, vector<string> &names) {
	auto bind = make_uniq<XmlPathBindData>();
	bind->path = input.inputs[0].GetValue<string>();
	PostHogTelemetry::Instance().CaptureFunctionExecution("sap_idoc_read_xml");
	names = {"document_key", "seq", "segnam", "hlevel", "field_name", "value"};
	return_types = {LogicalType::BIGINT,  LogicalType::BIGINT,  LogicalType::VARCHAR,
	                LogicalType::INTEGER, LogicalType::VARCHAR, LogicalType::VARCHAR};
	return std::move(bind);
}
static unique_ptr<GlobalTableFunctionState> ReadXmlInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<XmlPathBindData>();
	auto xml = XmlReadWholeFile(context, bind.path);
	auto idocs = ParseIdocXml(xml);
	auto state = make_uniq<XmlLongGlobalState>();
	int64_t dk = 0;
	for (auto &idoc : idocs) {
		dk++;
		int64_t seq = 0;
		for (auto &f : idoc.control) {
			state->rows.push_back(XmlLongRow{dk, seq, "EDI_DC40", 0, f.name, f.value});
		}
		for (auto &seg : idoc.segments) {
			seq++;
			if (seg.fields.empty()) {
				// still surface the segment (a field-less segment, e.g. all-blank fields)
				state->rows.push_back(XmlLongRow{dk, seq, seg.segnam, seg.hlevel, std::string(), std::string()});
				continue;
			}
			for (auto &f : seg.fields) {
				state->rows.push_back(XmlLongRow{dk, seq, seg.segnam, seg.hlevel, f.name, f.value});
			}
		}
	}
	return std::move(state);
}
static void ReadXmlScan(ClientContext &, TableFunctionInput &data_p, DataChunk &output) {
	auto &gs = data_p.global_state->Cast<XmlLongGlobalState>();
	idx_t n = 0;
	while (gs.cursor < gs.rows.size() && n < STANDARD_VECTOR_SIZE) {
		auto &r = gs.rows[gs.cursor++];
		output.SetValue(0, n, Value::BIGINT(r.document_key));
		output.SetValue(1, n, Value::BIGINT(r.seq));
		output.SetValue(2, n, Value(r.segnam));
		output.SetValue(3, n, Value::INTEGER(r.hlevel));
		output.SetValue(4, n, Value(r.field_name));
		output.SetValue(5, n, Value(erpl_idoc::RTrim(r.value)));
		n++;
	}
	output.SetCardinality(n);
}

// ===========================================================================
// sap_idoc_to_xml(flat_path, dict) — convert a flat IDoc file to IDoc-XML (one row).
// ===========================================================================
struct FlatDictBindData : public TableFunctionData {
	std::string path;
	std::string dict;
};
struct SingleStringGlobalState : public GlobalTableFunctionState {
	std::string value;
	bool emitted = false;
	idx_t MaxThreads() const override {
		return 1;
	}
};
static unique_ptr<FunctionData> ToXmlBind(ClientContext &, TableFunctionBindInput &input,
                                          vector<LogicalType> &return_types, vector<string> &names) {
	auto bind = make_uniq<FlatDictBindData>();
	bind->path = input.inputs[0].GetValue<string>();
	bind->dict = input.inputs[1].GetValue<string>();
	PostHogTelemetry::Instance().CaptureFunctionExecution("sap_idoc_to_xml");
	names = {"xml"};
	return_types = {LogicalType::VARCHAR};
	return std::move(bind);
}
static unique_ptr<GlobalTableFunctionState> ToXmlInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<FlatDictBindData>();
	auto data = XmlReadWholeFile(context, bind.path);
	auto parsed = erpl_idoc::ParseImageAuto(data);
	auto rules = LoadDictRules(context, bind.dict);

	std::map<int64_t, XmlIdoc> by_doc;
	for (auto &rec : parsed.records) {
		auto &idoc = by_doc[rec.document_key];
		if (rec.is_control) {
			for (auto &fs : erpl_idoc::EDI_DC40_FIELDS) {
				idoc.control.push_back(XmlField{fs.name, erpl_idoc::RTrim(erpl_idoc::GetFieldRaw(rec.bytes, fs))});
			}
		} else {
			XmlSegment seg;
			seg.segnam = erpl_idoc::RTrim(erpl_idoc::GetFieldRaw(rec.bytes, erpl_idoc::EDI_DD40_FIELDS[0]));
			auto hl = erpl_idoc::RTrim(erpl_idoc::GetFieldRaw(rec.bytes, erpl_idoc::EDI_DD40_FIELDS[5]));
			seg.hlevel = hl.empty() ? 1 : std::stoi(hl);
			auto sdata = erpl_idoc::GetFieldRaw(rec.bytes, erpl_idoc::EDI_DD40_FIELDS[6]);
			auto it = rules.find(seg.segnam);
			if (it != rules.end()) {
				for (auto &fr : it->second) {
					std::string val;
					if (fr.offset >= 0 && fr.length >= 0 &&
					    static_cast<size_t>(fr.offset + fr.length) <= sdata.size()) {
						val = erpl_idoc::RTrim(sdata.substr(fr.offset, fr.length));
					}
					seg.fields.push_back(XmlField{fr.field_name, val});
				}
			}
			idoc.segments.push_back(std::move(seg));
		}
	}
	vector<XmlIdoc> idocs;
	for (auto &kv : by_doc) {
		idocs.push_back(std::move(kv.second));
	}
	auto state = make_uniq<SingleStringGlobalState>();
	state->value = EmitIdocXml(idocs);
	return std::move(state);
}
static void ToXmlScan(ClientContext &, TableFunctionInput &data_p, DataChunk &output) {
	auto &gs = data_p.global_state->Cast<SingleStringGlobalState>();
	if (gs.emitted) {
		output.SetCardinality(0);
		return;
	}
	gs.emitted = true;
	output.SetValue(0, 0, Value(gs.value));
	output.SetCardinality(1);
}

// ===========================================================================
// sap_idoc_xml_to_records(xml_path, dict) — convert IDoc-XML to flat records
// (document_key, record_index, record_type, raw_record BLOB) for COPY (FORMAT sap_idoc).
// ===========================================================================
struct RawRec {
	int64_t document_key;
	int64_t record_index;
	bool is_control;
	std::string bytes;
};
struct RawRecGlobalState : public GlobalTableFunctionState {
	vector<RawRec> recs;
	idx_t cursor = 0;
	idx_t MaxThreads() const override {
		return 1;
	}
};
static unique_ptr<FunctionData> XmlToRecBind(ClientContext &, TableFunctionBindInput &input,
                                             vector<LogicalType> &return_types, vector<string> &names) {
	auto bind = make_uniq<FlatDictBindData>();
	bind->path = input.inputs[0].GetValue<string>();
	bind->dict = input.inputs[1].GetValue<string>();
	PostHogTelemetry::Instance().CaptureFunctionExecution("sap_idoc_xml_to_records");
	names = {"document_key", "record_index", "record_type", "raw_record"};
	return_types = {LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::VARCHAR, LogicalType::BLOB};
	return std::move(bind);
}
static unique_ptr<GlobalTableFunctionState> XmlToRecInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<FlatDictBindData>();
	auto xml = XmlReadWholeFile(context, bind.path);
	auto idocs = ParseIdocXml(xml);
	auto rules = LoadDictRules(context, bind.dict);

	auto state = make_uniq<RawRecGlobalState>();
	int64_t dk = 0, ri = 0;
	for (auto &idoc : idocs) {
		dk++;
		// control record: 36 EDI_DC40 fields in order, values from XML (by name)
		vector<std::string> cvals;
		for (auto &fs : erpl_idoc::EDI_DC40_FIELDS) {
			cvals.push_back(XmlFieldValue(idoc.control, fs.name));
		}
		state->recs.push_back(RawRec{dk, ri++, true, erpl_idoc::EncodeControl(cvals)});

		auto docnum = XmlFieldValue(idoc.control, "DOCNUM");
		int64_t docnum_i = docnum.empty() ? 0 : std::stoll(docnum);
		auto mandt = XmlFieldValue(idoc.control, "MANDT");

		// recompute SEGNUM (sequential) and PSGNUM (parent = last segnum at hlevel-1)
		std::map<int, int64_t> last_at_level;
		int64_t segnum = 0;
		for (auto &seg : idoc.segments) {
			segnum++;
			int64_t psgnum = 0;
			auto pit = last_at_level.find(seg.hlevel - 1);
			if (seg.hlevel > 1 && pit != last_at_level.end()) {
				psgnum = pit->second;
			}
			last_at_level[seg.hlevel] = segnum;

			// SDATA from dict rules + segment field values (by name)
			std::string sdata(erpl_idoc::SDATA_LEN, ' ');
			auto it = rules.find(seg.segnam);
			if (it != rules.end()) {
				vector<int64_t> offs, lens;
				vector<std::string> vals;
				for (auto &fr : it->second) {
					offs.push_back(fr.offset);
					lens.push_back(fr.length);
					vals.push_back(XmlFieldValue(seg.fields, fr.field_name));
				}
				sdata = erpl_idoc::EncodeSdata(offs, lens, vals);
			}
			auto rec = erpl_idoc::EncodeDataRecord(seg.segnam, mandt, docnum_i, segnum, psgnum, seg.hlevel, sdata);
			state->recs.push_back(RawRec{dk, ri++, false, std::move(rec)});
		}
	}
	return std::move(state);
}
static void XmlToRecScan(ClientContext &, TableFunctionInput &data_p, DataChunk &output) {
	auto &gs = data_p.global_state->Cast<RawRecGlobalState>();
	idx_t n = 0;
	while (gs.cursor < gs.recs.size() && n < STANDARD_VECTOR_SIZE) {
		auto &r = gs.recs[gs.cursor++];
		output.SetValue(0, n, Value::BIGINT(r.document_key));
		output.SetValue(1, n, Value::BIGINT(r.record_index));
		output.SetValue(2, n, Value(r.is_control ? "C" : "D"));
		output.SetValue(3, n, Value::BLOB_RAW(r.bytes));
		n++;
	}
	output.SetCardinality(n);
}

void RegisterIdocXmlFunctions(ExtensionLoader &loader) {
	RegisterDocTableFunction(
	    loader, TableFunction("sap_idoc_read_xml", {LogicalType::VARCHAR}, ReadXmlScan, ReadXmlBind, ReadXmlInit),
	    "Read an IDoc-XML file as generic long rows: document_key, seq, segnam, hlevel, field_name, value "
	    "(control fields appear as segnam='EDI_DC40', hlevel 0). IDoc-XML is self-describing, so no dictionary "
	    "is needed.",
	    {"SELECT * FROM sap_idoc_read_xml('flight.xml')"}, {"path"});

	RegisterDocTableFunction(
	    loader,
	    TableFunction("sap_idoc_to_xml", {LogicalType::VARCHAR, LogicalType::VARCHAR}, ToXmlScan, ToXmlBind, ToXmlInit),
	    "Convert a flat IDoc file to IDoc-XML (returns one row with the XML text). The dictionary names each "
	    "SDATA field. Inverse of sap_idoc_xml_to_records.",
	    {"SELECT xml FROM sap_idoc_to_xml('flight.idoc', 'flight_dict.csv')"}, {"flat_path", "dict"});

	RegisterDocTableFunction(
	    loader,
	    TableFunction("sap_idoc_xml_to_records", {LogicalType::VARCHAR, LogicalType::VARCHAR}, XmlToRecScan,
	                  XmlToRecBind, XmlToRecInit),
	    "Convert an IDoc-XML file to flat records (document_key, record_index, record_type, raw_record BLOB), "
	    "recomputing SEGNUM/PSGNUM from the XML nesting and packing SDATA per the dictionary. Feed raw_record "
	    "to COPY (FORMAT sap_idoc) to write a flat file.",
	    {"COPY (SELECT raw_record FROM sap_idoc_xml_to_records('flight.xml','flight_dict.csv') ORDER BY "
	     "record_index) TO 'flight.idoc' (FORMAT sap_idoc)"},
	    {"xml_path", "dict"});
}

} // namespace duckdb
