#ifndef ARROW_ROW_INCLUDE_ROW_CODEC_HPP_
#define ARROW_ROW_INCLUDE_ROW_CODEC_HPP_

#include <arrow/api.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace arrow_row {

using ArrowRow = std::vector<uint8_t>;

// Custom row buffer format (little-endian throughout):
//
//   [SCHEMA_HASH : 8 bytes ] FNV-1a 64-bit hash of the schema fingerprint
//   [VERSION     : 1 byte  ] = 0x01
//
//   For each field (in schema order):
//     [NULL_FLAG : 1 byte ] 0x00 = valid, 0x01 = null
//     If valid, field payload:
//       Fixed-width types (bool, int*, uint*, float, double, half_float,
//                          date32, date64, timestamp,
//                          time32, time64, duration,
//                          fixed_size_binary,
//                          interval_months,
//                          interval_day_time,
//                          interval_month_day_nano,
//                          decimal128, decimal256):
//           raw bytes, little-endian
//           fixed_size_binary : byte_width bytes, no length prefix
//           interval_day_time : int32 days, int32 milliseconds (8 bytes)
//           interval_month_day_nano : int32 months, int32 days,
//                                     int64 nanoseconds (16 bytes)
//           decimal128        : 16 bytes two's-complement little-endian
//           decimal256        : 32 bytes two's-complement little-endian
//       Variable-width types (string, large_string, binary, large_binary):
//           [LENGTH : 4 bytes uint32] [DATA : <LENGTH> bytes]

static constexpr uint8_t kVersion = 0x01u;

// Compute a 64-bit FNV-1a hash of the schema's structural fingerprint.
//
// Two schemas with identical fields, types, and nullability produce the same
// hash.  Field-level metadata is not included (Arrow fingerprints omit it).
// Field names are not included; only types and nullability are compared.
//
// Throws std::invalid_argument if the schema contains types that Arrow cannot
// fingerprint (e.g. unregistered extension types).
uint64_t FingerprintHash(const arrow::Schema& schema);

// Like FingerprintHash, but also folds each field's name into the hash.
//
// Two schemas must have identical field names, types, and nullability to
// produce the same hash.  Use this when field names are semantically
// significant and a schema rename should produce a different hash.
//
// Throws std::invalid_argument if the schema cannot be fingerprinted.
uint64_t FingerprintHashWithFieldNames(const arrow::Schema& schema);

// Binds a schema to EncodeRow / DecodeRow so callers don't pass it on every call.
class RowCodec {
 public:
    explicit RowCodec(std::shared_ptr<arrow::Schema> schema);

    ArrowRow EncodeRow(
        const std::vector<std::shared_ptr<arrow::Scalar>>& values) const;

    std::vector<std::shared_ptr<arrow::Scalar>> DecodeRow(
        const ArrowRow& buf) const;

    const arrow::Schema& schema()      const noexcept { return *schema_; }
    uint64_t             schema_hash() const noexcept { return schema_hash_; }

 private:
    std::shared_ptr<arrow::Schema> schema_;
    uint64_t                       schema_hash_;
};

}  // namespace arrow_row

#endif  // ARROW_ROW_INCLUDE_ROW_CODEC_HPP_
