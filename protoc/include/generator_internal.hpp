// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#pragma once

// Shared internal declarations for the protoc plugin's field-walk model.
//
// These helpers were originally file-local to generator.cpp's anonymous
// namespace. RBA-2 relocates their definitions into `namespace fletcher`
// (external linkage) and declares them here so the RecordBatch accessor
// emitter (recordbatch_accessor_emitter.cpp) can reuse the exact same schema
// model that generator.cpp uses. The relocation is a pure linkage change with
// no behavioural effect — emitted bytes are unchanged, guarded by the RBA-1
// no-drift test (AccessorTest.OptGatedEmissionLeavesExistingOutputsByteIdentical).
//
// GatherFieldsImpl is intentionally NOT declared here: it remains file-local to
// generator.cpp because the accessor TU only needs the public GatherFields entry
// point.

#include <google/protobuf/descriptor.h>

#include <string>
#include <vector>

#include "type_mapper.hpp"

namespace fletcher {

// Per-field information gathered before code generation. Mirrors the schema
// model produced by GatherFields: one entry per supported (mapped) field, with
// field-level-flatten sub-messages inlined.
struct FieldInfo {
    std::string name;
    FieldMapping mapping;
    int field_number = 0;  // leaf proto field number
    std::string field_id;  // dotted field-number path, unique even when field-level
                           // flatten inlines sub-messages (e.g. "2.1"); equals the
                           // field_number string for non-inlined top-level fields
    const google::protobuf::FieldDescriptor* descriptor{};  // original proto descriptor
};

// Topologically ordered, generatable messages of `file` (dependencies first,
// synthetic map-entries / recursive / out-of-file messages excluded).
std::vector<const google::protobuf::Descriptor*> OrderedMessages(
    const google::protobuf::FileDescriptor* file);

// Arrow type expression for the schema, constructed from the FieldMapping.
std::string ArrowTypeExpr(const FieldInfo& fi);

// Gather the supported fields of `msg` (flatten-inlining applied). Unsupported
// fields are appended to `*skipped_comment` and omitted from the result.
std::vector<FieldInfo> GatherFields(const google::protobuf::Descriptor* msg,
                                    std::string* skipped_comment);

}  // namespace fletcher
