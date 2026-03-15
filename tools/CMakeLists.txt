find_package(spdlog CONFIG REQUIRED)
message(STATUS "Found spdlog ${spdlog_VERSION} at ${spdlog_DIR}")

CPMAddPackage(
    NAME cxxopts
    GITHUB_REPOSITORY jarro2783/cxxopts
    VERSION 3.3.1
    OPTIONS
        "CXXOPTS_BUILD_EXAMPLES OFF"
        "CXXOPTS_BUILD_TESTS OFF"
)

# --------------------------------------------------------------------
# Tool compile flags
# --------------------------------------------------------------------

add_library(skimdb_tooling_flags INTERFACE)

target_compile_definitions(skimdb_tooling_flags INTERFACE
    SPDLOG_USE_STD_FORMAT
)

target_compile_options(skimdb_tooling_flags INTERFACE

    # Debug
    $<$<CONFIG:Debug>:
        -O0
        -g3
        -fno-omit-frame-pointer
        -Wall
        -Wextra
        -Wpedantic
        -Wno-implicit-fallthrough
    >

    # Release
    $<$<CONFIG:Release>:
        -O3
        -march=native
        -ffast-math
        -funroll-loops
    >
)

# --------------------------------------------------------------------
# Tool dependencies
# --------------------------------------------------------------------

add_library(skimdb_tool_deps INTERFACE)

target_link_libraries(skimdb_tool_deps INTERFACE
    skimdb
    cxxopts::cxxopts
    spdlog::spdlog_header_only
    fmtextra
    skimdb_tooling_flags
)

# --------------------------------------------------------------------
# Tool helper
# --------------------------------------------------------------------

function(skimdb_add_tool name)
    add_executable(${name} ${name}.cpp)

    target_link_libraries(${name} PRIVATE
        skimdb_tool_deps
        ${ARGN}
    )

    install(TARGETS ${name}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endfunction()

# --------------------------------------------------------------------
# Base tools
# --------------------------------------------------------------------

skimdb_add_tool(skimdb-index-create)
skimdb_add_tool(skimdb-index-query)
skimdb_add_tool(skimdb-kmer-generate)

# --------------------------------------------------------------------
# gRPC tools
# --------------------------------------------------------------------

if(SKIMDB_BUILD_GRPC)
    skimdb_add_tool(skimdb-index-serve-rpc skimdb::proto)
    skimdb_add_tool(skimdb-index-query-rpc skimdb::proto)
endif()

# --------------------------------------------------------------------
# SPIR tools
# --------------------------------------------------------------------

if(SKIMDB_BUILD_SPIR)
    find_package(OpenMP REQUIRED)
    skimdb_add_tool(skimdb-spir-create spirdb::proto OpenMP::OpenMP_CXX)
    skimdb_add_tool(skimdb-spir-dbsize)
    skimdb_add_tool(skimdb-spir-query OpenMP::OpenMP_CXX)
    skimdb_add_tool(skimdb-spir-serve-rpc spirdb::proto OpenMP::OpenMP_CXX)
    skimdb_add_tool(skimdb-spir-query-rpc spirdb::proto OpenMP::OpenMP_CXX)
endif()
