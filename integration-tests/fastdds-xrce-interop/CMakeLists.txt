# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
cmake_minimum_required(VERSION 3.15)
project(fastdds-xrce-interop-integration CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(fletcher-pubsub-arrow CONFIG REQUIRED)
find_package(fletcher-fastdds-pubsub-provider CONFIG REQUIRED)
find_package(fletcher-xrcedds-pubsub-provider CONFIG REQUIRED)
find_package(Arrow CONFIG REQUIRED)
find_package(GTest REQUIRED)

enable_testing()

# ── MicroXRCEAgent ──────────────────────────────────────────────────
# Build eProsima's Agent from source as an ExternalProject so this test
# is fully self-contained. UAGENT_SUPERBUILD=ON tells the Agent's CMake
# to fetch and build its own fast-dds / fast-cdr / asio / tinyxml2 /
# micro-cdr — keeps the Agent's dependency tree completely isolated
# from the Conan-provided dependencies the test itself links against.
#
# First build is slow (~10–15 min). Subsequent builds use the CMake
# build directory's cache, so iteration is fast.
include(ExternalProject)

set(AGENT_INSTALL_DIR ${CMAKE_BINARY_DIR}/microxrcedds_agent-install)
set(AGENT_BINARY ${AGENT_INSTALL_DIR}/bin/MicroXRCEAgent${CMAKE_EXECUTABLE_SUFFIX})
set(AGENT_LIB_DIR ${AGENT_INSTALL_DIR}/lib)

ExternalProject_Add(microxrcedds_agent_proj
    GIT_REPOSITORY https://github.com/eProsima/Micro-XRCE-DDS-Agent.git
    GIT_TAG        v2.4.3
    CMAKE_ARGS
        -DCMAKE_INSTALL_PREFIX=${AGENT_INSTALL_DIR}
        -DCMAKE_BUILD_TYPE=$<CONFIG>
        -DUAGENT_SUPERBUILD=ON
        -DUAGENT_BUILD_TESTS=OFF
        -DUAGENT_BUILD_EXAMPLES=OFF
    BUILD_BYPRODUCTS ${AGENT_BINARY}
)

# ── Integration test binary ─────────────────────────────────────────
add_executable(integration_tests
    tests/test_interop.cpp)

target_compile_definitions(integration_tests PRIVATE
    MICRO_XRCE_AGENT_PATH="${AGENT_BINARY}"
    MICRO_XRCE_AGENT_LIB_DIR="${AGENT_LIB_DIR}")

# The test fixture spawns the Agent at runtime, so we don't link to it
# — but the binary must exist before ctest runs.
add_dependencies(integration_tests microxrcedds_agent_proj)

target_link_libraries(integration_tests PRIVATE
    GTest::gtest_main
    fletcher-pubsub-arrow::fletcher-pubsub-arrow
    fletcher-fastdds-pubsub-provider::fletcher-fastdds-pubsub-provider
    fletcher-xrcedds-pubsub-provider::fletcher-xrcedds-pubsub-provider)

# A single ctest entry that runs all gtest cases inside one process.
# This is intentional: the MicroXRCEAgent fixture binds UDP port 2018,
# so two ctest-spawned processes would race for the port. Sharing one
# process means one Agent setup/teardown for the whole suite.
add_test(NAME integration_tests COMMAND integration_tests)
