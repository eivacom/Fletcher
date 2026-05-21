# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
cmake_minimum_required(VERSION 3.15)
project(fletcher-core LANGUAGES CXX)

add_library(fletcher-core INTERFACE)
target_include_directories(fletcher-core INTERFACE include)
# Headers use C++20 features (<bit>, std::endian, std::span, …). Propagate the
# requirement to consumers so a profile/configure that forgets to set the
# standard fails fast instead of producing cryptic errors in the headers.
target_compile_features(fletcher-core INTERFACE cxx_std_20)

if(FLETCHER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
