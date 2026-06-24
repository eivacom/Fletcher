// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include "recordbatch_accessor_emitter.hpp"

#include <algorithm>
#include <sstream>
#include <string>

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

// Returns the ScalarArrayInfo for a scalar FieldInfo, or nullptr if the Arrow
// type expression is not a supported RBA-2 scalar (e.g. a composite or an as-yet
// unmapped type). Keyed by a stable prefix of arrow_type_expr so timestamp /
// duration unit variants all resolve to the same array class.
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

// Emit the accessor's private storage members: the row count, the live schema
// fields/metadata (storage only — metadata getters are RBA-3), and one cached
// typed-array handle per scalar field.
void EmitStorage(std::ostringstream& o, const std::vector<FieldInfo>& fields) {
    o << "  int64_t num_rows_ = 0;\n"
      << "  arrow::FieldVector fields_;\n"
      << "  std::shared_ptr<const arrow::KeyValueMetadata> schema_metadata_;\n";
    for (const auto& fi : fields) {
        if (!IsSupportedScalar(fi)) continue;
        const ScalarArrayInfo* sa = LookupScalarArray(fi.mapping.scalar.arrow_type_expr);
        o << "  std::shared_ptr<" << sa->concrete_array << "> " << fi.name << "_;\n";
    }
}

}  // namespace

std::string EmitAccessorHeader(const google::protobuf::FileDescriptor* file) {
    std::ostringstream o;

    o << "// Generated by fletcher-protoc. DO NOT EDIT.\n"
      << "// Source: " << file->name() << "\n"
      << "// RecordBatch accessor header (read-only). Emitted by --fletcher_opt=accessor.\n"
      << "#pragma once\n\n";

    o << "#include <arrow/api.h>\n\n"
      << "#include <cstdint>\n"
      << "#include <memory>\n"
      << "#include <optional>\n"
      << "#include <string_view>\n"
      << "#include <utility>\n"
      << "#include <vector>\n\n";

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
          << "    return FromColumns_(length, cols, st.fields(),\n"
          << "                        /*schema_metadata=*/nullptr);\n"
          << "  }\n\n";

        // num_rows.
        o << "  int64_t num_rows() const { return num_rows_; }\n\n";

        // Lifetime note for callers (string/binary getters).
        o << "  // NOTE: utf8/binary getters return std::string_view that borrows the\n"
          << "  // cached column buffers owned by this accessor. The returned views are\n"
          << "  // valid only while this accessor is alive; copy into a std::string to\n"
          << "  // outlive it. Numeric/bool/temporal getters return by value (no borrow).\n";

        // Getters (scalar fields only). Unsupported fields keep an explicit
        // generated comment so they are never silently misrepresented.
        for (const auto& fi : fields) {
            if (!IsSupportedScalar(fi)) {
                o << "  // Field '" << fi.name
                  << "' is not a scalar supported by RBA-2; accessor support is added in"
                     " RBA-4.\n";
                continue;
            }
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
        }
        o << "\n";

        // Private section: default ctor, FromColumns_, storage.
        o << " private:\n";
        o << "  " << acc << "() = default;\n\n";

        o << "  static arrow::Result<" << acc << "> FromColumns_(\n"
          << "      int64_t num_rows, const arrow::ArrayVector& cols, arrow::FieldVector fields,\n"
          << "      std::shared_ptr<const arrow::KeyValueMetadata> schema_metadata) {\n";

        // RBA-2 is scalar-scoped. If the message has any composite field (struct /
        // repeated / map / nested-list) the accessor cannot type-validate or cast
        // that column yet; letting Make succeed would expose a bypassable
        // validation gate (D-RBA-4). Fail fast unconditionally at the first
        // composite column. RBA-4 replaces this with real composite handling. The
        // guard is emitted before the scalar loop so no scalar-handling code is
        // generated after a guaranteed early return (avoids unreachable code).
        std::size_t first_composite = fields.size();
        for (std::size_t i = 0; i < fields.size(); ++i) {
            if (!IsSupportedScalar(fields[i])) {
                first_composite = i;
                break;
            }
        }
        const bool has_composite = first_composite < fields.size();
        if (has_composite) {
            o << "    // RBA-4: composite columns not yet supported — fail fast.\n"
              << "    return arrow::Status::Invalid(\n"
              << "        \"" << cls << " column " << first_composite << " '"
              << fields[first_composite].name
              << "': composite columns not supported until RBA-4\");\n"
              << "  }\n\n";
            EmitStorage(o, fields);
            o << "};\n\n";
            continue;
        }

        // Count gate. Every mapped (scalar) field occupies one schema column.
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
            const auto& fi = fields[i];
            const ScalarArrayInfo* sa = LookupScalarArray(fi.mapping.scalar.arrow_type_expr);
            const std::string& expr = fi.mapping.scalar.arrow_type_expr;
            o << "    {\n"
              << "      const auto expected_type = " << expr << ";\n"
              << "      const auto& col = cols[" << i << "];\n"
              << "      if (col == nullptr)\n"
              << "        return arrow::Status::Invalid(\"" << cls << " column " << i << " '"
              << fi.name << "': null column\");\n"
              << "      if (!col->type()->Equals(*expected_type, /*check_metadata=*/false))\n"
              << "        return arrow::Status::Invalid(\n"
              << "            \"" << cls << " column " << i << " '" << fi.name
              << "': expected \", expected_type->ToString(), \", got \",\n"
              << "            col->type()->ToString());\n";
            if (!fi.mapping.nullable) {
                o << "      if (col->null_count() != 0)\n"
                  << "        return arrow::Status::Invalid(\n"
                  << "            \"" << cls << " column " << i << " '" << fi.name
                  << "': non-nullable, found \", col->null_count(), \" nulls\");\n";
            }
            o << "      self." << fi.name << "_ = std::static_pointer_cast<" << sa->concrete_array
              << ">(col);\n"
              << "    }\n";
        }

        o << "    return self;\n"
          << "  }\n\n";

        EmitStorage(o, fields);

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
