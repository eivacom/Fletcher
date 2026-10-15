// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// BIND-7b: ListView, MapView and the element readers a generated view's collections use
// (D-BIND-101), over arrays built here, without any generated code or native asset.
using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.CompilerServices;

using Apache.Arrow;
using Apache.Arrow.Types;

using Xunit;

namespace Eiva.Fletcher.Model.Tests;

public sealed class CollectionViewTests
{
    private enum Level
    {
        Zero = 0,
        Two = 2,
    }

    // [[1, 2], [], [3, null, 5]] over one Int32 child.
    private static ListArray Ints()
    {
        var values = new Int32Array.Builder().Append(1).Append(2).Append(3).AppendNull().Append(5).Build();
        var offsets = new ArrowBuffer.Builder<int>().Append(0).Append(2).Append(2).Append(5).Build();
        return new ListArray(new ListType(new Field("item", Int32Type.Default, true)), 3, offsets, values, ArrowBuffer.Empty, 0);
    }

    [Fact]
    public void AListReadsItsRowsElementsInPlace()
    {
        ListArray list = Ints();
        var elements = new PrimitiveElements<int>((Int32Array)list.Values);

        ListView<int> first = ListView<int>.Of(list, 0, elements);
        ListView<int> empty = ListView<int>.Of(list, 1, elements);
        ListView<int> third = ListView<int>.Of(list, 2, elements);

        Assert.Equal(new[] { 1, 2 }, first.ToList());
        Assert.Empty(empty);
        Assert.Empty(empty.ToList());
        // A null element reads as its default, with IsNull beside it (D-BIND-101).
        Assert.Equal(new[] { 3, 0, 5 }, third.ToList());
        Assert.Equal(new[] { false, true, false }, Enumerable.Range(0, 3).Select(third.IsNull));
        Assert.Equal(5, third[2]);
    }

    [Fact]
    public void ASlicedListKeepsAbsoluteOffsets()
    {
        // A sliced ListArray keeps its whole values child; its offsets index it absolutely.
        var sliced = (ListArray)Ints().Slice(1, 2);
        var elements = new PrimitiveElements<int>((Int32Array)sliced.Values);

        Assert.Empty(ListView<int>.Of(sliced, 0, elements));
        Assert.Equal(new[] { 3, 0, 5 }, ListView<int>.Of(sliced, 1, elements).ToList());
    }

    [Fact]
    public void AnIndexOutsideTheListThrows()
    {
        ListArray list = Ints();
        ListView<int> first = ListView<int>.Of(list, 0, new PrimitiveElements<int>((Int32Array)list.Values));
        foreach (int index in new[] { -1, 2, int.MaxValue })
        {
            Assert.Throws<ArgumentOutOfRangeException>(() => first[index]);
            Assert.Throws<ArgumentOutOfRangeException>(() => first.IsNull(index));
        }
        Assert.Empty(default(ListView<int>));
        Assert.Empty(default(ListView<int>).ToList());
    }

    [Fact]
    public void ForeachMatchesTheIndexerAndAllocatesNothing()
    {
        ListArray list = Ints();
        ListView<int> third = ListView<int>.Of(list, 2, new PrimitiveElements<int>((Int32Array)list.Values));
        var seen = new List<int>();
        foreach (int x in third)
        {
            seen.Add(x);
        }
        Assert.Equal(third.ToList(), seen);
        Assert.Equal(third.ToList(), third.AsEnumerable().ToList());

        // The runtime allocates a few hundred bytes once while it compiles and tiers the loop up
        // (a scratch probe measures 0 from the second call on), so the bound is on what reads
        // would cost: one allocating read is at least 24 bytes, times 60,000 reads here.
        Read(third, 10_000);
        long before = GC.GetAllocatedBytesForCurrentThread();
        long total = Read(third, 10_000);
        long allocated = GC.GetAllocatedBytesForCurrentThread() - before;
        Assert.Equal(160_000, total);
        Assert.True(allocated < 4096, $"{allocated} bytes for 60,000 reads");
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static long Read(ListView<int> list, int rounds)
    {
        long sum = 0;
        for (int round = 0; round < rounds; round++)
        {
            foreach (int x in list)
            {
                sum += x;
            }
            for (int j = 0; j < list.Count; j++)
            {
                sum += list[j];
            }
        }
        return sum;
    }

    [Fact]
    public void EachElementKindReadsAsTheModelsTypeAndANullAsItsDefault()
    {
        var bools = new BooleanArray.Builder().Append(true).AppendNull().Build();
        var boolElements = new BooleanElements(bools);
        Assert.True(boolElements.Get(0));
        Assert.False(boolElements.Get(1));
        Assert.True(boolElements.IsNull(1));

        var strings = new StringArray.Builder().Append("héllo").AppendNull().Build();
        var stringElements = new StringElements(strings);
        Assert.Equal("héllo", stringElements.Get(0));
        Assert.Equal("", stringElements.Get(1));
        Assert.True(stringElements.IsNull(1));

        var bytes = new BinaryArray.Builder().Append(new byte[] { 1, 2 }.AsSpan()).AppendNull().Build();
        var bytesElements = new BinaryElements(bytes);
        Assert.Equal(new byte[] { 1, 2 }, bytesElements.Get(0));
        Assert.Empty(bytesElements.Get(1));

        var levels = new Int32Array.Builder().Append(2).AppendNull().Build();
        var levelElements = new EnumElements<Level>(levels);
        Assert.Equal(Level.Two, levelElements.Get(0));
        Assert.Equal(Level.Zero, levelElements.Get(1));

        var stamps = new TimestampArray.Builder(new TimestampType(TimeUnit.Microsecond, "UTC")).Append(DateTimeOffset.UnixEpoch.AddTicks(10)).Build();
        Assert.Equal(new Timestamp(1, TimeUnit.Microsecond, "UTC"), new TimestampElements(stamps).Get(0));

        var spans = new DurationArray.Builder(DurationType.Millisecond).Append(TimeSpan.FromSeconds(2)).Build();
        Assert.Equal(new Duration(2000, TimeUnit.Millisecond), new DurationElements(spans).Get(0));
    }

    [Fact]
    public void AnEnumThatIsNotIntBackedIsRefused()
    {
        var levels = new Int32Array.Builder().Append(1).Build();
        Assert.Throws<ArgumentException>(() => new EnumElements<ByteLevel>(levels));
    }

    private enum ByteLevel : byte
    {
        One = 1,
    }

    // Rows 0 and 2 present, row 1 null, read as "row i" so the test sees which row it got.
    private sealed class FakeRows : IArrowRows<int>
    {
        public bool IsNull(int row) => row == 1;

        public int At(int row) => 100 + row;
    }

    [Fact]
    public void AStructElementReadsAsItsViewAndANullOneAsNull()
    {
        var elements = new ViewElements<int>(new FakeRows());
        var window = new ListView<int?>(elements, 0, 3);
        Assert.Equal(new int?[] { 100, null, 102 }, window.ToList());
        Assert.True(window.IsNull(1));
    }

    [Fact]
    public void AListOfListsReadsInnerWindowsAndANullInnerListAsEmpty()
    {
        // [[1, 2], null, [3, null, 5]] as the inner lists of one outer list. The null inner list
        // still spans elements (9, 9), which Arrow allows: it must read as empty all the same.
        var values = new Int32Array.Builder().Append(1).Append(2).Append(9).Append(9).Append(3).AppendNull().Append(5).Build();
        var innerOffsets = new ArrowBuffer.Builder<int>().Append(0).Append(2).Append(4).Append(7).Build();
        var validity = new ArrowBuffer.BitmapBuilder().Append(true).Append(false).Append(true).Build();
        var inner = new ListArray(new ListType(new Field("item", Int32Type.Default, true)), 3, innerOffsets, values, validity, 1);
        var outerOffsets = new ArrowBuffer.Builder<int>().Append(0).Append(3).Build();
        var outer = new ListArray(new ListType(new Field("item", inner.Data.DataType, true)), 1, outerOffsets, inner, ArrowBuffer.Empty, 0);

        var elements = new ListElements<int>(inner, new PrimitiveElements<int>(values));
        ListView<ListView<int>> lists = ListView<ListView<int>>.Of(outer, 0, elements);

        Assert.Equal(3, lists.Count);
        Assert.Equal(new[] { 1, 2 }, lists[0].ToList());
        Assert.True(lists.IsNull(1));
        Assert.Empty(lists[1]);
        Assert.Equal(new[] { 3, 0, 5 }, lists[2].ToList());
    }

    [Fact]
    public void AMapReadsItsEntriesInTheColumnsOrder()
    {
        var keys = new StringArray.Builder().Append("b").Append("a").Append("b").Build();
        var values = new Int32Array.Builder().Append(1).AppendNull().Append(3).Build();
        var type = new MapType(StringType.Default, Int32Type.Default);
        var entries = new StructArray(new StructType(new List<Field> { type.KeyField, type.ValueField }), 3,
            new IArrowArray[] { keys, values }, ArrowBuffer.Empty, 0);
        var offsets = new ArrowBuffer.Builder<int>().Append(0).Append(0).Append(3).Build();
        var map = new MapArray(type, 2, offsets, entries, ArrowBuffer.Empty, 0);

        var keyElements = new StringElements((StringArray)map.Keys);
        var valueElements = new PrimitiveElements<int>((Int32Array)map.Values);
        MapView<string, int> empty = MapView<string, int>.Of(map, 0, keyElements, valueElements);
        MapView<string, int> second = MapView<string, int>.Of(map, 1, keyElements, valueElements);

        Assert.Empty(empty);
        Assert.Equal(3, second.Count);
        Assert.Equal("a", second.Key(1));
        Assert.Equal(0, second.Value(1));
        Assert.True(second.ValueIsNull(1));
        var expected = new List<KeyValuePair<string, int>> { new("b", 1), new("a", 0), new("b", 3) };
        Assert.Equal(expected, second.ToList());
        Assert.Equal(expected, second.Select(e => e).ToList());
        Assert.Throws<ArgumentOutOfRangeException>(() => second.Key(3));
    }
}
