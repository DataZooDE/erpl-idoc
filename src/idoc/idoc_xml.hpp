#pragma once

// Pure, offline IDoc-XML model (parse + emit) built on tinyxml2. No DuckDB, no SAP.
// IDoc-XML is the self-describing serialization: the root element is the basic type
// (IDOCTYP), each <IDOC> holds a control record <EDI_DC40> and segment elements whose
// leaf children are named fields. Hierarchy is XML nesting (hlevel = nesting depth).

#include <string>
#include <vector>

namespace erpl_idoc {

struct XmlField {
	std::string name;
	std::string value;
};

struct XmlSegment {
	std::string segnam;
	int hlevel;                    // 1 for a direct child of <IDOC>, +1 per nesting
	std::vector<XmlField> fields;  // leaf field elements of this segment
};

struct XmlIdoc {
	std::vector<XmlField> control;   // EDI_DC40 fields
	std::vector<XmlSegment> segments; // flattened, in document order, with hlevel
};

// Parse an IDoc-XML document into one XmlIdoc per <IDOC>. Throws std::runtime_error on
// malformed XML or a structure that is not recognizably IDoc-XML.
std::vector<XmlIdoc> ParseIdocXml(const std::string &xml);

// Emit IDoc-XML for the given IDocs. Segment nesting is rebuilt from hlevel (a segment
// at level L nests inside the most recent preceding segment at level L-1). Empty field
// values are omitted; the root element name is the control's IDOCTYP.
std::string EmitIdocXml(const std::vector<XmlIdoc> &idocs);

// Look up a control/segment field value by (case-insensitive) name; "" if absent.
std::string XmlFieldValue(const std::vector<XmlField> &fields, const std::string &name);

} // namespace erpl_idoc
