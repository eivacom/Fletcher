// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include "ts_backend_visitor.hpp"

#include <map>
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

// The messages of OTHER files a field's emitted TypeScript names, by file (D-BIND-118). It
// walks `node` as InterfaceType and AppendComposite render it. A struct is named by reference
// (`I<Msg>` and `<Msg>.fields`), so its own fields are not walked: the imported file resolves
// those. WKT/wrapper scalars carry no struct identity and import nothing. Before D-BIND-118
// this kept file names only and the import listed no symbol.
void CollectCrossFileStructs(const IrNode& node,
                             const google::protobuf::FileDescriptor* context_file,
                             std::map<std::string, std::set<std::string>>& imports) {
    switch (node.kind) {
        case NodeKind::SCALAR:
        case NodeKind::UNSUPPORTED:
            return;
        case NodeKind::LIST:
        case NodeKind::FIXED_SIZE_LIST:
            CollectCrossFileStructs(*ListElement(node), context_file, imports);
            return;
        case NodeKind::STRUCT: {
            const Descriptor* d = StructIdentity(node);
            if (d->file() != context_file) imports[d->file()->name()].insert(TsSchemaConstName(d));
            return;
        }
        case NodeKind::MAP: {
            const auto& mp = std::get<ir::MapNode>(node.node);
            CollectCrossFileStructs(*mp.key, context_file, imports);
            CollectCrossFileStructs(*mp.value, context_file, imports);
            return;
        }
    }
}

// `a, b, c` with `prefix` before each name.
std::string JoinNames(const std::set<std::string>& names, std::string_view prefix) {
    std::string out;
    for (const auto& name : names) {
        if (!out.empty()) out += ", ";
        out += prefix;
        out += name;
    }
    return out;
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
            return TsInterfaceName(StructIdentity(node));
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
            o << ", fields: " << TsSchemaConstName(StructIdentity(node)) << ".fields";
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

    // Cross-file imports (D-BIND-118): each message of another file a field names, its
    // TypedSchema as a value (`<Msg>.fields`) and its interface as a type (`I<Msg>`), from
    // that file's generated module. Walks the shared flattened field list, IR-native.
    // Ordered lexically by file, then by name.
    std::map<std::string, std::set<std::string>> ts_imports;
    auto messages = fletcher::OrderedMessages(file_);
    for (const auto* msg : messages) {
        if (fletcher::IsRecursive(msg)) continue;
        for (const auto& rec : cpp_backend::BuildFlattenedFieldList(msg))
            CollectCrossFileStructs(*rec.node, file_, ts_imports);
    }
    for (const auto& [proto_file, names] : ts_imports) {
        const std::string module = "'./" + StripProtoSuffix(proto_file) + ".fletcher.js'";
        o << "import { " << JoinNames(names, "") << " } from " << module << ";\n"
          << "import type { " << JoinNames(names, "I") << " } from " << module << ";\n";
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
