// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include "cpp_backend_decode_visitor.hpp"

#include <string>
#include <variant>

#include "cpp_backend_type_table.hpp"

namespace fletcher::cpp_backend {

// ---------------------------------------------------------------------------
// Edge DECODE recursive visitor (decode only). Maps IR recursion to generated
// code recursion, reproducing the former FieldMapping-switch EmitFieldDecode
// byte-for-byte: field order, IsNull() gating, list/map layout, map value
// bitfield sync, nested-list resize/indexed-assign, and owned-copy string /
// binary extraction are all unchanged (GIR-2 wire behaviour is preserved).
//
// The visitor dispatches on the top-level field's IR node kind. Struct/list/map
// element decode delegates to the nested message's own generated constructor
// (`<Class>(sr)` / `member_.emplace(sr)`), exactly as before — the recursion is
// realised through those generated constructors, not by re-walking struct
// children here.
// ---------------------------------------------------------------------------

namespace {

// The PositionalReader method name for a scalar logical identity. The C++
// backend table already carries this (CppScalarInfo::positional_read); it is
// the derivation the old generator did at call time in PositionalReadCall
// (keyed on storage_type + arrow_type_expr, incl. ReadTimestamp/ReadDuration
// and ReadBinary vs ReadString). Kept a backend concern — no read-method string
// is added to any IR node (locked decision #1).
const std::string& ReadMethod(const ir::ScalarNode& s) {
    return LookupScalar(s.logical_type, s.enum_identity).positional_read;
}

const std::string& StorageType(const ir::ScalarNode& s) {
    return LookupScalar(s.logical_type, s.enum_identity).storage_type;
}

class EdgeDecodeVisitor {
   public:
    EdgeDecodeVisitor(std::ostringstream& out, std::string value_expr, size_t field_index,
                      const google::protobuf::FileDescriptor* context_file)
        : out_(out),
          n_(std::move(value_expr)),
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
                EmitUnsupported(node);
                break;
        }
    }

   private:
    // ---- scalar (top-level, std::optional<> storage) ----------------------
    void EmitScalar(const ir::ScalarNode& s, bool nullable) {
        const std::string& method = ReadMethod(s);
        if (method == "ReadBinary") {
            // Binary: ReadBinary() returns pair<const uint8_t*, size_t>; copy the
            // bytes into the owned std::string member (no zero-copy borrow).
            if (nullable) {
                out_ << "        if (!r.IsNull(" << si_ << ")) {\n"
                     << "            auto [p, n] = r.ReadBinary();\n"
                     << "            " << n_ << ".emplace(reinterpret_cast<const char*>(p), n);\n"
                     << "        }\n";
            } else {
                out_ << "        { auto [p, n] = r.ReadBinary();\n"
                     << "          " << n_ << ".emplace(reinterpret_cast<const char*>(p), n); }\n";
            }
        } else if (method == "ReadString") {
            if (nullable) {
                out_ << "        if (!r.IsNull(" << si_ << ")) " << n_ << " = std::string(r."
                     << method << "());\n";
            } else {
                out_ << "        " << n_ << " = std::string(r." << method << "());\n";
            }
        } else {
            if (nullable) {
                out_ << "        if (!r.IsNull(" << si_ << ")) " << n_ << " = r." << method
                     << "();\n";
            } else {
                out_ << "        " << n_ << " = r." << method << "();\n";
            }
        }
    }

    // ---- struct (top-level, std::optional<> storage) ----------------------
    void EmitStruct(const ir::StructNode& st, bool nullable) {
        const std::string cls = CppClassName(st.identity.descriptor, context_file_);
        if (nullable) {
            out_ << "        if (!r.IsNull(" << si_ << ")) {\n"
                 << "            auto sr = r.ReadStruct(" << cls << "Schema()->n_children);\n"
                 << "            " << n_ << ".emplace(sr);\n"
                 << "        }\n";
        } else {
            out_ << "        { auto sr = r.ReadStruct(" << cls << "Schema()->n_children);\n"
                 << "          " << n_ << ".emplace(sr); }\n";
        }
    }

    // ---- list: single-level scalar / struct, or nested (struct innermost) --
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
                EmitUnsupported(list_node);
                break;
        }
    }

    void EmitRepeatedScalar(const ir::ScalarNode& e) {
        const std::string& method = ReadMethod(e);
        out_ << "        { auto lh = r.ReadListHeader();\n"
             << "          " << n_ << ".clear();\n"
             << "          " << n_ << ".reserve(lh.count);\n"
             << "          for (uint32_t li_ = 0; li_ < lh.count; ++li_) {\n";
        if (method == "ReadBinary") {
            out_ << "            auto [p, n] = r.ReadBinary();\n"
                 << "            " << n_ << ".emplace_back(reinterpret_cast<const char*>(p), n);\n";
        } else if (method == "ReadString") {
            out_ << "            " << n_ << ".emplace_back(r." << method << "());\n";
        } else {
            out_ << "            " << n_ << ".push_back(r." << method << "());\n";
        }
        out_ << "          } }\n";
    }

    void EmitRepeatedStruct(const ir::StructNode& st) {
        const std::string nc = CppClassName(st.identity.descriptor, context_file_);
        out_ << "        { auto lh = r.ReadListHeader();\n"
             << "          " << n_ << ".clear();\n"
             << "          " << n_ << ".reserve(lh.count);\n"
             << "          for (uint32_t li_ = 0; li_ < lh.count; ++li_) {\n"
             << "            auto sr = r.ReadStruct(" << nc << "Schema()->n_children);\n"
             << "            " << n_ << ".emplace_back(sr);\n"
             << "          } }\n";
    }

    void EmitNestedList(const ir::IrNode& list_node) {
        const bool nullable = list_node.facts.nullable;

        // Depth = number of list levels (List<List<leaf>> = 2). Matches the
        // IR->FieldMapping projection's NESTED_LIST depth computation. The
        // innermost leaf is a struct (List<List<...<Struct>>>) or, GIR-10, a
        // scalar (List<List<...<Scalar>>>).
        int depth = 1;
        const ir::IrNode* cur = std::get<ir::ListNode>(list_node.node).element.get();
        while (cur->kind == ir::NodeKind::LIST) {
            depth += 1;
            cur = std::get<ir::ListNode>(cur->node).element.get();
        }
        const bool struct_leaf = cur->kind == ir::NodeKind::STRUCT;
        const bool scalar_leaf = cur->kind == ir::NodeKind::SCALAR;
        if (!struct_leaf && !scalar_leaf) {
            EmitUnsupported(list_node);
            return;
        }

        if (nullable) {
            out_ << "        if (!r.IsNull(" << si_ << ")) {\n";
        } else {
            out_ << "        {\n";
        }

        const std::string target = nullable ? (n_ + ".emplace()") : n_;
        const std::string ref = nullable ? ("(*" + n_ + ")") : n_;
        std::string indent = "            ";

        if (nullable) {
            out_ << indent << target << ";\n";
        }

        std::string cur_ref = ref;
        for (int d = 0; d < depth; ++d) {
            const std::string var = "lh_" + std::to_string(d);
            const std::string idx_var = "i_" + std::to_string(d);
            out_ << indent << "auto " << var << " = r.ReadListHeader();\n"
                 << indent << cur_ref << ".resize(" << var << ".count);\n"
                 << indent << "for (uint32_t " << idx_var << " = 0; " << idx_var << " < " << var
                 << ".count; ++" << idx_var << ") {\n";
            cur_ref = cur_ref + "[" + idx_var + "]";
            indent += "    ";
        }
        if (struct_leaf) {
            const std::string nc = CppClassName(
                std::get<ir::StructNode>(cur->node).identity.descriptor, context_file_);
            out_ << indent << "auto sr = r.ReadStruct(" << nc << "Schema()->n_children);\n"
                 << indent << cur_ref << " = " << nc << "(sr);\n";
        } else {
            // Scalar leaf: read the innermost element through the C++ backend's
            // positional read method, with owned-copy string/binary handling that
            // mirrors EmitRepeatedScalar's leaf extraction.
            const std::string& method = ReadMethod(std::get<ir::ScalarNode>(cur->node));
            if (method == "ReadBinary") {
                out_ << indent << "{ auto [p, n] = r.ReadBinary();\n"
                     << indent << "  " << cur_ref
                     << ".assign(reinterpret_cast<const char*>(p), n); }\n";
            } else if (method == "ReadString") {
                out_ << indent << cur_ref << " = std::string(r." << method << "());\n";
            } else {
                out_ << indent << cur_ref << " = r." << method << "();\n";
            }
        }
        for (int d = 0; d < depth; ++d) {
            indent = indent.substr(4);
            out_ << indent << "}\n";
        }

        out_ << "        }\n";
    }

    // ---- map: keys pass, value bitfield sync, then values pass ------------
    void EmitMap(const ir::MapNode& map) {
        const ir::ScalarNode& key = std::get<ir::ScalarNode>(map.key->node);
        const std::string& key_read = ReadMethod(key);
        const std::string& key_type = StorageType(key);
        const bool val_is_msg = map.value->kind == ir::NodeKind::STRUCT;

        out_ << "        { auto count = r.ReadMapCount();\n"
             << "          std::vector<" << key_type << "> keys_;\n"
             << "          keys_.reserve(count);\n"
             << "          for (uint32_t mi_ = 0; mi_ < count; ++mi_) {\n";
        if (key_read == "ReadString") {
            out_ << "            keys_.emplace_back(r." << key_read << "());\n";
        } else {
            out_ << "            keys_.push_back(r." << key_read << "());\n";
        }
        out_ << "          }\n"
             << "          auto vbf = r.ReadMapValueBitfield(count);\n"
             << "          " << n_ << ".clear();\n"
             << "          " << n_ << ".reserve(count);\n"
             << "          for (uint32_t mi_ = 0; mi_ < count; ++mi_) {\n";

        if (val_is_msg) {
            const std::string mvc = CppClassName(
                std::get<ir::StructNode>(map.value->node).identity.descriptor, context_file_);
            out_ << "            auto sr = r.ReadStruct(" << mvc << "Schema()->n_children);\n"
                 << "            " << n_ << ".emplace_back(std::move(keys_[mi_]), " << mvc
                 << "(sr));\n";
        } else {
            const std::string& val_read = ReadMethod(std::get<ir::ScalarNode>(map.value->node));
            if (val_read == "ReadString") {
                out_ << "            " << n_ << ".emplace_back(std::move(keys_[mi_]), "
                     << "std::string(r." << val_read << "()));\n";
            } else if (val_read == "ReadBinary") {
                out_ << "            auto [p, n] = r.ReadBinary();\n"
                     << "            " << n_ << ".emplace_back(std::move(keys_[mi_]), "
                     << "std::string(reinterpret_cast<const char*>(p), n));\n";
            } else {
                out_ << "            " << n_ << ".emplace_back(std::move(keys_[mi_]), " << "r."
                     << val_read << "());\n";
            }
        }

        out_ << "          } }\n";
    }

    // ---- unsupported / not-yet-emitted shape: diagnostic, never silent -----
    // GatherFields drops fields whose projection is nullopt (Unsupported,
    // FixedSizeList, List<List<Scalar>>), so these do not reach decode in
    // practice; emit an explicit comment rather than a corrupting decode body
    // (GIR-8 turns this into a build error).
    void EmitUnsupported(const ir::IrNode& node) {
        std::string reason = "unsupported decode shape";
        if (node.kind == ir::NodeKind::UNSUPPORTED)
            reason = std::get<ir::UnsupportedNode>(node.node).reason;
        out_ << "        // unsupported field skipped: " << reason << "\n";
    }

    std::ostringstream& out_;
    const std::string n_;   // storage member expression, e.g. "field_"
    const std::string si_;  // top-level positional index string, for IsNull()
    const google::protobuf::FileDescriptor* context_file_;
};

}  // namespace

void EmitFieldDecodeFromIr(std::ostringstream& out, const ir::IrNode& node,
                           const std::string& value_expr, size_t field_index,
                           const google::protobuf::FileDescriptor* context_file) {
    EdgeDecodeVisitor visitor(out, value_expr, field_index, context_file);
    visitor.EmitField(node);
}

}  // namespace fletcher::cpp_backend
