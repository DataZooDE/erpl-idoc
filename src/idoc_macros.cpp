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

namespace {

// CreateInternalMacroInfo / CreateTableMacroInfo build the info but leave
// `descriptions` empty, which is why macros normally arrive undocumented even though
// CreateMacroInfo derives from CreateFunctionInfo like everything else. Pushing the
// description on before registering is all that is needed.
//
// parameter_names is deliberately NOT set: a macro already reports its real parameter
// names, and a non-empty parameter_names would replace the whole list.
FunctionDescription MacroDoc(string description, vector<string> examples) {
	FunctionDescription d;
	d.description = std::move(description);
	d.examples = std::move(examples);
	d.categories = {"sap", "idoc"};
	return d;
}

} // namespace

void RegisterIdocMacros(ExtensionLoader &loader) {
	// Descriptions restate the header comment above, which is where the two-macro
	// split is explained.
	const char *scalar_docs[] = {
	    "Build the IDOCTYPE_READ_COMPLETE import-parameter struct that sap_idoc_dictionary passes to "
	    "sap_rfc_invoke. Separate from sap_idoc_dictionary because sap_rfc_invoke executes the RFC at "
	    "BIND time, so its argument must already be constant there; calling this with literals folds to "
	    "one."};
	const char *scalar_examples[] = {"sap_idoc_params('FLIGHTBOOKING_CREATEFROMDAT01')"};
	for (idx_t i = 0; IDOC_SCALAR_MACROS[i].name != nullptr; i++) {
		auto info = DefaultFunctionGenerator::CreateInternalMacroInfo(IDOC_SCALAR_MACROS[i]);
		info->descriptions.push_back(MacroDoc(scalar_docs[i], {scalar_examples[i]}));
		loader.RegisterFunction(*info);
	}

	const char *table_docs[] = {
	    "Fetch and normalise a segment dictionary (SPEC B4 schema: segnam, field_pos, field_name, offset, "
	    "length, datatype) from a live SAP system over erpl_rfc. 'params' must be a struct carrying "
	    "PI_IDOCTYP/PI_CIMTYP/PI_VERSION -- build it with sap_idoc_params. Requires erpl_rfc to be loaded; "
	    "on a SAP-less host use a persisted dictionary file instead."};
	const char *table_examples[] = {
	    "SELECT * FROM sap_idoc_dictionary(sap_idoc_params('FLIGHTBOOKING_CREATEFROMDAT01'));"};
	for (idx_t i = 0; IDOC_TABLE_MACROS[i].name != nullptr; i++) {
		auto info = DefaultTableFunctionGenerator::CreateTableMacroInfo(IDOC_TABLE_MACROS[i]);
		info->descriptions.push_back(MacroDoc(table_docs[i], {table_examples[i]}));
		loader.RegisterFunction(*info);
	}
}

} // namespace duckdb
