# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
find_package(GTest REQUIRED)

add_executable(copy_probe_tests
    test_tracing_window.cpp)

target_link_libraries(copy_probe_tests PRIVATE
    GTest::gtest_main
    fletcher-copy-probe)

include(GoogleTest)

# Every row is work on a fixed in-memory slot - no provider thread, no wait - so
# 30 s is orders of magnitude above any honest run and exists only so a hang
# fails the job rather than running until it is cancelled (see
# pubsub/tests/CMakeLists.txt for why this suite has no default cap).
gtest_discover_tests(copy_probe_tests PROPERTIES TIMEOUT 30)
