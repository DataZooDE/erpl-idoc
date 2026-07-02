#define DUCKDB_EXTENSION_MAIN

#include "erpl_idoc_extension.hpp"
#include "idoc_functions.hpp"
#include "idoc_doc.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/database.hpp"
#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>

#include "telemetry.hpp"

namespace duckdb {

// Default (public) PostHog project key, shared with the rest of the erpl family.
static const char *ERPL_TELEMETRY_KEY = "phc_t3wwRLtpyEmLHYaZCSszG0MqVr74J6wnCrj9D41zk2t";

static void OnTelemetryEnabled(ClientContext &, SetScope, Value &parameter) {
	PostHogTelemetry::Instance().SetEnabled(parameter.GetValue<bool>());
}
static void OnTelemetryKey(ClientContext &, SetScope, Value &parameter) {
	PostHogTelemetry::Instance().SetAPIKey(parameter.GetValue<std::string>());
}

static void RegisterConfiguration(DatabaseInstance &instance) {
	auto &config = DBConfig::GetConfig(instance);
	config.AddExtensionOption("erpl_telemetry_enabled",
	                          "Enable anonymous ERPL telemetry (opt-out); see https://erpl.io/telemetry.",
	                          LogicalTypeId::BOOLEAN, Value(true), OnTelemetryEnabled);
	config.AddExtensionOption("erpl_telemetry_key", "PostHog project key for ERPL telemetry.",
	                          LogicalTypeId::VARCHAR, Value(ERPL_TELEMETRY_KEY), OnTelemetryKey);
}

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

	// Anonymous, opt-out telemetry (SET erpl_telemetry_enabled=false to disable).
	PostHogTelemetry::Instance().SetAPIKey(ERPL_TELEMETRY_KEY);
	PostHogTelemetry::Instance().CaptureExtensionLoad("erpl_idoc", "0.1.0");
	RegisterConfiguration(loader.GetDatabaseInstance());

	RegisterDocScalarFunction(
	    loader, ScalarFunction("sap_idoc_version", {LogicalType::VARCHAR}, LogicalType::VARCHAR, IdocVersionScalarFun),
	    "Smoke/echo function proving erpl_idoc is loaded; returns 'erpl_idoc <arg>'.",
	    {"SELECT sap_idoc_version('ok')"}, {"tag"});

	RegisterIdocReaderFunctions(loader);
	RegisterIdocTypedReaderFunctions(loader);
	RegisterIdocEncoderFunctions(loader);
	RegisterIdocCopyFunction(loader);
	RegisterIdocMacros(loader);
	RegisterIdocDictFunctions(loader);
	RegisterIdocXmlFunctions(loader);
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
