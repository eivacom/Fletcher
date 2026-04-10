#include "row_codec.hpp"
#include "row_reader.hpp"
#include "scalar_codec.hpp"
#include "schema_evolution.hpp"

#include <arrow/scalar.h>
#include <arrow/type.h>

#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

namespace fletcher {
namespace {

// ---------------------------------------------------------------------------
// Encoder helpers
// ---------------------------------------------------------------------------

template <typename T>
void AppendFixed(std::vector<uint8_t>& buf, T value) {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&value);
    buf.insert(buf.end(), bytes, bytes + sizeof(T));
}

void AppendVariableLength(std::vector<uint8_t>& buf,
                          const uint8_t*        data,
                          uint32_t              len) {
    AppendFixed(buf, len);
    buf.insert(buf.end(), data, data + len);
}

// Extract the integer "field_number" metadata value from an Arrow field.
// When no metadata is present, returns the positional fallback (1-indexed).
int GetFieldNumber(const arrow::Field& field, int positional_fallback) {
    auto meta = field.metadata();
    if (!meta) return positional_fallback;
    auto idx = meta->FindKey("field_number");
    if (idx < 0) return positional_fallback;
    return std::stoi(meta->value(idx));
}

// ---------------------------------------------------------------------------
// Tagged struct encoder (recursive)
// ---------------------------------------------------------------------------

void EncodeStructTagged(std::vector<uint8_t>& buf,
                        const arrow::StructScalar& scalar,
                        const arrow::StructType& stype);

// Encode a single value's payload bytes.
// For struct types, uses the recursive tagged format.
// For other composites (list, map, union), encodes elements using
// detail::EncodeScalar for leaf scalars and recursion for struct elements.
void EncodePayload(std::vector<uint8_t>& buf,
                   const arrow::Scalar& scalar) {
    using T = arrow::Type;

    switch (scalar.type->id()) {
    case T::STRUCT: {
        const auto& ss    = static_cast<const arrow::StructScalar&>(scalar);
        const auto& stype = static_cast<const arrow::StructType&>(*scalar.type);
        EncodeStructTagged(buf, ss, stype);
        return;
    }
    case T::LIST:
    case T::LARGE_LIST: {
        const auto& ls    = static_cast<const arrow::BaseListScalar&>(scalar);
        const int64_t len = ls.value->length();
        AppendFixed(buf, static_cast<uint32_t>(len));
        for (int64_t i = 0; i < len; ++i) {
            auto elem = ls.value->GetScalar(i).ValueOrDie();
            const bool is_null = !elem->is_valid;
            buf.push_back(is_null ? 0x01u : 0x00u);
            if (!is_null) EncodePayload(buf, *elem);
        }
        return;
    }
    case T::FIXED_SIZE_LIST: {
        const auto& ls = static_cast<const arrow::BaseListScalar&>(scalar);
        for (int64_t i = 0; i < ls.value->length(); ++i) {
            auto elem = ls.value->GetScalar(i).ValueOrDie();
            const bool is_null = !elem->is_valid;
            buf.push_back(is_null ? 0x01u : 0x00u);
            if (!is_null) EncodePayload(buf, *elem);
        }
        return;
    }
    case T::MAP: {
        const auto& ms       = static_cast<const arrow::MapScalar&>(scalar);
        auto struct_arr      = std::static_pointer_cast<arrow::StructArray>(ms.value);
        const int64_t count  = struct_arr->length();
        auto key_arr         = struct_arr->field(0);
        auto val_arr         = struct_arr->field(1);
        AppendFixed(buf, static_cast<uint32_t>(count));
        for (int64_t i = 0; i < count; ++i) {
            auto key = key_arr->GetScalar(i).ValueOrDie();
            detail::EncodeScalar(buf, *key);
            auto val = val_arr->GetScalar(i).ValueOrDie();
            buf.push_back(!val->is_valid ? 0x01u : 0x00u);
            if (val->is_valid) EncodePayload(buf, *val);
        }
        return;
    }
    case T::SPARSE_UNION: {
        const auto& us         = static_cast<const arrow::SparseUnionScalar&>(scalar);
        const auto& union_type = static_cast<const arrow::SparseUnionType&>(*scalar.type);
        AppendFixed(buf, us.type_code);
        const auto& codes = union_type.type_codes();
        int child_id = -1;
        for (int i = 0; i < static_cast<int>(codes.size()); ++i) {
            if (codes[i] == us.type_code) { child_id = i; break; }
        }
        if (child_id < 0 || child_id >= static_cast<int>(us.value.size()))
            throw std::invalid_argument("EncodeRow: invalid sparse union type_code");
        detail::EncodeScalar(buf, *us.value[static_cast<size_t>(child_id)]);
        return;
    }
    case T::DENSE_UNION: {
        const auto& us = static_cast<const arrow::DenseUnionScalar&>(scalar);
        AppendFixed(buf, us.type_code);
        detail::EncodeScalar(buf, *us.value);
        return;
    }
    default:
        // Scalar types — delegate to the raw encoder.
        detail::EncodeScalar(buf, scalar);
        return;
    }
}

void EncodeStructTagged(std::vector<uint8_t>& buf,
                        const arrow::StructScalar& scalar,
                        const arrow::StructType& stype) {
    const int num_fields = stype.num_fields();
    AppendFixed(buf, static_cast<uint16_t>(num_fields));

    for (int i = 0; i < num_fields; ++i) {
        const auto& child_field = stype.field(i);
        const auto& child       = scalar.value[static_cast<size_t>(i)];
        const bool  is_null     = !child || !child->is_valid;

        uint32_t field_num = static_cast<uint32_t>(GetFieldNumber(*child_field, i + 1));
        WireTypeId wire_type = ArrowTypeToWireTypeId(*child_field->type());

        AppendFixed(buf, field_num);
        buf.push_back(static_cast<uint8_t>(wire_type));
        buf.push_back(is_null ? 0x01u : 0x00u);

        if (!is_null) {
            std::vector<uint8_t> payload;
            payload.reserve(32);
            EncodePayload(payload, *child);
            AppendFixed(buf, static_cast<uint32_t>(payload.size()));
            buf.insert(buf.end(), payload.begin(), payload.end());
        }
    }
}

// ---------------------------------------------------------------------------
// Tagged struct decoder (recursive)
// ---------------------------------------------------------------------------

// Forward declarations.
std::shared_ptr<arrow::Scalar> DecodeStructTagged(
    detail::Reader& r,
    const DecodingMap& map,
    const std::shared_ptr<arrow::DataType>& struct_type);

// Map WireTypeId to a representative arrow::DataType for decoding wire bytes
// when the wire type differs from the reader type (promotion case).
std::shared_ptr<arrow::DataType> WireTypeIdToDecodeType(WireTypeId id) {
    switch (id) {
        case WireTypeId::BOOL:       return arrow::boolean();
        case WireTypeId::INT8:       return arrow::int8();
        case WireTypeId::INT16:      return arrow::int16();
        case WireTypeId::INT32:      return arrow::int32();
        case WireTypeId::INT64:      return arrow::int64();
        case WireTypeId::UINT8:      return arrow::uint8();
        case WireTypeId::UINT16:     return arrow::uint16();
        case WireTypeId::UINT32:     return arrow::uint32();
        case WireTypeId::UINT64:     return arrow::uint64();
        case WireTypeId::FLOAT32:    return arrow::float32();
        case WireTypeId::FLOAT64:    return arrow::float64();
        case WireTypeId::STRING:     return arrow::utf8();
        case WireTypeId::BINARY:     return arrow::binary();
        case WireTypeId::LARGE_STRING: return arrow::large_utf8();
        case WireTypeId::LARGE_BINARY: return arrow::large_binary();
        default:
            throw std::invalid_argument(
                "WireTypeIdToDecodeType: unsupported wire type for promotion");
    }
}

// Promote a decoded scalar from wire type to reader type.
std::shared_ptr<arrow::Scalar> PromoteScalar(
    const std::shared_ptr<arrow::Scalar>& scalar,
    PromotionKind promotion,
    const std::shared_ptr<arrow::DataType>& target_type) {

    if (promotion == PromotionKind::IDENTITY)
        return scalar;

    using T = arrow::Type;

    switch (promotion) {
    case PromotionKind::WIDEN_INT: {
        switch (target_type->id()) {
            case T::INT16:
                if (scalar->type->id() == T::INT8)
                    return std::make_shared<arrow::Int16Scalar>(
                        static_cast<const arrow::Int8Scalar&>(*scalar).value);
                break;
            case T::INT32:
                if (scalar->type->id() == T::INT8)
                    return std::make_shared<arrow::Int32Scalar>(
                        static_cast<int32_t>(static_cast<const arrow::Int8Scalar&>(*scalar).value));
                if (scalar->type->id() == T::INT16)
                    return std::make_shared<arrow::Int32Scalar>(
                        static_cast<int32_t>(static_cast<const arrow::Int16Scalar&>(*scalar).value));
                break;
            case T::INT64:
                if (scalar->type->id() == T::INT8)
                    return std::make_shared<arrow::Int64Scalar>(
                        static_cast<int64_t>(static_cast<const arrow::Int8Scalar&>(*scalar).value));
                if (scalar->type->id() == T::INT16)
                    return std::make_shared<arrow::Int64Scalar>(
                        static_cast<int64_t>(static_cast<const arrow::Int16Scalar&>(*scalar).value));
                if (scalar->type->id() == T::INT32)
                    return std::make_shared<arrow::Int64Scalar>(
                        static_cast<int64_t>(static_cast<const arrow::Int32Scalar&>(*scalar).value));
                break;
            case T::UINT16:
                if (scalar->type->id() == T::UINT8)
                    return std::make_shared<arrow::UInt16Scalar>(
                        static_cast<const arrow::UInt8Scalar&>(*scalar).value);
                break;
            case T::UINT32:
                if (scalar->type->id() == T::UINT8)
                    return std::make_shared<arrow::UInt32Scalar>(
                        static_cast<uint32_t>(static_cast<const arrow::UInt8Scalar&>(*scalar).value));
                if (scalar->type->id() == T::UINT16)
                    return std::make_shared<arrow::UInt32Scalar>(
                        static_cast<uint32_t>(static_cast<const arrow::UInt16Scalar&>(*scalar).value));
                break;
            case T::UINT64:
                if (scalar->type->id() == T::UINT8)
                    return std::make_shared<arrow::UInt64Scalar>(
                        static_cast<uint64_t>(static_cast<const arrow::UInt8Scalar&>(*scalar).value));
                if (scalar->type->id() == T::UINT16)
                    return std::make_shared<arrow::UInt64Scalar>(
                        static_cast<uint64_t>(static_cast<const arrow::UInt16Scalar&>(*scalar).value));
                if (scalar->type->id() == T::UINT32)
                    return std::make_shared<arrow::UInt64Scalar>(
                        static_cast<uint64_t>(static_cast<const arrow::UInt32Scalar&>(*scalar).value));
                break;
            case T::DECIMAL256: {
                const auto& d128 = static_cast<const arrow::Decimal128Scalar&>(*scalar);
                return std::make_shared<arrow::Decimal256Scalar>(
                    arrow::Decimal256(d128.value), target_type);
            }
            default: break;
        }
        break;
    }
    case PromotionKind::WIDEN_FLOAT: {
        float v = static_cast<const arrow::FloatScalar&>(*scalar).value;
        return std::make_shared<arrow::DoubleScalar>(static_cast<double>(v));
    }
    case PromotionKind::INT_TO_FLOAT: {
        int32_t v = static_cast<const arrow::Int32Scalar&>(*scalar).value;
        return std::make_shared<arrow::DoubleScalar>(static_cast<double>(v));
    }
    default: break;
    }

    throw std::invalid_argument(
        "PromoteScalar: unhandled promotion from " + scalar->type->ToString() +
        " to " + target_type->ToString());
}

std::shared_ptr<arrow::Scalar> DecodeStructTagged(
    detail::Reader& r,
    const DecodingMap& map,
    const std::shared_ptr<arrow::DataType>& struct_type) {

    const auto& stype = static_cast<const arrow::StructType&>(*struct_type);
    const int num_reader_fields = stype.num_fields();

    // Pre-fill with nulls.
    arrow::ScalarVector children(num_reader_fields);
    for (int i = 0; i < num_reader_fields; ++i)
        children[i] = arrow::MakeNullScalar(stype.field(i)->type());

    uint16_t wire_field_count = r.Read<uint16_t>();

    for (uint16_t wi = 0; wi < wire_field_count; ++wi) {
        /*uint32_t field_num =*/ r.Read<uint32_t>();
        auto wire_tid = static_cast<WireTypeId>(r.Read<uint8_t>());
        uint8_t null_flag = r.Read<uint8_t>();

        if (wi >= map.wire_to_reader.size())
            throw std::invalid_argument("DecodeStructTagged: wire field index out of range");

        const auto& slot = map.wire_to_reader[wi];

        if (null_flag == 0x01u)
            continue;

        uint32_t data_len = r.Read<uint32_t>();
        size_t payload_start = r.pos;

        if (slot.reader_index < 0) {
            r.pos = payload_start + data_len;
            continue;
        }

        if (slot.sub_map) {
            auto child = DecodeStructTagged(r, *slot.sub_map, slot.reader_type);
            children[slot.reader_index] = PromoteScalar(child, slot.promotion, slot.reader_type);
        } else {
            // Determine decode type: for promotions, decode as the wire type first.
            std::shared_ptr<arrow::DataType> decode_type;
            if (slot.promotion == PromotionKind::IDENTITY) {
                decode_type = slot.reader_type;
            } else {
                decode_type = WireTypeIdToDecodeType(wire_tid);
            }

            detail::Reader sub{r.data + payload_start, data_len};
            auto decoded = detail::DecodeScalarFromReader(sub, decode_type);
            children[slot.reader_index] = PromoteScalar(decoded, slot.promotion, slot.reader_type);
        }

        r.pos = payload_start + data_len;
    }

    return std::make_shared<arrow::StructScalar>(std::move(children), struct_type);
}

// ---------------------------------------------------------------------------
// FNV-1a hasher
// ---------------------------------------------------------------------------

uint64_t Fnv1a64(const std::string& s) {
    uint64_t hash = 14695981039346656037ULL;
    for (unsigned char c : s) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    return hash;
}

}  // namespace

// ---------------------------------------------------------------------------
// detail:: scalar encode/decode (raw payload bytes — unchanged from v1)
// ---------------------------------------------------------------------------

namespace detail {

void EncodeScalar(std::vector<uint8_t>& buf, const arrow::Scalar& scalar) {
    using T = arrow::Type;

    switch (scalar.type->id()) {
        case T::BOOL: {
            auto v = static_cast<const arrow::BooleanScalar&>(scalar).value;
            AppendFixed<uint8_t>(buf, v ? 1u : 0u);
            break;
        }
        case T::INT8:
            AppendFixed(buf, static_cast<const arrow::Int8Scalar&>(scalar).value);
            break;
        case T::INT16:
            AppendFixed(buf, static_cast<const arrow::Int16Scalar&>(scalar).value);
            break;
        case T::INT32:
            AppendFixed(buf, static_cast<const arrow::Int32Scalar&>(scalar).value);
            break;
        case T::INT64:
            AppendFixed(buf, static_cast<const arrow::Int64Scalar&>(scalar).value);
            break;
        case T::UINT8:
            AppendFixed(buf, static_cast<const arrow::UInt8Scalar&>(scalar).value);
            break;
        case T::UINT16:
            AppendFixed(buf, static_cast<const arrow::UInt16Scalar&>(scalar).value);
            break;
        case T::UINT32:
            AppendFixed(buf, static_cast<const arrow::UInt32Scalar&>(scalar).value);
            break;
        case T::UINT64:
            AppendFixed(buf, static_cast<const arrow::UInt64Scalar&>(scalar).value);
            break;
        case T::FLOAT:
            AppendFixed(buf, static_cast<const arrow::FloatScalar&>(scalar).value);
            break;
        case T::DOUBLE:
            AppendFixed(buf, static_cast<const arrow::DoubleScalar&>(scalar).value);
            break;

        case T::STRING:
        case T::LARGE_STRING:
        case T::BINARY:
        case T::LARGE_BINARY:
        case T::STRING_VIEW:
        case T::BINARY_VIEW: {
            const auto&   s       = static_cast<const arrow::BaseBinaryScalar&>(scalar);
            const int64_t raw_len = s.value->size();
            if (raw_len < 0 || raw_len > static_cast<int64_t>(std::numeric_limits<uint32_t>::max()))
                throw std::invalid_argument(
                    "EncodeRow: variable-length field exceeds 4 GiB limit");
            AppendVariableLength(
                buf,
                reinterpret_cast<const uint8_t*>(s.value->data()),
                static_cast<uint32_t>(raw_len));
            break;
        }

        case T::DATE32:
            AppendFixed(buf, static_cast<const arrow::Date32Scalar&>(scalar).value);
            break;
        case T::DATE64:
            AppendFixed(buf, static_cast<const arrow::Date64Scalar&>(scalar).value);
            break;
        case T::TIMESTAMP:
            AppendFixed(buf, static_cast<const arrow::TimestampScalar&>(scalar).value);
            break;
        case T::TIME32:
            AppendFixed(buf, static_cast<const arrow::Time32Scalar&>(scalar).value);
            break;
        case T::TIME64:
            AppendFixed(buf, static_cast<const arrow::Time64Scalar&>(scalar).value);
            break;
        case T::DURATION:
            AppendFixed(buf, static_cast<const arrow::DurationScalar&>(scalar).value);
            break;
        case T::FIXED_SIZE_BINARY: {
            const auto&   s          = static_cast<const arrow::FixedSizeBinaryScalar&>(scalar);
            const int32_t byte_width =
                static_cast<const arrow::FixedSizeBinaryType&>(*scalar.type).byte_width();
            const auto* data = reinterpret_cast<const uint8_t*>(s.value->data());
            buf.insert(buf.end(), data, data + byte_width);
            break;
        }
        case T::HALF_FLOAT:
            AppendFixed(buf, static_cast<const arrow::HalfFloatScalar&>(scalar).value);
            break;
        case T::INTERVAL_MONTHS:
            AppendFixed(buf, static_cast<const arrow::MonthIntervalScalar&>(scalar).value);
            break;
        case T::INTERVAL_DAY_TIME: {
            const auto& v = static_cast<const arrow::DayTimeIntervalScalar&>(scalar).value;
            AppendFixed(buf, v.days);
            AppendFixed(buf, v.milliseconds);
            break;
        }
        case T::INTERVAL_MONTH_DAY_NANO: {
            const auto& v = static_cast<const arrow::MonthDayNanoIntervalScalar&>(scalar).value;
            AppendFixed(buf, v.months);
            AppendFixed(buf, v.days);
            AppendFixed(buf, v.nanoseconds);
            break;
        }
        case T::DECIMAL128: {
            const auto& v = static_cast<const arrow::Decimal128Scalar&>(scalar).value;
            uint8_t bytes[16];
            v.ToBytes(bytes);
            buf.insert(buf.end(), bytes, bytes + 16);
            break;
        }
        case T::DECIMAL256: {
            const auto& v = static_cast<const arrow::Decimal256Scalar&>(scalar).value;
            uint8_t bytes[32];
            v.ToBytes(bytes);
            buf.insert(buf.end(), bytes, bytes + 32);
            break;
        }

        case T::DICTIONARY:
            throw std::invalid_argument(
                "EncodeRow: DICTIONARY type is not supported in the per-row format; "
                "use a non-dictionary schema field instead");

        default:
            throw std::invalid_argument(
                "EncodeRow: unsupported Arrow type: " + scalar.type->ToString());
    }
}

// Decode a single scalar from a Reader, advancing the reader position.
// Handles all scalar types.  Composite types (list, map, struct, union)
// use the raw (non-tagged) element encoding for list/map elements.
std::shared_ptr<arrow::Scalar> DecodeScalarFromReader(
    Reader& r,
    const std::shared_ptr<arrow::DataType>& type) {

    using T = arrow::Type;
    switch (type->id()) {
        case T::BOOL:
            return std::make_shared<arrow::BooleanScalar>(r.Read<uint8_t>() != 0);
        case T::INT8:
            return std::make_shared<arrow::Int8Scalar>(r.Read<int8_t>());
        case T::INT16:
            return std::make_shared<arrow::Int16Scalar>(r.Read<int16_t>());
        case T::INT32:
            return std::make_shared<arrow::Int32Scalar>(r.Read<int32_t>());
        case T::INT64:
            return std::make_shared<arrow::Int64Scalar>(r.Read<int64_t>());
        case T::UINT8:
            return std::make_shared<arrow::UInt8Scalar>(r.Read<uint8_t>());
        case T::UINT16:
            return std::make_shared<arrow::UInt16Scalar>(r.Read<uint16_t>());
        case T::UINT32:
            return std::make_shared<arrow::UInt32Scalar>(r.Read<uint32_t>());
        case T::UINT64:
            return std::make_shared<arrow::UInt64Scalar>(r.Read<uint64_t>());
        case T::FLOAT:
            return std::make_shared<arrow::FloatScalar>(r.Read<float>());
        case T::DOUBLE:
            return std::make_shared<arrow::DoubleScalar>(r.Read<double>());

        case T::STRING:
        case T::LARGE_STRING:
        case T::BINARY:
        case T::LARGE_BINARY:
        case T::STRING_VIEW:
        case T::BINARY_VIEW: {
            uint32_t       len  = r.Read<uint32_t>();
            const uint8_t* ptr  = r.ReadBytes(len);
            auto           ibuf = std::make_shared<arrow::Buffer>(ptr, len);
            switch (type->id()) {
                case T::STRING:       return std::make_shared<arrow::StringScalar>(ibuf);
                case T::LARGE_STRING: return std::make_shared<arrow::LargeStringScalar>(ibuf);
                case T::BINARY:       return std::make_shared<arrow::BinaryScalar>(ibuf);
                case T::LARGE_BINARY: return std::make_shared<arrow::LargeBinaryScalar>(ibuf);
                case T::STRING_VIEW:  return std::make_shared<arrow::StringViewScalar>(ibuf);
                case T::BINARY_VIEW:  return std::make_shared<arrow::BinaryViewScalar>(ibuf);
                default:              break;
            }
            break;
        }

        case T::DATE32:
            return std::make_shared<arrow::Date32Scalar>(r.Read<int32_t>());
        case T::DATE64:
            return std::make_shared<arrow::Date64Scalar>(r.Read<int64_t>());
        case T::TIMESTAMP: {
            int64_t v = r.Read<int64_t>();
            return std::make_shared<arrow::TimestampScalar>(v, type);
        }
        case T::TIME32: {
            int32_t v = r.Read<int32_t>();
            return std::make_shared<arrow::Time32Scalar>(v, type);
        }
        case T::TIME64: {
            int64_t v = r.Read<int64_t>();
            return std::make_shared<arrow::Time64Scalar>(v, type);
        }
        case T::DURATION: {
            int64_t v = r.Read<int64_t>();
            return std::make_shared<arrow::DurationScalar>(v, type);
        }
        case T::FIXED_SIZE_BINARY: {
            const int32_t byte_width =
                static_cast<const arrow::FixedSizeBinaryType&>(*type).byte_width();
            const uint8_t* ptr  = r.ReadBytes(byte_width);
            auto           ibuf = std::make_shared<arrow::Buffer>(ptr, byte_width);
            return std::make_shared<arrow::FixedSizeBinaryScalar>(ibuf, type);
        }
        case T::HALF_FLOAT:
            return std::make_shared<arrow::HalfFloatScalar>(r.Read<uint16_t>());
        case T::INTERVAL_MONTHS:
            return std::make_shared<arrow::MonthIntervalScalar>(r.Read<int32_t>());
        case T::INTERVAL_DAY_TIME: {
            arrow::DayTimeIntervalType::DayMilliseconds v;
            v.days         = r.Read<int32_t>();
            v.milliseconds = r.Read<int32_t>();
            return std::make_shared<arrow::DayTimeIntervalScalar>(v);
        }
        case T::INTERVAL_MONTH_DAY_NANO: {
            arrow::MonthDayNanoIntervalType::MonthDayNanos v;
            v.months      = r.Read<int32_t>();
            v.days        = r.Read<int32_t>();
            v.nanoseconds = r.Read<int64_t>();
            return std::make_shared<arrow::MonthDayNanoIntervalScalar>(v);
        }
        case T::DECIMAL128: {
            const uint8_t* ptr = r.ReadBytes(16);
            return std::make_shared<arrow::Decimal128Scalar>(
                arrow::Decimal128(ptr), type);
        }
        case T::DECIMAL256: {
            const uint8_t* ptr = r.ReadBytes(32);
            return std::make_shared<arrow::Decimal256Scalar>(
                arrow::Decimal256(ptr), type);
        }
        case T::STRUCT: {
            // Decode a struct from the recursive tagged format.
            // Reads field_count + per-field tagged entries and matches them
            // positionally to the struct type's child fields.
            const auto& stype = static_cast<const arrow::StructType&>(*type);
            const int num_fields = stype.num_fields();
            arrow::ScalarVector children(num_fields);
            for (int i = 0; i < num_fields; ++i)
                children[i] = arrow::MakeNullScalar(stype.field(i)->type());

            uint16_t wire_field_count = r.Read<uint16_t>();
            for (uint16_t wi = 0; wi < wire_field_count; ++wi) {
                /*uint32_t field_num =*/ r.Read<uint32_t>();
                /*WireTypeId wire_tid =*/ r.Read<uint8_t>();
                uint8_t null_flag = r.Read<uint8_t>();
                if (null_flag == 0x01u) continue;
                uint32_t data_len = r.Read<uint32_t>();
                size_t payload_start = r.pos;
                if (wi < static_cast<uint16_t>(num_fields)) {
                    Reader sub{r.data + payload_start, data_len};
                    children[wi] = DecodeScalarFromReader(sub, stype.field(wi)->type());
                }
                r.pos = payload_start + data_len;
            }
            return std::make_shared<arrow::StructScalar>(std::move(children), type);
        }
        case T::LIST:
        case T::LARGE_LIST: {
            const auto& list_type = static_cast<const arrow::BaseListType&>(*type);
            const auto  elem_type = list_type.value_type();
            const uint32_t count  = r.Read<uint32_t>();
            auto builder          = arrow::MakeBuilder(elem_type).ValueOrDie();
            for (uint32_t i = 0; i < count; ++i) {
                const uint8_t null_flag = r.Read<uint8_t>();
                arrow::Status st = (null_flag == 0x01u)
                    ? builder->AppendNull()
                    : builder->AppendScalar(*DecodeScalarFromReader(r, elem_type));
                if (!st.ok())
                    throw std::invalid_argument(
                        "DecodeRow: builder append failed: " + st.ToString());
            }
            auto arr = builder->Finish().ValueOrDie();
            // Use the caller-supplied type so that nested list scalars
            // preserve field nullability and match the parent builder's
            // expected type during AppendScalar.
            if (type->id() == arrow::Type::LIST) {
                return std::make_shared<arrow::ListScalar>(arr, type);
            }
            {
                return std::make_shared<arrow::LargeListScalar>(arr, type);
            }
        }
        case T::FIXED_SIZE_LIST: {
            const auto& fsl_type  = static_cast<const arrow::FixedSizeListType&>(*type);
            const auto  elem_type = fsl_type.value_type();
            const int32_t count   = fsl_type.list_size();
            auto builder          = arrow::MakeBuilder(elem_type).ValueOrDie();
            for (int32_t i = 0; i < count; ++i) {
                const uint8_t null_flag = r.Read<uint8_t>();
                arrow::Status st = (null_flag == 0x01u)
                    ? builder->AppendNull()
                    : builder->AppendScalar(*DecodeScalarFromReader(r, elem_type));
                if (!st.ok())
                    throw std::invalid_argument(
                        "DecodeRow: builder append failed: " + st.ToString());
            }
            auto arr = builder->Finish().ValueOrDie();
            auto actual_type = arrow::fixed_size_list(arr->type(), count);
            return std::make_shared<arrow::FixedSizeListScalar>(arr, actual_type);
        }
        case T::SPARSE_UNION: {
            const auto& union_type = static_cast<const arrow::SparseUnionType&>(*type);
            const int8_t type_code = r.Read<int8_t>();
            const auto& codes = union_type.type_codes();
            int child_id = -1;
            for (int i = 0; i < static_cast<int>(codes.size()); ++i) {
                if (codes[i] == type_code) { child_id = i; break; }
            }
            if (child_id < 0)
                throw std::invalid_argument(
                    "DecodeRow: unknown sparse union type_code " + std::to_string(type_code));
            auto active = DecodeScalarFromReader(r, union_type.field(child_id)->type());
            arrow::ScalarVector children;
            children.reserve(union_type.num_fields());
            for (int i = 0; i < union_type.num_fields(); ++i) {
                children.push_back(i == child_id
                    ? active
                    : arrow::MakeNullScalar(union_type.field(i)->type()));
            }
            return std::make_shared<arrow::SparseUnionScalar>(
                std::move(children), type_code, type);
        }
        case T::DENSE_UNION: {
            const auto& union_type = static_cast<const arrow::DenseUnionType&>(*type);
            const int8_t type_code = r.Read<int8_t>();
            const auto& codes = union_type.type_codes();
            int child_id = -1;
            for (int i = 0; i < static_cast<int>(codes.size()); ++i) {
                if (codes[i] == type_code) { child_id = i; break; }
            }
            if (child_id < 0)
                throw std::invalid_argument(
                    "DecodeRow: unknown dense union type_code " + std::to_string(type_code));
            auto value = DecodeScalarFromReader(r, union_type.field(child_id)->type());
            return std::make_shared<arrow::DenseUnionScalar>(value, type_code, type);
        }
        case T::MAP: {
            const auto& map_type = static_cast<const arrow::MapType&>(*type);
            const uint32_t count = r.Read<uint32_t>();
            auto key_builder = arrow::MakeBuilder(map_type.key_type()).ValueOrDie();
            auto val_builder = arrow::MakeBuilder(map_type.item_type()).ValueOrDie();
            for (uint32_t i = 0; i < count; ++i) {
                auto key = DecodeScalarFromReader(r, map_type.key_type());
                auto st = key_builder->AppendScalar(*key);
                if (!st.ok())
                    throw std::invalid_argument(
                        "DecodeRow: key append failed: " + st.ToString());
                const uint8_t null_flag = r.Read<uint8_t>();
                st = (null_flag == 0x01u)
                    ? val_builder->AppendNull()
                    : val_builder->AppendScalar(*DecodeScalarFromReader(r, map_type.item_type()));
                if (!st.ok())
                    throw std::invalid_argument(
                        "DecodeRow: value append failed: " + st.ToString());
            }
            auto key_arr = key_builder->Finish().ValueOrDie();
            auto val_arr = val_builder->Finish().ValueOrDie();
            // Build the entries struct type from the actual array types to
            // avoid nullability mismatches with the schema's map type.
            auto entries_type = arrow::struct_({
                arrow::field("key", key_arr->type(), false),
                arrow::field("value", val_arr->type())});
            auto entries = std::make_shared<arrow::StructArray>(
                entries_type,
                static_cast<int64_t>(count),
                arrow::ArrayVector{key_arr, val_arr});
            auto actual_map_type = arrow::map(key_arr->type(), val_arr->type());
            return std::make_shared<arrow::MapScalar>(entries, actual_map_type);
        }

        case T::DICTIONARY:
            throw std::invalid_argument(
                "DecodeRow: DICTIONARY type is not supported in the per-row format; "
                "use a non-dictionary schema field instead");

        default:
            throw std::invalid_argument(
                "DecodeRow: unsupported Arrow type: " + type->ToString());
    }
    throw std::invalid_argument("DecodeRow: unsupported Arrow type: " + type->ToString());
}

// Convenience wrapper.
std::shared_ptr<arrow::Scalar> DecodeScalar(
    const uint8_t*                          data,
    size_t                                  size,
    const std::shared_ptr<arrow::DataType>& type) {
    Reader r{data, size};
    return DecodeScalarFromReader(r, type);
}

}  // namespace detail

// ---------------------------------------------------------------------------
// RowCodec
// ---------------------------------------------------------------------------

RowCodec::RowCodec(std::shared_ptr<arrow::Schema> schema)
    : schema_(std::move(schema))
    , schema_hash_(FingerprintHash(*schema_))
    , cache_mutex_(std::make_unique<std::mutex>()) {}

EncodedRow RowCodec::EncodeRow(const ArrowRow& values) const {

    const int num_fields = schema_->num_fields();

    if (static_cast<int>(values.size()) != num_fields)
        throw std::invalid_argument(
            "EncodeRow: values.size() (" + std::to_string(values.size()) +
            ") does not match schema.num_fields() (" + std::to_string(num_fields) + ")");

    std::vector<uint8_t> buf;
    buf.reserve(128);

    // Header: schema_hash (8 bytes) + field_count (2 bytes).
    AppendFixed(buf, schema_hash_);
    AppendFixed(buf, static_cast<uint16_t>(num_fields));

    for (int i = 0; i < num_fields; ++i) {
        const auto& field  = schema_->field(i);
        const auto& scalar = values[i];
        const bool  is_null = !scalar || !scalar->is_valid;

        uint32_t field_num = static_cast<uint32_t>(GetFieldNumber(*field, i + 1));
        WireTypeId wire_type = ArrowTypeToWireTypeId(*field->type());

        AppendFixed(buf, field_num);
        buf.push_back(static_cast<uint8_t>(wire_type));

        if (is_null) {
            buf.push_back(0x01u);
            continue;
        }

        // Compare top-level type ID only.  Nested nullability (e.g. list
        // item nullable vs non-nullable) may differ between the generated
        // code and the declared schema without affecting wire encoding.
        if (scalar->type->id() != field->type()->id())
            throw std::invalid_argument(
                "EncodeRow: type mismatch for field '" + field->name() +
                "': schema expects " + field->type()->ToString() +
                ", got " + scalar->type->ToString());

        buf.push_back(0x00u);

        // Encode payload to temp buffer to measure data_len.
        std::vector<uint8_t> payload;
        payload.reserve(32);
        EncodePayload(payload, *scalar);

        AppendFixed(buf, static_cast<uint32_t>(payload.size()));
        buf.insert(buf.end(), payload.begin(), payload.end());
    }

    return buf;
}

ArrowRow RowCodec::DecodeRow(const EncodedRow& buf) const {

    detail::Reader r{buf.data(), buf.size()};

    uint64_t writer_hash = r.Read<uint64_t>();
    uint16_t field_count = r.Read<uint16_t>();

    // Get or build the DecodingMap for this writer hash.
    const DecodingMap* map_ptr = nullptr;
    {
        std::lock_guard<std::mutex> lock(*cache_mutex_);
        auto it = decoding_cache_.find(writer_hash);
        if (it == decoding_cache_.end()) {
            // BuildDecodingMap expects data starting at FIELD_COUNT.
            // r.pos is currently after hash(8) + field_count(2) = 10.
            // Rewind to include the field_count in the slice.
            auto new_map = BuildDecodingMap(
                *schema_,
                buf.data() + 8,
                buf.size() - 8);
            new_map.writer_hash = writer_hash;
            auto [ins_it, _] = decoding_cache_.emplace(writer_hash, std::move(new_map));
            map_ptr = &ins_it->second;
        } else {
            map_ptr = &it->second;
        }
    }

    const auto& map = *map_ptr;
    const int num_reader_fields = schema_->num_fields();

    // Pre-allocate output filled with nulls.
    ArrowRow values(num_reader_fields);
    for (int i = 0; i < num_reader_fields; ++i)
        values[i] = arrow::MakeNullScalar(schema_->field(i)->type());

    // Decode each wire field using the cached map.
    for (uint16_t wi = 0; wi < field_count; ++wi) {
        /*uint32_t field_num =*/ r.Read<uint32_t>();
        auto wire_tid = static_cast<WireTypeId>(r.Read<uint8_t>());
        uint8_t null_flag = r.Read<uint8_t>();

        const auto& slot = map.wire_to_reader[wi];

        if (null_flag == 0x01u)
            continue;

        uint32_t data_len = r.Read<uint32_t>();
        size_t payload_start = r.pos;

        if (slot.reader_index < 0) {
            r.pos = payload_start + data_len;
            continue;
        }

        if (slot.sub_map) {
            auto child = DecodeStructTagged(r, *slot.sub_map, slot.reader_type);
            values[slot.reader_index] = PromoteScalar(child, slot.promotion, slot.reader_type);
        } else {
            // For type promotions, decode as the wire type first, then promote.
            std::shared_ptr<arrow::DataType> decode_type;
            if (slot.promotion == PromotionKind::IDENTITY) {
                decode_type = slot.reader_type;
            } else {
                decode_type = WireTypeIdToDecodeType(wire_tid);
            }

            detail::Reader sub{r.data + payload_start, data_len};
            auto decoded = detail::DecodeScalarFromReader(sub, decode_type);
            values[slot.reader_index] = PromoteScalar(decoded, slot.promotion, slot.reader_type);
        }

        r.pos = payload_start + data_len;
    }

    return values;
}

// ---------------------------------------------------------------------------
// FingerprintHash
// ---------------------------------------------------------------------------

uint64_t FingerprintHash(const arrow::Schema& schema) {
    const std::string fp = schema.fingerprint();
    if (fp.empty())
        throw std::invalid_argument(
            "FingerprintHash: schema contains types that cannot be fingerprinted");
    return Fnv1a64(fp);
}

}  // namespace fletcher
