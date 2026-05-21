# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
cmake_minimum_required(VERSION 3.15)
project(pubsub-arrow-fastdds-integration CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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
gtest_discover_tests(integration_tests)
