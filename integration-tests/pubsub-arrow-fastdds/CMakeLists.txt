# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
cmake_minimum_required(VERSION 3.15)
project(pubsub-arrow-fastdds-integration CXX)

find_package(fletcher-pubsub-arrow CONFIG REQUIRED)
find_package(fletcher-fastdds-pubsub-provider CONFIG REQUIRED)
find_package(Arrow CONFIG REQUIRED)
find_package(GTest REQUIRED)

enable_testing()

add_executable(integration_tests
    tests/test_roundtrip.cpp)

target_link_libraries(integration_tests PRIVATE
    GTest::gtest_main
    fletcher-pubsub-arrow::fletcher-pubsub-arrow
    fletcher-fastdds-pubsub-provider::fletcher-fastdds-pubsub-provider)

include(GoogleTest)

# ---------------------------------------------------------------------------
# TIMEOUT, stated because this suite has no default one to fall back on.
#
# This directory calls `enable_testing()` rather than `include(CTest)`, so no
# `DartConfiguration.tcl` is generated and ctest's usual 1500 s per-test cap DOES NOT APPLY: an
# unbounded test here runs until the CI job itself is cancelled. The precedent and the reasoning
# in full are in `xrcedds-pubsub-provider/tests/CMakeLists.txt`, whose first Linux run spent
# 2 h 04 m inside one hung test before the job was cancelled with no result at all.
#
# 120 s, chosen from this suite's own measured wall clock AND from the budgets the tests impose on
# themselves. Measured (msvc-194 x86_64 Release): SchemaAndRowDeliveredAcrossDdsBoundary 0.52 s,
# BatchedRecordBatchDeliveredAcrossDdsBoundary 0.47 s, MultipleRowsDeliveredInOrder 0.45 s,
# SubscribeBeforePublishDeliversWithSchema 0.38 s. Every row is dominated by Fast DDS discovery
# between a publisher and a subscriber in one process, which is WORK - a slower or more contended
# runner, or one with a busier multicast interface, genuinely stretches it, unlike a sleep. So the
# cap is set from the ceiling each test already enforces on itself rather than from the half-second
# happy path: each bounds itself at 15 s (AwaitArrowSchema) plus 5-10 s (the delivery
# `cv.wait_for`), i.e. ~25 s if every deadline were spent, after which the test FAILS itself with
# a diagnosable message. A cap below that would replace that message with a bare timeout, so 120 s
# is deliberately ~5x the self-imposed ceiling (and ~230x the measured slowest row): it cannot
# false-red, it never pre-empts the tests' own reporting, and it turns a hang BELOW those
# deadlines into a two-minute red instead of a cancelled job.
gtest_discover_tests(integration_tests DISCOVERY_MODE PRE_TEST
    PROPERTIES TIMEOUT 120)
