# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
# #53 invariant guard for the arrow-bridge RUNTIME sources.
#
# Companion to protoc/tests/check_no_value_or_die_in_emitters.cmake. That guard
# scans the IR EMITTER translation units, so it proves the invariant for every
# line of GENERATED code. It is structurally blind to hand-written runtime code,
# and that gap was real: GIR-10 added ArrowNestedScalarList/ArrowNestedScalarList2
# to arrow_row_view.hpp using raw `.ValueOrDie()`, in the same round GIR-8 had
# just removed every `.ValueOrDie()` from the emitters — every sibling accessor in
# that header already routed through detail::ValueOrThrow(). Caught in review of
# PR #125, not by a test. This closes that surface.
#
# `.ValueOrDie()` calls abort() on a failed Arrow Result, which is exactly the
# #53 class of failure (H-INV-2: recoverable errors throw, they never abort).
# Runtime code must use detail::ValueOrThrow(), which throws
# std::invalid_argument naming the operation.
#
# EXCLUDED: detail/arrow_result.hpp — it DEFINES ValueOrThrow, and its doc
# comment legitimately names the `.ValueOrDie()` it exists to replace. Excluding
# the definition site is narrower than weakening the regex to skip comments.
#
# Required -D arguments:
#   RUNTIME_INCLUDE_DIR  arrow-bridge/include (public headers)
#   RUNTIME_SRC_DIR      arrow-bridge/src     (implementation)

if(NOT RUNTIME_INCLUDE_DIR OR NOT RUNTIME_SRC_DIR)
    message(FATAL_ERROR
        "check_no_value_or_die_in_runtime: -DRUNTIME_INCLUDE_DIR and "
        "-DRUNTIME_SRC_DIR are required")
endif()
foreach(_d "${RUNTIME_INCLUDE_DIR}" "${RUNTIME_SRC_DIR}")
    if(NOT IS_DIRECTORY "${_d}")
        message(FATAL_ERROR
            "check_no_value_or_die_in_runtime: not a directory: ${_d}")
    endif()
endforeach()

file(GLOB_RECURSE _runtime_files
    "${RUNTIME_INCLUDE_DIR}/*.hpp"
    "${RUNTIME_INCLUDE_DIR}/*.h"
    "${RUNTIME_SRC_DIR}/*.cpp"
    "${RUNTIME_SRC_DIR}/*.hpp")

set(_scanned "")
set(_offenders "")

foreach(_f IN LISTS _runtime_files)
    # Vendored nanoarrow/flatcc is not ours to police.
    if(_f MATCHES "/third_party/")
        continue()
    endif()
    # The definition site of the replacement helper (see header comment).
    if(_f MATCHES "detail/arrow_result\\.hpp$")
        continue()
    endif()

    list(APPEND _scanned "${_f}")
    file(STRINGS "${_f}" _hits REGEX "\\.ValueOrDie\\(")
    if(_hits)
        list(LENGTH _hits _n)
        list(APPEND _offenders "${_f} (${_n} line(s))")
    endif()
endforeach()

list(LENGTH _scanned _num_scanned)
if(_num_scanned EQUAL 0)
    message(FATAL_ERROR
        "check_no_value_or_die_in_runtime: no runtime sources found under "
        "${RUNTIME_INCLUDE_DIR} / ${RUNTIME_SRC_DIR}.")
endif()

if(_offenders)
    string(REPLACE ";" "\n  " _pretty "${_offenders}")
    message(FATAL_ERROR
        "found .ValueOrDie( in arrow-bridge runtime sources. Use "
        "detail::ValueOrThrow() instead (#53 / H-INV-2):\n  ${_pretty}")
endif()

message(STATUS "no .ValueOrDie( in ${_num_scanned} arrow-bridge runtime source(s)")
