// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include "csharp_backend_visitor.hpp"

#include <optional>
#include <set>
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
using google::protobuf::FileDescriptor;
using ir::IrNode;
using ir::NodeKind;

// The kind a field without a C# type is reported as. Naming it in the output keeps
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

// Every enum declared in `msg`, then in its nested messages, in declaration order.
void CollectEnums(const Descriptor* msg, std::vector<const EnumDescriptor*>& out) {
    for (int i = 0; i < msg->enum_type_count(); ++i) out.push_back(msg->enum_type(i));
    for (int i = 0; i < msg->nested_type_count(); ++i) CollectEnums(msg->nested_type(i), out);
}

// ---------------------------------------------------------------------------
// Classification: what each column of a message converts as (BIND-6c-2).
// ---------------------------------------------------------------------------

// One list element, one map key or one map value: a mapped scalar, or a message of
// this file whose own class converts.
struct Elem {
    bool is_struct = false;
    CsScalarInfo scalar;  // when !is_struct
    std::string cls;      // when is_struct
};

enum class ColKind { SCALAR, TEMPORAL, STRUCT, LIST, MAP };

struct Column {
    size_t index = 0;
    std::string property;
    ColKind kind = ColKind::SCALAR;
    bool nullable = false;
    CsScalarInfo scalar;        // SCALAR
    bool is_timestamp = false;  // TEMPORAL: Timestamp, else Duration
    std::string cls;            // STRUCT
    Elem elem;                  // LIST element, MAP value
    Elem key;                   // MAP key
};

std::string ConversionBlocker(const Descriptor* msg, const FileDescriptor* file,
                              std::set<const Descriptor*>& visiting);

// The reason a message-typed element or field cannot convert, or empty when it can.
std::string StructBlocker(const ir::StructNode& s, const FileDescriptor* file,
                          std::set<const Descriptor*>& visiting) {
    if (s.identity.descriptor == nullptr || s.identity.descriptor->file() != file)
        return "a message from another file (BIND-6e)";
    if (ConversionBlocker(s.identity.descriptor, file, visiting).empty()) return {};
    return "a message whose own conversion waits";
}

std::optional<Elem> ElemOf(const IrNode& node, const FileDescriptor* file,
                           std::set<const Descriptor*>& visiting, std::string& why) {
    if (node.kind == NodeKind::STRUCT) {
        const auto& s = std::get<ir::StructNode>(node.node);
        why = StructBlocker(s, file, visiting);
        if (!why.empty()) return std::nullopt;
        Elem e;
        e.is_struct = true;
        e.cls = CsTypeName(s.identity.descriptor);
        return e;
    }
    if (node.kind == NodeKind::SCALAR) {
        const auto& s = std::get<ir::ScalarNode>(node.node);
        if (auto info = CsLookupScalar(s.logical_type, s.enum_identity)) {
            Elem e;
            e.scalar = *info;
            return e;
        }
    }
    why = std::string("a ") + PendingKind(node) + " element this backend cannot convert";
    return std::nullopt;
}

// The column for `node`, or nullopt with `why` set.
std::optional<Column> ClassifyColumn(const IrNode& node, const FileDescriptor* file,
                                     std::set<const Descriptor*>& visiting, std::string& why) {
    Column c;
    c.nullable = node.facts.nullable;
    switch (node.kind) {
        case NodeKind::SCALAR: {
            const auto& s = std::get<ir::ScalarNode>(node.node);
            if (auto info = CsLookupScalar(s.logical_type, s.enum_identity)) {
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
            why = StructBlocker(s, file, visiting);
            if (!why.empty()) return std::nullopt;
            c.kind = ColKind::STRUCT;
            c.nullable = true;  // as its property: a message field can be absent
            c.cls = CsTypeName(s.identity.descriptor);
            return c;
        }
        case NodeKind::LIST: {
            auto e = ElemOf(*std::get<ir::ListNode>(node.node).element, file, visiting, why);
            if (!e) return std::nullopt;
            c.kind = ColKind::LIST;
            c.elem = *e;
            return c;
        }
        case NodeKind::MAP: {
            const auto& m = std::get<ir::MapNode>(node.node);
            auto k = ElemOf(*m.key, file, visiting, why);
            if (!k) return std::nullopt;
            auto v = ElemOf(*m.value, file, visiting, why);
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
                              std::set<const Descriptor*>& visiting) {
    if (!visiting.insert(msg).second) return {};
    std::string blocker;
    for (const auto& rec : cpp_backend::BuildFlattenedFieldList(msg)) {
        std::string why;
        if (!ClassifyColumn(*rec.node, file, visiting, why)) {
            blocker = "field '" + rec.name + "' is " + why + ".";
            break;
        }
    }
    visiting.erase(msg);
    return blocker;
}

// ---------------------------------------------------------------------------
// Emission. Generated C# writes no wire bytes (D-BIND-1): ToArrow builds Arrow
// arrays, the C++ codec behind the C ABI encodes them. Every Arrow type a column
// is built with is read back from the class's own Schema, so no column can be
// built with a type its Schema disagrees with.
// ---------------------------------------------------------------------------

constexpr const char* kEmpty = "global::Apache.Arrow.ArrowBuffer.Empty";

std::string ArrayClass(const CsScalarInfo& info) {
    return "global::Apache.Arrow." + info.arrow_array;
}

// The expression a builder appends for `value` (a property or a pattern variable):
// enums cross as int32, bytes as a span.
std::string AppendArgument(const CsScalarInfo& info, const std::string& value) {
    if (info.is_enum) return "(int)" + value;
    if (info.type_text == "byte[]") return "(global::System.ReadOnlySpan<byte>)" + value;
    return value;
}

// The element at `index` of the typed array `arr`, as its non-nullable C# value.
std::string ReadElement(const CsScalarInfo& info, const std::string& arr, const std::string& j) {
    if (info.type_text == "byte[]") return arr + ".GetBytes(" + j + ").ToArray()";
    if (info.type_text == "string") return arr + ".GetString(" + j + ") ?? \"\"";
    if (info.is_enum)
        return "(" + info.type_text + ")" + arr + ".GetValue(" + j + ").GetValueOrDefault()";
    return arr + ".GetValue(" + j + ").GetValueOrDefault()";
}

std::string ElemTypeText(const Elem& e) { return e.is_struct ? e.cls : e.scalar.type_text; }

std::string FieldType(size_t n, const std::string& type_class) {
    return "(global::Apache.Arrow.Types." + type_class + ")Schema.GetFieldByIndex(" +
           std::to_string(n) + ").DataType";
}

std::string ListOf(const std::string& t) {
    return "global::System.Collections.Generic.List<" + t + ">";
}

// The statement that counts the nulls of column `n` (bitmap builder m<n>) into z<n>,
// and the validity buffer that follows from it: no buffer at all when nothing is
// null. Counted first, so the count never depends on argument evaluation order.
std::string CountNulls(const std::string& n) {
    return "        var z" + n + " = m" + n + ".UnsetBitCount;\n";
}
std::string Bitmap(const std::string& n) {
    return "z" + n + " > 0 ? m" + n + ".Build() : " + kEmpty;
}

// Statements that build `c<n>`, the array for one column, from `rows`.
void EmitColumn(std::ostringstream& o, const Column& c) {
    const std::string n = std::to_string(c.index);
    const std::string in = "        ";
    switch (c.kind) {
        case ColKind::SCALAR: {
            o << in << "var b" << n << " = new " << ArrayClass(c.scalar) << ".Builder();\n"
              << in << "foreach (var row in rows)\n"
              << in << "{\n";
            if (c.nullable)
                o << in << "    if (row." << c.property << " is { } x" << n << ") b" << n
                  << ".Append(" << AppendArgument(c.scalar, "x" + n) << "); else b" << n
                  << ".AppendNull();\n";
            else
                o << in << "    b" << n << ".Append("
                  << AppendArgument(c.scalar, "row." + c.property) << ");\n";
            o << in << "}\n" << in << "var c" << n << " = b" << n << ".Build();\n";
            return;
        }
        case ColKind::TEMPORAL: {
            const std::string type = c.is_timestamp ? "TimestampType" : "DurationType";
            const std::string array = c.is_timestamp ? "TimestampArray" : "DurationArray";
            // The value is recounted in the column's unit EXACTLY (WithUnit refuses to
            // drop a remainder), so writing never loses a digit (D-BIND-26).
            o << in << "var t" << n << " = " << FieldType(c.index, type) << ";\n"
              << in << "var v" << n
              << " = new global::Apache.Arrow.ArrowBuffer.Builder<long>(rows.Count);\n"
              << in << "var m" << n
              << " = new global::Apache.Arrow.ArrowBuffer.BitmapBuilder(rows.Count);\n"
              << in << "foreach (var row in rows)\n"
              << in << "{\n";
            if (c.nullable)
                o << in << "    if (row." << c.property << " is { } x" << n << ") { v" << n
                  << ".Append(x" << n << ".WithUnit(t" << n << ".Unit).Value); m" << n
                  << ".Append(true); }\n"
                  << in << "    else { v" << n << ".Append(0); m" << n << ".Append(false); }\n";
            else
                o << in << "    v" << n << ".Append(row." << c.property << ".WithUnit(t" << n
                  << ".Unit).Value);\n"
                  << in << "    m" << n << ".Append(true);\n";
            o << in << "}\n"
              << CountNulls(n) << in << "var c" << n << " = new global::Apache.Arrow." << array
              << "(t" << n << ", v" << n << ".Build(), " << Bitmap(n) << ", rows.Count, z" << n
              << ", 0);\n";
            return;
        }
        case ColKind::STRUCT: {
            // A null message keeps its slot: its row holds a default instance, whose
            // children are not read back because the validity bit says null.
            o << in << "var t" << n << " = " << FieldType(c.index, "StructType") << ";\n"
              << in << "var r" << n << " = new " << ListOf(c.cls) << "(rows.Count);\n"
              << in << "var m" << n
              << " = new global::Apache.Arrow.ArrowBuffer.BitmapBuilder(rows.Count);\n"
              << in << "foreach (var row in rows)\n"
              << in << "{\n"
              << in << "    if (row." << c.property << " is { } x" << n << ") { r" << n << ".Add(x"
              << n << "); m" << n << ".Append(true); }\n"
              << in << "    else { r" << n << ".Add(new " << c.cls << "()); m" << n
              << ".Append(false); }\n"
              << in << "}\n"
              << CountNulls(n) << in << "var c" << n << " = new global::Apache.Arrow.StructArray(t"
              << n << ", rows.Count, " << c.cls << ".ToArrowColumns(r" << n << "), " << Bitmap(n)
              << ", z" << n << ", 0);\n";
            return;
        }
        case ColKind::LIST: {
            o << in << "var t" << n << " = " << FieldType(c.index, "ListType") << ";\n"
              << in << "var o" << n
              << " = new global::Apache.Arrow.ArrowBuffer.Builder<int>(rows.Count + 1);\n"
              << in << "o" << n << ".Append(0);\n";
            if (c.elem.is_struct) {
                o << in << "var f" << n << " = new " << ListOf(c.elem.cls) << "();\n"
                  << in << "foreach (var row in rows)\n"
                  << in << "{\n"
                  << in << "    f" << n << ".AddRange(row." << c.property << ");\n"
                  << in << "    o" << n << ".Append(f" << n << ".Count);\n"
                  << in << "}\n"
                  << in << "var e" << n << " = new global::Apache.Arrow.StructArray("
                  << "(global::Apache.Arrow.Types.StructType)t" << n << ".ValueDataType, f" << n
                  << ".Count, " << c.elem.cls << ".ToArrowColumns(f" << n << "), " << kEmpty
                  << ", 0, 0);\n";
            } else {
                o << in << "var b" << n << " = new " << ArrayClass(c.elem.scalar) << ".Builder();\n"
                  << in << "foreach (var row in rows)\n"
                  << in << "{\n"
                  << in << "    foreach (var x" << n << " in row." << c.property << ") b" << n
                  << ".Append(" << AppendArgument(c.elem.scalar, "x" + n) << ");\n"
                  << in << "    o" << n << ".Append(b" << n << ".Length);\n"
                  << in << "}\n"
                  << in << "var e" << n << " = b" << n << ".Build();\n";
            }
            o << in << "var c" << n << " = new global::Apache.Arrow.ListArray(t" << n
              << ", rows.Count, o" << n << ".Build(), e" << n << ", " << kEmpty << ", 0, 0);\n";
            return;
        }
        case ColKind::MAP: {
            o << in << "var t" << n << " = " << FieldType(c.index, "MapType") << ";\n"
              << in << "var o" << n
              << " = new global::Apache.Arrow.ArrowBuffer.Builder<int>(rows.Count + 1);\n"
              << in << "o" << n << ".Append(0);\n"
              << in << "var k" << n << " = new " << ArrayClass(c.key.scalar) << ".Builder();\n";
            if (c.elem.is_struct)
                o << in << "var f" << n << " = new " << ListOf(c.elem.cls) << "();\n";
            else
                o << in << "var b" << n << " = new " << ArrayClass(c.elem.scalar)
                  << ".Builder();\n";
            o << in << "foreach (var row in rows)\n"
              << in << "{\n"
              << in << "    foreach (var x" << n << " in row." << c.property << ")\n"
              << in << "    {\n"
              << in << "        k" << n << ".Append("
              << AppendArgument(c.key.scalar, "x" + n + ".Key") << ");\n";
            if (c.elem.is_struct)
                o << in << "        f" << n << ".Add(x" << n << ".Value);\n";
            else
                o << in << "        b" << n << ".Append("
                  << AppendArgument(c.elem.scalar, "x" + n + ".Value") << ");\n";
            o << in << "    }\n"
              << in << "    o" << n << ".Append(k" << n << ".Length);\n"
              << in << "}\n";
            const std::string values =
                c.elem.is_struct
                    ? "new "
                      "global::Apache.Arrow.StructArray((global::Apache.Arrow.Types.StructType)t" +
                          n + ".ValueField.DataType, f" + n + ".Count, " + c.elem.cls +
                          ".ToArrowColumns(f" + n + "), " + kEmpty + ", 0, 0)"
                    : "b" + n + ".Build()";
            o << in << "var kv" << n << " = new global::Apache.Arrow.StructArray(t" << n
              << ".KeyValueType, k" << n << ".Length, new global::Apache.Arrow.IArrowArray[] { k"
              << n << ".Build(), " << values << " }, " << kEmpty << ", 0, 0);\n"
              << in << "var c" << n << " = new global::Apache.Arrow.MapArray(t" << n
              << ", rows.Count, o" << n << ".Build(), kv" << n << ", " << kEmpty << ", 0, 0);\n";
            return;
        }
    }
}

// static RecordBatch ToArrow(IEnumerable<T>), and the columns it is built from,
// which a message embedding this one as a struct reuses.
std::string GenerateToArrow(const std::string& cls, const std::vector<Column>& columns) {
    std::ostringstream o;
    o << "    public static global::Apache.Arrow.RecordBatch ToArrow("
      << "global::System.Collections.Generic.IEnumerable<" << cls << "> rows)\n    {\n"
      << "        global::System.ArgumentNullException.ThrowIfNull(rows);\n"
      << "        var all = rows as global::System.Collections.Generic.IReadOnlyList<" << cls
      << "> ?? new " << ListOf(cls) << "(rows);\n"
      << "        return new global::Apache.Arrow.RecordBatch(Schema, ToArrowColumns(all), "
         "all.Count);\n"
      << "    }\n\n"
      << "    internal static global::Apache.Arrow.IArrowArray[] ToArrowColumns("
      << "global::System.Collections.Generic.IReadOnlyList<" << cls << "> rows)\n    {\n";
    for (const auto& c : columns) EmitColumn(o, c);
    o << "        return new global::Apache.Arrow.IArrowArray[] { ";
    for (size_t i = 0; i < columns.size(); ++i) o << (i > 0 ? ", " : "") << "c" << columns[i].index;
    o << (columns.empty() ? "};\n" : " };\n") << "    }\n";
    return o.str();
}

// Statements that read row `index` of one column back into `row`.
void EmitRead(std::ostringstream& o, const Column& c) {
    const std::string n = std::to_string(c.index);
    const std::string field = "array.Fields[" + n + "]";
    const std::string in = "        ";
    switch (c.kind) {
        case ColKind::SCALAR: {
            const std::string column = "((" + ArrayClass(c.scalar) + ")" + field + ")";
            const std::string& type = c.scalar.type_text;
            o << in;
            if (type == "byte[]") {
                o << "{ var a = (" << ArrayClass(c.scalar) << ")" << field << "; row." << c.property
                  << " = a.IsNull(index) ? "
                  << (c.nullable ? "null" : "global::System.Array.Empty<byte>()")
                  << " : a.GetBytes(index).ToArray(); }\n";
            } else if (type == "string") {
                o << "row." << c.property << " = " << column << ".GetString(index)"
                  << (c.nullable ? "" : " ?? \"\"") << ";\n";
            } else if (c.scalar.is_enum) {
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
            return;
        }
        case ColKind::TEMPORAL: {
            // Read back in the column's own unit and zone: a value written in another
            // unit returns recounted, the same instant or span.
            const std::string array = c.is_timestamp ? "TimestampArray" : "DurationArray";
            const std::string type = c.is_timestamp ? "TimestampType" : "DurationType";
            const std::string model = c.is_timestamp ? "global::Eiva.Fletcher.Model.Timestamp"
                                                     : "global::Eiva.Fletcher.Model.Duration";
            const std::string extra = c.is_timestamp ? ", t.Timezone" : "";
            o << in << "{\n"
              << in << "    var a = (global::Apache.Arrow." << array << ")" << field << ";\n"
              << in << "    var t = (global::Apache.Arrow.Types." << type << ")a.Data.DataType;\n";
            if (c.nullable)
                o << in << "    row." << c.property << " = a.GetValue(index) is long v ? new "
                  << model << "(v, t.Unit" << extra << ") : null;\n";
            else
                o << in << "    row." << c.property << " = new " << model
                  << "(a.GetValue(index).GetValueOrDefault(), t.Unit" << extra << ");\n";
            o << in << "}\n";
            return;
        }
        case ColKind::STRUCT:
            o << in << "{ var s = (global::Apache.Arrow.StructArray)" << field << "; row."
              << c.property << " = s.IsNull(index) ? null : " << c.cls
              << ".FromArrow(s, index); }\n";
            return;
        case ColKind::LIST: {
            const std::string values =
                c.elem.is_struct ? "global::Apache.Arrow.StructArray" : ArrayClass(c.elem.scalar);
            o << in << "{\n"
              << in << "    var l = (global::Apache.Arrow.ListArray)" << field << ";\n"
              << in << "    var e = (" << values
              << ")l.Values;\n"
              // ValueOffsets is already sliced to the array's own offset; the older
              // GetValueOffset is obsolete in Apache.Arrow 23 (CS0618 fails the build).
              << in << "    var start = l.ValueOffsets[index];\n"
              << in << "    var end = l.ValueOffsets[index + 1];\n"
              << in << "    var x = new " << ListOf(ElemTypeText(c.elem)) << "(end - start);\n"
              << in << "    for (var j = start; j < end; j++) x.Add("
              << (c.elem.is_struct ? c.elem.cls + ".FromArrow(e, j)"
                                   : ReadElement(c.elem.scalar, "e", "j"))
              << ");\n"
              << in << "    row." << c.property << " = x;\n"
              << in << "}\n";
            return;
        }
        case ColKind::MAP: {
            const std::string values =
                c.elem.is_struct ? "global::Apache.Arrow.StructArray" : ArrayClass(c.elem.scalar);
            const std::string pair = "global::System.Collections.Generic.KeyValuePair<" +
                                     c.key.scalar.type_text + ", " + ElemTypeText(c.elem) + ">";
            o << in << "{\n"
              << in << "    var m = (global::Apache.Arrow.MapArray)" << field << ";\n"
              << in << "    var k = (" << ArrayClass(c.key.scalar) << ")m.Keys;\n"
              << in << "    var e = (" << values << ")m.Values;\n"
              << in << "    var start = m.ValueOffsets[index];\n"
              << in << "    var end = m.ValueOffsets[index + 1];\n"
              << in << "    var x = new " << ListOf(pair) << "(end - start);\n"
              << in << "    for (var j = start; j < end; j++) x.Add(new " << pair << "("
              << ReadElement(c.key.scalar, "k", "j") << ", "
              << (c.elem.is_struct ? c.elem.cls + ".FromArrow(e, j)"
                                   : ReadElement(c.elem.scalar, "e", "j"))
              << "));\n"
              << in << "    row." << c.property << " = x;\n"
              << in << "}\n";
            return;
        }
    }
}

// static T FromArrow(StructArray, int): row `index` back into a new instance.
std::string GenerateFromArrow(const std::string& cls, const std::vector<Column>& columns) {
    std::ostringstream o;
    o << "    public static " << cls
      << " FromArrow(global::Apache.Arrow.StructArray array, int index)\n    {\n"
      << "        global::System.ArgumentNullException.ThrowIfNull(array);\n"
      << "        var row = new " << cls << "();\n";
    for (const auto& c : columns) EmitRead(o, c);
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
    std::string pending;  // the first field ToArrow / FromArrow cannot convert
    std::set<const Descriptor*> visiting{msg};
    for (size_t i = 0; i < records.size(); ++i) {
        const auto& rec = records[i];
        const IrNode& node = *rec.node;
        const auto field_type = CsFieldTypeOf(node, file_);
        if (!field_type.has_value()) {
            // The one way a mapped field gets no type today: it uses a message declared in
            // another file (a repeated google.protobuf.Timestamp is one: the IR maps it as a
            // struct, not as a timestamp), and cross-file is BIND-6e's.
            o << "    // Not generated yet (BIND-6e): field '" << rec.name << "' ("
              << PendingKind(node) << ") has no C# type here.\n";
            if (pending.empty())
                pending = "field '" + rec.name + "' is a message from another file (BIND-6e).";
            continue;
        }
        const bool nullable = field_type->nullable;
        const std::string property = CsPropertyName(rec.name, msg);
        o << "    public " << field_type->type_text << (nullable ? "?" : "") << " " << property
          << " { get; set; }";
        // A collection starts empty and a non-nullable reference starts as the proto
        // default, never null, so the property honours its own annotation.
        if (field_type->is_collection)
            o << " = new();";
        else if (field_type->is_reference && !nullable)
            o << (field_type->type_text == "string" ? " = \"\";"
                                                    : " = global::System.Array.Empty<byte>();");
        o << "\n";

        // Conversion is generated only when every column converts, so no ToArrow can
        // build a batch its own Schema disagrees with.
        std::string why;
        auto column = ClassifyColumn(node, file_, visiting, why);
        if (!column) {
            if (pending.empty()) pending = "field '" + rec.name + "' is " + why + ".";
            continue;
        }
        column->index = i;
        column->property = property;
        columns.push_back(std::move(*column));
    }

    // The schema C++ generates for this message, from the same visitor (BIND-6b).
    // A flatten wrapper has none, in C++ or in the .ipc set, because it is inlined
    // into the messages that use it.
    if (fletcher::IsFlattenedWrapper(msg)) {
        o << "    // No Schema: a flatten wrapper is inlined into the messages that use it.\n";
    } else {
        o << "\n    public static global::Apache.Arrow.Schema Schema { get; } = "
          << RenderMessageSchema(msg, resolver_, "    ") << ";\n";
        if (!pending.empty())
            o << "\n    // ToArrow and FromArrow are not generated: " << pending << "\n";
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
