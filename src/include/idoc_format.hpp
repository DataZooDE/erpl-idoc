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

// A physical record with document grouping metadata (FR-R5: multiple IDocs per file).
struct IdocRecord {
	int64_t document_key;  // 1-based; increments at each control record
	int64_t record_index;  // 0-based position in the file
	bool is_control;       // true for EDI_DC40, false for EDI_DD40
	std::string bytes;     // exact record bytes (524 or 1063)
};

struct ParsedIdoc {
	Framing framing;
	std::vector<IdocRecord> records;
};

// Split a file image and attach document_key / record_index / type to each record.
// Data records inherit the document_key of the most recent control record.
ParsedIdoc ParseImage(const std::string &data, Framing framing);

// Convenience: detect framing then ParseImage.
ParsedIdoc ParseImageAuto(const std::string &data);

// Trailing-space trim (right trim only) — for the friendly generic/typed views.
std::string RTrim(const std::string &s);

// Human-readable framing name / parse from string (for the COPY 'framing' option).
const char *FramingName(Framing framing);
Framing FramingFromString(const std::string &s); // "fixed" | "lf" | "crlf" (case-insensitive)

// ---- Writer composition (typed mode, FR-W2) --------------------------------
// Left zero-pad a non-negative integer to `width` (SAP numeric flat convention,
// e.g. DOCNUM(16), SEGNUM(6), HLEVEL(2)); truncates the low digits if too wide.
std::string ZeroPad(int64_t value, size_t width);

// Compose a 1000-byte SDATA from field slices. offsets/lengths/values are parallel
// arrays; each value is placed (space-padded/truncated) at its offset. Throws if a
// field would exceed SDATA bounds.
std::string EncodeSdata(const std::vector<int64_t> &offsets, const std::vector<int64_t> &lengths,
                        const std::vector<std::string> &values);

// Compose a 1063-byte EDI_DD40 data record. Numeric header fields are zero-padded to
// SAP widths; segnam/mandt/sdata are placed as-is (space-padded).
std::string EncodeDataRecord(const std::string &segnam, const std::string &mandt, int64_t docnum, int64_t segnum,
                             int64_t psgnum, int64_t hlevel, const std::string &sdata);

// Compose a 524-byte EDI_DC40 control record from up to 36 field values (in the
// EDI_DC40_FIELDS order); missing/short values are space-padded.
std::string EncodeControl(const std::vector<std::string> &values);

} // namespace erpl_idoc
