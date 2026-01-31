include(FindPackageHandleStandardArgs)

message(STATUS "Searching for gRPC")

find_package(gRPC CONFIG QUIET)

if(gRPC_FOUND)
    message(STATUS "Found gRPC via CMake config")
    set(gRPCCompat_FOUND TRUE)
    find_package_handle_standard_args(gRPCCompat DEFAULT_MSG gRPCCompat_FOUND)
    return()
endif()

message(STATUS "gRPC CMake config not found, falling back to pkg-config")

find_package(PkgConfig QUIET)

if(NOT PkgConfig_FOUND)
    set(gRPCCompat_FOUND FALSE)
    find_package_handle_standard_args(gRPCCompat DEFAULT_MSG gRPCCompat_FOUND)
    return()
endif()

pkg_check_modules(GRPC_PKG   QUIET grpc)
pkg_check_modules(GRPCXX_PKG QUIET grpc++)

if(NOT GRPCXX_PKG_FOUND)
    set(gRPCCompat_FOUND FALSE)
    find_package_handle_standard_args(gRPCCompat DEFAULT_MSG gRPCCompat_FOUND)
    return()
endif()

function(_grpc_resolve_library out_var libname search_paths)
    find_library(${out_var}
        NAMES ${libname}
        HINTS ${search_paths}
        PATH_SUFFIXES lib lib64
    )
endfunction()

_grpc_resolve_library(GRPC_LIB   grpc   "${GRPC_PKG_LIBRARY_DIRS}")
_grpc_resolve_library(GRPCPP_LIB grpc++ "${GRPCXX_PKG_LIBRARY_DIRS}")

if(NOT GRPCPP_LIB)
    set(gRPCCompat_FOUND FALSE)
    find_package_handle_standard_args(gRPCCompat DEFAULT_MSG gRPCCompat_FOUND)
    return()
endif()

separate_arguments(GRPCXX_LDFLAGS_LIST NATIVE_COMMAND "${GRPCXX_PKG_LDFLAGS}")

if(GRPC_LIB AND NOT TARGET gRPC::grpc)
    add_library(gRPC::grpc UNKNOWN IMPORTED GLOBAL)
    set_target_properties(gRPC::grpc PROPERTIES
        IMPORTED_LOCATION "${GRPC_LIB}"
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${GRPC_PKG_INCLUDE_DIRS}"
        INTERFACE_LINK_LIBRARIES Threads::Threads
    )
endif()

if(NOT TARGET gRPC::grpc++)
    add_library(gRPC::grpc++ UNKNOWN IMPORTED GLOBAL)
    set_target_properties(gRPC::grpc++ PROPERTIES
        IMPORTED_LOCATION "${GRPCPP_LIB}"
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${GRPCXX_PKG_INCLUDE_DIRS}"
        INTERFACE_LINK_LIBRARIES "gRPC::grpc;${GRPCXX_LDFLAGS_LIST}"
    )
endif()

find_program(GRPC_CPP_PLUGIN grpc_cpp_plugin
    HINTS ${GRPCXX_PKG_PREFIX} ${GRPC_PKG_PREFIX}
    PATH_SUFFIXES bin
)

if(GRPC_CPP_PLUGIN AND NOT TARGET gRPC::grpc_cpp_plugin)
    add_executable(gRPC::grpc_cpp_plugin IMPORTED GLOBAL)
    set_target_properties(gRPC::grpc_cpp_plugin PROPERTIES
        IMPORTED_LOCATION "${GRPC_CPP_PLUGIN}"
    )
endif()

set(gRPC_FOUND TRUE)
set(gRPCCompat_FOUND TRUE)

find_package_handle_standard_args(gRPCCompat DEFAULT_MSG gRPCCompat_FOUND)
