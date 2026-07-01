#include "duckdb.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/file_open_flags.hpp"

#include "idoc_functions.hpp"
#include "idoc_format.hpp"

namespace duckdb {

using erpl_idoc::EDI_DC40_FIELDS;
using erpl_idoc::EDI_DD40_FIELDS;
using erpl_idoc::Framing;
using erpl_idoc::GetFieldRaw;
using erpl_idoc::ParsedIdoc;
using erpl_idoc::RTrim;

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

// Read a whole file through DuckDB's virtual filesystem (works with local paths,
// globs resolved by the caller, httpfs, etc.).
static std::string ReadWholeFile(ClientContext &context, const std::string &path) {
	auto &fs = FileSystem::GetFileSystem(context);
	auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ);
	auto size = static_cast<idx_t>(handle->GetFileSize());
	std::string buffer;
	buffer.resize(size);
	idx_t read_total = 0;
	while (read_total < size) {
		auto n = handle->Read(reinterpret_cast<void *>(&buffer[read_total]), size - read_total);
		if (n <= 0) {
			break;
		}
		read_total += static_cast<idx_t>(n);
	}
	buffer.resize(read_total);
	return buffer;
}

// Bind data shared by all three readers: the file path and an optional framing override.
struct IdocReadBindData : public TableFunctionData {
	std::string path;
	bool has_framing_override = false;
	Framing framing_override = Framing::FIXED;
};

// Global state: parse the whole file once, then stream rows out in chunks.
struct IdocReadGlobalState : public GlobalTableFunctionState {
	ParsedIdoc parsed;
	idx_t cursor = 0; // index into `parsed.records`
	idx_t MaxThreads() const override {
		return 1;
	}
};

static Framing resolve_framing(const std::string &data, const IdocReadBindData &bind) {
	if (bind.has_framing_override) {
		return bind.framing_override;
	}
	return erpl_idoc::DetectFraming(data);
}

static unique_ptr<GlobalTableFunctionState> IdocInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<IdocReadBindData>();
	auto data = ReadWholeFile(context, bind.path);
	auto framing = resolve_framing(data, bind);
	auto state = make_uniq<IdocReadGlobalState>();
	state->parsed = erpl_idoc::ParseImage(data, framing);
	return std::move(state);
}

// Common bind: (VARCHAR path) + named param framing.
static void ParseCommonBindArgs(TableFunctionBindInput &input, IdocReadBindData &bind) {
	bind.path = input.inputs[0].GetValue<string>();
	auto it = input.named_parameters.find("framing");
	if (it != input.named_parameters.end() && !it->second.IsNull()) {
		bind.has_framing_override = true;
		bind.framing_override = erpl_idoc::FramingFromString(it->second.GetValue<string>());
	}
}

// ---------------------------------------------------------------------------
// read_idoc  — generic long data-record schema
// ---------------------------------------------------------------------------

static unique_ptr<FunctionData> ReadIdocBind(ClientContext &context, TableFunctionBindInput &input,
                                             vector<LogicalType> &return_types, vector<string> &names) {
	auto bind = make_uniq<IdocReadBindData>();
	ParseCommonBindArgs(input, *bind);

	names = {"document_key", "docnum", "segnum", "segnam", "psgnum", "hlevel", "mandt", "sdata"};
	return_types = {LogicalType::BIGINT,  LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR};
	return std::move(bind);
}

static void ReadIdocScan(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &gstate = data_p.global_state->Cast<IdocReadGlobalState>();
	auto &records = gstate.parsed.records;
	idx_t out_row = 0;
	while (gstate.cursor < records.size() && out_row < STANDARD_VECTOR_SIZE) {
		auto &rec = records[gstate.cursor++];
		if (rec.is_control) {
			continue; // read_idoc only surfaces data records
		}
		output.SetValue(0, out_row, Value::BIGINT(rec.document_key));
		output.SetValue(1, out_row, Value(RTrim(GetFieldRaw(rec.bytes, EDI_DD40_FIELDS[2])))); // DOCNUM
		output.SetValue(2, out_row, Value(RTrim(GetFieldRaw(rec.bytes, EDI_DD40_FIELDS[3])))); // SEGNUM
		output.SetValue(3, out_row, Value(RTrim(GetFieldRaw(rec.bytes, EDI_DD40_FIELDS[0])))); // SEGNAM
		output.SetValue(4, out_row, Value(RTrim(GetFieldRaw(rec.bytes, EDI_DD40_FIELDS[4])))); // PSGNUM
		output.SetValue(5, out_row, Value(RTrim(GetFieldRaw(rec.bytes, EDI_DD40_FIELDS[5])))); // HLEVEL
		output.SetValue(6, out_row, Value(RTrim(GetFieldRaw(rec.bytes, EDI_DD40_FIELDS[1])))); // MANDT
		output.SetValue(7, out_row, Value(GetFieldRaw(rec.bytes, EDI_DD40_FIELDS[6])));        // SDATA (raw 1000)
		out_row++;
	}
	output.SetCardinality(out_row);
}

// ---------------------------------------------------------------------------
// read_idoc_control  — typed EDI_DC40 (36 fields)
// ---------------------------------------------------------------------------

static unique_ptr<FunctionData> ReadIdocControlBind(ClientContext &context, TableFunctionBindInput &input,
                                                    vector<LogicalType> &return_types, vector<string> &names) {
	auto bind = make_uniq<IdocReadBindData>();
	ParseCommonBindArgs(input, *bind);

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
	return std::move(bind);
}

static void ReadIdocControlScan(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &gstate = data_p.global_state->Cast<IdocReadGlobalState>();
	auto &records = gstate.parsed.records;
	idx_t out_row = 0;
	while (gstate.cursor < records.size() && out_row < STANDARD_VECTOR_SIZE) {
		auto &rec = records[gstate.cursor++];
		if (!rec.is_control) {
			continue;
		}
		output.SetValue(0, out_row, Value::BIGINT(rec.document_key));
		for (idx_t i = 0; i < EDI_DC40_FIELDS.size(); i++) {
			output.SetValue(1 + i, out_row, Value(RTrim(GetFieldRaw(rec.bytes, EDI_DC40_FIELDS[i]))));
		}
		out_row++;
	}
	output.SetCardinality(out_row);
}

// ---------------------------------------------------------------------------
// read_idoc_raw  — one row per physical record, exact bytes (round-trip source)
// ---------------------------------------------------------------------------

static unique_ptr<FunctionData> ReadIdocRawBind(ClientContext &context, TableFunctionBindInput &input,
                                                vector<LogicalType> &return_types, vector<string> &names) {
	auto bind = make_uniq<IdocReadBindData>();
	ParseCommonBindArgs(input, *bind);

	names = {"document_key", "record_index", "record_type", "raw_record"};
	return_types = {LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::VARCHAR, LogicalType::BLOB};
	return std::move(bind);
}

static void ReadIdocRawScan(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &gstate = data_p.global_state->Cast<IdocReadGlobalState>();
	auto &records = gstate.parsed.records;
	idx_t out_row = 0;
	while (gstate.cursor < records.size() && out_row < STANDARD_VECTOR_SIZE) {
		auto &rec = records[gstate.cursor++];
		output.SetValue(0, out_row, Value::BIGINT(rec.document_key));
		output.SetValue(1, out_row, Value::BIGINT(rec.record_index));
		output.SetValue(2, out_row, Value(rec.is_control ? "C" : "D"));
		output.SetValue(3, out_row, Value::BLOB_RAW(rec.bytes));
		out_row++;
	}
	output.SetCardinality(out_row);
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void RegisterIdocReaderFunctions(ExtensionLoader &loader) {
	{
		TableFunction f("read_idoc", {LogicalType::VARCHAR}, ReadIdocScan, ReadIdocBind, IdocInitGlobal);
		f.named_parameters["framing"] = LogicalType::VARCHAR;
		loader.RegisterFunction(f);
	}
	{
		TableFunction f("read_idoc_control", {LogicalType::VARCHAR}, ReadIdocControlScan, ReadIdocControlBind,
		                IdocInitGlobal);
		f.named_parameters["framing"] = LogicalType::VARCHAR;
		loader.RegisterFunction(f);
	}
	{
		TableFunction f("read_idoc_raw", {LogicalType::VARCHAR}, ReadIdocRawScan, ReadIdocRawBind, IdocInitGlobal);
		f.named_parameters["framing"] = LogicalType::VARCHAR;
		loader.RegisterFunction(f);
	}
}

} // namespace duckdb
