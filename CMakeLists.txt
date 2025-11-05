# we keep 3.22 to support some older NVIDIA devices
cmake_minimum_required(VERSION 3.22)

project(skimdb
  VERSION 0.0.1
  LANGUAGES CXX
  DESCRIPTION "Memory Efficient Metagenomic Classifier for ONT Reads.")

# for clangd compatibility
set(CMAKE_EXPORT_COMPILE_COMMANDS 1)

# configure build information
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(skimdb INTERFACE)
add_library(skimdb::skimdb ALIAS skimdb)
add_subdirectory(include)
