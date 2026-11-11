// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include "csharp_backend_accessor_visitor.hpp"

#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "cpp_backend_schema_visitor.hpp"
#include "csharp_backend_type_table.hpp"
#include "generator_internal.hpp"

namespace fletcher::csharp_backend {

namespace {

using google::protobuf::Descriptor;
using google::protobuf::EnumDescriptor;
using google::protobuf::FileDescriptor;
using ir::IrNode;

// The messages that get a model class with a Schema, and so an accessor and a view: the
// model's own set, without recursive messages (no class) and flatten wrappers (no Schema).
std::vector<const Descriptor*> AccessorMessages(const FileDescriptor* file) {
    std::vector<const Descriptor*> out;
    for (const auto* msg : fletcher::OrderedMessages(file))
        if (!fletcher::IsRecursive(msg) && !fletcher::IsFlattenedWrapper(msg)) out.push_back(msg);
    return out;
}

// The name a message is called by in generated messages: its class name without the `@`.
std::string Label(const Descriptor* msg) {
    const std::string view = CsViewName(msg);
    return view.substr(0, view.size() - 4);
}

// Q6, D-BIND-9: C++'s accessor reads lists of lists to depth 3, and so does this one.
constexpr int kMaxListDepth = 3;

// How many list levels column `c` has: 0 for a scalar, a struct or a map, 1 for a list.
int ListDepth(const Column& c) {
    if (c.kind != ColKind::LIST) return 0;
    int depth = 1;
    for (const Elem* e = &c.elem; e->is_list; e = e->inner.get()) ++depth;
    return depth;
}

// One field of a message as the accessor and view see it, in Schema column order.
struct AccessorField {
    size_t index = 0;
    std::string name;              // the proto field name, for messages
    std::string property;          // empty when the model has no property for it
    std::string type_text;         // the model property's type, without `?`
    bool nullable = false;         // the model property's `?`
    std::optional<Column> column;  // nullopt when this accessor does not read it
    std::string why;               // when not read: the reason
    std::string span;              // a string or bytes field: its ReadOnlySpan<byte> getter
};

std::string AccessorRefusalOf(const Descriptor* msg, std::set<const Descriptor*>& walking);

// The messages a column reads through their own accessors: a struct's, and the struct leaf of a
// list or a map value.
std::vector<const Descriptor*> ReachedMessages(const Column& c) {
    std::vector<const Descriptor*> out;
    if (c.kind == ColKind::STRUCT) out.push_back(c.msg);
    if (c.kind == ColKind::LIST || c.kind == ColKind::MAP) {
        const Elem* e = &c.elem;
        while (e->is_list) e = e->inner.get();
        if (e->is_struct) out.push_back(e->msg);
    }
    return out;
}

// The fields of `msg`, walked as CsVisitor::GenerateMessage walks them, so every property
// here carries the model's name (the same D-BIND-89 taken set, in the same order). `walking`
// holds the messages whose accessors are being asked whether they refuse (T-D1).
std::vector<AccessorField> FieldsOf(const Descriptor* msg, const FileDescriptor* file,
                                    const CrossFile& cross, std::set<const Descriptor*>& walking) {
    std::vector<AccessorField> out;
    const auto records = cpp_backend::BuildFlattenedFieldList(msg);
    std::set<const Descriptor*> visiting{msg};
    std::set<std::string> taken;
    for (size_t i = 0; i < records.size(); ++i) {
        const auto& rec = records[i];
        const IrNode& node = *rec.node;
        AccessorField f;
        f.index = i;
        f.name = rec.name;
        const auto field_type = CsFieldTypeOf(node, file);
        if (!field_type.has_value()) {
            f.why = std::string("a ") + PendingKind(node) + " the model class has no property for";
            out.push_back(std::move(f));
            continue;
        }
        f.property = CsUniquePropertyName(rec.name, msg, taken);
        f.type_text = field_type->type_text;
        f.nullable = field_type->nullable;
        std::string why;
        auto column = ClassifyColumn(node, file, cross, visiting, why);
        if (!column) {
            f.why = why;
        } else if (ListDepth(*column) > kMaxListDepth) {
            // Q6: lists of lists are read to the depth C++'s accessor reads, 3, and a deeper one
            // keeps the class and refuses, as C++ does (D-BIND-96).
            f.why = "a list nested " + std::to_string(ListDepth(*column)) +
                    " deep, beyond the accessor's cap of " + std::to_string(kMaxListDepth);
        } else if (std::string refused =
                       [&] {
                           for (const auto* reached : ReachedMessages(*column)) {
                               std::string why = AccessorRefusalOf(reached, walking);
                               if (!why.empty()) return why;
                           }
                           return std::string();
                       }();
                   !refused.empty()) {
            // T-D1 of BIND-7's review, D-BIND-96: a message whose conversion reaches one this
            // accessor cannot read refuses up front, naming the inner reason, and its view has a
            // comment where the property would be.
            f.why = "a message whose accessor refuses (" + refused + ")";
        } else {
            column->index = i;
            column->property = f.property;
            f.column = std::move(*column);
        }
        out.push_back(std::move(f));
    }
    // D-BIND-102: a string field's UTF-8 bytes and a bytes field's bytes, read in place, named
    // after every model property so none of them moves, and never the view's own name.
    for (auto& f : out) {
        if (!f.column || f.column->kind != ColKind::SCALAR) continue;
        const std::string& type = f.column->scalar.type_text;
        if (type != "string" && type != "byte[]") continue;
        f.span = f.property + (type == "string" ? "Utf8" : "Span");
        while (taken.count(f.span) != 0 || f.span == CsViewName(msg)) f.span += '_';
        taken.insert(f.span);
    }
    return out;
}

std::vector<AccessorField> FieldsOf(const Descriptor* msg, const FileDescriptor* file,
                                    const CrossFile& cross) {
    std::set<const Descriptor*> walking{msg};
    return FieldsOf(msg, file, cross, walking);
}

// T-D1: the first reason `msg`'s accessor always refuses, as its TryMake words it, or empty. A
// message already being asked does not refuse: a cycle is a recursive message, never generated.
std::string AccessorRefusalOf(const Descriptor* msg, std::set<const Descriptor*>& walking) {
    if (!walking.insert(msg).second) return {};
    std::string refusal;
    for (const auto& f :
         FieldsOf(msg, msg->file(), CollectCrossFileMessages(msg->file()), walking)) {
        if (f.column) continue;
        refusal = Label(msg) + " column " + std::to_string(f.index) + " '" + f.name + "': " + f.why;
        break;
    }
    walking.erase(msg);
    return refusal;
}

// The opening every generated C# file shares, as CsVisitor writes it.
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

// The view type of a struct column, as a declaration in `file` names it.
std::string ViewRef(const Descriptor* msg, const FileDescriptor* file) {
    if (CsNamespace(msg->file()) == CsNamespace(file)) return CsViewName(msg);
    return CsViewExpr(msg);
}

std::string ArrayClass(const Column& c) {
    switch (c.kind) {
        case ColKind::SCALAR:
            return "global::Apache.Arrow." + c.scalar.arrow_array;
        case ColKind::TEMPORAL:
            return c.is_timestamp ? "global::Apache.Arrow.TimestampArray"
                                  : "global::Apache.Arrow.DurationArray";
        case ColKind::LIST:
            return "global::Apache.Arrow.ListArray";
        case ColKind::MAP:
            return "global::Apache.Arrow.MapArray";
        default:
            return "global::Apache.Arrow.StructArray";
    }
}

constexpr const char* kModel = "global::Eiva.Fletcher.Model.";

// What one element of a list, or one key or value of a map, reads as (D-BIND-101): a scalar as
// the model's type, a struct as its view, null on a null element, and an inner list as a window.
std::string ElemType(const Elem& e, const FileDescriptor* file) {
    if (e.is_list) return std::string(kModel) + "ListView<" + ElemType(*e.inner, file) + ">";
    if (e.is_struct) return ViewRef(e.msg, file) + "?";
    return e.scalar.type_text;
}

// The Eiva.Fletcher.Model reader of a scalar element child.
std::string ScalarElements(const CsScalarInfo& info) {
    if (info.type_text == "bool") return std::string(kModel) + "BooleanElements";
    if (info.type_text == "string") return std::string(kModel) + "StringElements";
    if (info.type_text == "byte[]") return std::string(kModel) + "BinaryElements";
    if (info.is_enum) return std::string(kModel) + "EnumElements<" + info.type_text + ">";
    return std::string(kModel) + "PrimitiveElements<" + info.type_text + ">";
}

// Statements in FromColumns that validate the element child `child` (a C# expression) against
// the Arrow type `expected` and leave its reader, an IArrowElements of ElemType(e), in `l<id>`.
// A struct element is validated by its own accessor; an inner list recurses (`id` + "_").
void EmitElements(std::ostringstream& o, const Elem& e, const std::string& child,
                  const std::string& expected, const std::string& where, const std::string& id,
                  const FileDescriptor* file) {
    const std::string in = "        ";
    const std::string x = "x" + id;
    const auto refuse = [&](const std::string& test) {
        o << in << "if (" << child << " is not " << test << ")\n"
          << in << "{\n"
          << in << "    error = Mismatch(" << CsStringLiteral(where) << ", " << expected << ", "
          << child << ");\n"
          << in << "    return false;\n"
          << in << "}\n";
    };
    if (e.is_list) {
        refuse("global::Apache.Arrow.ListArray " + x + " || " + x +
               ".Data.DataType.TypeId != global::Apache.Arrow.Types.ArrowTypeId.List");
        EmitElements(o, *e.inner, x + ".Values",
                     "((global::Apache.Arrow.Types.ListType)" + expected + ").ValueDataType",
                     where + " element", id + "_", file);
        o << in << "var l" << id << " = new " << kModel << "ListElements<"
          << ElemType(*e.inner, file) << ">(" << x << ", l" << id << "_);\n";
        return;
    }
    if (e.is_struct) {
        refuse("global::Apache.Arrow.StructArray " + x + " || " + x +
               ".Data.DataType.TypeId != global::Apache.Arrow.Types.ArrowTypeId.Struct");
        o << in << "if (!" << CsAccessorExpr(e.msg) << ".TryMake(" << x << ", out var a" << id
          << ", out var e" << id << "))\n"
          << in << "{\n"
          << in << "    error = " << CsStringLiteral(where + ": ") << " + e" << id << ";\n"
          << in << "    return false;\n"
          << in << "}\n"
          << in << "var l" << id << " = new " << kModel << "ViewElements<" << ViewRef(e.msg, file)
          << ">(a" << id << ");\n";
        return;
    }
    refuse("global::Apache.Arrow." + e.scalar.arrow_array + " " + x + " || !SameType(" + x +
           ".Data.DataType, " + expected + ")");
    o << in << "var l" << id << " = new " << ScalarElements(e.scalar) << "(" << x << ");\n";
}

constexpr const char* kDict =
    "global::System.Collections.Generic.IReadOnlyDictionary<string, string>";
constexpr const char* kNotNullWhen = "[global::System.Diagnostics.CodeAnalysis.NotNullWhen";

// Statements in FromColumns that validate column `c` and leave it, cast, in `c<n>` (a struct:
// its accessor in `a<n>`).
void EmitValidate(std::ostringstream& o, const Column& c, const std::string& where,
                  const std::string& schema, const FileDescriptor* file) {
    const std::string n = std::to_string(c.index);
    const std::string in = "        ";
    const std::string expected = schema + ".GetFieldByIndex(" + n + ").DataType";
    // A composite column is checked by its type id, then element by element: MapArray derives
    // from ListArray, so the class alone would take a map for a list.
    const char* type_id = c.kind == ColKind::STRUCT ? "Struct"
                          : c.kind == ColKind::LIST ? "List"
                          : c.kind == ColKind::MAP  ? "Map"
                                                    : nullptr;
    if (type_id != nullptr) {
        o << in << "if (columns[" << n << "] is not " << ArrayClass(c) << " c" << n << " || c" << n
          << ".Data.DataType.TypeId != global::Apache.Arrow.Types.ArrowTypeId." << type_id << ")\n";
    } else {
        o << in << "if (columns[" << n << "] is not " << ArrayClass(c) << " c" << n
          << " || !SameType(c" << n << ".Data.DataType, " << expected << "))\n";
    }
    o << in << "{\n"
      << in << "    error = Mismatch(" << CsStringLiteral(where) << ", " << expected << ", columns["
      << n << "]);\n"
      << in << "    return false;\n"
      << in << "}\n";
    // As C++: the nullable FLAG is tolerated, but a column the proto says is never null must
    // hold no nulls, so its getter can return a plain value (D-RBA-4).
    // D-BIND-106: under a StructArray source only the rows where the struct itself is valid count;
    // a null struct's children may hold anything, nulls included.
    if (!c.nullable) {
        o << in << "var n" << n << " = NullsUnderValidRows(c" << n << ", validity);\n"
          << in << "if (n" << n << " != 0)\n"
          << in << "{\n"
          << in << "    error = $\"" << where << ": non-nullable, found {n" << n << "} nulls.\";\n"
          << in << "    return false;\n"
          << in << "}\n";
    }
    if (c.kind == ColKind::STRUCT) {
        // StructArray.Fields returns each child windowed to the struct's own slice (D-BIND-10),
        // so the inner accessor reads row r of the struct at row r.
        o << in << "if (!" << CsAccessorExpr(c.msg) << ".TryMake(c" << n << ", out var a" << n
          << ", out var e" << n << "))\n"
          << in << "{\n"
          << in << "    error = " << CsStringLiteral(where + ": ") << " + e" << n << ";\n"
          << in << "    return false;\n"
          << in << "}\n";
    }
    // A list's values child, and a map's keys and values, are whole (not sliced to the column),
    // and the column's ValueOffsets index them absolutely, so each element reader is built once
    // over the whole child.
    if (c.kind == ColKind::LIST) {
        EmitElements(o, c.elem, "c" + n + ".Values",
                     "((global::Apache.Arrow.Types.ListType)" + expected + ").ValueDataType",
                     where + " element", n, file);
    }
    if (c.kind == ColKind::MAP) {
        const std::string map_type = "((global::Apache.Arrow.Types.MapType)" + expected + ")";
        EmitElements(o, c.key, "c" + n + ".Keys", map_type + ".KeyField.DataType", where + " key",
                     n + "k", file);
        EmitElements(o, c.elem, "c" + n + ".Values", map_type + ".ValueField.DataType",
                     where + " value", n, file);
    }
}

std::string GenerateAccessor(const Descriptor* msg, const FileDescriptor* file,
                             const CrossFile& cross) {
    const auto fields = FieldsOf(msg, file, cross);
    const std::string cls = CsAccessorName(msg);
    const std::string view = CsViewName(msg);
    const std::string label = Label(msg);
    const std::string model = CsTypeExpr(msg);
    const std::string schema = model + ".Schema";
    std::string refusal;
    for (const auto& f : fields) {
        if (f.column) continue;
        refusal =
            label + " column " + std::to_string(f.index) + " '" + f.name + "': " + f.why + ".";
        break;
    }
    // Whether the model has FromArrow, which ToRow returns through (D-BIND-102).
    std::set<const Descriptor*> seen;
    const bool model_converts = ConversionBlocker(msg, file, cross, seen).empty();

    std::ostringstream o;
    o << "/// <summary>Reads <see cref=\"" << CsTypeName(msg)
      << "\"/> rows from Arrow column by column: every column is\n"
      << "/// validated and cast once, by TryMake, and read in place by <see cref=\"" << view
      << "\"/>.</summary>\n"
      << "/// <remarks>\n"
      << "/// Validation is positional and type-only: column names and the nullable flag are "
         "tolerated,\n"
      << "/// and a column the proto says is never null must hold no nulls. TryMake never "
         "throws on\n"
      << "/// bad input and returns the reason instead (D-BIND-94). It BORROWS its source: it, "
         "its\n"
      << "/// views, their windows and span getters are valid only while the source batch or "
         "array\n"
      << "/// is not disposed, whose buffers Dispose frees (D-BIND-103).\n"
      << "/// </remarks>\n"
      // IArrowRows: what a list of this message, or a map with it as value, reads rows through
      // (D-BIND-101). Its two members are the accessor's own IsNull and At: no member is added.
      << "public sealed class " << cls << " : " << kModel << "IArrowRows<" << view << ">\n{\n"
      << "    private static readonly " << kDict
      << " NoMetadata =\n"
         "        global::System.Collections.ObjectModel.ReadOnlyDictionary<string, "
         "string>.Empty;\n";
    if (!refusal.empty()) {
        // A field this accessor cannot read keeps the class, and TryMake refuses with the
        // reason, as C++'s Make does (D-BIND-96). A static field, not a constant, so the
        // statements after the refusal are not unreachable code.
        o << "    private static readonly string? Refusal = " << CsStringLiteral(refusal) << ";\n";
    }
    o << "    private readonly int _length;\n"
      << "    private readonly global::Apache.Arrow.StructArray? _validity;\n"
      << "    private readonly " << kDict << " _schemaMetadata;\n"
      << "    private readonly global::Apache.Arrow.Field[] _fields;\n"
      << "    private readonly global::Apache.Arrow.StructArray _rows;\n";
    for (const auto& f : fields) {
        if (!f.column) continue;
        const auto& c = *f.column;
        const std::string n = std::to_string(c.index);
        if (c.kind == ColKind::STRUCT)
            o << "    internal " << CsAccessorExpr(c.msg) << " _c" << n << " = null!;\n";
        else
            o << "    internal " << ArrayClass(c) << " _c" << n << " = null!;\n";
        if (c.kind == ColKind::TEMPORAL)
            o << "    internal global::Apache.Arrow.Types."
              << (c.is_timestamp ? "TimestampType" : "DurationType") << " _t" << n << " = null!;\n";
        if (c.kind == ColKind::MAP)
            o << "    internal " << kModel << "IArrowElements<" << ElemType(c.key, file) << "> _k"
              << n << " = null!;\n";
        if (c.kind == ColKind::LIST || c.kind == ColKind::MAP)
            o << "    internal " << kModel << "IArrowElements<" << ElemType(c.elem, file) << "> _e"
              << n << " = null!;\n";
    }
    o << "\n"
      << "    private " << cls << "(int length, global::Apache.Arrow.StructArray? validity, "
      << kDict << " schemaMetadata,\n"
      << "        global::Apache.Arrow.Field[] fields, global::Apache.Arrow.StructArray rows)\n"
      << "    {\n"
      << "        _length = length;\n"
      << "        _validity = validity;\n"
      << "        _schemaMetadata = schemaMetadata;\n"
      << "        _fields = fields;\n"
      << "        _rows = rows;\n"
      << "    }\n\n"
      << "    /// <summary>The number of rows.</summary>\n"
      << "    public int Length => _length;\n\n"
      << "    /// <summary>Whether row <paramref name=\"row\"/> is null: always false for a "
         "RecordBatch, the\n"
      << "    /// struct's own validity for a StructArray.</summary>\n"
      << "    public bool IsNull(int row)\n"
      << "    {\n"
      << "        CheckRow(row);\n"
      << "        return _validity is not null && _validity.IsNull(row);\n"
      << "    }\n\n"
      << "    /// <summary>Row <paramref name=\"row\"/>, read in place. The view borrows this "
         "accessor.</summary>\n"
      << "    public " << view << " At(int row)\n"
      << "    {\n"
      << "        CheckRow(row);\n"
      << "        return new " << view << "(this, row);\n"
      << "    }\n\n"
      << "    /// <summary>Row <paramref name=\"row\"/> as a new <see cref=\"" << CsTypeName(msg)
      << "\"/>, read by its <c>FromArrow</c>, so it is\n"
      << "    /// the row the model and its subscriber read (D-BIND-102). A null row of a "
         "StructArray reads its\n"
      << "    /// children's values; <see cref=\"IsNull\"/> tells.</summary>\n"
      << "    public " << CsTypeName(msg) << " ToRow(int row)\n"
      << "    {\n"
      << "        CheckRow(row);\n";
    if (model_converts)
        o << "        return " << model << ".FromArrow(_rows, row);\n";
    else
        // No FromArrow exists for this message, and its TryMake always refuses, so no accessor
        // of it can be made: the statement only has to compile.
        o << "        throw new global::System.InvalidOperationException(Refusal);\n";
    o << "    }\n\n"
      << "    /// <summary>The batch schema's metadata; empty, never null, for a StructArray or "
         "when absent.</summary>\n"
      << "    public " << kDict << " SchemaMetadata => _schemaMetadata;\n\n"
      << "    /// <summary>The metadata of column <paramref name=\"index\"/>; empty, never null, "
         "when absent.</summary>\n"
      << "    public " << kDict << " FieldMetadata(int index)\n"
      << "    {\n"
      << "        if ((uint)index >= (uint)_fields.Length)\n"
      << "            throw new global::System.ArgumentOutOfRangeException(nameof(index), index, "
         "null);\n"
      << "        return _fields[index].Metadata ?? NoMetadata;\n"
      << "    }\n\n"
      << "    /// <summary>The metadata of the first column named <paramref name=\"name\"/> in "
         "the batch, which\n"
      << "    /// may name its columns differently from the proto; empty when none "
         "is.</summary>\n"
      << "    public " << kDict << " FieldMetadata(string name)\n"
      << "    {\n"
      << "        global::System.ArgumentNullException.ThrowIfNull(name);\n"
      << "        foreach (var field in _fields)\n"
      << "            if (field.Name == name) return field.Metadata ?? NoMetadata;\n"
      << "        return NoMetadata;\n"
      << "    }\n\n"
      << "    /// <summary>The schema metadata's value for <paramref name=\"key\"/>, or null "
         "when absent.</summary>\n"
      << "    public string? Metadata(string key)\n"
      << "    {\n"
      << "        global::System.ArgumentNullException.ThrowIfNull(key);\n"
      << "        return _schemaMetadata.TryGetValue(key, out var value) ? value : null;\n"
      << "    }\n\n"
      << "    /// <summary>Validates <paramref name=\"batch\"/> and casts its columns "
         "once.</summary>\n"
      << "    /// <returns>True with the accessor, or false with the reason in <paramref "
         "name=\"error\"/>.</returns>\n"
      << "    public static bool TryMake(global::Apache.Arrow.RecordBatch? batch,\n"
      << "        " << kNotNullWhen << "(true)] out " << cls << "? accessor,\n"
      << "        " << kNotNullWhen << "(false)] out string? error)\n"
      << "    {\n"
      << "        if (batch is null)\n"
      << "        {\n"
      << "            accessor = null;\n"
      << "            error = " << CsStringLiteral(label + ": the batch is null.") << ";\n"
      << "            return false;\n"
      << "        }\n"
      << "        try\n"
      << "        {\n"
      << "            return FromColumns(batch.Length, [.. batch.Arrays], [.. "
         "batch.Schema.FieldsList],\n"
      << "                batch.Schema.Metadata, null, out accessor, out error);\n"
      << "        }\n"
      << "        catch (global::System.Exception exception) when (IsMalformedInput(exception))\n"
      << "        {\n"
      << "            accessor = null;\n"
      << "            error = $\"" << label
      << ": the batch is malformed ({exception.GetType().Name}: {exception.Message})\";\n"
      << "            return false;\n"
      << "        }\n"
      << "    }\n\n"
      << "    /// <summary>Validates the children of <paramref name=\"array\"/>, a column of "
         "this message, and\n"
      << "    /// casts them once.</summary>\n"
      << "    /// <returns>True with the accessor, or false with the reason in <paramref "
         "name=\"error\"/>.</returns>\n"
      << "    public static bool TryMake(global::Apache.Arrow.StructArray? array,\n"
      << "        " << kNotNullWhen << "(true)] out " << cls << "? accessor,\n"
      << "        " << kNotNullWhen << "(false)] out string? error)\n"
      << "    {\n"
      << "        if (array is null)\n"
      << "        {\n"
      << "            accessor = null;\n"
      << "            error = " << CsStringLiteral(label + ": the array is null.") << ";\n"
      << "            return false;\n"
      << "        }\n"
      << "        try\n"
      << "        {\n"
      << "            // Fields is read once: Apache.Arrow 23 allocates on every read of it.\n"
      << "            return FromColumns(array.Length, [.. array.Fields],\n"
      << "                [.. "
         "((global::Apache.Arrow.Types.StructType)array.Data.DataType).Fields], "
         "null, array,\n"
      << "                out accessor, out error);\n"
      << "        }\n"
      << "        catch (global::System.Exception exception) when (IsMalformedInput(exception))\n"
      << "        {\n"
      << "            accessor = null;\n"
      << "            error = $\"" << label
      << ": the array is malformed ({exception.GetType().Name}: {exception.Message})\";\n"
      << "            return false;\n"
      << "        }\n"
      << "    }\n\n"
      << "    private static bool FromColumns(int length, global::Apache.Arrow.IArrowArray[] "
         "columns,\n"
      << "        global::Apache.Arrow.Field[] fields, " << kDict << "? schemaMetadata,\n"
      << "        global::Apache.Arrow.StructArray? validity,\n"
      << "        " << kNotNullWhen << "(true)] out " << cls << "? accessor,\n"
      << "        " << kNotNullWhen << "(false)] out string? error)\n"
      << "    {\n"
      << "        accessor = null;\n";
    if (!refusal.empty()) {
        o << "        if (Refusal is not null)\n"
          << "        {\n"
          << "            error = Refusal;\n"
          << "            return false;\n"
          << "        }\n";
    }
    o << "        if (columns.Length != " << fields.size()
      << " || fields.Length != " << fields.size() << ")\n"
      << "        {\n"
      << "            error = $\"" << label << ": expected " << fields.size()
      << " columns, got {columns.Length}.\";\n"
      << "            return false;\n"
      << "        }\n"
      << "        for (var i = 0; i < columns.Length; i++)\n"
      << "        {\n"
      << "            if (columns[i] is null)\n"
      << "            {\n"
      << "                error = $\"" << label << " column {i}: the array is null.\";\n"
      << "                return false;\n"
      << "            }\n"
      << "            if (columns[i].Length != length)\n"
      << "            {\n"
      << "                error = $\"" << label
      << " column {i}: {columns[i].Length} rows, expected {length}.\";\n"
      << "                return false;\n"
      << "            }\n"
      << "        }\n";
    for (const auto& f : fields) {
        if (!f.column) continue;
        EmitValidate(o, *f.column,
                     label + " column " + std::to_string(f.index) + " '" + f.name + "'", schema,
                     file);
    }
    o << "        var result = new " << cls
      << "(length, validity, schemaMetadata ?? NoMetadata, fields,\n"
      // ToRow's columns: a StructArray source itself, or a batch's columns wrapped, no copy.
      << "            validity ?? new global::Apache.Arrow.StructArray(new "
         "global::Apache.Arrow.Types.StructType(fields), length,\n"
      << "                columns, global::Apache.Arrow.ArrowBuffer.Empty, 0));\n";
    for (const auto& f : fields) {
        if (!f.column) continue;
        const auto& c = *f.column;
        const std::string n = std::to_string(c.index);
        o << "        result._c" << n << " = " << (c.kind == ColKind::STRUCT ? "a" : "c") << n
          << ";\n";
        if (c.kind == ColKind::TEMPORAL)
            o << "        result._t" << n << " = (global::Apache.Arrow.Types."
              << (c.is_timestamp ? "TimestampType" : "DurationType") << ")c" << n
              << ".Data.DataType;\n";
        if (c.kind == ColKind::MAP) o << "        result._k" << n << " = l" << n << "k;\n";
        if (c.kind == ColKind::LIST || c.kind == ColKind::MAP)
            o << "        result._e" << n << " = l" << n << ";\n";
    }
    o << "        accessor = result;\n"
      << "        error = null;\n"
      << "        return true;\n"
      << "    }\n\n"
      << "    private void CheckRow(int row)\n"
      << "    {\n"
      << "        if ((uint)row >= (uint)_length)\n"
      << "            throw new global::System.ArgumentOutOfRangeException(nameof(row), row, "
         "null);\n"
      << "    }\n\n"
      // D-BIND-106: the nulls of `column` in the rows where `parent`, the StructArray source, is
      // valid; every null when there is no parent (a batch) or it has no nulls.
      << "    private static int NullsUnderValidRows(global::Apache.Arrow.IArrowArray column, "
         "global::Apache.Arrow.StructArray? parent)\n"
      << "    {\n"
      << "        if (column.NullCount == 0 || parent is null || parent.NullCount == 0) return "
         "column.NullCount;\n"
      << "        var nulls = 0;\n"
      << "        for (var i = 0; i < column.Length; i++)\n"
      << "            if (column.IsNull(i) && !parent.IsNull(i)) nulls++;\n"
      << "        return nulls;\n"
      << "    }\n\n"
      // R-D1 of BIND-7's review: what Apache.Arrow throws on a malformed array (a null column,
      // a child it cannot build, a map whose entries are not a struct), which TryMake turns into
      // a refusal, never throwing on bad input (D-BIND-94).
      << "    private static bool IsMalformedInput(global::System.Exception exception) =>\n"
      << "        exception is global::System.ArgumentException or "
         "global::System.InvalidCastException\n"
      << "            or global::System.InvalidOperationException or "
         "global::System.NotSupportedException\n"
      << "            or global::System.NullReferenceException or "
         "global::System.IndexOutOfRangeException;\n\n"
      // Apache.Arrow's types have no value equality (Equals is by reference), so a column's
      // type is compared by its id and, for the temporal types, the unit and zone C++'s
      // DataType::Equals also compares.
      << "    private static bool SameType(global::Apache.Arrow.Types.IArrowType actual, "
         "global::Apache.Arrow.Types.IArrowType expected) =>\n"
      << "        actual.TypeId == expected.TypeId && (actual, expected) switch\n"
      << "        {\n"
      << "            (global::Apache.Arrow.Types.TimestampType a, "
         "global::Apache.Arrow.Types.TimestampType e) =>\n"
      << "                a.Unit == e.Unit && a.Timezone == e.Timezone,\n"
      << "            (global::Apache.Arrow.Types.DurationType a, "
         "global::Apache.Arrow.Types.DurationType e) => a.Unit == e.Unit,\n"
      << "            _ => true,\n"
      << "        };\n\n"
      << "    private static string Mismatch(string where, global::Apache.Arrow.Types.IArrowType "
         "expected,\n"
      << "        global::Apache.Arrow.IArrowArray actual) =>\n"
      << "        $\"{where}: expected {Describe(expected)}, got "
         "{Describe(actual.Data.DataType)}.\";\n\n"
      << "    private static string Describe(global::Apache.Arrow.Types.IArrowType type) => type "
         "switch\n"
      << "    {\n"
      << "        global::Apache.Arrow.Types.TimestampType t => $\"timestamp[{t.Unit}, "
         "{t.Timezone ?? \"no zone\"}]\",\n"
      << "        global::Apache.Arrow.Types.DurationType t => $\"duration[{t.Unit}]\",\n"
      << "        _ => type.Name,\n"
      << "    };\n"
      << "}\n";
    return o.str();
}

// The type a view property declares for column `c`: the model property's, but a struct's view
// for a struct, and a borrowed window for a list or a map (D-BIND-101).
std::string ViewType(const Column& c, const std::string& type_text, const FileDescriptor* file) {
    switch (c.kind) {
        case ColKind::STRUCT:
            return ViewRef(c.msg, file);
        case ColKind::LIST:
            return std::string(kModel) + "ListView<" + ElemType(c.elem, file) + ">";
        case ColKind::MAP:
            return std::string(kModel) + "MapView<" + ElemType(c.key, file) + ", " +
                   ElemType(c.elem, file) + ">";
        default:
            return type_text;
    }
}

// The expression a view property returns for column `c`, read at `_row`. `nullable` is the
// model property's `?`: an optional flatten-wrapper list is nullable in its column but not in
// the model (G-Q4), so its null row reads as an empty window there.
std::string ReadExpression(const Column& c, const std::string& type_text, bool nullable,
                           const FileDescriptor* file) {
    const std::string n = std::to_string(c.index);
    const std::string a = "_accessor._c" + n;
    switch (c.kind) {
        case ColKind::SCALAR: {
            const std::string& type = c.scalar.type_text;
            if (type == "byte[]")
                return c.nullable ? a + ".IsNull(_row) ? null : " + a + ".GetBytes(_row).ToArray()"
                                  : a + ".GetBytes(_row).ToArray()";
            if (type == "string") return a + ".GetString(_row)" + (c.nullable ? "" : " ?? \"\"");
            if (c.scalar.is_enum)
                return c.nullable ? a + ".GetValue(_row) is int v ? (" + type + ")v : null"
                                  : "(" + type + ")" + a + ".GetValue(_row).GetValueOrDefault()";
            return a + ".GetValue(_row)" + (c.nullable ? "" : ".GetValueOrDefault()");
        }
        case ColKind::TEMPORAL: {
            const std::string t = "_accessor._t" + n;
            const std::string unit = t + ".Unit" + (c.is_timestamp ? ", " + t + ".Timezone" : "");
            if (c.nullable)
                return a + ".GetValue(_row) is long v ? new " + type_text + "(v, " + unit +
                       ") : null";
            return "new " + type_text + "(" + a + ".GetValue(_row).GetValueOrDefault(), " + unit +
                   ")";
        }
        case ColKind::LIST:
        case ColKind::MAP: {
            const std::string window =
                ViewType(c, type_text, file) + ".Of(" + a + ", _row, " +
                (c.kind == ColKind::MAP ? "_accessor._k" + n + ", " : std::string()) +
                "_accessor._e" + n + ")";
            if (!c.nullable) return window;
            return a + ".IsNull(_row) ? " + (nullable ? "null" : "default") + " : " + window;
        }
        default:
            return c.nullable ? a + ".IsNull(_row) ? null : " + a + ".At(_row)" : a + ".At(_row)";
    }
}

std::string GenerateView(const Descriptor* msg, const FileDescriptor* file,
                         const CrossFile& cross) {
    const auto fields = FieldsOf(msg, file, cross);
    const std::string view = CsViewName(msg);
    const std::string cls = CsAccessorName(msg);
    std::ostringstream o;
    o << "/// <summary>One row of a <see cref=\"" << cls << "\"/>, read in place: its properties "
      << "are <see cref=\"" << CsTypeName(msg) << "\"/>'s,\n"
      << "/// with the same names and types.</summary>\n"
      << "/// <remarks>Borrows its accessor, and is valid only while the accessor's source is not\n"
      << "/// disposed (D-BIND-103). A string property decodes UTF-8 on every read.</remarks>\n"
      << "public readonly struct " << view << "\n{\n"
      << "    private readonly " << cls << " _accessor;\n"
      << "    private readonly int _row;\n\n"
      << "    internal " << view << "(" << cls << " accessor, int row)\n"
      << "    {\n"
      << "        _accessor = accessor;\n"
      << "        _row = row;\n"
      << "    }\n";
    for (const auto& f : fields) {
        if (!f.column) {
            o << "\n    // Not read: field '" << f.name << "' is " << f.why << ".\n";
            continue;
        }
        const auto& c = *f.column;
        o << "\n    public " << ViewType(c, f.type_text, file) << (f.nullable ? "?" : "") << " "
          << f.property << " => " << ReadExpression(c, f.type_text, f.nullable, file) << ";\n";
        if (!f.span.empty()) {
            // D-BIND-102: the same value's bytes in place, copying nothing. Apache.Arrow's GetBytes
            // reads a null value as empty even where its offsets span bytes (a test pins it), so
            // a nullable field needs no guard.
            o << "\n    public global::System.ReadOnlySpan<byte> " << f.span << " => _accessor._c"
              << c.index << ".GetBytes(_row);\n";
        }
    }
    o << "}\n";
    return o.str();
}

// Every type name the model declares in `ns`, from `file` and the files it imports.
void CollectDeclaredNames(const FileDescriptor* file, const std::string& ns,
                          std::set<const FileDescriptor*>& seen,
                          std::vector<std::pair<std::string, std::string>>& out) {
    if (!seen.insert(file).second) return;
    for (int i = 0; i < file->dependency_count(); ++i)
        CollectDeclaredNames(file->dependency(i), ns, seen, out);
    if (CsNamespace(file) != ns) return;
    auto bare = [](std::string name) { return name.rfind('@', 0) == 0 ? name.substr(1) : name; };
    for (const auto* msg : fletcher::OrderedMessages(file))
        if (!fletcher::IsRecursive(msg))
            out.emplace_back(bare(CsTypeName(msg)), "message '" + msg->full_name() + "'");
    std::vector<const EnumDescriptor*> enums;
    for (int i = 0; i < file->enum_type_count(); ++i) enums.push_back(file->enum_type(i));
    std::vector<const Descriptor*> stack;
    for (int i = 0; i < file->message_type_count(); ++i) stack.push_back(file->message_type(i));
    while (!stack.empty()) {
        const auto* m = stack.back();
        stack.pop_back();
        for (int i = 0; i < m->enum_type_count(); ++i) enums.push_back(m->enum_type(i));
        for (int i = 0; i < m->nested_type_count(); ++i) stack.push_back(m->nested_type(i));
    }
    for (const auto* enm : enums)
        out.emplace_back(bare(CsTypeName(enm)), "enum '" + enm->full_name() + "'");
}

}  // namespace

std::optional<std::string> CsAccessorNameConflict(const FileDescriptor* file) {
    const std::string ns = CsNamespace(file);
    std::set<const FileDescriptor*> seen;
    std::vector<std::pair<std::string, std::string>> declared;
    CollectDeclaredNames(file, ns, seen, declared);
    // B2 of BIND-7's review: the generated names of every visible file of the namespace, the
    // imported ones too, since `message AView` here clashes with an imported `message A`'s view.
    // The plugin cannot see whether an import was generated with csharp_accessor; it assumes so,
    // as one protoc run passes one parameter set to every file.
    for (const auto* visible : seen) {
        if (CsNamespace(visible) != ns) continue;
        for (const auto* msg : AccessorMessages(visible)) {
            for (const auto& generated : {CsViewName(msg), CsAccessorName(msg)}) {
                for (const auto& [name, what] : declared) {
                    if (name != generated) continue;
                    return "csharp_accessor: message '" + msg->full_name() +
                           "' would get the type '" + generated +
                           "', which is also the C# name of " + what + " in namespace " + ns +
                           " (D-BIND-100). Rename one of the two.";
                }
            }
        }
    }
    const CrossFile cross = CollectCrossFileMessages(file);
    for (const auto* msg : AccessorMessages(file)) {
        for (const auto& f : FieldsOf(msg, file, cross)) {
            // P-D1 of the review: only a field that gets a property can clash with the view.
            if (!f.column || f.property != CsViewName(msg)) continue;
            return "csharp_accessor: field '" + f.name + "' of message '" + msg->full_name() +
                   "' would be the property '" + f.property +
                   "' of the view struct of the same name, which C# does not allow (D-BIND-100). "
                   "Rename the field or the message.";
        }
    }
    return std::nullopt;
}

CsAccessorVisitor::CsAccessorVisitor(const FileDescriptor* file)
    : file_(file), cross_file_(CollectCrossFileMessages(file)) {}

std::string CsAccessorVisitor::GenerateAccessorFile() {
    std::ostringstream o;
    o << FileHeader(file_);
    for (const auto* msg : AccessorMessages(file_))
        o << "\n" << GenerateAccessor(msg, file_, cross_file_);
    return o.str();
}

std::string CsAccessorVisitor::GenerateViewFile() {
    std::ostringstream o;
    o << FileHeader(file_);
    for (const auto* msg : AccessorMessages(file_))
        o << "\n" << GenerateView(msg, file_, cross_file_);
    return o.str();
}

}  // namespace fletcher::csharp_backend
