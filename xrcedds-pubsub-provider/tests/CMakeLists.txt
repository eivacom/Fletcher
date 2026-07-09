# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
find_package(GTest REQUIRED)

add_executable(xrce_provider_tests
    test_xrce_provider.cpp)

# The re-entrant-Unsubscribe test (issue #62 residual, HARD-4) includes the
# non-installed internal test hook header directly, and must see the same
# FLETCHER_BUILD_TESTS guard the provider .cpp is built with so the hook
# declaration is visible and the #error guard never fires.
target_include_directories(xrce_provider_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_definitions(xrce_provider_tests PRIVATE FLETCHER_BUILD_TESTS)

target_link_libraries(xrce_provider_tests PRIVATE
    GTest::gtest_main
    fletcher-xrcedds-pubsub-provider
    microxrcedds_client)

include(GoogleTest)
gtest_discover_tests(xrce_provider_tests DISCOVERY_MODE PRE_TEST)
