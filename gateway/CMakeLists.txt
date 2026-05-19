# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
# Gateway is not a published Conan package; it is built either via
# add_subdirectory() from a parent project (e.g.
# integration-tests/gateway-end-to-end/), or as a standalone CMake
# project against a Conan cache that has the deps below.
#
# Boost.Beast and Boost.Asio are header-only.  The Conan Boost recipe
# does not populate per-component INCLUDE_DIRS, so we locate the
# headers via find_path (the Conan cache is already on
# CMAKE_PREFIX_PATH for any consumer that ran conan install).
find_package(eiva-fletcher-pubsub CONFIG REQUIRED)
find_package(eiva-fletcher-core   CONFIG REQUIRED)
find_package(nlohmann_json        CONFIG REQUIRED)
find_path(BOOST_INCLUDE_DIR boost/beast.hpp REQUIRED)

# ─────────────────────────────────────────────────────────────────────
# Gateway library — the production code under src/.
# ─────────────────────────────────────────────────────────────────────
add_library(gateway STATIC
    src/web_gateway.cpp
    src/ws_session.cpp)
target_include_directories(gateway
    PUBLIC  include
    PRIVATE src ${BOOST_INCLUDE_DIR})
target_link_libraries(gateway
    PUBLIC  eiva-fletcher-pubsub::eiva-fletcher-pubsub
    PRIVATE nlohmann_json::nlohmann_json)
if(WIN32)
    target_compile_definitions(gateway PRIVATE
        WIN32_LEAN_AND_MEAN
        _WIN32_WINNT=0x0A00)   # Windows 10+
endif()

# ─────────────────────────────────────────────────────────────────────
# Test-fixture binary — `gateway-test-server`.
# Wraps WebGateway with an InProcessProvider so a CI test (or a dev
# loop) can drive the WebSocket protocol against a real exe. Not a
# production binary: gateway has no real DDS-backed provider yet.
# Placed under tests/ to match how the rest of Fletcher's components
# scope their non-production fixtures.
# ─────────────────────────────────────────────────────────────────────
add_executable(gateway-test-server tests/server_main.cpp)
target_link_libraries(gateway-test-server PRIVATE
    gateway
    eiva-fletcher-pubsub::eiva-fletcher-pubsub
    eiva-fletcher-core::eiva-fletcher-core)
if(WIN32)
    target_compile_definitions(gateway-test-server PRIVATE
        WIN32_LEAN_AND_MEAN
        _WIN32_WINNT=0x0A00)
endif()
