#include "duckdb.hpp"

#include "idoc_functions.hpp"
#include "idoc_format.hpp"
#include "idoc_multifile.hpp"
#include "idoc_doc.hpp"
#include "telemetry.hpp"

namespace duckdb {

using erpl_idoc::EDI_DC40_FIELDS;
using erpl_idoc::EDI_DD40_FIELDS;
using erpl_idoc::Framing;
using erpl_idoc::GetFieldRaw;
using erpl_idoc::RTrim;

enum class ReaderKind { DATA, CONTROL, RAW };

// Bind data shared by all three readers: the resolved file list + options.
struct IdocReadBindData : public TableFunctionData {
	vector<std::string> files;
	bool has_framing_override = false;
	Framing framing_override = Framing::FIXED;
	bool lenient = false;
	bool with_filename = false;
	std::string encoding = "utf-8";
	ReaderKind kind = ReaderKind::DATA;
};

// Common bind: (VARCHAR path/glob or LIST(VARCHAR)) + named params.
static void ParseCommonBindArgs(ClientContext &context, TableFunctionBindInput &input, IdocReadBindData &bind) {
	bind.files = IdocResolvePaths(context, input.inputs[0]);
	auto &np = input.named_parameters;
	if (np.count("framing") && !np["framing"].IsNull()) {
		bind.has_framing_override = true;
		bind.framing_override = erpl_idoc::FramingFromString(np["framing"].GetValue<string>());
	}
	if (np.count("lenient") && !np["lenient"].IsNull()) {
		bind.lenient = np["lenient"].GetValue<bool>();
	}
	if (np.count("encoding") && !np["encoding"].IsNull()) {
		bind.encoding = np["encoding"].GetValue<string>();
	}
	if (np.count("filename") && !np["filename"].IsNull()) {
		bind.with_filename = np["filename"].GetValue<bool>();
	}
}

static void MaybeAddFilenameColumn(const IdocReadBindData &bind, vector<LogicalType> &return_types,
                                   vector<string> &names) {
	if (bind.with_filename) {
		names.push_back("filename");
		return_types.push_back(LogicalType::VARCHAR);
	}
}

// Init: build the parallel work queue (global) and the per-thread streamer (local).
// Only the control reader may parse IDoc-XML (its records are self-describing);
// data/raw operate on the flat form.
static unique_ptr<GlobalTableFunctionState> IdocInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<IdocReadBindData>();
	return MakeIdocGlobal(context, bind.files, bind.lenient, bind.has_framing_override, bind.framing_override,
	                      bind.kind == ReaderKind::CONTROL);
}
static unique_ptr<LocalTableFunctionState> IdocInitLocal(ExecutionContext &, TableFunctionInitInput &,
                                                         GlobalTableFunctionState *) {
	return MakeIdocLocal();
}

// ---------------------------------------------------------------------------
// sap_idoc_read  — generic long data-record schema
// ---------------------------------------------------------------------------

static unique_ptr<FunctionData> ReadIdocBind(ClientContext &context, TableFunctionBindInput &input,
                                             vector<LogicalType> &return_types, vector<string> &names) {
	auto bind = make_uniq<IdocReadBindData>();
	ParseCommonBindArgs(context, input, *bind);
	bind->kind = ReaderKind::DATA;
	PostHogTelemetry::Instance().CaptureFunctionExecution("sap_idoc_read");

	names = {"document_key", "docnum", "segnum", "segnam", "psgnum", "hlevel", "mandt", "sdata"};
	return_types = {LogicalType::BIGINT,  LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR};
	MaybeAddFilenameColumn(*bind, return_types, names);
	return std::move(bind);
}

static void ReadIdocScan(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind = data_p.bind_data->Cast<IdocReadBindData>();
	auto &g = data_p.global_state->Cast<IdocGlobalState>();
	auto &l = data_p.local_state->Cast<IdocLocalState>();
	idx_t out_row = 0;
	while (out_row < STANDARD_VECTOR_SIZE && IdocPeek(context, g, l)) {
		auto &rec = l.pending;
		if (rec.is_control) {
			IdocConsume(l); // sap_idoc_read only surfaces data records
			continue;
		}
		output.SetValue(0, out_row, Value::BIGINT(rec.document_key));
		output.SetValue(1, out_row, Value(RTrim(GetFieldRaw(rec.bytes, EDI_DD40_FIELDS[2])))); // DOCNUM
		output.SetValue(2, out_row, Value(RTrim(GetFieldRaw(rec.bytes, EDI_DD40_FIELDS[3])))); // SEGNUM
		output.SetValue(3, out_row, Value(RTrim(GetFieldRaw(rec.bytes, EDI_DD40_FIELDS[0])))); // SEGNAM
		output.SetValue(4, out_row, Value(RTrim(GetFieldRaw(rec.bytes, EDI_DD40_FIELDS[4])))); // PSGNUM
		output.SetValue(5, out_row, Value(RTrim(GetFieldRaw(rec.bytes, EDI_DD40_FIELDS[5])))); // HLEVEL
		output.SetValue(6, out_row, Value(RTrim(GetFieldRaw(rec.bytes, EDI_DD40_FIELDS[1])))); // MANDT
		output.SetValue(7, out_row,
		                Value(erpl_idoc::DecodeText(GetFieldRaw(rec.bytes, EDI_DD40_FIELDS[6]), bind.encoding)));
		if (bind.with_filename) {
			output.SetValue(output.ColumnCount() - 1, out_row, Value(l.current_file));
		}
		IdocConsume(l);
		out_row++;
	}
	output.SetCardinality(out_row);
}

// ---------------------------------------------------------------------------
// sap_idoc_read_control  — typed EDI_DC40 (36 fields)
// ---------------------------------------------------------------------------

static unique_ptr<FunctionData> ReadIdocControlBind(ClientContext &context, TableFunctionBindInput &input,
                                                    vector<LogicalType> &return_types, vector<string> &names) {
	auto bind = make_uniq<IdocReadBindData>();
	ParseCommonBindArgs(context, input, *bind);
	bind->kind = ReaderKind::CONTROL;
	PostHogTelemetry::Instance().CaptureFunctionExecution("sap_idoc_read_control");

	names.push_back("document_key");
	return_types.push_back(LogicalType::BIGINT);
	for (auto &f : EDI_DC40_FIELDS) {
		std::string lower;
		for (const char *p = f.name; *p; p++) {
			lower += static_cast<char>(tolower(static_cast<unsigned char>(*p)));
		}
		names.push_back(lower);
		return_types.push_back(LogicalType::VARCHAR);
	}
	MaybeAddFilenameColumn(*bind, return_types, names);
	return std::move(bind);
}

static void ReadIdocControlScan(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind = data_p.bind_data->Cast<IdocReadBindData>();
	auto &g = data_p.global_state->Cast<IdocGlobalState>();
	auto &l = data_p.local_state->Cast<IdocLocalState>();
	idx_t out_row = 0;
	while (out_row < STANDARD_VECTOR_SIZE && IdocPeek(context, g, l)) {
		auto &rec = l.pending;
		if (!rec.is_control) {
			IdocConsume(l);
			continue;
		}
		output.SetValue(0, out_row, Value::BIGINT(rec.document_key));
		for (idx_t i = 0; i < EDI_DC40_FIELDS.size(); i++) {
			auto raw = erpl_idoc::DecodeText(GetFieldRaw(rec.bytes, EDI_DC40_FIELDS[i]), bind.encoding);
			output.SetValue(1 + i, out_row, Value(RTrim(raw)));
		}
		if (bind.with_filename) {
			output.SetValue(output.ColumnCount() - 1, out_row, Value(l.current_file));
		}
		IdocConsume(l);
		out_row++;
	}
	output.SetCardinality(out_row);
}

// ---------------------------------------------------------------------------
// sap_idoc_read_raw  — one row per physical record, exact bytes (round-trip source)
// ---------------------------------------------------------------------------

static unique_ptr<FunctionData> ReadIdocRawBind(ClientContext &context, TableFunctionBindInput &input,
                                                vector<LogicalType> &return_types, vector<string> &names) {
	auto bind = make_uniq<IdocReadBindData>();
	ParseCommonBindArgs(context, input, *bind);
	bind->kind = ReaderKind::RAW;
	PostHogTelemetry::Instance().CaptureFunctionExecution("sap_idoc_read_raw");

	names = {"document_key", "record_index", "record_type", "raw_record"};
	return_types = {LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::VARCHAR, LogicalType::BLOB};
	MaybeAddFilenameColumn(*bind, return_types, names);
	return std::move(bind);
}

static void ReadIdocRawScan(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind = data_p.bind_data->Cast<IdocReadBindData>();
	auto &g = data_p.global_state->Cast<IdocGlobalState>();
	auto &l = data_p.local_state->Cast<IdocLocalState>();
	idx_t out_row = 0;
	while (out_row < STANDARD_VECTOR_SIZE && IdocPeek(context, g, l)) {
		auto &rec = l.pending;
		output.SetValue(0, out_row, Value::BIGINT(rec.document_key));
		output.SetValue(1, out_row, Value::BIGINT(rec.record_index));
		output.SetValue(2, out_row, Value(rec.is_control ? "C" : "D"));
		output.SetValue(3, out_row, Value::BLOB_RAW(rec.bytes));
		if (bind.with_filename) {
			output.SetValue(output.ColumnCount() - 1, out_row, Value(l.current_file));
		}
		IdocConsume(l);
		out_row++;
	}
	output.SetCardinality(out_row);
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

static void AddReaderParams(TableFunction &f) {
	f.named_parameters["framing"] = LogicalType::VARCHAR;  // 'fixed' | 'lf' | 'crlf'
	f.named_parameters["lenient"] = LogicalType::BOOLEAN;  // salvage complete records
	f.named_parameters["encoding"] = LogicalType::VARCHAR; // 'utf-8' (default) | 'latin-1'
	f.named_parameters["filename"] = LogicalType::BOOLEAN; // add a source-file column
}

// Build an overload set for a reader: the first argument is either a VARCHAR
// path/glob or a LIST(VARCHAR) of paths/globs; everything else is shared.
static TableFunctionSet MakeReaderSet(const string &name, table_function_t scan, table_function_bind_t bind,
                                      table_function_init_global_t init) {
	TableFunctionSet set(name);
	for (auto &first : vector<LogicalType> {LogicalType::VARCHAR, LogicalType::LIST(LogicalType::VARCHAR)}) {
		TableFunction f(name, {first}, scan, bind, init, IdocInitLocal);
		AddReaderParams(f);
		set.AddFunction(std::move(f));
	}
	return set;
}

void RegisterIdocReaderFunctions(ExtensionLoader &loader) {
	RegisterDocTableFunctionSet(
	    loader, MakeReaderSet("sap_idoc_read", ReadIdocScan, ReadIdocBind, IdocInitGlobal),
	    "Read one or many SAP IDoc flat files as generic long rows: one row per data (EDI_DD40) record with "
	    "document_key, docnum, segnum, segnam, psgnum, hlevel, mandt and the raw 1000-char SDATA. The path may "
	    "be a single file, a glob ('dir/*.idoc', 's3://bucket/idocs/*.idoc') resolved over DuckDB's filesystem, "
	    "or a LIST of paths. Framing is auto-detected; 'lenient' salvages truncated files, 'encoding' decodes "
	    "non-UTF-8 SDATA, and 'filename := true' adds the source-file column.",
	    {"SELECT * FROM sap_idoc_read('flight.idoc')",
	     "SELECT filename, segnam FROM sap_idoc_read('corpus/*.idoc', filename=true)",
	     "SELECT * FROM sap_idoc_read(['a.idoc', 'b.idoc'])"},
	    {"path"});

	RegisterDocTableFunctionSet(
	    loader, MakeReaderSet("sap_idoc_read_control", ReadIdocControlScan, ReadIdocControlBind, IdocInitGlobal),
	    "Read the typed control record(s) of one or many SAP IDoc flat (or XML) files — all 36 EDI_DC40 fields "
	    "(tabnam, docnum, idoctyp, mestyp, sndprn, rcvprn, …) plus a document_key. One row per IDoc; accepts a "
	    "single path, a glob, or a LIST of paths, and 'filename := true'.",
	    {"SELECT idoctyp, mestyp, sndprn FROM sap_idoc_read_control('flight.idoc')",
	     "SELECT filename, idoctyp FROM sap_idoc_read_control('corpus/*.idoc', filename=true)"},
	    {"path"});

	RegisterDocTableFunctionSet(
	    loader, MakeReaderSet("sap_idoc_read_raw", ReadIdocRawScan, ReadIdocRawBind, IdocInitGlobal),
	    "Read every physical record of one or many SAP IDoc flat files with exact bytes: document_key, "
	    "record_index, record_type ('C' control / 'D' data) and raw_record (BLOB). Byte-exact source for the "
	    "writer — COPY (…) TO … (FORMAT sap_idoc). Accepts a single path, a glob, or a LIST, and 'filename := true'.",
	    {"COPY (SELECT raw_record FROM sap_idoc_read_raw('f.idoc') ORDER BY record_index) TO 'g.idoc' (FORMAT "
	     "sap_idoc)"},
	    {"path"});
}

} // namespace duckdb
