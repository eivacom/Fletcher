# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
find_package(GTest REQUIRED)

add_executable(fastdds_pubsub_tests
    test_fast_dds_pubsub_provider.cpp)

# The OrderedDelivery white-box test includes the provider's internal header
# directly (it is header-only and std-only).
target_include_directories(fastdds_pubsub_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src)

target_link_libraries(fastdds_pubsub_tests PRIVATE
    GTest::gtest_main
    fletcher-fastdds-pubsub-provider
    fast-dds::fast-dds)

include(GoogleTest)
gtest_discover_tests(fastdds_pubsub_tests DISCOVERY_MODE PRE_TEST)
