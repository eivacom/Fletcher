// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#ifndef FLETCHER_INCLUDE_CORE_TYPES_HPP_
#define FLETCHER_INCLUDE_CORE_TYPES_HPP_

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "fletcher/core/status.hpp"

namespace fletcher {

// Binary-encoded row data.
using EncodedRow = std::vector<uint8_t>;

/// Opaque binary payload crossing the seam: an OWNER plus a SPAN.
///
/// It used to be `shared_ptr<const vector<uint8_t>>`, which could only ever name
/// bytes Fletcher had allocated. That is the single thing standing between the
/// seam and zero-copy receive (spec §3.2, §8): a transport holding a loaned
/// sample could not hand those bytes over without first copying them into a
/// vector. An owner plus a span can — the owner is whatever keeps the bytes
/// alive, and Fletcher never needs to know what it is.
///
/// ── The normative rule (spec §3.2 clauses 1-5), stated here because a C view
///    on either side of the seam must derive the SAME one ────────────────────
///
/// A blob is `{owner, data, size}`.
///
///  1. `owner` keeps `[data, data + size)` alive for as long as any copy of this
///     Blob lives. There is no view-only form: non-null `data` with a null
///     `owner` is refused at construction, so §3.2 clause 1's "a callee that
///     keeps it takes its own reference" is exactly true, and a C boundary
///     implements "keep it" as `retain(owner)`.
///  2. Bytes are **immutable** once they cross. `data()` is a `const uint8_t*`
///     and there is no `resize`.
///  3. The reference operations — the C form's `retain(owner)` / `release(owner)`
///     — must be safe from **any thread, concurrently**.
///  4. `release` must never throw and must never re-enter the seam.
///  5. Empty is a null data pointer and a zero size.
///
/// A blob passed as an argument is **borrowed for the duration of that call**; a
/// callee that wants it afterwards copies the Blob, which retains.
///
/// **The C form is conceptual, never a memory image.** `std::shared_ptr<const
/// void>` is two words and a control block; a C `{void* owner, const uint8_t*
/// data, size_t size}` is not a reinterpretation of it, and no layout
/// compatibility is implied or permitted. A boundary *constructs* a Blob from
/// the three fields. That is also why the two ABI rounds need no shared C header
/// (decision 2): they never exchange a struct with each other, they each wrap
/// this same C++ value from their own side, so their C spellings may differ
/// freely.
///
/// There is deliberately **no conversion from the retired
/// `shared_ptr<const vector<uint8_t>>` alias**. One would leave every call site
/// compiling and the copy in place, unnoticed — a coexistence window in a change
/// whose whole point is that there is none.
class Blob {
   public:
    /// Empty: null data, zero size.
    Blob() noexcept = default;

    /// The ONE general form. `owner` is anything whose lifetime covers the
    /// bytes: a vector, a transport's loan handle, an arena, a driver-side
    /// release token.
    ///
    /// Throws PubSubError(kInvalidArgument) for a blob that cannot honour the
    /// rule above: null `data` with a non-zero `size` (bytes that are not
    /// there), or non-null `data` with a null `owner` (bytes nothing keeps
    /// alive).
    Blob(std::shared_ptr<const void> owner, const uint8_t* data, size_t size)
        : owner_(std::move(owner)), data_(data), size_(size) {
        if (data_ == nullptr && size_ != 0) {
            throw PubSubError(
                PubSubStatus::kInvalidArgument,
                "Blob: a null data pointer cannot carry " + std::to_string(size_) + " bytes");
        }
        if (data_ != nullptr && owner_ == nullptr) {
            throw PubSubError(PubSubStatus::kInvalidArgument,
                              "Blob: bytes crossing the seam need an owner that keeps them alive; "
                              "there is no view-only Blob");
        }
    }

    /// Bytes Fletcher allocated. The vector becomes the owner, so this is the
    /// general form with the owner filled in for you.
    explicit Blob(std::vector<uint8_t> bytes) {
        auto owned = std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
        data_ = owned->data();
        size_ = owned->size();
        owner_ = std::move(owned);
    }

    [[nodiscard]] const uint8_t* data() const noexcept { return data_; }
    [[nodiscard]] size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

   private:
    std::shared_ptr<const void> owner_;
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
};

// Key-value sidecar data attached to a message during transit.
using Attachments = std::unordered_map<std::string, Blob>;

}  // namespace fletcher

#endif  // FLETCHER_INCLUDE_CORE_TYPES_HPP_
