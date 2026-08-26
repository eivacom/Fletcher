# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
cmake_minimum_required(VERSION 3.15)
project(fletcher-fastdds-pubsub-provider-benchmarks CXX)

find_package(fletcher-fastdds-pubsub-provider CONFIG REQUIRED)
find_package(fletcher-core CONFIG REQUIRED)
find_package(benchmark CONFIG REQUIRED)

add_executable(bench_pub_sub_type bench_pub_sub_type.cpp)
target_link_libraries(bench_pub_sub_type PRIVATE benchmark::benchmark)

# The narrated walkthrough: same types, same row, no timing.
add_executable(example_arrow_roundtrip example_arrow_roundtrip.cpp)

add_executable(exp_zero_copy exp_zero_copy.cpp)

foreach(_target bench_pub_sub_type example_arrow_roundtrip exp_zero_copy)
    target_compile_features(${_target} PRIVATE cxx_std_20)

    # src/internal is on the include path so that the type exercised is the shipped one rather than a
    # copy of it. Only the legacy arm is vendored, because it no longer exists anywhere else.
    target_include_directories(
        ${_target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/internal)

    target_link_libraries(
        ${_target}
        PRIVATE fletcher-core::fletcher-core
                fletcher-fastdds-pubsub-provider::fletcher-fastdds-pubsub-provider)
endforeach()
