// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include "ts_backend_visitor.hpp"

#include <cctype>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "cpp_backend_schema_visitor.hpp"
#include "generator_internal.hpp"
#include "ts_backend_type_table.hpp"
#include "type_mapper.hpp"

namespace fletcher::ts_backend {

namespace {

using google::protobuf::Descriptor;
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

const IrNode* ListElement(const IrNode& node) {
    if (node.kind == NodeKind::LIST) return std::get<ir::ListNode>(node.node).element.get();
    return std::get<ir::FixedSizeListNode>(node.node).element.get();
}

const Descriptor* StructIdentity(const IrNode& node) {
    return std::get<ir::StructNode>(node.node).identity.descriptor;
}

// The OTHER files whose messages a field's emitted TypeScript names (D-BIND-118, D-BIND-122). It
// walks `node` as InterfaceType and AppendComposite render it. A struct is named by reference
// (`<ns>.I<Msg>` and `<ns>.<Msg>.fields`), so its own fields are not walked: the imported file
// resolves those. WKT/wrapper scalars carry no struct identity and import nothing.
void CollectImportedFiles(const IrNode& node, const google::protobuf::FileDescriptor* context_file,
                          std::set<std::string>& files) {
    switch (node.kind) {
        case NodeKind::SCALAR:
        case NodeKind::UNSUPPORTED:
            return;
        case NodeKind::LIST:
        case NodeKind::FIXED_SIZE_LIST:
            CollectImportedFiles(*ListElement(node), context_file, files);
            return;
        case NodeKind::STRUCT: {
            const Descriptor* d = StructIdentity(node);
            if (d->file() != context_file) files.insert(d->file()->name());
            return;
        }
        case NodeKind::MAP: {
            const auto& mp = std::get<ir::MapNode>(node.node);
            CollectImportedFiles(*mp.key, context_file, files);
            CollectImportedFiles(*mp.value, context_file, files);
            return;
        }
    }
}

// The namespace another file is imported as (D-BIND-122): its path without `.proto`, every
// character that is not a letter or digit made `_`, then `_fletcher` (`dir/dep.proto` ->
// `dir_dep_fletcher`). One namespace per file, so no imported name can clash with a local one,
// another file's, or the runtime's `TypedSchema` / `WireTypeId`.
std::string NamespaceAlias(const std::string& proto_file) {
    std::string alias = StripProtoSuffix(proto_file);
    for (char& c : alias) {
        if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
    }
    return alias + "_fletcher";
}

// The module specifier of `to`'s generated file as `from`'s generated file imports it: relative
// to `from`'s directory (review B1), since each `.fletcher.ts` is written at its `.proto`'s path.
std::string RelativeModulePath(const std::string& from, const std::string& to) {
    auto segments = [](const std::string& path) {
        std::vector<std::string> out;
        std::string part;
        for (char c : path) {
            if (c == '/') {
                out.push_back(part);
                part.clear();
            } else {
                part += c;
            }
        }
        out.push_back(part);
        return out;
    };
    std::vector<std::string> from_dir = segments(from);
    from_dir.pop_back();  // the importing file's own name
    const std::vector<std::string> target = segments(StripProtoSuffix(to) + ".fletcher.js");

    size_t common = 0;
    while (common < from_dir.size() && common + 1 < target.size() &&
           from_dir[common] == target[common]) {
        ++common;
    }
    std::string out;
    for (size_t i = common; i < from_dir.size(); ++i) out += "../";
    if (out.empty()) out = "./";
    for (size_t i = common; i < target.size(); ++i) {
        out += target[i];
        if (i + 1 < target.size()) out += '/';
    }
    return out;
}

}  // namespace

TsVisitor::TsVisitor(const google::protobuf::FileDescriptor* file,
                     std::map<const Descriptor*, std::string> schema_ipc)
    : file_(file), schema_ipc_(std::move(schema_ipc)) {}

std::string TsVisitor::SchemaName(const Descriptor* msg) const {
    if (msg->file() == file_) return TsSchemaConstName(msg);
    return NamespaceAlias(msg->file()->name()) + "." + TsSchemaConstName(msg);
}

std::string TsVisitor::InterfaceName(const Descriptor* msg) const {
    if (msg->file() == file_) return TsInterfaceName(msg);
    return NamespaceAlias(msg->file()->name()) + "." + TsInterfaceName(msg);
}

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

// A struct is always rendered by its IR identity, the message the wire carries (D-BIND-119).
// A singular flatten wrapper (`StructListWrapper foo = N;`) is List<Struct(Leaf)> in the IR and
// on the wire, so it is ILeaf[] / Leaf.fields; naming the wrapper there (IStructListWrapper[] /
// StructListWrapper.fields, as GIR-7 did to keep the pre-IR output) described a list of the
// WRAPPER, which no other language writes or reads.
std::string TsVisitor::InterfaceType(const IrNode& node) {
    switch (node.kind) {
        case NodeKind::SCALAR: {
            const auto& s = std::get<ir::ScalarNode>(node.node);
            return TsLookupScalar(s.logical_type, s.enum_identity).ts_type_text;
        }
        case NodeKind::LIST:
        case NodeKind::FIXED_SIZE_LIST:
            return InterfaceType(*ListElement(node)) + "[]";
        case NodeKind::STRUCT:
            return InterfaceName(StructIdentity(node));
        case NodeKind::MAP: {
            const auto& mp = std::get<ir::MapNode>(node.node);
            return "Map<" + InterfaceType(*mp.key) + ", " + InterfaceType(*mp.value) + ">";
        }
        case NodeKind::UNSUPPORTED:
            return "unknown";
    }
    return "unknown";
}

void TsVisitor::AppendComposite(std::ostringstream& o, const IrNode& node) {
    switch (node.kind) {
        case NodeKind::SCALAR:
        case NodeKind::UNSUPPORTED:
            return;
        case NodeKind::LIST:
        case NodeKind::FIXED_SIZE_LIST:
            o << ", element: " << ElementDescriptor(*ListElement(node));
            return;
        case NodeKind::STRUCT:
            o << ", fields: " << SchemaName(StructIdentity(node)) << ".fields";
            return;
        case NodeKind::MAP: {
            const auto& mp = std::get<ir::MapNode>(node.node);
            o << ", mapKey: " << ElementDescriptor(*mp.key)
              << ", mapValue: " << ElementDescriptor(*mp.value);
            return;
        }
    }
}

std::string TsVisitor::ElementDescriptor(const IrNode& node) {
    std::ostringstream o;
    o << "{ name: '', fieldNumber: 0, wireType: " << WireType(node) << ", nullable: false";
    AppendComposite(o, node);
    o << " }";
    return o.str();
}

std::string TsVisitor::DescriptorLiteral(const IrNode& node, std::string_view name,
                                         int32_t field_number, std::string_view indent) {
    std::ostringstream o;
    o << indent << "{ name: '" << name << "', fieldNumber: " << field_number
      << ", wireType: " << WireType(node)
      << ", nullable: " << (node.facts.nullable ? "true" : "false");
    AppendComposite(o, node);
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
        std::string ts_type = InterfaceType(*rec.node);
        if (rec.node->facts.nullable) ts_type += " | null";
        o << "  " << rec.name << ": " << ts_type << ";\n";
    }
    o << "}\n\n";

    o << "export const " << schema_name << ": TypedSchema<" << iface << "> = {\n"
      << "  fields: [\n";
    for (const auto& rec : fields) {
        o << DescriptorLiteral(*rec.node, rec.name, rec.field_number, "    ");
    }
    o << "  ],\n"
      << "  protoPackage: '" << msg->file()->package() << "',\n"
      << "  protoMessage: '" << msg->name() << "',\n";
    if (const auto ipc = schema_ipc_.find(msg); ipc != schema_ipc_.end()) {
        // D-BIND-130: the native declaration's own Arrow IPC bytes, so createTopic announces
        // the schema with full fidelity.
        o << "  schemaIpc: '" << ipc->second << "',\n";
    }
    o << "};\n\n";

    return o.str();
}

std::string TsVisitor::GenerateFile() {
    std::ostringstream o;

    o << "// Generated by fletcher-protoc. DO NOT EDIT.\n"
      << "// Source: " << file_->name() << "\n\n"
      << "import type { TypedSchema } from '@eiva/fletcher-gateway-client';\n"
      << "import { WireTypeId } from '@eiva/fletcher-gateway-client';\n\n";

    // Cross-file imports (D-BIND-118, D-BIND-122): each other file whose messages a field
    // names, as ONE namespace, from its generated module by a path relative to this file.
    // Walks the shared flattened field list, IR-native. Ordered lexically by file.
    std::set<std::string> imported;
    auto messages = fletcher::OrderedMessages(file_);
    for (const auto* msg : messages) {
        if (fletcher::IsRecursive(msg)) continue;
        for (const auto& rec : cpp_backend::BuildFlattenedFieldList(msg))
            CollectImportedFiles(*rec.node, file_, imported);
    }
    for (const auto& proto_file : imported) {
        o << "import * as " << NamespaceAlias(proto_file) << " from '"
          << RelativeModulePath(file_->name(), proto_file) << "';\n";
    }
    if (!imported.empty()) o << "\n";

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

            // The package is ONE segment, dots kept, as in the C++ TopicSegments() and
            // TopicKey(): the gateway splits this string on '/', so turning dots into
            // slashes would name a different topic than a C++ publisher of the same
            // method (TopicPath.DottedPackageAgreesAcrossBackends).
            const std::string pkg = file_->package();
            std::string topic_path;
            if (!pkg.empty()) topic_path += pkg + "/";
            topic_path += svc->name() + "/" + method->name();

            o << "export const " << svc->name() << "_" << method->name() << "Topic = '"
              << topic_path << "';\n\n";
        }
    }

    return o.str();
}

}  // namespace fletcher::ts_backend
