# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
cmake_minimum_required(VERSION 3.15)
project(fastdds-xrce-interop-integration CXX)

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

# UAGENT_SUPERBUILD nests the Agent's own fast-dds / fast-cdr / asio /
# tinyxml2 / spdlog ExternalProjects several levels under this project's
# prefix. On Windows the default prefix (deep under the test's build/
# dir) pushes those nested build paths past the 260-char MAX_PATH limit,
# which breaks MSBuild's FileTracker (FTK1011 / C1083) during the
# superbuild's own configure. A short prefix keeps the nested paths well
# under the limit. Overridable (cache var) for local builds that prefer a
# different writable location.
#
# The install dir is likewise a fixed path on Windows (not under the
# per-run CMake build dir) so CI can cache the built Agent across runs —
# see the "Cache MicroXRCEAgent build" step in
# ci.integration-test.fastdds-xrce-interop.yml. The cached unit is the
# install dir; wipe it to force a rebuild after bumping GIT_TAG.
if(WIN32)
    set(AGENT_PREFIX "C:/fl-uxa" CACHE PATH
        "MicroXRCEAgent ExternalProject prefix (short on Windows to avoid MAX_PATH)")
    set(AGENT_INSTALL_DIR "C:/fl-uxa-install" CACHE PATH
        "MicroXRCEAgent install dir (fixed path so CI can cache it across runs)")
else()
    set(AGENT_PREFIX "${CMAKE_BINARY_DIR}/microxrcedds_agent_proj-prefix" CACHE PATH
        "MicroXRCEAgent ExternalProject prefix")
    set(AGENT_INSTALL_DIR "${CMAKE_BINARY_DIR}/microxrcedds_agent-install" CACHE PATH
        "MicroXRCEAgent install dir")
endif()

set(AGENT_BINARY ${AGENT_INSTALL_DIR}/bin/MicroXRCEAgent${CMAKE_EXECUTABLE_SUFFIX})
set(AGENT_LIB_DIR ${AGENT_INSTALL_DIR}/lib)

# Skip the (slow) superbuild entirely when a *complete* Agent install is
# already present — restored from the CI cache, or left by a previous
# local build. We require both the executable and a non-empty lib dir:
# the test injects AGENT_LIB_DIR into the Agent child's loader path
# (MICRO_XRCE_AGENT_LIB_DIR), so a bare binary from a partial/interrupted
# install would make us skip the build and then fail at runtime when the
# Agent can't load its libraries. An empty stand-in target keeps the
# add_dependencies() below valid. Delete AGENT_INSTALL_DIR to force a
# fresh build after a GIT_TAG bump.
file(GLOB _agent_runtime_libs "${AGENT_LIB_DIR}/*")
if(EXISTS "${AGENT_BINARY}" AND _agent_runtime_libs)
    message(STATUS "MicroXRCEAgent found at ${AGENT_BINARY} — skipping ExternalProject build.")
    add_custom_target(microxrcedds_agent_proj)
else()
    # The Agent uses *this* project's Fast DDS and Fast CDR — the ones Conan resolved and the ones
    # the test binary itself links — rather than building its own.
    #
    # Left to itself the Agent's superbuild clones Fast DDS from the moving `3.x` branch
    # (Micro-XRCE-DDS-Agent's CMakeLists: `set(_fastdds_tag 3.x)`), so an Agent built today gets
    # whatever Fast DDS released most recently while this repo pins fast-dds/3.4.0. Nobody chooses
    # that version and nothing pins it, which is how the Agent here ended up on 3.6 — and its
    # from-source build on MSVC 19.4 then failed inside create_participant with
    # "foonathan::memory ... received invalid size/alignment 64, max supported is 56", a node-size
    # table Fast DDS fixes at build time for the toolchains it knows. Conan's 3.4.0 has no such
    # problem; every other suite in this repo creates participants against it constantly.
    #
    # UAGENT_USE_SYSTEM_FASTDDS/FASTCDR switch the superbuild from cloning those two to
    # find_package(fastdds 3) / find_package(fastcdr 2); CMAKE_PREFIX_PATH is what lets them
    # resolve, and it is this project's own — so the Agent links the very packages the test binary
    # links. Conan builds fast-dds static here, so there is not even a DLL to resolve at run time.
    #
    # Passing CMAKE_PREFIX_PATH rather than CMAKE_TOOLCHAIN_FILE is deliberate. The Agent's
    # SuperBuild.cmake forwards only CMAKE_TOOLCHAIN_FILE to its own nested ExternalProjects and
    # nothing else, so handing it the Conan toolchain makes spdlog's configure fail on
    # "CMake policy CMP0091 must be NEW" — the policy Conan normally sets on the command line beside
    # the toolchain, which never reaches that far. The prefix path needs no such companion. Its
    # final stage shares this binary dir (and therefore this cache) with the superbuild pass, so the
    # options set here are the ones its find_package sees.
    #
    # LIST_SEPARATOR because CMAKE_PREFIX_PATH is a CMake list and would otherwise be cut at its
    # first semicolon.
    string(REPLACE ";" "|" _agent_prefix_path "${CMAKE_PREFIX_PATH}")
    ExternalProject_Add(microxrcedds_agent_proj
        PREFIX         ${AGENT_PREFIX}
        GIT_REPOSITORY https://github.com/eProsima/Micro-XRCE-DDS-Agent.git
        GIT_TAG        v3.0.1
        LIST_SEPARATOR |
        CMAKE_ARGS
            -DCMAKE_INSTALL_PREFIX=${AGENT_INSTALL_DIR}
            -DCMAKE_BUILD_TYPE=$<CONFIG>
            -DCMAKE_PREFIX_PATH=${_agent_prefix_path}
            -DUAGENT_SUPERBUILD=ON
            -DUAGENT_USE_SYSTEM_FASTDDS=ON
            -DUAGENT_USE_SYSTEM_FASTCDR=ON
            -DUAGENT_BUILD_TESTS=OFF
            -DUAGENT_BUILD_EXAMPLES=OFF
        BUILD_BYPRODUCTS ${AGENT_BINARY}
    )
endif()

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
