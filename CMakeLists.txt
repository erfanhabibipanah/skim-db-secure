cmake_minimum_required(VERSION 3.22)
project(skimdb VERSION 0.0.1 LANGUAGES CXX)

# Compile commands
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# C++ setup
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# -------------------------------------------------------------------
# Dependencies via CPM
# -------------------------------------------------------------------

file(DOWNLOAD
    https://github.com/cpm-cmake/CPM.cmake/releases/download/v0.40.8/CPM.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/cmake/CPM.cmake
    EXPECTED_HASH SHA256=78ba32abdf798bc616bab7c73aac32a17bbd7b06ad9e26a6add69de8f3ae4791
)
include(${CMAKE_CURRENT_BINARY_DIR}/cmake/CPM.cmake)

CPMAddPackage(
    NAME cereal
    GITHUB_REPOSITORY USCiLab/cereal
    VERSION 1.3.2
)

CPMAddPackage(
    NAME roaring
    GITHUB_REPOSITORY RoaringBitmap/CRoaring
    GIT_TAG v2.0.4
    OPTIONS "ENABLE_ROARING_TESTS OFF"
)

CPMAddPackage(
    NAME cxxopts
    GITHUB_REPOSITORY jarro2783/cxxopts
    VERSION 3.1.1
    OPTIONS
      "CXXOPTS_BUILD_EXAMPLES OFF"
      "CXXOPTS_BUILD_TESTS OFF"
)

CPMAddPackage(
    NAME fastxrd
    GIT_REPOSITORY git@gitlab.com:SCoRe-Group/fastxrd.git
    GIT_TAG main
)

# fastxrd does not declare cxxopts => fix it
target_link_libraries(fastxrd INTERFACE cxxopts)

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
    fastxrd
    roaring
)