# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
cmake_minimum_required(VERSION 3.15)
project(fletcher-fastdds-pubsub-provider-benchmarks CXX)

find_package(fletcher-fastdds-pubsub-provider CONFIG REQUIRED)
find_package(fletcher-core CONFIG REQUIRED)
# Explicit, because the provider links fast-dds PRIVATE and no longer exports its headers: every
# TU here includes <fastdds/...> directly (PDA-DEC-6 DEBT-4).
find_package(fastdds CONFIG REQUIRED)
find_package(benchmark CONFIG REQUIRED)

add_executable(bench_pub_sub_type bench_pub_sub_type.cpp)
target_link_libraries(bench_pub_sub_type PRIVATE benchmark::benchmark)

# The narrated walkthrough: same types, same row, no timing.
add_executable(example_arrow_roundtrip example_arrow_roundtrip.cpp)

add_executable(exp_zero_copy exp_zero_copy.cpp)

# BEFORE/AFTER numbers for the reader-side redesign: e2e latency and throughput, intraprocess.
add_executable(bench_e2e bench_e2e.cpp)

# Repeated Subscribe/Unsubscribe cycles against a flat-out publisher, own process per run: probes
# whether data_sharing AUTOMATIC on the data reader (item D) reproduces the Fast DDS 3.4.0
# DataSharingListener teardown spin the __schema channel is already known to hit (qos_defaults.cpp).
add_executable(probe_teardown probe_teardown.cpp)

foreach(_target bench_pub_sub_type example_arrow_roundtrip exp_zero_copy bench_e2e probe_teardown)
    target_compile_features(${_target} PRIVATE cxx_std_20)

    # src/internal is on the include path so that the type exercised is the shipped one rather than a
    # copy of it. Only the legacy arm is vendored, because it no longer exists anywhere else.
    target_include_directories(
        ${_target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/internal)

    target_link_libraries(
        ${_target}
        PRIVATE fletcher-core::fletcher-core
                fletcher-fastdds-pubsub-provider::fletcher-fastdds-pubsub-provider
                fastdds)
endforeach()
