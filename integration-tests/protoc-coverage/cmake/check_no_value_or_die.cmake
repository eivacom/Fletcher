# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
# GIR-8 forcing test: GenErrors.NoValueOrDieInIrGeneratedCode.
#
# Asserts ZERO `.ValueOrDie()` occurrences in the IR-emitted generated code,
# EXCLUDING RBA-generated output (RBA is read-only and reconciled separately in
# round RIR). Emitter code must instead route Arrow Result<T> unwrapping through
# the generated `detail::FletcherValueOrThrow()` helper, which throws a
# descriptive std::runtime_error on failure instead of aborting via .ValueOrDie().
#
# IR-emitted files (scanned):
#   *.fletcher.pb.h        edge nanoarrow header (view-class GetScalar reads)
#   *.fletcher.arrow.pb.h  Arrow view header (view ctors + ToArrowRow builders)
#   *.fletcher.ts          TypeScript
#
# Excluded:
#   *.fletcher.accessor.pb.h  RBA C++ accessor header
#   *.fletcher.rs             RBA Rust accessor module
#   __rba.fletcher.rs         shared RBA Rust helper module
#   *.ipc                     binary Arrow IPC schema streams
#
# Required -D arguments:
#   GENERATED_DIR  directory protoc emitted the generated files into

if(NOT GENERATED_DIR)
    message(FATAL_ERROR "check_no_value_or_die: -DGENERATED_DIR is required")
endif()
if(NOT IS_DIRECTORY "${GENERATED_DIR}")
    message(FATAL_ERROR "check_no_value_or_die: GENERATED_DIR is not a directory: ${GENERATED_DIR}")
endif()

# Recurse so this stays correct if the suite ever nests per-harness subdirs.
file(GLOB_RECURSE _all_files "${GENERATED_DIR}/*")

set(_scanned "")
set(_offenders "")

foreach(_f IN LISTS _all_files)
    if(IS_DIRECTORY "${_f}")
        continue()
    endif()

    # Exclude RBA-generated outputs and binary IPC streams.
    if(_f MATCHES "\\.fletcher\\.accessor\\.pb\\.h$"
       OR _f MATCHES "\\.fletcher\\.rs$"
       OR _f MATCHES "__rba\\.fletcher\\.rs$"
       OR _f MATCHES "\\.ipc$")
        continue()
    endif()

    # Only scan IR-emitted generated source surfaces.
    if(NOT (_f MATCHES "\\.fletcher\\.pb\\.h$"
            OR _f MATCHES "\\.fletcher\\.arrow\\.pb\\.h$"
            OR _f MATCHES "\\.fletcher\\.ts$"))
        continue()
    endif()

    list(APPEND _scanned "${_f}")
    file(STRINGS "${_f}" _hits REGEX "ValueOrDie")
    if(_hits)
        list(LENGTH _hits _n)
        list(APPEND _offenders "${_f} (${_n} occurrence(s))")
    endif()
endforeach()

list(LENGTH _scanned _num_scanned)
if(_num_scanned EQUAL 0)
    message(FATAL_ERROR
        "check_no_value_or_die: no IR-emitted generated files found under "
        "${GENERATED_DIR}; generation may not have run.")
endif()

if(_offenders)
    string(REPLACE ";" "\n  " _pretty "${_offenders}")
    message(FATAL_ERROR
        "found .ValueOrDie() in IR-emitted generated code (RBA excluded). "
        "Emitters must use detail::FletcherValueOrThrow() instead:\n  ${_pretty}")
endif()

string(REPLACE ";" "\n  " _pretty_scanned "${_scanned}")
message(STATUS "no ValueOrDie() in ${_num_scanned} IR-emitted file(s):\n  ${_pretty_scanned}")
