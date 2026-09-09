// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include "fletcher/arrow_bridge/codec.hpp"

#include <arrow/api.h>

#include <cstdint>
#include <cstring>
#include <fletcher/core/positional_io.hpp>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "fletcher/arrow_bridge/detail/arrow_result.hpp"
#include "row_reader.hpp"
#include "scalar_codec.hpp"

namespace fletcher {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

using detail::AllValid;
using detail::AppendRun;
using detail::BitfieldBytes;
using detail::FixedWidth;
using detail::ReadNullBit;

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------

void EncodeScalarValue(PositionalWriter& w, const arrow::Scalar& scalar,
                       const arrow::DataType& type);
void EncodeElement(PositionalWriter& w, const arrow::Array& arr, int64_t i,
                   const arrow::DataType& type);
std::shared_ptr<arrow::Scalar> DecodePositionalValue(detail::Reader& r,
                                                     const std::shared_ptr<arrow::DataType>& type);

// ---------------------------------------------------------------------------
// Encode helpers
// ---------------------------------------------------------------------------

// A wire-supplied element/entry count is checked against this before it is narrowed to the
// wire's uint32_t COUNT field — the wire format has no room for more.
void CheckCountFits(int64_t count, const char* what) {
    if (count > static_cast<int64_t>(std::numeric_limits<uint32_t>::max()))
        throw std::invalid_argument(std::string("Codec: ") + what + " exceeds 4 GiB limit");
}

// The walker below indexes an array's buffers directly (both the bulk fixed-width path and the
// per-element accessors), unlike arrow::Array::GetScalar's checked access — so a caller-supplied
// array whose structure doesn't match its own declared length (e.g. a child shorter than its
// struct parent claims) is a raw out-of-bounds read here, not a caught Status. Validate() is
// O(descendants), not O(elements) — it checks buffer sizes against declared lengths/offsets, not
// element content — so this is one cheap call per top-level list/map value, not a per-element
// cost, and it recursively covers every array nested inside `arr` too.
void CheckStructurallyValid(const arrow::Array& arr, const char* what) {
    auto st = arr.Validate();
    if (!st.ok())
        throw std::invalid_argument(std::string("Codec: malformed ") + what + ": " + st.ToString());
}

// PositionalWriter has no fixed-size-list primitive: same element bitfield as a list, no count.
PositionalWriter::ListContext BeginFixedSizeList(WriteBuffer& buf, int32_t n) {
    const size_t offset = buf.Position();
    buf.AppendZeros(BitfieldBytes(n));
    return PositionalWriter::ListContext{buf, offset, static_cast<uint32_t>(n)};
}

// The body of a list: element null bits, then the non-null payloads. An all-valid run of a
// fixed-width type (not bool — Arrow packs bools into bits) goes out as one Append straight from
// the values buffer.
void EncodeElements(PositionalWriter& w, PositionalWriter::ListContext& lc, const arrow::Array& arr,
                    int64_t start, int64_t count, const arrow::DataType& elem_type) {
    bool any_null = false;
    for (int64_t j = 0; j < count; ++j) {
        if (arr.IsNull(start + j)) {
            lc.SetElementNull(static_cast<uint32_t>(j));
            any_null = true;
        }
    }
    if (count == 0) return;
    const int32_t width = FixedWidth(elem_type);
    if (!any_null && width > 0 && elem_type.id() != arrow::Type::BOOL) {
        // buffers[1] is the values buffer of every fixed-width layout (numeric, temporal,
        // interval, decimal, fixed_size_binary); arr.offset() accounts for a sliced array,
        // start for the list window.
        const uint8_t* raw =
            arr.data()->buffers[1]->data() + (arr.offset() + start) * static_cast<int64_t>(width);
        w.buf().Append(raw, static_cast<size_t>(count) * static_cast<size_t>(width));
        return;
    }
    for (int64_t j = 0; j < count; ++j) {
        if (!arr.IsNull(start + j)) EncodeElement(w, arr, start + j, elem_type);
    }
}

// Writes one non-null value of `type` at index `i` of `arr` (the schema element type; the array's
// own type equals it by construction). Used for list/map/union elements, where Arrow has no
// GetScalar-free per-value scalar type — only a typed array to index into.
template <typename ArrowType>
void AppendValue(PositionalWriter& w, const arrow::Array& arr, int64_t i) {
    w.buf().AppendFixed(
        static_cast<const typename arrow::TypeTraits<ArrowType>::ArrayType&>(arr).Value(i));
}

void EncodeElement(PositionalWriter& w, const arrow::Array& arr, int64_t i,
                   const arrow::DataType& type) {
    using T = arrow::Type;

    switch (type.id()) {
        case T::BOOL:
            w.WriteBool(static_cast<const arrow::BooleanArray&>(arr).Value(i));
            return;
        case T::INT8:
            AppendValue<arrow::Int8Type>(w, arr, i);
            return;
        case T::INT16:
            AppendValue<arrow::Int16Type>(w, arr, i);
            return;
        case T::INT32:
            AppendValue<arrow::Int32Type>(w, arr, i);
            return;
        case T::INT64:
            AppendValue<arrow::Int64Type>(w, arr, i);
            return;
        case T::UINT8:
            AppendValue<arrow::UInt8Type>(w, arr, i);
            return;
        case T::UINT16:
            AppendValue<arrow::UInt16Type>(w, arr, i);
            return;
        case T::UINT32:
            AppendValue<arrow::UInt32Type>(w, arr, i);
            return;
        case T::UINT64:
            AppendValue<arrow::UInt64Type>(w, arr, i);
            return;
        case T::HALF_FLOAT:
            AppendValue<arrow::HalfFloatType>(w, arr, i);
            return;
        case T::FLOAT:
            AppendValue<arrow::FloatType>(w, arr, i);
            return;
        case T::DOUBLE:
            AppendValue<arrow::DoubleType>(w, arr, i);
            return;
        case T::DATE32:
            AppendValue<arrow::Date32Type>(w, arr, i);
            return;
        case T::DATE64:
            AppendValue<arrow::Date64Type>(w, arr, i);
            return;
        case T::TIME32:
            AppendValue<arrow::Time32Type>(w, arr, i);
            return;
        case T::TIME64:
            AppendValue<arrow::Time64Type>(w, arr, i);
            return;
        case T::TIMESTAMP:
            AppendValue<arrow::TimestampType>(w, arr, i);
            return;
        case T::DURATION:
            AppendValue<arrow::DurationType>(w, arr, i);
            return;
        case T::INTERVAL_MONTHS:
            AppendValue<arrow::MonthIntervalType>(w, arr, i);
            return;
        case T::INTERVAL_DAY_TIME:
            AppendValue<arrow::DayTimeIntervalType>(w, arr, i);
            return;
        case T::INTERVAL_MONTH_DAY_NANO:
            AppendValue<arrow::MonthDayNanoIntervalType>(w, arr, i);
            return;
        case T::DECIMAL128:
        case T::DECIMAL256:
        case T::FIXED_SIZE_BINARY: {
            // Decimal128Array/Decimal256Array derive from FixedSizeBinaryArray; GetValue is
            // offset-adjusted, and the bytes it returns are the same little-endian layout
            // Decimal128::ToBytes/Decimal256::ToBytes produce.
            const auto& a = static_cast<const arrow::FixedSizeBinaryArray&>(arr);
            w.buf().Append(a.GetValue(i), static_cast<size_t>(a.byte_width()));
            return;
        }
        case T::STRING:
        case T::BINARY: {
            auto v = static_cast<const arrow::BinaryArray&>(arr).GetView(i);
            CheckCountFits(static_cast<int64_t>(v.size()), "variable-length element");
            w.WriteBinary(reinterpret_cast<const uint8_t*>(v.data()), v.size());
            return;
        }
        case T::LARGE_STRING:
        case T::LARGE_BINARY: {
            auto v = static_cast<const arrow::LargeBinaryArray&>(arr).GetView(i);
            CheckCountFits(static_cast<int64_t>(v.size()), "variable-length element");
            w.WriteBinary(reinterpret_cast<const uint8_t*>(v.data()), v.size());
            return;
        }
        case T::STRING_VIEW:
        case T::BINARY_VIEW: {
            auto v = static_cast<const arrow::BinaryViewArray&>(arr).GetView(i);
            CheckCountFits(static_cast<int64_t>(v.size()), "variable-length element");
            w.WriteBinary(reinterpret_cast<const uint8_t*>(v.data()), v.size());
            return;
        }
        case T::STRUCT: {
            const auto& sa = static_cast<const arrow::StructArray&>(arr);
            const auto& st = static_cast<const arrow::StructType&>(type);
            PositionalWriter sw = w.BeginStruct(st.num_fields());
            for (int k = 0; k < st.num_fields(); ++k) {
                // field(k) is already windowed to the struct's own offset — index with the
                // same i as the struct itself, never re-slice.
                auto child = sa.field(k);
                if (child->IsNull(i)) {
                    sw.SetNull(k);
                } else {
                    EncodeElement(sw, *child, i, *st.field(k)->type());
                }
            }
            return;
        }
        case T::LIST: {
            const auto& la = static_cast<const arrow::ListArray&>(arr);
            const auto& list_type = static_cast<const arrow::ListType&>(type);
            const int64_t off = la.value_offset(i);
            const int64_t len = la.value_length(i);
            CheckCountFits(len, "list element count");
            auto lc = w.BeginList(static_cast<uint32_t>(len));
            // values() is the UNSLICED child — index it at off + j, not i + j.
            EncodeElements(w, lc, *la.values(), off, len, *list_type.value_type());
            return;
        }
        case T::LARGE_LIST: {
            const auto& la = static_cast<const arrow::LargeListArray&>(arr);
            const auto& list_type = static_cast<const arrow::LargeListType&>(type);
            const int64_t off = la.value_offset(i);
            const int64_t len = la.value_length(i);
            CheckCountFits(len, "list element count");
            auto lc = w.BeginList(static_cast<uint32_t>(len));
            EncodeElements(w, lc, *la.values(), off, len, *list_type.value_type());
            return;
        }
        case T::FIXED_SIZE_LIST: {
            const auto& fa = static_cast<const arrow::FixedSizeListArray&>(arr);
            const auto& fsl_type = static_cast<const arrow::FixedSizeListType&>(type);
            const int32_t n = fsl_type.list_size();
            auto lc = BeginFixedSizeList(w.buf(), n);
            EncodeElements(w, lc, *fa.values(), fa.value_offset(i), n, *fsl_type.value_type());
            return;
        }
        case T::MAP: {
            const auto& ma = static_cast<const arrow::MapArray&>(arr);
            const auto& map_type = static_cast<const arrow::MapType&>(type);
            const int64_t off = ma.value_offset(i);
            const int64_t len = ma.value_length(i);
            CheckCountFits(len, "map entry count");
            // keys()/items() are the flattened children — index at off + j.
            const arrow::Array& keys = *ma.keys();
            const arrow::Array& items = *ma.items();
            auto mc = w.BeginMap(static_cast<uint32_t>(len));
            for (int64_t j = 0; j < len; ++j) {
                if (keys.IsNull(off + j))
                    throw std::invalid_argument("Codec: map key must not be null");
                EncodeElement(w, keys, off + j, *map_type.key_type());
            }
            auto vals = mc.BeginValues();
            for (int64_t j = 0; j < len; ++j) {
                if (items.IsNull(off + j))
                    vals.SetElementNull(static_cast<uint32_t>(j));
                else
                    EncodeElement(w, items, off + j, *map_type.item_type());
            }
            return;
        }
        case T::SPARSE_UNION:
        case T::DENSE_UNION: {
            const auto& ua = static_cast<const arrow::UnionArray&>(arr);
            const auto& utype = static_cast<const arrow::UnionType&>(type);
            const int8_t code = ua.type_code(i);
            const int child = ua.child_id(i);
            w.buf().AppendFixed(code);
            // UnionArray::field(): sparse is offset-adjusted (index i); dense is raw
            // (index DenseUnionArray::value_offset(i), itself offset-adjusted).
            const arrow::Array& c = *ua.field(child);
            const int64_t idx =
                type.id() == T::DENSE_UNION
                    ? static_cast<const arrow::DenseUnionArray&>(arr).value_offset(i)
                    : i;
            EncodeElement(w, c, idx, *utype.field(child)->type());
            return;
        }
        case T::DICTIONARY: {
            const auto& da = static_cast<const arrow::DictionaryArray&>(arr);
            const auto& dict_type = static_cast<const arrow::DictionaryType&>(type);
            const int64_t idx = da.GetValueIndex(i);
            const arrow::Array& dict = *da.dictionary();
            if (dict.IsNull(idx)) throw std::invalid_argument("Codec: dictionary entry is null");
            EncodeElement(w, dict, idx, *dict_type.value_type());
            return;
        }
        default:
            throw std::invalid_argument("Codec: unsupported Arrow type: " + type.ToString());
    }
}

// Writes one field/child value (which may itself be a nested composite) from its arrow::Scalar
// representation. Used for the row's top-level fields and for a struct scalar's children, where
// Arrow hands each field an individually-typed Scalar rather than an array + index.
void EncodeScalarValue(PositionalWriter& w, const arrow::Scalar& scalar,
                       const arrow::DataType& type) {
    using T = arrow::Type;

    switch (type.id()) {
        case T::STRUCT: {
            const auto& ss = static_cast<const arrow::StructScalar&>(scalar);
            const auto& st = static_cast<const arrow::StructType&>(type);
            PositionalWriter sw = w.BeginStruct(st.num_fields());
            for (int k = 0; k < st.num_fields(); ++k) {
                const auto& child = ss.value[static_cast<size_t>(k)];
                if (!child || !child->is_valid) {
                    sw.SetNull(k);
                } else {
                    EncodeScalarValue(sw, *child, *st.field(k)->type());
                }
            }
            return;
        }
        case T::LIST:
        case T::LARGE_LIST: {
            const auto& ls = static_cast<const arrow::BaseListScalar&>(scalar);
            const auto& list_type = static_cast<const arrow::BaseListType&>(type);
            const arrow::Array& arr = *ls.value;
            CheckStructurallyValid(arr, "list value");
            const int64_t count = arr.length();
            CheckCountFits(count, "list element count");
            auto lc = w.BeginList(static_cast<uint32_t>(count));
            EncodeElements(w, lc, arr, 0, count, *list_type.value_type());
            return;
        }
        case T::FIXED_SIZE_LIST: {
            const auto& ls = static_cast<const arrow::BaseListScalar&>(scalar);
            const auto& fsl_type = static_cast<const arrow::FixedSizeListType&>(type);
            const arrow::Array& arr = *ls.value;
            CheckStructurallyValid(arr, "fixed_size_list value");
            const int32_t list_size = fsl_type.list_size();
            if (arr.length() != list_size)
                throw std::invalid_argument(
                    "Codec: fixed_size_list value length (" + std::to_string(arr.length()) +
                    ") does not match schema list_size (" + std::to_string(list_size) + ")");
            // No COUNT prefix — the fixed size is in the schema.
            auto lc = BeginFixedSizeList(w.buf(), list_size);
            EncodeElements(w, lc, arr, 0, list_size, *fsl_type.value_type());
            return;
        }
        case T::MAP: {
            const auto& ms = static_cast<const arrow::MapScalar&>(scalar);
            const auto& map_type = static_cast<const arrow::MapType&>(type);
            const auto& entries = static_cast<const arrow::StructArray&>(*ms.value);
            CheckStructurallyValid(entries, "map entries");
            // entries.field(k) is windowed to the entries struct's own offset — index at j.
            const arrow::Array& keys = *entries.field(0);
            const arrow::Array& items = *entries.field(1);
            const int64_t n = entries.length();
            CheckCountFits(n, "map entry count");
            auto mc = w.BeginMap(static_cast<uint32_t>(n));
            // Keys: no null bitfield (keys are never null).
            for (int64_t j = 0; j < n; ++j) {
                if (keys.IsNull(j)) throw std::invalid_argument("Codec: map key must not be null");
                EncodeElement(w, keys, j, *map_type.key_type());
            }
            auto vals = mc.BeginValues();
            for (int64_t j = 0; j < n; ++j) {
                if (items.IsNull(j))
                    vals.SetElementNull(static_cast<uint32_t>(j));
                else
                    EncodeElement(w, items, j, *map_type.item_type());
            }
            return;
        }
        case T::SPARSE_UNION: {
            const auto& us = static_cast<const arrow::SparseUnionScalar&>(scalar);
            const auto& union_type = static_cast<const arrow::SparseUnionType&>(type);
            const auto& ids = union_type.child_ids();
            int child_id = (us.type_code < 0 || us.type_code >= static_cast<int>(ids.size()))
                               ? -1
                               : ids[static_cast<size_t>(us.type_code)];
            if (child_id < 0 || child_id >= static_cast<int>(us.value.size()))
                throw std::invalid_argument("Codec: invalid sparse union type_code");
            w.buf().AppendFixed(us.type_code);
            EncodeScalarValue(w, *us.value[static_cast<size_t>(child_id)],
                              *union_type.field(child_id)->type());
            return;
        }
        case T::DENSE_UNION: {
            const auto& us = static_cast<const arrow::DenseUnionScalar&>(scalar);
            const auto& union_type = static_cast<const arrow::DenseUnionType&>(type);
            const auto& ids = union_type.child_ids();
            int child_id = (us.type_code < 0 || us.type_code >= static_cast<int>(ids.size()))
                               ? -1
                               : ids[static_cast<size_t>(us.type_code)];
            if (child_id < 0) throw std::invalid_argument("Codec: invalid dense union type_code");
            w.buf().AppendFixed(us.type_code);
            EncodeScalarValue(w, *us.value, *union_type.field(child_id)->type());
            return;
        }
        default:
            // Scalar — reuse the existing scalar encoder.
            detail::EncodeScalar(w.buf(), scalar);
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
    // A DICTIONARY list element is transferred as its resolved value — the same contract as a
    // top-level dictionary field (codec.hpp) — and DecodePositionalValue's default case already
    // returns that plain value scalar for it. A DictionaryBuilder's AppendScalar requires an
    // actual DictionaryScalar, so building one here and feeding it a value scalar corrupts memory
    // instead of failing cleanly; build the value-typed array directly instead. The caller
    // (DecodePositionalValue) types the resulting List/LargeList/FixedSizeListScalar to match.
    if (elem_type->id() == arrow::Type::DICTIONARY) {
        return DecodeListElements(
            r, count, static_cast<const arrow::DictionaryType&>(*elem_type).value_type());
    }
    const uint8_t* bitfield = r.ReadBytes(BitfieldBytes(count));

    auto builder =
        detail::ValueOrThrow(arrow::MakeBuilder(elem_type), "Codec: list MakeBuilder failed");

    // A run of fixed-width elements with no nulls is laid out on the wire as `count` values back
    // to back (EncodeElements writes an all-valid fixed-width run with one bulk Append), so for
    // any fixed-width type the typed builder is filled straight from the buffer with one memcpy.
    // The scalar path below costs one heap-allocated arrow::Scalar and one virtual AppendScalar per
    // element — ~13 300 allocations for a 2 667-point cloud row, ~450 k/s at a 10 Hz 8 k-point
    // cloud — which was the bridge's dominant CPU cost.
    const int32_t width = FixedWidth(*elem_type);
    if (count > 0 && width > 0 && AllValid(bitfield, count)) {
        // Divide, never multiply: a wrapped `count * width` would pass ReadBytes' own check.
        if (static_cast<uint64_t>(count) > r.remaining() / static_cast<size_t>(width))
            throw std::invalid_argument("Codec: list payload exceeds remaining buffer");
        const uint8_t* bytes = r.ReadBytes(static_cast<size_t>(count) * static_cast<size_t>(width));
        AppendRun(*builder, elem_type->id(), bytes, count);
        return detail::ValueOrThrow(builder->Finish(), "Codec: list builder Finish failed");
    }
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
    return detail::ValueOrThrow(builder->Finish(), "Codec: list builder Finish failed");
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
            // A dictionary-valued list decodes to its resolved value array (see
            // DecodeListElements); type the scalar to match what it actually holds rather than
            // the schema's dictionary type.
            const bool is_dict = list_type.value_type()->id() == arrow::Type::DICTIONARY;
            if (type->id() == T::LIST) {
                return std::make_shared<arrow::ListScalar>(
                    arr, is_dict ? arrow::list(arr->type()) : type);
            }
            return std::make_shared<arrow::LargeListScalar>(
                arr, is_dict ? arrow::large_list(arr->type()) : type);
        }
        case T::FIXED_SIZE_LIST: {
            const auto& fsl_type = static_cast<const arrow::FixedSizeListType&>(*type);
            int32_t count = fsl_type.list_size();
            auto arr = DecodeListElements(r, count, fsl_type.value_type());
            const bool is_dict = fsl_type.value_type()->id() == arrow::Type::DICTIONARY;
            return std::make_shared<arrow::FixedSizeListScalar>(
                arr, is_dict ? arrow::fixed_size_list(arr->type(), count) : type);
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
            auto key_builder = detail::ValueOrThrow(arrow::MakeBuilder(map_type.key_type()),
                                                    "Codec: map key MakeBuilder failed");
            for (uint32_t i = 0; i < count; ++i) {
                auto key = DecodePositionalValue(r, map_type.key_type());
                auto st = key_builder->AppendScalar(*key);
                if (!st.ok())
                    throw std::invalid_argument("Codec: key append failed: " + st.ToString());
            }

            // Values: null bitfield + payloads.
            const uint8_t* val_bitfield = r.ReadBytes(BitfieldBytes(count));
            auto val_builder = detail::ValueOrThrow(arrow::MakeBuilder(map_type.item_type()),
                                                    "Codec: map value MakeBuilder failed");
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

            auto key_arr =
                detail::ValueOrThrow(key_builder->Finish(), "Codec: map key builder Finish failed");
            auto val_arr = detail::ValueOrThrow(val_builder->Finish(),
                                                "Codec: map value builder Finish failed");
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
            const auto& ids = union_type.child_ids();
            int child_id = (type_code < 0 || type_code >= static_cast<int>(ids.size()))
                               ? -1
                               : ids[static_cast<size_t>(type_code)];
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
            const auto& ids = union_type.child_ids();
            int child_id = (type_code < 0 || type_code >= static_cast<int>(ids.size()))
                               ? -1
                               : ids[static_cast<size_t>(type_code)];
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

void Codec::EncodeRow(const ArrowRow& values, WriteBuffer& out) const {
    const int num_fields = schema_->num_fields();

    if (static_cast<int>(values.size()) != num_fields)
        throw std::invalid_argument(
            "Codec::EncodeRow: values.size() (" + std::to_string(values.size()) +
            ") does not match schema.num_fields() (" + std::to_string(num_fields) + ")");

    PositionalWriter w(out, num_fields);

    for (int i = 0; i < num_fields; ++i) {
        const auto& scalar = values[i];
        if (!scalar || !scalar->is_valid) {
            w.SetNull(i);
            continue;
        }

        const auto& field_type = *schema_->field(i)->type();
        bool type_ok = field_type.Equals(*scalar->type);
        if (!type_ok && field_type.id() == arrow::Type::DICTIONARY) {
            // A dictionary field may be supplied as a DictionaryScalar or as a
            // plain value-type scalar; it is transferred as its value type.
            const auto& value_type =
                *static_cast<const arrow::DictionaryType&>(field_type).value_type();
            type_ok = value_type.Equals(*scalar->type);
        }
        if (!type_ok)
            throw std::invalid_argument(
                "Codec::EncodeRow: type mismatch for field '" + schema_->field(i)->name() +
                "': schema expects " + field_type.ToString() + ", got " + scalar->type->ToString());

        EncodeScalarValue(w, *scalar, field_type);
    }
}

EncodedRow Codec::EncodeRow(const ArrowRow& values) const {
    VectorWriteBuffer buf;
    EncodeRow(values, buf);
    return buf.Finish();
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
