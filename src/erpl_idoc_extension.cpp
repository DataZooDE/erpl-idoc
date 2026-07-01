#define DUCKDB_EXTENSION_MAIN

#include "erpl_idoc_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"
#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>

namespace duckdb {

// Minimal M0 smoke function: proves the extension loads and registers.
// Real IDoc reader/writer/COPY functions are registered in later milestones.
inline void IdocVersionScalarFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &name_vector = args.data[0];
	UnaryExecutor::Execute<string_t, string_t>(name_vector, result, args.size(), [&](string_t name) {
		return StringVector::AddString(result, "erpl_idoc " + name.GetString());
	});
}

static void LoadInternal(ExtensionLoader &loader) {
	loader.SetDescription("SAP IDoc flat-file reader and writer for DuckDB — read IDoc files as tables and "
	                      "emit byte-valid IDoc files from SQL. Offline core; live-SAP access via erpl_rfc.");

	auto idoc_version_fun =
	    ScalarFunction("idoc_version", {LogicalType::VARCHAR}, LogicalType::VARCHAR, IdocVersionScalarFun);
	loader.RegisterFunction(idoc_version_fun);
}

void ErplIdocExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}
std::string ErplIdocExtension::Name() {
	return "erpl_idoc";
}

std::string ErplIdocExtension::Version() const {
#ifdef EXT_VERSION_ERPL_IDOC
	return EXT_VERSION_ERPL_IDOC;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(erpl_idoc, loader) {
	duckdb::LoadInternal(loader);
}
}
