cmake_minimum_required(VERSION 3.5)

# Set extension name here
set(TARGET_NAME erpl_idoc)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD "17" CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# tinyxml2 (via vcpkg) provides IDoc-XML parsing — same as erpl-web.
find_package(tinyxml2 CONFIG REQUIRED)

include_directories(src/include src/idoc)

# The flat-file IDoc core is pure and offline: no SAP / NW RFC SDK, no OpenSSL.
# Live-SAP access is composed via erpl_rfc at the SQL layer (see sql/).
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

# Link tinyxml2 into both the static and loadable extension.
target_link_libraries(${EXTENSION_NAME} tinyxml2::tinyxml2)
target_link_libraries(${LOADABLE_EXTENSION_NAME} tinyxml2::tinyxml2)

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
