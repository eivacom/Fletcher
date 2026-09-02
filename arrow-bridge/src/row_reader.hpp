// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#ifndef FLETCHER_SRC_ROW_READER_HPP_
#define FLETCHER_SRC_ROW_READER_HPP_

// Internal implementation detail used by codec.cpp.  Not part of the public API.

#include <arrow/api.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

namespace fletcher {
namespace detail {

struct Reader {
    const uint8_t* data;
    size_t size;
    size_t pos{0};

    // Bytes not yet consumed. Invariant: pos <= size, so this never underflows.
    size_t remaining() const { return size - pos; }

    template <typename T>
    T Read() {
        // Overflow-safe form of `pos + sizeof(T) > size` (pos <= size always).
        if (sizeof(T) > size - pos) throw std::invalid_argument("fletcher: buffer underrun");
        T value;
        std::memcpy(&value, data + pos, sizeof(T));
        pos += sizeof(T);
        return value;
    }

    const uint8_t* ReadBytes(size_t n) {
        // Overflow-safe: a wrapping `pos + n` could otherwise pass the check
        // for an attacker-controlled length and allow an out-of-bounds read.
        if (n > size - pos) throw std::invalid_argument("fletcher: buffer underrun");
        const uint8_t* ptr = data + pos;
        pos += n;
        return ptr;
    }
};

// Number of bytes needed for a null bitfield covering `n` items.
// Takes int64_t so a wire-supplied uint32_t count never narrows to a negative
// int (which would make the (n + 7) / 8 arithmetic produce garbage).
inline size_t BitfieldBytes(int64_t n) { return static_cast<size_t>((n + 7) / 8); }

// bit i of `bitfield` (LSB-first within each byte).
inline bool ReadNullBit(const uint8_t* bitfield, int index) {
    return (bitfield[index / 8] >> (index % 8)) & 1u;
}

// True if none of the first `count` bits is set (no nulls).
inline bool AllValid(const uint8_t* bitfield, int64_t count) {
    const size_t bytes = BitfieldBytes(count);
    for (size_t i = 0; i < bytes; ++i)
        if (bitfield[i]) return false;
    return true;
}

// True if every one of the first `count` bits is set (all null).
inline bool AllNull(const uint8_t* bitfield, int64_t count) {
    const int64_t full_bytes = count / 8;
    for (int64_t i = 0; i < full_bytes; ++i) {
        if (bitfield[static_cast<size_t>(i)] != 0xFFu) return false;
    }
    const int rem = static_cast<int>(count % 8);
    if (rem > 0) {
        const uint8_t mask = static_cast<uint8_t>((1u << rem) - 1);
        if ((bitfield[static_cast<size_t>(full_bytes)] & mask) != mask) return false;
    }
    return true;
}

// The wire width in bytes of a fixed-width type's payload, for exactly the types AppendRun below
// knows how to bulk-append. 0 for any other type (the caller falls back to the per-element path).
inline int32_t FixedWidth(const arrow::DataType& type) {
    using T = arrow::Type;
    switch (type.id()) {
        case T::BOOL:
        case T::INT8:
        case T::UINT8:
            return 1;
        case T::INT16:
        case T::UINT16:
        case T::HALF_FLOAT:
            return 2;
        case T::INT32:
        case T::UINT32:
        case T::FLOAT:
        case T::DATE32:
        case T::TIME32:
        case T::INTERVAL_MONTHS:
            return 4;
        case T::INT64:
        case T::UINT64:
        case T::DOUBLE:
        case T::DATE64:
        case T::TIME64:
        case T::TIMESTAMP:
        case T::DURATION:
        case T::INTERVAL_DAY_TIME:
            return 8;
        case T::INTERVAL_MONTH_DAY_NANO:
        case T::DECIMAL128:
            return 16;
        case T::DECIMAL256:
            return 32;
        case T::FIXED_SIZE_BINARY:
            return static_cast<const arrow::FixedSizeBinaryType&>(type).byte_width();
        default:
            return 0;
    }
}

// The append below cannot fail on validated input (the builder was just freshly created for this
// exact type and the byte count was already bounds-checked), so a failure here is an internal
// invariant violation, not a bad-input condition.
inline void ThrowIfNotOk(const arrow::Status& st, const char* operation) {
    if (!st.ok()) throw std::runtime_error(std::string(operation) + ": " + st.ToString());
}

// Bulk-fills a NumericBuilder<ArrowType> (or a subclass, e.g. HalfFloatBuilder,
// DayTimeIntervalBuilder) from `count` little-endian wire values laid out back to back.
template <typename ArrowType>
void AppendNumericRun(arrow::ArrayBuilder& builder, const uint8_t* bytes, int64_t count) {
    using CType = typename ArrowType::c_type;
    auto& b = static_cast<arrow::NumericBuilder<ArrowType>&>(builder);
    ThrowIfNotOk(b.Reserve(count), "AppendRun: builder Reserve failed");
    std::memcpy(b.GetMutableValue(b.length()), bytes, static_cast<size_t>(count) * sizeof(CType));
    b.UnsafeAdvance(count);
}

// Appends `count` all-valid fixed-width values to `builder`, laid out contiguously as raw
// little-endian wire bytes, with ONE memcpy and no reinterpret_cast of the wire pointer. Returns
// false (and appends nothing) for any type this function does not recognise; the caller falls
// back to the per-element path in that case.
inline bool AppendRun(arrow::ArrayBuilder& builder, arrow::Type::type id, const uint8_t* bytes,
                      int64_t count) {
    using T = arrow::Type;
    switch (id) {
        case T::INT8:
            AppendNumericRun<arrow::Int8Type>(builder, bytes, count);
            return true;
        case T::INT16:
            AppendNumericRun<arrow::Int16Type>(builder, bytes, count);
            return true;
        case T::INT32:
            AppendNumericRun<arrow::Int32Type>(builder, bytes, count);
            return true;
        case T::INT64:
            AppendNumericRun<arrow::Int64Type>(builder, bytes, count);
            return true;
        case T::UINT8:
            AppendNumericRun<arrow::UInt8Type>(builder, bytes, count);
            return true;
        case T::UINT16:
            AppendNumericRun<arrow::UInt16Type>(builder, bytes, count);
            return true;
        case T::UINT32:
            AppendNumericRun<arrow::UInt32Type>(builder, bytes, count);
            return true;
        case T::UINT64:
            AppendNumericRun<arrow::UInt64Type>(builder, bytes, count);
            return true;
        case T::HALF_FLOAT:
            AppendNumericRun<arrow::HalfFloatType>(builder, bytes, count);
            return true;
        case T::FLOAT:
            AppendNumericRun<arrow::FloatType>(builder, bytes, count);
            return true;
        case T::DOUBLE:
            AppendNumericRun<arrow::DoubleType>(builder, bytes, count);
            return true;
        case T::DATE32:
            AppendNumericRun<arrow::Date32Type>(builder, bytes, count);
            return true;
        case T::DATE64:
            AppendNumericRun<arrow::Date64Type>(builder, bytes, count);
            return true;
        case T::TIME32:
            AppendNumericRun<arrow::Time32Type>(builder, bytes, count);
            return true;
        case T::TIME64:
            AppendNumericRun<arrow::Time64Type>(builder, bytes, count);
            return true;
        case T::TIMESTAMP:
            AppendNumericRun<arrow::TimestampType>(builder, bytes, count);
            return true;
        case T::DURATION:
            AppendNumericRun<arrow::DurationType>(builder, bytes, count);
            return true;
        case T::INTERVAL_MONTHS:
            AppendNumericRun<arrow::MonthIntervalType>(builder, bytes, count);
            return true;
        case T::INTERVAL_DAY_TIME:
            AppendNumericRun<arrow::DayTimeIntervalType>(builder, bytes, count);
            return true;
        case T::INTERVAL_MONTH_DAY_NANO:
            AppendNumericRun<arrow::MonthDayNanoIntervalType>(builder, bytes, count);
            return true;
        case T::BOOL: {
            auto st = static_cast<arrow::BooleanBuilder&>(builder).AppendValues(bytes, count);
            ThrowIfNotOk(st, "AppendRun: BooleanBuilder AppendValues failed");
            return true;
        }
        case T::DECIMAL128:
        case T::DECIMAL256:
        case T::FIXED_SIZE_BINARY: {
            auto st =
                static_cast<arrow::FixedSizeBinaryBuilder&>(builder).AppendValues(bytes, count);
            ThrowIfNotOk(st, "AppendRun: FixedSizeBinaryBuilder AppendValues failed");
            return true;
        }
        default:
            return false;
    }
}

}  // namespace detail
}  // namespace fletcher

#endif  // FLETCHER_SRC_ROW_READER_HPP_
