// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#ifndef FLETCHER_INCLUDE_CORE_POSITIONAL_IO_HPP_
#define FLETCHER_INCLUDE_CORE_POSITIONAL_IO_HPP_

// Header-only positional wire format reader/writer on WriteBuffer.
//
// Produces the same byte layout as Codec (codec.hpp):
//   Struct/Row: [NULL_BITFIELD : ceil(n/8)] [payloads for non-null fields...]
//   Scalar:     fixed-width LE, or [LEN:4 LE][DATA] for variable-length
//   List:       [COUNT:4 LE] [ELEM_NULL_BITFIELD : ceil(count/8)] [payloads...]
//   Map:        [COUNT:4 LE] [key payloads...] [VAL_NULL_BITFIELD] [val payloads...]
//
// Depends only on WriteBuffer (no Arrow C++ dependency).

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include "write_buffer.hpp"

namespace fletcher {

static_assert(std::endian::native == std::endian::little,
              "Positional wire format assumes little-endian host");

// ---------------------------------------------------------------------------
// PositionalWriter — writes positional wire format into a WriteBuffer.
// ---------------------------------------------------------------------------

class PositionalWriter {
   public:
    // Start a struct/row with num_fields fields.
    // Writes a zeroed null bitfield placeholder.
    PositionalWriter(WriteBuffer& buf, int num_fields)
        : buf_(buf), num_fields_(num_fields), bitfield_offset_(buf.Position()) {
        if (num_fields < 0)
            throw std::invalid_argument("PositionalWriter: num_fields must be >= 0");
        size_t nbytes = BitfieldBytes(num_fields);
        for (size_t i = 0; i < nbytes; ++i) buf_.AppendByte(0);
    }

    // Mark field at field_index as null.  Call before writing payloads.
    void SetNull(int field_index) {
        if (field_index < 0 || field_index >= num_fields_)
            throw std::out_of_range("PositionalWriter::SetNull: field_index out of range");
        size_t byte_offset = bitfield_offset_ + static_cast<size_t>(field_index / 8);
        uint8_t bit = static_cast<uint8_t>(1u << (field_index % 8));
        buf_.PatchByte(byte_offset, bit);
    }

    // --- Scalar writers (fixed-width) ---

    void WriteInt8(int8_t v) { buf_.AppendFixed(v); }
    void WriteInt16(int16_t v) { buf_.AppendFixed(v); }
    void WriteInt32(int32_t v) { buf_.AppendFixed(v); }
    void WriteInt64(int64_t v) { buf_.AppendFixed(v); }
    void WriteUint8(uint8_t v) { buf_.AppendByte(v); }
    void WriteUint16(uint16_t v) { buf_.AppendFixed(v); }
    void WriteUint32(uint32_t v) { buf_.AppendFixed(v); }
    void WriteUint64(uint64_t v) { buf_.AppendFixed(v); }
    void WriteFloat(float v) { buf_.AppendFixed(v); }
    void WriteDouble(double v) { buf_.AppendFixed(v); }
    void WriteBool(bool v) { buf_.AppendByte(v ? 1u : 0u); }

    // Timestamp/Duration are int64 on the wire.
    void WriteTimestamp(int64_t v) { buf_.AppendFixed(v); }
    void WriteDuration(int64_t v) { buf_.AppendFixed(v); }

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
    PositionalWriter BeginStruct(int num_fields) { return PositionalWriter(buf_, num_fields); }

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
        size_t nbytes = BitfieldBytes(count);
        for (size_t i = 0; i < nbytes; ++i) buf_.AppendByte(0);
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
            for (size_t i = 0; i < nbytes; ++i) buf.AppendByte(0);
            return ListContext{buf, bf_offset, count};
        }
    };

    MapContext BeginMap(uint32_t count) {
        buf_.AppendFixed(count);
        return MapContext{buf_, count};
    }

    WriteBuffer& buf() { return buf_; }

   private:
    static size_t BitfieldBytes(size_t n) { return (n + 7) / 8; }

    WriteBuffer& buf_;
    int num_fields_;
    size_t bitfield_offset_;
};

// ---------------------------------------------------------------------------
// AppendTrailingUint64Field — stamp a reserved UInt64 system column (`_ingest_offset`).
// ---------------------------------------------------------------------------
//
// Appends one trailing, non-null, **UInt64** field to an already-encoded positional
// row, returning the bytes re-encoded against the schema that has this one extra
// field appended at the END (it becomes field index `base_num_fields`).
//
// The motivating use is stamping the RowBatcher's reserved monotonic
// **`_ingest_offset`** system column onto a row whose data fields were produced
// upstream, WITHOUT decoding and re-encoding. `_ingest_offset` is a monotonic
// counter that never goes negative, so it is always UInt64 (non-nullable) and this
// is fixed to `uint64_t` rather than templated — the wire type must match the column
// declared in the schema, and there is exactly one such column. It is format-aware
// so callers never reproduce the wire layout, which carries no version tag (see
// codec.hpp):
//
//   - The leading null bitfield is ceil(num_fields / 8) bytes. Adding a field grows
//     it by one byte ONLY when `base_num_fields` is a multiple of 8; otherwise the
//     new field reuses the next unused bit (position `base_num_fields % 8`) of the
//     last bitfield byte and the result is a pure trailing append.
//   - The new field is always non-null; its null bit is cleared explicitly, so a
//     stray spare bit in a foreign encoding cannot make it decode as NULL. NO
//     existing field's bit changes; existing field payloads are copied verbatim.
//   - A UInt64 has no length prefix, so its payload is just the 8 little-endian
//     value bytes, written last (it is the final, non-null field).
//
// `row`/`row_len` must be a well-formed positional encoding of `base_num_fields`
// fields. Decode the result with a Codec built for the schema that has
// `_ingest_offset` (UInt64) appended as its last field.
inline std::vector<uint8_t> AppendTrailingUint64Field(const uint8_t* row, size_t row_len,
                                                      int base_num_fields, uint64_t ingest_offset) {
    if (base_num_fields < 0)
        throw std::invalid_argument("AppendTrailingUint64Field: base_num_fields must be >= 0");

    const size_t old_bitfield = (static_cast<size_t>(base_num_fields) + 7) / 8;
    const size_t new_bitfield = (static_cast<size_t>(base_num_fields) + 1 + 7) / 8;
    if (row_len < old_bitfield)
        throw std::invalid_argument(
            "AppendTrailingUint64Field: row shorter than its null bitfield");

    std::vector<uint8_t> out;
    out.reserve(row_len + (new_bitfield - old_bitfield) + sizeof(uint64_t));

    // Leading bitfield bytes — existing fields keep their bits unchanged.
    out.insert(out.end(), row, row + old_bitfield);
    // If the bitfield grew, the new field's bit lives in a fresh zero byte.
    out.insert(out.end(), new_bitfield - old_bitfield, static_cast<uint8_t>(0));
    // The appended field is non-null: clear its null bit at (base_num_fields % 8)
    // of byte (base_num_fields / 8). When the bitfield grew that byte is already
    // zero; when it did not, this defends against a stray spare bit in a foreign
    // encoding that would otherwise decode as NULL and desynchronise the trailing
    // 8 bytes under schema decode.
    out[static_cast<size_t>(base_num_fields) / 8] &=
        static_cast<uint8_t>(~(1u << (static_cast<size_t>(base_num_fields) % 8)));
    // Existing field payloads, verbatim.
    out.insert(out.end(), row + old_bitfield, row + row_len);
    // The `_ingest_offset` payload: 8 little-endian bytes, written last (non-null).
    // memcpy mirrors WriteBuffer::AppendFixed's object-representation copy, so the
    // schema-decode path reads these bytes exactly as any other fixed-width field.
    const size_t value_pos = out.size();
    out.resize(value_pos + sizeof(uint64_t));
    std::memcpy(out.data() + value_pos, &ingest_offset, sizeof(uint64_t));
    return out;
}

// Convenience overload for a byte-vector row (e.g. an EncodedRow).
inline std::vector<uint8_t> AppendTrailingUint64Field(const std::vector<uint8_t>& row,
                                                      int base_num_fields, uint64_t ingest_offset) {
    return AppendTrailingUint64Field(row.data(), row.size(), base_num_fields, ingest_offset);
}

// ---------------------------------------------------------------------------
// ReadTrailingUint64Field — the read-side counterpart of AppendTrailingUint64Field.
// ---------------------------------------------------------------------------
//
// Returns the trailing `_ingest_offset` value of a row produced by
// AppendTrailingUint64Field. Because that field is the LAST, non-null, fixed-width
// field, its 8 little-endian bytes are exactly the final 8 bytes of the row — so
// this is an O(1) read with NO schema and NO full decode.
//
// This is the **no-schema fast path**, for consumers that need only the offset
// (dedup key, stream cursor, WAL-offset scan, reconciliation). It is valid ONLY for
// rows whose schema keeps a non-null UInt64 as the last field — the matched
// counterpart to AppendTrailingUint64Field; do not call it on an arbitrary row.
//
// **WITH A SCHEMA in play, prefer the decode path:** `Codec::DecodeRow(row)` against
// the registered schema yields an `ArrowRow`, and `_ingest_offset` is its last field
// (read it as an `arrow::UInt64Scalar`). Use that whenever you need the row anyway —
// it is schema-validated. Both paths return the same value by construction (see the
// `IngestOffsetTrailingField*` tests in arrow-bridge/tests/test_codec.cpp).
inline uint64_t ReadTrailingUint64Field(const uint8_t* row, size_t row_len) {
    if (row_len < sizeof(uint64_t))
        throw std::invalid_argument("ReadTrailingUint64Field: row shorter than a uint64");
    uint64_t value;
    std::memcpy(&value, row + row_len - sizeof(uint64_t), sizeof(uint64_t));
    return value;
}

// Convenience overload for a byte-vector row (e.g. an EncodedRow).
inline uint64_t ReadTrailingUint64Field(const std::vector<uint8_t>& row) {
    return ReadTrailingUint64Field(row.data(), row.size());
}

// ---------------------------------------------------------------------------
// PositionalReader — reads positional wire format from a byte buffer.
// ---------------------------------------------------------------------------

class PositionalReader {
   public:
    PositionalReader(const uint8_t* data, size_t len, int num_fields)
        : data_(data), len_(len), pos_(0), num_fields_(num_fields) {
        // Read the null bitfield.
        size_t nbytes = BitfieldBytes(static_cast<size_t>(num_fields));
        if (nbytes > len_ - pos_)
            throw std::invalid_argument("PositionalReader: buffer underrun (bitfield)");
        bitfield_ = data_ + pos_;
        pos_ += nbytes;
    }

    [[nodiscard]] bool IsNull(int field_index) const {
        if (field_index < 0 || field_index >= num_fields_)
            throw std::out_of_range("PositionalReader::IsNull: field_index out of range");
        return (bitfield_[field_index / 8] >> (field_index % 8)) & 1u;
    }

    // --- Scalar readers (fixed-width) ---

    int8_t ReadInt8() { return Read<int8_t>(); }
    int16_t ReadInt16() { return Read<int16_t>(); }
    int32_t ReadInt32() { return Read<int32_t>(); }
    int64_t ReadInt64() { return Read<int64_t>(); }
    uint8_t ReadUint8() { return Read<uint8_t>(); }
    uint16_t ReadUint16() { return Read<uint16_t>(); }
    uint32_t ReadUint32() { return Read<uint32_t>(); }
    uint64_t ReadUint64() { return Read<uint64_t>(); }
    float ReadFloat() { return Read<float>(); }
    double ReadDouble() { return Read<double>(); }
    bool ReadBool() { return Read<uint8_t>() != 0; }

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
        // Reject a corrupt/oversized count before the caller loops `count`
        // times. List elements can be null, and a null element has no payload
        // bytes, so a valid all-null list's count can legitimately exceed the
        // remaining byte count. The only safe lower bound is the element null
        // bitfield itself: require BitfieldBytes(count) to fit.
        if (BitfieldBytes(count) > Remaining())
            throw std::invalid_argument("PositionalReader: list count exceeds remaining buffer");
        const uint8_t* bf = ReadBytes(BitfieldBytes(count));
        return ListHeader{count, bf};
    }

    // Read a map header: COUNT (no bitfield — keys are never null).
    // Caller reads key payloads, then calls ReadMapValueBitfield().
    uint32_t ReadMapCount() {
        uint32_t count = Read<uint32_t>();
        // Map keys are non-null, so every entry needs at least one key byte:
        // count > remaining is a valid (tight) lower bound here, unlike lists.
        if (count > Remaining())
            throw std::invalid_argument("PositionalReader: map count exceeds remaining buffer");
        return count;
    }

    const uint8_t* ReadMapValueBitfield(uint32_t count) { return ReadBytes(BitfieldBytes(count)); }

    // Bytes consumed so far.
    size_t BytesConsumed() const { return pos_; }

    // Remaining bytes.
    size_t Remaining() const { return len_ - pos_; }

    // Throws if this reader has not consumed its entire buffer. Trailing bytes
    // mean the payload does not match the schema (corruption, a length-prefix
    // bug, or a padded/concatenated buffer). Generated message constructors
    // should call this on the top-level reader after reading all fields.
    void VerifyFullyConsumed() const {
        if (pos_ != len_)
            throw std::invalid_argument("PositionalReader: buffer not fully consumed");
    }

    // Called when a sub-reader (from ReadStruct) is done, to advance
    // the parent's position.
    ~PositionalReader() {
        if (parent_) parent_->pos_ += pos_;
    }

    // Move only (parent pointer).
    PositionalReader(PositionalReader&& o) noexcept
        : data_(o.data_),
          len_(o.len_),
          pos_(o.pos_),
          num_fields_(o.num_fields_),
          bitfield_(o.bitfield_),
          parent_(o.parent_) {
        o.parent_ = nullptr;  // prevent double-advance
    }
    PositionalReader& operator=(PositionalReader&&) = delete;
    PositionalReader(const PositionalReader&) = delete;
    PositionalReader& operator=(const PositionalReader&) = delete;

   private:
    // Sub-reader constructor (tracks parent for position advance).
    PositionalReader(const uint8_t* data, size_t len, int num_fields, PositionalReader* parent)
        : data_(data), len_(len), pos_(0), num_fields_(num_fields), parent_(parent) {
        size_t nbytes = BitfieldBytes(static_cast<size_t>(num_fields));
        if (nbytes > len_ - pos_)
            throw std::invalid_argument("PositionalReader: buffer underrun (struct bitfield)");
        bitfield_ = data_ + pos_;
        pos_ += nbytes;
    }

    template <typename T>
    T Read() {
        // Overflow-safe form of `pos_ + sizeof(T) > len_` (pos_ <= len_ always).
        if (sizeof(T) > len_ - pos_)
            throw std::invalid_argument("PositionalReader: buffer underrun");
        T value;
        std::memcpy(&value, data_ + pos_, sizeof(T));
        pos_ += sizeof(T);
        return value;
    }

    const uint8_t* ReadBytes(size_t n) {
        // Overflow-safe: a wrapping `pos_ + n` could pass for an
        // attacker-controlled length and permit an out-of-bounds read.
        if (n > len_ - pos_) throw std::invalid_argument("PositionalReader: buffer underrun");
        const uint8_t* ptr = data_ + pos_;
        pos_ += n;
        return ptr;
    }

    // size_t param so a wire-supplied uint32_t count never narrows to a
    // negative int before the (n + 7) / 8 arithmetic.
    static size_t BitfieldBytes(size_t n) { return (n + 7) / 8; }

    const uint8_t* data_;
    size_t len_;
    size_t pos_;
    int num_fields_;
    const uint8_t* bitfield_ = nullptr;
    PositionalReader* parent_ = nullptr;
};

}  // namespace fletcher

#endif  // FLETCHER_INCLUDE_CORE_POSITIONAL_IO_HPP_
