// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
//! GIR-1 Rust accessor compile-check crate.
//!
//! It has no hand-written source beyond these mount points: the generated
//! bare-item RBA files are `include!`-mounted from the directory the CTest
//! wrapper (cmake/run_cargo_check.cmake) exports as `FLETCHER_GENERATED_RUST_DIR`.
//! Compiling this crate (`cargo check`) is the well-formedness check for the
//! generated `--fletcher_opt=rust` output of coverage.proto.
//!
//! The generated accessors reference the shared span/Row helpers by the
//! absolute path `crate::fletcher_gen::__rba::*`, so `__rba.fletcher.rs` is
//! mounted at exactly that path, above the (bare-item) accessor file. The
//! accessors themselves carry no `mod <pkg>` wrapper, so they are mounted
//! directly under `fletcher_gen` and reached as
//! `crate::fletcher_gen::<Class>Accessor`.
//!
//! GIR-1 is read-only w.r.t. RBA: this crate compiles the generated accessor,
//! it does not reshape it.

pub mod fletcher_gen {
    pub mod __rba {
        include!(concat!(env!("FLETCHER_GENERATED_RUST_DIR"), "/__rba.fletcher.rs"));
    }

    include!(concat!(env!("FLETCHER_GENERATED_RUST_DIR"), "/coverage.fletcher.rs"));
}
