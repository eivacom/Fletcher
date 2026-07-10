// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// GIR-1 Rust compile-check: assert the generated coverage accessors are public
// and nameable at their expected module path. This does not exercise runtime
// behavior (no Arrow fixture) — GIR-1 only compile-checks the generated Rust.
// GIR-2+ may add value-level Rust reads on top of this fixture.

use fletcher_coverage_rust_check::fletcher_gen::{CompositeCoverageAccessor, ScalarCoverageAccessor};

fn assert_type<T>() {}

#[test]
fn generated_accessor_types_are_nameable() {
    assert_type::<CompositeCoverageAccessor>();
    assert_type::<ScalarCoverageAccessor>();
}
