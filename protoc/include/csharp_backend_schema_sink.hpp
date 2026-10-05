// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#pragma once

// BIND-6b: the third SchemaSink. cpp_backend::SchemaVisitor drives one walk into a
// sink; CppSchemaSink renders it as C++ source and NanoarrowSchemaSink builds a
// live ArrowSchema. This sink records the same calls as a small tree and renders
// it as Apache.Arrow C# construction code, so a generated C# `Schema` is the schema
// C++ generates by construction: the same fields, nullability and metadata,
// `metadata_from_option` extras included, from the same visitor.
//
// The tree mirrors what nanoarrow does with each call, defaults included: a list's
// child is "item" and nullable; a map's "entries" and "key" are non-nullable and
// its "value" nullable; a nested message is copied inline by running the visitor
// into the subtree, exactly as NanoarrowSchemaSink deep-copies it. Every C# string
// comes from csharp_backend_type_table.

#include <google/protobuf/descriptor.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "cpp_backend_schema_visitor.hpp"

namespace fletcher::csharp_backend {

class CsSchemaSink : public cpp_backend::SchemaSink {
   public:
    // One node of the recorded schema. Defaults are nanoarrow's ArrowSchemaInit.
    struct Node {
        enum class Kind { kUnset, kScalar, kDateTime, kList, kMap, kStruct };
        Kind kind = Kind::kUnset;
        ArrowType type = NANOARROW_TYPE_UNINITIALIZED;
        ArrowTimeUnit unit = NANOARROW_TIME_UNIT_NANO;
        std::optional<std::string> timezone;
        std::string name;
        bool nullable = true;
        std::vector<std::pair<std::string, std::string>> metadata;
        std::vector<std::unique_ptr<Node>> children;
    };

    CsSchemaSink();
    // A sink whose root is `root`: the nested-message copy runs the visitor into it.
    explicit CsSchemaSink(Node* root);

    cpp_backend::SchemaRef Root() override;
    void InitRootStruct(cpp_backend::SchemaRef root, int64_t child_count) override;
    cpp_backend::SchemaRef Child(cpp_backend::SchemaRef parent, int i) override;
    void SetTypeScalar(cpp_backend::SchemaRef schema, ArrowType type) override;
    void SetTypeList(cpp_backend::SchemaRef schema) override;
    void SetTypeMap(cpp_backend::SchemaRef schema) override;
    void SetTypeDateTime(cpp_backend::SchemaRef schema, ArrowType type, ArrowTimeUnit time_unit,
                         const char* timezone) override;
    void SetName(cpp_backend::SchemaRef schema, std::string_view name) override;
    void SetNullable(cpp_backend::SchemaRef schema, bool nullable) override;
    void SetMetadata(cpp_backend::SchemaRef schema,
                     const std::vector<std::pair<std::string, std::string>>& pairs) override;
    void DeepCopyMessageStruct(const google::protobuf::Descriptor* nested_msg,
                               cpp_backend::SchemaRef dst,
                               const OptionMetadataResolver* resolver) override;

    // The recorded root rendered as the initialiser of a static `Schema` property:
    // `new global::Apache.Arrow.Schema(<fields>, <metadata>)`, one top-level field per
    // line, each indented by `indent` plus one level.
    std::string RenderSchema(const std::string& indent) const;

   private:
    std::unique_ptr<Node> owned_root_;
    Node* root_;
};

// The C# `Schema` initialiser for `msg`, through SchemaVisitor and CsSchemaSink.
std::string RenderMessageSchema(const google::protobuf::Descriptor* msg,
                                const OptionMetadataResolver* resolver, const std::string& indent);

}  // namespace fletcher::csharp_backend
