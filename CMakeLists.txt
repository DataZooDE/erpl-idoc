cmake_minimum_required(VERSION 3.5)

# Set extension name here
set(TARGET_NAME erpl_idoc)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD "17" CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# tinyxml2 (IDoc-XML) and OpenSSL (used by the PostHog telemetry HTTPS client) — vcpkg.
find_package(tinyxml2 CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)

include_directories(
    src/include
    src/idoc
    ${CMAKE_CURRENT_SOURCE_DIR}/duckdb/third_party
    ${CMAKE_CURRENT_SOURCE_DIR}/duckdb/third_party/httplib
    posthog-telemetry/include
)

# PostHog telemetry (same anonymous, opt-out telemetry as erpl / erpl-web).
# posthog-telemetry/CMakeLists.txt links OpenSSL itself, so we don't add it here
# (linking a subdir target's libs from the parent needs CMP0079/CMake 3.13+).
set(DUCKDB_THIRD_PARTY "${CMAKE_CURRENT_SOURCE_DIR}/duckdb/third_party")
add_subdirectory(posthog-telemetry)

# The flat-file IDoc core is pure and offline (no SAP / NW RFC SDK). Live-SAP access
# is composed via erpl_rfc at the SQL layer (see sql/).
set(EXTENSION_SOURCES
    src/erpl_idoc_extension.cpp
    src/idoc/idoc_format.cpp
    src/idoc/idoc_xml.cpp
    src/idoc_reader.cpp
    src/idoc_typed_reader.cpp
    src/idoc_encoder.cpp
    src/idoc_copy.cpp
    src/idoc_macros.cpp
    src/idoc_dict.cpp
    src/idoc_xml_funcs.cpp
)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# Force C++17 on the extension objects (match DuckDB core).
set_target_properties(${EXTENSION_NAME} ${LOADABLE_EXTENSION_NAME}
                      PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

# GCC 16 emits STB_GNU_UNIQUE ('u') definitions for ODR-used DuckDB class statics
# (LogicalType::VARCHAR/BOOLEAN/INTEGER, FileFlags::FILE_FLAGS_READ, ...) in our TUs.
# DuckDB core emits these as ordinary strong defs, so the linker sees "multiple
# definition" (unique vs strong). -fno-gnu-unique turns ours into weak COMDAT defs,
# which the strong core def cleanly overrides. GCC-only; harmless elsewhere.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(${EXTENSION_NAME} PRIVATE -fno-gnu-unique)
    target_compile_options(${LOADABLE_EXTENSION_NAME} PRIVATE -fno-gnu-unique)
endif()

# Link tinyxml2, telemetry, and OpenSSL into both the static and loadable extension.
target_link_libraries(${EXTENSION_NAME} tinyxml2::tinyxml2 posthog_telemetry OpenSSL::SSL OpenSSL::Crypto)
target_link_libraries(${LOADABLE_EXTENSION_NAME} tinyxml2::tinyxml2 posthog_telemetry OpenSSL::SSL OpenSSL::Crypto)

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
