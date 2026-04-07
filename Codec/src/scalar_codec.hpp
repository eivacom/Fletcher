#ifndef FLETCHER_SRC_SCALAR_CODEC_HPP_
#define FLETCHER_SRC_SCALAR_CODEC_HPP_

// Internal header — not part of the public API.
// Exposes scalar-level encode/decode so that translation units other than
// row_codec.cpp (e.g. sqlite_wal.cpp) can serialise individual scalars to
// and from raw byte buffers.

#include "row_reader.hpp"

#include <arrow/scalar.h>
#include <arrow/type.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace fletcher {
namespace detail {

// Append the binary encoding of scalar to buf.
// Throws std::invalid_argument for unsupported types or for DICTIONARY.
void EncodeScalar(std::vector<uint8_t>& buf, const arrow::Scalar& scalar);

// Decode one scalar of the given type from a Reader, advancing its position.
// Handles all scalar types plus composites (list, map, struct, union).
// Throws std::invalid_argument on type mismatch or truncated input.
std::shared_ptr<arrow::Scalar> DecodeScalarFromReader(
    Reader& r,
    const std::shared_ptr<arrow::DataType>& type);

// Decode one scalar of the given type from [data, data+size).
// Convenience wrapper around DecodeScalarFromReader.
// Throws std::invalid_argument on type mismatch or truncated input.
std::shared_ptr<arrow::Scalar> DecodeScalar(
    const uint8_t*                          data,
    size_t                                  size,
    const std::shared_ptr<arrow::DataType>& type);

}  // namespace detail
}  // namespace fletcher

#endif  // FLETCHER_SRC_SCALAR_CODEC_HPP_
