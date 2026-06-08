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
        -g
        -O0
        -fno-omit-frame-pointer
        -Wall
        -Wextra
        -Wpedantic
    >

    # Release
    $<$<CONFIG:Release>:
        -O3
        -ffast-math
        -funroll-loops
#       clang only
#        -Rpass=loop-vectorize              # report successfully vectorized loops
#        -Rpass-missed=loop-vectorize       # report failed vectorization with reason
#        -Rpass-analysis=loop-vectorize     # extra detail on why it failed
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
skimdb_add_tool(skimdb-index-analyze)
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
# SiPeR tools
# --------------------------------------------------------------------

if(SKIMDB_BUILD_SIPER)
    find_package(OpenSSL REQUIRED)
    skimdb_add_tool(skimdb-siper-create siperdb::proto OpenSSL::Crypto)
    skimdb_add_tool(skimdb-siper-dbsize OpenSSL::Crypto)
    skimdb_add_tool(skimdb-siper-reconfigure OpenSSL::Crypto)
    skimdb_add_tool(skimdb-siper-query OpenSSL::Crypto)
    skimdb_add_tool(skimdb-siper-serve-rpc siperdb::proto OpenSSL::Crypto)
    skimdb_add_tool(skimdb-siper-query-rpc siperdb::proto OpenSSL::Crypto)
    skimdb_add_tool(skimdb-siper-query-rpc-flood siperdb::proto OpenSSL::Crypto)
    skimdb_add_tool(skimdb-siper-batched-rpc siperdb::proto OpenSSL::Crypto)
    skimdb_add_tool(skimdb-siper-batched-rpc-flood siperdb::proto OpenSSL::Crypto)
endif()
