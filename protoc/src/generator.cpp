// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include "generator.hpp"

#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/io/zero_copy_stream.h>

#include <algorithm>
#include <cstring>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "type_mapper.hpp"

namespace fletcher {

namespace {

// -----------------------------------------------------------------------
// String helpers
// -----------------------------------------------------------------------

std::string ReplaceAll(std::string s, const std::string& from, const std::string& to) {
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

std::string DotToColons(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2 * std::count(s.begin(), s.end(), '.'));
    for (char c : s) {
        if (c == '.')
            out += "::";
        else
            out += c;
    }
    return out;
}

std::string StripProtoSuffix(const std::string& proto_name) {
    constexpr std::string_view kSuffix = ".proto";
    std::string base = proto_name;
    if (base.size() > kSuffix.size() && base.substr(base.size() - kSuffix.size()) == kSuffix)
        base.resize(base.size() - kSuffix.size());
    return base;
}

std::string OutputFilename(const std::string& proto_name) {
    return StripProtoSuffix(proto_name) + ".fletcher.pb.h";
}

bool WriteToStream(google::protobuf::io::ZeroCopyOutputStream* out, const std::string& s,
                   std::string* error) {
    const char* data = s.data();
    size_t remaining = s.size();
    while (remaining > 0) {
        void* buf;
        int size;
        if (!out->Next(&buf, &size)) {
            *error = "ZeroCopyOutputStream::Next failed";
            return false;
        }
        const size_t n = std::min(static_cast<size_t>(size), remaining);
        std::memcpy(buf, data, n);
        if (static_cast<size_t>(size) > n) out->BackUp(static_cast<int>(size - n));
        data += n;
        remaining -= n;
    }
    return true;
}

// -----------------------------------------------------------------------
// Cross-file include collection
// -----------------------------------------------------------------------

// Scan all supported field mappings in a message (and its nested types) and
// accumulate the include paths of any cross-file generated headers needed.
void CollectCrossFileIncludesFromMessage(const google::protobuf::Descriptor* msg,
                                         std::set<std::string>& headers) {
    for (int fi = 0; fi < msg->field_count(); ++fi) {
        const auto* fd = msg->field(fi);
        if (auto m = MapField(fd)) {
            if (!m->nested_header.empty()) headers.insert(m->nested_header);
            if (!m->map_value_header.empty()) headers.insert(m->map_value_header);
        }
    }
    for (int ni = 0; ni < msg->nested_type_count(); ++ni)
        CollectCrossFileIncludesFromMessage(msg->nested_type(ni), headers);
}

std::set<std::string> CollectCrossFileIncludes(const google::protobuf::FileDescriptor* file) {
    std::set<std::string> headers;
    for (int mi = 0; mi < file->message_type_count(); ++mi)
        CollectCrossFileIncludesFromMessage(file->message_type(mi), headers);
    return headers;
}

// -----------------------------------------------------------------------
// Topological ordering of messages
// -----------------------------------------------------------------------

void TopologicalVisit(const google::protobuf::Descriptor* msg,
                      const google::protobuf::FileDescriptor* file,
                      std::set<const google::protobuf::Descriptor*>& emitted,
                      std::vector<const google::protobuf::Descriptor*>& order) {
    if (emitted.count(msg)) return;
    // Skip synthetic map-entry messages.
    if (msg->options().map_entry()) return;
    // Only generate classes for messages in this file.
    if (msg->file() != file) return;
    // Skip recursive messages entirely.
    if (IsRecursive(msg)) {
        emitted.insert(msg);
        return;
    }

    // Visit nested types first.
    for (int i = 0; i < msg->nested_type_count(); ++i)
        TopologicalVisit(msg->nested_type(i), file, emitted, order);

    // Visit message-type dependencies.
    for (int i = 0; i < msg->field_count(); ++i) {
        const auto* f = msg->field(i);
        if (f->type() != google::protobuf::FieldDescriptor::TYPE_MESSAGE) continue;
        if (f->is_map()) {
            const auto* val = f->message_type()->field(1);
            if (val->type() == google::protobuf::FieldDescriptor::TYPE_MESSAGE)
                TopologicalVisit(val->message_type(), file, emitted, order);
        } else {
            TopologicalVisit(f->message_type(), file, emitted, order);
        }
    }

    emitted.insert(msg);
    order.push_back(msg);
}

std::vector<const google::protobuf::Descriptor*> OrderedMessages(
    const google::protobuf::FileDescriptor* file) {
    std::set<const google::protobuf::Descriptor*> emitted;
    std::vector<const google::protobuf::Descriptor*> order;
    for (int i = 0; i < file->message_type_count(); ++i)
        TopologicalVisit(file->message_type(i), file, emitted, order);
    return order;
}

// -----------------------------------------------------------------------
// Per-field information gathered before code generation
// -----------------------------------------------------------------------

struct FieldInfo {
    std::string name;
    FieldMapping mapping;
    int field_number = 0;  // leaf proto field number
    std::string field_id;  // dotted field-number path, unique even when field-level
                           // flatten inlines sub-messages (e.g. "2.1"); equals the
                           // field_number string for non-inlined top-level fields
    const google::protobuf::FieldDescriptor* descriptor{};  // original proto descriptor
};

// -----------------------------------------------------------------------
// Arrow type expression for the schema — constructed from the FieldMapping
// -----------------------------------------------------------------------

std::string ArrowTypeExpr(const FieldInfo& fi) {
    switch (fi.mapping.kind) {
        case FieldKind::SCALAR:
            return fi.mapping.scalar.arrow_type_expr;

        case FieldKind::REPEATED_SCALAR:
            // Arrow convention: list "item" child is nullable (matches arrow::list(type) and the
            // nanoarrow schema emitted by GenerateSchemaFunction). Keep these in sync.
            return "arrow::list(arrow::field(\"item\", " + fi.mapping.element.arrow_type_expr +
                   ", true))";

        case FieldKind::STRUCT:
            return "arrow::struct_(" + fi.mapping.nested_class + "Schema()->fields())";

        case FieldKind::REPEATED_STRUCT:
            return "arrow::list(arrow::field(\"item\", arrow::struct_(" + fi.mapping.nested_class +
                   "Schema()->fields()), true))";

        case FieldKind::NESTED_LIST: {
            // Build nested arrow::list() from inside out.
            std::string expr = "arrow::struct_(" + fi.mapping.nested_class + "Schema()->fields())";
            for (int d = 0; d < fi.mapping.list_depth; ++d)
                expr = "arrow::list(arrow::field(\"item\", " + expr + ", true))";
            return expr;
        }

        case FieldKind::MAP: {
            std::string val_type =
                fi.mapping.map_value_is_message
                    ? "arrow::struct_(" + fi.mapping.map_value_class + "Schema()->fields())"
                    : fi.mapping.map_value.arrow_type_expr;
            // Map "value" child is nullable; "key" is non-nullable (Arrow spec).
            return "arrow::map(" + fi.mapping.map_key.arrow_type_expr +
                   ", arrow::field(\"value\", " + val_type + ", true))";
        }
    }
    return "/* unknown */";
}

// -----------------------------------------------------------------------
// Storage member declaration
// -----------------------------------------------------------------------

std::string StorageDecl(const FieldInfo& fi) {
    switch (fi.mapping.kind) {
        case FieldKind::SCALAR:
            return "std::optional<" + fi.mapping.scalar.storage_type + "> " + fi.name + "_";

        case FieldKind::REPEATED_SCALAR:
            return "std::vector<" + fi.mapping.element.storage_type + "> " + fi.name + "_";

        case FieldKind::STRUCT:
            return "std::optional<" + fi.mapping.nested_class + "> " + fi.name + "_";

        case FieldKind::REPEATED_STRUCT:
            return "std::vector<" + fi.mapping.nested_class + "> " + fi.name + "_";

        case FieldKind::NESTED_LIST: {
            std::string type = fi.mapping.nested_class;
            for (int d = 0; d < fi.mapping.list_depth; ++d) type = "std::vector<" + type + ">";
            if (fi.mapping.nullable) return "std::optional<" + type + "> " + fi.name + "_";
            return type + " " + fi.name + "_";
        }

        case FieldKind::MAP: {
            std::string val_type = fi.mapping.map_value_is_message
                                       ? fi.mapping.map_value_class
                                       : fi.mapping.map_value.storage_type;
            return "std::vector<std::pair<" + fi.mapping.map_key.storage_type + ", " + val_type +
                   ">> " + fi.name + "_";
        }
    }
    return "/* unknown */ " + fi.name + "_";
}

// -----------------------------------------------------------------------
// Setter generation
// -----------------------------------------------------------------------

void EmitSetters(std::ostringstream& o, const std::string& cls,
                 const std::vector<FieldInfo>& fields) {
    for (const auto& fi : fields) {
        switch (fi.mapping.kind) {
            case FieldKind::SCALAR: {
                bool coerce = (fi.mapping.scalar.param_type != fi.mapping.scalar.storage_type);
                o << "    " << cls << "& set_" << fi.name << "(" << fi.mapping.scalar.param_type
                  << " v) {\n";
                if (coerce)
                    o << "        " << fi.name << "_ = std::string(v);\n";
                else
                    o << "        " << fi.name << "_ = v;\n";
                o << "        return *this;\n    }\n";
                if (fi.mapping.nullable) {
                    o << "    " << cls << "& clear_" << fi.name << "() {\n"
                      << "        " << fi.name << "_.reset();\n"
                      << "        return *this;\n    }\n";
                }
                break;
            }

            case FieldKind::REPEATED_SCALAR:
                o << "    " << cls << "& set_" << fi.name << "(std::vector<"
                  << fi.mapping.element.storage_type << "> v) {\n"
                  << "        " << fi.name << "_ = std::move(v);\n"
                  << "        return *this;\n    }\n";
                break;

            case FieldKind::STRUCT:
                o << "    " << cls << "& set_" << fi.name << "(" << fi.mapping.nested_class
                  << " v) {\n"
                  << "        " << fi.name << "_ = std::move(v);\n"
                  << "        return *this;\n    }\n";
                if (fi.mapping.nullable) {
                    o << "    " << cls << "& clear_" << fi.name << "() {\n"
                      << "        " << fi.name << "_.reset();\n"
                      << "        return *this;\n    }\n";
                }
                break;

            case FieldKind::REPEATED_STRUCT:
                o << "    " << cls << "& set_" << fi.name << "(std::vector<"
                  << fi.mapping.nested_class << "> v) {\n"
                  << "        " << fi.name << "_ = std::move(v);\n"
                  << "        return *this;\n    }\n";
                break;

            case FieldKind::NESTED_LIST: {
                std::string type = fi.mapping.nested_class;
                for (int d = 0; d < fi.mapping.list_depth; ++d) type = "std::vector<" + type + ">";
                o << "    " << cls << "& set_" << fi.name << "(" << type << " v) {\n"
                  << "        " << fi.name << "_ = std::move(v);\n"
                  << "        return *this;\n    }\n";
                if (fi.mapping.nullable) {
                    o << "    " << cls << "& clear_" << fi.name << "() {\n"
                      << "        " << fi.name << "_.reset();\n"
                      << "        return *this;\n    }\n";
                }
                break;
            }

            case FieldKind::MAP: {
                std::string val_type = fi.mapping.map_value_is_message
                                           ? fi.mapping.map_value_class
                                           : fi.mapping.map_value.storage_type;
                o << "    " << cls << "& set_" << fi.name << "(std::vector<std::pair<"
                  << fi.mapping.map_key.storage_type << ", " << val_type << ">> v) {\n"
                  << "        " << fi.name << "_ = std::move(v);\n"
                  << "        return *this;\n    }\n";
                break;
            }
        }
    }
}

// -----------------------------------------------------------------------
// Getter generation (mutable class — returns const refs to internal storage)
// -----------------------------------------------------------------------

void EmitGetters(std::ostringstream& o, const std::vector<FieldInfo>& fields) {
    for (const auto& fi : fields) {
        switch (fi.mapping.kind) {
            case FieldKind::SCALAR: {
                const auto& sc = fi.mapping.scalar;

                if (fi.mapping.nullable) {
                    if (sc.value_is_buffer) {
                        o << "    std::optional<std::string_view> " << fi.name << "() const {\n"
                          << "        if (!" << fi.name << "_.has_value()) return std::nullopt;\n"
                          << "        return std::string_view{*" << fi.name << "_};\n"
                          << "    }\n";
                    } else {
                        o << "    std::optional<" << sc.storage_type << "> " << fi.name
                          << "() const { return " << fi.name << "_; }\n";
                    }
                } else {
                    if (sc.value_is_buffer) {
                        // Can't use value_or("") — temporary would dangle.
                        o << "    std::string_view " << fi.name << "() const {\n"
                          << "        if (!" << fi.name << "_.has_value()) return {};\n"
                          << "        return *" << fi.name << "_;\n"
                          << "    }\n";
                    } else {
                        o << "    " << sc.storage_type << " " << fi.name << "() const { return "
                          << fi.name << "_.value_or(" << sc.default_value << "); }\n";
                    }
                }
                break;
            }

            case FieldKind::STRUCT:
                if (fi.mapping.nullable) {
                    o << "    const " << fi.mapping.nested_class << "* " << fi.name
                      << "() const {\n"
                      << "        return " << fi.name << "_.has_value() ? &*" << fi.name
                      << "_ : nullptr;\n"
                      << "    }\n";
                } else {
                    o << "    const " << fi.mapping.nested_class << "& " << fi.name
                      << "() const {\n"
                      << "        static const " << fi.mapping.nested_class << " kDefault{};\n"
                      << "        return " << fi.name << "_.has_value() ? *" << fi.name
                      << "_ : kDefault;\n"
                      << "    }\n";
                }
                break;

            case FieldKind::REPEATED_SCALAR:
                o << "    const std::vector<" << fi.mapping.element.storage_type << ">& " << fi.name
                  << "() const { return " << fi.name << "_; }\n";
                break;

            case FieldKind::REPEATED_STRUCT:
                o << "    const std::vector<" << fi.mapping.nested_class << ">& " << fi.name
                  << "() const { return " << fi.name << "_; }\n";
                break;

            case FieldKind::NESTED_LIST: {
                std::string type = fi.mapping.nested_class;
                for (int d = 0; d < fi.mapping.list_depth; ++d) type = "std::vector<" + type + ">";
                if (fi.mapping.nullable) {
                    o << "    const " << type << "* " << fi.name << "() const {\n"
                      << "        return " << fi.name << "_.has_value() ? &*" << fi.name
                      << "_ : nullptr;\n"
                      << "    }\n";
                } else {
                    o << "    const " << type << "& " << fi.name << "() const { return " << fi.name
                      << "_; }\n";
                }
                break;
            }

            case FieldKind::MAP: {
                std::string val_type = fi.mapping.map_value_is_message
                                           ? fi.mapping.map_value_class
                                           : fi.mapping.map_value.storage_type;
                o << "    const std::vector<std::pair<" << fi.mapping.map_key.storage_type << ", "
                  << val_type << ">>& " << fi.name << "() const { return " << fi.name << "_; }\n";
                break;
            }
        }
    }
}

// -----------------------------------------------------------------------
// Composite scalar helper methods
// -----------------------------------------------------------------------

void EmitScalarHelper(std::ostringstream& o, const FieldInfo& fi) {
    const std::string fn = "Make" + fi.name + "Scalar_";

    switch (fi.mapping.kind) {
        case FieldKind::REPEATED_SCALAR:
            o << "    std::shared_ptr<arrow::Scalar> " << fn << "() const {\n"
              << "        " << fi.mapping.element.builder_type << " builder;\n"
              << "        for (const auto& v : " << fi.name << "_)\n"
              << "            (void)builder.Append(v);\n"
              << "        return std::make_shared<arrow::ListScalar>(\n"
              << "            *builder.Finish(),\n"
              << "            arrow::list(arrow::field(\"item\", "
              << fi.mapping.element.arrow_type_expr << ", true)));\n"
              << "    }\n";
            break;

        case FieldKind::STRUCT:
            o << "    std::shared_ptr<arrow::Scalar> " << fn << "() const {\n"
              << "        auto type = arrow::struct_(" << fi.mapping.nested_class
              << "Schema()->fields());\n";
            if (fi.mapping.nullable) {
                o << "        if (!" << fi.name << "_.has_value())\n"
                  << "            return arrow::MakeNullScalar(type);\n"
                  << "        return std::make_shared<arrow::StructScalar>(\n"
                  << "            " << fi.name << "_->ToScalars(), type);\n";
            } else {
                o << "        auto values = " << fi.name << "_.has_value()\n"
                  << "            ? " << fi.name << "_->ToScalars()\n"
                  << "            : " << fi.mapping.nested_class << "().ToScalars();\n"
                  << "        return std::make_shared<arrow::StructScalar>(\n"
                  << "            std::move(values), type);\n";
            }
            o << "    }\n";
            break;

        case FieldKind::REPEATED_STRUCT:
            o << "    std::shared_ptr<arrow::Scalar> " << fn << "() const {\n"
              << "        auto type = arrow::struct_(" << fi.mapping.nested_class
              << "Schema()->fields());\n"
              << "        auto builder = arrow::MakeBuilder(type).ValueOrDie();\n"
              << "        for (const auto& v : " << fi.name << "_) {\n"
              << "            auto s = std::make_shared<arrow::StructScalar>(\n"
              << "                v.ToScalars(), type);\n"
              << "            (void)builder->AppendScalar(*s);\n"
              << "        }\n"
              << "        return std::make_shared<arrow::ListScalar>(\n"
              << "            *builder->Finish(),\n"
              << "            arrow::list(arrow::field(\"item\", type, true)));\n"
              << "    }\n";
            break;

        case FieldKind::NESTED_LIST: {
            o << "    std::shared_ptr<arrow::Scalar> " << fn << "() const {\n";

            // Nullable: return null scalar if not set.
            if (fi.mapping.nullable) {
                o << "        if (!" << fi.name << "_.has_value())\n"
                  << "            return arrow::MakeNullScalar(" << ArrowTypeExpr(fi) << ");\n";
            }

            // Reference to the data — either *optional or the bare member.
            std::string data_ref = fi.mapping.nullable ? ("(*" + fi.name + "_)") : (fi.name + "_");

            o << "        auto coord_type = arrow::struct_(" << fi.mapping.nested_class
              << "Schema()->fields());\n";

            if (fi.mapping.list_depth == 2) {
                // List<List<Struct>>
                o << "        auto inner_list_type = arrow::list(\n"
                  << "            arrow::field(\"item\", coord_type, true));\n"
                  << "        auto outer_builder = "
                     "arrow::MakeBuilder(inner_list_type).ValueOrDie();\n"
                  << "        for (const auto& ring : " << data_ref << ") {\n"
                  << "            auto inner_builder = "
                     "arrow::MakeBuilder(coord_type).ValueOrDie();\n"
                  << "            for (const auto& v : ring) {\n"
                  << "                auto s = std::make_shared<arrow::StructScalar>(\n"
                  << "                    v.ToScalars(), coord_type);\n"
                  << "                (void)inner_builder->AppendScalar(*s);\n"
                  << "            }\n"
                  << "            (void)outer_builder->AppendScalar(\n"
                  << "                arrow::ListScalar(*inner_builder->Finish(), "
                     "inner_list_type));\n"
                  << "        }\n"
                  << "        return std::make_shared<arrow::ListScalar>(\n"
                  << "            *outer_builder->Finish());\n";
            } else if (fi.mapping.list_depth == 3) {
                // List<List<List<Struct>>>
                o << "        auto ring_list_type = arrow::list(\n"
                  << "            arrow::field(\"item\", coord_type, true));\n"
                  << "        auto poly_list_type = arrow::list(\n"
                  << "            arrow::field(\"item\", ring_list_type, true));\n"
                  << "        auto outer_builder = "
                     "arrow::MakeBuilder(poly_list_type).ValueOrDie();\n"
                  << "        for (const auto& poly : " << data_ref << ") {\n"
                  << "            auto mid_builder = "
                     "arrow::MakeBuilder(ring_list_type).ValueOrDie();\n"
                  << "            for (const auto& ring : poly) {\n"
                  << "                auto inner_builder = "
                     "arrow::MakeBuilder(coord_type).ValueOrDie();\n"
                  << "                for (const auto& v : ring) {\n"
                  << "                    auto s = std::make_shared<arrow::StructScalar>(\n"
                  << "                        v.ToScalars(), coord_type);\n"
                  << "                    (void)inner_builder->AppendScalar(*s);\n"
                  << "                }\n"
                  << "                (void)mid_builder->AppendScalar(\n"
                  << "                    arrow::ListScalar(*inner_builder->Finish(), "
                     "ring_list_type));\n"
                  << "            }\n"
                  << "            (void)outer_builder->AppendScalar(\n"
                  << "                arrow::ListScalar(*mid_builder->Finish(), poly_list_type));\n"
                  << "        }\n"
                  << "        return std::make_shared<arrow::ListScalar>(\n"
                  << "            *outer_builder->Finish());\n";
            }

            o << "    }\n";
            break;
        }

        case FieldKind::MAP: {
            o << "    std::shared_ptr<arrow::Scalar> " << fn << "() const {\n"
              << "        " << fi.mapping.map_key.builder_type << " key_builder;\n";

            if (fi.mapping.map_value_is_message) {
                o << "        auto val_type = arrow::struct_(" << fi.mapping.map_value_class
                  << "Schema()->fields());\n"
                  << "        auto val_builder = arrow::MakeBuilder(val_type).ValueOrDie();\n"
                  << "        for (const auto& [k, v] : " << fi.name << "_) {\n"
                  << "            (void)key_builder.Append(k);\n"
                  << "            auto s = std::make_shared<arrow::StructScalar>(\n"
                  << "                v.ToScalars(), val_type);\n"
                  << "            (void)val_builder->AppendScalar(*s);\n"
                  << "        }\n"
                  << "        auto keys = *key_builder.Finish();\n"
                  << "        auto vals = *val_builder->Finish();\n";
            } else {
                o << "        " << fi.mapping.map_value.builder_type << " val_builder;\n"
                  << "        for (const auto& [k, v] : " << fi.name << "_) {\n"
                  << "            (void)key_builder.Append(k);\n"
                  << "            (void)val_builder.Append(v);\n"
                  << "        }\n"
                  << "        auto keys = *key_builder.Finish();\n"
                  << "        auto vals = *val_builder.Finish();\n";
            }

            if (fi.mapping.map_value_is_message) {
                o << "        auto val_field = arrow::field(\"value\", val_type, true);\n"
                  << "        auto kv = *arrow::StructArray::Make(\n"
                  << "            {keys, vals},\n"
                  << "            {arrow::field(\"key\", " << fi.mapping.map_key.arrow_type_expr
                  << ", false),\n"
                  << "             val_field});\n"
                  << "        return std::make_shared<arrow::MapScalar>(kv,\n"
                  << "            arrow::map(" << fi.mapping.map_key.arrow_type_expr
                  << ", val_field));\n";
            } else {
                o << "        auto val_field = arrow::field(\"value\", "
                  << fi.mapping.map_value.arrow_type_expr << ", true);\n"
                  << "        auto kv = *arrow::StructArray::Make(\n"
                  << "            {keys, vals},\n"
                  << "            {arrow::field(\"key\", " << fi.mapping.map_key.arrow_type_expr
                  << ", false),\n"
                  << "             val_field});\n"
                  << "        return std::make_shared<arrow::MapScalar>(kv,\n"
                  << "            arrow::map(" << fi.mapping.map_key.arrow_type_expr
                  << ", val_field));\n";
            }
            o << "    }\n";
            break;
        }

        default:
            break;  // SCALAR — no helper needed
    }
}

// -----------------------------------------------------------------------
// ToScalars() entry for one field
// -----------------------------------------------------------------------

std::string ScalarEntry(const FieldInfo& fi) {
    if (fi.mapping.kind != FieldKind::SCALAR) {
        // Composite types delegate to a helper method.
        return "Make" + fi.name + "Scalar_()";
    }

    // Scalar field.
    if (fi.mapping.nullable) {
        const std::string ctor =
            ReplaceAll(fi.mapping.scalar.scalar_ctor, "{val}", "*" + fi.name + "_");
        return fi.name + "_.has_value()\n" + "                ? std::shared_ptr<arrow::Scalar>(" +
               ctor + ")\n" + "                : arrow::MakeNullScalar(" +
               fi.mapping.scalar.arrow_type_expr + ")";
    }

    const std::string val = fi.name + "_.value_or(" + fi.mapping.scalar.default_value + ")";
    return ReplaceAll(fi.mapping.scalar.scalar_ctor, "{val}", val);
}

// -----------------------------------------------------------------------
// Field extraction for SetFromScalars_ / EncodedRow constructor
// -----------------------------------------------------------------------

void EmitFieldExtraction(std::ostringstream& o, const FieldInfo& fi, size_t idx) {
    const std::string si = std::to_string(idx);

    switch (fi.mapping.kind) {
        case FieldKind::SCALAR: {
            const auto& sc = fi.mapping.scalar;
            std::string extract =
                sc.value_is_buffer
                    ? "static_cast<const " + sc.scalar_type + "&>(*scalars[" + si +
                          "]).value->ToString()"
                    : "static_cast<const " + sc.scalar_type + "&>(*scalars[" + si + "]).value";
            if (fi.mapping.nullable) {
                o << "        if (scalars[" << si << "]->is_valid)\n"
                  << "            " << fi.name << "_ = " << extract << ";\n"
                  << "        else\n"
                  << "            " << fi.name << "_.reset();\n";
            } else {
                o << "        " << fi.name << "_ = " << extract << ";\n";
            }
            break;
        }

        case FieldKind::STRUCT:
            if (fi.mapping.nullable) {
                o << "        if (scalars[" << si << "]->is_valid) {\n"
                  << "            " << fi.name << "_.emplace();\n"
                  << "            " << fi.name << "_->SetFromScalars_(\n"
                  << "                static_cast<const arrow::StructScalar&>(\n"
                  << "                    *scalars[" << si << "]).value);\n"
                  << "        } else {\n"
                  << "            " << fi.name << "_.reset();\n"
                  << "        }\n";
            } else {
                o << "        " << fi.name << "_.emplace();\n"
                  << "        " << fi.name << "_->SetFromScalars_(\n"
                  << "            static_cast<const arrow::StructScalar&>(\n"
                  << "                *scalars[" << si << "]).value);\n";
            }
            break;

        case FieldKind::REPEATED_SCALAR: {
            const auto& el = fi.mapping.element;
            std::string extract =
                el.value_is_buffer
                    ? "static_cast<const " + el.scalar_type + "&>(*s).value->ToString()"
                    : "static_cast<const " + el.scalar_type + "&>(*s).value";
            o << "        {\n"
              << "            const auto& ls = static_cast<const arrow::ListScalar&>(\n"
              << "                *scalars[" << si << "]);\n"
              << "            " << fi.name << "_.clear();\n"
              << "            " << fi.name << "_.reserve(ls.value->length());\n"
              << "            for (int64_t j = 0; j < ls.value->length(); ++j) {\n"
              << "                auto s = ls.value->GetScalar(j).ValueOrDie();\n"
              << "                " << fi.name << "_.push_back(" << extract << ");\n"
              << "            }\n"
              << "        }\n";
            break;
        }

        case FieldKind::REPEATED_STRUCT:
            o << "        {\n"
              << "            const auto& ls = static_cast<const arrow::ListScalar&>(\n"
              << "                *scalars[" << si << "]);\n"
              << "            " << fi.name << "_.clear();\n"
              << "            " << fi.name << "_.resize(ls.value->length());\n"
              << "            for (int64_t j = 0; j < ls.value->length(); ++j) {\n"
              << "                auto s = ls.value->GetScalar(j).ValueOrDie();\n"
              << "                " << fi.name << "_[j].SetFromScalars_(\n"
              << "                    static_cast<const arrow::StructScalar&>(*s).value);\n"
              << "            }\n"
              << "        }\n";
            break;

        case FieldKind::NESTED_LIST: {
            // Nullable: check validity first.
            if (fi.mapping.nullable) {
                o << "        if (!scalars[" << si << "]->is_valid) {\n"
                  << "            " << fi.name << "_.reset();\n"
                  << "        } else {\n";
            }

            // Target reference — either the optional's emplaced value or the bare member.
            std::string target = fi.mapping.nullable ? (fi.name + "_.emplace()") : (fi.name + "_");
            // For nullable, .emplace() returns the reference, but subsequent access uses *optional.
            std::string ref = fi.mapping.nullable ? ("(*" + fi.name + "_)") : (fi.name + "_");
            std::string indent = fi.mapping.nullable ? "    " : "";

            if (fi.mapping.list_depth == 2) {
                o << indent << "        {\n"
                  << indent
                  << "            const auto& ls = static_cast<const arrow::ListScalar&>(\n"
                  << indent << "                *scalars[" << si << "]);\n"
                  << indent << "            " << target << ";\n"
                  << indent << "            " << ref << ".clear();\n"
                  << indent << "            " << ref << ".resize(ls.value->length());\n"
                  << indent << "            for (int64_t i = 0; i < ls.value->length(); ++i) {\n"
                  << indent
                  << "                auto inner_s = ls.value->GetScalar(i).ValueOrDie();\n"
                  << indent
                  << "                const auto& inner_ls = static_cast<const "
                     "arrow::ListScalar&>(*inner_s);\n"
                  << indent << "                " << ref
                  << "[i].resize(inner_ls.value->length());\n"
                  << indent
                  << "                for (int64_t j = 0; j < inner_ls.value->length(); ++j) {\n"
                  << indent
                  << "                    auto s = inner_ls.value->GetScalar(j).ValueOrDie();\n"
                  << indent << "                    " << ref << "[i][j].SetFromScalars_(\n"
                  << indent
                  << "                        static_cast<const arrow::StructScalar&>(*s).value);\n"
                  << indent << "                }\n"
                  << indent << "            }\n"
                  << indent << "        }\n";
            } else if (fi.mapping.list_depth == 3) {
                o << indent << "        {\n"
                  << indent
                  << "            const auto& ls = static_cast<const arrow::ListScalar&>(\n"
                  << indent << "                *scalars[" << si << "]);\n"
                  << indent << "            " << target << ";\n"
                  << indent << "            " << ref << ".clear();\n"
                  << indent << "            " << ref << ".resize(ls.value->length());\n"
                  << indent << "            for (int64_t i = 0; i < ls.value->length(); ++i) {\n"
                  << indent << "                auto mid_s = ls.value->GetScalar(i).ValueOrDie();\n"
                  << indent
                  << "                const auto& mid_ls = static_cast<const "
                     "arrow::ListScalar&>(*mid_s);\n"
                  << indent << "                " << ref << "[i].resize(mid_ls.value->length());\n"
                  << indent
                  << "                for (int64_t j = 0; j < mid_ls.value->length(); ++j) {\n"
                  << indent
                  << "                    auto inner_s = mid_ls.value->GetScalar(j).ValueOrDie();\n"
                  << indent
                  << "                    const auto& inner_ls = static_cast<const "
                     "arrow::ListScalar&>(*inner_s);\n"
                  << indent << "                    " << ref
                  << "[i][j].resize(inner_ls.value->length());\n"
                  << indent
                  << "                    for (int64_t k = 0; k < inner_ls.value->length(); ++k) "
                     "{\n"
                  << indent
                  << "                        auto s = inner_ls.value->GetScalar(k).ValueOrDie();\n"
                  << indent << "                        " << ref << "[i][j][k].SetFromScalars_(\n"
                  << indent
                  << "                            static_cast<const "
                     "arrow::StructScalar&>(*s).value);\n"
                  << indent << "                    }\n"
                  << indent << "                }\n"
                  << indent << "            }\n"
                  << indent << "        }\n";
            }

            if (fi.mapping.nullable) {
                o << "        }\n";
            }
            break;
        }

        case FieldKind::MAP: {
            std::string key_extract =
                fi.mapping.map_key.value_is_buffer
                    ? "static_cast<const " + fi.mapping.map_key.scalar_type +
                          "&>(*ks).value->ToString()"
                    : "static_cast<const " + fi.mapping.map_key.scalar_type + "&>(*ks).value";

            o << "        {\n"
              << "            const auto& ms = static_cast<const arrow::MapScalar&>(\n"
              << "                *scalars[" << si << "]);\n"
              << "            const auto& sa = static_cast<const arrow::StructArray&>(\n"
              << "                *ms.value);\n"
              << "            " << fi.name << "_.clear();\n"
              << "            " << fi.name << "_.reserve(sa.length());\n"
              << "            for (int64_t j = 0; j < sa.length(); ++j) {\n"
              << "                auto ks = sa.field(0)->GetScalar(j).ValueOrDie();\n"
              << "                auto vs = sa.field(1)->GetScalar(j).ValueOrDie();\n";

            if (fi.mapping.map_value_is_message) {
                o << "                " << fi.name << "_.emplace_back(\n"
                  << "                    " << key_extract << ",\n"
                  << "                    " << fi.mapping.map_value_class << "());\n"
                  << "                " << fi.name << "_.back().second.SetFromScalars_(\n"
                  << "                    static_cast<const arrow::StructScalar&>(*vs).value);\n";
            } else {
                std::string val_extract =
                    fi.mapping.map_value.value_is_buffer
                        ? "static_cast<const " + fi.mapping.map_value.scalar_type +
                              "&>(*vs).value->ToString()"
                        : "static_cast<const " + fi.mapping.map_value.scalar_type + "&>(*vs).value";
                o << "                " << fi.name << "_.emplace_back(\n"
                  << "                    " << key_extract << ", " << val_extract << ");\n";
            }

            o << "            }\n"
              << "        }\n";
            break;
        }
    }
}

// -----------------------------------------------------------------------
// Gather supported fields from a message
// -----------------------------------------------------------------------

// `id_prefix` is the dotted field-number path of the chain of field-level
// flatten wrappers we have descended through (empty at the top level).  It is
// used to build a unique `field_id` for each inlined field: a field-flattened
// sub-message's fields keep their own (inner) proto `field_number`, which can
// collide with the enclosing message's numbers, so `field_id` carries the full
// path (e.g. "2.1") to disambiguate them in the schema metadata.
void GatherFieldsImpl(const google::protobuf::Descriptor* msg, std::vector<FieldInfo>& fields,
                      std::string* skipped_comment, const std::string& id_prefix) {
    for (int i = 0; i < msg->field_count(); ++i) {
        const auto* fd = msg->field(i);

        const std::string path = id_prefix.empty() ? std::to_string(fd->number())
                                                   : id_prefix + "." + std::to_string(fd->number());

        // Field-level flatten: inline the referenced message's fields, carrying
        // this field's number into the path so inlined field_ids stay unique.
        if (fd->type() == google::protobuf::FieldDescriptor::TYPE_MESSAGE && !fd->is_repeated() &&
            HasFieldFlatten(fd)) {
            GatherFieldsImpl(fd->message_type(), fields, skipped_comment, path);
            continue;
        }

        if (auto m = MapField(fd)) {
            fields.push_back({fd->name(), std::move(*m), fd->number(), path, fd});
        } else {
            *skipped_comment += "//   " + fd->name() + ": " + UnsupportedReason(fd) + "\n";
        }
    }
}

std::vector<FieldInfo> GatherFields(const google::protobuf::Descriptor* msg,
                                    std::string* skipped_comment) {
    std::vector<FieldInfo> fields;
    GatherFieldsImpl(msg, fields, skipped_comment, "");
    return fields;
}

// -----------------------------------------------------------------------
// Free schema function for one message
// -----------------------------------------------------------------------

// Helper: emit nanoarrow type setup code for a single child schema.
// `child_expr` is the C expression for the ArrowSchema* child pointer.
void EmitNanoarrowTypeSetup(std::ostringstream& o, const std::string& child_expr,
                            const FieldInfo& fi, const std::string& indent) {
    switch (fi.mapping.kind) {
        case FieldKind::SCALAR: {
            const auto& expr = fi.mapping.scalar.arrow_type_expr;
            if (expr.find("timestamp") != std::string::npos) {
                o << indent << "ArrowSchemaSetTypeDateTime(" << child_expr
                  << ", NANOARROW_TYPE_TIMESTAMP, NANOARROW_TIME_UNIT_NANO, nullptr);\n";
            } else if (expr.find("duration") != std::string::npos) {
                o << indent << "ArrowSchemaSetTypeDateTime(" << child_expr
                  << ", NANOARROW_TYPE_DURATION, NANOARROW_TIME_UNIT_NANO, nullptr);\n";
            } else if (expr == "arrow::boolean()") {
                o << indent << "ArrowSchemaSetType(" << child_expr << ", NANOARROW_TYPE_BOOL);\n";
            } else if (expr == "arrow::int32()") {
                o << indent << "ArrowSchemaSetType(" << child_expr << ", NANOARROW_TYPE_INT32);\n";
            } else if (expr == "arrow::int64()") {
                o << indent << "ArrowSchemaSetType(" << child_expr << ", NANOARROW_TYPE_INT64);\n";
            } else if (expr == "arrow::uint32()") {
                o << indent << "ArrowSchemaSetType(" << child_expr << ", NANOARROW_TYPE_UINT32);\n";
            } else if (expr == "arrow::uint64()") {
                o << indent << "ArrowSchemaSetType(" << child_expr << ", NANOARROW_TYPE_UINT64);\n";
            } else if (expr == "arrow::float32()") {
                o << indent << "ArrowSchemaSetType(" << child_expr << ", NANOARROW_TYPE_FLOAT);\n";
            } else if (expr == "arrow::float64()") {
                o << indent << "ArrowSchemaSetType(" << child_expr << ", NANOARROW_TYPE_DOUBLE);\n";
            } else if (expr == "arrow::utf8()") {
                o << indent << "ArrowSchemaSetType(" << child_expr << ", NANOARROW_TYPE_STRING);\n";
            } else if (expr == "arrow::binary()") {
                o << indent << "ArrowSchemaSetType(" << child_expr << ", NANOARROW_TYPE_BINARY);\n";
            } else {
                o << indent << "// TODO: unknown scalar type: " << expr << "\n";
            }
            break;
        }

        case FieldKind::STRUCT:
            o << indent << "ArrowSchemaDeepCopy(" << fi.mapping.nested_class << "Schema().get(), "
              << child_expr << ");\n";
            break;

        case FieldKind::REPEATED_SCALAR: {
            // list(element_type)
            o << indent << "ArrowSchemaSetType(" << child_expr << ", NANOARROW_TYPE_LIST);\n";
            // The list child ("item") is allocated by ArrowSchemaSetType.
            // Set item type.
            const auto& elem_expr = fi.mapping.element.arrow_type_expr;
            std::string item = child_expr + "->children[0]";
            if (elem_expr.find("timestamp") != std::string::npos) {
                o << indent << "ArrowSchemaSetTypeDateTime(" << item
                  << ", NANOARROW_TYPE_TIMESTAMP, NANOARROW_TIME_UNIT_NANO, nullptr);\n";
            } else if (elem_expr.find("duration") != std::string::npos) {
                o << indent << "ArrowSchemaSetTypeDateTime(" << item
                  << ", NANOARROW_TYPE_DURATION, NANOARROW_TIME_UNIT_NANO, nullptr);\n";
            } else if (elem_expr == "arrow::boolean()") {
                o << indent << "ArrowSchemaSetType(" << item << ", NANOARROW_TYPE_BOOL);\n";
            } else if (elem_expr == "arrow::int32()") {
                o << indent << "ArrowSchemaSetType(" << item << ", NANOARROW_TYPE_INT32);\n";
            } else if (elem_expr == "arrow::int64()") {
                o << indent << "ArrowSchemaSetType(" << item << ", NANOARROW_TYPE_INT64);\n";
            } else if (elem_expr == "arrow::uint32()") {
                o << indent << "ArrowSchemaSetType(" << item << ", NANOARROW_TYPE_UINT32);\n";
            } else if (elem_expr == "arrow::uint64()") {
                o << indent << "ArrowSchemaSetType(" << item << ", NANOARROW_TYPE_UINT64);\n";
            } else if (elem_expr == "arrow::float32()") {
                o << indent << "ArrowSchemaSetType(" << item << ", NANOARROW_TYPE_FLOAT);\n";
            } else if (elem_expr == "arrow::float64()") {
                o << indent << "ArrowSchemaSetType(" << item << ", NANOARROW_TYPE_DOUBLE);\n";
            } else if (elem_expr == "arrow::utf8()") {
                o << indent << "ArrowSchemaSetType(" << item << ", NANOARROW_TYPE_STRING);\n";
            } else if (elem_expr == "arrow::binary()") {
                o << indent << "ArrowSchemaSetType(" << item << ", NANOARROW_TYPE_BINARY);\n";
            } else {
                o << indent << "// TODO: unknown element type: " << elem_expr << "\n";
            }
            o << indent << "ArrowSchemaSetName(" << item << ", \"item\");\n";
            break;
        }

        case FieldKind::REPEATED_STRUCT:
            // list(struct(...))
            o << indent << "ArrowSchemaSetType(" << child_expr << ", NANOARROW_TYPE_LIST);\n";
            o << indent << "ArrowSchemaDeepCopy(" << fi.mapping.nested_class << "Schema().get(), "
              << child_expr << "->children[0]);\n";
            o << indent << "ArrowSchemaSetName(" << child_expr << "->children[0], \"item\");\n";
            break;

        case FieldKind::NESTED_LIST: {
            // List<List<...<Struct>>>
            // Build from outside in: list -> list -> ... -> struct
            std::string cur = child_expr;
            for (int d = 0; d < fi.mapping.list_depth; ++d) {
                o << indent << "ArrowSchemaSetType(" << cur << ", NANOARROW_TYPE_LIST);\n";
                std::string item = cur + "->children[0]";
                o << indent << "ArrowSchemaSetName(" << item << ", \"item\");\n";
                cur = item;
            }
            // Innermost: struct (deep copy overwrites the name, restore "item")
            o << indent << "ArrowSchemaDeepCopy(" << fi.mapping.nested_class << "Schema().get(), "
              << cur << ");\n";
            o << indent << "ArrowSchemaSetName(" << cur << ", \"item\");\n";
            break;
        }

        case FieldKind::MAP: {
            // map(key_type, value_type)
            o << indent << "ArrowSchemaSetType(" << child_expr << ", NANOARROW_TYPE_MAP);\n";
            // MAP creates a child "entries" struct with two children: "key" and "value".
            std::string entries = child_expr + "->children[0]";
            std::string key_child = entries + "->children[0]";
            std::string val_child = entries + "->children[1]";

            // Key type
            const auto& key_expr = fi.mapping.map_key.arrow_type_expr;
            if (key_expr == "arrow::utf8()") {
                o << indent << "ArrowSchemaSetType(" << key_child << ", NANOARROW_TYPE_STRING);\n";
            } else if (key_expr == "arrow::int32()") {
                o << indent << "ArrowSchemaSetType(" << key_child << ", NANOARROW_TYPE_INT32);\n";
            } else if (key_expr == "arrow::int64()") {
                o << indent << "ArrowSchemaSetType(" << key_child << ", NANOARROW_TYPE_INT64);\n";
            } else if (key_expr == "arrow::uint32()") {
                o << indent << "ArrowSchemaSetType(" << key_child << ", NANOARROW_TYPE_UINT32);\n";
            } else if (key_expr == "arrow::uint64()") {
                o << indent << "ArrowSchemaSetType(" << key_child << ", NANOARROW_TYPE_UINT64);\n";
            } else if (key_expr == "arrow::boolean()") {
                o << indent << "ArrowSchemaSetType(" << key_child << ", NANOARROW_TYPE_BOOL);\n";
            } else {
                o << indent << "// TODO: unknown map key type: " << key_expr << "\n";
            }

            // Value type
            if (fi.mapping.map_value_is_message) {
                o << indent << "ArrowSchemaDeepCopy(" << fi.mapping.map_value_class
                  << "Schema().get(), " << val_child << ");\n";
                o << indent << "ArrowSchemaSetName(" << val_child << ", \"value\");\n";
            } else {
                const auto& val_expr = fi.mapping.map_value.arrow_type_expr;
                if (val_expr == "arrow::utf8()") {
                    o << indent << "ArrowSchemaSetType(" << val_child
                      << ", NANOARROW_TYPE_STRING);\n";
                } else if (val_expr == "arrow::int32()") {
                    o << indent << "ArrowSchemaSetType(" << val_child
                      << ", NANOARROW_TYPE_INT32);\n";
                } else if (val_expr == "arrow::int64()") {
                    o << indent << "ArrowSchemaSetType(" << val_child
                      << ", NANOARROW_TYPE_INT64);\n";
                } else if (val_expr == "arrow::uint32()") {
                    o << indent << "ArrowSchemaSetType(" << val_child
                      << ", NANOARROW_TYPE_UINT32);\n";
                } else if (val_expr == "arrow::uint64()") {
                    o << indent << "ArrowSchemaSetType(" << val_child
                      << ", NANOARROW_TYPE_UINT64);\n";
                } else if (val_expr == "arrow::boolean()") {
                    o << indent << "ArrowSchemaSetType(" << val_child
                      << ", NANOARROW_TYPE_BOOL);\n";
                } else if (val_expr == "arrow::float32()") {
                    o << indent << "ArrowSchemaSetType(" << val_child
                      << ", NANOARROW_TYPE_FLOAT);\n";
                } else if (val_expr == "arrow::float64()") {
                    o << indent << "ArrowSchemaSetType(" << val_child
                      << ", NANOARROW_TYPE_DOUBLE);\n";
                } else if (val_expr == "arrow::binary()") {
                    o << indent << "ArrowSchemaSetType(" << val_child
                      << ", NANOARROW_TYPE_BINARY);\n";
                } else {
                    o << indent << "// TODO: unknown map value type: " << val_expr << "\n";
                }
            }
            break;
        }
    }  // switch
}

std::string GenerateSchemaFunction(const std::string& cls, const std::vector<FieldInfo>& fields,
                                   const google::protobuf::Descriptor* msg) {
    std::ostringstream o;

    o << "/// Returns the nanoarrow schema describing this message's wire layout.\n"
      << "/// Providers publish this schema on companion topics so that subscribers\n"
      << "/// can decode rows without prior knowledge of the message definition.\n";
    o << "inline fletcher::OwnedSchema " << cls << "Schema() {\n"
      << "    fletcher::OwnedSchema schema;\n"
      << "    ArrowSchemaInit(schema.get());\n"
      << "    ArrowSchemaSetTypeStruct(schema.get(), " << fields.size() << ");\n\n";

    // Schema-level metadata: proto_package + proto_message
    o << "    {\n"
      << "        struct ArrowBuffer buf;\n"
      << "        ArrowBufferInit(&buf);\n"
      << "        ArrowMetadataBuilderInit(&buf, nullptr);\n"
      << "        ArrowMetadataBuilderAppend(&buf,\n"
      << "            ArrowCharView(\"proto_package\"),\n"
      << "            ArrowCharView(\"" << msg->file()->package() << "\"));\n"
      << "        ArrowMetadataBuilderAppend(&buf,\n"
      << "            ArrowCharView(\"proto_message\"),\n"
      << "            ArrowCharView(\"" << msg->name() << "\"));\n"
      << "        ArrowSchemaSetMetadata(schema.get(),\n"
      << "            reinterpret_cast<const char*>(buf.data));\n"
      << "        ArrowBufferReset(&buf);\n"
      << "    }\n\n";

    for (size_t i = 0; i < fields.size(); ++i) {
        const auto& fi = fields[i];
        std::string ci = "schema->children[" + std::to_string(i) + "]";

        if (!fi.mapping.warning.empty()) o << "    // Warning: " << fi.mapping.warning << "\n";

        // Set field type
        EmitNanoarrowTypeSetup(o, ci, fi, "    ");

        // Set field name
        o << "    ArrowSchemaSetName(" << ci << ", \"" << fi.name << "\");\n";

        // Set nullable flag
        if (fi.mapping.nullable)
            o << "    " << ci << "->flags |= ARROW_FLAG_NULLABLE;\n";
        else
            o << "    " << ci << "->flags &= ~ARROW_FLAG_NULLABLE;\n";

        // Per-field metadata: field_number + field_id
        o << "    {\n"
          << "        struct ArrowBuffer buf;\n"
          << "        ArrowBufferInit(&buf);\n"
          << "        ArrowMetadataBuilderInit(&buf, nullptr);\n"
          << "        ArrowMetadataBuilderAppend(&buf,\n"
          << "            ArrowCharView(\"field_number\"),\n"
          << "            ArrowCharView(\"" << fi.field_number << "\"));\n"
          << "        ArrowMetadataBuilderAppend(&buf,\n"
          << "            ArrowCharView(\"field_id\"),\n"
          << "            ArrowCharView(\"" << fi.field_id << "\"));\n";

        o << "        ArrowSchemaSetMetadata(" << ci << ",\n"
          << "            reinterpret_cast<const char*>(buf.data));\n"
          << "        ArrowBufferReset(&buf);\n"
          << "    }\n\n";
    }

    o << "    return schema;\n"
      << "}\n";
    return o.str();
}

// -----------------------------------------------------------------------
// View helpers — derive Arrow array type from scalar type, getter return
// type from ScalarTypeInfo
// -----------------------------------------------------------------------

std::string ArrayTypeFromScalar(const std::string& scalar_type) {
    constexpr std::string_view suffix = "Scalar";
    if (scalar_type.size() > suffix.size() &&
        scalar_type.compare(scalar_type.size() - suffix.size(), suffix.size(), suffix) == 0) {
        return scalar_type.substr(0, scalar_type.size() - suffix.size()) + "Array";
    }
    return scalar_type;
}

std::string GetterType(const ScalarTypeInfo& sc) {
    return sc.value_is_buffer ? "std::string_view" : sc.storage_type;
}

// -----------------------------------------------------------------------
// View getter generation
// -----------------------------------------------------------------------

void EmitViewGetters(std::ostringstream& o, const std::vector<FieldInfo>& fields) {
    for (size_t idx = 0; idx < fields.size(); ++idx) {
        const auto& fi = fields[idx];
        const std::string si = std::to_string(idx);

        switch (fi.mapping.kind) {
            case FieldKind::SCALAR: {
                const auto& sc = fi.mapping.scalar;
                std::string ret = GetterType(sc);

                if (fi.mapping.nullable) {
                    o << "    std::optional<" << ret << "> " << fi.name << "() const {\n"
                      << "        if (!scalars_[" << si << "]->is_valid) return std::nullopt;\n";
                    if (sc.value_is_buffer) {
                        o << "        const auto& s = static_cast<const " << sc.scalar_type
                          << "&>(*scalars_[" << si << "]);\n"
                          << "        return std::string_view{\n"
                          << "            reinterpret_cast<const char*>"
                             "(s.value->data()),\n"
                          << "            static_cast<size_t>"
                             "(s.value->size())};\n";
                    } else {
                        o << "        return static_cast<const " << sc.scalar_type
                          << "&>(*scalars_[" << si << "]).value;\n";
                    }
                    o << "    }\n";
                } else {
                    o << "    " << ret << " " << fi.name << "() const {\n";
                    if (sc.value_is_buffer) {
                        o << "        const auto& s = static_cast<const " << sc.scalar_type
                          << "&>(*scalars_[" << si << "]);\n"
                          << "        return {reinterpret_cast<const char*>"
                             "(s.value->data()),\n"
                          << "                static_cast<size_t>"
                             "(s.value->size())};\n";
                    } else {
                        o << "        return static_cast<const " << sc.scalar_type
                          << "&>(*scalars_[" << si << "]).value;\n";
                    }
                    o << "    }\n";
                }
                break;
            }

            case FieldKind::STRUCT: {
                std::string vt = fi.mapping.nested_class + "View";
                if (fi.mapping.nullable) {
                    o << "    std::optional<" << vt << "> " << fi.name << "() const {\n"
                      << "        if (!scalars_[" << si << "]->is_valid) return std::nullopt;\n"
                      << "        return " << vt << "(scalars_[" << si << "]);\n"
                      << "    }\n";
                } else {
                    o << "    " << vt << " " << fi.name << "() const {\n"
                      << "        return " << vt << "(scalars_[" << si << "]);\n"
                      << "    }\n";
                }
                break;
            }

            case FieldKind::REPEATED_SCALAR: {
                const auto& el = fi.mapping.element;
                std::string vt = GetterType(el);
                std::string at = ArrayTypeFromScalar(el.scalar_type);
                o << "    fletcher::ArrowScalarList<" << vt << ", " << at << "> " << fi.name
                  << "() const {\n"
                  << "        const auto& ls = static_cast"
                     "<const arrow::ListScalar&>(\n"
                  << "            *scalars_[" << si << "]);\n"
                  << "        return fletcher::ArrowScalarList<" << vt << ", " << at
                  << ">(ls.value);\n"
                  << "    }\n";
                break;
            }

            case FieldKind::REPEATED_STRUCT: {
                std::string vt = fi.mapping.nested_class + "View";
                o << "    fletcher::ArrowRowViewList<" << vt << "> " << fi.name << "() const {\n"
                  << "        const auto& ls = static_cast"
                     "<const arrow::ListScalar&>(\n"
                  << "            *scalars_[" << si << "]);\n"
                  << "        return fletcher::ArrowRowViewList<" << vt << ">(ls.value);\n"
                  << "    }\n";
                break;
            }

            case FieldKind::NESTED_LIST: {
                std::string vt = fi.mapping.nested_class + "View";
                std::string tmpl = (fi.mapping.list_depth == 3)
                                       ? "fletcher::ArrowNestedList2<" + vt + ">"
                                       : "fletcher::ArrowNestedList<" + vt + ">";
                o << "    " << tmpl << " " << fi.name << "() const {\n"
                  << "        const auto& ls = static_cast"
                     "<const arrow::ListScalar&>(\n"
                  << "            *scalars_[" << si << "]);\n"
                  << "        return " << tmpl << "(ls.value);\n"
                  << "    }\n";
                break;
            }

            case FieldKind::MAP: {
                std::string kv = GetterType(fi.mapping.map_key);
                std::string ka = ArrayTypeFromScalar(fi.mapping.map_key.scalar_type);

                if (fi.mapping.map_value_is_message) {
                    std::string vt = fi.mapping.map_value_class + "View";
                    o << "    fletcher::ArrowRowViewMap<" << kv << ", " << ka << ", " << vt << "> "
                      << fi.name << "() const {\n"
                      << "        const auto& ms = static_cast"
                         "<const arrow::MapScalar&>(\n"
                      << "            *scalars_[" << si << "]);\n"
                      << "        return fletcher::ArrowRowViewMap<" << kv << ", " << ka << ", "
                      << vt << ">(ms.value);\n"
                      << "    }\n";
                } else {
                    std::string vv = GetterType(fi.mapping.map_value);
                    std::string va = ArrayTypeFromScalar(fi.mapping.map_value.scalar_type);
                    o << "    fletcher::ArrowScalarMap<" << kv << ", " << ka << ", " << vv << ", "
                      << va << "> " << fi.name << "() const {\n"
                      << "        const auto& ms = static_cast"
                         "<const arrow::MapScalar&>(\n"
                      << "            *scalars_[" << si << "]);\n"
                      << "        return fletcher::ArrowScalarMap<" << kv << ", " << ka << ", "
                      << vv << ", " << va << ">(ms.value);\n"
                      << "    }\n";
                }
                break;
            }
        }
    }
}

// -----------------------------------------------------------------------
// Full immutable view class generation for one message
// -----------------------------------------------------------------------

std::string GenerateViewClass(const std::string& view_cls, const std::vector<FieldInfo>& fields) {
    std::ostringstream o;

    o << "/// Immutable, typed view over a row of Arrow scalars.\n"
      << "///\n"
      << "/// Wraps an ArrowRow, a StructScalar, a RecordBatch row, or a Table row\n"
      << "/// and exposes each field through a zero-copy typed getter.\n"
      << "class " << view_cls << " {\n public:\n";

    // Constructor from ArrowRow (vector of scalars)
    o << "    /// Wraps a pre-existing vector of Arrow scalars (e.g. from ToArrowRow()\n"
      << "    /// or Codec::DecodeRow).\n"
      << "    explicit " << view_cls << "(fletcher::ArrowRow scalars)\n"
      << "        : scalars_(std::move(scalars)) {}\n\n";

    // Constructor from shared_ptr<Scalar> (for nested struct views)
    o << "    /// Wraps a StructScalar — used when this message is a nested struct\n"
      << "    /// field inside a parent view.\n"
      << "    explicit " << view_cls << "(std::shared_ptr<arrow::Scalar> scalar)\n"
      << "        : scalars_(static_cast<const arrow::StructScalar&>"
         "(*scalar).value) {}\n\n";

    // Constructor from RecordBatch + row index
    o << "    /// Extracts one row from a RecordBatch by index, materialising each\n"
      << "    /// column value as a scalar.\n"
      << "    " << view_cls << "(const arrow::RecordBatch& batch, int64_t row) {\n"
      << "        scalars_.reserve(batch.num_columns());\n"
      << "        for (int i = 0; i < batch.num_columns(); ++i)\n"
      << "            scalars_.push_back(\n"
      << "                batch.column(i)->GetScalar(row).ValueOrDie());\n"
      << "    }\n\n";

    // Constructor from Table + row index
    o << "    /// Extracts one row from a Table by index, resolving the correct chunk\n"
      << "    /// in each ChunkedArray.\n"
      << "    " << view_cls << "(const arrow::Table& table, int64_t row) {\n"
      << "        scalars_.reserve(table.num_columns());\n"
      << "        for (int i = 0; i < table.num_columns(); ++i) {\n"
      << "            const auto& chunked = *table.column(i);\n"
      << "            int64_t offset = row;\n"
      << "            for (const auto& chunk : chunked.chunks()) {\n"
      << "                if (offset < chunk->length()) {\n"
      << "                    scalars_.push_back(\n"
      << "                        chunk->GetScalar(offset).ValueOrDie());\n"
      << "                    break;\n"
      << "                }\n"
      << "                offset -= chunk->length();\n"
      << "            }\n"
      << "        }\n"
      << "    }\n\n";

    // Getters
    EmitViewGetters(o, fields);

    // Private
    o << "\n private:\n"
      << "    fletcher::ArrowRow scalars_;\n";

    o << "};\n";
    return o.str();
}

// -----------------------------------------------------------------------
// EncodeTo / EncodeStructTo_ — direct encoding to WriteBuffer
// -----------------------------------------------------------------------

// Helper: return the PositionalWriter method name for a scalar storage type.
std::string PositionalWriteCall(const ScalarTypeInfo& info) {
    const auto& st = info.storage_type;
    const auto& expr = info.arrow_type_expr;
    if (expr.find("timestamp") != std::string::npos) return "WriteTimestamp";
    if (expr.find("duration") != std::string::npos) return "WriteDuration";
    if (st == "bool") return "WriteBool";
    if (st == "int32_t") return "WriteInt32";
    if (st == "int64_t") return "WriteInt64";
    if (st == "uint32_t") return "WriteUint32";
    if (st == "uint64_t") return "WriteUint64";
    if (st == "float") return "WriteFloat";
    if (st == "double") return "WriteDouble";
    if (st == "std::string") {
        if (expr == "arrow::binary()") return "WriteBinary";
        return "WriteString";
    }
    return "/* unknown write */";
}

// Helper: return the PositionalReader method name for a scalar storage type.
std::string PositionalReadCall(const ScalarTypeInfo& info) {
    const auto& st = info.storage_type;
    const auto& expr = info.arrow_type_expr;
    if (expr.find("timestamp") != std::string::npos) return "ReadTimestamp";
    if (expr.find("duration") != std::string::npos) return "ReadDuration";
    if (st == "bool") return "ReadBool";
    if (st == "int32_t") return "ReadInt32";
    if (st == "int64_t") return "ReadInt64";
    if (st == "uint32_t") return "ReadUint32";
    if (st == "uint64_t") return "ReadUint64";
    if (st == "float") return "ReadFloat";
    if (st == "double") return "ReadDouble";
    if (st == "std::string") {
        if (expr == "arrow::binary()") return "ReadBinary";
        return "ReadString";
    }
    return "/* unknown read */";
}

// Emit the scalar write expression for a value through a PositionalWriter.
void EmitScalarWrite(std::ostringstream& o, const ScalarTypeInfo& info, const std::string& val_expr,
                     const std::string& indent) {
    std::string method = PositionalWriteCall(info);
    if (method == "WriteBinary") {
        o << indent << "w.WriteBinary(reinterpret_cast<const uint8_t*>(" << val_expr << ".data()), "
          << val_expr << ".size());\n";
    } else {
        o << indent << "w." << method << "(" << val_expr << ");\n";
    }
}

// Emit positional-format encoding for a single field.
void EmitFieldEncode(std::ostringstream& o, const FieldInfo& fi, size_t idx) {
    const std::string n = fi.name + "_";
    const std::string si = std::to_string(idx);

    switch (fi.mapping.kind) {
        case FieldKind::SCALAR: {
            const auto& s = fi.mapping.scalar;
            if (fi.mapping.nullable) {
                o << "        if (!" << n << ".has_value()) w.SetNull(" << si << ");\n"
                  << "        else ";
                std::string method = PositionalWriteCall(s);
                if (method == "WriteBinary") {
                    o << "w.WriteBinary(reinterpret_cast<const uint8_t*>(" << "*" << n
                      << "->data()), " << n << "->size());\n";
                } else {
                    o << "w." << method << "(*" << n << ");\n";
                }
            } else {
                // Non-nullable: write default if not set
                std::string method = PositionalWriteCall(s);
                if (method == "WriteBinary") {
                    o << "        { const auto& val = " << n << ".value_or(" << s.default_value
                      << ");\n"
                      << "          w.WriteBinary(reinterpret_cast<const uint8_t*>(val.data()), "
                         "val.size()); }\n";
                } else {
                    o << "        w." << method << "(" << n << ".value_or(" << s.default_value
                      << "));\n";
                }
            }
            break;
        }

        case FieldKind::STRUCT: {
            const auto& nc = fi.mapping.nested_class;
            if (fi.mapping.nullable) {
                o << "        if (!" << n << ".has_value()) w.SetNull(" << si << ");\n"
                  << "        else { auto sw = w.BeginStruct(" << nc << "Schema()->n_children); "
                  << n << "->EncodeStructTo_(sw); }\n";
            } else {
                o << "        { auto sw = w.BeginStruct(" << nc << "Schema()->n_children);\n"
                  << "          if (" << n << ".has_value()) " << n << "->EncodeStructTo_(sw);\n"
                  << "          else " << nc << "().EncodeStructTo_(sw); }\n";
            }
            break;
        }

        case FieldKind::REPEATED_SCALAR: {
            const auto& e = fi.mapping.element;
            o << "        { auto lc = w.BeginList(static_cast<uint32_t>(" << n << ".size()));\n"
              << "          for (uint32_t li_ = 0; li_ < " << n << ".size(); ++li_) {\n"
              << "            ";
            EmitScalarWrite(o, e, n + "[li_]", "");
            o << "          } }\n";
            break;
        }

        case FieldKind::REPEATED_STRUCT: {
            const auto& nc = fi.mapping.nested_class;
            o << "        { auto lc = w.BeginList(static_cast<uint32_t>(" << n << ".size()));\n"
              << "          for (uint32_t li_ = 0; li_ < " << n << ".size(); ++li_) {\n"
              << "            auto sw = w.BeginStruct(" << nc << "Schema()->n_children);\n"
              << "            " << n << "[li_].EncodeStructTo_(sw);\n"
              << "          } }\n";
            break;
        }

        case FieldKind::NESTED_LIST: {
            int depth = fi.mapping.list_depth;
            const auto& nc = fi.mapping.nested_class;

            // For nullable, check the optional
            if (fi.mapping.nullable) {
                o << "        if (!" << n << ".has_value()) w.SetNull(" << si << ");\n"
                  << "        else {\n";
            } else {
                o << "        {\n";
            }

            std::string src = fi.mapping.nullable ? ("(*" + n + ")") : n;

            // Nested loops for each list depth
            for (int d = 0; d < depth; ++d) {
                std::string var = "nl_" + std::to_string(d);
                o << "            auto lc_" << d << " = w.BeginList(static_cast<uint32_t>(" << src
                  << ".size()));\n";
                o << "            for (const auto& " << var << " : " << src << ") {\n";
                src = var;
            }
            // Innermost: encode struct
            o << "                auto sw = w.BeginStruct(" << nc << "Schema()->n_children);\n"
              << "                " << src << ".EncodeStructTo_(sw);\n";
            // Close loops
            for (int d = 0; d < depth; ++d) o << "            }\n";

            o << "        }\n";
            break;
        }

        case FieldKind::MAP: {
            const auto& mk = fi.mapping.map_key;
            bool val_is_msg = fi.mapping.map_value_is_message;

            o << "        { auto mc = w.BeginMap(static_cast<uint32_t>(" << n << ".size()));\n"
              << "          for (const auto& [k, v] : " << n << ") {\n"
              << "            ";
            EmitScalarWrite(o, mk, "k", "");
            o << "          }\n"
              << "          auto vc = mc.BeginValues();\n"
              << "          for (const auto& [k, v] : " << n << ") {\n";

            if (val_is_msg) {
                const auto& mvc = fi.mapping.map_value_class;
                o << "            auto sw = w.BeginStruct(" << mvc << "Schema()->n_children);\n"
                  << "            v.EncodeStructTo_(sw);\n";
            } else {
                o << "            ";
                EmitScalarWrite(o, fi.mapping.map_value, "v", "");
            }

            o << "          } }\n";
            break;
        }
    }  // switch
}

// Emit EncodeTo, EncodeStructTo_, and Encode methods for a message class.
void EmitEncodeTo(std::ostringstream& o, const std::string& cls,
                  const std::vector<FieldInfo>& fields) {
    std::string fc = std::to_string(fields.size());

    // EncodeStructTo_ — positional format, writes fields into a parent-provided writer.
    o << "    /// Internal: writes this message's fields into a parent writer\n"
      << "    /// when the message is nested as a struct field inside another row.\n";
    o << "    void EncodeStructTo_(fletcher::PositionalWriter& w) const {\n";
    for (size_t i = 0; i < fields.size(); ++i) EmitFieldEncode(o, fields[i], i);
    o << "    }\n\n";

    // EncodeTo — creates a PositionalWriter and writes fields positionally.
    o << "    /// Serialises the row into the given buffer in the positional wire\n"
      << "    /// format. Used by Publisher to write directly into the provider's\n"
      << "    /// transport buffer without an intermediate copy.\n";
    o << "    void EncodeTo(fletcher::WriteBuffer& buf) const {\n"
      << "        fletcher::PositionalWriter w(buf, " << fc << ");\n";
    for (size_t i = 0; i < fields.size(); ++i) EmitFieldEncode(o, fields[i], i);
    o << "    }\n\n";

    // Encode() — convenience returning EncodedRow.
    o << "    /// Convenience method that returns a self-contained EncodedRow.\n"
      << "    /// Useful for testing, WAL storage, or any context where you need\n"
      << "    /// the encoded bytes as a standalone value.\n";
    o << "    fletcher::EncodedRow Encode() const {\n"
      << "        fletcher::EncodedRow row;\n"
      << "        fletcher::VectorWriteBuffer buf(row);\n"
      << "        EncodeTo(buf);\n"
      << "        return row;\n"
      << "    }\n\n";
}

// -----------------------------------------------------------------------
// Full class generation for one message
// -----------------------------------------------------------------------

// Emit positional decode for a single field from a PositionalReader.
void EmitFieldDecode(std::ostringstream& o, const FieldInfo& fi, size_t idx) {
    const std::string n = fi.name + "_";
    const std::string si = std::to_string(idx);

    switch (fi.mapping.kind) {
        case FieldKind::SCALAR: {
            const auto& s = fi.mapping.scalar;
            std::string method = PositionalReadCall(s);
            if (method == "ReadBinary") {
                // Binary: returns pair<const uint8_t*, size_t>
                if (fi.mapping.nullable) {
                    o << "        if (!r.IsNull(" << si << ")) {\n"
                      << "            auto [p, n] = r.ReadBinary();\n"
                      << "            " << n << ".emplace(reinterpret_cast<const char*>(p), n);\n"
                      << "        }\n";
                } else {
                    o << "        { auto [p, n] = r.ReadBinary();\n"
                      << "          " << n << ".emplace(reinterpret_cast<const char*>(p), n); }\n";
                }
            } else if (method == "ReadString") {
                if (fi.mapping.nullable) {
                    o << "        if (!r.IsNull(" << si << ")) " << n << " = std::string(r."
                      << method << "());\n";
                } else {
                    o << "        " << n << " = std::string(r." << method << "());\n";
                }
            } else {
                if (fi.mapping.nullable) {
                    o << "        if (!r.IsNull(" << si << ")) " << n << " = r." << method
                      << "();\n";
                } else {
                    o << "        " << n << " = r." << method << "();\n";
                }
            }
            break;
        }

        case FieldKind::STRUCT: {
            const auto& nc = fi.mapping.nested_class;
            if (fi.mapping.nullable) {
                o << "        if (!r.IsNull(" << si << ")) {\n"
                  << "            auto sr = r.ReadStruct(" << nc << "Schema()->n_children);\n"
                  << "            " << n << ".emplace(sr);\n"
                  << "        }\n";
            } else {
                o << "        { auto sr = r.ReadStruct(" << nc << "Schema()->n_children);\n"
                  << "          " << n << ".emplace(sr); }\n";
            }
            break;
        }

        case FieldKind::REPEATED_SCALAR: {
            const auto& e = fi.mapping.element;
            std::string method = PositionalReadCall(e);
            o << "        { auto lh = r.ReadListHeader();\n"
              << "          " << n << ".clear();\n"
              << "          " << n << ".reserve(lh.count);\n"
              << "          for (uint32_t li_ = 0; li_ < lh.count; ++li_) {\n";
            if (method == "ReadBinary") {
                o << "            auto [p, n] = r.ReadBinary();\n"
                  << "            " << n << ".emplace_back(reinterpret_cast<const char*>(p), n);\n";
            } else if (method == "ReadString") {
                o << "            " << n << ".emplace_back(r." << method << "());\n";
            } else {
                o << "            " << n << ".push_back(r." << method << "());\n";
            }
            o << "          } }\n";
            break;
        }

        case FieldKind::REPEATED_STRUCT: {
            const auto& nc = fi.mapping.nested_class;
            o << "        { auto lh = r.ReadListHeader();\n"
              << "          " << n << ".clear();\n"
              << "          " << n << ".reserve(lh.count);\n"
              << "          for (uint32_t li_ = 0; li_ < lh.count; ++li_) {\n"
              << "            auto sr = r.ReadStruct(" << nc << "Schema()->n_children);\n"
              << "            " << n << ".emplace_back(sr);\n"
              << "          } }\n";
            break;
        }

        case FieldKind::NESTED_LIST: {
            int depth = fi.mapping.list_depth;
            const auto& nc = fi.mapping.nested_class;

            if (fi.mapping.nullable) {
                o << "        if (!r.IsNull(" << si << ")) {\n";
            } else {
                o << "        {\n";
            }

            std::string target = fi.mapping.nullable ? (n + ".emplace()") : n;
            std::string ref = fi.mapping.nullable ? ("(*" + n + ")") : n;
            std::string indent = "            ";

            if (fi.mapping.nullable) {
                o << indent << target << ";\n";
            }

            // Generate nested loops
            // depth 2: List<List<Struct>>
            // depth 3: List<List<List<Struct>>>
            std::string cur_ref = ref;
            for (int d = 0; d < depth; ++d) {
                std::string var = "lh_" + std::to_string(d);
                std::string idx_var = "i_" + std::to_string(d);
                o << indent << "auto " << var << " = r.ReadListHeader();\n"
                  << indent << cur_ref << ".resize(" << var << ".count);\n"
                  << indent << "for (uint32_t " << idx_var << " = 0; " << idx_var << " < " << var
                  << ".count; ++" << idx_var << ") {\n";
                cur_ref = cur_ref + "[" + idx_var + "]";
                indent += "    ";
            }
            // Innermost: decode struct
            o << indent << "auto sr = r.ReadStruct(" << nc << "Schema()->n_children);\n"
              << indent << cur_ref << " = " << nc << "(sr);\n";
            // Close loops
            for (int d = 0; d < depth; ++d) {
                indent = indent.substr(4);
                o << indent << "}\n";
            }

            o << "        }\n";
            break;
        }

        case FieldKind::MAP: {
            const auto& mk = fi.mapping.map_key;
            bool val_is_msg = fi.mapping.map_value_is_message;
            std::string key_read = PositionalReadCall(mk);
            std::string key_type = mk.storage_type;
            std::string val_type =
                val_is_msg ? fi.mapping.map_value_class : fi.mapping.map_value.storage_type;

            o << "        { auto count = r.ReadMapCount();\n"
              << "          std::vector<" << key_type << "> keys_;\n"
              << "          keys_.reserve(count);\n"
              << "          for (uint32_t mi_ = 0; mi_ < count; ++mi_) {\n";
            if (key_read == "ReadString") {
                o << "            keys_.emplace_back(r." << key_read << "());\n";
            } else {
                o << "            keys_.push_back(r." << key_read << "());\n";
            }
            o << "          }\n"
              << "          auto vbf = r.ReadMapValueBitfield(count);\n"
              << "          " << n << ".clear();\n"
              << "          " << n << ".reserve(count);\n"
              << "          for (uint32_t mi_ = 0; mi_ < count; ++mi_) {\n";

            if (val_is_msg) {
                const auto& mvc = fi.mapping.map_value_class;
                o << "            auto sr = r.ReadStruct(" << mvc << "Schema()->n_children);\n"
                  << "            " << n << ".emplace_back(std::move(keys_[mi_]), " << mvc
                  << "(sr));\n";
            } else {
                std::string val_read = PositionalReadCall(fi.mapping.map_value);
                if (val_read == "ReadString") {
                    o << "            " << n << ".emplace_back(std::move(keys_[mi_]), "
                      << "std::string(r." << val_read << "()));\n";
                } else if (val_read == "ReadBinary") {
                    o << "            auto [p, n] = r.ReadBinary();\n"
                      << "            " << n << ".emplace_back(std::move(keys_[mi_]), "
                      << "std::string(reinterpret_cast<const char*>(p), n));\n";
                } else {
                    o << "            " << n << ".emplace_back(std::move(keys_[mi_]), " << "r."
                      << val_read << "());\n";
                }
            }

            o << "          } }\n";
            break;
        }
    }  // switch
}

std::string GenerateMessageClass(const std::string& cls, const std::vector<FieldInfo>& fields) {
    std::ostringstream o;
    std::string fc = std::to_string(fields.size());

    // ---- class header ---------------------------------------------------
    o << "class " << cls << " {\n public:\n";

    // Default constructor
    o << "    /// Constructs an empty row. Use the setters to populate fields\n"
      << "    /// before calling Encode() to produce the wire-format buffer.\n";
    o << "    " << cls << "() = default;\n\n";

    // Constructor from raw bytes
    o << "    /// Reconstructs a row from a raw wire-format buffer, e.g. one\n"
      << "    /// received from a Subscriber callback or read from a WAL.\n";
    o << "    explicit " << cls << "(const uint8_t* data, size_t len) {\n"
      << "        fletcher::PositionalReader r(data, len, " << fc << ");\n";
    for (size_t i = 0; i < fields.size(); ++i) EmitFieldDecode(o, fields[i], i);
    o << "    }\n\n";

    // Constructor from EncodedRow
    o << "    /// Convenience overload that accepts an EncodedRow directly,\n"
      << "    /// so callers do not need to extract the pointer and length.\n";
    o << "    explicit " << cls << "(const fletcher::EncodedRow& row)\n"
      << "        : " << cls << "(row.data(), row.size()) {}\n\n";

    // Constructor from PositionalReader& (for nested struct decoding)
    o << "    /// Used internally when this message is embedded as a struct\n"
      << "    /// field inside another message — the parent reader is passed\n"
      << "    /// through so nested fields are decoded in position.\n";
    o << "    explicit " << cls << "(fletcher::PositionalReader& r) {\n";
    for (size_t i = 0; i < fields.size(); ++i) EmitFieldDecode(o, fields[i], i);
    o << "    }\n\n";

    // Setters
    EmitSetters(o, cls, fields);
    o << "\n";

    // Getters
    EmitGetters(o, fields);
    o << "\n";

    // EncodeTo / EncodeStructTo_ / Encode — positional format encoding
    EmitEncodeTo(o, cls, fields);

    // ---- private section ------------------------------------------------
    o << " private:\n";

    // Storage members
    for (const auto& fi : fields) o << "    " << StorageDecl(fi) << ";\n";

    o << "};\n";
    return o.str();
}

// -----------------------------------------------------------------------
// Service method validation
// -----------------------------------------------------------------------

bool ValidateServiceMethod(const google::protobuf::MethodDescriptor* method,
                           const std::set<const google::protobuf::Descriptor*>& generated_msgs,
                           std::string* reason) {
    if (!method->client_streaming()) {
        *reason = "request is not streaming (pub/sub requires 'stream' on request)";
        return false;
    }
    if (method->server_streaming()) {
        *reason = "server-streaming is not supported for pub/sub (no replies)";
        return false;
    }
    if (method->output_type()->full_name() != "google.protobuf.Empty") {
        *reason = "return type must be google.protobuf.Empty for pub/sub";
        return false;
    }
    if (!generated_msgs.count(method->input_type())) {
        *reason = "input message '" + method->input_type()->name() +
                  "' has no generated Arrow mapping in this file";
        return false;
    }
    return true;
}

// -----------------------------------------------------------------------
// Per-class helpers shared by publisher and subscriber generation
// -----------------------------------------------------------------------

void EmitTopicSegments(std::ostringstream& o, const std::string& package,
                       const std::string& svc_name, const std::string& method_name) {
    o << "    /// Returns the hierarchical topic path derived from the proto\n"
      << "    /// service and method names. Used by the Publisher/Subscriber to\n"
      << "    /// register, look up, and route messages on the pub/sub layer.\n";
    o << "    static const std::vector<std::string>& TopicSegments() {\n"
      << "        static const std::vector<std::string> kSegments = {";
    if (!package.empty()) o << "\n            \"" << package << "\",";
    o << "\n            \"" << svc_name << "\"," << "\n            \"" << method_name << "\"\n"
      << "        };\n"
      << "        return kSegments;\n"
      << "    }\n\n";
}

void EmitSchema(std::ostringstream& o, const std::string& msg_class) {
    o << "    /// Exposes the message schema on the publisher/subscriber class so\n"
      << "    /// callers can inspect the wire layout without constructing a row.\n";
    o << "    static fletcher::OwnedSchema Schema() {\n"
      << "        return " << msg_class << "Schema();\n"
      << "    }\n\n";
}

// -----------------------------------------------------------------------
// Publisher class generation for a single service method
// -----------------------------------------------------------------------

std::string GeneratePublisherClass(const google::protobuf::MethodDescriptor* method,
                                   const std::string& package) {
    const std::string svc_name = method->service()->name();
    const std::string method_name = method->name();
    const std::string cls = svc_name + "_" + method_name + "Publisher";
    const std::string msg_class = ClassName(method->input_type());

    std::ostringstream o;
    o << "class " << cls << " {\n public:\n";

    EmitTopicSegments(o, package, svc_name, method_name);
    EmitSchema(o, msg_class);

    // Topic key (segments joined with '/')
    o << "    /// Returns the flat string form of the topic path (segments joined\n"
      << "    /// with '/'). Useful for logging, diagnostics, and WebGateway URLs\n"
      << "    /// where a single string is expected instead of path segments.\n";
    o << "    static const std::string& TopicKey() {\n"
      << "        static const std::string kKey = \"";
    if (!package.empty()) o << package << "/";
    o << svc_name << "/" << method_name << "\";\n"
      << "        return kKey;\n"
      << "    }\n\n";

    // Constructor
    o << "    /// Creates the publisher and registers the topic with its schema\n"
      << "    /// on the provider. After construction, subscribers can discover\n"
      << "    /// the topic and receive the schema for decoding.\n";
    o << "    explicit " << cls << "(\n"
      << "            std::shared_ptr<fletcher::PubSubProvider> provider)\n"
      << "        : publisher_(std::make_unique<fletcher::Publisher>(std::move(provider)))\n"
      << "    {\n"
      << "        publisher_->CreateTopic(TopicSegments(), " << msg_class << "Schema());\n"
      << "    }\n\n";

    // Publish (without attachments)
    o << "    /// Encodes and publishes a single row. The encoding happens\n"
      << "    /// directly into the provider's transport buffer to avoid copies.\n";
    o << "    void Publish(const " << msg_class << "& row) {\n"
      << "        publisher_->Publish(TopicSegments(),\n"
      << "            [&](fletcher::WriteBuffer& buf) { row.EncodeTo(buf); });\n"
      << "    }\n\n";

    // Publish (with attachments)
    o << "    /// Publishes a row together with keyed binary attachments (e.g.\n"
      << "    /// images, point clouds) that travel alongside the row as part\n"
      << "    /// of the same Envelope.\n";
    o << "    void Publish(const " << msg_class << "& row,\n"
      << "                 fletcher::Attachments attachments) {\n"
      << "        publisher_->Publish(TopicSegments(),\n"
      << "            [&](fletcher::WriteBuffer& buf) { row.EncodeTo(buf); },\n"
      << "            std::move(attachments));\n"
      << "    }\n\n";

    // Private
    o << " private:\n"
      << "    std::unique_ptr<fletcher::Publisher> publisher_;\n"
      << "};\n";

    return o.str();
}

// -----------------------------------------------------------------------
// Subscriber class generation for a single service method
// -----------------------------------------------------------------------

std::string GenerateSubscriberClass(const google::protobuf::MethodDescriptor* method,
                                    const std::string& package) {
    const std::string svc_name = method->service()->name();
    const std::string method_name = method->name();
    const std::string cls = svc_name + "_" + method_name + "Subscriber";
    const std::string msg_class = ClassName(method->input_type());

    std::ostringstream o;
    o << "class " << cls << " {\n public:\n";

    EmitTopicSegments(o, package, svc_name, method_name);
    EmitSchema(o, msg_class);

    // Constructor — subscriber does not call CreateTopic; the schema
    // is discovered from the provider when Subscribe() is called.
    o << "    /// Binds to the provider without creating a topic — subscribers\n"
      << "    /// discover the topic and its schema when Subscribe() is called.\n";
    o << "    explicit " << cls << "(\n"
      << "            std::shared_ptr<fletcher::PubSubProvider> provider)\n"
      << "        : subscriber_(std::make_unique<fletcher::Subscriber>(std::move(provider))) "
         "{}\n\n";

    // Subscribe — delivers decoded message + Attachments to the caller.
    // Returns the schema received from the publisher via the provider.
    o << "    /// Begins receiving rows on this topic. The raw wire-format bytes\n"
      << "    /// are decoded into a typed message before being delivered to the\n"
      << "    /// callback, so subscribers never handle raw buffers directly.\n"
      << "    /// Returns the subscription ID (used for Unsubscribe).\n";
    o << "    uint64_t Subscribe(\n"
      << "        std::function<void(" << msg_class << ", fletcher::Attachments)> cb)\n"
      << "    {\n"
      << "        auto result = subscriber_->Subscribe(TopicSegments(),\n"
      << "            [cb = std::move(cb)](uint64_t /*subscription_id*/,\n"
      << "                                 const uint8_t* data, size_t len,\n"
      << "                                 fletcher::SharedSchema /*schema*/,\n"
      << "                                 fletcher::Attachments att) {\n"
      << "                cb(" << msg_class << "(data, len), std::move(att));\n"
      << "            });\n"
      << "        return result.subscription_id;\n"
      << "    }\n\n";

    // Unsubscribe
    o << "    /// Stops delivery and releases the subscription on the provider.\n";
    o << "    void Unsubscribe(uint64_t subscription_id) {\n"
      << "        subscriber_->Unsubscribe(subscription_id);\n"
      << "    }\n\n";

    // Private
    o << " private:\n"
      << "    std::unique_ptr<fletcher::Subscriber> subscriber_;\n"
      << "};\n";

    return o.str();
}

// -----------------------------------------------------------------------
// File generation
// -----------------------------------------------------------------------

std::string GenerateFile(const google::protobuf::FileDescriptor* file, bool schema_only) {
    std::ostringstream o;

    o << "// Generated by fletcher-protoc. DO NOT EDIT.\n"
      << "// Source: " << file->name() << "\n"
      << "#pragma once\n\n"
      << "#include <nanoarrow/nanoarrow.h>\n"
      << "#include <fletcher/pubsub/owned_schema.hpp>\n"
      << "#include <fletcher/core/types.hpp>\n";

    if (!schema_only) {
        o << "#include <fletcher/core/positional_io.hpp>\n"
          << "#include <fletcher/core/write_buffer.hpp>\n";
    }

    o << "\n"
      << "#include <cstdint>\n"
      << "#include <memory>\n"
      << "#include <vector>\n";

    if (!schema_only) {
        o << "#include <optional>\n"
          << "#include <string>\n"
          << "#include <string_view>\n"
          << "#include <utility>\n";
    }

    if (!schema_only && file->service_count() > 0) {
        o << "#include <fletcher/pubsub/provider.hpp>\n"
          << "#include <fletcher/pubsub/publisher.hpp>\n"
          << "#include <fletcher/pubsub/subscriber.hpp>\n"
          << "#include <functional>\n";
    }

    // TODO: CRS utilities — will be restored in a later phase.

    // Cross-file generated headers (for referenced messages from other .proto files).
    const auto cross_includes = CollectCrossFileIncludes(file);
    if (!cross_includes.empty()) {
        o << "\n";
        for (const auto& h : cross_includes) o << "#include \"" << h << "\"\n";
    }
    o << "\n";

    o << "namespace fletcher_gen {\n";
    const std::string ns = DotToColons(file->package());
    if (!ns.empty()) o << "namespace " << ns << " {\n";
    o << "\n";

    // Emit messages in dependency order.
    auto messages = OrderedMessages(file);
    std::set<const google::protobuf::Descriptor*> generated_msgs;

    for (const auto* msg : messages) {
        if (IsRecursive(msg)) {
            o << "// Skipped: " << msg->name()
              << " is recursive and cannot be represented in Arrow.\n\n";
            continue;
        }

        // Flattened wrapper messages are absorbed into the parent's schema
        // by the type mapper — skip class generation.
        if (IsFlattenedWrapper(msg)) continue;

        std::string skipped;
        auto fields = GatherFields(msg, &skipped);
        const std::string cls = ClassName(msg);

        generated_msgs.insert(msg);

        if (!skipped.empty()) {
            o << "// Note: the following fields of " << msg->name()
              << " have no Arrow mapping and are absent from the schema:\n"
              << skipped << "//\n";
        }

        // Always emit the free schema function.
        o << GenerateSchemaFunction(cls, fields, msg) << "\n";

        // Optionally emit the row class.
        // View class omitted — generated separately in .fletcher.arrow.pb.h.
        if (!schema_only) {
            o << GenerateMessageClass(cls, fields) << "\n";
        }
    }

    // Service definitions → publisher and subscriber classes (skip in schema_only mode).
    if (!schema_only) {
        for (int si = 0; si < file->service_count(); ++si) {
            const auto* svc = file->service(si);
            for (int mi = 0; mi < svc->method_count(); ++mi) {
                const auto* method = svc->method(mi);
                std::string reason;
                if (!ValidateServiceMethod(method, generated_msgs, &reason)) {
                    o << "// Skipped: " << svc->name() << "." << method->name() << " — " << reason
                      << "\n";
                    continue;
                }
                o << GeneratePublisherClass(method, file->package()) << "\n"
                  << GenerateSubscriberClass(method, file->package()) << "\n";
            }
        }
    }

    if (!ns.empty()) o << "}  // namespace " << ns << "\n";
    o << "}  // namespace fletcher_gen\n";

    return o.str();
}

// -----------------------------------------------------------------------
// TypeScript file generation
// -----------------------------------------------------------------------

std::string TsOutputFilename(const std::string& proto_name) {
    return StripProtoSuffix(proto_name) + ".fletcher.ts";
}

// Convert a package "foo.bar" to "foo/bar" for topic paths.
std::string DotToSlash(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '.')
            out += '/';
        else
            out += c;
    }
    return out;
}

// Walk flatten chain to the innermost leaf struct.
const google::protobuf::Descriptor* FlattenLeafStruct(const google::protobuf::Descriptor* msg) {
    while (IsFlattenedWrapper(msg) &&
           msg->field(0)->type() == google::protobuf::FieldDescriptor::TYPE_MESSAGE)
        msg = msg->field(0)->message_type();
    return msg;
}

// Const name for a message's TypedSchema binding: Outer.Inner →
// "Outer_Inner". No "Schema" suffix — the const is the runtime
// representation of the message itself, bundling both the schema
// fields and the phantom TS type, so call sites read like
//   client.publish(topic, Telemetry, data)
// not
//   client.publish(topic, TelemetrySchema, data)
std::string TsSchemaConstName(const google::protobuf::Descriptor* msg) {
    std::string name = msg->name();
    const auto* parent = msg->containing_type();
    while (parent) {
        name = parent->name() + "_" + name;
        parent = parent->containing_type();
    }
    return name;
}

// TypeScript type for a FieldInfo, handling scalars, composites, and well-known types.
std::string TsFieldType(const FieldInfo& fi, const google::protobuf::FieldDescriptor* fd) {
    switch (fi.mapping.kind) {
        case FieldKind::SCALAR: {
            // Check well-known types first.
            if (fd->type() == google::protobuf::FieldDescriptor::TYPE_MESSAGE) {
                const auto& fqn = fd->message_type()->full_name();
                if (fqn == "google.protobuf.Timestamp" || fqn == "google.protobuf.Duration")
                    return "bigint";
                // Wrapper types: the scalar info tells us the underlying type.
                return TsScalarType(fd->message_type()->field(0)->type());
            }
            return TsScalarType(fd->type());
        }

        case FieldKind::REPEATED_SCALAR:
            return TsScalarType(fd->type()) + "[]";

        case FieldKind::STRUCT:
            return TsInterfaceName(fd->message_type());

        case FieldKind::REPEATED_STRUCT:
            return TsInterfaceName(fd->message_type()) + "[]";

        case FieldKind::NESTED_LIST: {
            const auto* coord = FlattenLeafStruct(fd->message_type());
            std::string ts = TsInterfaceName(coord);
            for (int d = 0; d < fi.mapping.list_depth; ++d) ts += "[]";
            return ts;
        }

        case FieldKind::MAP: {
            std::string key_type = TsScalarType(fd->message_type()->field(0)->type());
            const auto* val_fd = fd->message_type()->field(1);
            std::string val_type;
            if (fi.mapping.map_value_is_message) {
                val_type = TsInterfaceName(val_fd->message_type());
            } else {
                val_type = TsScalarType(val_fd->type());
            }
            return "Map<" + key_type + ", " + val_type + ">";
        }
    }
    return "unknown";
}

// WireTypeId name for a FieldInfo.
std::string TsWireTypeId(const FieldInfo& fi, const google::protobuf::FieldDescriptor* fd) {
    switch (fi.mapping.kind) {
        case FieldKind::SCALAR: {
            if (fd->type() == google::protobuf::FieldDescriptor::TYPE_MESSAGE) {
                const auto& fqn = fd->message_type()->full_name();
                if (fqn == "google.protobuf.Timestamp") return "WireTypeId.TIMESTAMP_NANO";
                if (fqn == "google.protobuf.Duration") return "WireTypeId.DURATION_NANO";
                // Wrapper types: use the underlying scalar's wire type.
                return WireTypeIdName(fd->message_type()->field(0)->type());
            }
            return WireTypeIdName(fd->type());
        }
        case FieldKind::REPEATED_SCALAR:
            return "WireTypeId.LIST";
        case FieldKind::STRUCT:
            return "WireTypeId.STRUCT";
        case FieldKind::REPEATED_STRUCT:
            return "WireTypeId.LIST";
        case FieldKind::NESTED_LIST:
            return "WireTypeId.LIST";
        case FieldKind::MAP:
            return "WireTypeId.MAP";
    }
    return "WireTypeId.UNKNOWN";
}

// Emit the FieldDescriptor literal for a field, with nested descriptors
// for composite types.
void EmitTsFieldDescriptor(std::ostringstream& o, const FieldInfo& fi,
                           const google::protobuf::FieldDescriptor* fd, const std::string& indent) {
    o << indent << "{ " << "name: '" << fi.name << "', " << "fieldNumber: " << fi.field_number
      << ", " << "wireType: " << TsWireTypeId(fi, fd) << ", "
      << "nullable: " << (fi.mapping.nullable ? "true" : "false");

    // Nested descriptors for composite types.
    if (fi.mapping.kind == FieldKind::REPEATED_SCALAR) {
        o << ", element: { name: '', fieldNumber: 0, wireType: " << WireTypeIdName(fd->type())
          << ", nullable: false }";
    } else if (fi.mapping.kind == FieldKind::STRUCT) {
        o << ", fields: " << TsSchemaConstName(fd->message_type()) << ".fields";
    } else if (fi.mapping.kind == FieldKind::REPEATED_STRUCT) {
        o << ", element: { name: '', fieldNumber: 0, wireType: WireTypeId.STRUCT"
          << ", nullable: false, fields: " << TsSchemaConstName(fd->message_type()) << ".fields }";
    } else if (fi.mapping.kind == FieldKind::NESTED_LIST) {
        // Build nested element descriptors from inside out.
        const auto* coord = FlattenLeafStruct(fd->message_type());
        std::string inner =
            "{ name: '', fieldNumber: 0, wireType: WireTypeId.STRUCT"
            ", nullable: false, fields: " +
            TsSchemaConstName(coord) + ".fields }";

        for (int d = 1; d < fi.mapping.list_depth; ++d)
            inner =
                "{ name: '', fieldNumber: 0, wireType: WireTypeId.LIST"
                ", nullable: false, element: " +
                inner + " }";
        o << ", element: " << inner;
    } else if (fi.mapping.kind == FieldKind::MAP) {
        const auto* key_fd = fd->message_type()->field(0);
        const auto* val_fd = fd->message_type()->field(1);
        o << ", mapKey: { name: '', fieldNumber: 0, wireType: " << WireTypeIdName(key_fd->type())
          << ", nullable: false }";
        o << ", mapValue: { name: '', fieldNumber: 0, wireType: ";
        if (fi.mapping.map_value_is_message) {
            o << "WireTypeId.STRUCT, nullable: false, fields: "
              << TsSchemaConstName(val_fd->message_type()) << ".fields";
        } else {
            o << WireTypeIdName(val_fd->type()) << ", nullable: false";
        }
        o << " }";
    }

    o << " },\n";
}

// Generate TypeScript interface + SchemaDescriptor for a single message.
std::string GenerateTsMessage(const google::protobuf::Descriptor* msg,
                              const google::protobuf::FileDescriptor* file) {
    std::string skipped;
    auto fields = GatherFields(msg, &skipped);
    if (fields.empty()) return "";

    const std::string iface = TsInterfaceName(msg);
    const std::string schema_name = TsSchemaConstName(msg);

    std::ostringstream o;

    // Interface
    o << "export interface " << iface << " {\n";
    for (size_t i = 0; i < fields.size(); ++i) {
        const auto* fd = fields[i].descriptor;
        std::string ts_type = TsFieldType(fields[i], fd);
        if (fields[i].mapping.nullable) ts_type += " | null";
        o << "  " << fields[i].name << ": " << ts_type << ";\n";
    }
    o << "}\n\n";

    // TypedSchema<IFoo> — runtime SchemaDescriptor + phantom TS type
    // so call sites can write `client.publish(topic, Foo, data)` and
    // have `data`'s type inferred from the proto-generated interface.
    o << "export const " << schema_name << ": TypedSchema<" << iface << "> = {\n"
      << "  fields: [\n";
    for (size_t i = 0; i < fields.size(); ++i) {
        EmitTsFieldDescriptor(o, fields[i], fields[i].descriptor, "    ");
    }
    o << "  ],\n"
      << "  protoPackage: '" << msg->file()->package() << "',\n"
      << "  protoMessage: '" << msg->name() << "',\n"
      << "};\n\n";

    return o.str();
}

// Generate the full .fletcher.ts file for a .proto file.
std::string GenerateTypeScriptFile(const google::protobuf::FileDescriptor* file) {
    std::ostringstream o;

    o << "// Generated by fletcher-protoc. DO NOT EDIT.\n"
      << "// Source: " << file->name() << "\n\n"
      << "import type { TypedSchema } from '@eiva/fletcher-gateway-client';\n"
      << "import { WireTypeId } from '@eiva/fletcher-gateway-client';\n\n";

    // Collect cross-file TypeScript imports.
    std::set<std::string> ts_imports;  // set of .proto filenames we depend on
    auto messages = OrderedMessages(file);
    for (const auto* msg : messages) {
        if (IsRecursive(msg)) continue;
        for (int fi = 0; fi < msg->field_count(); ++fi) {
            const auto* fd = msg->field(fi);
            if (auto m = MapField(fd)) {
                if (m->kind == FieldKind::STRUCT || m->kind == FieldKind::REPEATED_STRUCT) {
                    if (fd->message_type()->file() != file)
                        ts_imports.insert(fd->message_type()->file()->name());
                }
                if (m->kind == FieldKind::NESTED_LIST) {
                    const auto* coord = FlattenLeafStruct(fd->message_type());
                    if (coord->file() != file) ts_imports.insert(coord->file()->name());
                }
                if (m->kind == FieldKind::MAP && m->map_value_is_message) {
                    const auto* val_fd = fd->message_type()->field(1);
                    if (val_fd->message_type()->file() != file)
                        ts_imports.insert(val_fd->message_type()->file()->name());
                }
            }
        }
    }
    for (const auto& proto_file : ts_imports) {
        o << "import { " << "/* cross-file types */ " << "} from './"
          << StripProtoSuffix(proto_file) << ".fletcher.js';\n";
    }
    if (!ts_imports.empty()) o << "\n";

    // Emit messages in dependency order.
    for (const auto* msg : messages) {
        if (IsRecursive(msg)) {
            o << "// Skipped: " << msg->name() << " is recursive and cannot be represented.\n\n";
            continue;
        }
        o << GenerateTsMessage(msg, file);
    }

    // Topic constants for service methods.
    std::set<const google::protobuf::Descriptor*> generated_msgs(messages.begin(), messages.end());

    for (int si = 0; si < file->service_count(); ++si) {
        const auto* svc = file->service(si);
        for (int mi = 0; mi < svc->method_count(); ++mi) {
            const auto* method = svc->method(mi);
            std::string reason;
            if (!ValidateServiceMethod(method, generated_msgs, &reason)) {
                o << "// Skipped: " << svc->name() << "." << method->name() << " — " << reason
                  << "\n";
                continue;
            }

            const std::string pkg = file->package();
            std::string topic_path;
            if (!pkg.empty()) topic_path += DotToSlash(pkg) + "/";
            topic_path += svc->name() + "/" + method->name();

            o << "export const " << svc->name() << "_" << method->name() << "Topic = '"
              << topic_path << "';\n\n";
        }
    }

    return o.str();
}

// -----------------------------------------------------------------------
// ToArrowRow() free function generation
//
// Generates an inline free function per message that converts a nanoarrow
// class instance to fletcher::ArrowRow (vector<shared_ptr<arrow::Scalar>>)
// using only the public getter API.  Found via ADL.
// -----------------------------------------------------------------------

std::string GenerateToArrowRow(const std::string& cls, const std::vector<FieldInfo>& fields) {
    std::ostringstream o;
    o << "/// Converts a nanoarrow-only " << cls << " instance to an ArrowRow\n"
      << "/// (vector of Arrow scalars).  Uses only public getters, so the\n"
      << "/// nanoarrow class remains free of any Arrow C++ dependency.\n"
      << "/// Found via ADL — call as ToArrowRow(msg) without qualification.\n"
      << "inline fletcher::ArrowRow ToArrowRow(const " << cls << "& msg) {\n"
      << "    fletcher::ArrowRow row;\n"
      << "    row.reserve(" << fields.size() << ");\n";

    for (const auto& fi : fields) {
        const std::string getter = "msg." + fi.name + "()";

        switch (fi.mapping.kind) {
            case FieldKind::SCALAR: {
                const auto& sc = fi.mapping.scalar;
                if (fi.mapping.nullable) {
                    std::string val_expr;
                    if (sc.value_is_buffer)
                        val_expr =
                            ReplaceAll(sc.scalar_ctor, "{val}", "std::string(*" + getter + ")");
                    else
                        val_expr = ReplaceAll(sc.scalar_ctor, "{val}", "*" + getter);
                    o << "    row.push_back(" << getter << ".has_value()\n"
                      << "        ? std::shared_ptr<arrow::Scalar>(" << val_expr << ")\n"
                      << "        : arrow::MakeNullScalar(" << sc.arrow_type_expr << "));\n";
                } else {
                    std::string val_expr;
                    if (sc.value_is_buffer)
                        val_expr =
                            ReplaceAll(sc.scalar_ctor, "{val}", "std::string(" + getter + ")");
                    else
                        val_expr = ReplaceAll(sc.scalar_ctor, "{val}", getter);
                    o << "    row.push_back(" << val_expr << ");\n";
                }
                break;
            }

            case FieldKind::STRUCT: {
                const auto& nc = fi.mapping.nested_class;
                o << "    {\n"
                  << "        auto type = arrow::struct_(\n"
                  << "            detail::ImportSchema(" << nc << "Schema())->fields());\n";
                if (fi.mapping.nullable) {
                    o << "        if (" << getter << " != nullptr)\n"
                      << "            row.push_back(std::make_shared"
                         "<arrow::StructScalar>(\n"
                      << "                ToArrowRow(*" << getter << "), type));\n"
                      << "        else\n"
                      << "            row.push_back(arrow::MakeNullScalar"
                         "(type));\n";
                } else {
                    o << "        row.push_back(std::make_shared"
                         "<arrow::StructScalar>(\n"
                      << "            ToArrowRow(" << getter << "), type));\n";
                }
                o << "    }\n";
                break;
            }

            case FieldKind::REPEATED_SCALAR: {
                const auto& el = fi.mapping.element;
                o << "    {\n"
                  << "        " << el.builder_type << " builder;\n"
                  << "        for (const auto& v : " << getter << ")\n"
                  << "            (void)builder.Append(v);\n"
                  << "        row.push_back(std::make_shared<arrow::ListScalar>(\n"
                  << "            *builder.Finish(),\n"
                  << "            arrow::list(arrow::field(\"item\", " << el.arrow_type_expr
                  << ", true))));\n"
                  << "    }\n";
                break;
            }

            case FieldKind::REPEATED_STRUCT: {
                const auto& nc = fi.mapping.nested_class;
                o << "    {\n"
                  << "        auto type = arrow::struct_(\n"
                  << "            detail::ImportSchema(" << nc << "Schema())->fields());\n"
                  << "        auto builder = arrow::MakeBuilder(type)"
                     ".ValueOrDie();\n"
                  << "        for (const auto& v : " << getter << ") {\n"
                  << "            auto s = std::make_shared"
                     "<arrow::StructScalar>(\n"
                  << "                ToArrowRow(v), type);\n"
                  << "            (void)builder->AppendScalar(*s);\n"
                  << "        }\n"
                  << "        row.push_back(std::make_shared<arrow::ListScalar>(\n"
                  << "            *builder->Finish(),\n"
                  << "            arrow::list(arrow::field(\"item\", type,"
                     " true))));\n"
                  << "    }\n";
                break;
            }

            case FieldKind::NESTED_LIST: {
                const auto& nc = fi.mapping.nested_class;
                o << "    {\n"
                  << "        auto coord_type = arrow::struct_(\n"
                  << "            detail::ImportSchema(" << nc << "Schema())->fields());\n";

                // Reference to the data.
                std::string data_ref = fi.mapping.nullable ? ("(*" + getter + ")") : getter;

                if (fi.mapping.list_depth == 2) {
                    o << "        auto inner_list_type = arrow::list(\n"
                      << "            arrow::field(\"item\", coord_type,"
                         " true));\n";
                    if (fi.mapping.nullable) {
                        o << "        if (" << getter << " == nullptr) {\n"
                          << "            row.push_back(arrow::MakeNullScalar(\n"
                          << "                arrow::list(arrow::field(\"item\","
                             " inner_list_type, true))));\n"
                          << "        } else {\n";
                    }
                    o << "        auto outer_builder = arrow::MakeBuilder"
                         "(inner_list_type).ValueOrDie();\n"
                      << "        for (const auto& ring : " << data_ref << ") {\n"
                      << "            auto inner_builder = arrow::MakeBuilder"
                         "(coord_type).ValueOrDie();\n"
                      << "            for (const auto& v : ring) {\n"
                      << "                auto s = std::make_shared"
                         "<arrow::StructScalar>(\n"
                      << "                    ToArrowRow(v), coord_type);\n"
                      << "                (void)inner_builder->AppendScalar"
                         "(*s);\n"
                      << "            }\n"
                      << "            (void)outer_builder->AppendScalar(\n"
                      << "                arrow::ListScalar(*inner_builder->"
                         "Finish(), inner_list_type));\n"
                      << "        }\n"
                      << "        row.push_back(std::make_shared"
                         "<arrow::ListScalar>(\n"
                      << "            *outer_builder->Finish()));\n";
                } else if (fi.mapping.list_depth == 3) {
                    o << "        auto ring_list_type = arrow::list(\n"
                      << "            arrow::field(\"item\", coord_type,"
                         " true));\n"
                      << "        auto poly_list_type = arrow::list(\n"
                      << "            arrow::field(\"item\", ring_list_type,"
                         " true));\n";
                    if (fi.mapping.nullable) {
                        o << "        if (" << getter << " == nullptr) {\n"
                          << "            row.push_back(arrow::MakeNullScalar(\n"
                          << "                arrow::list(arrow::field(\"item\","
                             " poly_list_type, true))));\n"
                          << "        } else {\n";
                    }
                    o << "        auto outer_builder = arrow::MakeBuilder"
                         "(poly_list_type).ValueOrDie();\n"
                      << "        for (const auto& poly : " << data_ref << ") {\n"
                      << "            auto mid_builder = arrow::MakeBuilder"
                         "(ring_list_type).ValueOrDie();\n"
                      << "            for (const auto& ring : poly) {\n"
                      << "                auto inner_builder = arrow::MakeBuilder"
                         "(coord_type).ValueOrDie();\n"
                      << "                for (const auto& v : ring) {\n"
                      << "                    auto s = std::make_shared"
                         "<arrow::StructScalar>(\n"
                      << "                        ToArrowRow(v), coord_type);\n"
                      << "                    (void)inner_builder->AppendScalar"
                         "(*s);\n"
                      << "                }\n"
                      << "                (void)mid_builder->AppendScalar(\n"
                      << "                    arrow::ListScalar("
                         "*inner_builder->Finish(), ring_list_type));\n"
                      << "            }\n"
                      << "            (void)outer_builder->AppendScalar(\n"
                      << "                arrow::ListScalar(*mid_builder->"
                         "Finish(), poly_list_type));\n"
                      << "        }\n"
                      << "        row.push_back(std::make_shared"
                         "<arrow::ListScalar>(\n"
                      << "            *outer_builder->Finish()));\n";
                }

                if (fi.mapping.nullable) {
                    o << "        }\n";  // close else
                }
                o << "    }\n";
                break;
            }

            case FieldKind::MAP: {
                const auto& mk = fi.mapping.map_key;
                o << "    {\n"
                  << "        " << mk.builder_type << " key_builder;\n";

                if (fi.mapping.map_value_is_message) {
                    const auto& mvc = fi.mapping.map_value_class;
                    o << "        auto val_type = arrow::struct_(\n"
                      << "            detail::ImportSchema(" << mvc << "Schema())->fields());\n"
                      << "        auto val_builder = arrow::MakeBuilder"
                         "(val_type).ValueOrDie();\n"
                      << "        for (const auto& [k, v] : " << getter << ") {\n"
                      << "            (void)key_builder.Append(k);\n"
                      << "            auto s = std::make_shared"
                         "<arrow::StructScalar>(\n"
                      << "                ToArrowRow(v), val_type);\n"
                      << "            (void)val_builder->AppendScalar(*s);\n"
                      << "        }\n"
                      << "        auto keys = *key_builder.Finish();\n"
                      << "        auto vals = *val_builder->Finish();\n";
                } else {
                    const auto& mv = fi.mapping.map_value;
                    o << "        " << mv.builder_type << " val_builder;\n"
                      << "        for (const auto& [k, v] : " << getter << ") {\n"
                      << "            (void)key_builder.Append(k);\n"
                      << "            (void)val_builder.Append(v);\n"
                      << "        }\n"
                      << "        auto keys = *key_builder.Finish();\n"
                      << "        auto vals = *val_builder.Finish();\n";
                }

                if (fi.mapping.map_value_is_message) {
                    o << "        auto val_field = arrow::field(\"value\","
                         " val_type, true);\n";
                } else {
                    o << "        auto val_field = arrow::field(\"value\", "
                      << fi.mapping.map_value.arrow_type_expr << ", true);\n";
                }
                o << "        auto kv = *arrow::StructArray::Make(\n"
                  << "            {keys, vals},\n"
                  << "            {arrow::field(\"key\", " << mk.arrow_type_expr
                  << ", false), val_field});\n"
                  << "        row.push_back(std::make_shared"
                     "<arrow::MapScalar>(kv,\n"
                  << "            arrow::map(" << mk.arrow_type_expr << ", val_field)));\n"
                  << "    }\n";
                break;
            }
        }
    }

    o << "    return row;\n"
      << "}\n";
    return o.str();
}

// -----------------------------------------------------------------------
// Arrow file generation (.fletcher.arrow.pb.h)
//
// Generates an Arrow C++ dependent header for each proto file.
// Contains view classes (typed wrappers around ArrowRow / RecordBatch / Table)
// and ToArrowRow() free functions that convert nanoarrow classes to ArrowRow.
// -----------------------------------------------------------------------

std::string ViewOutputFilename(const std::string& proto_name) {
    return StripProtoSuffix(proto_name) + ".fletcher.arrow.pb.h";
}

std::string GenerateViewFile(const google::protobuf::FileDescriptor* file) {
    auto messages = OrderedMessages(file);

    // Check if any message can produce a view class.
    bool has_views = false;
    for (const auto* msg : messages) {
        if (IsRecursive(msg) || IsFlattenedWrapper(msg)) continue;
        has_views = true;
        break;
    }
    if (!has_views) return {};

    std::ostringstream o;

    o << "// Generated by fletcher-protoc. DO NOT EDIT.\n"
      << "// Source: " << file->name() << "\n"
      << "// Arrow C++ view classes and ToArrowRow() converters — server-side only.\n"
      << "#pragma once\n\n"
      << "#include <arrow/api.h>\n"
      << "#include <arrow/c/bridge.h>\n"
      << "#include <fletcher/arrow_bridge/arrow_row_view.hpp>\n"
      << "#include <fletcher/arrow_bridge/codec.hpp>\n\n"
      << "#include \"" << OutputFilename(file->name()) << "\"\n";

    // Cross-file view includes (for nested view types from other proto files).
    const auto cross_includes = CollectCrossFileIncludes(file);
    if (!cross_includes.empty()) {
        o << "\n";
        for (const auto& h : cross_includes) {
            // Replace .fletcher.pb.h → .fletcher.arrow.pb.h
            std::string vh = h;
            const std::string suffix = ".fletcher.pb.h";
            if (vh.size() > suffix.size() && vh.substr(vh.size() - suffix.size()) == suffix) {
                vh = vh.substr(0, vh.size() - suffix.size()) + ".fletcher.arrow.pb.h";
            }
            o << "#include \"" << vh << "\"\n";
        }
    }

    o << "\n"
      << "#include <cstdint>\n"
      << "#include <memory>\n"
      << "#include <optional>\n"
      << "#include <string>\n"
      << "#include <string_view>\n"
      << "#include <vector>\n\n";

    o << "namespace fletcher_gen {\n";
    const std::string ns = DotToColons(file->package());
    if (!ns.empty()) o << "namespace " << ns << " {\n";
    o << "\n";

    // Helper: convert OwnedSchema → shared_ptr<arrow::Schema> via C Data Interface.
    // Guard against redefinition when multiple view headers from the same package
    // are included in a single translation unit.
    {
        std::string guard = "FLETCHER_DETAIL_IMPORT_SCHEMA_";
        for (char c : file->package()) guard += (c == '.' ? '_' : std::toupper(c));
        guard += "_DEFINED";
        o << "#ifndef " << guard << "\n"
          << "#define " << guard << "\n"
          << "namespace detail {\n"
          << "/// Converts a nanoarrow OwnedSchema to a shared_ptr<arrow::Schema>\n"
          << "/// via the Arrow C Data Interface.  Used internally by view classes\n"
          << "/// and ToArrowRow() to obtain Arrow type metadata.\n"
          << "inline std::shared_ptr<arrow::Schema> ImportSchema(fletcher::OwnedSchema nano) {\n"
          << "    auto result = arrow::ImportSchema(nano.get());\n"
          << "    return result.ok() ? *result : nullptr;\n"
          << "}\n"
          << "}  // namespace detail\n"
          << "#endif\n\n";
    }

    for (const auto* msg : messages) {
        if (IsRecursive(msg) || IsFlattenedWrapper(msg)) continue;

        std::string skipped;
        auto fields = GatherFields(msg, &skipped);
        const std::string cls = ClassName(msg);
        const std::string view_cls = cls + "View";

        o << GenerateViewClass(view_cls, fields) << "\n";
        o << GenerateToArrowRow(cls, fields) << "\n";
    }

    if (!ns.empty()) o << "}  // namespace " << ns << "\n";
    o << "}  // namespace fletcher_gen\n";

    return o.str();
}

}  // namespace

// -----------------------------------------------------------------------
// CodeGenerator interface
// -----------------------------------------------------------------------

bool ArrowRowGenerator::Generate(const google::protobuf::FileDescriptor* file,
                                 const std::string& parameter,
                                 google::protobuf::compiler::GeneratorContext* context,
                                 std::string* error) const {
    // Parse comma-separated options from --fletcher_opt=...
    bool schema_only = false;
    bool emit_ts = false;
    {
        std::istringstream ss(parameter);
        std::string token;
        while (std::getline(ss, token, ',')) {
            if (token == "schema_only")
                schema_only = true;
            else if (token == "ts")
                emit_ts = true;
        }
    }

    // Always emit the C++ header (edge-compatible, nanoarrow only).
    {
        const std::string content = GenerateFile(file, schema_only);
        const std::string out_name = OutputFilename(file->name());
        std::unique_ptr<google::protobuf::io::ZeroCopyOutputStream> stream(context->Open(out_name));
        if (!WriteToStream(stream.get(), content, error)) return false;
    }

    // Emit the view header (Arrow C++ dependent, server-side).
    if (!schema_only) {
        const std::string view_content = GenerateViewFile(file);
        if (!view_content.empty()) {
            const std::string view_name = ViewOutputFilename(file->name());
            std::unique_ptr<google::protobuf::io::ZeroCopyOutputStream> stream(
                context->Open(view_name));
            if (!WriteToStream(stream.get(), view_content, error)) return false;
        }
    }

    // Optionally emit the TypeScript file.
    if (emit_ts) {
        const std::string ts_content = GenerateTypeScriptFile(file);
        const std::string ts_out_name = TsOutputFilename(file->name());
        std::unique_ptr<google::protobuf::io::ZeroCopyOutputStream> stream(
            context->Open(ts_out_name));
        if (!WriteToStream(stream.get(), ts_content, error)) return false;
    }

    return true;
}

}  // namespace fletcher
