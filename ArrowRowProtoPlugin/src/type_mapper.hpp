#pragma once

#include <google/protobuf/descriptor.h>

#include <optional>
#include <string>

namespace arrow_row_plugin {

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
    bool        value_is_buffer = false;  // true for string/binary (value->ToString())
};

enum class FieldKind {
    SCALAR,           // Simple scalar: int32, string, bool, enum, well-known wrappers
    REPEATED_SCALAR,  // repeated int32 → list(int32)
    STRUCT,           // Nested message → struct<...>
    REPEATED_STRUCT,  // repeated Message → list(struct<...>)
    MAP,              // map<K,V>
};

struct FieldMapping {
    FieldKind kind;
    bool      nullable;
    std::string warning;       // Non-empty → emit as a comment in generated code

    // SCALAR kind:
    ScalarTypeInfo scalar;

    // REPEATED_SCALAR kind — describes the list element:
    ScalarTypeInfo element;

    // STRUCT / REPEATED_STRUCT kind — the C++ generated class for the message:
    std::string nested_class;   // C++ type reference (globally qualified when cross-file)
    std::string nested_header;  // non-empty → #include this path (cross-file dependency)

    // MAP kind:
    ScalarTypeInfo map_key;
    ScalarTypeInfo map_value;           // populated when value is a scalar type
    std::string    map_value_class;     // C++ type reference (globally qualified when cross-file)
    std::string    map_value_header;    // non-empty → #include this path (cross-file dep)
    bool           map_value_is_message = false;
};

// Classify a proto field and return enough information to generate Arrow code.
// Returns nullopt for unsupported constructs (oneof, recursive, etc.).
std::optional<FieldMapping> MapField(const google::protobuf::FieldDescriptor* field);

// Human-readable explanation of why MapField returned nullopt.
std::string UnsupportedReason(const google::protobuf::FieldDescriptor* field);

// True if the message (directly or transitively) references itself.
bool IsRecursive(const google::protobuf::Descriptor* msg);

// Maximum struct-nesting depth starting from msg.
// A flat message (only scalar fields) has depth 0.
int NestingDepth(const google::protobuf::Descriptor* msg);

// C++ class name for the generated ArrowRow wrapper.
// Handles nested messages: Outer.Inner → "Outer_InnerArrowRow".
std::string ClassName(const google::protobuf::Descriptor* msg);

}  // namespace arrow_row_plugin
