# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
find_package(GTest REQUIRED)

add_executable(arrow_bridge_tests
    test_codec.cpp
    test_codec_edge.cpp
    test_codec_property_fuzz.cpp)

target_link_libraries(arrow_bridge_tests PRIVATE
    GTest::gtest_main
    fletcher-arrow-bridge)

# The HARD-1 scalar-level forcing tests call the internal detail:: API
# (Reader, DecodeScalarFromReader) declared in arrow-bridge/src, which is a
# PRIVATE include on the library target and thus not otherwise visible here.
target_include_directories(arrow_bridge_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src)

include(GoogleTest)

# ---------------------------------------------------------------------------
# TIMEOUT, stated because this suite has no default one to fall back on.
#
# `arrow-bridge/CMakeLists.txt` calls `enable_testing()` rather than `include(CTest)`, so no
# `DartConfiguration.tcl` is generated and ctest's usual 1500 s per-test cap DOES NOT APPLY: an
# unbounded test here runs until the CI job itself is cancelled. The precedent and the reasoning
# in full are in `xrcedds-pubsub-provider/tests/CMakeLists.txt`, whose first Linux run spent
# 2 h 04 m inside one hung test before the job was cancelled with no result at all.
#
# 30 s, chosen from this suite's own measured wall clock: the slowest row is
# `Fuzz.DecodeRowSurvivesRandomTruncatedBuffers` at 0.10 s, then
# `CodecEdge.LargeStringAndLargeBinaryRoundtrip` at 0.08 s, with the remaining 58 rows at
# 0.04-0.05 s and the whole suite at 2.97 s (msvc-194 x86_64 Release). Every row is WORK, not a
# wait - Arrow encode/decode plus the fuzz row's random truncation loop - so a slower runner
# scales it, but 30 s is ~300x the slowest row and no amount of contention scales in-process
# arithmetic by 300. It turns a future hang into a 30-second red instead of a cancelled job.
gtest_discover_tests(arrow_bridge_tests DISCOVERY_MODE PRE_TEST
    PROPERTIES TIMEOUT 30)

# ---------------------------------------------------------------------------
# #53 invariant guard for hand-written runtime code. The emitter-side guard
# (protoc/tests/check_no_value_or_die_in_emitters.cmake) covers generated code
# only; this covers the accessors in this component. See the .cmake header for
# why it exists — GIR-10 reintroduced two `.ValueOrDie()` sites here that the
# emitter guard could not see.
# ---------------------------------------------------------------------------
add_test(
    NAME ArrowBridgeInvariants.NoValueOrDieInRuntimeSources
    COMMAND "${CMAKE_COMMAND}"
        -DRUNTIME_INCLUDE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/../include
        -DRUNTIME_SRC_DIR=${CMAKE_CURRENT_SOURCE_DIR}/../src
        -P ${CMAKE_CURRENT_SOURCE_DIR}/check_no_value_or_die_in_runtime.cmake)

# Same reason as the gtest rows above - without this there is no cap at all, and a
# `gtest_discover_tests(PROPERTIES ...)` argument does not reach an `add_test` entry. This one
# shells out to `cmake -P`, so its cost is a script interpreter walking two source trees: 0.07 s
# measured, work rather than a wait. 30 s is ~430x that.
set_tests_properties(ArrowBridgeInvariants.NoValueOrDieInRuntimeSources PROPERTIES
    TIMEOUT 30)
