// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include "type_mapper.hpp"

#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/unknown_field_set.h>

#include <algorithm>
#include <set>
#include <string>
#include <variant>

#include "cpp_backend_type_table.hpp"
#include "ir.hpp"

namespace fletcher {

// Single source of truth for the dotted-path → C++ namespace-path transform
// (declared in type_mapper.hpp). Shared by the row generator and the accessor
// emitter; replaces the former per-TU DotToColons / DotToColonsTM /
// PackageToNamespace copies.
std::string DotToColons(const std::string& s) {
    std::string out;
    out.reserve(s.size() +
                2 * static_cast<std::string::size_type>(std::count(s.begin(), s.end(), '.')));
    for (char c : s) {
        if (c == '.')
            out += "::";
        else
            out += c;
    }
    return out;
}

// -----------------------------------------------------------------------
// Custom option helpers (visible to both anonymous namespace and public API)
// -----------------------------------------------------------------------

using FD = google::protobuf::FieldDescriptor;

constexpr int kFlattenOptionNumber = 50000;

static bool FindBoolOption(const google::protobuf::Message& opts, int number) {
    const auto& unknown = opts.GetReflection()->GetUnknownFields(opts);
    for (int i = 0; i < unknown.field_count(); ++i) {
        const auto& f = unknown.field(i);
        if (f.number() == number && f.type() == google::protobuf::UnknownField::TYPE_VARINT)
            return f.varint() != 0;
    }
    return false;
}

// Public (declared in type_mapper.hpp): the IR classifier in ir.cpp reads it too.
bool HasMessageFlatten(const google::protobuf::Descriptor* msg) {
    return FindBoolOption(msg->options(), kFlattenOptionNumber);
}

// Public (declared in type_mapper.hpp): shared by the anonymous-namespace bridge
// helpers below and by the IR classifier in ir.cpp.
bool IsFieldNullable(const google::protobuf::FieldDescriptor* field) {
    if (field->has_optional_keyword()) return true;
    if (field->file()->syntax() == google::protobuf::FileDescriptor::SYNTAX_PROTO2 &&
        field->label() == FD::LABEL_OPTIONAL)
        return true;
    return false;
}

namespace {
// -----------------------------------------------------------------------
// Recursion detection
// -----------------------------------------------------------------------

bool IsRecursiveImpl(const google::protobuf::Descriptor* msg,
                     std::set<const google::protobuf::Descriptor*>& stack) {
    if (stack.count(msg)) return true;
    stack.insert(msg);
    for (int i = 0; i < msg->field_count(); ++i) {
        const auto* f = msg->field(i);
        if (f->type() != FD::TYPE_MESSAGE) continue;
        if (f->is_map()) {
            // Only the value type can introduce a cycle.
            const auto* val_field = f->message_type()->field(1);
            if (val_field->type() == FD::TYPE_MESSAGE &&
                IsRecursiveImpl(val_field->message_type(), stack))
                return true;
        } else {
            if (IsRecursiveImpl(f->message_type(), stack)) return true;
        }
    }
    stack.erase(msg);
    return false;
}

int NestingDepthImpl(const google::protobuf::Descriptor* msg,
                     std::set<const google::protobuf::Descriptor*>& visited) {
    if (visited.count(msg)) return 0;  // cycle — handled by IsRecursive
    visited.insert(msg);
    int max_d = 0;
    for (int i = 0; i < msg->field_count(); ++i) {
        const auto* f = msg->field(i);
        if (f->type() != FD::TYPE_MESSAGE || f->is_map()) continue;
        max_d = std::max(max_d, 1 + NestingDepthImpl(f->message_type(), visited));
    }
    visited.erase(msg);
    return max_d;
}

}  // namespace

// -----------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------

// GIR-3: thin bridge over the single canonical classifier. MapField() no longer
// classifies independently — it builds the language-neutral IR and projects it.
// RBA / decode / schema / view / TS all consume FieldMapping derived from this
// same BuildFieldIr() source, so they cannot silently drift.
std::optional<FieldMapping> MapField(const google::protobuf::FieldDescriptor* field) {
    return ProjectIrToFieldMapping(ir::BuildFieldIr(field), field->file());
}

// -----------------------------------------------------------------------
// Canonical IR -> FieldMapping projection (single-source bridge)
// -----------------------------------------------------------------------

namespace {

ScalarTypeInfo ToScalarTypeInfo(const cpp_backend::CppScalarInfo& c) {
    ScalarTypeInfo s;
    s.arrow_type_expr = c.arrow_type_expr;
    s.storage_type = c.storage_type;
    s.param_type = c.param_type;
    s.scalar_ctor = c.scalar_ctor;
    s.default_value = c.default_value;
    s.builder_type = c.builder_type;
    s.scalar_type = c.scalar_type;
    s.value_is_buffer = c.value_is_buffer;
    return s;
}

}  // namespace

std::optional<FieldMapping> ProjectIrToFieldMapping(
    const ir::IrNode& node, const google::protobuf::FileDescriptor* context_file) {
    using ir::IrNode;
    using ir::NodeKind;

    switch (node.kind) {
        case NodeKind::UNSUPPORTED:
        case NodeKind::FIXED_SIZE_LIST:
            return std::nullopt;

        case NodeKind::SCALAR: {
            const auto& s = std::get<ir::ScalarNode>(node.node);
            FieldMapping m{};
            m.kind = FieldKind::SCALAR;
            m.nullable = node.facts.nullable;
            m.warning = node.facts.warning;
            m.scalar = ToScalarTypeInfo(cpp_backend::LookupScalar(s.logical_type, s.enum_identity));
            return m;
        }

        case NodeKind::STRUCT: {
            const auto& st = std::get<ir::StructNode>(node.node);
            FieldMapping m{};
            m.kind = FieldKind::STRUCT;
            m.nullable = node.facts.nullable;
            m.warning = node.facts.warning;
            m.nested_class = cpp_backend::CppClassName(st.identity.descriptor, context_file);
            m.nested_header = cpp_backend::CppCrossFileHeader(st.identity.descriptor, context_file);
            m.nested_msg = st.identity.descriptor;
            return m;
        }

        case NodeKind::LIST: {
            const IrNode& elem = *std::get<ir::ListNode>(node.node).element;

            if (elem.kind == NodeKind::SCALAR) {
                const auto& s = std::get<ir::ScalarNode>(elem.node);
                FieldMapping m{};
                m.kind = FieldKind::REPEATED_SCALAR;
                m.nullable = node.facts.nullable;
                m.warning = node.facts.warning;
                m.element =
                    ToScalarTypeInfo(cpp_backend::LookupScalar(s.logical_type, s.enum_identity));
                return m;
            }
            if (elem.kind == NodeKind::STRUCT) {
                const auto& st = std::get<ir::StructNode>(elem.node);
                FieldMapping m{};
                m.kind = FieldKind::REPEATED_STRUCT;
                m.nullable = node.facts.nullable;
                m.warning = node.facts.warning;
                m.nested_class = cpp_backend::CppClassName(st.identity.descriptor, context_file);
                m.nested_header =
                    cpp_backend::CppCrossFileHeader(st.identity.descriptor, context_file);
                m.nested_msg = st.identity.descriptor;
                return m;
            }
            if (elem.kind == NodeKind::LIST) {
                // Count nesting depth until the leaf. The flat bridge only models
                // struct-leaf nested lists (List<List<...<Struct>>>); a scalar leaf
                // (List<List<Scalar>>) is not representable here — explicit nullopt.
                int depth = 1;
                const IrNode* cur = &elem;
                while (cur->kind == NodeKind::LIST) {
                    depth += 1;
                    cur = std::get<ir::ListNode>(cur->node).element.get();
                }
                if (cur->kind != NodeKind::STRUCT) return std::nullopt;
                const auto& st = std::get<ir::StructNode>(cur->node);
                FieldMapping m{};
                m.kind = FieldKind::NESTED_LIST;
                m.nullable = node.facts.nullable;
                m.warning = node.facts.warning;
                m.list_depth = depth;
                m.nested_class = cpp_backend::CppClassName(st.identity.descriptor, context_file);
                m.nested_header =
                    cpp_backend::CppCrossFileHeader(st.identity.descriptor, context_file);
                m.nested_msg = st.identity.descriptor;
                return m;
            }
            return std::nullopt;
        }

        case NodeKind::MAP: {
            const auto& mp = std::get<ir::MapNode>(node.node);
            if (mp.key->kind != NodeKind::SCALAR) return std::nullopt;
            const IrNode& v = *mp.value;

            FieldMapping m{};
            m.kind = FieldKind::MAP;
            m.nullable = node.facts.nullable;
            m.warning = node.facts.warning;
            const auto& k = std::get<ir::ScalarNode>(mp.key->node);
            m.map_key = ToScalarTypeInfo(cpp_backend::LookupScalar(k.logical_type, k.enum_identity));

            if (v.kind == NodeKind::STRUCT) {
                const auto& vst = std::get<ir::StructNode>(v.node);
                m.map_value_is_message = true;
                m.map_value_class = cpp_backend::CppClassName(vst.identity.descriptor, context_file);
                m.map_value_header =
                    cpp_backend::CppCrossFileHeader(vst.identity.descriptor, context_file);
                m.map_value_msg = vst.identity.descriptor;
            } else if (v.kind == NodeKind::SCALAR) {
                const auto& vs = std::get<ir::ScalarNode>(v.node);
                m.map_value_is_message = false;
                m.map_value =
                    ToScalarTypeInfo(cpp_backend::LookupScalar(vs.logical_type, vs.enum_identity));
            } else {
                return std::nullopt;
            }
            return m;
        }
    }
    return std::nullopt;
}

std::string UnsupportedReason(const google::protobuf::FieldDescriptor* field) {
    if (field->real_containing_oneof())
        return "oneof '" + field->real_containing_oneof()->name() +
               "' cannot be mapped to a Parquet-safe Arrow type; "
               "consider using separate optional fields instead";

    if (field->type() == FD::TYPE_MESSAGE) {
        const auto* msg = field->message_type();
        const std::string& fqn = msg->full_name();

        if (fqn == "google.protobuf.Any")
            return "google.protobuf.Any is dynamically typed and has no static Arrow mapping";
        if (fqn == "google.protobuf.Struct")
            return "google.protobuf.Struct has a dynamic schema and cannot be mapped to Arrow";

        if (IsRecursive(msg))
            return "message '" + fqn + "' is recursive and cannot be represented in Arrow";
    }

    if (field->type() == FD::TYPE_GROUP) return "proto2 groups are not supported";

    return "unsupported proto field type";
}

bool IsRecursive(const google::protobuf::Descriptor* msg) {
    std::set<const google::protobuf::Descriptor*> stack;
    return IsRecursiveImpl(msg, stack);
}

bool IsFlattenedWrapper(const google::protobuf::Descriptor* msg) {
    return HasMessageFlatten(msg) && msg->field_count() == 1;
}

bool HasFieldFlatten(const google::protobuf::FieldDescriptor* field) {
    return FindBoolOption(field->options(), kFlattenOptionNumber);
}

int NestingDepth(const google::protobuf::Descriptor* msg) {
    std::set<const google::protobuf::Descriptor*> visited;
    return NestingDepthImpl(msg, visited);
}

std::string ClassName(const google::protobuf::Descriptor* msg) {
    std::string name = msg->name();
    const auto* parent = msg->containing_type();
    while (parent) {
        name = parent->name() + "_" + name;
        parent = parent->containing_type();
    }
    return name;
}

std::string ViewClassName(const google::protobuf::Descriptor* msg) {
    return ClassName(msg) + "View";
}

// -----------------------------------------------------------------------
// TypeScript code generation helpers
// -----------------------------------------------------------------------

std::string TsScalarType(google::protobuf::FieldDescriptor::Type type) {
    using FDT = google::protobuf::FieldDescriptor;
    switch (type) {
        case FDT::TYPE_BOOL:
            return "boolean";
        case FDT::TYPE_INT32:
        case FDT::TYPE_SINT32:
        case FDT::TYPE_SFIXED32:
            return "number";
        case FDT::TYPE_INT64:
        case FDT::TYPE_SINT64:
        case FDT::TYPE_SFIXED64:
            return "bigint";
        case FDT::TYPE_UINT32:
        case FDT::TYPE_FIXED32:
            return "number";
        case FDT::TYPE_UINT64:
        case FDT::TYPE_FIXED64:
            return "bigint";
        case FDT::TYPE_FLOAT:
            return "number";
        case FDT::TYPE_DOUBLE:
            return "number";
        case FDT::TYPE_STRING:
            return "string";
        case FDT::TYPE_BYTES:
            return "Uint8Array";
        case FDT::TYPE_ENUM:
            return "number";
        default:
            return "";
    }
}

// -----------------------------------------------------------------------
// C++ wire format helpers
// -----------------------------------------------------------------------

std::string CppWireTypeIdHex(google::protobuf::FieldDescriptor::Type type) {
    using FDT = google::protobuf::FieldDescriptor;
    switch (type) {
        case FDT::TYPE_BOOL:
            return "0x01";
        case FDT::TYPE_INT32:
        case FDT::TYPE_SINT32:
        case FDT::TYPE_SFIXED32:
            return "0x04";
        case FDT::TYPE_INT64:
        case FDT::TYPE_SINT64:
        case FDT::TYPE_SFIXED64:
            return "0x05";
        case FDT::TYPE_UINT32:
        case FDT::TYPE_FIXED32:
            return "0x08";
        case FDT::TYPE_UINT64:
        case FDT::TYPE_FIXED64:
            return "0x09";
        case FDT::TYPE_FLOAT:
            return "0x0A";
        case FDT::TYPE_DOUBLE:
            return "0x0B";
        case FDT::TYPE_STRING:
            return "0x0C";
        case FDT::TYPE_BYTES:
            return "0x0D";
        case FDT::TYPE_ENUM:
            return "0x04";  // enums map to INT32
        default:
            return "";
    }
}

std::string CppWireTypeIdHex(FieldKind kind) {
    switch (kind) {
        case FieldKind::STRUCT:
            return "0x20";
        case FieldKind::REPEATED_SCALAR:
        case FieldKind::REPEATED_STRUCT:
            return "0x21";
        case FieldKind::MAP:
            return "0x24";
        default:
            return "";
    }
}

std::string WireTypeIdName(google::protobuf::FieldDescriptor::Type type) {
    using FDT = google::protobuf::FieldDescriptor;
    switch (type) {
        case FDT::TYPE_BOOL:
            return "WireTypeId.BOOL";
        case FDT::TYPE_INT32:
        case FDT::TYPE_SINT32:
        case FDT::TYPE_SFIXED32:
            return "WireTypeId.INT32";
        case FDT::TYPE_INT64:
        case FDT::TYPE_SINT64:
        case FDT::TYPE_SFIXED64:
            return "WireTypeId.INT64";
        case FDT::TYPE_UINT32:
        case FDT::TYPE_FIXED32:
            return "WireTypeId.UINT32";
        case FDT::TYPE_UINT64:
        case FDT::TYPE_FIXED64:
            return "WireTypeId.UINT64";
        case FDT::TYPE_FLOAT:
            return "WireTypeId.FLOAT32";
        case FDT::TYPE_DOUBLE:
            return "WireTypeId.FLOAT64";
        case FDT::TYPE_STRING:
            return "WireTypeId.STRING";
        case FDT::TYPE_BYTES:
            return "WireTypeId.BINARY";
        case FDT::TYPE_ENUM:
            return "WireTypeId.INT32";
        default:
            return "";
    }
}

std::string TsInterfaceName(const google::protobuf::Descriptor* msg) {
    std::string name = msg->name();
    const auto* parent = msg->containing_type();
    while (parent) {
        name = parent->name() + "_" + name;
        parent = parent->containing_type();
    }
    return "I" + name;
}

}  // namespace fletcher
