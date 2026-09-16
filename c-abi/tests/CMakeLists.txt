# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
find_package(GTest REQUIRED)

# The ORACLE's dependencies, and test-only on purpose.
#
# `fletcher-c-abi` must never link `arrow-bridge`: that would pull Arrow C++ into
# the per-RID native asset the NuGet package ships, which is most of the reason
# the codec was written on nanoarrow at all. The byte-identity test links it
# because the oracle has to be the real encoder rather than a transcription of
# it. CI's packed-size check measures the packaged shim, not this binary, so the
# two facts do not fight.
find_package(fletcher-arrow-bridge CONFIG REQUIRED)
find_package(Arrow CONFIG REQUIRED)

# The codec itself is reached through `fletcher-c-abi-impl`, the object library
# the shim is built from. The alternative - linking the shim and hoping its
# symbols are reachable - cannot work: everything inside it is hidden by design,
# and the export table is exactly what binding.h declares. Testing behind a
# boundary means linking behind it.

add_executable(c_abi_tests
    test_binding_abi.cpp
    test_nanoarrow_codec.cpp)

target_link_libraries(c_abi_tests PRIVATE
    GTest::gtest_main
    fletcher-c-abi
    fletcher-c-abi-impl
    fletcher-arrow-bridge::fletcher-arrow-bridge
    arrow::arrow)

# ---------------------------------------------------------------------------
# The shim is a SHARED library — the only one in the tree — so the loader has to
# find it before a single test runs. CMake puts the DLL in the top-level build
# directory and test executables in build/tests/<config>, and
# gtest_discover_tests launches the executable itself, with no environment of
# ours: on Windows that is exit code 0xC0000135 (DLL not found) during
# DISCOVERY, which reads as "error running test executable" and not as a missing
# library. Building the test executables straight into the shim's own directory
# is the fix that needs no PATH, no VirtualRunEnv and no copy step.
#
# Linux does not need it (CMake gives the executable a build-tree RPATH pointing
# at the shim), but the same directory is harmless there and keeps the two
# platforms symmetric — which is the point, since this suite exists to make the
# Linux and Windows signals identical from day one.
set_target_properties(c_abi_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:fletcher-c-abi>)

include(GoogleTest)

# ---------------------------------------------------------------------------
# TIMEOUT, stated because this suite has no default one to fall back on:
# `c-abi/CMakeLists.txt` calls `enable_testing()` rather than `include(CTest)`,
# so no DartConfiguration.tcl is generated and ctest's usual 1500 s per-test cap
# DOES NOT APPLY. The precedent and the full reasoning are in
# `xrcedds-pubsub-provider/tests/CMakeLists.txt`, whose first Linux run spent
# 2 h 04 m inside one hung test before the job was cancelled with no result.
#
# 30 s. Every row here is arithmetic on a constant — no wait, no I/O, no
# transport — so the measured cost is the process start. The cap exists to turn
# a future hang (the first row that touches a provider will be in this suite)
# into a 30-second red instead of a cancelled job.
gtest_discover_tests(c_abi_tests DISCOVERY_MODE PRE_TEST
    PROPERTIES TIMEOUT 30)

# ---------------------------------------------------------------------------
# The header is C, and stays C: `c99_probe.c` includes binding.h first and
# nothing else, and is compiled as strict C with warnings promoted to errors.
#
# Polarity, unlike the [[nodiscard]] probes elsewhere in the tree, is
# BUILD-SUCCEEDS: the probe is EXPECTED to compile, so the CTest entry simply
# builds it and gates on the exit code. No PASS_REGULAR_EXPRESSION is involved
# and there is nothing for a compile flag echoed into the log to self-match.
#
# The target is part of ALL as well, so a header that stops being C breaks the
# component build immediately and does not wait for someone to run ctest.
#
# MSVC has no /std:c99 — C11 is the oldest C mode it offers — so the probe is
# held to C11 there and to C99 everywhere else. That is the stricter reading of
# "pure C99" on the compiler that cannot check it exactly: anything C99 rejects
# and C11 accepts (mainly _Generic, _Static_assert, anonymous structs) is caught
# by the gcc leg of the same CI run.
add_library(c99_probe OBJECT
    c99_probe.c)

target_link_libraries(c99_probe PRIVATE fletcher-c-abi)

if(MSVC)
    target_compile_options(c99_probe PRIVATE /std:c11 /W4 /WX)
else()
    target_compile_options(c99_probe PRIVATE -std=c99 -Wall -Wextra -Werror -pedantic-errors)
endif()

add_test(
    NAME BindingAbi.CompilesAsC99AndIsSelfContained
    COMMAND ${CMAKE_COMMAND}
            --build ${CMAKE_BINARY_DIR}
            --target c99_probe
            --config $<CONFIG>)

# 300 s, and stated for the same reason the gtest rows state theirs. This entry
# shells out to `cmake --build`, so its cost is a compiler invocation — the kind
# of cost a cold MSBuild/ninja start on a contended runner genuinely stretches,
# unlike the fixed arithmetic of the rows above. Matches the equivalent probe in
# `pubsub-arrow/tests/CMakeLists.txt`.
set_tests_properties(BindingAbi.CompilesAsC99AndIsSelfContained PROPERTIES
    TIMEOUT 300)
