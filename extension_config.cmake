# This file is included by DuckDB's build system. It specifies which extension to load

# Extension from this repo
duckdb_extension_load(erpl_idoc
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
)

# erpl_rfc is an OPTIONAL runtime dependency (typed-mode dictionary + live-SAP recipes).
# It is NOT linked into erpl_idoc; typed/E2E SQL tests LOAD a prebuilt erpl_rfc binary
# in the same DuckDB session. The generic flat-file core builds and works without it.
