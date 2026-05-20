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
gtest_discover_tests(gateway_tests)
