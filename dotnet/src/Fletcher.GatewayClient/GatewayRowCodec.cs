// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The managed positional row codec: the one managed-codec exception (D-BIND-1).
//
// ── The one thing to keep in mind while reading ─────────────────────────────
// Every byte written here must equal what C++ writes for the same row. The layout
// belongs to docs/wire-format-specification.md; this file reproduces it, and the
// shape of this file is c-abi/src/nanoarrow_codec.cpp's on purpose: the same accept
// list (so a schema the native C# binding accepts is a schema the gateway client
// accepts, and the same refusals name the same fields), the same dictionary rule, the
// same map-key rule. CoverageWireBytesTests in Fletcher.GatewayClient.Tests holds it to
// the C++-written goldens byte for byte, both directions.
//
//   row          [NULL_BITFIELD : ceil(n/8), LSB-first, set = null] then non-null payloads
//   fixed scalar [PAYLOAD : N bytes, little-endian]
//   string/bytes [LEN : u32][PAYLOAD : LEN]
//   struct       [NULL_BITFIELD] then non-null payloads   (recursive)
//   list         [COUNT : u32][NULL_BITFIELD : ceil(count/8)] then non-null elements
//   fixed list   [NULL_BITFIELD : ceil(size/8)] then non-null elements  (no COUNT)
//   map          [COUNT : u32][keys...][VALUE_NULL_BITFIELD] then non-null values
//
// ── Batches in, batches out (D-BIND-108) ─────────────────────────────────────
// The codec reads a RecordBatch and writes one; the generated gateway pair reaches it
// through the model's ToArrow / FromArrow, exactly as the native pair reaches the shim's
// codec. It works on ArrayData - Arrow's buffers, offsets and children - rather than on
// the typed arrays, for two reasons: one walk serves every type, and it never touches
// StructArray.Fields, which allocates on every read in Apache.Arrow 23.
//
// Indexing follows the Arrow format: an index passed down is LOGICAL within the array
// it is passed with, and that array's own Offset is added when its buffers are read. A
// struct's child is indexed at (struct.Offset + i); a list's offsets name logical
// positions in its child. So a sliced batch encodes as its window, nested slices too.
using System;
using System.Buffers;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.IO;
using System.Linq;

using Apache.Arrow;
using Apache.Arrow.Types;

namespace Eiva.Fletcher.GatewayClient;

/// <summary>The Fletcher positional wire format over one Arrow schema, in managed code.</summary>
/// <remarks>
/// Immutable after construction: any number of threads may encode and decode through one
/// instance concurrently.
/// </remarks>
internal sealed class GatewayRowCodec
{
    private readonly Node[] _fields;

    /// <summary>Open a codec over <paramref name="schema"/>.</summary>
    /// <exception cref="ArgumentNullException"><paramref name="schema"/> is null.</exception>
    /// <exception cref="ArgumentException">
    /// The schema contains a type the wire format does not carry. The message names the field.
    /// </exception>
    public GatewayRowCodec(Schema schema)
    {
        ArgumentNullException.ThrowIfNull(schema);
        Schema = schema;
        _fields = schema.FieldsList.Select(f => Plan(f, string.Empty)).ToArray();
        DecodedSchema = _fields.Any(n => n.ContainsDictionary)
            ? new Schema(_fields.Select(n => n.DecodedField), schema.Metadata)
            : schema;
    }

    /// <summary>The schema rows are encoded against.</summary>
    public Schema Schema { get; }

    /// <summary>
    /// The schema of a decoded batch: <see cref="Schema"/>, with every dictionary field
    /// replaced by its value type, because the wire carries values and never indices.
    /// </summary>
    public Schema DecodedSchema { get; }

    // ───────────────────────────── Plan ─────────────────────────────

    private enum Kind
    {
        Bool,
        Fixed,
        Binary,
        LargeBinary,
        Struct,
        List,
        LargeList,
        FixedSizeList,
        Map,
    }

    // One schema node, validated, with what encode and decode need of it.
    private sealed class Node
    {
        public required Kind Kind { get; init; }

        public required Field Field { get; init; }

        public required string Path { get; init; }

        public int Width { get; init; }

        public int FixedSize { get; init; }

        public Node[] Children { get; init; } = System.Array.Empty<Node>();

        public bool ContainsDictionary { get; init; }

        // The field as a decoded batch carries it (dictionaries resolved below it).
        public required Field DecodedField { get; init; }

        public bool IsScalar => Kind is Kind.Bool or Kind.Fixed or Kind.Binary or Kind.LargeBinary;
    }

    private static string Describe(string path, string? name) =>
        string.IsNullOrEmpty(name) ? path : path.Length == 0 ? name : path + "." + name;

    private static ArgumentException Refuse(string what) => new("GatewayRowCodec: " + what, "schema");

    private static Node Plan(Field field, string path)
    {
        string here = Describe(path, field.Name);
        IArrowType type = field.DataType;

        if (type is DictionaryType dictionary)
        {
            // A dictionary is a scalar MODIFIER: the wire carries the value, one per row
            // (spec "Dictionary Types"), so its plan IS its value type's plan.
            var valueField = new Field(field.Name, dictionary.ValueType, field.IsNullable, field.Metadata);
            Node value = Plan(valueField, path);
            if (!value.IsScalar)
            {
                throw Refuse("field '" + here + "' is a dictionary whose value type is not a scalar. The wire " +
                    "format carries a dictionary as its value type, one value per row, so a nested value type " +
                    "(struct, list, map or union) has no single-row form (spec \"Dictionary Types\")");
            }

            return new Node
            {
                Kind = value.Kind,
                Field = field,
                Path = here,
                Width = value.Width,
                ContainsDictionary = true,
                DecodedField = valueField,
            };
        }

        int width = FixedWidth(type.TypeId);
        if (width > 0)
        {
            return new Node { Kind = Kind.Fixed, Field = field, Path = here, Width = width, DecodedField = field };
        }

        switch (type.TypeId)
        {
            case ArrowTypeId.Boolean:
                return new Node { Kind = Kind.Bool, Field = field, Path = here, DecodedField = field };
            case ArrowTypeId.String:
            case ArrowTypeId.Binary:
                return new Node { Kind = Kind.Binary, Field = field, Path = here, DecodedField = field };
            case ArrowTypeId.LargeString:
            case ArrowTypeId.LargeBinary:
                return new Node { Kind = Kind.LargeBinary, Field = field, Path = here, DecodedField = field };

            case ArrowTypeId.Struct:
                {
                    var st = (StructType)type;
                    Node[] children = st.Fields.Select(f => Plan(f, here)).ToArray();
                    bool dict = children.Any(c => c.ContainsDictionary);
                    Field decoded = dict
                        ? new Field(field.Name, new StructType(children.Select(c => c.DecodedField).ToList()), field.IsNullable, field.Metadata)
                        : field;
                    return new Node { Kind = Kind.Struct, Field = field, Path = here, Children = children, ContainsDictionary = dict, DecodedField = decoded };
                }

            case ArrowTypeId.List:
            case ArrowTypeId.LargeList:
                {
                    bool large = type.TypeId == ArrowTypeId.LargeList;
                    Field valueField = large ? ((LargeListType)type).ValueField : ((ListType)type).ValueField;
                    Node element = Plan(valueField, here);
                    Field decoded = field;
                    if (element.ContainsDictionary)
                    {
                        IArrowType decodedType = large ? new LargeListType(element.DecodedField) : new ListType(element.DecodedField);
                        decoded = new Field(field.Name, decodedType, field.IsNullable, field.Metadata);
                    }

                    return new Node
                    {
                        Kind = large ? Kind.LargeList : Kind.List,
                        Field = field,
                        Path = here,
                        Children = new[] { element },
                        ContainsDictionary = element.ContainsDictionary,
                        DecodedField = decoded,
                    };
                }

            case ArrowTypeId.FixedSizeList:
                {
                    var fsl = (FixedSizeListType)type;
                    Node element = Plan(fsl.ValueField, here);
                    Field decoded = element.ContainsDictionary
                        ? new Field(field.Name, new FixedSizeListType(element.DecodedField, fsl.ListSize), field.IsNullable, field.Metadata)
                        : field;
                    return new Node
                    {
                        Kind = Kind.FixedSizeList,
                        Field = field,
                        Path = here,
                        FixedSize = fsl.ListSize,
                        Children = new[] { element },
                        ContainsDictionary = element.ContainsDictionary,
                        DecodedField = decoded,
                    };
                }

            case ArrowTypeId.Map:
                {
                    var map = (MapType)type;
                    Node key = Plan(map.KeyField, here);
                    Node value = Plan(map.ValueField, here);

                    // A composite key is writable but not decodable in one pass: a map's
                    // keys all arrive before its values. Proto keys are integral, bool or
                    // string, so this is refused rather than half-supported (as the C ABI).
                    if (!key.IsScalar)
                    {
                        throw Refuse("field '" + here + "' is a map with a non-scalar key. The proto mapping produces " +
                            "integral, bool and string keys only, and the wire's keys-then-values layout makes a " +
                            "composite key undecodable in a single pass");
                    }

                    bool dict = key.ContainsDictionary || value.ContainsDictionary;
                    Field decoded = field;
                    if (dict)
                    {
                        Field entries = new(
                            map.Fields[0].Name,
                            new StructType(new List<Field> { key.DecodedField, value.DecodedField }),
                            map.Fields[0].IsNullable,
                            map.Fields[0].Metadata);
                        decoded = new Field(field.Name, new MapType(entries, map.KeySorted), field.IsNullable, field.Metadata);
                    }

                    return new Node
                    {
                        Kind = Kind.Map,
                        Field = field,
                        Path = here,
                        Children = new[] { key, value },
                        ContainsDictionary = dict,
                        DecodedField = decoded,
                    };
                }

            case ArrowTypeId.Union:
                throw Refuse("field '" + here + "' is a union. The proto mapping produces no unions, and the wire " +
                    "format's union framing exists for the Arrow-native tier only");

            default:
                throw Refuse("field '" + here + "' has a type the wire format does not carry (" + type.Name + ")");
        }
    }

    // The payload width of a fixed-width scalar, 0 for anything else. The accept list is
    // the C ABI's IsWireScalar: no half float, decimal, interval or fixed-size binary.
    private static int FixedWidth(ArrowTypeId id) => id switch
    {
        ArrowTypeId.Int8 or ArrowTypeId.UInt8 => 1,
        ArrowTypeId.Int16 or ArrowTypeId.UInt16 => 2,
        ArrowTypeId.Int32 or ArrowTypeId.UInt32 or ArrowTypeId.Float or ArrowTypeId.Date32 or ArrowTypeId.Time32 => 4,
        ArrowTypeId.Int64 or ArrowTypeId.UInt64 or ArrowTypeId.Double or ArrowTypeId.Date64 or ArrowTypeId.Time64
            or ArrowTypeId.Timestamp or ArrowTypeId.Duration => 8,
        _ => 0,
    };

    // ───────────────────────────── Encode ─────────────────────────────

    /// <summary>Check <paramref name="batch"/> against the schema once, for any number of <see cref="Encode(BoundBatch, int, IBufferWriter{byte})"/> calls.</summary>
    /// <exception cref="ArgumentException">The batch's columns do not match the schema.</exception>
    /// <remarks>The batch is BORROWED: it must stay alive, undisposed, while the bound form is in use.</remarks>
    public BoundBatch Bind(RecordBatch batch)
    {
        ArgumentNullException.ThrowIfNull(batch);
        if (batch.ColumnCount != _fields.Length)
        {
            throw new ArgumentException(
                "GatewayRowCodec: the batch has " + batch.ColumnCount + " columns and the schema " + _fields.Length,
                nameof(batch));
        }

        var columns = new ArrayData[_fields.Length];
        for (int i = 0; i < _fields.Length; ++i)
        {
            ArrayData data = batch.Column(i).Data;
            CheckShape(_fields[i], data.DataType);
            columns[i] = data;
        }

        RequireNoForbiddenNulls(columns, batch.Length);
        return new BoundBatch(columns, batch.Length);
    }

    // ───────────────────────────── Nullability (D-BIND-156) ─────────────────────────────
    //
    // The native codec's rule, in the same terms: a field the schema marks NOT nullable must
    // hold a value wherever its parent does, because its decoders (a C++ row class above all)
    // have nowhere to put a null. Refused at Bind, before any row is encoded. A null below a
    // null parent is no value and is fine; map keys keep their own refusal, at encode
    // (D-BIND-131); dictionaries are values the encoder resolves and checks itself. The rows
    // are walked only when some non-nullable field holds any null at all.
    private void RequireNoForbiddenNulls(ArrayData[] columns, int length)
    {
        for (int i = 0; i < _fields.Length; ++i)
        {
            if (!MayHoldForbiddenNull(_fields[i], columns[i]))
            {
                continue;
            }

            for (int row = 0; row < length; ++row)
            {
                RequireElement(_fields[i], columns[i], row, row);
            }
        }
    }

    private static bool MayHoldForbiddenNull(Node node, ArrayData data)
    {
        if (!node.Field.IsNullable && data.NullCount != 0)
        {
            return true;
        }

        if (data.DataType is DictionaryType)
        {
            return false;
        }

        return node.Kind switch
        {
            Kind.Struct => node.Children.Where((child, k) => MayHoldForbiddenNull(child, data.Children[k])).Any(),
            Kind.List or Kind.LargeList or Kind.FixedSizeList => MayHoldForbiddenNull(node.Children[0], data.Children[0]),
            Kind.Map => MayHoldForbiddenNull(node.Children[1], data.Children[0].Children[1]),
            _ => false,
        };
    }

    private static void RequireElement(Node node, ArrayData data, int index, int row)
    {
        if (IsNull(data, index))
        {
            if (!node.Field.IsNullable)
            {
                throw new ArgumentException("GatewayRowCodec: field '" + node.Path + "' is not nullable, but row " + row +
                    " has a null there; a decoder of this schema has nowhere to put it (D-BIND-156)", "batch");
            }

            return;
        }

        RequirePresentChildren(node, data, index, row);
    }

    // The fields below a PRESENT value, indexed exactly as EncodeValue indexes them.
    private static void RequirePresentChildren(Node node, ArrayData data, int index, int row)
    {
        if (data.DataType is DictionaryType)
        {
            return;
        }

        int at = data.Offset + index;
        switch (node.Kind)
        {
            case Kind.Struct:
                for (int k = 0; k < node.Children.Length; ++k)
                {
                    RequireElement(node.Children[k], data.Children[k], at, row);
                }

                return;

            case Kind.List:
                {
                    ReadOnlySpan<int> offsets = Int32s(data.Buffers[1]);
                    for (int e = offsets[at]; e < offsets[at + 1]; ++e)
                    {
                        RequireElement(node.Children[0], data.Children[0], e, row);
                    }

                    return;
                }

            case Kind.LargeList:
                {
                    ReadOnlySpan<long> offsets = Int64s(data.Buffers[1]);
                    for (long e = offsets[at]; e < offsets[at + 1]; ++e)
                    {
                        RequireElement(node.Children[0], data.Children[0], checked((int)e), row);
                    }

                    return;
                }

            case Kind.FixedSizeList:
                for (int e = at * node.FixedSize; e < (at + 1) * node.FixedSize; ++e)
                {
                    RequireElement(node.Children[0], data.Children[0], e, row);
                }

                return;

            case Kind.Map:
                {
                    ReadOnlySpan<int> offsets = Int32s(data.Buffers[1]);
                    ArrayData entries = data.Children[0];
                    int entryAt = entries.Offset + offsets[at];
                    for (int j = 0; j < offsets[at + 1] - offsets[at]; ++j)
                    {
                        // Keys: refused at encode, with their own message (D-BIND-131).
                        RequireElement(node.Children[1], entries.Children[1], entryAt + j, row);
                    }

                    return;
                }

            default:
                return;
        }
    }

    /// <summary>Encode row <paramref name="row"/> of a bound batch into <paramref name="output"/>.</summary>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="row"/> is outside the batch.</exception>
    /// <exception cref="ArgumentException">A value the wire cannot carry (a null map key, a dictionary index outside its dictionary).</exception>
    public void Encode(BoundBatch batch, int row, IBufferWriter<byte> output)
    {
        ArgumentNullException.ThrowIfNull(output);
        var writer = new WireWriter();
        EncodeRow(batch, row, writer);
        output.Write(writer.Written);
    }

    /// <summary>Encode row <paramref name="row"/> of a bound batch.</summary>
    public byte[] Encode(BoundBatch batch, int row)
    {
        var writer = new WireWriter();
        EncodeRow(batch, row, writer);
        return writer.Written.ToArray();
    }

    private void EncodeRow(BoundBatch batch, int row, WireWriter w)
    {
        if (batch.Columns is null || batch.Columns.Length != _fields.Length)
        {
            throw new ArgumentException("GatewayRowCodec: the batch was not bound by this codec", nameof(batch));
        }

        if ((uint)row >= (uint)batch.Length)
        {
            throw new ArgumentOutOfRangeException(nameof(row), row, "outside a batch of " + batch.Length + " rows");
        }

        int bitfield = w.Reserve(BitfieldBytes(_fields.Length));
        for (int i = 0; i < _fields.Length; ++i)
        {
            if (IsNull(batch.Columns[i], row))
            {
                w.SetBit(bitfield, i);
            }
        }

        for (int i = 0; i < _fields.Length; ++i)
        {
            if (!IsNull(batch.Columns[i], row))
            {
                EncodeValue(_fields[i], batch.Columns[i], row, w);
            }
        }
    }

    private static void CheckShape(Node node, IArrowType actual)
    {
        IArrowType expected = node.Field.DataType;

        // A dictionary field may be supplied as a dictionary of any index type, or as a
        // plain value; only the value type has to agree (as arrow-bridge's Codec).
        if (expected is DictionaryType ed)
        {
            IArrowType supplied = actual is DictionaryType ad ? ad.ValueType : actual;
            if (!SameType(ed.ValueType, supplied))
            {
                throw Mismatch(node, actual);
            }

            return;
        }

        if (actual.TypeId != expected.TypeId)
        {
            throw Mismatch(node, actual);
        }

        switch (node.Kind)
        {
            case Kind.Struct:
                {
                    var st = (StructType)actual;
                    if (st.Fields.Count != node.Children.Length)
                    {
                        throw Mismatch(node, actual);
                    }

                    for (int k = 0; k < node.Children.Length; ++k)
                    {
                        CheckShape(node.Children[k], st.Fields[k].DataType);
                    }

                    return;
                }

            case Kind.List:
                CheckShape(node.Children[0], ((ListType)actual).ValueDataType);
                return;
            case Kind.LargeList:
                CheckShape(node.Children[0], ((LargeListType)actual).ValueDataType);
                return;
            case Kind.FixedSizeList:
                if (((FixedSizeListType)actual).ListSize != node.FixedSize)
                {
                    throw Mismatch(node, actual);
                }

                CheckShape(node.Children[0], ((FixedSizeListType)actual).ValueDataType);
                return;
            case Kind.Map:
                CheckShape(node.Children[0], ((MapType)actual).KeyField.DataType);
                CheckShape(node.Children[1], ((MapType)actual).ValueField.DataType);
                return;
            default:
                if (!SameType(expected, actual))
                {
                    throw Mismatch(node, actual);
                }

                return;
        }
    }

    // Scalar type equality. IArrowType has no value equality (Equals is by reference),
    // so the parameters that change what a value MEANS are compared here.
    private static bool SameType(IArrowType a, IArrowType b)
    {
        if (a.TypeId != b.TypeId)
        {
            return false;
        }

        return (a, b) switch
        {
            (TimestampType x, TimestampType y) => x.Unit == y.Unit && x.Timezone == y.Timezone,
            (DurationType x, DurationType y) => x.Unit == y.Unit,
            (Time32Type x, Time32Type y) => x.Unit == y.Unit,
            (Time64Type x, Time64Type y) => x.Unit == y.Unit,
            _ => true,
        };
    }

    private static ArgumentException Mismatch(Node node, IArrowType actual) =>
        new("GatewayRowCodec: type mismatch for field '" + node.Path + "': the schema expects " +
            node.Field.DataType.Name + ", the batch has " + actual.Name, "batch");

    private static bool IsNull(ArrayData data, int index)
    {
        if (data.DataType.TypeId == ArrowTypeId.Null)
        {
            return true;
        }

        if (data.Buffers.Length == 0 || data.Buffers[0].IsEmpty)
        {
            return false;
        }

        int bit = data.Offset + index;
        return (data.Buffers[0].Span[bit >> 3] & (1 << (bit & 7))) == 0;
    }

    private static void EncodeValue(Node node, ArrayData data, int index, WireWriter w)
    {
        // A dictionary column stores indices; the wire carries the value they name.
        if (data.DataType is DictionaryType dictionaryType)
        {
            long code = ReadIndex(data, dictionaryType.IndexType, index);
            ArrayData values = data.Dictionary;
            if (code < 0 || code >= values.Length)
            {
                throw new ArgumentException("GatewayRowCodec: a dictionary index of " + code +
                    " is outside the dictionary's " + values.Length + " values (field '" + node.Path + "')", "batch");
            }

            if (IsNull(values, (int)code))
            {
                throw new ArgumentException("GatewayRowCodec: field '" + node.Path +
                    "' names a null dictionary entry; a valid row cannot carry it", "batch");
            }

            EncodeValue(node, values, (int)code, w);
            return;
        }

        int at = data.Offset + index;
        switch (node.Kind)
        {
            case Kind.Bool:
                w.WriteByte((byte)((data.Buffers[1].Span[at >> 3] >> (at & 7)) & 1));
                return;

            case Kind.Fixed:
                w.Write(data.Buffers[1].Span.Slice(at * node.Width, node.Width));
                return;

            case Kind.Binary:
                {
                    ReadOnlySpan<int> offsets = Int32s(data.Buffers[1]);
                    int start = offsets[at];
                    int length = offsets[at + 1] - start;
                    w.WriteU32((uint)length);
                    w.Write(data.Buffers[2].Span.Slice(start, length));
                    return;
                }

            case Kind.LargeBinary:
                {
                    ReadOnlySpan<long> offsets = Int64s(data.Buffers[1]);
                    long start = offsets[at];
                    long length = offsets[at + 1] - start;
                    if (length > int.MaxValue)
                    {
                        throw new ArgumentException("GatewayRowCodec: variable-length field '" + node.Path + "' exceeds what one buffer holds", "batch");
                    }

                    w.WriteU32((uint)length);
                    w.Write(data.Buffers[2].Span.Slice(checked((int)start), (int)length));
                    return;
                }

            case Kind.Struct:
                {
                    int n = node.Children.Length;
                    int bitfield = w.Reserve(BitfieldBytes(n));
                    for (int k = 0; k < n; ++k)
                    {
                        if (IsNull(data.Children[k], at))
                        {
                            w.SetBit(bitfield, k);
                        }
                    }

                    for (int k = 0; k < n; ++k)
                    {
                        if (!IsNull(data.Children[k], at))
                        {
                            EncodeValue(node.Children[k], data.Children[k], at, w);
                        }
                    }

                    return;
                }

            case Kind.List:
                {
                    ReadOnlySpan<int> offsets = Int32s(data.Buffers[1]);
                    int first = offsets[at];
                    EncodeElements(node.Children[0], data.Children[0], first, offsets[at + 1] - first, writeCount: true, w);
                    return;
                }

            case Kind.LargeList:
                {
                    ReadOnlySpan<long> offsets = Int64s(data.Buffers[1]);
                    long first = offsets[at];
                    EncodeElements(node.Children[0], data.Children[0], checked((int)first), checked((int)(offsets[at + 1] - first)), writeCount: true, w);
                    return;
                }

            case Kind.FixedSizeList:
                // No COUNT: the size is in the schema, so both sides know it.
                EncodeElements(node.Children[0], data.Children[0], at * node.FixedSize, node.FixedSize, writeCount: false, w);
                return;

            case Kind.Map:
                {
                    ReadOnlySpan<int> offsets = Int32s(data.Buffers[1]);
                    int first = offsets[at];
                    int count = offsets[at + 1] - first;
                    ArrayData entries = data.Children[0];
                    ArrayData keys = entries.Children[0];
                    ArrayData values = entries.Children[1];
                    int entryAt = entries.Offset + first;

                    w.WriteU32((uint)count);

                    // Keys first, with NO null bitfield: a null key is not representable.
                    for (int j = 0; j < count; ++j)
                    {
                        if (IsNull(keys, entryAt + j))
                        {
                            throw new ArgumentException("GatewayRowCodec: map key must not be null (field '" + node.Path + "')", "batch");
                        }

                        EncodeValue(node.Children[0], keys, entryAt + j, w);
                    }

                    EncodeElements(node.Children[1], values, entryAt, count, writeCount: false, w);
                    return;
                }

            default:
                throw new InvalidOperationException("GatewayRowCodec: internal: a planned kind reached encode unhandled");
        }
    }

    // The element framing shared by list, large list, fixed-size list and a map's values:
    // an optional COUNT, a null bitfield over `count`, then the non-null payloads. An
    // all-valid run of a fixed-width type goes out as one copy from the values buffer,
    // which is byte-identical to writing the values one by one.
    private static void EncodeElements(Node element, ArrayData child, int first, int count, bool writeCount, WireWriter w)
    {
        if (writeCount)
        {
            w.WriteU32((uint)count);
        }

        int bitfield = w.Reserve(BitfieldBytes(count));
        bool anyNull = false;
        for (int j = 0; j < count; ++j)
        {
            if (IsNull(child, first + j))
            {
                w.SetBit(bitfield, j);
                anyNull = true;
            }
        }

        if (!anyNull && element.Kind == Kind.Fixed && child.DataType.TypeId != ArrowTypeId.Dictionary)
        {
            w.Write(child.Buffers[1].Span.Slice((child.Offset + first) * element.Width, count * element.Width));
            return;
        }

        for (int j = 0; j < count; ++j)
        {
            if (!IsNull(child, first + j))
            {
                EncodeValue(element, child, first + j, w);
            }
        }
    }

    private static long ReadIndex(ArrayData data, IArrowType indexType, int index)
    {
        int at = data.Offset + index;
        ReadOnlySpan<byte> b = data.Buffers[1].Span;
        return indexType.TypeId switch
        {
            ArrowTypeId.Int8 => (sbyte)b[at],
            ArrowTypeId.UInt8 => b[at],
            ArrowTypeId.Int16 => BinaryPrimitives.ReadInt16LittleEndian(b[(at * 2)..]),
            ArrowTypeId.UInt16 => BinaryPrimitives.ReadUInt16LittleEndian(b[(at * 2)..]),
            ArrowTypeId.Int32 => BinaryPrimitives.ReadInt32LittleEndian(b[(at * 4)..]),
            ArrowTypeId.UInt32 => BinaryPrimitives.ReadUInt32LittleEndian(b[(at * 4)..]),
            ArrowTypeId.Int64 => BinaryPrimitives.ReadInt64LittleEndian(b[(at * 8)..]),
            ArrowTypeId.UInt64 => (long)Math.Min(BinaryPrimitives.ReadUInt64LittleEndian(b[(at * 8)..]), long.MaxValue),
            _ => throw new ArgumentException("GatewayRowCodec: a dictionary index type of " + indexType.Name + " is not an integer", "batch"),
        };
    }

    private static ReadOnlySpan<int> Int32s(ArrowBuffer buffer) => System.Runtime.InteropServices.MemoryMarshal.Cast<byte, int>(buffer.Span);

    private static ReadOnlySpan<long> Int64s(ArrowBuffer buffer) => System.Runtime.InteropServices.MemoryMarshal.Cast<byte, long>(buffer.Span);

    private static int BitfieldBytes(int count) => (count + 7) / 8;

    // ───────────────────────────── Decode ─────────────────────────────

    /// <summary>Decode one row into a one-row batch over <see cref="DecodedSchema"/>.</summary>
    /// <exception cref="InvalidDataException">The bytes do not match the schema.</exception>
    public RecordBatch Decode(ReadOnlySpan<byte> row)
    {
        Column[] columns = NewColumns();
        DecodeRow(row, columns);
        return Finish(columns, 1);
    }

    /// <summary>Decode several rows into one batch over <see cref="DecodedSchema"/>.</summary>
    /// <exception cref="InvalidDataException">A row does not match the schema; the message names which.</exception>
    public RecordBatch DecodeBatch(IReadOnlyList<ReadOnlyMemory<byte>> rows)
    {
        ArgumentNullException.ThrowIfNull(rows);
        Column[] columns = NewColumns();
        for (int r = 0; r < rows.Count; ++r)
        {
            try
            {
                DecodeRow(rows[r].Span, columns);
            }
            catch (InvalidDataException e)
            {
                throw new InvalidDataException("GatewayRowCodec: row " + r + ": " + e.Message, e);
            }
        }

        return Finish(columns, rows.Count);
    }

    private Column[] NewColumns() => _fields.Select(f => new Column(f)).ToArray();

    private RecordBatch Finish(Column[] columns, int length)
    {
        var arrays = new IArrowArray[columns.Length];
        for (int i = 0; i < columns.Length; ++i)
        {
            arrays[i] = ArrowArrayFactory.BuildArray(columns[i].Finish());
        }

        return new RecordBatch(DecodedSchema, arrays, length);
    }

    private void DecodeRow(ReadOnlySpan<byte> row, Column[] columns)
    {
        var r = new WireReader(row);
        ReadOnlySpan<byte> bitfield = r.Read(BitfieldBytes(_fields.Length));
        for (int i = 0; i < _fields.Length; ++i)
        {
            if (BitSet(bitfield, i))
            {
                columns[i].AppendNull();
            }
            else
            {
                columns[i].AppendValue(ref r);
            }
        }

        // A well-formed row is consumed exactly. Top level only: nested values share the
        // cursor and must not be checked on their own.
        if (r.Remaining != 0)
        {
            throw new InvalidDataException("buffer not fully consumed (" + r.Remaining +
                " trailing byte(s)); does not match schema");
        }
    }

    private static bool BitSet(ReadOnlySpan<byte> bitfield, int i) => ((bitfield[i >> 3] >> (i & 7)) & 1) != 0;

    // One column under construction: Arrow's buffers for one schema node, built directly.
    private sealed class Column
    {
        private readonly Node _node;
        private readonly Column[] _children;
        private readonly ArrowBuffer.BitmapBuilder _validity = new();
        private readonly ArrowBuffer.BitmapBuilder _bits = new();
        private readonly ArrowBuffer.Builder<byte> _bytes = new();
        private readonly ArrowBuffer.Builder<int> _offsets32 = new();
        private readonly ArrowBuffer.Builder<long> _offsets64 = new();
        private int _length;
        private int _nullCount;

        // A map's entries struct: its length is the entry count, every entry valid.
        private int _entries;

        public Column(Node node)
        {
            _node = node;
            _children = node.Children.Select(c => new Column(c)).ToArray();
            switch (node.Kind)
            {
                case Kind.Binary:
                case Kind.List:
                case Kind.Map:
                    _offsets32.Append(0);
                    break;
                case Kind.LargeBinary:
                case Kind.LargeList:
                    _offsets64.Append(0L);
                    break;
            }
        }

        public void AppendNull()
        {
            _validity.Append(false);
            ++_nullCount;
            ++_length;
            switch (_node.Kind)
            {
                case Kind.Bool:
                    _bits.Append(false);
                    break;
                case Kind.Fixed:
                    for (int i = 0; i < _node.Width; ++i)
                    {
                        _bytes.Append(0);
                    }

                    break;
                case Kind.Binary:
                    _offsets32.Append(_bytes.Length);
                    break;
                case Kind.LargeBinary:
                    _offsets64.Append(_bytes.Length);
                    break;
                case Kind.Struct:
                    foreach (Column child in _children)
                    {
                        child.AppendNull();
                    }

                    break;
                case Kind.List:
                    _offsets32.Append(_children[0]._length);
                    break;
                case Kind.LargeList:
                    _offsets64.Append(_children[0]._length);
                    break;
                case Kind.FixedSizeList:
                    for (int i = 0; i < _node.FixedSize; ++i)
                    {
                        _children[0].AppendNull();
                    }

                    break;
                case Kind.Map:
                    _offsets32.Append(_entries);
                    break;
            }
        }

        public void AppendValue(ref WireReader r)
        {
            _validity.Append(true);
            ++_length;
            switch (_node.Kind)
            {
                case Kind.Bool:
                    _bits.Append(r.Read(1)[0] != 0);
                    return;

                case Kind.Fixed:
                    _bytes.Append(r.Read(_node.Width));
                    return;

                case Kind.Binary:
                case Kind.LargeBinary:
                    {
                        uint length = r.ReadU32();
                        if (length > (uint)r.Remaining)
                        {
                            throw new InvalidDataException("variable-length payload of " + length + " bytes exceeds remaining buffer");
                        }

                        _bytes.Append(r.Read((int)length));
                        if (_node.Kind == Kind.Binary)
                        {
                            _offsets32.Append(_bytes.Length);
                        }
                        else
                        {
                            _offsets64.Append(_bytes.Length);
                        }

                        return;
                    }

                case Kind.Struct:
                    {
                        ReadOnlySpan<byte> bitfield = r.Read(BitfieldBytes(_children.Length));
                        for (int k = 0; k < _children.Length; ++k)
                        {
                            if (BitSet(bitfield, k))
                            {
                                _children[k].AppendNull();
                            }
                            else
                            {
                                _children[k].AppendValue(ref r);
                            }
                        }

                        return;
                    }

                case Kind.List:
                case Kind.LargeList:
                    {
                        uint count = r.ReadU32();

                        // A null element has no payload, so an all-null list may legitimately
                        // be longer than the bytes left; only its bitfield must fit.
                        if ((count + 7L) / 8 > r.Remaining)
                        {
                            throw new InvalidDataException("list element count " + count + " exceeds remaining buffer");
                        }

                        AppendElements(ref r, (int)count);
                        if (_node.Kind == Kind.List)
                        {
                            _offsets32.Append(_children[0]._length);
                        }
                        else
                        {
                            _offsets64.Append(_children[0]._length);
                        }

                        return;
                    }

                case Kind.FixedSizeList:
                    AppendElements(ref r, _node.FixedSize);
                    return;

                case Kind.Map:
                    {
                        uint count = r.ReadU32();

                        // Keys are never null and never empty on the wire, so every entry needs
                        // at least one byte: a tight lower bound, unlike a list's.
                        if (count > (uint)r.Remaining)
                        {
                            throw new InvalidDataException("map entry count " + count + " exceeds remaining buffer");
                        }

                        for (uint i = 0; i < count; ++i)
                        {
                            _children[0].AppendValue(ref r);
                        }

                        ReadOnlySpan<byte> bitfield = r.Read(BitfieldBytes((int)count));
                        for (int i = 0; i < (int)count; ++i)
                        {
                            if (BitSet(bitfield, i))
                            {
                                _children[1].AppendNull();
                            }
                            else
                            {
                                _children[1].AppendValue(ref r);
                            }
                        }

                        _entries += (int)count;
                        _offsets32.Append(_entries);
                        return;
                    }

                default:
                    throw new InvalidOperationException("GatewayRowCodec: internal: a planned kind reached decode unhandled");
            }
        }

        private void AppendElements(ref WireReader r, int count)
        {
            ReadOnlySpan<byte> bitfield = r.Read(BitfieldBytes(count));
            Column element = _children[0];
            for (int i = 0; i < count; ++i)
            {
                if (BitSet(bitfield, i))
                {
                    element.AppendNull();
                }
                else
                {
                    element.AppendValue(ref r);
                }
            }
        }

        public ArrayData Finish()
        {
            ArrowBuffer validity = _nullCount == 0 ? ArrowBuffer.Empty : _validity.Build();
            IArrowType type = _node.DecodedField.DataType;
            switch (_node.Kind)
            {
                case Kind.Bool:
                    return new ArrayData(type, _length, _nullCount, 0, new[] { validity, _bits.Build() });
                case Kind.Fixed:
                    return new ArrayData(type, _length, _nullCount, 0, new[] { validity, _bytes.Build() });
                case Kind.Binary:
                    return new ArrayData(type, _length, _nullCount, 0, new[] { validity, _offsets32.Build(), _bytes.Build() });
                case Kind.LargeBinary:
                    return new ArrayData(type, _length, _nullCount, 0, new[] { validity, _offsets64.Build(), _bytes.Build() });
                case Kind.Struct:
                    return new ArrayData(type, _length, _nullCount, 0, new[] { validity }, _children.Select(c => c.Finish()).ToArray());
                case Kind.List:
                    return new ArrayData(type, _length, _nullCount, 0, new[] { validity, _offsets32.Build() }, new[] { _children[0].Finish() });
                case Kind.LargeList:
                    return new ArrayData(type, _length, _nullCount, 0, new[] { validity, _offsets64.Build() }, new[] { _children[0].Finish() });
                case Kind.FixedSizeList:
                    return new ArrayData(type, _length, _nullCount, 0, new[] { validity }, new[] { _children[0].Finish() });
                case Kind.Map:
                    {
                        var map = (MapType)type;
                        var entries = new ArrayData(
                            map.KeyValueType,
                            _entries,
                            0,
                            0,
                            new[] { ArrowBuffer.Empty },
                            new[] { _children[0].Finish(), _children[1].Finish() });
                        return new ArrayData(type, _length, _nullCount, 0, new[] { validity, _offsets32.Build() }, new[] { entries });
                    }

                default:
                    throw new InvalidOperationException("GatewayRowCodec: internal: a planned kind reached finish unhandled");
            }
        }
    }

    // ───────────────────────────── Buffers ─────────────────────────────

    // A growable output buffer that can set a bit in a bitfield it has already written:
    // the null bitfield precedes the payloads it describes, so it is reserved first and
    // filled as the values are visited.
    private sealed class WireWriter
    {
        private byte[] _buffer = new byte[256];
        private int _position;

        public ReadOnlySpan<byte> Written => _buffer.AsSpan(0, _position);

        public int Reserve(int count)
        {
            Ensure(count);
            int start = _position;
            _buffer.AsSpan(start, count).Clear();
            _position += count;
            return start;
        }

        public void SetBit(int bitfieldStart, int index) => _buffer[bitfieldStart + (index >> 3)] |= (byte)(1 << (index & 7));

        public void WriteByte(byte value)
        {
            Ensure(1);
            _buffer[_position++] = value;
        }

        public void WriteU32(uint value)
        {
            Ensure(4);
            BinaryPrimitives.WriteUInt32LittleEndian(_buffer.AsSpan(_position), value);
            _position += 4;
        }

        public void Write(ReadOnlySpan<byte> bytes)
        {
            Ensure(bytes.Length);
            bytes.CopyTo(_buffer.AsSpan(_position));
            _position += bytes.Length;
        }

        private void Ensure(int count)
        {
            if (_buffer.Length - _position >= count)
            {
                return;
            }

            long wanted = Math.Max((long)_buffer.Length * 2, (long)_position + count);
            System.Array.Resize(ref _buffer, (int)Math.Min(wanted, System.Array.MaxLength));
        }
    }

    // A forward cursor whose every read is bounds-checked: the decoder's refusals of a
    // short buffer all come from here, as the C++ decoder's all come from its reader.
    private ref struct WireReader
    {
        private readonly ReadOnlySpan<byte> _data;
        private int _position;

        public WireReader(ReadOnlySpan<byte> data)
        {
            _data = data;
            _position = 0;
        }

        public readonly int Remaining => _data.Length - _position;

        public ReadOnlySpan<byte> Read(int count)
        {
            if (count > Remaining)
            {
                throw new InvalidDataException("unexpected end of buffer (" + count + " bytes wanted, " + Remaining + " left)");
            }

            ReadOnlySpan<byte> bytes = _data.Slice(_position, count);
            _position += count;
            return bytes;
        }

        public uint ReadU32() => BinaryPrimitives.ReadUInt32LittleEndian(Read(4));
    }
}

/// <summary>A batch checked against a <see cref="GatewayRowCodec"/>'s schema, ready to encode row by row.</summary>
internal readonly struct BoundBatch
{
    internal BoundBatch(ArrayData[] columns, int length)
    {
        Columns = columns;
        Length = length;
    }

    internal ArrayData[] Columns { get; }

    /// <summary>The number of rows.</summary>
    public int Length { get; }
}
