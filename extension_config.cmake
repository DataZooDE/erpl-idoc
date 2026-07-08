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
duckdb_extension_load(erpl_idoc
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
)

# erpl_rfc is an OPTIONAL runtime dependency (typed-mode dictionary + live-SAP recipes).
# It is NOT linked into erpl_idoc; typed/E2E SQL tests LOAD a prebuilt erpl_rfc binary
# in the same DuckDB session. The generic flat-file core builds and works without it.
