# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
cmake_minimum_required(VERSION 3.15)
project(fletcher-arrow-bridge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(eiva-fletcher-core CONFIG REQUIRED)
find_package(Arrow CONFIG REQUIRED)

add_library(fletcher-arrow-bridge STATIC
    src/codec.cpp
    src/scalar_codec.cpp
    src/crs_utils.cpp)

target_include_directories(fletcher-arrow-bridge
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE src)

target_link_libraries(fletcher-arrow-bridge PUBLIC
    eiva-fletcher-core::eiva-fletcher-core
    arrow::arrow)

option(FLETCHER_BUILD_TESTS "Build unit tests" OFF)
if(FLETCHER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
