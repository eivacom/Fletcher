# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
find_package(GTest REQUIRED)

add_executable(pubsub_tests
    test_driver.cpp)

target_link_libraries(pubsub_tests PRIVATE
    GTest::gtest_main
    fletcher-pubsub)

include(GoogleTest)
gtest_discover_tests(pubsub_tests)
