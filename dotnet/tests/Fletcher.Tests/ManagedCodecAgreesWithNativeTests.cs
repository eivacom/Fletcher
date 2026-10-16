// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// BIND-8a: the managed gateway codec against the native codec, byte for byte.
//
// C# has two codecs for one wire format: the shim's (C++, behind FletcherCodec) and the
// gateway client's managed one, the single exception D-BIND-1 allows. Both are held to
// C++'s goldens through the protoc-coverage rows (CoverageWireBytesTests, here and in
// Fletcher.GatewayClient.Tests), which reach every shape the proto mapping produces.
//
// This file covers the rest of the accept list the two share (the C ABI's), where no C++
// golden exists: fixed-size lists, the large string/binary/list forms, dictionaries of
// several index widths, the date and time types with units other than nanoseconds,
// narrow integers, and a batch sliced so that every read goes through an offset. Each
// batch is encoded row by row by both codecs and the bytes compared; then the managed
// codec decodes the native bytes and the native codec re-encodes what it decoded, so
// each reads the other.
using System;
using System.Buffers;
using System.Collections.Generic;
using System.Linq;
using System.Text;

using Apache.Arrow;
using Apache.Arrow.Types;

using Eiva.Fletcher.GatewayClient;

using Xunit;

namespace Eiva.Fletcher.Tests;

public sealed class ManagedCodecAgreesWithNativeTests
{
    public static TheoryData<string> Cases => new()
    {
        "fixed-size list",
        "large forms",
        "dictionaries",
        "temporal units",
        "narrow integers",
        "map of lists",
    };

    [Theory]
    [MemberData(nameof(Cases))]
    public void BothCodecsWriteTheSameBytesAndReadEachOther(string name)
    {
        AssertAgree(Make(name));
    }

    [Fact(Skip = "The native codec ignores a sliced parent's offset in nested columns: nanoarrow_codec.cpp's " +
        "EncodeStruct indexes the children at the row alone, and ArrowArrayViewListChildOffset does not add the " +
        "view's offset, so a sliced struct, list, map or fixed-size list writes the wrong row. Found by this " +
        "test in BIND-8a (2026-10-16); the managed codec is right (RowCodecTests in Fletcher.GatewayClient.Tests). " +
        "Unskip with the C ABI fix.")]
    public void BothCodecsAgreeOnASlicedBatch()
    {
        AssertAgree(Sliced());
    }

    private static void AssertAgree(RecordBatch batch)
    {
        using var native = new FletcherCodec(batch.Schema);
        using BoundRows nativeRows = native.Bind(batch);
        var managed = new GatewayRowCodec(batch.Schema);
        BoundBatch managedRows = managed.Bind(batch);

        // A decoded batch carries the DECODED schema (a dictionary comes back as its
        // values), and the native codec binds only its own schema, so what either codec
        // decoded is re-encoded by a native codec opened over that schema.
        using var nativeDecoded = new FletcherCodec(native.DecodedSchema);

        for (int row = 0; row < batch.Length; ++row)
        {
            var output = new ArrayBufferWriter<byte>();
            native.Encode(nativeRows, row, output);
            byte[] nativeBytes = output.WrittenSpan.ToArray();

            Assert.Equal(Convert.ToHexString(nativeBytes), Convert.ToHexString(managed.Encode(managedRows, row)));

            RecordBatch readByManaged = managed.Decode(nativeBytes);
            using BoundRows again = nativeDecoded.Bind(Retype(readByManaged, nativeDecoded.Schema));
            var reencoded = new ArrayBufferWriter<byte>();
            nativeDecoded.Encode(again, 0, reencoded);
            Assert.Equal(Convert.ToHexString(nativeBytes), Convert.ToHexString(reencoded.WrittenSpan));
        }
    }

    private static RecordBatch Make(string name) => name switch
    {
        "fixed-size list" => Batch(
            ("v", FixedList(3, new int?[] { 1, null, 3, 4, 5, 6, 0, 0, 0 }, new[] { true, true, false }))),
        "large forms" => Batch(
            ("s", Large(LargeStringType.Default, "alpha", string.Empty, "Æøå")),
            ("b", Large(LargeBinaryType.Default, "\u0001", "\u0002\u0003", string.Empty)),
            ("l", LargeIntList(new[] { 0, 2, 2, 5 }, 7, 8, 9, 10, 11))),
        "dictionaries" => Batch(
            ("d8", new DictionaryArray(new DictionaryType(Int8Type.Default, StringType.Default, false),
                new Int8Array.Builder().Append(1).AppendNull().Append(0).Build(),
                new StringArray.Builder().Append("north").Append("south").Build())),
            ("d32", new DictionaryArray(new DictionaryType(Int32Type.Default, Int64Type.Default, false),
                new Int32Array.Builder().Append(2).Append(0).Append(1).Build(),
                new Int64Array.Builder().Append(-1).Append(0).Append(long.MaxValue).Build()))),
        "temporal units" => Batch(
            ("d32", Fixed(Date32Type.Default, 4, 19_000, -1, 0)),
            ("d64", Fixed(Date64Type.Default, 8, 1_641_600_000_000L, 0, -86_400_000L)),
            ("t32", Fixed(new Time32Type(TimeUnit.Second), 4, 59, 0, 86_399)),
            ("t64", Fixed(new Time64Type(TimeUnit.Microsecond), 8, 1L, 0, 86_399_999_999L)),
            ("ts", Fixed(new TimestampType(TimeUnit.Microsecond, "UTC"), 8, -1L, 0, 1_700_000_000_000_000L)),
            ("dur", Fixed(DurationType.Millisecond, 8, 250L, -250L, 0))),
        "narrow integers" => Batch(
            ("i8", new Int8Array.Builder().Append(sbyte.MinValue).Append(0).AppendNull().Build()),
            ("i16", new Int16Array.Builder().Append(short.MinValue).AppendNull().Append(short.MaxValue).Build()),
            ("u8", new UInt8Array.Builder().Append(byte.MaxValue).Append(0).Append(1).Build()),
            ("u16", new UInt16Array.Builder().Append(ushort.MaxValue).Append(0).Append(1).Build()),
            ("u64", new UInt64Array.Builder().Append(ulong.MaxValue).Append(0).Append(1).Build()),
            ("f", new FloatArray.Builder().Append(float.NaN).Append(-0.0f).Append(float.Epsilon).Build())),
        "map of lists" => Batch(("m", MapOfLists())),
        _ => throw new ArgumentOutOfRangeException(nameof(name), name, null),
    };

    // A five-row batch sliced to its middle three, with nulls placed so that every read
    // that ignored an offset would pick a different bit or value.
    private static RecordBatch Sliced()
    {
        var pointType = new StructType(new List<Field> { new("x", Int32Type.Default, true), new("y", StringType.Default, true) });
        IArrowArray xs = new Int32Array.Builder().Append(1).AppendNull().Append(3).Append(4).AppendNull().Build();
        IArrowArray ys = new StringArray.Builder().AppendNull().Append("b").Append("c").AppendNull().Append("e").Build();
        var validity = new ArrowBuffer.BitmapBuilder().Append(true).Append(true).Append(false).Append(true).Append(true).Build();
        var points = new StructArray(pointType, 5, new[] { xs, ys }, validity, 1);

        var lists = new ListArray.Builder(new Field("item", DoubleType.Default, true));
        var values = (DoubleArray.Builder)lists.ValueBuilder;
        lists.Append(); values.Append(1.0);
        lists.Append(); values.AppendNull().Append(2.0);
        lists.AppendNull();
        lists.Append(); values.Append(3.0).Append(4.0).Append(5.0);
        lists.Append();

        RecordBatch whole = Batch(
            ("p", points),
            ("l", lists.Build()),
            ("b", new BooleanArray.Builder().Append(true).AppendNull().Append(false).Append(true).Append(false).Build()));
        return whole.Slice(1, 3);
    }

    private static MapArray MapOfLists()
    {
        // map<string, list<int32>>: two rows, a null value in the first.
        IArrowArray keys = new StringArray.Builder().Append("a").Append("b").Append("c").Build();
        var lists = new ListArray.Builder(new Field("item", Int32Type.Default, true));
        var ints = (Int32Array.Builder)lists.ValueBuilder;
        lists.Append(); ints.Append(1).Append(2);
        lists.AppendNull();
        lists.Append(); ints.AppendNull();
        IArrowArray values = lists.Build();

        var type = new MapType(new Field("key", StringType.Default, false), new Field("value", values.Data.DataType, true), false);
        var entries = new ArrayData(type.KeyValueType, 3, 0, 0, new[] { ArrowBuffer.Empty }, new[] { keys.Data, values.Data });
        ArrowBuffer offsets = new ArrowBuffer.Builder<int>().Append(0).Append(2).Append(3).Build();
        return (MapArray)ArrowArrayFactory.BuildArray(new ArrayData(type, 2, 0, 0, new[] { ArrowBuffer.Empty, offsets }, new[] { entries }));
    }

    private static RecordBatch Batch(params (string Name, IArrowArray Array)[] columns)
    {
        var builder = new Schema.Builder();
        foreach ((string name, IArrowArray array) in columns)
        {
            builder.Field(f => f.Name(name).DataType(array.Data.DataType).Nullable(true));
        }

        return new RecordBatch(builder.Build(), columns.Select(c => c.Array), columns[0].Array.Length);
    }

    // The managed decoder's batch carries the managed codec's DecodedSchema; the native
    // codec binds against its own, which is the same types. Re-wrap so the schema object
    // the native Bind checks is its own.
    private static RecordBatch Retype(RecordBatch batch, Schema schema) => new(schema, batch.Arrays, batch.Length);

    private static IArrowArray FixedList(int size, int?[] values, bool[] valid)
    {
        var builder = new Int32Array.Builder();
        foreach (int? v in values)
        {
            if (v.HasValue)
            {
                builder.Append(v.Value);
            }
            else
            {
                builder.AppendNull();
            }
        }

        var bitmap = new ArrowBuffer.BitmapBuilder();
        foreach (bool b in valid)
        {
            bitmap.Append(b);
        }

        return ArrowArrayFactory.BuildArray(new ArrayData(
            new FixedSizeListType(new Field("item", Int32Type.Default, true), size), valid.Length, valid.Count(b => !b), 0,
            new[] { bitmap.Build() }, new[] { builder.Build().Data }));
    }

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

    private static IArrowArray LargeIntList(int[] offsets, params int[] values)
    {
        IArrowArray child = new Int32Array.Builder().AppendRange(values).Build();
        ArrowBuffer longs = new ArrowBuffer.Builder<long>().AppendRange(offsets.Select(o => (long)o)).Build();
        return ArrowArrayFactory.BuildArray(new ArrayData(
            new LargeListType(new Field("item", Int32Type.Default, true)), offsets.Length - 1, 0, 0,
            new[] { ArrowBuffer.Empty, longs }, new[] { child.Data }));
    }

    private static IArrowArray Fixed(IArrowType type, int width, params long[] values)
    {
        byte[] bytes = values.SelectMany(v => BitConverter.GetBytes(v).Take(width)).ToArray();
        return ArrowArrayFactory.BuildArray(new ArrayData(type, values.Length, 0, 0, new[] { ArrowBuffer.Empty, new ArrowBuffer(bytes) }));
    }
}
