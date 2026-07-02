#include "idoc_format.hpp"

#include <cctype>
#include <cstring>

namespace erpl_idoc {

// ---- Field tables (offsets/widths verified from flight.idoc, VALIDATION.md §3) ----

const std::array<FieldSpec, EDI_DC40_FIELD_COUNT> EDI_DC40_FIELDS = {{
    {"TABNAM", 0, 10},    {"MANDT", 10, 3},    {"DOCNUM", 13, 16},  {"DOCREL", 29, 4},
    {"STATUS", 33, 2},    {"DIRECT", 35, 1},   {"OUTMOD", 36, 1},   {"EXPRSS", 37, 1},
    {"TEST", 38, 1},      {"IDOCTYP", 39, 30}, {"CIMTYP", 69, 30},  {"MESTYP", 99, 30},
    {"MESCOD", 129, 3},   {"MESFCT", 132, 3},  {"STD", 135, 1},     {"STDVRS", 136, 6},
    {"STDMES", 142, 6},   {"SNDPOR", 148, 10}, {"SNDPRT", 158, 2},  {"SNDPFC", 160, 2},
    {"SNDPRN", 162, 10},  {"SNDSAD", 172, 21}, {"SNDLAD", 193, 70}, {"RCVPOR", 263, 10},
    {"RCVPRT", 273, 2},   {"RCVPFC", 275, 2},  {"RCVPRN", 277, 10}, {"RCVSAD", 287, 21},
    {"RCVLAD", 308, 70},  {"CREDAT", 378, 8},  {"CRETIM", 386, 6},  {"REFINT", 392, 14},
    {"REFGRP", 406, 14},  {"REFMES", 420, 14}, {"ARCKEY", 434, 70}, {"SERIAL", 504, 20},
}};

const std::array<FieldSpec, EDI_DD40_FIELD_COUNT> EDI_DD40_FIELDS = {{
    {"SEGNAM", 0, 30}, {"MANDT", 30, 3},  {"DOCNUM", 33, 16}, {"SEGNUM", 49, 6},
    {"PSGNUM", 55, 6}, {"HLEVEL", 61, 2}, {"SDATA", 63, SDATA_LEN},
}};

bool IsControlRecord(const std::string &record) {
	// Control records carry TABNAM = "EDI_DC40"; any "EDI_DC" prefix marks the envelope.
	return record.size() >= 6 && record.compare(0, 6, "EDI_DC") == 0;
}

// Walk a byte image as contiguous fixed-width records. A record beginning with
// "EDI_DC" is a 524-byte control record; otherwise a 1063-byte data record. Returns
// false (without partial output) if the walk does not consume the buffer exactly.
static bool TryWalkFixed(const std::string &data, std::vector<std::string> &out) {
	std::vector<std::string> records;
	size_t pos = 0;
	const size_t len = data.size();
	while (pos < len) {
		size_t block = (len - pos >= 6 && data.compare(pos, 6, "EDI_DC") == 0) ? CONTROL_RECORD_LEN
		                                                                       : DATA_RECORD_LEN;
		if (pos + block > len) {
			return false; // truncated / misaligned
		}
		records.push_back(data.substr(pos, block));
		pos += block;
	}
	out = std::move(records);
	return true;
}

Framing DetectFraming(const std::string &data) {
	if (data.size() < CONTROL_RECORD_LEN) {
		throw std::runtime_error("IDoc image too short to contain a control record (" +
		                         std::to_string(data.size()) + " < " + std::to_string(CONTROL_RECORD_LEN) + ")");
	}
	// Prefer contiguous fixed-width (the validated production layout).
	std::vector<std::string> tmp;
	if (TryWalkFixed(data, tmp)) {
		return Framing::FIXED;
	}
	// Otherwise inspect the byte right after the first 524-byte record for a terminator.
	if (data.size() > CONTROL_RECORD_LEN + 1 && data[CONTROL_RECORD_LEN] == '\r' &&
	    data[CONTROL_RECORD_LEN + 1] == '\n') {
		return Framing::TERMINATED_CRLF;
	}
	if (data.size() > CONTROL_RECORD_LEN && data[CONTROL_RECORD_LEN] == '\n') {
		return Framing::TERMINATED_LF;
	}
	throw std::runtime_error("Unable to detect IDoc framing (neither contiguous fixed-width nor LF/CRLF-terminated)");
}

std::vector<std::string> SplitRecords(const std::string &data, Framing framing) {
	if (framing == Framing::FIXED) {
		std::vector<std::string> out;
		if (!TryWalkFixed(data, out)) {
			throw std::runtime_error("IDoc image is not aligned to fixed-width records (truncated or wrong framing)");
		}
		return out;
	}

	// Terminated framing: split on '\n', stripping a trailing '\r' for CRLF.
	std::vector<std::string> out;
	size_t pos = 0;
	const size_t len = data.size();
	while (pos < len) {
		size_t nl = data.find('\n', pos);
		if (nl == std::string::npos) {
			// trailing chunk without a terminator
			std::string tail = data.substr(pos);
			if (!tail.empty()) {
				out.push_back(std::move(tail));
			}
			break;
		}
		std::string line = data.substr(pos, nl - pos);
		if (framing == Framing::TERMINATED_CRLF && !line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		// Validate record geometry so malformed terminated records fail early with a
		// clear error rather than surfacing as bad rows downstream.
		size_t expected = IsControlRecord(line) ? CONTROL_RECORD_LEN : DATA_RECORD_LEN;
		if (line.size() != expected) {
			throw std::runtime_error("malformed terminated IDoc record #" + std::to_string(out.size()) +
			                         ": got " + std::to_string(line.size()) + " bytes, expected " +
			                         std::to_string(expected));
		}
		out.push_back(std::move(line));
		pos = nl + 1;
	}
	return out;
}

std::string JoinRecords(const std::vector<std::string> &records, Framing framing) {
	std::string out;
	const char *term = (framing == Framing::TERMINATED_CRLF) ? "\r\n"
	                   : (framing == Framing::TERMINATED_LF)  ? "\n"
	                                                          : "";
	for (const auto &rec : records) {
		out += rec;
		out += term;
	}
	return out;
}

std::string GetFieldRaw(const std::string &record, const FieldSpec &spec) {
	// Overflow-safe: never form offset+length (which can wrap for adversarial specs).
	if (spec.offset > record.size() || spec.length > record.size() - spec.offset) {
		throw std::runtime_error(std::string("field '") + spec.name + "' at offset " +
		                         std::to_string(spec.offset) + " length " + std::to_string(spec.length) +
		                         " exceeds record size " + std::to_string(record.size()));
	}
	return record.substr(spec.offset, spec.length);
}

std::string GetFieldRaw(const std::string &record, const std::array<FieldSpec, EDI_DC40_FIELD_COUNT> &fields,
                        const char *name) {
	for (const auto &f : fields) {
		if (std::strcmp(f.name, name) == 0) {
			return GetFieldRaw(record, f);
		}
	}
	throw std::runtime_error(std::string("unknown field '") + name + "'");
}

void SetFieldRaw(std::string &record, const FieldSpec &spec, const std::string &value) {
	// Guard against overflow when growing the record to the field's end.
	if (spec.length > record.max_size() - spec.offset) {
		throw std::runtime_error(std::string("field '") + spec.name + "' offset/length too large");
	}
	if (record.size() < spec.offset + spec.length) {
		record.resize(spec.offset + spec.length, ' ');
	}
	for (size_t i = 0; i < spec.length; i++) {
		record[spec.offset + i] = (i < value.size()) ? value[i] : ' ';
	}
}

ParsedIdoc ParseImage(const std::string &data, Framing framing) {
	ParsedIdoc result;
	result.framing = framing;
	auto records = SplitRecords(data, framing);
	int64_t document_key = 0;
	int64_t idx = 0;
	for (auto &rec : records) {
		bool is_control = IsControlRecord(rec);
		if (is_control) {
			document_key++;
		}
		result.records.push_back(IdocRecord{document_key, idx, is_control, std::move(rec)});
		idx++;
	}
	return result;
}

ParsedIdoc ParseImageAuto(const std::string &data) {
	return ParseImage(data, DetectFraming(data));
}

std::vector<std::string> SplitRecordsLenient(const std::string &data) {
	std::vector<std::string> out;
	size_t pos = 0;
	const size_t len = data.size();
	while (pos < len) {
		size_t block = (len - pos >= 6 && data.compare(pos, 6, "EDI_DC") == 0) ? CONTROL_RECORD_LEN
		                                                                       : DATA_RECORD_LEN;
		if (pos + block > len) {
			break; // drop the trailing partial record
		}
		out.push_back(data.substr(pos, block));
		pos += block;
	}
	return out;
}

ParsedIdoc ParseImageLenient(const std::string &data) {
	ParsedIdoc result;
	result.framing = Framing::FIXED;
	auto records = SplitRecordsLenient(data);
	int64_t document_key = 0;
	int64_t idx = 0;
	for (auto &rec : records) {
		bool is_control = IsControlRecord(rec);
		if (is_control) {
			document_key++;
		}
		result.records.push_back(IdocRecord{document_key, idx, is_control, std::move(rec)});
		idx++;
	}
	return result;
}

// ---- Streaming reader -------------------------------------------------------

Framing DetectFramingPrefix(const char *data, size_t size) {
	if (size < CONTROL_RECORD_LEN) {
		throw std::runtime_error("IDoc image too short to detect framing (" + std::to_string(size) + " < " +
		                         std::to_string(CONTROL_RECORD_LEN) + ")");
	}
	if (size > CONTROL_RECORD_LEN + 1 && data[CONTROL_RECORD_LEN] == '\r' && data[CONTROL_RECORD_LEN + 1] == '\n') {
		return Framing::TERMINATED_CRLF;
	}
	if (size > CONTROL_RECORD_LEN && data[CONTROL_RECORD_LEN] == '\n') {
		return Framing::TERMINATED_LF;
	}
	return Framing::FIXED;
}

RecordStreamer::RecordStreamer(ByteSource src, Framing framing, bool framing_known, bool lenient)
    : src_(std::move(src)), framing_(framing), framing_known_(framing_known), lenient_(lenient) {
}

RecordStreamer::RecordStreamer(ByteSource src, Framing framing, bool lenient)
    : RecordStreamer(std::move(src), framing, /*framing_known=*/true, lenient) {
}

RecordStreamer RecordStreamer::Auto(ByteSource src, bool lenient) {
	return RecordStreamer(std::move(src), Framing::FIXED, /*framing_known=*/false, lenient);
}

bool RecordStreamer::Fill(size_t need) {
	char tmp[8192];
	while (buf_.size() < need && !eof_) {
		size_t n = src_(tmp, sizeof(tmp));
		if (n == 0) {
			eof_ = true;
			break;
		}
		buf_.append(tmp, n);
		if (buf_.size() > high_water_) {
			high_water_ = buf_.size();
		}
	}
	return buf_.size() >= need;
}

bool RecordStreamer::Emit(std::string rec, IdocRecord &out) {
	bool is_control = IsControlRecord(rec);
	if (is_control) {
		document_key_++;
	}
	out = IdocRecord{document_key_, record_index_++, is_control, std::move(rec)};
	return true;
}

bool RecordStreamer::Next(IdocRecord &out) {
	if (!framing_known_) {
		Fill(CONTROL_RECORD_LEN + 2);
		if (buf_.empty()) {
			return false;
		}
		framing_ = DetectFramingPrefix(buf_.data(), buf_.size());
		framing_known_ = true;
	}

	if (framing_ == Framing::FIXED) {
		if (!Fill(6)) {
			if (buf_.empty()) {
				return false; // clean end of stream
			}
			if (lenient_) {
				buf_.clear();
				return false;
			}
			throw std::runtime_error("IDoc image is not aligned to fixed-width records (truncated or wrong framing)");
		}
		size_t block = (buf_.compare(0, 6, "EDI_DC") == 0) ? CONTROL_RECORD_LEN : DATA_RECORD_LEN;
		if (!Fill(block)) {
			if (lenient_) {
				buf_.clear();
				return false; // drop the trailing partial record
			}
			throw std::runtime_error("IDoc image is not aligned to fixed-width records (truncated or wrong framing)");
		}
		std::string rec = buf_.substr(0, block);
		buf_.erase(0, block);
		return Emit(std::move(rec), out);
	}

	// TERMINATED_LF / TERMINATED_CRLF: read until the next '\n', bounded by one record.
	size_t nl;
	while ((nl = buf_.find('\n')) == std::string::npos) {
		if (buf_.size() > DATA_RECORD_LEN + 2) {
			if (lenient_) {
				buf_.clear();
				return false;
			}
			throw std::runtime_error("terminated IDoc record exceeds maximum length without a terminator");
		}
		size_t before = buf_.size();
		Fill(before + 4096);
		if (buf_.size() == before) {
			break; // EOF reached with no further data
		}
	}
	if (nl == std::string::npos) {
		if (buf_.empty()) {
			return false;
		}
		// trailing record without a terminator (last line)
		std::string line = std::move(buf_);
		buf_.clear();
		if (framing_ == Framing::TERMINATED_CRLF && !line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		if (!lenient_) {
			size_t expected = IsControlRecord(line) ? CONTROL_RECORD_LEN : DATA_RECORD_LEN;
			if (line.size() != expected) {
				throw std::runtime_error("malformed terminated IDoc record: got " + std::to_string(line.size()) +
				                         " bytes, expected " + std::to_string(expected));
			}
		}
		return Emit(std::move(line), out);
	}
	std::string line = buf_.substr(0, nl);
	buf_.erase(0, nl + 1);
	if (framing_ == Framing::TERMINATED_CRLF && !line.empty() && line.back() == '\r') {
		line.pop_back();
	}
	if (!lenient_) {
		size_t expected = IsControlRecord(line) ? CONTROL_RECORD_LEN : DATA_RECORD_LEN;
		if (line.size() != expected) {
			throw std::runtime_error("malformed terminated IDoc record #" + std::to_string(record_index_) + ": got " +
			                         std::to_string(line.size()) + " bytes, expected " + std::to_string(expected));
		}
	}
	return Emit(std::move(line), out);
}

std::string DecodeText(const std::string &raw, const std::string &encoding) {
	std::string lower;
	for (char c : encoding) {
		lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	if (lower == "latin-1" || lower == "latin1" || lower == "iso-8859-1" || lower == "iso8859-1") {
		std::string out;
		out.reserve(raw.size());
		for (unsigned char c : raw) {
			if (c < 0x80) {
				out += static_cast<char>(c);
			} else {
				out += static_cast<char>(0xC0 | (c >> 6));
				out += static_cast<char>(0x80 | (c & 0x3F));
			}
		}
		return out;
	}
	return raw; // utf-8 / unknown: pass through
}

std::string RTrim(const std::string &s) {
	size_t end = s.size();
	while (end > 0 && s[end - 1] == ' ') {
		end--;
	}
	return s.substr(0, end);
}

const char *FramingName(Framing framing) {
	switch (framing) {
	case Framing::FIXED:
		return "fixed";
	case Framing::TERMINATED_LF:
		return "lf";
	case Framing::TERMINATED_CRLF:
		return "crlf";
	}
	return "fixed";
}

std::string ZeroPad(int64_t value, size_t width) {
	if (value < 0) {
		value = 0;
	}
	std::string digits = std::to_string(value);
	if (digits.size() >= width) {
		return digits.substr(digits.size() - width); // keep low-order digits
	}
	return std::string(width - digits.size(), '0') + digits;
}

std::string EncodeSdata(const std::vector<int64_t> &offsets, const std::vector<int64_t> &lengths,
                        const std::vector<std::string> &values) {
	if (offsets.size() != lengths.size() || offsets.size() != values.size()) {
		throw std::runtime_error("EncodeSdata: offsets/lengths/values length mismatch");
	}
	std::string sdata(SDATA_LEN, ' ');
	for (size_t i = 0; i < offsets.size(); i++) {
		// Overflow-safe bounds test (never form offset+length on int64 inputs).
		if (offsets[i] < 0 || lengths[i] < 0 || offsets[i] > static_cast<int64_t>(SDATA_LEN) ||
		    lengths[i] > static_cast<int64_t>(SDATA_LEN) - offsets[i]) {
			throw std::runtime_error("EncodeSdata: field at offset " + std::to_string(offsets[i]) +
			                         " length " + std::to_string(lengths[i]) + " exceeds SDATA bounds");
		}
		FieldSpec spec{"", static_cast<size_t>(offsets[i]), static_cast<size_t>(lengths[i])};
		SetFieldRaw(sdata, spec, values[i]);
	}
	return sdata;
}

std::string EncodeDataRecord(const std::string &segnam, const std::string &mandt, int64_t docnum, int64_t segnum,
                             int64_t psgnum, int64_t hlevel, const std::string &sdata) {
	std::string rec(DATA_RECORD_LEN, ' ');
	SetFieldRaw(rec, EDI_DD40_FIELDS[0], segnam);                 // SEGNAM(30)
	SetFieldRaw(rec, EDI_DD40_FIELDS[1], mandt);                  // MANDT(3)
	SetFieldRaw(rec, EDI_DD40_FIELDS[2], ZeroPad(docnum, 16));    // DOCNUM(16)
	SetFieldRaw(rec, EDI_DD40_FIELDS[3], ZeroPad(segnum, 6));     // SEGNUM(6)
	SetFieldRaw(rec, EDI_DD40_FIELDS[4], ZeroPad(psgnum, 6));     // PSGNUM(6)
	SetFieldRaw(rec, EDI_DD40_FIELDS[5], ZeroPad(hlevel, 2));     // HLEVEL(2)
	SetFieldRaw(rec, EDI_DD40_FIELDS[6], sdata);                  // SDATA(1000)
	return rec;
}

std::string EncodeControl(const std::vector<std::string> &values) {
	std::string rec(CONTROL_RECORD_LEN, ' ');
	for (size_t i = 0; i < EDI_DC40_FIELDS.size() && i < values.size(); i++) {
		SetFieldRaw(rec, EDI_DC40_FIELDS[i], values[i]);
	}
	return rec;
}

Framing FramingFromString(const std::string &s) {
	std::string lower;
	for (char c : s) {
		lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	if (lower == "fixed" || lower == "contiguous") {
		return Framing::FIXED;
	}
	if (lower == "lf" || lower == "newline" || lower == "terminated_lf") {
		return Framing::TERMINATED_LF;
	}
	if (lower == "crlf" || lower == "terminated_crlf") {
		return Framing::TERMINATED_CRLF;
	}
	throw std::runtime_error("unknown framing '" + s + "' (expected 'fixed', 'lf' or 'crlf')");
}

} // namespace erpl_idoc
