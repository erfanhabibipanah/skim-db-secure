cmake_minimum_required(VERSION 3.22)
project(skimdb VERSION 0.0.1 LANGUAGES CXX)

option(SKIMDB_BUILD_TOOLS "Build tool programs" OFF)
option(SKIMDB_BUILD_GRPC "Build gRPC client/server" OFF)
option(SKIMDB_BUILD_SPIR "Build SPIR implementation" OFF)

# Compile commands
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# C++ setup
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake/")

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
    GIT_REPOSITORY https://gitlab.com/SCoRe-Group/dgpp.git
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
    NAME phmap
    GITHUB_REPOSITORY greg7mdp/parallel-hashmap
    VERSION 2.0.0
)

CPMAddPackage(
    NAME fastxrd
    GIT_REPOSITORY https://gitlab.com/SCoRe-Group/fastxrd.git
    GIT_TAG main
)

CPMAddPackage(
    NAME fmtextra
    GIT_REPOSITORY https://gitlab.com/jzola/fmtextra.git
    GIT_TAG main
)

# required for par execution policy
find_package(TBB REQUIRED)

# -------------------------------------------------------------------
# Library target
# -------------------------------------------------------------------

add_library(skimdb INTERFACE)
add_library(skimdb::skimdb ALIAS skimdb)

target_include_directories(skimdb INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(skimdb INTERFACE
    cereal
    dgpp
    fastxrd
    fmtextra
    phmap
    roaring
    TBB::tbb
)

install(
    DIRECTORY include/
    DESTINATION include
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

# SPIR
if (SKIMDB_BUILD_SPIR)
    add_subdirectory(include/skimdb/spir/net/gRPC)
endif()
