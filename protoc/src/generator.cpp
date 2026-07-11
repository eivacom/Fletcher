// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include "generator.hpp"

#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/io/zero_copy_stream.h>

#include <algorithm>
#include <cstring>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "cpp_backend_decode_visitor.hpp"
#include "cpp_backend_schema_visitor.hpp"
#include "cpp_backend_type_table.hpp"
#include "cpp_backend_view_visitor.hpp"
#include "generator_internal.hpp"
#include "ir.hpp"
#include "recordbatch_accessor_emitter.hpp"
#include "schema_builder.hpp"
#include "ts_backend_visitor.hpp"
#include "type_mapper.hpp"

namespace fletcher {

namespace {

// -----------------------------------------------------------------------
// String helpers
// -----------------------------------------------------------------------

std::string StripProtoSuffix(const std::string& proto_name) {
    constexpr std::string_view kSuffix = ".proto";
    std::string base = proto_name;
    if (base.size() > kSuffix.size() && base.substr(base.size() - kSuffix.size()) == kSuffix)
        base.resize(base.size() - kSuffix.size());
    return base;
}

std::string OutputFilename(const std::string& proto_name) {
    return StripProtoSuffix(proto_name) + ".fletcher.pb.h";
}

// RecordBatch accessor outputs (RBA-1). Same stem derivation as the other
// output-name helpers; emitted only when --fletcher_opt=accessor / rust is set.
std::string AccessorOutputFilename(const std::string& proto_name) {
    return StripProtoSuffix(proto_name) + ".fletcher.accessor.pb.h";
}

std::string RustAccessorOutputFilename(const std::string& proto_name) {
    return StripProtoSuffix(proto_name) + ".fletcher.rs";
}

// Schemas are per-message while protoc output is per-file, so the message
// class name is part of the file name: <stem>.<Message>.ipc.
std::string IpcOutputFilename(const std::string& proto_name, const std::string& cls) {
    return StripProtoSuffix(proto_name) + "." + cls + ".ipc";
}

bool WriteToStream(google::protobuf::io::ZeroCopyOutputStream* out, const std::string& s,
                   std::string* error) {
    const char* data = s.data();
    size_t remaining = s.size();
    while (remaining > 0) {
        void* buf;
        int size;
        if (!out->Next(&buf, &size)) {
            *error = "ZeroCopyOutputStream::Next failed";
            return false;
        }
        const size_t n = std::min(static_cast<size_t>(size), remaining);
        std::memcpy(buf, data, n);
        if (static_cast<size_t>(size) > n) out->BackUp(static_cast<int>(size - n));
        data += n;
        remaining -= n;
    }
    return true;
}

// -----------------------------------------------------------------------
// Cross-file include collection
// -----------------------------------------------------------------------

// Scan all supported field mappings in a message (and its nested types) and
// accumulate the include paths of any cross-file generated headers needed.
void CollectCrossFileIncludesFromMessage(const google::protobuf::Descriptor* msg,
                                         std::set<std::string>& headers) {
    for (int fi = 0; fi < msg->field_count(); ++fi) {
        const auto* fd = msg->field(fi);
        if (auto m = MapField(fd)) {
            if (!m->nested_header.empty()) headers.insert(m->nested_header);
            if (!m->map_value_header.empty()) headers.insert(m->map_value_header);
        }
    }
    for (int ni = 0; ni < msg->nested_type_count(); ++ni)
        CollectCrossFileIncludesFromMessage(msg->nested_type(ni), headers);
}

// GIR-9 (#75): the EnumDescriptor a field's generated C++ typed accessors
// reference, or nullptr when the field is not enum-typed. Singular / nullable /
// repeated enum fields expose the field's own enum; an enum-valued map exposes
// its value's enum. Descriptor-based (FieldDescriptor::enum_type /
// EnumDescriptor::containing_type) — never a stored C++ string (locked #1/#7).
const google::protobuf::EnumDescriptor* FieldEnumType(const google::protobuf::FieldDescriptor* fd) {
    if (fd->is_map()) {
        const auto* val = fd->message_type()->field(1);
        return val->type() == google::protobuf::FieldDescriptor::TYPE_ENUM ? val->enum_type()
                                                                           : nullptr;
    }
    return fd->type() == google::protobuf::FieldDescriptor::TYPE_ENUM ? fd->enum_type() : nullptr;
}

// GIR-9 (#75): generated-header includes for enum types referenced from OTHER
// files. Message references already contribute a cross-file header through the
// FieldMapping (nested_header / map_value_header), but enum references do not —
// the imported enum's typed C++ accessors need its owning file's generated
// header visible. Same `<stem>.fletcher.pb.h` path shape as the message-include
// path, so the shared include set dedupes cleanly.
void CollectCrossFileEnumIncludesFromMessage(const google::protobuf::Descriptor* msg,
                                             const google::protobuf::FileDescriptor* file,
                                             std::set<std::string>& headers) {
    for (int i = 0; i < msg->field_count(); ++i) {
        const auto* fd = msg->field(i);
        // Field-level flatten inlines the referenced message's fields into this
        // message (see GatherFieldsImpl), so an inlined imported enum field's
        // typed accessor lands HERE and needs the enum's defining-file header.
        // Descend exactly as the field walk does so that path is covered.
        if (fd->type() == google::protobuf::FieldDescriptor::TYPE_MESSAGE && !fd->is_repeated() &&
            HasFieldFlatten(fd)) {
            CollectCrossFileEnumIncludesFromMessage(fd->message_type(), file, headers);
            continue;
        }
        const auto* ed = FieldEnumType(fd);
        if (ed != nullptr && ed->file() != file)
            headers.insert(StripProtoSuffix(ed->file()->name()) + ".fletcher.pb.h");
    }
    for (int ni = 0; ni < msg->nested_type_count(); ++ni)
        CollectCrossFileEnumIncludesFromMessage(msg->nested_type(ni), file, headers);
}

// -----------------------------------------------------------------------
// Topological ordering of messages
// -----------------------------------------------------------------------

void TopologicalVisit(const google::protobuf::Descriptor* msg,
                      const google::protobuf::FileDescriptor* file,
                      std::set<const google::protobuf::Descriptor*>& visiting,
                      std::set<const google::protobuf::Descriptor*>& emitted,
                      std::vector<const google::protobuf::Descriptor*>& order) {
    if (emitted.count(msg)) return;
    // GIR-9 (#75): shared in-progress cycle guard. `emitted` only marks COMPLETED
    // messages, so a dependency edge that points back into a message currently on
    // the recursion stack would loop forever. Message-type cycles are caught by
    // IsRecursive below, but the enum-owner edge can form a cycle among
    // NON-recursive messages (M's field names O's nested enum and O's field names
    // M's nested enum); this guard makes every edge (message-type AND enum-owner)
    // terminate on such a cycle instead of overflowing the stack. On a genuine
    // enum-owner cycle the emitted order cannot satisfy both references (a nested
    // enum can't be forward-declared), but the generator now finishes
    // deterministically rather than crashing.
    if (visiting.count(msg)) return;
    // Skip synthetic map-entry messages.
    if (msg->options().map_entry()) return;
    // Only generate classes for messages in this file.
    if (msg->file() != file) return;
    // Skip recursive messages entirely.
    if (IsRecursive(msg)) {
        emitted.insert(msg);
        return;
    }

    visiting.insert(msg);

    // Visit nested types first.
    for (int i = 0; i < msg->nested_type_count(); ++i)
        TopologicalVisit(msg->nested_type(i), file, visiting, emitted, order);

    // Visit message-type dependencies.
    for (int i = 0; i < msg->field_count(); ++i) {
        const auto* f = msg->field(i);
        if (f->type() != google::protobuf::FieldDescriptor::TYPE_MESSAGE) continue;
        if (f->is_map()) {
            const auto* val = f->message_type()->field(1);
            if (val->type() == google::protobuf::FieldDescriptor::TYPE_MESSAGE)
                TopologicalVisit(val->message_type(), file, visiting, emitted, order);
        } else {
            TopologicalVisit(f->message_type(), file, visiting, emitted, order);
        }
    }

    // GIR-9 (#75): enum-owner dependencies. A typed enum accessor names the
    // enum's owning generated class (e.g. `EnumOwner::InnerStatus`), and a
    // nested enum cannot be forward-declared apart from its owner, so the owner
    // must be emitted before this message — even though there is no TYPE_MESSAGE
    // field between them. Only added when the enum class is actually emittable
    // (CppEnumTypeEmittable): a nested enum whose owner is recursive or a
    // flattened wrapper is never declared, so no typed accessor (and no edge) is
    // produced for it. Skipped for top-level enums (no owner) and self-owned
    // enums (owner == msg). Cross-file owners are handled by the recursive call's
    // own in-file guard.
    for (int i = 0; i < msg->field_count(); ++i) {
        const auto* ed = FieldEnumType(msg->field(i));
        if (ed == nullptr || !cpp_backend::CppEnumTypeEmittable(ed)) continue;
        const auto* owner = ed->containing_type();
        if (owner != nullptr && owner != msg && owner->file() == file)
            TopologicalVisit(owner, file, visiting, emitted, order);
    }

    visiting.erase(msg);
    emitted.insert(msg);
    order.push_back(msg);
}

}  // namespace

// OrderedMessages, ArrowTypeExpr, and GatherFields (declared in
// generator_internal.hpp, namespace fletcher) have external linkage so the
// RecordBatch accessor emitter shares the exact same schema model. RBA-2
// relocated these definitions out of the anonymous namespace; the logic is
// unchanged and the emitted bytes are identical (guarded by the RBA-1 no-drift
// test). FieldInfo is likewise declared in generator_internal.hpp.

std::vector<const google::protobuf::Descriptor*> OrderedMessages(
    const google::protobuf::FileDescriptor* file) {
    std::set<const google::protobuf::Descriptor*> visiting;
    std::set<const google::protobuf::Descriptor*> emitted;
    std::vector<const google::protobuf::Descriptor*> order;
    for (int i = 0; i < file->message_type_count(); ++i)
        TopologicalVisit(file->message_type(i), file, visiting, emitted, order);
    return order;
}

// Relocated out of the anonymous namespace (declaration in
// generator_internal.hpp) so the accessor emitter reuses the SAME cross-file
// include discovery (D-RBA-10 single source of truth). The recursive walk helper
// (CollectCrossFileIncludesFromMessage) stays file-local above; only this public
// entry point gains external linkage. Pure linkage move — emitted bytes are
// unchanged, guarded by the RBA-1 no-drift test.
std::set<std::string> CollectCrossFileIncludes(const google::protobuf::FileDescriptor* file) {
    std::set<std::string> headers;
    for (int mi = 0; mi < file->message_type_count(); ++mi)
        CollectCrossFileIncludesFromMessage(file->message_type(mi), headers);
    return headers;
}

// -----------------------------------------------------------------------
// Arrow type expression for the schema — constructed from the FieldMapping
// -----------------------------------------------------------------------

std::string ArrowTypeExpr(const FieldInfo& fi) {
    switch (fi.mapping.kind) {
        case FieldKind::SCALAR:
            return fi.mapping.scalar.arrow_type_expr;

        case FieldKind::REPEATED_SCALAR:
            // Arrow convention: list "item" child is nullable (matches arrow::list(type) and the
            // nanoarrow schema emitted by GenerateSchemaFunction). Keep these in sync.
            return "arrow::list(arrow::field(\"item\", " + fi.mapping.element.arrow_type_expr +
                   ", true))";

        case FieldKind::STRUCT:
            return "arrow::struct_(" + fi.mapping.nested_class + "Schema()->fields())";

        case FieldKind::REPEATED_STRUCT:
            return "arrow::list(arrow::field(\"item\", arrow::struct_(" + fi.mapping.nested_class +
                   "Schema()->fields()), true))";

        case FieldKind::NESTED_LIST: {
            // Build nested arrow::list() from inside out.
            std::string expr = "arrow::struct_(" + fi.mapping.nested_class + "Schema()->fields())";
            for (int d = 0; d < fi.mapping.list_depth; ++d)
                expr = "arrow::list(arrow::field(\"item\", " + expr + ", true))";
            return expr;
        }

        case FieldKind::MAP: {
            std::string val_type =
                fi.mapping.map_value_is_message
                    ? "arrow::struct_(" + fi.mapping.map_value_class + "Schema()->fields())"
                    : fi.mapping.map_value.arrow_type_expr;
            // Map "value" child is nullable; "key" is non-nullable (Arrow spec).
            return "arrow::map(" + fi.mapping.map_key.arrow_type_expr +
                   ", arrow::field(\"value\", " + val_type + ", true))";
        }
    }
    return "/* unknown */";
}

namespace {

// -----------------------------------------------------------------------
// Storage member declaration
// -----------------------------------------------------------------------

std::string StorageDecl(const FieldInfo& fi) {
    switch (fi.mapping.kind) {
        case FieldKind::SCALAR:
            return "std::optional<" + fi.mapping.scalar.storage_type + "> " + fi.name + "_";

        case FieldKind::REPEATED_SCALAR:
            return "std::vector<" + fi.mapping.element.storage_type + "> " + fi.name + "_";

        case FieldKind::STRUCT:
            return "std::optional<" + fi.mapping.nested_class + "> " + fi.name + "_";

        case FieldKind::REPEATED_STRUCT:
            return "std::vector<" + fi.mapping.nested_class + "> " + fi.name + "_";

        case FieldKind::NESTED_LIST: {
            std::string type = fi.mapping.nested_class;
            for (int d = 0; d < fi.mapping.list_depth; ++d) type = "std::vector<" + type + ">";
            if (fi.mapping.nullable) return "std::optional<" + type + "> " + fi.name + "_";
            return type + " " + fi.name + "_";
        }

        case FieldKind::MAP: {
            std::string val_type = fi.mapping.map_value_is_message
                                       ? fi.mapping.map_value_class
                                       : fi.mapping.map_value.storage_type;
            return "std::vector<std::pair<" + fi.mapping.map_key.storage_type + ", " + val_type +
                   ">> " + fi.name + "_";
        }
    }
    return "/* unknown */ " + fi.name + "_";
}

// -----------------------------------------------------------------------
// Enum class emission (GIR-9, #75)
// -----------------------------------------------------------------------

// Emit a scoped `enum class <Name> : int32_t { SYMBOL = number, ... };`. Storage
// stays int32 (locked #2): the fixed-width base is int32_t and every enumerator
// carries its proto number verbatim (open proto3 enums round-trip unchanged).
// `indent` is "" for a top-level enum (package-namespace scope) or "    " for a
// nested enum (inside the owning generated class's public section).
void EmitEnumClass(std::ostringstream& o, const google::protobuf::EnumDescriptor* ed,
                   const std::string& indent) {
    o << indent << "enum class " << ed->name() << " : int32_t {\n";
    for (int i = 0; i < ed->value_count(); ++i)
        o << indent << "    " << ed->value(i)->name() << " = " << ed->value(i)->number() << ",\n";
    o << indent << "};\n";
}

// -----------------------------------------------------------------------
// Setter generation
// -----------------------------------------------------------------------

void EmitSetters(std::ostringstream& o, const std::string& cls,
                 const std::vector<FieldInfo>& fields) {
    for (const auto& fi : fields) {
        switch (fi.mapping.kind) {
            case FieldKind::SCALAR: {
                bool coerce = (fi.mapping.scalar.param_type != fi.mapping.scalar.storage_type);
                o << "    " << cls << "& set_" << fi.name << "(" << fi.mapping.scalar.param_type
                  << " v) {\n";
                if (coerce)
                    o << "        " << fi.name << "_ = std::string(v);\n";
                else
                    o << "        " << fi.name << "_ = v;\n";
                o << "        return *this;\n    }\n";
                if (fi.mapping.nullable) {
                    o << "    " << cls << "& clear_" << fi.name << "() {\n"
                      << "        " << fi.name << "_.reset();\n"
                      << "        return *this;\n    }\n";
                }
                // GIR-9: additive fluent typed setter for a singular/nullable
                // enum field, delegating to the raw int32 setter (storage stays
                // int32; the raw int32 overload is retained for compatibility).
                if (const auto* ed = FieldEnumType(fi.descriptor);
                    ed && cpp_backend::CppEnumTypeEmittable(ed)) {
                    const std::string en = cpp_backend::CppEnumName(ed, fi.descriptor->file());
                    o << "    " << cls << "& set_" << fi.name << "(" << en << " v) {\n"
                      << "        return set_" << fi.name << "(static_cast<int32_t>(v));\n"
                      << "    }\n";
                }
                break;
            }

            case FieldKind::REPEATED_SCALAR:
                o << "    " << cls << "& set_" << fi.name << "(std::vector<"
                  << fi.mapping.element.storage_type << "> v) {\n"
                  << "        " << fi.name << "_ = std::move(v);\n"
                  << "        return *this;\n    }\n";
                break;

            case FieldKind::STRUCT:
                o << "    " << cls << "& set_" << fi.name << "(" << fi.mapping.nested_class
                  << " v) {\n"
                  << "        " << fi.name << "_ = std::move(v);\n"
                  << "        return *this;\n    }\n";
                if (fi.mapping.nullable) {
                    o << "    " << cls << "& clear_" << fi.name << "() {\n"
                      << "        " << fi.name << "_.reset();\n"
                      << "        return *this;\n    }\n";
                }
                break;

            case FieldKind::REPEATED_STRUCT:
                o << "    " << cls << "& set_" << fi.name << "(std::vector<"
                  << fi.mapping.nested_class << "> v) {\n"
                  << "        " << fi.name << "_ = std::move(v);\n"
                  << "        return *this;\n    }\n";
                break;

            case FieldKind::NESTED_LIST: {
                std::string type = fi.mapping.nested_class;
                for (int d = 0; d < fi.mapping.list_depth; ++d) type = "std::vector<" + type + ">";
                o << "    " << cls << "& set_" << fi.name << "(" << type << " v) {\n"
                  << "        " << fi.name << "_ = std::move(v);\n"
                  << "        return *this;\n    }\n";
                if (fi.mapping.nullable) {
                    o << "    " << cls << "& clear_" << fi.name << "() {\n"
                      << "        " << fi.name << "_.reset();\n"
                      << "        return *this;\n    }\n";
                }
                break;
            }

            case FieldKind::MAP: {
                std::string val_type = fi.mapping.map_value_is_message
                                           ? fi.mapping.map_value_class
                                           : fi.mapping.map_value.storage_type;
                o << "    " << cls << "& set_" << fi.name << "(std::vector<std::pair<"
                  << fi.mapping.map_key.storage_type << ", " << val_type << ">> v) {\n"
                  << "        " << fi.name << "_ = std::move(v);\n"
                  << "        return *this;\n    }\n";
                break;
            }
        }
    }
}

// -----------------------------------------------------------------------
// Getter generation (mutable class — returns const refs to internal storage)
// -----------------------------------------------------------------------

void EmitGetters(std::ostringstream& o, const std::vector<FieldInfo>& fields) {
    for (const auto& fi : fields) {
        switch (fi.mapping.kind) {
            case FieldKind::SCALAR: {
                const auto& sc = fi.mapping.scalar;

                if (fi.mapping.nullable) {
                    if (sc.value_is_buffer) {
                        o << "    std::optional<std::string_view> " << fi.name << "() const {\n"
                          << "        if (!" << fi.name << "_.has_value()) return std::nullopt;\n"
                          << "        return std::string_view{*" << fi.name << "_};\n"
                          << "    }\n";
                    } else {
                        o << "    std::optional<" << sc.storage_type << "> " << fi.name
                          << "() const { return " << fi.name << "_; }\n";
                    }
                } else {
                    if (sc.value_is_buffer) {
                        // Can't use value_or("") — temporary would dangle.
                        o << "    std::string_view " << fi.name << "() const {\n"
                          << "        if (!" << fi.name << "_.has_value()) return {};\n"
                          << "        return *" << fi.name << "_;\n"
                          << "    }\n";
                    } else {
                        o << "    " << sc.storage_type << " " << fi.name << "() const { return "
                          << fi.name << "_.value_or(" << sc.default_value << "); }\n";
                    }
                }
                // GIR-9: additive typed getter for a singular/nullable enum
                // field (a cast over the retained raw int32 getter; no
                // validation — open proto3 values round-trip).
                if (const auto* ed = FieldEnumType(fi.descriptor);
                    ed && cpp_backend::CppEnumTypeEmittable(ed)) {
                    const std::string en = cpp_backend::CppEnumName(ed, fi.descriptor->file());
                    if (fi.mapping.nullable) {
                        o << "    std::optional<" << en << "> " << fi.name << "_typed() const {\n"
                          << "        auto v = " << fi.name << "();\n"
                          << "        if (!v.has_value()) return std::nullopt;\n"
                          << "        return static_cast<" << en << ">(*v);\n"
                          << "    }\n";
                    } else {
                        o << "    " << en << " " << fi.name << "_typed() const {\n"
                          << "        return static_cast<" << en << ">(" << fi.name << "());\n"
                          << "    }\n";
                    }
                }
                break;
            }

            case FieldKind::STRUCT:
                if (fi.mapping.nullable) {
                    o << "    const " << fi.mapping.nested_class << "* " << fi.name
                      << "() const {\n"
                      << "        return " << fi.name << "_.has_value() ? &*" << fi.name
                      << "_ : nullptr;\n"
                      << "    }\n";
                } else {
                    o << "    const " << fi.mapping.nested_class << "& " << fi.name
                      << "() const {\n"
                      << "        static const " << fi.mapping.nested_class << " kDefault{};\n"
                      << "        return " << fi.name << "_.has_value() ? *" << fi.name
                      << "_ : kDefault;\n"
                      << "    }\n";
                }
                break;

            case FieldKind::REPEATED_SCALAR:
                o << "    const std::vector<" << fi.mapping.element.storage_type << ">& " << fi.name
                  << "() const { return " << fi.name << "_; }\n";
                // GIR-9: additive typed getter for a repeated enum field. Casts
                // the value side only; the raw int32-container setter/getter are
                // retained (no typed repeated setter this round).
                if (const auto* ed = FieldEnumType(fi.descriptor);
                    ed && cpp_backend::CppEnumTypeEmittable(ed)) {
                    const std::string en = cpp_backend::CppEnumName(ed, fi.descriptor->file());
                    o << "    std::vector<" << en << "> " << fi.name << "_typed() const {\n"
                      << "        std::vector<" << en << "> out;\n"
                      << "        out.reserve(" << fi.name << "().size());\n"
                      << "        for (int32_t v : " << fi.name << "()) out.push_back(static_cast<"
                      << en << ">(v));\n"
                      << "        return out;\n"
                      << "    }\n";
                }
                break;

            case FieldKind::REPEATED_STRUCT:
                o << "    const std::vector<" << fi.mapping.nested_class << ">& " << fi.name
                  << "() const { return " << fi.name << "_; }\n";
                break;

            case FieldKind::NESTED_LIST: {
                std::string type = fi.mapping.nested_class;
                for (int d = 0; d < fi.mapping.list_depth; ++d) type = "std::vector<" + type + ">";
                if (fi.mapping.nullable) {
                    o << "    const " << type << "* " << fi.name << "() const {\n"
                      << "        return " << fi.name << "_.has_value() ? &*" << fi.name
                      << "_ : nullptr;\n"
                      << "    }\n";
                } else {
                    o << "    const " << type << "& " << fi.name << "() const { return " << fi.name
                      << "_; }\n";
                }
                break;
            }

            case FieldKind::MAP: {
                std::string val_type = fi.mapping.map_value_is_message
                                           ? fi.mapping.map_value_class
                                           : fi.mapping.map_value.storage_type;
                o << "    const std::vector<std::pair<" << fi.mapping.map_key.storage_type << ", "
                  << val_type << ">>& " << fi.name << "() const { return " << fi.name << "_; }\n";
                // GIR-9: additive typed getter for an enum-valued map. Casts the
                // value side only and preserves the raw map container shape (no
                // typed map setter this round).
                if (const auto* ed = FieldEnumType(fi.descriptor);
                    ed && cpp_backend::CppEnumTypeEmittable(ed)) {
                    const std::string en = cpp_backend::CppEnumName(ed, fi.descriptor->file());
                    const std::string& kt = fi.mapping.map_key.storage_type;
                    o << "    std::vector<std::pair<" << kt << ", " << en << ">> " << fi.name
                      << "_typed() const {\n"
                      << "        std::vector<std::pair<" << kt << ", " << en << ">> out;\n"
                      << "        out.reserve(" << fi.name << "().size());\n"
                      << "        for (const auto& e : " << fi.name << "())\n"
                      << "            out.emplace_back(e.first, static_cast<" << en
                      << ">(e.second));\n"
                      << "        return out;\n"
                      << "    }\n";
                }
                break;
            }
        }
    }
}

// -----------------------------------------------------------------------
// Gather supported fields from a message
// -----------------------------------------------------------------------

// `id_prefix` is the dotted field-number path of the chain of field-level
// flatten wrappers we have descended through (empty at the top level).  It is
// used to build a unique `field_id` for each inlined field: a field-flattened
// sub-message's fields keep their own (inner) proto `field_number`, which can
// collide with the enclosing message's numbers, so `field_id` carries the full
// path (e.g. "2.1") to disambiguate them in the schema metadata.
void GatherFieldsImpl(const google::protobuf::Descriptor* msg, std::vector<FieldInfo>& fields,
                      std::string* skipped_comment, const std::string& id_prefix) {
    for (int i = 0; i < msg->field_count(); ++i) {
        const auto* fd = msg->field(i);

        const std::string path = id_prefix.empty() ? std::to_string(fd->number())
                                                   : id_prefix + "." + std::to_string(fd->number());

        // Field-level flatten: inline the referenced message's fields, carrying
        // this field's number into the path so inlined field_ids stay unique.
        if (fd->type() == google::protobuf::FieldDescriptor::TYPE_MESSAGE && !fd->is_repeated() &&
            HasFieldFlatten(fd)) {
            GatherFieldsImpl(fd->message_type(), fields, skipped_comment, path);
            continue;
        }

        // Build the canonical IR once; the projection feeds the bridge mapping
        // and the same IR node is stored for the edge ENCODE visitor to walk.
        auto ir_node = ir::BuildFieldIr(fd);
        if (auto m = ProjectIrToFieldMapping(ir_node, fd->file())) {
            fields.push_back({fd->name(), std::move(*m), fd->number(), path, fd,
                              std::make_shared<const ir::IrNode>(std::move(ir_node))});
        } else {
            *skipped_comment += "//   " + fd->name() + ": " + UnsupportedReason(fd) + "\n";
        }
    }
}

}  // namespace

// GatherFields has external linkage (declared in generator_internal.hpp,
// namespace fletcher) so the accessor emitter reuses it; GatherFieldsImpl stays
// file-local. RBA-2 relocation only — logic and emitted bytes unchanged.
std::vector<FieldInfo> GatherFields(const google::protobuf::Descriptor* msg,
                                    std::string* skipped_comment) {
    std::vector<FieldInfo> fields;
    GatherFieldsImpl(msg, fields, skipped_comment, "");
    return fields;
}

namespace {

// -----------------------------------------------------------------------
// Free schema function for one message
// -----------------------------------------------------------------------

// Helper: emit nanoarrow type setup code for a single child schema.
// `child_expr` is the C expression for the ArrowSchema* child pointer.
void EmitNanoarrowTypeSetup(std::ostringstream& o, const std::string& child_expr,
                            const FieldInfo& fi, const std::string& indent) {
    switch (fi.mapping.kind) {
        case FieldKind::SCALAR: {
            const auto& expr = fi.mapping.scalar.arrow_type_expr;
            if (expr.find("timestamp") != std::string::npos) {
                o << indent << "ArrowSchemaSetTypeDateTime(" << child_expr
                  << ", NANOARROW_TYPE_TIMESTAMP, NANOARROW_TIME_UNIT_NANO, nullptr);\n";
            } else if (expr.find("duration") != std::string::npos) {
                o << indent << "ArrowSchemaSetTypeDateTime(" << child_expr
                  << ", NANOARROW_TYPE_DURATION, NANOARROW_TIME_UNIT_NANO, nullptr);\n";
            } else if (expr == "arrow::boolean()") {
                o << indent << "ArrowSchemaSetType(" << child_expr << ", NANOARROW_TYPE_BOOL);\n";
            } else if (expr == "arrow::int32()") {
                o << indent << "ArrowSchemaSetType(" << child_expr << ", NANOARROW_TYPE_INT32);\n";
            } else if (expr == "arrow::int64()") {
                o << indent << "ArrowSchemaSetType(" << child_expr << ", NANOARROW_TYPE_INT64);\n";
            } else if (expr == "arrow::uint32()") {
                o << indent << "ArrowSchemaSetType(" << child_expr << ", NANOARROW_TYPE_UINT32);\n";
            } else if (expr == "arrow::uint64()") {
                o << indent << "ArrowSchemaSetType(" << child_expr << ", NANOARROW_TYPE_UINT64);\n";
            } else if (expr == "arrow::float32()") {
                o << indent << "ArrowSchemaSetType(" << child_expr << ", NANOARROW_TYPE_FLOAT);\n";
            } else if (expr == "arrow::float64()") {
                o << indent << "ArrowSchemaSetType(" << child_expr << ", NANOARROW_TYPE_DOUBLE);\n";
            } else if (expr == "arrow::utf8()") {
                o << indent << "ArrowSchemaSetType(" << child_expr << ", NANOARROW_TYPE_STRING);\n";
            } else if (expr == "arrow::binary()") {
                o << indent << "ArrowSchemaSetType(" << child_expr << ", NANOARROW_TYPE_BINARY);\n";
            } else {
                o << indent << "// TODO: unknown scalar type: " << expr << "\n";
            }
            break;
        }

        case FieldKind::STRUCT:
            o << indent << "ArrowSchemaDeepCopy(" << fi.mapping.nested_class << "Schema().get(), "
              << child_expr << ");\n";
            break;

        case FieldKind::REPEATED_SCALAR: {
            // list(element_type)
            o << indent << "ArrowSchemaSetType(" << child_expr << ", NANOARROW_TYPE_LIST);\n";
            // The list child ("item") is allocated by ArrowSchemaSetType.
            // Set item type.
            const auto& elem_expr = fi.mapping.element.arrow_type_expr;
            std::string item = child_expr + "->children[0]";
            if (elem_expr.find("timestamp") != std::string::npos) {
                o << indent << "ArrowSchemaSetTypeDateTime(" << item
                  << ", NANOARROW_TYPE_TIMESTAMP, NANOARROW_TIME_UNIT_NANO, nullptr);\n";
            } else if (elem_expr.find("duration") != std::string::npos) {
                o << indent << "ArrowSchemaSetTypeDateTime(" << item
                  << ", NANOARROW_TYPE_DURATION, NANOARROW_TIME_UNIT_NANO, nullptr);\n";
            } else if (elem_expr == "arrow::boolean()") {
                o << indent << "ArrowSchemaSetType(" << item << ", NANOARROW_TYPE_BOOL);\n";
            } else if (elem_expr == "arrow::int32()") {
                o << indent << "ArrowSchemaSetType(" << item << ", NANOARROW_TYPE_INT32);\n";
            } else if (elem_expr == "arrow::int64()") {
                o << indent << "ArrowSchemaSetType(" << item << ", NANOARROW_TYPE_INT64);\n";
            } else if (elem_expr == "arrow::uint32()") {
                o << indent << "ArrowSchemaSetType(" << item << ", NANOARROW_TYPE_UINT32);\n";
            } else if (elem_expr == "arrow::uint64()") {
                o << indent << "ArrowSchemaSetType(" << item << ", NANOARROW_TYPE_UINT64);\n";
            } else if (elem_expr == "arrow::float32()") {
                o << indent << "ArrowSchemaSetType(" << item << ", NANOARROW_TYPE_FLOAT);\n";
            } else if (elem_expr == "arrow::float64()") {
                o << indent << "ArrowSchemaSetType(" << item << ", NANOARROW_TYPE_DOUBLE);\n";
            } else if (elem_expr == "arrow::utf8()") {
                o << indent << "ArrowSchemaSetType(" << item << ", NANOARROW_TYPE_STRING);\n";
            } else if (elem_expr == "arrow::binary()") {
                o << indent << "ArrowSchemaSetType(" << item << ", NANOARROW_TYPE_BINARY);\n";
            } else {
                o << indent << "// TODO: unknown element type: " << elem_expr << "\n";
            }
            o << indent << "ArrowSchemaSetName(" << item << ", \"item\");\n";
            break;
        }

        case FieldKind::REPEATED_STRUCT:
            // list(struct(...))
            o << indent << "ArrowSchemaSetType(" << child_expr << ", NANOARROW_TYPE_LIST);\n";
            o << indent << "ArrowSchemaDeepCopy(" << fi.mapping.nested_class << "Schema().get(), "
              << child_expr << "->children[0]);\n";
            o << indent << "ArrowSchemaSetName(" << child_expr << "->children[0], \"item\");\n";
            break;

        case FieldKind::NESTED_LIST: {
            // List<List<...<Struct>>>
            // Build from outside in: list -> list -> ... -> struct
            std::string cur = child_expr;
            for (int d = 0; d < fi.mapping.list_depth; ++d) {
                o << indent << "ArrowSchemaSetType(" << cur << ", NANOARROW_TYPE_LIST);\n";
                std::string item = cur + "->children[0]";
                o << indent << "ArrowSchemaSetName(" << item << ", \"item\");\n";
                cur = item;
            }
            // Innermost: struct (deep copy overwrites the name, restore "item")
            o << indent << "ArrowSchemaDeepCopy(" << fi.mapping.nested_class << "Schema().get(), "
              << cur << ");\n";
            o << indent << "ArrowSchemaSetName(" << cur << ", \"item\");\n";
            break;
        }

        case FieldKind::MAP: {
            // map(key_type, value_type)
            o << indent << "ArrowSchemaSetType(" << child_expr << ", NANOARROW_TYPE_MAP);\n";
            // MAP creates a child "entries" struct with two children: "key" and "value".
            std::string entries = child_expr + "->children[0]";
            std::string key_child = entries + "->children[0]";
            std::string val_child = entries + "->children[1]";

            // Key type
            const auto& key_expr = fi.mapping.map_key.arrow_type_expr;
            if (key_expr == "arrow::utf8()") {
                o << indent << "ArrowSchemaSetType(" << key_child << ", NANOARROW_TYPE_STRING);\n";
            } else if (key_expr == "arrow::int32()") {
                o << indent << "ArrowSchemaSetType(" << key_child << ", NANOARROW_TYPE_INT32);\n";
            } else if (key_expr == "arrow::int64()") {
                o << indent << "ArrowSchemaSetType(" << key_child << ", NANOARROW_TYPE_INT64);\n";
            } else if (key_expr == "arrow::uint32()") {
                o << indent << "ArrowSchemaSetType(" << key_child << ", NANOARROW_TYPE_UINT32);\n";
            } else if (key_expr == "arrow::uint64()") {
                o << indent << "ArrowSchemaSetType(" << key_child << ", NANOARROW_TYPE_UINT64);\n";
            } else if (key_expr == "arrow::boolean()") {
                o << indent << "ArrowSchemaSetType(" << key_child << ", NANOARROW_TYPE_BOOL);\n";
            } else {
                o << indent << "// TODO: unknown map key type: " << key_expr << "\n";
            }

            // Value type
            if (fi.mapping.map_value_is_message) {
                o << indent << "ArrowSchemaDeepCopy(" << fi.mapping.map_value_class
                  << "Schema().get(), " << val_child << ");\n";
                o << indent << "ArrowSchemaSetName(" << val_child << ", \"value\");\n";
            } else {
                const auto& val_expr = fi.mapping.map_value.arrow_type_expr;
                if (val_expr == "arrow::utf8()") {
                    o << indent << "ArrowSchemaSetType(" << val_child
                      << ", NANOARROW_TYPE_STRING);\n";
                } else if (val_expr == "arrow::int32()") {
                    o << indent << "ArrowSchemaSetType(" << val_child
                      << ", NANOARROW_TYPE_INT32);\n";
                } else if (val_expr == "arrow::int64()") {
                    o << indent << "ArrowSchemaSetType(" << val_child
                      << ", NANOARROW_TYPE_INT64);\n";
                } else if (val_expr == "arrow::uint32()") {
                    o << indent << "ArrowSchemaSetType(" << val_child
                      << ", NANOARROW_TYPE_UINT32);\n";
                } else if (val_expr == "arrow::uint64()") {
                    o << indent << "ArrowSchemaSetType(" << val_child
                      << ", NANOARROW_TYPE_UINT64);\n";
                } else if (val_expr == "arrow::boolean()") {
                    o << indent << "ArrowSchemaSetType(" << val_child
                      << ", NANOARROW_TYPE_BOOL);\n";
                } else if (val_expr == "arrow::float32()") {
                    o << indent << "ArrowSchemaSetType(" << val_child
                      << ", NANOARROW_TYPE_FLOAT);\n";
                } else if (val_expr == "arrow::float64()") {
                    o << indent << "ArrowSchemaSetType(" << val_child
                      << ", NANOARROW_TYPE_DOUBLE);\n";
                } else if (val_expr == "arrow::binary()") {
                    o << indent << "ArrowSchemaSetType(" << val_child
                      << ", NANOARROW_TYPE_BINARY);\n";
                } else {
                    o << indent << "// TODO: unknown map value type: " << val_expr << "\n";
                }
            }
            break;
        }
    }  // switch
}

// GIR-5: the C++ <Class>Schema() source is now emitted by the ONE IR-driven
// schema visitor (cpp_backend::GenerateSchemaFunctionFromIr), the same visitor
// that BuildMessageSchemaInto executes in-process — the two paths cannot drift
// (locked decision #5). `fields` is retained for signature compatibility with
// the generation loop but is no longer read here (the visitor rebuilds the
// flattened field list from the IR). The emitted source may differ cosmetically
// from the pre-GIR-5 output (nanoarrow-call ordering for nested lists, dropped
// per-field warning comments), but the runtime schema — and therefore the .ipc
// bytes — is byte-identical.
std::string GenerateSchemaFunction(const std::string& cls, const std::vector<FieldInfo>& fields,
                                   const google::protobuf::Descriptor* msg) {
    (void)fields;
    return cpp_backend::GenerateSchemaFunctionFromIr(cls, msg, msg->file());
}

// -----------------------------------------------------------------------
// In-process schema construction (--fletcher_opt=ipc)
//
// Executes the same nanoarrow calls that GenerateSchemaFunction /
// EmitNanoarrowTypeSetup emit as C++ source, so the schema built here is
// identical to the one the generated <Class>Schema() builds at runtime.
// Any change to the emitted schema code must be mirrored here.
// -----------------------------------------------------------------------

void CheckNa(ArrowErrorCode code, const char* context) {
    if (code != NANOARROW_OK) {
        throw std::runtime_error(std::string("BuildMessageSchema: ") + context + " failed");
    }
}

// Counterpart of the scalar branches in EmitNanoarrowTypeSetup.
void SetScalarSchemaType(ArrowSchema* schema, const std::string& expr) {
    if (expr.find("timestamp") != std::string::npos) {
        CheckNa(ArrowSchemaSetTypeDateTime(schema, NANOARROW_TYPE_TIMESTAMP,
                                           NANOARROW_TIME_UNIT_NANO, nullptr),
                "set timestamp type");
        return;
    }
    if (expr.find("duration") != std::string::npos) {
        CheckNa(ArrowSchemaSetTypeDateTime(schema, NANOARROW_TYPE_DURATION,
                                           NANOARROW_TIME_UNIT_NANO, nullptr),
                "set duration type");
        return;
    }

    ArrowType type;
    if (expr == "arrow::boolean()") {
        type = NANOARROW_TYPE_BOOL;
    } else if (expr == "arrow::int32()") {
        type = NANOARROW_TYPE_INT32;
    } else if (expr == "arrow::int64()") {
        type = NANOARROW_TYPE_INT64;
    } else if (expr == "arrow::uint32()") {
        type = NANOARROW_TYPE_UINT32;
    } else if (expr == "arrow::uint64()") {
        type = NANOARROW_TYPE_UINT64;
    } else if (expr == "arrow::float32()") {
        type = NANOARROW_TYPE_FLOAT;
    } else if (expr == "arrow::float64()") {
        type = NANOARROW_TYPE_DOUBLE;
    } else if (expr == "arrow::utf8()") {
        type = NANOARROW_TYPE_STRING;
    } else if (expr == "arrow::binary()") {
        type = NANOARROW_TYPE_BINARY;
    } else {
        throw std::runtime_error("BuildMessageSchema: unsupported scalar type " + expr);
    }
    CheckNa(ArrowSchemaSetType(schema, type), "set scalar type");
}

void SetMetadataPairs(ArrowSchema* schema,
                      const std::vector<std::pair<std::string, std::string>>& pairs) {
    ArrowBuffer buf;
    ArrowBufferInit(&buf);
    // Collect the first failure but always reach ArrowBufferReset, then throw.
    ArrowErrorCode code = ArrowMetadataBuilderInit(&buf, nullptr);
    for (const auto& [key, value] : pairs) {
        if (code != NANOARROW_OK) break;
        code = ArrowMetadataBuilderAppend(&buf, ArrowCharView(key.c_str()),
                                          ArrowCharView(value.c_str()));
    }
    if (code == NANOARROW_OK) {
        code = ArrowSchemaSetMetadata(schema, reinterpret_cast<const char*>(buf.data));
    }
    ArrowBufferReset(&buf);
    CheckNa(code, "set metadata");
}

const google::protobuf::Descriptor* RequireNestedMsg(const google::protobuf::Descriptor* nested,
                                                     const std::string& field_name) {
    if (!nested) {
        throw std::runtime_error("BuildMessageSchema: missing nested descriptor for field '" +
                                 field_name + "'");
    }
    return nested;
}

// GIR-5: the in-process ArrowSchema is now built by the SAME IR-driven schema
// visitor that emits the generated <Class>Schema() source
// (cpp_backend::BuildMessageSchemaIntoFromIr) — one visitor, both paths (locked
// decision #5). `schema` must be uninitialized (or released) on entry.
void BuildMessageSchemaInto(const google::protobuf::Descriptor* msg, ArrowSchema* schema) {
    cpp_backend::BuildMessageSchemaIntoFromIr(msg, schema);
}

// -----------------------------------------------------------------------
// View getter generation
//
// GIR-6: the generated `<Class>View` getters are now emitted by the ONE
// IR-driven view visitor (cpp_backend::EmitViewGetterFromIr,
// cpp_backend_view_visitor.cpp), walking the same language-neutral IR the encode
// / decode / schema visitors consume. The former FieldMapping-switch here (plus
// the ArrayTypeFromScalar / GetterType view helpers, now folded into the C++
// backend table as CppScalarInfo::array_type / getter_type) was retired; the
// emitted view getters are byte-identical (guarded by the coverage round-trip
// oracle). `si`/context come from the FieldInfo (position + owning proto file).
// -----------------------------------------------------------------------

void EmitViewGetters(std::ostringstream& o, const std::vector<FieldInfo>& fields) {
    for (size_t idx = 0; idx < fields.size(); ++idx)
        cpp_backend::EmitViewGetterFromIr(o, *fields[idx].ir, fields[idx].name, idx,
                                          fields[idx].descriptor->file());
}

// -----------------------------------------------------------------------
// Full immutable view class generation for one message
// -----------------------------------------------------------------------

std::string GenerateViewClass(const std::string& view_cls, const std::vector<FieldInfo>& fields) {
    std::ostringstream o;

    o << "/// Immutable, typed view over a row of Arrow scalars.\n"
      << "///\n"
      << "/// Wraps an ArrowRow, a StructScalar, a RecordBatch row, or a Table row\n"
      << "/// and exposes each field through a zero-copy typed getter.\n"
      << "class " << view_cls << " {\n public:\n";

    // Constructor from ArrowRow (vector of scalars)
    o << "    /// Wraps a pre-existing vector of Arrow scalars (e.g. from ToArrowRow()\n"
      << "    /// or Codec::DecodeRow).\n"
      << "    explicit " << view_cls << "(fletcher::ArrowRow scalars)\n"
      << "        : scalars_(std::move(scalars)) {}\n\n";

    // Constructor from shared_ptr<Scalar> (for nested struct views)
    o << "    /// Wraps a StructScalar — used when this message is a nested struct\n"
      << "    /// field inside a parent view.\n"
      << "    explicit " << view_cls << "(std::shared_ptr<arrow::Scalar> scalar)\n"
      << "        : scalars_(static_cast<const arrow::StructScalar&>"
         "(*scalar).value) {}\n\n";

    // Constructor from RecordBatch + row index
    o << "    /// Extracts one row from a RecordBatch by index, materialising each\n"
      << "    /// column value as a scalar.\n"
      << "    " << view_cls << "(const arrow::RecordBatch& batch, int64_t row) {\n"
      << "        scalars_.reserve(batch.num_columns());\n"
      << "        for (int i = 0; i < batch.num_columns(); ++i)\n"
      << "            scalars_.push_back(detail::FletcherValueOrThrow(\n"
      << "                batch.column(i)->GetScalar(row), \"RecordBatch::GetScalar\"));\n"
      << "    }\n\n";

    // Constructor from Table + row index
    o << "    /// Extracts one row from a Table by index, resolving the correct chunk\n"
      << "    /// in each ChunkedArray.\n"
      << "    " << view_cls << "(const arrow::Table& table, int64_t row) {\n"
      << "        scalars_.reserve(table.num_columns());\n"
      << "        for (int i = 0; i < table.num_columns(); ++i) {\n"
      << "            const auto& chunked = *table.column(i);\n"
      << "            int64_t offset = row;\n"
      << "            for (const auto& chunk : chunked.chunks()) {\n"
      << "                if (offset < chunk->length()) {\n"
      << "                    scalars_.push_back(detail::FletcherValueOrThrow(\n"
      << "                        chunk->GetScalar(offset), \"Array::GetScalar\"));\n"
      << "                    break;\n"
      << "                }\n"
      << "                offset -= chunk->length();\n"
      << "            }\n"
      << "        }\n"
      << "    }\n\n";

    // Getters
    EmitViewGetters(o, fields);

    // Private
    o << "\n private:\n"
      << "    fletcher::ArrowRow scalars_;\n";

    o << "};\n";
    return o.str();
}

// -----------------------------------------------------------------------
// EncodeTo / EncodeStructTo_ — direct encoding to WriteBuffer
// -----------------------------------------------------------------------

// NOTE: the edge ENCODE and DECODE emitters are now IR-driven recursive
// visitors — cpp_backend::EmitFieldEncodeFromIr (cpp_backend_type_table.cpp)
// and cpp_backend::EmitFieldDecodeFromIr (cpp_backend_decode_visitor.cpp). The
// old FieldMapping-switch EmitFieldEncode/EmitScalarWrite/PositionalWriteCall
// (GIR-3) and EmitFieldDecode/PositionalReadCall (GIR-4) were retired; the
// PositionalReader method-name derivation now lives in the C++ backend table
// (CppScalarInfo::positional_read).

// Emit EncodeTo, EncodeStructTo_, and Encode methods for a message class.
void EmitEncodeTo(std::ostringstream& o, const std::string& cls,
                  const std::vector<FieldInfo>& fields) {
    std::string fc = std::to_string(fields.size());

    // EncodeStructTo_ — positional format, writes fields into a parent-provided writer.
    o << "    /// Internal: writes this message's fields into a parent writer\n"
      << "    /// when the message is nested as a struct field inside another row.\n";
    o << "    void EncodeStructTo_(fletcher::PositionalWriter& w) const {\n";
    for (size_t i = 0; i < fields.size(); ++i)
        cpp_backend::EmitFieldEncodeFromIr(o, *fields[i].ir, fields[i].name + "_", i,
                                           fields[i].descriptor->file());
    o << "    }\n\n";

    // EncodeTo — creates a PositionalWriter and writes fields positionally.
    o << "    /// Serialises the row into the given buffer in the positional wire\n"
      << "    /// format. Used by Publisher to write directly into the provider's\n"
      << "    /// transport buffer without an intermediate copy.\n";
    o << "    void EncodeTo(fletcher::WriteBuffer& buf) const {\n"
      << "        fletcher::PositionalWriter w(buf, " << fc << ");\n";
    for (size_t i = 0; i < fields.size(); ++i)
        cpp_backend::EmitFieldEncodeFromIr(o, *fields[i].ir, fields[i].name + "_", i,
                                           fields[i].descriptor->file());
    o << "    }\n\n";

    // Encode() — convenience returning EncodedRow.
    o << "    /// Convenience method that returns a self-contained EncodedRow.\n"
      << "    /// Useful for testing, WAL storage, or any context where you need\n"
      << "    /// the encoded bytes as a standalone value.\n";
    o << "    fletcher::EncodedRow Encode() const {\n"
      << "        fletcher::EncodedRow row;\n"
      << "        fletcher::VectorWriteBuffer buf(row);\n"
      << "        EncodeTo(buf);\n"
      << "        return row;\n"
      << "    }\n\n";
}

// -----------------------------------------------------------------------
// Full class generation for one message
// -----------------------------------------------------------------------

std::string GenerateMessageClass(const std::string& cls, const std::vector<FieldInfo>& fields,
                                 const google::protobuf::Descriptor* msg) {
    std::ostringstream o;
    std::string fc = std::to_string(fields.size());

    // ---- class header ---------------------------------------------------
    o << "class " << cls << " {\n public:\n";

    // GIR-9 (#75): nested enums are emitted once, inside their owning class's
    // public section, before any accessor that may name them. A nested enum
    // cannot be forward-declared apart from its owner, so this is its only
    // declaration site.
    for (int i = 0; i < msg->enum_type_count(); ++i) {
        EmitEnumClass(o, msg->enum_type(i), "    ");
        o << "\n";
    }

    // Default constructor
    o << "    /// Constructs an empty row. Use the setters to populate fields\n"
      << "    /// before calling Encode() to produce the wire-format buffer.\n";
    o << "    " << cls << "() = default;\n\n";

    // Constructor from raw bytes
    o << "    /// Reconstructs a row from a raw wire-format buffer, e.g. one\n"
      << "    /// received from a Subscriber callback or read from a WAL.\n";
    o << "    explicit " << cls << "(const uint8_t* data, size_t len) {\n"
      << "        fletcher::PositionalReader r(data, len, " << fc << ");\n";
    for (size_t i = 0; i < fields.size(); ++i)
        cpp_backend::EmitFieldDecodeFromIr(o, *fields[i].ir, fields[i].name + "_", i,
                                           fields[i].descriptor->file());
    o << "    }\n\n";

    // Constructor from EncodedRow
    o << "    /// Convenience overload that accepts an EncodedRow directly,\n"
      << "    /// so callers do not need to extract the pointer and length.\n";
    o << "    explicit " << cls << "(const fletcher::EncodedRow& row)\n"
      << "        : " << cls << "(row.data(), row.size()) {}\n\n";

    // Constructor from PositionalReader& (for nested struct decoding)
    o << "    /// Used internally when this message is embedded as a struct\n"
      << "    /// field inside another message — the parent reader is passed\n"
      << "    /// through so nested fields are decoded in position.\n";
    o << "    explicit " << cls << "(fletcher::PositionalReader& r) {\n";
    for (size_t i = 0; i < fields.size(); ++i)
        cpp_backend::EmitFieldDecodeFromIr(o, *fields[i].ir, fields[i].name + "_", i,
                                           fields[i].descriptor->file());
    o << "    }\n\n";

    // Setters
    EmitSetters(o, cls, fields);
    o << "\n";

    // Getters
    EmitGetters(o, fields);
    o << "\n";

    // EncodeTo / EncodeStructTo_ / Encode — positional format encoding
    EmitEncodeTo(o, cls, fields);

    // ---- private section ------------------------------------------------
    o << " private:\n";

    // Storage members
    for (const auto& fi : fields) o << "    " << StorageDecl(fi) << ";\n";

    o << "};\n";
    return o.str();
}

// -----------------------------------------------------------------------
// Per-class helpers shared by publisher and subscriber generation
// -----------------------------------------------------------------------

void EmitTopicSegments(std::ostringstream& o, const std::string& package,
                       const std::string& svc_name, const std::string& method_name) {
    o << "    /// Returns the hierarchical topic path derived from the proto\n"
      << "    /// service and method names. Used by the Publisher/Subscriber to\n"
      << "    /// register, look up, and route messages on the pub/sub layer.\n";
    o << "    static const std::vector<std::string>& TopicSegments() {\n"
      << "        static const std::vector<std::string> kSegments = {";
    if (!package.empty()) o << "\n            \"" << package << "\",";
    o << "\n            \"" << svc_name << "\"," << "\n            \"" << method_name << "\"\n"
      << "        };\n"
      << "        return kSegments;\n"
      << "    }\n\n";
}

void EmitSchema(std::ostringstream& o, const std::string& msg_class) {
    o << "    /// Exposes the message schema on the publisher/subscriber class so\n"
      << "    /// callers can inspect the wire layout without constructing a row.\n";
    o << "    static fletcher::OwnedSchema Schema() {\n"
      << "        return " << msg_class << "Schema();\n"
      << "    }\n\n";
}

// -----------------------------------------------------------------------
// Publisher class generation for a single service method
// -----------------------------------------------------------------------

std::string GeneratePublisherClass(const google::protobuf::MethodDescriptor* method,
                                   const std::string& package) {
    const std::string svc_name = method->service()->name();
    const std::string method_name = method->name();
    const std::string cls = svc_name + "_" + method_name + "Publisher";
    const std::string msg_class = ClassName(method->input_type());

    std::ostringstream o;
    o << "class " << cls << " {\n public:\n";

    EmitTopicSegments(o, package, svc_name, method_name);
    EmitSchema(o, msg_class);

    // Topic key (segments joined with '/')
    o << "    /// Returns the flat string form of the topic path (segments joined\n"
      << "    /// with '/'). Useful for logging, diagnostics, and WebGateway URLs\n"
      << "    /// where a single string is expected instead of path segments.\n";
    o << "    static const std::string& TopicKey() {\n"
      << "        static const std::string kKey = \"";
    if (!package.empty()) o << package << "/";
    o << svc_name << "/" << method_name << "\";\n"
      << "        return kKey;\n"
      << "    }\n\n";

    // Constructor
    o << "    /// Creates the publisher and registers the topic with its schema\n"
      << "    /// on the provider. After construction, subscribers can discover\n"
      << "    /// the topic and receive the schema for decoding.\n";
    o << "    explicit " << cls << "(\n"
      << "            std::shared_ptr<fletcher::PubSubProvider> provider)\n"
      << "        : publisher_(std::make_unique<fletcher::Publisher>(std::move(provider)))\n"
      << "    {\n"
      << "        publisher_->CreateTopic(TopicSegments(), " << msg_class << "Schema());\n"
      << "    }\n\n";

    // Publish (without attachments)
    o << "    /// Encodes and publishes a single row. The encoding happens\n"
      << "    /// directly into the provider's transport buffer to avoid copies.\n";
    o << "    void Publish(const " << msg_class << "& row) {\n"
      << "        publisher_->Publish(TopicSegments(),\n"
      << "            [&](fletcher::WriteBuffer& buf) { row.EncodeTo(buf); });\n"
      << "    }\n\n";

    // Publish (with attachments)
    o << "    /// Publishes a row together with keyed binary attachments (e.g.\n"
      << "    /// images, point clouds) that travel alongside the row as part\n"
      << "    /// of the same Envelope.\n";
    o << "    void Publish(const " << msg_class << "& row,\n"
      << "                 fletcher::Attachments attachments) {\n"
      << "        publisher_->Publish(TopicSegments(),\n"
      << "            [&](fletcher::WriteBuffer& buf) { row.EncodeTo(buf); },\n"
      << "            std::move(attachments));\n"
      << "    }\n\n";

    // Private
    o << " private:\n"
      << "    std::unique_ptr<fletcher::Publisher> publisher_;\n"
      << "};\n";

    return o.str();
}

// -----------------------------------------------------------------------
// Subscriber class generation for a single service method
// -----------------------------------------------------------------------

std::string GenerateSubscriberClass(const google::protobuf::MethodDescriptor* method,
                                    const std::string& package) {
    const std::string svc_name = method->service()->name();
    const std::string method_name = method->name();
    const std::string cls = svc_name + "_" + method_name + "Subscriber";
    const std::string msg_class = ClassName(method->input_type());

    std::ostringstream o;
    o << "class " << cls << " {\n public:\n";

    EmitTopicSegments(o, package, svc_name, method_name);
    EmitSchema(o, msg_class);

    // Constructor — subscriber does not call CreateTopic; the schema
    // is discovered from the provider when Subscribe() is called.
    o << "    /// Binds to the provider without creating a topic — subscribers\n"
      << "    /// discover the topic and its schema when Subscribe() is called.\n";
    o << "    explicit " << cls << "(\n"
      << "            std::shared_ptr<fletcher::PubSubProvider> provider)\n"
      << "        : subscriber_(std::make_unique<fletcher::Subscriber>(std::move(provider))) "
         "{}\n\n";

    // Subscribe — delivers decoded message + Attachments to the caller.
    // Returns the schema received from the publisher via the provider.
    o << "    /// Begins receiving rows on this topic. The raw wire-format bytes\n"
      << "    /// are decoded into a typed message before being delivered to the\n"
      << "    /// callback, so subscribers never handle raw buffers directly.\n"
      << "    /// Returns the subscription ID (used for Unsubscribe).\n";
    o << "    uint64_t Subscribe(\n"
      << "        std::function<void(" << msg_class << ", fletcher::Attachments)> cb)\n"
      << "    {\n"
      << "        auto result = subscriber_->Subscribe(TopicSegments(),\n"
      << "            [cb = std::move(cb)](uint64_t /*subscription_id*/,\n"
      << "                                 const uint8_t* data, size_t len,\n"
      << "                                 fletcher::SharedSchema /*schema*/,\n"
      << "                                 fletcher::Attachments att) {\n"
      << "                cb(" << msg_class << "(data, len), std::move(att));\n"
      << "            });\n"
      << "        return result.subscription_id;\n"
      << "    }\n\n";

    // Unsubscribe
    o << "    /// Stops delivery and releases the subscription on the provider.\n";
    o << "    void Unsubscribe(uint64_t subscription_id) {\n"
      << "        subscriber_->Unsubscribe(subscription_id);\n"
      << "    }\n\n";

    // Private
    o << " private:\n"
      << "    std::unique_ptr<fletcher::Subscriber> subscriber_;\n"
      << "};\n";

    return o.str();
}

// -----------------------------------------------------------------------
// File generation
// -----------------------------------------------------------------------

std::string GenerateFile(const google::protobuf::FileDescriptor* file, bool schema_only) {
    std::ostringstream o;

    o << "// Generated by fletcher-protoc. DO NOT EDIT.\n"
      << "// Source: " << file->name() << "\n"
      << "#pragma once\n\n"
      << "#include <nanoarrow/nanoarrow.h>\n"
      << "#include <fletcher/pubsub/owned_schema.hpp>\n"
      << "#include <fletcher/core/types.hpp>\n";

    if (!schema_only) {
        o << "#include <fletcher/core/positional_io.hpp>\n"
          << "#include <fletcher/core/write_buffer.hpp>\n";
    }

    o << "\n"
      << "#include <cstdint>\n"
      << "#include <memory>\n"
      << "#include <vector>\n";

    if (!schema_only) {
        o << "#include <optional>\n"
          << "#include <string>\n"
          << "#include <string_view>\n"
          << "#include <utility>\n";
    }

    if (!schema_only && file->service_count() > 0) {
        o << "#include <fletcher/pubsub/provider.hpp>\n"
          << "#include <fletcher/pubsub/publisher.hpp>\n"
          << "#include <fletcher/pubsub/subscriber.hpp>\n"
          << "#include <functional>\n";
    }

    // TODO: CRS utilities — will be restored in a later phase.

    // Cross-file generated headers (for referenced messages from other .proto
    // files) plus GIR-9 imported-enum headers (an imported enum's typed C++
    // accessors need its owning file's generated header visible).
    auto cross_includes = CollectCrossFileIncludes(file);
    for (int mi = 0; mi < file->message_type_count(); ++mi)
        CollectCrossFileEnumIncludesFromMessage(file->message_type(mi), file, cross_includes);
    if (!cross_includes.empty()) {
        o << "\n";
        for (const auto& h : cross_includes) o << "#include \"" << h << "\"\n";
    }
    o << "\n";

    o << "namespace fletcher_gen {\n";
    const std::string ns = DotToColons(file->package());
    if (!ns.empty()) o << "namespace " << ns << " {\n";
    o << "\n";

    // GIR-9 (#75): emit every file-level enum (referenced AND standalone) as a
    // scoped `enum class : int32_t` at package-namespace scope, before the
    // message classes that may name them.
    for (int i = 0; i < file->enum_type_count(); ++i) {
        EmitEnumClass(o, file->enum_type(i), "");
        o << "\n";
    }

    // Emit messages in dependency order.
    auto messages = OrderedMessages(file);
    std::set<const google::protobuf::Descriptor*> generated_msgs;

    for (const auto* msg : messages) {
        if (IsRecursive(msg)) {
            o << "// Skipped: " << msg->name()
              << " is recursive and cannot be represented in Arrow.\n\n";
            continue;
        }

        // Flattened wrapper messages are absorbed into the parent's schema
        // by the type mapper — skip class generation.
        if (IsFlattenedWrapper(msg)) continue;

        std::string skipped;
        auto fields = GatherFields(msg, &skipped);
        const std::string cls = ClassName(msg);

        generated_msgs.insert(msg);

        if (!skipped.empty()) {
            o << "// Note: the following fields of " << msg->name()
              << " have no Arrow mapping and are absent from the schema:\n"
              << skipped << "//\n";
        }

        // Always emit the free schema function.
        o << GenerateSchemaFunction(cls, fields, msg) << "\n";

        // Optionally emit the row class.
        // View class omitted — generated separately in .fletcher.arrow.pb.h.
        if (!schema_only) {
            o << GenerateMessageClass(cls, fields, msg) << "\n";
        }
    }

    // Service definitions → publisher and subscriber classes (skip in schema_only mode).
    if (!schema_only) {
        for (int si = 0; si < file->service_count(); ++si) {
            const auto* svc = file->service(si);
            for (int mi = 0; mi < svc->method_count(); ++mi) {
                const auto* method = svc->method(mi);
                std::string reason;
                if (!ValidateServiceMethod(method, generated_msgs, &reason)) {
                    o << "// Skipped: " << svc->name() << "." << method->name() << " — " << reason
                      << "\n";
                    continue;
                }
                o << GeneratePublisherClass(method, file->package()) << "\n"
                  << GenerateSubscriberClass(method, file->package()) << "\n";
            }
        }
    }

    if (!ns.empty()) o << "}  // namespace " << ns << "\n";
    o << "}  // namespace fletcher_gen\n";

    return o.str();
}

// -----------------------------------------------------------------------
// TypeScript file generation
// -----------------------------------------------------------------------

std::string TsOutputFilename(const std::string& proto_name) {
    return StripProtoSuffix(proto_name) + ".fletcher.ts";
}

// Generate the full .fletcher.ts file for a .proto file. GIR-7: the interface +
// runtime TypedSchema/SchemaDescriptor generation is now a direct recursive IR
// visitor (ts_backend::TsVisitor); all TypeScript type text and WireTypeId member
// names live in ts_backend, never on an IR node (locked decision #1). Output is
// byte-identical to the pre-migration emitter (TsVisitor.DescriptorByteIdentical).
std::string GenerateTypeScriptFile(const google::protobuf::FileDescriptor* file) {
    return ts_backend::TsVisitor(file).GenerateFile();
}

// -----------------------------------------------------------------------
// ToArrowRow() free function generation
//
// Generates an inline free function per message that converts a nanoarrow
// class instance to fletcher::ArrowRow (vector<shared_ptr<arrow::Scalar>>)
// using only the public getter API.  Found via ADL.
// -----------------------------------------------------------------------

std::string GenerateToArrowRow(const std::string& cls, const std::vector<FieldInfo>& fields) {
    std::ostringstream o;
    o << "/// Converts a nanoarrow-only " << cls << " instance to an ArrowRow\n"
      << "/// (vector of Arrow scalars).  Uses only public getters, so the\n"
      << "/// nanoarrow class remains free of any Arrow C++ dependency.\n"
      << "/// Found via ADL — call as ToArrowRow(msg) without qualification.\n"
      << "inline fletcher::ArrowRow ToArrowRow(const " << cls << "& msg) {\n"
      << "    fletcher::ArrowRow row;\n"
      << "    row.reserve(" << fields.size() << ");\n";

    // GIR-6: ToArrowRow() field emission is now driven by the ONE IR-driven view
    // visitor (cpp_backend::EmitToArrowRowFieldFromIr, cpp_backend_view_visitor.cpp),
    // the same visitor that emits the `<Class>View` getters. It reads each field
    // through the public getter "msg.<name>()" (which already applies
    // value_or(default) for non-nullable scalars) and branches only on nullable vs
    // non-nullable — no double-default. The former FieldMapping-switch here was
    // retired; the emitted ToArrowRow() body is byte-identical (guarded by the
    // coverage round-trip oracle).
    for (size_t i = 0; i < fields.size(); ++i)
        cpp_backend::EmitToArrowRowFieldFromIr(o, *fields[i].ir, "msg." + fields[i].name + "()", i,
                                               fields[i].descriptor->file());

    o << "    return row;\n"
      << "}\n";
    return o.str();
}

// -----------------------------------------------------------------------
// Arrow file generation (.fletcher.arrow.pb.h)
//
// Generates an Arrow C++ dependent header for each proto file.
// Contains view classes (typed wrappers around ArrowRow / RecordBatch / Table)
// and ToArrowRow() free functions that convert nanoarrow classes to ArrowRow.
// -----------------------------------------------------------------------

std::string ViewOutputFilename(const std::string& proto_name) {
    return StripProtoSuffix(proto_name) + ".fletcher.arrow.pb.h";
}

std::string GenerateViewFile(const google::protobuf::FileDescriptor* file) {
    auto messages = OrderedMessages(file);

    // Check if any message can produce a view class.
    bool has_views = false;
    for (const auto* msg : messages) {
        if (IsRecursive(msg) || IsFlattenedWrapper(msg)) continue;
        has_views = true;
        break;
    }
    if (!has_views) return {};

    std::ostringstream o;

    o << "// Generated by fletcher-protoc. DO NOT EDIT.\n"
      << "// Source: " << file->name() << "\n"
      << "// Arrow C++ view classes and ToArrowRow() converters — server-side only.\n"
      << "#pragma once\n\n"
      << "#include <arrow/api.h>\n"
      << "#include <arrow/c/bridge.h>\n"
      << "#include <fletcher/arrow_bridge/arrow_row_view.hpp>\n"
      << "#include <fletcher/arrow_bridge/codec.hpp>\n\n"
      << "#include \"" << OutputFilename(file->name()) << "\"\n";

    // Cross-file view includes (for nested view types from other proto files).
    const auto cross_includes = CollectCrossFileIncludes(file);
    if (!cross_includes.empty()) {
        o << "\n";
        for (const auto& h : cross_includes) {
            // Replace .fletcher.pb.h → .fletcher.arrow.pb.h
            std::string vh = h;
            const std::string suffix = ".fletcher.pb.h";
            if (vh.size() > suffix.size() && vh.substr(vh.size() - suffix.size()) == suffix) {
                vh = vh.substr(0, vh.size() - suffix.size()) + ".fletcher.arrow.pb.h";
            }
            o << "#include \"" << vh << "\"\n";
        }
    }

    o << "\n"
      << "#include <cstdint>\n"
      << "#include <memory>\n"
      << "#include <optional>\n"
      << "#include <stdexcept>\n"
      << "#include <string>\n"
      << "#include <string_view>\n"
      << "#include <utility>\n"
      << "#include <vector>\n\n";

    o << "namespace fletcher_gen {\n";
    const std::string ns = DotToColons(file->package());
    if (!ns.empty()) o << "namespace " << ns << " {\n";
    o << "\n";

    // Helper: convert OwnedSchema → shared_ptr<arrow::Schema> via C Data Interface.
    // Guard against redefinition when multiple view headers from the same package
    // are included in a single translation unit.
    {
        std::string guard = "FLETCHER_DETAIL_IMPORT_SCHEMA_";
        for (char c : file->package()) guard += (c == '.' ? '_' : std::toupper(c));
        guard += "_DEFINED";
        o << "#ifndef " << guard << "\n"
          << "#define " << guard << "\n"
          << "namespace detail {\n"
          << "/// Converts a nanoarrow OwnedSchema to a shared_ptr<arrow::Schema>\n"
          << "/// via the Arrow C Data Interface.  Used internally by view classes\n"
          << "/// and ToArrowRow() to obtain Arrow type metadata.\n"
          << "inline std::shared_ptr<arrow::Schema> ImportSchema(fletcher::OwnedSchema nano) {\n"
          << "    auto result = arrow::ImportSchema(nano.get());\n"
          << "    return result.ok() ? *result : nullptr;\n"
          << "}\n"
          << "}  // namespace detail\n"
          << "#endif\n\n";
    }

    // GIR-8 (#53): checked Arrow Result<T> unwrap helper. Replaces the raw
    // unchecked-unwrap calls in generated view code: value-identical on ok()
    // (returns ValueUnsafe()), but throws a descriptive std::runtime_error
    // carrying the call-site context and the failing status instead of aborting.
    // Guarded like ImportSchema above so including several same-package view
    // headers in one translation unit does not redefine it.
    {
        std::string guard = "FLETCHER_DETAIL_VALUE_OR_THROW_";
        for (char c : file->package()) guard += (c == '.' ? '_' : std::toupper(c));
        guard += "_DEFINED";
        o << "#ifndef " << guard << "\n"
          << "#define " << guard << "\n"
          << "namespace detail {\n"
          << "/// Returns the value of an arrow::Result on success, or throws a\n"
          << "/// std::runtime_error carrying `context` and the failing status on\n"
          << "/// error. Used by generated view code to unwrap Arrow results so a\n"
          << "/// failed Arrow call surfaces a descriptive exception rather than\n"
          << "/// aborting the process.\n"
          << "template <typename T>\n"
          << "T FletcherValueOrThrow(arrow::Result<T>&& result, const char* context) {\n"
          << "    if (!result.ok()) {\n"
          << "        throw std::runtime_error(\n"
          << "            std::string(context) + \": \" + result.status().ToString());\n"
          << "    }\n"
          << "    return std::move(result).ValueUnsafe();\n"
          << "}\n"
          << "}  // namespace detail\n"
          << "#endif\n\n";
    }

    for (const auto* msg : messages) {
        if (IsRecursive(msg) || IsFlattenedWrapper(msg)) continue;

        std::string skipped;
        auto fields = GatherFields(msg, &skipped);
        const std::string cls = ClassName(msg);
        const std::string view_cls = cls + "View";

        o << GenerateViewClass(view_cls, fields) << "\n";
        o << GenerateToArrowRow(cls, fields) << "\n";
    }

    if (!ns.empty()) o << "}  // namespace " << ns << "\n";
    o << "}  // namespace fletcher_gen\n";

    return o.str();
}

// -----------------------------------------------------------------------
// Generation-front validation: fail fatally on genuinely-unsupported types
// (GIR-8, #55)
//
// Today GatherFieldsImpl() silently drops fields whose IR is
// NodeKind::UNSUPPORTED (a comment in the header is the only trace). GIR-8 turns
// those into a fatal protoc error BEFORE any artifact is written, but ONLY for
// messages that would actually be generated: recursive messages and flattened
// wrappers are skipped here with the SAME predicate the emit loops use
// (GenerateFile / GenerateViewFile / IPC: IsRecursive(msg) ||
// IsFlattenedWrapper(msg)), so recursion stays skipped/non-fatal exactly as
// today. Only genuinely-unsupported TYPES reached inside a generated message —
// google.protobuf.Any / Struct, real oneof, unsupported map key/value, and
// unsupported flatten-wrapper leaves — are fatal. Proto2 groups map to INT32
// via BuildFieldIr and never produce UNSUPPORTED.
// -----------------------------------------------------------------------

// Recursively locate the first NodeKind::UNSUPPORTED leaf reachable from `node`,
// returning a human-readable "unsupported field '<name>': <reason>" message.
// Exhaustive over every child-bearing IR node kind (LIST / FIXED_SIZE_LIST / MAP
// key+value / STRUCT fields); SCALAR carries no IR children. FIXED_SIZE_LIST is
// defensive/future-proofing — BuildFieldIr does not currently emit it — but any
// future child-bearing node kind MUST be added here.
std::optional<std::string> FindUnsupportedIr(const ir::IrNode& node) {
    if (node.kind == ir::NodeKind::UNSUPPORTED) {
        const auto& u = std::get<ir::UnsupportedNode>(node.node);
        return "unsupported field '" + node.facts.proto_full_name + "': " + u.reason;
    }

    if (node.kind == ir::NodeKind::LIST) {
        return FindUnsupportedIr(*std::get<ir::ListNode>(node.node).element);
    }

    if (node.kind == ir::NodeKind::FIXED_SIZE_LIST) {
        return FindUnsupportedIr(*std::get<ir::FixedSizeListNode>(node.node).element);
    }

    if (node.kind == ir::NodeKind::MAP) {
        const auto& m = std::get<ir::MapNode>(node.node);
        if (auto e = FindUnsupportedIr(*m.key)) return e;
        return FindUnsupportedIr(*m.value);
    }

    if (node.kind == ir::NodeKind::STRUCT) {
        for (const auto& f : std::get<ir::StructNode>(node.node).fields) {
            if (auto e = FindUnsupportedIr(*f.type)) return e;
        }
    }

    return std::nullopt;
}

bool ValidateNoUnsupportedIr(const google::protobuf::FileDescriptor* file, std::string* error) {
    for (const auto* msg : OrderedMessages(file)) {
        // Mirror the skip predicate from GenerateFile, GenerateViewFile, and the
        // IPC emission loop: skip recursive messages and flattened wrappers so
        // validation fires ONLY on messages that would actually be generated.
        // (OrderedMessages already excludes recursive messages; the IsRecursive
        // term is a harmless defensive mirror. IsFlattenedWrapper IS load-bearing
        // — wrappers appear in OrderedMessages and are skipped at emit.)
        if (IsRecursive(msg) || IsFlattenedWrapper(msg)) continue;

        // BuildFieldIr(field) is used directly rather than replicating
        // GatherFieldsImpl's field-level-flatten inlining: for a field-flattened
        // field BuildFieldIr yields a STRUCT that FindUnsupportedIr recurses
        // into, so the same descendant fields are still checked — equivalent
        // detection.
        for (int i = 0; i < msg->field_count(); ++i) {
            auto node = ir::BuildFieldIr(msg->field(i));
            if (auto e = FindUnsupportedIr(node)) {
                *error = *e;
                return false;
            }
        }
    }
    return true;
}

}  // namespace

// ValidateServiceMethod (declared in generator_internal.hpp, namespace fletcher)
// has external linkage so the TS backend (ts_backend::TsVisitor) shares the exact
// same pub/sub topic-eligibility rules + skip-reason text. Relocated out of the
// anonymous namespace following the RBA-2 OrderedMessages pattern: a pure linkage
// change with no behavioural effect (guarded byte-for-byte by the coverage
// goldens and the RBA no-drift test).
bool ValidateServiceMethod(const google::protobuf::MethodDescriptor* method,
                           const std::set<const google::protobuf::Descriptor*>& generated_msgs,
                           std::string* reason) {
    if (!method->client_streaming()) {
        *reason = "request is not streaming (pub/sub requires 'stream' on request)";
        return false;
    }
    if (method->server_streaming()) {
        *reason = "server-streaming is not supported for pub/sub (no replies)";
        return false;
    }
    if (method->output_type()->full_name() != "google.protobuf.Empty") {
        *reason = "return type must be google.protobuf.Empty for pub/sub";
        return false;
    }
    if (!generated_msgs.count(method->input_type())) {
        *reason = "input message '" + method->input_type()->name() +
                  "' has no generated Arrow mapping in this file";
        return false;
    }
    return true;
}

// -----------------------------------------------------------------------
// Public schema builder (declared in schema_builder.hpp)
// -----------------------------------------------------------------------

nanoarrow::UniqueSchema BuildMessageSchema(const google::protobuf::Descriptor* msg) {
    nanoarrow::UniqueSchema schema;
    BuildMessageSchemaInto(msg, schema.get());
    return schema;
}

// -----------------------------------------------------------------------
// CodeGenerator interface
// -----------------------------------------------------------------------

bool ArrowRowGenerator::Generate(const google::protobuf::FileDescriptor* file,
                                 const std::string& parameter,
                                 google::protobuf::compiler::GeneratorContext* context,
                                 std::string* error) const {
    // GIR-8 (#55): fail fatally on genuinely-unsupported types before writing any
    // artifact (C++ header / view / TS / IPC / RBA). Recursion stays skipped.
    if (!ValidateNoUnsupportedIr(file, error)) return false;

    // Parse comma-separated options from --fletcher_opt=...
    bool schema_only = false;
    bool emit_ts = false;
    bool emit_ipc = false;
    bool emit_accessor = false;
    bool emit_rust = false;
    {
        std::istringstream ss(parameter);
        std::string token;
        while (std::getline(ss, token, ',')) {
            if (token == "schema_only")
                schema_only = true;
            else if (token == "ts")
                emit_ts = true;
            else if (token == "ipc")
                emit_ipc = true;
            else if (token == "accessor")
                emit_accessor = true;
            else if (token == "rust")
                emit_rust = true;
        }
    }

    // Always emit the C++ header (edge-compatible, nanoarrow only).
    {
        const std::string content = GenerateFile(file, schema_only);
        const std::string out_name = OutputFilename(file->name());
        std::unique_ptr<google::protobuf::io::ZeroCopyOutputStream> stream(context->Open(out_name));
        if (!WriteToStream(stream.get(), content, error)) return false;
    }

    // Emit the view header (Arrow C++ dependent, server-side).
    if (!schema_only) {
        const std::string view_content = GenerateViewFile(file);
        if (!view_content.empty()) {
            const std::string view_name = ViewOutputFilename(file->name());
            std::unique_ptr<google::protobuf::io::ZeroCopyOutputStream> stream(
                context->Open(view_name));
            if (!WriteToStream(stream.get(), view_content, error)) return false;
        }
    }

    // Optionally emit the TypeScript file.
    if (emit_ts) {
        const std::string ts_content = GenerateTypeScriptFile(file);
        const std::string ts_out_name = TsOutputFilename(file->name());
        std::unique_ptr<google::protobuf::io::ZeroCopyOutputStream> stream(
            context->Open(ts_out_name));
        if (!WriteToStream(stream.get(), ts_content, error)) return false;
    }

    // Optionally emit one serialized Arrow IPC schema file per message
    // (<stem>.<Message>.ipc). Same message set as the generated header's
    // Schema() functions, and byte-identical to the schema bytes providers
    // announce at runtime.
    if (emit_ipc) {
        for (const auto* msg : OrderedMessages(file)) {
            if (IsRecursive(msg)) continue;
            if (IsFlattenedWrapper(msg)) continue;

            std::vector<uint8_t> ipc;
            try {
                nanoarrow::UniqueSchema schema = BuildMessageSchema(msg);
                ipc = SerializeSchemaIpc(schema.get());
            } catch (const std::exception& e) {
                *error = "failed to build IPC schema for '" + msg->full_name() + "': " + e.what();
                return false;
            }

            const std::string ipc_out_name = IpcOutputFilename(file->name(), ClassName(msg));
            std::unique_ptr<google::protobuf::io::ZeroCopyOutputStream> stream(
                context->Open(ipc_out_name));
            if (!WriteToStream(stream.get(), std::string(ipc.begin(), ipc.end()), error))
                return false;
        }
    }

    // Optionally emit the RecordBatch accessor C++ header (RBA-1). Additive
    // read-side artifact: independent of schema_only, and emitted unconditionally
    // (one file per token, never content-gated) whenever the option is set.
    if (emit_accessor) {
        const std::string content = EmitAccessorHeader(file);
        const std::string out_name = AccessorOutputFilename(file->name());
        std::unique_ptr<google::protobuf::io::ZeroCopyOutputStream> stream(context->Open(out_name));
        if (!WriteToStream(stream.get(), content, error)) return false;
    }

    // Optionally emit the RecordBatch accessor Rust module (RBA-1). Same
    // additive, unconditional emission contract as the accessor header above.
    if (emit_rust) {
        const std::string content = EmitRustAccessor(file);
        const std::string out_name = RustAccessorOutputFilename(file->name());
        std::unique_ptr<google::protobuf::io::ZeroCopyOutputStream> stream(context->Open(out_name));
        if (!WriteToStream(stream.get(), content, error)) return false;
        // NB: the shared `__rba.fletcher.rs` span/Row helper module is NOT emitted
        // here. Generate() runs once per input .proto, so emitting __rba here would
        // write it N times in a single multi-file protoc invocation
        // (`protoc a.proto b.proto …`) and protoc rejects the duplicate filename.
        // It is emitted EXACTLY ONCE from GenerateAll() instead (code-review P1).
    }

    return true;
}

bool ArrowRowGenerator::GenerateAll(
    const std::vector<const google::protobuf::FileDescriptor*>& files, const std::string& parameter,
    google::protobuf::compiler::GeneratorContext* context, std::string* error) const {
    // Per-file artifacts: identical to the default GenerateAll loop. Each file's
    // outputs (C++ header / view / ts / ipc / accessor / rust accessor) are
    // emitted by Generate().
    for (const auto* file : files) {
        if (!Generate(file, parameter, context, error)) return false;
    }

    // The shared `__rba` span/Row helper module is emitted EXACTLY ONCE per protoc
    // invocation, regardless of how many .proto files were passed (code-review
    // P1). It carries zero per-file/per-message content; the build.rs assembler
    // include!s it once directly under crate::fletcher_gen::__rba (N1). Only emit
    // when the `rust` opt is set.
    bool emit_rust = false;
    {
        std::istringstream ss(parameter);
        std::string token;
        while (std::getline(ss, token, ',')) {
            if (token == "rust") emit_rust = true;
        }
    }
    if (emit_rust) {
        const std::string rba_content = EmitRustRbaHelpers();
        std::unique_ptr<google::protobuf::io::ZeroCopyOutputStream> rba_stream(
            context->Open("__rba.fletcher.rs"));
        if (!WriteToStream(rba_stream.get(), rba_content, error)) return false;
    }

    return true;
}

}  // namespace fletcher
