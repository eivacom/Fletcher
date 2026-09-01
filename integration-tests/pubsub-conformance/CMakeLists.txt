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
# configure concurrently) but on Windows the SAME install dir as
# integration-tests/fastdds-xrce-interop — that install is what the CI cache
# buys, so the ~10-15 min superbuild happens once per machine, not once per
# harness. On Linux the default install dir is inside the build tree, which
# nothing persists; the CI lane therefore points it at a cacheable workspace path
# via FLETCHER_AGENT_INSTALL_DIR (below), since `conan build .` takes no -D.
# See that harness's CMakeLists for why the paths are short on Windows and why
# UAGENT_USE_SYSTEM_FASTDDS is on.
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
        # Default is inside the build tree, which nothing caches. The CI lane
        # drives this harness through `conan build .` and so cannot pass -D, so
        # the install dir is also settable from the environment: the Linux job
        # points it at a workspace path it caches, and the ~10-15 min superbuild
        # then happens once there too rather than on every run.
        if(DEFINED ENV{FLETCHER_AGENT_INSTALL_DIR})
            set(_agent_install_default "$ENV{FLETCHER_AGENT_INSTALL_DIR}")
            message(STATUS
                "MicroXRCEAgent install dir taken from FLETCHER_AGENT_INSTALL_DIR: "
                "${_agent_install_default}")
        else()
            set(_agent_install_default "${CMAKE_BINARY_DIR}/microxrcedds_agent-install")
        endif()
        set(AGENT_INSTALL_DIR "${_agent_install_default}" CACHE PATH
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
    src/child_process.cpp)
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
# The peer children's request/reply loop, linked ONLY into the peer binaries —
# it has no business in a subject binary.
add_library(conformance_peer_loop OBJECT src/peer_main.cpp)
target_link_libraries(conformance_peer_loop PUBLIC conformance_support)

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
# Keeps gtest_main: it spawns no child and opens no pipe, so it has no SIGPIPE
# hazard to disarm.
add_executable(conformance_inprocess subjects/inprocess_main.cpp)
target_link_libraries(conformance_inprocess PRIVATE
    conformance_support
    conformance_clauses
    GTest::gtest_main)

# The SAME loopback, constructed schema-CARRYING — the sixth subject. Its own
# binary, because clause 2's axis gate is the link line and INSTANTIATE_TEST_SUITE_P
# registers every clause in a binary against every subject in it; see
# subjects/inprocess_carrying_main.cpp.
add_executable(conformance_inprocess_carrying subjects/inprocess_carrying_main.cpp)
target_link_libraries(conformance_inprocess_carrying PRIVATE
    conformance_support
    conformance_clauses
    conformance_clauses_carried
    GTest::gtest_main)

# ── The copy-accounting oracle ──────────────────────────────────────
# A SECOND suite in this harness, not a second harness: it inherits the CI lane,
# the format/licence gates and the subject-parameterised shape a later ABI round
# registers a driver-backed subject into.
#
# Its own binary, and it links NO provider SDK — every subject is in-process by
# construction (address provenance means nothing across an address space), so it
# runs in milliseconds and no transport can slow or flake it.
#
# Keeps gtest_main: it spawns no child and opens no pipe, so it has no SIGPIPE
# hazard to disarm.
add_executable(conformance_copy_accounting
    src/copy_accounting.cpp
    src/copy_clauses.cpp)
target_link_libraries(conformance_copy_accounting PRIVATE
    conformance_support
    GTest::gtest_main)

# ── The seam-vocabulary suite ───────────────────────────────────────
# A THIRD suite in this harness, for the same reasons the copy oracle is a
# second one. It asserts what the crossing types make representable — ownership,
# schema arrival, the status taxonomy — which no provider-parameterised clause
# can reach: `InProcessLocal` is subject-axis kAbsent, so the loopback is never
# handed a real schema by CONF_MUST_DECLARE, and only the gateway exercises that
# path today (review debt C2-1).
#
# It links the copy oracle's instrument, not a second one, so there is exactly
# one scoring path in this harness.
#
# Keeps gtest_main: it spawns no child and opens no pipe.
add_executable(conformance_seam_vocabulary
    src/seam_vocabulary.cpp
    src/copy_accounting.cpp)
target_link_libraries(conformance_seam_vocabulary PRIVATE
    conformance_support
    GTest::gtest_main)

add_executable(conformance_fastdds_peer subjects/fastdds_peer_main.cpp)
target_link_libraries(conformance_fastdds_peer PRIVATE
    conformance_support
    conformance_peer_loop
    fletcher-fastdds-pubsub-provider::fletcher-fastdds-pubsub-provider)

add_executable(conformance_fastdds subjects/fastdds_main.cpp)
target_link_libraries(conformance_fastdds PRIVATE
    conformance_support
    conformance_clauses
    conformance_clauses_carried
    fletcher-fastdds-pubsub-provider::fletcher-fastdds-pubsub-provider
    # gtest, NOT gtest_main: this binary spawns a peer child, so it defines its
    # own main to set SIGPIPE's disposition before any thread exists.
    GTest::gtest)
target_compile_definitions(conformance_fastdds PRIVATE
    CONFORMANCE_FASTDDS_PEER="$<TARGET_FILE:conformance_fastdds_peer>")
add_dependencies(conformance_fastdds conformance_fastdds_peer)

if(FLETCHER_CONFORMANCE_XRCE)
    add_executable(conformance_xrce_peer subjects/xrce_peer_main.cpp)
    target_link_libraries(conformance_xrce_peer PRIVATE
        conformance_support
        conformance_peer_loop
        fletcher-xrcedds-pubsub-provider::fletcher-xrcedds-pubsub-provider)

    add_executable(conformance_xrce subjects/xrce_main.cpp)
    target_link_libraries(conformance_xrce PRIVATE
        conformance_support
        conformance_clauses
        conformance_clauses_carried
        fletcher-xrcedds-pubsub-provider::fletcher-xrcedds-pubsub-provider
        # gtest, NOT gtest_main - see conformance_fastdds above.
        GTest::gtest)
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
# TIMEOUTs are stated rather than left to ctest's 1500 s default: a clause's own
# budget is 20 s plus setup, and a peer spawn is bounded, so anything past a few
# minutes is a hang and should be reported as one.
include(GoogleTest)
gtest_discover_tests(conformance_inprocess DISCOVERY_MODE PRE_TEST
    PROPERTIES TIMEOUT 120)
gtest_discover_tests(conformance_inprocess_carrying DISCOVERY_MODE PRE_TEST
    PROPERTIES TIMEOUT 120)
# No RESOURCE_LOCK: no domain, no port, no child, no shared state — the copy
# oracle is safe to run fully parallel with everything else here. 60 s is two
# orders of magnitude above what it costs; anything near it is a hang.
gtest_discover_tests(conformance_copy_accounting DISCOVERY_MODE PRE_TEST
    PROPERTIES TIMEOUT 60)
# Same shape, same reasoning: no domain, no port, no child.
gtest_discover_tests(conformance_seam_vocabulary DISCOVERY_MODE PRE_TEST
    PROPERTIES TIMEOUT 60)
# One lock per BINARY, not per domain: ctest properties are applied target-wide
# and conformance_fastdds carries two subjects (domains 151 and 152). It also
# spawns a peer child, so two of its clauses must never run at once.
gtest_discover_tests(conformance_fastdds DISCOVERY_MODE PRE_TEST
    PROPERTIES RESOURCE_LOCK conformance_fastdds TIMEOUT 180)

if(FLETCHER_CONFORMANCE_XRCE)
    # A SINGLE ctest entry for the XRCE binary (the fastdds-xrce-interop
    # precedent): one Agent binds one UDP port, and per-clause entries would pay
    # ~24 Agent start/stop cycles for the same coverage.
    add_test(NAME conformance_xrce COMMAND conformance_xrce)
    # 24 clauses, ~26 peer spawns and one Agent handshake per session, in one
    # process.
    set_tests_properties(conformance_xrce PROPERTIES
        RESOURCE_LOCK conformance_xrce
        TIMEOUT 900)
endif()
