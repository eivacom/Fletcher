// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#pragma once

#include <google/protobuf/descriptor.h>

#include <string>

namespace fletcher {

// The RecordBatch accessor emitters produce real accessor types (not empty
// skeletons): the column-oriented C++ <Class>Accessor classes and their Rust
// counterparts.
//
// The callers in ArrowRowGenerator::Generate run them UNCONDITIONALLY whenever
// the matching --fletcher_opt token is set (never content-gated). The `accessor`
// token emits one C++ header per proto invocation; the `rust` token emits the
// per-file <stem>.fletcher.rs PLUS a shared, once-per-invocation
// __rba.fletcher.rs span/Row helper module (RBA-6a). That is what makes the
// no-drift forcing test's "+3 files under accessor,rust" assertion hold for
// every fixture.

// Emits the C++ accessor header (<stem>.fletcher.accessor.pb.h): the
// generated-file banner, #pragma once, and the real column-oriented
// <Class>Accessor class for each message in fletcher_gen::<package>.
std::string EmitAccessorHeader(const google::protobuf::FileDescriptor* file);

// Emits the Rust accessor (<stem>.fletcher.rs): the real Rust accessor types as
// bare items (no `mod <pkg>` wrapper) — package mounting is owned by the RBA-5
// assembler (D-RBA-10).
std::string EmitRustAccessor(const google::protobuf::FileDescriptor* file);

// Emits the shared `__rba` helper module text (RBA-6): the span / Row helper
// types (ScalarSpan, StructSpan, the RowAccess trait) that the generated Rust
// accessors reference by the fully-qualified path crate::fletcher_gen::__rba::*.
//
// The plugin owns this text so the helper arrow-API spellings stay in lock-step
// with the per-impl getters that instantiate them (versioned with arrow
// =59.0.0). It carries ZERO per-file / per-message content, so every copy the
// plugin writes per protoc run is byte-identical and the build.rs assembler
// include!s it EXACTLY ONCE under crate::fletcher_gen::__rba (N1).
std::string EmitRustRbaHelpers();

}  // namespace fletcher
