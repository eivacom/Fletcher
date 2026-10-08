// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#pragma once

// BIND-6a: the C# backend lookup table. This is the ONLY place a C# type string is
// allowed to live (GIR locked decision #1). It is keyed by the language-neutral IR
// logical identity (ir::LogicalType + optional ir::EnumIdentity), as the TypeScript
// table is, and also holds the C# naming rules.
//
// The naming rules are protoc's own, read from its source (compiler/csharp/names.cc,
// csharp_helpers.cc, csharp_enum.cc) and adopted by D-BIND-69, D-BIND-70 and
// D-BIND-73, so a generated property or enum member is named exactly as protoc's
// --csharp_out names it. The namespace is the one deliberate difference: Fletcher
// owns it (Fletcher.Gen.<Package>) and ignores `option csharp_namespace`, so our
// types can sit beside protoc's in one project.

#include <google/protobuf/descriptor.h>
#include <nanoarrow/nanoarrow.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ir.hpp"

namespace fletcher::csharp_backend {

// The C# text for one scalar logical identity.
struct CsScalarInfo {
    std::string type_text;      // e.g. "int", "string", "byte[]", or a generated enum's name
    bool is_reference = false;  // a reference type: nullable as `T?`, defaulted when not
    std::string arrow_array;    // the Apache.Arrow array class, e.g. "Int32Array" (BIND-6b)
    bool is_enum = false;       // stored as int32, cast to and from the generated enum
};

// Map a scalar logical identity to its C# type, or nullopt for a kind this backend
// does not map yet (temporal types arrive with BIND-6c, losslessly per D-BIND-26).
// An enum is named as seen from `current_file`, the file being generated (CsTypeRef).
std::optional<CsScalarInfo> CsLookupScalar(const ir::LogicalType& type,
                                           const std::optional<ir::EnumIdentity>& enum_identity,
                                           const google::protobuf::FileDescriptor* current_file);

// How one field's property is declared on the row class (BIND-6c). Every field the IR
// maps has one, including the ones ToArrow / FromArrow cannot convert yet.
struct CsFieldType {
    std::string type_text;       // the property's type, without a trailing '?'
    bool is_collection = false;  // a List<...>: never null, starts empty
    bool nullable = false;       // the property is annotated `?` (an absent value is null)
    bool is_reference =
        false;  // a scalar reference type (string, byte[]) defaulted when not nullable
};

// The C# property type for an IR field node, or nullopt for a node this backend has
// no type for. The shapes (D-BIND-75):
//   scalar            the CsLookupScalar type
//   Timestamp/Duration  global::Eiva.Fletcher.Model.Timestamp / Duration (D-BIND-26, D-BIND-74),
//                     nullable only where the field is (the schema says which)
//   message           the generated class, `?` when the field can be absent
//   repeated T        List<T>
//   map<K,V>          List<KeyValuePair<K,V>>: entry order is on the wire, and C++'s row class
//                     holds ordered pairs too, so a Dictionary's unspecified order is refused
// A message or enum type is named as `CsTypeRef` names it from `current_file`, the file being
// generated: a type from another file is its class in that file's namespace (BIND-6e), which
// the consumer generates too, as C++ includes the other file's header. That holds for
// `repeated google.protobuf.Timestamp`, which the IR maps as the struct seconds/nanos, not as
// a timestamp: its class is generated from timestamp.proto.
std::optional<CsFieldType> CsFieldTypeOf(const ir::IrNode& node,
                                         const google::protobuf::FileDescriptor* current_file);

// The Apache.Arrow C# expression for a nanoarrow scalar type, e.g. NANOARROW_TYPE_INT32 ->
// "global::Apache.Arrow.Types.Int32Type.Default", or nullopt for one the proto mapping never
// produces (BIND-6b; the schema sink renders through these).
std::optional<std::string> CsArrowScalarTypeExpr(ArrowType type);

// The same for a timestamp or duration: the unit, and a timestamp's timezone (null for none).
std::optional<std::string> CsArrowDateTimeTypeExpr(ArrowType type, ArrowTimeUnit unit,
                                                   const char* timezone);

// A C# regular string literal, quotes included, with every character that cannot appear
// raw escaped. Metadata values come from proto options, so they can hold anything.
std::string CsStringLiteral(std::string_view text);

// protoc's UnderscoresToCamelCase (names.cc), verbatim in behaviour.
std::string UnderscoresToCamelCase(std::string_view input, bool cap_next_letter,
                                   bool preserve_period);

// protoc's ShoutyToPascalCase and TryRemovePrefix (csharp_helpers.cc).
std::string ShoutyToPascalCase(std::string_view input);
std::string TryRemovePrefix(std::string_view prefix, std::string_view value);

// "Fletcher.Gen." + the package PascalCased keeping dots; "Fletcher.Gen" for no
// package. `option csharp_namespace` is ignored (D-BIND-69).
std::string CsNamespace(const google::protobuf::FileDescriptor* file);

// A message's or enum's C# type name: nested types flattened with '_'
// (Outer.Inner -> Outer_Inner), as every Fletcher backend does (D-BIND-69).
std::string CsTypeName(const google::protobuf::Descriptor* msg);
std::string CsTypeName(const google::protobuf::EnumDescriptor* enm);

// The same type as code in `current_file` names it (BIND-6e): CsTypeName when both files share
// a namespace, else `global::<CsNamespace>.<CsTypeName>`, so a type from another package
// resolves and no name in this file can shadow it.
std::string CsTypeRef(const google::protobuf::Descriptor* msg,
                      const google::protobuf::FileDescriptor* current_file);
std::string CsTypeRef(const google::protobuf::EnumDescriptor* enm,
                      const google::protobuf::FileDescriptor* current_file);

// The property for proto field `field_name` of `owner`: protoc's GetPropertyName,
// with the generated class's own members added to the reserved list (D-BIND-73).
std::string CsPropertyName(std::string_view field_name, const google::protobuf::Descriptor* owner);

// An enum's members in declaration order, named as protoc's enum generator names
// them: prefix removed, ShoutyToPascalCase, a leading '_' before a digit, and '_'
// appended to a name already used. Aliases keep their own entries.
struct CsEnumMember {
    std::string name;
    int32_t number = 0;
};
std::vector<CsEnumMember> CsEnumMembers(const google::protobuf::EnumDescriptor* enm);

}  // namespace fletcher::csharp_backend
