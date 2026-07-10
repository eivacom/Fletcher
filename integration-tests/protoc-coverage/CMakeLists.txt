# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
# GIR-1 generator compile-and-run coverage harness. One broad coverage.proto is
# run through protoc-gen-fletcher and every generated surface is compiled and
# executed as a refactor guard before the IR rewrite. Mirrors the CMake/Conan
# conventions of integration-tests/protoc-arrow-bridge.
cmake_minimum_required(VERSION 3.15)
project(protoc-coverage-integration CXX)

find_package(fletcher-protoc CONFIG REQUIRED)
find_package(fletcher-arrow-bridge CONFIG REQUIRED)
find_package(fletcher-pubsub CONFIG REQUIRED)
find_package(protobuf CONFIG REQUIRED)
find_package(Arrow CONFIG REQUIRED)
find_package(GTest REQUIRED)

# fletcher-protoc-target.cmake only WARNs (not FATALs) when the plugin
# executable cannot be located — fail loudly here so the build doesn't continue
# into add_custom_command and blow up with a confusing $<TARGET_FILE:...> error.
if(NOT TARGET fletcher-protoc::plugin)
    message(FATAL_ERROR
        "fletcher-protoc::plugin target was not created. The protoc plugin "
        "executable is required for code generation; check the fletcher-protoc "
        "package install for the binary.")
endif()

# Locate the protobuf well-known-types include dir (google/protobuf/*.proto) so
# messages importing Timestamp / Duration / Wrappers / Empty compile.
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

# Validate the resolved include dir before it is fed to protoc as an `-I`. A
# broken/empty symlink or a non-standard protobuf package layout would resolve
# to a path that does not exist (or exists but is missing the WKT protos),
# producing a valid CMake configure but a cryptic protoc "File not found" during
# generation. Fail loudly here instead. coverage.proto imports Timestamp,
# Duration, Wrappers and Empty, so require each of those protos to be present.
if(NOT IS_DIRECTORY "${PROTOBUF_WKT_INCLUDE_DIR}")
    message(FATAL_ERROR
        "Resolved Protobuf WKT include dir does not exist or is not a "
        "directory: '${PROTOBUF_WKT_INCLUDE_DIR}'. The protobuf package layout "
        "may differ from the expected <protoc>/../include, or a symlink is "
        "broken; cannot -I the well-known-types.")
endif()
foreach(_wkt timestamp duration wrappers empty)
    if(NOT EXISTS "${PROTOBUF_WKT_INCLUDE_DIR}/google/protobuf/${_wkt}.proto")
        message(FATAL_ERROR
            "Protobuf well-known-type proto missing under resolved include dir: "
            "'${PROTOBUF_WKT_INCLUDE_DIR}/google/protobuf/${_wkt}.proto'. The "
            "protobuf install looks broken or the include dir is an empty "
            "symlink; coverage.proto's WKT imports would not resolve.")
    endif()
endforeach()

set(PROTO_DIR     "${CMAKE_CURRENT_SOURCE_DIR}/proto")
set(GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${GENERATED_DIR}")

# ---------------------------------------------------------------------------
# Generation. A SINGLE protoc invocation with --fletcher_opt=ipc,accessor,ts,rust
# emits every artifact (verified for GIR-1): the three C++ headers, the TS and
# Rust files (incl. the shared __rba.fletcher.rs helper), and an .ipc schema
# stream per top-level message (bar the inlined flatten list-wrappers). Only
# coverage.proto is wired — it is named explicitly below, never globbed, so
# coverage_future.proto stays committed-but-unwired until GIR-10 (see
# proto/coverage_future.proto). The post-generation .ipc guard is the backstop:
# were a coverage_future message ever pulled in, its extra .ipc would fail the
# build loudly rather than silently expanding coverage.
# ---------------------------------------------------------------------------
set(_stem coverage)
set(_src  "${PROTO_DIR}/${_stem}.proto")

set(GENERATED_CPP_HEADERS
    "${GENERATED_DIR}/${_stem}.fletcher.pb.h"
    "${GENERATED_DIR}/${_stem}.fletcher.arrow.pb.h"
    "${GENERATED_DIR}/${_stem}.fletcher.accessor.pb.h")
set(GENERATED_TS
    "${GENERATED_DIR}/${_stem}.fletcher.ts")
set(GENERATED_RUST
    "${GENERATED_DIR}/${_stem}.fletcher.rs"
    "${GENERATED_DIR}/__rba.fletcher.rs")
# The complete set of top-level messages protoc-gen-fletcher emits an .ipc
# schema stream for. NOTE: this is NOT literally every message in coverage.proto
# — the flatten *list* wrappers (StructListWrapper, NestedStructListWrapper) are
# inlined and emit no standalone .ipc, whereas normal messages and message-level
# flatten structs (FlattenedPoint) do. This list is the single source of truth
# for "what generation must produce"; a post-generation guard
# (cmake/validate_generated_ipc.cmake, wired below) fails the build if protoc's
# emitted .ipc set drifts from it, so adding / removing / renaming a top-level
# message can't silently under- or over-generate the fixture (review finding 2).
set(_ipc_all_messages
    Branch
    CompositeCoverage
    FieldFlattenedPosition
    FlattenedPoint
    Leaf
    NestedEnums
    ScalarCoverage
    ServiceReply
    ServiceRequest)

# The runtime C++ test only opens these; per the design, only the .ipc files a
# test reads back are declared as custom-command outputs (dependency edges). The
# rest are still produced by the same protoc call and are covered by the guard.
set(_ipc_opened_messages
    ScalarCoverage
    CompositeCoverage
    ServiceRequest
    ServiceReply)

set(GENERATED_IPC "")
foreach(_m IN LISTS _ipc_opened_messages)
    list(APPEND GENERATED_IPC "${GENERATED_DIR}/${_stem}.${_m}.ipc")
endforeach()

# '|'-joined list of every .ipc basename the guard expects (semicolons can't
# survive as a single -D argument through add_custom_command).
set(_expected_ipc_names "")
foreach(_m IN LISTS _ipc_all_messages)
    list(APPEND _expected_ipc_names "${_stem}.${_m}.ipc")
endforeach()
string(REPLACE ";" "|" _expected_ipc_arg "${_expected_ipc_names}")

set(GENERATED_OUTPUTS
    ${GENERATED_CPP_HEADERS} ${GENERATED_TS} ${GENERATED_RUST} ${GENERATED_IPC})

add_custom_command(
    OUTPUT  ${GENERATED_OUTPUTS}
    COMMAND "$<TARGET_FILE:protobuf::protoc>"
            "--plugin=protoc-gen-fletcher=$<TARGET_FILE:fletcher-protoc::plugin>"
            "--fletcher_opt=ipc,accessor,ts,rust"
            "--fletcher_out=${GENERATED_DIR}"
            "-I" "${PROTO_DIR}"
            "-I" "${FLETCHER_PROTO_INCLUDE_DIR}"
            "-I" "${PROTOBUF_WKT_INCLUDE_DIR}"
            "${_src}"
    # Immediately after generation, assert the emitted .ipc set matches the
    # single-source list above (review finding 2). A non-zero exit fails the
    # custom command and therefore the build.
    COMMAND "${CMAKE_COMMAND}"
            "-DGENERATED_DIR=${GENERATED_DIR}"
            "-DEXPECTED_IPC_NAMES=${_expected_ipc_arg}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/validate_generated_ipc.cmake"
    DEPENDS "${_src}" fletcher-protoc::plugin
            "${FLETCHER_PROTO_INCLUDE_DIR}/fletcher/options.proto"
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/validate_generated_ipc.cmake"
    COMMENT "protoc-gen-fletcher (ipc,accessor,ts,rust): ${_stem}.proto"
    VERBATIM
)

add_custom_target(generate_coverage_outputs ALL DEPENDS ${GENERATED_OUTPUTS})

enable_testing()

# ---------------------------------------------------------------------------
# C++ harness: edge row + Arrow view / ToArrowRow + IPC schema. The service
# surface is compiled (pubsub linkage) but no transport is executed.
# ---------------------------------------------------------------------------
add_executable(coverage_harness_tests tests/test_coverage_harness.cpp)
add_dependencies(coverage_harness_tests generate_coverage_outputs)
target_include_directories(coverage_harness_tests PRIVATE "${GENERATED_DIR}")
target_compile_definitions(coverage_harness_tests PRIVATE
    GENERATED_DIR_PATH="${GENERATED_DIR}")
target_link_libraries(coverage_harness_tests PRIVATE
    GTest::gtest_main
    arrow::arrow
    fletcher-arrow-bridge::fletcher-arrow-bridge
    fletcher-pubsub::fletcher-pubsub)

# ---------------------------------------------------------------------------
# RBA C++ accessor: build an Arrow RecordBatch matching CompositeCoverage and
# read representative values through the generated accessor (read-only guard).
# ---------------------------------------------------------------------------
add_executable(coverage_accessor_tests tests/test_coverage_accessor.cpp)
add_dependencies(coverage_accessor_tests generate_coverage_outputs)
target_include_directories(coverage_accessor_tests PRIVATE "${GENERATED_DIR}")
# fletcher-pubsub is linked (beyond arrow + arrow-bridge) because this test
# builds its RecordBatch via the generated ToArrowRow, whose edge/arrow headers
# transitively include <fletcher/pubsub/owned_schema.hpp> (OwnedSchema).
target_link_libraries(coverage_accessor_tests PRIVATE
    GTest::gtest_main
    arrow::arrow
    fletcher-arrow-bridge::fletcher-arrow-bridge
    fletcher-pubsub::fletcher-pubsub)

include(GoogleTest)
gtest_discover_tests(coverage_harness_tests DISCOVERY_MODE PRE_TEST)
gtest_discover_tests(coverage_accessor_tests DISCOVERY_MODE PRE_TEST)

# ---------------------------------------------------------------------------
# Optional toolchain checks (graceful skip). tsc / cargo+rustc are found at
# configure time; the wrapper scripts print SKIP_MARKER and exit 0 when the
# tool is absent, and SKIP_REGULAR_EXPRESSION reports the test as Skipped
# (never Passed or Failed) irrespective of exit code.
# ---------------------------------------------------------------------------
find_program(TSC_EXECUTABLE tsc)
find_program(CARGO_EXECUTABLE cargo)
find_program(RUSTC_EXECUTABLE rustc)
message(STATUS "tsc:   ${TSC_EXECUTABLE}")
message(STATUS "cargo: ${CARGO_EXECUTABLE}")
message(STATUS "rustc: ${RUSTC_EXECUTABLE}")

add_test(NAME CoverageHarness.GeneratedTypescriptCompiles
    COMMAND "${CMAKE_COMMAND}"
        -DTSC_EXECUTABLE=${TSC_EXECUTABLE}
        -DGENERATED_DIR=${GENERATED_DIR}
        -DTS_DIR=${CMAKE_CURRENT_SOURCE_DIR}/ts
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/run_tsc_check.cmake)
set_tests_properties(CoverageHarness.GeneratedTypescriptCompiles
    PROPERTIES SKIP_REGULAR_EXPRESSION "SKIP_MARKER")

add_test(NAME CoverageHarness.GeneratedRustAccessorCompiles
    COMMAND "${CMAKE_COMMAND}"
        -DCARGO_EXECUTABLE=${CARGO_EXECUTABLE}
        -DRUSTC_EXECUTABLE=${RUSTC_EXECUTABLE}
        -DGENERATED_DIR=${GENERATED_DIR}
        -DRUST_DIR=${CMAKE_CURRENT_SOURCE_DIR}/rust-accessor
        -DCARGO_TARGET_DIR=${CMAKE_CURRENT_BINARY_DIR}/rust-target
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/run_cargo_check.cmake)
set_tests_properties(CoverageHarness.GeneratedRustAccessorCompiles
    PROPERTIES SKIP_REGULAR_EXPRESSION "SKIP_MARKER")

# The TS/Rust checks read the emitted generated files, so they must run after
# generation. They invoke external toolchains, not the C++ test binaries, so
# express the dependency as a fixture ordering via the generate target through
# the build (ALL target guarantees generation before ctest is invoked by
# `conan build .` / the presets flow).
