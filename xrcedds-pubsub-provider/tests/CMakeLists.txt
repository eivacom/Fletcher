# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
find_package(GTest REQUIRED)

add_executable(xrce_provider_tests
    test_xrce_provider.cpp)

target_link_libraries(xrce_provider_tests PRIVATE
    GTest::gtest_main
    fletcher-xrcedds-pubsub-provider
    microxrcedds_client)

include(GoogleTest)
gtest_discover_tests(xrce_provider_tests DISCOVERY_MODE PRE_TEST)
