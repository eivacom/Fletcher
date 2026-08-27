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
#include <vector>

namespace fletcher {

// Sequential binary output with random-access patching. Appends are inline over a byte window the
// subclass provides; only running out of room reaches the virtual Grow().
class WriteBuffer {
   public:
    virtual ~WriteBuffer() = default;

    void Append(const uint8_t* data, size_t len) {
        // Subtracts, never adds: `pos_ + len` could wrap for an attacker-supplied length.
        if (len > capacity_ - pos_) Grow(len);
        std::memcpy(data_ + pos_, data, len);
        pos_ += len;
    }

    void AppendByte(uint8_t byte) {
        if (pos_ == capacity_) Grow(1);
        data_[pos_++] = byte;
    }

    // Null bitfield placeholders.
    void AppendZeros(size_t len) {
        if (len > capacity_ - pos_) Grow(len);
        std::memset(data_ + pos_, 0, len);
        pos_ += len;
    }

    size_t Position() const { return pos_; }

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

    // `len` more bytes do not fit: make them (updating data_/capacity_) or throw.
    virtual void Grow(size_t len) = 0;

    uint8_t* data_;
    size_t capacity_;
    size_t pos_;
};

// Appends to a std::vector<uint8_t>, whose size is the write position after every call.
// Leave the vector alone while the writer is alive: it tracks the position itself.
class VectorWriteBuffer : public WriteBuffer {
   public:
    explicit VectorWriteBuffer(std::vector<uint8_t>& buf)
        : WriteBuffer(buf.data(), buf.size(), buf.size()), buf_(buf) {}

   private:
    void Grow(size_t len) override {
        // resize() alone grows 1.5x from empty: six allocations for a 60-byte row.
        if (buf_.capacity() < pos_ + len) buf_.reserve(std::max<size_t>(2 * (pos_ + len), 128));
        buf_.resize(pos_ + len);
        data_ = buf_.data();
        capacity_ = buf_.size();
    }

    std::vector<uint8_t>& buf_;
};

// WriteBuffer backed by a fixed-size pre-allocated byte array.
class FixedWriteBuffer : public WriteBuffer {
   public:
    FixedWriteBuffer(uint8_t* data, size_t capacity) : WriteBuffer(data, capacity) {}

   private:
    void Grow(size_t) override { throw std::overflow_error("FixedWriteBuffer: overflow"); }
};

}  // namespace fletcher

#endif  // FLETCHER_INCLUDE_CORE_WRITE_BUFFER_HPP_
