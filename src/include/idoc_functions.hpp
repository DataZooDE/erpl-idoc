#pragma once

#include "duckdb.hpp"

namespace duckdb {

// Reader table functions:
//   sap_idoc_read(path)          -> generic long data-record schema (analytics view)
//   sap_idoc_read_control(path)  -> typed EDI_DC40 control record (36 fields)
//   sap_idoc_read_raw(path)      -> one row per physical record with exact bytes
//                               (the byte-exact round-trip source for COPY)
void RegisterIdocReaderFunctions(ExtensionLoader &loader);

// Typed reader: sap_idoc_read_segment(path, segnam, dict [, framing]) -> one column per
// dictionary field (values sliced from SDATA per offset/length). `dict` is a
// relation source (a .csv/.parquet path, a table/view name, or a relation
// expression); its origin (live RFC, persisted file, hand-authored) is irrelevant.
void RegisterIdocTypedReaderFunctions(ExtensionLoader &loader);

// Writer: COPY (<relation>) TO '<path>' (FORMAT idoc [, framing 'fixed'|'lf'|'crlf'])
// Consumes a single BLOB/VARCHAR column of raw records and frames them to a file.
void RegisterIdocCopyFunction(ExtensionLoader &loader);

// Typed-write encoders (pure, dict-free — the SQL layer supplies offsets/lengths
// from the dictionary): sap_idoc_encode_sdata / sap_idoc_encode_data_record /
// sap_idoc_encode_control compose raw record bytes that feed COPY (FORMAT idoc).
void RegisterIdocEncoderFunctions(ExtensionLoader &loader);

// SQL macros that compose erpl_rfc (sap_idoc_params + sap_idoc_dictionary). Pure
// SQL — no RFC in C++. Lazily bound, so they load fine without erpl_rfc present.
void RegisterIdocMacros(ExtensionLoader &loader);

// Dictionary helper functions (pure, no RFC):
//   sap_idoc_dict_offsets(dict)     -> dict rows with offset computed from cumulative length
//   sap_idoc_dict_validate(dict)    -> one row per structural problem (empty = sound)
//   sap_idoc_dict_from_fields(...)  -> normalize IDOCTYPE_READ_COMPLETE PT_FIELDS to B4 rows
void RegisterIdocDictFunctions(ExtensionLoader &loader);

} // namespace duckdb
