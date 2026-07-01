PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=erpl_idoc
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

# ---------------------------------------------------------------------------
# Convenience targets
# ---------------------------------------------------------------------------

# Run every SQL test one-by-one against the debug unittest binary (the runner
# only accepts a single test path per invocation). Live-SAP tests self-skip
# unless the ERPL_SAP_* env vars are set (see test/sql/*_e2e.test).
UNITTEST=build/debug/test/unittest
.PHONY: sql_tests
sql_tests: debug
	@for t in test/sql/*.test; do echo "== $$t =="; $(UNITTEST) --test-dir . "$$t" || exit 1; done

# Fast, DuckDB-free unit tests for the pure format core (compiled standalone with
# the bundled Catch2 — seconds, no full extension build needed).
.PHONY: core_tests
core_tests:
	g++ -std=c++17 -O0 -g -Isrc/include -Iduckdb/third_party/catch \
	    test/cpp/test_main.cpp test/cpp/test_idoc_format.cpp src/idoc/idoc_format.cpp \
	    -o build/core_tests
	IDOC_FIXTURE=test/fixtures/flight.idoc build/core_tests