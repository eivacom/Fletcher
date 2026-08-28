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
#include <vector>

#include "cpp_backend_type_table.hpp"
#include "ir.hpp"
#include "option_reader.hpp"

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
            // DICT-2 (D1/D8): the ONLY writer of these two members, and it derives
            // them from the SAME node's facts — never from a descriptor read, so
            // they cannot become a second source of truth. `m.scalar` above is
            // untouched: it stays the VALUE type (locked #7). Set here only, in the
            // SCALAR branch, because locked #9 makes the option legal only for a
            // SCALAR-mapped field and ValidateDictionaryDeclarations fails the
            // plugin for every other kind before any emitter runs.
            if (node.facts.dictionary) {
                m.is_dictionary = true;
                m.dict_index_type_expr =
                    cpp_backend::DictionaryIndexArrowTypeExpr(node.facts.dictionary_index_kind);
            }
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
                // Count nesting depth until the leaf. Struct-leaf nested lists
                // (List<List<...<Struct>>>) carry nested_class; GIR-10 also models
                // scalar-leaf nested lists (List<List<...<Scalar>>>) via `element`
                // plus nested_leaf_is_scalar. The scalar-leaf shape is never fed to
                // the read-only RBA accessor (locked #3): its fixture is generated
                // without accessor/rust, so RBA only ever sees struct leaves.
                int depth = 1;
                const IrNode* cur = &elem;
                while (cur->kind == NodeKind::LIST) {
                    depth += 1;
                    cur = std::get<ir::ListNode>(cur->node).element.get();
                }
                FieldMapping m{};
                m.kind = FieldKind::NESTED_LIST;
                m.nullable = node.facts.nullable;
                m.warning = node.facts.warning;
                m.list_depth = depth;
                if (cur->kind == NodeKind::STRUCT) {
                    const auto& st = std::get<ir::StructNode>(cur->node);
                    m.nested_class =
                        cpp_backend::CppClassName(st.identity.descriptor, context_file);
                    m.nested_header =
                        cpp_backend::CppCrossFileHeader(st.identity.descriptor, context_file);
                    m.nested_msg = st.identity.descriptor;
                    return m;
                }
                if (cur->kind == NodeKind::SCALAR) {
                    const auto& s = std::get<ir::ScalarNode>(cur->node);
                    m.nested_leaf_is_scalar = true;
                    m.element = ToScalarTypeInfo(
                        cpp_backend::LookupScalar(s.logical_type, s.enum_identity));
                    return m;
                }
                return std::nullopt;
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
            m.map_key =
                ToScalarTypeInfo(cpp_backend::LookupScalar(k.logical_type, k.enum_identity));

            if (v.kind == NodeKind::STRUCT) {
                const auto& vst = std::get<ir::StructNode>(v.node);
                m.map_value_is_message = true;
                m.map_value_class =
                    cpp_backend::CppClassName(vst.identity.descriptor, context_file);
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

// -----------------------------------------------------------------------
// DICT-2: (fletcher.dictionary) legality (declared in type_mapper.hpp)
// -----------------------------------------------------------------------

namespace {

// R2's kind-word. Exhaustive over ir::NodeKind with NO `default`, so a new kind
// raises a -Wswitch WARNING here (the build sets no -Werror//WX, and the fallback
// `return` below keeps it compiling with a generic string -- matching the tree's
// existing convention at ir.cpp:126). It is NOT a compile error; if that is ever
// wanted, add -Werror=switch or make the fallback abort. SCALAR is
// unreachable (R2 only fires for kind != SCALAR) and UNSUPPORTED has its own
// dedicated clause at the call site ("maps to an unsupported field type" reads
// oddly for a field that has no Arrow mapping at all).
std::string DictionaryKindWord(ir::NodeKind kind) {
    switch (kind) {
        case ir::NodeKind::LIST:
            return "a list column";
        case ir::NodeKind::MAP:
            return "a map column";
        case ir::NodeKind::STRUCT:
            return "a struct column";
        case ir::NodeKind::FIXED_SIZE_LIST:
            // Disclosed gap shared by all three IR walks: ir::BuildFieldIr never
            // constructs a FIXED_SIZE_LIST today, so no .proto can reach this.
            return "a fixed-size-list column";
        case ir::NodeKind::SCALAR:
        case ir::NodeKind::UNSUPPORTED:
            break;  // handled by the caller
    }
    return "a non-scalar column";
}

// Proto-facing word for a declared index type. Deliberately NOT
// cpp_backend::DictionaryIndexArrowTypeExpr: this text goes into a .proto
// author's error message, so it must not leak C++/Arrow spelling.
std::string DictionaryIndexWord(ir::DictionaryIndexKind kind) {
    switch (kind) {
        case ir::DictionaryIndexKind::INT8:
            return "int8";
        case ir::DictionaryIndexKind::INT16:
            return "int16";
        case ir::DictionaryIndexKind::INT32:
            return "int32";
        case ir::DictionaryIndexKind::INT64:
            return "int64";
    }
    return "int32";  // unreachable: the switch above is exhaustive
}

std::string DescribeDictionaryOption(const DictionaryOption& o) {
    return "index " + DictionaryIndexWord(o.index_kind) + ", ordered " +
           (o.ordered ? "true" : "false");
}

// R4's equality: BOTH members of the DECODED option (design D2 / step-2 B2).
// Comparing index_kind alone would leave locked #8 unenforced for a
// wrapper-declared `ordered` (leaf-wins at ir.cpp:332-336 discards the outer
// option wholesale, so R3 never sees it). Comparing the DECODED option is what
// makes `= {}`, `{index_type: DICTIONARY_INDEX_UNSPECIFIED}` and
// `{index_type: DICTIONARY_INDEX_INT32}` all EQUAL: spelling differences are
// not conflicts (locked #4 resolves UNSPECIFIED -> int32 in the reader).
bool SameDictionaryOption(const DictionaryOption& a, const DictionaryOption& b) {
    return a.index_kind == b.index_kind && a.ordered == b.ordered;
}

struct DictionaryDecl {
    DictionaryOption option;
    std::string fqn;
};

// R4 -- two or more DISAGREEING declarations on one SINGULAR, single-field
// message-level-flatten chain. Mirrors BuildFlattenedSingular's recursion
// (ir.cpp:309-338), because the conflict is invisible on every IR node: ir.cpp
// keeps only the WINNER (the `if (!inner_ir.facts.dictionary)` guard).
//
// TWO TERMS ARE LOAD-BEARING (step-2 cycle-2 item R2). R4 is the SINGULAR-chain
// rule and it must stop exactly where ir.cpp stops READING:
//   * a REPEATED outer field is R5's business -- otherwise R4 steals
//     `repeated W xs` from R5 and reports the worse message;
//   * a REPEATED inner field's OWN option still counts (BuildFlattenedRepeated
//     does read BaseFacts(inner)), but nothing BELOW it is read by ir.cpp, so
//     collecting deeper would make R4 report a "conflict" between two
//     declarations NEITHER of which is used, with advice ("make them identical")
//     that leads to silence rather than a fix.
std::optional<std::string> ConflictingFlattenChain(const google::protobuf::FieldDescriptor* field,
                                                   const std::string& prefix) {
    if (field->is_repeated()) return std::nullopt;  // repeated outer -> R5's business
    if (field->type() != FD::TYPE_MESSAGE) return std::nullopt;

    std::vector<DictionaryDecl> decls;
    // The OUTER field's option counts too: a conflict is between the outer field
    // and the chain, and R2/R3 compare nothing.
    if (const auto d = ReadFieldDictionaryOption(field)) decls.push_back({*d, field->full_name()});

    const google::protobuf::Descriptor* msg = field->message_type();
    std::set<const google::protobuf::Descriptor*> visited;
    while (HasMessageFlatten(msg) && msg->field_count() == 1 && visited.insert(msg).second) {
        const google::protobuf::FieldDescriptor* inner = msg->field(0);
        if (const auto d = ReadFieldDictionaryOption(inner))
            decls.push_back({*d, inner->full_name()});
        if (inner->is_repeated()) break;
        if (inner->type() != FD::TYPE_MESSAGE) break;
        msg = inner->message_type();
    }

    // Deterministic: compare every entry against the first. (If decls[0] ==
    // decls[1] but decls[1] != decls[2] then decls[0] != decls[2] too, so this
    // finds every disagreement.)
    for (size_t i = 1; i < decls.size(); ++i) {
        if (SameDictionaryOption(decls[0].option, decls[i].option)) continue;
        return prefix + "conflicting (fletcher.dictionary) declarations reached through " +
               "(fletcher.flatten): " + DescribeDictionaryOption(decls[0].option) + " on '" +
               decls[0].fqn + "' vs " + DescribeDictionaryOption(decls[i].option) + " on '" +
               decls[i].fqn + "'; make them identical or remove one";
    }
    return std::nullopt;
}

// R5 -- an INNER declaration under a single-field `repeated` (fletcher.flatten)
// wrapper (spec section 7.1 gap 2). Mirrors BuildFlattenedRepeated's chain loop
// (ir.cpp:374-427), INCLUDING its `field_count() == 1` term.
//
// THAT TERM IS LOAD-BEARING (step-2 B1): `repeated W xs` where W is
// `{flatten; string k [(dictionary)]; int32 n;}` is a LEGAL, EMITTED shape --
// BuildFlattenedRepeated returns List<Struct(W)> at ir.cpp:366-372 without ever
// entering its chain loop, W is not IsFlattenedWrapper so the validation pass
// judges W on its own, and W.k's scalar dictionary is honoured by
// BuildStructVariant. Dropping the term would permanently outlaw that proto.
//
// Collects INNER declarations ONLY: a declaration on the OUTER field is R2's
// business (the node is a LIST), and double-reporting it here would give one
// shape two different messages.
std::optional<std::string> InnerDeclaredUnderRepeatedFlatten(
    const google::protobuf::FieldDescriptor* field, const std::string& prefix) {
    if (!field->is_repeated() || field->is_map()) return std::nullopt;
    if (field->type() != FD::TYPE_MESSAGE) return std::nullopt;

    const google::protobuf::Descriptor* current = field->message_type();
    std::set<const google::protobuf::Descriptor*> visited;
    while (HasMessageFlatten(current) && current->field_count() == 1 &&
           visited.insert(current).second) {
        const google::protobuf::FieldDescriptor* inner = current->field(0);
        if (HasFieldDictionary(inner)) {
            return prefix + "(fletcher.dictionary) declared on '" + inner->full_name() +
                   "' inside a repeated (fletcher.flatten) wrapper: the resulting column is a "
                   "list, which cannot be dictionary-encoded; remove the option";
        }
        if (inner->type() != FD::TYPE_MESSAGE) break;
        current = inner->message_type();
    }
    return std::nullopt;
}

}  // namespace

std::optional<std::string> DictionaryUnsupportedReason(
    const google::protobuf::FieldDescriptor* field) {
    // Every message names the offending declaration's FULLY-QUALIFIED proto name
    // (the existing front-end passes' convention; the ctest harness asserts
    // "Message.field"). ASCII only -- no em-dash / section sign in emitted text.
    const std::string prefix = "field '" + field->full_name() + "': ";

    // ---- R1: a (fletcher.flatten_field) wrapper carrying the option ---------
    // THE FULL THREE-TERM PREDICATE, byte-identical to the one both inlining
    // walks use (generator.cpp:606-607, cpp_backend_schema_visitor.cpp:78-79).
    // HasFieldFlatten alone would wrongly reject `string s = 2 [(flatten_field),
    // (dictionary)]`, which spec section 4 documents as a no-op + a LEGAL
    // dictionary (DICT-1 pins it as `Rec.s`).
    //
    // R1 MUST PRECEDE R2 and is NOT redundant with it. Two shapes reach a SCALAR
    // node that R2 would ACCEPT while emission drops the fact entirely:
    //   * a flatten_field wrapper over a message-level (fletcher.flatten) wrapper
    //     -- BuildFlattenedSingular propagates the wrapper's option onto the
    //     resolved inner SCALAR (ir.cpp:332-336), but both walks `continue` past
    //     the wrapper field and build from the inner field, which carries nothing;
    //   * a WKT wrapper (google.protobuf.StringValue etc.) -- TryBuildWkt
    //     (ir.cpp:601) yields a nullable SCALAR carrying the dictionary, while
    //     both walks inline StringValue.value as a plain non-nullable utf8 column.
    // DO NOT exclude WKTs from this rule: that silently reopens the hole. (A WKT
    // wrapper WITHOUT flatten_field is locked #9's acceptance case and stays
    // legal.)
    if (field->type() == FD::TYPE_MESSAGE && !field->is_repeated() && HasFieldFlatten(field) &&
        HasFieldDictionary(field)) {
        return prefix +
               "(fletcher.dictionary) cannot be combined with (fletcher.flatten_field): the "
               "wrapper's fields are inlined as separate columns, so there is no single column "
               "to dictionary-encode; remove (fletcher.flatten_field), or move the option onto "
               "the inlined field or fields";
    }

    const ir::IrNode node = ir::BuildFieldIr(field);  // built once, reused by R2/R3

    // ---- R2: the kind gate (locked #9, spec section 4) ----------------------
    // Gates on the TOP-LEVEL node's kind ONLY -- never an OR over the subtree.
    // Spec section 7.1's closing rule licenses the subtree-OR only for
    // DICT-1.5's backend-availability predicate, where over-approximating is
    // safe; for a LEGALITY gate an OR would permanently outlaw a legal proto (a
    // scalar dictionary inside a struct child). Each inner field is judged on
    // its OWN mapped kind when its own message is walked.
    //
    // ir::NodeKind::SCALAR is exactly locked #9's FieldKind::SCALAR:
    // ProjectIrToFieldMapping's SCALAR branch is the only producer of
    // FieldKind::SCALAR and always produces it. Gating on the IR kind is the
    // same gate, and is additionally well-defined for the kinds where the
    // projection returns nullopt (FIXED_SIZE_LIST, UNSUPPORTED).
    if (node.facts.dictionary && node.kind != ir::NodeKind::SCALAR) {
        if (node.kind == ir::NodeKind::UNSUPPORTED) {
            return prefix +
                   "(fletcher.dictionary) is declared on a field that has no Arrow mapping at "
                   "all; remove the option (the field's own unsupported-type error is reported "
                   "first when generating)";
        }
        return prefix + "(fletcher.dictionary) requires a scalar column, but this field maps to " +
               DictionaryKindWord(node.kind) +
               " (see docs/dictionary-option-spec.md section 4); remove the option";
    }

    // ---- R3: ordered: true (locked #8, spec section 6) ---------------------
    if (node.facts.dictionary && node.facts.dictionary_ordered) {
        return prefix +
               "(fletcher.dictionary) with ordered: true is not supported in v1 (the runtime "
               "re-fold produces a first-seen-order dictionary); remove ordered or set it to "
               "false";
    }

    // ---- R4 / R5: the two descriptor-level chain rules ---------------------
    if (auto e = ConflictingFlattenChain(field, prefix)) return e;
    if (auto e = InnerDeclaredUnderRepeatedFlatten(field, prefix)) return e;

    return std::nullopt;
}

namespace {

std::optional<std::string> FindIllegalDictionaryFieldImpl(
    const google::protobuf::Descriptor* msg,
    std::set<const google::protobuf::Descriptor*>& visited) {
    if (!visited.insert(msg).second) return std::nullopt;
    for (int i = 0; i < msg->field_count(); ++i) {
        const google::protobuf::FieldDescriptor* fd = msg->field(i);
        if (auto e = DictionaryUnsupportedReason(fd)) return e;  // includes R1
        // Descend through a (fletcher.flatten_field) wrapper with the SAME
        // three-term predicate both inlining walks use, so this sees exactly the
        // fields that become columns of `msg`. R1 above already cleared the
        // wrapper field itself.
        if (fd->type() == FD::TYPE_MESSAGE && !fd->is_repeated() && HasFieldFlatten(fd)) {
            if (auto e = FindIllegalDictionaryFieldImpl(fd->message_type(), visited)) return e;
            continue;
        }

        // STRUCT-CHILD / MAP-VALUE descent (step-4 re-review, S3 closure). Without
        // it the SAME declaration is fatal or silently accepted depending only on
        // which .proto protoc was pointed at: `protoc inner2.proto` rejects
        // `xf.i2.M.p`, while `protoc outer2.proto` (which merely imports it)
        // exited 0. The importing file's schema for that child is
        // `ArrowSchemaDeepCopy(<Child>Schema())`, i.e. the child message's OWN
        // inlining walk, so the child's columns are judged by exactly the rules
        // that produce them.
        //
        // R1 IS SOUND HERE, and that is not obvious -- it is why the exclusion
        // below is the load-bearing term rather than the descent:
        //   * a struct child's columns come from the child's own generated schema
        //     function, which IS built by GatherFieldsImpl /
        //     BuildFlattenedFieldListImpl -- so a (fletcher.flatten_field) field
        //     inside it really is inlined and its dictionary really is dropped.
        //     Corroborating structural fact: NO emitter reads
        //     ir::StructNode.fields; the only readers repo-wide are the three
        //     validation walks. So the "IR view" in which such a field stays a
        //     scalar carrying the dictionary never reaches an artifact.
        //   * an IsFlattenedWrapper child has NO generated schema function, so
        //     nothing inlines its fields; `flatten_field` inside it is a NO-OP and
        //     the declaration is RESOLVED AND HONOURED. Judging one would make R1
        //     a FALSE POSITIVE -- proved on
        //     `FfW {flatten; FfLeaf p = 1 [(flatten_field), (dictionary)={INT16}]}`,
        //     which generates today at exit 0 with a live int16 dictionary.
        //     NEVER descend into one. Pinned end-to-end by ctest
        //     GenErrors.DictionaryLiveInsideFlattenWrapperAccepted, and at unit
        //     level by Chains.ff_inside_wrapper.
        // IsRecursive is excluded for the same reason the pass skips it at the
        // top: such a message is never generated.
        // WHY THESE POSITIONS, AND WHICH ONE IS MISSING.
        // cpp_backend_schema_visitor emits a child message's OWN schema function
        // via DeepCopyMessageStruct at FOUR positions, from TWO call sites
        // (NodeKind::STRUCT at :449 and NodeKind::MAP at :468 -- the LIST case
        // RECURSES into EmitNodeType rather than calling):
        //   (1) a singular nested struct field
        //   (2) the element of List<Struct>
        //   (3) the struct LEAF of List<List<...<Struct>>>
        //   (4) a map value
        // This walk judges a position when the child message is named DIRECTLY by
        // a field of a judged message: (1), (2) and (4) below.
        //
        // It does NOT judge a position whose child is reached THROUGH a
        // (fletcher.flatten) WRAPPER hop, because the !IsFlattenedWrapper(child)
        // term cuts the walk at the wrapper -- and that term is required for R1's
        // soundness, so this is NOT fixable by descending into the wrapper. That
        // excludes (3) entirely (a nested list is only constructible through a
        // wrapper hop, via BuildFlattenedRepeated) and excludes (1)/(2)/(4)
        // whenever a wrapper sits in between. Reproduced: Top{repeated NestWrap} /
        // NestWrap{flatten; repeated xf.li.Inner ms} exits 0 while still emitting
        // DeepCopy(InnerSchema(), children[0]->children[0]->children[0]), whereas
        // `protoc leaf_inner.proto` rejects xf.li.Inner.k. Closing it needs the
        // wrapper CHAIN followed to its leaf message (judging the leaf, never the
        // wrapper's own fields); carried with the S2 follow-up. Disclosed in
        // docs/dictionary-option-spec.md section 7.1.1.
        const google::protobuf::Descriptor* child = nullptr;
        if (fd->is_map()) {
            const google::protobuf::FieldDescriptor* val = fd->message_type()->field(1);
            if (val->type() == FD::TYPE_MESSAGE) child = val->message_type();
        } else if (fd->type() == FD::TYPE_MESSAGE) {
            child = fd->message_type();
        }
        if (child != nullptr && !IsFlattenedWrapper(child) && !IsRecursive(child)) {
            if (auto e = FindIllegalDictionaryFieldImpl(child, visited)) return e;
        }
    }
    return std::nullopt;
}

}  // namespace

std::optional<std::string> FindIllegalDictionaryField(const google::protobuf::Descriptor* msg) {
    // The `visited` set does real work now that the walk descends into struct /
    // map-value children (step-4 S3 closure): it terminates cycles AND collapses a
    // diamond (the same child reached from two fields of one root is judged once).
    // Determinism is unaffected -- a child's own fields are judged identically
    // whichever path reaches them, and every message names the offending
    // declaration's own fully-qualified field name.
    std::set<const google::protobuf::Descriptor*> visited;
    return FindIllegalDictionaryFieldImpl(msg, visited);
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
