#ifndef ARROW_ROW_INCLUDE_ROW_CODEC_HPP_
#define ARROW_ROW_INCLUDE_ROW_CODEC_HPP_

#include <arrow/api.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace arrow_row {

using ArrowRow = std::vector<std::shared_ptr<arrow::Scalar>>;
using EncodedRow = std::vector<uint8_t>;

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
//       Variable-width types (string, large_string, binary, large_binary,
//                             string_view, binary_view):
//           [LENGTH : 4 bytes uint32] [DATA : <LENGTH> bytes]
//           string_view / binary_view are materialized to a contiguous buffer
//           by the Arrow scalar API; the array-level multi-buffer layout is
//           not visible here.
//       struct     : fields encoded in order, each with its own NULL_FLAG
//       list / large_list   : [COUNT : 4 bytes] (NULL_FLAG + element)*
//       fixed_size_list     : (NULL_FLAG + element)* — no count prefix
//       map        : [COUNT : 4 bytes] (key, NULL_FLAG + value)* per entry
//                    key has no null flag (Arrow keys are always non-null)
//       sparse_union / dense_union : [TYPE_CODE : 1 byte] active-child payload
//
//   DICTIONARY is not supported.  It is a columnar storage optimisation
//   whose dictionary array lives outside any individual row; there is no
//   lossless, self-contained per-row representation.  Use a non-dictionary
//   field type instead (e.g. utf8 rather than dictionary<int32, utf8>).

static constexpr uint8_t kVersion = 0x01u;

// Compute a 64-bit FNV-1a hash of the schema's structural fingerprint.
//
// Two schemas with identical field names, types, and nullability produce the
// same hash.  Field-level metadata is not included (Arrow fingerprints omit it).
//
// Throws std::invalid_argument if the schema contains types that Arrow cannot
// fingerprint (e.g. unregistered extension types).
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
};

}  // namespace arrow_row

#endif  // ARROW_ROW_INCLUDE_ROW_CODEC_HPP_
