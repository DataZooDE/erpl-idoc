#include "catch.hpp"
#include "idoc_format.hpp"

#include <fstream>
#include <string>

using namespace erpl_idoc;

static std::string ReadFile(const std::string &path) {
	std::ifstream in(path, std::ios::binary);
	REQUIRE(in.good());
	return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// The fixture path is passed via the IDOC_FIXTURE env var (set by the test runner),
// falling back to the in-repo location.
static std::string FixturePath() {
	if (const char *p = std::getenv("IDOC_FIXTURE")) {
		return p;
	}
	return "test/fixtures/flight.idoc";
}

TEST_CASE("record geometry constants match the verified layout", "[idoc][format]") {
	REQUIRE(CONTROL_RECORD_LEN == 524);
	REQUIRE(DATA_RECORD_LEN == 1063);
	REQUIRE(DATA_HEADER_LEN + SDATA_LEN == DATA_RECORD_LEN);
}

TEST_CASE("EDI_DC40 field table is contiguous and totals 524 bytes", "[idoc][format]") {
	size_t expected_off = 0;
	for (const auto &f : EDI_DC40_FIELDS) {
		REQUIRE(f.offset == expected_off);
		expected_off += f.length;
	}
	REQUIRE(expected_off == CONTROL_RECORD_LEN);
}

TEST_CASE("EDI_DD40 field table is contiguous and totals 1063 bytes", "[idoc][format]") {
	size_t expected_off = 0;
	for (const auto &f : EDI_DD40_FIELDS) {
		REQUIRE(f.offset == expected_off);
		expected_off += f.length;
	}
	REQUIRE(expected_off == DATA_RECORD_LEN);
}

TEST_CASE("fixture is detected as FIXED framing and splits into 1 control + 2 data", "[idoc][format]") {
	auto data = ReadFile(FixturePath());
	REQUIRE(data.size() == 2650);
	REQUIRE(DetectFraming(data) == Framing::FIXED);

	auto records = SplitRecords(data, Framing::FIXED);
	REQUIRE(records.size() == 3);
	REQUIRE(records[0].size() == CONTROL_RECORD_LEN);
	REQUIRE(records[1].size() == DATA_RECORD_LEN);
	REQUIRE(records[2].size() == DATA_RECORD_LEN);

	REQUIRE(IsControlRecord(records[0]));
	REQUIRE_FALSE(IsControlRecord(records[1]));
}

TEST_CASE("control record parses to typed fields", "[idoc][format]") {
	auto records = SplitRecords(ReadFile(FixturePath()), Framing::FIXED);
	const auto &ctrl = records[0];

	auto tabnam = GetFieldRaw(ctrl, EDI_DC40_FIELDS, "TABNAM");
	REQUIRE(tabnam.substr(0, 8) == "EDI_DC40");

	auto idoctyp = GetFieldRaw(ctrl, EDI_DC40_FIELDS, "IDOCTYP");
	REQUIRE(idoctyp.substr(0, 29) == "FLIGHTBOOKING_CREATEFROMDAT01");
}

TEST_CASE("data records expose SEGNAM, HLEVEL and raw SDATA", "[idoc][format]") {
	auto records = SplitRecords(ReadFile(FixturePath()), Framing::FIXED);

	REQUIRE(GetFieldRaw(records[1], EDI_DD40_FIELDS[0]).substr(0, 9) == "E1SBO_CRE");
	REQUIRE(GetFieldRaw(records[2], EDI_DD40_FIELDS[0]).substr(0, 10) == "E1BPSBONEW");

	// SDATA of E1BPSBONEW begins with the airline id 'LH' and flight date 20260715.
	auto sdata = GetFieldRaw(records[2], EDI_DD40_FIELDS[6]);
	REQUIRE(sdata.size() == SDATA_LEN);
	REQUIRE(sdata.substr(0, 3) == "LH ");
	REQUIRE(sdata.substr(7, 8) == "20260715");
}

TEST_CASE("split -> join is the byte-exact identity (round-trip anchor)", "[idoc][format][roundtrip]") {
	auto data = ReadFile(FixturePath());
	auto records = SplitRecords(data, Framing::FIXED);
	auto rebuilt = JoinRecords(records, Framing::FIXED);
	REQUIRE(rebuilt == data);
}

TEST_CASE("SetFieldRaw pads and truncates within the field width", "[idoc][format]") {
	std::string rec(CONTROL_RECORD_LEN, ' ');
	SetFieldRaw(rec, EDI_DC40_FIELDS[0], "EDI_DC40"); // TABNAM(10)
	REQUIRE(rec.substr(0, 10) == "EDI_DC40  ");

	// Truncation: a value longer than the field is cut to the field width.
	FieldSpec three{"X", 0, 3};
	std::string r2;
	SetFieldRaw(r2, three, "ABCDE");
	REQUIRE(r2 == "ABC");
}

TEST_CASE("bounds-checked field access throws on short records (NFR-7)", "[idoc][format][safety]") {
	std::string tiny = "EDI_DC40";
	REQUIRE_THROWS_AS(GetFieldRaw(tiny, EDI_DC40_FIELDS, "SERIAL"), std::runtime_error);
	REQUIRE_THROWS_AS(DetectFraming("short"), std::runtime_error);
}
