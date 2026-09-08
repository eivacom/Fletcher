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
# RBA-7 capstone (test_accessor_capstone.cpp) parses the shared committed
# expected/fixture JSON with nlohmann_json — a test-only dependency.
find_package(nlohmann_json CONFIG REQUIRED)

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
# #117 custom-option metadata passthrough. A separate generation command because
# this stem is the only one that needs mapping rules; the loop above stays on a
# bare `--fletcher_opt=ipc` so its outputs remain the untouched baseline.
#
# The rules themselves are declared in conanfile.py's generate() and arrive as
# FLETCHER_METADATA_RULES (a list, rule order preserved and significant).
# ----------------------------------------------------------------------------
if(NOT DEFINED FLETCHER_METADATA_RULES)
    message(FATAL_ERROR
        "FLETCHER_METADATA_RULES is not set. This harness is driven by "
        "`conan build .`, which supplies the metadata_from_option rules from "
        "conanfile.py. Configure through Conan, or pass "
        "-DFLETCHER_METADATA_RULES=\"rule1;rule2;...\" explicitly. Generating "
        "the fixture without rules would silently produce metadata-less "
        "schemas and fail the MetadataOptions tests in a confusing way.")
endif()

set(_META_RULES)
foreach(_rule IN LISTS FLETCHER_METADATA_RULES)
    list(APPEND _META_RULES "--fletcher_opt=${_rule}")
endforeach()

set(_meta_src  "${PROTO_DIR}/option_metadata.proto")
set(_meta_hdr  "${GENERATED_DIR}/option_metadata.fletcher.pb.h")
set(_meta_vhdr "${GENERATED_DIR}/option_metadata.fletcher.arrow.pb.h")
set(_meta_ipc)
foreach(_msg IN ITEMS Sample Pos Coord)
    list(APPEND _meta_ipc "${GENERATED_DIR}/option_metadata.${_msg}.ipc")
endforeach()
add_custom_command(
    OUTPUT  "${_meta_hdr}" "${_meta_vhdr}" ${_meta_ipc}
    COMMAND "$<TARGET_FILE:protobuf::protoc>"
            "--plugin=protoc-gen-fletcher=$<TARGET_FILE:fletcher-protoc::plugin>"
            "--fletcher_opt=ipc"
            ${_META_RULES}
            "--fletcher_out=${GENERATED_DIR}"
            "-I" "${PROTO_DIR}"
            "-I" "${FLETCHER_PROTO_INCLUDE_DIR}"
            "-I" "${PROTOBUF_WKT_INCLUDE_DIR}"
            "${_meta_src}"
    DEPENDS "${_meta_src}" fletcher-protoc::plugin
            "${FLETCHER_PROTO_INCLUDE_DIR}/fletcher/options.proto"
    COMMENT "protoc-gen-fletcher (metadata_from_option): option_metadata.proto"
    VERBATIM
)
add_custom_target(generate_metadata_headers ALL
    DEPENDS "${_meta_hdr}" "${_meta_vhdr}" ${_meta_ipc})

# ----------------------------------------------------------------------------
# RBA-2 accessor fixture: generate the real <Class>Accessor header for the
# flat scalar fixture (accessor_scalar.proto) so the forcing test can include
# and compile it. The accessor token is what triggers the .accessor.pb.h
# emission; the no-drift guard above proves it adds exactly that file.
# ----------------------------------------------------------------------------
# Each accessor fixture stem generates <stem>.fletcher.accessor.pb.h. The RBA-4a
# composite fixture (accessor_composite.proto) imports accessor_geo.proto, so its
# emitted accessor header #includes accessor_geo.fletcher.accessor.pb.h — both
# stems must be generated. The cross-file include is proven by the no-drift test
# above (existing outputs byte-identical) plus the composite forcing test, which
# only compiles if the cross-file accessor include is correct.
set(_ACCESSOR_STEMS
    accessor_scalar
    accessor_geo
    accessor_composite)

set(_ACCESSOR_HEADERS)
foreach(_acc_stem IN LISTS _ACCESSOR_STEMS)
    set(_acc_src "${PROTO_DIR}/${_acc_stem}.proto")
    set(_acc_hdr "${GENERATED_DIR}/${_acc_stem}.fletcher.accessor.pb.h")
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
        COMMENT "protoc-gen-fletcher (accessor): ${_acc_stem}.proto"
        VERBATIM
    )
    list(APPEND _ACCESSOR_HEADERS "${_acc_hdr}")
endforeach()
add_custom_target(generate_accessor_headers ALL DEPENDS ${_ACCESSOR_HEADERS})

# ----------------------------------------------------------------------------
# RBA-7 capstone: generate the C++ accessor header for the SHARED capstone proto
# that the Rust crate also consumes (integration-tests/accessor-capstone/). It is
# a NEW protoc input in a NEW directory, so it is outside every existing fixture's
# generation set and does not affect the RBA-1 no-drift guard. The proto imports
# fletcher/options.proto for the NESTED_LIST flatten wrapper.
# ----------------------------------------------------------------------------
set(CAPSTONE_DIR     "${CMAKE_CURRENT_SOURCE_DIR}/../accessor-capstone")
set(CAPSTONE_PROTO_DIR "${CAPSTONE_DIR}/proto")
set(_capstone_src "${CAPSTONE_PROTO_DIR}/accessor_capstone.proto")
set(_capstone_hdr "${GENERATED_DIR}/accessor_capstone.fletcher.accessor.pb.h")
add_custom_command(
    OUTPUT  "${_capstone_hdr}"
    COMMAND "$<TARGET_FILE:protobuf::protoc>"
            "--plugin=protoc-gen-fletcher=$<TARGET_FILE:fletcher-protoc::plugin>"
            "--fletcher_opt=accessor"
            "--fletcher_out=${GENERATED_DIR}"
            "-I" "${CAPSTONE_PROTO_DIR}"
            "-I" "${FLETCHER_PROTO_INCLUDE_DIR}"
            "-I" "${PROTOBUF_WKT_INCLUDE_DIR}"
            "${_capstone_src}"
    DEPENDS "${_capstone_src}" fletcher-protoc::plugin
            "${FLETCHER_PROTO_INCLUDE_DIR}/fletcher/options.proto"
    COMMENT "protoc-gen-fletcher (accessor): accessor_capstone.proto"
    VERBATIM
)
add_custom_target(generate_capstone_header ALL DEPENDS "${_capstone_hdr}")

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
    tests/test_ipc_parity.cpp
    tests/test_metadata_options.cpp)
add_dependencies(integration_tests generate_headers generate_metadata_headers)

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

# ---------------------------------------------------------------------------
# TIMEOUT, stated because this suite has no default one to fall back on.
#
# This directory calls `enable_testing()` rather than `include(CTest)`, so no
# `DartConfiguration.tcl` is generated and ctest's usual 1500 s per-test cap DOES NOT APPLY: an
# unbounded test here runs until the CI job itself is cancelled. The precedent and the reasoning
# in full are in `xrcedds-pubsub-provider/tests/CMakeLists.txt`, whose first Linux run spent
# 2 h 04 m inside one hung test before the job was cancelled with no result at all.
#
# 30 s for the `integration_tests` binary, chosen from its own measured wall clock: the slowest of
# its 76 rows is `NullabilityConsistencyTest.MetricsToArrowRowTypesMatchSchema` at 0.06 s, then
# `TimedEventTest.SchemaStructure` and `IpcParityTest.FlatMessageIpcFileMatchesRuntimeSchemaBytes`
# at 0.05 s, with the rest at 0.04 s (msvc-194 x86_64 Release). Every row is WORK, not a wait -
# encode/decode of pre-generated headers plus reading back the emitted .ipc files - and none of
# them spawns a process or sleeps. 30 s is ~500x the slowest row, so contention cannot false-red
# it, and it turns a future hang into a 30-second red instead of a cancelled job. `accessor_tests`
# below is a different cost class and gets its own, longer cap.
gtest_discover_tests(integration_tests DISCOVERY_MODE PRE_TEST
    PROPERTIES TIMEOUT 30)

# ----------------------------------------------------------------------------
# RBA-1 accessor no-drift test (AccessorTest.*). Scoped by `ctest -R 'Accessor'`.
#
# This test invokes the plugin itself at runtime into temporary directories, so
# it needs the protoc + plugin executables and all three -I proto search roots
# as compile definitions. It does NOT depend on the pre-generated headers above.
# ----------------------------------------------------------------------------
add_executable(accessor_tests
    tests/test_accessor.cpp
    tests/test_metadata_nodrift.cpp
    tests/test_accessor_scalar.cpp
    tests/test_accessor_composite.cpp
    tests/test_accessor_capstone.cpp
    tests/test_accessor_readme_example.cpp)

# The scalar accessor test #includes the generated accessor header and builds
# Arrow RecordBatch/StructArray fixtures at runtime. The capstone test
# additionally #includes the shared accessor_capstone accessor header and reads
# the shared committed fixture/expected JSON at runtime.
add_dependencies(accessor_tests generate_accessor_headers generate_capstone_header)

target_include_directories(accessor_tests PRIVATE "${GENERATED_DIR}")

target_link_libraries(accessor_tests PRIVATE
    GTest::gtest_main
    arrow::arrow
    fletcher-arrow-bridge::fletcher-arrow-bridge
    nlohmann_json::nlohmann_json)

target_compile_definitions(accessor_tests PRIVATE
    GENERATED_DIR_PATH="${GENERATED_DIR}"
    PROTOC_PATH="$<TARGET_FILE:protobuf::protoc>"
    PLUGIN_PATH="$<TARGET_FILE:fletcher-protoc::plugin>"
    PROTO_DIR="${PROTO_DIR}"
    FLETCHER_PROTO_INCLUDE_DIR="${FLETCHER_PROTO_INCLUDE_DIR}"
    PROTOBUF_WKT_INCLUDE_DIR="${PROTOBUF_WKT_INCLUDE_DIR}"
    # The capstone gtest reads the ONE committed shared fixture/expected JSON
    # from the source tree (NOT a copy), so it stays the single source of truth.
    CAPSTONE_FIXTURES_DIR="${CAPSTONE_DIR}/fixtures")

# 120 s rather than the 30 s the `integration_tests` rows get, for the same
# no-default-cap reason (see above) but a different cost class: the slow rows in this binary shell
# out to protoc + the fletcher plugin repeatedly and diff whole output trees byte for byte.
# Measured (msvc-194 x86_64 Release): `MetadataNoDriftTest.InertRulesLeaveEveryOutputByteIdentical`
# 3.97 s and `AccessorTest.OptGatedEmissionLeavesExistingOutputsByteIdentical` 3.29 s - two orders
# of magnitude above their siblings (the next is 0.42 s, and the accessor/capstone rows are
# 0.04-0.05 s). Those two are WORK, and subprocess work is exactly what a slower runner stretches,
# so this cap keeps a much smaller multiple than the pure-computation suites: 120 s is ~30x the
# 3.97 s row. `AccessorTest.GeneratedRustFileParsesWithRustc` is an unconditional GTEST_SKIP and so
# contributes no time on any platform.
gtest_discover_tests(accessor_tests DISCOVERY_MODE PRE_TEST
    PROPERTIES TIMEOUT 120)
