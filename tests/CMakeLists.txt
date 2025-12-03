file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/reference
  DESTINATION ${CMAKE_CURRENT_BINARY_DIR})

add_executable(demo_skimdb demo_skimdb.cpp)
target_link_libraries(demo_skimdb PRIVATE skimdb::skimdb)

target_compile_definitions(demo_skimdb PRIVATE 
  REFERENCE_DIR="${CMAKE_CURRENT_BINARY_DIR}/reference")
