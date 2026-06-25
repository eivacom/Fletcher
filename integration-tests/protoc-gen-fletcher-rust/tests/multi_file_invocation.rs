// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
//! RBA-6a code-review P1 regression test: a SINGLE `protoc` invocation passed
//! MULTIPLE `.proto` files under `--fletcher_opt=rust` must emit the shared
//! `__rba.fletcher.rs` helper module EXACTLY ONCE (not once per input file).
//!
//! Before the fix, `__rba.fletcher.rs` was opened from `Generate()` (run once per
//! input file), so `protoc a.proto b.proto …` opened it N times and protoc
//! rejected the duplicate output filename. After the fix it is emitted once from
//! the all-files `GenerateAll()` hook. This test shells out to the same protoc +
//! plugin the build used (paths exported by build.rs) and asserts:
//!   - the multi-file invocation SUCCEEDS, and
//!   - exactly one `__rba.fletcher.rs` is produced, and
//!   - each input file still gets its own `<stem>.fletcher.rs`.

use std::path::PathBuf;
use std::process::Command;

const PROTOC: &str = env!("FLETCHER_TEST_PROTOC");
const PLUGIN: &str = env!("FLETCHER_TEST_PLUGIN");
const PROTO_DIR: &str = env!("FLETCHER_TEST_PROTO_DIR");
const WKT_INCLUDE: &str = env!("FLETCHER_TEST_WKT_INCLUDE");

#[test]
fn multi_file_invocation_emits_single_rba_helper() {
    let proto_dir = PathBuf::from(PROTO_DIR);

    // A unique scratch output dir under the crate's target dir.
    let out_dir = PathBuf::from(env!("CARGO_TARGET_TMPDIR")).join("rba_multi_file_out");
    let _ = std::fs::remove_dir_all(&out_dir);
    std::fs::create_dir_all(&out_dir).expect("create out dir");

    // Two import-free standalone fixtures in ONE invocation. Both define composite
    // accessors; neither imports WKT, so the invocation is hermetic.
    let a = proto_dir.join("geo_child.proto");
    let b = proto_dir.join("nopkg_child.proto");

    let mut cmd = Command::new(PROTOC);
    cmd.arg(format!("--plugin=protoc-gen-fletcher={PLUGIN}"))
        .arg("--fletcher_opt=rust")
        .arg(format!("--fletcher_out={}", out_dir.display()))
        .arg("-I")
        .arg(&proto_dir);
    if !WKT_INCLUDE.is_empty() {
        cmd.arg("-I").arg(WKT_INCLUDE);
    }
    cmd.arg(&a).arg(&b);

    let output = cmd.output().expect("failed to launch protoc");
    assert!(
        output.status.success(),
        "multi-file protoc invocation must succeed (no duplicate __rba filename):\n\
         --- stdout ---\n{}\n--- stderr ---\n{}",
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr),
    );

    // Exactly one __rba.fletcher.rs, plus one per-file accessor module each.
    assert!(
        out_dir.join("__rba.fletcher.rs").is_file(),
        "the single shared __rba.fletcher.rs must be emitted"
    );
    assert!(
        out_dir.join("geo_child.fletcher.rs").is_file(),
        "geo_child.fletcher.rs must be emitted"
    );
    assert!(
        out_dir.join("nopkg_child.fletcher.rs").is_file(),
        "nopkg_child.fletcher.rs must be emitted"
    );

    // Count emitted __rba files (protoc writes to a flat OUT_DIR; there can be at
    // most one file with this fixed name — assert the directory holds exactly one).
    let rba_count = std::fs::read_dir(&out_dir)
        .expect("read out dir")
        .filter_map(Result::ok)
        .filter(|e| e.file_name() == std::ffi::OsStr::new("__rba.fletcher.rs"))
        .count();
    assert_eq!(rba_count, 1, "exactly one __rba.fletcher.rs expected");
}
