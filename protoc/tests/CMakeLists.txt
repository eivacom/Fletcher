# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
find_package(GTest REQUIRED)

add_executable(fletcher_proto_plugin_tests
    test_type_mapper.cpp
    test_ir.cpp
    test_schema_builder.cpp
    test_schema_visitor.cpp
    test_schema_codec_lockstep.cpp
    test_option_metadata.cpp
    test_ts_visitor.cpp)

target_link_libraries(fletcher_proto_plugin_tests PRIVATE
    GTest::gtest_main
    fletcher_plugin_core)

# GIR-5: committed per-node-kind .ipc goldens (captured from today's emitter)
# live in tests/golden. The forcing test reads them from the source tree so they
# are reviewed as source, never regenerated silently from the build tree.
target_compile_definitions(fletcher_proto_plugin_tests PRIVATE
    SCHEMA_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden")

include(GoogleTest)
# TIMEOUT: every test in this suite runs in well under a second. The cap exists
# so a re-regression of the flatten-cycle guard (which manifests as a hang or a
# stack overflow inside BuildFieldIr, not an assertion failure) fails the job in
# two minutes instead of stalling it for ctest's 25-minute default.
gtest_discover_tests(fletcher_proto_plugin_tests DISCOVERY_MODE PRE_TEST
    PROPERTIES TIMEOUT 120)

# GIR-8 (#53) full-surface guard: assert the IR emitters (protoc/src, excluding
# the read-only RBA emitter) contain zero `.ValueOrDie(`, so the "no unchecked
# ValueOrDie in generated IR code" invariant holds for EVERY downstream harness
# at the source — not just protoc-coverage's generated output (which is grepped
# separately by GenErrors.NoValueOrDieInIrGeneratedCode).
add_test(NAME GenErrors.NoValueOrDieInEmitterSources
    COMMAND "${CMAKE_COMMAND}"
        -DEMITTER_SRC_DIR=${CMAKE_CURRENT_SOURCE_DIR}/../src
        -P ${CMAKE_CURRENT_SOURCE_DIR}/check_no_value_or_die_in_emitters.cmake)

# TIMEOUT, stated for the same reason the gtest rows above state theirs, and easy to miss for one
# reason worth recording: a `gtest_discover_tests(PROPERTIES ...)` argument does NOT reach an
# `add_test` entry, so the 120 s above never applied here. `protoc/CMakeLists.txt` calls
# `enable_testing()` rather than `include(CTest)`, so no `DartConfiguration.tcl` is generated and
# ctest's usual 1500 s per-test cap DOES NOT APPLY either - this entry had no cap of any kind. It
# was the last such entry in the tree after the nine suites capped alongside it; the full
# reasoning, and the 2 h 04 m cancelled job that prompted it, are in
# `xrcedds-pubsub-provider/tests/CMakeLists.txt`.
#
# 60 s, matching the sibling `cmake -P` source scans in
# `integration-tests/protoc-coverage/CMakeLists.txt` and `arrow-bridge/tests/CMakeLists.txt`, and
# chosen the same way: this entry is a script interpreter walking `protoc/src` with no subprocess
# and no wait, measured at 0.15 s, so 60 s is ~400x it. Work rather than a fixed wait, but a file
# scan is not the kind of work a slower runner stretches by 400.
set_tests_properties(GenErrors.NoValueOrDieInEmitterSources PROPERTIES
    TIMEOUT 60)
