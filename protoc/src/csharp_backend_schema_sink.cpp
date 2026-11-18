// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include "csharp_backend_schema_sink.hpp"

#include <sstream>
#include <stdexcept>
#include <string>

#include "csharp_backend_type_table.hpp"

namespace fletcher::csharp_backend {

namespace {

using Node = CsSchemaSink::Node;

Node* AsNode(cpp_backend::SchemaRef ref) { return static_cast<Node*>(ref); }

std::unique_ptr<Node> NewNode(std::string name, bool nullable) {
    auto node = std::make_unique<Node>();
    node->name = std::move(name);
    node->nullable = nullable;
    return node;
}

std::string MetadataExpr(const std::vector<std::pair<std::string, std::string>>& pairs) {
    std::string out = "new global::System.Collections.Generic.KeyValuePair<string, string>[] { ";
    for (size_t i = 0; i < pairs.size(); ++i) {
        if (i > 0) out += ", ";
        out += "new(" + CsStringLiteral(pairs[i].first) + ", " + CsStringLiteral(pairs[i].second) +
               ")";
    }
    return out + " }";
}

std::string TypeExpr(const Node& node);

// `new Field(name, type, nullable[, metadata])`: the metadata argument is left out
// when there is none, which is what Field's own default means.
std::string FieldExpr(const Node& node) {
    std::string out = "new global::Apache.Arrow.Field(" + CsStringLiteral(node.name) + ", " +
                      TypeExpr(node) + ", " + (node.nullable ? "true" : "false");
    if (!node.metadata.empty()) out += ", " + MetadataExpr(node.metadata);
    return out + ")";
}

std::string FieldsExpr(const Node& node) {
    std::string out = "new global::Apache.Arrow.Field[] { ";
    for (size_t i = 0; i < node.children.size(); ++i) {
        if (i > 0) out += ", ";
        out += FieldExpr(*node.children[i]);
    }
    return out + " }";
}

std::string TypeExpr(const Node& node) {
    switch (node.kind) {
        case Node::Kind::kScalar:
            if (auto expr = CsArrowScalarTypeExpr(node.type)) return *expr;
            break;
        case Node::Kind::kDateTime:
            if (auto expr = CsArrowDateTimeTypeExpr(
                    node.type, node.unit, node.timezone ? node.timezone->c_str() : nullptr))
                return *expr;
            break;
        case Node::Kind::kList:
            return "new global::Apache.Arrow.Types.ListType(" + FieldExpr(*node.children.at(0)) +
                   ")";
        case Node::Kind::kMap: {
            // MapType builds its own "entries" struct; nanoarrow's is non-nullable
            // with exactly these two children, and the visitor never touches it.
            const Node& entries = *node.children.at(0);
            return "new global::Apache.Arrow.Types.MapType(" + FieldExpr(*entries.children.at(0)) +
                   ", " + FieldExpr(*entries.children.at(1)) + ")";
        }
        case Node::Kind::kStruct:
            return "new global::Apache.Arrow.Types.StructType(" + FieldsExpr(node) + ")";
        case Node::Kind::kUnset:
            break;
    }
    throw std::runtime_error("C# schema sink: field '" + node.name + "' has no C# Arrow type");
}

}  // namespace

CsSchemaSink::CsSchemaSink() : owned_root_(std::make_unique<Node>()), root_(owned_root_.get()) {}

CsSchemaSink::CsSchemaSink(Node* root) : root_(root) {}

cpp_backend::SchemaRef CsSchemaSink::Root() { return root_; }

void CsSchemaSink::InitRootStruct(cpp_backend::SchemaRef root, int64_t child_count) {
    // ArrowSchemaInit + SetTypeStruct: a fresh nullable, unnamed struct. On a nested
    // copy this is what replaces the destination, before the overlay renames it.
    Node* node = AsNode(root);
    *node = Node{};
    node->kind = Node::Kind::kStruct;
    for (int64_t i = 0; i < child_count; ++i) node->children.push_back(NewNode("", true));
}

cpp_backend::SchemaRef CsSchemaSink::Child(cpp_backend::SchemaRef parent, int i) {
    return AsNode(parent)->children.at(static_cast<size_t>(i)).get();
}

void CsSchemaSink::SetTypeScalar(cpp_backend::SchemaRef schema, ArrowType type) {
    Node* node = AsNode(schema);
    node->kind = Node::Kind::kScalar;
    node->type = type;
}

void CsSchemaSink::SetTypeList(cpp_backend::SchemaRef schema) {
    Node* node = AsNode(schema);
    node->kind = Node::Kind::kList;
    node->children.clear();
    node->children.push_back(NewNode("item", true));
}

void CsSchemaSink::SetTypeMap(cpp_backend::SchemaRef schema) {
    Node* node = AsNode(schema);
    node->kind = Node::Kind::kMap;
    node->children.clear();
    auto entries = NewNode("entries", false);
    entries->kind = Node::Kind::kStruct;
    entries->children.push_back(NewNode("key", false));
    entries->children.push_back(NewNode("value", true));
    node->children.push_back(std::move(entries));
}

void CsSchemaSink::SetTypeDateTime(cpp_backend::SchemaRef schema, ArrowType type,
                                   ArrowTimeUnit time_unit, const char* timezone) {
    Node* node = AsNode(schema);
    node->kind = Node::Kind::kDateTime;
    node->type = type;
    node->unit = time_unit;
    node->timezone = timezone ? std::optional<std::string>(timezone) : std::nullopt;
}

void CsSchemaSink::SetName(cpp_backend::SchemaRef schema, std::string_view name) {
    AsNode(schema)->name = std::string(name);
}

void CsSchemaSink::SetNullable(cpp_backend::SchemaRef schema, bool nullable) {
    AsNode(schema)->nullable = nullable;
}

void CsSchemaSink::SetMetadata(cpp_backend::SchemaRef schema,
                               const std::vector<std::pair<std::string, std::string>>& pairs) {
    AsNode(schema)->metadata = pairs;
}

void CsSchemaSink::DeepCopyMessageStruct(const google::protobuf::Descriptor* nested_msg,
                                         cpp_backend::SchemaRef dst,
                                         const OptionMetadataResolver* resolver) {
    // As NanoarrowSchemaSink: build the nested message's own schema, with the same
    // resolver, into the destination. Its root metadata and nullability survive
    // wherever the visitor does not overlay them (a list's "item", a map's "value").
    CsSchemaSink nested(AsNode(dst));
    cpp_backend::SchemaVisitor visitor(nested_msg, nested_msg->file(), nested, resolver);
    visitor.Visit();
}

std::string CsSchemaSink::RenderSchema(const std::string& indent) const {
    std::ostringstream o;
    o << "new global::Apache.Arrow.Schema(\n"
      << indent << "    new global::Apache.Arrow.Field[]\n"
      << indent << "    {\n";
    for (const auto& child : root_->children)
        o << indent << "        " << FieldExpr(*child) << ",\n";
    o << indent << "    },\n" << indent << "    " << MetadataExpr(root_->metadata) << ")";
    return o.str();
}

std::string RenderMessageSchema(const google::protobuf::Descriptor* msg,
                                const OptionMetadataResolver* resolver, const std::string& indent) {
    CsSchemaSink sink;
    cpp_backend::SchemaVisitor visitor(msg, msg->file(), sink, resolver);
    visitor.Visit();
    return sink.RenderSchema(indent);
}

}  // namespace fletcher::csharp_backend
