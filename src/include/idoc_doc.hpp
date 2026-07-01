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
