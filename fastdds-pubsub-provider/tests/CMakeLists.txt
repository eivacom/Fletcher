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
    GTest::gmock
    fletcher-fastdds-pubsub-provider
    fastdds)

include(GoogleTest)
gtest_discover_tests(fastdds_pubsub_tests DISCOVERY_MODE PRE_TEST)

# ---------------------------------------------------------------------------
# HARD-6 forcing test for the CONCRETE FastDDSPubSubProvider::Subscribe override.
#
# See discard_probe.cpp for why a base-virtual annotation is not enough:
# [[nodiscard]] is not inherited and the diagnostic keys off the static type at
# the call site. The regex is DIAGNOSTIC-SPECIFIC so only a real compiler
# diagnostic can supply the match -- never a flag or filename echoed into the
# scanned output ("C4834" appears only in `error C4834:`, not in `/we4834`;
# "ignoring return value" is the gcc/clang phrase and appears in no flag).
# ---------------------------------------------------------------------------
add_library(fastdds_discard_probe_tu OBJECT EXCLUDE_FROM_ALL
    discard_probe.cpp)

target_link_libraries(fastdds_discard_probe_tu PRIVATE
    fletcher-fastdds-pubsub-provider)

target_compile_features(fastdds_discard_probe_tu PRIVATE cxx_std_20)

if(MSVC)
    target_compile_options(fastdds_discard_probe_tu PRIVATE /we4834)
else()
    target_compile_options(fastdds_discard_probe_tu PRIVATE -Werror=unused-result)
endif()

add_test(
    NAME NodiscardTest.FastDDSConcreteSubscribeCompileFailsOnDiscard
    COMMAND ${CMAKE_COMMAND}
            --build ${CMAKE_BINARY_DIR}
            --target fastdds_discard_probe_tu
            --config $<CONFIG>)

set_tests_properties(NodiscardTest.FastDDSConcreteSubscribeCompileFailsOnDiscard PROPERTIES
    PASS_REGULAR_EXPRESSION "C4834|ignoring return value")
