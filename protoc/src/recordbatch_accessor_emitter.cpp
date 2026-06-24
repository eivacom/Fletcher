// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include "recordbatch_accessor_emitter.hpp"

#include <algorithm>
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

// True if RBA-4a generates real storage/getters for this field. RBA-4a covers
// scalars (RBA-2), STRUCT, REPEATED_SCALAR (supported leaf), and REPEATED_STRUCT.
// MAP and NESTED_LIST remain RBA-4b and are NOT supported here.
bool IsRba4aSupported(const FieldInfo& fi) {
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
        case FieldKind::NESTED_LIST:
            return false;
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
            o << "  std::shared_ptr<arrow::ListArray> " << ListMember(fi) << ";\n"
              << "  std::optional<" << acc << "> " << ListInnerMember(fi) << ";\n";
            break;
        }
        case FieldKind::MAP:
        case FieldKind::NESTED_LIST:
            // RBA-4b: no storage emitted (getter is comment-only; FromColumns_
            // fails fast for the message).
            break;
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
        case FieldKind::MAP:
            o << "  // Field '" << fi.name
              << "' (map) — RBA-4b: map accessor support not yet emitted.\n";
            break;
        case FieldKind::NESTED_LIST:
            o << "  // Field '" << fi.name
              << "' (nested list) — RBA-4b: nested-list accessor support not yet emitted.\n";
            break;
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
        case FieldKind::MAP:
        case FieldKind::NESTED_LIST:
            // RBA-4b: no forwarder (no getter to forward to).
            break;
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
              << "      self." << ListInnerMember(fi) << " = std::move(inner);\n";
            break;
        }
        case FieldKind::MAP:
        case FieldKind::NESTED_LIST:
            // Never reached: messages with these kinds fail fast before the loop.
            break;
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
            if (fi.mapping.kind == FieldKind::REPEATED_SCALAR ||
                fi.mapping.kind == FieldKind::REPEATED_STRUCT) {
                needs_spans = true;
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

        // RBA-4b: a message with any MAP / NESTED_LIST field cannot be type-
        // validated yet; FromColumns_ fails fast at the first such column so
        // Make() never exposes a bypassable validation gate (D-RBA-4). The 4a
        // getters before it still compile; they are simply unreachable.
        std::size_t first_unsupported = fields.size();
        for (std::size_t i = 0; i < fields.size(); ++i) {
            if (!IsRba4aSupported(fields[i])) {
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
            if (IsRba4aSupported(fi)) {
                EmitFieldGetter(o, fi);
            } else if (fi.mapping.kind == FieldKind::MAP ||
                       fi.mapping.kind == FieldKind::NESTED_LIST) {
                EmitFieldGetter(o, fi);  // emits the RBA-4b comment
            } else {
                // An as-yet unmapped/unsupported construct: keep an explicit
                // generation-time comment (D-RBA-6 no silent gap).
                o << "  // Field '" << fi.name
                  << "' is not supported by the RBA accessor yet; no getter emitted.\n";
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
            if (IsRba4aSupported(fi)) EmitRowViewForward(o, fi);
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
            const char* kind_word =
                bad.mapping.kind == FieldKind::MAP ? "map" : "nested-list";
            o << "    // RBA-4b: " << kind_word << " columns not yet supported — fail fast so\n"
              << "    // Make() never exposes a bypassable validation gate (D-RBA-4).\n"
              << "    return arrow::Status::Invalid(\n"
              << "        \"" << cls << " column " << first_unsupported << " '" << bad.name
              << "': " << kind_word << " columns not supported until RBA-4b\");\n"
              << "  }\n\n";
            // Emit storage (incl. struct_validity_) so the class is well-formed.
            o << "  int64_t num_rows_ = 0;\n"
              << "  arrow::FieldVector fields_;\n"
              << "  std::shared_ptr<const arrow::KeyValueMetadata> schema_metadata_;\n"
              << "  std::shared_ptr<arrow::StructArray> struct_validity_;\n";
            for (const auto& fi : fields) {
                if (IsRba4aSupported(fi)) EmitFieldStorage(o, fi);
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

std::string EmitRustAccessor(const google::protobuf::FileDescriptor* file) {
    std::ostringstream o;

    o << "// Generated by fletcher-protoc. DO NOT EDIT.\n"
      << "// Source: " << file->name() << "\n"
      << "// RecordBatch accessor module (read-only). Content emitted by RBA-2+.\n"
      << "//\n"
      << "// RBA-1: minimal skeleton. No package wrapper modules — package mounting\n"
      << "// is owned by the RBA-5 assembler (D-RBA-10).\n";

    return o.str();
}

}  // namespace fletcher
