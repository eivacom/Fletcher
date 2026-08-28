# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
# GIR-10 forcing test: the GenErrors.ScalarLeafNestedListRejectedBy{Accessor,Rust}
# family (locked decision #3). DICT-1.5 extends this script (additively) to
# also assert the OTHER direction of the same guard — a proto that must be
# ACCEPTED (exit 0, with the expected artifacts actually emitted) — so both
# halves of ValidateBackendsSupportFields' contract share one protoc-invocation
# shape instead of a near-duplicate script.
#
# Invokes the real protoc + protoc-gen-fletcher plugin on a fixture proto WITH a
# backend that cannot yet represent some shape the fixture carries
# (`--fletcher_opt=accessor` or `--fletcher_opt=rust`). The
# ValidateBackendsSupportFields() front-end guard must make the plugin FAIL
# generation (non-zero exit) with a clear error, BEFORE the read-only RBA emitter
# runs — never emit malformed accessor/Rust code. Pre-guard the plugin would emit
# invalid code and exit 0, so this script FATAL_ERRORs (test red) until the
# relevant guard predicate lands.
#
# Parametrised so the accessor / rust / shape variants share one script:
#   FLETCHER_OPT    the opt token(s) under test (e.g. accessor / rust / ipc,ts)
#   EXPECT_MESSAGE  guard-error substring the failure must carry (required
#                   unless EXPECT_SUCCESS is set)
#   EXPECT_FIELD    optional: substring (typically "Message.field") the
#                   combined stdout+stderr must also match, asserting the error
#                   NAMES the offending field
#   EXPECT_SUCCESS  optional: when truthy, invert the verdict — require exit 0
#                   and skip EXPECT_MESSAGE/EXPECT_FIELD entirely
#   EXPECT_ARTIFACTS  required when EXPECT_SUCCESS is set: a `|`-joined list of
#                   basenames relative to OUT_DIR, each of which must EXIST and
#                   be non-empty. This is what makes the success mode
#                   non-vacuous: `rc EQUAL 0` alone would pass just as happily
#                   if the plugin emitted nothing at all.
#
# Other required -D arguments:
#   PROTOC                     $<TARGET_FILE:protobuf::protoc>
#   FLETCHER_PLUGIN            $<TARGET_FILE:fletcher-protoc::plugin>
#   PROTO_DIR                  dir containing the fixture proto
#   PROTO_FILE                 fixture basename (coverage_scalar_nested.proto)
#   OUT_DIR                    protoc --fletcher_out target (created here). MUST
#                              be unique PER TEST (e.g. a per-test subdirectory), not
#                              shared with any other add_test using this script: the
#                              REMOVE_RECURSE below races under `ctest -j` if two
#                              tests share an OUT_DIR (step-4b should-fix S3).
#   FLETCHER_PROTO_INCLUDE_DIR fletcher/*.proto include root
#   PROTOBUF_WKT_INCLUDE_DIR   google/protobuf/*.proto include root

foreach(_req PROTOC FLETCHER_PLUGIN PROTO_DIR PROTO_FILE OUT_DIR
             FLETCHER_PROTO_INCLUDE_DIR PROTOBUF_WKT_INCLUDE_DIR
             FLETCHER_OPT)
    if(NOT ${_req})
        message(FATAL_ERROR "run_backend_guard_check: -D${_req} is required")
    endif()
endforeach()

if(EXPECT_SUCCESS)
    if(NOT DEFINED EXPECT_ARTIFACTS OR EXPECT_ARTIFACTS STREQUAL "")
        message(FATAL_ERROR
            "run_backend_guard_check: -DEXPECT_ARTIFACTS is required when "
            "EXPECT_SUCCESS is set (rc EQUAL 0 alone cannot detect a plugin that "
            "emitted nothing)")
    endif()
else()
    if(NOT DEFINED EXPECT_MESSAGE OR EXPECT_MESSAGE STREQUAL "")
        message(FATAL_ERROR
            "run_backend_guard_check: -DEXPECT_MESSAGE is required unless "
            "EXPECT_SUCCESS is set")
    endif()
endif()

# S2 fix (step-4b should-fix): without cleaning OUT_DIR first, a stale
# artifact left behind by a PREVIOUS run (e.g. a prior green EXPECT_SUCCESS
# pass) would satisfy the EXISTS + non-empty checks below even if the plugin
# regressed to "exit 0, emit nothing" on THIS run -- exactly the vacuity this
# script exists to close. Always start from an empty OUT_DIR.
#
# S3 fix (step-4b should-fix, false-red only): this REMOVE_RECURSE is why
# OUT_DIR must be unique PER TEST (see the docblock above) -- if two add_test
# invocations shared an OUT_DIR, running them under `ctest -j` could have one
# test's REMOVE_RECURSE/MAKE_DIRECTORY wipe a sibling's artifacts mid-check,
# false-redding the sibling. Every add_test call site in CMakeLists.txt gives
# this script its own OUT_DIR subdirectory.
file(REMOVE_RECURSE "${OUT_DIR}")
file(MAKE_DIRECTORY "${OUT_DIR}")

execute_process(
    COMMAND "${PROTOC}"
        "--plugin=protoc-gen-fletcher=${FLETCHER_PLUGIN}"
        "--fletcher_opt=${FLETCHER_OPT}"
        "--fletcher_out=${OUT_DIR}"
        "-I" "${PROTO_DIR}"
        "-I" "${FLETCHER_PROTO_INCLUDE_DIR}"
        "-I" "${PROTOBUF_WKT_INCLUDE_DIR}"
        "${PROTO_DIR}/${PROTO_FILE}"
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err)

string(CONCAT combined "${out}" "\n" "${err}")

if(EXPECT_SUCCESS)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR
            "'${PROTO_FILE}' with --fletcher_opt=${FLETCHER_OPT} unexpectedly "
            "FAILED generation (expected exit 0, a no-false-positive case).\n"
            "  stdout: ${out}\n  stderr: ${err}")
    endif()

    # EXPECT_ARTIFACTS is a `|`-joined list of basenames relative to OUT_DIR
    # (not CMake's native `;` separator, so a caller cannot accidentally hand a
    # pre-split list here). Each must exist AND be non-empty: exit 0 alone
    # passes just as happily if the plugin emitted nothing.
    string(REPLACE "|" ";" _expect_artifacts_list "${EXPECT_ARTIFACTS}")
    foreach(_artifact ${_expect_artifacts_list})
        set(_artifact_path "${OUT_DIR}/${_artifact}")
        if(NOT EXISTS "${_artifact_path}")
            message(FATAL_ERROR
                "'${PROTO_FILE}' with --fletcher_opt=${FLETCHER_OPT} exited 0 but "
                "expected artifact '${_artifact_path}' does not exist — the plugin "
                "must still generate everything else normally.\n"
                "  stdout: ${out}\n  stderr: ${err}")
        endif()
        file(SIZE "${_artifact_path}" _artifact_size)
        if(_artifact_size EQUAL 0)
            message(FATAL_ERROR
                "'${PROTO_FILE}' with --fletcher_opt=${FLETCHER_OPT} exited 0 but "
                "expected artifact '${_artifact_path}' is empty.\n"
                "  stdout: ${out}\n  stderr: ${err}")
        endif()
    endforeach()

    message(STATUS
        "'${PROTO_FILE}' with --fletcher_opt=${FLETCHER_OPT} correctly succeeded "
        "with all expected artifacts present: ${EXPECT_ARTIFACTS}")
    return()
endif()

if(rc EQUAL 0)
    message(FATAL_ERROR
        "'${PROTO_FILE}' with --fletcher_opt=${FLETCHER_OPT} unexpectedly generated "
        "successfully (exit 0). The offending shape must be REJECTED by the "
        "backend-availability guard, not emitted as malformed accessor/Rust code.\n"
        "  stdout: ${out}\n  stderr: ${err}")
endif()

if(NOT combined MATCHES "${EXPECT_MESSAGE}")
    message(FATAL_ERROR
        "generation failed (good) but the error did not carry the expected guard "
        "message '${EXPECT_MESSAGE}':\n${combined}")
endif()

if(DEFINED EXPECT_FIELD AND NOT EXPECT_FIELD STREQUAL "")
    if(NOT combined MATCHES "${EXPECT_FIELD}")
        message(FATAL_ERROR
            "generation failed (good) but the error did not name the expected "
            "field '${EXPECT_FIELD}':\n${combined}")
    endif()
endif()

message(STATUS
    "'${PROTO_FILE}' with --fletcher_opt=${FLETCHER_OPT} correctly rejected: ${combined}")
