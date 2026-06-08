# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
cmake_minimum_required(VERSION 3.15)
project(gateway-end-to-end-integration CXX)

# Pull gateway's CMakeLists.txt into our build tree. That gives us
# the `gateway` exe target without duplicating any gateway build
# configuration here. gateway/ has no Conan recipe, so we build it
# from source; the deps it needs (pubsub, core, boost, nlohmann_json,
# yaml-cpp) are declared in conanfile.py and are already on
# CMAKE_PREFIX_PATH by the time we get here.
add_subdirectory(
    "${CMAKE_CURRENT_SOURCE_DIR}/../../gateway"
    "${CMAKE_CURRENT_BINARY_DIR}/gateway_build")

# ─────────────────────────────────────────────────────────────────────
# Generate the TypeScript proto-class used by the proto-gen test case.
# Only the TS output is needed — the gateway exe takes its schema from
# test-config.yml, not from the proto. The .proto and the YAML must
# describe the same row layout (kept in sync manually for now).
# ─────────────────────────────────────────────────────────────────────
find_package(fletcher-protoc CONFIG REQUIRED)
find_package(protobuf             CONFIG REQUIRED)

if(NOT TARGET fletcher-protoc::plugin)
    message(FATAL_ERROR
        "fletcher-protoc::plugin target was not created — the protoc "
        "plugin executable is required for code generation.")
endif()

set(PROTO_DIR        "${CMAKE_CURRENT_SOURCE_DIR}/proto")
set(GENERATED_TS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/generated-ts")
file(MAKE_DIRECTORY "${GENERATED_TS_DIR}")

set(PROTO_STEMS telemetry)
set(GENERATED_TS_FILES)
foreach(_stem IN LISTS PROTO_STEMS)
    set(_src    "${PROTO_DIR}/${_stem}.proto")
    set(_ts_out "${GENERATED_TS_DIR}/${_stem}.fletcher.ts")

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
    list(APPEND GENERATED_TS_FILES "${_ts_out}")
endforeach()

add_custom_target(generate_ts_fletcher ALL DEPENDS ${GENERATED_TS_FILES})

# Make sure protoc generation happens before vitest is asked to import
# the generated module. The integration test invokes `npm test` after
# `cmake --build`, but having the dependency edge here keeps the build
# graph honest.
add_dependencies(gateway generate_ts_fletcher)
