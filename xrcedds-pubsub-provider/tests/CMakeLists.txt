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

# ---------------------------------------------------------------------------
# TIMEOUT, stated because this suite has no default one to fall back on.
#
# `xrcedds-pubsub-provider/CMakeLists.txt` calls `enable_testing()` rather than
# `include(CTest)`, so no `DartConfiguration.tcl` is generated and ctest's
# usual 1500 s per-test cap DOES NOT APPLY: an unbounded test here runs until
# the CI job itself is cancelled. It did, once - PR #126's first-ever Linux run
# of this suite spent 2 h 04 m inside `XrceConfig.DocumentConfiguresTransport`
# (a POSIX `close()` does not wake a blocked `accept()`; see the TcpListener
# comment in test_xrce_document.cpp) and was cancelled with no result at all.
#
# 60 s, chosen from this suite's own measured wall clock rather than a round
# number: the slowest legitimate case is `XrceConfig.DocumentConfiguresTransport`
# at 3.08 s, followed by `XrceConfig.AgentUnreachableIsATransportFailure` at
# 1.03 s and everything else at 0.03 s. Both slow rows are WAITS, not work -
# their cost is the XRCE session handshake spending its `connect_timeout_ms`
# budget (3 x 1000 ms and 1 x 1000 ms), which is wall clock and therefore does
# not stretch on a slower or more contended runner. 60 s is ~19x the slowest
# row, which is generous headroom for a suite whose real ceiling is fixed by a
# timer, and it turns a future hang into a one-minute red instead of a
# cancelled job. The matching per-test discovery cap is gtest_discover_tests'
# own 5 s default and is left alone.
gtest_discover_tests(xrce_provider_tests DISCOVERY_MODE PRE_TEST
    PROPERTIES TIMEOUT 60)

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

    # TIMEOUT is 300 s rather than the 60 s the gtest rows get: this entry shells
    # out to `cmake --build`, so its cost is a compiler invocation (1.92 s
    # measured, once the provider library is already built) and not a fixed
    # wait. Same reason as above - without it there is no cap at all.
    set_tests_properties(NodiscardTest.XrceConcreteSubscribeCompileFailsOnDiscard PROPERTIES
        PASS_REGULAR_EXPRESSION "C4834"
        TIMEOUT 300)
endif()
