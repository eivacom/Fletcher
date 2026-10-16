// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// BIND-8a: the managed row codec, case by case.
//
// Byte parity with C++ is CoverageWireBytesTests' job, for every shape the proto mapping
// produces. This file holds the rest:
//
// * gateway-client-ts's codec tests, ported (Bucket 2): test/wire-encode.test.ts's eight
//   round trips and test/object-backend.test.ts's three decodes. TypeScript encodes plain
//   objects through a descriptor; C# encodes Arrow batches (D-BIND-108), so each case
//   builds the batch the TS object stands for and asserts the same values come back,
//   plus the bytes wherever the TS case states them.
// * the Arrow layouts the proto mapping never produces but the codec accepts, because the
//   native C# codec accepts them (the accept list is the C ABI's): fixed-size lists, the
//   large string/binary/list forms, dictionaries, the date and time types, sliced arrays.
//   No C++ golden holds these; each is asserted against a form whose bytes ARE pinned
//   (a large string writes what a string writes, a dictionary what its values write, a
//   slice what the unsliced row writes), or against bytes computed from the spec.
// * the refusals: a type outside the accept list, a batch of another shape, a value the
//   wire cannot carry, and malformed bytes.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;

using Apache.Arrow;
using Apache.Arrow.Types;

using Xunit;

namespace Eiva.Fletcher.GatewayClient.Tests;

public sealed class RowCodecTests
{
    // ── gateway-client-ts test/wire-encode.test.ts ──

    [Fact]
    public void RoundTripsScalarFields()
    {
        RecordBatch batch = Batch(
            ("a", new Int32Array.Builder().Append(-7).Build()),
            ("b", new DoubleArray.Builder().Append(1.23).Build()),
            ("c", new BooleanArray.Builder().Append(true).Build()),
            ("d", new UInt32Array.Builder().Append(999u).Build()));

        RecordBatch decoded = RoundTrip(batch);

        Assert.Equal(-7, ((Int32Array)decoded.Column(0)).GetValue(0));
        Assert.Equal(1.23, ((DoubleArray)decoded.Column(1)).GetValue(0));
        Assert.True(((BooleanArray)decoded.Column(2)).GetValue(0));
        Assert.Equal(999u, ((UInt32Array)decoded.Column(3)).GetValue(0));
    }

    [Fact]
    public void RoundTrips64BitIntegers()
    {
        var ts = new TimestampType(TimeUnit.Nanosecond, (string?)null);
        RecordBatch batch = Batch(
            ("ts", new TimestampArray.Builder(ts).Append(DateTimeOffset.FromUnixTimeMilliseconds(1_234_567_890_123)).Build()),
            ("id", new UInt64Array.Builder().Append(ulong.MaxValue).Build()));

        RecordBatch decoded = RoundTrip(batch);

        Assert.Equal(1_234_567_890_123_000_000L, ((TimestampArray)decoded.Column(0)).GetValue(0));
        Assert.Equal(ulong.MaxValue, ((UInt64Array)decoded.Column(1)).GetValue(0));
    }

    [Fact]
    public void RoundTripsStrings()
    {
        RecordBatch batch = Batch(("name", new StringArray.Builder().Append("hello, world!").Build()));

        RecordBatch decoded = RoundTrip(batch);

        Assert.Equal("hello, world!", ((StringArray)decoded.Column(0)).GetString(0));
    }

    [Fact]
    public void AStringsLengthOnTheWireIsItsUtf8ByteCount()
    {
        // "Æøå" is 3 characters and 6 UTF-8 bytes; the wire counts bytes, never characters.
        RecordBatch batch = Batch(("name", new StringArray.Builder().Append("Æøå").Build()));

        byte[] bytes = Encode(batch);

        Assert.Equal("00" + "06000000" + Convert.ToHexString(Encoding.UTF8.GetBytes("Æøå")), Convert.ToHexString(bytes));
        Assert.Equal("Æøå", ((StringArray)RoundTrip(batch).Column(0)).GetString(0));
    }

    [Fact]
    public void RoundTripsNullFields()
    {
        RecordBatch batch = Batch(
            ("x", new Int32Array.Builder().AppendNull().Build()),
            ("y", new StringArray.Builder().Append("present").Build()));

        // Bit 0 set (x is null) and no payload for x: the bitfield, then y alone.
        Assert.Equal("01" + "07000000" + Convert.ToHexString(Encoding.UTF8.GetBytes("present")), Convert.ToHexString(Encode(batch)));

        RecordBatch decoded = RoundTrip(batch);
        Assert.True(decoded.Column(0).IsNull(0));
        Assert.Equal("present", ((StringArray)decoded.Column(1)).GetString(0));
    }

    [Fact]
    public void RoundTripsBinaryData()
    {
        byte[] blob = { 0xff, 0x00, 0xab, 0xcd };
        RecordBatch batch = Batch(("data", new BinaryArray.Builder().Append(blob.AsSpan()).Build()));

        RecordBatch decoded = RoundTrip(batch);

        Assert.Equal(blob, ((BinaryArray)decoded.Column(0)).GetBytes(0).ToArray());
    }

    [Fact]
    public void RoundTripsAListOfInt32()
    {
        RecordBatch batch = Batch(("nums", Int32List(new int?[] { 10, 20, 30 })));

        // Bitfield, COUNT = 3, the element bitfield (none null), three values.
        Assert.Equal("00" + "03000000" + "00" + "0A000000" + "14000000" + "1E000000", Convert.ToHexString(Encode(batch)));

        RecordBatch decoded = RoundTrip(batch);
        Assert.Equal(new int?[] { 10, 20, 30 }, Ints(decoded.Column(0), 0));
    }

    [Fact]
    public void RoundTripsAListWithANullElement()
    {
        RecordBatch batch = Batch(("nums", Int32List(new int?[] { 10, null, 30 })));

        // Element bit 1 set, and the null element writes no payload.
        Assert.Equal("00" + "03000000" + "02" + "0A000000" + "1E000000", Convert.ToHexString(Encode(batch)));
        Assert.Equal(new int?[] { 10, null, 30 }, Ints(RoundTrip(batch).Column(0), 0));
    }

    [Fact]
    public void RoundTripsAMapOfStringToInt32()
    {
        RecordBatch batch = Batch(("tags", StringIntMap(("a", 1), ("b", 2))));

        // COUNT = 2, both keys with no bitfield, then the value bitfield and the values.
        Assert.Equal(
            "00" + "02000000" + "01000000" + "61" + "01000000" + "62" + "00" + "01000000" + "02000000",
            Convert.ToHexString(Encode(batch)));

        var map = (MapArray)RoundTrip(batch).Column(0);
        var keys = (StringArray)map.Keys;
        var values = (Int32Array)map.Values;
        Assert.Equal(new[] { "a", "b" }, new[] { keys.GetString(0), keys.GetString(1) });
        Assert.Equal(new int?[] { 1, 2 }, new[] { values.GetValue(0), values.GetValue(1) });
    }

    [Fact]
    public void AMapValueMayBeNullAndAKeyMayNot()
    {
        RecordBatch batch = Batch(("tags", StringIntMap(("a", 1), ("b", null))));

        Assert.Equal(
            "00" + "02000000" + "01000000" + "61" + "01000000" + "62" + "02" + "01000000",
            Convert.ToHexString(Encode(batch)));
        var map = (MapArray)RoundTrip(batch).Column(0);
        Assert.True(map.Values.IsNull(1));
    }

    [Fact]
    public void ProducesCompactEncodingWithoutFieldHeaders()
    {
        RecordBatch batch = Batch(
            ("a", new Int32Array.Builder().Append(1).Build()),
            ("b", new Int32Array.Builder().Append(2).Build()));

        // 1 byte of null bitfield + 4 bytes + 4 bytes: positional, no tags.
        Assert.Equal("00" + "01000000" + "02000000", Convert.ToHexString(Encode(batch)));
    }

    // ── gateway-client-ts test/object-backend.test.ts ──

    [Fact]
    public void DecodesACompleteRow()
    {
        RecordBatch batch = Batch(
            ("id", new Int32Array.Builder().Append(42).Build()),
            ("label", new StringArray.Builder().Append("sensor").Build()),
            ("value", new DoubleArray.Builder().Append(3.5).Build()));

        RecordBatch decoded = new GatewayRowCodec(batch.Schema).Decode(Encode(batch));

        Assert.Equal(1, decoded.Length);
        Assert.Equal(42, ((Int32Array)decoded.Column(0)).GetValue(0));
        Assert.Equal("sensor", ((StringArray)decoded.Column(1)).GetString(0));
        Assert.Equal(3.5, ((DoubleArray)decoded.Column(2)).GetValue(0));
    }

    [Fact]
    public void ANullFieldDecodesAsNull()
    {
        // TS "returns null for null fields": a row whose bitfield marks a field null.
        Schema schema = new Schema.Builder()
            .Field(f => f.Name("id").DataType(Int32Type.Default).Nullable(true))
            .Field(f => f.Name("label").DataType(StringType.Default).Nullable(true))
            .Build();

        RecordBatch decoded = new GatewayRowCodec(schema).Decode(Convert.FromHexString("02" + "2A000000"));

        Assert.Equal(42, ((Int32Array)decoded.Column(0)).GetValue(0));
        Assert.True(decoded.Column(1).IsNull(0));
    }

    [Fact]
    public void EveryFieldNullDecodesAsAllNull()
    {
        // TS "returns null for explicitly null fields": every bit set, no payload at all.
        Schema schema = new Schema.Builder()
            .Field(f => f.Name("id").DataType(Int32Type.Default).Nullable(true))
            .Field(f => f.Name("label").DataType(StringType.Default).Nullable(true))
            .Build();

        RecordBatch decoded = new GatewayRowCodec(schema).Decode(Convert.FromHexString("03"));

        Assert.True(decoded.Column(0).IsNull(0));
        Assert.True(decoded.Column(1).IsNull(0));
    }

    // ── Layouts beyond the proto mapping, accepted as the native codec accepts them ──

    [Fact]
    public void AFixedSizeListWritesAListsBytesWithoutTheCount()
    {
        IArrowArray values = new Int32Array.Builder().Append(1).AppendNull().Append(3).Build();
        var fsl = (FixedSizeListArray)ArrowArrayFactory.BuildArray(new ArrayData(
            new FixedSizeListType(new Field("item", Int32Type.Default, true), 3), 1, 0, 0,
            new[] { ArrowBuffer.Empty }, new[] { values.Data }));

        byte[] asFixed = Encode(Batch(("v", fsl)));
        byte[] asList = Encode(Batch(("v", Int32List(new int?[] { 1, null, 3 }))));

        Assert.Equal(Convert.ToHexString(asList).Remove(2, 8), Convert.ToHexString(asFixed));
        Assert.Equal(new int?[] { 1, null, 3 }, Ints(RoundTrip(Batch(("v", fsl))).Column(0), 0));
    }

    [Fact]
    public void LargeStringsBinariesAndListsWriteWhatTheirSmallFormsWrite()
    {
        IArrowArray largeString = Large(LargeStringType.Default, "hi", "there");
        IArrowArray largeBinary = Large(LargeBinaryType.Default, "\u0001\u0002", string.Empty);
        IArrowArray ints = new Int32Array.Builder().Append(5).Append(6).Build();
        IArrowArray largeList = ArrowArrayFactory.BuildArray(new ArrayData(
            new LargeListType(new Field("item", Int32Type.Default, true)), 2, 0, 0,
            new[] { ArrowBuffer.Empty, Longs(0, 2, 2) }, new[] { ints.Data }));

        RecordBatch large = Batch(("s", largeString), ("b", largeBinary), ("l", largeList));
        RecordBatch small = Batch(
            ("s", new StringArray.Builder().Append("hi").Append("there").Build()),
            ("b", new BinaryArray.Builder().Append(new byte[] { 1, 2 }.AsSpan()).Append(System.Array.Empty<byte>().AsSpan()).Build()),
            ("l", Int32List(new int?[] { 5, 6 }, System.Array.Empty<int?>())));

        var largeCodec = new GatewayRowCodec(large.Schema);
        var smallCodec = new GatewayRowCodec(small.Schema);
        for (int row = 0; row < 2; ++row)
        {
            Assert.Equal(
                Convert.ToHexString(smallCodec.Encode(smallCodec.Bind(small), row)),
                Convert.ToHexString(largeCodec.Encode(largeCodec.Bind(large), row)));
        }

        RecordBatch decoded = largeCodec.Decode(largeCodec.Encode(largeCodec.Bind(large), 0));
        Assert.Equal("hi", ((LargeStringArray)decoded.Column(0)).GetString(0));
        Assert.Equal(new int?[] { 5, 6 }, Ints(decoded.Column(2), 0));
    }

    [Fact]
    public void ADictionaryColumnWritesItsValuesAndDecodesAsThem()
    {
        var dictionaryType = new DictionaryType(Int8Type.Default, StringType.Default, false);
        IArrowArray indices = new Int8Array.Builder().Append(1).Append(0).Append(1).Build();
        IArrowArray dictionary = new StringArray.Builder().Append("north").Append("south").Build();
        var column = new DictionaryArray(dictionaryType, indices, dictionary);
        RecordBatch dict = Batch(("heading", column));
        RecordBatch plain = Batch(("heading", new StringArray.Builder().Append("south").Append("north").Append("south").Build()));

        var codec = new GatewayRowCodec(dict.Schema);
        var plainCodec = new GatewayRowCodec(plain.Schema);
        BoundBatch bound = codec.Bind(dict);
        for (int row = 0; row < 3; ++row)
        {
            Assert.Equal(
                Convert.ToHexString(plainCodec.Encode(plainCodec.Bind(plain), row)),
                Convert.ToHexString(codec.Encode(bound, row)));
        }

        // The wire carries values, so a decoded batch carries the value type.
        Assert.Equal(ArrowTypeId.String, codec.DecodedSchema.FieldsList[0].DataType.TypeId);
        RecordBatch decoded = codec.Decode(codec.Encode(bound, 0));
        Assert.Equal("south", ((StringArray)decoded.Column(0)).GetString(0));

        // A dictionary field also accepts its plain value type (as arrow-bridge's Codec).
        Assert.Equal(Convert.ToHexString(codec.Encode(bound, 1)), Convert.ToHexString(codec.Encode(codec.Bind(plain), 1)));
    }

    [Fact]
    public void ADictionaryIndexOutsideItsDictionaryIsRefused()
    {
        var dictionaryType = new DictionaryType(Int8Type.Default, StringType.Default, false);
        IArrowArray indices = new Int8Array.Builder().Append(2).Build();
        IArrowArray dictionary = new StringArray.Builder().Append("only").Build();
        RecordBatch batch = Batch(("heading", new DictionaryArray(dictionaryType, indices, dictionary)));
        var codec = new GatewayRowCodec(batch.Schema);

        ArgumentException e = Assert.Throws<ArgumentException>(() => codec.Encode(codec.Bind(batch), 0));
        Assert.Contains("outside the dictionary", e.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void DateTimeAndDurationTypesAreTheirIntegersAndKeepTheirUnits()
    {
        var time32 = new Time32Type(TimeUnit.Millisecond);
        var time64 = new Time64Type(TimeUnit.Microsecond);
        var timestamp = new TimestampType(TimeUnit.Microsecond, "UTC");
        RecordBatch batch = Batch(
            ("d32", Fixed(Date32Type.Default, 4, 19_000)),
            ("d64", Fixed(Date64Type.Default, 8, 1_641_600_000_000L)),
            ("t32", Fixed(time32, 4, 3_600_000)),
            ("t64", Fixed(time64, 8, 3_600_000_000L)),
            ("ts", Fixed(timestamp, 8, -1L)),
            ("dur", Fixed(DurationType.Millisecond, 8, 250L)));

        string expected = "00" + Le(19_000, 4) + Le(1_641_600_000_000L, 8) + Le(3_600_000, 4) +
            Le(3_600_000_000L, 8) + Le(-1L, 8) + Le(250L, 8);
        Assert.Equal(expected, Convert.ToHexString(Encode(batch)));

        RecordBatch decoded = RoundTrip(batch);
        Assert.Equal(TimeUnit.Millisecond, ((Time32Type)decoded.Schema.FieldsList[2].DataType).Unit);
        Assert.Equal("UTC", ((TimestampType)decoded.Schema.FieldsList[4].DataType).Timezone);
        Assert.Equal(-1L, ((TimestampArray)decoded.Column(4)).GetValue(0));
    }

    [Fact]
    public void ASlicedBatchEncodesItsWindowRowForRow()
    {
        // Every column kind whose reads depend on an offset: fixed, bool, string, struct
        // (children indexed through the parent's offset), list (offsets into the child).
        RecordBatch whole = Batch(
            ("i", new Int32Array.Builder().Append(1).AppendNull().Append(3).Append(4).Build()),
            ("b", new BooleanArray.Builder().Append(true).Append(false).Append(true).AppendNull().Build()),
            ("s", new StringArray.Builder().Append("a").Append("bb").AppendNull().Append("dddd").Build()),
            ("p", Point((1, 2), null, (5, 6), (7, 8))),
            ("l", Int32List(new int?[] { 1 }, new int?[] { 2, 3 }, null, new int?[] { 4, null, 6 })));
        var codec = new GatewayRowCodec(whole.Schema);
        BoundBatch wholeBound = codec.Bind(whole);

        RecordBatch window = whole.Slice(1, 3);
        BoundBatch windowBound = codec.Bind(window);
        Assert.All(window.Arrays, a => Assert.Equal(1, a.Data.Offset));

        for (int row = 0; row < 3; ++row)
        {
            Assert.Equal(
                Convert.ToHexString(codec.Encode(wholeBound, row + 1)),
                Convert.ToHexString(codec.Encode(windowBound, row)));
        }
    }

    [Fact]
    public void ASlicedStructReadsItsChildrensNullBitsThroughItsOwnOffset()
    {
        // A struct child is indexed at (struct.Offset + i). With a null in the child at a
        // position the window shifts, reading it at i alone would pick the wrong bit.
        IArrowArray xs = new Int32Array.Builder().Append(1).AppendNull().Append(3).Build();
        IArrowArray ys = new Int32Array.Builder().AppendNull().Append(2).Append(3).Build();
        var type = new StructType(new List<Field> { new("x", Int32Type.Default, true), new("y", Int32Type.Default, true) });
        RecordBatch whole = Batch(("p", new StructArray(type, 3, new[] { xs, ys }, ArrowBuffer.Empty, 0)));
        var codec = new GatewayRowCodec(whole.Schema);

        BoundBatch window = codec.Bind(whole.Slice(1, 2));

        // Row 1 of the whole: x null, y = 2.
        Assert.Equal("00" + "01" + "02000000", Convert.ToHexString(codec.Encode(window, 0)));
        Assert.Equal(Convert.ToHexString(codec.Encode(codec.Bind(whole), 2)), Convert.ToHexString(codec.Encode(window, 1)));
    }

    [Fact]
    public void AListOverAChildWithItsOwnOffsetReadsFromThatOffset()
    {
        // A list's offsets name LOGICAL positions in its child, and the child's own Offset
        // is added when its buffer is read. The all-valid fixed-width run is copied in one
        // piece, so that copy must start at the child's offset too.
        IArrowArray values = new Int32Array.Builder().Append(99).Append(10).Append(20).Append(30).Build().Slice(1, 3);
        Assert.Equal(1, values.Data.Offset);
        var offsets = new ArrowBuffer.Builder<int>().Append(0).Append(3).Build();
        IArrowArray list = ArrowArrayFactory.BuildArray(new ArrayData(
            new ListType(new Field("item", Int32Type.Default, true)), 1, 0, 0,
            new[] { ArrowBuffer.Empty, offsets }, new[] { values.Data }));

        Assert.Equal(
            Convert.ToHexString(Encode(Batch(("l", Int32List(new int?[] { 10, 20, 30 }))))),
            Convert.ToHexString(Encode(Batch(("l", list)))));
    }

    [Fact]
    public void ANullStructWritesOnlyItsBitAndDecodesAsNull()
    {
        RecordBatch batch = Batch(("p", Point(new (int X, int Y)?[] { null })));

        Assert.Equal("01", Convert.ToHexString(Encode(batch)));
        Assert.True(RoundTrip(batch).Column(0).IsNull(0));
    }

    // ── Refusals ──

    public static TheoryData<string, IArrowType> UncarriedTypes => new()
    {
        { "a union", new UnionType(new[] { new Field("x", Int32Type.Default, true) }, new[] { 0 }, UnionMode.Sparse) },
        { "a type the wire format does not carry", new Decimal128Type(10, 2) },
        { "a type the wire format does not carry", HalfFloatType.Default },
        { "a type the wire format does not carry", new FixedSizeBinaryType(4) },
        { "a type the wire format does not carry", StringViewType.Default },
        { "a type the wire format does not carry", new IntervalType(IntervalUnit.DayTime) },
        { "a type the wire format does not carry", NullType.Default },
    };

    [Theory]
    [MemberData(nameof(UncarriedTypes))]
    public void ASchemaWithATypeOutsideTheAcceptListIsRefusedNamingTheField(string reason, IArrowType type)
    {
        var inner = new StructType(new List<Field> { new("bad", type, true) });
        Schema schema = new Schema.Builder().Field(f => f.Name("outer").DataType(inner).Nullable(true)).Build();

        ArgumentException e = Assert.Throws<ArgumentException>(() => new GatewayRowCodec(schema));

        Assert.Contains("'outer.bad'", e.Message, StringComparison.Ordinal);
        Assert.Contains(reason, e.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void AMapWithACompositeKeyIsRefused()
    {
        var key = new StructType(new List<Field> { new("x", Int32Type.Default, false) });
        Schema schema = new Schema.Builder()
            .Field(f => f.Name("m").DataType(new MapType(new Field("key", key, false), new Field("value", Int32Type.Default, true), false)))
            .Build();

        ArgumentException e = Assert.Throws<ArgumentException>(() => new GatewayRowCodec(schema));
        Assert.Contains("non-scalar key", e.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void ADictionaryOfACompositeIsRefused()
    {
        var value = new ListType(Int32Type.Default);
        Schema schema = new Schema.Builder()
            .Field(f => f.Name("d").DataType(new DictionaryType(Int32Type.Default, value, false)))
            .Build();

        ArgumentException e = Assert.Throws<ArgumentException>(() => new GatewayRowCodec(schema));
        Assert.Contains("value type is not a scalar", e.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void ABatchOfAnotherShapeIsRefusedAtBind()
    {
        RecordBatch batch = Batch(("a", new Int32Array.Builder().Append(1).Build()));
        Schema twoColumns = new Schema.Builder()
            .Field(f => f.Name("a").DataType(Int32Type.Default))
            .Field(f => f.Name("b").DataType(Int32Type.Default))
            .Build();
        Schema otherType = new Schema.Builder().Field(f => f.Name("a").DataType(Int64Type.Default)).Build();

        Assert.Contains("columns", Assert.Throws<ArgumentException>(() => new GatewayRowCodec(twoColumns).Bind(batch)).Message, StringComparison.Ordinal);
        Assert.Contains("type mismatch for field 'a'", Assert.Throws<ArgumentException>(() => new GatewayRowCodec(otherType).Bind(batch)).Message, StringComparison.Ordinal);
    }

    [Fact]
    public void ATimestampOfAnotherUnitIsAMismatchNotAReinterpretation()
    {
        RecordBatch batch = Batch(("ts", Fixed(new TimestampType(TimeUnit.Microsecond, (string?)null), 8, 1L)));
        Schema nanos = new Schema.Builder()
            .Field(f => f.Name("ts").DataType(new TimestampType(TimeUnit.Nanosecond, (string?)null)))
            .Build();

        Assert.Throws<ArgumentException>(() => new GatewayRowCodec(nanos).Bind(batch));
    }

    [Fact]
    public void ARowOutsideTheBatchIsRefused()
    {
        RecordBatch batch = Batch(("a", new Int32Array.Builder().Append(1).Build()));
        var codec = new GatewayRowCodec(batch.Schema);

        Assert.Throws<ArgumentOutOfRangeException>(() => codec.Encode(codec.Bind(batch), 1));
    }

    [Fact]
    public void ANullMapKeyIsRefused()
    {
        IArrowArray keys = new StringArray.Builder().AppendNull().Build();
        IArrowArray values = new Int32Array.Builder().Append(1).Build();
        RecordBatch batch = Batch(("m", MapOf(new MapType(StringType.Default, Int32Type.Default), keys, values, 0, 1)));
        var codec = new GatewayRowCodec(batch.Schema);

        ArgumentException e = Assert.Throws<ArgumentException>(() => codec.Encode(codec.Bind(batch), 0));
        Assert.Contains("map key must not be null", e.Message, StringComparison.Ordinal);
    }

    // ── Malformed bytes: refused as C++'s reader refuses them ──

    public static TheoryData<string, string> MalformedRows => new()
    {
        // Schema for all: [int32 a, string s, list<int32> l, map<string,int32> m], all nullable.
        { "empty buffer: no bitfield", string.Empty },
        { "truncated int32", "00" + "0100" },
        { "a string longer than the buffer", "0C" + "01000000" + "FF000000" },
        { "a list count whose bitfield cannot fit", "0B" + "FFFFFFFF" },
        { "a map count beyond the buffer", "07" + "64000000" + "00" },
        { "trailing bytes after the row", "0F" + "01000000" + "00" },
    };

    [Theory]
    [MemberData(nameof(MalformedRows))]
    public void AMalformedRowIsRefused(string what, string hex)
    {
        _ = what;
        var codec = new GatewayRowCodec(new Schema.Builder()
            .Field(f => f.Name("a").DataType(Int32Type.Default).Nullable(true))
            .Field(f => f.Name("s").DataType(StringType.Default).Nullable(true))
            .Field(f => f.Name("l").DataType(new ListType(Int32Type.Default)).Nullable(true))
            .Field(f => f.Name("m").DataType(new MapType(StringType.Default, Int32Type.Default)).Nullable(true))
            .Build());

        Assert.Throws<InvalidDataException>(() => codec.Decode(Convert.FromHexString(hex)));
    }

    [Fact]
    public void AMalformedRowInABatchIsNamedByItsIndex()
    {
        var codec = new GatewayRowCodec(new Schema.Builder().Field(f => f.Name("a").DataType(Int32Type.Default)).Build());
        var rows = new List<ReadOnlyMemory<byte>> { Convert.FromHexString("00" + "01000000"), Convert.FromHexString("00" + "01") };

        InvalidDataException e = Assert.Throws<InvalidDataException>(() => codec.DecodeBatch(rows));
        Assert.Contains("row 1", e.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void AnAllNullListLongerThanTheBufferIsNotRefused()
    {
        // A null element has no payload, so only the element bitfield must fit: 16
        // elements, all null, are 4 + 2 bytes. A count check against the payload would
        // refuse this valid row; C++'s reader accepts it.
        var codec = new GatewayRowCodec(new Schema.Builder()
            .Field(f => f.Name("l").DataType(new ListType(new Field("item", Int32Type.Default, true)))).Build());

        RecordBatch decoded = codec.Decode(Convert.FromHexString("00" + "10000000" + "FFFF"));

        Assert.Equal(Enumerable.Repeat<int?>(null, 16), Ints(decoded.Column(0), 0));
    }

    // ── Helpers ──

    private static RecordBatch Batch(params (string Name, IArrowArray Array)[] columns)
    {
        var builder = new Schema.Builder();
        foreach ((string name, IArrowArray array) in columns)
        {
            builder.Field(f => f.Name(name).DataType(array.Data.DataType).Nullable(true));
        }

        return new RecordBatch(builder.Build(), columns.Select(c => c.Array), columns[0].Array.Length);
    }

    private static byte[] Encode(RecordBatch batch)
    {
        var codec = new GatewayRowCodec(batch.Schema);
        return codec.Encode(codec.Bind(batch), 0);
    }

    private static RecordBatch RoundTrip(RecordBatch batch)
    {
        var codec = new GatewayRowCodec(batch.Schema);
        return codec.Decode(codec.Encode(codec.Bind(batch), 0));
    }

    private static ListArray Int32List(params int?[]?[] rows)
    {
        var builder = new ListArray.Builder(new Field("item", Int32Type.Default, true));
        var values = (Int32Array.Builder)builder.ValueBuilder;
        foreach (int?[]? row in rows)
        {
            if (row is null)
            {
                builder.AppendNull();
                continue;
            }

            builder.Append();
            foreach (int? v in row)
            {
                if (v.HasValue)
                {
                    values.Append(v.Value);
                }
                else
                {
                    values.AppendNull();
                }
            }
        }

        return builder.Build();
    }

    private static int?[] Ints(IArrowArray column, int row)
    {
        (IArrowArray values, int first, int count) = column switch
        {
            ListArray l => (l.Values, l.ValueOffsets[row], l.ValueOffsets[row + 1] - l.ValueOffsets[row]),
            LargeListArray l => (l.Values, (int)l.ValueOffsets[row], (int)(l.ValueOffsets[row + 1] - l.ValueOffsets[row])),
            FixedSizeListArray f => (f.Values, (f.Offset + row) * ((FixedSizeListType)f.Data.DataType).ListSize, ((FixedSizeListType)f.Data.DataType).ListSize),
            _ => throw new ArgumentException("not a list", nameof(column)),
        };
        var ints = (Int32Array)values;
        return Enumerable.Range(first, count).Select(i => ints.GetValue(i)).ToArray();
    }

    private static MapArray StringIntMap(params (string Key, int? Value)[] entries)
    {
        var keys = new StringArray.Builder();
        var values = new Int32Array.Builder();
        foreach ((string key, int? value) in entries)
        {
            keys.Append(key);
            if (value.HasValue)
            {
                values.Append(value.Value);
            }
            else
            {
                values.AppendNull();
            }
        }

        return MapOf(new MapType(StringType.Default, Int32Type.Default), keys.Build(), values.Build(), 0, entries.Length);
    }

    private static MapArray MapOf(MapType type, IArrowArray keys, IArrowArray values, int first, int count)
    {
        var entries = new ArrayData(type.KeyValueType, keys.Length, 0, 0, new[] { ArrowBuffer.Empty }, new[] { keys.Data, values.Data });
        var offsets = new ArrowBuffer.Builder<int>().Append(first).Append(first + count).Build();
        return (MapArray)ArrowArrayFactory.BuildArray(new ArrayData(type, 1, 0, 0, new[] { ArrowBuffer.Empty, offsets }, new[] { entries }));
    }

    // A struct {x: int32, y: int32} column, a null tuple being a null struct.
    private static StructArray Point(params (int X, int Y)?[] rows)
    {
        var xs = new Int32Array.Builder();
        var ys = new Int32Array.Builder();
        var validity = new ArrowBuffer.BitmapBuilder();
        int nulls = 0;
        foreach ((int X, int Y)? row in rows)
        {
            xs.Append(row?.X ?? 0);
            ys.Append(row?.Y ?? 0);
            validity.Append(row.HasValue);
            nulls += row.HasValue ? 0 : 1;
        }

        var type = new StructType(new List<Field> { new("x", Int32Type.Default, false), new("y", Int32Type.Default, false) });
        return new StructArray(type, rows.Length, new IArrowArray[] { xs.Build(), ys.Build() }, validity.Build(), nulls);
    }

    // A large string or binary column over 64-bit offsets, built by hand (no builder needed).
    private static IArrowArray Large(IArrowType type, params string[] values)
    {
        var data = new ArrowBuffer.Builder<byte>();
        var offsets = new ArrowBuffer.Builder<long>().Append(0L);
        foreach (string value in values)
        {
            data.Append(type.TypeId == ArrowTypeId.LargeBinary ? value.Select(c => (byte)c).ToArray() : Encoding.UTF8.GetBytes(value));
            offsets.Append(data.Length);
        }

        return ArrowArrayFactory.BuildArray(new ArrayData(type, values.Length, 0, 0, new[] { ArrowBuffer.Empty, offsets.Build(), data.Build() }));
    }

    private static ArrowBuffer Longs(params long[] values) => new ArrowBuffer.Builder<long>().AppendRange(values).Build();

    // A one-row column of a fixed-width type, from its little-endian integer.
    private static IArrowArray Fixed(IArrowType type, int width, long value)
    {
        byte[] bytes = BitConverter.GetBytes(value).AsSpan(0, width).ToArray();
        return ArrowArrayFactory.BuildArray(new ArrayData(type, 1, 0, 0, new[] { ArrowBuffer.Empty, new ArrowBuffer(bytes) }));
    }

    private static string Le(long value, int width) => Convert.ToHexString(BitConverter.GetBytes(value).AsSpan(0, width));
}
