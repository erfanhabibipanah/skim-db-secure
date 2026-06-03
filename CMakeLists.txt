cmake_minimum_required(VERSION 3.22)
project(skimdb VERSION 0.2.3 LANGUAGES CXX)

# Get git commit hash
find_package(Git QUIET)

if(GIT_FOUND)
  execute_process(
    COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
endif()
if(NOT DEFINED GIT_COMMIT OR GIT_COMMIT STREQUAL "")
  set(GIT_COMMIT "unknown")
endif()

configure_file(
  ${CMAKE_SOURCE_DIR}/cmake/skimdb_version.h.in
  ${CMAKE_BINARY_DIR}/include/skimdb/skimdb_version.h
  @ONLY
)

option(SKIMDB_64BIT "Build with 64bit k-mer encoding" OFF)
option(SKIMDB_BUILD_TOOLS "Build tool programs" OFF)
option(SKIMDB_BUILD_GRPC "Build gRPC client/server" OFF)
option(SKIMDB_BUILD_SIPER "Build SiPeR implementation" OFF)

# Compile commands
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# C++ setup
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake/")

include(CheckCXXSourceCompiles)

check_cxx_source_compiles("
    void foo(int* __restrict__ a, int* __restrict__ b) { *a = *b; }
    int main() { return 0; }
" HAVE_RESTRICT)

if(NOT HAVE_RESTRICT)
    message(FATAL_ERROR "Compiler does not support __restrict__")
endif()

# -------------------------------------------------------------------
# Dependencies
# -------------------------------------------------------------------

set(CPM_DOWNLOAD_VERSION 0.42.1)
set(CPM_EXPECTED_HASH SHA256=f3a6dcc6a04ce9e7f51a127307fa4f699fb2bade357a8eb4c5b45df76e1dc6a5)

set(CPM_DOWNLOAD_LOCATION
    ${CMAKE_BINARY_DIR}/cmake/CPM_${CPM_DOWNLOAD_VERSION}.cmake
)

if(NOT EXISTS ${CPM_DOWNLOAD_LOCATION})
    file(DOWNLOAD
        https://github.com/cpm-cmake/CPM.cmake/releases/download/v${CPM_DOWNLOAD_VERSION}/CPM.cmake
        ${CPM_DOWNLOAD_LOCATION}
        EXPECTED_HASH ${CPM_EXPECTED_HASH}
    )
endif()

include(${CPM_DOWNLOAD_LOCATION})

# persistent dependency cache
set(CPM_SOURCE_CACHE ${CMAKE_SOURCE_DIR}/.cpm-cache)

CPMAddPackage(
    NAME cereal
    GITHUB_REPOSITORY USCiLab/cereal
    VERSION 1.3.2
    OPTIONS "JUST_INSTALL_CEREAL ON"
)

CPMAddPackage(
    NAME dgpp
    GIT_REPOSITORY https://codeberg.org/SCoRe-Group/dgpp.git
    GIT_TAG main
)

CPMAddPackage(
    NAME roaring
    GITHUB_REPOSITORY RoaringBitmap/CRoaring
    GIT_TAG v4.6.1
    OPTIONS "ENABLE_ROARING_TESTS OFF"
    EXCLUDE_FROM_ALL YES
)

CPMAddPackage(
    NAME bbhash
    GIT_REPOSITORY https://codeberg.org/SCoRe-Group/bbhash.git
    GIT_TAG main
)

CPMAddPackage(
    NAME fastxrd
    GIT_REPOSITORY https://codeberg.org/SCoRe-Group/fastxrd.git
    GIT_TAG main
)

CPMAddPackage(
    NAME fmtextra
    GIT_REPOSITORY https://codeberg.org/SCoRe-Group/fmtextra.git
    GIT_TAG main
)

# required for par execution policy and other parallel things
find_package(TBB REQUIRED)
find_package(OpenMP REQUIRED)

# -------------------------------------------------------------------
# Library target
# -------------------------------------------------------------------

add_library(skimdb INTERFACE)
add_library(skimdb::skimdb ALIAS skimdb)

if(SKIMDB_64BIT)
    target_compile_definitions(skimdb INTERFACE SKIMDB_64BIT)
endif()

target_include_directories(skimdb INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(skimdb INTERFACE
    cereal
    dgpp
    fastxrd
    fmtextra
    bbhash
    roaring
    TBB::tbb
    OpenMP::OpenMP_CXX
)

install(
    DIRECTORY include/
    DESTINATION include
)

install(
  FILES "${CMAKE_BINARY_DIR}/include/skimdb/skimdb_version.h"
  DESTINATION include/skimdb
)

# -------------------------------------------------------------------
# Tools
if(SKIMDB_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

# gRPC
if(SKIMDB_BUILD_GRPC)
    add_subdirectory(include/skimdb/net/gRPC)
endif()

# SiPeR
if (SKIMDB_BUILD_SIPER)
    add_subdirectory(include/skimdb/siper/net/gRPC)
endif()
