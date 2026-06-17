# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
cmake_minimum_required(VERSION 3.15)
project(fletcher-fastdds-pubsub-provider CXX)

find_package(fletcher-pubsub REQUIRED)
find_package(fletcher-core REQUIRED)
find_package(fastdds REQUIRED)

add_library(fletcher-fastdds-pubsub-provider STATIC
    src/fast_dds_pubsub_provider.cpp
    src/qos_defaults.cpp)

# C++ standard comes from the Conan profile (compiler.cppstd). This only
# states the public minimum the headers require, propagated to consumers.
target_compile_features(fletcher-fastdds-pubsub-provider PUBLIC cxx_std_20)
target_include_directories(fletcher-fastdds-pubsub-provider PUBLIC include)
# fast-dds is PUBLIC because FastDDSProviderOptions exposes DataWriterQos
# and DataReaderQos in the public header — consumers need the FastDDS
# headers transitively to construct the options.
target_link_libraries(fletcher-fastdds-pubsub-provider
    PUBLIC  fletcher-pubsub::fletcher-pubsub
            fastdds::fastdds
    PRIVATE fletcher-core::fletcher-core)

if(FLETCHER_BUILD_TESTS)
    include(CTest)
    add_subdirectory(tests)
endif()
