// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// BIND-3's restated acceptance (D-BIND-39): EVERY TYPE THE PROTO MAPPING
// PRODUCES ROUND-TRIPS THROUGH THE BINDING.
//
// ── Why this replaced "bucket 1 green in C#" ────────────────────────────────
// Bucket 1 is `arrow-bridge`'s suite, and `arrow-bridge` is the ARROW-NATIVE
// tier: it carries unions, decimals, intervals, half-float, the view types and
// fixed-size binary. Porting it here would have asked the binding to implement a
// tier it does not serve, and would have spent most of its cases on types no
// `.proto` can produce — `oneof` is in the wire spec's own §"Unsupported Types",
// and the rest have no proto construct at all.
//
// What a binding owes is the PROTO MAPPING, because that is what every generator
// emits: C++, C#, TypeScript and Rust all produce these types and no others. So
// this file is the same suite each binding runs, and it is what makes the
// cross-language parity claim testable rather than asserted.
//
// ── The case list mirrors docs/wire-format-specification.md ─────────────────
// §"Scalar Types", §"Well-Known Types" and §"Composite Types". The cases are
// named after the PROTO side so the list can be diffed against that table by eye,
// and `TheSuiteCoversTheWholeMapping` guards the other direction with a count.
//
// A count is normally the wrong shape for an acceptance — BIND-0's review F1 —
// but F1's concern was counts invalidated by a rebase adding a case to a test
// file. This one is stated against a FROZEN SPECIFICATION: it changes only when
// the mapping does, and when the mapping changes this test SHOULD fail and make
// somebody look. That is the opposite failure mode from the one F1 warned about.
using System;
using System.Collections.Generic;
using System.Linq;

using Apache.Arrow;
using Apache.Arrow.Types;

using Xunit;

namespace Eiva.Fletcher.Tests;

public sealed class ProtoMappingParityTests
{
    /// <summary>Distinct ARROW types the proto mapping can produce.</summary>
    /// <remarks>
    /// Derived from the spec, once, so the guard below is readable: boolean,
    /// int32, int64, uint32, uint64, float32, float64, utf8, binary (the nine
    /// scalars, with enum folding onto int32), timestamp and duration (the two
    /// temporal well-known types), and struct, list and map (the three
    /// composites). The eleven nullable wrapper types add no Arrow type — they
    /// make an existing one nullable, which is why every case below carries a
    /// null row.
    /// </remarks>
    /// <remarks>
    /// Internal rather than private only because .editorconfig's PascalCase
    /// carve-out is written for `static readonly` and does not match a `const`,
    /// so a private one must be `_camelCase` — which contradicts that carve-out's
    /// own stated intent. Second time this round; the config gap is real but
    /// fixing it is a tree-wide change, not this slice's.
    /// </remarks>
    internal const int DistinctArrowTypesInTheMapping = 14;

    /// <summary>Every case, named after the proto construct it stands for.</summary>
    /// <remarks>
    /// A plain array rather than the <c>TheoryData</c> directly, because the
    /// coverage guard below has to enumerate the same list and xunit's data type
    /// is not enumerable as one.
    /// </remarks>
    private static readonly string[] CaseNames =
    [
        // §"Scalar Types" — one per Arrow type, enum folded onto int32.
        "bool", "int32", "int64", "uint32", "uint64", "float", "double", "string", "bytes",
        "enum",
        // §"Well-Known Types" — the two that carry a temporal Arrow type. The
        // nine wrapper types map onto scalars already covered, as NULLABLE, and
        // every case here has a null row for that reason.
        "google.protobuf.Timestamp", "google.protobuf.Duration",
        // A timestamp whose timezone lives in the TYPE. If a crossing lost it,
        // every timestamp would silently become naive.
        "google.protobuf.Timestamp(tz)",
        // §"Composite Types".
        "nested message", "repeated scalar", "repeated message", "map<scalar,scalar>",
        "map<scalar,message>",
    ];

    public static TheoryData<string> MappedTypes => [.. CaseNames];

    /// <summary>Every mapped type survives encode and decode through the binding.</summary>
    [Theory]
    [MemberData(nameof(MappedTypes))]
    public void EveryMappedTypeRoundTripsThroughTheBinding(string protoType)
    {
        RecordBatch batch = Build(protoType);
        using var codec = new FletcherCodec(batch.Schema);

        byte[] encoded;
        using (BoundRows rows = codec.Bind(batch))
        {
            encoded = CodecFixtures.EncodeAll(codec, rows);
        }

        Assert.NotEmpty(encoded);

        using RecordBatch decoded = codec.DecodeBatch(encoded, batch.Length);

        Assert.Equal(batch.Length, decoded.Length);
        Assert.Equal(batch.ColumnCount, decoded.ColumnCount);
        AssertSameColumn(batch.Column(0), decoded.Column(0), protoType);
    }

    /// <summary>The cases cover the whole mapping table, not part of it.</summary>
    /// <remarks>
    /// The other direction of the acceptance. Without this, a type could be
    /// dropped from the list and every remaining case would still pass — the
    /// failure mode of any hand-written coverage list, and the one this round has
    /// removed twice already (the goldens, the conformance manifest).
    /// </remarks>
    [Fact]
    public void TheSuiteCoversTheWholeMapping()
    {
        HashSet<ArrowTypeId> covered = [];
        foreach (string protoType in CaseNames)
        {
            RecordBatch batch = Build(protoType);
            covered.Add(batch.Schema.FieldsList[0].DataType.TypeId);
        }

        Assert.Equal(DistinctArrowTypesInTheMapping, covered.Count);
    }

    /// <summary>A dictionary nested inside each composite resolves, both sides.</summary>
    /// <remarks>
    /// D-BIND-39 left this branch compiled and unexercised on BOTH sides: the
    /// shim's `ResolveDictionaries` and the managed `DecodedSchema.Resolve` each
    /// recurse into composites, and every fixture written for the ruling put its
    /// dictionary at the top level. "Never checked" and "checked, found nothing"
    /// are the same observable, which is the shape BIND-2 was caught by.
    ///
    /// The struct case is the end-to-end one — it encodes, decodes and compares
    /// values, so it exercises the C++ recursion too. The remaining composites are
    /// covered by <see cref="DecodedSchemaTests"/>, which walks the rewrite
    /// directly and needs no arrays to do it.
    /// </remarks>
    [Fact]
    public void ADictionaryInsideAStructRoundTripsAsItsValueType()
    {
        var dictionaryType = new DictionaryType(Int32Type.Default, StringType.Default, ordered: false);
        var inner = new StructType(
        [
            new Field("id", Int32Type.Default, nullable: true),
            new Field("category", dictionaryType, nullable: true),
        ]);
        var schema = new Schema([new Field("row", inner, nullable: true)], metadata: null);

        StringArray values = new StringArray.Builder().Append("alpha").Append("beta").Build();
        Int32Array indices = new Int32Array.Builder().Append(1).Append(0).Append(1).Build();
        var categories = new DictionaryArray(dictionaryType, indices, values);
        Int32Array ids = new Int32Array.Builder().Append(7).Append(8).AppendNull().Build();

        var validity = new ArrowBuffer.BitmapBuilder(3);
        validity.Append(true);
        validity.Append(true);
        validity.Append(false);

        var rows = new StructArray(inner, 3, [ids, categories], validity.Build(), nullCount: 1);
        var batch = new RecordBatch(schema, [rows], length: 3);

        using var codec = new FletcherCodec(schema);

        // The rewrite reached INSIDE the struct: the nested field is utf8 now, and
        // it kept its name. That is the managed half.
        var decodedInner = (StructType)codec.DecodedSchema.FieldsList[0].DataType;
        Assert.Equal("category", decodedInner.Fields[1].Name);
        Assert.Equal(ArrowTypeId.String, decodedInner.Fields[1].DataType.TypeId);

        byte[] encoded;
        using (BoundRows bound = codec.Bind(batch))
        {
            encoded = CodecFixtures.EncodeAll(codec, bound);
        }

        using RecordBatch decoded = codec.DecodeBatch(encoded, 3);

        // And the values came back resolved, which is the C++ half: the shim's own
        // recursion had to rewrite the same node for this to import at all.
        var decodedRows = (StructArray)decoded.Column(0);
        var decodedCategories = (StringArray)decodedRows.Fields[1];
        Assert.Equal("beta", decodedCategories.GetString(0));
        Assert.Equal("alpha", decodedCategories.GetString(1));
        Assert.True(decodedRows.IsNull(2));
    }

    // ── the fixtures, one per mapped construct ──────────────────────────────

    /// <summary>
    /// Three rows for every case: a value, the extreme where a wrong width or a
    /// wrong cast would show, and a NULL.
    /// </summary>
    /// <remarks>
    /// The null row is not padding. Every proto wrapper type maps to a nullable
    /// scalar, so nullability is part of the mapping rather than an edge case, and
    /// a null takes a different path through the encoder — a bit in a bitfield and
    /// no payload at all. A corpus of set rows would test half the encoder.
    /// </remarks>
    private static RecordBatch Build(string protoType)
    {
        (IArrowType type, IArrowArray column) = protoType switch
        {
            "bool" => (BooleanType.Default, (IArrowArray)new BooleanArray.Builder()
                .Append(true).Append(false).AppendNull().Build()),
            "int32" => (Int32Type.Default, new Int32Array.Builder()
                .Append(1).Append(int.MinValue).AppendNull().Build()),
            "enum" => (Int32Type.Default, new Int32Array.Builder()
                .Append(0).Append(int.MaxValue).AppendNull().Build()),
            "int64" => (Int64Type.Default, new Int64Array.Builder()
                .Append(1L).Append(long.MinValue).AppendNull().Build()),
            "uint32" => (UInt32Type.Default, new UInt32Array.Builder()
                .Append(1u).Append(uint.MaxValue).AppendNull().Build()),
            "uint64" => (UInt64Type.Default, new UInt64Array.Builder()
                .Append(1ul).Append(ulong.MaxValue).AppendNull().Build()),
            "float" => (FloatType.Default, new FloatArray.Builder()
                .Append(1.5f).Append(float.MaxValue).AppendNull().Build()),
            "double" => (DoubleType.Default, new DoubleArray.Builder()
                .Append(1.5).Append(double.MaxValue).AppendNull().Build()),
            // Non-ASCII deliberately: the boundary is UTF-16 in managed memory and
            // UTF-8 on the wire (D-BIND-1b), and an ASCII fixture would not notice
            // a length counted in the wrong unit.
            "string" => (StringType.Default, new StringArray.Builder()
                .Append("fletcher").Append("blåbærgrød").AppendNull().Build()),
            "bytes" => (BinaryType.Default, new BinaryArray.Builder()
                .Append([0x00, 0xFF, 0x7F]).Append([]).AppendNull().Build()),
            "google.protobuf.Timestamp" => BuildTimestamp(null),
            "google.protobuf.Timestamp(tz)" => BuildTimestamp("UTC"),
            "google.protobuf.Duration" => (DurationType.Nanosecond,
                new DurationArray.Builder(DurationType.Nanosecond)
                    .Append(1_000_000_000L).Append(-1L).AppendNull().Build()),
            "nested message" => BuildNestedMessage(),
            "repeated scalar" => BuildRepeatedScalar(),
            "repeated message" => BuildRepeatedMessage(),
            "map<scalar,scalar>" => BuildMap(Int32Type.Default),
            "map<scalar,message>" => BuildMap(
                new StructType([new Field("n", Int32Type.Default, nullable: true)])),
            _ => throw new ArgumentOutOfRangeException(
                nameof(protoType), protoType, "no fixture for this mapping row"),
        };

        var schema = new Schema([new Field("value", type, nullable: true)], metadata: null);
        return new RecordBatch(schema, [column], length: 3);
    }

    private static (IArrowType, IArrowArray) BuildTimestamp(string? timezone)
    {
        var type = new TimestampType(TimeUnit.Nanosecond, timezone);
        return (type, new TimestampArray.Builder(type)
            .Append(DateTimeOffset.UnixEpoch)
            .Append(new DateTimeOffset(2026, 9, 21, 12, 0, 0, TimeSpan.Zero))
            .AppendNull().Build());
    }

    private static (IArrowType, IArrowArray) BuildNestedMessage()
    {
        var type = new StructType(
        [
            new Field("id", Int32Type.Default, nullable: true),
            new Field("label", StringType.Default, nullable: true),
        ]);

        Int32Array ids = new Int32Array.Builder().Append(1).Append(2).AppendNull().Build();
        StringArray labels = new StringArray.Builder().Append("a").AppendNull().Append("c").Build();

        var validity = new ArrowBuffer.BitmapBuilder(3);
        validity.Append(true);
        validity.Append(true);
        validity.Append(false);

        return (type, new StructArray(type, 3, [ids, labels], validity.Build(), nullCount: 1));
    }

    private static (IArrowType, IArrowArray) BuildRepeatedScalar()
    {
        var builder = new ListArray.Builder(Int32Type.Default);
        var values = (Int32Array.Builder)builder.ValueBuilder;

        builder.Append();
        values.Append(10).Append(20);
        builder.Append();       // an EMPTY list, which is not a null list
        builder.AppendNull();

        ListArray array = builder.Build();
        return (array.Data.DataType, array);
    }

    private static (IArrowType, IArrowArray) BuildRepeatedMessage()
    {
        (IArrowType itemType, IArrowArray items) = BuildNestedMessage();

        var offsets = new ArrowBuffer.Builder<int>();
        offsets.Append(0).Append(2).Append(3).Append(3);

        var validity = new ArrowBuffer.BitmapBuilder(3);
        validity.Append(true);
        validity.Append(true);
        validity.Append(false);

        var type = new ListType(new Field("item", itemType, nullable: true));
        var data = new ArrayData(
            type, length: 3, nullCount: 1, offset: 0,
            buffers: [validity.Build(), offsets.Build()],
            children: [((StructArray)items).Data]);

        return (type, new ListArray(data));
    }

    /// <summary>A map with a scalar key — the only key the wire format carries.</summary>
    private static (IArrowType, IArrowArray) BuildMap(IArrowType valueType)
    {
        var type = new MapType(StringType.Default, valueType, keySorted: false);

        // Checked BEFORE the builder is constructed: MapArray.Builder throws
        // NotSupportedException for a struct value type rather than returning a
        // builder that cannot be used, so a struct-valued map is assembled from
        // its parts instead.
        if (valueType is StructType)
        {
            return BuildMapOfStruct(type);
        }

        var builder = new MapArray.Builder(type);
        var keys = (StringArray.Builder)builder.KeyBuilder;
        var values = (Int32Array.Builder)builder.ValueBuilder;
        builder.Append();
        keys.Append("alpha");
        values.Append(1);
        keys.Append("beta");
        values.Append(2);
        builder.Append();
        keys.Append("gamma");
        values.Append(3);
        builder.AppendNull();

        return (type, builder.Build());
    }

    private static (IArrowType, IArrowArray) BuildMapOfStruct(MapType type)
    {
        var valueType = (StructType)type.ValueField.DataType;

        StringArray keys = new StringArray.Builder().Append("alpha").Append("beta").Append("gamma").Build();
        Int32Array numbers = new Int32Array.Builder().Append(1).Append(2).Append(3).Build();
        var values = new StructArray(valueType, 3, [numbers], ArrowBuffer.Empty, nullCount: 0);

        var entries = new StructArray(
            (StructType)type.Fields[0].DataType, 3, [keys, values], ArrowBuffer.Empty, nullCount: 0);

        var offsets = new ArrowBuffer.Builder<int>();
        offsets.Append(0).Append(2).Append(3).Append(3);

        var validity = new ArrowBuffer.BitmapBuilder(3);
        validity.Append(true);
        validity.Append(true);
        validity.Append(false);

        var data = new ArrayData(
            type, length: 3, nullCount: 1, offset: 0,
            buffers: [validity.Build(), offsets.Build()],
            children: [entries.Data]);

        return (type, new MapArray(data));
    }

    // ── comparison ──────────────────────────────────────────────────────────

    /// <summary>Same type, same null positions, same values.</summary>
    /// <remarks>
    /// Row-aware rather than array-aware, and that is not a style choice. WHEN A
    /// STRUCT IS NULL, ITS CHILDREN ARE NOT ON THE WIRE: the row's bitfield says
    /// the struct is absent and no payload follows, so a non-null child underneath
    /// a null parent is unrecoverable by design. Comparing whole child arrays
    /// asserted that Fletcher transmits something the format deliberately drops —
    /// and the "nested message" fixture has exactly that shape (a label under a
    /// null struct), which is how the first version of this file failed.
    ///
    /// So every comparison descends only through values the encoder actually
    /// wrote, which is also what makes the null-position checks mean something at
    /// depth rather than comparing two arrays' incidental padding.
    /// </remarks>
    private static void AssertSameColumn(IArrowArray expected, IArrowArray actual, string protoType)
    {
        Assert.Equal(expected.Data.DataType.TypeId, actual.Data.DataType.TypeId);
        Assert.Equal(expected.Length, actual.Length);
        Assert.Equal(expected.NullCount, actual.NullCount);

        AssertRows(expected, actual, protoType);
    }

    /// <summary>Null positions, then the values at the rows that have them.</summary>
    private static void AssertRows(IArrowArray expected, IArrowArray actual, string protoType)
    {
        Assert.Equal(expected.Length, actual.Length);

        for (int row = 0; row < expected.Length; ++row)
        {
            // Null POSITIONS, not just a count: two columns can agree on how many
            // nulls they hold and disagree about which rows they are, and the wire
            // format's whole framing rests on the bitfield that says which.
            Assert.Equal(expected.IsNull(row), actual.IsNull(row));

            if (!expected.IsNull(row))
            {
                AssertValueAt(expected, actual, row, protoType);
            }
        }
    }

    /// <summary>One value, by type, recursing into composites.</summary>
    private static void AssertValueAt(IArrowArray expected, IArrowArray actual, int row, string protoType)
    {
        switch (expected)
        {
            case BooleanArray left:
                Assert.Equal(left.GetValue(row), ((BooleanArray)actual).GetValue(row));
                return;
            case Int32Array left:
                Assert.Equal(left.GetValue(row), ((Int32Array)actual).GetValue(row));
                return;
            case Int64Array left:
                Assert.Equal(left.GetValue(row), ((Int64Array)actual).GetValue(row));
                return;
            case UInt32Array left:
                Assert.Equal(left.GetValue(row), ((UInt32Array)actual).GetValue(row));
                return;
            case UInt64Array left:
                Assert.Equal(left.GetValue(row), ((UInt64Array)actual).GetValue(row));
                return;
            case FloatArray left:
                Assert.Equal(left.GetValue(row), ((FloatArray)actual).GetValue(row));
                return;
            case DoubleArray left:
                Assert.Equal(left.GetValue(row), ((DoubleArray)actual).GetValue(row));
                return;
            case TimestampArray left:
                Assert.Equal(left.GetTimestamp(row), ((TimestampArray)actual).GetTimestamp(row));
                return;
            case DurationArray left:
                Assert.Equal(left.GetValue(row), ((DurationArray)actual).GetValue(row));
                return;
            case StringArray left:
                Assert.Equal(left.GetString(row), ((StringArray)actual).GetString(row));
                return;
            case BinaryArray left:
                Assert.Equal(left.GetBytes(row).ToArray(), ((BinaryArray)actual).GetBytes(row).ToArray());
                return;

            case StructArray left:
                {
                    var right = (StructArray)actual;
                    Assert.Equal(left.Fields.Count, right.Fields.Count);
                    for (int field = 0; field < left.Fields.Count; ++field)
                    {
                        IArrowArray leftField = left.Fields[field];
                        IArrowArray rightField = right.Fields[field];

                        Assert.Equal(leftField.IsNull(row), rightField.IsNull(row));
                        if (!leftField.IsNull(row))
                        {
                            AssertValueAt(leftField, rightField, row, protoType);
                        }
                    }

                    return;
                }

            case MapArray left:
                AssertSlice(left.GetSlicedValues(row), ((MapArray)actual).GetSlicedValues(row), protoType);
                return;
            case ListArray left:
                AssertSlice(left.GetSlicedValues(row), ((ListArray)actual).GetSlicedValues(row), protoType);
                return;

            default:
                throw new InvalidOperationException(
                    $"'{protoType}' produced {expected.GetType().Name}, which this suite cannot " +
                    "compare — add an arm rather than letting the case pass unchecked");
        }
    }

    /// <summary>One list's or map's elements: same count, same nulls, same values.</summary>
    private static void AssertSlice(IArrowArray expected, IArrowArray actual, string protoType)
    {
        Assert.Equal(expected.Length, actual.Length);
        AssertRows(expected, actual, protoType);
    }
}
