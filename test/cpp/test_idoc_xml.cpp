#include "catch.hpp"
#include "idoc_xml.hpp"

using namespace erpl_idoc;

static const char *kFlightXml = R"(<?xml version="1.0" encoding="utf-8"?>
<FLIGHTBOOKING_CREATEFROMDAT01>
  <IDOC BEGIN="1">
    <EDI_DC40 SEGMENT="1">
      <TABNAM>EDI_DC40</TABNAM>
      <MANDT>001</MANDT>
      <DOCNUM>0000000000000008</DOCNUM>
      <IDOCTYP>FLIGHTBOOKING_CREATEFROMDAT01</IDOCTYP>
      <MESTYP>FLIGHTBOOKING_CREATEFROMDAT</MESTYP>
    </EDI_DC40>
    <E1SBO_CRE SEGMENT="1">
      <E1BPSBONEW SEGMENT="1">
        <AIRLINEID>LH</AIRLINEID>
        <CONNECTID>0400</CONNECTID>
        <FLIGHTDATE>20260715</FLIGHTDATE>
        <PASSNAME>MUELLER</PASSNAME>
      </E1BPSBONEW>
    </E1SBO_CRE>
  </IDOC>
</FLIGHTBOOKING_CREATEFROMDAT01>)";

TEST_CASE("ParseIdocXml extracts control fields", "[idoc][xml]") {
	auto idocs = ParseIdocXml(kFlightXml);
	REQUIRE(idocs.size() == 1);
	REQUIRE(XmlFieldValue(idocs[0].control, "IDOCTYP") == "FLIGHTBOOKING_CREATEFROMDAT01");
	REQUIRE(XmlFieldValue(idocs[0].control, "mandt") == "001"); // case-insensitive lookup
	REQUIRE(XmlFieldValue(idocs[0].control, "MESTYP") == "FLIGHTBOOKING_CREATEFROMDAT");
}

TEST_CASE("ParseIdocXml flattens segments with hierarchy from nesting", "[idoc][xml]") {
	auto idocs = ParseIdocXml(kFlightXml);
	auto &segs = idocs[0].segments;
	REQUIRE(segs.size() == 2);
	REQUIRE(segs[0].segnam == "E1SBO_CRE");
	REQUIRE(segs[0].hlevel == 1);
	REQUIRE(segs[1].segnam == "E1BPSBONEW");
	REQUIRE(segs[1].hlevel == 2); // nested inside E1SBO_CRE
	REQUIRE(XmlFieldValue(segs[1].fields, "AIRLINEID") == "LH");
	REQUIRE(XmlFieldValue(segs[1].fields, "FLIGHTDATE") == "20260715");
	REQUIRE(XmlFieldValue(segs[1].fields, "PASSNAME") == "MUELLER");
}

TEST_CASE("EmitIdocXml -> ParseIdocXml is a semantic round trip", "[idoc][xml]") {
	auto idocs = ParseIdocXml(kFlightXml);
	auto emitted = EmitIdocXml(idocs);
	auto reparsed = ParseIdocXml(emitted);

	REQUIRE(reparsed.size() == 1);
	REQUIRE(XmlFieldValue(reparsed[0].control, "IDOCTYP") == "FLIGHTBOOKING_CREATEFROMDAT01");
	REQUIRE(reparsed[0].segments.size() == 2);
	REQUIRE(reparsed[0].segments[1].segnam == "E1BPSBONEW");
	REQUIRE(reparsed[0].segments[1].hlevel == 2);
	REQUIRE(XmlFieldValue(reparsed[0].segments[1].fields, "FLIGHTDATE") == "20260715");
}

TEST_CASE("emitted XML nests the child segment inside its parent", "[idoc][xml]") {
	auto emitted = EmitIdocXml(ParseIdocXml(kFlightXml));
	auto pe1sbo = emitted.find("<E1SBO_CRE");
	auto pchild = emitted.find("<E1BPSBONEW");
	auto pclose = emitted.find("</E1SBO_CRE>");
	REQUIRE(pe1sbo != std::string::npos);
	REQUIRE(pchild != std::string::npos);
	REQUIRE(pe1sbo < pchild);   // parent opens first
	REQUIRE(pchild < pclose);   // child is inside the parent
}

TEST_CASE("malformed XML throws (NFR-7)", "[idoc][xml][safety]") {
	REQUIRE_THROWS(ParseIdocXml("<FOO><IDOC><EDI_DC40><X>1</X></EDI_DC40>"));  // unclosed
	REQUIRE_THROWS(ParseIdocXml("not xml at all"));
}
