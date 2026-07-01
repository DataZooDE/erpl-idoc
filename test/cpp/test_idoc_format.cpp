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

TEST_CASE("ParseImage groups records under a document_key", "[idoc][format][multi]") {
	auto parsed = ParseImageAuto(ReadFile(FixturePath()));
	REQUIRE(parsed.framing == Framing::FIXED);
	REQUIRE(parsed.records.size() == 3);
	// one IDoc: control + both data records share document_key 1
	REQUIRE(parsed.records[0].document_key == 1);
	REQUIRE(parsed.records[0].is_control);
	REQUIRE(parsed.records[1].document_key == 1);
	REQUIRE(parsed.records[2].document_key == 1);
	REQUIRE(parsed.records[2].record_index == 2);
}

TEST_CASE("ParseImage groups multiple IDocs in one file", "[idoc][format][multi]") {
	// Two IDocs back to back: each = control(524) + one data(1063).
	auto data = ReadFile(FixturePath());
	auto records = SplitRecords(data, Framing::FIXED); // [ctrl, data, data]
	std::string two = records[0] + records[1] + records[0] + records[2];
	auto parsed = ParseImage(two, Framing::FIXED);
	REQUIRE(parsed.records.size() == 4);
	REQUIRE(parsed.records[0].document_key == 1);
	REQUIRE(parsed.records[1].document_key == 1);
	REQUIRE(parsed.records[2].document_key == 2); // second control
	REQUIRE(parsed.records[3].document_key == 2);
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

TEST_CASE("ZeroPad matches SAP numeric flat widths", "[idoc][format][writer]") {
	REQUIRE(ZeroPad(2, 6) == "000002");
	REQUIRE(ZeroPad(8, 16) == "0000000000000008");
	REQUIRE(ZeroPad(0, 2) == "00");
	REQUIRE(ZeroPad(1, 2) == "01");
}

TEST_CASE("EncodeSdata reproduces E1BPSBONEW SDATA from typed values", "[idoc][format][writer]") {
	// dictionary offsets/lengths for E1BPSBONEW
	std::vector<int64_t> off = {0, 3, 7, 15, 23, 24, 32, 40, 65, 80};
	std::vector<int64_t> len = {3, 4, 8, 8, 1, 8, 8, 25, 15, 8};
	std::vector<std::string> val = {"LH", "0400", "20260715", "00000042", "Y", "", "", "MUELLER", "Mr", "19800101"};
	auto sdata = EncodeSdata(off, len, val);
	REQUIRE(sdata.size() == SDATA_LEN);
	REQUIRE(sdata.substr(0, 3) == "LH ");
	REQUIRE(sdata.substr(7, 8) == "20260715");
	REQUIRE(sdata.substr(40, 25) == "MUELLER                  ");
	REQUIRE(sdata.substr(80, 8) == "19800101");
}

TEST_CASE("EncodeDataRecord reproduces the fixture's data records byte-exact", "[idoc][format][writer]") {
	auto records = SplitRecords(ReadFile(FixturePath()), Framing::FIXED);
	for (int i = 1; i <= 2; i++) {
		const auto &orig = records[i];
		auto segnam = GetFieldRaw(orig, EDI_DD40_FIELDS[0]);
		auto mandt = GetFieldRaw(orig, EDI_DD40_FIELDS[1]);
		auto docnum = std::stoll(GetFieldRaw(orig, EDI_DD40_FIELDS[2]));
		auto segnum = std::stoll(GetFieldRaw(orig, EDI_DD40_FIELDS[3]));
		auto psgnum = std::stoll(GetFieldRaw(orig, EDI_DD40_FIELDS[4]));
		auto hlevel = std::stoll(GetFieldRaw(orig, EDI_DD40_FIELDS[5]));
		auto sdata = GetFieldRaw(orig, EDI_DD40_FIELDS[6]);
		auto rebuilt = EncodeDataRecord(segnam, mandt, docnum, segnum, psgnum, hlevel, sdata);
		REQUIRE(rebuilt == orig);
	}
}

TEST_CASE("EncodeControl reproduces the fixture's control record byte-exact", "[idoc][format][writer]") {
	auto records = SplitRecords(ReadFile(FixturePath()), Framing::FIXED);
	const auto &orig = records[0];
	std::vector<std::string> values;
	for (const auto &f : EDI_DC40_FIELDS) {
		values.push_back(GetFieldRaw(orig, f)); // exact raw slices, no trimming
	}
	REQUIRE(EncodeControl(values) == orig);
}

TEST_CASE("terminated framing round-trips and auto-detects (FR-R4)", "[idoc][format][framing]") {
	auto data = ReadFile(FixturePath());
	auto records = SplitRecords(data, Framing::FIXED);

	for (auto fr : {Framing::TERMINATED_LF, Framing::TERMINATED_CRLF}) {
		auto joined = JoinRecords(records, fr);
		REQUIRE(DetectFraming(joined) == fr);
		auto back = SplitRecords(joined, fr);
		REQUIRE(back == records);
		// re-join reproduces the terminated image byte-exact
		REQUIRE(JoinRecords(back, fr) == joined);
	}
	// LF image is 3 newlines longer; CRLF is 6 bytes longer
	REQUIRE(JoinRecords(records, Framing::TERMINATED_LF).size() == data.size() + 3);
	REQUIRE(JoinRecords(records, Framing::TERMINATED_CRLF).size() == data.size() + 6);
}

TEST_CASE("lenient split salvages complete records from a truncated file (FR-R8)", "[idoc][format][safety]") {
	auto data = ReadFile(FixturePath());
	// 524 control + 300 bytes of a partial data record
	auto truncated = data.substr(0, CONTROL_RECORD_LEN + 300);
	REQUIRE_THROWS_AS(DetectFraming(truncated), std::runtime_error); // strict cannot frame it
	auto salvaged = SplitRecordsLenient(truncated);
	REQUIRE(salvaged.size() == 1); // the control record only; partial dropped
	REQUIRE(IsControlRecord(salvaged[0]));

	auto parsed = ParseImageLenient(truncated);
	REQUIRE(parsed.records.size() == 1);
	REQUIRE(parsed.records[0].document_key == 1);
}

TEST_CASE("DecodeText converts latin-1 high bytes to UTF-8 (FR-R6)", "[idoc][format][encoding]") {
	std::string latin1 = "M\xFCller"; // ü = 0xFC in latin-1
	auto utf8 = DecodeText(latin1, "latin-1");
	REQUIRE(utf8 == "M\xC3\xBCller"); // ü = U+00FC = C3 BC in UTF-8
	REQUIRE(DecodeText(latin1, "utf-8") == latin1); // pass-through
}

TEST_CASE("parsing never crashes on garbled / truncated input (NFR-7 fuzz)", "[idoc][format][safety]") {
	// Deterministic pseudo-random blobs of varying lengths; lenient parse must not throw.
	uint64_t seed = 0x9E3779B97F4A7C15ull;
	for (int t = 0; t < 500; t++) {
		seed = seed * 6364136223846793005ull + 1442695040888963407ull;
		size_t n = seed % 4000;
		std::string blob;
		blob.reserve(n);
		uint64_t s = seed;
		for (size_t i = 0; i < n; i++) {
			s = s * 6364136223846793005ull + 1442695040888963407ull;
			blob += static_cast<char>((s >> 33) & 0xFF);
		}
		// occasionally prefix with the control magic to exercise the control path
		if (t % 3 == 0 && blob.size() >= 6) {
			blob.replace(0, 6, "EDI_DC");
		}
		REQUIRE_NOTHROW(ParseImageLenient(blob));      // salvage, never throw
		REQUIRE_NOTHROW(SplitRecordsLenient(blob));    // bounds-safe
	}
}
