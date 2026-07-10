// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include "cpp_backend_type_table.hpp"

#include <string>
#include <variant>

#include "type_mapper.hpp"

namespace fletcher::cpp_backend {

// ---------------------------------------------------------------------------
// Scalar lookup: language-neutral logical identity -> C++ backend strings.
// These are the ONLY C++/Arrow type strings in the pipeline (locked #1).
// The field order matches CppScalarInfo:
//   {arrow_type_expr, storage_type, param_type, scalar_ctor, default_value,
//    builder_type, scalar_type, value_is_buffer, positional_write, positional_read}
// ---------------------------------------------------------------------------

const CppScalarInfo& LookupScalar(const ir::LogicalType& type,
                                  const std::optional<ir::EnumIdentity>& enum_identity) {
    // clang-format off
    static const CppScalarInfo kBool{
        "arrow::boolean()", "bool", "bool",
        "std::make_shared<arrow::BooleanScalar>({val})", "false",
        "arrow::BooleanBuilder", "arrow::BooleanScalar", false, "WriteBool", "ReadBool",
        "arrow::BooleanArray", "bool"};
    static const CppScalarInfo kInt32{
        "arrow::int32()", "int32_t", "int32_t",
        "std::make_shared<arrow::Int32Scalar>({val})", "0",
        "arrow::Int32Builder", "arrow::Int32Scalar", false, "WriteInt32", "ReadInt32",
        "arrow::Int32Array", "int32_t"};
    static const CppScalarInfo kInt64{
        "arrow::int64()", "int64_t", "int64_t",
        "std::make_shared<arrow::Int64Scalar>({val})", "INT64_C(0)",
        "arrow::Int64Builder", "arrow::Int64Scalar", false, "WriteInt64", "ReadInt64",
        "arrow::Int64Array", "int64_t"};
    static const CppScalarInfo kUInt32{
        "arrow::uint32()", "uint32_t", "uint32_t",
        "std::make_shared<arrow::UInt32Scalar>({val})", "0u",
        "arrow::UInt32Builder", "arrow::UInt32Scalar", false, "WriteUint32", "ReadUint32",
        "arrow::UInt32Array", "uint32_t"};
    static const CppScalarInfo kUInt64{
        "arrow::uint64()", "uint64_t", "uint64_t",
        "std::make_shared<arrow::UInt64Scalar>({val})", "UINT64_C(0)",
        "arrow::UInt64Builder", "arrow::UInt64Scalar", false, "WriteUint64", "ReadUint64",
        "arrow::UInt64Array", "uint64_t"};
    static const CppScalarInfo kFloat{
        "arrow::float32()", "float", "float",
        "std::make_shared<arrow::FloatScalar>({val})", "0.0f",
        "arrow::FloatBuilder", "arrow::FloatScalar", false, "WriteFloat", "ReadFloat",
        "arrow::FloatArray", "float"};
    static const CppScalarInfo kDouble{
        "arrow::float64()", "double", "double",
        "std::make_shared<arrow::DoubleScalar>({val})", "0.0",
        "arrow::DoubleBuilder", "arrow::DoubleScalar", false, "WriteDouble", "ReadDouble",
        "arrow::DoubleArray", "double"};
    static const CppScalarInfo kString{
        "arrow::utf8()", "std::string", "std::string_view",
        "std::make_shared<arrow::StringScalar>({val})", "\"\"",
        "arrow::StringBuilder", "arrow::StringScalar", true, "WriteString", "ReadString",
        "arrow::StringArray", "std::string_view"};
    static const CppScalarInfo kBytes{
        "arrow::binary()", "std::string", "std::string_view",
        "std::make_shared<arrow::BinaryScalar>({val})", "\"\"",
        "arrow::BinaryBuilder", "arrow::BinaryScalar", true, "WriteBinary", "ReadBinary",
        "arrow::BinaryArray", "std::string_view"};
    static const CppScalarInfo kEnum{
        "arrow::int32()", "int32_t", "int32_t",
        "std::make_shared<arrow::Int32Scalar>(static_cast<int32_t>({val}))", "0",
        "arrow::Int32Builder", "arrow::Int32Scalar", false, "WriteInt32", "ReadInt32",
        "arrow::Int32Array", "int32_t"};
    static const CppScalarInfo kTimestamp{
        "arrow::timestamp(arrow::TimeUnit::NANO)", "int64_t", "int64_t",
        "std::make_shared<arrow::TimestampScalar>"
        "({val}, arrow::timestamp(arrow::TimeUnit::NANO))",
        "INT64_C(0)", "arrow::TimestampBuilder", "arrow::TimestampScalar", false,
        "WriteTimestamp", "ReadTimestamp", "arrow::TimestampArray", "int64_t"};
    static const CppScalarInfo kDuration{
        "arrow::duration(arrow::TimeUnit::NANO)", "int64_t", "int64_t",
        "std::make_shared<arrow::DurationScalar>"
        "({val}, arrow::duration(arrow::TimeUnit::NANO))",
        "INT64_C(0)", "arrow::DurationBuilder", "arrow::DurationScalar", false,
        "WriteDuration", "ReadDuration", "arrow::DurationArray", "int64_t"};
    static const CppScalarInfo kUnknown{
        "", "", "", "", "", "", "", false, "/* unknown write */", "/* unknown read */",
        "", ""};
    // clang-format on

    using LK = ir::LogicalKind;
    switch (type.kind) {
        case LK::BOOL:
            return kBool;
        case LK::INT32:
            return enum_identity.has_value() ? kEnum : kInt32;
        case LK::INT64:
            return kInt64;
        case LK::UINT32:
            return kUInt32;
        case LK::UINT64:
            return kUInt64;
        case LK::FLOAT32:
            return kFloat;
        case LK::FLOAT64:
            return kDouble;
        case LK::UTF8:
            return kString;
        case LK::BINARY:
        case LK::FIXED_SIZE_BINARY:
            return kBytes;
        case LK::WKT_TIMESTAMP:
            return kTimestamp;
        case LK::WKT_DURATION:
            return kDuration;
        default:
            return kUnknown;
    }
}

// ---------------------------------------------------------------------------
// Struct class-name / cross-file header resolution (C++ backend concern).
// ---------------------------------------------------------------------------

std::string CppClassName(const google::protobuf::Descriptor* msg,
                         const google::protobuf::FileDescriptor* context_file) {
    const std::string bare = ClassName(msg);
    if (msg->file() == context_file) return bare;
    if (msg->file()->package() == context_file->package()) return bare;
    const std::string& pkg = msg->file()->package();
    if (pkg.empty()) return "::fletcher_gen::" + bare;
    return "::fletcher_gen::" + DotToColons(pkg) + "::" + bare;
}

std::string CppCrossFileHeader(const google::protobuf::Descriptor* msg,
                               const google::protobuf::FileDescriptor* context_file) {
    if (msg->file() == context_file) return "";
    const std::string& name = msg->file()->name();
    constexpr std::string_view kSuffix = ".proto";
    if (name.size() > kSuffix.size() && name.substr(name.size() - kSuffix.size()) == kSuffix)
        return name.substr(0, name.size() - kSuffix.size()) + ".fletcher.pb.h";
    return name + ".fletcher.pb.h";
}

// ---------------------------------------------------------------------------
// Edge ENCODE recursive visitor (encode only). Maps IR recursion to generated
// code recursion, preserving GIR-2's exact wire behavior (field order, null-bit
// handling, list/map layout, default writes for unset non-nullable scalars).
// ---------------------------------------------------------------------------

namespace {

enum class ValueAccessMode {
    RAW_VALUE,         // list element, map key/value, struct leaf
    DEREF_OPTIONAL,    // nullable top-level: *opt / opt->
    VALUE_OR_DEFAULT,  // non-nullable top-level: opt.value_or(default)
};

struct EncodeContext {
    std::ostringstream& out;
    std::string writer_expr;
    std::string value_expr;
    ValueAccessMode access_mode;
    std::string field_index_expr;
    std::string indent;
    int depth;
    bool top_level_field;
    const google::protobuf::FileDescriptor* context_file;
};

class EdgeEncodeVisitor {
public:
    explicit EdgeEncodeVisitor(EncodeContext ctx) : ctx_(std::move(ctx)) {}

    void EmitField(const ir::IrNode& node) {
        const bool nullable = node.facts.nullable;
        const bool optional_shape =
            node.kind == ir::NodeKind::SCALAR || node.kind == ir::NodeKind::STRUCT;

        // Only scalar/struct top-level fields have std::optional<> storage and
        // therefore honour the null bit. Lists and maps are non-nullable
        // containers at the proto level (their storage is a vector), so they are
        // emitted directly on the member — matching GIR-2 wire behavior exactly.
        if (ctx_.top_level_field && optional_shape && nullable) {
            ctx_.out << ctx_.indent << "if (!" << ctx_.value_expr << ".has_value()) "
                     << ctx_.writer_expr << ".SetNull(" << ctx_.field_index_expr << ");\n"
                     << ctx_.indent << "else {\n";
            EncodeContext inner = ctx_;
            inner.access_mode = ValueAccessMode::DEREF_OPTIONAL;
            inner.indent = ctx_.indent + "    ";
            inner.top_level_field = false;
            EmitValue(node, inner);
            ctx_.out << ctx_.indent << "}\n";
            return;
        }

        if (ctx_.top_level_field && optional_shape && !nullable) {
            EncodeContext inner = ctx_;
            inner.access_mode = ValueAccessMode::VALUE_OR_DEFAULT;
            inner.top_level_field = false;
            EmitValue(node, inner);
            return;
        }

        // List / map top-level (or interior value): emit directly on value_expr.
        EncodeContext inner = ctx_;
        inner.access_mode = ValueAccessMode::RAW_VALUE;
        inner.top_level_field = false;
        EmitValue(node, inner);
    }

    void EmitValue(const ir::IrNode& node, const EncodeContext& ctx) {
        switch (node.kind) {
            case ir::NodeKind::SCALAR:
                EmitScalar(std::get<ir::ScalarNode>(node.node), ctx);
                break;
            case ir::NodeKind::LIST:
                EmitList(std::get<ir::ListNode>(node.node), ctx);
                break;
            case ir::NodeKind::FIXED_SIZE_LIST:
                EmitList(ir::ListNode{}, ctx);  // not produced by proto; defensive no-op path
                break;
            case ir::NodeKind::STRUCT:
                EmitStruct(std::get<ir::StructNode>(node.node), ctx);
                break;
            case ir::NodeKind::MAP:
                EmitMap(std::get<ir::MapNode>(node.node), ctx);
                break;
            case ir::NodeKind::UNSUPPORTED:
                ctx.out << ctx.indent << "// unsupported field skipped: "
                        << std::get<ir::UnsupportedNode>(node.node).reason << "\n";
                break;
        }
    }

private:
    void EmitScalar(const ir::ScalarNode& s, const EncodeContext& ctx) {
        const CppScalarInfo& info = LookupScalar(s.logical_type, s.enum_identity);
        const bool is_binary = s.logical_type.kind == ir::LogicalKind::BINARY;
        const bool is_fsb = s.logical_type.kind == ir::LogicalKind::FIXED_SIZE_BINARY;

        if (is_binary || is_fsb) {
            std::string obj;
            switch (ctx.access_mode) {
                case ValueAccessMode::RAW_VALUE:
                    obj = ctx.value_expr;
                    break;
                case ValueAccessMode::DEREF_OPTIONAL:
                    obj = "(*" + ctx.value_expr + ")";  // fix 2a: (*field_).data(), not *field_.data()
                    break;
                case ValueAccessMode::VALUE_OR_DEFAULT:
                    obj = ctx.value_expr + ".value_or(" + info.default_value + ")";
                    break;
            }
            ctx.out << ctx.indent << ctx.writer_expr
                    << ".WriteBinary(reinterpret_cast<const uint8_t*>(" << obj << ".data()), ";
            if (is_binary)
                ctx.out << obj << ".size());\n";
            else
                ctx.out << (s.logical_type.fixed_size_binary_width
                                ? *s.logical_type.fixed_size_binary_width
                                : 0)
                        << ");\n";
            return;
        }

        std::string val;
        switch (ctx.access_mode) {
            case ValueAccessMode::RAW_VALUE:
                val = ctx.value_expr;
                break;
            case ValueAccessMode::DEREF_OPTIONAL:
                val = "*" + ctx.value_expr;
                break;
            case ValueAccessMode::VALUE_OR_DEFAULT:
                val = ctx.value_expr + ".value_or(" + info.default_value + ")";
                break;
        }
        ctx.out << ctx.indent << ctx.writer_expr << "." << info.positional_write << "(" << val
                << ");\n";
    }

    void EmitList(const ir::ListNode& list, const EncodeContext& ctx) {
        if (!list.element) return;  // defensive (FIXED_SIZE_LIST placeholder)
        const std::string loop = "li_" + std::to_string(ctx.depth);
        const std::string lc = "lc_" + std::to_string(ctx.depth);
        const std::string container = ctx.value_expr;  // RAW at every list position

        ctx.out << ctx.indent << "{ auto " << lc << " = " << ctx.writer_expr
                << ".BeginList(static_cast<uint32_t>(" << container << ".size()));\n"
                << ctx.indent << "  for (uint32_t " << loop << " = 0; " << loop << " < "
                << container << ".size(); ++" << loop << ") {\n";

        EncodeContext e = ctx;
        e.value_expr = container + "[" + loop + "]";
        e.access_mode = ValueAccessMode::RAW_VALUE;
        e.indent = ctx.indent + "      ";
        e.depth = ctx.depth + 1;
        e.top_level_field = false;
        EmitValue(*list.element, e);

        ctx.out << ctx.indent << "  }\n" << ctx.indent << "}\n";
    }

    void EmitStruct(const ir::StructNode& st, const EncodeContext& ctx) {
        const std::string cls = CppClassName(st.identity.descriptor, ctx.context_file);
        ctx.out << ctx.indent << "{ auto sw = " << ctx.writer_expr << ".BeginStruct(" << cls
                << "Schema()->n_children);\n";
        switch (ctx.access_mode) {
            case ValueAccessMode::RAW_VALUE:
                ctx.out << ctx.indent << "  " << ctx.value_expr << ".EncodeStructTo_(sw);\n";
                break;
            case ValueAccessMode::DEREF_OPTIONAL:
                // fix 2a: field_->EncodeStructTo_, not *field_.EncodeStructTo_
                ctx.out << ctx.indent << "  " << ctx.value_expr << "->EncodeStructTo_(sw);\n";
                break;
            case ValueAccessMode::VALUE_OR_DEFAULT:
                ctx.out << ctx.indent << "  " << ctx.value_expr << ".value_or(" << cls
                        << "()).EncodeStructTo_(sw);\n";
                break;
        }
        ctx.out << ctx.indent << "}\n";
    }

    void EmitMap(const ir::MapNode& map, const EncodeContext& ctx) {
        const std::string container = ctx.value_expr;

        ctx.out << ctx.indent << "{ auto mc = " << ctx.writer_expr
                << ".BeginMap(static_cast<uint32_t>(" << container << ".size()));\n"
                << ctx.indent << "  for (const auto& [k, v] : " << container << ") {\n";
        EncodeContext kc = ctx;
        kc.value_expr = "k";
        kc.access_mode = ValueAccessMode::RAW_VALUE;
        kc.indent = ctx.indent + "      ";
        kc.top_level_field = false;
        EmitValue(*map.key, kc);

        ctx.out << ctx.indent << "  }\n"
                << ctx.indent << "  auto vc = mc.BeginValues();\n"
                << ctx.indent << "  for (const auto& [k, v] : " << container << ") {\n";
        EncodeContext vc = ctx;
        vc.value_expr = "v";
        vc.access_mode = ValueAccessMode::RAW_VALUE;
        vc.indent = ctx.indent + "      ";
        vc.top_level_field = false;
        EmitValue(*map.value, vc);

        ctx.out << ctx.indent << "  }\n" << ctx.indent << "}\n";
    }

    EncodeContext ctx_;
};

}  // namespace

void EmitFieldEncodeFromIr(std::ostringstream& out, const ir::IrNode& node,
                           const std::string& value_expr, size_t field_index,
                           const google::protobuf::FileDescriptor* context_file) {
    EncodeContext ctx{out,
                      "w",
                      value_expr,
                      ValueAccessMode::VALUE_OR_DEFAULT,
                      std::to_string(field_index),
                      "        ",
                      0,
                      true,
                      context_file};
    EdgeEncodeVisitor visitor(std::move(ctx));
    visitor.EmitField(node);
}

}  // namespace fletcher::cpp_backend
