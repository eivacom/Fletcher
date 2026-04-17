#ifndef FLETCHER_INCLUDE_WRITE_BUFFER_HPP_
#define FLETCHER_INCLUDE_WRITE_BUFFER_HPP_

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace fletcher {

// Abstract sequential binary output with random-access patching.
//
// Implementations target different backing stores:
//   - VectorWriteBuffer wraps a growable std::vector<uint8_t>.
//   - FixedWriteBuffer wraps a pre-allocated byte array (e.g. DDS payload).
class WriteBuffer {
 public:
    virtual ~WriteBuffer() = default;

    virtual void Append(const uint8_t* data, size_t len) = 0;
    virtual void AppendByte(uint8_t byte) = 0;
    virtual size_t Position() const = 0;

    // Overwrite 4 bytes at a previous offset (for length prefixes).
    virtual void PatchU32(size_t offset, uint32_t value) = 0;

    // OR a byte into a previous offset (for null bitfield patching).
    virtual void PatchByte(size_t offset, uint8_t bits) = 0;

    template <typename T>
    void AppendFixed(T value) {
        Append(reinterpret_cast<const uint8_t*>(&value), sizeof(T));
    }

    // Write a uint32 length placeholder and return its offset for later patching.
    size_t WriteLengthPlaceholder() {
        size_t pos = Position();
        AppendFixed<uint32_t>(0);
        return pos;
    }
};

// WriteBuffer backed by a growable std::vector<uint8_t>.
class VectorWriteBuffer : public WriteBuffer {
 public:
    explicit VectorWriteBuffer(std::vector<uint8_t>& buf) : buf_(buf) {}

    void Append(const uint8_t* data, size_t len) override {
        buf_.insert(buf_.end(), data, data + len);
    }

    void AppendByte(uint8_t byte) override {
        buf_.push_back(byte);
    }

    size_t Position() const override {
        return buf_.size();
    }

    void PatchU32(size_t offset, uint32_t value) override {
        if (offset + sizeof(value) > buf_.size())
            throw std::out_of_range("VectorWriteBuffer::PatchU32: offset out of range");
        std::memcpy(buf_.data() + offset, &value, sizeof(value));
    }

    void PatchByte(size_t offset, uint8_t bits) override {
        if (offset >= buf_.size())
            throw std::out_of_range("VectorWriteBuffer::PatchByte: offset out of range");
        buf_[offset] |= bits;
    }

 private:
    std::vector<uint8_t>& buf_;
};

// WriteBuffer backed by a fixed-size pre-allocated byte array.
class FixedWriteBuffer : public WriteBuffer {
 public:
    FixedWriteBuffer(uint8_t* data, size_t capacity)
        : data_(data), capacity_(capacity) {}

    void Append(const uint8_t* data, size_t len) override {
        if (pos_ + len > capacity_)
            throw std::overflow_error("FixedWriteBuffer: overflow");
        std::memcpy(data_ + pos_, data, len);
        pos_ += len;
    }

    void AppendByte(uint8_t byte) override {
        if (pos_ >= capacity_)
            throw std::overflow_error("FixedWriteBuffer: overflow");
        data_[pos_++] = byte;
    }

    size_t Position() const override { return pos_; }

    void PatchU32(size_t offset, uint32_t value) override {
        if (offset + sizeof(value) > capacity_)
            throw std::out_of_range("FixedWriteBuffer::PatchU32: offset out of range");
        std::memcpy(data_ + offset, &value, sizeof(value));
    }

    void PatchByte(size_t offset, uint8_t bits) override {
        if (offset >= capacity_)
            throw std::out_of_range("FixedWriteBuffer::PatchByte: offset out of range");
        data_[offset] |= bits;
    }

 private:
    uint8_t* data_;
    size_t   capacity_;
    size_t   pos_ = 0;
};

}  // namespace fletcher

#endif  // FLETCHER_INCLUDE_WRITE_BUFFER_HPP_
