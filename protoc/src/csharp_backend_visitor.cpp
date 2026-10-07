// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include "csharp_backend_visitor.hpp"

#include <memory>
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

// One list element, one map key or one map value: a mapped scalar, a message of this
// file whose own class converts, or (6c-3) itself a list of such elements, which is how
// a flatten wrapper around a repeated field nests: `repeated StructListWrapper` is a
// list<list<struct>>.
struct Elem {
    bool is_struct = false;
    bool is_list = false;
    CsScalarInfo scalar;          // when neither
    std::string cls;              // when is_struct
    std::shared_ptr<Elem> inner;  // when is_list: the element of this inner list
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
    if (node.kind == NodeKind::LIST) {
        auto inner = ElemOf(*std::get<ir::ListNode>(node.node).element, file, visiting, why);
        if (!inner) return std::nullopt;
        Elem e;
        e.is_list = true;
        e.inner = std::make_shared<Elem>(std::move(*inner));
        return e;
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

std::string FieldType(size_t n, const std::string& type_class) {
    return "(global::Apache.Arrow.Types." + type_class + ")Schema.GetFieldByIndex(" +
           std::to_string(n) + ").DataType";
}

std::string ListOf(const std::string& t) {
    return "global::System.Collections.Generic.List<" + t + ">";
}

std::string ElemTypeText(const Elem& e) {
    if (e.is_list) return ListOf(ElemTypeText(*e.inner));
    return e.is_struct ? e.cls : e.scalar.type_text;
}

// 6c-3: statements that build ONE Arrow array holding every element of `values` (a C#
// IReadOnlyList of the element's type), typed `type` (a C# expression for its Arrow
// type, read from the Schema). Returns the variable holding the array. A list element
// recurses: its offsets come from the inner lists' lengths and its child is built from
// their elements flattened, so a list<list<list<struct>>> is three calls deep. `id`
// keeps every level's variables distinct.
std::string BuildElemArray(std::ostringstream& o, const Elem& e, const std::string& values,
                           const std::string& type, const std::string& id) {
    const std::string in = "        ";
    const std::string a = "a" + id;
    if (e.is_list) {
        const std::string t = "t" + id, off = "o" + id, flat = "f" + id, x = "x" + id;
        o << in << "var " << t << " = (global::Apache.Arrow.Types.ListType)" << type << ";\n"
          << in << "var " << off << " = new global::Apache.Arrow.ArrowBuffer.Builder<int>("
          << values << ".Count + 1);\n"
          << in << off << ".Append(0);\n"
          << in << "var " << flat << " = new " << ListOf(ElemTypeText(*e.inner)) << "();\n"
          << in << "foreach (var " << x << " in " << values << ")\n"
          << in << "{\n"
          << in << "    " << flat << ".AddRange(" << x << ");\n"
          << in << "    " << off << ".Append(" << flat << ".Count);\n"
          << in << "}\n";
        const std::string child = BuildElemArray(o, *e.inner, flat, t + ".ValueDataType", id + "_");
        o << in << "var " << a << " = new global::Apache.Arrow.ListArray(" << t << ", " << values
          << ".Count, " << off << ".Build(), " << child << ", " << kEmpty << ", 0, 0);\n";
        return a;
    }
    if (e.is_struct) {
        o << in << "var " << a << " = new global::Apache.Arrow.StructArray("
          << "(global::Apache.Arrow.Types.StructType)" << type << ", " << values << ".Count, "
          << e.cls << ".ToArrowColumns(" << values << "), " << kEmpty << ", 0, 0);\n";
        return a;
    }
    const std::string b = "b" + id, x = "x" + id;
    o << in << "var " << b << " = new " << ArrayClass(e.scalar) << ".Builder();\n"
      << in << "foreach (var " << x << " in " << values << ") " << b << ".Append("
      << AppendArgument(e.scalar, x) << ");\n"
      << in << "var " << a << " = " << b << ".Build();\n";
    return a;
}

// 6c-3: statements that read the elements `start` (inclusive) to `end` (exclusive) of
// the list array `list` into a new C# list `out`, recursing for a list element.
void EmitReadList(std::ostringstream& o, const Elem& e, const std::string& list,
                  const std::string& start, const std::string& end, const std::string& out,
                  const std::string& id, const std::string& in) {
    const std::string values = "e" + id, j = "j" + id;
    const std::string values_class = e.is_list     ? "global::Apache.Arrow.ListArray"
                                     : e.is_struct ? "global::Apache.Arrow.StructArray"
                                                   : ArrayClass(e.scalar);
    o << in << "var " << out << " = new " << ListOf(ElemTypeText(e)) << "(" << end << " - " << start
      << ");\n"
      << in << "var " << values << " = (" << values_class << ")" << list << ".Values;\n"
      << in << "for (var " << j << " = " << start << "; " << j << " < " << end << "; " << j
      << "++)\n"
      << in << "{\n";
    if (e.is_list) {
        const std::string inner = "x" + id;
        EmitReadList(o, *e.inner, values, values + ".ValueOffsets[" + j + "]",
                     values + ".ValueOffsets[" + j + " + 1]", inner, id + "_", in + "    ");
        o << in << "    " << out << ".Add(" << inner << ");\n";
    } else {
        o << in << "    " << out << ".Add("
          << (e.is_struct ? e.cls + ".FromArrow(" + values + ", " + j + ")"
                          : ReadElement(e.scalar, values, j))
          << ");\n";
    }
    o << in << "}\n";
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
            if (c.elem.is_list) {
                // 6c-3: a list of lists. The column is itself a list element of the
                // rows, so the rows' values are gathered and built by the recursion.
                Elem column;
                column.is_list = true;
                column.inner = std::make_shared<Elem>(c.elem);
                o << in << "var v" << n << " = new " << ListOf(ElemTypeText(column))
                  << "(rows.Count);\n"
                  << in << "foreach (var row in rows) v" << n << ".Add(row." << c.property
                  << ");\n";
                const std::string built = BuildElemArray(
                    o, column, "v" + n, "Schema.GetFieldByIndex(" + n + ").DataType", n);
                o << in << "var c" << n << " = " << built << ";\n";
                return;
            }
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
            if (c.elem.is_list) {
                // 6c-3: a list of lists, read level by level.
                o << in << "{\n"
                  << in << "    var l = (global::Apache.Arrow.ListArray)" << field << ";\n";
                EmitReadList(o, c.elem, "l", "l.ValueOffsets[index]", "l.ValueOffsets[index + 1]",
                             "x", "0", in + "    ");
                o << in << "    row." << c.property << " = x;\n" << in << "}\n";
                return;
            }
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

// ---------------------------------------------------------------------------
// Pub/sub methods (BIND-6d): the topic classes in the model file (D-BIND-77) and
// the native pair in its own file (D-BIND-76).
// ---------------------------------------------------------------------------

// One service method, as C++ and TypeScript see it: eligible by the SAME rules
// (fletcher::ValidateServiceMethod) or skipped with the same reason text.
struct PubSubMethod {
    std::string service;
    std::string method;
    const Descriptor* input = nullptr;
    std::string skip_reason;  // empty when eligible
    std::string Base() const { return service + "_" + method; }
};

std::vector<PubSubMethod> PubSubMethods(const FileDescriptor* file) {
    std::set<const Descriptor*> generated;
    for (const auto* msg : fletcher::OrderedMessages(file))
        if (!fletcher::IsRecursive(msg)) generated.insert(msg);
    std::vector<PubSubMethod> out;
    for (int si = 0; si < file->service_count(); ++si) {
        const auto* svc = file->service(si);
        for (int mi = 0; mi < svc->method_count(); ++mi) {
            const auto* m = svc->method(mi);
            PubSubMethod p{svc->name(), m->name(), m->input_type(), {}};
            fletcher::ValidateServiceMethod(m, generated, &p.skip_reason);
            out.push_back(std::move(p));
        }
    }
    return out;
}

// The topic's segments, C++'s form: the package is ONE segment, dots kept
// (D-BIND-72 point 6), so a C# and a C++ publisher of one method share a topic.
std::vector<std::string> TopicSegments(const FileDescriptor* file, const PubSubMethod& p) {
    std::vector<std::string> segments;
    if (!file->package().empty()) segments.push_back(file->package());
    segments.push_back(p.service);
    segments.push_back(p.method);
    return segments;
}

std::string TopicKey(const std::vector<std::string>& segments) {
    std::string key;
    for (size_t i = 0; i < segments.size(); ++i) key += (i > 0 ? "/" : "") + segments[i];
    return key;
}

// `public static class <Svc>_<Method>Topic` in the model layer (D-BIND-77).
std::string GenerateTopicClass(const FileDescriptor* file, const PubSubMethod& p) {
    const auto segments = TopicSegments(file, p);
    std::ostringstream o;
    o << "/// <summary>The topic of " << p.service << "." << p.method
      << ": the segments a C++ publisher of the same method uses.</summary>\n"
      << "public static class " << p.Base() << "Topic\n{\n"
      << "    /// <summary>The topic's segments, the package being one segment with its dots "
         "kept.</summary>\n"
      << "    public static global::System.Collections.Generic.IReadOnlyList<string> Segments "
         "{ get; } = new[] { ";
    for (size_t i = 0; i < segments.size(); ++i)
        o << (i > 0 ? ", " : "") << CsStringLiteral(segments[i]);
    o << " };\n\n"
      << "    /// <summary>The segments joined with '/'.</summary>\n"
      << "    public const string Key = " << CsStringLiteral(TopicKey(segments)) << ";\n"
      << "}\n";
    return o.str();
}

constexpr const char* kNs = "global::Eiva.Fletcher.";

// The members both halves of the pair share: the topic and the schema.
std::string PairStatics(const PubSubMethod& p, const std::string& msg) {
    std::ostringstream o;
    o << "    /// <summary>The topic, from <see cref=\"" << p.Base() << "Topic\"/>.</summary>\n"
      << "    public static " << kNs << "TopicPath Topic { get; } = " << kNs << "TopicPath.Of([.. "
      << p.Base() << "Topic.Segments]);\n\n"
      << "    /// <summary>The topic's segments joined with '/'.</summary>\n"
      << "    public const string TopicKey = " << p.Base() << "Topic.Key;\n\n"
      << "    /// <summary>The schema rows travel under: <see cref=\"" << msg
      << ".Schema\"/>.</summary>\n"
      << "    public static global::Apache.Arrow.Schema Schema => " << msg << ".Schema;\n\n";
    return o.str();
}

std::string GeneratePublisherPair(const PubSubMethod& p, const std::string& msg) {
    const std::string cls = p.Base() + "Publisher";
    std::ostringstream o;
    o << "/// <summary>Publishes <see cref=\"" << msg << "\"/> rows on " << p.service << "."
      << p.method << ", over one provider.</summary>\n"
      << "/// <remarks>\n"
      << "/// Declares the topic with <see cref=\"" << msg
      << ".Schema\"/> when constructed: the native\n"
      << "/// publish path accepts rows only on a topic THIS publisher declared. Not thread-safe "
         "to\n"
      << "/// dispose while publishing.\n"
      << "/// </remarks>\n"
      << "public sealed class " << cls << " : global::System.IDisposable\n{\n"
      << PairStatics(p, msg) << "    private readonly " << kNs << "Publisher _publisher;\n"
      << "    private readonly " << kNs << "FletcherCodec _codec;\n\n"
      << "    /// <summary>Declares the topic on <paramref name=\"provider\"/> with the row "
         "schema.</summary>\n"
      << "    /// <param name=\"provider\">Borrowed; it must outlive this publisher.</param>\n"
      << "    /// <param name=\"options\">The topic's options, or <see langword=\"null\"/> for "
         "the provider's defaults.</param>\n"
      << "    public " << cls << "(" << kNs << "PubSubProviderHandle provider, " << kNs
      << "TopicOptions? options = null)\n    {\n"
      << "        global::System.ArgumentNullException.ThrowIfNull(provider);\n"
      << "        _codec = new " << kNs << "FletcherCodec(" << msg << ".Schema);\n"
      << "        try\n        {\n"
      << "            _publisher = new " << kNs << "Publisher(provider);\n"
      << "            try\n            {\n"
      << "                _publisher.CreateTopic(Topic, " << msg << ".Schema, options);\n"
      << "            }\n            catch\n            {\n"
      << "                _publisher.Dispose();\n                throw;\n            }\n"
      << "        }\n        catch\n        {\n"
      << "            _codec.Dispose();\n            throw;\n        }\n    }\n\n"
      // D-BIND-78: per-row, synchronous, and the price stated where it is chosen.
      << "    /// <summary>Publishes one row, before returning.</summary>\n"
      << "    /// <remarks>\n"
      << "    /// Builds a one-row Arrow batch, binds it and publishes it. That is the costly "
         "shape: measured\n"
      << "    /// at about 3.8 microseconds a row, 19.6 times the generated C++ publisher, "
         "mostly in\n"
      << "    /// Apache.Arrow building and exporting the batch (c-abi/benchmarks/README.md, "
         "D-BIND-49).\n"
      << "    /// To publish many rows, prefer <c>Publish(IEnumerable&lt;" << msg << "&gt;)</c>,\n"
      << "    /// which builds and binds once (about 1.23 times C++ per row).\n"
      << "    /// </remarks>\n"
      << "    public void Publish(" << msg << " row) => Publish(row, null);\n\n"
      << "    /// <summary>Publishes one row with its attachments, before returning.</summary>\n"
      << "    /// <remarks>The cost is <see cref=\"Publish(" << msg << ")\"/>'s.</remarks>\n"
      << "    public void Publish(" << msg << " row, " << kNs
      << "AttachmentsBuilder? attachments)\n    {\n"
      << "        global::System.ArgumentNullException.ThrowIfNull(row);\n"
      << "        using var batch = " << msg << ".ToArrow(new[] { row });\n"
      << "        using var bound = _codec.Bind(batch);\n"
      << "        _publisher.Publish(Topic, bound, 0, attachments);\n    }\n\n"
      << "    /// <summary>Publishes every row, building and binding them once: the fast "
         "path.</summary>\n"
      << "    /// <remarks>A failure at row k leaves rows before it published: the runtime does "
         "not unwind a partial publication.</remarks>\n"
      << "    public void Publish(global::System.Collections.Generic.IEnumerable<" << msg
      << "> rows)\n    {\n"
      << "        global::System.ArgumentNullException.ThrowIfNull(rows);\n"
      << "        using var batch = " << msg << ".ToArrow(rows);\n"
      << "        if (batch.Length == 0) return;\n"
      << "        using var bound = _codec.Bind(batch);\n"
      << "        _publisher.Publish(Topic, bound);\n    }\n\n"
      << "    /// <summary>Releases the publisher and its codec.</summary>\n"
      << "    public void Dispose()\n    {\n"
      << "        _publisher.Dispose();\n        _codec.Dispose();\n    }\n"
      << "}\n";
    return o.str();
}

std::string GenerateSubscriberPair(const PubSubMethod& p, const std::string& msg) {
    const std::string cls = p.Base() + "Subscriber";
    std::ostringstream o;
    o << "/// <summary>Receives <see cref=\"" << msg << "\"/> rows published on " << p.service
      << "." << p.method << ", over one provider.</summary>\n"
      << "/// <remarks>\n"
      << "/// Each row is decoded with <see cref=\"" << msg
      << ".Schema\"/>, as C++'s generated subscriber decodes\n"
      << "/// with its own class. There is no <c>SubscribeInPlace</c> (Q16): for many rows, "
         "receive\n"
      << "/// batches with <c>Eiva.Fletcher.SubscriberArrow</c> and read them with <see "
         "cref=\""
      << msg << ".FromArrow\"/>.\n"
      << "/// </remarks>\n"
      << "public sealed class " << cls << " : global::System.IDisposable\n{\n"
      << PairStatics(p, msg) << "    private readonly " << kNs << "Subscriber _subscriber;\n"
      << "    private readonly " << kNs << "FletcherCodec _codec;\n\n"
      << "    /// <summary>Binds to <paramref name=\"provider\"/>; the topic is not declared "
         "here.</summary>\n"
      << "    /// <param name=\"provider\">Borrowed; it must outlive this subscriber.</param>\n"
      << "    public " << cls << "(" << kNs << "PubSubProviderHandle provider)\n    {\n"
      << "        global::System.ArgumentNullException.ThrowIfNull(provider);\n"
      << "        _codec = new " << kNs << "FletcherCodec(" << msg << ".Schema);\n"
      << "        try\n        {\n"
      << "            _subscriber = new " << kNs << "Subscriber(provider);\n"
      << "        }\n        catch\n        {\n"
      << "            _codec.Dispose();\n            throw;\n        }\n    }\n\n"
      << "    /// <summary>Begins delivering rows to <paramref name=\"handler\"/>; never "
         "blocks.</summary>\n"
      << "    /// <remarks>\n"
      << "    /// The handler runs on a transport thread with the row it owns and the "
         "attachments\n"
      << "    /// BORROWED for the call. An exception it throws is absorbed and raised as\n"
      << "    /// <see cref=\"HandlerFaulted\"/>.\n"
      << "    /// </remarks>\n"
      << "    /// <param name=\"handler\">Called once per row.</param>\n"
      << "    /// <param name=\"options\">The topic's options, or <see langword=\"null\"/> for "
         "the provider's defaults.</param>\n"
      << "    /// <returns>The subscription; dispose it or pass it to <see "
         "cref=\"Unsubscribe\"/> to stop.</returns>\n"
      << "    public " << kNs << "Subscription Subscribe(" << p.Base() << "Handler handler, " << kNs
      << "TopicOptions? options = null)\n    {\n"
      << "        global::System.ArgumentNullException.ThrowIfNull(handler);\n"
      << "        var codec = _codec;\n"
      << "        var result = _subscriber.Subscribe(Topic, (bytes, schema, attachments) =>\n"
      << "        {\n"
      << "            using var batch = codec.Decode(bytes);\n"
      << "            var columns = new global::Apache.Arrow.StructArray(\n"
      << "                new global::Apache.Arrow.Types.StructType(batch.Schema.FieldsList), "
         "batch.Length, batch.Arrays,\n"
      << "                global::Apache.Arrow.ArrowBuffer.Empty, 0);\n"
      << "            handler(" << msg << ".FromArrow(columns, 0), attachments);\n"
      << "        }, options);\n"
      << "        result.Schema.Dispose();\n"
      << "        return result.Subscription;\n    }\n\n"
      << "    /// <summary>Stops a subscription; no delivery begins after this "
         "returns.</summary>\n"
      << "    public void Unsubscribe(" << kNs
      << "Subscription subscription) => _subscriber.Unsubscribe(subscription);\n\n"
      << "    /// <summary>Raised when a handler throws; the exception does not reach the "
         "transport.</summary>\n"
      << "    public event global::System.EventHandler<" << kNs
      << "HandlerFaultedEventArgs>? HandlerFaulted\n    {\n"
      << "        add => _subscriber.HandlerFaulted += value;\n"
      << "        remove => _subscriber.HandlerFaulted -= value;\n    }\n\n"
      << "    /// <summary>How many handler exceptions have been absorbed.</summary>\n"
      << "    public ulong AbsorbedCallbackFailures => _subscriber.AbsorbedCallbackFailures;\n\n"
      << "    /// <summary>Releases the subscriber and its codec.</summary>\n"
      << "    public void Dispose()\n    {\n"
      << "        _subscriber.Dispose();\n        _codec.Dispose();\n    }\n"
      << "}\n";
    return o.str();
}

// The opening every generated C# file shares. The <auto-generated> block is what
// Roslyn's analysers look for to skip a file, as protoc's own C# output does.
std::string FileHeader(const FileDescriptor* file) {
    std::ostringstream o;
    o << "// <auto-generated>\n"
      << "//     Generated by fletcher-protoc. DO NOT EDIT.\n"
      << "//     Source: " << file->name() << "\n"
      << "// </auto-generated>\n"
      << "#nullable enable\n\n"
      << "namespace " << CsNamespace(file) << ";\n";
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
            // A mapped field without a C# type says why, in the classifier's own words. The
            // common case today is a message declared in another file (a repeated
            // google.protobuf.Timestamp is one: the IR maps it as a struct), which is BIND-6e's.
            std::string why;
            std::set<const Descriptor*> seen{msg};
            ClassifyColumn(node, file_, seen, why);
            o << "    // Not generated yet: field '" << rec.name << "' (" << PendingKind(node)
              << ") has no C# type here: " << why << ".\n";
            if (pending.empty()) pending = "field '" + rec.name + "' is " + why + ".";
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
    // Without the <auto-generated> block a consumer building with analysers as errors
    // fails on our names by design (CA1707 on Outer_Inner and Player_, CA1819 on
    // byte[], CA1069 on enum aliases).
    o << FileHeader(file_);

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

    // One topic class per pub/sub method (D-BIND-77), in the model layer because the
    // native pair and BIND-8's gateway pair both need it. A method that is not pub/sub
    // is named with the reason C++ and TypeScript give.
    for (const auto& p : PubSubMethods(file_)) {
        o << "\n";
        if (!p.skip_reason.empty())
            o << "// Skipped: " << p.service << "." << p.method << " — " << p.skip_reason << "\n";
        else
            o << GenerateTopicClass(file_, p);
    }
    return o.str();
}

std::string CsVisitor::GenerateNativeFile() {
    std::ostringstream o;
    o << FileHeader(file_);
    for (const auto& p : PubSubMethods(file_)) {
        o << "\n";
        if (!p.skip_reason.empty()) {
            o << "// Skipped: " << p.service << "." << p.method << " — " << p.skip_reason << "\n";
            continue;
        }
        // The pair publishes through ToArrow and reads through FromArrow, so a message
        // whose conversion waits (a field from another file, BIND-6e) gets no pair.
        std::set<const Descriptor*> visiting;
        const std::string blocker = ConversionBlocker(p.input, file_, visiting);
        const std::string msg = CsTypeName(p.input);
        if (!blocker.empty() || fletcher::IsFlattenedWrapper(p.input)) {
            o << "// Skipped: " << p.service << "." << p.method << " — " << msg
              << " has no ToArrow / FromArrow: "
              << (blocker.empty() ? "it is a flatten wrapper." : blocker) << "\n";
            continue;
        }
        o << "/// <summary>Receives one <see cref=\"" << msg << "\"/> published on " << p.service
          << "." << p.method << ", with its attachments.</summary>\n"
          << "/// <param name=\"row\">The decoded row; the handler owns it.</param>\n"
          << "/// <param name=\"attachments\">BORROWED for this call only.</param>\n"
          << "public delegate void " << p.Base() << "Handler(" << msg << " row, " << kNs
          << "AttachmentsView attachments);\n\n"
          << GeneratePublisherPair(p, msg) << "\n"
          << GenerateSubscriberPair(p, msg);
    }
    return o.str();
}

}  // namespace fletcher::csharp_backend
