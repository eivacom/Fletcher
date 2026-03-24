#include "row_codec.h"
#include "row_reader.h"    // detail::Reader

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
void appendFixed(std::vector<uint8_t>& buf, T value) {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&value);
    buf.insert(buf.end(), bytes, bytes + sizeof(T));
}

void appendVariableLength(std::vector<uint8_t>& buf,
                          const uint8_t*        data,
                          uint32_t              len) {
    appendFixed(buf, len);
    buf.insert(buf.end(), data, data + len);
}

void encodeScalar(std::vector<uint8_t>& buf, const arrow::Scalar& scalar) {
    using T = arrow::Type;

    switch (scalar.type->id()) {
        case T::BOOL: {
            auto v = static_cast<const arrow::BooleanScalar&>(scalar).value;
            appendFixed<uint8_t>(buf, v ? 1u : 0u);
            break;
        }
        case T::INT8:
            appendFixed(buf, static_cast<const arrow::Int8Scalar&>(scalar).value);
            break;
        case T::INT16:
            appendFixed(buf, static_cast<const arrow::Int16Scalar&>(scalar).value);
            break;
        case T::INT32:
            appendFixed(buf, static_cast<const arrow::Int32Scalar&>(scalar).value);
            break;
        case T::INT64:
            appendFixed(buf, static_cast<const arrow::Int64Scalar&>(scalar).value);
            break;
        case T::UINT8:
            appendFixed(buf, static_cast<const arrow::UInt8Scalar&>(scalar).value);
            break;
        case T::UINT16:
            appendFixed(buf, static_cast<const arrow::UInt16Scalar&>(scalar).value);
            break;
        case T::UINT32:
            appendFixed(buf, static_cast<const arrow::UInt32Scalar&>(scalar).value);
            break;
        case T::UINT64:
            appendFixed(buf, static_cast<const arrow::UInt64Scalar&>(scalar).value);
            break;
        case T::FLOAT:
            appendFixed(buf, static_cast<const arrow::FloatScalar&>(scalar).value);
            break;
        case T::DOUBLE:
            appendFixed(buf, static_cast<const arrow::DoubleScalar&>(scalar).value);
            break;

        // Variable-width types share BaseBinaryScalar with a Buffer value.
        case T::STRING:
        case T::LARGE_STRING:
        case T::BINARY:
        case T::LARGE_BINARY: {
            const auto&   s      = static_cast<const arrow::BaseBinaryScalar&>(scalar);
            const int64_t rawLen = s.value->size();
            if (rawLen < 0 || rawLen > static_cast<int64_t>(std::numeric_limits<uint32_t>::max()))
                throw std::invalid_argument(
                    "encodeRow: variable-length field exceeds 4 GiB limit");
            appendVariableLength(
                buf,
                reinterpret_cast<const uint8_t*>(s.value->data()),
                static_cast<uint32_t>(rawLen));
            break;
        }

        case T::DATE32:
            appendFixed(buf, static_cast<const arrow::Date32Scalar&>(scalar).value);
            break;
        case T::DATE64:
            appendFixed(buf, static_cast<const arrow::Date64Scalar&>(scalar).value);
            break;
        case T::TIMESTAMP:
            appendFixed(buf, static_cast<const arrow::TimestampScalar&>(scalar).value);
            break;

        default:
            throw std::invalid_argument(
                "encodeRow: unsupported Arrow type: " + scalar.type->ToString());
    }
}

// ---------------------------------------------------------------------------
// Decoder helpers
// ---------------------------------------------------------------------------

using detail::Reader;

std::shared_ptr<arrow::Scalar> decodeScalar(Reader&                                 r,
                                             const std::shared_ptr<arrow::DataType>& type) {
    using T = arrow::Type;
    switch (type->id()) {
        case T::BOOL:
            return std::make_shared<arrow::BooleanScalar>(r.read<uint8_t>() != 0);
        case T::INT8:
            return std::make_shared<arrow::Int8Scalar>(r.read<int8_t>());
        case T::INT16:
            return std::make_shared<arrow::Int16Scalar>(r.read<int16_t>());
        case T::INT32:
            return std::make_shared<arrow::Int32Scalar>(r.read<int32_t>());
        case T::INT64:
            return std::make_shared<arrow::Int64Scalar>(r.read<int64_t>());
        case T::UINT8:
            return std::make_shared<arrow::UInt8Scalar>(r.read<uint8_t>());
        case T::UINT16:
            return std::make_shared<arrow::UInt16Scalar>(r.read<uint16_t>());
        case T::UINT32:
            return std::make_shared<arrow::UInt32Scalar>(r.read<uint32_t>());
        case T::UINT64:
            return std::make_shared<arrow::UInt64Scalar>(r.read<uint64_t>());
        case T::FLOAT:
            return std::make_shared<arrow::FloatScalar>(r.read<float>());
        case T::DOUBLE:
            return std::make_shared<arrow::DoubleScalar>(r.read<double>());

        case T::STRING:
        case T::LARGE_STRING:
        case T::BINARY:
        case T::LARGE_BINARY: {
            uint32_t       len  = r.read<uint32_t>();
            const uint8_t* ptr  = r.readBytes(len);
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
            return std::make_shared<arrow::Date32Scalar>(r.read<int32_t>());
        case T::DATE64:
            return std::make_shared<arrow::Date64Scalar>(r.read<int64_t>());
        case T::TIMESTAMP: {
            int64_t v = r.read<int64_t>();
            return std::make_shared<arrow::TimestampScalar>(v, type);
        }

        default:
            throw std::invalid_argument(
                "decodeRow: unsupported Arrow type: " + type->ToString());
    }
    throw std::invalid_argument("decodeRow: unsupported Arrow type: " + type->ToString());
}

} // namespace

// ---------------------------------------------------------------------------
// RowCodec
// ---------------------------------------------------------------------------

RowCodec::RowCodec(std::shared_ptr<arrow::Schema> schema)
    : schema_(std::move(schema)), schemaHash_(fingerprintHash(*schema_)) {}

std::vector<uint8_t> RowCodec::encodeRow(
    const std::vector<std::shared_ptr<arrow::Scalar>>& values) const {

    const int numFields = schema_->num_fields();

    if (static_cast<int>(values.size()) != numFields)
        throw std::invalid_argument(
            "encodeRow: values.size() (" + std::to_string(values.size()) +
            ") does not match schema.num_fields() (" + std::to_string(numFields) + ")");

    std::vector<uint8_t> buf;
    buf.reserve(64);

    appendFixed(buf, schemaHash_);
    appendFixed(buf, kVersion);

    for (int i = 0; i < numFields; ++i) {
        const auto& scalar = values[i];
        const bool  isNull = !scalar || !scalar->is_valid;

        if (isNull) {
            buf.push_back(0x01u);
            continue;
        }

        if (!scalar->type->Equals(*schema_->field(i)->type()))
            throw std::invalid_argument(
                "encodeRow: type mismatch for field '" + schema_->field(i)->name() +
                "': schema expects " + schema_->field(i)->type()->ToString() +
                ", got " + scalar->type->ToString());

        buf.push_back(0x00u);
        encodeScalar(buf, *scalar);
    }

    return buf;
}

std::vector<std::shared_ptr<arrow::Scalar>> RowCodec::decodeRow(
    const std::vector<uint8_t>& buf) const {

    Reader r{buf.data(), buf.size()};

    uint64_t hash = r.read<uint64_t>();
    if (hash != schemaHash_)
        throw std::invalid_argument("decodeRow: schema hash mismatch");

    uint8_t version = r.read<uint8_t>();
    if (version != kVersion)
        throw std::invalid_argument(
            "decodeRow: unsupported version " + std::to_string(version));

    const int numFields = schema_->num_fields();
    std::vector<std::shared_ptr<arrow::Scalar>> values;
    values.reserve(numFields);

    for (int i = 0; i < numFields; ++i) {
        uint8_t nullFlag = r.read<uint8_t>();
        if (nullFlag == 0x01u) {
            values.push_back(arrow::MakeNullScalar(schema_->field(i)->type()));
        } else {
            values.push_back(decodeScalar(r, schema_->field(i)->type()));
        }
    }

    return values;
}

// ---------------------------------------------------------------------------
// fingerprintHash
// ---------------------------------------------------------------------------

namespace {

uint64_t fnv1a64(const std::string& s) {
    uint64_t hash = 14695981039346656037ULL;
    for (unsigned char c : s) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    return hash;
}

} // namespace

uint64_t fingerprintHash(const arrow::Schema& schema) {
    const std::string fp = schema.fingerprint();
    if (fp.empty())
        throw std::invalid_argument(
            "fingerprintHash: schema contains types that cannot be fingerprinted");
    return fnv1a64(fp);
}

} // namespace arrow_row
