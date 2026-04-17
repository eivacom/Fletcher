#ifndef FLETCHER_INCLUDE_POSITIONAL_IO_HPP_
#define FLETCHER_INCLUDE_POSITIONAL_IO_HPP_

// Header-only positional wire format reader/writer on WriteBuffer.
//
// Produces the same byte layout as PositionalCodec (positional_codec.hpp):
//   Struct/Row: [NULL_BITFIELD : ceil(n/8)] [payloads for non-null fields...]
//   Scalar:     fixed-width LE, or [LEN:4 LE][DATA] for variable-length
//   List:       [COUNT:4 LE] [ELEM_NULL_BITFIELD : ceil(count/8)] [payloads...]
//   Map:        [COUNT:4 LE] [key payloads...] [VAL_NULL_BITFIELD] [val payloads...]
//
// Depends only on WriteBuffer (no Arrow C++ dependency).

#include "write_buffer.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace fletcher {

// ---------------------------------------------------------------------------
// PositionalWriter — writes positional wire format into a WriteBuffer.
// ---------------------------------------------------------------------------

class PositionalWriter {
 public:
    // Start a struct/row with num_fields fields.
    // Writes a zeroed null bitfield placeholder.
    PositionalWriter(WriteBuffer& buf, int num_fields)
        : buf_(buf)
        , num_fields_(num_fields)
        , bitfield_offset_(buf.Position()) {
        // Write placeholder null bitfield (all zeros = all non-null).
        size_t nbytes = BitfieldBytes(num_fields);
        for (size_t i = 0; i < nbytes; ++i)
            buf_.AppendByte(0);
    }

    // Mark field at field_index as null.  Call before writing payloads.
    void SetNull(int field_index) {
        size_t byte_offset = bitfield_offset_ + field_index / 8;
        uint8_t bit = static_cast<uint8_t>(1u << (field_index % 8));
        buf_.PatchByte(byte_offset, bit);
    }

    // --- Scalar writers (fixed-width) ---

    void WriteInt8(int8_t v)        { buf_.AppendFixed(v); }
    void WriteInt16(int16_t v)      { buf_.AppendFixed(v); }
    void WriteInt32(int32_t v)      { buf_.AppendFixed(v); }
    void WriteInt64(int64_t v)      { buf_.AppendFixed(v); }
    void WriteUint8(uint8_t v)      { buf_.AppendByte(v); }
    void WriteUint16(uint16_t v)    { buf_.AppendFixed(v); }
    void WriteUint32(uint32_t v)    { buf_.AppendFixed(v); }
    void WriteUint64(uint64_t v)    { buf_.AppendFixed(v); }
    void WriteFloat(float v)        { buf_.AppendFixed(v); }
    void WriteDouble(double v)      { buf_.AppendFixed(v); }
    void WriteBool(bool v)          { buf_.AppendByte(v ? 1u : 0u); }

    // Timestamp/Duration are int64 on the wire.
    void WriteTimestamp(int64_t v)  { buf_.AppendFixed(v); }
    void WriteDuration(int64_t v)   { buf_.AppendFixed(v); }

    // --- Variable-length writers ---

    void WriteString(std::string_view s) {
        auto len = static_cast<uint32_t>(s.size());
        buf_.AppendFixed(len);
        buf_.Append(reinterpret_cast<const uint8_t*>(s.data()), len);
    }

    void WriteBinary(const uint8_t* data, size_t len) {
        buf_.AppendFixed(static_cast<uint32_t>(len));
        buf_.Append(data, len);
    }

    // --- Composite writers ---

    // Begin a nested struct with num_fields child fields.
    // Returns a sub-writer; caller writes child payloads through it.
    PositionalWriter BeginStruct(int num_fields) {
        return PositionalWriter(buf_, num_fields);
    }

    // Begin a list.  Writes COUNT, then a null bitfield placeholder for
    // `count` elements.  Returns a ListContext for writing elements.
    struct ListContext {
        WriteBuffer& buf;
        size_t bitfield_offset;
        uint32_t count;

        void SetElementNull(uint32_t index) {
            size_t byte_offset = bitfield_offset + index / 8;
            uint8_t bit = static_cast<uint8_t>(1u << (index % 8));
            buf.PatchByte(byte_offset, bit);
        }
    };

    ListContext BeginList(uint32_t count) {
        buf_.AppendFixed(count);
        size_t bf_offset = buf_.Position();
        size_t nbytes = BitfieldBytes(static_cast<int>(count));
        for (size_t i = 0; i < nbytes; ++i)
            buf_.AppendByte(0);
        return ListContext{buf_, bf_offset, count};
    }

    // Begin a map.  Writes COUNT.  Caller writes key payloads, then calls
    // BeginMapValues() to get the value null bitfield context.
    struct MapContext {
        WriteBuffer& buf;
        uint32_t count;

        // Call after writing all key payloads.  Writes value null bitfield.
        ListContext BeginValues() {
            size_t bf_offset = buf.Position();
            size_t nbytes = (count + 7) / 8;
            for (size_t i = 0; i < nbytes; ++i)
                buf.AppendByte(0);
            return ListContext{buf, bf_offset, count};
        }
    };

    MapContext BeginMap(uint32_t count) {
        buf_.AppendFixed(count);
        return MapContext{buf_, count};
    }

    WriteBuffer& buf() { return buf_; }

 private:
    static size_t BitfieldBytes(int n) {
        return static_cast<size_t>((n + 7) / 8);
    }

    WriteBuffer& buf_;
    int num_fields_;
    size_t bitfield_offset_;
};

// ---------------------------------------------------------------------------
// PositionalReader — reads positional wire format from a byte buffer.
// ---------------------------------------------------------------------------

class PositionalReader {
 public:
    PositionalReader(const uint8_t* data, size_t len, int num_fields)
        : data_(data), len_(len), pos_(0), num_fields_(num_fields) {
        // Read the null bitfield.
        size_t nbytes = BitfieldBytes(num_fields);
        if (pos_ + nbytes > len_)
            throw std::invalid_argument("PositionalReader: buffer underrun (bitfield)");
        bitfield_ = data_ + pos_;
        pos_ += nbytes;
    }

    bool IsNull(int field_index) const {
        return (bitfield_[field_index / 8] >> (field_index % 8)) & 1u;
    }

    // --- Scalar readers (fixed-width) ---

    int8_t   ReadInt8()    { return Read<int8_t>(); }
    int16_t  ReadInt16()   { return Read<int16_t>(); }
    int32_t  ReadInt32()   { return Read<int32_t>(); }
    int64_t  ReadInt64()   { return Read<int64_t>(); }
    uint8_t  ReadUint8()   { return Read<uint8_t>(); }
    uint16_t ReadUint16()  { return Read<uint16_t>(); }
    uint32_t ReadUint32()  { return Read<uint32_t>(); }
    uint64_t ReadUint64()  { return Read<uint64_t>(); }
    float    ReadFloat()   { return Read<float>(); }
    double   ReadDouble()  { return Read<double>(); }
    bool     ReadBool()    { return Read<uint8_t>() != 0; }

    int64_t ReadTimestamp() { return Read<int64_t>(); }
    int64_t ReadDuration() { return Read<int64_t>(); }

    // --- Variable-length readers ---

    std::string_view ReadString() {
        uint32_t len = Read<uint32_t>();
        const uint8_t* p = ReadBytes(len);
        return {reinterpret_cast<const char*>(p), len};
    }

    std::pair<const uint8_t*, size_t> ReadBinary() {
        uint32_t len = Read<uint32_t>();
        const uint8_t* p = ReadBytes(len);
        return {p, len};
    }

    // --- Composite readers ---

    // Read a nested struct with num_fields child fields.
    PositionalReader ReadStruct(int num_fields) {
        // The struct's data starts at our current position and runs to
        // the end of the buffer.  The sub-reader will advance pos_ for us
        // via SubReaderFinished().
        return PositionalReader(data_ + pos_, len_ - pos_, num_fields, this);
    }

    // Read a list header: COUNT + element null bitfield.
    struct ListHeader {
        uint32_t count;
        const uint8_t* elem_bitfield;

        bool IsElementNull(uint32_t index) const {
            return (elem_bitfield[index / 8] >> (index % 8)) & 1u;
        }
    };

    ListHeader ReadListHeader() {
        uint32_t count = Read<uint32_t>();
        size_t nbytes = BitfieldBytes(static_cast<int>(count));
        const uint8_t* bf = ReadBytes(nbytes);
        return ListHeader{count, bf};
    }

    // Read a map header: COUNT (no bitfield — keys are never null).
    // Caller reads key payloads, then calls ReadMapValueBitfield().
    uint32_t ReadMapCount() { return Read<uint32_t>(); }

    const uint8_t* ReadMapValueBitfield(uint32_t count) {
        size_t nbytes = BitfieldBytes(static_cast<int>(count));
        return ReadBytes(nbytes);
    }

    // Bytes consumed so far.
    size_t BytesConsumed() const { return pos_; }

    // Remaining bytes.
    size_t Remaining() const { return len_ - pos_; }

    // Called when a sub-reader (from ReadStruct) is done, to advance
    // the parent's position.
    ~PositionalReader() {
        if (parent_)
            parent_->pos_ += pos_;
    }

    // Move only (parent pointer).
    PositionalReader(PositionalReader&& o) noexcept
        : data_(o.data_), len_(o.len_), pos_(o.pos_),
          num_fields_(o.num_fields_), bitfield_(o.bitfield_),
          parent_(o.parent_) {
        o.parent_ = nullptr;  // prevent double-advance
    }
    PositionalReader& operator=(PositionalReader&&) = delete;
    PositionalReader(const PositionalReader&) = delete;
    PositionalReader& operator=(const PositionalReader&) = delete;

 private:
    // Sub-reader constructor (tracks parent for position advance).
    PositionalReader(const uint8_t* data, size_t len, int num_fields,
                     PositionalReader* parent)
        : data_(data), len_(len), pos_(0), num_fields_(num_fields),
          parent_(parent) {
        size_t nbytes = BitfieldBytes(num_fields);
        if (pos_ + nbytes > len_)
            throw std::invalid_argument("PositionalReader: buffer underrun (struct bitfield)");
        bitfield_ = data_ + pos_;
        pos_ += nbytes;
    }

    template <typename T>
    T Read() {
        if (pos_ + sizeof(T) > len_)
            throw std::invalid_argument("PositionalReader: buffer underrun");
        T value;
        std::memcpy(&value, data_ + pos_, sizeof(T));
        pos_ += sizeof(T);
        return value;
    }

    const uint8_t* ReadBytes(size_t n) {
        if (pos_ + n > len_)
            throw std::invalid_argument("PositionalReader: buffer underrun");
        const uint8_t* ptr = data_ + pos_;
        pos_ += n;
        return ptr;
    }

    static size_t BitfieldBytes(int n) {
        return static_cast<size_t>((n + 7) / 8);
    }

    const uint8_t* data_;
    size_t len_;
    size_t pos_;
    int num_fields_;
    const uint8_t* bitfield_ = nullptr;
    PositionalReader* parent_ = nullptr;
};

}  // namespace fletcher

#endif  // FLETCHER_INCLUDE_POSITIONAL_IO_HPP_
