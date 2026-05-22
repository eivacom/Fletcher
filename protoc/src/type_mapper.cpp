// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include "type_mapper.hpp"

#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/unknown_field_set.h>

#include <set>

namespace fletcher {

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

static bool HasMessageFlatten(const google::protobuf::Descriptor* msg) {
    return FindBoolOption(msg->options(), kFlattenOptionNumber);
}

namespace {

// -----------------------------------------------------------------------
// Cross-file reference helpers
// -----------------------------------------------------------------------

static std::string DotToColonsTM(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2 * std::count(s.begin(), s.end(), '.'));
    for (char c : s) {
        if (c == '.')
            out += "::";
        else
            out += c;
    }
    return out;
}

// Compute the bare (unqualified) generated class name for a message.
// Handles nested messages: Outer.Inner → "Outer_Inner".
// (Mirrors the public ClassName() function below; defined here so the
// cross-file helpers can call it without a forward declaration.)
static std::string ClassNameImpl(const google::protobuf::Descriptor* msg) {
    std::string name = msg->name();
    const auto* parent = msg->containing_type();
    while (parent) {
        name = parent->name() + "_" + name;
        parent = parent->containing_type();
    }
    return name;
}

// C++ globally-qualified class reference for msg, as seen from context_file.
// Returns the plain class name when both are in the same package (or file);
// otherwise prefixes with "::<package>::" so the reference is valid regardless
// of the current namespace.
static std::string QualifiedClassName(const google::protobuf::Descriptor* msg,
                                      const google::protobuf::FileDescriptor* context_file) {
    const std::string bare = ClassNameImpl(msg);
    if (msg->file() == context_file) return bare;
    // Same package → same C++ namespace, so bare name is sufficient.
    if (msg->file()->package() == context_file->package()) return bare;
    const std::string& pkg = msg->file()->package();
    if (pkg.empty()) return "::fletcher_gen::" + bare;
    return "::fletcher_gen::" + DotToColonsTM(pkg) + "::" + bare;
}

// Include path for the generated header of msg's file, relative to the proto root.
// Returns empty string when msg is in the same file as context_file.
static std::string CrossFileHeader(const google::protobuf::Descriptor* msg,
                                   const google::protobuf::FileDescriptor* context_file) {
    if (msg->file() == context_file) return "";
    const std::string& name = msg->file()->name();
    constexpr std::string_view kSuffix = ".proto";
    if (name.size() > kSuffix.size() && name.substr(name.size() - kSuffix.size()) == kSuffix)
        return name.substr(0, name.size() - kSuffix.size()) + ".fletcher.pb.h";
    return name + ".fletcher.pb.h";
}

// -----------------------------------------------------------------------
// Scalar base mappings (nullable=false; caller patches later)
// -----------------------------------------------------------------------

const ScalarTypeInfo* BaseScalar(FD::Type type) {
    // clang-format off
    static const ScalarTypeInfo kBool{
        "arrow::boolean()", "bool", "bool",
        "std::make_shared<arrow::BooleanScalar>({val})",
        "false", "arrow::BooleanBuilder",
        "arrow::BooleanScalar", false};
    static const ScalarTypeInfo kInt32{
        "arrow::int32()", "int32_t", "int32_t",
        "std::make_shared<arrow::Int32Scalar>({val})",
        "0", "arrow::Int32Builder",
        "arrow::Int32Scalar", false};
    static const ScalarTypeInfo kInt64{
        "arrow::int64()", "int64_t", "int64_t",
        "std::make_shared<arrow::Int64Scalar>({val})",
        "INT64_C(0)", "arrow::Int64Builder",
        "arrow::Int64Scalar", false};
    static const ScalarTypeInfo kUInt32{
        "arrow::uint32()", "uint32_t", "uint32_t",
        "std::make_shared<arrow::UInt32Scalar>({val})",
        "0u", "arrow::UInt32Builder",
        "arrow::UInt32Scalar", false};
    static const ScalarTypeInfo kUInt64{
        "arrow::uint64()", "uint64_t", "uint64_t",
        "std::make_shared<arrow::UInt64Scalar>({val})",
        "UINT64_C(0)", "arrow::UInt64Builder",
        "arrow::UInt64Scalar", false};
    static const ScalarTypeInfo kFloat{
        "arrow::float32()", "float", "float",
        "std::make_shared<arrow::FloatScalar>({val})",
        "0.0f", "arrow::FloatBuilder",
        "arrow::FloatScalar", false};
    static const ScalarTypeInfo kDouble{
        "arrow::float64()", "double", "double",
        "std::make_shared<arrow::DoubleScalar>({val})",
        "0.0", "arrow::DoubleBuilder",
        "arrow::DoubleScalar", false};
    static const ScalarTypeInfo kString{
        "arrow::utf8()", "std::string", "std::string_view",
        "std::make_shared<arrow::StringScalar>({val})",
        "\"\"", "arrow::StringBuilder",
        "arrow::StringScalar", true};
    static const ScalarTypeInfo kBytes{
        "arrow::binary()", "std::string", "std::string_view",
        "std::make_shared<arrow::BinaryScalar>({val})",
        "\"\"", "arrow::BinaryBuilder",
        "arrow::BinaryScalar", true};
    static const ScalarTypeInfo kEnum{
        "arrow::int32()", "int32_t", "int32_t",
        "std::make_shared<arrow::Int32Scalar>(static_cast<int32_t>({val}))",
        "0", "arrow::Int32Builder",
        "arrow::Int32Scalar", false};
    // clang-format on

    switch (type) {
        case FD::TYPE_BOOL:
            return &kBool;
        case FD::TYPE_INT32:
        case FD::TYPE_SINT32:
        case FD::TYPE_SFIXED32:
            return &kInt32;
        case FD::TYPE_INT64:
        case FD::TYPE_SINT64:
        case FD::TYPE_SFIXED64:
            return &kInt64;
        case FD::TYPE_UINT32:
        case FD::TYPE_FIXED32:
            return &kUInt32;
        case FD::TYPE_UINT64:
        case FD::TYPE_FIXED64:
            return &kUInt64;
        case FD::TYPE_FLOAT:
            return &kFloat;
        case FD::TYPE_DOUBLE:
            return &kDouble;
        case FD::TYPE_STRING:
            return &kString;
        case FD::TYPE_BYTES:
            return &kBytes;
        case FD::TYPE_ENUM:
            return &kEnum;
        default:
            return nullptr;
    }
}

bool IsFieldNullable(const google::protobuf::FieldDescriptor* field) {
    if (field->has_optional_keyword()) return true;
    if (field->file()->syntax() == google::protobuf::FileDescriptor::SYNTAX_PROTO2 &&
        field->label() == FD::LABEL_OPTIONAL)
        return true;
    return false;
}

// -----------------------------------------------------------------------
// Well-known type helpers
// -----------------------------------------------------------------------

// google.protobuf.*Value wrappers → nullable scalar of the inner type.
const ScalarTypeInfo* WrapperTypeInfo(const std::string& fqn) {
    static const ScalarTypeInfo kBoolVal{"arrow::boolean()",
                                         "bool",
                                         "bool",
                                         "std::make_shared<arrow::BooleanScalar>({val})",
                                         "false",
                                         "arrow::BooleanBuilder",
                                         "arrow::BooleanScalar",
                                         false};
    static const ScalarTypeInfo kInt32Val{"arrow::int32()",
                                          "int32_t",
                                          "int32_t",
                                          "std::make_shared<arrow::Int32Scalar>({val})",
                                          "0",
                                          "arrow::Int32Builder",
                                          "arrow::Int32Scalar",
                                          false};
    static const ScalarTypeInfo kInt64Val{"arrow::int64()",
                                          "int64_t",
                                          "int64_t",
                                          "std::make_shared<arrow::Int64Scalar>({val})",
                                          "INT64_C(0)",
                                          "arrow::Int64Builder",
                                          "arrow::Int64Scalar",
                                          false};
    static const ScalarTypeInfo kUInt32Val{"arrow::uint32()",
                                           "uint32_t",
                                           "uint32_t",
                                           "std::make_shared<arrow::UInt32Scalar>({val})",
                                           "0u",
                                           "arrow::UInt32Builder",
                                           "arrow::UInt32Scalar",
                                           false};
    static const ScalarTypeInfo kUInt64Val{"arrow::uint64()",
                                           "uint64_t",
                                           "uint64_t",
                                           "std::make_shared<arrow::UInt64Scalar>({val})",
                                           "UINT64_C(0)",
                                           "arrow::UInt64Builder",
                                           "arrow::UInt64Scalar",
                                           false};
    static const ScalarTypeInfo kFloatVal{"arrow::float32()",
                                          "float",
                                          "float",
                                          "std::make_shared<arrow::FloatScalar>({val})",
                                          "0.0f",
                                          "arrow::FloatBuilder",
                                          "arrow::FloatScalar",
                                          false};
    static const ScalarTypeInfo kDoubleVal{"arrow::float64()",
                                           "double",
                                           "double",
                                           "std::make_shared<arrow::DoubleScalar>({val})",
                                           "0.0",
                                           "arrow::DoubleBuilder",
                                           "arrow::DoubleScalar",
                                           false};
    static const ScalarTypeInfo kStringVal{"arrow::utf8()",
                                           "std::string",
                                           "std::string_view",
                                           "std::make_shared<arrow::StringScalar>({val})",
                                           "\"\"",
                                           "arrow::StringBuilder",
                                           "arrow::StringScalar",
                                           true};
    static const ScalarTypeInfo kBytesVal{"arrow::binary()",
                                          "std::string",
                                          "std::string_view",
                                          "std::make_shared<arrow::BinaryScalar>({val})",
                                          "\"\"",
                                          "arrow::BinaryBuilder",
                                          "arrow::BinaryScalar",
                                          true};

    if (fqn == "google.protobuf.BoolValue") return &kBoolVal;
    if (fqn == "google.protobuf.Int32Value") return &kInt32Val;
    if (fqn == "google.protobuf.Int64Value") return &kInt64Val;
    if (fqn == "google.protobuf.UInt32Value") return &kUInt32Val;
    if (fqn == "google.protobuf.UInt64Value") return &kUInt64Val;
    if (fqn == "google.protobuf.FloatValue") return &kFloatVal;
    if (fqn == "google.protobuf.DoubleValue") return &kDoubleVal;
    if (fqn == "google.protobuf.StringValue") return &kStringVal;
    if (fqn == "google.protobuf.BytesValue") return &kBytesVal;
    return nullptr;
}

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

// -----------------------------------------------------------------------
// MapField helpers for composite kinds
// -----------------------------------------------------------------------

std::optional<FieldMapping> MapScalarField(const FD* field) {
    const ScalarTypeInfo* base = BaseScalar(field->type());
    if (!base) return std::nullopt;
    FieldMapping m{};
    m.kind = FieldKind::SCALAR;
    m.nullable = IsFieldNullable(field);
    m.scalar = *base;
    return m;
}

std::optional<FieldMapping> MapRepeatedScalar(const FD* field) {
    const ScalarTypeInfo* base = BaseScalar(field->type());
    if (!base) return std::nullopt;
    FieldMapping m{};
    m.kind = FieldKind::REPEATED_SCALAR;
    m.nullable = false;  // repeated fields are never null — empty list is the default
    m.element = *base;
    return m;
}

std::optional<FieldMapping> MapRepeatedEnum(const FD* /*field*/) {
    const ScalarTypeInfo* base = BaseScalar(FD::TYPE_ENUM);
    FieldMapping m{};
    m.kind = FieldKind::REPEATED_SCALAR;
    m.nullable = false;
    m.element = *base;
    return m;
}

std::optional<FieldMapping> MapStructField(const FD* field) {
    const auto* msg = field->message_type();

    // Message-level flatten: strip the wrapper and map the inner field.
    auto flat = MapFlattenedSingular(field);
    if (flat) return flat;

    if (IsRecursive(msg)) return std::nullopt;

    FieldMapping m{};
    m.kind = FieldKind::STRUCT;
    m.nullable = IsFieldNullable(field);
    m.nested_class = QualifiedClassName(msg, field->file());
    m.nested_header = CrossFileHeader(msg, field->file());

    int depth = NestingDepth(msg);
    if (depth >= 3)
        m.warning = "nesting depth " + std::to_string(depth + 1) +
                    " — some Arrow consumers may not handle deep nesting well";
    return m;
}

std::optional<FieldMapping> MapRepeatedMessage(const FD* field) {
    const auto* msg = field->message_type();

    // Message-level flatten: strip the wrapper, accumulate list depth.
    auto flat = MapFlattenedRepeated(field);
    if (flat) return flat;

    if (IsRecursive(msg)) return std::nullopt;

    FieldMapping m{};
    m.kind = FieldKind::REPEATED_STRUCT;
    m.nullable = false;
    m.nested_class = QualifiedClassName(msg, field->file());
    m.nested_header = CrossFileHeader(msg, field->file());

    int depth = NestingDepth(msg);
    if (depth >= 3)
        m.warning = "list of deeply nested struct (depth " + std::to_string(depth + 1) + ")";
    return m;
}

std::optional<FieldMapping> MapMapField(const FD* field) {
    const auto* entry = field->message_type();
    const auto* key_fd = entry->field(0);  // "key"
    const auto* val_fd = entry->field(1);  // "value"

    // Key must be a supported scalar (proto restricts keys to integral/string/bool).
    const ScalarTypeInfo* key_info = BaseScalar(key_fd->type());
    if (!key_info) return std::nullopt;

    FieldMapping m{};
    m.kind = FieldKind::MAP;
    m.nullable = false;  // repeated (map) fields are never null
    m.map_key = *key_info;
    m.warning =
        "map type has limited Arrow compute kernel support; "
        "consider named struct fields if the key set is known at schema time";

    if (val_fd->type() == FD::TYPE_ENUM) {
        m.map_value_is_message = false;
        m.map_value = *BaseScalar(FD::TYPE_ENUM);
    } else if (val_fd->type() == FD::TYPE_MESSAGE) {
        const auto* val_msg = val_fd->message_type();
        if (IsRecursive(val_msg)) return std::nullopt;
        m.map_value_is_message = true;
        m.map_value_class = QualifiedClassName(val_msg, field->file());
        m.map_value_header = CrossFileHeader(val_msg, field->file());
        m.warning += "; map with message values has fragile Parquet round-trip";
    } else {
        const ScalarTypeInfo* vi = BaseScalar(val_fd->type());
        if (!vi) return std::nullopt;
        m.map_value_is_message = false;
        m.map_value = *vi;
    }
    return m;
}

// -----------------------------------------------------------------------
// Message-level flatten: resolve wrapper chains
// -----------------------------------------------------------------------

// For a singular message field whose target has (fletcher.flatten), resolve
// through the wrapper chain and return the mapping of the innermost field.
std::optional<FieldMapping> MapFlattenedSingular(const FD* field) {
    const auto* msg = field->message_type();
    if (!HasMessageFlatten(msg)) return std::nullopt;

    if (msg->field_count() != 1) {
        FieldMapping m{};
        m.kind = FieldKind::STRUCT;
        m.nullable = IsFieldNullable(field);
        m.nested_class = QualifiedClassName(msg, field->file());
        m.nested_header = CrossFileHeader(msg, field->file());
        m.warning = "(fletcher.flatten) ignored on " + msg->full_name() + " (" +
                    std::to_string(msg->field_count()) +
                    " fields); apply flatten to individual fields instead";
        return m;
    }

    // Walk through the single inner field — may itself be another flattened
    // wrapper, a scalar, a repeated, etc.  Use MapField on the inner field
    // so all dispatch logic (WKT, flatten chaining, etc.) applies.
    const auto* inner = msg->field(0);

    if (inner->is_repeated()) {
        std::optional<FieldMapping> resolved;
        if (inner->type() == FD::TYPE_MESSAGE)
            resolved = MapRepeatedMessage(inner);
        else if (inner->type() == FD::TYPE_ENUM)
            resolved = MapRepeatedEnum(inner);
        else
            resolved = MapRepeatedScalar(inner);
        if (resolved && IsFieldNullable(field)) resolved->nullable = true;
        return resolved;
    }

    // Singular inner field — resolve recursively (handles chaining).
    auto resolved = MapField(inner);
    if (resolved && IsFieldNullable(field)) resolved->nullable = true;
    return resolved;
}

// For a repeated message field whose target has (fletcher.flatten), walk
// the flatten chain counting list levels until we reach a leaf type.
// Generalises the previous hardcoded wrapper chain-walking logic.
std::optional<FieldMapping> MapFlattenedRepeated(const FD* field) {
    const auto* msg = field->message_type();
    if (!HasMessageFlatten(msg)) return std::nullopt;

    if (msg->field_count() != 1) {
        FieldMapping m{};
        m.kind = FieldKind::REPEATED_STRUCT;
        m.nullable = false;
        m.nested_class = QualifiedClassName(msg, field->file());
        m.nested_header = CrossFileHeader(msg, field->file());
        m.warning = "(fletcher.flatten) ignored on " + msg->full_name() + " (" +
                    std::to_string(msg->field_count()) +
                    " fields); apply flatten to individual fields instead";
        return m;
    }

    // Walk the chain: each flattened wrapper with a repeated-message inner
    // field adds one list nesting level.  depth counts only chain-internal
    // levels; the caller's own `repeated` keyword adds +1 at the end.
    int depth = 0;
    const google::protobuf::Descriptor* current = msg;
    while (HasMessageFlatten(current) && current->field_count() == 1) {
        const auto* inner = current->field(0);

        if (inner->is_repeated() && inner->type() == FD::TYPE_MESSAGE) {
            ++depth;
            current = inner->message_type();
            continue;
        }

        if (inner->is_repeated()) {
            // Leaf is a repeated scalar/enum — produces one more list level.
            const ScalarTypeInfo* base = (inner->type() == FD::TYPE_ENUM)
                                             ? BaseScalar(FD::TYPE_ENUM)
                                             : BaseScalar(inner->type());
            if (!base) return std::nullopt;

            if (depth == 0) {
                FieldMapping m{};
                m.kind = FieldKind::REPEATED_SCALAR;
                m.nullable = false;
                m.element = *base;
                return m;
            }
            // depth > 0: nested list of scalars — not yet supported by the
            // generator (would need NESTED_LIST with scalar leaf).  Fall
            // through to produce a warning.
            return std::nullopt;
        }

        // Singular inner field — the wrapper strips without adding a list
        // level.  If the inner is itself a flattened message, continue walking.
        if (inner->type() == FD::TYPE_MESSAGE && HasMessageFlatten(inner->message_type())) {
            current = inner->message_type();
            continue;
        }

        // Singular scalar/enum leaf inside the chain.  The caller's field
        // is `repeated`, so the result is always list-wrapped.
        if (inner->type() != FD::TYPE_MESSAGE) {
            const ScalarTypeInfo* base = (inner->type() == FD::TYPE_ENUM)
                                             ? BaseScalar(FD::TYPE_ENUM)
                                             : BaseScalar(inner->type());
            if (!base) return std::nullopt;

            if (depth == 0) {
                FieldMapping m{};
                m.kind = FieldKind::REPEATED_SCALAR;
                m.nullable = false;
                m.element = *base;
                return m;
            }
            return std::nullopt;
        }

        // Singular non-flattened message leaf — it becomes a struct.
        current = inner->message_type();
        break;
    }

    // 'current' is the leaf struct.  Total list depth = depth (from chain)
    // + 1 (from the caller's own `repeated` keyword).
    if (IsRecursive(current)) return std::nullopt;

    int total = depth + 1;

    FieldMapping m{};
    m.nullable = false;
    m.nested_class = QualifiedClassName(current, field->file());
    m.nested_header = CrossFileHeader(current, field->file());

    if (total <= 1) {
        m.kind = FieldKind::REPEATED_STRUCT;
    } else {
        m.kind = FieldKind::NESTED_LIST;
        m.list_depth = total;
    }
    return m;
}

// -----------------------------------------------------------------------
// Well-known type recognition (Timestamp, Duration, *Value wrappers)
// -----------------------------------------------------------------------

std::optional<FieldMapping> MapWellKnown(const FD* field) {
    const std::string& fqn = field->message_type()->full_name();

    // Timestamp → arrow::timestamp(NANO)
    if (fqn == "google.protobuf.Timestamp") {
        FieldMapping m{};
        m.kind = FieldKind::SCALAR;
        m.nullable = IsFieldNullable(field);
        m.scalar = {"arrow::timestamp(arrow::TimeUnit::NANO)",
                    "int64_t",
                    "int64_t",
                    "std::make_shared<arrow::TimestampScalar>"
                    "({val}, arrow::timestamp(arrow::TimeUnit::NANO))",
                    "INT64_C(0)",
                    "arrow::TimestampBuilder",
                    "arrow::TimestampScalar",
                    false};
        return m;
    }

    // Duration → arrow::duration(NANO)
    if (fqn == "google.protobuf.Duration") {
        FieldMapping m{};
        m.kind = FieldKind::SCALAR;
        m.nullable = IsFieldNullable(field);
        m.scalar = {"arrow::duration(arrow::TimeUnit::NANO)",
                    "int64_t",
                    "int64_t",
                    "std::make_shared<arrow::DurationScalar>"
                    "({val}, arrow::duration(arrow::TimeUnit::NANO))",
                    "INT64_C(0)",
                    "arrow::DurationBuilder",
                    "arrow::DurationScalar",
                    false};
        return m;
    }

    // Wrapper types → nullable scalar of the inner type
    const ScalarTypeInfo* wrapper = WrapperTypeInfo(fqn);
    if (wrapper) {
        FieldMapping m{};
        m.kind = FieldKind::SCALAR;
        m.nullable = true;  // wrappers exist to express "nullable T"
        m.scalar = *wrapper;
        return m;
    }

    return std::nullopt;  // unknown well-known or unsupported message
}

}  // namespace

// -----------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------

std::optional<FieldMapping> MapField(const google::protobuf::FieldDescriptor* field) {
    // Reject oneof fields (user-defined, not synthetic proto3 optional).
    if (field->real_containing_oneof()) return std::nullopt;

    // Map fields (detected before repeated, since maps are encoded as repeated).
    if (field->is_map()) return MapMapField(field);

    // Repeated fields.
    if (field->is_repeated()) {
        if (field->type() == FD::TYPE_MESSAGE) return MapRepeatedMessage(field);
        if (field->type() == FD::TYPE_ENUM) return MapRepeatedEnum(field);
        return MapRepeatedScalar(field);
    }

    // Singular message fields (struct or well-known type).
    if (field->type() == FD::TYPE_MESSAGE) {
        auto wk = MapWellKnown(field);
        if (wk) return wk;
        return MapStructField(field);
    }

    // Singular enum.
    if (field->type() == FD::TYPE_ENUM) return MapScalarField(field);

    // Singular scalar.
    return MapScalarField(field);
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
