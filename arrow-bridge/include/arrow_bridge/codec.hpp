#ifndef FLETCHER_INCLUDE_CODEC_HPP_
#define FLETCHER_INCLUDE_CODEC_HPP_

#include <arrow/api.h>

#include <core/types.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace fletcher {

using ArrowRow = std::vector<std::shared_ptr<arrow::Scalar>>;

// Positional row buffer format (little-endian throughout):
//
//   [NULL_BITFIELD : ceil(num_fields / 8) bytes, LSB-first bit order]
//
//   For each field in schema order (skip if null bit set):
//     Fixed-size scalar:  [PAYLOAD : N bytes]       (N known from schema type)
//     Variable-size:      [LEN : 4 bytes LE] [PAYLOAD : LEN bytes]
//     Struct:             [NULL_BITFIELD] [payloads...]  (recursive positional)
//     List/Large_list:    [COUNT : 4 LE] [NULL_BITFIELD : ceil(count/8)] [payloads...]
//     Fixed_size_list:    [NULL_BITFIELD : ceil(fixed_size/8)] [payloads...]
//     Map:                [COUNT : 4 LE] [key payloads...] [val NULL_BITFIELD] [val payloads...]
//     Union:              [TYPE_CODE : 1] [payload]
//
//   This format assumes both sides share the exact same schema.  There is
//   no schema hash, no field numbers, and no wire type tags — the schema
//   defines the layout completely.  Schema discovery is handled by the
//   pub/sub layer's companion topic mechanism.
//
//   DICTIONARY is not supported.

// Binds a schema to EncodeRow / DecodeRow for the positional format.
class Codec {
 public:
    explicit Codec(std::shared_ptr<arrow::Schema> schema);

    EncodedRow EncodeRow(const ArrowRow& values) const;

    ArrowRow DecodeRow(const EncodedRow& buf) const;
    ArrowRow DecodeRow(const uint8_t* data, size_t len) const;

    const arrow::Schema& schema() const noexcept { return *schema_; }

 private:
    std::shared_ptr<arrow::Schema> schema_;
};

}  // namespace fletcher

#endif  // FLETCHER_INCLUDE_CODEC_HPP_
