# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
cmake_minimum_required(VERSION 3.15)
project(fletcher-c-abi-benchmarks C CXX)

find_package(fletcher-protoc CONFIG REQUIRED)
find_package(fletcher-pubsub CONFIG REQUIRED)
find_package(fletcher-c-abi CONFIG REQUIRED)
find_package(protobuf CONFIG REQUIRED)
find_package(benchmark CONFIG REQUIRED)

if(NOT TARGET fletcher-protoc::plugin)
    message(FATAL_ERROR "fletcher-protoc::plugin target was not created; the generated "
                        "publisher cannot be produced without it.")
endif()

# The protobuf well-known-types include dir, for pubsub.proto's google/protobuf/empty.proto.
# Derived from protoc's own location, as integration-tests/protoc-arrow-bridge does.
foreach(_cfg RELEASE DEBUG MINSIZEREL RELWITHDEBINFO "")
    if(_cfg)
        get_target_property(_protoc_loc protobuf::protoc IMPORTED_LOCATION_${_cfg})
    else()
        get_target_property(_protoc_loc protobuf::protoc IMPORTED_LOCATION)
    endif()
    if(_protoc_loc AND NOT _protoc_loc MATCHES "NOTFOUND")
        get_filename_component(_protoc_dir "${_protoc_loc}" DIRECTORY)
        get_filename_component(PROTOBUF_WKT_INCLUDE_DIR "${_protoc_dir}/../include" ABSOLUTE)
        break()
    endif()
endforeach()
if(NOT PROTOBUF_WKT_INCLUDE_DIR)
    message(FATAL_ERROR "Cannot determine the protobuf well-known-types include dir")
endif()

# ONE row type for every arm, C++ and C#: the `Telemetry` message the protoc-arrow-bridge suite
# already uses for its generated-publisher test. Read from there rather than copied, so there is one
# definition of the row the numbers are about.
set(PROTO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../integration-tests/protoc-arrow-bridge/proto")
set(GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${GENERATED_DIR}")

add_custom_command(
    OUTPUT "${GENERATED_DIR}/pubsub.fletcher.pb.h"
    COMMAND "$<TARGET_FILE:protobuf::protoc>"
            "--plugin=protoc-gen-fletcher=$<TARGET_FILE:fletcher-protoc::plugin>"
            "--fletcher_out=${GENERATED_DIR}"
            "-I" "${PROTO_DIR}"
            "-I" "${FLETCHER_PROTO_INCLUDE_DIR}"
            "-I" "${PROTOBUF_WKT_INCLUDE_DIR}"
            "${PROTO_DIR}/pubsub.proto"
    DEPENDS "${PROTO_DIR}/pubsub.proto" fletcher-protoc::plugin
    COMMENT "protoc-gen-fletcher: pubsub.proto"
    VERBATIM)
add_custom_target(generate_headers DEPENDS "${GENERATED_DIR}/pubsub.fletcher.pb.h")

add_executable(bench_publish bench_publish.cpp)
add_dependencies(bench_publish generate_headers)
target_compile_features(bench_publish PRIVATE cxx_std_20)
target_include_directories(bench_publish PRIVATE "${GENERATED_DIR}")
target_link_libraries(bench_publish
    PRIVATE fletcher-pubsub::fletcher-pubsub
            fletcher-c-abi::fletcher-c-abi
            benchmark::benchmark)
if(NOT WIN32)
    # dlsym(RTLD_DEFAULT, "fbench_alloc_count") - see alloc_count.c.
    target_link_libraries(bench_publish PRIVATE ${CMAKE_DL_LIBS})
endif()

# The allocation counter, Linux only (D-BIND-48). Not linked into anything: it is LD_PRELOADed into
# BOTH harnesses, this one and dotnet/benchmarks, so the two count with one instrument.
if(NOT WIN32)
    add_library(fbench_alloc_count SHARED alloc_count.c)
    set_target_properties(fbench_alloc_count PROPERTIES C_VISIBILITY_PRESET default)
endif()
