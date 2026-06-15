# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
cmake_minimum_required(VERSION 3.15)
project(fletcher_protoc_test LANGUAGES CXX)

find_package(fletcher-protoc REQUIRED)
find_package(protobuf REQUIRED)

if(NOT TARGET fletcher-protoc::plugin)
    message(FATAL_ERROR "fletcher-protoc::plugin target not found")
endif()

set(PROTO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/proto")
set(GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${GENERATED_DIR}")

# Generate C++ header from test.proto.
add_custom_command(
    OUTPUT "${GENERATED_DIR}/test.fletcher.pb.h"
    COMMAND "$<TARGET_FILE:protobuf::protoc>"
            "--plugin=protoc-gen-fletcher=$<TARGET_FILE:fletcher-protoc::plugin>"
            "--fletcher_out=${GENERATED_DIR}"
            "-I" "${PROTO_DIR}"
            "${PROTO_DIR}/test.proto"
    DEPENDS "${PROTO_DIR}/test.proto" fletcher-protoc::plugin
    COMMENT "Generating test.fletcher.pb.h"
)

# Generate TypeScript schema from test.proto.
add_custom_command(
    OUTPUT "${GENERATED_DIR}/test.fletcher.ts"
    COMMAND "$<TARGET_FILE:protobuf::protoc>"
            "--plugin=protoc-gen-fletcher=$<TARGET_FILE:fletcher-protoc::plugin>"
            "--fletcher_out=${GENERATED_DIR}"
            "--fletcher_opt=ts"
            "-I" "${PROTO_DIR}"
            "${PROTO_DIR}/test.proto"
    DEPENDS "${PROTO_DIR}/test.proto" fletcher-protoc::plugin
    COMMENT "Generating test.fletcher.ts"
)

# ---- Synthetic multi-file corpus (proto/corpus/) -----------------------
# Exercises the plugin features test.proto does not: cross-package imports,
# (fletcher.flatten) / (fletcher.flatten_field), a third-party custom option,
# well-known types, maps, proto3 optional, and nested/repeated structs.
# Imports reach outside the corpus tree — fletcher/options.proto (from the
# package) and google/protobuf/*.proto (from the protobuf toolchain) — so two
# extra -I roots are needed beyond the corpus root.

# protobuf's bundled well-known-types .proto dir lives next to the protoc exe.
foreach(_cfg RELEASE DEBUG MINSIZEREL RELWITHDEBINFO "")
    if(_cfg)
        get_target_property(_loc protobuf::protoc IMPORTED_LOCATION_${_cfg})
    else()
        get_target_property(_loc protobuf::protoc IMPORTED_LOCATION)
    endif()
    if(_loc AND NOT _loc MATCHES "NOTFOUND")
        get_filename_component(_bindir "${_loc}" DIRECTORY)
        get_filename_component(PROTOBUF_WKT_INCLUDE_DIR "${_bindir}/../include" ABSOLUTE)
        break()
    endif()
endforeach()
if(NOT PROTOBUF_WKT_INCLUDE_DIR)
    message(FATAL_ERROR "Cannot determine protobuf WKT include dir")
endif()

set(CORPUS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/proto/corpus")
set(CORPUS_PROTOS common/types schemas/sample)

# Per-proto .ipc outputs (one file per non-flattened message; Length is a
# (fletcher.flatten) wrapper and gets none) and extra import dependencies.
# Declaring these makes generation rerun when an .ipc is deleted or an
# imported proto changes — keyed by the proto path with '/' → '_'.
set(_ipc_common_types  "${GENERATED_DIR}/common/types.GeoPoint.ipc")
set(_ipc_schemas_sample "${GENERATED_DIR}/schemas/sample.Sample.ipc")
set(_imports_schemas_sample "${CORPUS_DIR}/common/types.proto")

# ts,ipc covers all four output kinds in one invocation: the edge header is
# always emitted, the Arrow C++ view header unless schema_only, the .ts
# descriptor for ts, and the .ipc schema files for ipc.
set(CORPUS_OUTPUTS)
foreach(_rel IN LISTS CORPUS_PROTOS)
    string(REPLACE "/" "_" _key "${_rel}")
    set(_outs
        "${GENERATED_DIR}/${_rel}.fletcher.pb.h"
        "${GENERATED_DIR}/${_rel}.fletcher.arrow.pb.h"
        "${GENERATED_DIR}/${_rel}.fletcher.ts"
        ${_ipc_${_key}})
    add_custom_command(
        OUTPUT ${_outs}
        COMMAND "$<TARGET_FILE:protobuf::protoc>"
                "--plugin=protoc-gen-fletcher=$<TARGET_FILE:fletcher-protoc::plugin>"
                "--fletcher_opt=ts,ipc"
                "--fletcher_out=${GENERATED_DIR}"
                "-I" "${CORPUS_DIR}"
                "-I" "${FLETCHER_PROTO_INCLUDE_DIR}"
                "-I" "${PROTOBUF_WKT_INCLUDE_DIR}"
                "${CORPUS_DIR}/${_rel}.proto"
        DEPENDS "${CORPUS_DIR}/${_rel}.proto" fletcher-protoc::plugin
                ${_imports_${_key}}
        COMMENT "protoc-gen-fletcher (corpus): ${_rel}.proto"
        VERBATIM
    )
    list(APPEND CORPUS_OUTPUTS ${_outs})
endforeach()

add_custom_target(generate_fletcher ALL
    DEPENDS
        "${GENERATED_DIR}/test.fletcher.pb.h"
        "${GENERATED_DIR}/test.fletcher.ts"
        ${CORPUS_OUTPUTS}
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

# Corpus generation succeeded: every declared output — edge header, Arrow
# view header, TypeScript descriptor, and the spot-checked .ipc schema files
# — exists (md5sum exits non-zero on any missing file, and unlike cat does
# not dump binary content into the test log).
add_test(
    NAME fletcher_protoc_generates_corpus
    COMMAND "${CMAKE_COMMAND}" -E md5sum ${CORPUS_OUTPUTS}
)
