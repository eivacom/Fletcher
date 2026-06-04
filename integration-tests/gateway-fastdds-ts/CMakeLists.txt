# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
cmake_minimum_required(VERSION 3.15)
project(gateway-fastdds-ts-integration CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Pull gateway's CMakeLists.txt into our build tree to get the `gateway`
# exe target (which now always links the FastDDS provider). Same pattern as
# gateway-end-to-end; gateway has no Conan recipe, so it is built from source
# and its deps must be on CMAKE_PREFIX_PATH (declared in conanfile.py).
add_subdirectory(
    "${CMAKE_CURRENT_SOURCE_DIR}/../../gateway"
    "${CMAKE_CURRENT_BINARY_DIR}/gateway_build")

find_package(fletcher-protoc CONFIG REQUIRED)
find_package(fletcher-fastdds-pubsub-provider CONFIG REQUIRED)
find_package(fletcher-pubsub CONFIG REQUIRED)
find_package(fletcher-core   CONFIG REQUIRED)
find_package(protobuf        CONFIG REQUIRED)

if(NOT TARGET fletcher-protoc::plugin)
    message(FATAL_ERROR
        "fletcher-protoc::plugin target was not created — the protoc plugin "
        "executable is required for code generation.")
endif()

# Locate protobuf well-known-types include dir so the proto can import
# google/protobuf/empty.proto (the service rpcs return Empty).
foreach(_cfg RELEASE DEBUG MINSIZEREL RELWITHDEBINFO)
    get_target_property(_protoc_loc protobuf::protoc IMPORTED_LOCATION_${_cfg})
    if(_protoc_loc AND NOT _protoc_loc MATCHES "NOTFOUND")
        get_filename_component(_protoc_dir  "${_protoc_loc}" DIRECTORY)
        get_filename_component(_protoc_root "${_protoc_dir}/.." ABSOLUTE)
        set(PROTOBUF_WKT_INCLUDE_DIR "${_protoc_root}/include")
        break()
    endif()
endforeach()
if(NOT PROTOBUF_WKT_INCLUDE_DIR)
    get_target_property(_protoc_loc protobuf::protoc IMPORTED_LOCATION)
    if(_protoc_loc AND NOT _protoc_loc MATCHES "NOTFOUND")
        get_filename_component(_protoc_dir  "${_protoc_loc}" DIRECTORY)
        get_filename_component(_protoc_root "${_protoc_dir}/.." ABSOLUTE)
        set(PROTOBUF_WKT_INCLUDE_DIR "${_protoc_root}/include")
    else()
        message(FATAL_ERROR "Cannot determine protobuf WKT include dir")
    endif()
endif()
message(STATUS "Protobuf WKT include dir: ${PROTOBUF_WKT_INCLUDE_DIR}")

set(PROTO        "${CMAKE_CURRENT_SOURCE_DIR}/proto/sensor_reading.proto")
set(GENERATED_DIR    "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(GENERATED_TS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/generated-ts")
file(MAKE_DIRECTORY "${GENERATED_DIR}")
file(MAKE_DIRECTORY "${GENERATED_TS_DIR}")

# ─────────────────────────────────────────────────────────────────────
# C++ generation: edge-tier nanoarrow header (consumed by fastdds_peer)
# plus the Arrow-view header (emitted by the plugin but not compiled here,
# so no Arrow C++ dependency is pulled in).
# ─────────────────────────────────────────────────────────────────────
set(CPP_HDR  "${GENERATED_DIR}/sensor_reading.fletcher.pb.h")
set(CPP_VHDR "${GENERATED_DIR}/sensor_reading.fletcher.arrow.pb.h")
add_custom_command(
    OUTPUT  "${CPP_HDR}" "${CPP_VHDR}"
    COMMAND "$<TARGET_FILE:protobuf::protoc>"
            "--plugin=protoc-gen-fletcher=$<TARGET_FILE:fletcher-protoc::plugin>"
            "--fletcher_out=${GENERATED_DIR}"
            "-I" "${CMAKE_CURRENT_SOURCE_DIR}/proto"
            "-I" "${PROTOBUF_WKT_INCLUDE_DIR}"
            "${PROTO}"
    DEPENDS "${PROTO}" fletcher-protoc::plugin
    COMMENT "protoc-gen-fletcher (C++): sensor_reading.proto"
    VERBATIM)
add_custom_target(generate_cpp_fletcher DEPENDS "${CPP_HDR}" "${CPP_VHDR}")

# ─────────────────────────────────────────────────────────────────────
# TypeScript generation: ISensorReading + SensorReading (TypedSchema) +
# the SensorFeed_*Topic constants used by the vitest suite.
# ─────────────────────────────────────────────────────────────────────
set(TS_OUT "${GENERATED_TS_DIR}/sensor_reading.fletcher.ts")
add_custom_command(
    OUTPUT  "${TS_OUT}"
    COMMAND "$<TARGET_FILE:protobuf::protoc>"
            "--plugin=protoc-gen-fletcher=$<TARGET_FILE:fletcher-protoc::plugin>"
            "--fletcher_out=ts:${GENERATED_TS_DIR}"
            "-I" "${CMAKE_CURRENT_SOURCE_DIR}/proto"
            "-I" "${PROTOBUF_WKT_INCLUDE_DIR}"
            "${PROTO}"
    DEPENDS "${PROTO}" fletcher-protoc::plugin
    COMMENT "protoc-gen-fletcher (TS): sensor_reading.proto"
    VERBATIM)
add_custom_target(generate_ts_fletcher ALL DEPENDS "${TS_OUT}")

# ─────────────────────────────────────────────────────────────────────
# fastdds_peer — C++ FastDDS application driven by the vitest suite.
# ─────────────────────────────────────────────────────────────────────
add_executable(fastdds_peer src/fastdds_peer.cpp)
add_dependencies(fastdds_peer generate_cpp_fletcher)
target_include_directories(fastdds_peer PRIVATE "${GENERATED_DIR}")
target_link_libraries(fastdds_peer PRIVATE
    fletcher-fastdds-pubsub-provider::fletcher-fastdds-pubsub-provider
    fletcher-pubsub::fletcher-pubsub
    fletcher-core::fletcher-core)
