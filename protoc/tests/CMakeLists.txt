# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
find_package(GTest REQUIRED)

add_executable(fletcher_proto_plugin_tests
    test_type_mapper.cpp
    test_ir.cpp
    test_schema_builder.cpp
    test_schema_visitor.cpp
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
gtest_discover_tests(fletcher_proto_plugin_tests DISCOVERY_MODE PRE_TEST)

# GIR-8 (#53) full-surface guard: assert the IR emitters (protoc/src, excluding
# the read-only RBA emitter) contain zero `.ValueOrDie(`, so the "no unchecked
# ValueOrDie in generated IR code" invariant holds for EVERY downstream harness
# at the source — not just protoc-coverage's generated output (which is grepped
# separately by GenErrors.NoValueOrDieInIrGeneratedCode).
add_test(NAME GenErrors.NoValueOrDieInEmitterSources
    COMMAND "${CMAKE_COMMAND}"
        -DEMITTER_SRC_DIR=${CMAKE_CURRENT_SOURCE_DIR}/../src
        -P ${CMAKE_CURRENT_SOURCE_DIR}/check_no_value_or_die_in_emitters.cmake)
