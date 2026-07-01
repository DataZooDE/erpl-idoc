#pragma once

#include "duckdb.hpp"

namespace duckdb {

// Reader table functions:
//   read_idoc(path)          -> generic long data-record schema (analytics view)
//   read_idoc_control(path)  -> typed EDI_DC40 control record (36 fields)
//   read_idoc_raw(path)      -> one row per physical record with exact bytes
//                               (the byte-exact round-trip source for COPY)
void RegisterIdocReaderFunctions(ExtensionLoader &loader);

// Writer: COPY (<relation>) TO '<path>' (FORMAT idoc [, framing 'fixed'|'lf'|'crlf'])
// Consumes a single BLOB/VARCHAR column of raw records and frames them to a file.
void RegisterIdocCopyFunction(ExtensionLoader &loader);

} // namespace duckdb
