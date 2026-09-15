// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// Risk N-9, answered with a test instead of an assumption.
//
// The whole value-transfer design (D-BIND-1a / D-BIND-23) rests on one claim:
// that `Apache.Arrow` can EXPORT a C# array across the Arrow C Data Interface
// and IMPORT what the shim hands back, for every type the wire-format mapping
// produces — nested ones included. The plan says to verify it at BIND-0, before
// generated code depends on it, and to pin the version that was verified.
//
// So this file walks the mapping in `docs/wire-format-specification.md`
// (§"Scalar Types", §"Composite Types", §"Well-Known Types") and round-trips one
// array per Arrow type through `Apache.Arrow.C`. A gap found here is cheap: it is
// a different pinned version, or a type the generator must avoid. A gap found at
// BIND-7 is a rewrite.
//
// What is deliberately NOT here: any Fletcher code. Nothing above the ABI exists
// yet at BIND-0, and this test does not need it — the subject is the dependency.
using System;
using System.Collections.Generic;

using Apache.Arrow;
using Apache.Arrow.C;
using Apache.Arrow.Types;

using Xunit;

namespace Eiva.Fletcher.Tests;

public sealed class ArrowCDataInterfaceTests
{
    /// <summary>Every Arrow type the proto mapping can produce, by the name used in the wire-format spec.</summary>
    /// <remarks>
    /// Cases are named rather than passed as data because xunit serialises theory
    /// data, and an <see cref="IArrowArray"/> is not serialisable: the name is the
    /// key, and the array is built inside the test.
    /// </remarks>
    private static readonly string[] CaseNames =
    [
        // Scalar types — the proto scalars and enum.
        "boolean", "int32", "int64", "uint32", "uint64", "float32", "float64", "utf8", "binary",
        // Well-known types, flattened.
        "timestamp_ns", "timestamp_ns_utc", "duration_ns",
        // Composite types — nested message, repeated, repeated message, map.
        "struct", "list_int32", "list_struct", "map_utf8_int32",
    ];

    public static TheoryData<string> MappingTypes => [.. CaseNames];

    [Theory]
    [MemberData(nameof(MappingTypes))]
    public void ArrayRoundTripsThroughCDataInterface(string typeName)
    {
        IArrowArray original = BuildArray(typeName);

        IArrowArray imported = RoundTrip(original);

        AssertSameType(original.Data.DataType, imported.Data.DataType);
        Assert.Equal(original.Length, imported.Length);
        Assert.Equal(original.NullCount, imported.NullCount);
        AssertSameValues(original, imported);
    }

    /// <summary>
    /// A whole batch at once, which is the shape `fl_rows_bind` actually receives:
    /// one exported <c>ArrowArray</c> whose children are the columns.
    /// </summary>
    [Fact]
    public void RecordBatchRoundTripsThroughCDataInterface()
    {
        List<Field> fields = [];
        List<IArrowArray> columns = [];
        foreach (string name in CaseNames)
        {
            IArrowArray column = BuildArray(name);
            fields.Add(new Field(name, column.Data.DataType, nullable: true));
            columns.Add(column);
        }

        var batch = new RecordBatch(new Schema(fields, metadata: null), columns, columns[0].Length);

        RecordBatch imported = RoundTrip(batch);

        Assert.Equal(batch.Length, imported.Length);
        Assert.Equal(batch.ColumnCount, imported.ColumnCount);
        for (int i = 0; i < batch.ColumnCount; i++)
        {
            Assert.Equal(batch.Schema.FieldsList[i].Name, imported.Schema.FieldsList[i].Name);
            AssertSameType(batch.Schema.FieldsList[i].DataType, imported.Schema.FieldsList[i].DataType);
            AssertSameValues(batch.Column(i), imported.Column(i));
        }
    }

    /// <summary>
    /// Field-level metadata survives the crossing.
    /// </summary>
    /// <remarks>
    /// Not incidental: the generator puts the proto field number into field
    /// metadata (so a rename is traceable across schema evolution), and the schema
    /// C# hands the shim is the schema the transport publishes. If metadata were
    /// dropped by the C Data Interface, a C# publisher and a C++ publisher would
    /// put different schemas on the wire for the same proto.
    /// </remarks>
    [Fact]
    public void SchemaMetadataSurvivesTheCrossing()
    {
        var fieldMetadata = new Dictionary<string, string> { ["fletcher.field_number"] = "7" };
        var schemaMetadata = new Dictionary<string, string> { ["fletcher.message"] = "Example" };
        var schema = new Schema(
            [new Field("scored", Int32Type.Default, nullable: true, fieldMetadata)],
            schemaMetadata);

        Schema imported = RoundTripSchema(schema);

        Assert.Equal("scored", imported.FieldsList[0].Name);
        Assert.True(imported.FieldsList[0].HasMetadata);
        Assert.Equal("7", imported.FieldsList[0].Metadata["fletcher.field_number"]);
        Assert.Equal("Example", imported.Metadata["fletcher.message"]);
    }

    // ── the crossing itself ─────────────────────────────────────────────────

    private static unsafe IArrowArray RoundTrip(IArrowArray array)
    {
        CArrowSchema* cSchema = CArrowSchema.Create();
        CArrowArray* cArray = CArrowArray.Create();
        try
        {
            CArrowSchemaExporter.ExportType(array.Data.DataType, cSchema);
            CArrowArrayExporter.ExportArray(array, cArray);

            IArrowType importedType = CArrowSchemaImporter.ImportType(cSchema);
            return CArrowArrayImporter.ImportArray(cArray, importedType);
        }
        finally
        {
            CArrowSchema.Free(cSchema);
            CArrowArray.Free(cArray);
        }
    }

    private static unsafe RecordBatch RoundTrip(RecordBatch batch)
    {
        CArrowSchema* cSchema = CArrowSchema.Create();
        CArrowArray* cArray = CArrowArray.Create();
        try
        {
            CArrowSchemaExporter.ExportSchema(batch.Schema, cSchema);
            CArrowArrayExporter.ExportRecordBatch(batch, cArray);

            Schema importedSchema = CArrowSchemaImporter.ImportSchema(cSchema);
            return CArrowArrayImporter.ImportRecordBatch(cArray, importedSchema);
        }
        finally
        {
            CArrowSchema.Free(cSchema);
            CArrowArray.Free(cArray);
        }
    }

    private static unsafe Schema RoundTripSchema(Schema schema)
    {
        CArrowSchema* cSchema = CArrowSchema.Create();
        try
        {
            CArrowSchemaExporter.ExportSchema(schema, cSchema);
            return CArrowSchemaImporter.ImportSchema(cSchema);
        }
        finally
        {
            CArrowSchema.Free(cSchema);
        }
    }

    // ── one array per mapped type ───────────────────────────────────────────

    private static IArrowArray BuildArray(string typeName) => typeName switch
    {
        "boolean" => new BooleanArray.Builder().Append(true).Append(false).AppendNull().Build(),
        "int32" => new Int32Array.Builder().Append(1).Append(-2).AppendNull().Build(),
        "int64" => new Int64Array.Builder().Append(1L).Append(long.MinValue).AppendNull().Build(),
        "uint32" => new UInt32Array.Builder().Append(1u).Append(uint.MaxValue).AppendNull().Build(),
        "uint64" => new UInt64Array.Builder().Append(1ul).Append(ulong.MaxValue).AppendNull().Build(),
        "float32" => new FloatArray.Builder().Append(1.5f).Append(float.MaxValue).AppendNull().Build(),
        "float64" => new DoubleArray.Builder().Append(1.5).Append(double.MaxValue).AppendNull().Build(),
        // Non-ASCII on purpose: the transcoding boundary is UTF-16 in managed
        // memory to UTF-8 on the wire, and a pure-ASCII fixture would not notice
        // if it went wrong.
        "utf8" => new StringArray.Builder().Append("fletcher").Append("blåbærgrød").AppendNull().Build(),
        "binary" => new BinaryArray.Builder().Append([0x00, 0xFF, 0x7F]).Append([]).AppendNull().Build(),
        "timestamp_ns" => new TimestampArray.Builder(new TimestampType(TimeUnit.Nanosecond, (string?)null))
            .Append(DateTimeOffset.UnixEpoch).Append(new DateTimeOffset(2026, 9, 14, 12, 0, 0, TimeSpan.Zero))
            .AppendNull().Build(),
        // The timezone lives in the TYPE, not in the values: if the C Data
        // Interface lost it, every timestamp would silently become naive.
        "timestamp_ns_utc" => new TimestampArray.Builder(new TimestampType(TimeUnit.Nanosecond, "UTC"))
            .Append(DateTimeOffset.UnixEpoch).Append(new DateTimeOffset(2026, 9, 14, 12, 0, 0, TimeSpan.Zero))
            .AppendNull().Build(),
        "duration_ns" => new DurationArray.Builder(DurationType.Nanosecond)
            .Append(1_000_000_000L).Append(-1L).AppendNull().Build(),
        "struct" => BuildStructArray(),
        "list_int32" => BuildListOfInt32(),
        "list_struct" => BuildListOfStruct(),
        "map_utf8_int32" => BuildMapArray(),
        _ => throw new ArgumentOutOfRangeException(nameof(typeName), typeName, "no fixture for this type"),
    };

    private static StructType ExampleStructType { get; } = new StructType(
    [
        new Field("id", Int32Type.Default, nullable: true),
        new Field("label", StringType.Default, nullable: true),
    ]);

    private static StructArray BuildStructArray()
    {
        Int32Array ids = new Int32Array.Builder().Append(1).Append(2).AppendNull().Build();
        StringArray labels = new StringArray.Builder().Append("a").AppendNull().Append("c").Build();

        // Third slot null at the struct level too, so the validity bitmap has
        // something to carry: a struct whose bitmap is all-ones would not notice
        // a bitmap that failed to cross.
        var validity = new ArrowBuffer.BitmapBuilder(3);
        validity.Append(true);
        validity.Append(true);
        validity.Append(false);

        return new StructArray(ExampleStructType, length: 3, [ids, labels], validity.Build(), nullCount: 1);
    }

    private static ListArray BuildListOfInt32()
    {
        var builder = new ListArray.Builder(Int32Type.Default);
        var values = (Int32Array.Builder)builder.ValueBuilder;

        builder.Append();
        values.Append(10).Append(20);
        builder.Append();
        values.Append(30);
        builder.AppendNull();

        return builder.Build();
    }

    private static ListArray BuildListOfStruct()
    {
        // ListArray.Builder's value builder is typed by the value type, and there
        // is no struct builder in this version of the library, so the list is
        // assembled from its parts: offsets, a validity bitmap, and the struct
        // array built above as the values.
        StructArray values = BuildStructArray();

        var offsets = new ArrowBuffer.Builder<int>();
        offsets.Append(0).Append(2).Append(3).Append(3);

        var validity = new ArrowBuffer.BitmapBuilder(3);
        validity.Append(true);
        validity.Append(true);
        validity.Append(false);

        var type = new ListType(new Field("item", ExampleStructType, nullable: true));
        var data = new ArrayData(
            type,
            length: 3,
            nullCount: 1,
            offset: 0,
            buffers: [validity.Build(), offsets.Build()],
            children: [values.Data]);

        return new ListArray(data);
    }

    private static MapArray BuildMapArray()
    {
        var type = new MapType(StringType.Default, Int32Type.Default, keySorted: false);
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

        return builder.Build();
    }

    // ── comparisons ─────────────────────────────────────────────────────────

    /// <summary>
    /// Structural type equality, recursive, and specific about the parts that a
    /// crossing can quietly lose: a timestamp's timezone, a map's key sortedness,
    /// a list's or struct's children.
    /// </summary>
    private static void AssertSameType(IArrowType expected, IArrowType actual)
    {
        Assert.Equal(expected.TypeId, actual.TypeId);

        switch (expected)
        {
            case TimestampType timestamp:
                var actualTimestamp = Assert.IsType<TimestampType>(actual);
                Assert.Equal(timestamp.Unit, actualTimestamp.Unit);
                Assert.Equal(timestamp.Timezone, actualTimestamp.Timezone);
                break;

            case DurationType duration:
                Assert.Equal(duration.Unit, Assert.IsType<DurationType>(actual).Unit);
                break;

            case StructType structType:
                var actualStruct = Assert.IsType<StructType>(actual);
                Assert.Equal(structType.Fields.Count, actualStruct.Fields.Count);
                for (int i = 0; i < structType.Fields.Count; i++)
                {
                    Assert.Equal(structType.Fields[i].Name, actualStruct.Fields[i].Name);
                    AssertSameType(structType.Fields[i].DataType, actualStruct.Fields[i].DataType);
                }

                break;

            case MapType mapType:
                var actualMap = Assert.IsType<MapType>(actual);
                Assert.Equal(mapType.KeySorted, actualMap.KeySorted);
                AssertSameType(mapType.KeyField.DataType, actualMap.KeyField.DataType);
                AssertSameType(mapType.ValueField.DataType, actualMap.ValueField.DataType);
                break;

            case ListType listType:
                AssertSameType(listType.ValueDataType, Assert.IsType<ListType>(actual).ValueDataType);
                break;

            default:
                Assert.Equal(expected.Name, actual.Name);
                break;
        }
    }

    /// <summary>
    /// Values, compared through the buffers rather than through a typed reader, so
    /// one comparison covers every case without a per-type visitor.
    /// </summary>
    /// <remarks>
    /// Buffer-level comparison is the strict reading of "the same array arrived":
    /// it catches an offset that did not cross, a validity bitmap that was
    /// reallocated as all-valid, and a child array that came back empty. It is
    /// only valid because both sides are freshly built with offset 0.
    /// </remarks>
    private static void AssertSameValues(IArrowArray expected, IArrowArray actual)
    {
        ArrayData expectedData = expected.Data;
        ArrayData actualData = actual.Data;

        Assert.Equal(expectedData.Length, actualData.Length);
        Assert.Equal(expectedData.NullCount, actualData.NullCount);
        Assert.Equal(expectedData.Offset, actualData.Offset);
        Assert.Equal(expectedData.Buffers.Length, actualData.Buffers.Length);

        // ArrayData.Children is null, not empty, for an array with no children.
        int expectedChildren = expectedData.Children?.Length ?? 0;
        int actualChildren = actualData.Children?.Length ?? 0;
        Assert.Equal(expectedChildren, actualChildren);

        for (int i = 0; i < expectedData.Buffers.Length; i++)
        {
            ReadOnlySpan<byte> expectedBytes = expectedData.Buffers[i].Span;
            ReadOnlySpan<byte> actualBytes = actualData.Buffers[i].Span;

            // The validity buffer of an all-valid array is allowed to be absent on
            // either side — that is a representation choice the C Data Interface
            // does not preserve, and both spellings mean "no nulls".
            if (i == 0 && expectedData.NullCount == 0 && (expectedBytes.IsEmpty || actualBytes.IsEmpty))
            {
                continue;
            }

            // Trailing padding is not part of the value: compare the bytes the
            // shorter side carries, having already agreed on length and nulls.
            int common = Math.Min(expectedBytes.Length, actualBytes.Length);
            Assert.True(
                expectedBytes[..common].SequenceEqual(actualBytes[..common]),
                $"buffer {i} differs after the crossing");
        }

        for (int i = 0; i < expectedChildren; i++)
        {
            AssertSameValues(
                ArrowArrayFactory.BuildArray(expectedData.Children![i]),
                ArrowArrayFactory.BuildArray(actualData.Children![i]));
        }
    }
}
