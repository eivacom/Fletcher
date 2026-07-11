# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
# GIR-8 (#53) full-surface invariant guard: NO `.ValueOrDie(` in the IR code
# emitters.
#
# This is the harness-independent companion to protoc-coverage's
# GenErrors.NoValueOrDieInIrGeneratedCode. That test greps the generated output
# of ONE harness (coverage.proto). This test instead greps the EMITTER SOURCES —
# the code that writes every generated file — so the invariant "generated
# IR-emitted code contains no unchecked `.ValueOrDie()` unwrap" is guaranteed for
# EVERY downstream harness at once, regardless of which proto shapes each
# exercises. A future emitter regression that reintroduces `.ValueOrDie(` into an
# emitted string is caught here even if coverage.proto never hits that path.
#
# Emitters must instead emit detail::FletcherValueOrThrow(...), which throws a
# descriptive std::runtime_error on failure instead of aborting.
#
# The RBA emitter (recordbatch_accessor_emitter.*) is EXCLUDED: RBA is read-only
# and reconciled separately in round RIR (it emits zero .ValueOrDie() today).
#
# Required -D arguments:
#   EMITTER_SRC_DIR  the protoc/src directory (emitter translation units)

if(NOT EMITTER_SRC_DIR)
    message(FATAL_ERROR "check_no_value_or_die_in_emitters: -DEMITTER_SRC_DIR is required")
endif()
if(NOT IS_DIRECTORY "${EMITTER_SRC_DIR}")
    message(FATAL_ERROR
        "check_no_value_or_die_in_emitters: EMITTER_SRC_DIR is not a directory: ${EMITTER_SRC_DIR}")
endif()

file(GLOB _emitter_files
    "${EMITTER_SRC_DIR}/*.cpp"
    "${EMITTER_SRC_DIR}/*.hpp")

set(_scanned "")
set(_offenders "")

foreach(_f IN LISTS _emitter_files)
    # Exclude the read-only RBA emitter (reconciled in round RIR).
    if(_f MATCHES "recordbatch_accessor_emitter\\.")
        continue()
    endif()

    list(APPEND _scanned "${_f}")
    # Match the call token `.ValueOrDie(` — whether in an emitted string literal
    # or anywhere else in the emitter source. Emitters must route Arrow Result<T>
    # unwrapping through detail::FletcherValueOrThrow() instead.
    file(STRINGS "${_f}" _hits REGEX "\\.ValueOrDie\\(")
    if(_hits)
        list(LENGTH _hits _n)
        list(APPEND _offenders "${_f} (${_n} line(s))")
    endif()
endforeach()

list(LENGTH _scanned _num_scanned)
if(_num_scanned EQUAL 0)
    message(FATAL_ERROR
        "check_no_value_or_die_in_emitters: no emitter sources found under "
        "${EMITTER_SRC_DIR}.")
endif()

if(_offenders)
    string(REPLACE ";" "\n  " _pretty "${_offenders}")
    message(FATAL_ERROR
        "found .ValueOrDie( in IR emitter sources (RBA excluded). Emitters must "
        "emit detail::FletcherValueOrThrow() instead:\n  ${_pretty}")
endif()

message(STATUS "no .ValueOrDie( in ${_num_scanned} IR emitter source(s)")
