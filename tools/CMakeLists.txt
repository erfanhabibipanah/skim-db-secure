find_package(TBB REQUIRED)

add_executable(skimdb-index-create skimdb-index-create.cpp)
target_link_libraries(skimdb-index-create PRIVATE
  cxxopts::cxxopts skimdb::skimdb TBB::tbb)

install(TARGETS
        skimdb-index-create
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
