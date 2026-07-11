# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
# GIR-1 TypeScript compile-check wrapper (graceful skip).
#
# Invoked by CTest as `cmake -P run_tsc_check.cmake` with:
#   -DTSC_EXECUTABLE  path to tsc (may be empty if not found at configure time)
#   -DGENERATED_DIR   dir containing coverage.fletcher.ts
#   -DTS_DIR          source dir of the ts/ check package
#
# When tsc is absent, prints the SKIP_MARKER and exits 0 so CTest reports the
# test as Skipped (via SKIP_REGULAR_EXPRESSION) rather than Failed/Passed. When
# tsc is present, type-checks the generated .ts (no emit, no network install).

if(NOT TSC_EXECUTABLE OR NOT EXISTS "${TSC_EXECUTABLE}")
    message("SKIP_MARKER: tsc not found on PATH")
    return()
endif()

set(_gen_ts "${GENERATED_DIR}/coverage.fletcher.ts")
if(NOT EXISTS "${_gen_ts}")
    message(FATAL_ERROR "generated TypeScript missing: ${_gen_ts}")
endif()

# Reference the generated .ts from the check package: copy it next to the
# hand-written compile_check.ts so tsconfig's include picks it up.
#
# The ABSENT-toolchain case is the ONLY graceful skip (handled above). Every
# other problem — a filesystem/permission fault staging the file — must be a
# hard FAILURE, never a Skip and never a confusing downstream tsc error. Verify
# each file operation landed: file(COPY)/file(MAKE_DIRECTORY) abort on hard
# errors, but a post-condition check also catches a partial/broken copy.
file(MAKE_DIRECTORY "${TS_DIR}/generated")
if(NOT IS_DIRECTORY "${TS_DIR}/generated")
    message(FATAL_ERROR
        "could not create staging dir ${TS_DIR}/generated "
        "(filesystem/permission error) — failing rather than skipping")
endif()
file(COPY "${_gen_ts}" DESTINATION "${TS_DIR}/generated")
get_filename_component(_gen_ts_name "${_gen_ts}" NAME)
if(NOT EXISTS "${TS_DIR}/generated/${_gen_ts_name}")
    message(FATAL_ERROR
        "failed to stage generated TypeScript into "
        "${TS_DIR}/generated/${_gen_ts_name} (broken/partial copy) — "
        "failing rather than skipping")
endif()

# GIR-9: also type-check the enum_coverage generation unit's .ts when present
# (tsconfig includes generated/**/*.ts, so a self-contained module is checked
# standalone). Same hard-fail-on-broken-copy contract as above.
set(_gen_enum_ts "${GENERATED_DIR}/enum_coverage.fletcher.ts")
if(EXISTS "${_gen_enum_ts}")
    file(COPY "${_gen_enum_ts}" DESTINATION "${TS_DIR}/generated")
    get_filename_component(_gen_enum_ts_name "${_gen_enum_ts}" NAME)
    if(NOT EXISTS "${TS_DIR}/generated/${_gen_enum_ts_name}")
        message(FATAL_ERROR
            "failed to stage generated TypeScript into "
            "${TS_DIR}/generated/${_gen_enum_ts_name} (broken/partial copy) — "
            "failing rather than skipping")
    endif()
endif()

execute_process(
    COMMAND "${TSC_EXECUTABLE}" --noEmit -p "${TS_DIR}/tsconfig.json"
    WORKING_DIRECTORY "${TS_DIR}"
    RESULT_VARIABLE _tsc_rc
    OUTPUT_VARIABLE _tsc_out
    ERROR_VARIABLE _tsc_err)

if(NOT _tsc_rc EQUAL 0)
    message(FATAL_ERROR
        "tsc --noEmit failed (rc=${_tsc_rc}):\n${_tsc_out}\n${_tsc_err}")
endif()
message("tsc --noEmit OK")
