// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The encode half of the codec. Decode arrives with BIND-2b.
//
// ── The one thing to keep in mind while reading ─────────────────────────────
// Every byte written here is compared against `arrow-bridge`'s `Codec` by
// `NanoarrowCodec.ByteIdenticalToArrowBridge`. The layout decisions are not this
// file's to make: they belong to `docs/wire-format-specification.md`, and where
// this file looks like it is choosing something it is reproducing something.
//
// The layout, restated once so the switch below can be read against it:
//
//   row          [NULL_BITFIELD : ceil(n/8), LSB-first] then non-null payloads
//   fixed scalar [PAYLOAD : N bytes, little-endian]
//   string/bytes [LEN : u32][PAYLOAD : LEN]
//   struct       [NULL_BITFIELD] then non-null payloads   (recursive)
//   list         [COUNT : u32][NULL_BITFIELD : ceil(count/8)] then non-null elements
//   fixed list   [NULL_BITFIELD : ceil(size/8)] then non-null elements  (no COUNT)
//   map          [COUNT : u32][keys...][VALUE_NULL_BITFIELD] then non-null values
//
// Keys of a map carry no null bitfield: a null key is not representable.
#include "nanoarrow_codec.hpp"

#include <cstring>
#include <fletcher/core/detail/bitfield.hpp>
#include <fletcher/core/positional_io.hpp>
#include <fletcher/core/status.hpp>
#include <limits>
#include <string>
#include <string_view>

namespace fletcher::abi {
namespace {

/// The name a refusal uses for a field, so the reader can find it.
///
/// nanoarrow leaves `name` NULL for an unnamed child (a list's element, a map's
/// key), which is exactly where a bare name would be least useful, so the path
/// is built as it descends: `readings[]`, `tags{}.value`.
std::string Describe(const std::string& path, const char* name) {
    if (name == nullptr || *name == '\0') return path;
    return path.empty() ? std::string(name) : path + "." + name;
}

[[noreturn]] void Refuse(const std::string& what) {
    throw PubSubError(PubSubStatus::kInvalidArgument, "NanoarrowCodec: " + what);
}

/// Validate one schema node against the wire mapping.
///
/// This is the whole of the "field plan" for now: the mapping is positional and
/// the array view already carries the type tree, so what open must do is REFUSE
/// early and precisely. A type reaching the encoder's switch that this did not
/// accept is a bug in one of the two, which is why the switch's default throws
/// rather than assuming.
void ValidateNode(const ArrowSchema& schema, const std::string& path) {
    ArrowSchemaView view;
    ArrowError error;
    if (ArrowSchemaViewInit(&view, &schema, &error) != NANOARROW_OK) {
        Refuse("field '" + Describe(path, schema.name) + "' has a format string nanoarrow " +
               "cannot parse: " + error.message);
    }

    const std::string here = Describe(path, schema.name);

    switch (view.type) {
        // Scalars the mapping carries. Every one of these is a fixed-width
        // little-endian payload, or a length-prefixed byte run.
        case NANOARROW_TYPE_BOOL:
        case NANOARROW_TYPE_INT8:
        case NANOARROW_TYPE_INT16:
        case NANOARROW_TYPE_INT32:
        case NANOARROW_TYPE_INT64:
        case NANOARROW_TYPE_UINT8:
        case NANOARROW_TYPE_UINT16:
        case NANOARROW_TYPE_UINT32:
        case NANOARROW_TYPE_UINT64:
        case NANOARROW_TYPE_FLOAT:
        case NANOARROW_TYPE_DOUBLE:
        case NANOARROW_TYPE_STRING:
        case NANOARROW_TYPE_LARGE_STRING:
        case NANOARROW_TYPE_BINARY:
        case NANOARROW_TYPE_LARGE_BINARY:
        case NANOARROW_TYPE_DATE32:
        case NANOARROW_TYPE_DATE64:
        case NANOARROW_TYPE_TIME32:
        case NANOARROW_TYPE_TIME64:
        case NANOARROW_TYPE_TIMESTAMP:
        case NANOARROW_TYPE_DURATION:
            return;

        // Composites, recursively.
        case NANOARROW_TYPE_STRUCT:
        case NANOARROW_TYPE_LIST:
        case NANOARROW_TYPE_LARGE_LIST:
        case NANOARROW_TYPE_FIXED_SIZE_LIST:
        case NANOARROW_TYPE_MAP:
            for (int64_t i = 0; i < schema.n_children; ++i) {
                ValidateNode(*schema.children[i], here);
            }
            return;

        // Refused, and each for its own reason rather than a shared shrug.
        case NANOARROW_TYPE_DICTIONARY:
            Refuse("field '" + here +
                   "' is a dictionary. The wire format carries the VALUE type one value per row "
                   "- the indices are a columnar optimisation - so a dictionary column is "
                   "encoded as its value type by the tier above, never here (D-BIND-8)");
        case NANOARROW_TYPE_SPARSE_UNION:
        case NANOARROW_TYPE_DENSE_UNION:
            Refuse("field '" + here +
                   "' is a union. The proto mapping produces no unions, and the wire format's "
                   "union framing exists for the Arrow-native tier only");
        default:
            Refuse("field '" + here + "' has a type the wire format does not carry");
    }
}

/// `true` when the child at `index` of `view` is null at `row`.
bool IsNull(const ArrowArrayView& view, int64_t index, int64_t row) {
    return ArrowArrayViewIsNull(view.children[index], row) != 0;
}

void EncodeValue(const ArrowArrayView& view, int64_t index, WriteBuffer& out);

/// A struct's own framing: a null bitfield over its fields, then the payloads of
/// the ones that are not null, in schema order.
void EncodeStruct(const ArrowArrayView& view, int64_t index, WriteBuffer& out) {
    const int64_t n = view.n_children;
    PositionalWriter writer(out, static_cast<int>(n));

    for (int64_t i = 0; i < n; ++i) {
        if (IsNull(view, i, index)) writer.SetNull(static_cast<int>(i));
    }
    for (int64_t i = 0; i < n; ++i) {
        if (IsNull(view, i, index)) continue;
        EncodeValue(*view.children[i], index, out);
    }
}

/// The elements of one list-shaped value: a null bitfield over `count`, then the
/// payloads of the ones that are not null.
///
/// Shared by list, large list, fixed-size list and a map's values, because the
/// wire format gives all four the same element framing. What differs is only
/// whether a COUNT precedes it - a fixed-size list carries its size in the
/// schema, so writing one would put four bytes on the wire that the decoder does
/// not read - hence `write_count` rather than two near-identical functions.
///
/// The bitfield is written the way `PositionalWriter::BeginList` writes it
/// (append zeros, then OR the null bits into place); it is spelled out here
/// because `BeginList` also writes the count, and this function must be able not
/// to.
void EncodeElements(const ArrowArrayView& child, int64_t first, int64_t count, bool write_count,
                    WriteBuffer& out) {
    if (write_count) out.AppendFixed(static_cast<uint32_t>(count));

    const size_t bitfield_offset = out.Position();
    out.AppendZeros(detail::BitfieldBytes(count));

    for (int64_t i = 0; i < count; ++i) {
        if (ArrowArrayViewIsNull(&child, first + i) == 0) continue;
        out.PatchByte(bitfield_offset + static_cast<size_t>(i / 8),
                      static_cast<uint8_t>(1u << (i % 8)));
    }
    for (int64_t i = 0; i < count; ++i) {
        if (ArrowArrayViewIsNull(&child, first + i) != 0) continue;
        EncodeValue(child, first + i, out);
    }
}

/// One value, by type. The switch mirrors ValidateNode's accept list exactly;
/// its default is unreachable unless the two drift, and says so.
void EncodeValue(const ArrowArrayView& view, int64_t index, WriteBuffer& out) {
    PositionalWriter scalars(out, 0);  // a 0-field writer writes no bitfield

    switch (view.storage_type) {
        case NANOARROW_TYPE_BOOL:
            scalars.WriteBool(ArrowArrayViewGetIntUnsafe(&view, index) != 0);
            return;
        case NANOARROW_TYPE_INT8:
            scalars.WriteInt8(static_cast<int8_t>(ArrowArrayViewGetIntUnsafe(&view, index)));
            return;
        case NANOARROW_TYPE_INT16:
            scalars.WriteInt16(static_cast<int16_t>(ArrowArrayViewGetIntUnsafe(&view, index)));
            return;
        case NANOARROW_TYPE_INT32:
        case NANOARROW_TYPE_DATE32:
        case NANOARROW_TYPE_TIME32:
            scalars.WriteInt32(static_cast<int32_t>(ArrowArrayViewGetIntUnsafe(&view, index)));
            return;
        case NANOARROW_TYPE_INT64:
        case NANOARROW_TYPE_DATE64:
        case NANOARROW_TYPE_TIME64:
        case NANOARROW_TYPE_TIMESTAMP:
        case NANOARROW_TYPE_DURATION:
            scalars.WriteInt64(ArrowArrayViewGetIntUnsafe(&view, index));
            return;
        case NANOARROW_TYPE_UINT8:
            scalars.WriteUint8(static_cast<uint8_t>(ArrowArrayViewGetUIntUnsafe(&view, index)));
            return;
        case NANOARROW_TYPE_UINT16:
            scalars.WriteUint16(static_cast<uint16_t>(ArrowArrayViewGetUIntUnsafe(&view, index)));
            return;
        case NANOARROW_TYPE_UINT32:
            scalars.WriteUint32(static_cast<uint32_t>(ArrowArrayViewGetUIntUnsafe(&view, index)));
            return;
        case NANOARROW_TYPE_UINT64:
            scalars.WriteUint64(ArrowArrayViewGetUIntUnsafe(&view, index));
            return;
        case NANOARROW_TYPE_FLOAT:
            scalars.WriteFloat(static_cast<float>(ArrowArrayViewGetDoubleUnsafe(&view, index)));
            return;
        case NANOARROW_TYPE_DOUBLE:
            scalars.WriteDouble(ArrowArrayViewGetDoubleUnsafe(&view, index));
            return;

        case NANOARROW_TYPE_STRING:
        case NANOARROW_TYPE_LARGE_STRING:
        case NANOARROW_TYPE_BINARY:
        case NANOARROW_TYPE_LARGE_BINARY: {
            // One shape for all four: [LEN : u32][bytes]. The distinction
            // between string and binary is the schema's, not the wire's, and
            // between 32- and 64-bit offsets is the array's, not the wire's.
            const ArrowBufferView bytes = ArrowArrayViewGetBytesUnsafe(&view, index);
            if (bytes.size_bytes < 0 ||
                bytes.size_bytes > static_cast<int64_t>(std::numeric_limits<uint32_t>::max())) {
                throw std::invalid_argument(
                    "NanoarrowCodec: variable-length field exceeds 4 GiB limit");
            }
            scalars.WriteBinary(bytes.data.as_uint8, static_cast<size_t>(bytes.size_bytes));
            return;
        }

        case NANOARROW_TYPE_STRUCT:
            EncodeStruct(view, index, out);
            return;

        case NANOARROW_TYPE_LIST:
        case NANOARROW_TYPE_LARGE_LIST: {
            const int64_t first = ArrowArrayViewListChildOffset(&view, index);
            const int64_t last = ArrowArrayViewListChildOffset(&view, index + 1);
            const int64_t count = last - first;
            if (count < 0 || count > static_cast<int64_t>(std::numeric_limits<uint32_t>::max())) {
                throw std::invalid_argument("NanoarrowCodec: list length exceeds the wire's u32");
            }
            EncodeElements(*view.children[0], first, count, /*write_count=*/true, out);
            return;
        }

        case NANOARROW_TYPE_FIXED_SIZE_LIST: {
            // No COUNT prefix: the size is in the schema, so both sides know it.
            const int64_t size = view.layout.child_size_elements;
            EncodeElements(*view.children[0], index * size, size, /*write_count=*/false, out);
            return;
        }

        case NANOARROW_TYPE_MAP: {
            // A map's child 0 is a struct of {key, value}; the entry range comes
            // from the list offsets exactly as a list's does.
            const int64_t first = ArrowArrayViewListChildOffset(&view, index);
            const int64_t last = ArrowArrayViewListChildOffset(&view, index + 1);
            const int64_t count = last - first;
            if (count < 0 || count > static_cast<int64_t>(std::numeric_limits<uint32_t>::max())) {
                throw std::invalid_argument("NanoarrowCodec: map size exceeds the wire's u32");
            }
            const ArrowArrayView& entries = *view.children[0];
            const ArrowArrayView& keys = *entries.children[0];
            const ArrowArrayView& values = *entries.children[1];

            out.AppendFixed(static_cast<uint32_t>(count));

            // Keys first, with NO null bitfield: a null key is not representable.
            for (int64_t i = 0; i < count; ++i) {
                EncodeValue(keys, first + i, out);
            }
            EncodeElements(values, first, count, /*write_count=*/false, out);
            return;
        }

        default:
            // Unreachable: ValidateNode refused every type this switch does not
            // handle, at open. If this fires, the two have drifted apart.
            Refuse(
                "internal: a value of a type the schema validation accepted reached the "
                "encoder's switch unhandled");
    }
}

}  // namespace

NanoarrowCodec::NanoarrowCodec(const ArrowSchema& schema)
    : schema_(OwnedSchema::DeepCopy(&schema)) {
    ArrowSchemaView view;
    ArrowError error;
    if (ArrowSchemaViewInit(&view, schema_.get(), &error) != NANOARROW_OK) {
        Refuse(std::string("the schema's format string cannot be parsed: ") + error.message);
    }
    if (view.type != NANOARROW_TYPE_STRUCT) {
        Refuse(
            "the schema must be a struct - a row is a struct of fields, and a top-level scalar "
            "would have no null bitfield to live in");
    }
    for (int64_t i = 0; i < schema_.get()->n_children; ++i) {
        ValidateNode(*schema_.get()->children[i], std::string());
    }
}

BoundRows::BoundRows(const NanoarrowCodec& codec, const ArrowArray& array)
    : view_(std::make_unique<ArrowArrayView>()) {
    ArrowError error;
    if (ArrowArrayViewInitFromSchema(view_.get(), codec.schema_.get(), &error) != NANOARROW_OK) {
        Refuse(std::string("the codec's schema could not be viewed: ") + error.message);
    }
    if (ArrowArrayViewSetArray(view_.get(), &array, &error) != NANOARROW_OK) {
        // This is where nanoarrow validates every buffer of every child, once
        // per batch. A malformed export is caught HERE rather than at row 4711.
        ArrowArrayViewReset(view_.get());
        Refuse(std::string("the array does not match the codec's schema: ") + error.message);
    }
    length_ = array.length;
}

BoundRows::~BoundRows() {
    // Releases the VIEW only. The array itself is borrowed: calling its release
    // callback here would consume an export the caller still owns, which is the
    // one mistake this type exists to make impossible.
    ArrowArrayViewReset(view_.get());
}

void NanoarrowCodec::EncodeRow(const BoundRows& rows, int64_t index, WriteBuffer& out) const {
    if (index < 0 || index >= rows.length()) {
        throw std::out_of_range("NanoarrowCodec::EncodeRow: row index " + std::to_string(index) +
                                " is outside the bound batch of " + std::to_string(rows.length()) +
                                " rows");
    }
    EncodeStruct(rows.view(), index, out);
}

}  // namespace fletcher::abi
