# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
find_package(GTest REQUIRED)

add_executable(xrce_provider_tests
    test_xrce_provider.cpp
    test_xrce_document.cpp)

# The re-entrant-Unsubscribe test (issue #62 residual, HARD-4) includes the
# non-installed internal test hook header directly, and must see the same
# FLETCHER_BUILD_TESTS guard the provider .cpp is built with so the hook
# declaration is visible and the #error guard never fires.
target_include_directories(xrce_provider_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_definitions(xrce_provider_tests PRIVATE FLETCHER_BUILD_TESTS)

# PDA-DEC-7: XrceConfig.PublishedDefaultsAreExact reads the published default document out of
# README.md itself rather than holding a copy, so editing either the README block or a default in
# the code alone turns the test red. The README is exported by conanfile.py so the cache build
# reads the same file the repository holds. It is read at RUN time - a file(READ)/configure_file
# bake-in here would re-create the held-copy defect the disk read exists to kill.
target_compile_definitions(xrce_provider_tests PRIVATE
    FLETCHER_XRCE_README_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../README.md")

target_link_libraries(xrce_provider_tests PRIVATE
    GTest::gtest_main
    fletcher-xrcedds-pubsub-provider
    microxrcedds_client)

include(GoogleTest)
gtest_discover_tests(xrce_provider_tests DISCOVERY_MODE PRE_TEST)

# ---------------------------------------------------------------------------
# HARD-6 forcing test for the CONCRETE XrceDDSPubSubProvider::Subscribe override.
#
# See discard_probe.cpp for why a base-virtual annotation is not enough:
# [[nodiscard]] is not inherited and the diagnostic keys off the static type at
# the call site. The regex is DIAGNOSTIC-SPECIFIC so only a real compiler
# diagnostic can supply the match -- never a flag or filename echoed into the
# scanned output ("C4834" appears only in `error C4834:`, not in `/we4834`;
# "ignoring return value" is the gcc/clang phrase and appears in no flag).
# ---------------------------------------------------------------------------
# GCC LIMITATION (measured with g++ 13.3.0, the version the Linux CI profile
# pins): gcc emits NO -Wunused-result diagnostic for a discarded [[nodiscard]]
# return of a VIRTUAL function -- not through the concrete type, and not through
# the base reference either. Only non-virtual declarations are diagnosed. MSVC
# (C4834) diagnoses both. `XrceDDSPubSubProvider::Subscribe` is a virtual
# override, so this probe can only be a gate on MSVC; registering it on gcc
# would fail for a reason that has nothing to do with the annotation being
# present. The annotation is kept because it IS effective for MSVC consumers.
#
# Consequence to know: for the virtual provider Subscribe methods, #56's
# compile-time guarantee holds on MSVC only. The non-virtual surfaces
# (SubscriberArrow::Subscribe, PublisherArrow::ListTopics, the core/codec
# helpers) are diagnosed on both compilers.
if(MSVC)
    add_library(xrce_discard_probe_tu OBJECT EXCLUDE_FROM_ALL
        discard_probe.cpp)

    target_link_libraries(xrce_discard_probe_tu PRIVATE
        fletcher-xrcedds-pubsub-provider)

    target_compile_features(xrce_discard_probe_tu PRIVATE cxx_std_20)

    target_compile_options(xrce_discard_probe_tu PRIVATE /we4834)

    add_test(
        NAME NodiscardTest.XrceConcreteSubscribeCompileFailsOnDiscard
        COMMAND ${CMAKE_COMMAND}
                --build ${CMAKE_BINARY_DIR}
                --target xrce_discard_probe_tu
                --config $<CONFIG>)

    set_tests_properties(NodiscardTest.XrceConcreteSubscribeCompileFailsOnDiscard PROPERTIES
        PASS_REGULAR_EXPRESSION "C4834")
endif()
