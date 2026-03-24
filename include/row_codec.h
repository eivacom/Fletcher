#pragma once

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
//       Fixed-width types (bool, int*, uint*, float, double,
//                          date32, date64, timestamp):
//           raw bytes, little-endian
//       Variable-width types (string, large_string, binary, large_binary):
//           [LENGTH : 4 bytes uint32] [DATA : <LENGTH> bytes]

static constexpr uint8_t kVersion = 0x01u;

// Compute a 64-bit FNV-1a hash of the schema's structural fingerprint.
//
// Two schemas with identical fields, types, and nullability produce the same
// hash.  Field-level metadata is not included (Arrow fingerprints omit it).
//
// Throws std::invalid_argument if the schema contains types that Arrow cannot
// fingerprint (e.g. unregistered extension types).
uint64_t fingerprintHash(const arrow::Schema& schema);

// Binds a schema to encodeRow / decodeRow so callers don't pass it on every call.
class RowCodec {
public:
    explicit RowCodec(std::shared_ptr<arrow::Schema> schema);

    ArrowRow encodeRow(
        const std::vector<std::shared_ptr<arrow::Scalar>>& values) const;

    std::vector<std::shared_ptr<arrow::Scalar>> decodeRow(
        const ArrowRow& buf) const;

    const arrow::Schema& schema()     const noexcept { return *schema_; }
    uint64_t             schemaHash() const noexcept { return schemaHash_; }

private:
    std::shared_ptr<arrow::Schema> schema_;
    uint64_t                       schemaHash_;
};

} // namespace arrow_row
