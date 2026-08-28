// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include "ir.hpp"

#include <memory>
#include <optional>
#include <string>
#include <variant>

#include "option_reader.hpp"
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
    const bool proto2 = field->file()->syntax() == google::protobuf::FileDescriptor::SYNTAX_PROTO2;
    f.proto2_optional = proto2 && field->label() == FD::LABEL_OPTIONAL;
    f.proto3_optional = !proto2 && field->has_optional_keyword();
    // DICT-1: the (fletcher.dictionary) option is read HERE, in the one place
    // every field-built node passes through, so every such node carries the fact
    // identically.
    //
    // PLACEMENT RULE (step-4b SF-2, qualified by re-review RR-1) — a dictionary
    // declaration is a FIELD-level fact, and it lands on EVERY node that is
    // itself built from BaseFacts(field). Precisely (all pinned by
    // TypeMapperTest.ReadsDictionaryOption):
    //   * singular scalar / WKT wrapper / struct / map / oneof-unsupported: the
    //     one node built from the field. Map KEY and VALUE nodes are built from
    //     the synthetic map entry's own fields and therefore do NOT carry it.
    //   * `repeated` scalar or enum: the LIST node AND its element, because
    //     BuildRepeatedScalarOrEnum calls BaseFacts(field) for both.
    //   * message-level-flatten propagation: ApplyDictionaryFacts mirrors that,
    //     so a wrapper over a `repeated` inner field also lands on both.
    //   * BUT NOT the INTERMEDIATE levels of a nested-list shape: the extra list
    //     levels that BuildFlattenedRepeated builds with MakeListOf keep DEFAULT
    //     facts, so `repeated <wrapper of a repeated scalar>` reads
    //     outer LIST = true, intermediate LIST = FALSE, leaf SCALAR = true. A
    //     consumer walking down therefore finds -> loses -> refinds the fact;
    //     do not treat an interior `dictionary == false` as authoritative.
    // Gating stays on the TOP-LEVEL node's kind, never on "some node has
    // dictionary = true". See docs/dictionary-option-spec.md 7.1.
    if (const std::optional<DictionaryOption> d = ReadFieldDictionaryOption(field)) {
        f.dictionary = true;
        f.dictionary_index_kind = d->index_kind;
        f.dictionary_ordered = d->ordered;
    }
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
    return LogicalType{
        k, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt};
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
        s.logical_type = LogicalType{LogicalKind::WKT_TIMESTAMP,
                                     std::nullopt,
                                     TimeUnit::NANO,
                                     std::nullopt,
                                     std::nullopt,
                                     std::nullopt,
                                     std::nullopt};
        n.node = std::move(s);
        return n;
    }
    if (fqn == "google.protobuf.Duration") {
        IrNode n = MakeNode(NodeKind::SCALAR);
        n.facts = BaseFacts(field);
        n.facts.nullable = IsFieldNullable(field);
        n.facts.wkt = WktKind::DURATION;
        ScalarNode s;
        s.logical_type = LogicalType{LogicalKind::WKT_DURATION,
                                     std::nullopt,
                                     TimeUnit::NANO,
                                     std::nullopt,
                                     std::nullopt,
                                     std::nullopt,
                                     std::nullopt};
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

// DICT-1 (step-4b SF-2): apply a propagated dictionary declaration to `node` AND
// to the nodes below it that were built from the SAME proto field — today that is
// a LIST's element (a flatten wrapper whose single inner field is `repeated`), so
// the flatten path lands the fact in the same places BaseFacts would have. Leaf
// wins is enforced per node: an existing declaration is never overwritten.
void ApplyDictionaryFacts(IrNode& node, const DictionaryOption& d) {
    if (!node.facts.dictionary) {
        node.facts.dictionary = true;
        node.facts.dictionary_index_kind = d.index_kind;
        node.facts.dictionary_ordered = d.ordered;
    }
    if (node.kind == NodeKind::LIST) {
        if (const ListNode* list = std::get_if<ListNode>(&node.node)) {
            if (list->element) ApplyDictionaryFacts(*list->element, d);
        }
    }
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
    // DICT-1: propagate the OUTER (wrapper) field's (fletcher.dictionary) onto
    // the resolved inner node. What is returned is the INNER field's IR, so a
    // wrapper-declared option would otherwise be silently dropped and a
    // value-typed column emitted for a field the author declared dictionary.
    // LEAF WINS when both declare it — the most specific declaration governs, so
    // a shared-library wrapper cannot dictate a consumer's column. A DISAGREEING
    // pair is accepted silently: facts.warning is no longer rendered anywhere
    // post-GIR, so escalating the conflict belongs to the item that owns the
    // error channel, not here.
    if (!inner_ir.facts.dictionary) {
        if (const std::optional<DictionaryOption> outer = ReadFieldDictionaryOption(field)) {
            ApplyDictionaryFacts(inner_ir, *outer);
        }
    }
    return inner_ir;
}

// Repeated message field whose target has (fletcher.flatten): walk the flatten
// chain, wrapping the leaf in one List level per chain hop plus the caller's own
// `repeated`. Mirrors type_mapper's MapFlattenedRepeated (including its current
// treatment of scalar leaves: depth-0 collapses to List<Scalar>, depth>0 is
// Unsupported until GIR-10).
//
// DICT-1 (step-4b SF-1) — KNOWN GAP, deliberately deferred, owned by the
// validation item that owns the error channel (DICT-2), tracked next to the
// (fletcher.flatten_field) hole in docs/dictionary-option-spec.md §7.1:
// the OUTERMOST node and the leaf here are built from BaseFacts(FIELD), i.e. the
// OUTER field, so an outer `[(fletcher.dictionary)]` is carried on those (but NOT
// on the intermediate list levels of a nested-list shape — RR-1, and see the
// PLACEMENT RULE on BaseFacts) — while this path never
// calls BuildFieldIr(inner), so a dictionary declared on the wrapper's INNER
// field is NOT read, and `repeated W xs` therefore disagrees with `W w` (which
// goes through BuildFlattenedSingular). Not fixed here because the resulting node
// is always LIST/UNSUPPORTED, which DICT-2 rejects as non-SCALAR anyway, and
// mirroring the propagation across this function's seven return sites (four
// LIST, three MakeUnsupported) would put
// the GIR-10 nested-list shapes at risk for no user-visible gain. Today's
// behaviour is PINNED by TypeMapperTest.ReadsDictionaryOption
// ("flattened repeated: inner-declared dictionary is dropped (SF-1)"), so
// closing the gap flips a test rather than silently changing behaviour.
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
            // The wrapper's own `repeated <scalar>` is ONE list level; the caller's
            // `repeated` (the field being classified) is ANOTHER; each intermediate
            // repeated-message flatten hop counted in `depth` adds one more. So the
            // faithful shape is (depth + 2) list levels: e.g. `repeated
            // ScalarListWrapper` (depth 0) -> List<List<Scalar>>, `repeated
            // NestedScalarListWrapper` (depth 1) -> List<List<List<Scalar>>>. GIR-10
            // enables these scalar-leaf nested lists (they previously COLLAPSED —
            // the caller's `repeated` level was dropped to a flat List<Scalar> — and
            // were parked in coverage_future.proto).
            IrNode leaf = MakeNode(NodeKind::SCALAR);
            leaf.facts = BaseFacts(field);
            leaf.node = ScalarVariantFor(inner);
            IrNode node = MakeListOf(std::move(leaf));
            for (int d = 0; d < depth + 1; ++d) node = MakeListOf(std::move(node));
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
        node.facts.warning =
            "list of deeply nested struct (depth " + std::to_string(depth + 1) + ")";
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
        return MakeUnsupported(
            field, "message '" + fqn + "' is recursive and cannot be represented in Arrow");

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
