# This file is included by DuckDB's build system. It specifies which extension to load

# GCC 14+ (the rolling ci-tools Linux image) emits STB_GNU_UNIQUE defs for ODR-used
# DuckDB class statics, which collide with core's strong defs at link (fails duckdb's
# own tools/plan_serializer). Apply -fno-gnu-unique globally on GCC so they become weak
# COMDAT defs. Kept here (not only in our MainDistributionPipeline.yml) so builders that
# don't pass extra_extension_config — notably duckdb/community-extensions — also get it.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-gnu-unique")
endif()

# Extension from this repo
# DuckDB v1.5.6 stamps the short commit SHA instead of the release tag: its duckdb_extension_generate_version
# passes --match '${VERSIONING_TAG_MATCH}' with literal single quotes, so no tag ever matches. Pass the version
# explicitly when HEAD is on a vX.Y.Z tag (2.0.x detects it correctly too; this keeps both lines identical).
set(ERPL_IDOC_EXT_VERSION "")
find_package(Git QUIET)
if(Git_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --always --match "v*.*.*"
        WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
        RESULT_VARIABLE ERPL_IDOC_GIT_RESULT
        OUTPUT_VARIABLE ERPL_IDOC_GIT_DESCRIBE
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(ERPL_IDOC_GIT_RESULT EQUAL 0 AND ERPL_IDOC_GIT_DESCRIBE MATCHES "^v[0-9]+\\.[0-9]+\\.[0-9]+(-[a-zA-Z0-9\\.]+)?$")
        set(ERPL_IDOC_EXT_VERSION "${ERPL_IDOC_GIT_DESCRIBE}")
    endif()
endif()
if(ERPL_IDOC_EXT_VERSION STREQUAL "")
    duckdb_extension_load(erpl_idoc
        SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    )
else()
    message(STATUS "erpl_idoc extension version ${ERPL_IDOC_EXT_VERSION}")
    duckdb_extension_load(erpl_idoc
        SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
        EXTENSION_VERSION ${ERPL_IDOC_EXT_VERSION}
    )
endif()

# erpl_rfc is an OPTIONAL runtime dependency (typed-mode dictionary + live-SAP recipes).
# It is NOT linked into erpl_idoc; typed/E2E SQL tests LOAD a prebuilt erpl_rfc binary
# in the same DuckDB session. The generic flat-file core builds and works without it.
