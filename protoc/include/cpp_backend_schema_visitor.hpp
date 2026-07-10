// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#pragma once

// GIR-5: the unified schema visitor. ONE IR-driven walk drives both schema
// construction paths (locked decision #5):
//
//   * the generated C++ `<Class>Schema()` source (CppSchemaSink -> text), and
//   * the in-process / IPC ArrowSchema (NanoarrowSchemaSink -> nanoarrow calls).
//
// Previously these were two hand-kept-in-lockstep emitters
// (GenerateSchemaFunction / EmitNanoarrowTypeSetup and BuildMessageSchemaInto /
// SetScalarSchemaType). Now a single SchemaVisitor issues the same logical
// operations (SetType, SetName, SetNullable, SetMetadata, DeepCopyMessageStruct)
// through a sink abstraction, so the two outputs cannot drift.
//
// The visitor reads ONLY the language-neutral IR (ir::IrNode): the LogicalKind ->
// nanoarrow physical type mapping lives in the C++ sink, never on an IR node
// (locked decision #1). The flatten walk (BuildFlattenedFieldList) is a
// language-neutral mirror of GatherFieldsImpl and filters exactly the fields the
// IR->FieldMapping projection drops today (type_mapper.cpp:150-152 and the
// non-representable list/map shapes), preserving byte-identical schema output.

#include <google/protobuf/descriptor.h>

#include <cstdint>
#include <deque>
#include <memory>
#include <nanoarrow/nanoarrow.hpp>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ir.hpp"

namespace fletcher::cpp_backend {

// One flattened top-level schema field: the IR node to emit, its leaf proto
// field number, and the dotted field_id path (unique across field-level-flatten
// inlining, e.g. "2.1"). Mirrors the schema-relevant subset of FieldInfo.
struct SchemaFieldRecord {
    std::string name;
    int32_t field_number = 0;
    std::string field_id;  // dotted path, e.g. "2.1"
    std::shared_ptr<const ir::IrNode> node;
    // GIR-7: the source proto field for a non-flattened top-level field (nullptr
    // for field-level-flatten inlined fields). The TS backend consults this to
    // recover the declared wrapper name of a singular flatten-wrapper list field
    // (Descriptor::name(), a language-neutral proto fact — no type string on the
    // IR node). Unused by the schema paths.
    const google::protobuf::FieldDescriptor* source_field = nullptr;
};

// Walk `msg` applying field-level flatten and building the flattened field list
// with dotted field_id paths. Language-neutral mirror of GatherFieldsImpl: it
// reads only the proto descriptor + IR node (no FieldMapping projection). Fields
// whose IR node is not representable in the flat Arrow schema are dropped,
// reproducing today's ProjectIrToFieldMapping-nullopt filter so the emitted
// schema keeps the same children.
std::vector<SchemaFieldRecord> BuildFlattenedFieldList(const google::protobuf::Descriptor* msg,
                                                       const std::string& id_prefix = "");

// Opaque handle to a schema node. For the nanoarrow sink it is an ArrowSchema*;
// for the C++ source sink it is a pointer to an owned C-expression string.
using SchemaRef = void*;

// Sink abstraction: encapsulates every nanoarrow call and child navigation so
// the visitor is sink-agnostic. Both sinks implement this identically.
class SchemaSink {
public:
    virtual ~SchemaSink() = default;

    // Root of the schema being built (sink-specific handle).
    virtual SchemaRef Root() = 0;

    // Initialise `root` as a struct with `child_count` children.
    virtual void InitRootStruct(SchemaRef root, int64_t child_count) = 0;

    // Navigate to child `i` of `parent` (children allocated by a prior SetType).
    virtual SchemaRef Child(SchemaRef parent, int i) = 0;

    // Map navigation, derived from nanoarrow MAP layout (map -> entries ->
    // [key, value]). Non-virtual: expressed in terms of Child().
    SchemaRef MapEntries(SchemaRef map_parent) { return Child(map_parent, 0); }
    SchemaRef MapKey(SchemaRef entries) { return Child(entries, 0); }
    SchemaRef MapValue(SchemaRef entries) { return Child(entries, 1); }

    // Type setters (allocate children where nanoarrow does).
    virtual void SetTypeScalar(SchemaRef schema, ArrowType type) = 0;
    virtual void SetTypeList(SchemaRef schema) = 0;
    virtual void SetTypeMap(SchemaRef schema) = 0;
    virtual void SetTypeDateTime(SchemaRef schema, ArrowType type, ArrowTimeUnit time_unit,
                                 const char* timezone) = 0;

    // Field overlay.
    virtual void SetName(SchemaRef schema, std::string_view name) = 0;
    virtual void SetNullable(SchemaRef schema, bool nullable) = 0;
    virtual void SetMetadata(SchemaRef schema,
                             const std::vector<std::pair<std::string, std::string>>& pairs) = 0;

    // Emit a nested message's full struct schema at `dst` (deep-copy semantics).
    // The visitor applies the field overlay (name/nullable/metadata) after this.
    virtual void DeepCopyMessageStruct(const google::protobuf::Descriptor* nested_msg,
                                       SchemaRef dst) = 0;
};

// Renders the schema-construction program as C++ source lines into `out`.
class CppSchemaSink : public SchemaSink {
public:
    CppSchemaSink(std::ostringstream& out, std::string indent,
                  const google::protobuf::FileDescriptor* context_file);

    SchemaRef Root() override;
    void InitRootStruct(SchemaRef root, int64_t child_count) override;
    SchemaRef Child(SchemaRef parent, int i) override;
    void SetTypeScalar(SchemaRef schema, ArrowType type) override;
    void SetTypeList(SchemaRef schema) override;
    void SetTypeMap(SchemaRef schema) override;
    void SetTypeDateTime(SchemaRef schema, ArrowType type, ArrowTimeUnit time_unit,
                         const char* timezone) override;
    void SetName(SchemaRef schema, std::string_view name) override;
    void SetNullable(SchemaRef schema, bool nullable) override;
    void SetMetadata(SchemaRef schema,
                     const std::vector<std::pair<std::string, std::string>>& pairs) override;
    void DeepCopyMessageStruct(const google::protobuf::Descriptor* nested_msg,
                               SchemaRef dst) override;

private:
    const std::string& Expr(SchemaRef ref) const;
    SchemaRef Intern(std::string expr);

    std::ostringstream& out_;
    std::string indent_;
    const google::protobuf::FileDescriptor* context_file_;
    // Stable storage for the C-expression strings SchemaRef points at. deque
    // keeps element addresses stable across push_back.
    std::deque<std::string> exprs_;
};

// Executes the schema-construction program in-process against an ArrowSchema*.
class NanoarrowSchemaSink : public SchemaSink {
public:
    explicit NanoarrowSchemaSink(ArrowSchema* root);

    SchemaRef Root() override;
    void InitRootStruct(SchemaRef root, int64_t child_count) override;
    SchemaRef Child(SchemaRef parent, int i) override;
    void SetTypeScalar(SchemaRef schema, ArrowType type) override;
    void SetTypeList(SchemaRef schema) override;
    void SetTypeMap(SchemaRef schema) override;
    void SetTypeDateTime(SchemaRef schema, ArrowType type, ArrowTimeUnit time_unit,
                         const char* timezone) override;
    void SetName(SchemaRef schema, std::string_view name) override;
    void SetNullable(SchemaRef schema, bool nullable) override;
    void SetMetadata(SchemaRef schema,
                     const std::vector<std::pair<std::string, std::string>>& pairs) override;
    void DeepCopyMessageStruct(const google::protobuf::Descriptor* nested_msg,
                               SchemaRef dst) override;

private:
    ArrowSchema* root_;
};

// Orchestrates the flatten walk + per-field emission, driving the given sink.
class SchemaVisitor {
public:
    SchemaVisitor(const google::protobuf::Descriptor* msg,
                  const google::protobuf::FileDescriptor* context_file, SchemaSink& sink);

    // Build the full message schema into sink_.Root().
    void Visit();

private:
    void EmitNodeType(const ir::IrNode& node, SchemaRef schema);
    void EmitScalarType(const ir::ScalarNode& scalar, SchemaRef schema);

    const google::protobuf::Descriptor* message_;
    const google::protobuf::FileDescriptor* context_file_;
    SchemaSink& sink_;
};

// Public entry points (drop-in replacements for the two old emitters).

// Generate the C++ source text for the free `<cls>Schema()` function.
std::string GenerateSchemaFunctionFromIr(const std::string& cls,
                                         const google::protobuf::Descriptor* msg,
                                         const google::protobuf::FileDescriptor* context_file);

// Build the in-process ArrowSchema for `msg` (uninitialised/released on entry).
void BuildMessageSchemaIntoFromIr(const google::protobuf::Descriptor* msg, ArrowSchema* schema);

}  // namespace fletcher::cpp_backend
