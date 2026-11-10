# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
cmake_minimum_required(VERSION 3.15)
project(protoc-gateway-client-ts-integration CXX)

find_package(fletcher-protoc CONFIG REQUIRED)
find_package(fletcher-pubsub CONFIG REQUIRED)
find_package(fletcher-core CONFIG REQUIRED)
find_package(protobuf CONFIG REQUIRED)

if(NOT TARGET fletcher-protoc::plugin)
    message(FATAL_ERROR
        "fletcher-protoc::plugin target was not created — the protoc "
        "plugin executable is required for code generation.")
endif()

set(PROTO_DIR     "${CMAKE_CURRENT_SOURCE_DIR}/proto")
set(GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(GENERATED_TS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/generated-ts")
file(MAKE_DIRECTORY "${GENERATED_DIR}")
file(MAKE_DIRECTORY "${GENERATED_TS_DIR}")

# Run protoc-gen-fletcher twice per proto: once for the C++ row class
# (.fletcher.pb.h, used by the vector emitter), once for the TypeScript
# bindings (.fletcher.ts, used by the vitest test). The TS output goes
# next to the test so vitest can import it without a build artifact path.
set(PROTO_STEMS telemetry)

set(GENERATED_HEADERS)
foreach(_stem IN LISTS PROTO_STEMS)
    set(_src     "${PROTO_DIR}/${_stem}.proto")
    set(_cpp_hdr "${GENERATED_DIR}/${_stem}.fletcher.pb.h")
    set(_ts_out  "${GENERATED_TS_DIR}/${_stem}.fletcher.ts")

    add_custom_command(
        OUTPUT  "${_cpp_hdr}"
        COMMAND "$<TARGET_FILE:protobuf::protoc>"
                "--plugin=protoc-gen-fletcher=$<TARGET_FILE:fletcher-protoc::plugin>"
                "--fletcher_out=${GENERATED_DIR}"
                "-I" "${PROTO_DIR}"
                "${_src}"
        DEPENDS "${_src}" fletcher-protoc::plugin
        COMMENT "protoc-gen-fletcher (C++): ${_stem}.proto"
        VERBATIM
    )

    add_custom_command(
        OUTPUT  "${_ts_out}"
        COMMAND "$<TARGET_FILE:protobuf::protoc>"
                "--plugin=protoc-gen-fletcher=$<TARGET_FILE:fletcher-protoc::plugin>"
                "--fletcher_out=ts:${GENERATED_TS_DIR}"
                "-I" "${PROTO_DIR}"
                "${_src}"
        DEPENDS "${_src}" fletcher-protoc::plugin
        COMMENT "protoc-gen-fletcher (TS): ${_stem}.proto"
        VERBATIM
    )

    list(APPEND GENERATED_HEADERS "${_cpp_hdr}" "${_ts_out}")
endforeach()

# BIND-T-3 (D-BIND-184): the generated pub/sub pair, TypeScript only. pair.proto's method
# returns google.protobuf.Empty, so protoc needs the well-known types' include directory, found
# beside the protoc executable as gateway-fastdds-ts finds it.
foreach(_cfg RELEASE DEBUG MINSIZEREL RELWITHDEBINFO)
    get_target_property(_protoc_loc protobuf::protoc IMPORTED_LOCATION_${_cfg})
    if(_protoc_loc AND NOT _protoc_loc MATCHES "NOTFOUND")
        get_filename_component(_protoc_dir "${_protoc_loc}" DIRECTORY)
        get_filename_component(PROTOBUF_WKT_INCLUDE_DIR "${_protoc_dir}/../include" ABSOLUTE)
        break()
    endif()
endforeach()
if(NOT PROTOBUF_WKT_INCLUDE_DIR)
    get_target_property(_protoc_loc protobuf::protoc IMPORTED_LOCATION)
    if(_protoc_loc AND NOT _protoc_loc MATCHES "NOTFOUND")
        get_filename_component(_protoc_dir "${_protoc_loc}" DIRECTORY)
        get_filename_component(PROTOBUF_WKT_INCLUDE_DIR "${_protoc_dir}/../include" ABSOLUTE)
    else()
        message(FATAL_ERROR "Cannot determine protobuf WKT include dir")
    endif()
endif()

set(_pair_ts "${GENERATED_TS_DIR}/pair.fletcher.ts")
add_custom_command(
    OUTPUT  "${_pair_ts}"
    COMMAND "$<TARGET_FILE:protobuf::protoc>"
            "--plugin=protoc-gen-fletcher=$<TARGET_FILE:fletcher-protoc::plugin>"
            "--fletcher_out=ts:${GENERATED_TS_DIR}"
            "-I" "${PROTO_DIR}"
            "-I" "${PROTOBUF_WKT_INCLUDE_DIR}"
            "${PROTO_DIR}/pair.proto"
    DEPENDS "${PROTO_DIR}/pair.proto" fletcher-protoc::plugin
    COMMENT "protoc-gen-fletcher (TS): pair.proto"
    VERBATIM
)
list(APPEND GENERATED_HEADERS "${_pair_ts}")

add_custom_target(generate_headers ALL DEPENDS ${GENERATED_HEADERS})

add_executable(emit_vectors src/emit_vectors.cpp)
add_dependencies(emit_vectors generate_headers)

target_include_directories(emit_vectors PRIVATE "${GENERATED_DIR}")
target_link_libraries(emit_vectors PRIVATE
    fletcher-pubsub::fletcher-pubsub
    fletcher-core::fletcher-core)
