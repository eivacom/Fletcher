# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
cmake_minimum_required(VERSION 3.15)
project(fastdds-xrce-interop-integration CXX)

find_package(fletcher-pubsub-arrow CONFIG REQUIRED)
find_package(fletcher-fastdds-pubsub-provider CONFIG REQUIRED)
find_package(fletcher-xrcedds-pubsub-provider CONFIG REQUIRED)
find_package(Arrow CONFIG REQUIRED)
find_package(GTest REQUIRED)

enable_testing()

# ── MicroXRCEAgent ──────────────────────────────────────────────────
# Built from the shared recipe, which the C# transport-conformance lane
# uses too (D-BIND-56). It defines AGENT_BINARY, AGENT_LIB_DIR and the
# microxrcedds_agent_proj target used below.
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/MicroXrceAgent.cmake)

# ── Integration test binary ─────────────────────────────────────────
add_executable(integration_tests
    tests/test_interop.cpp)

target_compile_definitions(integration_tests PRIVATE
    MICRO_XRCE_AGENT_PATH="${AGENT_BINARY}"
    MICRO_XRCE_AGENT_LIB_DIR="${AGENT_LIB_DIR}")

# The test fixture spawns the Agent at runtime, so we don't link to it
# — but the binary must exist before ctest runs.
add_dependencies(integration_tests microxrcedds_agent_proj)

target_link_libraries(integration_tests PRIVATE
    GTest::gtest_main
    fletcher-pubsub-arrow::fletcher-pubsub-arrow
    fletcher-fastdds-pubsub-provider::fletcher-fastdds-pubsub-provider
    fletcher-xrcedds-pubsub-provider::fletcher-xrcedds-pubsub-provider)

if(WIN32)
    # PDA-DEC-1H: the Agent fixture proves that OUR child holds the UDP port the three interop
    # cases run across, and on Windows that answer comes from GetExtendedUdpTable. A system
    # library, not a new dependency: no package, no option, nothing to fetch. The POSIX side
    # reads /proc and links nothing. The same two lines exist in
    # integration-tests/pubsub-conformance/CMakeLists.txt.
    target_link_libraries(integration_tests PRIVATE iphlpapi)
endif()

# A single ctest entry that runs all gtest cases inside one process.
# This is intentional: the MicroXRCEAgent fixture binds UDP port 2018,
# so two ctest-spawned processes would race for the port. Sharing one
# process means one Agent setup/teardown for the whole suite.
# 4 gtest cases in this one entry: the three interop directions plus
# PDA-DEC-1H's `AForeignAgentDoesNotSatisfyTheHarness`, which stands up two
# more Agents of its own on port 2118 inside its own scope.
add_test(NAME integration_tests COMMAND integration_tests)

# ---------------------------------------------------------------------------
# TIMEOUT, stated because this suite has no default one to fall back on.
#
# This directory calls `enable_testing()` rather than `include(CTest)`, so no
# `DartConfiguration.tcl` is generated and ctest's usual 1500 s per-test cap DOES NOT APPLY: an
# unbounded entry here runs until the CI job itself is cancelled. The precedent and the reasoning
# in full are in `xrcedds-pubsub-provider/tests/CMakeLists.txt`, whose first Linux run spent
# 2 h 04 m inside one hung test before the job was cancelled with no result at all. That risk is
# concentrated here more than anywhere else in the tree, because the single entry above is the
# WHOLE binary: a hang in any one of its five gtest cases hangs the only row ctest can see.
#
# 300 s, chosen from this entry's own measured wall clock AND from the budgets the tests impose on
# themselves. Measured: 7.04 s for the entry (msvc-194 x86_64 Release), of which ~3.7 s is inside
# gtest - AForeignAgentDoesNotSatisfyTheHarness 1.03 s, AFailedOwnershipQuery... 1.02 s, and the
# three interop directions 0.45-0.70 s - and the remaining ~3.3 s is the fixture forking
# MicroXRCEAgent and waiting for it to answer an XRCE probe. That is a MIXTURE, not a pure wait:
# the Agent spawn, the DDS discovery handshake and the row round-trips are work a slower runner
# stretches, while the ownership probes' 1 s poll budgets are wall clock and do not stretch.
# So the cap is set from the ceiling the suite already enforces on itself rather than from the
# 7 s happy path: each interop case bounds itself at 15 s (AwaitArrowSchema) + 10 s (row
# delivery), the reachability probe at 15 s, and the foreign-Agent case stands up two more Agents
# with the same 15 s budget each - about 120 s in total if every single deadline were spent. A
# PASSING run therefore cannot exceed ~120 s no matter how slow the runner, so 300 s (43x the
# measured figure, 2.5x the self-imposed ceiling) cannot false-red, and it turns a hang below
# those deadlines - a `connect()` that never returns, an Agent that never dies - into a
# five-minute red instead of a cancelled job.
set_tests_properties(integration_tests PROPERTIES
    TIMEOUT 300)
