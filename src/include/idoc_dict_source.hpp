#pragma once

// Shared, injection-hardened handling of a "dictionary source" argument — a file
// path (.csv/.parquet/.json), a table/view name, or an explicit relation expression.
// Used by read_idoc_segment, idoc_dict_offsets and idoc_validate_dict so the same
// safety rules apply everywhere.

#include "duckdb.hpp"

#include <cctype>
#include <string>

namespace duckdb {

inline std::string DictSqlEscape(const std::string &s) {
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

inline bool DictIsSafeIdentifier(const std::string &s) {
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

// Turn a dictionary source into a SQL FROM expression. File paths become
// quoted-literal reader calls (injection-safe); a bare name must be a safe
// identifier; a relation expression (contains '(') is treated as trusted SQL.
inline std::string DictSource(const std::string &dict) {
	auto ends_with = [&](const char *suf) {
		std::string s(suf);
		return dict.size() >= s.size() && dict.compare(dict.size() - s.size(), s.size(), s) == 0;
	};
	if (ends_with(".csv")) {
		return "read_csv_auto('" + DictSqlEscape(dict) + "')";
	}
	if (ends_with(".parquet")) {
		return "read_parquet('" + DictSqlEscape(dict) + "')";
	}
	if (ends_with(".json")) {
		return "read_json_auto('" + DictSqlEscape(dict) + "')";
	}
	if (dict.find('(') != std::string::npos) {
		return dict; // explicit relation expression — trusted SQL
	}
	if (dict.find('/') != std::string::npos || dict.find('\\') != std::string::npos) {
		return "read_csv_auto('" + DictSqlEscape(dict) + "')"; // path without a known extension
	}
	if (!DictIsSafeIdentifier(dict)) {
		throw BinderException(
		    "dictionary '%s' is not a file path, a valid table/view name, or a relation expression", dict);
	}
	return dict; // validated table / view identifier
}

} // namespace duckdb
