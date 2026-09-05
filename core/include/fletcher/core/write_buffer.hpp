// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#ifndef FLETCHER_INCLUDE_CORE_WRITE_BUFFER_HPP_
#define FLETCHER_INCLUDE_CORE_WRITE_BUFFER_HPP_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include "fletcher/core/status.hpp"

namespace fletcher {

// Sequential binary output with random-access patching. Appends are inline over a byte window the
// subclass provides; only running out of room reaches the virtual slow path, bytes in hand.
//
// ── The normative rule for the crossing (spec §3.1), stated here because a C view on either side
//    of the seam must derive the SAME one ───────────────────────────────────────────────────────
//
// The window is `{data, capacity, pos}` — `Data()` is its base, `Position()` how much has been
// written. A window is **borrowed for the duration of the encode call** and must never be stored
// past it: the buffer that owns it may be a transport's loaned payload, valid only while the
// publish that lent it is running.
//
// A window means **one crossing per refill, not one per append**, which is the whole reason this
// shape is the C-expressible one: a C encoder writes straight into `{data, capacity, pos}` and only
// calls back when it runs out of room.
//
// Bytes already written must not move or be flushed **except inside a refill**, which preserves
// them verbatim at a new base (Fletcher back-patches length prefixes and null bitfields below
// `pos`). Refill movement is permitted and measured; every other byte movement is a zero-copy
// violation (§8.1).
//
// The window has a **write end as well as a readable one** (§3.1 clause 6, `AppendInPlace`
// below). Without it a caller holding only a `WriteBuffer&` — which is all `Publish`'s inversion
// hands a language binding — could not put a byte into the window without supplying that byte
// from somewhere else, so every foreign-language row cost one whole-row copy that §8 says cannot
// exist and that the copy oracle, measuring only from the window base onwards, could not see.
class WriteBuffer {
   public:
    virtual ~WriteBuffer() = default;

    void Append(const uint8_t* data, size_t len) {
        // Subtracts, never adds: `pos_ + len` could wrap for an attacker-supplied length.
        if (len > capacity_ - pos_) {
            AppendSlow(data, len);
            return;
        }
        std::memcpy(data_ + pos_, data, len);
        pos_ += len;
    }

    void AppendByte(uint8_t byte) {
        if (pos_ == capacity_) {
            AppendSlow(&byte, 1);
            return;
        }
        data_[pos_++] = byte;
    }

    // Null bitfield placeholders.
    void AppendZeros(size_t len) {
        if (len > capacity_ - pos_) {
            AppendZerosSlow(len);
            return;
        }
        std::memset(data_ + pos_, 0, len);
        pos_ += len;
    }

    size_t Position() const { return pos_; }

    // Base of the current window. Only `[Data(), Data() + Position())` is defined: the bytes
    // written so far, in the order they were written. Bytes at or past `Position()` are whatever
    // the last refill left there, and reading them is a bug.
    //
    // The pointer is INVALIDATED by any append that refills the window (a growable buffer
    // reallocates, spec §3.1 clause 1 — the bytes are preserved verbatim, at a new address) and by
    // `VectorWriteBuffer::Finish()`, which hands the bytes away and leaves this null. It is also
    // null on a growable buffer before its first refill, when no window exists yet.
    //
    // Exists so copy accounting is possible from OUTSIDE a provider: whether the bytes a
    // subscriber sees are the bytes the encoder wrote is an address question, and nothing else at
    // the seam can answer it (spec §8.1).
    const uint8_t* Data() const { return data_; }

    // Overwrite 4 bytes at a previous offset (for length prefixes).
    void PatchU32(size_t offset, uint32_t value) {
        // Subtracts, never adds (§3.1 clause 3): `offset + 4` could wrap for a hostile offset.
        // A1 documents patching from inside an AppendInPlace writer as a supported route, which
        // widens who reaches this bound.
        if (pos_ < sizeof(value) || offset > pos_ - sizeof(value))
            throw std::out_of_range("WriteBuffer::PatchU32: offset out of range");
        std::memcpy(data_ + offset, &value, sizeof(value));
    }

    // OR a byte into a previous offset (for null bitfield patching).
    void PatchByte(size_t offset, uint8_t bits) {
        if (offset >= pos_) throw std::out_of_range("WriteBuffer::PatchByte: offset out of range");
        data_[offset] |= bits;
    }

    template <typename T>
    void AppendFixed(T value) {
        static_assert(std::is_trivially_copyable_v<T>,
                      "AppendFixed requires a trivially copyable type");
        Append(reinterpret_cast<const uint8_t*>(&value), sizeof(T));
    }

    // Write a uint32 length placeholder and return its offset for later patching.
    size_t WriteLengthPlaceholder() {
        size_t pos = Position();
        AppendFixed<uint32_t>(0);
        return pos;
    }

    // ── The window's WRITE end (spec §3.1 clause 6) ─────────────────────────────────────────────
    //
    // Lend the window's write cursor to a producer for EXACTLY ONE CALL, then commit what it says
    // it wrote. This is the member that makes an uncopied row reachable by a client: every other
    // way of advancing `pos` requires the caller to already hold the bytes somewhere else.
    //
    // `writer` is invoked exactly once as
    //     size_t writer(uint8_t* dst, size_t room)
    // and returns how many bytes it wrote at `dst`. `room` is the WHOLE remaining window, never
    // just `min_bytes`, so a variable-length producer never needs a second crossing to ask how
    // much is left.
    //
    // C form (normative — both ABI rounds must derive THIS one, exactly as §3.2 does for Blob):
    //     size_t (*writer)(void* ctx, uint8_t* dst, size_t room)
    // Note what the C writer does NOT receive: a buffer handle. Foreign code therefore cannot
    // re-enter this buffer at all, and the re-entry refusal below is a C++-only concern.
    //
    // Sequence, normatively:
    //   1. `min_bytes == 0` is refused (kInvalidArgument) — a fill of no bytes names nothing.
    //      A NESTED fill is refused at the door, also kInvalidArgument: while a window is lent,
    //      no second lend of the same buffer can be taken.
    //   2. A `min_bytes` no window could ever satisfy (`> SIZE_MAX - pos`) is refused BEFORE the
    //      refill virtual is entered, with std::overflow_error → kPayloadTooLarge. Clause 3's
    //      subtract-never-add rule, applied to the one member whose normative C form takes a
    //      length straight from foreign code: a refill computes `pos + n` internally, so an
    //      un-satisfiable length must never reach it.
    //   3. If the window is short, room is MADE through the existing refill virtual. A
    //      fixed-capacity buffer therefore refuses with std::overflow_error → kPayloadTooLarge,
    //      which is §3.1 clause 4 unchanged, and the writer is not invoked.
    //   4. `dst` is `Data() + Position()` and stays valid for the writer's frame only.
    //   5. On return: a writer that moved the window is refused (kInvalidArgument); a writer
    //      reporting more than `room` is refused (kInvalidArgument); otherwise the position
    //      advances by exactly the reported count. The commit is one assignment after all
    //      validation, so there is no partial commit to observe or unwind.
    //
    // PERMITTED from inside the writer: const reads (`Data()`, `Position()`) — which is what lets
    // a copy oracle measure the lend from inside it — and `PatchU32`/`PatchByte` at offsets BELOW
    // the lend point, the back-patch route a variable-length row needs. Neither moves the window.
    // An exception thrown by the writer COMMITS NOTHING and propagates unchanged: the commit is
    // the last statement, so the position is still what it was.
    //
    // ── Two residues, disclosed rather than forbidden ───────────────────────────────────────────
    // Zero-copy means handing over real memory, so a raw pointer is the only C-expressible shape
    // and neither of these can be checked without giving that up:
    //
    //   (a) A writer that writes PAST `room`, or that stashes `dst` and uses it after returning.
    //       Memory is corrupt, or freed, before any check could run. This is the same exposure
    //       `Append(const uint8_t*, size_t)` has carried since this file was written — no new
    //       class. Both a C++ lambda capture and a C `ctx` field can spell the stash, so it is
    //       disclosed here rather than claimed impossible.
    //
    //   (b) A writer that REPORTS MORE THAN IT WROTE. This one IS a new class, and it is stated
    //       plainly because it is new: `used` is trusted against `room`, not against what was
    //       actually touched, so the bytes in `[written, used)` are committed and published
    //       having been written by nobody. Until this member, no public member advanced `pos`
    //       over a byte it did not itself write. The residue is UNIFORM across subclasses — it
    //       is NOT "a leak of zeros" on the growable ones. Whatever was last left above `pos`
    //       is what leaks, and this member is itself one of the things that leaves bytes there:
    //       a prior lend that scribbled N bytes and reported fewer puts the EARLIER producer's
    //       payload directly under the next lend, on `VectorWriteBuffer` exactly as on a fixed
    //       one. On `FixedWriteBuffer` the same span is, in this tree, a transport's payload —
    //       recycled pool memory that may still hold THE PREVIOUS SAMPLE'S BYTES — which is
    //       precisely the zero-copy path this member exists to open. It is not memset away:
    //       that would cost O(room) on exactly that path. A producer must report what it wrote.
    template <typename Writer>
    void AppendInPlace(size_t min_bytes, Writer&& writer) {
        if (min_bytes == 0) {
            throw PubSubError(PubSubStatus::kInvalidArgument,
                              "WriteBuffer::AppendInPlace: min_bytes must be non-zero");
        }

        // The one re-entry the return comparison below CANNOT see: a nested fill whose own refill
        // neither relocates nor moves `pos` (the storage already had spare capacity) and whose
        // writer commits nothing leaves all three window fields exactly where the outer lend left
        // them — after having scribbled over the very span the outer writer was lent. Refused at
        // the door instead, which makes it unrepresentable rather than detected too late. The
        // flag is read HERE only: the inline append path (`Append`/`AppendByte`/`AppendZeros`)
        // still pays nothing, and every mutating re-entry through those moves the window and is
        // caught on return.
        if (lending_) {
            throw PubSubError(PubSubStatus::kInvalidArgument,
                              "WriteBuffer::AppendInPlace: a nested in-place fill was attempted "
                              "while a window was already lent; nothing was committed");
        }

        // Subtracts, never adds — the same rule as every other append here, and this member needs
        // it TWICE. The refill virtual below computes `pos + n` internally on every growable
        // subclass, so a length that no window could satisfy must be refused before it gets
        // there: otherwise it wraps inside the refill, SHRINKS the window, and the restored
        // `pos_` is left above `capacity_` — after which the post-condition's own subtraction
        // underflows into a pass and the writer is handed a `room` of nearly 2^64 over a handful
        // of real bytes, leaving the buffer invariant-broken for every later append.
        if (min_bytes > SIZE_MAX - pos_) {
            throw std::overflow_error(
                "WriteBuffer::AppendInPlace: min_bytes cannot be satisfied by any window");
        }

        if (min_bytes > capacity_ - pos_) {
            // No new virtual: PDA-ABI mirrors the vtable it already has. A growable subclass's
            // refill zero-fills anyway, so nothing extra is paid for going through it.
            const size_t before_refill = pos_;
            AppendZerosSlow(min_bytes);
            pos_ = before_refill;
        }
        // Guards a subclass whose refill does not deliver CONTIGUOUS room at the restored
        // position. Loud, not silent: without it the writer would be handed a short span. The
        // `capacity_ < pos_` half detects a subclass that broke the window invariant outright,
        // instead of being defeated by the underflow that state produces.
        if (capacity_ < pos_ || capacity_ - pos_ < min_bytes) {
            throw std::overflow_error(
                "WriteBuffer::AppendInPlace: the refill did not deliver contiguous room");
        }

        uint8_t* const base0 = data_;
        const size_t pos0 = pos_;
        const size_t cap0 = capacity_;
        const size_t room = capacity_ - pos0;

        const Lend lend(*this);
        const size_t used = std::forward<Writer>(writer)(data_ + pos0, room);

        // Checked in this order, and BEFORE anything is committed. The commit below is
        // ABSOLUTE (`pos0 + used`) rather than a delta on whatever `pos_` became, so a mutating
        // re-entry — Append, AppendByte, AppendZeros, Finish — moves `data_`, `pos_` or
        // `capacity_` and is caught by this one comparison at return. A nested AppendInPlace
        // never reaches here; it was refused at the door above. The hot inline append path pays
        // nothing for either check.
        if (data_ != base0 || pos_ != pos0 || capacity_ != cap0) {
            throw PubSubError(
                PubSubStatus::kInvalidArgument,
                "WriteBuffer::AppendInPlace: the writer re-entered the buffer, invalidating the "
                "window it was lent; nothing was committed");
        }
        if (used > room) {
            throw PubSubError(PubSubStatus::kInvalidArgument,
                              "WriteBuffer::AppendInPlace: the writer reported more bytes than it "
                              "was lent; nothing was committed");
        }
        pos_ = pos0 + used;
    }

   protected:
    WriteBuffer(uint8_t* data, size_t capacity, size_t pos = 0)
        : data_(data), capacity_(capacity), pos_(pos) {}

    // The window is full: append the bytes anyway, updating data_/capacity_/pos_, or throw.
    virtual void AppendSlow(const uint8_t* data, size_t len) = 0;
    virtual void AppendZerosSlow(size_t len) = 0;

    uint8_t* data_;
    size_t capacity_;
    size_t pos_;

   private:
    // Set for the writer's frame only, and cleared however that frame leaves — including by the
    // writer's own exception, which still commits nothing and propagates unchanged.
    class Lend {
       public:
        explicit Lend(WriteBuffer& buffer) : buffer_(buffer) { buffer_.lending_ = true; }
        ~Lend() { buffer_.lending_ = false; }
        Lend(const Lend&) = delete;
        Lend& operator=(const Lend&) = delete;

       private:
        WriteBuffer& buffer_;
    };

    bool lending_ = false;
};

// Owns its vector and runs ahead of the position; Finish() trims it and hands it over.
class VectorWriteBuffer : public WriteBuffer {
   public:
    VectorWriteBuffer() : WriteBuffer(nullptr, 0) {}

    // Continues after the vector's bytes; pass a cleared one to reuse its allocation.
    explicit VectorWriteBuffer(std::vector<uint8_t> buf)
        : WriteBuffer(nullptr, buf.size(), buf.size()), buf_(std::move(buf)) {
        data_ = buf_.data();
    }

    // The bytes written so far, trimmed. Leaves the writer empty and reusable.
    std::vector<uint8_t> Finish() {
        buf_.resize(pos_);
        std::vector<uint8_t> out = std::move(buf_);
        buf_ = std::vector<uint8_t>();
        data_ = nullptr;
        capacity_ = pos_ = 0;
        return out;
    }

   private:
    // Small appends refill the window by kChunk; large ones go in with one copy and leave it full.
    static constexpr size_t kChunk = 256;

    void AppendSlow(const uint8_t* data, size_t len) override {
        if (len >= kChunk) {
            buf_.resize(pos_);
            ReserveStorage(len);
            buf_.insert(buf_.end(), data, data + len);
            Sync(buf_.size());
            return;
        }
        Refill(len);
        std::memcpy(data_ + pos_, data, len);
        pos_ += len;
    }

    // resize() zero-fills the new bytes, which is the value wanted here.
    void AppendZerosSlow(size_t len) override {
        Refill(len);
        pos_ += len;
    }

    void Refill(size_t len) {
        // Template form on purpose: a bare `max(` is a macro in TUs that include <windows.h>.
        const size_t grow = std::max<size_t>(len, kChunk);
        ReserveStorage(grow);
        buf_.resize(pos_ + grow);
        Sync(pos_);
    }

    // Grows the underlying STORAGE, not the window — named so no reader mistakes a
    // std::vector::reserve helper for a window reserve. There is no WriteBuffer::Reserve and
    // deliberately no Reserve/Commit pair: the window is lent for one call (AppendInPlace), so a
    // reservation that outlives a refill, is committed twice, or is never taken cannot be spelled.
    // The vector alone grows 1.5x from empty: six allocations for a 60-byte row.
    void ReserveStorage(size_t len) {
        if (buf_.capacity() < pos_ + len) buf_.reserve(std::max<size_t>(2 * (pos_ + len), 128));
    }

    void Sync(size_t pos) {
        data_ = buf_.data();
        capacity_ = buf_.size();
        pos_ = pos;
    }

    std::vector<uint8_t> buf_;
};

// WriteBuffer backed by a fixed-size pre-allocated byte array.
class FixedWriteBuffer : public WriteBuffer {
   public:
    FixedWriteBuffer(uint8_t* data, size_t capacity) : WriteBuffer(data, capacity) {}

   private:
    void AppendSlow(const uint8_t*, size_t) override { Overflow(); }
    void AppendZerosSlow(size_t) override { Overflow(); }

    [[noreturn]] static void Overflow() { throw std::overflow_error("FixedWriteBuffer: overflow"); }
};

}  // namespace fletcher

#endif  // FLETCHER_INCLUDE_CORE_WRITE_BUFFER_HPP_
