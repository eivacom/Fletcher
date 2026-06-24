// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#pragma once

#include <google/protobuf/descriptor.h>

#include <string>

namespace fletcher {

// RBA-1 minimal phase: the RecordBatch accessor emitters produce minimal but
// valid artifacts. Content (accessor classes/structs) is owned by RBA-2+.
//
// Both entry points emit exactly one file per proto invocation. The callers in
// ArrowRowGenerator::Generate run them UNCONDITIONALLY whenever the matching
// --fletcher_opt token is set (never content-gated), which is what makes the
// no-drift forcing test's "exactly +2 files" assertion hold for every fixture.

// Emits the C++ accessor header (<stem>.fletcher.accessor.pb.h):
// generated-file banner, #pragma once, and an empty fletcher_gen::<package>
// namespace skeleton. No accessor classes yet.
std::string EmitAccessorHeader(const google::protobuf::FileDescriptor* file);

// Emits the Rust accessor module (<stem>.fletcher.rs): generated-file banner
// that parses as a standalone Rust source file. No package wrapper modules —
// package mounting is owned by the RBA-5 assembler (D-RBA-10).
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
