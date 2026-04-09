#include "generator.hpp"
#include "type_mapper.hpp"

#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/io/zero_copy_stream.h>

#include <algorithm>
#include <cstring>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fletcher_plugin {

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
        if (c == '.') out += "::";
        else          out += c;
    }
    return out;
}

std::string StripProtoSuffix(const std::string& proto_name) {
    constexpr std::string_view kSuffix = ".proto";
    std::string base = proto_name;
    if (base.size() > kSuffix.size()
        && base.substr(base.size() - kSuffix.size()) == kSuffix)
        base.resize(base.size() - kSuffix.size());
    return base;
}

std::string OutputFilename(const std::string& proto_name) {
    return StripProtoSuffix(proto_name) + ".fletcher.pb.h";
}

bool WriteToStream(google::protobuf::io::ZeroCopyOutputStream* out,
                   const std::string& s, std::string* error) {
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
        if (static_cast<size_t>(size) > n)
            out->BackUp(static_cast<int>(size - n));
        data      += n;
        remaining -= n;
    }
    return true;
}

// -----------------------------------------------------------------------
// Cross-file include collection
// -----------------------------------------------------------------------

// Scan all supported field mappings in a message (and its nested types) and
// accumulate the include paths of any cross-file generated headers needed.
void CollectCrossFileIncludesFromMessage(
    const google::protobuf::Descriptor* msg,
    std::set<std::string>& headers)
{
    for (int fi = 0; fi < msg->field_count(); ++fi) {
        const auto* fd = msg->field(fi);
        if (auto m = MapField(fd)) {
            if (!m->nested_header.empty())
                headers.insert(m->nested_header);
            if (!m->map_value_header.empty())
                headers.insert(m->map_value_header);
        }
    }
    for (int ni = 0; ni < msg->nested_type_count(); ++ni)
        CollectCrossFileIncludesFromMessage(msg->nested_type(ni), headers);
}

std::set<std::string> CollectCrossFileIncludes(
    const google::protobuf::FileDescriptor* file)
{
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
    if (emitted.count(msg))
        return;
    // Skip synthetic map-entry messages.
    if (msg->options().map_entry())
        return;
    // Only generate classes for messages in this file.
    if (msg->file() != file)
        return;
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
        if (f->type() != google::protobuf::FieldDescriptor::TYPE_MESSAGE)
            continue;
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

std::vector<const google::protobuf::Descriptor*>
OrderedMessages(const google::protobuf::FileDescriptor* file) {
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
    std::string  name;
    FieldMapping mapping;
    int          field_number = 0;  // proto field number — stable across renames
    std::string  field_id;          // string form of field_number for metadata
};

// -----------------------------------------------------------------------
// Arrow type expression for the schema — constructed from the FieldMapping
// -----------------------------------------------------------------------

std::string ArrowTypeExpr(const FieldInfo& fi) {
    switch (fi.mapping.kind) {
        case FieldKind::SCALAR:
            return fi.mapping.scalar.arrow_type_expr;

        case FieldKind::REPEATED_SCALAR:
            return "arrow::list(arrow::field(\"item\", "
                 + fi.mapping.element.arrow_type_expr + ", false))";

        case FieldKind::STRUCT:
            return "arrow::struct_(" + fi.mapping.nested_class
                 + "Schema()->fields())";

        case FieldKind::REPEATED_STRUCT:
            return "arrow::list(arrow::field(\"item\", arrow::struct_("
                 + fi.mapping.nested_class
                 + "Schema()->fields()), false))";

        case FieldKind::MAP: {
            std::string val_type = fi.mapping.map_value_is_message
                ? "arrow::struct_(" + fi.mapping.map_value_class
                  + "Schema()->fields())"
                : fi.mapping.map_value.arrow_type_expr;
            return "arrow::map(" + fi.mapping.map_key.arrow_type_expr
                 + ", arrow::field(\"value\", " + val_type + ", false))";
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
            return "std::optional<" + fi.mapping.scalar.storage_type + "> "
                 + fi.name + "_";

        case FieldKind::REPEATED_SCALAR:
            return "std::vector<" + fi.mapping.element.storage_type + "> "
                 + fi.name + "_";

        case FieldKind::STRUCT:
            return "std::optional<" + fi.mapping.nested_class + "> "
                 + fi.name + "_";

        case FieldKind::REPEATED_STRUCT:
            return "std::vector<" + fi.mapping.nested_class + "> "
                 + fi.name + "_";

        case FieldKind::MAP: {
            std::string val_type = fi.mapping.map_value_is_message
                ? fi.mapping.map_value_class
                : fi.mapping.map_value.storage_type;
            return "std::vector<std::pair<" + fi.mapping.map_key.storage_type
                 + ", " + val_type + ">> " + fi.name + "_";
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
                bool coerce = (fi.mapping.scalar.param_type
                            != fi.mapping.scalar.storage_type);
                o << "    " << cls << "& set_" << fi.name
                  << "(" << fi.mapping.scalar.param_type << " v) {\n";
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
                o << "    " << cls << "& set_" << fi.name
                  << "(std::vector<" << fi.mapping.element.storage_type << "> v) {\n"
                  << "        " << fi.name << "_ = std::move(v);\n"
                  << "        return *this;\n    }\n";
                break;

            case FieldKind::STRUCT:
                o << "    " << cls << "& set_" << fi.name
                  << "(" << fi.mapping.nested_class << " v) {\n"
                  << "        " << fi.name << "_ = std::move(v);\n"
                  << "        return *this;\n    }\n";
                if (fi.mapping.nullable) {
                    o << "    " << cls << "& clear_" << fi.name << "() {\n"
                      << "        " << fi.name << "_.reset();\n"
                      << "        return *this;\n    }\n";
                }
                break;

            case FieldKind::REPEATED_STRUCT:
                o << "    " << cls << "& set_" << fi.name
                  << "(std::vector<" << fi.mapping.nested_class << "> v) {\n"
                  << "        " << fi.name << "_ = std::move(v);\n"
                  << "        return *this;\n    }\n";
                break;

            case FieldKind::MAP: {
                std::string val_type = fi.mapping.map_value_is_message
                    ? fi.mapping.map_value_class
                    : fi.mapping.map_value.storage_type;
                o << "    " << cls << "& set_" << fi.name
                  << "(std::vector<std::pair<" << fi.mapping.map_key.storage_type
                  << ", " << val_type << ">> v) {\n"
                  << "        " << fi.name << "_ = std::move(v);\n"
                  << "        return *this;\n    }\n";
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
              << "            *builder.Finish());\n"
              << "    }\n";
            break;

        case FieldKind::STRUCT:
            o << "    std::shared_ptr<arrow::Scalar> " << fn << "() const {\n"
              << "        auto type = arrow::struct_("
              << fi.mapping.nested_class << "Schema()->fields());\n";
            if (fi.mapping.nullable) {
                o << "        if (!" << fi.name << "_.has_value())\n"
                  << "            return arrow::MakeNullScalar(type);\n"
                  << "        return std::make_shared<arrow::StructScalar>(\n"
                  << "            " << fi.name << "_->ToScalars(), type);\n";
            } else {
                o << "        auto values = " << fi.name << "_.has_value()\n"
                  << "            ? " << fi.name << "_->ToScalars()\n"
                  << "            : " << fi.mapping.nested_class
                  << "().ToScalars();\n"
                  << "        return std::make_shared<arrow::StructScalar>(\n"
                  << "            std::move(values), type);\n";
            }
            o << "    }\n";
            break;

        case FieldKind::REPEATED_STRUCT:
            o << "    std::shared_ptr<arrow::Scalar> " << fn << "() const {\n"
              << "        auto type = arrow::struct_("
              << fi.mapping.nested_class << "Schema()->fields());\n"
              << "        auto builder = arrow::MakeBuilder(type).ValueOrDie();\n"
              << "        for (const auto& v : " << fi.name << "_) {\n"
              << "            auto s = std::make_shared<arrow::StructScalar>(\n"
              << "                v.ToScalars(), type);\n"
              << "            (void)builder->AppendScalar(*s);\n"
              << "        }\n"
              << "        return std::make_shared<arrow::ListScalar>(\n"
              << "            *builder->Finish());\n"
              << "    }\n";
            break;

        case FieldKind::MAP: {
            o << "    std::shared_ptr<arrow::Scalar> " << fn << "() const {\n"
              << "        " << fi.mapping.map_key.builder_type << " key_builder;\n";

            if (fi.mapping.map_value_is_message) {
                o << "        auto val_type = arrow::struct_("
                  << fi.mapping.map_value_class
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
                o << "        " << fi.mapping.map_value.builder_type
                  << " val_builder;\n"
                  << "        for (const auto& [k, v] : " << fi.name << "_) {\n"
                  << "            (void)key_builder.Append(k);\n"
                  << "            (void)val_builder.Append(v);\n"
                  << "        }\n"
                  << "        auto keys = *key_builder.Finish();\n"
                  << "        auto vals = *val_builder.Finish();\n";
            }

            o << "        auto kv_type = arrow::struct_({\n"
              << "            arrow::field(\"key\", keys->type(), false),\n"
              << "            arrow::field(\"value\", vals->type())});\n"
              << "        auto kv = std::make_shared<arrow::StructArray>(\n"
              << "            kv_type, keys->length(),\n"
              << "            arrow::ArrayVector{keys, vals});\n"
              << "        return std::make_shared<arrow::MapScalar>(kv);\n"
              << "    }\n";
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
        return fi.name + "_.has_value()\n"
             + "                ? std::shared_ptr<arrow::Scalar>(" + ctor + ")\n"
             + "                : arrow::MakeNullScalar("
             + fi.mapping.scalar.arrow_type_expr + ")";
    }

    const std::string val =
        fi.name + "_.value_or(" + fi.mapping.scalar.default_value + ")";
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
            std::string extract = sc.value_is_buffer
                ? "static_cast<const " + sc.scalar_type + "&>(*scalars[" + si + "]).value->ToString()"
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
            std::string extract = el.value_is_buffer
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

        case FieldKind::MAP: {
            std::string key_extract = fi.mapping.map_key.value_is_buffer
                ? "static_cast<const " + fi.mapping.map_key.scalar_type + "&>(*ks).value->ToString()"
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
                std::string val_extract = fi.mapping.map_value.value_is_buffer
                    ? "static_cast<const " + fi.mapping.map_value.scalar_type + "&>(*vs).value->ToString()"
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

std::vector<FieldInfo> GatherFields(const google::protobuf::Descriptor* msg,
                                    std::string* skipped_comment) {
    std::vector<FieldInfo> fields;
    for (int i = 0; i < msg->field_count(); ++i) {
        const auto* fd = msg->field(i);
        if (auto m = MapField(fd)) {
            fields.push_back({fd->name(), std::move(*m), fd->number(),
                              std::to_string(fd->number())});
        } else {
            *skipped_comment += "//   " + fd->name() + ": "
                             + UnsupportedReason(fd) + "\n";
        }
    }
    return fields;
}

// -----------------------------------------------------------------------
// Free schema function for one message
// -----------------------------------------------------------------------

std::string GenerateSchemaFunction(const std::string& cls,
                                   const std::vector<FieldInfo>& fields,
                                   const google::protobuf::Descriptor* msg) {
    std::ostringstream o;
    o << "inline std::shared_ptr<arrow::Schema> " << cls << "Schema() {\n"
      << "    return arrow::schema({\n";
    for (const auto& fi : fields) {
        if (!fi.mapping.warning.empty())
            o << "        // Warning: " << fi.mapping.warning << "\n";
        o << "        arrow::field(\"" << fi.name << "\", "
          << ArrowTypeExpr(fi) << ", "
          << (fi.mapping.nullable ? "true" : "false") << ", "
          << "arrow::key_value_metadata({\"field_number\", \"field_id\"}, {\""
          << fi.field_number << "\", \"" << fi.field_id << "\"})),\n";
    }
    o << "    }, arrow::key_value_metadata(\n"
      << "        {\"proto_package\", \"proto_message\"},\n"
      << "        {\"" << msg->file()->package() << "\", \""
      << msg->name() << "\"}));\n}\n";
    return o.str();
}

// -----------------------------------------------------------------------
// Full class generation for one message
// -----------------------------------------------------------------------

std::string GenerateMessageClass(const std::string& cls,
                                 const std::vector<FieldInfo>& fields) {
    std::ostringstream o;

    // ---- class header ---------------------------------------------------
    o << "class " << cls << " {\n public:\n";

    // Default constructor (explicit because we add the EncodedRow constructor)
    o << "    " << cls << "() = default;\n\n";

    // EncodedRow constructor
    o << "    explicit " << cls << "(const fletcher::EncodedRow& row) {\n"
      << "        SetFromScalars_(Codec().DecodeRow(row));\n"
      << "    }\n\n";

    // Setters
    EmitSetters(o, cls, fields);
    o << "\n";

    // SetFromScalars_ — public so parent classes can call it for struct fields
    o << "    void SetFromScalars_(\n"
      << "        const fletcher::ArrowRow& scalars) {\n";
    for (size_t i = 0; i < fields.size(); ++i)
        EmitFieldExtraction(o, fields[i], i);
    o << "    }\n\n";

    // ToScalars() — public because parent-class struct helpers call it on nested types
    o << "    fletcher::ArrowRow ToScalars() const {\n"
      << "        return {\n";
    for (const auto& fi : fields)
        o << "            " << ScalarEntry(fi) << ",\n";
    o << "        };\n    }\n\n";

    // Encode()
    o << "    fletcher::EncodedRow Encode() const {\n"
      << "        return Codec().EncodeRow(ToScalars());\n"
      << "    }\n\n";

    // ---- private section ------------------------------------------------
    o << " private:\n";

    // Codec singleton
    o << "    static fletcher::RowCodec& Codec() {\n"
      << "        static fletcher::RowCodec kCodec(" << cls << "Schema());\n"
      << "        return kCodec;\n"
      << "    }\n\n";

    // Composite scalar helpers
    bool has_helpers = false;
    for (const auto& fi : fields) {
        if (fi.mapping.kind != FieldKind::SCALAR) {
            EmitScalarHelper(o, fi);
            has_helpers = true;
        }
    }
    if (has_helpers)
        o << "\n";

    // Storage members
    for (const auto& fi : fields)
        o << "    " << StorageDecl(fi) << ";\n";

    o << "};\n";
    return o.str();
}

// -----------------------------------------------------------------------
// Service method validation
// -----------------------------------------------------------------------

bool ValidateServiceMethod(
    const google::protobuf::MethodDescriptor* method,
    const std::set<const google::protobuf::Descriptor*>& generated_msgs,
    std::string* reason)
{
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
        *reason = "input message '" + method->input_type()->name()
                + "' has no generated Arrow mapping in this file";
        return false;
    }
    return true;
}

// -----------------------------------------------------------------------
// Per-class helpers shared by publisher and subscriber generation
// -----------------------------------------------------------------------

void EmitTopicSegments(std::ostringstream& o, const std::string& package,
                       const std::string& svc_name, const std::string& method_name) {
    o << "    static const std::vector<std::string>& TopicSegments() {\n"
      << "        static const std::vector<std::string> kSegments = {";
    if (!package.empty())
        o << "\n            \"" << package << "\",";
    o << "\n            \"" << svc_name << "\","
      << "\n            \"" << method_name << "\"\n"
      << "        };\n"
      << "        return kSegments;\n"
      << "    }\n\n";
}

void EmitSchema(std::ostringstream& o, const std::string& msg_class) {
    o << "    static std::shared_ptr<arrow::Schema> Schema() {\n"
      << "        return " << msg_class << "Schema();\n"
      << "    }\n\n";
}

// -----------------------------------------------------------------------
// Publisher class generation for a single service method
// -----------------------------------------------------------------------

std::string GeneratePublisherClass(const google::protobuf::MethodDescriptor* method,
                                   const std::string& package) {
    const std::string svc_name    = method->service()->name();
    const std::string method_name = method->name();
    const std::string cls         = svc_name + "_" + method_name + "Publisher";
    const std::string msg_class   = ClassName(method->input_type());

    std::ostringstream o;
    o << "class " << cls << " {\n public:\n";

    EmitTopicSegments(o, package, svc_name, method_name);
    EmitSchema(o, msg_class);

    // Constructor
    o << "    explicit " << cls << "(\n"
      << "            std::shared_ptr<fletcher::PubSubProvider> provider)\n"
      << "        : provider_(std::move(provider))\n"
      << "    {\n"
      << "        provider_->CreateTopic(TopicSegments(), Schema());\n"
      << "    }\n\n";

    // Publish (without attachments)
    o << "    void Publish(const " << msg_class << "& row) {\n"
      << "        provider_->Publish(TopicSegments(), row.ToScalars());\n"
      << "    }\n\n";

    // Publish (with attachments)
    o << "    void Publish(const " << msg_class << "& row,\n"
      << "                 fletcher::Attachments attachments) {\n"
      << "        provider_->Publish(TopicSegments(), row.ToScalars(),\n"
      << "                           std::move(attachments));\n"
      << "    }\n\n";

    // Private
    o << " private:\n"
      << "    std::shared_ptr<fletcher::PubSubProvider> provider_;\n"
      << "};\n";

    return o.str();
}

// -----------------------------------------------------------------------
// Subscriber class generation for a single service method
// -----------------------------------------------------------------------

std::string GenerateSubscriberClass(const google::protobuf::MethodDescriptor* method,
                                    const std::string& package) {
    const std::string svc_name    = method->service()->name();
    const std::string method_name = method->name();
    const std::string cls         = svc_name + "_" + method_name + "Subscriber";
    const std::string msg_class   = ClassName(method->input_type());

    std::ostringstream o;
    o << "class " << cls << " {\n public:\n";

    EmitTopicSegments(o, package, svc_name, method_name);
    EmitSchema(o, msg_class);

    // Constructor
    o << "    explicit " << cls << "(\n"
      << "            std::shared_ptr<fletcher::PubSubProvider> provider)\n"
      << "        : provider_(std::move(provider))\n"
      << "    {\n"
      << "        provider_->CreateTopic(TopicSegments(), Schema());\n"
      << "    }\n\n";

    // Subscribe — delivers decoded ArrowRow + Attachments to the caller
    o << "    void Subscribe(\n"
      << "        std::function<void(fletcher::ArrowRow, fletcher::Attachments)> cb)\n"
      << "    {\n"
      << "        provider_->Subscribe(TopicSegments(), std::move(cb));\n"
      << "    }\n\n";

    // Unsubscribe
    o << "    void Unsubscribe() {\n"
      << "        provider_->Unsubscribe(TopicSegments());\n"
      << "    }\n\n";

    // Private
    o << " private:\n"
      << "    std::shared_ptr<fletcher::PubSubProvider> provider_;\n"
      << "};\n";

    return o.str();
}

// -----------------------------------------------------------------------
// File generation
// -----------------------------------------------------------------------

std::string GenerateFile(const google::protobuf::FileDescriptor* file,
                         bool schema_only) {
    std::ostringstream o;

    o << "// Generated by protoc-gen-fletcher. DO NOT EDIT.\n"
      << "// Source: " << file->name() << "\n"
      << "#pragma once\n\n"
      << "#include <arrow/api.h>\n";

    if (!schema_only) {
        o << "#include <row_codec.hpp>\n";
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
        o << "#include <pubsub/pubsub_provider.hpp>\n"
          << "#include <functional>\n";
    }

    // Cross-file generated headers (for referenced messages from other .proto files).
    const auto cross_includes = CollectCrossFileIncludes(file);
    if (!cross_includes.empty()) {
        o << "\n";
        for (const auto& h : cross_includes)
            o << "#include \"" << h << "\"\n";
    }
    o << "\n";

    const std::string ns = DotToColons(file->package());
    if (!ns.empty())
        o << "namespace " << ns << " {\n\n";

    // Emit messages in dependency order.
    auto messages = OrderedMessages(file);
    std::set<const google::protobuf::Descriptor*> generated_msgs;

    for (const auto* msg : messages) {
        if (IsRecursive(msg)) {
            o << "// Skipped: " << msg->name()
              << " is recursive and cannot be represented in Arrow.\n\n";
            continue;
        }

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
        if (!schema_only)
            o << GenerateMessageClass(cls, fields) << "\n";
    }

    // Service definitions → publisher and subscriber classes (skip in schema_only mode).
    if (!schema_only) {
        for (int si = 0; si < file->service_count(); ++si) {
            const auto* svc = file->service(si);
            for (int mi = 0; mi < svc->method_count(); ++mi) {
                const auto* method = svc->method(mi);
                std::string reason;
                if (!ValidateServiceMethod(method, generated_msgs, &reason)) {
                    o << "// Skipped: " << svc->name() << "." << method->name()
                      << " — " << reason << "\n";
                    continue;
                }
                o << GeneratePublisherClass(method, file->package()) << "\n"
                  << GenerateSubscriberClass(method, file->package()) << "\n";
            }
        }
    }

    if (!ns.empty())
        o << "}  // namespace " << ns << "\n";

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
        if (c == '.') out += '/';
        else          out += c;
    }
    return out;
}

// Schema const name for a message: Outer.Inner → "Outer_InnerSchema".
std::string TsSchemaConstName(const google::protobuf::Descriptor* msg) {
    std::string name = msg->name();
    const auto* parent = msg->containing_type();
    while (parent) {
        name = parent->name() + "_" + name;
        parent = parent->containing_type();
    }
    return name + "Schema";
}

// TypeScript type for a FieldInfo, handling scalars, composites, and well-known types.
std::string TsFieldType(const FieldInfo& fi,
                        const google::protobuf::FieldDescriptor* fd) {
    switch (fi.mapping.kind) {
        case FieldKind::SCALAR: {
            // Check well-known types first.
            if (fd->type() == google::protobuf::FieldDescriptor::TYPE_MESSAGE) {
                const auto& fqn = fd->message_type()->full_name();
                if (fqn == "google.protobuf.Timestamp" ||
                    fqn == "google.protobuf.Duration")
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

        case FieldKind::MAP: {
            std::string key_type = TsScalarType(
                fd->message_type()->field(0)->type());
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
std::string TsWireTypeId(const FieldInfo& fi,
                         const google::protobuf::FieldDescriptor* fd) {
    switch (fi.mapping.kind) {
        case FieldKind::SCALAR: {
            if (fd->type() == google::protobuf::FieldDescriptor::TYPE_MESSAGE) {
                const auto& fqn = fd->message_type()->full_name();
                if (fqn == "google.protobuf.Timestamp")
                    return "WireTypeId.TIMESTAMP_NANO";
                if (fqn == "google.protobuf.Duration")
                    return "WireTypeId.DURATION_NANO";
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
        case FieldKind::MAP:
            return "WireTypeId.MAP";
    }
    return "WireTypeId.UNKNOWN";
}

// Emit the FieldDescriptor literal for a field, with nested descriptors
// for composite types.
void EmitTsFieldDescriptor(std::ostringstream& o,
                           const FieldInfo& fi,
                           const google::protobuf::FieldDescriptor* fd,
                           const std::string& indent) {
    o << indent << "{ "
      << "name: '" << fi.name << "', "
      << "fieldNumber: " << fi.field_number << ", "
      << "wireType: " << TsWireTypeId(fi, fd) << ", "
      << "nullable: " << (fi.mapping.nullable ? "true" : "false");

    // Nested descriptors for composite types.
    if (fi.mapping.kind == FieldKind::REPEATED_SCALAR) {
        o << ", element: { name: '', fieldNumber: 0, wireType: "
          << WireTypeIdName(fd->type())
          << ", nullable: false }";
    } else if (fi.mapping.kind == FieldKind::STRUCT) {
        o << ", fields: " << TsSchemaConstName(fd->message_type()) << ".fields";
    } else if (fi.mapping.kind == FieldKind::REPEATED_STRUCT) {
        o << ", element: { name: '', fieldNumber: 0, wireType: WireTypeId.STRUCT"
          << ", nullable: false, fields: "
          << TsSchemaConstName(fd->message_type()) << ".fields }";
    } else if (fi.mapping.kind == FieldKind::MAP) {
        const auto* key_fd = fd->message_type()->field(0);
        const auto* val_fd = fd->message_type()->field(1);
        o << ", mapKey: { name: '', fieldNumber: 0, wireType: "
          << WireTypeIdName(key_fd->type()) << ", nullable: false }";
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
        const auto* fd = msg->field(
            // Find the FieldDescriptor matching our FieldInfo by field number.
            // GatherFields iterates msg->field(i) in order, so we search.
            0);
        // Correct lookup: find fd by field number.
        for (int fi = 0; fi < msg->field_count(); ++fi) {
            if (msg->field(fi)->number() == fields[i].field_number) {
                fd = msg->field(fi);
                break;
            }
        }
        std::string ts_type = TsFieldType(fields[i], fd);
        if (fields[i].mapping.nullable)
            ts_type += " | null";
        o << "  " << fields[i].name << ": " << ts_type << ";\n";
    }
    o << "}\n\n";

    // SchemaDescriptor
    o << "export const " << schema_name << ": SchemaDescriptor = {\n"
      << "  schemaHash: 0n,  // Set at runtime or from C++ FingerprintHash\n"
      << "  fields: [\n";
    for (size_t i = 0; i < fields.size(); ++i) {
        const google::protobuf::FieldDescriptor* fd = nullptr;
        for (int fi = 0; fi < msg->field_count(); ++fi) {
            if (msg->field(fi)->number() == fields[i].field_number) {
                fd = msg->field(fi);
                break;
            }
        }
        EmitTsFieldDescriptor(o, fields[i], fd, "    ");
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

    o << "// Generated by protoc-gen-fletcher. DO NOT EDIT.\n"
      << "// Source: " << file->name() << "\n\n"
      << "import type { SchemaDescriptor } from '@fletcher/web-client';\n"
      << "import { WireTypeId } from '@fletcher/web-client';\n\n";

    // Collect cross-file TypeScript imports.
    std::set<std::string> ts_imports; // set of .proto filenames we depend on
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
                if (m->kind == FieldKind::MAP && m->map_value_is_message) {
                    const auto* val_fd = fd->message_type()->field(1);
                    if (val_fd->message_type()->file() != file)
                        ts_imports.insert(val_fd->message_type()->file()->name());
                }
            }
        }
    }
    for (const auto& proto_file : ts_imports) {
        o << "import { "
          << "/* cross-file types */ "
          << "} from './" << StripProtoSuffix(proto_file) << ".fletcher.js';\n";
    }
    if (!ts_imports.empty()) o << "\n";

    // Emit messages in dependency order.
    for (const auto* msg : messages) {
        if (IsRecursive(msg)) {
            o << "// Skipped: " << msg->name()
              << " is recursive and cannot be represented.\n\n";
            continue;
        }
        o << GenerateTsMessage(msg, file);
    }

    // Topic constants for service methods.
    std::set<const google::protobuf::Descriptor*> generated_msgs(
        messages.begin(), messages.end());

    for (int si = 0; si < file->service_count(); ++si) {
        const auto* svc = file->service(si);
        for (int mi = 0; mi < svc->method_count(); ++mi) {
            const auto* method = svc->method(mi);
            std::string reason;
            if (!ValidateServiceMethod(method, generated_msgs, &reason)) {
                o << "// Skipped: " << svc->name() << "." << method->name()
                  << " — " << reason << "\n";
                continue;
            }

            const std::string pkg = file->package();
            std::string topic_path;
            if (!pkg.empty()) topic_path += DotToSlash(pkg) + "/";
            topic_path += svc->name() + "/" + method->name();

            o << "export const " << svc->name() << "_" << method->name()
              << "Topic = '" << topic_path << "';\n\n";
        }
    }

    return o.str();
}

}  // namespace

// -----------------------------------------------------------------------
// CodeGenerator interface
// -----------------------------------------------------------------------

bool ArrowRowGenerator::Generate(
    const google::protobuf::FileDescriptor* file,
    const std::string& parameter,
    google::protobuf::compiler::GeneratorContext* context,
    std::string* error) const
{
    // Parse comma-separated options from --fletcher_opt=...
    bool schema_only = false;
    bool emit_ts     = false;
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

    // Always emit the C++ header.
    {
        const std::string content  = GenerateFile(file, schema_only);
        const std::string out_name = OutputFilename(file->name());
        std::unique_ptr<google::protobuf::io::ZeroCopyOutputStream> stream(
            context->Open(out_name));
        if (!WriteToStream(stream.get(), content, error))
            return false;
    }

    // Optionally emit the TypeScript file.
    if (emit_ts) {
        const std::string ts_content  = GenerateTypeScriptFile(file);
        const std::string ts_out_name = TsOutputFilename(file->name());
        std::unique_ptr<google::protobuf::io::ZeroCopyOutputStream> stream(
            context->Open(ts_out_name));
        if (!WriteToStream(stream.get(), ts_content, error))
            return false;
    }

    return true;
}

}  // namespace fletcher_plugin
