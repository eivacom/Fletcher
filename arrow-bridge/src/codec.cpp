// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include "fletcher/arrow_bridge/codec.hpp"

#include <arrow/api.h>

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "row_reader.hpp"
#include "scalar_codec.hpp"

namespace fletcher {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

template <typename T>
void AppendFixed(std::vector<uint8_t>& buf, T value) {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&value);
    buf.insert(buf.end(), bytes, bytes + sizeof(T));
}

// Number of bytes needed for a null bitfield covering `n` items.
// Takes int64_t so a wire-supplied uint32_t count never narrows to a negative
// int (which would make the (n + 7) / 8 arithmetic produce garbage).
size_t BitfieldBytes(int64_t n) { return static_cast<size_t>((n + 7) / 8); }

// Write a null bitfield: bit i is 1 if the i-th item is null.
void WriteNullBitfield(std::vector<uint8_t>& buf, const arrow::ScalarVector& scalars, int count) {
    size_t nbytes = BitfieldBytes(count);
    size_t start = buf.size();
    buf.resize(start + nbytes, 0);
    for (int i = 0; i < count; ++i) {
        bool is_null = !scalars[i] || !scalars[i]->is_valid;
        if (is_null) buf[start + i / 8] |= static_cast<uint8_t>(1u << (i % 8));
    }
}

bool ReadNullBit(const uint8_t* bitfield, int index) {
    return (bitfield[index / 8] >> (index % 8)) & 1u;
}

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------

void EncodePositionalValue(std::vector<uint8_t>& buf, const arrow::Scalar& scalar,
                           const arrow::DataType& type);
std::shared_ptr<arrow::Scalar> DecodePositionalValue(detail::Reader& r,
                                                     const std::shared_ptr<arrow::DataType>& type);

// ---------------------------------------------------------------------------
// Encode helpers
// ---------------------------------------------------------------------------

void EncodePositionalStruct(std::vector<uint8_t>& buf, const arrow::StructScalar& scalar,
                            const arrow::StructType& stype) {
    const int n = stype.num_fields();
    arrow::ScalarVector children(n);
    for (int i = 0; i < n; ++i) children[i] = scalar.value[static_cast<size_t>(i)];

    WriteNullBitfield(buf, children, n);

    for (int i = 0; i < n; ++i) {
        const auto& child = children[i];
        if (!child || !child->is_valid) continue;
        EncodePositionalValue(buf, *child, *stype.field(i)->type());
    }
}

void EncodeListElements(std::vector<uint8_t>& buf, const std::shared_ptr<arrow::Array>& arr,
                        const arrow::DataType& elem_type) {
    const int64_t count = arr->length();

    // Write null bitfield for elements. Pass the int64_t count directly — a
    // static_cast<int> would wrap negative for counts > INT_MAX and corrupt
    // the bitfield size (the very narrowing this hardening removes).
    size_t nbytes = BitfieldBytes(count);
    size_t start = buf.size();
    buf.resize(start + nbytes, 0);
    for (int64_t i = 0; i < count; ++i) {
        if (arr->IsNull(i)) buf[start + i / 8] |= static_cast<uint8_t>(1u << (i % 8));
    }

    // Write non-null element payloads.
    for (int64_t i = 0; i < count; ++i) {
        if (arr->IsNull(i)) continue;
        auto elem = arr->GetScalar(i).ValueOrDie();
        EncodePositionalValue(buf, *elem, elem_type);
    }
}

void EncodePositionalValue(std::vector<uint8_t>& buf, const arrow::Scalar& scalar,
                           const arrow::DataType& type) {
    using T = arrow::Type;

    switch (type.id()) {
        case T::STRUCT: {
            const auto& ss = static_cast<const arrow::StructScalar&>(scalar);
            const auto& stype = static_cast<const arrow::StructType&>(type);
            EncodePositionalStruct(buf, ss, stype);
            return;
        }
        case T::LIST:
        case T::LARGE_LIST: {
            const auto& ls = static_cast<const arrow::BaseListScalar&>(scalar);
            const auto& list_type = static_cast<const arrow::BaseListType&>(type);
            AppendFixed(buf, static_cast<uint32_t>(ls.value->length()));
            EncodeListElements(buf, ls.value, *list_type.value_type());
            return;
        }
        case T::FIXED_SIZE_LIST: {
            const auto& ls = static_cast<const arrow::BaseListScalar&>(scalar);
            const auto& fsl_type = static_cast<const arrow::FixedSizeListType&>(type);
            // No COUNT prefix — the fixed size is in the schema.
            EncodeListElements(buf, ls.value, *fsl_type.value_type());
            return;
        }
        case T::MAP: {
            const auto& ms = static_cast<const arrow::MapScalar&>(scalar);
            const auto& map_type = static_cast<const arrow::MapType&>(type);
            auto struct_arr = std::static_pointer_cast<arrow::StructArray>(ms.value);
            const int64_t count = struct_arr->length();
            auto key_arr = struct_arr->field(0);
            auto val_arr = struct_arr->field(1);

            AppendFixed(buf, static_cast<uint32_t>(count));

            // Keys: no null bitfield (keys are never null).
            for (int64_t i = 0; i < count; ++i) {
                auto key = key_arr->GetScalar(i).ValueOrDie();
                EncodePositionalValue(buf, *key, *map_type.key_type());
            }

            // Values: null bitfield + payloads. Pass int64_t count directly
            // (no narrowing static_cast<int>).
            size_t nbytes = BitfieldBytes(count);
            size_t start = buf.size();
            buf.resize(start + nbytes, 0);
            for (int64_t i = 0; i < count; ++i) {
                if (val_arr->IsNull(i)) buf[start + i / 8] |= static_cast<uint8_t>(1u << (i % 8));
            }
            for (int64_t i = 0; i < count; ++i) {
                if (val_arr->IsNull(i)) continue;
                auto val = val_arr->GetScalar(i).ValueOrDie();
                EncodePositionalValue(buf, *val, *map_type.item_type());
            }
            return;
        }
        case T::SPARSE_UNION: {
            const auto& us = static_cast<const arrow::SparseUnionScalar&>(scalar);
            const auto& union_type = static_cast<const arrow::SparseUnionType&>(type);
            AppendFixed(buf, us.type_code);
            const auto& codes = union_type.type_codes();
            int child_id = -1;
            for (int i = 0; i < static_cast<int>(codes.size()); ++i) {
                if (codes[i] == us.type_code) {
                    child_id = i;
                    break;
                }
            }
            if (child_id < 0 || child_id >= static_cast<int>(us.value.size()))
                throw std::invalid_argument("Codec: invalid sparse union type_code");
            EncodePositionalValue(buf, *us.value[static_cast<size_t>(child_id)],
                                  *union_type.field(child_id)->type());
            return;
        }
        case T::DENSE_UNION: {
            const auto& us = static_cast<const arrow::DenseUnionScalar&>(scalar);
            const auto& union_type = static_cast<const arrow::DenseUnionType&>(type);
            AppendFixed(buf, us.type_code);
            const auto& codes = union_type.type_codes();
            int child_id = -1;
            for (int i = 0; i < static_cast<int>(codes.size()); ++i) {
                if (codes[i] == us.type_code) {
                    child_id = i;
                    break;
                }
            }
            if (child_id < 0) throw std::invalid_argument("Codec: invalid dense union type_code");
            EncodePositionalValue(buf, *us.value, *union_type.field(child_id)->type());
            return;
        }
        default:
            // Scalar — reuse the existing scalar encoder.
            detail::EncodeScalar(buf, scalar);
            return;
    }
}

// ---------------------------------------------------------------------------
// Decode helpers
// ---------------------------------------------------------------------------

std::shared_ptr<arrow::Scalar> DecodePositionalStruct(
    detail::Reader& r, const std::shared_ptr<arrow::DataType>& type) {
    const auto& stype = static_cast<const arrow::StructType&>(*type);
    const int n = stype.num_fields();

    const uint8_t* bitfield = r.ReadBytes(BitfieldBytes(n));

    arrow::ScalarVector children(n);
    for (int i = 0; i < n; ++i) {
        if (ReadNullBit(bitfield, i)) {
            children[i] = arrow::MakeNullScalar(stype.field(i)->type());
        } else {
            children[i] = DecodePositionalValue(r, stype.field(i)->type());
        }
    }
    return std::make_shared<arrow::StructScalar>(std::move(children), type);
}

std::shared_ptr<arrow::Array> DecodeListElements(
    detail::Reader& r, int64_t count, const std::shared_ptr<arrow::DataType>& elem_type) {
    const uint8_t* bitfield = r.ReadBytes(BitfieldBytes(count));

    auto builder = arrow::MakeBuilder(elem_type).ValueOrDie();
    for (int64_t i = 0; i < count; ++i) {
        if (ReadNullBit(bitfield, static_cast<int>(i))) {
            auto st = builder->AppendNull();
            if (!st.ok())
                throw std::invalid_argument("Codec: builder AppendNull failed: " + st.ToString());
        } else {
            auto scalar = DecodePositionalValue(r, elem_type);
            auto st = builder->AppendScalar(*scalar);
            if (!st.ok())
                throw std::invalid_argument("Codec: builder AppendScalar failed: " + st.ToString());
        }
    }
    return builder->Finish().ValueOrDie();
}

std::shared_ptr<arrow::Scalar> DecodePositionalValue(detail::Reader& r,
                                                     const std::shared_ptr<arrow::DataType>& type) {
    using T = arrow::Type;

    switch (type->id()) {
        case T::STRUCT:
            return DecodePositionalStruct(r, type);

        case T::LIST:
        case T::LARGE_LIST: {
            const auto& list_type = static_cast<const arrow::BaseListType&>(*type);
            uint32_t count = r.Read<uint32_t>();
            // Reject a corrupt/oversized count before allocating or looping.
            // List elements can be null, and a null element has no payload
            // bytes, so a valid all-null list's count can legitimately exceed
            // the remaining byte count. The only safe lower bound is the
            // element null bitfield itself: require BitfieldBytes(count) to fit.
            if (BitfieldBytes(count) > r.remaining())
                throw std::invalid_argument("Codec: list element count exceeds remaining buffer");
            auto arr = DecodeListElements(r, count, list_type.value_type());
            if (type->id() == T::LIST) return std::make_shared<arrow::ListScalar>(arr, type);
            return std::make_shared<arrow::LargeListScalar>(arr, type);
        }
        case T::FIXED_SIZE_LIST: {
            const auto& fsl_type = static_cast<const arrow::FixedSizeListType&>(*type);
            int32_t count = fsl_type.list_size();
            auto arr = DecodeListElements(r, count, fsl_type.value_type());
            auto actual_type = arrow::fixed_size_list(arr->type(), count);
            return std::make_shared<arrow::FixedSizeListScalar>(arr, actual_type);
        }
        case T::MAP: {
            const auto& map_type = static_cast<const arrow::MapType&>(*type);
            uint32_t count = r.Read<uint32_t>();
            // Map keys are non-null, so every entry needs at least one key
            // byte: count > remaining is a valid (tight) lower bound here,
            // unlike lists (whose elements may be null and payload-free).
            if (count > r.remaining())
                throw std::invalid_argument("Codec: map entry count exceeds remaining buffer");

            // Keys: no null bitfield.
            auto key_builder = arrow::MakeBuilder(map_type.key_type()).ValueOrDie();
            for (uint32_t i = 0; i < count; ++i) {
                auto key = DecodePositionalValue(r, map_type.key_type());
                auto st = key_builder->AppendScalar(*key);
                if (!st.ok())
                    throw std::invalid_argument("Codec: key append failed: " + st.ToString());
            }

            // Values: null bitfield + payloads.
            const uint8_t* val_bitfield = r.ReadBytes(BitfieldBytes(count));
            auto val_builder = arrow::MakeBuilder(map_type.item_type()).ValueOrDie();
            for (uint32_t i = 0; i < count; ++i) {
                if (ReadNullBit(val_bitfield, static_cast<int>(i))) {
                    auto st = val_builder->AppendNull();
                    if (!st.ok())
                        throw std::invalid_argument("Codec: value AppendNull failed: " +
                                                    st.ToString());
                } else {
                    auto val = DecodePositionalValue(r, map_type.item_type());
                    auto st = val_builder->AppendScalar(*val);
                    if (!st.ok())
                        throw std::invalid_argument("Codec: value append failed: " + st.ToString());
                }
            }

            auto key_arr = key_builder->Finish().ValueOrDie();
            auto val_arr = val_builder->Finish().ValueOrDie();
            // Build the entries struct and the MapScalar from the schema's own
            // types (map_type.value_type() is the entries struct), and return a
            // scalar typed as the original field type — preserving the schema's
            // entries field names, value nullability, and keys_sorted flag
            // instead of reconstructing them with defaults.
            auto entries = std::make_shared<arrow::StructArray>(
                map_type.value_type(), static_cast<int64_t>(count),
                arrow::ArrayVector{key_arr, val_arr});
            return std::make_shared<arrow::MapScalar>(entries, type);
        }
        case T::SPARSE_UNION: {
            const auto& union_type = static_cast<const arrow::SparseUnionType&>(*type);
            int8_t type_code = r.Read<int8_t>();
            const auto& codes = union_type.type_codes();
            int child_id = -1;
            for (int i = 0; i < static_cast<int>(codes.size()); ++i) {
                if (codes[i] == type_code) {
                    child_id = i;
                    break;
                }
            }
            if (child_id < 0) throw std::invalid_argument("Codec: unknown sparse union type_code");
            auto active = DecodePositionalValue(r, union_type.field(child_id)->type());
            arrow::ScalarVector children;
            children.reserve(union_type.num_fields());
            for (int i = 0; i < union_type.num_fields(); ++i) {
                children.push_back(
                    i == child_id ? active : arrow::MakeNullScalar(union_type.field(i)->type()));
            }
            return std::make_shared<arrow::SparseUnionScalar>(std::move(children), type_code, type);
        }
        case T::DENSE_UNION: {
            const auto& union_type = static_cast<const arrow::DenseUnionType&>(*type);
            int8_t type_code = r.Read<int8_t>();
            const auto& codes = union_type.type_codes();
            int child_id = -1;
            for (int i = 0; i < static_cast<int>(codes.size()); ++i) {
                if (codes[i] == type_code) {
                    child_id = i;
                    break;
                }
            }
            if (child_id < 0) throw std::invalid_argument("Codec: unknown dense union type_code");
            return std::make_shared<arrow::DenseUnionScalar>(
                DecodePositionalValue(r, union_type.field(child_id)->type()), type_code, type);
        }
        default:
            // Scalar — reuse existing scalar decoder.
            return detail::DecodeScalarFromReader(r, type);
    }
}

}  // anonymous namespace

// ---------------------------------------------------------------------------
// Codec
// ---------------------------------------------------------------------------

Codec::Codec(std::shared_ptr<arrow::Schema> schema) : schema_(std::move(schema)) {}

EncodedRow Codec::EncodeRow(const ArrowRow& values) const {
    const int num_fields = schema_->num_fields();

    if (static_cast<int>(values.size()) != num_fields)
        throw std::invalid_argument(
            "Codec::EncodeRow: values.size() (" + std::to_string(values.size()) +
            ") does not match schema.num_fields() (" + std::to_string(num_fields) + ")");

    std::vector<uint8_t> buf;
    buf.reserve(128);

    // Null bitfield.
    WriteNullBitfield(buf, values, num_fields);

    // Payloads for non-null fields.
    for (int i = 0; i < num_fields; ++i) {
        const auto& scalar = values[i];
        if (!scalar || !scalar->is_valid) continue;

        const auto& field_type = *schema_->field(i)->type();
        bool type_ok = scalar->type->id() == field_type.id();
        if (!type_ok && field_type.id() == arrow::Type::DICTIONARY) {
            // A dictionary field may be supplied as a DictionaryScalar or as a
            // plain value-type scalar; it is transferred as its value type.
            const auto& value_type =
                *static_cast<const arrow::DictionaryType&>(field_type).value_type();
            type_ok = scalar->type->id() == value_type.id();
        }
        if (!type_ok)
            throw std::invalid_argument(
                "Codec::EncodeRow: type mismatch for field '" + schema_->field(i)->name() +
                "': schema expects " + field_type.ToString() + ", got " + scalar->type->ToString());

        EncodePositionalValue(buf, *scalar, field_type);
    }

    return buf;
}

ArrowRow Codec::DecodeRow(const EncodedRow& buf) const { return DecodeRow(buf.data(), buf.size()); }

ArrowRow Codec::DecodeRow(const uint8_t* data, size_t len) const {
    detail::Reader r{data, len};
    const int num_fields = schema_->num_fields();

    const uint8_t* bitfield = r.ReadBytes(BitfieldBytes(num_fields));

    ArrowRow values(num_fields);
    for (int i = 0; i < num_fields; ++i) {
        if (ReadNullBit(bitfield, i)) {
            values[i] = arrow::MakeNullScalar(schema_->field(i)->type());
        } else {
            values[i] = DecodePositionalValue(r, schema_->field(i)->type());
        }
    }

    // A well-formed buffer is consumed exactly. Trailing bytes mean the buffer
    // does not match the schema (corruption, a length-prefix bug, or a
    // concatenated/padded payload) — reject rather than silently accept.
    // Top-level only: nested struct/list/map/union decode share this same
    // reader cursor and must not be checked here.
    if (r.pos != r.size)
        throw std::invalid_argument("Codec::DecodeRow: buffer not fully consumed (" +
                                    std::to_string(r.remaining()) +
                                    " trailing byte(s)); does not match schema");

    return values;
}

}  // namespace fletcher
