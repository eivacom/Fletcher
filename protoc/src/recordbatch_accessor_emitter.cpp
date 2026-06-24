// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include "recordbatch_accessor_emitter.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "generator_internal.hpp"
#include "type_mapper.hpp"

namespace fletcher {

namespace {

// Convert a proto package "foo.bar" to a C++ nested-namespace path "foo::bar".
// Mirrors the package -> namespace derivation used by the existing generated
// headers (fletcher_gen::<package>); kept local to avoid touching generator.cpp.
std::string PackageToNamespace(const std::string& package) {
    std::string out;
    out.reserve(package.size() + 2 * static_cast<std::string::size_type>(
                                         std::count(package.begin(), package.end(), '.')));
    for (char c : package) {
        if (c == '.')
            out += "::";
        else
            out += c;
    }
    return out;
}

// Concrete-array derivation for a scalar field, keyed by the Arrow type
// expression produced by the type-mapper (ScalarTypeInfo.arrow_type_expr). The
// mapper remains the source of truth for the Arrow type and C++ value type; the
// accessor emitter only derives the concrete Arrow array class needed for the
// cached, cast-once handle and the getter form.
//
// `concrete_array` is the arrow::*Array class to static_pointer_cast to and
// cache. `value_type` is the C++ value the getter returns (the std::optional<>
// element type for nullable fields). `use_get_view` selects GetView(row) (utf8 /
// binary, zero-copy std::string_view) vs Value(row) (numeric / bool / temporal).
struct ScalarArrayInfo {
    std::string concrete_array;
    std::string value_type;
    bool use_get_view = false;
};

// Returns the ScalarArrayInfo for a scalar Arrow type expression, or nullptr if
// the expression is not a supported RBA-2 scalar. Keyed by a stable prefix of
// arrow_type_expr so timestamp / duration unit variants all resolve to the same
// array class.
const ScalarArrayInfo* LookupScalarArray(const std::string& expr) {
    // clang-format off
    static const ScalarArrayInfo kBool    {"arrow::BooleanArray",   "bool",        false};
    static const ScalarArrayInfo kInt8    {"arrow::Int8Array",      "int8_t",      false};
    static const ScalarArrayInfo kInt16   {"arrow::Int16Array",     "int16_t",     false};
    static const ScalarArrayInfo kInt32   {"arrow::Int32Array",     "int32_t",     false};
    static const ScalarArrayInfo kInt64   {"arrow::Int64Array",     "int64_t",     false};
    static const ScalarArrayInfo kUInt8   {"arrow::UInt8Array",     "uint8_t",     false};
    static const ScalarArrayInfo kUInt16  {"arrow::UInt16Array",    "uint16_t",    false};
    static const ScalarArrayInfo kUInt32  {"arrow::UInt32Array",    "uint32_t",    false};
    static const ScalarArrayInfo kUInt64  {"arrow::UInt64Array",    "uint64_t",    false};
    static const ScalarArrayInfo kFloat   {"arrow::FloatArray",     "float",       false};
    static const ScalarArrayInfo kDouble  {"arrow::DoubleArray",    "double",      false};
    static const ScalarArrayInfo kString  {"arrow::StringArray",    "std::string_view", true};
    static const ScalarArrayInfo kBinary  {"arrow::BinaryArray",    "std::string_view", true};
    static const ScalarArrayInfo kTime    {"arrow::TimestampArray", "int64_t",     false};
    static const ScalarArrayInfo kDuration{"arrow::DurationArray",  "int64_t",     false};
    // clang-format on

    // Exact-match families first.
    if (expr == "arrow::boolean()") return &kBool;
    if (expr == "arrow::int8()") return &kInt8;
    if (expr == "arrow::int16()") return &kInt16;
    if (expr == "arrow::int32()") return &kInt32;
    if (expr == "arrow::int64()") return &kInt64;
    if (expr == "arrow::uint8()") return &kUInt8;
    if (expr == "arrow::uint16()") return &kUInt16;
    if (expr == "arrow::uint32()") return &kUInt32;
    if (expr == "arrow::uint64()") return &kUInt64;
    if (expr == "arrow::float32()") return &kFloat;
    if (expr == "arrow::float64()") return &kDouble;
    if (expr == "arrow::utf8()") return &kString;
    if (expr == "arrow::binary()") return &kBinary;

    // Timestamp / duration carry a unit argument — match by prefix so every
    // SECOND/MILLI/MICRO/NANO variant resolves to the same array class. The
    // expected-type expression itself (with its unit) still comes verbatim from
    // arrow_type_expr, so the type-equality gate stays unit-precise.
    if (expr.rfind("arrow::timestamp(", 0) == 0) return &kTime;
    if (expr.rfind("arrow::duration(", 0) == 0) return &kDuration;

    return nullptr;
}

// True if the field is a scalar that RBA-2 emits a typed getter for.
bool IsSupportedScalar(const FieldInfo& fi) {
    if (fi.mapping.kind != FieldKind::SCALAR) return false;
    return LookupScalarArray(fi.mapping.scalar.arrow_type_expr) != nullptr;
}

// True if the field's element (REPEATED_SCALAR) is a supported scalar leaf.
bool IsSupportedRepeatedScalar(const FieldInfo& fi) {
    if (fi.mapping.kind != FieldKind::REPEATED_SCALAR) return false;
    return LookupScalarArray(fi.mapping.element.arrow_type_expr) != nullptr;
}

// True if the MAP field is supported: key leaf is a supported scalar and the
// value is either a message (struct value) or a supported scalar leaf.
bool IsSupportedMap(const FieldInfo& fi) {
    if (fi.mapping.kind != FieldKind::MAP) return false;
    if (LookupScalarArray(fi.mapping.map_key.arrow_type_expr) == nullptr) return false;
    if (fi.mapping.map_value_is_message) return true;
    return LookupScalarArray(fi.mapping.map_value.arrow_type_expr) != nullptr;
}

// True if the NESTED_LIST field is a supported depth (2 or 3 per spec §6). The
// leaf is always a struct (nested_class set by the type-mapper).
bool IsSupportedNestedList(const FieldInfo& fi) {
    if (fi.mapping.kind != FieldKind::NESTED_LIST) return false;
    return fi.mapping.list_depth == 2 || fi.mapping.list_depth == 3;
}

// True if the accessor generates real storage/getters for this field. RBA-4
// covers scalars (RBA-2), STRUCT, REPEATED_SCALAR (supported leaf),
// REPEATED_STRUCT (RBA-4a), MAP, and NESTED_LIST depth 2-3 (RBA-4b).
bool IsSupportedField(const FieldInfo& fi) {
    switch (fi.mapping.kind) {
        case FieldKind::SCALAR:
            return IsSupportedScalar(fi);
        case FieldKind::STRUCT:
            return true;
        case FieldKind::REPEATED_SCALAR:
            return IsSupportedRepeatedScalar(fi);
        case FieldKind::REPEATED_STRUCT:
            return true;
        case FieldKind::MAP:
            return IsSupportedMap(fi);
        case FieldKind::NESTED_LIST:
            return IsSupportedNestedList(fi);
    }
    return false;
}

// The generated accessor type reference for a nested message field. The mapper's
// nested_class is the generated row class (e.g. "Outer" same-file, or
// "::fletcher_gen::pkg::Foo" cross-file); the accessor is that name + "Accessor".
std::string AccessorRef(const std::string& nested_class) { return nested_class + "Accessor"; }

// Member-name helpers (keep storage names stable + collision-free per field).
std::string StructMember(const FieldInfo& fi) { return fi.name + "_struct_"; }
std::string StructAccMember(const FieldInfo& fi) { return fi.name + "_acc_"; }
std::string ListMember(const FieldInfo& fi) { return fi.name + "_list_"; }
std::string ListValuesMember(const FieldInfo& fi) { return fi.name + "_values_"; }
std::string ListInnerMember(const FieldInfo& fi) { return fi.name + "_inner_"; }
// MAP members.
std::string MapMember(const FieldInfo& fi) { return fi.name + "_map_"; }
std::string MapKeysMember(const FieldInfo& fi) { return fi.name + "_keys_"; }
std::string MapValuesMember(const FieldInfo& fi) { return fi.name + "_values_"; }
std::string MapInnerMember(const FieldInfo& fi) { return fi.name + "_inner_"; }
// NESTED_LIST members (outer list + intermediate list levels + explicit leaf
// StructArray handle + leaf accessor).
std::string NlOuterMember(const FieldInfo& fi) { return fi.name + "_list_"; }
std::string NlMidMember(const FieldInfo& fi) { return fi.name + "_mid_"; }
std::string NlInnerMember(const FieldInfo& fi) { return fi.name + "_inner_lists_"; }
std::string NlLeafValuesMember(const FieldInfo& fi) { return fi.name + "_leaf_values_"; }
std::string NlLeafMember(const FieldInfo& fi) { return fi.name + "_leaf_"; }

// The fletcher::ScalarMapSpan<...> type expression for a scalar-value map field.
std::string ScalarMapSpanType(const FieldInfo& fi) {
    const ScalarArrayInfo* k = LookupScalarArray(fi.mapping.map_key.arrow_type_expr);
    const ScalarArrayInfo* v = LookupScalarArray(fi.mapping.map_value.arrow_type_expr);
    return "fletcher::ScalarMapSpan<" + k->value_type + ", " + k->concrete_array + ", " +
           v->value_type + ", " + v->concrete_array + ", " + (k->use_get_view ? "true" : "false") +
           ", " + (v->use_get_view ? "true" : "false") + ">";
}

// The fletcher::StructMapSpan<...> type expression for a message-value map field.
std::string StructMapSpanType(const FieldInfo& fi) {
    const ScalarArrayInfo* k = LookupScalarArray(fi.mapping.map_key.arrow_type_expr);
    const std::string acc = AccessorRef(fi.mapping.map_value_class);
    return "fletcher::StructMapSpan<" + k->value_type + ", " + k->concrete_array + ", " + acc +
           ", " + (k->use_get_view ? "true" : "false") + ">";
}

// The fletcher::NestedStructSpan<AccT, Depth> type expression.
std::string NestedSpanType(const FieldInfo& fi) {
    const std::string acc = AccessorRef(fi.mapping.nested_class);
    return "fletcher::NestedStructSpan<" + acc + ", " + std::to_string(fi.mapping.list_depth) + ">";
}

// Emit one field's private storage members.
void EmitFieldStorage(std::ostringstream& o, const FieldInfo& fi) {
    switch (fi.mapping.kind) {
        case FieldKind::SCALAR: {
            const ScalarArrayInfo* sa = LookupScalarArray(fi.mapping.scalar.arrow_type_expr);
            o << "  std::shared_ptr<" << sa->concrete_array << "> " << fi.name << "_;\n";
            break;
        }
        case FieldKind::STRUCT: {
            const std::string acc = AccessorRef(fi.mapping.nested_class);
            // Cache the whole struct column (row-level validity for the nullable
            // 1:1 getter) AND the composed inner accessor (owns child columns,
            // supplies RowView).
            o << "  std::shared_ptr<arrow::StructArray> " << StructMember(fi) << ";\n"
              << "  std::optional<" << acc << "> " << StructAccMember(fi) << ";\n";
            break;
        }
        case FieldKind::REPEATED_SCALAR: {
            const ScalarArrayInfo* sa = LookupScalarArray(fi.mapping.element.arrow_type_expr);
            o << "  std::shared_ptr<arrow::ListArray> " << ListMember(fi) << ";\n"
              << "  std::shared_ptr<" << sa->concrete_array << "> " << ListValuesMember(fi)
              << ";\n";
            break;
        }
        case FieldKind::REPEATED_STRUCT: {
            const std::string acc = AccessorRef(fi.mapping.nested_class);
            // Cache the list, an explicit handle on the flattened values
            // StructArray (design's explicit-handle model), and the inner accessor.
            o << "  std::shared_ptr<arrow::ListArray> " << ListMember(fi) << ";\n"
              << "  std::shared_ptr<arrow::StructArray> " << ListValuesMember(fi) << ";\n"
              << "  std::optional<" << acc << "> " << ListInnerMember(fi) << ";\n";
            break;
        }
        case FieldKind::MAP: {
            const ScalarArrayInfo* k = LookupScalarArray(fi.mapping.map_key.arrow_type_expr);
            o << "  std::shared_ptr<arrow::MapArray> " << MapMember(fi) << ";\n"
              << "  std::shared_ptr<" << k->concrete_array << "> " << MapKeysMember(fi) << ";\n";
            if (fi.mapping.map_value_is_message) {
                const std::string acc = AccessorRef(fi.mapping.map_value_class);
                // Explicit handle on the flattened item StructArray + inner accessor.
                o << "  std::shared_ptr<arrow::StructArray> " << MapValuesMember(fi) << ";\n"
                  << "  std::optional<" << acc << "> " << MapInnerMember(fi) << ";\n";
            } else {
                const ScalarArrayInfo* v = LookupScalarArray(fi.mapping.map_value.arrow_type_expr);
                o << "  std::shared_ptr<" << v->concrete_array << "> " << MapValuesMember(fi)
                  << ";\n";
            }
            break;
        }
        case FieldKind::NESTED_LIST: {
            const std::string acc = AccessorRef(fi.mapping.nested_class);
            // Outer list + (depth-1) intermediate list levels + an explicit handle
            // on the flattened leaf StructArray + leaf accessor.
            o << "  std::shared_ptr<arrow::ListArray> " << NlOuterMember(fi) << ";\n";
            if (fi.mapping.list_depth == 3)
                o << "  std::shared_ptr<arrow::ListArray> " << NlMidMember(fi) << ";\n";
            o << "  std::shared_ptr<arrow::ListArray> " << NlInnerMember(fi) << ";\n"
              << "  std::shared_ptr<arrow::StructArray> " << NlLeafValuesMember(fi) << ";\n"
              << "  std::optional<" << acc << "> " << NlLeafMember(fi) << ";\n";
            break;
        }
    }
}

// Emit the public getter(s) for one supported field.
void EmitFieldGetter(std::ostringstream& o, const FieldInfo& fi) {
    switch (fi.mapping.kind) {
        case FieldKind::SCALAR: {
            const ScalarArrayInfo* sa = LookupScalarArray(fi.mapping.scalar.arrow_type_expr);
            const std::string read = sa->use_get_view ? "GetView(row)" : "Value(row)";
            if (fi.mapping.nullable) {
                o << "  std::optional<" << sa->value_type << "> " << fi.name
                  << "(int64_t row) const {\n"
                  << "    if (" << fi.name << "_->IsNull(row)) return std::nullopt;\n"
                  << "    return " << fi.name << "_->" << read << ";\n"
                  << "  }\n";
            } else {
                o << "  " << sa->value_type << " " << fi.name << "(int64_t row) const { return "
                  << fi.name << "_->" << read << "; }\n";
            }
            break;
        }
        case FieldKind::STRUCT: {
            const std::string acc = AccessorRef(fi.mapping.nested_class);
            if (fi.mapping.nullable) {
                // B2 no-read-through-null: check the parent struct column's
                // validity before returning the inner row view.
                o << "  std::optional<" << acc << "::RowView> " << fi.name
                  << "(int64_t row) const {\n"
                  << "    if (" << StructMember(fi) << "->IsNull(row)) return std::nullopt;\n"
                  << "    return " << StructAccMember(fi) << "->at(row);\n"
                  << "  }\n";
            } else {
                o << "  " << acc << "::RowView " << fi.name << "(int64_t row) const {\n"
                  << "    return " << StructAccMember(fi) << "->at(row);\n"
                  << "  }\n";
            }
            break;
        }
        case FieldKind::REPEATED_SCALAR: {
            const ScalarArrayInfo* sa = LookupScalarArray(fi.mapping.element.arrow_type_expr);
            const std::string span = "fletcher::ScalarSpan<" + sa->value_type + ", " +
                                     sa->concrete_array + ", " +
                                     (sa->use_get_view ? "true" : "false") + ">";
            const std::string body = "{" + ListValuesMember(fi) + ".get(), " + ListMember(fi) +
                                     "->value_offset(row), " + ListMember(fi) +
                                     "->value_length(row)}";
            if (fi.mapping.nullable) {
                // Row-nullable list: a null list row is distinct from an empty
                // list — return std::nullopt, never read through the null (B2).
                o << "  std::optional<" << span << "> " << fi.name << "(int64_t row) const {\n"
                  << "    if (" << ListMember(fi) << "->IsNull(row)) return std::nullopt;\n"
                  << "    return " << span << body << ";\n"
                  << "  }\n";
            } else {
                o << "  " << span << " " << fi.name << "(int64_t row) const {\n"
                  << "    return " << body << ";\n"
                  << "  }\n";
            }
            break;
        }
        case FieldKind::REPEATED_STRUCT: {
            const std::string acc = AccessorRef(fi.mapping.nested_class);
            const std::string span = "fletcher::StructSpan<" + acc + ">";
            const std::string body = "{&*" + ListInnerMember(fi) + ", " + ListMember(fi) +
                                     "->value_offset(row), " + ListMember(fi) +
                                     "->value_length(row)}";
            if (fi.mapping.nullable) {
                // Row-nullable list: null list row -> std::nullopt (B2). Distinct
                // from an empty (size 0) list.
                o << "  std::optional<" << span << "> " << fi.name << "(int64_t row) const {\n"
                  << "    if (" << ListMember(fi) << "->IsNull(row)) return std::nullopt;\n"
                  << "    return " << span << body << ";\n"
                  << "  }\n";
            } else {
                o << "  " << span << " " << fi.name << "(int64_t row) const {\n"
                  << "    return " << body << ";\n"
                  << "  }\n";
            }
            break;
        }
        case FieldKind::MAP: {
            std::string span;
            std::string body;
            if (fi.mapping.map_value_is_message) {
                span = StructMapSpanType(fi);
                body = "{" + MapKeysMember(fi) + ".get(), &*" + MapInnerMember(fi) + ", " +
                       MapMember(fi) + "->value_offset(row), " + MapMember(fi) +
                       "->value_length(row)}";
            } else {
                span = ScalarMapSpanType(fi);
                body = "{" + MapKeysMember(fi) + ".get(), " + MapValuesMember(fi) + ".get(), " +
                       MapMember(fi) + "->value_offset(row), " + MapMember(fi) +
                       "->value_length(row)}";
            }
            if (fi.mapping.nullable) {
                o << "  std::optional<" << span << "> " << fi.name << "(int64_t row) const {\n"
                  << "    if (" << MapMember(fi) << "->IsNull(row)) return std::nullopt;\n"
                  << "    return " << span << body << ";\n"
                  << "  }\n";
            } else {
                o << "  " << span << " " << fi.name << "(int64_t row) const {\n"
                  << "    return " << body << ";\n"
                  << "  }\n";
            }
            break;
        }
        case FieldKind::NESTED_LIST: {
            const std::string span = NestedSpanType(fi);
            std::string body;
            if (fi.mapping.list_depth == 3) {
                body = "{" + NlMidMember(fi) + ".get(), " + NlInnerMember(fi) + ".get(), &*" +
                       NlLeafMember(fi) + ", " + NlOuterMember(fi) + "->value_offset(row), " +
                       NlOuterMember(fi) + "->value_length(row)}";
            } else {
                body = "{" + NlInnerMember(fi) + ".get(), &*" + NlLeafMember(fi) + ", " +
                       NlOuterMember(fi) + "->value_offset(row), " + NlOuterMember(fi) +
                       "->value_length(row)}";
            }
            if (fi.mapping.nullable) {
                o << "  std::optional<" << span << "> " << fi.name << "(int64_t row) const {\n"
                  << "    if (" << NlOuterMember(fi) << "->IsNull(row)) return std::nullopt;\n"
                  << "    return " << span << body << ";\n"
                  << "  }\n";
            } else {
                o << "  " << span << " " << fi.name << "(int64_t row) const {\n"
                  << "    return " << body << ";\n"
                  << "  }\n";
            }
            break;
        }
    }
}

// Emit the RowView forwarder method for one supported field. The forwarder has
// no row argument: it forwards to the parent accessor getter at this->row.
void EmitRowViewForward(std::ostringstream& o, const FieldInfo& fi) {
    switch (fi.mapping.kind) {
        case FieldKind::SCALAR: {
            const ScalarArrayInfo* sa = LookupScalarArray(fi.mapping.scalar.arrow_type_expr);
            if (fi.mapping.nullable) {
                o << "    std::optional<" << sa->value_type << "> " << fi.name
                  << "() const { return a->" << fi.name << "(row); }\n";
            } else {
                o << "    " << sa->value_type << " " << fi.name << "() const { return a->"
                  << fi.name << "(row); }\n";
            }
            break;
        }
        case FieldKind::STRUCT: {
            const std::string acc = AccessorRef(fi.mapping.nested_class);
            if (fi.mapping.nullable) {
                o << "    std::optional<" << acc << "::RowView> " << fi.name
                  << "() const { return a->" << fi.name << "(row); }\n";
            } else {
                o << "    " << acc << "::RowView " << fi.name << "() const { return a->" << fi.name
                  << "(row); }\n";
            }
            break;
        }
        case FieldKind::REPEATED_SCALAR: {
            const ScalarArrayInfo* sa = LookupScalarArray(fi.mapping.element.arrow_type_expr);
            std::string ret = "fletcher::ScalarSpan<" + sa->value_type + ", " + sa->concrete_array +
                              ", " + (sa->use_get_view ? "true" : "false") + ">";
            if (fi.mapping.nullable) ret = "std::optional<" + ret + ">";
            o << "    " << ret << " " << fi.name << "() const { return a->" << fi.name
              << "(row); }\n";
            break;
        }
        case FieldKind::REPEATED_STRUCT: {
            const std::string acc = AccessorRef(fi.mapping.nested_class);
            std::string ret = "fletcher::StructSpan<" + acc + ">";
            if (fi.mapping.nullable) ret = "std::optional<" + ret + ">";
            o << "    " << ret << " " << fi.name << "() const { return a->" << fi.name
              << "(row); }\n";
            break;
        }
        case FieldKind::MAP: {
            std::string ret =
                fi.mapping.map_value_is_message ? StructMapSpanType(fi) : ScalarMapSpanType(fi);
            if (fi.mapping.nullable) ret = "std::optional<" + ret + ">";
            o << "    " << ret << " " << fi.name << "() const { return a->" << fi.name
              << "(row); }\n";
            break;
        }
        case FieldKind::NESTED_LIST: {
            std::string ret = NestedSpanType(fi);
            if (fi.mapping.nullable) ret = "std::optional<" + ret + ">";
            o << "    " << ret << " " << fi.name << "() const { return a->" << fi.name
              << "(row); }\n";
            break;
        }
    }
}

// Emit the FromColumns_ validation + cache block for one supported field at
// column index `i`.
void EmitFieldFromColumns(std::ostringstream& o, const FieldInfo& fi, std::size_t i,
                          const std::string& cls) {
    const std::string where = cls + " column " + std::to_string(i) + " '" + fi.name + "'";
    o << "    {\n"
      << "      const auto& col = cols[" << i << "];\n"
      << "      if (col == nullptr)\n"
      << "        return arrow::Status::Invalid(\"" << where << ": null column\");\n";

    switch (fi.mapping.kind) {
        case FieldKind::SCALAR: {
            const ScalarArrayInfo* sa = LookupScalarArray(fi.mapping.scalar.arrow_type_expr);
            const std::string& expr = fi.mapping.scalar.arrow_type_expr;
            o << "      const auto expected_type = " << expr << ";\n"
              << "      if (!col->type()->Equals(*expected_type, /*check_metadata=*/false))\n"
              << "        return arrow::Status::Invalid(\n"
              << "            \"" << where << ": expected \", expected_type->ToString(), \", got "
              << "\",\n"
              << "            col->type()->ToString());\n";
            if (!fi.mapping.nullable) {
                o << "      if (col->null_count() != 0)\n"
                  << "        return arrow::Status::Invalid(\n"
                  << "            \"" << where << ": non-nullable, found \", col->null_count(), \" "
                  << "nulls\");\n";
            }
            o << "      self." << fi.name << "_ = std::static_pointer_cast<" << sa->concrete_array
              << ">(col);\n";
            break;
        }
        case FieldKind::STRUCT: {
            const std::string acc = AccessorRef(fi.mapping.nested_class);
            o << "      if (col->type_id() != arrow::Type::STRUCT)\n"
              << "        return arrow::Status::Invalid(\n"
              << "            \"" << where << ": expected struct, got \", col->type()->ToString());\n"
              << "      auto struct_col = std::static_pointer_cast<arrow::StructArray>(col);\n";
            if (!fi.mapping.nullable) {
                o << "      if (struct_col->null_count() != 0)\n"
                  << "        return arrow::Status::Invalid(\n"
                  << "            \"" << where << ": non-nullable, found \", "
                  << "struct_col->null_count(), \" nulls\");\n";
            }
            // Recurse: the inner accessor validates the struct's children
            // positionally and gates its own non-nullable children (D-RBA-4).
            o << "      ARROW_ASSIGN_OR_RAISE(auto inner, " << acc << "::Make(struct_col));\n"
              << "      self." << StructMember(fi) << " = struct_col;\n"
              << "      self." << StructAccMember(fi) << " = std::move(inner);\n";
            break;
        }
        case FieldKind::REPEATED_SCALAR: {
            const ScalarArrayInfo* sa = LookupScalarArray(fi.mapping.element.arrow_type_expr);
            const std::string& expr = fi.mapping.element.arrow_type_expr;
            o << "      if (col->type_id() != arrow::Type::LIST)\n"
              << "        return arrow::Status::Invalid(\n"
              << "            \"" << where << ": expected list, got \", col->type()->ToString());\n"
              << "      auto list = std::static_pointer_cast<arrow::ListArray>(col);\n";
            if (!fi.mapping.nullable) {
                o << "      if (list->null_count() != 0)\n"
                  << "        return arrow::Status::Invalid(\n"
                  << "            \"" << where << ": non-nullable, found \", list->null_count(), "
                  << "\" nulls\");\n";
            }
            o << "      const auto expected_value_type = " << expr << ";\n"
              << "      auto values = list->values();\n"
              << "      if (!values->type()->Equals(*expected_value_type, /*check_metadata=*/"
              << "false))\n"
              << "        return arrow::Status::Invalid(\n"
              << "            \"" << where << " values: expected \", "
              << "expected_value_type->ToString(),\n"
              << "            \", got \", values->type()->ToString());\n"
              << "      self." << ListMember(fi) << " = list;\n"
              << "      self." << ListValuesMember(fi) << " = std::static_pointer_cast<"
              << sa->concrete_array << ">(values);\n";
            break;
        }
        case FieldKind::REPEATED_STRUCT: {
            const std::string acc = AccessorRef(fi.mapping.nested_class);
            o << "      if (col->type_id() != arrow::Type::LIST)\n"
              << "        return arrow::Status::Invalid(\n"
              << "            \"" << where << ": expected list, got \", col->type()->ToString());\n"
              << "      auto list = std::static_pointer_cast<arrow::ListArray>(col);\n";
            if (!fi.mapping.nullable) {
                o << "      if (list->null_count() != 0)\n"
                  << "        return arrow::Status::Invalid(\n"
                  << "            \"" << where << ": non-nullable, found \", list->null_count(), "
                  << "\" nulls\");\n";
            }
            // The inner accessor is built over the FULL flattened values
            // StructArray (offset-0 origin); per-row windows are addressed via
            // value_offset(row)+j by the StructSpan getter (coordinate-consistent).
            o << "      auto values = list->values();\n"
              << "      if (values->type_id() != arrow::Type::STRUCT)\n"
              << "        return arrow::Status::Invalid(\n"
              << "            \"" << where << " values: expected struct, got \", "
              << "values->type()->ToString());\n"
              << "      auto structs = std::static_pointer_cast<arrow::StructArray>(values);\n"
              << "      ARROW_ASSIGN_OR_RAISE(auto inner, " << acc << "::Make(structs));\n"
              << "      self." << ListMember(fi) << " = list;\n"
              << "      self." << ListValuesMember(fi) << " = structs;\n"
              << "      self." << ListInnerMember(fi) << " = std::move(inner);\n";
            break;
        }
        case FieldKind::MAP: {
            const ScalarArrayInfo* k = LookupScalarArray(fi.mapping.map_key.arrow_type_expr);
            o << "      if (col->type_id() != arrow::Type::MAP)\n"
              << "        return arrow::Status::Invalid(\n"
              << "            \"" << where << ": expected map, got \", col->type()->ToString());\n"
              << "      auto map = std::static_pointer_cast<arrow::MapArray>(col);\n";
            if (!fi.mapping.nullable) {
                o << "      if (map->null_count() != 0)\n"
                  << "        return arrow::Status::Invalid(\n"
                  << "            \"" << where << ": non-nullable, found \", map->null_count(), "
                  << "\" nulls\");\n";
            }
            // Validate the flattened key leaf type (Arrow map keys are
            // non-nullable by spec — no null gate here).
            o << "      const auto expected_key_type = " << fi.mapping.map_key.arrow_type_expr
              << ";\n"
              << "      auto keys = map->keys();\n"
              << "      if (!keys->type()->Equals(*expected_key_type, /*check_metadata=*/false))\n"
              << "        return arrow::Status::Invalid(\n"
              << "            \"" << where << " keys: expected \", expected_key_type->ToString(),\n"
              << "            \", got \", keys->type()->ToString());\n"
              << "      auto items = map->items();\n";
            if (fi.mapping.map_value_is_message) {
                const std::string acc = AccessorRef(fi.mapping.map_value_class);
                o << "      if (items->type_id() != arrow::Type::STRUCT)\n"
                  << "        return arrow::Status::Invalid(\n"
                  << "            \"" << where << " values: expected struct, got \", "
                  << "items->type()->ToString());\n"
                  << "      auto item_structs = std::static_pointer_cast<arrow::StructArray>(items);"
                  << "\n"
                  << "      ARROW_ASSIGN_OR_RAISE(auto inner, " << acc << "::Make(item_structs));\n"
                  << "      self." << MapMember(fi) << " = map;\n"
                  << "      self." << MapKeysMember(fi) << " = std::static_pointer_cast<"
                  << k->concrete_array << ">(keys);\n"
                  << "      self." << MapValuesMember(fi) << " = item_structs;\n"
                  << "      self." << MapInnerMember(fi) << " = std::move(inner);\n";
            } else {
                const ScalarArrayInfo* v = LookupScalarArray(fi.mapping.map_value.arrow_type_expr);
                o << "      const auto expected_value_type = "
                  << fi.mapping.map_value.arrow_type_expr << ";\n"
                  << "      if (!items->type()->Equals(*expected_value_type, /*check_metadata=*/"
                  << "false))\n"
                  << "        return arrow::Status::Invalid(\n"
                  << "            \"" << where << " values: expected \", "
                  << "expected_value_type->ToString(),\n"
                  << "            \", got \", items->type()->ToString());\n"
                  << "      self." << MapMember(fi) << " = map;\n"
                  << "      self." << MapKeysMember(fi) << " = std::static_pointer_cast<"
                  << k->concrete_array << ">(keys);\n"
                  << "      self." << MapValuesMember(fi) << " = std::static_pointer_cast<"
                  << v->concrete_array << ">(items);\n";
            }
            break;
        }
        case FieldKind::NESTED_LIST: {
            const std::string acc = AccessorRef(fi.mapping.nested_class);
            // Outer list level (= this column). Gate the outer null count only;
            // inner list levels are exposed as nullable in the access API.
            o << "      if (col->type_id() != arrow::Type::LIST)\n"
              << "        return arrow::Status::Invalid(\n"
              << "            \"" << where << ": expected list, got \", col->type()->ToString());\n"
              << "      auto outer_list = std::static_pointer_cast<arrow::ListArray>(col);\n";
            if (!fi.mapping.nullable) {
                o << "      if (outer_list->null_count() != 0)\n"
                  << "        return arrow::Status::Invalid(\n"
                  << "            \"" << where << ": non-nullable, found \", "
                  << "outer_list->null_count(), \" nulls\");\n";
            }
            o << "      self." << NlOuterMember(fi) << " = outer_list;\n";
            if (fi.mapping.list_depth == 3) {
                // outer -> mid (list) -> inner (list) -> leaf (struct)
                o << "      auto mid_vals = outer_list->values();\n"
                  << "      if (mid_vals->type_id() != arrow::Type::LIST)\n"
                  << "        return arrow::Status::Invalid(\n"
                  << "            \"" << where << " level 2: expected list, got \", "
                  << "mid_vals->type()->ToString());\n"
                  << "      auto mid_list = std::static_pointer_cast<arrow::ListArray>(mid_vals);\n"
                  << "      self." << NlMidMember(fi) << " = mid_list;\n"
                  << "      auto inner_vals = mid_list->values();\n"
                  << "      if (inner_vals->type_id() != arrow::Type::LIST)\n"
                  << "        return arrow::Status::Invalid(\n"
                  << "            \"" << where << " level 3: expected list, got \", "
                  << "inner_vals->type()->ToString());\n"
                  << "      auto inner_list = std::static_pointer_cast<arrow::ListArray>("
                  << "inner_vals);\n"
                  << "      self." << NlInnerMember(fi) << " = inner_list;\n"
                  << "      auto leaf_vals = inner_list->values();\n";
            } else {
                // outer -> inner (list) -> leaf (struct)
                o << "      auto inner_vals = outer_list->values();\n"
                  << "      if (inner_vals->type_id() != arrow::Type::LIST)\n"
                  << "        return arrow::Status::Invalid(\n"
                  << "            \"" << where << " level 2: expected list, got \", "
                  << "inner_vals->type()->ToString());\n"
                  << "      auto inner_list = std::static_pointer_cast<arrow::ListArray>("
                  << "inner_vals);\n"
                  << "      self." << NlInnerMember(fi) << " = inner_list;\n"
                  << "      auto leaf_vals = inner_list->values();\n";
            }
            // Explicit handle on the fully flattened leaf StructArray + the leaf
            // accessor built over that same member (design's explicit-handle model).
            o << "      if (leaf_vals->type_id() != arrow::Type::STRUCT)\n"
              << "        return arrow::Status::Invalid(\n"
              << "            \"" << where << " leaf: expected struct, got \", "
              << "leaf_vals->type()->ToString());\n"
              << "      auto leaf_structs = std::static_pointer_cast<arrow::StructArray>("
              << "leaf_vals);\n"
              << "      ARROW_ASSIGN_OR_RAISE(auto leaf, " << acc << "::Make(leaf_structs));\n"
              << "      self." << NlLeafValuesMember(fi) << " = leaf_structs;\n"
              << "      self." << NlLeafMember(fi) << " = std::move(leaf);\n";
            break;
        }
    }
    o << "    }\n";
}

// Cross-file generated-ACCESSOR #include paths for the file. Reuses the single
// canonical cross-file include discovery (CollectCrossFileIncludes, declared in
// generator_internal.hpp, namespace fletcher) read-only — NO fork of the import
// walk (D-RBA-10 single source of truth) — and rewrites each dependency-header
// suffix `.fletcher.pb.h` -> `.fletcher.accessor.pb.h` on its returned set.
std::set<std::string> CollectAccessorCrossFileIncludes(
    const google::protobuf::FileDescriptor* file) {
    const std::set<std::string> pb_headers = CollectCrossFileIncludes(file);

    std::set<std::string> acc_headers;
    constexpr std::string_view kSuffix = ".fletcher.pb.h";
    constexpr std::string_view kAccessorSuffix = ".fletcher.accessor.pb.h";
    for (const auto& h : pb_headers) {
        if (h.size() >= kSuffix.size() &&
            h.compare(h.size() - kSuffix.size(), kSuffix.size(), kSuffix) == 0) {
            acc_headers.insert(h.substr(0, h.size() - kSuffix.size()) +
                               std::string(kAccessorSuffix));
        } else {
            acc_headers.insert(h);  // defensive: keep as-is if shape is unexpected
        }
    }
    return acc_headers;
}

}  // namespace

std::string EmitAccessorHeader(const google::protobuf::FileDescriptor* file) {
    std::ostringstream o;

    o << "// Generated by fletcher-protoc. DO NOT EDIT.\n"
      << "// Source: " << file->name() << "\n"
      << "// RecordBatch accessor header (read-only). Emitted by --fletcher_opt=accessor.\n"
      << "#pragma once\n\n";

    // Decide which headers this file needs: the span library (any collection
    // field) and cross-file generated accessor headers (any cross-file nested
    // message / map message value).
    bool needs_spans = false;
    for (const auto* msg : OrderedMessages(file)) {
        if (IsRecursive(msg) || IsFlattenedWrapper(msg)) continue;
        std::string skipped;
        for (const auto& fi : GatherFields(msg, &skipped)) {
            switch (fi.mapping.kind) {
                case FieldKind::REPEATED_SCALAR:
                case FieldKind::REPEATED_STRUCT:
                case FieldKind::MAP:
                case FieldKind::NESTED_LIST:
                    needs_spans = true;
                    break;
                default:
                    break;
            }
        }
    }
    const std::set<std::string> cross_includes = CollectAccessorCrossFileIncludes(file);

    o << "#include <arrow/api.h>\n";
    if (needs_spans) o << "#include <fletcher/arrow_bridge/recordbatch_spans.hpp>\n";
    o << "\n"
      << "#include <cstdint>\n"
      << "#include <memory>\n"
      << "#include <optional>\n"
      << "#include <string>\n"
      << "#include <string_view>\n"
      << "#include <utility>\n"
      << "#include <vector>\n";
    if (!cross_includes.empty()) {
        o << "\n";
        for (const auto& h : cross_includes) o << "#include \"" << h << "\"\n";
    }
    o << "\n";

    o << "namespace fletcher_gen {\n";
    const std::string ns = PackageToNamespace(file->package());
    if (!ns.empty()) o << "namespace " << ns << " {\n";
    o << "\n";

    for (const auto* msg : OrderedMessages(file)) {
        if (IsRecursive(msg) || IsFlattenedWrapper(msg)) continue;

        std::string skipped;
        const std::vector<FieldInfo> fields = GatherFields(msg, &skipped);
        const std::string cls = ClassName(msg);
        const std::string acc = cls + "Accessor";

        // A message with any field the accessor still cannot type-validate (an
        // unmapped scalar leaf, an unsupported map key/value leaf, or a nested
        // list at an unsupported depth) cannot be safely constructed; FromColumns_
        // fails fast at the first such column so Make() never exposes a bypassable
        // validation gate (D-RBA-4). Supported getters before it still compile;
        // they are simply unreachable. RBA-4 supports STRUCT / REPEATED_SCALAR /
        // REPEATED_STRUCT / MAP / NESTED_LIST (depth 2-3).
        std::size_t first_unsupported = fields.size();
        for (std::size_t i = 0; i < fields.size(); ++i) {
            if (!IsSupportedField(fields[i])) {
                first_unsupported = i;
                break;
            }
        }
        const bool has_unsupported = first_unsupported < fields.size();

        o << "// RecordBatch accessor for " << msg->name() << ".\n"
          << "// Validation is positional + type-only (names and the nullable flag are\n"
          << "// tolerated); proto-non-nullable columns additionally require null_count()==0.\n"
          << "class " << acc << " {\n"
          << " public:\n";

        // Factories.
        o << "  static arrow::Result<" << acc << "> Make(\n"
          << "      const std::shared_ptr<arrow::RecordBatch>& batch) {\n"
          << "    if (batch == nullptr)\n"
          << "      return arrow::Status::Invalid(\"" << cls << ": null RecordBatch\");\n"
          << "    return FromColumns_(batch->num_rows(), batch->columns(),\n"
          << "                        batch->schema()->fields(), batch->schema()->metadata());\n"
          << "  }\n\n";

        o << "  static arrow::Result<" << acc << "> Make(\n"
          << "      const std::shared_ptr<arrow::StructArray>& struct_array) {\n"
          << "    if (struct_array == nullptr)\n"
          << "      return arrow::Status::Invalid(\"" << cls << ": null StructArray\");\n"
          << "    const int64_t length = struct_array->length();\n"
          << "    const auto& st =\n"
          << "        *std::static_pointer_cast<arrow::StructType>(struct_array->type());\n"
          << "    // StructArray::field(i) returns each child with its offset, length and\n"
          << "    // null count already adjusted to the struct's [offset, offset+length)\n"
          << "    // window (Arrow C++ semantics), so the children are correctly sliced\n"
          << "    // without an extra manual Slice. The cached child handles share the\n"
          << "    // source buffers and keep them alive after the caller drops the struct.\n"
          << "    arrow::ArrayVector cols;\n"
          << "    cols.reserve(struct_array->num_fields());\n"
          << "    for (int i = 0; i < struct_array->num_fields(); ++i)\n"
          << "      cols.push_back(struct_array->field(i));\n"
          << "    ARROW_ASSIGN_OR_RAISE(\n"
          << "        auto self,\n"
          << "        FromColumns_(length, cols, st.fields(), /*schema_metadata=*/nullptr));\n"
          << "    // Retain the whole StructArray so is_null(row) reflects struct-element\n"
          << "    // validity. The cached field(i) children and this validity bitmap share\n"
          << "    // one struct-logical coordinate origin (Arrow windows field(i) to\n"
          << "    // [offset,len)), so both index by the same row — no re-Slice.\n"
          << "    self.struct_validity_ = struct_array;\n"
          << "    return self;\n"
          << "  }\n\n";

        // num_rows.
        o << "  int64_t num_rows() const { return num_rows_; }\n\n";

        // RBA-4 / D-RBA-7: per-row struct-element null probe. Always emitted.
        // Null (false) when built from a RecordBatch (no struct validity bitmap);
        // reflects the retained StructArray validity when struct-sourced.
        o << "  // True iff this row is a null struct element. Always false for a\n"
          << "  // RecordBatch-sourced accessor (no top-level struct validity bitmap);\n"
          << "  // reflects the retained StructArray's validity when struct-sourced.\n"
          << "  bool is_null(int64_t row) const {\n"
          << "    return struct_validity_ != nullptr && struct_validity_->IsNull(row);\n"
          << "  }\n\n";

        // RBA-3 generic metadata getters (unchanged).
        o << "  // Schema-level metadata, borrowed for the accessor's lifetime.\n"
          << "  // Returns nullptr when absent — this is NOT an error. Absent means\n"
          << "  // either a struct-sourced accessor (no top-level arrow::Schema) or a\n"
          << "  // RecordBatch whose schema carries no metadata. Callers MUST null-check\n"
          << "  // before ->Contains(...)/->Get(...) rather than dereferencing.\n"
          << "  const arrow::KeyValueMetadata* schema_metadata() const {\n"
          << "    return schema_metadata_.get();\n"
          << "  }\n\n";

        o << "  // Canonical field-metadata getter, by positional index (aligns with\n"
          << "  // the positional validation contract; does not depend on field names).\n"
          << "  // Returns nullptr on out-of-bounds index, a null stored field, or a\n"
          << "  // field that simply carries no metadata — none of which is an error.\n"
          << "  const arrow::KeyValueMetadata* field_metadata(int i) const {\n"
          << "    if (i < 0 || static_cast<std::size_t>(i) >= fields_.size()) return nullptr;\n"
          << "    if (fields_[i] == nullptr) return nullptr;\n"
          << "    // arrow::Field::metadata() returns a *copy* of the field's member\n"
          << "    // shared_ptr (by value). The KeyValueMetadata it points to survives\n"
          << "    // this full expression only because fields_[i] (a shared_ptr<Field>\n"
          << "    // kept by this accessor) co-owns it via the field's own member — NOT\n"
          << "    // because the returned temporary survives. Do not 'simplify' by\n"
          << "    // storing the temporary's .get() past end-of-expression: that would\n"
          << "    // dangle.\n"
          << "    return fields_[i]->metadata().get();\n"
          << "  }\n\n";

        o << "  // Convenience field-metadata getter, by LIVE Arrow field name. This is\n"
          << "  // a best-effort linear scan over the stored fields: because RBA\n"
          << "  // construction is name-tolerant, the runtime names may differ from the\n"
          << "  // generated proto names. Code that must be robust to name drift should\n"
          << "  // use field_metadata(int). Unknown name -> nullptr (not an error); on\n"
          << "  // duplicate live names the first match in field order wins.\n"
          << "  const arrow::KeyValueMetadata* field_metadata(const std::string& name) const {\n"
          << "    for (const auto& field : fields_) {\n"
          << "      // See field_metadata(int): the returned pointer is kept alive by\n"
          << "      // 'field' (co-owned by fields_), not by the by-value metadata copy.\n"
          << "      if (field != nullptr && field->name() == name) return field->metadata().get();\n"
          << "    }\n"
          << "    return nullptr;\n"
          << "  }\n\n";

        // Lifetime note for callers (string/binary getters + borrowed spans/views).
        o << "  // NOTE: utf8/binary getters return std::string_view, and collection\n"
          << "  // getters return small span objects (ScalarSpan/StructSpan) that borrow\n"
          << "  // the cached column buffers / inner accessors owned by this accessor.\n"
          << "  // Spans and RowViews are valid only while this accessor is alive; copy\n"
          << "  // out anything that must outlive it. Numeric/bool/temporal scalar\n"
          << "  // getters return by value (no borrow).\n";

        // Getters.
        for (const auto& fi : fields) {
            if (IsSupportedField(fi)) {
                EmitFieldGetter(o, fi);
            } else {
                // An as-yet unmapped/unsupported construct (unsupported scalar
                // leaf, unsupported map key/value leaf, or a nested list at an
                // unsupported depth): keep an explicit generation-time comment
                // (D-RBA-6 no silent gap) and emit no getter.
                o << "  // Field '" << fi.name
                  << "' is not supported by the RBA accessor; no getter emitted.\n";
            }
        }
        o << "\n";

        // RowView: a borrowed two-word forwarder over (accessor*, row). Forwards
        // every emitted getter with no row argument.
        o << "  // Borrowed per-row view: a two-word forwarder {accessor*, row}. Valid\n"
          << "  // only while *this accessor is alive (it owns no arrays). Composes\n"
          << "  // recursively: a struct field forwards to the inner accessor's RowView.\n"
          << "  struct RowView {\n"
          << "    const " << acc << "* a = nullptr;\n"
          << "    int64_t row = 0;\n\n";
        for (const auto& fi : fields) {
            if (IsSupportedField(fi)) EmitRowViewForward(o, fi);
        }
        o << "  };\n\n";

        o << "  RowView at(int64_t row) const { return RowView{this, row}; }\n\n";

        // Private section: default ctor, FromColumns_, storage.
        o << " private:\n";
        o << "  " << acc << "() = default;\n\n";

        o << "  static arrow::Result<" << acc << "> FromColumns_(\n"
          << "      int64_t num_rows, const arrow::ArrayVector& cols, arrow::FieldVector fields,\n"
          << "      std::shared_ptr<const arrow::KeyValueMetadata> schema_metadata) {\n";

        if (has_unsupported) {
            const auto& bad = fields[first_unsupported];
            // The accessor cannot type-validate this column's leaf/shape, so it
            // refuses to construct rather than expose a bypassable gate (D-RBA-4).
            o << "    // Field '" << bad.name << "' has a construct the RBA accessor cannot\n"
              << "    // type-validate (unsupported leaf type or list depth) — fail fast so\n"
              << "    // Make() never exposes a bypassable validation gate (D-RBA-4).\n"
              << "    return arrow::Status::Invalid(\n"
              << "        \"" << cls << " column " << first_unsupported << " '" << bad.name
              << "': unsupported column construct for the RBA accessor\");\n"
              << "  }\n\n";
            // Emit storage (incl. struct_validity_) so the class is well-formed.
            o << "  int64_t num_rows_ = 0;\n"
              << "  arrow::FieldVector fields_;\n"
              << "  std::shared_ptr<const arrow::KeyValueMetadata> schema_metadata_;\n"
              << "  std::shared_ptr<arrow::StructArray> struct_validity_;\n";
            for (const auto& fi : fields) {
                if (IsSupportedField(fi)) EmitFieldStorage(o, fi);
            }
            o << "};\n\n";
            continue;
        }

        // Count gate. Every mapped field occupies one schema column.
        const std::size_t expected = fields.size();
        o << "    constexpr int64_t kExpectedColumns = " << expected << ";\n"
          << "    if (static_cast<int64_t>(cols.size()) != kExpectedColumns)\n"
          << "      return arrow::Status::Invalid(\"" << cls
          << ": expected \", kExpectedColumns, \" columns, got \", cols.size());\n\n";

        o << "    " << acc << " self;\n"
          << "    self.num_rows_ = num_rows;\n"
          << "    self.fields_ = std::move(fields);\n"
          << "    self.schema_metadata_ = std::move(schema_metadata);\n\n";

        for (std::size_t i = 0; i < fields.size(); ++i) {
            EmitFieldFromColumns(o, fields[i], i, cls);
        }

        o << "    return self;\n"
          << "  }\n\n";

        // Storage.
        o << "  int64_t num_rows_ = 0;\n"
          << "  arrow::FieldVector fields_;\n"
          << "  std::shared_ptr<const arrow::KeyValueMetadata> schema_metadata_;\n"
          << "  // Retained whole StructArray for is_null(row); null for RecordBatch source.\n"
          << "  std::shared_ptr<arrow::StructArray> struct_validity_;\n";
        for (const auto& fi : fields) {
            EmitFieldStorage(o, fi);
        }

        o << "};\n\n";
    }

    if (!ns.empty()) o << "}  // namespace " << ns << "\n";
    o << "}  // namespace fletcher_gen\n";

    return o.str();
}

// ===========================================================================
// RBA-5 — Rust scalar accessor emitter
// ===========================================================================
//
// The Rust emitter shares the exact same FieldInfo / type_mapper model as the
// C++ emitter (D-RBA-8). It emits a `<Class>Accessor` Rust struct + impl for
// every generated message whose fields are all supported scalars; a message
// with any composite field fails fast (RBA-6 adds composites). Generated files
// carry BARE items, no `mod <pkg>` wrapper — package mounting is owned by the
// RBA-5 build.rs assembler (D-RBA-10).

namespace {

// Mirrors the C++ ScalarArrayInfo (LookupScalarArray) one-for-one, but in
// arrow-rs terms: the concrete arrow-rs array class to cache as `Arc<T>`, the
// `expected` arrow-rs `DataType` constructor expression (the type-only gate
// target, D-RBA-4), and the Rust getter return type for non-null vs null.
struct RustScalarInfo {
    std::string array_type;  // e.g. "Float64Array", "TimestampNanosecondArray"
    std::string data_type;   // e.g. "DataType::Float64" — the type-only gate target
    std::string value_type;  // the non-null getter return, e.g. "f64", "&str", "&[u8]"
    bool is_view = false;    // utf8/binary: getter borrows (&str / &[u8]) via .value(row)
};

// Map a C++ TimeUnit enum token ("NANO"/"MICRO"/"MILLI"/"SECOND") to the
// arrow-rs TimeUnit variant ("Nanosecond"/...). Returns empty on an unknown
// token (treated as a generation error by the caller).
std::string RustTimeUnit(const std::string& cpp_unit) {
    if (cpp_unit == "NANO") return "Nanosecond";
    if (cpp_unit == "MICRO") return "Microsecond";
    if (cpp_unit == "MILLI") return "Millisecond";
    if (cpp_unit == "SECOND") return "Second";
    return std::string();
}

// Extract the inner argument list of an `arrow::timestamp(...)` /
// `arrow::duration(...)` expression: the substring between the first '(' and
// the matching ')'. The mapper today emits a single `arrow::TimeUnit::<UNIT>`
// argument (and, if ever present, a quoted timezone), so this parses generically
// over the unit (and an optional tz) rather than hardcoding NANO.
std::string TemporalArgs(const std::string& expr) {
    const auto lp = expr.find('(');
    const auto rp = expr.rfind(')');
    if (lp == std::string::npos || rp == std::string::npos || rp <= lp) return std::string();
    return expr.substr(lp + 1, rp - lp - 1);
}

// Pull the unit token (NANO/MICRO/MILLI/SECOND) out of a temporal arg list of
// the form "arrow::TimeUnit::NANO[, \"<tz>\"]".
std::string TemporalUnitToken(const std::string& args) {
    constexpr std::string_view kPrefix = "arrow::TimeUnit::";
    const auto pos = args.find(kPrefix);
    if (pos == std::string::npos) return std::string();
    auto start = pos + kPrefix.size();
    auto end = start;
    while (end < args.size() &&
           (std::isalnum(static_cast<unsigned char>(args[end])) || args[end] == '_')) {
        ++end;
    }
    return args.substr(start, end - start);
}

// Pull an optional timezone literal out of a timestamp arg list. The C++ form
// would be `arrow::timestamp(arrow::TimeUnit::NANO, "UTC")`; today the mapper
// emits no timezone, so this returns empty for every current field. When a tz
// is present, returns its contents WITHOUT the surrounding quotes.
std::string TemporalTimezone(const std::string& args) {
    const auto comma = args.find(',');
    if (comma == std::string::npos) return std::string();
    const auto q1 = args.find('"', comma);
    if (q1 == std::string::npos) return std::string();
    const auto q2 = args.find('"', q1 + 1);
    if (q2 == std::string::npos) return std::string();
    return args.substr(q1 + 1, q2 - q1 - 1);
}

// Fills *out with the RustScalarInfo for a scalar Arrow type expression and
// returns true, or returns false if the expression is not a supported scalar.
// Keyed on the same arrow_type_expr the C++ LookupScalarArray consumes, so the
// C++/Rust scalar sets stay in lockstep (D-RBA-8).
bool RustScalarFor(const std::string& expr, RustScalarInfo* out) {
    auto set = [&](const char* arr, const char* dt, const char* vt, bool view) {
        out->array_type = arr;
        out->data_type = dt;
        out->value_type = vt;
        out->is_view = view;
        return true;
    };
    // `data_type` is emitted as a fully-qualified arrow-rs `DataType` expression
    // (no `use`), so the generated code stays self-contained when same-package
    // files are co-mounted into one module (D-RBA-10).
    if (expr == "arrow::boolean()")
        return set("BooleanArray", "arrow::datatypes::DataType::Boolean", "bool", false);
    if (expr == "arrow::int8()")
        return set("Int8Array", "arrow::datatypes::DataType::Int8", "i8", false);
    if (expr == "arrow::int16()")
        return set("Int16Array", "arrow::datatypes::DataType::Int16", "i16", false);
    if (expr == "arrow::int32()")
        return set("Int32Array", "arrow::datatypes::DataType::Int32", "i32", false);
    if (expr == "arrow::int64()")
        return set("Int64Array", "arrow::datatypes::DataType::Int64", "i64", false);
    if (expr == "arrow::uint8()")
        return set("UInt8Array", "arrow::datatypes::DataType::UInt8", "u8", false);
    if (expr == "arrow::uint16()")
        return set("UInt16Array", "arrow::datatypes::DataType::UInt16", "u16", false);
    if (expr == "arrow::uint32()")
        return set("UInt32Array", "arrow::datatypes::DataType::UInt32", "u32", false);
    if (expr == "arrow::uint64()")
        return set("UInt64Array", "arrow::datatypes::DataType::UInt64", "u64", false);
    if (expr == "arrow::float32()")
        return set("Float32Array", "arrow::datatypes::DataType::Float32", "f32", false);
    if (expr == "arrow::float64()")
        return set("Float64Array", "arrow::datatypes::DataType::Float64", "f64", false);
    if (expr == "arrow::utf8()")
        return set("StringArray", "arrow::datatypes::DataType::Utf8", "&str", true);
    if (expr == "arrow::binary()")
        return set("BinaryArray", "arrow::datatypes::DataType::Binary", "&[u8]", true);

    // Timestamp / duration: derive the concrete array class + DataType from the
    // unit (and, for timestamps, an optional timezone) parsed out of the
    // expression — generic over the unit, never hardcoding NANO. Getter returns
    // the raw i64 (mirrors the C++ int64_t choice).
    if (expr.rfind("arrow::timestamp(", 0) == 0) {
        const std::string args = TemporalArgs(expr);
        const std::string unit = RustTimeUnit(TemporalUnitToken(args));
        if (unit.empty()) return false;
        const std::string tz = TemporalTimezone(args);
        out->array_type = "Timestamp" + unit + "Array";
        if (tz.empty()) {
            out->data_type =
                "arrow::datatypes::DataType::Timestamp(arrow::datatypes::TimeUnit::" + unit +
                ", None)";
        } else {
            out->data_type =
                "arrow::datatypes::DataType::Timestamp(arrow::datatypes::TimeUnit::" + unit +
                ", Some(\"" + tz + "\".into()))";
        }
        out->value_type = "i64";
        out->is_view = false;
        return true;
    }
    if (expr.rfind("arrow::duration(", 0) == 0) {
        const std::string args = TemporalArgs(expr);
        const std::string unit = RustTimeUnit(TemporalUnitToken(args));
        if (unit.empty()) return false;
        out->array_type = "Duration" + unit + "Array";
        out->data_type =
            "arrow::datatypes::DataType::Duration(arrow::datatypes::TimeUnit::" + unit + ")";
        out->value_type = "i64";
        out->is_view = false;
        return true;
    }
    return false;
}

// Rust keywords usable as RAW identifiers (`r#<kw>`). A proto field/segment that
// collides with one of these is emitted as r#<kw> (oracle §8.1 / D-RBA-10).
bool IsRawableRustKeyword(const std::string& s) {
    static const std::set<std::string> kKeywords = {
        "as",     "break",  "const",  "continue", "dyn",     "else",     "enum",
        "extern", "false",  "fn",     "for",      "if",      "impl",     "in",
        "let",    "loop",   "match",  "mod",      "move",    "mut",      "pub",
        "ref",    "return", "static", "struct",   "trait",   "true",     "type",
        "unsafe", "use",    "where",  "while",    "async",   "await",    "abstract",
        "become", "box",    "do",     "final",    "macro",   "override", "priv",
        "typeof", "unsized", "virtual", "yield",  "try",     "union"};
    return kKeywords.count(s) > 0;
}

// Rust keywords that CANNOT be raw identifiers (rustc rejects `r#crate` etc.). A
// proto field/segment equal to one of these is a generation error (D-RBA-10) —
// it is never silently renamed.
bool IsNonRawRustKeyword(const std::string& s) {
    return s == "crate" || s == "self" || s == "Self" || s == "super";
}

// Sanitize one identifier for Rust: a raw-able keyword becomes r#<id>; an
// otherwise-valid proto ident is unchanged. Callers MUST have already rejected
// non-raw keywords (IsNonRawRustKeyword) — this function asserts that contract
// by leaving such an input unchanged (which would fail to compile), but the
// per-message emission gates on it first and emits a compile_error! instead.
std::string RustIdent(const std::string& seg) {
    if (IsRawableRustKeyword(seg)) return "r#" + seg;
    return seg;
}

// Emit the per-impl `downcast_array` associated function (offset-preserving,
// type-gated). It is a PRIVATE associated fn of each accessor's `impl` (not a
// file-level free fn) so that co-mounting several same-package files into one
// module never produces a duplicate-definition error (D-RBA-10). It references
// arrow types by fully-qualified path so no `use` is needed.
void EmitRustDowncastAssoc(std::ostringstream& o) {
    o << "    /// Down-cast `col` to `T`, gating on Arrow type only (not name/nullable).\n"
      << "    /// Uses arrow-rs's offset-preserving, buffer-sharing `downcast_array`\n"
      << "    /// (NOT a re-Arc'd `downcast_ref` clone), so the cached handle keeps the\n"
      << "    /// column's offset/len (D-RBA-3 / D-RBA-7).\n"
      << "    #[allow(dead_code)] // unused for accessors with no scalar / repeated-scalar leaf\n"
      << "    fn downcast_array<\n"
      << "        T: From<arrow::array::ArrayData> + arrow::array::Array + 'static,\n"
      << "    >(\n"
      << "        col: &arrow::array::ArrayRef,\n"
      << "        ctx: &str,\n"
      << "        expected: &arrow::datatypes::DataType,\n"
      << "    ) -> Result<std::sync::Arc<T>, arrow::error::ArrowError> {\n"
      << "        if col.data_type() != expected {\n"
      << "            return Err(arrow::error::ArrowError::SchemaError(format!(\n"
      << "                \"{ctx}: expected {expected:?}, got {:?}\",\n"
      << "                col.data_type()\n"
      << "            )));\n"
      << "        }\n"
      << "        // Type checked above; downcast_array shares buffers, preserves offset/len.\n"
      << "        Ok(std::sync::Arc::new(arrow::array::downcast_array::<T>(col.as_ref())))\n"
      << "    }\n\n";
}

// Human label for a FieldKind (used in the composite fail-fast comment).
const char* FieldKindName(FieldKind k) {
    switch (k) {
        case FieldKind::SCALAR: return "SCALAR";
        case FieldKind::REPEATED_SCALAR: return "REPEATED_SCALAR";
        case FieldKind::STRUCT: return "STRUCT";
        case FieldKind::REPEATED_STRUCT: return "REPEATED_STRUCT";
        case FieldKind::NESTED_LIST: return "NESTED_LIST";
        case FieldKind::MAP: return "MAP";
    }
    return "UNKNOWN";
}

// ---------------------------------------------------------------------------
// RBA-6a composite support (STRUCT / REPEATED_SCALAR / REPEATED_STRUCT).
//
// MAP and NESTED_LIST remain fail-fast (RBA-6b). The Rust accessor composes
// nested-message accessors recursively, resolving cross-PACKAGE nested messages
// through the D-RBA-10 package tree (crate::fletcher_gen::<pkg-path>::<Cls>).
// ---------------------------------------------------------------------------

// True if a field kind is supported by the RBA-6a Rust accessor. Scalars must be
// a supported scalar leaf; STRUCT / REPEATED_STRUCT are always shape-validatable;
// REPEATED_SCALAR needs a supported element leaf. MAP / NESTED_LIST are RBA-6b.
bool RustFieldSupported(const FieldInfo& fi) {
    switch (fi.mapping.kind) {
        case FieldKind::SCALAR: {
            RustScalarInfo tmp;
            return RustScalarFor(fi.mapping.scalar.arrow_type_expr, &tmp);
        }
        case FieldKind::STRUCT:
        case FieldKind::REPEATED_STRUCT:
            return true;
        case FieldKind::REPEATED_SCALAR: {
            RustScalarInfo tmp;
            return RustScalarFor(fi.mapping.element.arrow_type_expr, &tmp);
        }
        case FieldKind::MAP:
        case FieldKind::NESTED_LIST:
            return false;  // RBA-6b
    }
    return false;
}

// The fully-qualified Rust accessor path for a nested message `msg`, as seen from
// `context_file` (D-RBA-10). Same proto package -> the bare local
// `<Class>Accessor` (the assembler co-mounts same-package files into one module).
// A different package -> crate::fletcher_gen::<pkg-path>::<Class>Accessor, with
// each package segment sanitized (keyword -> r#seg). A NO-package message in a
// different file resolves DIRECTLY under fletcher_gen as
// crate::fletcher_gen::<Class>Accessor (no empty segment, no double `::`). A
// non-raw-able keyword segment (crate/self/Self/super) is a generation error ->
// empty string, which the caller turns into a loud compile_error!.
std::string RustAccessorPath(const google::protobuf::Descriptor* msg,
                             const google::protobuf::FileDescriptor* context_file) {
    const std::string cls = ClassName(msg) + "Accessor";
    if (msg->file()->package() == context_file->package()) return cls;  // same module
    const std::string& pkg = msg->file()->package();
    // No-package message (different file, empty proto package): mount directly
    // under fletcher_gen — DO NOT emit an empty segment (that yields `::::`).
    if (pkg.empty()) return "crate::fletcher_gen::" + cls;
    std::string path = "crate::fletcher_gen::";
    bool first = true;
    std::string seg;
    auto flush = [&](const std::string& s) -> bool {
        if (IsNonRawRustKeyword(s)) return false;  // generation error
        if (!first) path += "::";
        path += RustIdent(s);
        first = false;
        return true;
    };
    for (char c : pkg) {
        if (c == '.') {
            if (!flush(seg)) return std::string();
            seg.clear();
        } else {
            seg += c;
        }
    }
    if (!seg.empty() && !flush(seg)) return std::string();
    path += "::" + cls;
    return path;
}

// The Descriptor behind a composite field's nested message (STRUCT /
// REPEATED_STRUCT). nullptr if the field is not message-composite.
const google::protobuf::Descriptor* CompositeMsg(const FieldInfo& fi) {
    return fi.mapping.nested_msg;
}

// Rust storage-member names (kept stable + collision-free per field).
std::string RustStructAccMember(const FieldInfo& fi) { return RustIdent(fi.name) + "_acc"; }
std::string RustStructValidityMember(const FieldInfo& fi) {
    return RustIdent(fi.name) + "_struct";
}
std::string RustListMember(const FieldInfo& fi) { return RustIdent(fi.name) + "_list"; }
std::string RustListValuesMember(const FieldInfo& fi) { return RustIdent(fi.name) + "_values"; }
std::string RustListInnerMember(const FieldInfo& fi) { return RustIdent(fi.name) + "_inner"; }

// The ScalarSpan<&'a TArray> type expression for a repeated-scalar leaf. The span
// holds a borrowed reference to the cached typed array (e.g. &Float64Array).
std::string RustScalarSpanType(const RustScalarInfo& info) {
    return "crate::fletcher_gen::__rba::ScalarSpan<&arrow::array::" + info.array_type + ">";
}

// Emit one field's private storage members (Rust).
void EmitRustFieldStorage(std::ostringstream& o, const FieldInfo& fi,
                          const google::protobuf::FileDescriptor* file) {
    switch (fi.mapping.kind) {
        case FieldKind::SCALAR: {
            RustScalarInfo info;
            RustScalarFor(fi.mapping.scalar.arrow_type_expr, &info);
            o << "    " << RustIdent(fi.name) << ": std::sync::Arc<arrow::array::"
              << info.array_type << ">,\n";
            break;
        }
        case FieldKind::REPEATED_SCALAR: {
            RustScalarInfo info;
            RustScalarFor(fi.mapping.element.arrow_type_expr, &info);
            o << "    " << RustListMember(fi) << ": std::sync::Arc<arrow::array::ListArray>,\n"
              << "    " << RustListValuesMember(fi) << ": std::sync::Arc<arrow::array::"
              << info.array_type << ">,\n";
            break;
        }
        case FieldKind::STRUCT: {
            const std::string path = RustAccessorPath(fi.mapping.nested_msg, file);
            // Cache the struct column (row-level validity for the nullable getter)
            // AND the composed inner accessor (owns the sliced child columns). The
            // validity handle is only read by the nullable getter, so suppress the
            // dead-code warning for a non-nullable struct field.
            o << "    #[allow(dead_code)]\n"
              << "    " << RustStructValidityMember(fi)
              << ": std::sync::Arc<arrow::array::StructArray>,\n"
              << "    " << RustStructAccMember(fi) << ": " << path << ",\n";
            break;
        }
        case FieldKind::REPEATED_STRUCT: {
            const std::string path = RustAccessorPath(fi.mapping.nested_msg, file);
            // Cache the list + the inner accessor built over the FLATTENED values
            // StructArray (offset-0 origin); per-row windows are addressed via
            // value_offsets()[row] + j by the StructSpan getter.
            o << "    " << RustListMember(fi) << ": std::sync::Arc<arrow::array::ListArray>,\n"
              << "    " << RustListInnerMember(fi) << ": " << path << ",\n";
            break;
        }
        case FieldKind::MAP:
        case FieldKind::NESTED_LIST:
            break;  // RBA-6b — unreachable (fail-fast gates these out)
    }
}

// Emit one field's validate + cache block in from_columns. Produces local
// bindings named after the storage members (moved into Self by EmitRustFieldInit).
void EmitRustFieldFromColumns(std::ostringstream& o, const FieldInfo& fi, std::size_t i,
                              const std::string& cls,
                              const google::protobuf::FileDescriptor* file) {
    const std::string where = cls + " column " + std::to_string(i) + " '" + fi.name + "'";

    // Non-nullable proto fields must hold no actual nulls (D-RBA-4). For composite
    // fields this recurses: the inner accessor gates its own non-nullable children.
    auto null_gate = [&](const char* arr_expr) {
        if (fi.mapping.nullable) return;
        o << "        if arrow::array::Array::null_count(" << arr_expr << ") != 0 {\n"
          << "            return Err(arrow::error::ArrowError::SchemaError(format!(\n"
          << "                \"" << where << ": non-nullable, found {} nulls\",\n"
          << "                arrow::array::Array::null_count(" << arr_expr << ")\n"
          << "            )));\n"
          << "        }\n";
    };

    switch (fi.mapping.kind) {
        case FieldKind::SCALAR: {
            RustScalarInfo info;
            RustScalarFor(fi.mapping.scalar.arrow_type_expr, &info);
            null_gate(("&cols[" + std::to_string(i) + "]").c_str());
            o << "        let " << RustIdent(fi.name)
              << " = Self::downcast_array::<arrow::array::" << info.array_type << ">(\n"
              << "            &cols[" << i << "],\n"
              << "            \"" << cls << "." << fi.name << "\",\n"
              << "            &" << info.data_type << ",\n"
              << "        )?;\n";
            break;
        }
        case FieldKind::REPEATED_SCALAR: {
            RustScalarInfo info;
            RustScalarFor(fi.mapping.element.arrow_type_expr, &info);
            // Shape gate: List variant (NOT a whole-composite DataType compare —
            // that would gate on child names/nullable flags).
            o << "        let " << RustListMember(fi) << " = {\n"
              << "            if !matches!(\n"
              << "                arrow::array::Array::data_type(&cols[" << i << "]),\n"
              << "                arrow::datatypes::DataType::List(_)\n"
              << "            ) {\n"
              << "                return Err(arrow::error::ArrowError::SchemaError(format!(\n"
              << "                    \"" << where << ": expected List, got {:?}\",\n"
              << "                    arrow::array::Array::data_type(&cols[" << i << "])\n"
              << "                )));\n"
              << "            }\n"
              << "            std::sync::Arc::new(arrow::array::downcast_array::"
              << "<arrow::array::ListArray>(cols[" << i << "].as_ref()))\n"
              << "        };\n";
            null_gate(("&cols[" + std::to_string(i) + "]").c_str());
            // Leaf values: exact scalar DataType gate (D-RBA-4) + offset-preserving
            // typed cache.
            o << "        let " << RustListValuesMember(fi) << " = Self::downcast_array::"
              << "<arrow::array::" << info.array_type << ">(\n"
              << "            &" << RustListMember(fi) << ".values().clone(),\n"
              << "            \"" << cls << "." << fi.name << " values\",\n"
              << "            &" << info.data_type << ",\n"
              << "        )?;\n";
            break;
        }
        case FieldKind::STRUCT: {
            const std::string path = RustAccessorPath(fi.mapping.nested_msg, file);
            o << "        let " << RustStructValidityMember(fi) << " = {\n"
              << "            if !matches!(\n"
              << "                arrow::array::Array::data_type(&cols[" << i << "]),\n"
              << "                arrow::datatypes::DataType::Struct(_)\n"
              << "            ) {\n"
              << "                return Err(arrow::error::ArrowError::SchemaError(format!(\n"
              << "                    \"" << where << ": expected Struct, got {:?}\",\n"
              << "                    arrow::array::Array::data_type(&cols[" << i << "])\n"
              << "                )));\n"
              << "            }\n"
              << "            std::sync::Arc::new(arrow::array::downcast_array::"
              << "<arrow::array::StructArray>(cols[" << i << "].as_ref()))\n"
              << "        };\n";
            null_gate(("&cols[" + std::to_string(i) + "]").c_str());
            // Recurse: from_struct validates the struct's children positionally and
            // recurses the non-null gate (D-RBA-4).
            o << "        let " << RustStructAccMember(fi) << " = " << path << "::from_struct(&"
              << RustStructValidityMember(fi) << ")?;\n";
            break;
        }
        case FieldKind::REPEATED_STRUCT: {
            const std::string path = RustAccessorPath(fi.mapping.nested_msg, file);
            o << "        let " << RustListMember(fi) << " = {\n"
              << "            if !matches!(\n"
              << "                arrow::array::Array::data_type(&cols[" << i << "]),\n"
              << "                arrow::datatypes::DataType::List(_)\n"
              << "            ) {\n"
              << "                return Err(arrow::error::ArrowError::SchemaError(format!(\n"
              << "                    \"" << where << ": expected List, got {:?}\",\n"
              << "                    arrow::array::Array::data_type(&cols[" << i << "])\n"
              << "                )));\n"
              << "            }\n"
              << "            std::sync::Arc::new(arrow::array::downcast_array::"
              << "<arrow::array::ListArray>(cols[" << i << "].as_ref()))\n"
              << "        };\n";
            null_gate(("&cols[" + std::to_string(i) + "]").c_str());
            // The inner accessor is built over the FULL flattened values
            // StructArray (offset-0 origin). The values child must be a Struct.
            o << "        let " << RustListInnerMember(fi) << " = {\n"
              << "            let values = " << RustListMember(fi) << ".values().clone();\n"
              << "            if !matches!(\n"
              << "                arrow::array::Array::data_type(&values),\n"
              << "                arrow::datatypes::DataType::Struct(_)\n"
              << "            ) {\n"
              << "                return Err(arrow::error::ArrowError::SchemaError(format!(\n"
              << "                    \"" << where << " values: expected Struct, got {:?}\",\n"
              << "                    arrow::array::Array::data_type(&values)\n"
              << "                )));\n"
              << "            }\n"
              << "            let structs = arrow::array::downcast_array::"
              << "<arrow::array::StructArray>(values.as_ref());\n"
              << "            " << path << "::from_struct(&structs)?\n"
              << "        };\n";
            break;
        }
        case FieldKind::MAP:
        case FieldKind::NESTED_LIST:
            break;  // RBA-6b
    }
}

// Emit the Self { ... } field initializers for one field (after from_columns has
// bound locals of the same names).
void EmitRustFieldInit(std::ostringstream& o, const FieldInfo& fi) {
    switch (fi.mapping.kind) {
        case FieldKind::SCALAR:
            o << "            " << RustIdent(fi.name) << ",\n";
            break;
        case FieldKind::REPEATED_SCALAR:
            o << "            " << RustListMember(fi) << ",\n"
              << "            " << RustListValuesMember(fi) << ",\n";
            break;
        case FieldKind::STRUCT:
            o << "            " << RustStructValidityMember(fi) << ",\n"
              << "            " << RustStructAccMember(fi) << ",\n";
            break;
        case FieldKind::REPEATED_STRUCT:
            o << "            " << RustListMember(fi) << ",\n"
              << "            " << RustListInnerMember(fi) << ",\n";
            break;
        case FieldKind::MAP:
        case FieldKind::NESTED_LIST:
            break;  // RBA-6b
    }
}

// Emit the public getter for one field.
void EmitRustFieldGetter(std::ostringstream& o, const FieldInfo& fi,
                         const google::protobuf::FileDescriptor* file) {
    const std::string member = RustIdent(fi.name);
    switch (fi.mapping.kind) {
        case FieldKind::SCALAR: {
            RustScalarInfo info;
            RustScalarFor(fi.mapping.scalar.arrow_type_expr, &info);
            if (fi.mapping.nullable) {
                o << "    pub fn " << member << "(&self, row: usize) -> Option<" << info.value_type
                  << "> {\n"
                  << "        if arrow::array::Array::is_null(&*self." << member << ", row) {\n"
                  << "            None\n"
                  << "        } else {\n"
                  << "            Some(self." << member << ".value(row))\n"
                  << "        }\n"
                  << "    }\n";
            } else {
                o << "    pub fn " << member << "(&self, row: usize) -> " << info.value_type
                  << " {\n"
                  << "        self." << member << ".value(row)\n"
                  << "    }\n";
            }
            break;
        }
        case FieldKind::REPEATED_SCALAR: {
            RustScalarInfo info;
            RustScalarFor(fi.mapping.element.arrow_type_expr, &info);
            const std::string span = RustScalarSpanType(info);
            // Turbofish form for the call expression (`Type::<Args>::new(...)`).
            const std::string span_call =
                "crate::fletcher_gen::__rba::ScalarSpan::<&arrow::array::" + info.array_type + ">";
            const std::string body =
                span_call + "::new(self." + RustListValuesMember(fi) + ".as_ref(), self." +
                RustListMember(fi) + ".value_offsets()[row] as usize, self." +
                RustListMember(fi) + ".value_length(row) as usize)";
            if (fi.mapping.nullable) {
                o << "    pub fn " << member << "(&self, row: usize) -> Option<" << span << "> {\n"
                  << "        if arrow::array::Array::is_null(&*self." << RustListMember(fi)
                  << ", row) {\n"
                  << "            None\n"
                  << "        } else {\n"
                  << "            Some(" << body << ")\n"
                  << "        }\n"
                  << "    }\n";
            } else {
                o << "    pub fn " << member << "(&self, row: usize) -> " << span << " {\n"
                  << "        " << body << "\n"
                  << "    }\n";
            }
            break;
        }
        case FieldKind::STRUCT: {
            const std::string path = RustAccessorPath(fi.mapping.nested_msg, file);
            const std::string row_t = path;  // <Path>::Row via RowAccess
            if (fi.mapping.nullable) {
                o << "    pub fn " << member
                  << "(&self, row: usize) -> Option<<" << path
                  << " as crate::fletcher_gen::__rba::RowAccess>::Row<'_>> {\n"
                  << "        if arrow::array::Array::is_null(self." << RustStructValidityMember(fi)
                  << ".as_ref(), row) {\n"
                  << "            None\n"
                  << "        } else {\n"
                  << "            Some(crate::fletcher_gen::__rba::RowAccess::row(&self."
                  << RustStructAccMember(fi) << ", row))\n"
                  << "        }\n"
                  << "    }\n";
            } else {
                o << "    pub fn " << member << "(&self, row: usize) -> <" << path
                  << " as crate::fletcher_gen::__rba::RowAccess>::Row<'_> {\n"
                  << "        crate::fletcher_gen::__rba::RowAccess::row(&self."
                  << RustStructAccMember(fi) << ", row)\n"
                  << "    }\n";
            }
            (void)row_t;
            break;
        }
        case FieldKind::REPEATED_STRUCT: {
            const std::string path = RustAccessorPath(fi.mapping.nested_msg, file);
            const std::string span = "crate::fletcher_gen::__rba::StructSpan<'_, " + path + ">";
            const std::string body =
                "crate::fletcher_gen::__rba::StructSpan::new(&self." + RustListInnerMember(fi) +
                ", self." + RustListMember(fi) + ".value_offsets()[row] as usize, self." +
                RustListMember(fi) + ".value_length(row) as usize)";
            if (fi.mapping.nullable) {
                o << "    pub fn " << member << "(&self, row: usize) -> Option<" << span << "> {\n"
                  << "        if arrow::array::Array::is_null(&*self." << RustListMember(fi)
                  << ", row) {\n"
                  << "            None\n"
                  << "        } else {\n"
                  << "            Some(" << body << ")\n"
                  << "        }\n"
                  << "    }\n";
            } else {
                o << "    pub fn " << member << "(&self, row: usize) -> " << span << " {\n"
                  << "        " << body << "\n"
                  << "    }\n";
            }
            break;
        }
        case FieldKind::MAP:
        case FieldKind::NESTED_LIST:
            break;  // RBA-6b
    }
}

// Emit the Row forwarder method for one field (no row argument; forwards to the
// parent accessor getter at this->row).
void EmitRustRowForward(std::ostringstream& o, const FieldInfo& fi,
                        const google::protobuf::FileDescriptor* file) {
    const std::string member = RustIdent(fi.name);
    switch (fi.mapping.kind) {
        case FieldKind::SCALAR: {
            RustScalarInfo info;
            RustScalarFor(fi.mapping.scalar.arrow_type_expr, &info);
            std::string ret = fi.mapping.nullable ? ("Option<" + info.value_type + ">")
                                                  : info.value_type;
            o << "    pub fn " << member << "(&self) -> " << ret << " {\n"
              << "        self.a." << member << "(self.row)\n"
              << "    }\n";
            break;
        }
        case FieldKind::REPEATED_SCALAR: {
            RustScalarInfo info;
            RustScalarFor(fi.mapping.element.arrow_type_expr, &info);
            std::string span = RustScalarSpanType(info);
            std::string ret = fi.mapping.nullable ? ("Option<" + span + ">") : span;
            o << "    pub fn " << member << "(&self) -> " << ret << " {\n"
              << "        self.a." << member << "(self.row)\n"
              << "    }\n";
            break;
        }
        case FieldKind::STRUCT: {
            const std::string path = RustAccessorPath(fi.mapping.nested_msg, file);
            std::string row = "<" + path +
                              " as crate::fletcher_gen::__rba::RowAccess>::Row<'_>";
            std::string ret = fi.mapping.nullable ? ("Option<" + row + ">") : row;
            o << "    pub fn " << member << "(&self) -> " << ret << " {\n"
              << "        self.a." << member << "(self.row)\n"
              << "    }\n";
            break;
        }
        case FieldKind::REPEATED_STRUCT: {
            const std::string path = RustAccessorPath(fi.mapping.nested_msg, file);
            std::string span = "crate::fletcher_gen::__rba::StructSpan<'_, " + path + ">";
            std::string ret = fi.mapping.nullable ? ("Option<" + span + ">") : span;
            o << "    pub fn " << member << "(&self) -> " << ret << " {\n"
              << "        self.a." << member << "(self.row)\n"
              << "    }\n";
            break;
        }
        case FieldKind::MAP:
        case FieldKind::NESTED_LIST:
            break;  // RBA-6b
    }
}

}  // namespace

std::string EmitRustAccessor(const google::protobuf::FileDescriptor* file) {
    std::ostringstream o;

    o << "// Generated by fletcher-protoc. DO NOT EDIT.\n"
      << "// Source: " << file->name() << "\n"
      << "// RecordBatch accessor module (read-only). Emitted by --fletcher_opt=rust.\n"
      << "//\n"
      << "// Bare items — no `mod <pkg>` wrapper. Package mounting is owned by the\n"
      << "// RBA-5 build.rs assembler (D-RBA-10). The arrow-dependent code here is\n"
      << "// compiled+validated by the integration-tests/protoc-gen-fletcher-rust\n"
      << "// cargo crate (the authoritative Rust well-formedness check).\n";

    // First pass: plan each message. A message whose fields are ALL supported by
    // the RBA-6a accessor (scalars, STRUCT, REPEATED_SCALAR, REPEATED_STRUCT)
    // gets a real accessor; the first MAP / NESTED_LIST / unmapped-scalar field
    // trips the fail-fast (RBA-6b adds map + nested-list).
    struct MsgPlan {
        const google::protobuf::Descriptor* msg = nullptr;
        std::string cls;
        std::vector<FieldInfo> fields;
        bool all_supported = false;
        std::string bad_name;  // first unsupported field (fail-fast)
        std::string bad_kind;
        // First message-composite field that resolves to a non-raw-able keyword
        // package segment (generation error). Empty when none.
        std::string bad_pkg_field;
    };

    std::vector<MsgPlan> plans;

    for (const auto* msg : OrderedMessages(file)) {
        if (IsRecursive(msg) || IsFlattenedWrapper(msg)) continue;

        MsgPlan plan;
        plan.msg = msg;
        plan.cls = ClassName(msg);
        std::string skipped;
        plan.fields = GatherFields(msg, &skipped);

        plan.all_supported = true;
        for (const auto& fi : plan.fields) {
            if (!RustFieldSupported(fi)) {
                plan.all_supported = false;
                plan.bad_name = fi.name;
                plan.bad_kind = (fi.mapping.kind == FieldKind::SCALAR ||
                                 fi.mapping.kind == FieldKind::REPEATED_SCALAR)
                                    ? "unsupported scalar leaf"
                                    : FieldKindName(fi.mapping.kind);
                break;
            }
            // Composite fields that resolve to a cross-package accessor must have
            // a representable package path; a non-raw-able keyword segment is a
            // generation error (loud compile_error!, no half-built accessor).
            if (const auto* nm = CompositeMsg(fi)) {
                if (RustAccessorPath(nm, file).empty()) plan.bad_pkg_field = fi.name;
            }
        }
        plans.push_back(std::move(plan));
    }

    // NOTE: no file-level `use` statements and no file-level free functions are
    // emitted. The build.rs assembler co-mounts every file of one proto package
    // into a SINGLE module (D-RBA-10 same-package case), so any file-level `use`
    // or free `fn` would collide across same-package files. Instead each accessor
    // is fully self-contained: it references arrow/std types AND the shared
    // crate::fletcher_gen::__rba helpers by fully-qualified path, and carries its
    // `downcast_array` cast helper as a PRIVATE ASSOCIATED function in its own
    // `impl` (unique per struct, never colliding).

    for (const auto& plan : plans) {
        const std::string& cls = plan.cls;
        const std::string acc = cls + "Accessor";

        if (!plan.all_supported) {
            // Fail-fast (§2.6): a clear generation-time comment in place of the
            // struct — no partial accessor (D-RBA-6 no silent gap). MAP and
            // NESTED_LIST land in RBA-6b.
            o << "// fletcher: " << cls << " has field '" << plan.bad_name << "' ("
              << plan.bad_kind << ") not yet supported by the Rust accessor"
              << " (MAP / NESTED_LIST land in RBA-6b).\n\n";
            continue;
        }

        // D-RBA-10 keyword rule: a field name that is a Rust keyword which cannot
        // be a raw identifier (crate/self/Self/super) is a generation error — it
        // is never silently renamed. Emit a loud compile_error! in place of the
        // struct (no half-built accessor) naming the offending field.
        bool has_non_raw = false;
        std::string bad_field;
        for (const auto& fi : plan.fields) {
            if (IsNonRawRustKeyword(fi.name)) {
                has_non_raw = true;
                bad_field = fi.name;
                break;
            }
        }
        if (has_non_raw) {
            o << "// fletcher: " << cls << " field '" << bad_field
              << "' is a Rust keyword that cannot be an identifier (even as r#" << bad_field
              << "); rename the proto field.\n"
              << "compile_error!(\"" << cls << ": proto field '" << bad_field
              << "' is a Rust keyword that cannot be used as a Rust identifier; "
              << "rename the proto field.\");\n\n";
            continue;
        }

        // D-RBA-10 package-segment rule: a cross-package nested message whose
        // package path contains a non-raw-able keyword segment cannot be named.
        if (!plan.bad_pkg_field.empty()) {
            o << "// fletcher: " << cls << " composite field '" << plan.bad_pkg_field
              << "' references a message in a package whose path contains a Rust\n"
              << "// keyword segment that cannot be an identifier; rename the proto package.\n"
              << "compile_error!(\"" << cls << ": composite field '" << plan.bad_pkg_field
              << "' references a cross-package message whose package path cannot be a "
              << "Rust module path; rename the proto package segment.\");\n\n";
            continue;
        }

        const std::size_t n = plan.fields.size();

        // -- struct definition + storage -------------------------------------
        o << "/// RecordBatch accessor for " << plan.msg->name() << ".\n"
          << "/// Validation is positional + type-only (names and the nullable flag are\n"
          << "/// tolerated); proto-non-nullable columns additionally require null_count()==0.\n"
          << "pub struct " << acc << " {\n"
          << "    num_rows: usize,\n"
          << "    fields: arrow::datatypes::Fields,\n"
          << "    metadata: std::collections::HashMap<String, String>,\n"
          << "    // Set only by from_struct: the sliced (0-based-window) StructArray whose\n"
          << "    // validity backs is_null(row). None for a RecordBatch source (D-RBA-7).\n"
          << "    struct_validity: Option<std::sync::Arc<arrow::array::StructArray>>,\n";
        for (std::size_t i = 0; i < n; ++i) {
            EmitRustFieldStorage(o, plan.fields[i], file);
        }
        o << "}\n\n";

        o << "impl " << acc << " {\n";

        // Private cast helper (associated fn — unique per struct, no collisions).
        EmitRustDowncastAssoc(o);

        // empty_metadata() — a process-wide empty map for absent field metadata
        // (total, never panics; mirrors C++ field_metadata nullptr-on-absent).
        o << "    /// A shared empty metadata map for absent/out-of-bounds field metadata\n"
          << "    /// (D-RBA-5: absent metadata is NOT an error). OnceLock-initialised.\n"
          << "    fn empty_metadata() -> &'static std::collections::HashMap<String, String> {\n"
          << "        static EMPTY: std::sync::OnceLock<std::collections::HashMap<String, String>> =\n"
          << "            std::sync::OnceLock::new();\n"
          << "        EMPTY.get_or_init(std::collections::HashMap::new)\n"
          << "    }\n\n";

        // try_new(RecordBatch)
        o << "    /// Construct from a `RecordBatch` (top-level factory).\n"
          << "    pub fn try_new(\n"
          << "        batch: arrow::record_batch::RecordBatch,\n"
          << "    ) -> Result<Self, arrow::error::ArrowError> {\n"
          << "        let s = batch.schema();\n"
          << "        Self::from_columns(\n"
          << "            batch.num_rows(),\n"
          << "            batch.columns().to_vec(),\n"
          << "            s.fields().clone(),\n"
          << "            s.metadata().clone(),\n"
          << "            None,\n"
          << "        )\n"
          << "    }\n\n";

        // from_struct(&StructArray)
        o << "    /// Construct from a `StructArray` (struct-source factory). Slices each\n"
          << "    /// child to the struct's [offset, offset+len) window: arrow-rs\n"
          << "    /// StructArray::columns() are NOT pre-rebased by the struct's logical\n"
          << "    /// offset (the OPPOSITE of C++ field(i)), so a sliced struct would\n"
          << "    /// otherwise misalign every getter (D-RBA-7, §2.4). The retained\n"
          << "    /// struct_validity is the SLICED struct, so its is_null(row) shares the\n"
          << "    /// same 0-based origin as the sliced children (R4).\n"
          << "    pub fn from_struct(\n"
          << "        s: &arrow::array::StructArray,\n"
          << "    ) -> Result<Self, arrow::error::ArrowError> {\n"
          << "        // `data_type`/`offset`/`len`/`slice` are `arrow::array::Array` trait\n"
          << "        // methods; called fully-qualified so no `use` is needed (D-RBA-10).\n"
          << "        let arrow::datatypes::DataType::Struct(fields) =\n"
          << "            arrow::array::Array::data_type(s).clone()\n"
          << "        else {\n"
          << "            return Err(arrow::error::ArrowError::SchemaError(\n"
          << "                \"" << cls << "::from_struct: source is not a Struct\".into(),\n"
          << "            ));\n"
          << "        };\n"
          << "        let off = arrow::array::Array::offset(s);\n"
          << "        let len = arrow::array::Array::len(s);\n"
          << "        let cols: Vec<arrow::array::ArrayRef> = s\n"
          << "            .columns()\n"
          << "            .iter()\n"
          << "            .map(|c| arrow::array::Array::slice(c, off, len))\n"
          << "            .collect();\n"
          << "        // Retain the SLICED struct for is_null(row): a windowed validity\n"
          << "        // handle sharing the 0-based origin of the sliced children (R4).\n"
          << "        let sliced = arrow::array::Array::slice(s, off, len);\n"
          << "        let validity = arrow::array::downcast_array::<arrow::array::StructArray>(\n"
          << "            sliced.as_ref(),\n"
          << "        );\n"
          << "        // A StructArray carries no schema-level metadata.\n"
          << "        Self::from_columns(\n"
          << "            len,\n"
          << "            cols,\n"
          << "            fields,\n"
          << "            std::collections::HashMap::new(),\n"
          << "            Some(std::sync::Arc::new(validity)),\n"
          << "        )\n"
          << "    }\n\n";

        // from_columns
        o << "    /// Shared by both factories: column-count + per-column type gate, then\n"
          << "    /// cache. Never panics — every failure path returns an `Err`.\n"
          << "    fn from_columns(\n"
          << "        num_rows: usize,\n"
          << "        cols: Vec<arrow::array::ArrayRef>,\n"
          << "        fields: arrow::datatypes::Fields,\n"
          << "        metadata: std::collections::HashMap<String, String>,\n"
          << "        struct_validity: Option<std::sync::Arc<arrow::array::StructArray>>,\n"
          << "    ) -> Result<Self, arrow::error::ArrowError> {\n"
          << "        if cols.len() != " << n << " {\n"
          << "            return Err(arrow::error::ArrowError::SchemaError(format!(\n"
          << "                \"" << cls << ": expected " << n << " columns, got {}\",\n"
          << "                cols.len()\n"
          << "            )));\n"
          << "        }\n";

        // Per-column validate + cache (type gate, non-null gate, recurse).
        for (std::size_t i = 0; i < n; ++i) {
            EmitRustFieldFromColumns(o, plan.fields[i], i, cls, file);
        }

        o << "        Ok(Self {\n"
          << "            num_rows,\n"
          << "            fields,\n"
          << "            metadata,\n"
          << "            struct_validity,\n";
        for (std::size_t i = 0; i < n; ++i) {
            EmitRustFieldInit(o, plan.fields[i]);
        }
        o << "        })\n"
          << "    }\n\n";

        // num_rows
        o << "    pub fn num_rows(&self) -> usize {\n"
          << "        self.num_rows\n"
          << "    }\n\n";

        // Metadata getters (RBA-3 parity; generic, absent -> empty, D-RBA-5).
        o << "    /// Schema-level metadata, borrowed for the accessor's lifetime. Absent\n"
          << "    /// metadata is an EMPTY map, never an error (D-RBA-5). Struct-sourced\n"
          << "    /// accessors always return empty (a StructArray has no schema metadata).\n"
          << "    pub fn schema_metadata(&self) -> &std::collections::HashMap<String, String> {\n"
          << "        &self.metadata\n"
          << "    }\n\n";

        o << "    /// Field metadata by positional index (aligns with positional\n"
          << "    /// validation; does not depend on field names). Out-of-bounds or absent\n"
          << "    /// metadata -> an empty map, never a panic (D-RBA-5).\n"
          << "    pub fn field_metadata(&self, i: usize) -> &std::collections::HashMap<String, String> {\n"
          << "        match self.fields.get(i) {\n"
          << "            Some(f) => f.metadata(),\n"
          << "            None => Self::empty_metadata(),\n"
          << "        }\n"
          << "    }\n";

        // Field getters (scalar / struct / repeated-scalar / repeated-struct).
        for (std::size_t i = 0; i < n; ++i) {
            o << "\n";
            EmitRustFieldGetter(o, plan.fields[i], file);
        }

        o << "}\n\n";

        // Row forwarder + RowAccess impl (composability + struct null probing).
        o << "/// Borrowed per-row view over " << acc << ": a forwarder {accessor, row}.\n"
          << "/// Valid only while the accessor is alive. Struct getters compose into the\n"
          << "/// inner accessor's Row.\n"
          << "pub struct " << cls << "Row<'a> {\n"
          << "    a: &'a " << acc << ",\n"
          << "    row: usize,\n"
          << "}\n\n";

        o << "impl<'a> " << cls << "Row<'a> {\n";
        for (std::size_t i = 0; i < n; ++i) {
            EmitRustRowForward(o, plan.fields[i], file);
        }
        o << "}\n\n";

        o << "impl crate::fletcher_gen::__rba::RowAccess for " << acc << " {\n"
          << "    type Row<'a> = " << cls << "Row<'a>;\n"
          << "    fn row(&self, row: usize) -> " << cls << "Row<'_> {\n"
          << "        " << cls << "Row { a: self, row }\n"
          << "    }\n"
          << "    fn is_null(&self, row: usize) -> bool {\n"
          << "        self.struct_validity\n"
          << "            .as_ref()\n"
          << "            .map_or(false, |s| arrow::array::Array::is_null(s.as_ref(), row))\n"
          << "    }\n"
          << "}\n\n";
    }

    return o.str();
}

std::string EmitRustRbaHelpers() {
    std::ostringstream o;
    o << "// Generated by fletcher-protoc. DO NOT EDIT.\n"
      << "// Shared RecordBatch-accessor span/Row helpers (RBA-6). Emitted by\n"
      << "// --fletcher_opt=rust, ONCE per protoc run (byte-identical every copy: no\n"
      << "// per-file/per-message content). The build.rs assembler include!s this file\n"
      << "// exactly once under crate::fletcher_gen::__rba (D-RBA-10 / N1).\n"
      << "//\n"
      << "// Versioned with arrow =59.0.0 (the same pin the generated getters commit\n"
      << "// to). All items are referenced from generated accessors by fully-qualified\n"
      << "// path (crate::fletcher_gen::__rba::*) — never via a per-file `use`.\n"
      << "\n"
      << "/// A type that can yield a borrowed per-row view and probe per-row struct\n"
      << "/// validity. Implemented by every generated accessor so spans can compose.\n"
      << "pub trait RowAccess {\n"
      << "    type Row<'a>\n"
      << "    where\n"
      << "        Self: 'a;\n"
      << "    fn row(&self, row: usize) -> Self::Row<'_>;\n"
      << "    fn is_null(&self, row: usize) -> bool;\n"
      << "}\n"
      << "\n"
      << "/// Borrowed window over a repeated-scalar (Arrow list<T>) element range. `A`\n"
      << "/// is held BY VALUE and is a borrowed array reference (e.g. &Float64Array),\n"
      << "/// so one span covers primitive / string / binary scalar leaves. Scalar\n"
      << "/// element nulls are probed via is_null(i) (no read-through-null collapse).\n"
      << "pub struct ScalarSpan<A: arrow::array::ArrayAccessor> {\n"
      << "    vals: A,\n"
      << "    base: usize,\n"
      << "    len: usize,\n"
      << "}\n"
      << "\n"
      << "impl<A: arrow::array::ArrayAccessor> ScalarSpan<A> {\n"
      << "    pub(crate) fn new(vals: A, base: usize, len: usize) -> Self {\n"
      << "        Self { vals, base, len }\n"
      << "    }\n"
      << "    pub fn len(&self) -> usize {\n"
      << "        self.len\n"
      << "    }\n"
      << "    pub fn is_empty(&self) -> bool {\n"
      << "        self.len == 0\n"
      << "    }\n"
      << "    pub fn is_null(&self, i: usize) -> bool {\n"
      << "        arrow::array::Array::is_null(&self.vals, self.base + i)\n"
      << "    }\n"
      << "    pub fn value(&self, i: usize) -> A::Item {\n"
      << "        self.vals.value(self.base + i)\n"
      << "    }\n"
      << "}\n"
      << "\n"
      << "/// Borrowed window over a repeated-struct (Arrow list<struct<...>>) element\n"
      << "/// range. `inner` is the nested accessor (owned by the parent), built over\n"
      << "/// the list's FLATTENED values StructArray (offset-0 origin); element j of a\n"
      << "/// row lives at the absolute index base + j. get(j) yields None on a null\n"
      << "/// struct element (element-level no-read-through-null).\n"
      << "pub struct StructSpan<'a, Acc: RowAccess> {\n"
      << "    inner: &'a Acc,\n"
      << "    base: usize,\n"
      << "    len: usize,\n"
      << "}\n"
      << "\n"
      << "impl<'a, Acc: RowAccess> StructSpan<'a, Acc> {\n"
      << "    pub(crate) fn new(inner: &'a Acc, base: usize, len: usize) -> Self {\n"
      << "        Self { inner, base, len }\n"
      << "    }\n"
      << "    pub fn len(&self) -> usize {\n"
      << "        self.len\n"
      << "    }\n"
      << "    pub fn is_empty(&self) -> bool {\n"
      << "        self.len == 0\n"
      << "    }\n"
      << "    pub fn is_null(&self, i: usize) -> bool {\n"
      << "        self.inner.is_null(self.base + i)\n"
      << "    }\n"
      << "    pub fn get(&self, i: usize) -> Option<Acc::Row<'_>> {\n"
      << "        let r = self.base + i;\n"
      << "        if self.inner.is_null(r) {\n"
      << "            None\n"
      << "        } else {\n"
      << "            Some(self.inner.row(r))\n"
      << "        }\n"
      << "    }\n"
      << "}\n";
    return o.str();
}

}  // namespace fletcher
