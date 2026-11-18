# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
# The cross-language lane of the C# gateway client (BIND-8d, D-BIND-116): builds the gateway exe
# (its Fast DDS provider is what the native peers meet it on), the C++ peer from the generated
# C++ pair, and the TypeScript peer's generated module. The C# test project and the C# native
# peer generate their own C# with protoc from MSBuild, as a consumer would.
cmake_minimum_required(VERSION 3.15)
project(gateway-dotnet-crosslang-integration CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(
    "${CMAKE_CURRENT_SOURCE_DIR}/../../gateway"
    "${CMAKE_CURRENT_BINARY_DIR}/gateway_build")

find_package(fletcher-protoc CONFIG REQUIRED)
find_package(fletcher-fastdds-pubsub-provider CONFIG REQUIRED)
find_package(fletcher-pubsub CONFIG REQUIRED)
find_package(fletcher-core   CONFIG REQUIRED)
find_package(protobuf        CONFIG REQUIRED)

# protoc's well-known types ship beside it, in ../include (the protos import empty.proto and,
# through coverage.proto, timestamp, duration and the wrappers).
foreach(_cfg RELEASE DEBUG MINSIZEREL RELWITHDEBINFO "")
    if(_cfg STREQUAL "")
        get_target_property(_protoc_loc protobuf::protoc IMPORTED_LOCATION)
    else()
        get_target_property(_protoc_loc protobuf::protoc IMPORTED_LOCATION_${_cfg})
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

# The ONE .proto (crosslang.proto) and the file it imports for its rows (coverage.proto).
set(PROTO_DIR    "${CMAKE_CURRENT_SOURCE_DIR}/proto")
set(COVERAGE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../protoc-coverage")
set(PROTOS "${PROTO_DIR}/crosslang.proto" "${COVERAGE_DIR}/proto/coverage.proto")
set(PROTO_INCLUDES
    "-I" "${PROTO_DIR}"
    "-I" "${COVERAGE_DIR}/proto"
    "-I" "${FLETCHER_PROTO_INCLUDE_DIR}"
    "-I" "${PROTOBUF_WKT_INCLUDE_DIR}")

set(GENERATED_DIR    "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(GENERATED_TS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ts/generated-ts")
file(MAKE_DIRECTORY "${GENERATED_DIR}" "${GENERATED_TS_DIR}")

set(CPP_HEADERS "${GENERATED_DIR}/crosslang.fletcher.pb.h" "${GENERATED_DIR}/coverage.fletcher.pb.h")
add_custom_command(
    OUTPUT  ${CPP_HEADERS}
    COMMAND "$<TARGET_FILE:protobuf::protoc>"
            "--plugin=protoc-gen-fletcher=$<TARGET_FILE:fletcher-protoc::plugin>"
            "--fletcher_out=${GENERATED_DIR}"
            ${PROTO_INCLUDES} ${PROTOS}
    DEPENDS ${PROTOS} fletcher-protoc::plugin
    COMMENT "protoc-gen-fletcher (C++): crosslang.proto, coverage.proto"
    VERBATIM)
add_custom_target(generate_cpp_fletcher DEPENDS ${CPP_HEADERS})

set(TS_MODULES "${GENERATED_TS_DIR}/crosslang.fletcher.ts" "${GENERATED_TS_DIR}/coverage.fletcher.ts")
add_custom_command(
    OUTPUT  ${TS_MODULES}
    COMMAND "$<TARGET_FILE:protobuf::protoc>"
            "--plugin=protoc-gen-fletcher=$<TARGET_FILE:fletcher-protoc::plugin>"
            "--fletcher_out=ts:${GENERATED_TS_DIR}"
            ${PROTO_INCLUDES} ${PROTOS}
    DEPENDS ${PROTOS} fletcher-protoc::plugin
    COMMENT "protoc-gen-fletcher (TS): crosslang.proto, coverage.proto"
    VERBATIM)
add_custom_target(generate_ts_fletcher ALL DEPENDS ${TS_MODULES})

add_executable(crosslang_cpp_peer cpp/cpp_peer.cpp)
add_dependencies(crosslang_cpp_peer generate_cpp_fletcher)
target_include_directories(crosslang_cpp_peer PRIVATE
    "${GENERATED_DIR}"
    "${COVERAGE_DIR}/tests"
    "${CMAKE_CURRENT_SOURCE_DIR}/cpp")
target_link_libraries(crosslang_cpp_peer PRIVATE
    fletcher-fastdds-pubsub-provider::fletcher-fastdds-pubsub-provider
    fletcher-pubsub::fletcher-pubsub
    fletcher-core::fletcher-core)
