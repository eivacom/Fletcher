// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#pragma once

#include <google/protobuf/descriptor.h>

#include <optional>
#include <string>

namespace fletcher {

// Describes a scalar (or scalar-like) Arrow type: enough information for the
// code generator to emit setters, builders, and scalar constructors.
struct ScalarTypeInfo {
    std::string arrow_type_expr;   // e.g. "arrow::int32()"
    std::string storage_type;      // C++ type for std::optional<>/vector<> — e.g. "int32_t"
    std::string param_type;        // setter param — may differ (e.g. "std::string_view")
    std::string scalar_ctor;       // format string with {val} token
    std::string default_value;     // proto3 zero-default as C++ literal
    std::string builder_type;      // e.g. "arrow::Int32Builder"
    std::string scalar_type;       // e.g. "arrow::Int32Scalar" — for downcasting when decoding
    bool value_is_buffer = false;  // true for string/binary (value->ToString())
};

enum class FieldKind {
    SCALAR,           // Simple scalar: int32, string, bool, enum, well-known wrappers
    REPEATED_SCALAR,  // repeated int32 → list(int32)
    STRUCT,           // Nested message → struct<...>
    REPEATED_STRUCT,  // repeated Message → list(struct<...>)
    NESTED_LIST,      // List<List<...<Struct>>> — depth-parameterised nested list
    MAP,              // map<K,V>
};

struct FieldMapping {
    FieldKind kind;
    bool nullable;
    std::string warning;  // Non-empty → emit as a comment in generated code

    // SCALAR kind:
    ScalarTypeInfo scalar;

    // REPEATED_SCALAR kind — describes the list element:
    ScalarTypeInfo element;

    // STRUCT / REPEATED_STRUCT / NESTED_LIST kind:
    std::string nested_class;   // C++ type reference (globally qualified when cross-file)
    std::string nested_header;  // non-empty → #include this path (cross-file dependency)
    int list_depth = 0;         // NESTED_LIST: 2 = List<List<Struct>>, 3 = List<List<List<Struct>>>
    // Descriptor behind nested_class — the message whose schema the generated
    // code references via <nested_class>Schema(). Used by the in-process
    // schema builder (--fletcher_opt=ipc) to build the same schema directly.
    const google::protobuf::Descriptor* nested_msg = nullptr;

    // MAP kind:
    ScalarTypeInfo map_key;
    ScalarTypeInfo map_value;      // populated when value is a scalar type
    std::string map_value_class;   // C++ type reference (globally qualified when cross-file)
    std::string map_value_header;  // non-empty → #include this path (cross-file dep)
    bool map_value_is_message = false;
    // Descriptor behind map_value_class (see nested_msg above).
    const google::protobuf::Descriptor* map_value_msg = nullptr;
};

// Classify a proto field and return enough information to generate Arrow code.
// Returns nullopt for unsupported constructs (oneof, recursive, etc.).
std::optional<FieldMapping> MapField(const google::protobuf::FieldDescriptor* field);

// Human-readable explanation of why MapField returned nullopt.
std::string UnsupportedReason(const google::protobuf::FieldDescriptor* field);

// True if the message (directly or transitively) references itself.
bool IsRecursive(const google::protobuf::Descriptor* msg);

// True if the message has (fletcher.flatten) = true and exactly one field.
// No class should be generated for these — their representation is absorbed
// into the enclosing message's Arrow schema.
bool IsFlattenedWrapper(const google::protobuf::Descriptor* msg);

// True if the field has [(fletcher.flatten_field) = true].
bool HasFieldFlatten(const google::protobuf::FieldDescriptor* field);

// Maximum struct-nesting depth starting from msg.
// A flat message (only scalar fields) has depth 0.
int NestingDepth(const google::protobuf::Descriptor* msg);

// C++ class name for the generated row wrapper (lives in fletcher_gen:: namespace).
// Handles nested messages: Outer.Inner → "Outer_Inner".
std::string ClassName(const google::protobuf::Descriptor* msg);

// C++ class name for the generated immutable view wrapper.
// Handles nested messages: Outer.Inner → "Outer_InnerView".
std::string ViewClassName(const google::protobuf::Descriptor* msg);

// -----------------------------------------------------------------------
// C++ wire format helpers (for EncodeTo code generation)
// -----------------------------------------------------------------------

// WireTypeId hex literal for a scalar proto field type.
// Returns "" for unsupported types.  e.g. TYPE_BOOL → "0x01", TYPE_INT32 → "0x04".
std::string CppWireTypeIdHex(google::protobuf::FieldDescriptor::Type type);

// WireTypeId hex literal for a composite field kind.
// STRUCT → "0x20", LIST → "0x21", MAP → "0x24".
std::string CppWireTypeIdHex(FieldKind kind);

// -----------------------------------------------------------------------
// TypeScript code generation helpers
// -----------------------------------------------------------------------

// TypeScript type string for a scalar proto field type.
// Returns "" for unsupported types.  e.g. TYPE_BOOL → "boolean", TYPE_INT64 → "bigint".
std::string TsScalarType(google::protobuf::FieldDescriptor::Type type);

// WireTypeId enum member name for a scalar proto field type.
// Returns "" for unsupported types.  e.g. TYPE_BOOL → "WireTypeId.BOOL".
std::string WireTypeIdName(google::protobuf::FieldDescriptor::Type type);

// TypeScript interface name for a message: Outer.Inner → "IOuter_Inner".
std::string TsInterfaceName(const google::protobuf::Descriptor* msg);

}  // namespace fletcher
