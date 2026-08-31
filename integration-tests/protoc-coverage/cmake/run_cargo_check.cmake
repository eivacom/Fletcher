# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
# GIR-1 Rust accessor compile-check wrapper (graceful skip).
#
# Invoked by CTest as `cmake -P run_cargo_check.cmake` with:
#   -DCARGO_EXECUTABLE  path to cargo (may be empty if not found)
#   -DRUSTC_EXECUTABLE  path to rustc (may be empty if not found)
#   -DGENERATED_DIR     dir containing coverage.fletcher.rs + __rba.fletcher.rs
#   -DRUST_DIR          source dir of the rust-accessor crate
#   -DCARGO_TARGET_DIR  out-of-source cargo target dir (build tree)
#
# When cargo/rustc are absent, prints the SKIP_MARKER and exits 0 so CTest
# reports Skipped (via SKIP_REGULAR_EXPRESSION) rather than Failed/Passed. When
# present, compile-checks the crate that include!s the generated Rust.
#
# NOTE: the crate is include!-based (env var FLETCHER_GENERATED_RUST_DIR points
# at the emitted .rs). It ships a committed Cargo.lock (arrow =59.0.0 tree), so
# `cargo check --locked` is deterministic per the design. `--tests` also checks
# tests/compile_check.rs (which names the generated accessors). The repo's
# authoritative Rust crate (integration-tests/protoc-gen-fletcher-rust) commits
# a lock too but runs plain `cargo test`; if --locked proves brittle in CI it
# can be relaxed without other changes.

if(NOT CARGO_EXECUTABLE OR NOT EXISTS "${CARGO_EXECUTABLE}"
   OR NOT RUSTC_EXECUTABLE OR NOT EXISTS "${RUSTC_EXECUTABLE}")
    message("SKIP_MARKER: rustc/cargo not found on PATH")
    return()
endif()

foreach(_rs coverage.fletcher.rs __rba.fletcher.rs)
    if(NOT EXISTS "${GENERATED_DIR}/${_rs}")
        message(FATAL_ERROR "generated Rust missing: ${GENERATED_DIR}/${_rs}")
    endif()
endforeach()

set(ENV{FLETCHER_GENERATED_RUST_DIR} "${GENERATED_DIR}")
if(CARGO_TARGET_DIR)
    set(ENV{CARGO_TARGET_DIR} "${CARGO_TARGET_DIR}")
endif()

execute_process(
    COMMAND "${CARGO_EXECUTABLE}" check --locked --tests --manifest-path "${RUST_DIR}/Cargo.toml"
    RESULT_VARIABLE _cargo_rc
    OUTPUT_VARIABLE _cargo_out
    ERROR_VARIABLE _cargo_err)

if(NOT _cargo_rc EQUAL 0)
    message(FATAL_ERROR
        "cargo check failed (rc=${_cargo_rc}):\n${_cargo_out}\n${_cargo_err}")
endif()
message("cargo check OK")
