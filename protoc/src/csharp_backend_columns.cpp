// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include "csharp_backend_columns.hpp"

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <variant>

#include "cpp_backend_schema_visitor.hpp"

namespace fletcher::csharp_backend {

using google::protobuf::Descriptor;
using google::protobuf::FileDescriptor;
using ir::IrNode;
using ir::NodeKind;

// the gap visible instead of dropping data silently.
const char* PendingKind(const IrNode& node) {
    switch (node.kind) {
        case NodeKind::LIST:
        case NodeKind::FIXED_SIZE_LIST:
            return "list";
        case NodeKind::STRUCT:
            return "struct";
        case NodeKind::MAP:
            return "map";
        case NodeKind::SCALAR:
            return "scalar";
        case NodeKind::UNSUPPORTED:
            return "unsupported";
    }
    return "unsupported";
}

namespace {

// The reason a message-typed element or field cannot convert, or empty when it can. A
// message from another file converts when its own class does (BIND-6e): that class is
// generated from its own file, by the same rules.
std::string StructBlocker(const ir::StructNode& s, const FileDescriptor* file,
                          const CrossFile& cross, std::set<const Descriptor*>& visiting) {
    if (s.identity.descriptor == nullptr) return "a message the IR does not identify";
    if (ConversionBlocker(s.identity.descriptor, file, cross, visiting).empty()) return {};
    return "a message whose own conversion waits";
}

std::optional<Elem> ElemOf(const IrNode& node, const FileDescriptor* file, const CrossFile& cross,
                           std::set<const Descriptor*>& visiting, std::string& why) {
    if (node.kind == NodeKind::STRUCT) {
        const auto& s = std::get<ir::StructNode>(node.node);
        why = StructBlocker(s, file, cross, visiting);
        if (!why.empty()) return std::nullopt;
        Elem e;
        e.is_struct = true;
        e.cls = CsTypeExpr(s.identity.descriptor);  // B2: named in full in method bodies
        e.other_file = cross.count(s.identity.descriptor) != 0;  // D-BIND-87
        return e;
    }
    if (node.kind == NodeKind::SCALAR) {
        const auto& s = std::get<ir::ScalarNode>(node.node);
        if (auto info = CsLookupScalar(s.logical_type, s.enum_identity, file)) {
            Elem e;
            e.scalar = *info;
            return e;
        }
    }
    if (node.kind == NodeKind::LIST) {
        auto inner = ElemOf(*std::get<ir::ListNode>(node.node).element, file, cross, visiting, why);
        if (!inner) return std::nullopt;
        Elem e;
        e.is_list = true;
        e.inner = std::make_shared<Elem>(std::move(*inner));
        return e;
    }
    why = std::string("a ") + PendingKind(node) + " element this backend cannot convert";
    return std::nullopt;
}

}  // namespace

// The column for `node`, or nullopt with `why` set.
std::optional<Column> ClassifyColumn(const IrNode& node, const FileDescriptor* file,
                                     const CrossFile& cross, std::set<const Descriptor*>& visiting,
                                     std::string& why) {
    Column c;
    c.nullable = node.facts.nullable;
    switch (node.kind) {
        case NodeKind::SCALAR: {
            const auto& s = std::get<ir::ScalarNode>(node.node);
            if (auto info = CsLookupScalar(s.logical_type, s.enum_identity, file)) {
                c.kind = ColKind::SCALAR;
                c.scalar = *info;
                return c;
            }
            if (s.logical_type.kind == ir::LogicalKind::WKT_TIMESTAMP ||
                s.logical_type.kind == ir::LogicalKind::WKT_DURATION) {
                c.kind = ColKind::TEMPORAL;
                c.is_timestamp = s.logical_type.kind == ir::LogicalKind::WKT_TIMESTAMP;
                return c;
            }
            why = "a scalar kind this backend does not map";
            return std::nullopt;
        }
        case NodeKind::STRUCT: {
            const auto& s = std::get<ir::StructNode>(node.node);
            why = StructBlocker(s, file, cross, visiting);
            if (!why.empty()) return std::nullopt;
            c.kind = ColKind::STRUCT;
            // As its property and its column: nullable only for `optional` (D-BIND-83).
            c.nullable = node.facts.nullable;
            c.cls = CsTypeExpr(s.identity.descriptor);  // B2: named in full in method bodies
            c.msg = s.identity.descriptor;
            c.other_file = cross.count(s.identity.descriptor) != 0;  // D-BIND-87
            return c;
        }
        case NodeKind::LIST: {
            auto e = ElemOf(*std::get<ir::ListNode>(node.node).element, file, cross, visiting, why);
            if (!e) return std::nullopt;
            c.kind = ColKind::LIST;
            c.elem = *e;
            return c;
        }
        case NodeKind::MAP: {
            const auto& m = std::get<ir::MapNode>(node.node);
            auto k = ElemOf(*m.key, file, cross, visiting, why);
            if (!k) return std::nullopt;
            auto v = ElemOf(*m.value, file, cross, visiting, why);
            if (!v) return std::nullopt;
            c.kind = ColKind::MAP;
            c.key = *k;
            c.elem = *v;
            return c;
        }
        case NodeKind::FIXED_SIZE_LIST:
        case NodeKind::UNSUPPORTED:
            break;
    }
    why = std::string("a ") + PendingKind(node) + " this backend does not map";
    return std::nullopt;
}

// The first reason `msg` gets no ToArrow / FromArrow, or empty when every column
// converts. A message already being walked counts as converting: recursive messages
// are skipped whole by GenerateFile, so the cycle never reaches emission.
std::string ConversionBlocker(const Descriptor* msg, const FileDescriptor* file,
                              const CrossFile& cross, std::set<const Descriptor*>& visiting) {
    if (!visiting.insert(msg).second) return {};
    std::string blocker;
    for (const auto& rec : cpp_backend::BuildFlattenedFieldList(msg)) {
        std::string why;
        if (!ClassifyColumn(*rec.node, file, cross, visiting, why)) {
            blocker = "field '" + rec.name + "' is " + why + ".";
            break;
        }
    }
    visiting.erase(msg);
    return blocker;
}

}  // namespace fletcher::csharp_backend
