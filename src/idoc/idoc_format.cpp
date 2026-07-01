#include "idoc_format.hpp"

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
	if (spec.offset + spec.length > record.size()) {
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
	if (record.size() < spec.offset + spec.length) {
		record.resize(spec.offset + spec.length, ' ');
	}
	for (size_t i = 0; i < spec.length; i++) {
		record[spec.offset + i] = (i < value.size()) ? value[i] : ' ';
	}
}

} // namespace erpl_idoc
