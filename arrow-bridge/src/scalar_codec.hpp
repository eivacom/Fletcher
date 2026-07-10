// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#ifndef FLETCHER_SRC_SCALAR_CODEC_HPP_
#define FLETCHER_SRC_SCALAR_CODEC_HPP_

// Internal header — not part of the public API.
// Exposes scalar-level encode/decode so that translation units other than
// codec.cpp can serialise individual scalars to and from raw byte buffers.

#include <arrow/scalar.h>
#include <arrow/type.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "row_reader.hpp"

namespace fletcher {

// Append value's raw object representation to buf (sizeof(T) little-endian
// bytes on a little-endian host). Single source of truth shared by codec.cpp
// and scalar_codec.cpp.
template <typename T>
inline void AppendFixed(std::vector<uint8_t>& buf, T value) {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&value);
    buf.insert(buf.end(), bytes, bytes + sizeof(T));
}

namespace detail {

// Append the binary encoding of scalar to buf.
// A DICTIONARY scalar is encoded as its resolved value (the value type must be
// a scalar type). Throws std::invalid_argument for unsupported types.
void EncodeScalar(std::vector<uint8_t>& buf, const arrow::Scalar& scalar);

// Decode one scalar of the given type from a Reader, advancing its position.
// Handles scalar types only.  Composite types (list, map, struct, union) are
// handled by the Codec itself.
// Throws std::invalid_argument on type mismatch or truncated input.
std::shared_ptr<arrow::Scalar> DecodeScalarFromReader(Reader& r,
                                                      const std::shared_ptr<arrow::DataType>& type);

}  // namespace detail
}  // namespace fletcher

#endif  // FLETCHER_SRC_SCALAR_CODEC_HPP_
