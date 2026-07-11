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

// GIR-9 (#75): C++ spelling of a generated `enum class` as seen from
// `context_file`. Single source of truth for enum-type spelling, shared by the
// row typed accessors (generator.cpp) and the view typed getters
// (cpp_backend_view_visitor.cpp) — the enum DECLARATION emits the bare
// `ed->name()` in its owning scope, every REFERENCE goes through this helper.
//   * top-level enum          -> bare name ("TopLevelStatus")
//   * nested enum             -> owner class + "::" + name
//                                ("NestedEnums::InnerStatus", "EnumOwner::InnerStatus")
//   * cross-package reference -> "::fletcher_gen::<pkg>::" prefixed
// Derived from descriptors/scope only; no C++ spelling is stored on any IR node
// (locked decision #1). Mirrors CppClassName's package-qualification rule so an
// imported same-package enum (GIR-9's enum_coverage importing coverage's
// TopLevelStatus) needs only an include, not a namespace prefix.
std::string CppEnumName(const google::protobuf::EnumDescriptor* enum_descriptor,
                        const google::protobuf::FileDescriptor* context_file);

// GIR-9 (#75): whether the generated `enum class` for `enum_descriptor` is
// actually emitted somewhere a typed accessor could name it. A top-level enum is
// always emitted at package scope; a NESTED enum is emitted only inside its
// owning generated message class, which is NOT generated when the owner is
// recursive or a flattened wrapper (the same skip predicate the emit loops use:
// IsRecursive || IsFlattenedWrapper). When this returns false the caller MUST
// emit only the raw int32 accessor — a typed accessor would name an undeclared
// class and break compilation. Shared by the row emitter (generator.cpp) and the
// view emitter (cpp_backend_view_visitor.cpp) so both gate identically.
bool CppEnumTypeEmittable(const google::protobuf::EnumDescriptor* enum_descriptor);

// Emit positional-format edge encoding for a single top-level field, driven by
// the recursive IR (NOT by FieldMapping). `value_expr` is the storage member
// expression (e.g. "field_"); `field_index` is the positional index used for
// SetNull on nullable scalar/struct fields.
void EmitFieldEncodeFromIr(std::ostringstream& out, const ir::IrNode& node,
                           const std::string& value_expr, size_t field_index,
                           const google::protobuf::FileDescriptor* context_file);

}  // namespace fletcher::cpp_backend
