#ifndef FLETCHER_INCLUDE_SCHEMA_EVOLUTION_HPP_
#define FLETCHER_INCLUDE_SCHEMA_EVOLUTION_HPP_

#include <arrow/api.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace fletcher {

// -----------------------------------------------------------------------
// Wire type identifiers — stable constants embedded in the tagged format.
// -----------------------------------------------------------------------

enum class WireTypeId : uint8_t {
    UNKNOWN           = 0x00,

    BOOL              = 0x01,
    INT8              = 0x02,
    INT16             = 0x03,
    INT32             = 0x04,
    INT64             = 0x05,
    UINT8             = 0x06,
    UINT16            = 0x07,
    UINT32            = 0x08,
    UINT64            = 0x09,
    FLOAT32           = 0x0A,
    FLOAT64           = 0x0B,
    STRING            = 0x0C,
    BINARY            = 0x0D,
    DATE32            = 0x0E,
    DATE64            = 0x0F,
    TIMESTAMP_NANO    = 0x10,
    TIME32            = 0x11,
    TIME64            = 0x12,
    DURATION_NANO     = 0x13,
    FIXED_SIZE_BINARY = 0x14,
    HALF_FLOAT        = 0x15,
    DECIMAL128        = 0x16,
    DECIMAL256        = 0x17,

    LARGE_STRING      = 0x18,
    LARGE_BINARY      = 0x19,
    STRING_VIEW       = 0x1A,
    BINARY_VIEW       = 0x1B,

    INTERVAL_MONTHS        = 0x1C,
    INTERVAL_DAY_TIME      = 0x1D,
    INTERVAL_MONTH_DAY_NANO = 0x1E,

    STRUCT            = 0x20,
    LIST              = 0x21,
    LARGE_LIST        = 0x22,
    FIXED_SIZE_LIST   = 0x23,
    MAP               = 0x24,
    SPARSE_UNION      = 0x25,
    DENSE_UNION       = 0x26,
};

// Convert an arrow::DataType to its wire type identifier.
WireTypeId ArrowTypeToWireTypeId(const arrow::DataType& type);

// -----------------------------------------------------------------------
// Type promotion classification (Iceberg-compliant rules)
// -----------------------------------------------------------------------

enum class PromotionKind : uint8_t {
    IDENTITY,       // Same type — no conversion needed
    WIDEN_INT,      // e.g. int32 → int64, uint16 → uint32
    WIDEN_FLOAT,    // float32 → float64
    INT_TO_FLOAT,   // int32 → float64
    ILLEGAL,        // Incompatible types — cannot promote
};

// Determine whether a wire type can be promoted to a reader type.
PromotionKind ClassifyPromotion(WireTypeId wire_type, WireTypeId reader_type);

// -----------------------------------------------------------------------
// Decoding map — cached mapping from a writer schema to a reader schema
// -----------------------------------------------------------------------

struct DecodingMap;  // forward declaration for sub_map pointer

struct FieldSlot {
    // Index in the reader's ArrowRow output.  -1 means "skip this wire field".
    int reader_index = -1;

    // The Arrow type the reader expects for this slot.
    std::shared_ptr<arrow::DataType> reader_type;

    // What kind of type promotion to apply when decoding.
    PromotionKind promotion = PromotionKind::IDENTITY;

    // For struct fields: a sub-map for recursively matching children.
    // nullptr for non-struct fields.
    std::shared_ptr<DecodingMap> sub_map;
};

struct DecodingMap {
    // Schema hash of the writer that produced the wire data.
    uint64_t writer_hash = 0;

    // Indexed by wire field ordinal position (0..N-1 as they appear in the
    // wire buffer).  Each entry describes where that wire field maps in the
    // reader's output.
    std::vector<FieldSlot> wire_to_reader;

    // Reader field indices that have no corresponding wire field.
    // These must be filled with null in the output row.
    std::vector<int> missing_reader_fields;

    // Total number of fields in the reader schema / struct type.
    int reader_field_count = 0;
};

// Build a DecodingMap by scanning the tagged field headers in a wire buffer
// and matching field numbers against the reader schema's "field_number"
// metadata.
//
// reader_schema: the subscriber's current schema.
// wire_data:     pointer to the start of the field sequence (after the
//                8-byte schema_hash + 2-byte field_count header).
// wire_size:     number of bytes from wire_data to the end of the buffer.
//
// For struct fields where both writer and reader agree on STRUCT type, the
// function recurses into the struct payload to build a sub-DecodingMap.
//
// Throws std::invalid_argument if an illegal type promotion is detected.
DecodingMap BuildDecodingMap(
    const arrow::Schema& reader_schema,
    const uint8_t* wire_data,
    size_t wire_size);

// Overload for building a sub-map inside a struct.
DecodingMap BuildDecodingMapForStruct(
    const arrow::StructType& reader_struct,
    const uint8_t* wire_data,
    size_t wire_size);

}  // namespace fletcher

#endif  // FLETCHER_INCLUDE_SCHEMA_EVOLUTION_HPP_
