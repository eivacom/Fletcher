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

#include <memory>
#include <set>
#include <string>
#include <vector>

#include "ir.hpp"
#include "option_metadata.hpp"
#include "type_mapper.hpp"

namespace fletcher {

// True if `method` is eligible to become a pub/sub topic (client-streaming
// request, no server streaming, google.protobuf.Empty reply, input message
// generated in this file). On failure, `*reason` is set to the user-facing skip
// text. Single source of truth for both the C++ pub/sub generator and the TS
// backend's topic-constant emission (GIR-7). Relocated to external linkage from
// generator.cpp's anonymous namespace (RBA-2 pattern) — no behavioural change.
bool ValidateServiceMethod(const google::protobuf::MethodDescriptor* method,
                           const std::set<const google::protobuf::Descriptor*>& generated_msgs,
                           std::string* reason);

// The topic of a pub/sub method as a segment list (D-BIND-180): the package split on '.',
// then the service, then the method. `package nexus.topics.v1;`, `LivePoints.Snapshot` gives
// {"nexus", "topics", "v1", "LivePoints", "Snapshot"}; no package gives {"LivePoints",
// "Snapshot"}. The ONE derivation every backend emits (C++'s TopicSegments(), C#'s Segments,
// TypeScript's constant through TopicKey), so a method names the same topic in every language.
std::vector<std::string> MethodTopicSegments(const google::protobuf::MethodDescriptor* method);

// The segments joined with '/': the topic's string form, the one the seam's join and the
// gateway's split agree on (C++'s TopicKey(), C#'s Key, TypeScript's constant).
std::string TopicKey(const std::vector<std::string>& segments);

// Per-field information gathered before code generation. Mirrors the schema
// model produced by GatherFields: one entry per supported (mapped) field, with
// field-level-flatten sub-messages inlined.
struct FieldInfo {
    std::string name;
    FieldMapping mapping;  // temporary bridge for the not-yet-migrated emitters
    int field_number = 0;  // leaf proto field number
    std::string field_id;  // dotted field-number path, unique even when field-level
                           // flatten inlines sub-messages (e.g. "2.1"); equals the
                           // field_number string for non-inlined top-level fields
    const google::protobuf::FieldDescriptor* descriptor{};  // original proto descriptor
    // GIR-3: the canonical language-neutral IR for this field — the source of
    // truth the edge ENCODE visitor walks. shared_ptr keeps FieldInfo copyable
    // despite IrNode owning unique_ptr children. `mapping` is a projection of
    // this same IR (single source), consumed by the other emitters for now.
    std::shared_ptr<const ir::IrNode> ir;
};

// What the C# backend writes for a .proto (BIND-6, D-BIND-76). Two tokens set it,
// `csharp` and the modifier `csharp_model_only`, but they mean three things, so they
// are folded into one value and the meaningless fourth combination (the modifier
// alone) cannot be represented: it is None, as if no C# token had been passed.
enum class CsharpOutput {
    None,          // no `csharp`: no C# at all
    ModelAndPair,  // `csharp`: <stem>.fletcher.cs and <stem>.fletcher.native.cs
    ModelOnly,     // `csharp,csharp_model_only`: <stem>.fletcher.cs alone, for an
                   // Apache.Arrow-only project (as C++'s `schema_only` opts out)
};

// Parsed form of the comma-separated --fletcher_opt=... plugin parameter.
struct PluginOptions {
    bool schema_only = false;
    bool ts = false;
    bool ipc = false;
    bool accessor = false;
    bool rust = false;
    CsharpOutput csharp = CsharpOutput::None;
    // `csharp_accessor` (BIND-7): <stem>.fletcher.accessor.cs and <stem>.fletcher.arrow.cs, the
    // C# accessor and view. It needs `csharp`, whose model class they read into (D-BIND-99).
    bool csharp_accessor = false;
    // `csharp_gateway` (BIND-8c, D-BIND-109): <stem>.fletcher.gateway.cs, the typed publisher and
    // subscriber over Eiva.Fletcher.GatewayClient. It needs `csharp`, whose model it converts with.
    bool csharp_gateway = false;
    std::vector<MetadataRule> metadata_rules;
};

// Parse the --fletcher_opt parameter into `*out`. Unknown tokens are ignored
// without error, unchanged from the behaviour every existing caller relies on.
// Returns false and sets *error only for a token the plugin recognises but
// cannot parse. Single tokenizer for both Generate() and GenerateAll() — a new
// opt token added here reaches BOTH, which is the point of the shared function.
bool ParsePluginParameter(const std::string& parameter, PluginOptions* out, std::string* error);

// Topologically ordered, generatable messages of `file` (dependencies first,
// synthetic map-entries / recursive / out-of-file messages excluded).
std::vector<const google::protobuf::Descriptor*> OrderedMessages(
    const google::protobuf::FileDescriptor* file);

// Arrow type expression for the schema, constructed from the FieldMapping.
std::string ArrowTypeExpr(const FieldInfo& fi);

// Gather the supported fields of `msg` (flatten-inlining applied). Unsupported
// fields are appended to `*skipped_comment` and omitted from the result.
//
// GIR-5: the schema paths no longer consume this — schema construction is driven
// by cpp_backend::BuildFlattenedFieldList (a language-neutral IR walk). Gather
// Fields remains the FieldMapping-bridge source for the not-yet-migrated
// emitters (row class, RBA accessors, TS) and for the skipped-field comment;
// the schema visitor reads it read-only via that shared field-order model.
std::vector<FieldInfo> GatherFields(const google::protobuf::Descriptor* msg,
                                    std::string* skipped_comment);

// Cross-file generated-header include paths needed by `file`: scans every
// supported field mapping (recursively through nested types) and collects the
// type-mapper's nested_header / map_value_header for messages referenced from
// other .proto files (the `<dep>.fletcher.pb.h` dependency path). Empty for
// same-file-only references. This is the single source of truth for cross-file
// include discovery (D-RBA-10); both the row-generator and the accessor emitter
// call it read-only — the accessor emitter additionally rewrites the suffix to
// `.fletcher.accessor.pb.h` on the returned set.
//
// Relocated (declaration here + definition in namespace fletcher) from
// generator.cpp's anonymous namespace following the RBA-2 GatherFields pattern:
// a pure linkage change with no behavioural effect, guarded byte-for-byte by the
// RBA-1 no-drift test.
std::set<std::string> CollectCrossFileIncludes(const google::protobuf::FileDescriptor* file);

// The walk CollectCrossFileIncludes derives its paths from (D-BIND-87): the
// messages from OTHER .proto files that `file`'s fields name, as a field, a list
// element at any depth or a map value, recursively through nested types. Each
// backend derives its own cross-file dependency from this one set: C++ an include
// path per message, C# whether a struct column is built through the other
// class's public ToArrow (D-BIND-80), so the two cannot disagree on which types
// come from elsewhere.
std::set<const google::protobuf::Descriptor*> CollectCrossFileMessages(
    const google::protobuf::FileDescriptor* file);

}  // namespace fletcher
