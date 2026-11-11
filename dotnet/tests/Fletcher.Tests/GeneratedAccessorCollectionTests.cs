// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// BIND-7b: the GENERATED accessor's lists, maps and lists of lists, read as borrowed windows
// (D-BIND-101), over batches the generated model's ToArrow builds. A null element, which no
// proto producer writes but Arrow allows, is put in by rebuilding one column of such a batch.
using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.CompilerServices;

using Apache.Arrow;
using Apache.Arrow.Types;

using Eiva.Fletcher.Model;

using Fletcher.Gen.Golden.Accessor;

using Xunit;

namespace Eiva.Fletcher.Tests;

public sealed class GeneratedAccessorCollectionTests
{
    private static Position P(int x) => new() { Lat = x, Lon = -x, Depth = x % 2 == 0 ? x : null };

    private static Track Full(int i) => new()
    {
        Id = i,
        Ints = { i, -1, int.MaxValue },
        Names = { "a" + i, "héllo ✓" },
        Blobs = { new byte[] { 1, (byte)i }, System.Array.Empty<byte>() },
        Flags = { true, false },
        Qualities = { Quality.Good, Quality.Poor },
        Values = { 0.5 * i, -1 },
        Points = { P(i), P(i + 10) },
        Scores = { new("b", i), new("a", -i), new("b", 3) },
        Places = { new("x", P(i)) },
        Labels = { new(long.MinValue, "min"), new(i, "i") },
        Grid = { new() { P(1), P(2) }, new(), new() { P(i) } },
        Cube = { new() { new() { P(i) }, new() }, new() },
        Matrix = { new() { 1, 2 }, new(), new() { i } },
        MaybePoints = { P(i + 100) },
    };

    private static TrackAccessor Make(RecordBatch batch)
    {
        Assert.True(TrackAccessor.TryMake(batch, out var accessor, out var error), error);
        return accessor;
    }

    private static string Refusal(RecordBatch batch)
    {
        Assert.False(TrackAccessor.TryMake(batch, out var accessor, out var error));
        Assert.Null(accessor);
        Assert.NotNull(error);
        return error;
    }

    private static RecordBatch Replace(RecordBatch batch, int index, IArrowArray column) =>
        new(batch.Schema, batch.Arrays.Select((array, i) => i == index ? column : array), batch.Length);

    // A validity bitmap of `length` bits, clear at `nulls`.
    private static ArrowBuffer Validity(int length, params int[] nulls)
    {
        var builder = new ArrowBuffer.BitmapBuilder();
        for (int i = 0; i < length; i++)
        {
            builder.Append(!nulls.Contains(i));
        }
        return builder.Build();
    }

    private static string Describe(Position p) => $"{p.Lat}/{p.Lon}/{p.Depth?.ToString(System.Globalization.CultureInfo.InvariantCulture) ?? "-"}";

    private static string Describe(PositionView? p) =>
        p is null ? "null" : $"{p.Value.Lat}/{p.Value.Lon}/{p.Value.Depth?.ToString(System.Globalization.CultureInfo.InvariantCulture) ?? "-"}";

    private static void AssertRow(Track expected, TrackView view)
    {
        Assert.Equal(expected.Id, view.Id);
        Assert.Equal(expected.Ints, view.Ints.ToList());
        Assert.Equal(expected.Names, view.Names.ToList());
        Assert.Equal(expected.Blobs, view.Blobs.ToList());
        Assert.Equal(expected.Flags, view.Flags.ToList());
        Assert.Equal(expected.Qualities, view.Qualities.ToList());
        Assert.Equal(expected.Values, view.Values.ToList());
        Assert.Equal(expected.Points.Select(Describe), view.Points.Select(Describe));
        Assert.Equal(expected.Scores, view.Scores.ToList());
        Assert.Equal(expected.Places.Select(e => e.Key + "=" + Describe(e.Value)),
            view.Places.Select(e => e.Key + "=" + Describe(e.Value)));
        Assert.Equal(expected.Labels, view.Labels.ToList());
        Assert.Equal(expected.Grid.Select(l => l.Select(Describe)), view.Grid.Select(l => l.Select(Describe)));
        Assert.Equal(expected.Cube.Select(g => g.Select(l => l.Select(Describe))),
            view.Cube.Select(g => g.Select(l => l.Select(Describe))));
        Assert.Equal(expected.Matrix, view.Matrix.Select(l => l.ToList()));
        Assert.Equal(expected.MaybePoints.Select(Describe), view.MaybePoints.Select(Describe));
    }

    [Fact]
    public void EveryCollectionReadsBackAsTheModelWroteIt()
    {
        Track[] rows = { Full(1), new Track { Id = 2 }, Full(3) };
        using RecordBatch batch = Track.ToArrow(rows);
        var accessor = Make(batch);
        for (int i = 0; i < rows.Length; i++)
        {
            AssertRow(rows[i], accessor.At(i));
        }
    }

    [Fact]
    public void ToRowCopiesEveryCollectionOutAsTheModelWroteIt()
    {
        // D-BIND-102: ToRow is the model's FromArrow, collections included; checked by writing the
        // row again and reading it through the view.
        Track[] rows = { Full(1), new Track { Id = 2 }, Full(3) };
        using RecordBatch batch = Track.ToArrow(rows);
        var accessor = Make(batch);
        for (int i = 0; i < rows.Length; i++)
        {
            using RecordBatch again = Track.ToArrow(new[] { accessor.ToRow(i) });
            AssertRow(rows[i], Make(again).At(0));
        }
    }

    [Fact]
    public void ACollectionOfAStructAtANonZeroOffsetReadsItsOwnRows()
    {
        // D-BIND-10 for lists: the struct's children come windowed, and a list child's offsets
        // then index its whole values child absolutely, so row 0 of a slice at 1 is source row 1.
        Track[] rows = { Full(1), Full(2), new Track { Id = 3 }, Full(4) };
        using RecordBatch batch = Track.ToArrow(rows);
        var array = new StructArray(new StructType(batch.Schema.FieldsList), batch.Length, batch.Arrays, ArrowBuffer.Empty, 0);
        var slice = (StructArray)array.Slice(1, 2);

        Assert.True(TrackAccessor.TryMake(slice, out var accessor, out var error), error);
        AssertRow(rows[1], accessor.At(0));
        AssertRow(rows[2], accessor.At(1));
    }

    [Fact]
    public void ANullElementReadsAsCppReadsIt()
    {
        // D-BIND-101: a scalar element as its default with IsNull, a struct element and a
        // message map value as null, an inner list as an empty window with IsNull.
        using RecordBatch model = Track.ToArrow(new[] { Full(5) });
        RecordBatch batch = model;

        var ints = (ListArray)batch.Column(1);
        var intValues = new Int32Array.Builder().AppendNull().Append(-1).Append(int.MaxValue).Build();
        batch = Replace(batch, 1, new ListArray(ints.Data.DataType, ints.Length, ints.ValueOffsetsBuffer, intValues, ArrowBuffer.Empty, 0));

        var points = (ListArray)batch.Column(7);
        var pointValues = (StructArray)points.Values;
        var nullPoint = new StructArray(pointValues.Data.DataType, pointValues.Length, pointValues.Fields, Validity(pointValues.Length, 0), 1);
        batch = Replace(batch, 7, new ListArray(points.Data.DataType, points.Length, points.ValueOffsetsBuffer, nullPoint, ArrowBuffer.Empty, 0));

        var scores = (MapArray)batch.Column(8);
        var scoreValues = new Int32Array.Builder().AppendNull().Append(-5).Append(3).Build();
        var scoreEntries = new StructArray(scores.KeyValues.Data.DataType, scores.KeyValues.Length, new IArrowArray[] { scores.Keys, scoreValues }, ArrowBuffer.Empty, 0);
        batch = Replace(batch, 8, new MapArray(scores.Data.DataType, scores.Length, scores.ValueOffsetsBuffer, scoreEntries, ArrowBuffer.Empty, 0));

        var places = (MapArray)batch.Column(9);
        var placeValues = (StructArray)places.Values;
        var nullPlace = new StructArray(placeValues.Data.DataType, placeValues.Length, placeValues.Fields, Validity(placeValues.Length, 0), 1);
        var placeEntries = new StructArray(places.KeyValues.Data.DataType, places.KeyValues.Length, new IArrowArray[] { places.Keys, nullPlace }, ArrowBuffer.Empty, 0);
        batch = Replace(batch, 9, new MapArray(places.Data.DataType, places.Length, places.ValueOffsetsBuffer, placeEntries, ArrowBuffer.Empty, 0));

        var grid = (ListArray)batch.Column(11);
        var rowsOfGrid = (ListArray)grid.Values;
        var nullRow = new ListArray(rowsOfGrid.Data.DataType, rowsOfGrid.Length, rowsOfGrid.ValueOffsetsBuffer, rowsOfGrid.Values, Validity(rowsOfGrid.Length, 0), 1);
        batch = Replace(batch, 11, new ListArray(grid.Data.DataType, grid.Length, grid.ValueOffsetsBuffer, nullRow, ArrowBuffer.Empty, 0));

        TrackView view = Make(batch).At(0);

        Assert.Equal(0, view.Ints[0]);
        Assert.True(view.Ints.IsNull(0));
        Assert.False(view.Ints.IsNull(1));
        Assert.Null(view.Points[0]);
        Assert.True(view.Points.IsNull(0));
        Assert.Equal(15, view.Points[1]!.Value.Lat);
        Assert.Equal(0, view.Scores.Value(0));
        Assert.True(view.Scores.ValueIsNull(0));
        Assert.Equal("b", view.Scores.Key(0));
        Assert.Null(view.Places.Value(0));
        Assert.True(view.Places.ValueIsNull(0));
        Assert.True(view.Grid.IsNull(0));
        Assert.Empty(view.Grid[0]);
        // B1 of BIND-7's review: ToRow, the model's FromArrow, reads the null inner list as empty too.
        Assert.Empty(Make(batch).ToRow(0).Grid[0]);
        Assert.Equal(new[] { "5/-5/-" }, view.Grid[2].Select(Describe));
    }

    [Fact]
    public void ANullRowOfAnOptionalWrapperListReadsAsAnEmptyWindow()
    {
        // G-Q4: `optional PositionList` is nullable in its column but a plain List in the model.
        using RecordBatch model = Track.ToArrow(new[] { Full(1), Full(2) });
        var maybe = (ListArray)model.Column(14);
        var withNull = new ListArray(maybe.Data.DataType, maybe.Length, maybe.ValueOffsetsBuffer, maybe.Values, Validity(2, 0), 1);
        var accessor = Make(Replace(model, 14, withNull));

        Assert.Empty(accessor.At(0).MaybePoints);
        // B1 of BIND-7's review: ToRow, the model's FromArrow, reads it as the view does.
        Assert.Empty(accessor.ToRow(0).MaybePoints);
        Assert.Equal(new[] { "102/-102/102" }, accessor.At(1).MaybePoints.Select(Describe));
    }

    [Fact]
    public void EveryCollectionRefusalSaysWhatAndWhere()
    {
        using RecordBatch batch = Track.ToArrow(new[] { Full(1), Full(2) });

        // MapArray derives from ListArray: a map where a list belongs is refused by type id.
        Assert.Equal("Track column 1 'ints': expected list, got map.", Refusal(Replace(batch, 1, batch.Column(8))));

        // StringArray derives from BinaryArray: a list of strings where a list of bytes belongs
        // passes the class check and is refused by the element's type.
        Assert.Equal("Track column 3 'blobs' element: expected binary, got utf8.", Refusal(Replace(batch, 3, batch.Column(2))));

        var ints = (ListArray)batch.Column(1);
        var longs = new Int64Array.Builder().AppendRange(Enumerable.Range(0, ints.Values.Length).Select(i => (long)i)).Build();
        var longList = new ListArray(new ListType(new Field("item", Int64Type.Default, true)), ints.Length, ints.ValueOffsetsBuffer, longs, ArrowBuffer.Empty, 0);
        Assert.Equal("Track column 1 'ints' element: expected int32, got int64.", Refusal(Replace(batch, 1, longList)));

        var nullRow = new ListArray(ints.Data.DataType, ints.Length, ints.ValueOffsetsBuffer, ints.Values, Validity(2, 1), 1);
        Assert.Equal("Track column 1 'ints': non-nullable, found 1 nulls.", Refusal(Replace(batch, 1, nullRow)));

        // Labels is map<int64, string>; Scores is map<string, int32>.
        Assert.Equal("Track column 10 'labels' key: expected int64, got utf8.", Refusal(Replace(batch, 10, batch.Column(8))));

        // A struct element is validated by its own accessor, whose reason is passed on whole.
        var points = (ListArray)batch.Column(7);
        var pointValues = (StructArray)points.Values;
        var badLat = new Int32Array.Builder().AppendRange(Enumerable.Range(0, pointValues.Length)).Build();
        var badPoints = new StructArray(
            new StructType(new[] { new Field("lat", Int32Type.Default, false) }.Concat(((StructType)pointValues.Data.DataType).Fields.Skip(1)).ToList()),
            pointValues.Length, new IArrowArray[] { badLat }.Concat(pointValues.Fields.Skip(1)), ArrowBuffer.Empty, 0);
        var badList = new ListArray(new ListType(new Field("item", badPoints.Data.DataType, true)), points.Length, points.ValueOffsetsBuffer, badPoints, ArrowBuffer.Empty, 0);
        Assert.Equal("Track column 7 'points' element: Position column 0 'lat': expected double, got int32.",
            Refusal(Replace(batch, 7, badList)));
    }

    [Fact]
    public void AListNestedBeyondTheCapRefusesNamingIt()
    {
        // Q6 and D-BIND-96: depth 4 is beyond C++'s cap of 3, so Hyper's accessor exists and refuses.
        using RecordBatch batch = Hyper.ToArrow(new[] { new Hyper { Id = 1 } });
        Assert.False(HyperAccessor.TryMake(batch, out var accessor, out var error));
        Assert.Null(accessor);
        Assert.Equal("Hyper column 1 'cells': a list nested 4 deep, beyond the accessor's cap of 3.", error);
    }

    [Fact]
    public void ReadingCollectionsAllocatesNothing()
    {
        // D-BIND-101: windows over columns cast once. A string or bytes element allocates by
        // design and is left out.
        using RecordBatch batch = Track.ToArrow(Enumerable.Range(0, 64).Select(Full).ToArray());
        var accessor = Make(batch);
        Read(accessor, 100);
        long before = GC.GetAllocatedBytesForCurrentThread();
        double total = Read(accessor, 100);
        long allocated = GC.GetAllocatedBytesForCurrentThread() - before;

        Assert.NotEqual(0, total);
        // The runtime allocates a few hundred bytes once while it tiers the loop up; an
        // allocating read would cost at least 24 bytes each, times about 400,000 reads here.
        Assert.True(allocated < 4096, $"{allocated} bytes");
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static double Read(TrackAccessor accessor, int rounds)
    {
        double sum = 0;
        for (int round = 0; round < rounds; round++)
        {
            for (int i = 0; i < accessor.Length; i++)
            {
                TrackView v = accessor.At(i);
                foreach (int x in v.Ints)
                {
                    sum += x;
                }
                foreach (PositionView? p in v.Points)
                {
                    sum += p?.Lat ?? 0;
                }
                // Values only: a string key is decoded, and allocated, on every read by design.
                for (int j = 0; j < v.Scores.Count; j++)
                {
                    sum += v.Scores.Value(j);
                }
                foreach (ListView<PositionView?> row in v.Grid)
                {
                    for (int j = 0; j < row.Count; j++)
                    {
                        sum += row[j]?.Lon ?? 0;
                    }
                }
                foreach (ListView<int> row in v.Matrix)
                {
                    foreach (int x in row)
                    {
                        sum += x;
                    }
                }
                sum += v.Places.Count + v.Qualities.Count + (int)v.Qualities[1];
            }
        }
        return sum;
    }
}
