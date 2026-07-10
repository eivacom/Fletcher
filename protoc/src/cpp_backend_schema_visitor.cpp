// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include "cpp_backend_schema_visitor.hpp"

#include <stdexcept>
#include <variant>

#include "cpp_backend_type_table.hpp"
#include "type_mapper.hpp"

namespace fletcher::cpp_backend {

namespace {

using ir::IrNode;
using ir::NodeKind;

// ---------------------------------------------------------------------------
// Flatten walk — language-neutral mirror of GatherFieldsImpl's field-level
// flatten + dotted field_id, plus the representability filter that reproduces
// ProjectIrToFieldMapping's nullopt set (type_mapper.cpp:150-152 and the
// non-representable list/map shapes). This is a schema-local walk: it reads only
// the proto descriptor and the IR node, never the C++ backend tables.
// ---------------------------------------------------------------------------

// True iff `node` maps to a schema child today (i.e. the IR->FieldMapping
// projection would NOT return nullopt). Kept in exact structural lockstep with
// ProjectIrToFieldMapping so BuildFlattenedFieldList drops precisely the fields
// GatherFieldsImpl drops — no schema drift.
bool IsSchemaRepresentable(const IrNode& node) {
    switch (node.kind) {
        case NodeKind::SCALAR:
        case NodeKind::STRUCT:
            return true;
        case NodeKind::UNSUPPORTED:
        case NodeKind::FIXED_SIZE_LIST:
            return false;
        case NodeKind::LIST: {
            const IrNode& elem = *std::get<ir::ListNode>(node.node).element;
            if (elem.kind == NodeKind::SCALAR || elem.kind == NodeKind::STRUCT) return true;
            if (elem.kind == NodeKind::LIST) {
                // Nested list: representable only when the innermost leaf is a
                // struct (List<List<...<Struct>>>); a scalar leaf is dropped.
                const IrNode* cur = &elem;
                while (cur->kind == NodeKind::LIST)
                    cur = std::get<ir::ListNode>(cur->node).element.get();
                return cur->kind == NodeKind::STRUCT;
            }
            return false;
        }
        case NodeKind::MAP: {
            const auto& mp = std::get<ir::MapNode>(node.node);
            if (mp.key->kind != NodeKind::SCALAR) return false;
            return mp.value->kind == NodeKind::SCALAR || mp.value->kind == NodeKind::STRUCT;
        }
    }
    return false;
}

void BuildFlattenedFieldListImpl(const google::protobuf::Descriptor* msg,
                                 std::vector<SchemaFieldRecord>& out, const std::string& id_prefix) {
    for (int i = 0; i < msg->field_count(); ++i) {
        const auto* fd = msg->field(i);

        const std::string path = id_prefix.empty()
                                     ? std::to_string(fd->number())
                                     : id_prefix + "." + std::to_string(fd->number());

        // Field-level flatten: inline the referenced message's fields, carrying
        // this field's number into the path (matches GatherFieldsImpl).
        if (fd->type() == google::protobuf::FieldDescriptor::TYPE_MESSAGE && !fd->is_repeated() &&
            HasFieldFlatten(fd)) {
            BuildFlattenedFieldListImpl(fd->message_type(), out, path);
            continue;
        }

        IrNode node = ir::BuildFieldIr(fd);
        if (!IsSchemaRepresentable(node)) continue;  // dropped today -> no schema child

        SchemaFieldRecord rec;
        rec.name = fd->name();
        rec.field_number = fd->number();
        rec.field_id = path;
        rec.node = std::make_shared<const IrNode>(std::move(node));
        // GIR-7: this is a non-flattened top-level field (field-level-flatten
        // inlining `continue`s above without recording), so its source field is
        // this fd. The TS backend uses it to recover a singular flatten-wrapper's
        // declared name; the schema paths ignore it.
        rec.source_field = fd;
        out.push_back(std::move(rec));
    }
}

// ---------------------------------------------------------------------------
// Nanoarrow error helper (in-process sink).
// ---------------------------------------------------------------------------

void CheckNa(ArrowErrorCode code, const char* context) {
    if (code != NANOARROW_OK)
        throw std::runtime_error(std::string("BuildMessageSchema: ") + context + " failed");
}

// ---------------------------------------------------------------------------
// C++ source rendering helpers.
// ---------------------------------------------------------------------------

const char* ArrowTypeName(ArrowType type) {
    switch (type) {
        case NANOARROW_TYPE_BOOL:
            return "NANOARROW_TYPE_BOOL";
        case NANOARROW_TYPE_INT32:
            return "NANOARROW_TYPE_INT32";
        case NANOARROW_TYPE_INT64:
            return "NANOARROW_TYPE_INT64";
        case NANOARROW_TYPE_UINT32:
            return "NANOARROW_TYPE_UINT32";
        case NANOARROW_TYPE_UINT64:
            return "NANOARROW_TYPE_UINT64";
        case NANOARROW_TYPE_FLOAT:
            return "NANOARROW_TYPE_FLOAT";
        case NANOARROW_TYPE_DOUBLE:
            return "NANOARROW_TYPE_DOUBLE";
        case NANOARROW_TYPE_STRING:
            return "NANOARROW_TYPE_STRING";
        case NANOARROW_TYPE_BINARY:
            return "NANOARROW_TYPE_BINARY";
        case NANOARROW_TYPE_LIST:
            return "NANOARROW_TYPE_LIST";
        case NANOARROW_TYPE_MAP:
            return "NANOARROW_TYPE_MAP";
        case NANOARROW_TYPE_TIMESTAMP:
            return "NANOARROW_TYPE_TIMESTAMP";
        case NANOARROW_TYPE_DURATION:
            return "NANOARROW_TYPE_DURATION";
        default:
            return "NANOARROW_TYPE_UNINITIALIZED";
    }
}

const char* ArrowTimeUnitName(ArrowTimeUnit unit) {
    switch (unit) {
        case NANOARROW_TIME_UNIT_SECOND:
            return "NANOARROW_TIME_UNIT_SECOND";
        case NANOARROW_TIME_UNIT_MILLI:
            return "NANOARROW_TIME_UNIT_MILLI";
        case NANOARROW_TIME_UNIT_MICRO:
            return "NANOARROW_TIME_UNIT_MICRO";
        case NANOARROW_TIME_UNIT_NANO:
            return "NANOARROW_TIME_UNIT_NANO";
        default:
            return "NANOARROW_TIME_UNIT_NANO";
    }
}

}  // namespace

std::vector<SchemaFieldRecord> BuildFlattenedFieldList(const google::protobuf::Descriptor* msg,
                                                       const std::string& id_prefix) {
    std::vector<SchemaFieldRecord> out;
    BuildFlattenedFieldListImpl(msg, out, id_prefix);
    return out;
}

// ===========================================================================
// CppSchemaSink
// ===========================================================================

CppSchemaSink::CppSchemaSink(std::ostringstream& out, std::string indent,
                             const google::protobuf::FileDescriptor* context_file)
    : out_(out), indent_(std::move(indent)), context_file_(context_file) {}

const std::string& CppSchemaSink::Expr(SchemaRef ref) const {
    return *static_cast<const std::string*>(ref);
}

SchemaRef CppSchemaSink::Intern(std::string expr) {
    exprs_.push_back(std::move(expr));
    return static_cast<SchemaRef>(&exprs_.back());
}

SchemaRef CppSchemaSink::Root() { return Intern("schema.get()"); }

void CppSchemaSink::InitRootStruct(SchemaRef root, int64_t child_count) {
    const std::string& r = Expr(root);
    out_ << indent_ << "ArrowSchemaInit(" << r << ");\n"
         << indent_ << "ArrowSchemaSetTypeStruct(" << r << ", " << child_count << ");\n\n";
}

SchemaRef CppSchemaSink::Child(SchemaRef parent, int i) {
    return Intern(Expr(parent) + "->children[" + std::to_string(i) + "]");
}

void CppSchemaSink::SetTypeScalar(SchemaRef schema, ArrowType type) {
    out_ << indent_ << "ArrowSchemaSetType(" << Expr(schema) << ", " << ArrowTypeName(type) << ");\n";
}

void CppSchemaSink::SetTypeList(SchemaRef schema) {
    out_ << indent_ << "ArrowSchemaSetType(" << Expr(schema) << ", NANOARROW_TYPE_LIST);\n";
}

void CppSchemaSink::SetTypeMap(SchemaRef schema) {
    out_ << indent_ << "ArrowSchemaSetType(" << Expr(schema) << ", NANOARROW_TYPE_MAP);\n";
}

void CppSchemaSink::SetTypeDateTime(SchemaRef schema, ArrowType type, ArrowTimeUnit time_unit,
                                    const char* timezone) {
    out_ << indent_ << "ArrowSchemaSetTypeDateTime(" << Expr(schema) << ", " << ArrowTypeName(type)
         << ", " << ArrowTimeUnitName(time_unit) << ", " << (timezone ? timezone : "nullptr")
         << ");\n";
}

void CppSchemaSink::SetName(SchemaRef schema, std::string_view name) {
    out_ << indent_ << "ArrowSchemaSetName(" << Expr(schema) << ", \"" << name << "\");\n";
}

void CppSchemaSink::SetNullable(SchemaRef schema, bool nullable) {
    if (nullable)
        out_ << indent_ << Expr(schema) << "->flags |= ARROW_FLAG_NULLABLE;\n";
    else
        out_ << indent_ << Expr(schema) << "->flags &= ~ARROW_FLAG_NULLABLE;\n";
}

void CppSchemaSink::SetMetadata(SchemaRef schema,
                                const std::vector<std::pair<std::string, std::string>>& pairs) {
    out_ << indent_ << "{\n"
         << indent_ << "    struct ArrowBuffer buf;\n"
         << indent_ << "    ArrowBufferInit(&buf);\n"
         << indent_ << "    ArrowMetadataBuilderInit(&buf, nullptr);\n";
    for (const auto& [key, value] : pairs) {
        out_ << indent_ << "    ArrowMetadataBuilderAppend(&buf,\n"
             << indent_ << "        ArrowCharView(\"" << key << "\"),\n"
             << indent_ << "        ArrowCharView(\"" << value << "\"));\n";
    }
    out_ << indent_ << "    ArrowSchemaSetMetadata(" << Expr(schema) << ",\n"
         << indent_ << "        reinterpret_cast<const char*>(buf.data));\n"
         << indent_ << "    ArrowBufferReset(&buf);\n"
         << indent_ << "}\n\n";
}

void CppSchemaSink::DeepCopyMessageStruct(const google::protobuf::Descriptor* nested_msg,
                                          SchemaRef dst) {
    out_ << indent_ << "ArrowSchemaDeepCopy(" << CppClassName(nested_msg, context_file_)
         << "Schema().get(), " << Expr(dst) << ");\n";
}

// ===========================================================================
// NanoarrowSchemaSink
// ===========================================================================

NanoarrowSchemaSink::NanoarrowSchemaSink(ArrowSchema* root) : root_(root) {}

SchemaRef NanoarrowSchemaSink::Root() { return static_cast<SchemaRef>(root_); }

void NanoarrowSchemaSink::InitRootStruct(SchemaRef root, int64_t child_count) {
    auto* schema = static_cast<ArrowSchema*>(root);
    ArrowSchemaInit(schema);
    CheckNa(ArrowSchemaSetTypeStruct(schema, child_count), "set struct type");
}

SchemaRef NanoarrowSchemaSink::Child(SchemaRef parent, int i) {
    return static_cast<SchemaRef>(static_cast<ArrowSchema*>(parent)->children[i]);
}

void NanoarrowSchemaSink::SetTypeScalar(SchemaRef schema, ArrowType type) {
    CheckNa(ArrowSchemaSetType(static_cast<ArrowSchema*>(schema), type), "set scalar type");
}

void NanoarrowSchemaSink::SetTypeList(SchemaRef schema) {
    CheckNa(ArrowSchemaSetType(static_cast<ArrowSchema*>(schema), NANOARROW_TYPE_LIST),
            "set list type");
}

void NanoarrowSchemaSink::SetTypeMap(SchemaRef schema) {
    CheckNa(ArrowSchemaSetType(static_cast<ArrowSchema*>(schema), NANOARROW_TYPE_MAP),
            "set map type");
}

void NanoarrowSchemaSink::SetTypeDateTime(SchemaRef schema, ArrowType type, ArrowTimeUnit time_unit,
                                          const char* timezone) {
    CheckNa(ArrowSchemaSetTypeDateTime(static_cast<ArrowSchema*>(schema), type, time_unit, timezone),
            "set datetime type");
}

void NanoarrowSchemaSink::SetName(SchemaRef schema, std::string_view name) {
    // ArrowSchemaSetName needs a NUL-terminated C string.
    const std::string owned(name);
    CheckNa(ArrowSchemaSetName(static_cast<ArrowSchema*>(schema), owned.c_str()), "set name");
}

void NanoarrowSchemaSink::SetNullable(SchemaRef schema, bool nullable) {
    auto* s = static_cast<ArrowSchema*>(schema);
    if (nullable)
        s->flags |= ARROW_FLAG_NULLABLE;
    else
        s->flags &= ~ARROW_FLAG_NULLABLE;
}

void NanoarrowSchemaSink::SetMetadata(SchemaRef schema,
                                      const std::vector<std::pair<std::string, std::string>>& pairs) {
    auto* s = static_cast<ArrowSchema*>(schema);
    ArrowBuffer buf;
    ArrowBufferInit(&buf);
    ArrowErrorCode code = ArrowMetadataBuilderInit(&buf, nullptr);
    for (const auto& [key, value] : pairs) {
        if (code != NANOARROW_OK) break;
        code = ArrowMetadataBuilderAppend(&buf, ArrowCharView(key.c_str()),
                                          ArrowCharView(value.c_str()));
    }
    if (code == NANOARROW_OK)
        code = ArrowSchemaSetMetadata(s, reinterpret_cast<const char*>(buf.data));
    ArrowBufferReset(&buf);
    CheckNa(code, "set metadata");
}

void NanoarrowSchemaSink::DeepCopyMessageStruct(const google::protobuf::Descriptor* nested_msg,
                                                SchemaRef dst) {
    nanoarrow::UniqueSchema nested;
    BuildMessageSchemaIntoFromIr(nested_msg, nested.get());
    CheckNa(ArrowSchemaDeepCopy(nested.get(), static_cast<ArrowSchema*>(dst)), "copy struct schema");
}

// ===========================================================================
// SchemaVisitor
// ===========================================================================

SchemaVisitor::SchemaVisitor(const google::protobuf::Descriptor* msg,
                             const google::protobuf::FileDescriptor* context_file, SchemaSink& sink)
    : message_(msg), context_file_(context_file), sink_(sink) {}

void SchemaVisitor::EmitScalarType(const ir::ScalarNode& scalar, SchemaRef schema) {
    const ir::LogicalType& lt = scalar.logical_type;
    using LK = ir::LogicalKind;
    switch (lt.kind) {
        case LK::BOOL:
            sink_.SetTypeScalar(schema, NANOARROW_TYPE_BOOL);
            return;
        case LK::INT32:
            // Enum lowers to INT32 storage (enum_identity present) — same physical
            // type either way (locked: enum symbol emission is GIR-9 scope).
            sink_.SetTypeScalar(schema, NANOARROW_TYPE_INT32);
            return;
        case LK::INT64:
            sink_.SetTypeScalar(schema, NANOARROW_TYPE_INT64);
            return;
        case LK::UINT32:
            sink_.SetTypeScalar(schema, NANOARROW_TYPE_UINT32);
            return;
        case LK::UINT64:
            sink_.SetTypeScalar(schema, NANOARROW_TYPE_UINT64);
            return;
        case LK::FLOAT32:
            sink_.SetTypeScalar(schema, NANOARROW_TYPE_FLOAT);
            return;
        case LK::FLOAT64:
            sink_.SetTypeScalar(schema, NANOARROW_TYPE_DOUBLE);
            return;
        case LK::UTF8:
            sink_.SetTypeScalar(schema, NANOARROW_TYPE_STRING);
            return;
        case LK::BINARY:
        case LK::FIXED_SIZE_BINARY:
            sink_.SetTypeScalar(schema, NANOARROW_TYPE_BINARY);
            return;
        case LK::TIMESTAMP:
        case LK::WKT_TIMESTAMP:
            // Timestamp unit is hardcoded to NANO in today's emitter.
            sink_.SetTypeDateTime(schema, NANOARROW_TYPE_TIMESTAMP, NANOARROW_TIME_UNIT_NANO,
                                  nullptr);
            return;
        case LK::DURATION:
        case LK::WKT_DURATION:
            sink_.SetTypeDateTime(schema, NANOARROW_TYPE_DURATION, NANOARROW_TIME_UNIT_NANO,
                                  nullptr);
            return;
        default:
            break;
    }
    // Defensive: mirrors SetScalarSchemaType's throw on an unmappable scalar.
    throw std::runtime_error("schema visitor: unsupported scalar logical kind");
}

void SchemaVisitor::EmitNodeType(const ir::IrNode& node, SchemaRef schema) {
    switch (node.kind) {
        case NodeKind::SCALAR:
            EmitScalarType(std::get<ir::ScalarNode>(node.node), schema);
            return;

        case NodeKind::LIST: {
            sink_.SetTypeList(schema);
            const IrNode& elem = *std::get<ir::ListNode>(node.node).element;
            SchemaRef item = sink_.Child(schema, 0);
            // Recurse into the element FIRST, then restore the "item" name AFTER
            // (a struct deep-copy overwrites the name). Correct for List<Scalar>,
            // List<Struct>, and List<List<...<Struct>>>.
            EmitNodeType(elem, item);
            sink_.SetName(item, "item");
            return;
        }

        case NodeKind::STRUCT:
            sink_.DeepCopyMessageStruct(std::get<ir::StructNode>(node.node).identity.descriptor,
                                        schema);
            return;

        case NodeKind::MAP: {
            const auto& mp = std::get<ir::MapNode>(node.node);
            sink_.SetTypeMap(schema);
            SchemaRef entries = sink_.MapEntries(schema);
            SchemaRef key_child = sink_.MapKey(entries);
            SchemaRef value_child = sink_.MapValue(entries);

            // Key is always a scalar; keep nanoarrow's default "key" name.
            EmitScalarType(std::get<ir::ScalarNode>(mp.key->node), key_child);

            if (mp.value->kind == NodeKind::STRUCT) {
                sink_.DeepCopyMessageStruct(
                    std::get<ir::StructNode>(mp.value->node).identity.descriptor, value_child);
                sink_.SetName(value_child, "value");  // deep-copy overwrote the name
            } else {
                // Scalar value keeps nanoarrow's default "value" name.
                EmitScalarType(std::get<ir::ScalarNode>(mp.value->node), value_child);
            }
            return;
        }

        case NodeKind::FIXED_SIZE_LIST:
        case NodeKind::UNSUPPORTED:
            // Filtered out by BuildFlattenedFieldList — unreachable.
            throw std::runtime_error("schema visitor: unrepresentable node reached emission");
    }
}

void SchemaVisitor::Visit() {
    const std::vector<SchemaFieldRecord> fields = BuildFlattenedFieldList(message_);

    SchemaRef root = sink_.Root();
    sink_.InitRootStruct(root, static_cast<int64_t>(fields.size()));
    sink_.SetMetadata(root, {{"proto_package", message_->file()->package()},
                             {"proto_message", message_->name()}});

    for (size_t i = 0; i < fields.size(); ++i) {
        const SchemaFieldRecord& f = fields[i];
        SchemaRef child = sink_.Child(root, static_cast<int>(i));

        // Type first, then the field overlay (name -> nullable -> metadata). The
        // order matters for nested structs: the deep-copy establishes the nested
        // schema's name + metadata, which the overlay then overwrites.
        EmitNodeType(*f.node, child);
        sink_.SetName(child, f.name);
        sink_.SetNullable(child, f.node->facts.nullable);
        sink_.SetMetadata(child, {{"field_number", std::to_string(f.field_number)},
                                  {"field_id", f.field_id}});
    }
}

// ===========================================================================
// Public entry points
// ===========================================================================

std::string GenerateSchemaFunctionFromIr(const std::string& cls,
                                         const google::protobuf::Descriptor* msg,
                                         const google::protobuf::FileDescriptor* context_file) {
    std::ostringstream o;
    o << "/// Returns the nanoarrow schema describing this message's wire layout.\n"
      << "/// Providers publish this schema on companion topics so that subscribers\n"
      << "/// can decode rows without prior knowledge of the message definition.\n";
    o << "inline fletcher::OwnedSchema " << cls << "Schema() {\n"
      << "    fletcher::OwnedSchema schema;\n";

    CppSchemaSink sink(o, "    ", context_file);
    SchemaVisitor visitor(msg, context_file, sink);
    visitor.Visit();

    o << "    return schema;\n}\n";
    return o.str();
}

void BuildMessageSchemaIntoFromIr(const google::protobuf::Descriptor* msg, ArrowSchema* schema) {
    NanoarrowSchemaSink sink(schema);
    SchemaVisitor visitor(msg, msg->file(), sink);
    visitor.Visit();
}

}  // namespace fletcher::cpp_backend
