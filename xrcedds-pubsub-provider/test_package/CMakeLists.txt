# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
cmake_minimum_required(VERSION 3.15)
project(PackageTest CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(fletcher-xrcedds-pubsub-provider CONFIG REQUIRED)

add_executable(example src/example.cpp)
target_link_libraries(example fletcher-xrcedds-pubsub-provider::fletcher-xrcedds-pubsub-provider)
