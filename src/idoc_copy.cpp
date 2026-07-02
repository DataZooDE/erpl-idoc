#include "duckdb.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/file_open_flags.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/function/copy_function.hpp"

#include "idoc_functions.hpp"
#include "idoc_format.hpp"

namespace duckdb {

using erpl_idoc::Framing;

// ---------------------------------------------------------------------------
// COPY (<relation>) TO '<path>' (FORMAT idoc [, framing 'fixed'|'lf'|'crlf'])
//
// The relation must be a single BLOB/VARCHAR column of raw records (as produced by
// sap_idoc_read_raw, or composed by the typed writer). The writer frames them into a
// file. With FIXED framing and exact-length records this is the byte-exact inverse
// of sap_idoc_read_raw (SPEC B9 round-trip identity).
// ---------------------------------------------------------------------------

struct IdocWriteBindData : public TableFunctionData {
	Framing framing = Framing::FIXED;
	bool validate = true; // reject records that are not 524 (control) or 1063 (data) bytes
};

struct IdocWriteGlobalState : public GlobalFunctionData {
	unique_ptr<FileHandle> handle;
	mutex lock;
};

struct IdocWriteLocalState : public LocalFunctionData {};

static string ParseSingleStringOption(const vector<Value> &values, const string &key) {
	if (values.size() != 1 || values[0].IsNull()) {
		throw BinderException("COPY (FORMAT sap_idoc): option \"%s\" expects a single string value", key);
	}
	return values[0].GetValue<string>();
}

static unique_ptr<FunctionData> IdocWriteBind(ClientContext &context, CopyFunctionBindInput &input,
                                              const vector<string> &names, const vector<LogicalType> &sql_types) {
	if (sql_types.size() != 1 ||
	    (sql_types[0].id() != LogicalTypeId::BLOB && sql_types[0].id() != LogicalTypeId::VARCHAR)) {
		throw BinderException(
		    "COPY (FORMAT sap_idoc) expects a single BLOB or VARCHAR column of raw IDoc records "
		    "(e.g. SELECT raw_record FROM sap_idoc_read_raw(...))");
	}
	auto result = make_uniq<IdocWriteBindData>();
	for (auto &opt : input.info.options) {
		if (StringUtil::CIEquals(opt.first, "framing")) {
			result->framing = erpl_idoc::FramingFromString(ParseSingleStringOption(opt.second, opt.first));
		} else if (StringUtil::CIEquals(opt.first, "validate")) {
			result->validate = opt.second.empty() || opt.second[0].IsNull() || BooleanValue::Get(opt.second[0]);
		} else {
			throw BinderException("COPY (FORMAT sap_idoc): unrecognized option \"%s\"", opt.first);
		}
	}
	return std::move(result);
}

static unique_ptr<GlobalFunctionData> IdocWriteInitGlobal(ClientContext &context, FunctionData &bind_data,
                                                          const string &file_path) {
	auto &fs = FileSystem::GetFileSystem(context);
	auto flags = FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE_NEW;
	auto result = make_uniq<IdocWriteGlobalState>();
	result->handle = fs.OpenFile(file_path, flags);
	return std::move(result);
}

static unique_ptr<LocalFunctionData> IdocWriteInitLocal(ExecutionContext &context, FunctionData &bind_data) {
	return make_uniq_base<LocalFunctionData, IdocWriteLocalState>();
}

static void WriteAll(FileHandle &handle, QueryContext &qc, const char *ptr, idx_t len) {
	idx_t written_total = 0;
	while (written_total < len) {
		auto n = handle.Write(qc, const_cast<char *>(ptr) + written_total, len - written_total);
		if (n <= 0) {
			throw IOException("Failed to write IDoc file");
		}
		written_total += static_cast<idx_t>(n);
	}
}

static void IdocWriteSink(ExecutionContext &context, FunctionData &bind_data, GlobalFunctionData &gstate,
                          LocalFunctionData &lstate, DataChunk &input) {
	auto &bind = bind_data.Cast<IdocWriteBindData>();
	auto &state = gstate.Cast<IdocWriteGlobalState>();
	lock_guard<mutex> glock(state.lock);
	if (!state.handle) {
		throw IOException("COPY (FORMAT sap_idoc): write attempted after file was finalized");
	}

	const char *term = (bind.framing == Framing::TERMINATED_CRLF) ? "\r\n"
	                   : (bind.framing == Framing::TERMINATED_LF)  ? "\n"
	                                                               : "";
	const idx_t term_len = strlen(term);

	UnifiedVectorFormat vdata;
	input.data[0].ToUnifiedFormat(input.size(), vdata);
	const auto records = UnifiedVectorFormat::GetData<string_t>(vdata);

	QueryContext qc(context.client);
	for (idx_t row = 0; row < input.size(); row++) {
		auto idx = vdata.sel->get_index(row);
		if (!vdata.validity.RowIsValid(idx)) {
			continue;
		}
		auto &rec = records[idx];
		if (bind.validate) {
			auto n = rec.GetSize();
			if (n != erpl_idoc::CONTROL_RECORD_LEN && n != erpl_idoc::DATA_RECORD_LEN) {
				throw InvalidInputException(
				    "COPY (FORMAT sap_idoc): record is %llu bytes; expected 524 (control) or 1063 (data). "
				    "Pass (validate false) to write non-standard records.",
				    (unsigned long long)n);
			}
		}
		WriteAll(*state.handle, qc, rec.GetData(), rec.GetSize());
		if (term_len) {
			WriteAll(*state.handle, qc, term, term_len);
		}
	}
}

static void IdocWriteCombine(ExecutionContext &context, FunctionData &bind_data, GlobalFunctionData &gstate,
                             LocalFunctionData &lstate) {
}

static void IdocWriteFinalize(ClientContext &context, FunctionData &bind_data, GlobalFunctionData &gstate) {
	auto &state = gstate.Cast<IdocWriteGlobalState>();
	lock_guard<mutex> glock(state.lock);
	if (state.handle) {
		state.handle->Close();
		state.handle.reset();
	}
}

void RegisterIdocCopyFunction(ExtensionLoader &loader) {
	CopyFunction info("sap_idoc");
	info.copy_to_bind = IdocWriteBind;
	info.copy_to_initialize_global = IdocWriteInitGlobal;
	info.copy_to_initialize_local = IdocWriteInitLocal;
	info.copy_to_sink = IdocWriteSink;
	info.copy_to_combine = IdocWriteCombine;
	info.copy_to_finalize = IdocWriteFinalize;
	info.extension = "idoc";
	loader.RegisterFunction(info);
}

} // namespace duckdb
