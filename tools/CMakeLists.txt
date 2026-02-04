find_package(spdlog 1.6 REQUIRED)
message(STATUS "Found spdlog ${spdlog_VERSION} at ${spdlog_DIR}")

CPMAddPackage(
    NAME cxxopts
    GITHUB_REPOSITORY jarro2783/cxxopts
    VERSION 3.3.1
    OPTIONS
      "CXXOPTS_BUILD_EXAMPLES OFF"
      "CXXOPTS_BUILD_TESTS OFF"
)

CPMAddPackage(
    NAME fmtextra
    GIT_REPOSITORY https://gitlab.com/jzola/fmtextra.git
    GIT_TAG main
)

add_executable(skimdb-index-create skimdb-index-create.cpp)
target_link_libraries(skimdb-index-create PRIVATE skimdb cxxopts::cxxopts spdlog::spdlog fmtextra)

add_executable(skimdb-index-query skimdb-index-query.cpp)
target_link_libraries(skimdb-index-query PRIVATE skimdb cxxopts::cxxopts spdlog::spdlog)

add_executable(skimdb-kmer-generate skimdb-kmer-generate.cpp)
target_link_libraries(skimdb-kmer-generate PRIVATE cxxopts::cxxopts spdlog::spdlog)

install(TARGETS
        skimdb-index-create
        skimdb-index-query
        skimdb-kmer-generate
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(SKIMDB_BUILD_GRPC)
    add_executable(skimdb-index-serve-rpc skimdb-index-serve-rpc.cpp)
    target_link_libraries(skimdb-index-serve-rpc PRIVATE skimdb skimdb_proto cxxopts::cxxopts spdlog::spdlog fmtextra)

    add_executable(skimdb-index-query-rpc skimdb-index-query-rpc.cpp)
    target_link_libraries(skimdb-index-query-rpc PRIVATE skimdb skimdb_proto cxxopts::cxxopts spdlog::spdlog fmtextra)

    install(TARGETS
        skimdb-index-serve-rpc
        skimdb-index-query-rpc
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(SKIMDB_BUILD_SPIR)
    find_package(OpenMP REQUIRED)

    add_executable(skimdb-spir-size skimdb-spir-size.cpp)
    target_link_libraries(skimdb-spir-size PRIVATE skimdb cxxopts::cxxopts spdlog::spdlog fmtextra)

    add_executable(skimdb-spir-query skimdb-spir-query.cpp)
    target_link_libraries(skimdb-spir-query PRIVATE skimdb cxxopts::cxxopts spdlog::spdlog fmtextra OpenMP::OpenMP_CXX)

    install(TARGETS
        skimdb-spir-size
        skimdb-spir-query
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
