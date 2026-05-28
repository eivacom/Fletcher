# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
find_package(GTest REQUIRED)

add_executable(arrow_bridge_tests
    test_codec.cpp)

target_link_libraries(arrow_bridge_tests PRIVATE
    GTest::gtest_main
    fletcher-arrow-bridge)

include(GoogleTest)
gtest_discover_tests(arrow_bridge_tests DISCOVERY_MODE PRE_TEST)
