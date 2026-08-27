# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
find_package(GTest REQUIRED)

add_executable(arrow_bridge_tests
    test_codec.cpp
    test_codec_edge.cpp
    test_codec_property_fuzz.cpp)

target_link_libraries(arrow_bridge_tests PRIVATE
    GTest::gtest_main
    fletcher-arrow-bridge)

# The HARD-1 scalar-level forcing tests call the internal detail:: API
# (Reader, DecodeScalarFromReader) declared in arrow-bridge/src, which is a
# PRIVATE include on the library target and thus not otherwise visible here.
target_include_directories(arrow_bridge_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src)

include(GoogleTest)
gtest_discover_tests(arrow_bridge_tests DISCOVERY_MODE PRE_TEST)

# ---------------------------------------------------------------------------
# #53 invariant guard for hand-written runtime code. The emitter-side guard
# (protoc/tests/check_no_value_or_die_in_emitters.cmake) covers generated code
# only; this covers the accessors in this component. See the .cmake header for
# why it exists — GIR-10 reintroduced two `.ValueOrDie()` sites here that the
# emitter guard could not see.
# ---------------------------------------------------------------------------
add_test(
    NAME ArrowBridgeInvariants.NoValueOrDieInRuntimeSources
    COMMAND "${CMAKE_COMMAND}"
        -DRUNTIME_INCLUDE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/../include
        -DRUNTIME_SRC_DIR=${CMAKE_CURRENT_SOURCE_DIR}/../src
        -P ${CMAKE_CURRENT_SOURCE_DIR}/check_no_value_or_die_in_runtime.cmake)
