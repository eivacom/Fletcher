# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
cmake_minimum_required(VERSION 3.15)
project(protoc-arrow-bridge-integration CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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

# Each .proto here is run through protoc-gen-fletcher to produce two headers:
#   <stem>.fletcher.pb.h        — edge-tier (proto + nanoarrow only)
#   <stem>.fletcher.arrow.pb.h  — server-tier (Arrow C++ ArrowRowView)
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

set(GENERATED_HEADERS)
foreach(_stem IN LISTS PROTO_STEMS)
    set(_src  "${PROTO_DIR}/${_stem}.proto")
    set(_hdr  "${GENERATED_DIR}/${_stem}.fletcher.pb.h")
    set(_vhdr "${GENERATED_DIR}/${_stem}.fletcher.arrow.pb.h")
    add_custom_command(
        OUTPUT  "${_hdr}" "${_vhdr}"
        COMMAND "$<TARGET_FILE:protobuf::protoc>"
                "--plugin=protoc-gen-fletcher=$<TARGET_FILE:fletcher-protoc::plugin>"
                "--fletcher_out=${GENERATED_DIR}"
                "-I" "${PROTO_DIR}"
                "-I" "${PROTOBUF_WKT_INCLUDE_DIR}"
                "${_src}"
        DEPENDS "${_src}" fletcher-protoc::plugin
                "${PROTO_DIR}/fletcher/options.proto"
        COMMENT "protoc-gen-fletcher: ${_stem}.proto"
        VERBATIM
    )
    list(APPEND GENERATED_HEADERS "${_hdr}" "${_vhdr}")
endforeach()

add_custom_target(generate_headers ALL DEPENDS ${GENERATED_HEADERS})

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
    tests/test_arrow_view.cpp)
add_dependencies(integration_tests generate_headers)

target_include_directories(integration_tests PRIVATE
    "${GENERATED_DIR}")

target_link_libraries(integration_tests PRIVATE
    GTest::gtest_main
    fletcher-arrow-bridge::fletcher-arrow-bridge
    fletcher-pubsub::fletcher-pubsub)

include(GoogleTest)
gtest_discover_tests(integration_tests DISCOVERY_MODE PRE_TEST)
