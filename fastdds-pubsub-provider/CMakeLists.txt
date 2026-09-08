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
# fast-dds is PRIVATE, and that is a machine check rather than hygiene: the public header names
# no eProsima type, so no consumer needs the Fast DDS headers to configure this provider (owner
# ruling 2026-08-31, "Fletcher never learns DDS vocabulary"). With `transitive_headers` also
# dropped from conanfile.py, `test_package` compiles with NO Fast DDS include directories at all,
# so any eProsima type that creeps back into the installed header is a compile error there. The
# library is STATIC, so `transitive_libs` must stay — consumers still LINK the chain.
target_link_libraries(fletcher-fastdds-pubsub-provider
    PUBLIC  fletcher-pubsub::fletcher-pubsub
    PRIVATE fletcher-core::fletcher-core
            fastdds)

if(FLETCHER_BUILD_TESTS)
    include(CTest)
    add_subdirectory(tests)
endif()
