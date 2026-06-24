// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
//! protoc-gen-fletcher-rust — RBA-5 integration crate.
//!
//! This crate has no hand-written source of its own beyond this mount point: it
//! runs the locally-built `protoc-gen-fletcher` plugin at build time
//! (`--fletcher_opt=rust`), and the `build.rs` assembler stitches the generated
//! bare-item `.fletcher.rs` files into the `fletcher_gen` module tree, grouped
//! by proto package (D-RBA-10). The aggregator is `include!`-mounted here.
//!
//! Accessors are reachable as
//! `crate::fletcher_gen::<pkg-path>::<Class>Accessor`, e.g.
//! `crate::fletcher_gen::fletcher::rba::telem::TelemetryAccessor`.

include!(concat!(env!("OUT_DIR"), "/fletcher_gen.rs"));
