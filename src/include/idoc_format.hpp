#pragma once

// Pure, offline IDoc flat-file format core.
//
// This header intentionally has NO DuckDB and NO SAP/RFC dependencies — it models
// the byte layout of SAP IDoc flat files (control record EDI_DC40, data record
// EDI_DD40), record framing, fixed-width field slicing/emitting, and derived-field
// math. It is unit-testable in isolation (Catch2, test/cpp) and is the byte-exact
// round-trip anchor (SPEC NFR-1 / B9).

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace erpl_idoc {

// ---- Record geometry (verified from the flight.idoc fixture, VALIDATION.md §3) ----
constexpr size_t CONTROL_RECORD_LEN = 524;  // EDI_DC40
constexpr size_t DATA_RECORD_LEN = 1063;    // EDI_DD40 = 63-byte header + 1000-byte SDATA
constexpr size_t DATA_HEADER_LEN = 63;
constexpr size_t SDATA_LEN = 1000;

// A fixed-width field within a record.
struct FieldSpec {
	const char *name;
	size_t offset;
	size_t length;
};

// EDI_DC40 control record: 36 fields, total 524 bytes.
constexpr size_t EDI_DC40_FIELD_COUNT = 36;
extern const std::array<FieldSpec, EDI_DC40_FIELD_COUNT> EDI_DC40_FIELDS;

// EDI_DD40 data record header: 7 fields (last is the 1000-byte SDATA), total 1063 bytes.
constexpr size_t EDI_DD40_FIELD_COUNT = 7;
extern const std::array<FieldSpec, EDI_DD40_FIELD_COUNT> EDI_DD40_FIELDS;

// Framing variants (SPEC B8 / FR-R4).
enum class Framing {
	FIXED,           // contiguous fixed-width records, no separators (as validated)
	TERMINATED_LF,   // each record followed by '\n'
	TERMINATED_CRLF, // each record followed by "\r\n"
};

// True if a record's bytes are a control record (TABNAM begins with "EDI_DC").
bool IsControlRecord(const std::string &record);

// Detect framing of a whole IDoc file image. Throws std::runtime_error if the bytes
// match no known framing (used by auto-detect; callers may override).
Framing DetectFraming(const std::string &data);

// Split a file image into raw record byte-strings (terminators stripped), using the
// given framing. For FIXED, record boundaries are walked by record type: a record
// starting with "EDI_DC" is 524 bytes, otherwise 1063. Throws on misalignment /
// truncation (bounds-checked; safe on untrusted input — NFR-7).
std::vector<std::string> SplitRecords(const std::string &data, Framing framing);

// Re-join raw records into a file image with the given framing (inverse of Split).
std::string JoinRecords(const std::vector<std::string> &records, Framing framing);

// Raw fixed-width field slice — exact bytes, no trimming (preserves trailing spaces
// so generic round-trip is the identity). Bounds-checked.
std::string GetFieldRaw(const std::string &record, const FieldSpec &spec);

// Convenience: raw field by name over a field table. Throws if name not found.
std::string GetFieldRaw(const std::string &record, const std::array<FieldSpec, EDI_DC40_FIELD_COUNT> &fields,
                        const char *name);

// Write a value into a fixed-width field: right-pad with spaces, or truncate to the
// field length. Grows `record` to the field's end if needed.
void SetFieldRaw(std::string &record, const FieldSpec &spec, const std::string &value);

} // namespace erpl_idoc
