# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
find_package(GTest REQUIRED)

add_executable(gateway_tests
    test_publish_frame.cpp
    test_schema_codec.cpp)

target_link_libraries(gateway_tests PRIVATE
    GTest::gtest_main
    gateway_codec)

include(GoogleTest)

# ---------------------------------------------------------------------------
# TIMEOUT, stated because this suite has no default one to fall back on.
#
# `gateway/CMakeLists.txt` calls `enable_testing()` rather than `include(CTest)`, so no
# `DartConfiguration.tcl` is generated and ctest's usual 1500 s per-test cap DOES NOT APPLY: an
# unbounded test here runs until the CI job itself is cancelled. The precedent and the reasoning
# in full are in `xrcedds-pubsub-provider/tests/CMakeLists.txt`, whose first Linux run spent
# 2 h 04 m inside one hung test before the job was cancelled with no result at all.
#
# 30 s, chosen from this suite's own measured wall clock: all 20 rows run in 0.02-0.03 s and the
# whole suite in 0.65 s (msvc-194 x86_64 Release). Every row is WORK, not a wait - frame parsing
# and JSON-to-Arrow schema construction, entirely in process, with no socket, no thread and no
# sleep anywhere in the suite. 30 s is ~1000x the slowest row, so it cannot false-red on a
# contended runner, and it turns a future hang into a 30-second red instead of a cancelled job.
gtest_discover_tests(gateway_tests DISCOVERY_MODE PRE_TEST
    PROPERTIES TIMEOUT 30)
