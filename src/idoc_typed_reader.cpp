#include "duckdb.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/file_open_flags.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/materialized_query_result.hpp"

#include "idoc_functions.hpp"
#include "idoc_format.hpp"

#include <cctype>

namespace duckdb {

using erpl_idoc::EDI_DD40_FIELDS;
using erpl_idoc::Framing;
using erpl_idoc::GetFieldRaw;
using erpl_idoc::ParsedIdoc;
using erpl_idoc::RTrim;

// Shared file read (kept local to avoid cross-TU coupling).
static std::string TypedReadWholeFile(ClientContext &context, const std::string &path) {
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

// One field's slicing rule taken from the dictionary.
struct TypedFieldRule {
	std::string name;
	int64_t offset;
	int64_t length;
	std::string datatype;
};

struct ReadSegmentBindData : public TableFunctionData {
	std::string path;
	std::string segnam;
	bool has_framing_override = false;
	Framing framing_override = Framing::FIXED;
	bool lenient = false;
	std::string encoding = "utf-8";
	vector<TypedFieldRule> fields;
};

struct ReadSegmentGlobalState : public GlobalTableFunctionState {
	ParsedIdoc parsed;
	idx_t cursor = 0;
	idx_t MaxThreads() const override {
		return 1;
	}
};

static std::string SqlEscape(const std::string &s) {
	std::string out;
	for (char c : s) {
		if (c == '\'') {
			out += "''";
		} else {
			out += c;
		}
	}
	return out;
}

// A conservative identifier: letters/digits/_/./$/" only, no whitespace, quotes as a
// pair are the caller's responsibility. Used to gate the "bare name" dict source so a
// crafted string cannot inject arbitrary SQL into the internal dictionary query.
static bool IsSafeIdentifier(const std::string &s) {
	if (s.empty()) {
		return false;
	}
	for (char c : s) {
		if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '$' || c == '"')) {
			return false;
		}
	}
	return true;
}

// Turn a dictionary "source" argument into a SQL FROM expression. File paths are
// wrapped in the appropriate reader as *quoted literals* (injection-safe); a bare
// table/view name must be a safe identifier; a relation expression (contains '(') is
// treated as trusted SQL. This is what makes the dictionary "data" (FR-D3).
static std::string DictSource(const std::string &dict) {
	auto ends_with = [&](const char *suf) {
		std::string s(suf);
		return dict.size() >= s.size() && dict.compare(dict.size() - s.size(), s.size(), s) == 0;
	};
	if (ends_with(".csv")) {
		return "read_csv_auto('" + SqlEscape(dict) + "')";
	}
	if (ends_with(".parquet")) {
		return "read_parquet('" + SqlEscape(dict) + "')";
	}
	if (ends_with(".json")) {
		return "read_json_auto('" + SqlEscape(dict) + "')";
	}
	if (dict.find('(') != std::string::npos) {
		return dict; // explicit relation expression, e.g. read_csv('…') — trusted SQL
	}
	// A path without a known extension: treat as a CSV file (quoted literal, safe).
	if (dict.find('/') != std::string::npos || dict.find('\\') != std::string::npos) {
		return "read_csv_auto('" + SqlEscape(dict) + "')";
	}
	if (!IsSafeIdentifier(dict)) {
		throw BinderException(
		    "read_idoc_segment: dictionary '%s' is not a file path, a valid table/view name, or a relation expression",
		    dict);
	}
	return dict; // validated table / view identifier
}

static unique_ptr<FunctionData> ReadSegmentBind(ClientContext &context, TableFunctionBindInput &input,
                                                vector<LogicalType> &return_types, vector<string> &names) {
	auto bind = make_uniq<ReadSegmentBindData>();
	bind->path = input.inputs[0].GetValue<string>();
	bind->segnam = input.inputs[1].GetValue<string>();
	auto dict = input.inputs[2].GetValue<string>();

	auto &np = input.named_parameters;
	if (np.count("framing") && !np["framing"].IsNull()) {
		bind->has_framing_override = true;
		bind->framing_override = erpl_idoc::FramingFromString(np["framing"].GetValue<string>());
	}
	if (np.count("lenient") && !np["lenient"].IsNull()) {
		bind->lenient = np["lenient"].GetValue<bool>();
	}
	if (np.count("encoding") && !np["encoding"].IsNull()) {
		bind->encoding = np["encoding"].GetValue<string>();
	}

	// Load the slicing rules for this segment from the dictionary relation.
	auto query = "SELECT field_name, \"offset\", length, datatype FROM " + DictSource(dict) +
	             " WHERE segnam = '" + SqlEscape(bind->segnam) + "' ORDER BY field_pos";
	Connection con(*context.db);
	auto result = con.Query(query);
	if (result->HasError()) {
		throw BinderException("read_idoc_segment: failed to read dictionary: " + result->GetError());
	}
	if (result->RowCount() == 0) {
		throw BinderException("read_idoc_segment: dictionary has no fields for segment '%s'", bind->segnam);
	}

	names = {"document_key", "segnum"};
	return_types = {LogicalType::BIGINT, LogicalType::VARCHAR};
	for (idx_t i = 0; i < result->RowCount(); i++) {
		TypedFieldRule rule;
		rule.name = result->GetValue(0, i).ToString();
		rule.offset = result->GetValue(1, i).GetValue<int64_t>();
		rule.length = result->GetValue(2, i).GetValue<int64_t>();
		auto dt = result->GetValue(3, i);
		rule.datatype = dt.IsNull() ? "" : dt.ToString();
		if (rule.offset < 0 || rule.length < 0 || rule.offset > static_cast<int64_t>(erpl_idoc::SDATA_LEN) ||
		    rule.length > static_cast<int64_t>(erpl_idoc::SDATA_LEN) - rule.offset) {
			throw BinderException("read_idoc_segment: field '%s' offset/length out of SDATA bounds", rule.name);
		}
		// lower-case the column name (SAP dict names are upper-case)
		std::string lower;
		for (char c : rule.name) {
			lower += static_cast<char>(tolower(static_cast<unsigned char>(c)));
		}
		names.push_back(lower);
		return_types.push_back(LogicalType::VARCHAR); // typed values as strings (acceptance #3)
		bind->fields.push_back(std::move(rule));
	}
	return std::move(bind);
}

static unique_ptr<GlobalTableFunctionState> ReadSegmentInitGlobal(ClientContext &context,
                                                                  TableFunctionInitInput &input) {
	auto &bind = input.bind_data->Cast<ReadSegmentBindData>();
	auto data = TypedReadWholeFile(context, bind.path);
	auto state = make_uniq<ReadSegmentGlobalState>();
	if (bind.lenient) {
		state->parsed = erpl_idoc::ParseImageLenient(data);
	} else {
		auto framing = bind.has_framing_override ? bind.framing_override : erpl_idoc::DetectFraming(data);
		state->parsed = erpl_idoc::ParseImage(data, framing);
	}
	return std::move(state);
}

static void ReadSegmentScan(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind = data_p.bind_data->Cast<ReadSegmentBindData>();
	auto &gstate = data_p.global_state->Cast<ReadSegmentGlobalState>();
	auto &records = gstate.parsed.records;
	idx_t out_row = 0;
	while (gstate.cursor < records.size() && out_row < STANDARD_VECTOR_SIZE) {
		auto &rec = records[gstate.cursor++];
		if (rec.is_control) {
			continue;
		}
		if (RTrim(GetFieldRaw(rec.bytes, EDI_DD40_FIELDS[0])) != bind.segnam) { // SEGNAM
			continue;
		}
		auto sdata = GetFieldRaw(rec.bytes, EDI_DD40_FIELDS[6]); // 1000-byte SDATA
		output.SetValue(0, out_row, Value::BIGINT(rec.document_key));
		output.SetValue(1, out_row, Value(RTrim(GetFieldRaw(rec.bytes, EDI_DD40_FIELDS[3])))); // SEGNUM
		for (idx_t i = 0; i < bind.fields.size(); i++) {
			auto &f = bind.fields[i];
			std::string raw = sdata.substr(f.offset, f.length);
			// decode per the source codepage, then trim trailing pad spaces
			output.SetValue(2 + i, out_row, Value(RTrim(erpl_idoc::DecodeText(raw, bind.encoding))));
		}
		out_row++;
	}
	output.SetCardinality(out_row);
}

void RegisterIdocTypedReaderFunctions(ExtensionLoader &loader) {
	TableFunction f("read_idoc_segment", {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
	                ReadSegmentScan, ReadSegmentBind, ReadSegmentInitGlobal);
	f.named_parameters["framing"] = LogicalType::VARCHAR;
	f.named_parameters["lenient"] = LogicalType::BOOLEAN;
	f.named_parameters["encoding"] = LogicalType::VARCHAR;
	loader.RegisterFunction(f);
}

} // namespace duckdb
