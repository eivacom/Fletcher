// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#pragma once

// GIR-7: the TypeScript interface + runtime TypedSchema/SchemaDescriptor
// generator, migrated from the flat FieldInfo/FieldMapping switches to a direct
// recursive IR visitor. This is the LAST emitter migration in the GIR cluster.
//
// The visitor reads ONLY the language-neutral IR (ir::IrNode) plus the source
// proto descriptor. Every TypeScript type string and WireTypeId member name comes
// from ts_backend::TsLookupScalar / TsInterfaceName / TsSchemaConstName — never
// from an IR node (locked decision #1). The message walk reuses the GIR-5 shared
// cpp_backend::BuildFlattenedFieldList (field-level flatten inlining + the
// unsupported-field drop set), so it cannot drift from the schema field set.
//
// Byte identity vs today's emitter is the ground-truth gate
// (TsVisitor.DescriptorByteIdentical). One subtlety it guards: a *singular*
// flatten-wrapper list field (e.g. `StructListWrapper flattened_struct_list`)
// flattens to List<Struct(Leaf)> in the IR, but today's TS preserves the DECLARED
// wrapper name (IStructListWrapper[] / StructListWrapper.fields), not the inner
// leaf identity. The visitor recovers that language-neutral proto fact from
// SchemaFieldRecord::source_field (see DeclaredWrapperFor). A *repeated*
// flatten-wrapper (nested list) keeps the leaf name (ILeaf[][]).

#include <google/protobuf/descriptor.h>

#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>

#include "ir.hpp"

namespace fletcher::ts_backend {

class TsVisitor {
public:
    explicit TsVisitor(const google::protobuf::FileDescriptor* file);

    // Generate the full .fletcher.ts file (header, cross-file placeholder imports,
    // per-message interface + TypedSchema, service topic constants).
    std::string GenerateFile();

private:
    // Interface + TypedSchema for one message; "" for an empty (all-dropped) message.
    std::string GenerateMessage(const google::protobuf::Descriptor* msg);

    // TypeScript interface type text for a node (no top-level " | null" suffix).
    std::string InterfaceType(const ir::IrNode& node,
                              const google::protobuf::Descriptor* declared_struct);

    // A top-level field's descriptor literal line (indented, trailing ",\n").
    std::string DescriptorLiteral(const ir::IrNode& node, std::string_view name,
                                  int32_t field_number,
                                  const google::protobuf::Descriptor* declared_struct,
                                  std::string_view indent);

    // A nested anonymous descriptor object ({ name: '', fieldNumber: 0, ...,
    // nullable: false, ... }); used for list elements and map key/value.
    std::string ElementDescriptor(const ir::IrNode& node,
                                  const google::protobuf::Descriptor* declared_struct);

    // Append the composite tail (element / fields / mapKey+mapValue) for a node.
    void AppendComposite(std::ostringstream& o, const ir::IrNode& node,
                         const google::protobuf::Descriptor* declared_struct);

    // Runtime WireTypeId member name for a node.
    std::string WireType(const ir::IrNode& node);

    const google::protobuf::FileDescriptor* file_;
};

}  // namespace fletcher::ts_backend
