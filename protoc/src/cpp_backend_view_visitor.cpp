// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include "cpp_backend_view_visitor.hpp"

#include <string>
#include <variant>

#include "cpp_backend_type_table.hpp"

namespace fletcher::cpp_backend {

// ---------------------------------------------------------------------------
// GIR-6 Arrow view getter + ToArrowRow() + AppendTo() recursive visitors. Maps
// IR recursion to generated-code recursion, reproducing the former
// FieldMapping-switch EmitViewGetters + GenerateToArrowRow: getter/row field order,
// nullable vs non-nullable branching, string/bytes std::string_view vs owned
// std::string handling, whole-shape nested-list (depth-2 ArrowNestedList /
// depth-3 ArrowNestedList2) classification with leaf-struct identity, and
// value-builder vs pointer-builder map/list mechanics are all unchanged.
//
// Nested-list depth is structural (ListNode has no list_depth): the visitor walks
// nested ListNode elements to the leaf StructNode and counts levels, matching the
// GIR-4 decode visitor and the IR->FieldMapping projection.
//
// AppendTo() is the third emitter here and the only one that writes Arrow data
// through typed child builders rather than one arrow::Scalar per value; the
// ToArrowRow() branches whose elements are messages (repeated struct, nested
// list of struct, map with a message value) route through it, so a composite
// field is materialised once, in typed form, on either path.
// ---------------------------------------------------------------------------

namespace {

// Replace every "{val}" token in a scalar_ctor template. scalar_ctor carries a
// single {val}; ReplaceAll matches the former generator.cpp helper exactly.
std::string ReplaceAll(std::string s, const std::string& from, const std::string& to) {
    std::size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

// The innermost leaf node of a nested list plus the number of list levels.
// Structural: walk ListNode elements until the leaf. The leaf is a StructNode
// (List<List<...<Struct>>>) or, GIR-10, a ScalarNode (List<List<...<Scalar>>>);
// callers branch on leaf->kind rather than assuming a struct identity (a former
// unconditional std::get<StructNode> here threw bad_variant_access on a scalar).
struct NestedListShape {
    int depth = 1;
    const ir::IrNode* leaf = nullptr;
};

NestedListShape ClassifyNestedList(const ir::IrNode& list_node) {
    NestedListShape shape;
    const ir::IrNode* cur = std::get<ir::ListNode>(list_node.node).element.get();
    while (cur->kind == ir::NodeKind::LIST) {
        shape.depth += 1;
        cur = std::get<ir::ListNode>(cur->node).element.get();
    }
    shape.leaf = cur;
    return shape;
}

// The maximum nested-list depth THIS FILE's two emitters can render. Both are
// written out per depth (depth-2 and depth-3 branches in ToArrowRow; the
// ArrowNestedList/ArrowNestedList2 template choice in the view getter), so a
// deeper list has no faithful emission here.
//
// Deeper lists are deliberately NOT rejected at the classifier: they are
// representable everywhere else — nanoarrow schema, row storage, edge encode and
// edge decode are all depth-generic and correct at any depth — and
// `integration-tests/protoc-gen-fletcher-rust/proto/transitive_gate.proto`
// (RBA-6b, D-RBA-8) depends on a depth-4 field being "in-scope representable but
// beyond the supported depth" so the RBA accessor's transitive skip gate has
// something to skip. Rejecting at the classifier would change the emitted schema
// (locked #2) and break that fixture.
//
// So the failure is placed exactly where the defect is: the generated Arrow
// header. Previously these emitters fell through both depth branches and emitted
// a block with NO row.push_back, leaving an N-1 row vector against an N-child
// schema — silently, with no compile error (PR #125 review, findings 1 and 2). A
// static_assert turns that silent arity corruption into a clear compile error for
// the only consumer it can affect: one that actually compiles this header.
constexpr int kMaxRenderableNestedListDepth = 3;

void EmitNestedListDepthUnsupported(std::ostringstream& out, const std::string& field_name,
                                    int depth, const char* context) {
    out << "    static_assert(false,\n"
        << "        \"" << field_name << ": nested list depth " << depth
        << " exceeds the maximum depth " << kMaxRenderableNestedListDepth << "\"\n"
        << "        \" that " << context << " can render. The nanoarrow schema, row storage\"\n"
        << "        \" and edge encode/decode handle this field correctly; only the Arrow\"\n"
        << "        \" view layer cannot. Either keep this field out of Arrow"
           " view/ToArrowRow\"\n"
        << "        \" consumers, or make both emitters depth-generic (round RIR).\");\n";
}

// ---- View getter visitor --------------------------------------------------

class EdgeViewGetterVisitor {
   public:
    EdgeViewGetterVisitor(std::ostringstream& out, std::string getter_name, std::size_t field_index,
                          const google::protobuf::FileDescriptor* context_file)
        : out_(out),
          name_(std::move(getter_name)),
          si_(std::to_string(field_index)),
          context_file_(context_file) {}

    void EmitField(const ir::IrNode& node) {
        switch (node.kind) {
            case ir::NodeKind::SCALAR:
                EmitScalar(std::get<ir::ScalarNode>(node.node), node.facts.nullable);
                break;
            case ir::NodeKind::STRUCT:
                EmitStruct(std::get<ir::StructNode>(node.node), node.facts.nullable);
                break;
            case ir::NodeKind::LIST:
                EmitList(node);
                break;
            case ir::NodeKind::MAP:
                EmitMap(std::get<ir::MapNode>(node.node));
                break;
            case ir::NodeKind::FIXED_SIZE_LIST:
            case ir::NodeKind::UNSUPPORTED:
                // GatherFields drops these before view emission (their projection
                // is nullopt); no view getter is produced.
                break;
        }
    }

   private:
    void EmitScalar(const ir::ScalarNode& s, bool nullable) {
        const CppScalarInfo& sc = LookupScalar(s.logical_type, s.enum_identity);
        const std::string& ret = sc.getter_type;

        if (nullable) {
            out_ << "    std::optional<" << ret << "> " << name_ << "() const {\n"
                 << "        if (!scalars_[" << si_ << "]->is_valid) return std::nullopt;\n";
            if (sc.value_is_buffer) {
                out_ << "        const auto& s = static_cast<const " << sc.scalar_type
                     << "&>(*scalars_[" << si_ << "]);\n"
                     << "        return std::string_view{\n"
                     << "            reinterpret_cast<const char*>"
                        "(s.value->data()),\n"
                     << "            static_cast<size_t>"
                        "(s.value->size())};\n";
            } else {
                out_ << "        return static_cast<const " << sc.scalar_type << "&>(*scalars_["
                     << si_ << "]).value;\n";
            }
            out_ << "    }\n";
        } else {
            out_ << "    " << ret << " " << name_ << "() const {\n";
            if (sc.value_is_buffer) {
                out_ << "        const auto& s = static_cast<const " << sc.scalar_type
                     << "&>(*scalars_[" << si_ << "]);\n"
                     << "        return {reinterpret_cast<const char*>"
                        "(s.value->data()),\n"
                     << "                static_cast<size_t>"
                        "(s.value->size())};\n";
            } else {
                out_ << "        return static_cast<const " << sc.scalar_type << "&>(*scalars_["
                     << si_ << "]).value;\n";
            }
            out_ << "    }\n";
        }

        // GIR-9 (#75): additive typed view getter for an enum scalar (a cast
        // over the retained raw int32 getter). References the row-header enum
        // declaration via the shared CppEnumName — the view header never
        // re-declares enums.
        if (s.enum_identity.has_value() && CppEnumTypeEmittable(s.enum_identity->descriptor)) {
            const std::string en = CppEnumName(s.enum_identity->descriptor, context_file_);
            if (nullable) {
                out_ << "    std::optional<" << en << "> " << name_ << "_typed() const {\n"
                     << "        auto v = " << name_ << "();\n"
                     << "        if (!v.has_value()) return std::nullopt;\n"
                     << "        return static_cast<" << en << ">(*v);\n"
                     << "    }\n";
            } else {
                out_ << "    " << en << " " << name_ << "_typed() const {\n"
                     << "        return static_cast<" << en << ">(" << name_ << "());\n"
                     << "    }\n";
            }
        }
    }

    void EmitStruct(const ir::StructNode& st, bool nullable) {
        const std::string vt = CppClassName(st.identity.descriptor, context_file_) + "View";
        if (nullable) {
            out_ << "    std::optional<" << vt << "> " << name_ << "() const {\n"
                 << "        if (!scalars_[" << si_ << "]->is_valid) return std::nullopt;\n"
                 << "        return " << vt << "(scalars_[" << si_ << "]);\n"
                 << "    }\n";
        } else {
            out_ << "    " << vt << " " << name_ << "() const {\n"
                 << "        return " << vt << "(scalars_[" << si_ << "]);\n"
                 << "    }\n";
        }
    }

    void EmitList(const ir::IrNode& list_node) {
        const ir::IrNode& elem = *std::get<ir::ListNode>(list_node.node).element;
        switch (elem.kind) {
            case ir::NodeKind::SCALAR:
                EmitRepeatedScalar(std::get<ir::ScalarNode>(elem.node));
                break;
            case ir::NodeKind::STRUCT:
                EmitRepeatedStruct(std::get<ir::StructNode>(elem.node));
                break;
            case ir::NodeKind::LIST:
                EmitNestedList(list_node);
                break;
            default:
                break;
        }
    }

    void EmitRepeatedScalar(const ir::ScalarNode& e) {
        const CppScalarInfo& el = LookupScalar(e.logical_type, e.enum_identity);
        const std::string& vt = el.getter_type;
        const std::string& at = el.array_type;
        out_ << "    fletcher::ArrowScalarList<" << vt << ", " << at << "> " << name_
             << "() const {\n"
             << "        const auto& ls = static_cast"
                "<const arrow::ListScalar&>(\n"
             << "            *scalars_[" << si_ << "]);\n"
             << "        return fletcher::ArrowScalarList<" << vt << ", " << at << ">(ls.value);\n"
             << "    }\n";

        // GIR-9 (#75): additive typed view getter for a repeated enum (casts the
        // value side of the retained raw ArrowScalarList).
        if (e.enum_identity.has_value() && CppEnumTypeEmittable(e.enum_identity->descriptor)) {
            const std::string en = CppEnumName(e.enum_identity->descriptor, context_file_);
            out_ << "    std::vector<" << en << "> " << name_ << "_typed() const {\n"
                 << "        std::vector<" << en << "> out;\n"
                 << "        auto raw = " << name_ << "();\n"
                 << "        out.reserve(static_cast<size_t>(raw.size()));\n"
                 << "        for (int32_t v : raw) out.push_back(static_cast<" << en << ">(v));\n"
                 << "        return out;\n"
                 << "    }\n";
        }
    }

    void EmitRepeatedStruct(const ir::StructNode& st) {
        const std::string vt = CppClassName(st.identity.descriptor, context_file_) + "View";
        out_ << "    fletcher::ArrowRowViewList<" << vt << "> " << name_ << "() const {\n"
             << "        const auto& ls = static_cast"
                "<const arrow::ListScalar&>(\n"
             << "            *scalars_[" << si_ << "]);\n"
             << "        return fletcher::ArrowRowViewList<" << vt << ">(ls.value);\n"
             << "    }\n";
    }

    void EmitNestedList(const ir::IrNode& list_node) {
        const NestedListShape shape = ClassifyNestedList(list_node);
        if (shape.depth > kMaxRenderableNestedListDepth) {
            // Was: any depth != 3 silently got the depth-2 accessor template.
            EmitNestedListDepthUnsupported(out_, name_, shape.depth, "the Arrow view accessor");
            return;
        }
        std::string tmpl;
        if (shape.leaf->kind == ir::NodeKind::SCALAR) {
            // GIR-10 scalar leaf: depth-2 -> ArrowNestedScalarList<V,A>, depth-3 ->
            // ArrowNestedScalarList2<V,A>. Both yield ArrowScalarList at the leaf.
            const auto& e = std::get<ir::ScalarNode>(shape.leaf->node);
            const CppScalarInfo& el = LookupScalar(e.logical_type, e.enum_identity);
            const std::string& vt = el.getter_type;
            const std::string& at = el.array_type;
            tmpl = (shape.depth == 3) ? "fletcher::ArrowNestedScalarList2<" + vt + ", " + at + ">"
                                      : "fletcher::ArrowNestedScalarList<" + vt + ", " + at + ">";
        } else {
            const std::string vt =
                CppClassName(std::get<ir::StructNode>(shape.leaf->node).identity.descriptor,
                             context_file_) +
                "View";
            tmpl = (shape.depth == 3) ? "fletcher::ArrowNestedList2<" + vt + ">"
                                      : "fletcher::ArrowNestedList<" + vt + ">";
        }
        out_ << "    " << tmpl << " " << name_ << "() const {\n"
             << "        const auto& ls = static_cast"
                "<const arrow::ListScalar&>(\n"
             << "            *scalars_[" << si_ << "]);\n"
             << "        return " << tmpl << "(ls.value);\n"
             << "    }\n";
    }

    void EmitMap(const ir::MapNode& map) {
        const ir::ScalarNode& key = std::get<ir::ScalarNode>(map.key->node);
        const CppScalarInfo& ki = LookupScalar(key.logical_type, key.enum_identity);
        const std::string& kv = ki.getter_type;
        const std::string& ka = ki.array_type;

        if (map.value->kind == ir::NodeKind::STRUCT) {
            const std::string vt =
                CppClassName(std::get<ir::StructNode>(map.value->node).identity.descriptor,
                             context_file_) +
                "View";
            out_ << "    fletcher::ArrowRowViewMap<" << kv << ", " << ka << ", " << vt << "> "
                 << name_ << "() const {\n"
                 << "        const auto& ms = static_cast"
                    "<const arrow::MapScalar&>(\n"
                 << "            *scalars_[" << si_ << "]);\n"
                 << "        return fletcher::ArrowRowViewMap<" << kv << ", " << ka << ", " << vt
                 << ">(ms.value);\n"
                 << "    }\n";
        } else {
            const ir::ScalarNode& val = std::get<ir::ScalarNode>(map.value->node);
            const CppScalarInfo& vi = LookupScalar(val.logical_type, val.enum_identity);
            const std::string& vv = vi.getter_type;
            const std::string& va = vi.array_type;
            out_ << "    fletcher::ArrowScalarMap<" << kv << ", " << ka << ", " << vv << ", " << va
                 << "> " << name_ << "() const {\n"
                 << "        const auto& ms = static_cast"
                    "<const arrow::MapScalar&>(\n"
                 << "            *scalars_[" << si_ << "]);\n"
                 << "        return fletcher::ArrowScalarMap<" << kv << ", " << ka << ", " << vv
                 << ", " << va << ">(ms.value);\n"
                 << "    }\n";

            // GIR-9 (#75): additive typed view getter for an enum-valued map
            // (casts the value side; the key is copied into its storage type).
            if (val.enum_identity.has_value() &&
                CppEnumTypeEmittable(val.enum_identity->descriptor)) {
                const std::string en = CppEnumName(val.enum_identity->descriptor, context_file_);
                const std::string& kt = ki.storage_type;
                out_ << "    std::vector<std::pair<" << kt << ", " << en << ">> " << name_
                     << "_typed() const {\n"
                     << "        std::vector<std::pair<" << kt << ", " << en << ">> out;\n"
                     << "        auto raw = " << name_ << "();\n"
                     << "        out.reserve(static_cast<size_t>(raw.size()));\n"
                     << "        for (const auto& e : raw)\n"
                     << "            out.emplace_back(" << kt << "(e.key), static_cast<" << en
                     << ">(e.value));\n"
                     << "        return out;\n"
                     << "    }\n";
            }
        }
    }

    std::ostringstream& out_;
    const std::string name_;  // generated getter method name (proto field name)
    const std::string si_;    // positional index into scalars_
    const google::protobuf::FileDescriptor* context_file_;
};

// ---- AppendTo() visitor ---------------------------------------------------

// Appends one message as ONE element of a pre-built arrow::StructBuilder,
// driving that struct's already-typed child builders directly instead of
// materialising one arrow::Scalar per element. Found via ADL in the generated
// header, and used recursively for every nested struct (nested STRUCT, repeated
// struct, nested list of struct, map with a message value) by AppendTo() itself
// and by ToArrowRow()'s composite branches.
//
// Unlike the two visitors above, `field_index` is load-bearing: it is the
// `b.field_builder(<i>)` slot this field writes. Composite builder types are
// literals (arrow::StructBuilder / arrow::ListBuilder / arrow::MapBuilder) —
// only the SCALAR leaf spelling comes from the C++ lookup table (locked #1).
// Every field is emitted as one brace-scoped block, so the child-builder
// reference names (fb/sb/lb.../mb) never collide between fields.
class EdgeAppendToVisitor {
   public:
    EdgeAppendToVisitor(std::ostringstream& out, std::string getter_expr, std::size_t field_index)
        : out_(out), getter_(std::move(getter_expr)), si_(std::to_string(field_index)) {}

    void EmitField(const ir::IrNode& node) {
        switch (node.kind) {
            case ir::NodeKind::SCALAR:
                EmitScalar(std::get<ir::ScalarNode>(node.node), node.facts.nullable);
                break;
            case ir::NodeKind::STRUCT:
                EmitStruct(node.facts.nullable);
                break;
            case ir::NodeKind::LIST:
                EmitList(node);
                break;
            case ir::NodeKind::MAP:
                EmitMap(std::get<ir::MapNode>(node.node));
                break;
            case ir::NodeKind::FIXED_SIZE_LIST:
            case ir::NodeKind::UNSUPPORTED:
                // GatherFields drops these before view emission, so no builder
                // slot is consumed and the indices stay aligned with the schema.
                break;
        }
    }

   private:
    // The public getter already applies value_or(default) for a non-nullable
    // scalar (generator.cpp EmitGetters), so this branches only on nullable vs
    // non-nullable and never re-applies a default. String/bytes arrive as a
    // std::string_view, which every Arrow binary builder appends directly (no
    // owning std::string is needed here: the builder copies into its buffer,
    // unlike ToArrowRow, whose arrow::Scalar has to own the bytes).
    void EmitScalar(const ir::ScalarNode& s, bool nullable) {
        const CppScalarInfo& sc = LookupScalar(s.logical_type, s.enum_identity);
        out_ << "    {\n"
             << "        // " << getter_ << "\n"
             << "        auto& fb = static_cast<" << sc.builder_type << "&>(*b.field_builder("
             << si_ << "));\n";
        if (nullable) {
            out_ << "        if (" << getter_ << ".has_value()) ARROW_RETURN_NOT_OK(fb.Append(*"
                 << getter_ << "));\n"
                 << "        else ARROW_RETURN_NOT_OK(fb.AppendNull());\n";
        } else {
            out_ << "        ARROW_RETURN_NOT_OK(fb.Append(" << getter_ << "));\n";
        }
        out_ << "    }\n";
    }

    // No class name is needed: AppendTo() is a free function resolved by overload
    // on the message type, and OrderedMessages() has already emitted the nested
    // message's AppendTo() above this one.
    void EmitStruct(bool nullable) {
        out_ << "    {\n"
             << "        // " << getter_ << "\n"
             << "        auto& sb = static_cast<arrow::StructBuilder&>(*b.field_builder(" << si_
             << "));\n";
        if (nullable) {
            out_ << "        if (const auto* v = " << getter_
                 << ") ARROW_RETURN_NOT_OK(AppendTo(sb, *v));\n"
                 << "        else ARROW_RETURN_NOT_OK(sb.AppendNull());\n";
        } else {
            out_ << "        ARROW_RETURN_NOT_OK(AppendTo(sb, " << getter_ << "));\n";
        }
        out_ << "    }\n";
    }

    void EmitList(const ir::IrNode& list_node) {
        const ir::IrNode& elem = *std::get<ir::ListNode>(list_node.node).element;
        switch (elem.kind) {
            case ir::NodeKind::SCALAR:
                EmitRepeatedScalar(std::get<ir::ScalarNode>(elem.node));
                break;
            case ir::NodeKind::STRUCT:
                EmitRepeatedStruct();
                break;
            case ir::NodeKind::LIST:
                EmitNestedList(list_node);
                break;
            default:
                break;
        }
    }

    void EmitRepeatedScalar(const ir::ScalarNode& e) {
        const CppScalarInfo& el = LookupScalar(e.logical_type, e.enum_identity);
        out_ << "    {\n"
             << "        // " << getter_ << "\n"
             << "        auto& lb = static_cast<arrow::ListBuilder&>(*b.field_builder(" << si_
             << "));\n"
             << "        ARROW_RETURN_NOT_OK(lb.Append());\n"
             << "        auto& vb = static_cast<" << el.builder_type
             << "&>(*lb.value_builder());\n";
        EmitLeafRun(el, getter_, "        ");
        out_ << "    }\n";
    }

    void EmitRepeatedStruct() {
        out_ << "    {\n"
             << "        // " << getter_ << "\n"
             << "        auto& lb = static_cast<arrow::ListBuilder&>(*b.field_builder(" << si_
             << "));\n"
             << "        ARROW_RETURN_NOT_OK(lb.Append());\n"
             << "        auto& sb = static_cast<arrow::StructBuilder&>(*lb.value_builder());\n"
             << "        for (const auto& v : " << getter_
             << ") ARROW_RETURN_NOT_OK(AppendTo(sb, v));\n"
             << "    }\n";
    }

    // list<list<...<leaf>>>, depth-generic up to the cap this file's emitters
    // share, with a struct OR a scalar leaf (GIR-10). Nullability comes from the
    // chained (fletcher.flatten) wrapper walk, whose getter is a pointer.
    void EmitNestedList(const ir::IrNode& list_node) {
        const NestedListShape shape = ClassifyNestedList(list_node);
        if (shape.depth > kMaxRenderableNestedListDepth) {
            EmitNestedListDepthUnsupported(out_, getter_, shape.depth, "AppendTo");
            return;
        }
        const bool nullable = list_node.facts.nullable;
        const bool scalar_leaf = shape.leaf->kind == ir::NodeKind::SCALAR;

        out_ << "    {\n"
             << "        // " << getter_ << "\n"
             << "        auto& lb0 = static_cast<arrow::ListBuilder&>(*b.field_builder(" << si_
             << "));\n";

        std::string indent = "        ";
        if (nullable) {
            out_ << indent << "if (" << getter_ << " == nullptr) {\n"
                 << indent << "    ARROW_RETURN_NOT_OK(lb0.AppendNull());\n"
                 << indent << "} else {\n";
            indent += "    ";
        }

        out_ << indent << "ARROW_RETURN_NOT_OK(lb0.Append());\n";
        for (int d = 1; d < shape.depth; ++d) {
            out_ << indent << "auto& lb" << d << " = static_cast<arrow::ListBuilder&>(*lb"
                 << (d - 1) << ".value_builder());\n";
        }

        const std::string innermost = "lb" + std::to_string(shape.depth - 1);
        const CppScalarInfo* leaf_scalar = nullptr;
        if (scalar_leaf) {
            const auto& e = std::get<ir::ScalarNode>(shape.leaf->node);
            leaf_scalar = &LookupScalar(e.logical_type, e.enum_identity);
            out_ << indent << "auto& vb = static_cast<" << leaf_scalar->builder_type << "&>(*"
                 << innermost << ".value_builder());\n";
        } else {
            out_ << indent << "auto& sb = static_cast<arrow::StructBuilder&>(*" << innermost
                 << ".value_builder());\n";
        }

        // One loop per list level above the innermost; each opens its own list.
        std::string src = nullable ? ("*" + getter_) : getter_;
        for (int d = 0; d < shape.depth - 1; ++d) {
            const std::string var = "l" + std::to_string(d);
            out_ << indent << "for (const auto& " << var << " : " << src << ") {\n";
            indent += "    ";
            out_ << indent << "ARROW_RETURN_NOT_OK(lb" << (d + 1) << ".Append());\n";
            src = var;
        }
        if (scalar_leaf) {
            EmitLeafRun(*leaf_scalar, src, indent);
        } else {
            out_ << indent << "for (const auto& v : " << src
                 << ") ARROW_RETURN_NOT_OK(AppendTo(sb, v));\n";
        }
        for (int d = 0; d < shape.depth - 1; ++d) {
            indent.resize(indent.size() - 4);
            out_ << indent << "}\n";
        }

        if (nullable) out_ << "        }\n";  // close else
        out_ << "    }\n";
    }

    void EmitMap(const ir::MapNode& map) {
        const ir::ScalarNode& key = std::get<ir::ScalarNode>(map.key->node);
        const CppScalarInfo& mk = LookupScalar(key.logical_type, key.enum_identity);

        out_ << "    {\n"
             << "        // " << getter_ << "\n"
             << "        auto& mb = static_cast<arrow::MapBuilder&>(*b.field_builder(" << si_
             << "));\n"
             << "        ARROW_RETURN_NOT_OK(mb.Append());\n"
             << "        auto& kb = static_cast<" << mk.builder_type << "&>(*mb.key_builder());\n";
        if (map.value->kind == ir::NodeKind::STRUCT) {
            out_ << "        auto& vsb = static_cast<arrow::StructBuilder&>(*mb.item_builder());\n"
                 << "        for (const auto& [k, v] : " << getter_ << ") {\n"
                 << "            ARROW_RETURN_NOT_OK(kb.Append(k));\n"
                 << "            ARROW_RETURN_NOT_OK(AppendTo(vsb, v));\n"
                 << "        }\n";
        } else {
            const ir::ScalarNode& val = std::get<ir::ScalarNode>(map.value->node);
            const CppScalarInfo& mv = LookupScalar(val.logical_type, val.enum_identity);
            out_ << "        auto& vb = static_cast<" << mv.builder_type
                 << "&>(*mb.item_builder());\n"
                 << "        for (const auto& [k, v] : " << getter_ << ") {\n"
                 << "            ARROW_RETURN_NOT_OK(kb.Append(k));\n"
                 << "            ARROW_RETURN_NOT_OK(vb.Append(v));\n"
                 << "        }\n";
        }
        out_ << "    }\n";
    }

    // Append a whole run of scalar leaves into `vb`. A fixed-width leaf goes in
    // one AppendValues() over the storage vector; a string/bytes leaf has no
    // vector overload, so it appends per element.
    void EmitLeafRun(const CppScalarInfo& leaf, const std::string& src, const std::string& indent) {
        if (leaf.value_is_buffer) {
            out_ << indent << "for (const auto& v : " << src
                 << ") ARROW_RETURN_NOT_OK(vb.Append(v));\n";
        } else {
            out_ << indent << "ARROW_RETURN_NOT_OK(vb.AppendValues(" << src << "));\n";
        }
    }

    std::ostringstream& out_;
    const std::string getter_;  // public getter expression, e.g. "msg.field()"
    const std::string si_;      // b.field_builder() slot this field writes
};

// ---- ToArrowRow() visitor -------------------------------------------------

class EdgeToArrowRowVisitor {
   public:
    EdgeToArrowRowVisitor(std::ostringstream& out, std::string getter_expr,
                          const google::protobuf::FileDescriptor* context_file)
        : out_(out), getter_(std::move(getter_expr)), context_file_(context_file) {}

    void EmitField(const ir::IrNode& node) {
        switch (node.kind) {
            case ir::NodeKind::SCALAR:
                EmitScalar(std::get<ir::ScalarNode>(node.node), node.facts.nullable);
                break;
            case ir::NodeKind::STRUCT:
                EmitStruct(std::get<ir::StructNode>(node.node), node.facts.nullable);
                break;
            case ir::NodeKind::LIST:
                EmitList(node);
                break;
            case ir::NodeKind::MAP:
                EmitMap(std::get<ir::MapNode>(node.node));
                break;
            case ir::NodeKind::FIXED_SIZE_LIST:
            case ir::NodeKind::UNSUPPORTED:
                break;
        }
    }

   private:
    // Non-nullable scalars read msg.field(), whose generated getter already
    // applies value_or(default) (generator.cpp:372) — so ToArrowRow() branches
    // only on nullable vs non-nullable and never re-applies a default. String /
    // bytes construct the Arrow scalar from an OWNED std::string so Arrow owns the
    // bytes (the view getter returns a std::string_view; ToArrowRow() copies).
    void EmitScalar(const ir::ScalarNode& s, bool nullable) {
        const CppScalarInfo& sc = LookupScalar(s.logical_type, s.enum_identity);
        if (nullable) {
            std::string val_expr;
            if (sc.value_is_buffer)
                val_expr = ReplaceAll(sc.scalar_ctor, "{val}", "std::string(*" + getter_ + ")");
            else
                val_expr = ReplaceAll(sc.scalar_ctor, "{val}", "*" + getter_);
            out_ << "    row.push_back(" << getter_ << ".has_value()\n"
                 << "        ? std::shared_ptr<arrow::Scalar>(" << val_expr << ")\n"
                 << "        : arrow::MakeNullScalar(" << sc.arrow_type_expr << "));\n";
        } else {
            std::string val_expr;
            if (sc.value_is_buffer)
                val_expr = ReplaceAll(sc.scalar_ctor, "{val}", "std::string(" + getter_ + ")");
            else
                val_expr = ReplaceAll(sc.scalar_ctor, "{val}", getter_);
            out_ << "    row.push_back(" << val_expr << ");\n";
        }
    }

    void EmitStruct(const ir::StructNode& st, bool nullable) {
        const std::string nc = CppClassName(st.identity.descriptor, context_file_);
        out_ << "    {\n"
             << "        auto type = arrow::struct_(\n"
             << "            detail::ImportSchema(" << nc << "Schema())->fields());\n";
        if (nullable) {
            out_ << "        if (" << getter_ << " != nullptr)\n"
                 << "            row.push_back(std::make_shared"
                    "<arrow::StructScalar>(\n"
                 << "                ToArrowRow(*" << getter_ << "), type));\n"
                 << "        else\n"
                 << "            row.push_back(arrow::MakeNullScalar"
                    "(type));\n";
        } else {
            out_ << "        row.push_back(std::make_shared"
                    "<arrow::StructScalar>(\n"
                 << "            ToArrowRow(" << getter_ << "), type));\n";
        }
        out_ << "    }\n";
    }

    void EmitList(const ir::IrNode& list_node) {
        const ir::IrNode& elem = *std::get<ir::ListNode>(list_node.node).element;
        switch (elem.kind) {
            case ir::NodeKind::SCALAR:
                EmitRepeatedScalar(std::get<ir::ScalarNode>(elem.node));
                break;
            case ir::NodeKind::STRUCT:
                EmitRepeatedStruct(std::get<ir::StructNode>(elem.node));
                break;
            case ir::NodeKind::LIST:
                EmitNestedList(list_node);
                break;
            default:
                break;
        }
    }

    void EmitRepeatedScalar(const ir::ScalarNode& e) {
        const CppScalarInfo& el = LookupScalar(e.logical_type, e.enum_identity);
        out_ << "    {\n"
             << "        " << el.builder_type << " builder;\n"
             << "        for (const auto& v : " << getter_ << ")\n"
             << "            (void)builder.Append(v);\n"
             << "        row.push_back(std::make_shared<arrow::ListScalar>(\n"
             << "            *builder.Finish(),\n"
             << "            arrow::list(arrow::field(\"item\", " << el.arrow_type_expr
             << ", true))));\n"
             << "    }\n";
    }

    void EmitRepeatedStruct(const ir::StructNode& st) {
        const std::string nc = CppClassName(st.identity.descriptor, context_file_);
        out_ << "    {\n"
             << "        auto type = arrow::struct_(\n"
             << "            detail::ImportSchema(" << nc << "Schema())->fields());\n"
             << "        auto builder = detail::FletcherValueOrThrow(\n"
                "            arrow::MakeBuilder(type), \"arrow::MakeBuilder\");\n"
             << "        auto& sb = static_cast<arrow::StructBuilder&>(*builder);\n"
             << "        for (const auto& v : " << getter_ << ")\n"
             << "            detail::FletcherThrowIfNotOk(AppendTo(sb, v),\n"
                "                                         \"ToArrowRow: AppendTo\");\n"
             << "        row.push_back(std::make_shared<arrow::ListScalar>(\n"
             << "            detail::FletcherValueOrThrow(sb.Finish(), "
                "\"ToArrowRow: Finish\"),\n"
             << "            arrow::list(arrow::field(\"item\", type,"
                " true))));\n"
             << "    }\n";
    }

    void EmitNestedList(const ir::IrNode& list_node) {
        const NestedListShape shape = ClassifyNestedList(list_node);
        if (shape.depth > kMaxRenderableNestedListDepth) {
            // Was: fell through both depth branches and emitted a block with no
            // row.push_back, shifting every later field's slot against the schema.
            EmitNestedListDepthUnsupported(out_, getter_, shape.depth, "ToArrowRow");
            return;
        }
        if (shape.leaf->kind == ir::NodeKind::SCALAR) {
            EmitNestedScalarList(list_node, shape);
            return;
        }
        const bool nullable = list_node.facts.nullable;
        const std::string nc = CppClassName(
            std::get<ir::StructNode>(shape.leaf->node).identity.descriptor, context_file_);

        out_ << "    {\n"
             << "        auto coord_type = arrow::struct_(\n"
             << "            detail::ImportSchema(" << nc << "Schema())->fields());\n";

        const std::string data_ref = nullable ? ("(*" + getter_ + ")") : getter_;

        if (shape.depth == 2) {
            out_ << "        auto inner_list_type = arrow::list(\n"
                 << "            arrow::field(\"item\", coord_type,"
                    " true));\n";
            if (nullable) {
                out_ << "        if (" << getter_ << " == nullptr) {\n"
                     << "            row.push_back(arrow::MakeNullScalar(\n"
                     << "                arrow::list(arrow::field(\"item\","
                        " inner_list_type, true))));\n"
                     << "        } else {\n";
            }
            out_ << "        auto builder = detail::FletcherValueOrThrow(\n"
                    "            arrow::MakeBuilder(inner_list_type), \"arrow::MakeBuilder\");\n"
                 << "        auto& lb = static_cast<arrow::ListBuilder&>(*builder);\n"
                 << "        auto& sb = static_cast<arrow::StructBuilder&>"
                    "(*lb.value_builder());\n"
                 << "        for (const auto& ring : " << data_ref << ") {\n"
                 << "            detail::FletcherThrowIfNotOk(lb.Append(), "
                    "\"ToArrowRow: Append\");\n"
                 << "            for (const auto& v : ring)\n"
                 << "                detail::FletcherThrowIfNotOk(AppendTo(sb, v),\n"
                    "                                             \"ToArrowRow: AppendTo\");\n"
                 << "        }\n"
                 << "        row.push_back(std::make_shared"
                    "<arrow::ListScalar>(\n"
                 << "            detail::FletcherValueOrThrow(lb.Finish(), "
                    "\"ToArrowRow: Finish\")));\n";
        } else if (shape.depth == 3) {
            out_ << "        auto ring_list_type = arrow::list(\n"
                 << "            arrow::field(\"item\", coord_type,"
                    " true));\n"
                 << "        auto poly_list_type = arrow::list(\n"
                 << "            arrow::field(\"item\", ring_list_type,"
                    " true));\n";
            if (nullable) {
                out_ << "        if (" << getter_ << " == nullptr) {\n"
                     << "            row.push_back(arrow::MakeNullScalar(\n"
                     << "                arrow::list(arrow::field(\"item\","
                        " poly_list_type, true))));\n"
                     << "        } else {\n";
            }
            out_ << "        auto builder = detail::FletcherValueOrThrow(\n"
                    "            arrow::MakeBuilder(poly_list_type), \"arrow::MakeBuilder\");\n"
                 << "        auto& lb0 = static_cast<arrow::ListBuilder&>(*builder);\n"
                 << "        auto& lb1 = static_cast<arrow::ListBuilder&>"
                    "(*lb0.value_builder());\n"
                 << "        auto& sb = static_cast<arrow::StructBuilder&>"
                    "(*lb1.value_builder());\n"
                 << "        for (const auto& poly : " << data_ref << ") {\n"
                 << "            detail::FletcherThrowIfNotOk(lb0.Append(), "
                    "\"ToArrowRow: Append\");\n"
                 << "            for (const auto& ring : poly) {\n"
                 << "                detail::FletcherThrowIfNotOk(lb1.Append(), "
                    "\"ToArrowRow: Append\");\n"
                 << "                for (const auto& v : ring)\n"
                 << "                    detail::FletcherThrowIfNotOk(AppendTo(sb, v),\n"
                    "                                                 "
                    "\"ToArrowRow: AppendTo\");\n"
                 << "            }\n"
                 << "        }\n"
                 << "        row.push_back(std::make_shared"
                    "<arrow::ListScalar>(\n"
                 << "            detail::FletcherValueOrThrow(lb0.Finish(), "
                    "\"ToArrowRow: Finish\")));\n";
        }

        if (nullable) {
            out_ << "        }\n";  // close else
        }
        out_ << "    }\n";
    }

    // GIR-10 scalar-leaf nested list -> arrow list<list<...<scalar>>>. Mirrors the
    // struct-leaf EmitNestedList shape but builds the innermost ring with the leaf
    // scalar's typed Arrow builder (Append(v)) instead of a StructScalar.
    void EmitNestedScalarList(const ir::IrNode& list_node, const NestedListShape& shape) {
        const bool nullable = list_node.facts.nullable;
        const auto& e = std::get<ir::ScalarNode>(shape.leaf->node);
        const CppScalarInfo& el = LookupScalar(e.logical_type, e.enum_identity);
        const std::string data_ref = nullable ? ("(*" + getter_ + ")") : getter_;

        out_ << "    {\n"
             << "        auto coord_type = " << el.arrow_type_expr << ";\n";

        if (shape.depth == 2) {
            out_ << "        auto inner_list_type = arrow::list(\n"
                 << "            arrow::field(\"item\", coord_type, true));\n";
            if (nullable) {
                out_ << "        if (" << getter_ << " == nullptr) {\n"
                     << "            row.push_back(arrow::MakeNullScalar(\n"
                     << "                arrow::list(arrow::field(\"item\","
                        " inner_list_type, true))));\n"
                     << "        } else {\n";
            }
            out_
                << "        auto outer_builder = detail::FletcherValueOrThrow(\n"
                   "            arrow::MakeBuilder(inner_list_type), \"arrow::MakeBuilder\");\n"
                << "        for (const auto& ring : " << data_ref << ") {\n"
                << "            " << el.builder_type << " inner_builder;\n"
                << "            for (const auto& v : ring) (void)inner_builder.Append(v);\n"
                << "            (void)outer_builder->AppendScalar(\n"
                << "                arrow::ListScalar(*inner_builder.Finish(), inner_list_type));\n"
                << "        }\n"
                << "        row.push_back(std::make_shared"
                   "<arrow::ListScalar>(\n"
                << "            *outer_builder->Finish()));\n";
        } else if (shape.depth == 3) {
            out_ << "        auto ring_list_type = arrow::list(\n"
                 << "            arrow::field(\"item\", coord_type, true));\n"
                 << "        auto poly_list_type = arrow::list(\n"
                 << "            arrow::field(\"item\", ring_list_type, true));\n";
            if (nullable) {
                out_ << "        if (" << getter_ << " == nullptr) {\n"
                     << "            row.push_back(arrow::MakeNullScalar(\n"
                     << "                arrow::list(arrow::field(\"item\","
                        " poly_list_type, true))));\n"
                     << "        } else {\n";
            }
            out_ << "        auto outer_builder = detail::FletcherValueOrThrow(\n"
                    "            arrow::MakeBuilder(poly_list_type), \"arrow::MakeBuilder\");\n"
                 << "        for (const auto& poly : " << data_ref << ") {\n"
                 << "            auto mid_builder = detail::FletcherValueOrThrow(\n"
                    "                arrow::MakeBuilder(ring_list_type), \"arrow::MakeBuilder\");\n"
                 << "            for (const auto& ring : poly) {\n"
                 << "                " << el.builder_type << " inner_builder;\n"
                 << "                for (const auto& v : ring) (void)inner_builder.Append(v);\n"
                 << "                (void)mid_builder->AppendScalar(\n"
                 << "                    arrow::ListScalar(*inner_builder.Finish(),"
                    " ring_list_type));\n"
                 << "            }\n"
                 << "            (void)outer_builder->AppendScalar(\n"
                 << "                arrow::ListScalar(*mid_builder->Finish(), poly_list_type));\n"
                 << "        }\n"
                 << "        row.push_back(std::make_shared"
                    "<arrow::ListScalar>(\n"
                 << "            *outer_builder->Finish()));\n";
        }

        if (nullable) {
            out_ << "        }\n";  // close else
        }
        out_ << "    }\n";
    }

    void EmitMap(const ir::MapNode& map) {
        const ir::ScalarNode& key = std::get<ir::ScalarNode>(map.key->node);
        const CppScalarInfo& mk = LookupScalar(key.logical_type, key.enum_identity);
        const bool val_is_msg = map.value->kind == ir::NodeKind::STRUCT;

        out_ << "    {\n"
             << "        " << mk.builder_type << " key_builder;\n";

        if (val_is_msg) {
            const std::string mvc = CppClassName(
                std::get<ir::StructNode>(map.value->node).identity.descriptor, context_file_);
            out_ << "        auto val_type = arrow::struct_(\n"
                 << "            detail::ImportSchema(" << mvc << "Schema())->fields());\n"
                 << "        auto val_builder = detail::FletcherValueOrThrow(\n"
                    "            arrow::MakeBuilder(val_type), \"arrow::MakeBuilder\");\n"
                 << "        auto& vsb = static_cast<arrow::StructBuilder&>(*val_builder);\n"
                 << "        for (const auto& [k, v] : " << getter_ << ") {\n"
                 << "            (void)key_builder.Append(k);\n"
                 << "            detail::FletcherThrowIfNotOk(AppendTo(vsb, v),\n"
                    "                                         \"ToArrowRow: AppendTo\");\n"
                 << "        }\n"
                 << "        auto keys = *key_builder.Finish();\n"
                 << "        auto vals = detail::FletcherValueOrThrow(vsb.Finish(),\n"
                    "                                                 "
                    "\"ToArrowRow: Finish\");\n";
        } else {
            const ir::ScalarNode& val = std::get<ir::ScalarNode>(map.value->node);
            const CppScalarInfo& mv = LookupScalar(val.logical_type, val.enum_identity);
            out_ << "        " << mv.builder_type << " val_builder;\n"
                 << "        for (const auto& [k, v] : " << getter_ << ") {\n"
                 << "            (void)key_builder.Append(k);\n"
                 << "            (void)val_builder.Append(v);\n"
                 << "        }\n"
                 << "        auto keys = *key_builder.Finish();\n"
                 << "        auto vals = *val_builder.Finish();\n";
        }

        if (val_is_msg) {
            out_ << "        auto val_field = arrow::field(\"value\","
                    " val_type, true);\n";
        } else {
            const ir::ScalarNode& val = std::get<ir::ScalarNode>(map.value->node);
            const CppScalarInfo& mv = LookupScalar(val.logical_type, val.enum_identity);
            out_ << "        auto val_field = arrow::field(\"value\", " << mv.arrow_type_expr
                 << ", true);\n";
        }
        out_ << "        auto kv = *arrow::StructArray::Make(\n"
             << "            {keys, vals},\n"
             << "            {arrow::field(\"key\", " << mk.arrow_type_expr
             << ", false), val_field});\n"
             << "        row.push_back(std::make_shared"
                "<arrow::MapScalar>(kv,\n"
             << "            arrow::map(" << mk.arrow_type_expr << ", val_field)));\n"
             << "    }\n";
    }

    std::ostringstream& out_;
    const std::string getter_;  // public getter expression, e.g. "msg.field()"
    const google::protobuf::FileDescriptor* context_file_;
};

}  // namespace

void EmitViewGetterFromIr(std::ostringstream& out, const ir::IrNode& node,
                          const std::string& getter_name, std::size_t field_index,
                          const google::protobuf::FileDescriptor* context_file) {
    EdgeViewGetterVisitor visitor(out, getter_name, field_index, context_file);
    visitor.EmitField(node);
}

void EmitToArrowRowFieldFromIr(std::ostringstream& out, const ir::IrNode& node,
                               const std::string& getter_expr, std::size_t /*field_index*/,
                               const google::protobuf::FileDescriptor* context_file) {
    EdgeToArrowRowVisitor visitor(out, getter_expr, context_file);
    visitor.EmitField(node);
}

void EmitAppendToFieldFromIr(std::ostringstream& out, const ir::IrNode& node,
                             const std::string& getter_expr, std::size_t field_index,
                             const google::protobuf::FileDescriptor* /*context_file*/) {
    // context_file is accepted for signature parity with the two visitors above
    // and is deliberately unused: AppendTo() names no generated class — every
    // nested message is reached through the ADL-resolved AppendTo() overload,
    // and every builder is either an Arrow composite literal or a lookup-table
    // scalar builder.
    EdgeAppendToVisitor visitor(out, getter_expr, field_index);
    visitor.EmitField(node);
}

}  // namespace fletcher::cpp_backend
