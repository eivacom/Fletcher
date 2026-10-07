// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include "csharp_backend_type_table.hpp"

#include <functional>
#include <set>
#include <string>

namespace fletcher::csharp_backend {

namespace {

bool IsLower(char c) { return 'a' <= c && c <= 'z'; }
bool IsUpper(char c) { return 'A' <= c && c <= 'Z'; }
bool IsDigit(char c) { return '0' <= c && c <= '9'; }
bool IsAlnum(char c) { return IsLower(c) || IsUpper(c) || IsDigit(c); }
char ToUpper(char c) { return IsLower(c) ? static_cast<char>(c - ('a' - 'A')) : c; }
char ToLower(char c) { return IsUpper(c) ? static_cast<char>(c + ('a' - 'A')) : c; }

// Members a property must not share a name with (D-BIND-73): protoc's reserved
// list from GetPropertyName (csharp_helpers.cc), then the generated class's own
// members. A slice that adds a member to the generated class adds it here.
const std::set<std::string, std::less<>>& ReservedMemberNames() {
    static const std::set<std::string, std::less<>> kNames = {
        // protoc's list, verbatim.
        "Types", "Descriptor", "Equals", "ToString", "GetHashCode", "WriteTo", "Clone",
        "CalculateSize", "MergeFrom", "OnConstruction", "Parser",
        // The generated class's own members.
        "Schema", "ToArrow", "FromArrow", "ToArrowColumns"};
    return kNames;
}

}  // namespace

// ---------------------------------------------------------------------------
// Scalar lookup: language-neutral logical identity -> C# type. These are the ONLY
// C# type strings in the pipeline (GIR locked decision #1).
// ---------------------------------------------------------------------------

std::optional<CsScalarInfo> CsLookupScalar(const ir::LogicalType& type,
                                           const std::optional<ir::EnumIdentity>& enum_identity) {
    // An enum lowers to INT32 storage and carries its identity; C# emits a real
    // `enum` for it (D-BIND-8), so the property's type is the generated enum.
    if (enum_identity.has_value() && enum_identity->descriptor != nullptr)
        return CsScalarInfo{CsTypeName(enum_identity->descriptor), false, "Int32Array", true};

    using LK = ir::LogicalKind;
    switch (type.kind) {
        case LK::BOOL:
            return CsScalarInfo{"bool", false, "BooleanArray"};
        case LK::INT32:
            return CsScalarInfo{"int", false, "Int32Array"};
        case LK::INT64:
            return CsScalarInfo{"long", false, "Int64Array"};
        case LK::UINT32:
            return CsScalarInfo{"uint", false, "UInt32Array"};
        case LK::UINT64:
            return CsScalarInfo{"ulong", false, "UInt64Array"};
        case LK::FLOAT32:
            return CsScalarInfo{"float", false, "FloatArray"};
        case LK::FLOAT64:
            return CsScalarInfo{"double", false, "DoubleArray"};
        case LK::UTF8:
            return CsScalarInfo{"string", true, "StringArray"};
        case LK::BINARY:
            return CsScalarInfo{"byte[]", true, "BinaryArray"};
        default:
            // Temporal types are BIND-6c's (D-BIND-26: lossless); the rest are not
            // produced by the proto mapping.
            return std::nullopt;
    }
}

namespace {

// The two well-known types that need a C# type of their own live in Eiva.Fletcher.Model
// (D-BIND-74), named in full so a message called Timestamp in the file cannot shadow them.
constexpr const char* kModelTimestamp = "global::Eiva.Fletcher.Model.Timestamp";
constexpr const char* kModelDuration = "global::Eiva.Fletcher.Model.Duration";

// The type of one list element or map entry part: no nullability, no collection.
std::optional<std::string> ElementTypeText(const ir::IrNode& node,
                                           const google::protobuf::FileDescriptor* current_file) {
    if (node.kind == ir::NodeKind::STRUCT) {
        const auto& s = std::get<ir::StructNode>(node.node);
        // A message declared in another file has no generated class here: that is
        // cross-file work (BIND-6e), and naming it would not compile. This includes
        // google.protobuf.Timestamp as a list element, which the IR maps as a struct.
        if (s.identity.descriptor == nullptr || s.identity.descriptor->file() != current_file)
            return std::nullopt;
        return CsTypeName(s.identity.descriptor);
    }
    // 6c-3: a list element that is itself a list, which is how a flatten wrapper around a
    // repeated field nests (`repeated StructListWrapper` is list<list<struct>>).
    if (node.kind == ir::NodeKind::LIST) {
        auto inner = ElementTypeText(*std::get<ir::ListNode>(node.node).element, current_file);
        if (!inner) return std::nullopt;
        return "global::System.Collections.Generic.List<" + *inner + ">";
    }
    if (node.kind != ir::NodeKind::SCALAR) return std::nullopt;
    const auto& s = std::get<ir::ScalarNode>(node.node);
    if (const auto info = CsLookupScalar(s.logical_type, s.enum_identity)) return info->type_text;
    if (s.logical_type.kind == ir::LogicalKind::WKT_TIMESTAMP) return std::string(kModelTimestamp);
    if (s.logical_type.kind == ir::LogicalKind::WKT_DURATION) return std::string(kModelDuration);
    return std::nullopt;
}

}  // namespace

std::optional<CsFieldType> CsFieldTypeOf(const ir::IrNode& node,
                                         const google::protobuf::FileDescriptor* current_file) {
    CsFieldType out;
    switch (node.kind) {
        case ir::NodeKind::SCALAR: {
            const auto& s = std::get<ir::ScalarNode>(node.node);
            auto text = ElementTypeText(node, current_file);
            if (!text) return std::nullopt;
            out.type_text = std::move(*text);
            out.nullable = node.facts.nullable;
            if (const auto info = CsLookupScalar(s.logical_type, s.enum_identity))
                out.is_reference = info->is_reference;
            return out;
        }
        case ir::NodeKind::STRUCT: {
            auto text = ElementTypeText(node, current_file);
            if (!text) return std::nullopt;
            out.type_text = std::move(*text);
            out.nullable = true;  // a message field can be absent
            return out;
        }
        case ir::NodeKind::LIST: {
            auto element =
                ElementTypeText(*std::get<ir::ListNode>(node.node).element, current_file);
            if (!element) return std::nullopt;
            out.type_text = "global::System.Collections.Generic.List<" + *element + ">";
            out.is_collection = true;
            return out;
        }
        case ir::NodeKind::MAP: {
            const auto& m = std::get<ir::MapNode>(node.node);
            auto key = ElementTypeText(*m.key, current_file);
            auto value = ElementTypeText(*m.value, current_file);
            if (!key || !value) return std::nullopt;
            out.type_text =
                "global::System.Collections.Generic.List<"
                "global::System.Collections.Generic.KeyValuePair<" +
                *key + ", " + *value + ">>";
            out.is_collection = true;
            return out;
        }
        case ir::NodeKind::FIXED_SIZE_LIST:
        case ir::NodeKind::UNSUPPORTED:
            return std::nullopt;
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Apache.Arrow C# type expressions for the nanoarrow types SchemaVisitor sets
// (BIND-6b). Fully qualified, so a generated class named Schema or Field cannot
// shadow them.
// ---------------------------------------------------------------------------

std::optional<std::string> CsArrowScalarTypeExpr(ArrowType type) {
    const char* name = nullptr;
    switch (type) {
        case NANOARROW_TYPE_BOOL:
            name = "BooleanType";
            break;
        case NANOARROW_TYPE_INT32:
            name = "Int32Type";
            break;
        case NANOARROW_TYPE_INT64:
            name = "Int64Type";
            break;
        case NANOARROW_TYPE_UINT32:
            name = "UInt32Type";
            break;
        case NANOARROW_TYPE_UINT64:
            name = "UInt64Type";
            break;
        case NANOARROW_TYPE_FLOAT:
            name = "FloatType";
            break;
        case NANOARROW_TYPE_DOUBLE:
            name = "DoubleType";
            break;
        case NANOARROW_TYPE_STRING:
            name = "StringType";
            break;
        case NANOARROW_TYPE_BINARY:
            name = "BinaryType";
            break;
        default:
            return std::nullopt;
    }
    return std::string("global::Apache.Arrow.Types.") + name + ".Default";
}

std::optional<std::string> CsArrowDateTimeTypeExpr(ArrowType type, ArrowTimeUnit unit,
                                                   const char* timezone) {
    const char* unit_name = nullptr;
    switch (unit) {
        case NANOARROW_TIME_UNIT_SECOND:
            unit_name = "Second";
            break;
        case NANOARROW_TIME_UNIT_MILLI:
            unit_name = "Millisecond";
            break;
        case NANOARROW_TIME_UNIT_MICRO:
            unit_name = "Microsecond";
            break;
        case NANOARROW_TIME_UNIT_NANO:
            unit_name = "Nanosecond";
            break;
    }
    if (unit_name == nullptr) return std::nullopt;
    const std::string unit_expr = std::string("global::Apache.Arrow.Types.TimeUnit.") + unit_name;
    if (type == NANOARROW_TYPE_TIMESTAMP)
        return "new global::Apache.Arrow.Types.TimestampType(" + unit_expr + ", " +
               (timezone != nullptr ? CsStringLiteral(timezone) : std::string("(string?)null")) +
               ")";
    // DurationType has no public constructor in Apache.Arrow 23, only per-unit
    // statics and FromTimeUnit (checked by reflection, 2026-10-05).
    if (type == NANOARROW_TYPE_DURATION)
        return "global::Apache.Arrow.Types.DurationType.FromTimeUnit(" + unit_expr + ")";
    return std::nullopt;
}

std::string CsStringLiteral(std::string_view text) {
    static const char kHex[] = "0123456789abcdef";
    std::string out = "\"";
    for (const char c : text) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                // Other control characters as \u escapes; bytes >= 0x80 are UTF-8
                // and the generated file is UTF-8, so they pass through.
                if (static_cast<unsigned char>(c) < 0x20) {
                    out += "\\u00";
                    out += kHex[(c >> 4) & 0xF];
                    out += kHex[c & 0xF];
                } else {
                    out += c;
                }
        }
    }
    out += '"';
    return out;
}

// ---------------------------------------------------------------------------
// protoc's naming rules, ported from compiler/csharp/names.cc and
// csharp_helpers.cc. Keep them behaviourally identical: D-BIND-73 makes protoc's
// output the specification, and test_csharp_type_table.cpp pins each rule.
// ---------------------------------------------------------------------------

std::string UnderscoresToCamelCase(std::string_view input, bool cap_next_letter,
                                   bool preserve_period) {
    std::string result;
    for (size_t i = 0; i < input.size(); i++) {
        const char c = input[i];
        if (IsLower(c)) {
            result += cap_next_letter ? ToUpper(c) : c;
            cap_next_letter = false;
        } else if (IsUpper(c)) {
            result += (i == 0 && !cap_next_letter) ? ToLower(c) : c;
            cap_next_letter = false;
        } else if (IsDigit(c)) {
            result += c;
            cap_next_letter = true;
        } else {
            cap_next_letter = true;
            if (c == '.' && preserve_period) result += '.';
        }
    }
    if (!input.empty() && input.back() == '#') result += '_';
    if (!result.empty() && IsDigit(result[0]) && !input.empty() && input[0] == '_')
        result.insert(0, 1, '_');
    return result;
}

std::string ShoutyToPascalCase(std::string_view input) {
    std::string result;
    char previous = '_';
    for (const char current : input) {
        if (!IsAlnum(current)) {
            previous = current;
            continue;
        }
        if (!IsAlnum(previous) || IsDigit(previous))
            result += ToUpper(current);
        else if (IsLower(previous))
            result += current;
        else
            result += ToLower(current);
        previous = current;
    }
    return result;
}

std::string TryRemovePrefix(std::string_view prefix, std::string_view value) {
    std::string prefix_to_match;
    for (const char c : prefix)
        if (c != '_') prefix_to_match += ToLower(c);

    size_t prefix_index = 0;
    size_t value_index = 0;
    for (; prefix_index < prefix_to_match.size() && value_index < value.size(); value_index++) {
        if (value[value_index] == '_') continue;
        if (ToLower(value[value_index]) != prefix_to_match[prefix_index++])
            return std::string(value);
    }
    if (prefix_index < prefix_to_match.size()) return std::string(value);
    while (value_index < value.size() && value[value_index] == '_') value_index++;
    if (value_index == value.size()) return std::string(value);
    return std::string(value.substr(value_index));
}

// ---------------------------------------------------------------------------
// Names Fletcher decides (D-BIND-69): the namespace root and flat nested types.
// ---------------------------------------------------------------------------

std::string CsNamespace(const google::protobuf::FileDescriptor* file) {
    const std::string package = UnderscoresToCamelCase(file->package(), true, true);
    return package.empty() ? "Fletcher.Gen" : "Fletcher.Gen." + package;
}

namespace {

template <typename D>
std::string FlatName(const D* d) {
    std::string name = d->name();
    for (const auto* parent = d->containing_type(); parent != nullptr;
         parent = parent->containing_type())
        name = parent->name() + "_" + name;
    return name;
}

}  // namespace

std::string CsTypeName(const google::protobuf::Descriptor* msg) { return FlatName(msg); }

std::string CsTypeName(const google::protobuf::EnumDescriptor* enm) { return FlatName(enm); }

std::string CsPropertyName(std::string_view field_name, const google::protobuf::Descriptor* owner) {
    std::string property = UnderscoresToCamelCase(field_name, true, false);
    // protoc compares with the message's own (unflattened) name. A flat name such
    // as Outer_Inner can never collide: PascalCase drops every underscore.
    if (property == owner->name() || ReservedMemberNames().count(property) != 0) property += '_';
    return property;
}

std::vector<CsEnumMember> CsEnumMembers(const google::protobuf::EnumDescriptor* enm) {
    std::vector<CsEnumMember> members;
    std::set<std::string> used;
    for (int i = 0; i < enm->value_count(); ++i) {
        const auto* value = enm->value(i);
        std::string name = ShoutyToPascalCase(TryRemovePrefix(enm->name(), value->name()));
        if (!name.empty() && IsDigit(name[0])) name.insert(0, 1, '_');
        // protoc's duplicate loop (csharp_enum.cc): append '_' until unused.
        while (!used.insert(name).second) name += '_';
        members.push_back({std::move(name), value->number()});
    }
    return members;
}

}  // namespace fletcher::csharp_backend
