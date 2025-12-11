find_package(spdlog 1.6 REQUIRED)
message(STATUS "Found spdlog ${spdlog_VERSION} at ${spdlog_DIR}")

CPMAddPackage(
    NAME cxxopts
    GITHUB_REPOSITORY jarro2783/cxxopts
    VERSION 3.1.1
    OPTIONS
      "CXXOPTS_BUILD_EXAMPLES OFF"
      "CXXOPTS_BUILD_TESTS OFF"
)

add_executable(skimdb-index-create skimdb-index-create.cpp)
target_link_libraries(skimdb-index-create PRIVATE skimdb cxxopts::cxxopts spdlog::spdlog)

add_executable(skimdb-index-query skimdb-index-query.cpp)
target_link_libraries(skimdb-index-query PRIVATE skimdb cxxopts::cxxopts spdlog::spdlog)

install(TARGETS
        skimdb-index-create
        skimdb-index-query
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
