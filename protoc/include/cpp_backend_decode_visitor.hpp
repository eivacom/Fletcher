// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#pragma once

// GIR-4: the IR-driven edge DECODE visitor. Mirrors the GIR-3 edge ENCODE
// visitor (cpp_backend_type_table.cpp): it walks the language-neutral
// ir::IrNode directly and turns each top-level field into the positional
// PositionalReader extraction that the generated
// `Foo(const uint8_t*, size_t)` / `Foo(PositionalReader&)` constructors run.
//
// The IR stays language-neutral (locked decision #1): C++ scalar storage
// types, PositionalReader method names ("ReadInt32", ...), and generated class
// names come from the C++ backend lookup table (cpp_backend_type_table.hpp),
// never from fields on IR nodes. This is a byte/behaviour-identical migration
// of generator.cpp's former FieldMapping-switch EmitFieldDecode: no wire byte,
// no decoded value, no null/list/map layout changes.

#include <google/protobuf/descriptor.h>

#include <sstream>
#include <string>

#include "ir.hpp"

namespace fletcher::cpp_backend {

// Emit positional-format edge DECODE for a single top-level field, driven by
// the recursive IR (NOT by FieldMapping). `value_expr` is the storage member
// expression (e.g. "field_"); `field_index` is the positional index used for
// IsNull() on nullable scalar/struct and nullable nested-list fields. The
// reads consume the constructor's PositionalReader `r` sequentially.
void EmitFieldDecodeFromIr(std::ostringstream& out, const ir::IrNode& node,
                           const std::string& value_expr, size_t field_index,
                           const google::protobuf::FileDescriptor* context_file);

}  // namespace fletcher::cpp_backend
