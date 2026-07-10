# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
find_package(GTest REQUIRED)

add_executable(arrow_bridge_tests
    test_codec.cpp)

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
