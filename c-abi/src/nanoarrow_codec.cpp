// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The codec behind the binding ABI: encode (BIND-2a) and decode (BIND-2b).
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
//
// ── Decode's one structural rule ────────────────────────────────────────────
// Decode reads through `PositionalReader` and nothing else. Every bounds check
// the HARD rounds put into that reader - the underrun checks, the list-count
// check, the map-count check, the not-fully-consumed check - therefore applies
// to the binding for free, and `DecodeRefusalsComeFromTheReader` asserts that
// property directly rather than maintaining a second taxonomy of malformed
// input here. A refusal this file invents where the reader already has one is a
// bug, because the two would then be free to drift.
#include "nanoarrow_codec.hpp"

#include <cstring>
#include <fletcher/core/detail/bitfield.hpp>
#include <fletcher/core/positional_io.hpp>
#include <fletcher/core/status.hpp>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

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

/// The mapping's scalars: every type that is one payload on the wire rather
/// than a framing plus children.
///
/// One list, two readers. `PlanNode` uses it to decide whether to recurse, and
/// `ReadScalar` to decide whether it can decode the node at all; splitting them
/// is how a type gets accepted at open and then dropped at decode.
bool IsWireScalar(ArrowType type) {
    switch (type) {
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
            return true;
        default:
            return false;
    }
}

/// Validate one schema node against the wire mapping and record its plan.
///
/// The mapping is positional, so "planning" is mostly REFUSING early and
/// precisely - a type reaching a decoder switch that this did not accept is a
/// bug in one of the two, which is why those switches throw rather than assume.
/// What the plan adds beyond the refusal is the type tree itself, which decode
/// has no array to read it from.
FieldPlan PlanNode(const ArrowSchema& schema, const std::string& path) {
    ArrowSchemaView view;
    ArrowError error;
    if (ArrowSchemaViewInit(&view, &schema, &error) != NANOARROW_OK) {
        Refuse("field '" + Describe(path, schema.name) + "' has a format string nanoarrow " +
               "cannot parse: " + error.message);
    }

    const std::string here = Describe(path, schema.name);

    FieldPlan plan;
    plan.type = view.type;

    // Scalars: a fixed-width little-endian payload, or a length-prefixed run.
    if (IsWireScalar(view.type)) return plan;

    switch (view.type) {
        // Composites, recursively.
        case NANOARROW_TYPE_STRUCT:
        case NANOARROW_TYPE_LIST:
        case NANOARROW_TYPE_LARGE_LIST:
        case NANOARROW_TYPE_FIXED_SIZE_LIST:
        case NANOARROW_TYPE_MAP:
            plan.fixed_size =
                view.type == NANOARROW_TYPE_FIXED_SIZE_LIST ? view.fixed_size : int64_t{0};
            plan.children.reserve(static_cast<size_t>(schema.n_children));
            for (int64_t i = 0; i < schema.n_children; ++i) {
                plan.children.push_back(PlanNode(*schema.children[i], here));
            }
            break;

        // A dictionary is a scalar MODIFIER, not a container: the wire carries
        // the VALUE type, one value per row, because the indices are a columnar
        // optimisation with no meaning in a single row
        // (`docs/wire-format-specification.md` §"Dictionary Types", and
        // `arrow-bridge`'s `Codec` has always done this). So the plan for a
        // dictionary field IS the plan for its value type, and every layer below
        // this one - decode's switch, the decoded schema - sees a plain value.
        //
        // D-BIND-8 deferred this to DICT; D-BIND-39 found the premise did not
        // reach here. The halt was `nanoarrow_ipc` rejecting dictionary types,
        // and this path never touches IPC - the schema arrives over the C Data
        // Interface, where nanoarrow carries dictionaries in full.
        case NANOARROW_TYPE_DICTIONARY: {
            if (schema.dictionary == nullptr) {
                Refuse("field '" + here +
                       "' says it is a dictionary but carries no value type, so there is nothing "
                       "to encode");
            }

            ArrowSchemaView value_view;
            ArrowError value_error;
            if (ArrowSchemaViewInit(&value_view, schema.dictionary, &value_error) != NANOARROW_OK) {
                Refuse("field '" + here + "' is a dictionary whose value type cannot be parsed: " +
                       value_error.message);
            }

            // The spec's own restriction, refused HERE so the message can name
            // the field. A composite value type has no single-row form: the wire
            // would have to carry the framing of a struct or a list where the
            // format says one value goes.
            if (!IsWireScalar(value_view.type)) {
                Refuse("field '" + here +
                       "' is a dictionary whose value type is not a scalar. The wire format "
                       "carries a dictionary as its value type, one value per row, so a nested "
                       "value type (struct, list, map or union) has no single-row form (spec "
                       "\"Dictionary Types\")");
            }

            FieldPlan value;
            value.type = value_view.type;
            return value;
        }

        // Refused, and each for its own reason rather than a shared shrug.
        case NANOARROW_TYPE_SPARSE_UNION:
        case NANOARROW_TYPE_DENSE_UNION:
            Refuse("field '" + here +
                   "' is a union. The proto mapping produces no unions, and the wire format's "
                   "union framing exists for the Arrow-native tier only");
        default:
            Refuse("field '" + here + "' has a type the wire format does not carry");
    }

    // A map's shape is load-bearing for both halves: child 0 is the {key, value}
    // entries struct, and both halves index it positionally.
    if (view.type == NANOARROW_TYPE_MAP) {
        if (plan.children.size() != 1 || plan.children[0].children.size() != 2) {
            Refuse("field '" + here +
                   "' is a map whose child is not a two-field {key, value} struct");
        }
        // A composite key is representable on the wire - the encoder would write
        // one - but NOT decodable in one pass: a map's keys all arrive before its
        // values, and holding a half-built composite aside until its value shows
        // up is a second decoder. The proto mapping never produces one (proto
        // restricts map keys to integral, bool and string), so this is refused at
        // open rather than half-supported at decode.
        if (!IsWireScalar(plan.children[0].children[0].type)) {
            Refuse("field '" + here +
                   "' is a map with a non-scalar key. The proto mapping produces integral, bool "
                   "and string keys only, and the wire's keys-then-values layout makes a "
                   "composite key undecodable in a single pass");
        }
    }

    return plan;
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
    // A dictionary column stores INDICES and the wire carries VALUES, so the
    // index is resolved here, once, before the switch - which is what keeps that
    // switch a mirror of the accept list rather than gaining a parallel set of
    // dictionary arms. `storage_type` on a dictionary view is the INDEX type, so
    // without this the encoder would happily put an int32 on the wire where the
    // schema promised a string.
    if (view.dictionary != nullptr) {
        const int64_t code = ArrowArrayViewGetIntUnsafe(&view, index);
        if (code < 0 || code >= view.dictionary->length) {
            // Not an internal error: the caller exported this array, and an index
            // outside its own dictionary is malformed input from the binding's
            // point of view. `ArrowArrayViewGetIntUnsafe` would not have caught it
            // and the read below would be out of bounds.
            throw std::invalid_argument("NanoarrowCodec: a dictionary index of " +
                                        std::to_string(code) + " is outside the dictionary's " +
                                        std::to_string(view.dictionary->length) + " values");
        }
        EncodeValue(*view.dictionary, code, out);
        return;
    }

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

// ---------------------------------------------------------------------------
// Decode (BIND-2b)
// ---------------------------------------------------------------------------

/// nanoarrow's builders return errno-shaped codes. None of them is a malformed
/// input - the reader has already refused those - so a failure here is an
/// allocation failure or a plan that does not match the array being built, and
/// both are internal.
void Must(int code, const char* what);

/// Rewrite every dictionary node of `node` into its value type, in place.
///
/// Decode builds its output array from a SCHEMA, and after D-BIND-39 the schema a
/// caller binds is no longer the schema decode produces: a dictionary field goes
/// out as its value type and comes back as a plain value array. Building the
/// output from the bind schema would tell nanoarrow to make a dictionary array
/// and then append plain values into it.
///
/// Rewriting in place rather than rebuilding composites child by child, because
/// the C Data Interface's own move idiom makes it short: release what the node
/// held, then memcpy the replacement over it. The node's OWN identity - its name,
/// its nullability, its metadata - belongs to the field and is put back on the
/// value type, which carries none of it.
void ResolveDictionaries(ArrowSchema* node) {
    // Children first: a dictionary can sit inside a struct or a list, and
    // resolving the parent afterwards must not walk into what it replaced.
    for (int64_t i = 0; i < node->n_children; ++i) {
        ResolveDictionaries(node->children[i]);
    }

    if (node->dictionary == nullptr) return;

    ArrowSchema value = {};
    Must(ArrowSchemaDeepCopy(node->dictionary, &value), "copying a dictionary's value type");
    Must(ArrowSchemaSetName(&value, node->name == nullptr ? "" : node->name),
         "naming a decoded dictionary field");
    Must(ArrowSchemaSetMetadata(&value, node->metadata),
         "copying a decoded dictionary field's metadata");
    value.flags = node->flags;

    node->release(node);
    std::memcpy(node, &value, sizeof(ArrowSchema));
}

void Must(int code, const char* what) {
    if (code != NANOARROW_OK) {
        Refuse(std::string("internal: ") + what + " failed with code " + std::to_string(code));
    }
}

/// `true` when bit `i` of an LSB-first bitfield is set.
///
/// The bitfield pointers come from `PositionalReader`, which has already checked
/// that `ceil(count / 8)` bytes are there; this only reads what that admitted.
bool BitSet(const uint8_t* bitfield, int64_t i) {
    return ((bitfield[i / 8] >> (i % 8)) & 1u) != 0u;
}

/// One scalar, decoded but not yet appended.
///
/// It exists for exactly one reason. A map's keys ALL arrive before its values,
/// but nanoarrow finishes a struct element only when every child is exactly one
/// longer than the struct - so an entry's key and value have to be appended
/// together, and the keys must wait. `bytes` points INTO the row buffer, which
/// outlives the decode call, so this stages a pointer rather than a copy.
struct ScalarValue {
    enum class Kind : uint8_t { kInt, kUint, kDouble, kBytes };
    Kind kind = Kind::kInt;
    int64_t i = 0;
    uint64_t u = 0;
    double d = 0.0;
    const uint8_t* bytes = nullptr;
    size_t size = 0;
};

/// Read one scalar in wire order. Mirrors `EncodeValue`'s scalar arms exactly:
/// the width read here is the width written there, or the two have drifted and
/// the byte-identity test is comparing the wrong thing.
ScalarValue ReadScalar(ArrowType type, PositionalReader& r) {
    ScalarValue v;
    switch (type) {
        case NANOARROW_TYPE_BOOL:
            v.i = r.ReadBool() ? 1 : 0;
            return v;
        case NANOARROW_TYPE_INT8:
            v.i = r.ReadInt8();
            return v;
        case NANOARROW_TYPE_INT16:
            v.i = r.ReadInt16();
            return v;
        case NANOARROW_TYPE_INT32:
        case NANOARROW_TYPE_DATE32:
        case NANOARROW_TYPE_TIME32:
            v.i = r.ReadInt32();
            return v;
        case NANOARROW_TYPE_INT64:
        case NANOARROW_TYPE_DATE64:
        case NANOARROW_TYPE_TIME64:
        case NANOARROW_TYPE_TIMESTAMP:
        case NANOARROW_TYPE_DURATION:
            v.i = r.ReadInt64();
            return v;

        // Unsigned values get their own kind rather than widening into `int64_t`:
        // nanoarrow's `ArrowArrayAppendInt` range-checks an unsigned target
        // against [0, INT64_MAX], so UINT64_MAX would be refused by the builder
        // after surviving the wire intact.
        case NANOARROW_TYPE_UINT8:
            v.kind = ScalarValue::Kind::kUint;
            v.u = r.ReadUint8();
            return v;
        case NANOARROW_TYPE_UINT16:
            v.kind = ScalarValue::Kind::kUint;
            v.u = r.ReadUint16();
            return v;
        case NANOARROW_TYPE_UINT32:
            v.kind = ScalarValue::Kind::kUint;
            v.u = r.ReadUint32();
            return v;
        case NANOARROW_TYPE_UINT64:
            v.kind = ScalarValue::Kind::kUint;
            v.u = r.ReadUint64();
            return v;

        // A float widened to double and narrowed back is exact, so one `kDouble`
        // kind serves both widths without a rounding step.
        case NANOARROW_TYPE_FLOAT:
            v.kind = ScalarValue::Kind::kDouble;
            v.d = r.ReadFloat();
            return v;
        case NANOARROW_TYPE_DOUBLE:
            v.kind = ScalarValue::Kind::kDouble;
            v.d = r.ReadDouble();
            return v;

        case NANOARROW_TYPE_STRING:
        case NANOARROW_TYPE_LARGE_STRING:
        case NANOARROW_TYPE_BINARY:
        case NANOARROW_TYPE_LARGE_BINARY: {
            const std::pair<const uint8_t*, size_t> bytes = r.ReadBinary();
            v.kind = ScalarValue::Kind::kBytes;
            v.bytes = bytes.first;
            v.size = bytes.second;
            return v;
        }

        default:
            // Unreachable: `PlanNode` accepted exactly `IsWireScalar`'s list and
            // `DecodeValue` sends only those here. If this fires, they drifted.
            Refuse(
                "internal: a scalar of a type the schema plan accepted reached the decoder "
                "unhandled");
    }
}

/// Append a staged scalar to the array being built.
void AppendScalar(const ScalarValue& v, ArrowArray* out) {
    switch (v.kind) {
        case ScalarValue::Kind::kInt:
            Must(ArrowArrayAppendInt(out, v.i), "appending an integer");
            return;
        case ScalarValue::Kind::kUint:
            Must(ArrowArrayAppendUInt(out, v.u), "appending an unsigned integer");
            return;
        case ScalarValue::Kind::kDouble:
            Must(ArrowArrayAppendDouble(out, v.d), "appending a floating-point value");
            return;
        case ScalarValue::Kind::kBytes: {
            ArrowBufferView view;
            view.data.as_uint8 = v.bytes;
            view.size_bytes = static_cast<int64_t>(v.size);
            // One call for utf8, large_utf8, binary and large_binary alike: the
            // distinction is the array's, and the wire has only length + bytes.
            Must(ArrowArrayAppendBytes(out, view), "appending bytes");
            return;
        }
    }
}

void DecodeValue(const FieldPlan& plan, PositionalReader& r, ArrowArray* out);

/// The fields of one struct, from a reader already positioned past its bitfield.
///
/// A null field carries no payload, so the loop asks the bitfield BEFORE reading:
/// the fields are in schema order but the payloads are only the non-null ones,
/// and a decoder that read unconditionally would be reading the next field's
/// bytes into this one.
void DecodeStructBody(const FieldPlan& plan, PositionalReader& r, ArrowArray* out) {
    const auto n = static_cast<int64_t>(plan.children.size());
    for (int64_t i = 0; i < n; ++i) {
        if (r.IsNull(static_cast<int>(i))) {
            Must(ArrowArrayAppendNull(out->children[i], 1), "appending a null field");
        } else {
            DecodeValue(plan.children[i], r, out->children[i]);
        }
    }
    Must(ArrowArrayFinishElement(out), "finishing a struct element");
}

/// `count` elements whose null bitfield the caller has already consumed.
///
/// Shared by list, large list, fixed-size list and a map's values, exactly as
/// `EncodeElements` is shared on the way out - the wire gives all four the same
/// element framing, and what differs is only where the count and the bitfield
/// came from.
template <typename IsNull>
void DecodeElements(const FieldPlan& element, PositionalReader& r, int64_t count, IsNull is_null,
                    ArrowArray* child) {
    for (int64_t i = 0; i < count; ++i) {
        if (is_null(i)) {
            Must(ArrowArrayAppendNull(child, 1), "appending a null element");
        } else {
            DecodeValue(element, r, child);
        }
    }
}

/// One value, by type. The switch mirrors `EncodeValue`'s arms one for one.
void DecodeValue(const FieldPlan& plan, PositionalReader& r, ArrowArray* out) {
    if (IsWireScalar(plan.type)) {
        AppendScalar(ReadScalar(plan.type, r), out);
        return;
    }

    switch (plan.type) {
        case NANOARROW_TYPE_STRUCT: {
            // The sub-reader's destructor advances this one past the struct, so
            // it has to die before `r` is read again - hence the block.
            PositionalReader sub = r.ReadStruct(static_cast<int>(plan.children.size()));
            DecodeStructBody(plan, sub, out);
            return;
        }

        case NANOARROW_TYPE_LIST:
        case NANOARROW_TYPE_LARGE_LIST: {
            const PositionalReader::ListHeader header = r.ReadListHeader();
            DecodeElements(
                plan.children[0], r, header.count,
                [&header](int64_t i) { return header.IsElementNull(static_cast<uint32_t>(i)); },
                out->children[0]);
            Must(ArrowArrayFinishElement(out), "finishing a list");
            return;
        }

        case NANOARROW_TYPE_FIXED_SIZE_LIST: {
            // No COUNT on the wire: the size is in the schema. The element null
            // bitfield has the same shape as a struct's, so `ReadStruct` reads it
            // - and brings its bounds check along rather than a second copy of it.
            PositionalReader sub = r.ReadStruct(static_cast<int>(plan.fixed_size));
            DecodeElements(
                plan.children[0], sub, plan.fixed_size,
                [&sub](int64_t i) { return sub.IsNull(static_cast<int>(i)); }, out->children[0]);
            Must(ArrowArrayFinishElement(out), "finishing a fixed-size list");
            return;
        }

        case NANOARROW_TYPE_MAP: {
            const uint32_t count = r.ReadMapCount();
            ArrowArray* entries = out->children[0];
            ArrowArray* keys = entries->children[0];
            ArrowArray* values = entries->children[1];
            const FieldPlan& key_plan = plan.children[0].children[0];
            const FieldPlan& value_plan = plan.children[0].children[1];

            // Keys first, with no null bitfield: a null key is not representable.
            // They are staged rather than appended because an entry's key and
            // value have to reach nanoarrow together (see ScalarValue). `count`
            // is already bounded by `ReadMapCount` - it cannot exceed the bytes
            // remaining - so this reserve cannot be talked into a huge allocation
            // by a corrupt count.
            std::vector<ScalarValue> staged;
            staged.reserve(count);
            for (uint32_t i = 0; i < count; ++i) {
                staged.push_back(ReadScalar(key_plan.type, r));
            }

            const uint8_t* value_nulls = r.ReadMapValueBitfield(count);
            for (uint32_t i = 0; i < count; ++i) {
                AppendScalar(staged[i], keys);
                if (BitSet(value_nulls, i)) {
                    Must(ArrowArrayAppendNull(values, 1), "appending a null map value");
                } else {
                    DecodeValue(value_plan, r, values);
                }
                Must(ArrowArrayFinishElement(entries), "finishing a map entry");
            }
            Must(ArrowArrayFinishElement(out), "finishing a map");
            return;
        }

        default:
            // Unreachable: `PlanNode` refused every type this switch does not
            // handle, at open. If this fires, the two have drifted apart.
            Refuse(
                "internal: a value of a type the schema plan accepted reached the decoder's "
                "switch unhandled");
    }
}

/// Releases a half-built array if decoding throws.
///
/// Decode's contract is that `out` is untouched on failure, and the array being
/// built owns real buffers from the first append - so a malformed row at index
/// 4711 must not leak the 4710 rows in front of it.
class ArrayGuard {
   public:
    explicit ArrayGuard(ArrowArray* array) : array_(array) {}
    ~ArrayGuard() {
        if (array_ != nullptr) ArrowArrayRelease(array_);
    }
    void Dismiss() noexcept { array_ = nullptr; }

    ArrayGuard(const ArrayGuard&) = delete;
    ArrayGuard& operator=(const ArrayGuard&) = delete;
    ArrayGuard(ArrayGuard&&) = delete;
    ArrayGuard& operator=(ArrayGuard&&) = delete;

   private:
    ArrowArray* array_;
};

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
    root_.type = NANOARROW_TYPE_STRUCT;
    root_.children.reserve(static_cast<size_t>(schema_.get()->n_children));
    for (int64_t i = 0; i < schema_.get()->n_children; ++i) {
        root_.children.push_back(PlanNode(*schema_.get()->children[i], std::string()));
    }

    // Built AFTER planning, so a schema the mapping refuses is refused by the
    // plan's message rather than by a copy failing for a second reason.
    decoded_schema_ = OwnedSchema::DeepCopy(schema_.get());
    ResolveDictionaries(decoded_schema_.get());
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

void NanoarrowCodec::DecodeRows(const uint8_t* bytes, size_t len, int64_t count,
                                ArrowArray* out) const {
    if (out == nullptr) {
        throw std::invalid_argument("NanoarrowCodec::DecodeRows: out must not be null");
    }
    if (count < 0) {
        throw std::invalid_argument("NanoarrowCodec::DecodeRows: count must be >= 0, got " +
                                    std::to_string(count));
    }
    if (bytes == nullptr && len != 0) {
        throw std::invalid_argument(
            "NanoarrowCodec::DecodeRows: bytes is null but len says there are " +
            std::to_string(len) + " of them");
    }

    ArrowArray building = {};
    ArrowError error;
    // The DECODED schema, not the bound one: a dictionary field goes out as its
    // value type and comes back as a plain value array (D-BIND-39). The two are
    // the same object for every schema that carries no dictionary.
    if (ArrowArrayInitFromSchema(&building, decoded_schema_.get(), &error) != NANOARROW_OK) {
        Refuse(std::string("internal: an array could not be built for the codec's decoded "
                           "schema: ") +
               error.message);
    }
    // Armed before the first append: from here on, every exit but the last one
    // releases the array.
    ArrayGuard guard(&building);
    Must(ArrowArrayStartAppending(&building), "starting the array");

    const auto fields = static_cast<int>(root_.children.size());
    size_t offset = 0;
    for (int64_t row = 0; row < count; ++row) {
        // A row is self-delimiting, so the reader itself says where the next one
        // starts. A row running past the end throws out of the reader, with the
        // reader's own message.
        PositionalReader reader(bytes + offset, len - offset, fields);
        DecodeStructBody(root_, reader, &building);
        offset += reader.BytesConsumed();
    }

    // `VerifyFullyConsumed`, raised to the batch: `count` rows that do not
    // exactly fill `len` mean the caller's count and the caller's bytes disagree,
    // and silently ignoring the tail is how a framing bug survives to production.
    // It is spelled here rather than per row because a row's reader legitimately
    // has the FOLLOWING rows left over. The message is the reader's, verbatim, so
    // that a caller matching on it sees one taxonomy rather than two.
    if (offset != len) {
        throw std::invalid_argument("PositionalReader: buffer not fully consumed");
    }

    if (ArrowArrayFinishBuildingDefault(&building, &error) != NANOARROW_OK) {
        Refuse(std::string("internal: the decoded array failed validation: ") + error.message);
    }

    guard.Dismiss();
    ArrowArrayMove(&building, out);
}

}  // namespace fletcher::abi
