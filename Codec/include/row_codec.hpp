#ifndef FLETCHER_INCLUDE_ROW_CODEC_HPP_
#define FLETCHER_INCLUDE_ROW_CODEC_HPP_

#include <arrow/api.h>
#include <pubsub/envelope.hpp>

#include "schema_evolution.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace fletcher {

using ArrowRow = std::vector<std::shared_ptr<arrow::Scalar>>;

// Tagged row buffer format (little-endian throughout):
//
//   [SCHEMA_HASH  : 8 bytes ] FNV-1a 64-bit hash of the schema fingerprint
//   [FIELD_COUNT  : 2 bytes ] uint16_t — number of top-level fields
//
//   For each field (in schema order):
//     [FIELD_NUM    : 4 bytes ] uint32_t — proto field number (local to message)
//     [WIRE_TYPE_ID : 1 byte  ] WireTypeId enum value
//     [NULL_FLAG    : 1 byte  ] 0x00 = valid, 0x01 = null
//     If valid:
//       [DATA_LEN  : 4 bytes ] uint32_t — byte length of payload
//       [PAYLOAD   : DATA_LEN bytes]
//
//   Payload encoding by kind:
//     Scalars:
//       Same encoding as scalar payloads (raw little-endian, length-prefixed
//       strings, etc.).
//     Struct:
//       Recursive tagged field sequence: [FIELD_COUNT : 2 bytes] followed by
//       tagged fields with the same per-field layout.  No schema_hash header.
//     List / large_list:
//       [COUNT : 4 bytes uint32] then per element:
//         [NULL_FLAG : 1 byte] + if valid: element payload.
//         Struct elements use the recursive tagged format.
//         Scalar elements use raw encoding.
//     Fixed-size list:
//       Same as list but without COUNT prefix (count from type).
//     Map:
//       [COUNT : 4 bytes uint32] then per entry:
//         key payload (no null flag) + [NULL_FLAG : 1 byte] + if valid:
//         value payload.  Struct values use recursive tagged format.
//     Sparse/dense union:
//       [TYPE_CODE : 1 byte] + active child payload.
//
//   Schema evolution:
//     The tagged format enables cross-schema decoding.  When the schema_hash
//     in the buffer does not match the reader's hash, a DecodingMap is built
//     by matching FIELD_NUM values against the reader schema's "field_number"
//     metadata.  Fields unknown to the reader are skipped via DATA_LEN.
//     Fields missing from the wire are filled with null.  Type promotions
//     (e.g. int32 → int64, float → double) are applied per Iceberg rules.
//
//   DICTIONARY is not supported.

// Compute a 64-bit FNV-1a hash of the schema's structural fingerprint.
//
// Two schemas with identical field names, types, and nullability produce the
// same hash.  Field-level metadata is not included (Arrow fingerprints omit it).
uint64_t FingerprintHash(const arrow::Schema& schema);

// Binds a schema to EncodeRow / DecodeRow so callers don't pass it on every call.
class RowCodec {
 public:
    explicit RowCodec(std::shared_ptr<arrow::Schema> schema);

    EncodedRow EncodeRow(const ArrowRow& values) const;

    ArrowRow DecodeRow(const EncodedRow& buf) const;

    const arrow::Schema& schema()      const noexcept { return *schema_; }
    uint64_t             schema_hash() const noexcept { return schema_hash_; }

 private:
    std::shared_ptr<arrow::Schema> schema_;
    uint64_t                       schema_hash_;

    // Cache of DecodingMaps keyed by writer schema_hash.
    // Populated lazily during DecodeRow; mutable because decode is logically const.
    // Uses unique_ptr<mutex> so RowCodec remains movable.
    mutable std::unique_ptr<std::mutex>               cache_mutex_;
    mutable std::unordered_map<uint64_t, DecodingMap> decoding_cache_;
};

}  // namespace fletcher

#endif  // FLETCHER_INCLUDE_ROW_CODEC_HPP_
