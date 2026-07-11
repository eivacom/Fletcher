// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include "ir.hpp"

#include <memory>
#include <optional>
#include <string>

#include "type_mapper.hpp"

namespace fletcher::ir {

using FD = google::protobuf::FieldDescriptor;
using google::protobuf::Descriptor;

namespace {

// ---------------------------------------------------------------------------
// Facts + small node builders
// ---------------------------------------------------------------------------

FieldFacts BaseFacts(const FD* field) {
    FieldFacts f;
    f.field_descriptor = field;
    f.containing_message = field->containing_type();
    f.proto_name = field->name();
    f.proto_full_name = field->full_name();
    f.wire_field_id = field->number();
    f.repeated = field->is_repeated();
    f.map_entry = field->is_map();
    f.in_real_oneof = field->real_containing_oneof() != nullptr;
    const bool proto2 =
        field->file()->syntax() == google::protobuf::FileDescriptor::SYNTAX_PROTO2;
    f.proto2_optional = proto2 && field->label() == FD::LABEL_OPTIONAL;
    f.proto3_optional = !proto2 && field->has_optional_keyword();
    return f;
}

IrNode MakeNode(NodeKind kind) {
    IrNode n;
    n.kind = kind;
    return n;
}

IrNode MakeUnsupported(const FD* field, const std::string& reason) {
    IrNode n = MakeNode(NodeKind::UNSUPPORTED);
    n.facts = BaseFacts(field);
    n.node = UnsupportedNode{reason};
    return n;
}

// The dynamically-typed well-known messages that have no static Arrow mapping.
// Detected identically wherever the message type appears — as a singular field,
// a repeated element, or a map value — so classification (and the GIR-8 #55
// front-end validation that reads it) rejects them consistently (review 4b).
// Returns the abstract UnsupportedNode reason, or nullopt for a mappable message.
std::optional<std::string> DynamicWktUnsupportedReason(const Descriptor* msg) {
    const std::string& fqn = msg->full_name();
    if (fqn == "google.protobuf.Any")
        return "google.protobuf.Any is dynamically typed and has no static Arrow mapping";
    if (fqn == "google.protobuf.Struct")
        return "google.protobuf.Struct has a dynamic schema and cannot be mapped to Arrow";
    return std::nullopt;
}

// Physical/logical kind for a proto primitive (or enum, which lowers to INT32).
LogicalKind PrimitiveKind(FD::Type t) {
    switch (t) {
        case FD::TYPE_BOOL:
            return LogicalKind::BOOL;
        case FD::TYPE_INT32:
        case FD::TYPE_SINT32:
        case FD::TYPE_SFIXED32:
            return LogicalKind::INT32;
        case FD::TYPE_INT64:
        case FD::TYPE_SINT64:
        case FD::TYPE_SFIXED64:
            return LogicalKind::INT64;
        case FD::TYPE_UINT32:
        case FD::TYPE_FIXED32:
            return LogicalKind::UINT32;
        case FD::TYPE_UINT64:
        case FD::TYPE_FIXED64:
            return LogicalKind::UINT64;
        case FD::TYPE_FLOAT:
            return LogicalKind::FLOAT32;
        case FD::TYPE_DOUBLE:
            return LogicalKind::FLOAT64;
        case FD::TYPE_STRING:
            return LogicalKind::UTF8;
        case FD::TYPE_BYTES:
            return LogicalKind::BINARY;
        case FD::TYPE_ENUM:
            return LogicalKind::INT32;
        default:
            return LogicalKind::INT32;  // unreachable for callers below
    }
}

bool IsSupportedScalarType(FD::Type t) {
    switch (t) {
        case FD::TYPE_BOOL:
        case FD::TYPE_INT32:
        case FD::TYPE_SINT32:
        case FD::TYPE_SFIXED32:
        case FD::TYPE_INT64:
        case FD::TYPE_SINT64:
        case FD::TYPE_SFIXED64:
        case FD::TYPE_UINT32:
        case FD::TYPE_FIXED32:
        case FD::TYPE_UINT64:
        case FD::TYPE_FIXED64:
        case FD::TYPE_FLOAT:
        case FD::TYPE_DOUBLE:
        case FD::TYPE_STRING:
        case FD::TYPE_BYTES:
        case FD::TYPE_ENUM:
            return true;
        default:
            return false;
    }
}

EnumIdentity BuildEnumIdentity(const google::protobuf::EnumDescriptor* ed) {
    EnumIdentity id;
    id.descriptor = ed;
    id.full_name = ed->full_name();
    for (int i = 0; i < ed->value_count(); ++i)
        id.symbols.push_back({ed->value(i)->name(), ed->value(i)->number()});
    return id;
}

LogicalType SimpleLogical(LogicalKind k) {
    return LogicalType{k, std::nullopt, std::nullopt, std::nullopt,
                       std::nullopt, std::nullopt, std::nullopt};
}

// SCALAR node for a scalar/enum leaf (no top-level facts — caller sets facts).
ScalarNode ScalarVariantFor(const FD* leaf) {
    ScalarNode s;
    if (leaf->type() == FD::TYPE_ENUM) {
        s.logical_type = SimpleLogical(LogicalKind::INT32);
        s.enum_identity = BuildEnumIdentity(leaf->enum_type());
    } else {
        s.logical_type = SimpleLogical(PrimitiveKind(leaf->type()));
    }
    return s;
}

IrNode MakeListOf(IrNode element) {
    IrNode n = MakeNode(NodeKind::LIST);
    ListNode l;
    l.element = std::make_unique<IrNode>(std::move(element));
    n.node = std::move(l);
    return n;
}

// Full Struct node (identity + recursively-built children).
StructNode BuildStructVariant(const Descriptor* msg);

IrNode MakeStructNode(const Descriptor* msg) {
    IrNode n = MakeNode(NodeKind::STRUCT);
    n.node = BuildStructVariant(msg);
    return n;
}

// ---------------------------------------------------------------------------
// Well-known types
// ---------------------------------------------------------------------------

struct WrapperInfo {
    LogicalKind kind;
    WktKind wkt;
};

const WrapperInfo* WrapperFor(const std::string& fqn) {
    static const WrapperInfo kBool{LogicalKind::BOOL, WktKind::WRAPPER_BOOL};
    static const WrapperInfo kI32{LogicalKind::INT32, WktKind::WRAPPER_INT32};
    static const WrapperInfo kI64{LogicalKind::INT64, WktKind::WRAPPER_INT64};
    static const WrapperInfo kU32{LogicalKind::UINT32, WktKind::WRAPPER_UINT32};
    static const WrapperInfo kU64{LogicalKind::UINT64, WktKind::WRAPPER_UINT64};
    static const WrapperInfo kF{LogicalKind::FLOAT32, WktKind::WRAPPER_FLOAT};
    static const WrapperInfo kD{LogicalKind::FLOAT64, WktKind::WRAPPER_DOUBLE};
    static const WrapperInfo kS{LogicalKind::UTF8, WktKind::WRAPPER_STRING};
    static const WrapperInfo kB{LogicalKind::BINARY, WktKind::WRAPPER_BYTES};
    if (fqn == "google.protobuf.BoolValue") return &kBool;
    if (fqn == "google.protobuf.Int32Value") return &kI32;
    if (fqn == "google.protobuf.Int64Value") return &kI64;
    if (fqn == "google.protobuf.UInt32Value") return &kU32;
    if (fqn == "google.protobuf.UInt64Value") return &kU64;
    if (fqn == "google.protobuf.FloatValue") return &kF;
    if (fqn == "google.protobuf.DoubleValue") return &kD;
    if (fqn == "google.protobuf.StringValue") return &kS;
    if (fqn == "google.protobuf.BytesValue") return &kB;
    return nullptr;
}

// Returns a scalar IR node for a singular WKT message field, or nullopt if the
// message is not a WKT the mapper recognises.
std::optional<IrNode> TryBuildWkt(const FD* field) {
    const std::string& fqn = field->message_type()->full_name();

    if (fqn == "google.protobuf.Timestamp") {
        IrNode n = MakeNode(NodeKind::SCALAR);
        n.facts = BaseFacts(field);
        n.facts.nullable = IsFieldNullable(field);
        n.facts.wkt = WktKind::TIMESTAMP;
        ScalarNode s;
        s.logical_type = LogicalType{LogicalKind::WKT_TIMESTAMP, std::nullopt, TimeUnit::NANO,
                                     std::nullopt, std::nullopt, std::nullopt, std::nullopt};
        n.node = std::move(s);
        return n;
    }
    if (fqn == "google.protobuf.Duration") {
        IrNode n = MakeNode(NodeKind::SCALAR);
        n.facts = BaseFacts(field);
        n.facts.nullable = IsFieldNullable(field);
        n.facts.wkt = WktKind::DURATION;
        ScalarNode s;
        s.logical_type = LogicalType{LogicalKind::WKT_DURATION, std::nullopt, TimeUnit::NANO,
                                     std::nullopt, std::nullopt, std::nullopt, std::nullopt};
        n.node = std::move(s);
        return n;
    }
    if (const WrapperInfo* w = WrapperFor(fqn)) {
        IrNode n = MakeNode(NodeKind::SCALAR);
        n.facts = BaseFacts(field);
        n.facts.nullable = true;  // wrappers exist to express nullable T
        n.facts.wkt = w->wkt;
        ScalarNode s;
        s.logical_type = SimpleLogical(w->kind);
        n.node = std::move(s);
        return n;
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Flatten resolution (message-level (fletcher.flatten))
// ---------------------------------------------------------------------------

std::string FlattenIgnoredWarning(const Descriptor* msg) {
    return "(fletcher.flatten) ignored on " + msg->full_name() + " (" +
           std::to_string(msg->field_count()) +
           " fields); apply flatten to individual fields instead";
}

// Singular message field whose target has (fletcher.flatten): resolve through
// the wrapper and return the inner field's IR, propagating the outer field's
// nullable when set. Mirrors type_mapper's MapFlattenedSingular.
IrNode BuildFlattenedSingular(const FD* field) {
    const Descriptor* msg = field->message_type();

    if (msg->field_count() != 1) {
        IrNode n = MakeStructNode(msg);
        n.facts = BaseFacts(field);
        n.facts.nullable = IsFieldNullable(field);
        n.facts.warning = FlattenIgnoredWarning(msg);
        return n;
    }

    const FD* inner = msg->field(0);
    IrNode inner_ir = BuildFieldIr(inner);
    if (IsFieldNullable(field)) inner_ir.facts.nullable = true;
    return inner_ir;
}

// Repeated message field whose target has (fletcher.flatten): walk the flatten
// chain, wrapping the leaf in one List level per chain hop plus the caller's own
// `repeated`. Mirrors type_mapper's MapFlattenedRepeated (including its current
// treatment of scalar leaves: depth-0 collapses to List<Scalar>, depth>0 is
// Unsupported until GIR-10).
IrNode BuildFlattenedRepeated(const FD* field) {
    const Descriptor* msg = field->message_type();

    if (msg->field_count() != 1) {
        IrNode n = MakeListOf(MakeStructNode(msg));
        n.facts = BaseFacts(field);
        n.facts.nullable = false;
        n.facts.warning = FlattenIgnoredWarning(msg);
        return n;
    }

    int depth = 0;
    const Descriptor* current = msg;
    while (HasMessageFlatten(current) && current->field_count() == 1) {
        const FD* inner = current->field(0);

        if (inner->is_repeated() && inner->type() == FD::TYPE_MESSAGE) {
            ++depth;
            current = inner->message_type();
            continue;
        }

        if (inner->is_repeated()) {
            if (!IsSupportedScalarType(inner->type()))
                return MakeUnsupported(field, "map value type unsupported");
            // depth-0 -> List<Scalar>; depth>0 -> List<List<...Scalar>> (projects
            // to Unsupported on the flat bridge, matching current behavior).
            IrNode leaf = MakeNode(NodeKind::SCALAR);
            leaf.facts = BaseFacts(field);
            leaf.node = ScalarVariantFor(inner);
            IrNode node = MakeListOf(std::move(leaf));
            for (int d = 0; d < depth; ++d) node = MakeListOf(std::move(node));
            node.facts = BaseFacts(field);
            node.facts.nullable = false;
            return node;
        }

        if (inner->type() == FD::TYPE_MESSAGE && HasMessageFlatten(inner->message_type())) {
            current = inner->message_type();
            continue;
        }

        if (inner->type() != FD::TYPE_MESSAGE) {
            if (!IsSupportedScalarType(inner->type()))
                return MakeUnsupported(field, "flatten wrapper leaf type unsupported");
            IrNode leaf = MakeNode(NodeKind::SCALAR);
            leaf.facts = BaseFacts(field);
            leaf.node = ScalarVariantFor(inner);
            IrNode node = MakeListOf(std::move(leaf));
            for (int d = 0; d < depth; ++d) node = MakeListOf(std::move(node));
            node.facts = BaseFacts(field);
            node.facts.nullable = false;
            return node;
        }

        current = inner->message_type();
        break;
    }

    if (IsRecursive(current))
        return MakeUnsupported(field, "message '" + current->full_name() +
                                          "' is recursive and cannot be represented in Arrow");

    // Leaf struct wrapped in (chain depth + 1) list levels.
    IrNode node = MakeListOf(MakeStructNode(current));
    for (int d = 0; d < depth; ++d) node = MakeListOf(std::move(node));
    node.facts = BaseFacts(field);
    node.facts.nullable = false;
    return node;
}

// ---------------------------------------------------------------------------
// Composite builders
// ---------------------------------------------------------------------------

IrNode BuildRepeatedMessage(const FD* field) {
    const Descriptor* msg = field->message_type();

    if (auto reason = DynamicWktUnsupportedReason(msg)) return MakeUnsupported(field, *reason);

    if (HasMessageFlatten(msg)) return BuildFlattenedRepeated(field);

    if (IsRecursive(msg))
        return MakeUnsupported(field, "message '" + msg->full_name() +
                                          "' is recursive and cannot be represented in Arrow");

    IrNode node = MakeListOf(MakeStructNode(msg));
    node.facts = BaseFacts(field);
    node.facts.nullable = false;
    int depth = NestingDepth(msg);
    if (depth >= 3)
        node.facts.warning = "list of deeply nested struct (depth " + std::to_string(depth + 1) +
                             ")";
    return node;
}

IrNode BuildRepeatedScalarOrEnum(const FD* field) {
    IrNode elem = MakeNode(NodeKind::SCALAR);
    elem.facts = BaseFacts(field);
    elem.node = ScalarVariantFor(field);
    IrNode node = MakeListOf(std::move(elem));
    node.facts = BaseFacts(field);
    node.facts.nullable = false;
    return node;
}

IrNode BuildMapNode(const FD* field) {
    const Descriptor* entry = field->message_type();
    const FD* key_fd = entry->field(0);
    const FD* val_fd = entry->field(1);

    IrNode node = MakeNode(NodeKind::MAP);
    node.facts = BaseFacts(field);
    node.facts.nullable = false;

    MapNode m;

    // Key (proto restricts keys to integral/bool/string, but guard anyway).
    if (!IsSupportedScalarType(key_fd->type()) || key_fd->type() == FD::TYPE_ENUM) {
        // enum keys are not produced by proto; treat non-scalar keys as unsupported.
        if (!IsSupportedScalarType(key_fd->type()))
            return MakeUnsupported(field, "map key type cannot map to a scalar Arrow type");
    }
    {
        IrNode k = MakeNode(NodeKind::SCALAR);
        k.facts = BaseFacts(key_fd);
        k.node = ScalarVariantFor(key_fd);
        m.key = std::make_unique<IrNode>(std::move(k));
    }

    std::string warning =
        "map type has limited Arrow compute kernel support; "
        "consider named struct fields if the key set is known at schema time";

    if (val_fd->type() == FD::TYPE_ENUM) {
        IrNode v = MakeNode(NodeKind::SCALAR);
        v.facts = BaseFacts(val_fd);
        v.node = ScalarVariantFor(val_fd);
        m.value = std::make_unique<IrNode>(std::move(v));
    } else if (val_fd->type() == FD::TYPE_MESSAGE) {
        const Descriptor* val_msg = val_fd->message_type();
        if (auto reason = DynamicWktUnsupportedReason(val_msg))
            return MakeUnsupported(field, *reason);
        if (IsRecursive(val_msg))
            return MakeUnsupported(field, "map value message '" + val_msg->full_name() +
                                              "' is recursive and cannot be represented in Arrow");
        m.value = std::make_unique<IrNode>(MakeStructNode(val_msg));
        warning += "; map with message values has fragile Parquet round-trip";
    } else if (IsSupportedScalarType(val_fd->type())) {
        IrNode v = MakeNode(NodeKind::SCALAR);
        v.facts = BaseFacts(val_fd);
        v.node = ScalarVariantFor(val_fd);
        m.value = std::make_unique<IrNode>(std::move(v));
    } else {
        return MakeUnsupported(field, "map value type unsupported");
    }

    node.facts.warning = warning;
    node.node = std::move(m);
    return node;
}

IrNode BuildSingularMessage(const FD* field) {
    const Descriptor* msg = field->message_type();
    const std::string& fqn = msg->full_name();

    if (auto reason = DynamicWktUnsupportedReason(msg)) return MakeUnsupported(field, *reason);

    if (HasMessageFlatten(msg)) return BuildFlattenedSingular(field);

    if (IsRecursive(msg))
        return MakeUnsupported(field, "message '" + fqn +
                                          "' is recursive and cannot be represented in Arrow");

    IrNode node = MakeStructNode(msg);
    node.facts = BaseFacts(field);
    node.facts.nullable = IsFieldNullable(field);
    int depth = NestingDepth(msg);
    if (depth >= 3)
        node.facts.warning = "nesting depth " + std::to_string(depth + 1) +
                             " — some Arrow consumers may not handle deep nesting well";
    return node;
}

IrNode BuildSingularScalarOrEnum(const FD* field) {
    IrNode node = MakeNode(NodeKind::SCALAR);
    node.facts = BaseFacts(field);
    node.facts.nullable = IsFieldNullable(field);
    node.node = ScalarVariantFor(field);
    return node;
}

StructNode BuildStructVariant(const Descriptor* msg) {
    StructNode s;
    s.identity.descriptor = msg;
    s.identity.full_name = msg->full_name();
    for (int i = 0; i < msg->field_count(); ++i) {
        const FD* f = msg->field(i);
        StructField sf;
        sf.name = f->name();
        sf.field_number = f->number();
        sf.type = std::make_unique<IrNode>(BuildFieldIr(f));
        s.fields.push_back(std::move(sf));
    }
    return s;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API — the single canonical classifier
// ---------------------------------------------------------------------------

IrNode BuildFieldIr(const google::protobuf::FieldDescriptor* field) {
    // 1. Real oneof (not synthetic proto3 optional).
    if (field->real_containing_oneof())
        return MakeUnsupported(field, "oneof '" + field->real_containing_oneof()->name() +
                                          "' cannot be mapped to a Parquet-safe Arrow type; "
                                          "consider using separate optional fields instead");

    // 2. Map (detected before repeated, since maps are encoded as repeated).
    if (field->is_map()) return BuildMapNode(field);

    // 3/4. Repeated fields.
    if (field->is_repeated()) {
        if (field->type() == FD::TYPE_MESSAGE) return BuildRepeatedMessage(field);
        return BuildRepeatedScalarOrEnum(field);
    }

    // 5. Singular message (WKT scalar, flatten, struct, or unsupported).
    if (field->type() == FD::TYPE_MESSAGE) {
        if (auto wkt = TryBuildWkt(field)) return std::move(*wkt);
        return BuildSingularMessage(field);
    }

    // 6/7. Singular enum or primitive.
    return BuildSingularScalarOrEnum(field);
}

StructNode BuildMessageIr(const google::protobuf::Descriptor* message) {
    return BuildStructVariant(message);
}

}  // namespace fletcher::ir
