// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#pragma once

// GIR-6: the IR-driven Arrow view-getter + ToArrowRow() visitors. These mirror
// the GIR-3 edge ENCODE (cpp_backend_type_table.cpp), GIR-4 edge DECODE
// (cpp_backend_decode_visitor.cpp), and GIR-5 schema (cpp_backend_schema_visitor)
// visitors: each walks the language-neutral ir::IrNode directly and turns a
// top-level message field into the generated Arrow C++ view getter (in the
// `<Class>View` class) and the matching ToArrowRow() field emission.
//
// This is a byte/behaviour-identical migration of generator.cpp's former
// FieldMapping-switch EmitViewGetters + GenerateToArrowRow: no view-produced
// Arrow value and no round-trip behaviour changes. The IR stays language-neutral
// (locked decision #1): Arrow scalar/array class names, view class names, storage
// / getter types, builder types, and Arrow type expressions come from the C++
// backend lookup table (cpp_backend_type_table.hpp), never from fields on IR
// nodes.
//
// ToArrowRow() reads each field through the public generated getter, which already
// applies value_or(default) for non-nullable scalars, so the row visitor branches
// ONLY on nullable vs non-nullable shape — it never re-threads GIR-3's
// ValueAccessMode / re-applies defaults (that would double-default).

#include <google/protobuf/descriptor.h>

#include <cstddef>
#include <sstream>
#include <string>

#include "ir.hpp"

namespace fletcher::cpp_backend {

// Emit the generated Arrow view getter for a single top-level field, driven by
// the recursive IR (NOT by FieldMapping). `getter_name` is the generated method
// name (the proto field name); `field_index` is the positional index into the
// view's `scalars_` vector.
void EmitViewGetterFromIr(std::ostringstream& out, const ir::IrNode& node,
                          const std::string& getter_name, std::size_t field_index,
                          const google::protobuf::FileDescriptor* context_file);

// Emit the ToArrowRow() field body for a single top-level field, driven by the
// recursive IR. `getter_expr` is the public getter expression the row reads from
// (e.g. "msg.field()"). `field_index` is accepted for signature parity with the
// view getter / edge visitors; ToArrowRow() reads only through `getter_expr`.
void EmitToArrowRowFieldFromIr(std::ostringstream& out, const ir::IrNode& node,
                               const std::string& getter_expr, std::size_t field_index,
                               const google::protobuf::FileDescriptor* context_file);

}  // namespace fletcher::cpp_backend
