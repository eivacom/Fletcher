// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#ifndef FLETCHER_INCLUDE_ARROW_BRIDGE_BATCH_DECODER_HPP_
#define FLETCHER_INCLUDE_ARROW_BRIDGE_BATCH_DECODER_HPP_

#include <arrow/type_fwd.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>

namespace fletcher {

// Append() throws this when the row is well-formed but appending it would overflow an Arrow 32-bit
// offset (utf8/binary, list, map and dense-union builders stop at Arrow's builder limit (2^31-2)
// bytes or elements). Nothing was appended: Finish() the batch and Append() the same row again.
class BatchCapacityExceeded : public std::length_error {
    using std::length_error::length_error;
};

// Decodes positional wire rows (the layout in codec.hpp) straight into one arrow::ArrayBuilder tree
// per field — no arrow::Scalar is created. This is the batched subscriber's decode path; Codec's
// DecodeRow stays the per-row ArrowRow path. Not thread-safe.
class BatchDecoder {
   public:
    // std::invalid_argument for a schema this decoder cannot build: null/extension/decimal32/64/
    // run-end/list-view types, a dictionary below the top level, a dictionary whose value type is
    // nested or float16, or an ordered dictionary. Two different reasons hide behind that one list.
    // The first six are not supported AT ALL — `scalar_codec.cpp` has no case for NA, EXTENSION,
    // RUN_END_ENCODED, LIST_VIEW, LARGE_LIST_VIEW, DECIMAL32 or DECIMAL64, so
    // `Codec::DecodeRow`/`EncodeRow` throw `std::invalid_argument` on the identical schema too (NA
    // is the one exception that looks supported: a NA-typed field is always null, so it is encoded
    // and decoded through the null bit alone and never reaches the type switch that would throw).
    // The three dictionary shapes are different: `Codec::DecodeRow` decodes all three fine — the
    // refusal here is this decoder's builder-tree shape, which is stricter, not a wire-format
    // limit. std::runtime_error: an internal builder/type mismatch while walking the schema (an
    // invariant failure, not bad input).
    explicit BatchDecoder(std::shared_ptr<arrow::Schema> schema);
    ~BatchDecoder();
    BatchDecoder(BatchDecoder&&) noexcept;
    BatchDecoder& operator=(BatchDecoder&&) noexcept;

    // One wire row. std::invalid_argument: malformed row, nothing appended. BatchCapacityExceeded:
    // nothing appended, see above. std::runtime_error: internal failure (allocation); the pending
    // rows are undefined — Finish() and discard the batch.
    void Append(const uint8_t* data, size_t len);

    // Pre-sizes the top-level builders for `rows` more rows.
    void Reserve(int64_t rows);

    [[nodiscard]] int64_t num_rows() const noexcept;

    // The pending rows as a RecordBatch (zero rows is valid) and resets for reuse.
    [[nodiscard]] std::shared_ptr<arrow::RecordBatch> Finish();

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace fletcher

#endif  // FLETCHER_INCLUDE_ARROW_BRIDGE_BATCH_DECODER_HPP_
