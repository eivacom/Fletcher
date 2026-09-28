# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
# The copy-accounting instrument, as a TEST-ONLY static library (D-BIND-62).
# Two consumers score with it: pubsub-conformance's CopyAccounting suite and the
# binding shim's probe variant, `fletcher-c-abi-probe`. Never released.
cmake_minimum_required(VERSION 3.15)
project(fletcher-copy-probe LANGUAGES CXX)

find_package(fletcher-pubsub CONFIG REQUIRED)

add_library(fletcher-copy-probe STATIC
    src/ledger.cpp
    src/seam_probe_provider.cpp
    src/tracing_window.cpp)

target_compile_features(fletcher-copy-probe PUBLIC cxx_std_20)

target_include_directories(fletcher-copy-probe PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

target_link_libraries(fletcher-copy-probe PUBLIC fletcher-pubsub::fletcher-pubsub)

option(FLETCHER_BUILD_TESTS "Build unit tests" OFF)
if(FLETCHER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
