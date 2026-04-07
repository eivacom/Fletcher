#include "schema_evolution.hpp"
#include "row_reader.hpp"

#include <arrow/type.h>

#include <stdexcept>
#include <string>
#include <unordered_map>

namespace fletcher {

// -----------------------------------------------------------------------
// ArrowTypeToWireTypeId
// -----------------------------------------------------------------------

WireTypeId ArrowTypeToWireTypeId(const arrow::DataType& type) {
    using T = arrow::Type;
    switch (type.id()) {
        case T::BOOL:                    return WireTypeId::BOOL;
        case T::INT8:                    return WireTypeId::INT8;
        case T::INT16:                   return WireTypeId::INT16;
        case T::INT32:                   return WireTypeId::INT32;
        case T::INT64:                   return WireTypeId::INT64;
        case T::UINT8:                   return WireTypeId::UINT8;
        case T::UINT16:                  return WireTypeId::UINT16;
        case T::UINT32:                  return WireTypeId::UINT32;
        case T::UINT64:                  return WireTypeId::UINT64;
        case T::FLOAT:                   return WireTypeId::FLOAT32;
        case T::DOUBLE:                  return WireTypeId::FLOAT64;
        case T::STRING:                  return WireTypeId::STRING;
        case T::LARGE_STRING:            return WireTypeId::LARGE_STRING;
        case T::BINARY:                  return WireTypeId::BINARY;
        case T::LARGE_BINARY:            return WireTypeId::LARGE_BINARY;
        case T::STRING_VIEW:             return WireTypeId::STRING_VIEW;
        case T::BINARY_VIEW:             return WireTypeId::BINARY_VIEW;
        case T::DATE32:                  return WireTypeId::DATE32;
        case T::DATE64:                  return WireTypeId::DATE64;
        case T::TIMESTAMP:               return WireTypeId::TIMESTAMP_NANO;
        case T::TIME32:                  return WireTypeId::TIME32;
        case T::TIME64:                  return WireTypeId::TIME64;
        case T::DURATION:                return WireTypeId::DURATION_NANO;
        case T::FIXED_SIZE_BINARY:       return WireTypeId::FIXED_SIZE_BINARY;
        case T::HALF_FLOAT:              return WireTypeId::HALF_FLOAT;
        case T::DECIMAL128:              return WireTypeId::DECIMAL128;
        case T::DECIMAL256:              return WireTypeId::DECIMAL256;
        case T::INTERVAL_MONTHS:         return WireTypeId::INTERVAL_MONTHS;
        case T::INTERVAL_DAY_TIME:       return WireTypeId::INTERVAL_DAY_TIME;
        case T::INTERVAL_MONTH_DAY_NANO: return WireTypeId::INTERVAL_MONTH_DAY_NANO;
        case T::STRUCT:                  return WireTypeId::STRUCT;
        case T::LIST:                    return WireTypeId::LIST;
        case T::LARGE_LIST:              return WireTypeId::LARGE_LIST;
        case T::FIXED_SIZE_LIST:         return WireTypeId::FIXED_SIZE_LIST;
        case T::MAP:                     return WireTypeId::MAP;
        case T::SPARSE_UNION:            return WireTypeId::SPARSE_UNION;
        case T::DENSE_UNION:             return WireTypeId::DENSE_UNION;
        default:
            throw std::invalid_argument(
                "ArrowTypeToWireTypeId: unsupported type: " + type.ToString());
    }
}

// -----------------------------------------------------------------------
// ClassifyPromotion
// -----------------------------------------------------------------------

namespace {

bool IsSignedInt(WireTypeId id) {
    return id == WireTypeId::INT8  || id == WireTypeId::INT16 ||
           id == WireTypeId::INT32 || id == WireTypeId::INT64;
}

bool IsUnsignedInt(WireTypeId id) {
    return id == WireTypeId::UINT8  || id == WireTypeId::UINT16 ||
           id == WireTypeId::UINT32 || id == WireTypeId::UINT64;
}

// Return the "width rank" for ordering: INT8=0, INT16=1, INT32=2, INT64=3.
int IntRank(WireTypeId id) {
    switch (id) {
        case WireTypeId::INT8:   case WireTypeId::UINT8:   return 0;
        case WireTypeId::INT16:  case WireTypeId::UINT16:  return 1;
        case WireTypeId::INT32:  case WireTypeId::UINT32:  return 2;
        case WireTypeId::INT64:  case WireTypeId::UINT64:  return 3;
        default: return -1;
    }
}

}  // namespace

PromotionKind ClassifyPromotion(WireTypeId wire_type, WireTypeId reader_type) {
    if (wire_type == reader_type)
        return PromotionKind::IDENTITY;

    // Signed int widening: int8 → int16/32/64, int16 → int32/64, int32 → int64
    if (IsSignedInt(wire_type) && IsSignedInt(reader_type) &&
        IntRank(wire_type) < IntRank(reader_type))
        return PromotionKind::WIDEN_INT;

    // Unsigned int widening
    if (IsUnsignedInt(wire_type) && IsUnsignedInt(reader_type) &&
        IntRank(wire_type) < IntRank(reader_type))
        return PromotionKind::WIDEN_INT;

    // Float widening: float32 → float64
    if (wire_type == WireTypeId::FLOAT32 && reader_type == WireTypeId::FLOAT64)
        return PromotionKind::WIDEN_FLOAT;

    // Int32 → float64 (safe, no precision loss for int32 range)
    if (wire_type == WireTypeId::INT32 && reader_type == WireTypeId::FLOAT64)
        return PromotionKind::INT_TO_FLOAT;

    // Decimal widening: decimal128 → decimal256
    if (wire_type == WireTypeId::DECIMAL128 && reader_type == WireTypeId::DECIMAL256)
        return PromotionKind::WIDEN_INT;

    // String/binary family: treat as compatible (same payload encoding)
    if ((wire_type == WireTypeId::STRING || wire_type == WireTypeId::LARGE_STRING ||
         wire_type == WireTypeId::STRING_VIEW) &&
        (reader_type == WireTypeId::STRING || reader_type == WireTypeId::LARGE_STRING ||
         reader_type == WireTypeId::STRING_VIEW))
        return PromotionKind::IDENTITY;

    if ((wire_type == WireTypeId::BINARY || wire_type == WireTypeId::LARGE_BINARY ||
         wire_type == WireTypeId::BINARY_VIEW) &&
        (reader_type == WireTypeId::BINARY || reader_type == WireTypeId::LARGE_BINARY ||
         reader_type == WireTypeId::BINARY_VIEW))
        return PromotionKind::IDENTITY;

    return PromotionKind::ILLEGAL;
}

// -----------------------------------------------------------------------
// BuildDecodingMap helpers
// -----------------------------------------------------------------------

namespace {

// Extract the integer "field_number" metadata value from an Arrow field.
// When no metadata is present, returns the positional fallback (1-indexed).
int GetFieldNumber(const arrow::Field& field, int positional_fallback) {
    auto meta = field.metadata();
    if (!meta) return positional_fallback;
    auto idx = meta->FindKey("field_number");
    if (idx < 0) return positional_fallback;
    return std::stoi(meta->value(idx));
}

// Build a lookup from field_number → {reader_index, reader_type} for a list
// of Arrow fields (either top-level schema fields or struct children).
std::unordered_map<uint32_t, std::pair<int, std::shared_ptr<arrow::DataType>>>
BuildReaderLookup(const std::vector<std::shared_ptr<arrow::Field>>& fields) {
    std::unordered_map<uint32_t, std::pair<int, std::shared_ptr<arrow::DataType>>> lookup;
    for (int i = 0; i < static_cast<int>(fields.size()); ++i) {
        int fn = GetFieldNumber(*fields[i], i + 1);
        lookup[static_cast<uint32_t>(fn)] = {i, fields[i]->type()};
    }
    return lookup;
}

// Core map-building logic shared between top-level and struct variants.
DecodingMap BuildMapFromFields(
    const std::vector<std::shared_ptr<arrow::Field>>& reader_fields,
    const uint8_t* wire_data,
    size_t wire_size)
{
    auto lookup = BuildReaderLookup(reader_fields);

    detail::Reader r{wire_data, wire_size};
    uint16_t field_count = r.Read<uint16_t>();

    DecodingMap map;
    map.reader_field_count = static_cast<int>(reader_fields.size());
    map.wire_to_reader.reserve(field_count);

    // Track which reader indices were matched.
    std::vector<bool> matched(reader_fields.size(), false);

    for (uint16_t i = 0; i < field_count; ++i) {
        uint32_t field_num   = r.Read<uint32_t>();
        auto     wire_type   = static_cast<WireTypeId>(r.Read<uint8_t>());
        uint8_t  null_flag   = r.Read<uint8_t>();

        uint32_t data_len = 0;
        if (null_flag == 0x00u)
            data_len = r.Read<uint32_t>();

        // Remember where the payload starts so we can recurse into structs.
        size_t payload_pos = r.pos;

        auto it = lookup.find(field_num);
        if (it == lookup.end()) {
            // Unknown field — skip.
            FieldSlot slot;
            slot.reader_index = -1;
            slot.promotion = PromotionKind::IDENTITY;
            map.wire_to_reader.push_back(std::move(slot));
        } else {
            int reader_idx = it->second.first;
            auto& reader_type = it->second.second;
            WireTypeId reader_wire_type = ArrowTypeToWireTypeId(*reader_type);

            PromotionKind promo = ClassifyPromotion(wire_type, reader_wire_type);
            if (promo == PromotionKind::ILLEGAL)
                throw std::invalid_argument(
                    "Schema evolution: illegal type promotion for field_number " +
                    std::to_string(field_num) + " from wire type 0x" +
                    std::to_string(static_cast<int>(wire_type)) + " to reader type 0x" +
                    std::to_string(static_cast<int>(reader_wire_type)));

            FieldSlot slot;
            slot.reader_index = reader_idx;
            slot.reader_type = reader_type;
            slot.promotion = promo;

            // If both sides are struct, build a sub-map.
            if (wire_type == WireTypeId::STRUCT &&
                reader_wire_type == WireTypeId::STRUCT &&
                null_flag == 0x00u) {
                const auto& struct_type =
                    static_cast<const arrow::StructType&>(*reader_type);
                slot.sub_map = std::make_shared<DecodingMap>(
                    BuildDecodingMapForStruct(
                        struct_type,
                        wire_data + payload_pos,
                        data_len));
            }

            matched[reader_idx] = true;
            map.wire_to_reader.push_back(std::move(slot));
        }

        // Skip past the payload.
        if (null_flag == 0x00u)
            r.pos = payload_pos + data_len;
    }

    // Collect unmatched reader fields.
    for (int i = 0; i < static_cast<int>(reader_fields.size()); ++i) {
        if (!matched[i])
            map.missing_reader_fields.push_back(i);
    }

    return map;
}

}  // namespace

// -----------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------

DecodingMap BuildDecodingMap(
    const arrow::Schema& reader_schema,
    const uint8_t* wire_data,
    size_t wire_size)
{
    return BuildMapFromFields(reader_schema.fields(), wire_data, wire_size);
}

DecodingMap BuildDecodingMapForStruct(
    const arrow::StructType& reader_struct,
    const uint8_t* wire_data,
    size_t wire_size)
{
    return BuildMapFromFields(reader_struct.fields(), wire_data, wire_size);
}

}  // namespace fletcher
