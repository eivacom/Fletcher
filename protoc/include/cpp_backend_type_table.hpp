// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#pragma once

// GIR-3: the C++ backend lookup table. This is the ONLY place a C++/Arrow type
// string is allowed to live (locked decision #1). It is keyed by the
// language-neutral IR logical identity (ir::LogicalType + optional
// ir::EnumIdentity) and turns that identity into the generated-code strings
// ("arrow::int32()", "int32_t", "WriteInt32", ...). The IR nodes themselves
// carry no such strings.

#include <google/protobuf/descriptor.h>

#include <optional>
#include <sstream>
#include <string>

#include "ir.hpp"

namespace fletcher::cpp_backend {

// All C++ generated-code strings for one scalar logical identity. Superset of
// type_mapper's ScalarTypeInfo (adds positional_write/positional_read and
// value_is_buffer as a bool); the projection copies the shared fields across.
struct CppScalarInfo {
    std::string arrow_type_expr;   // e.g. "arrow::int32()"
    std::string storage_type;      // e.g. "int32_t"
    std::string param_type;        // setter param — e.g. "std::string_view"
    std::string scalar_ctor;       // format string with {val} token
    std::string default_value;     // proto3 zero-default as C++ literal
    std::string builder_type;      // e.g. "arrow::Int32Builder"
    std::string scalar_type;       // e.g. "arrow::Int32Scalar"
    bool value_is_buffer = false;  // true for string/binary
    std::string positional_write;  // PositionalWriter method — e.g. "WriteInt32"
    std::string positional_read;   // PositionalReader method — e.g. "ReadInt32"
    // GIR-6 view/ToArrowRow spelling, derived here so no Arrow array name or
    // getter type leaks onto an IR node (locked decision #1). `array_type` is the
    // scalar's companion Arrow array class ("arrow::Int32Array"); `getter_type` is
    // the view getter return type (std::string_view for buffers, storage_type
    // otherwise). These reproduce the former generator.cpp ArrayTypeFromScalar /
    // GetterType helpers exactly.
    std::string array_type;   // e.g. "arrow::Int32Array"
    std::string getter_type;  // view getter return type — e.g. "int32_t"
};

// Map a language-neutral scalar logical identity to its C++ backend strings.
// `enum_identity` disambiguates a plain INT32 from an enum-as-INT32 (which needs
// a static_cast in its scalar constructor).
const CppScalarInfo& LookupScalar(const ir::LogicalType& type,
                                  const std::optional<ir::EnumIdentity>& enum_identity);

// Globally-qualified generated C++ class reference for `msg` as seen from
// `context_file` (bare name in-package; "::fletcher_gen::<pkg>::" prefixed
// cross-package). Single source of truth for struct class-name resolution.
std::string CppClassName(const google::protobuf::Descriptor* msg,
                         const google::protobuf::FileDescriptor* context_file);

// Generated-header include path for `msg`'s file relative to the proto root;
// empty when `msg` lives in `context_file`.
std::string CppCrossFileHeader(const google::protobuf::Descriptor* msg,
                               const google::protobuf::FileDescriptor* context_file);

// Emit positional-format edge encoding for a single top-level field, driven by
// the recursive IR (NOT by FieldMapping). `value_expr` is the storage member
// expression (e.g. "field_"); `field_index` is the positional index used for
// SetNull on nullable scalar/struct fields.
void EmitFieldEncodeFromIr(std::ostringstream& out, const ir::IrNode& node,
                           const std::string& value_expr, size_t field_index,
                           const google::protobuf::FileDescriptor* context_file);

}  // namespace fletcher::cpp_backend
