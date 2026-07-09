# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
find_package(GTest REQUIRED)

add_executable(pubsub_arrow_tests
    test_pubsub_arrow.cpp)

target_link_libraries(pubsub_arrow_tests PRIVATE
    GTest::gtest_main
    fletcher-pubsub-arrow)

include(GoogleTest)
gtest_discover_tests(pubsub_arrow_tests DISCOVERY_MODE PRE_TEST)

# ---------------------------------------------------------------------------
# HARD-6 forcing test: compile-fails-on-discard of a [[nodiscard]] return.
#
# `discard_probe.cpp` intentionally discards every HARD-6 annotated public API
# return. The object library is EXCLUDE_FROM_ALL, so a normal build never
# compiles it; only the NodiscardTest CTest entry does, with C4834 promoted to
# an error (MSVC) / -Werror=unused-result (gcc/clang). CTest gates SOLELY on the
# nodiscard diagnostic appearing in the compiler output (no WILL_FAIL): with the
# annotations present the compile fails with the diagnostic and the regex
# matches -> PASS; with the annotations removed the TU compiles clean, no
# diagnostic appears, the regex does not match -> FAIL (red-first polarity).
#
# The TU file and target names deliberately avoid the substring "nodiscard":
# MSVC/cl echoes the source filename and MSBuild target name to the output that
# PASS_REGULAR_EXPRESSION scans, so a "nodiscard" in a name would self-match and
# mask the red state. Only a genuine diagnostic may supply the match keyword.
#
# The regex is deliberately DIAGNOSTIC-SPECIFIC so that ONLY a real compiler
# diagnostic can supply the match -- never a compile flag or filename echoed into
# the scanned output. In particular the bare flag text "unused-result" must NOT
# be a regex alternative: build tools (make/ninja) echo the full compile command
# -- including gcc/clang's `-Werror=unused-result` flag -- even on a CLEAN
# compile, so matching "unused-result" would false-PASS the red state on Linux.
# The chosen alternatives are safe:
#   * "C4834" appears only in the MSVC diagnostic `error C4834:`; the promoting
#     flag `/we4834` does NOT contain the substring "C4834".
#   * "ignoring return value" is the gcc/clang diagnostic phrase and never
#     appears in a flag or filename.
# ---------------------------------------------------------------------------
add_library(discard_probe_tu OBJECT EXCLUDE_FROM_ALL
    discard_probe.cpp)

target_link_libraries(discard_probe_tu PRIVATE
    fletcher-core::fletcher-core
    fletcher-pubsub::fletcher-pubsub
    fletcher-arrow-bridge::fletcher-arrow-bridge)

target_compile_features(discard_probe_tu PRIVATE cxx_std_20)

if(MSVC)
    target_compile_options(discard_probe_tu PRIVATE /we4834)
else()
    target_compile_options(discard_probe_tu PRIVATE -Werror=unused-result)
endif()

add_test(
    NAME NodiscardTest.CompileFailsOnDiscard
    COMMAND ${CMAKE_COMMAND}
            --build ${CMAKE_BINARY_DIR}
            --target discard_probe_tu
            --config $<CONFIG>)

set_tests_properties(NodiscardTest.CompileFailsOnDiscard PROPERTIES
    PASS_REGULAR_EXPRESSION "C4834|ignoring return value")
