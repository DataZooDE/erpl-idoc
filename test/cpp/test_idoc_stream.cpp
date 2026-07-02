#include "catch.hpp"
#include "idoc_format.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>

using namespace erpl_idoc;

static std::string ReadFileBytes(const std::string &path) {
	std::ifstream in(path, std::ios::binary);
	REQUIRE(in.good());
	return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static std::string FixtureIdoc() {
	if (const char *p = std::getenv("IDOC_FIXTURE")) {
		return p;
	}
	return "test/fixtures/flight.idoc";
}

// A ByteSource over an in-memory image, handing out at most `chunk` bytes per call and
// tracking how many times it was asked — so tests can drive tiny/uneven reads and
// confirm the streamer copes with partial reads.
struct MemSource {
	std::string data;
	size_t pos = 0;
	size_t chunk;
	explicit MemSource(std::string d, size_t c) : data(std::move(d)), chunk(c) {}
	size_t operator()(char *dst, size_t n) {
		size_t give = std::min({n, chunk, data.size() - pos});
		std::memcpy(dst, data.data() + pos, give);
		pos += give;
		return give;
	}
};

static std::vector<IdocRecord> StreamAll(const std::string &image, Framing framing, size_t chunk, bool lenient,
                                         size_t &high_water) {
	auto src = std::make_shared<MemSource>(image, chunk);
	RecordStreamer s([src](char *d, size_t n) { return (*src)(d, n); }, framing, lenient);
	std::vector<IdocRecord> out;
	IdocRecord rec;
	while (s.Next(rec)) {
		out.push_back(rec);
	}
	high_water = s.high_water();
	return out;
}

static void RequireSameRecords(const std::vector<IdocRecord> &a, const std::vector<IdocRecord> &b) {
	REQUIRE(a.size() == b.size());
	for (size_t i = 0; i < a.size(); i++) {
		INFO("record " << i);
		REQUIRE(a[i].document_key == b[i].document_key);
		REQUIRE(a[i].record_index == b[i].record_index);
		REQUIRE(a[i].is_control == b[i].is_control);
		REQUIRE(a[i].bytes == b[i].bytes);
	}
}

TEST_CASE("streaming FIXED == ParseImage (oracle)", "[idoc][stream]") {
	auto image = ReadFileBytes(FixtureIdoc());
	auto oracle = ParseImage(image, Framing::FIXED);
	// Try several read-chunk sizes, including tiny (partial reads) and huge.
	for (size_t chunk : {1u, 3u, 7u, 64u, 524u, 1063u, 4096u, 1u << 20}) {
		size_t hw = 0;
		auto got = StreamAll(image, Framing::FIXED, chunk, /*lenient=*/false, hw);
		RequireSameRecords(got, oracle.records);
	}
}

TEST_CASE("streaming auto-detects FIXED framing", "[idoc][stream]") {
	auto image = ReadFileBytes(FixtureIdoc());
	auto oracle = ParseImage(image, Framing::FIXED);
	auto src = std::make_shared<MemSource>(image, 5);
	auto s = RecordStreamer::Auto([src](char *d, size_t n) { return (*src)(d, n); }, /*lenient=*/false);
	std::vector<IdocRecord> got;
	IdocRecord rec;
	while (s.Next(rec)) {
		got.push_back(rec);
	}
	REQUIRE(s.framing() == Framing::FIXED);
	RequireSameRecords(got, oracle.records);
}

TEST_CASE("streaming TERMINATED (LF and CRLF) == ParseImage", "[idoc][stream]") {
	auto image = ReadFileBytes(FixtureIdoc());
	auto records = SplitRecords(image, Framing::FIXED);
	for (Framing fr : {Framing::TERMINATED_LF, Framing::TERMINATED_CRLF}) {
		auto joined = JoinRecords(records, fr);
		auto oracle = ParseImage(joined, fr);
		size_t hw = 0;
		auto got = StreamAll(joined, fr, /*chunk=*/9, /*lenient=*/false, hw);
		RequireSameRecords(got, oracle.records);
	}
}

TEST_CASE("streaming holds bounded memory on a large multi-IDoc image", "[idoc][stream]") {
	auto one = ReadFileBytes(FixtureIdoc());
	std::string big;
	const int copies = 4000; // ~10 MB of IDocs
	big.reserve(one.size() * copies);
	for (int i = 0; i < copies; i++) {
		big += one;
	}
	auto oracle = ParseImage(big, Framing::FIXED);
	size_t hw = 0;
	auto got = StreamAll(big, Framing::FIXED, /*chunk=*/8192, /*lenient=*/false, hw);
	REQUIRE(got.size() == oracle.records.size());
	REQUIRE(got.back().document_key == copies); // per-file doc keys 1..copies
	// Peak buffer must be O(record), NOT O(file): far below the 10 MB image.
	INFO("high_water=" << hw << " image=" << big.size());
	REQUIRE(hw < (DATA_RECORD_LEN + 8192 + 4096));
}

TEST_CASE("streaming lenient stops cleanly on a truncated final record", "[idoc][stream]") {
	auto image = ReadFileBytes(FixtureIdoc());
	auto full = ParseImage(image, Framing::FIXED);
	// chop the last record in half
	std::string truncated = image.substr(0, image.size() - (DATA_RECORD_LEN / 2));
	size_t hw = 0;
	auto got = StreamAll(truncated, Framing::FIXED, /*chunk=*/256, /*lenient=*/true, hw);
	// salvages every complete record, drops the partial
	REQUIRE(got.size() == full.records.size() - 1);
	// strict mode throws instead
	auto src = std::make_shared<MemSource>(truncated, 256);
	RecordStreamer strict([src](char *d, size_t n) { return (*src)(d, n); }, Framing::FIXED, /*lenient=*/false);
	IdocRecord rec;
	bool threw = false;
	try {
		while (strict.Next(rec)) {
		}
	} catch (const std::exception &) {
		threw = true;
	}
	REQUIRE(threw);
}
