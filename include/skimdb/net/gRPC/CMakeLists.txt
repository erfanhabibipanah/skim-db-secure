find_package(Protobuf REQUIRED)
find_package(gRPCCompat REQUIRED)

set(PROTO_FILE ${CMAKE_CURRENT_SOURCE_DIR}/proto/skimdb.proto)

get_target_property(GRPC_CPP_PLUGIN gRPC::grpc_cpp_plugin LOCATION)

add_library(skimdb_proto)
target_sources(skimdb_proto PRIVATE ${PROTO_FILE})

protobuf_generate(
    TARGET skimdb_proto
    LANGUAGE cpp
)

protobuf_generate(
    TARGET skimdb_proto
    LANGUAGE grpc
    GENERATE_EXTENSIONS .grpc.pb.h .grpc.pb.cc
    PLUGIN "protoc-gen-grpc=${GRPC_CPP_PLUGIN}"
)

target_include_directories(skimdb_proto
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(skimdb_proto
    PUBLIC
        protobuf::libprotobuf
        gRPC::grpc++
)

add_library(skimdb::proto ALIAS skimdb_proto)

file(RELATIVE_PATH REL_PATH
     ${PROJECT_SOURCE_DIR}
     ${CMAKE_CURRENT_SOURCE_DIR})

install(TARGETS skimdb_proto
    EXPORT skimdbTargets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)

install(DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/proto/
        DESTINATION ${REL_PATH}
        FILES_MATCHING PATTERN "*.pb.h")
