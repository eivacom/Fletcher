# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
cmake_minimum_required(VERSION 3.15)
project(fletcher-arrow-bridge-benchmarks CXX)

find_package(fletcher-arrow-bridge CONFIG REQUIRED)
find_package(fletcher-pubsub-arrow CONFIG REQUIRED)
find_package(fletcher-pubsub CONFIG REQUIRED)
find_package(fletcher-core CONFIG REQUIRED)
find_package(Arrow CONFIG REQUIRED)
find_package(benchmark CONFIG REQUIRED)

add_executable(bench_arrow_codec bench_arrow_codec.cpp)
target_compile_features(bench_arrow_codec PRIVATE cxx_std_20)

# Counts allocations around one sample iteration for the allocs_per_row counters (see
# bench_arrow_codec.cpp). Default ON for this target; the override lives entirely in the .cpp, so
# turning it off just drops the operator new/delete overrides and the counters they feed.
target_compile_definitions(bench_arrow_codec PRIVATE FLETCHER_BENCH_COUNT_ALLOCS)

target_link_libraries(
    bench_arrow_codec
    PRIVATE fletcher-arrow-bridge::fletcher-arrow-bridge
            fletcher-pubsub-arrow::fletcher-pubsub-arrow
            fletcher-pubsub::fletcher-pubsub
            fletcher-core::fletcher-core
            arrow::arrow
            benchmark::benchmark)
