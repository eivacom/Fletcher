# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
# gateway is distributed as a single executable. There is no public
# library API, no installed headers: the WebSocket protocol is the
# only supported integration point. The CMakeLists.txt is consumed
# either standalone (against a Conan cache that has the deps below)
# or via add_subdirectory from a parent project that has already done
# the conan install.
#
# Boost.Beast / Boost.Asio are header-only. The Conan Boost recipe
# does not populate per-component INCLUDE_DIRS, so we locate the
# headers via find_path (the Conan cache is already on
# CMAKE_PREFIX_PATH for any consumer that ran conan install).
cmake_minimum_required(VERSION 3.15)
project(gateway LANGUAGES CXX)

find_package(fletcher-pubsub CONFIG REQUIRED)
find_package(fletcher-core   CONFIG REQUIRED)
find_package(nlohmann_json        CONFIG REQUIRED)
find_path(BOOST_INCLUDE_DIR boost/beast.hpp REQUIRED)

# ─────────────────────────────────────────────────────────────────────
# gateway_codec — internal helper library bundling the pure functions
# that the exe and the unit tests both consume. Not installed, not
# exported; the only reason it exists as a separate target is so the
# test executable can link the same objects without recompiling from
# scratch or reaching across to src/ via relative paths.
# ─────────────────────────────────────────────────────────────────────
add_library(gateway_codec STATIC
    src/schema_codec.cpp
    src/publish_frame.cpp)
target_include_directories(gateway_codec PUBLIC src)
target_link_libraries(gateway_codec PUBLIC
    fletcher-pubsub::fletcher-pubsub
    nlohmann_json::nlohmann_json)

# ─────────────────────────────────────────────────────────────────────
# gateway — the only artifact this directory produces. Links against
# gateway_codec for the pure helpers + brings in the WebSocket/IO
# code (main.cpp, gateway.cpp, ws_session.cpp).
# ─────────────────────────────────────────────────────────────────────
add_executable(gateway
    src/main.cpp
    src/gateway.cpp
    src/ws_session.cpp)
target_include_directories(gateway PRIVATE src ${BOOST_INCLUDE_DIR})
target_link_libraries(gateway PRIVATE
    gateway_codec
    fletcher-pubsub::fletcher-pubsub
    fletcher-core::fletcher-core
    nlohmann_json::nlohmann_json)
if(WIN32)
    target_compile_definitions(gateway PRIVATE
        WIN32_LEAN_AND_MEAN
        _WIN32_WINNT=0x0A00)   # Windows 10+
endif()

# ─────────────────────────────────────────────────────────────────────
# Optional unit-test suite. Wired in when FLETCHER_BUILD_TESTS is on,
# matching the cache-variable name other Fletcher components use.
# ─────────────────────────────────────────────────────────────────────
if(FLETCHER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
