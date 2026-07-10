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
