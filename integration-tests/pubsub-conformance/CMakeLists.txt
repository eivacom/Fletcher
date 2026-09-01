# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
cmake_minimum_required(VERSION 3.15)
project(pubsub-conformance-integration CXX)

find_package(fletcher-pubsub CONFIG REQUIRED)
find_package(fletcher-fastdds-pubsub-provider CONFIG REQUIRED)
find_package(GTest REQUIRED)

enable_testing()

# Type-check broadly, link narrowly. The clause set is compiled ONCE and each
# subject binary links it plus exactly ONE provider, so editing a clause
# recompiles one TU and relinks a handful of small executables — and no binary
# links two providers.
#
# XRCE gates the two XRCE subjects AND the MicroXRCEAgent ExternalProject, so
# the inner loop can iterate on the forcing test without the Agent in the graph.
option(FLETCHER_CONFORMANCE_XRCE
       "Build the XRCE-DDS conformance subjects (needs a MicroXRCEAgent)" ON)

# ── MicroXRCEAgent ──────────────────────────────────────────────────
# Its own PREFIX (a shared source+build tree collides when two harnesses
# configure concurrently) but the SAME install dir as
# integration-tests/fastdds-xrce-interop — that install is what the CI cache
# buys, so the ~10-15 min superbuild happens once per machine, not once per
# harness. See that harness's CMakeLists for why the paths are short on Windows
# and why UAGENT_USE_SYSTEM_FASTDDS is on.
if(FLETCHER_CONFORMANCE_XRCE)
    find_package(fletcher-xrcedds-pubsub-provider CONFIG REQUIRED)
    include(ExternalProject)

    if(WIN32)
        set(AGENT_PREFIX "C:/fl-uxa-conf" CACHE PATH
            "MicroXRCEAgent ExternalProject prefix (short on Windows to avoid MAX_PATH)")
        set(AGENT_INSTALL_DIR "C:/fl-uxa-install" CACHE PATH
            "MicroXRCEAgent install dir (shared with fastdds-xrce-interop, cached by CI)")
    else()
        set(AGENT_PREFIX "${CMAKE_BINARY_DIR}/microxrcedds_agent_proj-prefix" CACHE PATH
            "MicroXRCEAgent ExternalProject prefix")
        set(AGENT_INSTALL_DIR "${CMAKE_BINARY_DIR}/microxrcedds_agent-install" CACHE PATH
            "MicroXRCEAgent install dir")
    endif()

    set(AGENT_BINARY ${AGENT_INSTALL_DIR}/bin/MicroXRCEAgent${CMAKE_EXECUTABLE_SUFFIX})
    set(AGENT_LIB_DIR ${AGENT_INSTALL_DIR}/lib)

    # A complete install (binary AND a non-empty lib dir) short-circuits the
    # superbuild. A bare binary from an interrupted install would make us skip
    # the build and then fail at run time when the Agent cannot load its libs.
    file(GLOB _agent_runtime_libs "${AGENT_LIB_DIR}/*")
    if(EXISTS "${AGENT_BINARY}" AND _agent_runtime_libs)
        message(STATUS "MicroXRCEAgent found at ${AGENT_BINARY} — skipping ExternalProject build.")
        add_custom_target(microxrcedds_agent_proj)
    else()
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
else()
    message(STATUS
        "FLETCHER_CONFORMANCE_XRCE=OFF — the XrceLocal and XrceCrossProcess subjects "
        "do NOT exist in this build, and neither does the MicroXRCEAgent ExternalProject.")
endif()

# ── The contract, compiled once ─────────────────────────────────────
# OBJECT libraries, not STATIC: a TEST_P's registration is a static initialiser
# with no symbol the subject binary references, and a linker drops it out of a
# static archive. Object libraries put every object on the link line.
#
# `conformance_support` is gtest-free on purpose — the peer children include the
# schemas and the row codec, and must not link a test framework.
add_library(conformance_support OBJECT
    src/fixtures.cpp
    src/local_subject.cpp
    src/peer_subject.cpp
    src/child_process.cpp
    src/peer_main.cpp)
target_include_directories(conformance_support PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(conformance_support PUBLIC cxx_std_20)
# Note the link line: fletcher-pubsub and nothing else. A provider header cannot
# resolve from here, which is the machine check that the clause set is
# provider-agnostic.
target_link_libraries(conformance_support PUBLIC fletcher-pubsub::fletcher-pubsub)

# NB an OBJECT library's objects are not propagated TRANSITIVELY, so every
# subject binary lists conformance_support itself.
add_library(conformance_clauses OBJECT src/clauses.cpp)
target_link_libraries(conformance_clauses PUBLIC conformance_support GTest::gtest)

# Clause 2's axis gate IS this separate target: linked into the schema-carrying
# subjects' binaries only, so on a schema-less subject the clause is absent from
# the ctest list rather than present and skipped.
add_library(conformance_clauses_carried OBJECT src/clauses_carried.cpp)
target_link_libraries(conformance_clauses_carried PUBLIC conformance_support GTest::gtest)

# ── Subject binaries ────────────────────────────────────────────────
# The in-process loopback: schema-LESS (§7 clause 1's last sentence), so no
# clause-2 TU.
add_executable(conformance_inprocess subjects/inprocess_main.cpp)
target_link_libraries(conformance_inprocess PRIVATE
    conformance_support
    conformance_clauses
    GTest::gtest_main)

add_executable(conformance_fastdds_peer subjects/fastdds_peer_main.cpp)
target_link_libraries(conformance_fastdds_peer PRIVATE
    conformance_support
    fletcher-fastdds-pubsub-provider::fletcher-fastdds-pubsub-provider)

add_executable(conformance_fastdds subjects/fastdds_main.cpp)
target_link_libraries(conformance_fastdds PRIVATE
    conformance_support
    conformance_clauses
    conformance_clauses_carried
    fletcher-fastdds-pubsub-provider::fletcher-fastdds-pubsub-provider
    GTest::gtest_main)
target_compile_definitions(conformance_fastdds PRIVATE
    CONFORMANCE_FASTDDS_PEER="$<TARGET_FILE:conformance_fastdds_peer>")
add_dependencies(conformance_fastdds conformance_fastdds_peer)

if(FLETCHER_CONFORMANCE_XRCE)
    add_executable(conformance_xrce_peer subjects/xrce_peer_main.cpp)
    target_link_libraries(conformance_xrce_peer PRIVATE
        conformance_support
        fletcher-xrcedds-pubsub-provider::fletcher-xrcedds-pubsub-provider)

    add_executable(conformance_xrce subjects/xrce_main.cpp)
    target_link_libraries(conformance_xrce PRIVATE
        conformance_support
        conformance_clauses
        conformance_clauses_carried
        fletcher-xrcedds-pubsub-provider::fletcher-xrcedds-pubsub-provider
        GTest::gtest_main)
    target_compile_definitions(conformance_xrce PRIVATE
        CONFORMANCE_XRCE_PEER="$<TARGET_FILE:conformance_xrce_peer>"
        MICRO_XRCE_AGENT_PATH="${AGENT_BINARY}"
        MICRO_XRCE_AGENT_LIB_DIR="${AGENT_LIB_DIR}")
    add_dependencies(conformance_xrce conformance_xrce_peer microxrcedds_agent_proj)
endif()

# ── ctest wiring ────────────────────────────────────────────────────
# Per-clause ctest entries for the two Agent-free binaries, so
# `ctest -R 'ProviderConformance\.<Clause>'` scopes to one clause across every
# subject that has it — and so the list itself shows which (clause, subject)
# pairs exist.
include(GoogleTest)
gtest_discover_tests(conformance_inprocess DISCOVERY_MODE PRE_TEST)
# One lock per BINARY, not per domain: ctest properties are applied target-wide
# and conformance_fastdds carries two subjects (domains 151 and 152). It also
# spawns a peer child, so two of its clauses must never run at once.
gtest_discover_tests(conformance_fastdds DISCOVERY_MODE PRE_TEST
    PROPERTIES RESOURCE_LOCK conformance_fastdds)

if(FLETCHER_CONFORMANCE_XRCE)
    # A SINGLE ctest entry for the XRCE binary (the fastdds-xrce-interop
    # precedent): one Agent binds one UDP port, and per-clause entries would pay
    # ~24 Agent start/stop cycles for the same coverage.
    add_test(NAME conformance_xrce COMMAND conformance_xrce)
    set_tests_properties(conformance_xrce PROPERTIES RESOURCE_LOCK conformance_xrce)
endif()
