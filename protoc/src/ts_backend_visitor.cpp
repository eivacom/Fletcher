// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include "ts_backend_visitor.hpp"

#include <optional>
#include <set>
#include <string>
#include <variant>
#include <vector>

#include "cpp_backend_schema_visitor.hpp"
#include "generator_internal.hpp"
#include "ts_backend_type_table.hpp"
#include "type_mapper.hpp"

namespace fletcher::ts_backend {

namespace {

using google::protobuf::Descriptor;
using google::protobuf::FieldDescriptor;
using ir::IrNode;
using ir::NodeKind;

// ---------------------------------------------------------------------------
// Small language-neutral proto helpers (byte-shape copies of the former
// generator.cpp TS helpers; homed here now that TS owns file orchestration).
// ---------------------------------------------------------------------------

std::string StripProtoSuffix(const std::string& proto_name) {
    constexpr std::string_view kSuffix = ".proto";
    std::string base = proto_name;
    if (base.size() > kSuffix.size() && base.substr(base.size() - kSuffix.size()) == kSuffix)
        base.resize(base.size() - kSuffix.size());
    return base;
}

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

// The declared wrapper name to render for a field, or nullptr to use the IR's own
// struct identity. Recovers today's byte output for SINGULAR flatten-wrapper list
// fields: `StructListWrapper foo = N;` flattens to List<Struct(Leaf)> in the IR,
// but the TS emits the declared IStructListWrapper[] / StructListWrapper.fields.
// A REPEATED flatten-wrapper (a nested list) keeps the inner leaf name (ILeaf[][]),
// so it is gated out here. The name comes from Descriptor::name() (a
// language-neutral proto fact), never from a TS/C++ string on an IR node.
const Descriptor* DeclaredWrapperFor(const FieldDescriptor* source_field) {
    if (source_field == nullptr) return nullptr;
    if (source_field->type() != FieldDescriptor::TYPE_MESSAGE) return nullptr;
    if (source_field->is_repeated()) return nullptr;
    if (!fletcher::IsFlattenedWrapper(source_field->message_type())) return nullptr;
    return source_field->message_type();
}

const IrNode* ListElement(const IrNode& node) {
    if (node.kind == NodeKind::LIST) return std::get<ir::ListNode>(node.node).element.get();
    return std::get<ir::FixedSizeListNode>(node.node).element.get();
}

const Descriptor* StructIdentity(const IrNode& node) {
    return std::get<ir::StructNode>(node.node).identity.descriptor;
}

// IR-native cross-file import discovery (no flat-bridge projection). Recursively
// find every struct message referenced by `node` whose file differs from
// `context_file` and record that file's name. WKT/wrapper scalars carry no struct
// identity, so they contribute nothing (matching today's scan, which only imports
// STRUCT/REPEATED_STRUCT/NESTED_LIST-leaf/MAP-value message files). Cross-file
// symbol resolution itself is GIR-8 scope; this only feeds the placeholder import
// lines and must yield the same (empty, for the coverage fixture) set as before.
void CollectStructImportFiles(const IrNode& node,
                              const google::protobuf::FileDescriptor* context_file,
                              std::set<std::string>& imports) {
    switch (node.kind) {
        case NodeKind::SCALAR:
        case NodeKind::UNSUPPORTED:
            return;
        case NodeKind::LIST:
        case NodeKind::FIXED_SIZE_LIST:
            CollectStructImportFiles(*ListElement(node), context_file, imports);
            return;
        case NodeKind::STRUCT: {
            const Descriptor* d = StructIdentity(node);
            if (d->file() != context_file) imports.insert(d->file()->name());
            return;
        }
        case NodeKind::MAP: {
            const auto& mp = std::get<ir::MapNode>(node.node);
            CollectStructImportFiles(*mp.key, context_file, imports);
            CollectStructImportFiles(*mp.value, context_file, imports);
            return;
        }
    }
}

}  // namespace

TsVisitor::TsVisitor(const google::protobuf::FileDescriptor* file) : file_(file) {}

std::string TsVisitor::WireType(const IrNode& node) {
    switch (node.kind) {
        case NodeKind::SCALAR: {
            const auto& s = std::get<ir::ScalarNode>(node.node);
            return TsLookupScalar(s.logical_type, s.enum_identity).wire_type_id;
        }
        case NodeKind::LIST:
        case NodeKind::FIXED_SIZE_LIST:
            return "WireTypeId.LIST";
        case NodeKind::STRUCT:
            return "WireTypeId.STRUCT";
        case NodeKind::MAP:
            return "WireTypeId.MAP";
        case NodeKind::UNSUPPORTED:
            return "WireTypeId.UNKNOWN";
    }
    return "WireTypeId.UNKNOWN";
}

std::string TsVisitor::InterfaceType(const IrNode& node, const Descriptor* declared_struct) {
    switch (node.kind) {
        case NodeKind::SCALAR: {
            const auto& s = std::get<ir::ScalarNode>(node.node);
            return TsLookupScalar(s.logical_type, s.enum_identity).ts_type_text;
        }
        case NodeKind::LIST:
        case NodeKind::FIXED_SIZE_LIST: {
            // The recovered wrapper name applies ONLY at the outer singular-wrapper
            // level: a singular flatten wrapper that collapses to List<Struct> emits
            // the wrapper name (IStructListWrapper[]), but one that collapses to a
            // NESTED list keeps the leaf identity (ILeaf[][]). So only thread
            // declared_struct into a DIRECT struct element; drop it for deeper lists.
            const IrNode& elem = *ListElement(node);
            const Descriptor* elem_declared =
                elem.kind == NodeKind::STRUCT ? declared_struct : nullptr;
            return InterfaceType(elem, elem_declared) + "[]";
        }
        case NodeKind::STRUCT: {
            const Descriptor* d = declared_struct ? declared_struct : StructIdentity(node);
            return TsInterfaceName(d);
        }
        case NodeKind::MAP: {
            const auto& mp = std::get<ir::MapNode>(node.node);
            return "Map<" + InterfaceType(*mp.key, nullptr) + ", " +
                   InterfaceType(*mp.value, nullptr) + ">";
        }
        case NodeKind::UNSUPPORTED:
            return "unknown";
    }
    return "unknown";
}

void TsVisitor::AppendComposite(std::ostringstream& o, const IrNode& node,
                                const Descriptor* declared_struct) {
    switch (node.kind) {
        case NodeKind::SCALAR:
        case NodeKind::UNSUPPORTED:
            return;
        case NodeKind::LIST:
        case NodeKind::FIXED_SIZE_LIST: {
            // See InterfaceType: declared_struct applies only to a DIRECT struct
            // element (List<Struct>), never through a nested list.
            const IrNode& elem = *ListElement(node);
            const Descriptor* elem_declared =
                elem.kind == NodeKind::STRUCT ? declared_struct : nullptr;
            o << ", element: " << ElementDescriptor(elem, elem_declared);
            return;
        }
        case NodeKind::STRUCT: {
            const Descriptor* d = declared_struct ? declared_struct : StructIdentity(node);
            o << ", fields: " << TsSchemaConstName(d) << ".fields";
            return;
        }
        case NodeKind::MAP: {
            const auto& mp = std::get<ir::MapNode>(node.node);
            o << ", mapKey: " << ElementDescriptor(*mp.key, nullptr)
              << ", mapValue: " << ElementDescriptor(*mp.value, nullptr);
            return;
        }
    }
}

std::string TsVisitor::ElementDescriptor(const IrNode& node, const Descriptor* declared_struct) {
    std::ostringstream o;
    o << "{ name: '', fieldNumber: 0, wireType: " << WireType(node) << ", nullable: false";
    AppendComposite(o, node, declared_struct);
    o << " }";
    return o.str();
}

std::string TsVisitor::DescriptorLiteral(const IrNode& node, std::string_view name,
                                         int32_t field_number, const Descriptor* declared_struct,
                                         std::string_view indent) {
    std::ostringstream o;
    o << indent << "{ name: '" << name << "', fieldNumber: " << field_number
      << ", wireType: " << WireType(node)
      << ", nullable: " << (node.facts.nullable ? "true" : "false");
    AppendComposite(o, node, declared_struct);
    o << " },\n";
    return o.str();
}

std::string TsVisitor::GenerateMessage(const Descriptor* msg) {
    // GIR-5 shared flatten walk: field-level flatten inlining + unsupported-field
    // drops, identical to the schema field set (never BuildMessageIr(msg).fields).
    const std::vector<cpp_backend::SchemaFieldRecord> fields =
        cpp_backend::BuildFlattenedFieldList(msg);
    if (fields.empty()) return "";  // Skip empty messages.

    const std::string iface = TsInterfaceName(msg);
    const std::string schema_name = TsSchemaConstName(msg);

    std::ostringstream o;

    o << "export interface " << iface << " {\n";
    for (const auto& rec : fields) {
        const Descriptor* declared = DeclaredWrapperFor(rec.source_field);
        std::string ts_type = InterfaceType(*rec.node, declared);
        if (rec.node->facts.nullable) ts_type += " | null";
        o << "  " << rec.name << ": " << ts_type << ";\n";
    }
    o << "}\n\n";

    o << "export const " << schema_name << ": TypedSchema<" << iface << "> = {\n"
      << "  fields: [\n";
    for (const auto& rec : fields) {
        const Descriptor* declared = DeclaredWrapperFor(rec.source_field);
        o << DescriptorLiteral(*rec.node, rec.name, rec.field_number, declared, "    ");
    }
    o << "  ],\n"
      << "  protoPackage: '" << msg->file()->package() << "',\n"
      << "  protoMessage: '" << msg->name() << "',\n"
      << "};\n\n";

    return o.str();
}

std::string TsVisitor::GenerateFile() {
    std::ostringstream o;

    o << "// Generated by fletcher-protoc. DO NOT EDIT.\n"
      << "// Source: " << file_->name() << "\n\n"
      << "import type { TypedSchema } from '@eiva/fletcher-gateway-client';\n"
      << "import { WireTypeId } from '@eiva/fletcher-gateway-client';\n\n";

    // Cross-file placeholder imports (GIR-8 owns real symbol resolution). Fully
    // IR-native: walk the shared flattened field list and inspect each IR node for
    // referenced struct message files — no flat-bridge projection. Ordered
    // lexically by the std::set, as before.
    std::set<std::string> ts_imports;
    auto messages = fletcher::OrderedMessages(file_);
    for (const auto* msg : messages) {
        if (fletcher::IsRecursive(msg)) continue;
        for (const auto& rec : cpp_backend::BuildFlattenedFieldList(msg))
            CollectStructImportFiles(*rec.node, file_, ts_imports);
    }
    for (const auto& proto_file : ts_imports) {
        o << "import { " << "/* cross-file types */ " << "} from './"
          << StripProtoSuffix(proto_file) << ".fletcher.js';\n";
    }
    if (!ts_imports.empty()) o << "\n";

    // Emit messages in dependency order. Unlike the C++ schema path, the TS walk
    // does NOT skip IsFlattenedWrapper messages — they get their own interface +
    // TypedSchema (StructListWrapper / NestedStructListWrapper in the fixture).
    for (const auto* msg : messages) {
        if (fletcher::IsRecursive(msg)) {
            o << "// Skipped: " << msg->name() << " is recursive and cannot be represented.\n\n";
            continue;
        }
        o << GenerateMessage(msg);
    }

    // Topic constants for service methods.
    std::set<const Descriptor*> generated_msgs(messages.begin(), messages.end());
    for (int si = 0; si < file_->service_count(); ++si) {
        const auto* svc = file_->service(si);
        for (int mi = 0; mi < svc->method_count(); ++mi) {
            const auto* method = svc->method(mi);
            std::string reason;
            if (!fletcher::ValidateServiceMethod(method, generated_msgs, &reason)) {
                o << "// Skipped: " << svc->name() << "." << method->name() << " — " << reason
                  << "\n";
                continue;
            }

            const std::string pkg = file_->package();
            std::string topic_path;
            if (!pkg.empty()) topic_path += DotToSlash(pkg) + "/";
            topic_path += svc->name() + "/" + method->name();

            o << "export const " << svc->name() << "_" << method->name() << "Topic = '"
              << topic_path << "';\n\n";
        }
    }

    return o.str();
}

}  // namespace fletcher::ts_backend
