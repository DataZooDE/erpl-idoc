#pragma once
// Shared multi-file plumbing for the flat IDoc readers. One resolved path argument
// (an exact path OR a glob) is expanded through DuckDB's virtual filesystem — so the
// same code reads local files, an `s3://bucket/idocs/*.idoc` prefix over httpfs, etc.
// Files are read and parsed lazily, one at a time, as the scan streams rows.

#include "duckdb.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/file_open_flags.hpp"
#include "duckdb/parallel/task_scheduler.hpp"

#include "idoc_format.hpp"
#include "idoc_xml.hpp"

#include <algorithm>
#include <atomic>
#include <memory>
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
		// FileSystem::Glob(path, opener) has an identical signature across DuckDB
		// v1.4.x and v1.5.x, whereas GlobFiles diverged (v1.4 takes ClientContext&,
		// v1.5 takes FileGlobInput). Glob returns empty on no match — we raise our
		// own BinderException below, matching the previous ALLOW_EMPTY behaviour.
		auto files = fs.Glob(p);
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

// Read the rest of an open file handle into a string (used only for the whole-file
// IDoc-XML control path, which tinyxml cannot stream).
inline std::string IdocReadRemaining(FileHandle &handle) {
	std::string out;
	char tmp[8192];
	for (;;) {
		auto n = handle.Read(reinterpret_cast<void *>(tmp), sizeof(tmp));
		if (n <= 0) {
			break;
		}
		out.append(tmp, static_cast<size_t>(n));
	}
	return out;
}

// ---- Parallel, constant-memory scan state -----------------------------------
// GLOBAL state = the immutable work queue of files (one file = one split) plus an
// atomic cursor threads pull from. MaxThreads() reports file-granular parallelism.
struct IdocGlobalState : public GlobalTableFunctionState {
	vector<std::string> files;
	std::atomic<idx_t> next_file {0};
	bool lenient = false;
	bool has_framing_override = false;
	erpl_idoc::Framing framing_override = erpl_idoc::Framing::FIXED;
	bool allow_xml_control = false;
	idx_t max_threads = 1;
	idx_t MaxThreads() const override {
		return max_threads;
	}
};

// LOCAL (per-thread) state: streams ONE file at a time in O(record) memory. Holds a
// flat RecordStreamer, or a materialized record list for the (rare) IDoc-XML control
// path. `pending`/`field_cursor` let a record's fields span output chunks
// (sap_idoc_read_fields) without re-reading. Declaration order matters: `streamer`
// captures `handle`, so it is declared AFTER `handle` and destroyed first.
struct IdocLocalState : public LocalTableFunctionState {
	bool active = false;
	std::string current_file;
	unique_ptr<FileHandle> handle;
	unique_ptr<erpl_idoc::RecordStreamer> streamer;
	erpl_idoc::ParsedIdoc xml_parsed;
	idx_t xml_cursor = 0;
	bool is_xml = false;
	erpl_idoc::IdocRecord pending;
	bool has_pending = false;
	idx_t field_cursor = 0;
};

inline unique_ptr<GlobalTableFunctionState> MakeIdocGlobal(ClientContext &context, vector<std::string> files,
                                                           bool lenient, bool has_override,
                                                           erpl_idoc::Framing framing_override, bool allow_xml_control) {
	auto g = make_uniq<IdocGlobalState>();
	g->lenient = lenient;
	g->has_framing_override = has_override;
	g->framing_override = framing_override;
	g->allow_xml_control = allow_xml_control;
	idx_t nthreads = TaskScheduler::GetScheduler(context).NumberOfThreads();
	g->max_threads = MaxValue<idx_t>(1, MinValue<idx_t>(files.empty() ? 1 : files.size(), nthreads));
	g->files = std::move(files);
	return std::move(g);
}

inline unique_ptr<LocalTableFunctionState> MakeIdocLocal() {
	return make_uniq<IdocLocalState>();
}

// Claim the next file from the global queue and open a streamer over it (or, for
// IDoc-XML control, materialize its control records). Returns false when the queue is
// drained. Constant memory: only a bounded read buffer is held per open file.
inline bool IdocOpenNextFile(ClientContext &context, IdocGlobalState &g, IdocLocalState &l) {
	idx_t idx = g.next_file.fetch_add(1);
	if (idx >= g.files.size()) {
		return false;
	}
	l.current_file = g.files[idx];
	l.is_xml = false;
	l.xml_cursor = 0;
	l.field_cursor = 0;
	l.has_pending = false;
	l.streamer.reset();

	auto &fs = FileSystem::GetFileSystem(context);
	l.handle = fs.OpenFile(l.current_file, FileFlags::FILE_FLAGS_READ);

	// Peek a bounded prefix to detect IDoc-XML vs flat without reading the whole file.
	std::string prefix;
	prefix.resize(4096);
	auto got = l.handle->Read(reinterpret_cast<void *>(&prefix[0]), prefix.size());
	prefix.resize(got > 0 ? static_cast<size_t>(got) : 0);

	if (IdocLooksLikeXml(prefix)) {
		if (!g.allow_xml_control) {
			throw InvalidInputException("'%s' is IDoc-XML: use sap_idoc_read_xml(...) or sap_idoc_xml_to_records(...); "
			                            "this function operates on the flat form.",
			                            l.current_file);
		}
		l.xml_parsed = IdocParseXmlControl(prefix + IdocReadRemaining(*l.handle));
		l.is_xml = true;
		l.active = true;
		return true;
	}

	// Flat: a ByteSource that first drains the peeked prefix, then reads from the file.
	auto pre = std::make_shared<std::string>(std::move(prefix));
	auto pos = std::make_shared<size_t>(0);
	auto *handle_ptr = l.handle.get();
	erpl_idoc::ByteSource src = [pre, pos, handle_ptr](char *dst, size_t n) -> size_t {
		size_t left = pre->size() - *pos;
		if (left > 0) {
			size_t give = n < left ? n : left;
			memcpy(dst, pre->data() + *pos, give);
			*pos += give;
			return give;
		}
		auto r = handle_ptr->Read(reinterpret_cast<void *>(dst), n);
		return r > 0 ? static_cast<size_t>(r) : 0;
	};
	if (g.has_framing_override) {
		l.streamer = make_uniq<erpl_idoc::RecordStreamer>(std::move(src), g.framing_override, g.lenient);
	} else {
		l.streamer = make_uniq<erpl_idoc::RecordStreamer>(erpl_idoc::RecordStreamer::Auto(std::move(src), g.lenient));
	}
	l.active = true;
	return true;
}

// Ensure l.pending holds the next record, pulling from the current file's streamer and
// advancing to the next file as needed. Returns false when all files are drained.
inline bool IdocPeek(ClientContext &context, IdocGlobalState &g, IdocLocalState &l) {
	if (l.has_pending) {
		return true;
	}
	for (;;) {
		if (!l.active && !IdocOpenNextFile(context, g, l)) {
			return false;
		}
		bool got;
		if (l.is_xml) {
			got = l.xml_cursor < l.xml_parsed.records.size();
			if (got) {
				l.pending = l.xml_parsed.records[l.xml_cursor++];
			}
		} else {
			got = l.streamer->Next(l.pending);
		}
		if (got) {
			l.has_pending = true;
			l.field_cursor = 0;
			return true;
		}
		l.active = false; // current file exhausted; grab the next
	}
}

inline void IdocConsume(IdocLocalState &l) {
	l.has_pending = false;
	l.field_cursor = 0;
}

} // namespace duckdb
