#include "type_mapper.hpp"

#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/unknown_field_set.h>

#include <set>

namespace fletcher_plugin {

namespace {

using FD = google::protobuf::FieldDescriptor;

// -----------------------------------------------------------------------
// Cross-file reference helpers
// -----------------------------------------------------------------------

static std::string DotToColonsTM(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2 * std::count(s.begin(), s.end(), '.'));
    for (char c : s) {
        if (c == '.') out += "::";
        else          out += c;
    }
    return out;
}

// Compute the bare (unqualified) ArrowRow class name for a message.
// Handles nested messages: Outer.Inner → "Outer_InnerArrowRow".
// (Mirrors the public ClassName() function below; defined here so the
// cross-file helpers can call it without a forward declaration.)
static std::string ClassNameImpl(const google::protobuf::Descriptor* msg) {
    std::string name = msg->name();
    const auto* parent = msg->containing_type();
    while (parent) {
        name = parent->name() + "_" + name;
        parent = parent->containing_type();
    }
    return name + "ArrowRow";
}

// C++ globally-qualified class reference for msg, as seen from context_file.
// Returns the plain class name when both are in the same file; otherwise prefixes
// with "::<package>::" so the reference is valid regardless of current namespace.
static std::string QualifiedClassName(const google::protobuf::Descriptor* msg,
                                      const google::protobuf::FileDescriptor* context_file) {
    const std::string bare = ClassNameImpl(msg);
    if (msg->file() == context_file)
        return bare;
    // Same package → no global qualification needed; the #include brings
    // the class into the same namespace the consumer is already in.
    if (msg->file()->package() == context_file->package())
        return bare;
    const std::string& pkg = msg->file()->package();
    if (pkg.empty())
        return "::" + bare;
    return "::" + DotToColonsTM(pkg) + "::" + bare;
}

// Include path for the generated header of msg's file, relative to the proto root.
// Returns empty string when msg is in the same file as context_file.
static std::string CrossFileHeader(const google::protobuf::Descriptor* msg,
                                   const google::protobuf::FileDescriptor* context_file) {
    if (msg->file() == context_file)
        return "";
    const std::string& name = msg->file()->name();
    constexpr std::string_view kSuffix = ".proto";
    if (name.size() > kSuffix.size()
        && name.substr(name.size() - kSuffix.size()) == kSuffix)
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
        case FD::TYPE_BOOL:     return &kBool;
        case FD::TYPE_INT32:
        case FD::TYPE_SINT32:
        case FD::TYPE_SFIXED32: return &kInt32;
        case FD::TYPE_INT64:
        case FD::TYPE_SINT64:
        case FD::TYPE_SFIXED64: return &kInt64;
        case FD::TYPE_UINT32:
        case FD::TYPE_FIXED32:  return &kUInt32;
        case FD::TYPE_UINT64:
        case FD::TYPE_FIXED64:  return &kUInt64;
        case FD::TYPE_FLOAT:    return &kFloat;
        case FD::TYPE_DOUBLE:   return &kDouble;
        case FD::TYPE_STRING:   return &kString;
        case FD::TYPE_BYTES:    return &kBytes;
        case FD::TYPE_ENUM:     return &kEnum;
        default:                return nullptr;
    }
}

bool IsFieldNullable(const google::protobuf::FieldDescriptor* field) {
    if (field->has_optional_keyword())
        return true;
    if (field->file()->syntax() == google::protobuf::FileDescriptor::SYNTAX_PROTO2
        && field->label() == FD::LABEL_OPTIONAL)
        return true;
    return false;
}

// -----------------------------------------------------------------------
// Well-known type helpers
// -----------------------------------------------------------------------

struct WellKnownScalar {
    const char* full_name;
    const ScalarTypeInfo* info;
};

// google.protobuf.*Value wrappers → nullable scalar of the inner type.
const ScalarTypeInfo* WrapperTypeInfo(const std::string& fqn) {
    static const ScalarTypeInfo kBoolVal{
        "arrow::boolean()", "bool", "bool",
        "std::make_shared<arrow::BooleanScalar>({val})",
        "false", "arrow::BooleanBuilder",
        "arrow::BooleanScalar", false};
    static const ScalarTypeInfo kInt32Val{
        "arrow::int32()", "int32_t", "int32_t",
        "std::make_shared<arrow::Int32Scalar>({val})",
        "0", "arrow::Int32Builder",
        "arrow::Int32Scalar", false};
    static const ScalarTypeInfo kInt64Val{
        "arrow::int64()", "int64_t", "int64_t",
        "std::make_shared<arrow::Int64Scalar>({val})",
        "INT64_C(0)", "arrow::Int64Builder",
        "arrow::Int64Scalar", false};
    static const ScalarTypeInfo kUInt32Val{
        "arrow::uint32()", "uint32_t", "uint32_t",
        "std::make_shared<arrow::UInt32Scalar>({val})",
        "0u", "arrow::UInt32Builder",
        "arrow::UInt32Scalar", false};
    static const ScalarTypeInfo kUInt64Val{
        "arrow::uint64()", "uint64_t", "uint64_t",
        "std::make_shared<arrow::UInt64Scalar>({val})",
        "UINT64_C(0)", "arrow::UInt64Builder",
        "arrow::UInt64Scalar", false};
    static const ScalarTypeInfo kFloatVal{
        "arrow::float32()", "float", "float",
        "std::make_shared<arrow::FloatScalar>({val})",
        "0.0f", "arrow::FloatBuilder",
        "arrow::FloatScalar", false};
    static const ScalarTypeInfo kDoubleVal{
        "arrow::float64()", "double", "double",
        "std::make_shared<arrow::DoubleScalar>({val})",
        "0.0", "arrow::DoubleBuilder",
        "arrow::DoubleScalar", false};
    static const ScalarTypeInfo kStringVal{
        "arrow::utf8()", "std::string", "std::string_view",
        "std::make_shared<arrow::StringScalar>({val})",
        "\"\"", "arrow::StringBuilder",
        "arrow::StringScalar", true};
    static const ScalarTypeInfo kBytesVal{
        "arrow::binary()", "std::string", "std::string_view",
        "std::make_shared<arrow::BinaryScalar>({val})",
        "\"\"", "arrow::BinaryBuilder",
        "arrow::BinaryScalar", true};

    if (fqn == "google.protobuf.BoolValue")   return &kBoolVal;
    if (fqn == "google.protobuf.Int32Value")   return &kInt32Val;
    if (fqn == "google.protobuf.Int64Value")   return &kInt64Val;
    if (fqn == "google.protobuf.UInt32Value")  return &kUInt32Val;
    if (fqn == "google.protobuf.UInt64Value")  return &kUInt64Val;
    if (fqn == "google.protobuf.FloatValue")   return &kFloatVal;
    if (fqn == "google.protobuf.DoubleValue")  return &kDoubleVal;
    if (fqn == "google.protobuf.StringValue")  return &kStringVal;
    if (fqn == "google.protobuf.BytesValue")   return &kBytesVal;
    return nullptr;
}

// -----------------------------------------------------------------------
// Recursion detection
// -----------------------------------------------------------------------

bool IsRecursiveImpl(const google::protobuf::Descriptor* msg,
                     std::set<const google::protobuf::Descriptor*>& stack) {
    if (stack.count(msg))
        return true;
    stack.insert(msg);
    for (int i = 0; i < msg->field_count(); ++i) {
        const auto* f = msg->field(i);
        if (f->type() != FD::TYPE_MESSAGE)
            continue;
        if (f->is_map()) {
            // Only the value type can introduce a cycle.
            const auto* val_field = f->message_type()->field(1);
            if (val_field->type() == FD::TYPE_MESSAGE
                && IsRecursiveImpl(val_field->message_type(), stack))
                return true;
        } else {
            if (IsRecursiveImpl(f->message_type(), stack))
                return true;
        }
    }
    stack.erase(msg);
    return false;
}

int NestingDepthImpl(const google::protobuf::Descriptor* msg,
                     std::set<const google::protobuf::Descriptor*>& visited) {
    if (visited.count(msg))
        return 0;  // cycle — handled by IsRecursive
    visited.insert(msg);
    int max_d = 0;
    for (int i = 0; i < msg->field_count(); ++i) {
        const auto* f = msg->field(i);
        if (f->type() != FD::TYPE_MESSAGE || f->is_map())
            continue;
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
    if (!base)
        return std::nullopt;
    FieldMapping m{};
    m.kind     = FieldKind::SCALAR;
    m.nullable = IsFieldNullable(field);
    m.scalar   = *base;
    return m;
}

std::optional<FieldMapping> MapRepeatedScalar(const FD* field) {
    const ScalarTypeInfo* base = BaseScalar(field->type());
    if (!base)
        return std::nullopt;
    FieldMapping m{};
    m.kind    = FieldKind::REPEATED_SCALAR;
    m.nullable = false;  // repeated fields are never null — empty list is the default
    m.element = *base;
    return m;
}

std::optional<FieldMapping> MapRepeatedEnum(const FD* /*field*/) {
    const ScalarTypeInfo* base = BaseScalar(FD::TYPE_ENUM);
    FieldMapping m{};
    m.kind    = FieldKind::REPEATED_SCALAR;
    m.nullable = false;
    m.element = *base;
    return m;
}

std::optional<FieldMapping> MapStructField(const FD* field) {
    const auto* msg = field->message_type();
    if (IsRecursive(msg))
        return std::nullopt;

    FieldMapping m{};
    m.kind          = FieldKind::STRUCT;
    m.nullable      = IsFieldNullable(field);
    m.nested_class  = QualifiedClassName(msg, field->file());
    m.nested_header = CrossFileHeader(msg, field->file());

    int depth = NestingDepth(msg);
    if (depth >= 3)
        m.warning = "nesting depth " + std::to_string(depth + 1)
                  + " — some Arrow consumers may not handle deep nesting well";
    return m;
}

std::optional<FieldMapping> MapRepeatedMessage(const FD* field) {
    const auto* msg = field->message_type();
    if (IsRecursive(msg))
        return std::nullopt;

    FieldMapping m{};
    m.kind          = FieldKind::REPEATED_STRUCT;
    m.nullable      = false;
    m.nested_class  = QualifiedClassName(msg, field->file());
    m.nested_header = CrossFileHeader(msg, field->file());

    int depth = NestingDepth(msg);
    if (depth >= 3)
        m.warning = "list of deeply nested struct (depth "
                  + std::to_string(depth + 1) + ")";
    return m;
}

std::optional<FieldMapping> MapMapField(const FD* field) {
    const auto* entry = field->message_type();
    const auto* key_fd  = entry->field(0);  // "key"
    const auto* val_fd  = entry->field(1);  // "value"

    // Key must be a supported scalar (proto restricts keys to integral/string/bool).
    const ScalarTypeInfo* key_info = BaseScalar(key_fd->type());
    if (!key_info)
        return std::nullopt;

    FieldMapping m{};
    m.kind    = FieldKind::MAP;
    m.nullable = false;  // repeated (map) fields are never null
    m.map_key = *key_info;
    m.warning = "map type has limited Arrow compute kernel support; "
                "consider named struct fields if the key set is known at schema time";

    if (val_fd->type() == FD::TYPE_ENUM) {
        m.map_value_is_message = false;
        m.map_value = *BaseScalar(FD::TYPE_ENUM);
    } else if (val_fd->type() == FD::TYPE_MESSAGE) {
        const auto* val_msg = val_fd->message_type();
        if (IsRecursive(val_msg))
            return std::nullopt;
        m.map_value_is_message = true;
        m.map_value_class  = QualifiedClassName(val_msg, field->file());
        m.map_value_header = CrossFileHeader(val_msg, field->file());
        m.warning += "; map with message values has fragile Parquet round-trip";
    } else {
        const ScalarTypeInfo* vi = BaseScalar(val_fd->type());
        if (!vi)
            return std::nullopt;
        m.map_value_is_message = false;
        m.map_value = *vi;
    }
    return m;
}

// -----------------------------------------------------------------------
// GeoArrow extension type recognition
// -----------------------------------------------------------------------

// Maps a geoarrow.* FQN to its GeoArrow extension name.
// Returns empty string if not a recognized GeoArrow type.
std::string GeoArrowExtensionName(const std::string& fqn) {
    // Coordinate structs
    if (fqn == "geoarrow.Point"  || fqn == "geoarrow.PointZ")  return "geoarrow.point";
    if (fqn == "geoarrow.Box"    || fqn == "geoarrow.BoxZ")    return "geoarrow.box";
    // Phase 1 list wrappers (depth 1)
    if (fqn == "geoarrow.LineString"  || fqn == "geoarrow.LineStringZ")  return "geoarrow.linestring";
    if (fqn == "geoarrow.MultiPoint"  || fqn == "geoarrow.MultiPointZ") return "geoarrow.multipoint";
    // Phase 2 nested list types (depth 2-3)
    if (fqn == "geoarrow.Polygon"          || fqn == "geoarrow.PolygonZ")          return "geoarrow.polygon";
    if (fqn == "geoarrow.MultiLineString"  || fqn == "geoarrow.MultiLineStringZ")  return "geoarrow.multilinestring";
    if (fqn == "geoarrow.MultiPolygon"     || fqn == "geoarrow.MultiPolygonZ")     return "geoarrow.multipolygon";
    return {};
}

// Returns true for wrapper messages that are collapsed into list fields
// (no ArrowRow class is generated for these).
bool IsGeoArrowWrapper(const std::string& fqn) {
    return fqn == "geoarrow.LineString"       || fqn == "geoarrow.LineStringZ"
        || fqn == "geoarrow.MultiPoint"       || fqn == "geoarrow.MultiPointZ"
        || fqn == "geoarrow.LinearRing"       || fqn == "geoarrow.LinearRingZ"
        || fqn == "geoarrow.Polygon"          || fqn == "geoarrow.PolygonZ"
        || fqn == "geoarrow.MultiLineString"  || fqn == "geoarrow.MultiLineStringZ"
        || fqn == "geoarrow.MultiPolygon"     || fqn == "geoarrow.MultiPolygonZ";
}

// Search an options message's UnknownFieldSet for a string extension by number.
// Custom options are stored as unknown fields when the extension isn't compiled
// into the plugin binary.
static std::string FindStringOption(const google::protobuf::Message& opts, int number) {
    const auto& unknown = opts.GetReflection()->GetUnknownFields(opts);
    for (int i = 0; i < unknown.field_count(); ++i) {
        const auto& f = unknown.field(i);
        if (f.number() == number &&
            f.type() == google::protobuf::UnknownField::TYPE_LENGTH_DELIMITED)
            return f.length_delimited();
    }
    return {};
}

// Read the CRS string from fletcher.crs (field option) or fletcher.default_crs
// (message option).  Returns empty if neither is set.
std::string ReadCrsOption(const FD* field) {
    // Field-level: fletcher.crs (extension number 50002)
    auto crs = FindStringOption(field->options(), 50002);
    if (!crs.empty()) return crs;

    // Message-level: fletcher.default_crs (extension number 50001)
    return FindStringOption(field->containing_type()->options(), 50001);
}

std::optional<FieldMapping> MapGeoArrow(const FD* field) {
    const auto* msg = field->message_type();
    const std::string& fqn = msg->full_name();

    std::string ext_name = GeoArrowExtensionName(fqn);
    if (ext_name.empty())
        return std::nullopt;

    // Coordinate structs (Point, PointZ, Box, BoxZ) → STRUCT with extension metadata.
    if (!IsGeoArrowWrapper(fqn)) {
        auto m = MapStructField(field);
        if (!m) return std::nullopt;
        m->extension_name = std::move(ext_name);
        m->crs = ReadCrsOption(field);
        return m;
    }

    // Walk the wrapper chain to find the innermost coordinate struct.
    // Each wrapper must have field(0) = repeated Message.
    int depth = 0;
    const google::protobuf::Descriptor* current = msg;
    while (IsGeoArrowWrapper(current->full_name())) {
        if (current->field_count() < 1)
            return std::nullopt;
        const auto* inner_field = current->field(0);
        if (!inner_field->is_repeated() || inner_field->type() != FD::TYPE_MESSAGE)
            return std::nullopt;
        ++depth;
        current = inner_field->message_type();
    }
    // 'current' is now the coordinate struct (Point, PointZ, etc.)
    // 'depth' is the number of list levels

    FieldMapping m{};
    m.nullable       = IsFieldNullable(field);
    m.nested_class   = QualifiedClassName(current, field->file());
    m.nested_header  = CrossFileHeader(current, field->file());
    m.extension_name = std::move(ext_name);
    m.crs            = ReadCrsOption(field);

    if (depth == 1) {
        m.kind = FieldKind::REPEATED_STRUCT;
    } else {
        m.kind       = FieldKind::NESTED_LIST;
        m.list_depth = depth;
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
        m.scalar = {
            "arrow::timestamp(arrow::TimeUnit::NANO)",
            "int64_t", "int64_t",
            "std::make_shared<arrow::TimestampScalar>"
                "({val}, arrow::timestamp(arrow::TimeUnit::NANO))",
            "INT64_C(0)", "arrow::TimestampBuilder",
            "arrow::TimestampScalar", false};
        return m;
    }

    // Duration → arrow::duration(NANO)
    if (fqn == "google.protobuf.Duration") {
        FieldMapping m{};
        m.kind = FieldKind::SCALAR;
        m.nullable = IsFieldNullable(field);
        m.scalar = {
            "arrow::duration(arrow::TimeUnit::NANO)",
            "int64_t", "int64_t",
            "std::make_shared<arrow::DurationScalar>"
                "({val}, arrow::duration(arrow::TimeUnit::NANO))",
            "INT64_C(0)", "arrow::DurationBuilder",
            "arrow::DurationScalar", false};
        return m;
    }

    // Wrapper types → nullable scalar of the inner type
    const ScalarTypeInfo* wrapper = WrapperTypeInfo(fqn);
    if (wrapper) {
        FieldMapping m{};
        m.kind     = FieldKind::SCALAR;
        m.nullable = true;  // wrappers exist to express "nullable T"
        m.scalar   = *wrapper;
        return m;
    }

    // GeoArrow extension types
    auto geo = MapGeoArrow(field);
    if (geo)
        return geo;

    return std::nullopt;  // unknown well-known or unsupported message
}

}  // namespace

// -----------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------

std::optional<FieldMapping> MapField(const google::protobuf::FieldDescriptor* field) {
    // Reject oneof fields (user-defined, not synthetic proto3 optional).
    if (field->real_containing_oneof())
        return std::nullopt;

    // Map fields (detected before repeated, since maps are encoded as repeated).
    if (field->is_map())
        return MapMapField(field);

    // Repeated fields.
    if (field->is_repeated()) {
        if (field->type() == FD::TYPE_MESSAGE)
            return MapRepeatedMessage(field);
        if (field->type() == FD::TYPE_ENUM)
            return MapRepeatedEnum(field);
        return MapRepeatedScalar(field);
    }

    // Singular message fields (struct or well-known type).
    if (field->type() == FD::TYPE_MESSAGE) {
        auto wk = MapWellKnown(field);
        if (wk)
            return wk;
        return MapStructField(field);
    }

    // Singular enum.
    if (field->type() == FD::TYPE_ENUM)
        return MapScalarField(field);

    // Singular scalar.
    return MapScalarField(field);
}

std::string UnsupportedReason(const google::protobuf::FieldDescriptor* field) {
    if (field->real_containing_oneof())
        return "oneof '" + field->real_containing_oneof()->name()
             + "' cannot be mapped to a Parquet-safe Arrow type; "
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

    if (field->type() == FD::TYPE_GROUP)
        return "proto2 groups are not supported";

    return "unsupported proto field type";
}

bool IsRecursive(const google::protobuf::Descriptor* msg) {
    std::set<const google::protobuf::Descriptor*> stack;
    return IsRecursiveImpl(msg, stack);
}

bool IsGeoArrowWrapper(const google::protobuf::Descriptor* msg) {
    return IsGeoArrowWrapper(msg->full_name());
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
    return name + "ArrowRow";
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
        case FDT::TYPE_BOOL:     return "boolean";
        case FDT::TYPE_INT32:
        case FDT::TYPE_SINT32:
        case FDT::TYPE_SFIXED32: return "number";
        case FDT::TYPE_INT64:
        case FDT::TYPE_SINT64:
        case FDT::TYPE_SFIXED64: return "bigint";
        case FDT::TYPE_UINT32:
        case FDT::TYPE_FIXED32:  return "number";
        case FDT::TYPE_UINT64:
        case FDT::TYPE_FIXED64:  return "bigint";
        case FDT::TYPE_FLOAT:    return "number";
        case FDT::TYPE_DOUBLE:   return "number";
        case FDT::TYPE_STRING:   return "string";
        case FDT::TYPE_BYTES:    return "Uint8Array";
        case FDT::TYPE_ENUM:     return "number";
        default:                 return "";
    }
}

std::string WireTypeIdName(google::protobuf::FieldDescriptor::Type type) {
    using FDT = google::protobuf::FieldDescriptor;
    switch (type) {
        case FDT::TYPE_BOOL:     return "WireTypeId.BOOL";
        case FDT::TYPE_INT32:
        case FDT::TYPE_SINT32:
        case FDT::TYPE_SFIXED32: return "WireTypeId.INT32";
        case FDT::TYPE_INT64:
        case FDT::TYPE_SINT64:
        case FDT::TYPE_SFIXED64: return "WireTypeId.INT64";
        case FDT::TYPE_UINT32:
        case FDT::TYPE_FIXED32:  return "WireTypeId.UINT32";
        case FDT::TYPE_UINT64:
        case FDT::TYPE_FIXED64:  return "WireTypeId.UINT64";
        case FDT::TYPE_FLOAT:    return "WireTypeId.FLOAT32";
        case FDT::TYPE_DOUBLE:   return "WireTypeId.FLOAT64";
        case FDT::TYPE_STRING:   return "WireTypeId.STRING";
        case FDT::TYPE_BYTES:    return "WireTypeId.BINARY";
        case FDT::TYPE_ENUM:     return "WireTypeId.INT32";
        default:                 return "";
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

}  // namespace fletcher_plugin
