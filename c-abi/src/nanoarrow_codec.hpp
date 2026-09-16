// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The wire-format codec behind the binding ABI (BIND-2).
//
// ── Why this exists at all ──────────────────────────────────────────────────
// There is exactly ONE wire format, and until this round there was exactly one
// implementation of it above the byte level: `arrow-bridge`'s `Codec`, written
// against Arrow C++. That codec cannot sit behind a C ABI. Two reasons, and only
// the second is about C#:
//
//   * Its RETURN TYPE is the copy. `EncodeRow(const ArrowRow&) -> EncodedRow`
//     has written the row somewhere other than the transport's window, and the
//     caller then appends it — the whole-row copy the copy-accounting oracle's
//     `StagingProducerIsCaught` control exists to catch.
//   * Its PARAMETER is unbindable. A vector of `shared_ptr<arrow::Scalar>` is one
//     heap object per field per row; it cannot cross P/Invoke, and it is
//     re-validated on every call.
//
// So the encoder behind the ABI takes a DESTINATION it does not own, a BOUND
// ARRAY VIEW plus a row index rather than scalars, and hoists validation out of
// the per-row call. This class is that encoder. It links nanoarrow and never
// `arrow-bridge`: the binding does not depend on Arrow C++, which is what keeps
// the shipped native shim to one file per RID.
//
// ── What makes it correct ───────────────────────────────────────────────────
// Byte identity with `arrow-bridge`'s `Codec`, across a fixture corpus, is the
// forcing test (`NanoarrowCodec.ByteIdenticalToArrowBridge`). That is the whole
// safety argument for having a second encoder in the tree: the two are not
// allowed to diverge by a single byte, and the test says so on every run rather
// than the comment saying so once.
#ifndef FLETCHER_C_ABI_SRC_NANOARROW_CODEC_HPP_
#define FLETCHER_C_ABI_SRC_NANOARROW_CODEC_HPP_

#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <fletcher/core/write_buffer.hpp>
#include <fletcher/pubsub/owned_schema.hpp>
#include <memory>

namespace fletcher::abi {

class NanoarrowCodec;

/// One `ArrowArray` bound to a codec: validated once, borrowed throughout.
///
/// **The array is BORROWED and never consumed.** This class does not call the
/// array's release callback, and it must not outlive the export the caller made.
/// That rule is what lets ONE export serve N publishes — the batch mitigation of
/// risk B-2 made structural rather than remembered — and a binding is expected to
/// enforce it with a type of its own (C#'s `BoundRows.Dispose()` unbinds and
/// *then* releases, in that order).
///
/// Immutable after construction, so any number of threads may encode different
/// rows of one bound batch concurrently without a lock.
class BoundRows {
   public:
    /// Validates that `array` is a struct array matching the codec's schema and
    /// builds the array view. **Every buffer is validated here, once per batch** —
    /// nanoarrow walks them in `ArrowArrayViewSetArray`, so the per-row path
    /// allocates nothing and validates nothing.
    ///
    /// Throws `PubSubError(kInvalidArgument)` naming what did not match.
    BoundRows(const NanoarrowCodec& codec, const ArrowArray& array);
    ~BoundRows();

    BoundRows(const BoundRows&) = delete;
    BoundRows& operator=(const BoundRows&) = delete;
    BoundRows(BoundRows&&) = delete;
    BoundRows& operator=(BoundRows&&) = delete;

    [[nodiscard]] int64_t length() const noexcept { return length_; }
    [[nodiscard]] const ArrowArrayView& view() const noexcept { return *view_; }

   private:
    std::unique_ptr<ArrowArrayView> view_;
    int64_t length_ = 0;
};

/// A schema-bound field plan, opened once per topic.
///
/// Immutable after construction; safe to share across threads.
class NanoarrowCodec {
   public:
    /// Deep-copies `schema` (so the caller may release its export immediately)
    /// and validates every field against the wire mapping, refusing a type the
    /// format cannot carry **naming the field** — a refusal that says
    /// "dictionary fields are not supported" without saying which field is one
    /// costs its reader an afternoon.
    ///
    /// Throws `PubSubError(kInvalidArgument)` for a schema that is not a struct
    /// or carries an unsupported type.
    explicit NanoarrowCodec(const ArrowSchema& schema);

    /// Encode row `index` of `rows` into `out`, in the positional wire format.
    ///
    /// Writes THROUGH the buffer it is given: when that buffer is the provider's
    /// own window, the row's bytes come to exist inside the transport and the
    /// oracle counts zero encode copies. Nothing is returned, deliberately.
    ///
    /// Throws `PubSubError` / `std::invalid_argument` on a row the format cannot
    /// carry (a string longer than 4 GiB, a list longer than UINT32_MAX).
    void EncodeRow(const BoundRows& rows, int64_t index, WriteBuffer& out) const;

    [[nodiscard]] const ArrowSchema& schema() const noexcept { return *schema_.get(); }

   private:
    friend class BoundRows;

    OwnedSchema schema_;
};

}  // namespace fletcher::abi

#endif  // FLETCHER_C_ABI_SRC_NANOARROW_CODEC_HPP_
