PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=erpl_idoc
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# vcpkg provides dependencies (tinyxml2). Same wiring as erpl-web.
VCPKG_TOOLCHAIN_PATH?=${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake

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
# Fast, DuckDB-free unit tests for the pure format core (Catch2). tinyxml2 comes from
# the vcpkg_installed tree that `make debug`/`make release` populate, so run a build
# first. VCPKG_INSTALLED can be overridden to point at another install prefix.
# Derive the vcpkg triplet prefix from wherever tinyxml2.h landed (skips the
# vcpkg_installed/vcpkg metadata dir and is triplet-agnostic).
VCPKG_TXML_H := $(firstword $(wildcard build/debug/vcpkg_installed/*/include/tinyxml2.h) $(wildcard build/release/vcpkg_installed/*/include/tinyxml2.h))
VCPKG_PREFIX := $(patsubst %/include/tinyxml2.h,%,$(VCPKG_TXML_H))
# Format-core tests only — no tinyxml2, no vcpkg (just the bundled Catch2). Suitable
# for a fast CI job that has the duckdb submodule but no vcpkg install.
.PHONY: core_tests_format
core_tests_format:
	mkdir -p build
	g++ -std=c++17 -O0 -g -Isrc/include -Isrc/idoc -Iduckdb/third_party/catch \
	    test/cpp/test_main.cpp test/cpp/test_idoc_format.cpp src/idoc/idoc_format.cpp \
	    -o build/core_tests_format
	IDOC_FIXTURE=test/fixtures/flight.idoc build/core_tests_format

# Full core tests incl. the IDoc-XML core (tinyxml2 from the vcpkg_installed tree that
# `make debug`/`make release` populate — run a build first).
.PHONY: core_tests
core_tests:
	g++ -std=c++17 -O0 -g -Isrc/include -Isrc/idoc -I$(VCPKG_PREFIX)/include -Iduckdb/third_party/catch \
	    test/cpp/test_main.cpp test/cpp/test_idoc_format.cpp test/cpp/test_idoc_xml.cpp \
	    src/idoc/idoc_format.cpp src/idoc/idoc_xml.cpp \
	    -L$(VCPKG_PREFIX)/lib -ltinyxml2 \
	    -o build/core_tests
	IDOC_FIXTURE=test/fixtures/flight.idoc build/core_tests

# Live end-to-end tests against the real A4H trial (require erpl_rfc + a reachable
# SAP system). Each script self-skips if its prerequisites are missing.
.PHONY: e2e
e2e: debug
	@for s in test/e2e/m*.sh; do echo "== $$s =="; bash "$$s" || exit 1; done

# Mass-produce a corpus of standard IDoc types from the live A4H system (via erpl-adt)
# for performance/compatibility testing. Choose the output format with FORMAT and the
# volume with REPEAT, e.g.:
#   make corpus                          # flat IDocs of the default standard types
#   make corpus FORMAT=xml               # IDoc-XML instead
#   make corpus FORMAT=both REPEAT=1000  # both, plus large multi-IDoc volume files
.PHONY: corpus
corpus: debug
	@FORMAT=$(or $(FORMAT),flat) REPEAT=$(or $(REPEAT),1) MIXED=$(or $(MIXED),0) \
		bash scripts/gen_idoc_corpus.sh $(or $(OUT_DIR),corpus)