#pragma once
// Shared multi-file plumbing for the flat IDoc readers. One resolved path argument
// (an exact path OR a glob) is expanded through DuckDB's virtual filesystem — so the
// same code reads local files, an `s3://bucket/idocs/*.idoc` prefix over httpfs, etc.
// Files are read and parsed lazily, one at a time, as the scan streams rows.

#include "duckdb.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/file_open_flags.hpp"

#include "idoc_format.hpp"
#include "idoc_xml.hpp"

#include <algorithm>
#include <string>

namespace duckdb {

// Read a whole file through the VFS (local, httpfs/s3, gcs, hf, …).
inline std::string IdocReadWholeFile(ClientContext &context, const std::string &path) {
	auto &fs = FileSystem::GetFileSystem(context);
	auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ);
	auto size = static_cast<idx_t>(handle->GetFileSize());
	std::string buffer;
	buffer.resize(size);
	idx_t total = 0;
	while (total < size) {
		auto n = handle->Read(reinterpret_cast<void *>(&buffer[total]), size - total);
		if (n <= 0) {
			throw IOException("short read on '%s': got %llu of %llu bytes", path, (unsigned long long)total,
			                  (unsigned long long)size);
		}
		total += static_cast<idx_t>(n);
	}
	return buffer;
}

// Expand the path argument into a concrete, sorted file list. Exact paths pass through
// unchanged (preserving single-file "file not found" errors); glob patterns are
// expanded via the VFS. A LIST(VARCHAR) value is accepted too — each element expanded
// and concatenated.
inline vector<std::string> IdocResolvePaths(ClientContext &context, const Value &arg) {
	vector<std::string> out;
	auto add_one = [&](const std::string &p) {
		if (!FileSystem::HasGlob(p)) {
			out.push_back(p);
			return;
		}
		auto &fs = FileSystem::GetFileSystem(context);
		auto files = fs.GlobFiles(p, FileGlobOptions::ALLOW_EMPTY);
		if (files.empty()) {
			throw BinderException("sap_idoc: no files match the pattern '%s'", p);
		}
		for (auto &f : files) {
			out.push_back(f.path);
		}
	};
	if (arg.type().id() == LogicalTypeId::LIST) {
		for (auto &e : ListValue::GetChildren(arg)) {
			add_one(e.ToString());
		}
	} else {
		add_one(arg.ToString());
	}
	std::sort(out.begin(), out.end());
	return out;
}

// First non-whitespace byte is '<' -> IDoc-XML.
inline bool IdocLooksLikeXml(const std::string &data) {
	for (char c : data) {
		unsigned char u = static_cast<unsigned char>(c);
		if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || u == 0xEF || u == 0xBB || u == 0xBF) {
			continue;
		}
		return c == '<';
	}
	return false;
}

// Build a control-only ParsedIdoc from an IDoc-XML image (no dictionary needed).
inline erpl_idoc::ParsedIdoc IdocParseXmlControl(const std::string &xml) {
	erpl_idoc::ParsedIdoc parsed;
	parsed.framing = erpl_idoc::Framing::FIXED;
	auto idocs = erpl_idoc::ParseIdocXml(xml);
	int64_t dk = 0, idx = 0;
	for (auto &idoc : idocs) {
		dk++;
		std::vector<std::string> vals;
		for (auto &f : erpl_idoc::EDI_DC40_FIELDS) {
			vals.push_back(erpl_idoc::XmlFieldValue(idoc.control, f.name));
		}
		parsed.records.push_back(erpl_idoc::IdocRecord {dk, idx++, true, erpl_idoc::EncodeControl(vals)});
	}
	return parsed;
}

// Read + parse one file (flat, or IDoc-XML control when allow_xml_control).
inline erpl_idoc::ParsedIdoc IdocParseFile(ClientContext &context, const std::string &path, bool lenient,
                                           bool has_override, erpl_idoc::Framing framing_override,
                                           bool allow_xml_control) {
	auto data = IdocReadWholeFile(context, path);
	if (IdocLooksLikeXml(data)) {
		if (!allow_xml_control) {
			throw InvalidInputException("'%s' is IDoc-XML: use sap_idoc_read_xml(...) or sap_idoc_xml_to_records(...); "
			                            "this function operates on the flat form.",
			                            path);
		}
		return IdocParseXmlControl(data);
	}
	if (lenient) {
		return erpl_idoc::ParseImageLenient(data);
	}
	auto fr = has_override ? framing_override : erpl_idoc::DetectFraming(data);
	return erpl_idoc::ParseImage(data, fr);
}

// Streaming state shared by every flat reader: a resolved file list read lazily,
// one file parsed at a time.
struct IdocScanState : public GlobalTableFunctionState {
	vector<std::string> files;
	idx_t file_idx = 0;
	std::string current_file;
	bool lenient = false;
	bool has_framing_override = false;
	erpl_idoc::Framing framing_override = erpl_idoc::Framing::FIXED;
	bool allow_xml_control = false;
	erpl_idoc::ParsedIdoc parsed;
	bool loaded = false;
	idx_t cursor = 0;       // record index within the current file
	idx_t field_cursor = 0; // used by sap_idoc_read_fields (fields span chunks)
	idx_t MaxThreads() const override {
		return 1;
	}
};

// Ensure a record is available at (current_file, cursor); load subsequent files as
// needed. Returns false when every file is exhausted.
inline bool IdocAdvance(ClientContext &context, IdocScanState &st) {
	while (true) {
		if (st.loaded) {
			if (st.cursor < st.parsed.records.size()) {
				return true;
			}
			st.file_idx++;
			st.loaded = false;
		}
		if (st.file_idx >= st.files.size()) {
			return false;
		}
		st.current_file = st.files[st.file_idx];
		st.parsed = IdocParseFile(context, st.current_file, st.lenient, st.has_framing_override, st.framing_override,
		                          st.allow_xml_control);
		st.cursor = 0;
		st.field_cursor = 0;
		st.loaded = true;
	}
}

// Convenience: build the shared scan state from a resolved bind.
inline unique_ptr<GlobalTableFunctionState> MakeIdocScanState(vector<std::string> files, bool lenient,
                                                              bool has_override, erpl_idoc::Framing framing_override,
                                                              bool allow_xml_control) {
	auto st = make_uniq<IdocScanState>();
	st->files = std::move(files);
	st->lenient = lenient;
	st->has_framing_override = has_override;
	st->framing_override = framing_override;
	st->allow_xml_control = allow_xml_control;
	return std::move(st);
}

} // namespace duckdb
