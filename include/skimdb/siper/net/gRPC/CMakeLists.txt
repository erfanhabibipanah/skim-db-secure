find_package(gRPCCompat REQUIRED)
get_target_property(GRPC_CPP_PLUGIN gRPC::grpc_cpp_plugin LOCATION)

find_program(PROTOC_EXECUTABLE protoc REQUIRED)
find_package(Protobuf REQUIRED)

set(PROTOBUF_LIB ${Protobuf_LIBRARIES})
set(PROTOBUF_INCLUDE ${Protobuf_INCLUDE_DIRS})

set(PROTO_FILE ${CMAKE_CURRENT_SOURCE_DIR}/proto/siperdb.proto)
set(PROTO_GEN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/proto/generated)

set(PROTO_SRCS
    ${PROTO_GEN_DIR}/siperdb.pb.cc
    ${PROTO_GEN_DIR}/siperdb.grpc.pb.cc
)
set(PROTO_HDRS
    ${PROTO_GEN_DIR}/siperdb.pb.h
    ${PROTO_GEN_DIR}/siperdb.grpc.pb.h
)

execute_process(
    COMMAND ${PROTOC_EXECUTABLE} --version
    OUTPUT_VARIABLE PROTOC_VERSION_STRING
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

set(PROTO_VERSION_STAMP ${PROTO_GEN_DIR}/.protoc_version)

file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/protoc_version_candidate.txt
    "${PROTOC_VERSION_STRING}\n")

add_custom_command(
    OUTPUT ${PROTO_VERSION_STAMP}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${PROTO_GEN_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_BINARY_DIR}/protoc_version_candidate.txt
        ${PROTO_VERSION_STAMP}
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/protoc_version_candidate.txt
    COMMENT "Checking protoc version stamp for siperdb"
    VERBATIM
)

add_custom_command(
    OUTPUT
        ${PROTO_SRCS}
        ${PROTO_HDRS}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${PROTO_GEN_DIR}
    COMMAND ${PROTOC_EXECUTABLE}
        --cpp_out=${PROTO_GEN_DIR}
        --grpc_out=${PROTO_GEN_DIR}
        --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}
        -I ${CMAKE_CURRENT_SOURCE_DIR}/proto
        ${PROTO_FILE}
    DEPENDS
        ${PROTO_FILE}
        ${PROTO_VERSION_STAMP}
    COMMENT "Regenerating gRPC sources from ${PROTO_FILE}"
    VERBATIM
)

add_custom_target(siperdb_proto_gen
    DEPENDS ${PROTO_SRCS} ${PROTO_HDRS}
)

add_library(siperdb_proto
    ${PROTO_SRCS}
    ${PROTO_HDRS}
)
add_dependencies(siperdb_proto siperdb_proto_gen)

target_include_directories(siperdb_proto
    PUBLIC
        $<BUILD_INTERFACE:${PROTO_GEN_DIR}>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(siperdb_proto
    PUBLIC
        protobuf::libprotobuf
        gRPC::grpc++
)

add_library(siperdb::proto ALIAS siperdb_proto)

file(RELATIVE_PATH REL_PATH
     ${PROJECT_SOURCE_DIR}
     ${CMAKE_CURRENT_SOURCE_DIR})

install(TARGETS siperdb_proto
    EXPORT siperdbTargets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)
install(DIRECTORY ${PROTO_GEN_DIR}/
        DESTINATION ${REL_PATH}/proto/generated
        FILES_MATCHING PATTERN "*.pb.h")
