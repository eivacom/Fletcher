# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
# GIR-8 forcing test: the GenErrors.*UnsupportedTypeFailsBuild family.
#
# Invokes the real protoc + protoc-gen-fletcher plugin on a negative fixture
# proto (a genuinely-unsupported type). The ValidateNoUnsupportedIr() front-end
# pass must make the plugin FAIL generation (non-zero exit) with a descriptive
# error that names the offending field and carries the UnsupportedNode reason.
# Today (pre-GIR-8) the plugin silently skips the field and exits 0, so this
# script FATAL_ERRORs (test red) until GIR-8 lands.
#
# Parametrised so the singular / repeated / map google.protobuf.Any variants
# (review 4b) share one script:
#   PROTO_FILE     fixture basename under PROTO_DIR (e.g. coverage_unsupported.proto)
#   EXPECT_FIELD   the offending field's proto name the error must mention
#   EXPECT_REASON  the UnsupportedNode reason substring the error must carry
#
# Other required -D arguments:
#   PROTOC                     $<TARGET_FILE:protobuf::protoc>
#   FLETCHER_PLUGIN            $<TARGET_FILE:fletcher-protoc::plugin>
#   PROTO_DIR                  dir containing the fixture proto
#   OUT_DIR                    protoc --fletcher_out target (created here)
#   FLETCHER_PROTO_INCLUDE_DIR fletcher/*.proto include root
#   PROTOBUF_WKT_INCLUDE_DIR   google/protobuf/*.proto (any.proto) include root

foreach(_req PROTOC FLETCHER_PLUGIN PROTO_DIR OUT_DIR
             FLETCHER_PROTO_INCLUDE_DIR PROTOBUF_WKT_INCLUDE_DIR
             PROTO_FILE EXPECT_FIELD EXPECT_REASON)
    if(NOT ${_req})
        message(FATAL_ERROR "run_unsupported_generation_check: -D${_req} is required")
    endif()
endforeach()

# protoc requires the output directory to exist before it will write into it.
file(MAKE_DIRECTORY "${OUT_DIR}")

execute_process(
    COMMAND "${PROTOC}"
        "--plugin=protoc-gen-fletcher=${FLETCHER_PLUGIN}"
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
        "unsupported fixture '${PROTO_FILE}' unexpectedly generated successfully "
        "(exit 0). protoc-gen-fletcher must FAIL on the unsupported type instead "
        "of silently skipping the field.\n  stdout: ${out}\n  stderr: ${err}")
endif()

string(CONCAT combined "${out}" "\n" "${err}")

if(NOT combined MATCHES "${EXPECT_FIELD}")
    message(FATAL_ERROR
        "error did not name the unsupported field '${EXPECT_FIELD}': ${combined}")
endif()
if(NOT combined MATCHES "${EXPECT_REASON}")
    message(FATAL_ERROR
        "error did not include the UnsupportedNode reason '${EXPECT_REASON}': ${combined}")
endif()

message(STATUS "'${PROTO_FILE}' correctly failed generation: ${combined}")
