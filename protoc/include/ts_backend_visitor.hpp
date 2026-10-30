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
// The committed coverage golden is the ground-truth gate (TsVisitor.DescriptorByteIdentical).
// A struct is always rendered by its IR identity, the message the wire carries, so a singular
// flatten-wrapper list field is ILeaf[] / Leaf.fields (D-BIND-119). A message of another file is
// named through one namespace import of that file, by a path relative to the importing file
// (D-BIND-122). Each TypedSchema carries its Arrow IPC schema as base64 `schemaIpc` when the
// generator passes it (D-BIND-130).

#include <google/protobuf/descriptor.h>

#include <cstdint>
#include <map>
#include <sstream>
#include <string>
#include <string_view>

#include "ir.hpp"

namespace fletcher::ts_backend {

class TsVisitor {
   public:
    // `schema_ipc`: base64 Arrow IPC per message, written as the TypedSchema's `schemaIpc`
    // (D-BIND-130); a message absent from it gets none.
    explicit TsVisitor(const google::protobuf::FileDescriptor* file,
                       std::map<const google::protobuf::Descriptor*, std::string> schema_ipc = {});

    // Generate the full .fletcher.ts file (header, cross-file imports,
    // per-message interface + TypedSchema, service topic constants, then the
    // pub/sub pairs, D-BIND-181).
    std::string GenerateFile();

   private:
    // `<Svc>_<Method>Publisher` and `<Svc>_<Method>Subscriber` over a FletcherClient (D-BIND-181,
    // D-BIND-183): the topic constant and the input message's TypedSchema bound in.
    std::string GeneratePair(const google::protobuf::MethodDescriptor* method);

    // Interface + TypedSchema for one message; "" for an empty (all-dropped) message.
    std::string GenerateMessage(const google::protobuf::Descriptor* msg);

    // TypeScript interface type text for a node (no top-level " | null" suffix).
    std::string InterfaceType(const ir::IrNode& node);

    // A top-level field's descriptor literal line (indented, trailing ",\n").
    std::string DescriptorLiteral(const ir::IrNode& node, std::string_view name,
                                  int32_t field_number, std::string_view indent);

    // A nested anonymous descriptor object ({ name: '', fieldNumber: 0, ...,
    // nullable: false, ... }); used for list elements and map key/value.
    std::string ElementDescriptor(const ir::IrNode& node);

    // Append the composite tail (element / fields / mapKey+mapValue) for a node.
    void AppendComposite(std::ostringstream& o, const ir::IrNode& node);

    // Runtime WireTypeId member name for a node.
    std::string WireType(const ir::IrNode& node);

    // A message as this file names it: bare when it is this file's, else through the namespace
    // its file is imported as (D-BIND-122).
    std::string SchemaName(const google::protobuf::Descriptor* msg) const;
    std::string InterfaceName(const google::protobuf::Descriptor* msg) const;

    const google::protobuf::FileDescriptor* file_;
    std::map<const google::protobuf::Descriptor*, std::string> schema_ipc_;
};

}  // namespace fletcher::ts_backend
