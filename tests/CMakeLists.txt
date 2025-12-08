find_package(TBB REQUIRED)
add_executable(demo_skimdb demo_skimdb.cpp)
target_link_libraries(demo_skimdb PRIVATE skimdb::skimdb TBB::tbb)
