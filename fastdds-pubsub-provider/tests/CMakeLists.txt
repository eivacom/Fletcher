# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
find_package(GTest REQUIRED)

add_executable(fastdds_pubsub_tests
    test_driver.cpp)

target_link_libraries(fastdds_pubsub_tests PRIVATE
    GTest::gtest_main
    fletcher-fastdds-pubsub-provider)

include(GoogleTest)
gtest_discover_tests(fastdds_pubsub_tests DISCOVERY_MODE PRE_TEST)
