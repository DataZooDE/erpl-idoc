#pragma once

// Helpers to register table/scalar functions with full inline documentation
// (description + examples + categories + parameter names), following the erpl
// convention so the functions are self-documenting via duckdb_functions().

#include "duckdb.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"

namespace duckdb {

inline void RegisterDocTableFunction(ExtensionLoader &loader, TableFunction fun, string description,
                                     vector<string> examples, vector<string> parameter_names = {}) {
	CreateTableFunctionInfo info(std::move(fun));
	FunctionDescription d;
	d.description = std::move(description);
	d.examples = std::move(examples);
	d.categories = {"sap", "idoc"};
	d.parameter_names = std::move(parameter_names);
	info.descriptions.push_back(std::move(d));
	loader.RegisterFunction(std::move(info));
}

// Same, for an overload set (e.g. a reader that accepts either a VARCHAR path/glob or
// a LIST(VARCHAR) of paths). The description applies to every overload.
//
// Push exactly ONE description, never one per overload.
//
// duckdb_functions() picks a description by matching FunctionDescription::parameter_types
// against the overload's types, and CalcDescriptionSpecificity rejects any candidate whose
// parameter_types has a different SIZE. There is one escape hatch: when descriptions
// holds exactly one entry it is used for every overload regardless.
//
// This helper sets no parameter_types -- deliberately, since the whole point of these
// sets is that the same function accepts a VARCHAR or a LIST(VARCHAR) first argument, and
// pinning types would knock out one of them. So pushing N copies meant N candidates of
// size 0 against an overload of size 1 or more: every one disqualified, and the whole
// reader family reported NULL descriptions and col0 parameter names even though the call
// sites below supply both. The metadata was written correctly and thrown away silently.
inline void RegisterDocTableFunctionSet(ExtensionLoader &loader, TableFunctionSet set, string description,
                                        vector<string> examples, vector<string> parameter_names = {}) {
	CreateTableFunctionInfo info(std::move(set));
	FunctionDescription d;
	d.description = std::move(description);
	d.examples = std::move(examples);
	d.categories = {"sap", "idoc"};
	d.parameter_names = std::move(parameter_names);
	info.descriptions.push_back(std::move(d));
	loader.RegisterFunction(std::move(info));
}

inline void RegisterDocScalarFunction(ExtensionLoader &loader, ScalarFunction fun, string description,
                                      vector<string> examples, vector<string> parameter_names = {}) {
	CreateScalarFunctionInfo info(std::move(fun));
	FunctionDescription d;
	d.description = std::move(description);
	d.examples = std::move(examples);
	d.categories = {"sap", "idoc"};
	d.parameter_names = std::move(parameter_names);
	info.descriptions.push_back(std::move(d));
	loader.RegisterFunction(std::move(info));
}

} // namespace duckdb
