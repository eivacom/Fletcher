# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
cmake_minimum_required(VERSION 3.15)
project(protoc-arrow-bridge-integration CXX)

find_package(fletcher-protoc CONFIG REQUIRED)
find_package(fletcher-arrow-bridge CONFIG REQUIRED)
find_package(fletcher-pubsub CONFIG REQUIRED)
find_package(protobuf CONFIG REQUIRED)
find_package(Arrow CONFIG REQUIRED)
find_package(GTest REQUIRED)

# fletcher-protoc-target.cmake only WARNs (not FATALs) when the plugin
# executable cannot be located — find_package above will succeed regardless.
# Fail loudly here so the build doesn't continue into add_custom_command and
# blow up there with a confusing $<TARGET_FILE:...> error.
if(NOT TARGET fletcher-protoc::plugin)
    message(FATAL_ERROR
        "fletcher-protoc::plugin target was not created. The protoc "
        "plugin executable is required for code generation; check the "
        "fletcher-protoc package install for the binary.")
endif()

# Locate protobuf well-known-types include dir (google/protobuf/*.proto) so
# that messages importing Timestamp / Duration / Wrappers can be compiled.
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

set(PROTO_DIR     "${CMAKE_CURRENT_SOURCE_DIR}/proto")
set(GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${GENERATED_DIR}")

# Each .proto here is run through protoc-gen-fletcher to produce two headers
# plus one serialized Arrow IPC schema file per message:
#   <stem>.fletcher.pb.h        — edge-tier (proto + nanoarrow only)
#   <stem>.fletcher.arrow.pb.h  — server-tier (Arrow C++ ArrowRowView)
#   <stem>.<Message>.ipc        — schema-only Arrow IPC stream (--fletcher_opt=ipc)
set(PROTO_STEMS
    telemetry
    simple
    temporal
    nested
    collections
    maps
    complex
    pubsub
    evolution_v1
    evolution_v2
    flatten)

# The .ipc files test_ipc_parity.cpp reads back, declared as additional
# outputs of the generation command so it reruns when one is deleted
# independently of the headers. (Every message gets an .ipc file; only the
# ones a test consumes need declaring.)
set(_ipc_messages_pubsub  Telemetry)
set(_ipc_messages_nested  GeoPoint Address Location)

set(GENERATED_HEADERS)
foreach(_stem IN LISTS PROTO_STEMS)
    set(_src  "${PROTO_DIR}/${_stem}.proto")
    set(_hdr  "${GENERATED_DIR}/${_stem}.fletcher.pb.h")
    set(_vhdr "${GENERATED_DIR}/${_stem}.fletcher.arrow.pb.h")
    set(_ipc_files)
    if(DEFINED _ipc_messages_${_stem})
        foreach(_msg IN LISTS _ipc_messages_${_stem})
            list(APPEND _ipc_files "${GENERATED_DIR}/${_stem}.${_msg}.ipc")
        endforeach()
    endif()
    add_custom_command(
        OUTPUT  "${_hdr}" "${_vhdr}" ${_ipc_files}
        COMMAND "$<TARGET_FILE:protobuf::protoc>"
                "--plugin=protoc-gen-fletcher=$<TARGET_FILE:fletcher-protoc::plugin>"
                "--fletcher_opt=ipc"
                "--fletcher_out=${GENERATED_DIR}"
                "-I" "${PROTO_DIR}"
                "-I" "${FLETCHER_PROTO_INCLUDE_DIR}"
                "-I" "${PROTOBUF_WKT_INCLUDE_DIR}"
                "${_src}"
        DEPENDS "${_src}" fletcher-protoc::plugin
                "${FLETCHER_PROTO_INCLUDE_DIR}/fletcher/options.proto"
        COMMENT "protoc-gen-fletcher: ${_stem}.proto"
        VERBATIM
    )
    list(APPEND GENERATED_HEADERS "${_hdr}" "${_vhdr}" ${_ipc_files})
endforeach()

add_custom_target(generate_headers ALL DEPENDS ${GENERATED_HEADERS})

# ----------------------------------------------------------------------------
# RBA-2 accessor fixture: generate the real <Class>Accessor header for the
# flat scalar fixture (accessor_scalar.proto) so the forcing test can include
# and compile it. The accessor token is what triggers the .accessor.pb.h
# emission; the no-drift guard above proves it adds exactly that file.
# ----------------------------------------------------------------------------
set(_acc_src "${PROTO_DIR}/accessor_scalar.proto")
set(_acc_hdr "${GENERATED_DIR}/accessor_scalar.fletcher.accessor.pb.h")
add_custom_command(
    OUTPUT  "${_acc_hdr}"
    COMMAND "$<TARGET_FILE:protobuf::protoc>"
            "--plugin=protoc-gen-fletcher=$<TARGET_FILE:fletcher-protoc::plugin>"
            "--fletcher_opt=accessor"
            "--fletcher_out=${GENERATED_DIR}"
            "-I" "${PROTO_DIR}"
            "-I" "${FLETCHER_PROTO_INCLUDE_DIR}"
            "-I" "${PROTOBUF_WKT_INCLUDE_DIR}"
            "${_acc_src}"
    DEPENDS "${_acc_src}" fletcher-protoc::plugin
            "${FLETCHER_PROTO_INCLUDE_DIR}/fletcher/options.proto"
    COMMENT "protoc-gen-fletcher (accessor): accessor_scalar.proto"
    VERBATIM
)
add_custom_target(generate_accessor_headers ALL DEPENDS "${_acc_hdr}")

enable_testing()

add_executable(integration_tests
    tests/test_telemetry.cpp
    tests/test_simple.cpp
    tests/test_temporal.cpp
    tests/test_nested.cpp
    tests/test_collections.cpp
    tests/test_maps.cpp
    tests/test_complex.cpp
    tests/test_pubsub.cpp
    tests/test_schema_evolution.cpp
    tests/test_flatten.cpp
    tests/test_native_roundtrip.cpp
    tests/test_nullability_consistency.cpp
    tests/test_arrow_view.cpp
    tests/test_ipc_parity.cpp)
add_dependencies(integration_tests generate_headers)

target_include_directories(integration_tests PRIVATE
    "${GENERATED_DIR}")

# test_ipc_parity.cpp reads the emitted .ipc files back at runtime.
target_compile_definitions(integration_tests PRIVATE
    GENERATED_DIR_PATH="${GENERATED_DIR}")

target_link_libraries(integration_tests PRIVATE
    GTest::gtest_main
    fletcher-arrow-bridge::fletcher-arrow-bridge
    fletcher-pubsub::fletcher-pubsub)

include(GoogleTest)
gtest_discover_tests(integration_tests DISCOVERY_MODE PRE_TEST)

# ----------------------------------------------------------------------------
# RBA-1 accessor no-drift test (AccessorTest.*). Scoped by `ctest -R 'Accessor'`.
#
# This test invokes the plugin itself at runtime into temporary directories, so
# it needs the protoc + plugin executables and all three -I proto search roots
# as compile definitions. It does NOT depend on the pre-generated headers above.
# ----------------------------------------------------------------------------
add_executable(accessor_tests
    tests/test_accessor.cpp
    tests/test_accessor_scalar.cpp)

# The scalar accessor test #includes the generated accessor header and builds
# Arrow RecordBatch/StructArray fixtures at runtime.
add_dependencies(accessor_tests generate_accessor_headers)

target_include_directories(accessor_tests PRIVATE "${GENERATED_DIR}")

target_link_libraries(accessor_tests PRIVATE
    GTest::gtest_main
    arrow::arrow)

target_compile_definitions(accessor_tests PRIVATE
    GENERATED_DIR_PATH="${GENERATED_DIR}"
    PROTOC_PATH="$<TARGET_FILE:protobuf::protoc>"
    PLUGIN_PATH="$<TARGET_FILE:fletcher-protoc::plugin>"
    PROTO_DIR="${PROTO_DIR}"
    FLETCHER_PROTO_INCLUDE_DIR="${FLETCHER_PROTO_INCLUDE_DIR}"
    PROTOBUF_WKT_INCLUDE_DIR="${PROTOBUF_WKT_INCLUDE_DIR}")

gtest_discover_tests(accessor_tests DISCOVERY_MODE PRE_TEST)
