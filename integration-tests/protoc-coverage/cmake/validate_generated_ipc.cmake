# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
# GIR-1 generated-.ipc completeness guard.
#
# Invoked by CMake as `cmake -P validate_generated_ipc.cmake` from the
# generation custom command with:
#   -DGENERATED_DIR       dir protoc emitted the .ipc schema streams into
#   -DEXPECTED_IPC_NAMES  '|'-joined list of every .ipc basename the harness
#                         expects protoc-gen-fletcher to emit for coverage.proto
#
# Fails loudly if the set of actually-emitted .ipc files differs from the
# expected set in EITHER direction, so adding / removing / renaming a top-level
# message in coverage.proto (which changes the emitted .ipc set) can't silently
# drift the fixture out of sync with CMakeLists.txt's declared list. The
# expected list, not a glob, is authoritative — a glob alone would happily
# accept an under-generated set as "whatever was produced".

if(NOT GENERATED_DIR OR NOT EXPECTED_IPC_NAMES)
    message(FATAL_ERROR
        "validate_generated_ipc: -DGENERATED_DIR and -DEXPECTED_IPC_NAMES are required")
endif()

string(REPLACE "|" ";" _expected "${EXPECTED_IPC_NAMES}")

file(GLOB _actual_paths "${GENERATED_DIR}/*.ipc")
set(_actual "")
foreach(_p IN LISTS _actual_paths)
    get_filename_component(_n "${_p}" NAME)
    list(APPEND _actual "${_n}")
endforeach()

list(REMOVE_DUPLICATES _expected)
list(SORT _expected)
list(SORT _actual)

if(NOT _expected STREQUAL _actual)
    set(_missing "${_expected}")
    if(_actual)
        list(REMOVE_ITEM _missing ${_actual})
    endif()
    set(_unexpected "${_actual}")
    if(_expected)
        list(REMOVE_ITEM _unexpected ${_expected})
    endif()
    message(FATAL_ERROR
        "generated .ipc set does not match the harness's expected set.\n"
        "  expected: ${_expected}\n"
        "  actual  : ${_actual}\n"
        "  missing (expected but NOT emitted)   : ${_missing}\n"
        "  unexpected (emitted but NOT expected): ${_unexpected}\n"
        "A top-level message was likely added, removed, or renamed in "
        "proto/coverage.proto. Update _ipc_all_messages (and, if the runtime "
        "test opens it, _ipc_opened_messages) in "
        "integration-tests/protoc-coverage/CMakeLists.txt to match.")
endif()

message(STATUS "generated .ipc set OK (${_actual})")
