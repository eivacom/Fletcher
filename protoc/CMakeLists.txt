# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
cmake_minimum_required(VERSION 3.15)
project(fletcher-protoc LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(protobuf REQUIRED)

# Core logic as a static library (reusable by tests).
add_library(fletcher_plugin_core STATIC
    src/type_mapper.cpp
    src/generator.cpp)
target_include_directories(fletcher_plugin_core PUBLIC include PRIVATE src)
target_link_libraries(fletcher_plugin_core PUBLIC
    protobuf::libprotoc
    protobuf::libprotobuf)

# The protoc plugin executable.
add_executable(fletcher-protoc src/main.cpp)
target_link_libraries(fletcher-protoc PRIVATE fletcher_plugin_core)

# Tests (opt-in via FLETCHER_BUILD_TESTS, default OFF).
option(FLETCHER_BUILD_TESTS "Build unit tests" OFF)
if(FLETCHER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

install(TARGETS fletcher-protoc DESTINATION bin)
