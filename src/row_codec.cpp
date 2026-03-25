#include "row_codec.hpp"
#include "row_reader.hpp"    // detail::Reader

#include <arrow/scalar.h>
#include <arrow/type.h>

#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

namespace arrow_row {
namespace {

// ---------------------------------------------------------------------------
// Encoder helpers
// ---------------------------------------------------------------------------

template <typename T>
void AppendFixed(std::vector<uint8_t>& buf, T value) {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&value);
    buf.insert(buf.end(), bytes, bytes + sizeof(T));
}

void AppendVariableLength(std::vector<uint8_t>& buf,
                          const uint8_t*        data,
                          uint32_t              len) {
    AppendFixed(buf, len);
    buf.insert(buf.end(), data, data + len);
}

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

        // Variable-width types share BaseBinaryScalar with a Buffer value.
        case T::STRING:
        case T::LARGE_STRING:
        case T::BINARY:
        case T::LARGE_BINARY: {
            const auto&   s       = static_cast<const arrow::BaseBinaryScalar&>(scalar);
            const int64_t raw_len = s.value->size();
            if (raw_len < 0 || raw_len > static_cast<int64_t>(std::numeric_limits<uint32_t>::max()))
                throw std::invalid_argument(
                    "EncodeRow: variable-length field exceeds 4 GiB limit");
            AppendVariableLength(
                buf,
                reinterpret_cast<const uint8_t*>(s.value->data()),
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
            const auto&   s          = static_cast<const arrow::FixedSizeBinaryScalar&>(scalar);
            const int32_t byte_width =
                static_cast<const arrow::FixedSizeBinaryType&>(*scalar.type).byte_width();
            const auto* data = reinterpret_cast<const uint8_t*>(s.value->data());
            buf.insert(buf.end(), data, data + byte_width);
            break;
        }

        default:
            throw std::invalid_argument(
                "EncodeRow: unsupported Arrow type: " + scalar.type->ToString());
    }
}

// ---------------------------------------------------------------------------
// Decoder helpers
// ---------------------------------------------------------------------------

using detail::Reader;

std::shared_ptr<arrow::Scalar> DecodeScalar(Reader&                                 r,
                                             const std::shared_ptr<arrow::DataType>& type) {
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
        case T::LARGE_BINARY: {
            uint32_t       len  = r.Read<uint32_t>();
            const uint8_t* ptr  = r.ReadBytes(len);
            auto           ibuf = std::make_shared<arrow::Buffer>(ptr, len);
            switch (type->id()) {
                case T::STRING:       return std::make_shared<arrow::StringScalar>(ibuf);
                case T::LARGE_STRING: return std::make_shared<arrow::LargeStringScalar>(ibuf);
                case T::BINARY:       return std::make_shared<arrow::BinaryScalar>(ibuf);
                case T::LARGE_BINARY: return std::make_shared<arrow::LargeBinaryScalar>(ibuf);
                default:              break;
            }
            break;
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
            const uint8_t* ptr  = r.ReadBytes(byte_width);
            auto           ibuf = std::make_shared<arrow::Buffer>(ptr, byte_width);
            return std::make_shared<arrow::FixedSizeBinaryScalar>(ibuf, type);
        }

        default:
            throw std::invalid_argument(
                "DecodeRow: unsupported Arrow type: " + type->ToString());
    }
    throw std::invalid_argument("DecodeRow: unsupported Arrow type: " + type->ToString());
}

// ---------------------------------------------------------------------------
// FNV-1a hasher
// ---------------------------------------------------------------------------

uint64_t Fnv1a64(const std::string& s) {
    uint64_t hash = 14695981039346656037ULL;
    for (unsigned char c : s) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    return hash;
}

}  // namespace

// ---------------------------------------------------------------------------
// RowCodec
// ---------------------------------------------------------------------------

RowCodec::RowCodec(std::shared_ptr<arrow::Schema> schema)
    : schema_(std::move(schema)), schema_hash_(FingerprintHash(*schema_)) {}

ArrowRow RowCodec::EncodeRow(
    const std::vector<std::shared_ptr<arrow::Scalar>>& values) const {

    const int num_fields = schema_->num_fields();

    if (static_cast<int>(values.size()) != num_fields)
        throw std::invalid_argument(
            "EncodeRow: values.size() (" + std::to_string(values.size()) +
            ") does not match schema.num_fields() (" + std::to_string(num_fields) + ")");

    std::vector<uint8_t> buf;
    buf.reserve(64);

    AppendFixed(buf, schema_hash_);
    AppendFixed(buf, kVersion);

    for (int i = 0; i < num_fields; ++i) {
        const auto& scalar = values[i];
        const bool  is_null = !scalar || !scalar->is_valid;

        if (is_null) {
            buf.push_back(0x01u);
            continue;
        }

        if (!scalar->type->Equals(*schema_->field(i)->type()))
            throw std::invalid_argument(
                "EncodeRow: type mismatch for field '" + schema_->field(i)->name() +
                "': schema expects " + schema_->field(i)->type()->ToString() +
                ", got " + scalar->type->ToString());

        buf.push_back(0x00u);
        EncodeScalar(buf, *scalar);
    }

    return buf;
}

std::vector<std::shared_ptr<arrow::Scalar>> RowCodec::DecodeRow(
    const ArrowRow& buf) const {

    Reader r{buf.data(), buf.size()};

    uint64_t hash = r.Read<uint64_t>();
    if (hash != schema_hash_)
        throw std::invalid_argument("DecodeRow: schema hash mismatch");

    uint8_t version = r.Read<uint8_t>();
    if (version != kVersion)
        throw std::invalid_argument(
            "DecodeRow: unsupported version " + std::to_string(version));

    const int num_fields = schema_->num_fields();
    std::vector<std::shared_ptr<arrow::Scalar>> values;
    values.reserve(num_fields);

    for (int i = 0; i < num_fields; ++i) {
        uint8_t null_flag = r.Read<uint8_t>();
        if (null_flag == 0x01u) {
            values.push_back(arrow::MakeNullScalar(schema_->field(i)->type()));
        } else {
            values.push_back(DecodeScalar(r, schema_->field(i)->type()));
        }
    }

    return values;
}

// ---------------------------------------------------------------------------
// FingerprintHash
// ---------------------------------------------------------------------------

uint64_t FingerprintHash(const arrow::Schema& schema) {
    const std::string fp = schema.fingerprint();
    if (fp.empty())
        throw std::invalid_argument(
            "FingerprintHash: schema contains types that cannot be fingerprinted");
    return Fnv1a64(fp);
}

uint64_t FingerprintHashWithFieldNames(const arrow::Schema& schema) {
    const std::string fp = schema.fingerprint();
    if (fp.empty())
        throw std::invalid_argument(
            "FingerprintHashWithFieldNames: schema contains types that cannot be fingerprinted");

    // Combine the structural fingerprint with each field name, separated by
    // null bytes (which cannot appear in valid field names).
    std::string combined = fp;
    for (int i = 0; i < schema.num_fields(); ++i) {
        combined += '\0';
        combined += schema.field(i)->name();
    }
    return Fnv1a64(combined);
}

}  // namespace arrow_row
