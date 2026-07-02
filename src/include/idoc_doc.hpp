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
// a LIST(VARCHAR) of paths). The description is attached to every overload.
inline void RegisterDocTableFunctionSet(ExtensionLoader &loader, TableFunctionSet set, string description,
                                        vector<string> examples, vector<string> parameter_names = {}) {
	auto n = set.functions.size();
	CreateTableFunctionInfo info(std::move(set));
	for (idx_t i = 0; i < n; i++) {
		FunctionDescription d;
		d.description = description;
		d.examples = examples;
		d.categories = {"sap", "idoc"};
		d.parameter_names = parameter_names;
		info.descriptions.push_back(std::move(d));
	}
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
