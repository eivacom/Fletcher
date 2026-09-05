# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
find_package(GTest REQUIRED)

add_executable(pubsub_tests
    test_publisher_subscriber.cpp
    test_owned_schema.cpp
    test_segments.cpp)

target_link_libraries(pubsub_tests PRIVATE
    GTest::gtest_main
    fletcher-pubsub)

include(GoogleTest)

# ---------------------------------------------------------------------------
# TIMEOUT, stated because this suite has no default one to fall back on.
#
# `pubsub/CMakeLists.txt` calls `enable_testing()` rather than `include(CTest)`, so no
# `DartConfiguration.tcl` is generated and ctest's usual 1500 s per-test cap DOES NOT APPLY: an
# unbounded test here runs until the CI job itself is cancelled. The precedent and the reasoning
# in full are in `xrcedds-pubsub-provider/tests/CMakeLists.txt`, whose first Linux run spent
# 2 h 04 m inside one hung test before the job was cancelled with no result at all.
#
# 30 s, chosen from this suite's own measured wall clock: the slowest row is
# `SubscriberTest.SubscribeReturnsSchemaFromProvider` at 0.05 s, the rest at 0.03-0.04 s, and the
# whole suite at 0.69 s (msvc-194 x86_64 Release). Every row is WORK, not a wait: the provider
# under test is the in-process one, so fan-out and unsubscribe run synchronously on the calling
# thread with no transport and no timed wait. 30 s is ~600x the slowest row - unreachable by
# runner contention - and it turns a future hang into a 30-second red instead of a cancelled job.
gtest_discover_tests(pubsub_tests DISCOVERY_MODE PRE_TEST
    PROPERTIES TIMEOUT 30)
