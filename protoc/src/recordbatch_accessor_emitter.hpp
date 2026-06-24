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

}  // namespace fletcher
