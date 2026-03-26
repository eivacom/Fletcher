#include "row_codec.hpp"
#include "row_reader.hpp"    // detail::Reader
#include "scalar_codec.hpp"  // detail::EncodeScalar, detail::DecodeScalar

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
// Encoder helpers (internal to this TU; visible throughout arrow_row via the
// anonymous namespace's using-directive injection into the enclosing scope)
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

        // Variable-width types share BaseBinaryScalar with a Buffer value.
        // STRING_VIEW / BINARY_VIEW use the same scalar representation — the
        // array-level multi-buffer layout is hidden behind the scalar API.
        case T::STRING:
        case T::LARGE_STRING:
        case T::BINARY:
        case T::LARGE_BINARY:
        case T::STRING_VIEW:
        case T::BINARY_VIEW: {
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
        case T::STRUCT: {
            const auto& ss         = static_cast<const arrow::StructScalar&>(scalar);
            const auto& stype      = static_cast<const arrow::StructType&>(*scalar.type);
            const int   num_fields = stype.num_fields();
            for (int i = 0; i < num_fields; ++i) {
                const auto& child   = ss.value[static_cast<size_t>(i)];
                const bool  is_null = !child || !child->is_valid;
                buf.push_back(is_null ? 0x01u : 0x00u);
                if (!is_null) EncodeScalar(buf, *child);
            }
            break;
        }
        case T::LIST:
        case T::LARGE_LIST: {
            const auto& ls     = static_cast<const arrow::BaseListScalar&>(scalar);
            const int64_t len  = ls.value->length();
            AppendFixed(buf, static_cast<uint32_t>(len));
            for (int64_t i = 0; i < len; ++i) {
                auto result = ls.value->GetScalar(i);
                if (!result.ok())
                    throw std::invalid_argument(
                        "EncodeRow: GetScalar failed: " + result.status().ToString());
                const auto& elem    = *result;
                const bool  is_null = !elem->is_valid;
                buf.push_back(is_null ? 0x01u : 0x00u);
                if (!is_null) EncodeScalar(buf, *elem);
            }
            break;
        }
        case T::FIXED_SIZE_LIST: {
            // No count prefix — element count is fixed in the type.
            const auto& ls = static_cast<const arrow::BaseListScalar&>(scalar);
            for (int64_t i = 0; i < ls.value->length(); ++i) {
                auto result = ls.value->GetScalar(i);
                if (!result.ok())
                    throw std::invalid_argument(
                        "EncodeRow: GetScalar failed: " + result.status().ToString());
                const auto& elem    = *result;
                const bool  is_null = !elem->is_valid;
                buf.push_back(is_null ? 0x01u : 0x00u);
                if (!is_null) EncodeScalar(buf, *elem);
            }
            break;
        }
        case T::SPARSE_UNION: {
            // value is a ScalarVector — one slot per child type.  Find the
            // active child by matching type_code to its index.
            const auto& us         = static_cast<const arrow::SparseUnionScalar&>(scalar);
            const auto& union_type = static_cast<const arrow::SparseUnionType&>(*scalar.type);
            AppendFixed(buf, us.type_code);
            const auto& codes = union_type.type_codes();
            int child_id = -1;
            for (int i = 0; i < static_cast<int>(codes.size()); ++i) {
                if (codes[i] == us.type_code) { child_id = i; break; }
            }
            if (child_id < 0 || child_id >= static_cast<int>(us.value.size()))
                throw std::invalid_argument("EncodeScalar: invalid sparse union type_code");
            EncodeScalar(buf, *us.value[static_cast<size_t>(child_id)]);
            break;
        }
        case T::DENSE_UNION: {
            // value is a shared_ptr<Scalar> — only the active child is stored.
            const auto& us = static_cast<const arrow::DenseUnionScalar&>(scalar);
            AppendFixed(buf, us.type_code);
            EncodeScalar(buf, *us.value);
            break;
        }
        case T::MAP: {
            const auto& ms         = static_cast<const arrow::MapScalar&>(scalar);
            const auto& map_type   = static_cast<const arrow::MapType&>(*scalar.type);
            auto struct_arr        = std::static_pointer_cast<arrow::StructArray>(ms.value);
            const int64_t count    = struct_arr->length();
            auto key_arr           = struct_arr->field(0);
            auto val_arr           = struct_arr->field(1);
            AppendFixed(buf, static_cast<uint32_t>(count));
            for (int64_t i = 0; i < count; ++i) {
                // Keys are non-null by Arrow convention — no null flag.
                auto key_result = key_arr->GetScalar(i);
                if (!key_result.ok())
                    throw std::invalid_argument(
                        "EncodeRow: GetScalar (map key) failed: " +
                        key_result.status().ToString());
                EncodeScalar(buf, **key_result);
                auto val_result = val_arr->GetScalar(i);
                if (!val_result.ok())
                    throw std::invalid_argument(
                        "EncodeRow: GetScalar (map value) failed: " +
                        val_result.status().ToString());
                const auto& val = *val_result;
                buf.push_back(!val->is_valid ? 0x01u : 0x00u);
                if (val->is_valid) EncodeScalar(buf, *val);
            }
            (void)map_type;
            break;
        }

        case T::DICTIONARY:
            // Dictionary encoding is a columnar storage optimisation: the value
            // is an index into a shared dictionary array that lives outside the
            // row.  There is no meaningful way to embed that dictionary in a
            // self-contained per-row buffer without either discarding it (lossy)
            // or duplicating it in every row (prohibitively expensive).
            // Use a non-dictionary schema field instead.
            throw std::invalid_argument(
                "EncodeRow: DICTIONARY type is not supported in the per-row format; "
                "use a non-dictionary schema field instead");

        default:
            throw std::invalid_argument(
                "EncodeRow: unsupported Arrow type: " + scalar.type->ToString());
    }
}

}  // namespace detail

namespace {

// ---------------------------------------------------------------------------
// Decoder helpers
// ---------------------------------------------------------------------------

using detail::Reader;

std::shared_ptr<arrow::Scalar> DecodeScalarImpl(Reader&                                 r,
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
        case T::LARGE_BINARY:
        case T::STRING_VIEW:
        case T::BINARY_VIEW: {
            uint32_t       len  = r.Read<uint32_t>();
            const uint8_t* ptr  = r.ReadBytes(len);
            auto           ibuf = std::make_shared<arrow::Buffer>(ptr, len);
            switch (type->id()) {
                case T::STRING:       return std::make_shared<arrow::StringScalar>(ibuf);
                case T::LARGE_STRING: return std::make_shared<arrow::LargeStringScalar>(ibuf);
                case T::BINARY:       return std::make_shared<arrow::BinaryScalar>(ibuf);
                case T::LARGE_BINARY: return std::make_shared<arrow::LargeBinaryScalar>(ibuf);
                case T::STRING_VIEW:  return std::make_shared<arrow::StringViewScalar>(ibuf);
                case T::BINARY_VIEW:  return std::make_shared<arrow::BinaryViewScalar>(ibuf);
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
        case T::HALF_FLOAT:
            return std::make_shared<arrow::HalfFloatScalar>(r.Read<uint16_t>());
        case T::INTERVAL_MONTHS:
            return std::make_shared<arrow::MonthIntervalScalar>(r.Read<int32_t>());
        case T::INTERVAL_DAY_TIME: {
            arrow::DayTimeIntervalType::DayMilliseconds v;
            v.days         = r.Read<int32_t>();
            v.milliseconds = r.Read<int32_t>();
            return std::make_shared<arrow::DayTimeIntervalScalar>(v);
        }
        case T::INTERVAL_MONTH_DAY_NANO: {
            arrow::MonthDayNanoIntervalType::MonthDayNanos v;
            v.months      = r.Read<int32_t>();
            v.days        = r.Read<int32_t>();
            v.nanoseconds = r.Read<int64_t>();
            return std::make_shared<arrow::MonthDayNanoIntervalScalar>(v);
        }
        case T::DECIMAL128: {
            const uint8_t* ptr = r.ReadBytes(16);
            return std::make_shared<arrow::Decimal128Scalar>(
                arrow::Decimal128(ptr), type);
        }
        case T::DECIMAL256: {
            const uint8_t* ptr = r.ReadBytes(32);
            return std::make_shared<arrow::Decimal256Scalar>(
                arrow::Decimal256(ptr), type);
        }
        case T::STRUCT: {
            const auto& stype      = static_cast<const arrow::StructType&>(*type);
            const int   num_fields = stype.num_fields();
            arrow::ScalarVector children;
            children.reserve(num_fields);
            for (int i = 0; i < num_fields; ++i) {
                const uint8_t null_flag = r.Read<uint8_t>();
                if (null_flag == 0x01u) {
                    children.push_back(arrow::MakeNullScalar(stype.field(i)->type()));
                } else {
                    children.push_back(DecodeScalarImpl(r, stype.field(i)->type()));
                }
            }
            return std::make_shared<arrow::StructScalar>(std::move(children), type);
        }
        case T::LIST:
        case T::LARGE_LIST: {
            const auto& list_type = static_cast<const arrow::BaseListType&>(*type);
            const auto  elem_type = list_type.value_type();
            const uint32_t count  = r.Read<uint32_t>();
            auto maybe_builder    = arrow::MakeBuilder(elem_type);
            if (!maybe_builder.ok())
                throw std::invalid_argument(
                    "DecodeRow: MakeBuilder failed: " + maybe_builder.status().ToString());
            auto& builder = *maybe_builder;
            for (uint32_t i = 0; i < count; ++i) {
                const uint8_t null_flag = r.Read<uint8_t>();
                arrow::Status st = (null_flag == 0x01u)
                    ? builder->AppendNull()
                    : builder->AppendScalar(*DecodeScalarImpl(r, elem_type));
                if (!st.ok())
                    throw std::invalid_argument(
                        "DecodeRow: builder append failed: " + st.ToString());
            }
            auto finish = builder->Finish();
            if (!finish.ok())
                throw std::invalid_argument(
                    "DecodeRow: builder finish failed: " + finish.status().ToString());
            if (type->id() == arrow::Type::LIST)
                return std::make_shared<arrow::ListScalar>(*finish, type);
            return std::make_shared<arrow::LargeListScalar>(*finish, type);
        }
        case T::FIXED_SIZE_LIST: {
            // No count prefix — element count comes from the type.
            const auto& fsl_type  = static_cast<const arrow::FixedSizeListType&>(*type);
            const auto  elem_type = fsl_type.value_type();
            const int32_t count   = fsl_type.list_size();
            auto maybe_builder    = arrow::MakeBuilder(elem_type);
            if (!maybe_builder.ok())
                throw std::invalid_argument(
                    "DecodeRow: MakeBuilder failed: " + maybe_builder.status().ToString());
            auto& builder = *maybe_builder;
            for (int32_t i = 0; i < count; ++i) {
                const uint8_t null_flag = r.Read<uint8_t>();
                arrow::Status st = (null_flag == 0x01u)
                    ? builder->AppendNull()
                    : builder->AppendScalar(*DecodeScalarImpl(r, elem_type));
                if (!st.ok())
                    throw std::invalid_argument(
                        "DecodeRow: builder append failed: " + st.ToString());
            }
            auto finish = builder->Finish();
            if (!finish.ok())
                throw std::invalid_argument(
                    "DecodeRow: builder finish failed: " + finish.status().ToString());
            return std::make_shared<arrow::FixedSizeListScalar>(*finish, type);
        }
        case T::SPARSE_UNION: {
            const auto& union_type = static_cast<const arrow::SparseUnionType&>(*type);
            const int8_t type_code = r.Read<int8_t>();
            const auto& codes = union_type.type_codes();
            int child_id = -1;
            for (int i = 0; i < static_cast<int>(codes.size()); ++i) {
                if (codes[i] == type_code) { child_id = i; break; }
            }
            if (child_id < 0)
                throw std::invalid_argument(
                    "DecodeRow: unknown sparse union type_code " + std::to_string(type_code));
            auto active = DecodeScalarImpl(r, union_type.field(child_id)->type());
            // ScalarVector must have one slot per child; fill inactive ones with null.
            arrow::ScalarVector children;
            children.reserve(union_type.num_fields());
            for (int i = 0; i < union_type.num_fields(); ++i) {
                children.push_back(i == child_id
                    ? active
                    : arrow::MakeNullScalar(union_type.field(i)->type()));
            }
            return std::make_shared<arrow::SparseUnionScalar>(
                std::move(children), type_code, type);
        }
        case T::DENSE_UNION: {
            const auto& union_type = static_cast<const arrow::DenseUnionType&>(*type);
            const int8_t type_code = r.Read<int8_t>();
            const auto& codes = union_type.type_codes();
            int child_id = -1;
            for (int i = 0; i < static_cast<int>(codes.size()); ++i) {
                if (codes[i] == type_code) { child_id = i; break; }
            }
            if (child_id < 0)
                throw std::invalid_argument(
                    "DecodeRow: unknown dense union type_code " + std::to_string(type_code));
            auto value = DecodeScalarImpl(r, union_type.field(child_id)->type());
            return std::make_shared<arrow::DenseUnionScalar>(value, type_code, type);
        }
        case T::MAP: {
            const auto& map_type = static_cast<const arrow::MapType&>(*type);
            const uint32_t count = r.Read<uint32_t>();
            auto maybe_key_b     = arrow::MakeBuilder(map_type.key_type());
            if (!maybe_key_b.ok())
                throw std::invalid_argument(
                    "DecodeRow: MakeBuilder(key) failed: " + maybe_key_b.status().ToString());
            auto maybe_val_b = arrow::MakeBuilder(map_type.item_type());
            if (!maybe_val_b.ok())
                throw std::invalid_argument(
                    "DecodeRow: MakeBuilder(value) failed: " + maybe_val_b.status().ToString());
            for (uint32_t i = 0; i < count; ++i) {
                // Key — no null flag (Arrow keys are non-null by convention).
                auto st = (*maybe_key_b)->AppendScalar(*DecodeScalarImpl(r, map_type.key_type()));
                if (!st.ok())
                    throw std::invalid_argument(
                        "DecodeRow: key append failed: " + st.ToString());
                // Value — may be null.
                const uint8_t null_flag = r.Read<uint8_t>();
                st = (null_flag == 0x01u)
                    ? (*maybe_val_b)->AppendNull()
                    : (*maybe_val_b)->AppendScalar(*DecodeScalarImpl(r, map_type.item_type()));
                if (!st.ok())
                    throw std::invalid_argument(
                        "DecodeRow: value append failed: " + st.ToString());
            }
            auto key_finish = (*maybe_key_b)->Finish();
            if (!key_finish.ok())
                throw std::invalid_argument(
                    "DecodeRow: key builder finish failed: " + key_finish.status().ToString());
            auto val_finish = (*maybe_val_b)->Finish();
            if (!val_finish.ok())
                throw std::invalid_argument(
                    "DecodeRow: value builder finish failed: " + val_finish.status().ToString());
            // MapScalar::value is a StructArray with key and value child arrays.
            auto entries = std::make_shared<arrow::StructArray>(
                arrow::struct_({map_type.key_field(), map_type.item_field()}),
                static_cast<int64_t>(count),
                arrow::ArrayVector{*key_finish, *val_finish});
            return std::make_shared<arrow::MapScalar>(entries, type);
        }

        case T::DICTIONARY:
            throw std::invalid_argument(
                "DecodeRow: DICTIONARY type is not supported in the per-row format; "
                "use a non-dictionary schema field instead");

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

namespace detail {

std::shared_ptr<arrow::Scalar> DecodeScalar(
    const uint8_t*                          data,
    size_t                                  size,
    const std::shared_ptr<arrow::DataType>& type) {
    Reader r{data, size};
    return DecodeScalarImpl(r, type);
}

}  // namespace detail

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
        detail::EncodeScalar(buf, *scalar);
    }

    return buf;
}

std::vector<std::shared_ptr<arrow::Scalar>> RowCodec::DecodeRow(
    const ArrowRow& buf) const {

    detail::Reader r{buf.data(), buf.size()};

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
            values.push_back(DecodeScalarImpl(r, schema_->field(i)->type()));
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

}  // namespace arrow_row
