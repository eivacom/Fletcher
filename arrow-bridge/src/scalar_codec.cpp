// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include "scalar_codec.hpp"

#include <arrow/scalar.h>
#include <arrow/type.h>

#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

namespace fletcher {
namespace {

template <typename T>
void AppendFixed(std::vector<uint8_t>& buf, T value) {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&value);
    buf.insert(buf.end(), bytes, bytes + sizeof(T));
}

void AppendVariableLength(std::vector<uint8_t>& buf, const uint8_t* data, uint32_t len) {
    AppendFixed(buf, len);
    buf.insert(buf.end(), data, data + len);
}

// Dictionary columns are transferred as their value type per row (the indices
// are a columnar optimisation that is reconstructed by the RecordBatch
// subscriber, not sent on the wire). Only scalar value types are supported.
void EnsureDictionaryValueSupported(const arrow::DataType& value_type) {
    using T = arrow::Type;
    switch (value_type.id()) {
        case T::STRUCT:
        case T::LIST:
        case T::LARGE_LIST:
        case T::FIXED_SIZE_LIST:
        case T::MAP:
        case T::SPARSE_UNION:
        case T::DENSE_UNION:
        case T::DICTIONARY:
            throw std::invalid_argument(
                "Fletcher dictionary support: the dictionary value type must be a "
                "primitive/scalar type (got " +
                value_type.ToString() +
                "). Nested dictionary value types (struct/list/map/union/nested dictionary) "
                "are not supported.");
        default:
            break;
    }
}

}  // namespace

namespace detail {

void EncodeScalar(std::vector<uint8_t>& buf, const arrow::Scalar& scalar) {
    using T = arrow::Type;

    switch (scalar.type->id()) {
        case T::BOOL: {
            auto v = static_cast<const arrow::BooleanScalar&>(scalar).value;
            AppendFixed<uint8_t>(buf, v ? 1u : 0u);
            break;
        }
        case T::INT8:
            AppendFixed(buf, static_cast<const arrow::Int8Scalar&>(scalar).value);
            break;
        case T::INT16:
            AppendFixed(buf, static_cast<const arrow::Int16Scalar&>(scalar).value);
            break;
        case T::INT32:
            AppendFixed(buf, static_cast<const arrow::Int32Scalar&>(scalar).value);
            break;
        case T::INT64:
            AppendFixed(buf, static_cast<const arrow::Int64Scalar&>(scalar).value);
            break;
        case T::UINT8:
            AppendFixed(buf, static_cast<const arrow::UInt8Scalar&>(scalar).value);
            break;
        case T::UINT16:
            AppendFixed(buf, static_cast<const arrow::UInt16Scalar&>(scalar).value);
            break;
        case T::UINT32:
            AppendFixed(buf, static_cast<const arrow::UInt32Scalar&>(scalar).value);
            break;
        case T::UINT64:
            AppendFixed(buf, static_cast<const arrow::UInt64Scalar&>(scalar).value);
            break;
        case T::FLOAT:
            AppendFixed(buf, static_cast<const arrow::FloatScalar&>(scalar).value);
            break;
        case T::DOUBLE:
            AppendFixed(buf, static_cast<const arrow::DoubleScalar&>(scalar).value);
            break;

        case T::STRING:
        case T::LARGE_STRING:
        case T::BINARY:
        case T::LARGE_BINARY:
        case T::STRING_VIEW:
        case T::BINARY_VIEW: {
            const auto& s = static_cast<const arrow::BaseBinaryScalar&>(scalar);
            const int64_t raw_len = s.value->size();
            if (raw_len < 0 || raw_len > static_cast<int64_t>(std::numeric_limits<uint32_t>::max()))
                throw std::invalid_argument(
                    "EncodeScalar: variable-length field exceeds 4 GiB limit");
            AppendVariableLength(buf, reinterpret_cast<const uint8_t*>(s.value->data()),
                                 static_cast<uint32_t>(raw_len));
            break;
        }

        case T::DATE32:
            AppendFixed(buf, static_cast<const arrow::Date32Scalar&>(scalar).value);
            break;
        case T::DATE64:
            AppendFixed(buf, static_cast<const arrow::Date64Scalar&>(scalar).value);
            break;
        case T::TIMESTAMP:
            AppendFixed(buf, static_cast<const arrow::TimestampScalar&>(scalar).value);
            break;
        case T::TIME32:
            AppendFixed(buf, static_cast<const arrow::Time32Scalar&>(scalar).value);
            break;
        case T::TIME64:
            AppendFixed(buf, static_cast<const arrow::Time64Scalar&>(scalar).value);
            break;
        case T::DURATION:
            AppendFixed(buf, static_cast<const arrow::DurationScalar&>(scalar).value);
            break;
        case T::FIXED_SIZE_BINARY: {
            // Arrow's FixedSizeBinaryScalar constructor enforces
            // value->size() == byte_width (a hard CHECK), so a scalar reaching
            // here always has a buffer of exactly the declared width; no extra
            // size guard is needed (and the undersized case is unconstructable).
            const auto& s = static_cast<const arrow::FixedSizeBinaryScalar&>(scalar);
            const int32_t byte_width =
                static_cast<const arrow::FixedSizeBinaryType&>(*scalar.type).byte_width();
            const auto* data = reinterpret_cast<const uint8_t*>(s.value->data());
            buf.insert(buf.end(), data, data + byte_width);
            break;
        }
        case T::HALF_FLOAT:
            AppendFixed(buf, static_cast<const arrow::HalfFloatScalar&>(scalar).value);
            break;
        case T::INTERVAL_MONTHS:
            AppendFixed(buf, static_cast<const arrow::MonthIntervalScalar&>(scalar).value);
            break;
        case T::INTERVAL_DAY_TIME: {
            const auto& v = static_cast<const arrow::DayTimeIntervalScalar&>(scalar).value;
            AppendFixed(buf, v.days);
            AppendFixed(buf, v.milliseconds);
            break;
        }
        case T::INTERVAL_MONTH_DAY_NANO: {
            const auto& v = static_cast<const arrow::MonthDayNanoIntervalScalar&>(scalar).value;
            AppendFixed(buf, v.months);
            AppendFixed(buf, v.days);
            AppendFixed(buf, v.nanoseconds);
            break;
        }
        case T::DECIMAL128: {
            const auto& v = static_cast<const arrow::Decimal128Scalar&>(scalar).value;
            uint8_t bytes[16];
            v.ToBytes(bytes);
            buf.insert(buf.end(), bytes, bytes + 16);
            break;
        }
        case T::DECIMAL256: {
            const auto& v = static_cast<const arrow::Decimal256Scalar&>(scalar).value;
            uint8_t bytes[32];
            v.ToBytes(bytes);
            buf.insert(buf.end(), bytes, bytes + 32);
            break;
        }

        case T::DICTIONARY: {
            // Encode the underlying value (resolve index -> value), not the index.
            const auto& dict_type = static_cast<const arrow::DictionaryType&>(*scalar.type);
            EnsureDictionaryValueSupported(*dict_type.value_type());
            auto value = static_cast<const arrow::DictionaryScalar&>(scalar).GetEncodedValue();
            if (!value.ok())
                throw std::invalid_argument("EncodeScalar: cannot resolve dictionary value: " +
                                            value.status().ToString());
            EncodeScalar(buf, **value);
            break;
        }

        default:
            throw std::invalid_argument("EncodeScalar: unsupported Arrow type: " +
                                        scalar.type->ToString());
    }
}

std::shared_ptr<arrow::Scalar> DecodeScalarFromReader(
    Reader& r, const std::shared_ptr<arrow::DataType>& type) {
    using T = arrow::Type;
    switch (type->id()) {
        case T::BOOL:
            return std::make_shared<arrow::BooleanScalar>(r.Read<uint8_t>() != 0);
        case T::INT8:
            return std::make_shared<arrow::Int8Scalar>(r.Read<int8_t>());
        case T::INT16:
            return std::make_shared<arrow::Int16Scalar>(r.Read<int16_t>());
        case T::INT32:
            return std::make_shared<arrow::Int32Scalar>(r.Read<int32_t>());
        case T::INT64:
            return std::make_shared<arrow::Int64Scalar>(r.Read<int64_t>());
        case T::UINT8:
            return std::make_shared<arrow::UInt8Scalar>(r.Read<uint8_t>());
        case T::UINT16:
            return std::make_shared<arrow::UInt16Scalar>(r.Read<uint16_t>());
        case T::UINT32:
            return std::make_shared<arrow::UInt32Scalar>(r.Read<uint32_t>());
        case T::UINT64:
            return std::make_shared<arrow::UInt64Scalar>(r.Read<uint64_t>());
        case T::FLOAT:
            return std::make_shared<arrow::FloatScalar>(r.Read<float>());
        case T::DOUBLE:
            return std::make_shared<arrow::DoubleScalar>(r.Read<double>());

        case T::STRING:
        case T::LARGE_STRING:
        case T::BINARY:
        case T::LARGE_BINARY:
        case T::STRING_VIEW:
        case T::BINARY_VIEW: {
            uint32_t len = r.Read<uint32_t>();
            const uint8_t* ptr = r.ReadBytes(len);
            auto ibuf =
                arrow::Buffer::FromString(std::string(reinterpret_cast<const char*>(ptr), len));
            switch (type->id()) {
                case T::STRING:
                    return std::make_shared<arrow::StringScalar>(ibuf);
                case T::LARGE_STRING:
                    return std::make_shared<arrow::LargeStringScalar>(ibuf);
                case T::BINARY:
                    return std::make_shared<arrow::BinaryScalar>(ibuf);
                case T::LARGE_BINARY:
                    return std::make_shared<arrow::LargeBinaryScalar>(ibuf);
                case T::STRING_VIEW:
                    return std::make_shared<arrow::StringViewScalar>(ibuf);
                case T::BINARY_VIEW:
                    return std::make_shared<arrow::BinaryViewScalar>(ibuf);
                default:
                    throw std::invalid_argument("DecodeScalar: unsupported Arrow type: " +
                                                type->ToString());
            }
        }

        case T::DATE32:
            return std::make_shared<arrow::Date32Scalar>(r.Read<int32_t>());
        case T::DATE64:
            return std::make_shared<arrow::Date64Scalar>(r.Read<int64_t>());
        case T::TIMESTAMP: {
            int64_t v = r.Read<int64_t>();
            return std::make_shared<arrow::TimestampScalar>(v, type);
        }
        case T::TIME32: {
            int32_t v = r.Read<int32_t>();
            return std::make_shared<arrow::Time32Scalar>(v, type);
        }
        case T::TIME64: {
            int64_t v = r.Read<int64_t>();
            return std::make_shared<arrow::Time64Scalar>(v, type);
        }
        case T::DURATION: {
            int64_t v = r.Read<int64_t>();
            return std::make_shared<arrow::DurationScalar>(v, type);
        }
        case T::FIXED_SIZE_BINARY: {
            const int32_t byte_width =
                static_cast<const arrow::FixedSizeBinaryType&>(*type).byte_width();
            const uint8_t* ptr = r.ReadBytes(byte_width);
            auto ibuf = arrow::Buffer::FromString(
                std::string(reinterpret_cast<const char*>(ptr), byte_width));
            return std::make_shared<arrow::FixedSizeBinaryScalar>(ibuf, type);
        }
        case T::HALF_FLOAT:
            return std::make_shared<arrow::HalfFloatScalar>(r.Read<uint16_t>());
        case T::INTERVAL_MONTHS:
            return std::make_shared<arrow::MonthIntervalScalar>(r.Read<int32_t>());
        case T::INTERVAL_DAY_TIME: {
            arrow::DayTimeIntervalType::DayMilliseconds v;
            v.days = r.Read<int32_t>();
            v.milliseconds = r.Read<int32_t>();
            return std::make_shared<arrow::DayTimeIntervalScalar>(v);
        }
        case T::INTERVAL_MONTH_DAY_NANO: {
            arrow::MonthDayNanoIntervalType::MonthDayNanos v;
            v.months = r.Read<int32_t>();
            v.days = r.Read<int32_t>();
            v.nanoseconds = r.Read<int64_t>();
            return std::make_shared<arrow::MonthDayNanoIntervalScalar>(v);
        }
        case T::DECIMAL128: {
            const uint8_t* ptr = r.ReadBytes(16);
            return std::make_shared<arrow::Decimal128Scalar>(arrow::Decimal128(ptr), type);
        }
        case T::DECIMAL256: {
            const uint8_t* ptr = r.ReadBytes(32);
            return std::make_shared<arrow::Decimal256Scalar>(arrow::Decimal256(ptr), type);
        }

        case T::DICTIONARY: {
            // The wire carries the value type; decode to a plain value scalar.
            // The RecordBatch subscriber re-folds these into a DictionaryArray.
            const auto& dict_type = static_cast<const arrow::DictionaryType&>(*type);
            EnsureDictionaryValueSupported(*dict_type.value_type());
            return DecodeScalarFromReader(r, dict_type.value_type());
        }

        default:
            throw std::invalid_argument("DecodeScalar: unsupported Arrow type: " +
                                        type->ToString());
    }
}

}  // namespace detail
}  // namespace fletcher
