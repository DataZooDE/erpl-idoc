#include "idoc_xml.hpp"
#include "tinyxml2.h"

#include <cctype>
#include <cstring>
#include <sstream>
#include <stdexcept>

namespace erpl_idoc {

using namespace tinyxml2;

static bool CIEqual(const char *a, const char *b) {
	if (!a || !b) {
		return false;
	}
	while (*a && *b) {
		if (std::tolower(static_cast<unsigned char>(*a)) != std::tolower(static_cast<unsigned char>(*b))) {
			return false;
		}
		a++;
		b++;
	}
	return *a == *b;
}

// SAP renders '/' (ABAP namespaces, e.g. /BA1/F4_FX_CREATE01) as "_-" in XML element names. Verified
// against SAP's own XML serialization on an A4H system (test/e2e/m12_xml_names.sh; real output is checked
// in as test/fixtures/sap_asxml_namespaced.xml). Not ambiguous in practice: SAP names are letters, digits,
// '_' and '/', never '-', so a literal "_-" cannot occur in one.
static std::string EscapeXmlName(const std::string &name) {
	std::string out;
	for (char c : name) {
		if (c == '/') {
			out += "_-";
		} else {
			out += c;
		}
	}
	return out;
}

static std::string UnescapeXmlName(const std::string &name) {
	std::string out;
	for (size_t i = 0; i < name.size(); i++) {
		if (name[i] == '_' && i + 1 < name.size() && name[i + 1] == '-') {
			out += '/';
			i++;
		} else {
			out += name[i];
		}
	}
	return out;
}

std::string XmlFieldValue(const std::vector<XmlField> &fields, const std::string &name) {
	for (const auto &f : fields) {
		if (CIEqual(f.name.c_str(), name.c_str())) {
			return f.value;
		}
	}
	return std::string();
}

// An element is a "segment" (container) if it carries a SEGMENT attribute or has any
// child elements; otherwise it is a leaf field.
static bool IsSegmentElement(const XMLElement *el) {
	if (el->Attribute("SEGMENT")) {
		return true;
	}
	return el->FirstChildElement() != nullptr;
}

static std::string ElemText(const XMLElement *el) {
	const char *t = el->GetText();
	return t ? std::string(t) : std::string();
}

// Recursively collect a segment element and its nested child segments (depth = hlevel).
static void CollectSegment(const XMLElement *seg_el, int hlevel, std::vector<XmlSegment> &out) {
	XmlSegment seg;
	seg.segnam = UnescapeXmlName(seg_el->Name());
	seg.hlevel = hlevel;
	std::vector<const XMLElement *> child_segments;
	for (const XMLElement *child = seg_el->FirstChildElement(); child; child = child->NextSiblingElement()) {
		if (IsSegmentElement(child)) {
			child_segments.push_back(child);
		} else {
			seg.fields.push_back(XmlField{UnescapeXmlName(child->Name()), ElemText(child)});
		}
	}
	out.push_back(std::move(seg));
	for (auto *cs : child_segments) {
		CollectSegment(cs, hlevel + 1, out);
	}
}

std::vector<XmlIdoc> ParseIdocXml(const std::string &xml) {
	XMLDocument doc;
	if (doc.Parse(xml.c_str(), xml.size()) != XML_SUCCESS) {
		throw std::runtime_error(std::string("IDoc-XML parse error: ") + XMLDocument::ErrorIDToName(doc.ErrorID()));
	}
	const XMLElement *root = doc.RootElement();
	if (!root) {
		throw std::runtime_error("IDoc-XML has no root element");
	}
	// A well-formed document has one root; the parser would otherwise just ignore the rest.
	if (root->NextSiblingElement()) {
		throw std::runtime_error("IDoc-XML has more than one root element; several IDocs of one basic type go "
		                         "inside a single root as multiple <IDOC> elements");
	}

	// The root is the basic type; each <IDOC> is one document. Some renderings put the
	// <IDOC> directly at the root — handle both.
	std::vector<const XMLElement *> idoc_els;
	if (CIEqual(root->Name(), "IDOC")) {
		idoc_els.push_back(root);
	} else {
		for (const XMLElement *el = root->FirstChildElement(); el; el = el->NextSiblingElement()) {
			if (!CIEqual(el->Name(), "IDOC")) {
				// Not ignored: a stray element would be silently dropped from the conversion.
				throw std::runtime_error(std::string("IDoc-XML: unexpected element <") + el->Name() +
				                         "> under the root; only <IDOC> elements are allowed there");
			}
			idoc_els.push_back(el);
		}
	}
	if (idoc_els.empty()) {
		throw std::runtime_error("IDoc-XML: no <IDOC> element found");
	}

	std::vector<XmlIdoc> result;
	for (const XMLElement *idoc_el : idoc_els) {
		XmlIdoc idoc;
		for (const XMLElement *el = idoc_el->FirstChildElement(); el; el = el->NextSiblingElement()) {
			// Control record: EDI_DC40 (or any EDI_DC* control element).
			if (std::strncmp(el->Name(), "EDI_DC", 6) == 0) {
				for (const XMLElement *f = el->FirstChildElement(); f; f = f->NextSiblingElement()) {
					idoc.control.push_back(XmlField{UnescapeXmlName(f->Name()), ElemText(f)});
				}
			} else {
				CollectSegment(el, 1, idoc.segments);
			}
		}
		if (idoc.control.empty() && idoc.segments.empty()) {
			throw std::runtime_error("IDoc-XML: <IDOC> has neither a control record nor segments");
		}
		result.push_back(std::move(idoc));
	}
	return result;
}

static std::string RTrimValue(const std::string &s) {
	size_t end = s.size();
	while (end > 0 && s[end - 1] == ' ') {
		end--;
	}
	return s.substr(0, end);
}

static void XmlEscapeInto(std::string &out, const std::string &s) {
	for (char c : s) {
		switch (c) {
		case '&':
			out += "&amp;";
			break;
		case '<':
			out += "&lt;";
			break;
		case '>':
			out += "&gt;";
			break;
		default:
			out += c;
		}
	}
}

// XML Name restricted to ASCII: a letter or '_' first, then letters, digits, '_', '-' or '.'.
static bool IsXmlName(const std::string &s) {
	if (s.empty() || !(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_')) {
		return false;
	}
	for (unsigned char c : s) {
		if (!(std::isalnum(c) || c == '_' || c == '-' || c == '.')) {
			return false;
		}
	}
	return true;
}

// The (escaped) name of an IDoc, segment or field becomes an XML element name: refuse one that cannot be.
static std::string XmlElementName(const std::string &sap_name) {
	auto escaped = EscapeXmlName(sap_name);
	if (!IsXmlName(escaped)) {
		throw std::runtime_error("'" + sap_name + "' is not a valid XML element name");
	}
	return escaped;
}

static void EmitFields(std::string &out, const std::vector<XmlField> &fields, const std::string &indent) {
	for (const auto &f : fields) {
		auto v = RTrimValue(f.value);
		if (v.empty()) {
			continue; // omit empty fields
		}
		auto name = XmlElementName(f.name);
		out += indent + "<" + name + ">";
		XmlEscapeInto(out, v);
		out += "</" + name + ">\n";
	}
}

std::string EmitIdocXml(const std::vector<XmlIdoc> &idocs) {
	std::string out = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
	if (idocs.empty()) {
		throw std::runtime_error("no IDoc to convert to XML");
	}
	// SAP's multi-IDoc rendering: one root named after the basic type holding one <IDOC> per document.
	// A single XML document has a single root, so all IDocs must share the basic type.
	std::string idoctyp;
	for (size_t i = 0; i < idocs.size(); i++) {
		auto t = RTrimValue(XmlFieldValue(idocs[i].control, "IDOCTYP"));
		if (t.empty()) {
			t = "IDOC";
		}
		if (i == 0) {
			idoctyp = t;
		} else if (t != idoctyp) {
			throw std::runtime_error("one IDoc basic type per XML document: the file mixes '" + idoctyp + "' and '" + t +
			                         "'; convert the types separately");
		}
	}
	auto root = XmlElementName(idoctyp); // the basic type becomes the root element
	out += "<" + root + ">\n";
	for (const auto &idoc : idocs) {
		out += "  <IDOC BEGIN=\"1\">\n";
		out += "    <EDI_DC40 SEGMENT=\"1\">\n";
		EmitFields(out, idoc.control, "      ");
		out += "    </EDI_DC40>\n";

		// Rebuild nesting from hlevel: close deeper/sibling segments before opening one.
		std::vector<std::string> open; // stack of open segment tags with their hlevel
		std::vector<int> open_levels;
		auto close_to = [&](int level) {
			while (!open_levels.empty() && open_levels.back() >= level) {
				std::string ind(4 + open_levels.back() * 2, ' ');
				out += ind + "</" + open.back() + ">\n";
				open.pop_back();
				open_levels.pop_back();
			}
		};
		for (const auto &seg : idoc.segments) {
			close_to(seg.hlevel);
			std::string ind(4 + seg.hlevel * 2, ' ');
			auto tag = XmlElementName(seg.segnam);
			out += ind + "<" + tag + " SEGMENT=\"1\">\n";
			EmitFields(out, seg.fields, ind + "  ");
			open.push_back(tag);
			open_levels.push_back(seg.hlevel);
		}
		close_to(1);
		out += "  </IDOC>\n";
	}
	out += "</" + root + ">\n";
	return out;
}

} // namespace erpl_idoc
