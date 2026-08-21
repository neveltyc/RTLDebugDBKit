# Copyright (c) 2026 neveltyc
# released under the BSD 3-Clause License (see LICENSE)
#
# rtl-designdb — the design-database exporter.
#
# The export is a whole-design traversal, which is why it is C++ against slang
# directly: measured on the same design and the same slang revision, a pyslang
# walk needs ~23 s and ~1.0 GB where slang's own takes ~0.08 s and ~109 MB.

cmake_minimum_required(VERSION 3.20)

# C as well as C++: the bundled SQLite amalgamation is sqlite3.c, and the
# generate step fails with CMAKE_C_COMPILE_OBJECT unset if C is not enabled
# here. It surfaces only in the default configuration -- building against a
# system libsqlite3 compiles no C of our own and generates fine either way.
project(
  rtl-designdb
  VERSION 0.1.0
  LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------- dependencies
# CPM is fetched rather than vendored so the pin lives in one place (below).
set(CPM_DOWNLOAD_VERSION 0.40.2)
set(CPM_DOWNLOAD_LOCATION "${CMAKE_BINARY_DIR}/cmake/CPM_${CPM_DOWNLOAD_VERSION}.cmake")
if(NOT EXISTS ${CPM_DOWNLOAD_LOCATION})
  message(STATUS "Downloading CPM.cmake ${CPM_DOWNLOAD_VERSION}")
  file(DOWNLOAD
       "https://github.com/cpm-cmake/CPM.cmake/releases/download/v${CPM_DOWNLOAD_VERSION}/CPM.cmake"
       ${CPM_DOWNLOAD_LOCATION})
endif()
include(${CPM_DOWNLOAD_LOCATION})

# slang is pinned to an exact revision, not a range. The exporter reads private
# analysis structure (driver bit ranges, canonical instance bodies), so an
# upgrade is a deliberate, differential-tested step rather than a float.
# Mind the tag spelling: slang tags a release `vN.M`, with no patch component --
# `v11.0`, never `v11.0.0`. A three-part tag does not exist upstream, and CPM
# reports the miss as a failed fetch rather than as a pin that names nothing.
set(DESIGNDB_SLANG_TAG "v11.0")
cpmaddpackage(
  NAME slang
  GITHUB_REPOSITORY MikePopoloski/slang
  GIT_TAG ${DESIGNDB_SLANG_TAG}
  OPTIONS
    "SLANG_INCLUDE_TESTS OFF"
    "SLANG_INCLUDE_TOOLS OFF"
    "SLANG_INCLUDE_DOCS OFF"
    "SLANG_INCLUDE_PYLIB OFF"
  EXCLUDE_FROM_ALL OFF)

# SQLite: the official amalgamation, so the binary carries its own copy and the
# database does not depend on whatever libsqlite3 the host happens to ship.
option(DESIGNDB_SYSTEM_SQLITE "Link the host's libsqlite3 instead of bundling" OFF)
if(DESIGNDB_SYSTEM_SQLITE)
  find_package(SQLite3 REQUIRED)
  add_library(designdb_sqlite INTERFACE)
  target_link_libraries(designdb_sqlite INTERFACE SQLite::SQLite3)
else()
  set(SQLITE_AMALG_URL
      "https://www.sqlite.org/2025/sqlite-amalgamation-3500400.zip")
  include(FetchContent)
  FetchContent_Declare(sqlite_amalg URL ${SQLITE_AMALG_URL})
  FetchContent_MakeAvailable(sqlite_amalg)
  add_library(designdb_sqlite STATIC "${sqlite_amalg_SOURCE_DIR}/sqlite3.c")
  target_include_directories(designdb_sqlite PUBLIC "${sqlite_amalg_SOURCE_DIR}")
  # No loadable extensions: the exporter only ever writes its own schema, and
  # leaving the hook in would let a database influence the process that opens it.
  target_compile_definitions(designdb_sqlite PRIVATE
    SQLITE_OMIT_LOAD_EXTENSION=1 SQLITE_THREADSAFE=0 SQLITE_DQS=0)
endif()

# ---------------------------------------------------------------- the exporter
add_executable(rtl-designdb
  src/main.cpp
  src/DesignDb.cpp
  src/Extractor.cpp
  src/extract/TemplateBuilder.cpp
  src/extract/Stamper.cpp)

target_include_directories(rtl-designdb PRIVATE src)
target_link_libraries(rtl-designdb PRIVATE slang::slang designdb_sqlite)

# The commit the exporter was built from. `PROJECT_VERSION` moves once a
# release; the extraction semantics move far more often than that, so two
# databases carrying the same tool_version can still disagree about what they
# hold. This is what tells them apart. `--dirty` because a build from an edited
# tree is not the commit it names. Empty outside a git checkout -- a release
# tarball has no history -- and the meta row then says "unknown" rather than
# claiming a revision that was never there.
find_package(Git QUIET)
set(DESIGNDB_PRODUCER_REVISION "unknown")
if(GIT_FOUND AND EXISTS "${PROJECT_SOURCE_DIR}/.git")
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" describe --always --dirty --tags
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    OUTPUT_VARIABLE DESIGNDB_GIT_DESCRIBE
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
  if(DESIGNDB_GIT_DESCRIBE)
    set(DESIGNDB_PRODUCER_REVISION "${DESIGNDB_GIT_DESCRIBE}")
  endif()
endif()
message(STATUS "Producer revision : ${DESIGNDB_PRODUCER_REVISION}")

target_compile_definitions(rtl-designdb PRIVATE
  RTLDESIGNDB_VERSION="${PROJECT_VERSION}"
  RTLDESIGNDB_SLANG_TAG="${DESIGNDB_SLANG_TAG}"
  RTLDESIGNDB_PRODUCER_REVISION="${DESIGNDB_PRODUCER_REVISION}")

install(TARGETS rtl-designdb RUNTIME DESTINATION bin)
