# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
cmake_minimum_required(VERSION 3.15)
project(fletcher-pubsub-arrow LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(fletcher-pubsub CONFIG REQUIRED)
find_package(fletcher-arrow-bridge CONFIG REQUIRED)
find_package(Arrow CONFIG REQUIRED)

add_library(fletcher-pubsub-arrow STATIC
    src/pubsub_arrow.cpp)

target_include_directories(fletcher-pubsub-arrow
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

target_link_libraries(fletcher-pubsub-arrow PUBLIC
    fletcher-pubsub::fletcher-pubsub
    fletcher-arrow-bridge::fletcher-arrow-bridge
    arrow::arrow)

option(FLETCHER_BUILD_TESTS "Build unit tests" OFF)
if(FLETCHER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
