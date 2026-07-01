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
	seg.segnam = seg_el->Name();
	seg.hlevel = hlevel;
	std::vector<const XMLElement *> child_segments;
	for (const XMLElement *child = seg_el->FirstChildElement(); child; child = child->NextSiblingElement()) {
		if (IsSegmentElement(child)) {
			child_segments.push_back(child);
		} else {
			seg.fields.push_back(XmlField{child->Name(), ElemText(child)});
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

	// The root is the basic type; each <IDOC> is one document. Some renderings put the
	// <IDOC> directly at the root — handle both.
	std::vector<const XMLElement *> idoc_els;
	for (const XMLElement *el = root->FirstChildElement(); el; el = el->NextSiblingElement()) {
		if (CIEqual(el->Name(), "IDOC")) {
			idoc_els.push_back(el);
		}
	}
	if (idoc_els.empty() && CIEqual(root->Name(), "IDOC")) {
		idoc_els.push_back(root);
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
					idoc.control.push_back(XmlField{f->Name(), ElemText(f)});
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

static void EmitFields(std::string &out, const std::vector<XmlField> &fields, const std::string &indent) {
	for (const auto &f : fields) {
		auto v = RTrimValue(f.value);
		if (v.empty()) {
			continue; // omit empty fields
		}
		out += indent + "<" + f.name + ">";
		XmlEscapeInto(out, v);
		out += "</" + f.name + ">\n";
	}
}

std::string EmitIdocXml(const std::vector<XmlIdoc> &idocs) {
	std::string out = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
	for (const auto &idoc : idocs) {
		std::string idoctyp = XmlFieldValue(idoc.control, "IDOCTYP");
		if (idoctyp.empty()) {
			idoctyp = "IDOC";
		}
		idoctyp = RTrimValue(idoctyp);
		out += "<" + idoctyp + ">\n  <IDOC BEGIN=\"1\">\n";
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
			out += ind + "<" + seg.segnam + " SEGMENT=\"1\">\n";
			EmitFields(out, seg.fields, ind + "  ");
			open.push_back(seg.segnam);
			open_levels.push_back(seg.hlevel);
		}
		close_to(1);
		out += "  </IDOC>\n</" + idoctyp + ">\n";
	}
	return out;
}

} // namespace erpl_idoc
