# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
cmake_minimum_required(VERSION 3.15)
project(gateway-end-to-end-integration CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(eiva-fletcher-pubsub CONFIG REQUIRED)
find_package(eiva-fletcher-core CONFIG REQUIRED)
find_package(Boost CONFIG REQUIRED COMPONENTS headers)
find_package(nlohmann_json CONFIG REQUIRED)

# Boost.Beast / Boost.Asio are header-only and the Conan Boost recipe
# does not populate per-component INCLUDE_DIRS, so locate the headers
# the same way gateway/CMakeLists.txt does.
find_path(BOOST_INCLUDE_DIR boost/beast.hpp REQUIRED)

# ─────────────────────────────────────────────────────────────────────
# gateway has no conanfile.py and is not published as a Conan package,
# so we compile its sources inline into a private static library that
# only this integration test consumes. Path is relative to this
# CMakeLists.txt; the workflow checks the gateway/ tree alongside this
# directory.
# ─────────────────────────────────────────────────────────────────────
set(GATEWAY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../gateway")

add_library(web_gateway_inline STATIC
    "${GATEWAY_DIR}/src/web_gateway.cpp"
    "${GATEWAY_DIR}/src/ws_session.cpp")
target_include_directories(web_gateway_inline
    PUBLIC  "${GATEWAY_DIR}/include"
    PRIVATE "${GATEWAY_DIR}/src" ${BOOST_INCLUDE_DIR})
target_link_libraries(web_gateway_inline
    PUBLIC  eiva-fletcher-pubsub::eiva-fletcher-pubsub
    PRIVATE nlohmann_json::nlohmann_json)
if(WIN32)
    # Mirror gateway/CMakeLists.txt — these are Windows-only knobs that
    # would mis-cue Boost.Asio's platform detection on Linux.
    target_compile_definitions(web_gateway_inline PRIVATE
        WIN32_LEAN_AND_MEAN
        _WIN32_WINNT=0x0A00)
endif()

# ─────────────────────────────────────────────────────────────────────
# Test fixture binary — spawned and torn down by the vitest test.
# ─────────────────────────────────────────────────────────────────────
add_executable(test_server src/test_server.cpp)
target_link_libraries(test_server PRIVATE
    web_gateway_inline
    eiva-fletcher-pubsub::eiva-fletcher-pubsub
    eiva-fletcher-core::eiva-fletcher-core)
if(WIN32)
    target_compile_definitions(test_server PRIVATE
        WIN32_LEAN_AND_MEAN
        _WIN32_WINNT=0x0A00)
endif()
