// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
//! RBA-6b transitive-support-gate regression test (D-RBA-8, code-review should-fix).
//!
//! The Rust composite support gate must be TRANSITIVE: a composite field is only
//! "supported" (and its accessor generated) if the inner message's accessor will
//! ALSO be generated. Otherwise the parent emits a reference to an accessor type
//! the planner skipped -> dangling, uncompilable Rust.
//!
//! `transitive_gate.proto` defines `Unsupported` (which holds a depth-4 NESTED_LIST
//! — in-scope representable but beyond the supported depth 2/3) and three holders
//! that reference it via STRUCT / REPEATED_STRUCT / message-value MAP. With the
//! transitive gate, `Unsupported` and all three holders are SKIPPED (fail-fast
//! comments, no accessor) and the crate compiles. Without it, the holders would
//! dangle on `UnsupportedAccessor` and the crate would not compile.
//!
//! This test file COMPILING AT ALL is the primary proof (the holders are part of
//! the same crate, co-mounted by the assembler). It additionally inspects the
//! generated `transitive_gate.fletcher.rs` to confirm the skip/comment path fired
//! for `Unsupported` + the three holders, and that no accessor was emitted for any
//! of them — while the independently-supported `Pixel` message IS generated.

use std::path::PathBuf;

const OUT_DIR: &str = env!("FLETCHER_TEST_OUT_DIR");

fn generated() -> String {
    let path = PathBuf::from(OUT_DIR).join("transitive_gate.fletcher.rs");
    std::fs::read_to_string(&path)
        .unwrap_or_else(|e| panic!("read {}: {e}", path.display()))
}

#[test]
fn transitive_gate_skips_holders_of_unsupported_inner_message() {
    let src = generated();

    // The unsupported inner message and all three holders that reference it are
    // skipped via the fail-fast comment path (no accessor struct emitted).
    for skipped in ["Unsupported", "HolderStruct", "HolderRepeated", "HolderMap"] {
        let comment = format!("// fletcher: {skipped} ");
        assert!(
            src.contains(&comment),
            "expected a fail-fast skip comment for {skipped}; generated:\n{src}"
        );
        let accessor = format!("pub struct {skipped}Accessor");
        assert!(
            !src.contains(&accessor),
            "{skipped}Accessor must NOT be emitted (transitive gate); generated:\n{src}"
        );
    }

    // The independently-supported leaf message IS still generated (sanity: the gate
    // only skips messages that are actually unsupported, transitively).
    assert!(
        src.contains("pub struct PixelAccessor"),
        "PixelAccessor (supported on its own) should still be generated; generated:\n{src}"
    );
}
