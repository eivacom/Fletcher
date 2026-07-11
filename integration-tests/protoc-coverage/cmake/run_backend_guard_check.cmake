# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
# GIR-10 forcing test: the GenErrors.ScalarLeafNestedListRejectedBy{Accessor,Rust}
# family (locked decision #3).
#
# Invokes the real protoc + protoc-gen-fletcher plugin on a proto that carries a
# scalar-leaf nested list (List<List<scalar>>) WITH a backend that cannot
# represent it (`--fletcher_opt=accessor` or `--fletcher_opt=rust`). The GIR-10
# ValidateBackendsSupportFields() front-end guard must make the plugin FAIL
# generation (non-zero exit) with a clear error, BEFORE the read-only RBA emitter
# runs — never emit malformed accessor/Rust code. Pre-guard the plugin would emit
# invalid code (empty nested_class) and exit 0, so this script FATAL_ERRORs (test
# red) until the guard lands.
#
# Parametrised so the accessor / rust variants share one script:
#   FLETCHER_OPT   the offending opt token(s) (e.g. accessor / rust)
#   EXPECT_MESSAGE guard-error substring the failure must carry
#
# Other required -D arguments:
#   PROTOC                     $<TARGET_FILE:protobuf::protoc>
#   FLETCHER_PLUGIN            $<TARGET_FILE:fletcher-protoc::plugin>
#   PROTO_DIR                  dir containing the fixture proto
#   PROTO_FILE                 fixture basename (coverage_scalar_nested.proto)
#   OUT_DIR                    protoc --fletcher_out target (created here)
#   FLETCHER_PROTO_INCLUDE_DIR fletcher/*.proto include root
#   PROTOBUF_WKT_INCLUDE_DIR   google/protobuf/*.proto include root

foreach(_req PROTOC FLETCHER_PLUGIN PROTO_DIR PROTO_FILE OUT_DIR
             FLETCHER_PROTO_INCLUDE_DIR PROTOBUF_WKT_INCLUDE_DIR
             FLETCHER_OPT EXPECT_MESSAGE)
    if(NOT ${_req})
        message(FATAL_ERROR "run_backend_guard_check: -D${_req} is required")
    endif()
endforeach()

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

if(rc EQUAL 0)
    message(FATAL_ERROR
        "'${PROTO_FILE}' with --fletcher_opt=${FLETCHER_OPT} unexpectedly generated "
        "successfully (exit 0). The scalar-leaf nested list must be REJECTED by the "
        "backend-availability guard, not emitted as malformed accessor/Rust code.\n"
        "  stdout: ${out}\n  stderr: ${err}")
endif()

string(CONCAT combined "${out}" "\n" "${err}")

if(NOT combined MATCHES "${EXPECT_MESSAGE}")
    message(FATAL_ERROR
        "generation failed (good) but the error did not carry the expected guard "
        "message '${EXPECT_MESSAGE}':\n${combined}")
endif()

message(STATUS
    "'${PROTO_FILE}' with --fletcher_opt=${FLETCHER_OPT} correctly rejected: ${combined}")
