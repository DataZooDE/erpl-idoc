#include "duckdb.hpp"
#include "duckdb/catalog/default/default_functions.hpp"
#include "duckdb/catalog/default/default_table_functions.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include "idoc_functions.hpp"

namespace duckdb {

// SQL macros shipped as part of the extension. They compose erpl_rfc at the SQL
// layer (no RFC in erpl_idoc's C++). Macros are lazily bound, so registering them
// here is fine even when erpl_rfc is not loaded — they only error if called without
// it (e.g. on a detached, SAP-less host, which uses the persisted dictionary file
// instead).
//
// Ergonomics note: a table macro cannot build the sap_rfc_invoke argument from its
// own parameter, because sap_rfc_invoke executes the RFC at *bind* time — the value
// must already be a constant there. The two-macro split works around this: the
// scalar sap_idoc_params(...) builds the parameter struct from literals at the call
// site (foldable to a constant), and the table macro sap_idoc_dictionary(params)
// passes that struct straight through.
//
//   SELECT * FROM sap_idoc_dictionary(sap_idoc_params('FLIGHTBOOKING_CREATEFROMDAT01'));
//   COPY   (SELECT * FROM sap_idoc_dictionary(sap_idoc_params('MATMAS05')))
//          TO 'matmas05.dict.parquet' (FORMAT parquet);

static const DefaultMacro IDOC_SCALAR_MACROS[] = {
    // Build the IDOCTYPE_READ_COMPLETE import-parameter struct for sap_rfc_invoke.
    {DEFAULT_SCHEMA,
     "sap_idoc_params",
     {"idoctyp", nullptr},
     {{"cimtyp", "''"}, {"version", "'4'"}, {nullptr, nullptr}},
     "struct_pack(\"PI_IDOCTYP\" := idoctyp, \"PI_CIMTYP\" := cimtyp, \"PI_VERSION\" := version)"},
    {nullptr, nullptr, {nullptr}, {{nullptr, nullptr}}, nullptr}};

static const DefaultTableMacro IDOC_TABLE_MACROS[] = {
    // Fetch + normalize a segment dictionary (SPEC B4 schema) from a live system.
    // `params` must be a struct with PI_IDOCTYP/PI_CIMTYP/PI_VERSION (use
    // sap_idoc_params). Struct fields are read with bracket access so they are not
    // mis-parsed as table.column references.
    {DEFAULT_SCHEMA,
     "sap_idoc_dictionary",
     {"params", nullptr},
     {{nullptr, nullptr}},
     "WITH raw AS (SELECT PE_HEADER, PT_FIELDS, PT_SEGMENTS "
     "             FROM sap_rfc_invoke('IDOCTYPE_READ_COMPLETE', params)), "
     "     segs AS (SELECT s.SEGMENTTYP AS seg, s.SEGMENTDEF AS segdef "
     "              FROM raw, UNNEST(PT_SEGMENTS) AS t(s)) "
     "SELECT params['PI_IDOCTYP'] AS idoctyp, params['PI_CIMTYP'] AS cimtyp, "
     "       (SELECT PE_HEADER.RELEASED FROM raw) AS release, "
     "       f.SEGMENTTYP AS segnam, "
     "       (SELECT segdef FROM segs WHERE seg = f.SEGMENTTYP LIMIT 1) AS segdef, "
     "       CAST(f.FIELD_POS AS INTEGER) AS field_pos, f.FIELDNAME AS field_name, "
     "       CAST(f.BYTE_FIRST AS INTEGER) AS \"offset\", CAST(f.EXTLEN AS INTEGER) AS length, "
     "       f.DATATYPE AS datatype, f.ROLLNAME AS data_element, f.DESCRP AS description, "
     "       false AS mandatory "
     "FROM raw, UNNEST(PT_FIELDS) AS t(f) ORDER BY segnam, field_pos"},
    {nullptr, nullptr, {nullptr}, {{nullptr, nullptr}}, nullptr}};

void RegisterIdocMacros(ExtensionLoader &loader) {
	for (idx_t i = 0; IDOC_SCALAR_MACROS[i].name != nullptr; i++) {
		auto info = DefaultFunctionGenerator::CreateInternalMacroInfo(IDOC_SCALAR_MACROS[i]);
		loader.RegisterFunction(*info);
	}
	for (idx_t i = 0; IDOC_TABLE_MACROS[i].name != nullptr; i++) {
		auto info = DefaultTableFunctionGenerator::CreateTableMacroInfo(IDOC_TABLE_MACROS[i]);
		loader.RegisterFunction(*info);
	}
}

} // namespace duckdb
