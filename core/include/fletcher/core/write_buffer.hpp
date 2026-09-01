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

namespace fletcher {

// Sequential binary output with random-access patching. Appends are inline over a byte window the
// subclass provides; only running out of room reaches the virtual slow path, bytes in hand.
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
        if (offset + sizeof(value) > pos_)
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

   protected:
    WriteBuffer(uint8_t* data, size_t capacity, size_t pos = 0)
        : data_(data), capacity_(capacity), pos_(pos) {}

    // The window is full: append the bytes anyway, updating data_/capacity_/pos_, or throw.
    virtual void AppendSlow(const uint8_t* data, size_t len) = 0;
    virtual void AppendZerosSlow(size_t len) = 0;

    uint8_t* data_;
    size_t capacity_;
    size_t pos_;
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
            Reserve(len);
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
        Reserve(grow);
        buf_.resize(pos_ + grow);
        Sync(pos_);
    }

    // The vector alone grows 1.5x from empty: six allocations for a 60-byte row.
    void Reserve(size_t len) {
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
