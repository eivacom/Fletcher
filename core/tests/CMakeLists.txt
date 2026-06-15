# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
find_package(GTest REQUIRED)

add_executable(core_tests
    test_envelope.cpp
    test_positional_io.cpp)

target_link_libraries(core_tests PRIVATE
    GTest::gtest_main
    fletcher-core)

include(GoogleTest)
gtest_discover_tests(core_tests DISCOVERY_MODE PRE_TEST)