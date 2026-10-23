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

    // This case found a native bug (2026-10-16): nanoarrow_codec.cpp indexed a struct's
    // children at the row alone, and read a list's offsets at it, so a sliced struct,
    // list, map or fixed-size list wrote ANOTHER row's values. Fixed in the C ABI, where
    // ASlicedBatchEncodesEachRowAsTheWholeBatchDoes now holds it to arrow-bridge too.
    [Fact]
    public void BothCodecsAgreeOnASlicedBatch()
    {
        AssertAgree(Sliced());
    }

    public static TheoryData<string> Refusals => new()
    {
        "null map key",
        "null dictionary entry",
    };

    // D-BIND-131 (review of BIND-8): the native codec used to encode these from whatever bytes
    // lay under the null, where the managed codec refuses them; both refuse them now.
    [Theory]
    [MemberData(nameof(Refusals))]
    public void BothCodecsRefuseTheSameRows(string name)
    {
        RecordBatch batch = name switch
        {
            "null map key" => Batch(("m", NullKeyMap())),
            "null dictionary entry" => Batch(("d", NullEntryDictionary())),
            _ => throw new ArgumentOutOfRangeException(nameof(name)),
        };

        using var native = new FletcherCodec(batch.Schema);
        using BoundRows nativeRows = native.Bind(batch);
        Exception nativeRefusal = Assert.ThrowsAny<Exception>(() => native.Encode(nativeRows, 0, new ArrayBufferWriter<byte>()));

        var managed = new GatewayRowCodec(batch.Schema);
        Exception managedRefusal = Assert.ThrowsAny<ArgumentException>(() => managed.Encode(managed.Bind(batch), 0));

        string expected = name == "null map key" ? "map key must not be null" : "null dictionary entry";
        Assert.Contains(expected, nativeRefusal.Message, StringComparison.Ordinal);
        Assert.Contains(expected, managedRefusal.Message, StringComparison.Ordinal);
    }

    public static TheoryData<string> ForbiddenNulls => new()
    {
        "a top-level column",
        "a field of a present struct",
        "a list element",
        "a map value",
    };

    // D-BIND-156: a null where the schema promises a value (the field is not nullable, its
    // parent value is present) is refused by both codecs at BIND, before any row is encoded,
    // naming the row. The native codec is the C ABI's, the one C#'s Publisher encodes with.
    [Theory]
    [MemberData(nameof(ForbiddenNulls))]
    public void BothCodecsRefuseANullWhereTheSchemaPromisesAValue(string name)
    {
        RecordBatch batch = ForbiddenNull(name);

        using var native = new FletcherCodec(batch.Schema);
        Exception nativeRefusal = Assert.ThrowsAny<Exception>(() => native.Bind(batch).Dispose());

        var managed = new GatewayRowCodec(batch.Schema);
        Exception managedRefusal = Assert.ThrowsAny<ArgumentException>(() => managed.Bind(batch));

        foreach (Exception refusal in new[] { nativeRefusal, managedRefusal })
        {
            Assert.Contains("is not nullable, but row 0 has a null there", refusal.Message, StringComparison.Ordinal);
            Assert.Contains("D-BIND-156", refusal.Message, StringComparison.Ordinal);
        }
    }

    // ...while a null BELOW a null struct is no value at all: both codecs accept it and write
    // the same bytes.
    [Fact]
    public void BothCodecsAcceptANullBelowANullStructAndAgreeOnIt()
    {
        var x = new Field("x", Int32Type.Default, nullable: false);
        IArrowArray children = new Int32Array.Builder().AppendNull().Append(5).Build();
        ArrowBuffer validity = new ArrowBuffer.BitmapBuilder().Append(false).Append(true).Build();
        var s = new StructArray(new StructType(new[] { x }), 2, new[] { children }, validity, nullCount: 1);
        AssertAgree(Batch(("s", s)));
    }

    private static RecordBatch ForbiddenNull(string name)
    {
        IArrowArray ints = new Int32Array.Builder().AppendNull().Append(2).Build();
        switch (name)
        {
            case "a top-level column":
                return OneColumn(new Field("id", Int32Type.Default, nullable: false), ints);

            case "a field of a present struct":
                {
                    var type = new StructType(new[] { new Field("x", Int32Type.Default, nullable: false) });
                    return OneColumn(new Field("s", type, nullable: true), new StructArray(type, 2, new[] { ints }, ArrowBuffer.Empty, nullCount: 0));
                }

            case "a list element":
                {
                    var type = new ListType(new Field("item", Int32Type.Default, nullable: false));
                    ArrowBuffer offsets = new ArrowBuffer.Builder<int>().Append(0).Append(2).Build();
                    var list = (ListArray)ArrowArrayFactory.BuildArray(new ArrayData(type, 1, 0, 0, new[] { ArrowBuffer.Empty, offsets }, new[] { ints.Data }));
                    return OneColumn(new Field("readings", type, nullable: true), list);
                }

            case "a map value":
                {
                    IArrowArray keys = new StringArray.Builder().Append("a").Append("b").Build();
                    var type = new MapType(new Field("key", StringType.Default, false), new Field("value", Int32Type.Default, nullable: false), false);
                    var entries = new ArrayData(type.KeyValueType, 2, 0, 0, new[] { ArrowBuffer.Empty }, new[] { keys.Data, ints.Data });
                    ArrowBuffer offsets = new ArrowBuffer.Builder<int>().Append(0).Append(2).Build();
                    var map = (MapArray)ArrowArrayFactory.BuildArray(new ArrayData(type, 1, 0, 0, new[] { ArrowBuffer.Empty, offsets }, new[] { entries }));
                    return OneColumn(new Field("tags", type, nullable: true), map);
                }

            default:
                throw new ArgumentOutOfRangeException(nameof(name));
        }
    }

    private static RecordBatch OneColumn(Field field, IArrowArray column) =>
        new(new Schema(new[] { field }, null), new[] { column }, column.Length);

    private static MapArray NullKeyMap()
    {
        // map<string, int32>, one row {"a": 1, null: 2}: Apache.Arrow builds it without validating.
        IArrowArray keys = new StringArray.Builder().Append("a").AppendNull().Build();
        IArrowArray values = new Int32Array.Builder().Append(1).Append(2).Build();
        var type = new MapType(new Field("key", StringType.Default, false), new Field("value", Int32Type.Default, true), false);
        var entries = new ArrayData(type.KeyValueType, 2, 0, 0, new[] { ArrowBuffer.Empty }, new[] { keys.Data, values.Data });
        ArrowBuffer offsets = new ArrowBuffer.Builder<int>().Append(0).Append(2).Build();
        return (MapArray)ArrowArrayFactory.BuildArray(new ArrayData(type, 1, 0, 0, new[] { ArrowBuffer.Empty, offsets }, new[] { entries }));
    }

    private static DictionaryArray NullEntryDictionary()
    {
        // One row whose index names the dictionary's null entry.
        IArrowArray dictionary = new StringArray.Builder().Append("alpha").AppendNull().Build();
        IArrowArray indices = new Int32Array.Builder().Append(1).Build();
        return new DictionaryArray(new DictionaryType(Int32Type.Default, StringType.Default, false), indices, dictionary);
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

            using RecordBatch readByManaged = managed.Decode(nativeBytes);
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
