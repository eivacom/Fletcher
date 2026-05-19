# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
cmake_minimum_required(VERSION 3.15)
project(gateway-end-to-end-integration CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Pull gateway's CMakeLists.txt into our build tree. That gives us the
# `gateway-test-server` exe target without duplicating any gateway
# build configuration here. gateway/ has no Conan recipe, so we build
# it from source; the deps it needs (pubsub, core, boost, nlohmann_json)
# are declared in conanfile.py and are already on CMAKE_PREFIX_PATH by
# the time we get here.
add_subdirectory(
    "${CMAKE_CURRENT_SOURCE_DIR}/../../gateway"
    "${CMAKE_CURRENT_BINARY_DIR}/gateway_build")
