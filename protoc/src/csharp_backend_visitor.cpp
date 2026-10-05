// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include "csharp_backend_visitor.hpp"

#include <sstream>
#include <string>
#include <variant>

#include "cpp_backend_schema_visitor.hpp"
#include "csharp_backend_schema_sink.hpp"
#include "csharp_backend_type_table.hpp"
#include "generator_internal.hpp"
#include "type_mapper.hpp"

namespace fletcher::csharp_backend {

namespace {

using google::protobuf::Descriptor;
using google::protobuf::EnumDescriptor;
using ir::IrNode;
using ir::NodeKind;

// The kind a not-yet-generated field is reported as. Composite and temporal
// fields are BIND-6c's; naming them in the output keeps the slice boundary
// visible instead of dropping data silently.
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
            return "temporal";
        case NodeKind::UNSUPPORTED:
            return "unsupported";
    }
    return "unsupported";
}

// The same, as the reason a message gets no ToArrow / FromArrow yet.
std::string PendingReason(const IrNode& node) {
    const std::string kind = PendingKind(node);
    return kind == "temporal" || kind == "unsupported" ? kind : "a " + kind;
}

// Every enum declared in `msg`, then in its nested messages, in declaration order.
void CollectEnums(const Descriptor* msg, std::vector<const EnumDescriptor*>& out) {
    for (int i = 0; i < msg->enum_type_count(); ++i) out.push_back(msg->enum_type(i));
    for (int i = 0; i < msg->nested_type_count(); ++i) CollectEnums(msg->nested_type(i), out);
}

// One field of a message whose conversion is generated: its column, property and
// C# type information.
struct Column {
    size_t index;
    std::string property;
    CsScalarInfo info;
    bool nullable;
};

std::string ArrayClass(const CsScalarInfo& info) {
    return "global::Apache.Arrow." + info.arrow_array;
}

// The expression a column builder appends for `value` (a property or a pattern
// variable): enums cross as int32, bytes as a span.
std::string AppendArgument(const CsScalarInfo& info, const std::string& value) {
    if (info.is_enum) return "(int)" + value;
    if (info.type_text == "byte[]") return "(global::System.ReadOnlySpan<byte>)" + value;
    return value;
}

// static RecordBatch ToArrow(IEnumerable<T>): one builder per column, one pass.
std::string GenerateToArrow(const std::string& cls, const std::vector<Column>& columns) {
    std::ostringstream o;
    o << "    public static global::Apache.Arrow.RecordBatch ToArrow("
      << "global::System.Collections.Generic.IEnumerable<" << cls << "> rows)\n    {\n"
      << "        global::System.ArgumentNullException.ThrowIfNull(rows);\n";
    for (const auto& c : columns)
        o << "        var c" << c.index << " = new " << ArrayClass(c.info) << ".Builder();\n";
    o << "        var length = 0;\n"
      << "        foreach (var row in rows)\n        {\n";
    for (const auto& c : columns) {
        const std::string n = std::to_string(c.index);
        if (c.nullable)
            o << "            if (row." << c.property << " is { } v" << n << ") c" << n
              << ".Append(" << AppendArgument(c.info, "v" + n) << "); else c" << n
              << ".AppendNull();\n";
        else
            o << "            c" << n << ".Append(" << AppendArgument(c.info, "row." + c.property)
              << ");\n";
    }
    o << "            length++;\n        }\n"
      << "        return new global::Apache.Arrow.RecordBatch(Schema, "
      << "new global::Apache.Arrow.IArrowArray[] { ";
    for (size_t i = 0; i < columns.size(); ++i)
        o << (i > 0 ? ", " : "") << "c" << columns[i].index << ".Build()";
    o << (columns.empty() ? "}" : " }") << ", length);\n    }\n";
    return o.str();
}

// static T FromArrow(StructArray, int): row `index` back into a new instance.
std::string GenerateFromArrow(const std::string& cls, const std::vector<Column>& columns) {
    std::ostringstream o;
    o << "    public static " << cls
      << " FromArrow(global::Apache.Arrow.StructArray array, int index)\n    {\n"
      << "        global::System.ArgumentNullException.ThrowIfNull(array);\n"
      << "        var row = new " << cls << "();\n";
    for (const auto& c : columns) {
        const std::string n = std::to_string(c.index);
        const std::string column = "((" + ArrayClass(c.info) + ")array.Fields[" + n + "])";
        const std::string& type = c.info.type_text;
        o << "        ";
        if (type == "byte[]") {
            o << "{ var a = (" << ArrayClass(c.info) << ")array.Fields[" << n << "]; row."
              << c.property << " = a.IsNull(index) ? "
              << (c.nullable ? "null" : "global::System.Array.Empty<byte>()")
              << " : a.GetBytes(index).ToArray(); }\n";
        } else if (type == "string") {
            o << "row." << c.property << " = " << column << ".GetString(index)"
              << (c.nullable ? "" : " ?? \"\"") << ";\n";
        } else if (c.info.is_enum) {
            if (c.nullable)
                o << "row." << c.property << " = " << column << ".GetValue(index) is int v" << n
                  << " ? (" << type << ")v" << n << " : null;\n";
            else
                o << "row." << c.property << " = (" << type << ")" << column
                  << ".GetValue(index).GetValueOrDefault();\n";
        } else {
            o << "row." << c.property << " = " << column << ".GetValue(index)"
              << (c.nullable ? "" : ".GetValueOrDefault()") << ";\n";
        }
    }
    o << "        return row;\n    }\n";
    return o.str();
}

}  // namespace

CsVisitor::CsVisitor(const google::protobuf::FileDescriptor* file,
                     const OptionMetadataResolver* resolver)
    : file_(file), resolver_(resolver) {}

std::string CsVisitor::GenerateEnum(const EnumDescriptor* enm) {
    std::ostringstream o;
    o << "public enum " << CsTypeName(enm) << " : int\n{\n";
    for (const auto& member : CsEnumMembers(enm))
        o << "    " << member.name << " = " << member.number << ",\n";
    o << "}\n";
    return o.str();
}

std::string CsVisitor::GenerateMessage(const Descriptor* msg) {
    std::ostringstream o;
    const std::string cls = CsTypeName(msg);
    o << "public sealed class " << cls << "\n{\n";
    // The shared flatten walk, as TsVisitor uses it: field-level flatten inlining
    // and the unsupported-field drops are identical to the schema's field set, so
    // field i here is column i of the Schema below.
    const auto records = cpp_backend::BuildFlattenedFieldList(msg);
    std::vector<Column> columns;
    std::string pending;  // the first field ToArrow / FromArrow cannot convert yet
    for (size_t i = 0; i < records.size(); ++i) {
        const auto& rec = records[i];
        const IrNode& node = *rec.node;
        std::optional<CsScalarInfo> info;
        if (node.kind == NodeKind::SCALAR) {
            const auto& s = std::get<ir::ScalarNode>(node.node);
            info = CsLookupScalar(s.logical_type, s.enum_identity);
        }
        if (!info.has_value()) {
            o << "    // Not generated yet (BIND-6c): field '" << rec.name << "' ("
              << PendingKind(node) << ").\n";
            if (pending.empty())
                pending = "field '" + rec.name + "' is " + PendingReason(node) + ".";
            continue;
        }
        const bool nullable = node.facts.nullable;
        const std::string property = CsPropertyName(rec.name, msg);
        o << "    public " << info->type_text << (nullable ? "?" : "") << " " << property
          << " { get; set; }";
        // A non-nullable reference starts as the proto default, never null, so the
        // property honours its own annotation.
        if (info->is_reference && !nullable)
            o << (info->type_text == "string" ? " = \"\";"
                                              : " = global::System.Array.Empty<byte>();");
        o << "\n";
        columns.push_back({i, property, *info, nullable});
    }

    // The schema C++ generates for this message, from the same visitor (BIND-6b).
    // A flatten wrapper has none, in C++ or in the .ipc set, because it is inlined
    // into the messages that use it.
    if (fletcher::IsFlattenedWrapper(msg)) {
        o << "    // No Schema: a flatten wrapper is inlined into the messages that use it.\n";
    } else {
        o << "\n    public static global::Apache.Arrow.Schema Schema { get; } = "
          << RenderMessageSchema(msg, resolver_, "    ") << ";\n";
        // Conversion waits until every field converts: a ToArrow that skipped one
        // would build a batch its own Schema disagrees with.
        if (!pending.empty())
            o << "\n    // ToArrow and FromArrow are not generated yet (BIND-6c): " << pending
              << "\n";
        else
            o << "\n" << GenerateToArrow(cls, columns) << "\n" << GenerateFromArrow(cls, columns);
    }
    o << "}\n";
    return o.str();
}

std::string CsVisitor::GenerateFile() {
    std::ostringstream o;
    // The <auto-generated> block is what Roslyn's analysers look for to skip a
    // file, as protoc's own C# output does. Without it a consumer building with
    // analysers as errors fails on our names by design (CA1707 on Outer_Inner and
    // Player_, CA1819 on byte[], CA1069 on enum aliases).
    o << "// <auto-generated>\n"
      << "//     Generated by fletcher-protoc. DO NOT EDIT.\n"
      << "//     Source: " << file_->name() << "\n"
      << "// </auto-generated>\n"
      << "#nullable enable\n\n"
      << "namespace " << CsNamespace(file_) << ";\n";

    std::vector<const EnumDescriptor*> enums;
    for (int i = 0; i < file_->enum_type_count(); ++i) enums.push_back(file_->enum_type(i));
    for (int i = 0; i < file_->message_type_count(); ++i)
        CollectEnums(file_->message_type(i), enums);
    for (const auto* enm : enums) o << "\n" << GenerateEnum(enm);

    for (const auto* msg : fletcher::OrderedMessages(file_)) {
        o << "\n";
        if (fletcher::IsRecursive(msg)) {
            o << "// Skipped: " << CsTypeName(msg) << " is recursive and cannot be represented.\n";
            continue;
        }
        o << GenerateMessage(msg);
    }
    return o.str();
}

}  // namespace fletcher::csharp_backend
