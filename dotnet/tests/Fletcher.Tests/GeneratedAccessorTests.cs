// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// BIND-7a: the GENERATED accessor and view, reading Arrow batches.
//
// The classes under test are not written here: they are the plugin's output for
// protoc/tests/golden/csharp_accessor.proto with `csharp,csharp_model_only,csharp_accessor`,
// committed as goldens and compiled into this assembly (the plugin's own suite fails if they
// drift from its output by a byte). The batches are built by the generated model's ToArrow,
// so every case reads what a C# producer writes, and the refusals are built by replacing one
// column of such a batch.
using System;
using System.Collections.Generic;
using System.Linq;

using Apache.Arrow;
using Apache.Arrow.Types;

using Eiva.Fletcher.Model;

using Fletcher.Gen.Golden.Accessor;

using Xunit;

namespace Eiva.Fletcher.Tests;

public sealed class GeneratedAccessorTests
{
    // Every column at a value a default would not reproduce, every optional set.
    private static Sample Full(int id) => new()
    {
        Id = id,
        Ticks = long.MinValue + id,
        Flags = uint.MaxValue - (uint)id,
        Serial = ulong.MaxValue - (ulong)id,
        Gain = 1.5f * id,
        Value = -0.25 * id,
        Valid = true,
        Name = "héllo ✓ " + id,
        Payload = new byte[] { 0, 1, (byte)id, 255 },
        Quality = Quality.Poor,
        At = new Timestamp(1_700_000_000_123_456_789L + id, TimeUnit.Nanosecond),
        Span = new Duration(-42L * id, TimeUnit.Nanosecond),
        Position = new Position { Lat = 55.5 + id, Lon = -10.25, Depth = 3.5f },
        MaybeId = -id,
        MaybeName = "maybe " + id,
        MaybePayload = new byte[] { 9, (byte)id },
        MaybeQuality = Quality.Good,
        MaybeValid = false,
        MaybePosition = new Position { Lat = id, Lon = id + 0.5 },
        Note = new Sample_Note { Text = "note " + id },
    };

    // Every optional null, the rest at its proto default.
    private static Sample Sparse(int id) => new() { Id = id };

    private static SampleAccessor Make(RecordBatch batch)
    {
        Assert.True(SampleAccessor.TryMake(batch, out var accessor, out var error), error);
        return accessor;
    }

    private static string Refusal(RecordBatch batch)
    {
        Assert.False(SampleAccessor.TryMake(batch, out var accessor, out var error));
        Assert.Null(accessor);
        Assert.NotNull(error);
        return error;
    }

    // The batch with column `index` replaced by `column`, under the same schema.
    private static RecordBatch Replace(RecordBatch batch, int index, IArrowArray column) =>
        new(batch.Schema, batch.Arrays.Select((array, i) => i == index ? column : array), batch.Length);

    // The batch's columns as one StructArray, optionally with a validity bitmap.
    private static StructArray AsStruct(RecordBatch batch, ArrowBuffer validity = default, int nullCount = 0) =>
        new(new StructType(batch.Schema.FieldsList), batch.Length, batch.Arrays,
            validity.IsEmpty ? ArrowBuffer.Empty : validity, nullCount);

    private static void AssertRow(Sample expected, SampleView view)
    {
        Assert.Equal(expected.Id, view.Id);
        Assert.Equal(expected.Ticks, view.Ticks);
        Assert.Equal(expected.Flags, view.Flags);
        Assert.Equal(expected.Serial, view.Serial);
        Assert.Equal(expected.Gain, view.Gain);
        Assert.Equal(expected.Value, view.Value);
        Assert.Equal(expected.Valid, view.Valid);
        Assert.Equal(expected.Name, view.Name);
        Assert.Equal(expected.Payload, view.Payload);
        Assert.Equal(expected.Quality, view.Quality);
        Assert.Equal(expected.At, view.At);
        Assert.Equal(expected.Span, view.Span);
        Assert.Equal(expected.Position.Lat, view.Position.Lat);
        Assert.Equal(expected.Position.Lon, view.Position.Lon);
        Assert.Equal(expected.Position.Depth, view.Position.Depth);
        Assert.Equal(expected.MaybeId, view.MaybeId);
        Assert.Equal(expected.MaybeName, view.MaybeName);
        Assert.Equal(expected.MaybePayload, view.MaybePayload);
        Assert.Equal(expected.MaybeQuality, view.MaybeQuality);
        Assert.Equal(expected.MaybeValid, view.MaybeValid);
        Assert.Equal(expected.MaybePosition is null, view.MaybePosition is null);
        if (expected.MaybePosition is not null)
        {
            Assert.Equal(expected.MaybePosition.Lat, view.MaybePosition!.Value.Lat);
            Assert.Equal(expected.MaybePosition.Lon, view.MaybePosition.Value.Lon);
            Assert.Equal(expected.MaybePosition.Depth, view.MaybePosition.Value.Depth);
        }
        Assert.Equal(expected.Note.Text, view.Note.Text);
    }

    [Fact]
    public void EveryColumnReadsBackAsTheModelWroteIt()
    {
        Sample[] rows = { Full(1), Sparse(2), Full(3) };
        using RecordBatch batch = Sample.ToArrow(rows);
        var accessor = Make(batch);

        Assert.Equal(3, accessor.Length);
        for (var i = 0; i < rows.Length; i++)
        {
            Assert.False(accessor.IsNull(i));
            AssertRow(rows[i], accessor.At(i));
        }
    }

    [Fact]
    public void ANullOptionalReadsNullAndAPlainFieldItsDefault()
    {
        using RecordBatch batch = Sample.ToArrow(new[] { Sparse(7) });
        SampleView view = Make(batch).At(0);

        Assert.Null(view.MaybeId);
        Assert.Null(view.MaybeName);
        Assert.Null(view.MaybePayload);
        Assert.Null(view.MaybeQuality);
        Assert.Null(view.MaybeValid);
        Assert.Null(view.MaybePosition);
        Assert.Null(view.Position.Depth);
        Assert.Equal("", view.Name);
        Assert.Empty(view.Payload);
        Assert.Equal(Quality.Unspecified, view.Quality);
        Assert.Equal("", view.Note.Text);
    }

    [Fact]
    public void AStructArrayAtANonZeroOffsetReadsItsOwnRows()
    {
        // D-BIND-10's forcing test: Apache.Arrow's StructArray.Fields returns each child already
        // windowed to the struct's slice, so row 0 of a slice starting at 1 is source row 1,
        // nested structs included, and TryMake must not slice again.
        Sample[] rows = { Full(1), Full(2), Sparse(3), Full(4) };
        using RecordBatch batch = Sample.ToArrow(rows);
        var whole = AsStruct(batch);
        var slice = (StructArray)whole.Slice(1, 2);
        Assert.Equal(1, slice.Offset);

        Assert.True(SampleAccessor.TryMake(slice, out var accessor, out var error), error);
        Assert.Equal(2, accessor.Length);
        AssertRow(rows[1], accessor.At(0));
        AssertRow(rows[2], accessor.At(1));
    }

    [Fact]
    public void IsNullIsTheStructsValidityAndAlwaysFalseForABatch()
    {
        Sample[] rows = { Full(1), Full(2), Full(3) };
        using RecordBatch batch = Sample.ToArrow(rows);
        var validity = new ArrowBuffer.BitmapBuilder().Append(true).Append(false).Append(true).Build();
        var array = AsStruct(batch, validity, nullCount: 1);

        Assert.True(SampleAccessor.TryMake(array, out var fromStruct, out var error), error);
        Assert.Equal(new[] { false, true, false }, Enumerable.Range(0, 3).Select(fromStruct.IsNull));
        var sliced = (StructArray)array.Slice(1, 2);
        Assert.True(SampleAccessor.TryMake(sliced, out var fromSlice, out error), error);
        Assert.Equal(new[] { true, false }, Enumerable.Range(0, 2).Select(fromSlice.IsNull));

        var fromBatch = Make(batch);
        Assert.All(Enumerable.Range(0, 3), i => Assert.False(fromBatch.IsNull(i)));
    }

    [Fact]
    public void ARowOutsideTheBatchThrows()
    {
        using RecordBatch batch = Sample.ToArrow(new[] { Full(1), Full(2) });
        var accessor = Make(batch);
        foreach (var row in new[] { -1, 2, int.MaxValue })
        {
            Assert.Throws<ArgumentOutOfRangeException>(() => accessor.At(row));
            Assert.Throws<ArgumentOutOfRangeException>(() => accessor.IsNull(row));
        }
    }

    [Fact]
    public void NamesAndTheNullableFlagAreTolerated()
    {
        // D-RBA-4: positional and type-only. Renamed columns and a non-nullable column marked
        // nullable change nothing a getter reads.
        using RecordBatch batch = Sample.ToArrow(new[] { Full(1) });
        var renamed = new Schema(
            batch.Schema.FieldsList.Select((f, i) => new Field("c" + i, f.DataType, nullable: true, f.Metadata)),
            batch.Schema.Metadata);
        using var other = new RecordBatch(renamed, batch.Arrays, batch.Length);

        AssertRow(Full(1), Make(other).At(0));
    }

    [Fact]
    public void EveryRefusalSaysWhatAndWhere()
    {
        using RecordBatch batch = Sample.ToArrow(new[] { Full(1), Full(2) });
        int columns = Sample.Schema.FieldsList.Count;

        Assert.Equal("Sample: the batch is null.", Refusal(null!));

        var fewer = new Schema(batch.Schema.FieldsList.Take(columns - 1), batch.Schema.Metadata);
        using (var shorter = new RecordBatch(fewer, batch.Arrays.Take(columns - 1), batch.Length))
        {
            Assert.Equal($"Sample: expected {columns} columns, got {columns - 1}.", Refusal(shorter));
        }

        var longs = new Int64Array.Builder().Append(1).Append(2).Build();
        Assert.Equal("Sample column 0 'id': expected int32, got int64.", Refusal(Replace(batch, 0, longs)));

        var nulls = new Int32Array.Builder().Append(1).AppendNull().Build();
        Assert.Equal("Sample column 0 'id': non-nullable, found 1 nulls.", Refusal(Replace(batch, 0, nulls)));

        var one = new Int64Array.Builder().Append(1).Build();
        Assert.Equal("Sample column 1: 1 rows, expected 2.", Refusal(Replace(batch, 1, one)));

        // StringArray derives from BinaryArray: the type id, not the class, tells them apart.
        var text = new StringArray.Builder().Append("a").Append("b").Build();
        Assert.Equal("Sample column 8 'payload': expected binary, got utf8.", Refusal(Replace(batch, 8, text)));

        var millis = new TimestampArray.Builder(new TimestampType(TimeUnit.Millisecond, (string?)null)).Append(DateTimeOffset.UnixEpoch)
            .Append(DateTimeOffset.UnixEpoch).Build();
        Assert.Equal(
            "Sample column 10 'at': expected timestamp[Nanosecond, no zone], got timestamp[Millisecond, no zone].",
            Refusal(Replace(batch, 10, millis)));

        var zoned = new TimestampArray.Builder(new TimestampType(TimeUnit.Nanosecond, "UTC")).Append(DateTimeOffset.UnixEpoch)
            .Append(DateTimeOffset.UnixEpoch).Build();
        Assert.Equal(
            "Sample column 10 'at': expected timestamp[Nanosecond, no zone], got timestamp[Nanosecond, UTC].",
            Refusal(Replace(batch, 10, zoned)));

        // A struct column is validated by its own accessor, whose reason is passed on whole.
        var position = (StructArray)batch.Column(12);
        var badLat = new Int32Array.Builder().Append(1).Append(2).Build();
        var wrongPosition = new StructArray(
            new StructType(new[] { new Field("lat", Int32Type.Default, false) }.Concat(((StructType)position.Data.DataType).Fields.Skip(1)).ToList()),
            2, new IArrowArray[] { badLat }.Concat(position.Fields.Skip(1)), ArrowBuffer.Empty, 0);
        Assert.Equal(
            "Sample column 12 'position': Position column 0 'lat': expected double, got int32.",
            Refusal(Replace(batch, 12, wrongPosition)));

        Assert.Equal("Sample column 12 'position': expected struct, got int32.", Refusal(Replace(batch, 12, badLat)));
    }

    [Fact]
    public void MetadataIsTheBatchsAndEmptyNeverNullWhenAbsent()
    {
        // D-BIND-95: SchemaMetadata, FieldMetadata by position and by live name, Metadata(key).
        using RecordBatch model = Sample.ToArrow(new[] { Full(1) });
        var schema = new Schema(model.Schema.FieldsList, new Dictionary<string, string> { ["unit"] = "m" });
        using var batch = new RecordBatch(schema, model.Arrays, model.Length);
        var accessor = Make(batch);

        Assert.Equal("m", accessor.SchemaMetadata["unit"]);
        Assert.Equal("m", accessor.Metadata("unit"));
        Assert.Null(accessor.Metadata("absent"));
        Assert.Equal("1", accessor.FieldMetadata(0)["field_number"]);
        Assert.Equal("13", accessor.FieldMetadata("position")["field_number"]);
        Assert.Empty(accessor.FieldMetadata("absent"));
        Assert.Throws<ArgumentOutOfRangeException>(() => accessor.FieldMetadata(-1));
        Assert.Throws<ArgumentOutOfRangeException>(() => accessor.FieldMetadata(Sample.Schema.FieldsList.Count));

        // A StructArray has no schema, so no schema metadata: empty, not null.
        Assert.True(SampleAccessor.TryMake(AsStruct(batch), out var fromStruct, out var error), error);
        Assert.NotNull(fromStruct.SchemaMetadata);
        Assert.Empty(fromStruct.SchemaMetadata);
        Assert.Null(fromStruct.Metadata("unit"));
        Assert.Equal("1", fromStruct.FieldMetadata(0)["field_number"]);

        var bare = new Schema(model.Schema.FieldsList.Select(f => new Field(f.Name, f.DataType, f.IsNullable)), null);
        using var plain = new RecordBatch(bare, model.Arrays, model.Length);
        var noMetadata = Make(plain);
        Assert.NotNull(noMetadata.FieldMetadata(0));
        Assert.Empty(noMetadata.FieldMetadata(0));
        Assert.Empty(noMetadata.SchemaMetadata);
    }

    [Fact]
    public void ReadingThroughTheViewAllocatesNothing()
    {
        // D-BIND-97 and D-BIND-98: the view is (accessor, row) over columns cast once, so a read
        // of a value, an optional, an enum, a timestamp or a nested struct allocates nothing.
        // A string or bytes property allocates by design and is left out.
        using RecordBatch batch = Sample.ToArrow(Enumerable.Range(0, 64).Select(i => i % 2 == 0 ? Full(i) : Sparse(i)).ToArray());
        var accessor = Make(batch);

        double Sum()
        {
            double sum = 0;
            for (var i = 0; i < accessor.Length; i++)
            {
                SampleView v = accessor.At(i);
                sum += v.Id + v.Ticks + v.Value + (v.MaybeId ?? 0) + (int)v.Quality + (v.MaybeQuality is null ? 1 : 0)
                       + v.At.Value + v.Span.Value + v.Position.Lat + (v.Position.Depth ?? 0)
                       + (v.MaybePosition?.Lon ?? 0) + (v.Valid ? 1 : 0);
            }
            return sum;
        }

        for (var warm = 0; warm < 100; warm++)
        {
            Sum();
        }
        long before = GC.GetAllocatedBytesForCurrentThread();
        double total = 0;
        for (var round = 0; round < 1000; round++)
        {
            total += Sum();
        }
        long allocated = GC.GetAllocatedBytesForCurrentThread() - before;

        Assert.NotEqual(0, total);
        Assert.Equal(0, allocated);
    }
}
