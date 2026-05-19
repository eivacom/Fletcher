# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
cmake_minimum_required(VERSION 3.15)
project(fletcher_protoc_test LANGUAGES CXX)

find_package(eiva-fletcher-protoc REQUIRED)
find_package(protobuf REQUIRED)

if(NOT TARGET eiva-fletcher-protoc::plugin)
    message(FATAL_ERROR "eiva-fletcher-protoc::plugin target not found")
endif()

set(PROTO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/proto")
set(GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${GENERATED_DIR}")

# Generate C++ header from test.proto.
add_custom_command(
    OUTPUT "${GENERATED_DIR}/test.fletcher.pb.h"
    COMMAND "$<TARGET_FILE:protobuf::protoc>"
            "--plugin=protoc-gen-fletcher=$<TARGET_FILE:eiva-fletcher-protoc::plugin>"
            "--fletcher_out=${GENERATED_DIR}"
            "-I" "${PROTO_DIR}"
            "${PROTO_DIR}/test.proto"
    DEPENDS "${PROTO_DIR}/test.proto" eiva-fletcher-protoc::plugin
    COMMENT "Generating test.fletcher.pb.h"
)

# Generate TypeScript schema from test.proto.
add_custom_command(
    OUTPUT "${GENERATED_DIR}/test.fletcher.ts"
    COMMAND "$<TARGET_FILE:protobuf::protoc>"
            "--plugin=protoc-gen-fletcher=$<TARGET_FILE:eiva-fletcher-protoc::plugin>"
            "--fletcher_out=${GENERATED_DIR}"
            "--fletcher_opt=ts"
            "-I" "${PROTO_DIR}"
            "${PROTO_DIR}/test.proto"
    DEPENDS "${PROTO_DIR}/test.proto" eiva-fletcher-protoc::plugin
    COMMENT "Generating test.fletcher.ts"
)

add_custom_target(generate_fletcher ALL
    DEPENDS
        "${GENERATED_DIR}/test.fletcher.pb.h"
        "${GENERATED_DIR}/test.fletcher.ts"
)

enable_testing()

add_test(
    NAME fletcher_protoc_generates_cpp
    COMMAND "${CMAKE_COMMAND}" -E cat "${GENERATED_DIR}/test.fletcher.pb.h"
)

add_test(
    NAME fletcher_protoc_generates_ts
    COMMAND "${CMAKE_COMMAND}" -E cat "${GENERATED_DIR}/test.fletcher.ts"
)
