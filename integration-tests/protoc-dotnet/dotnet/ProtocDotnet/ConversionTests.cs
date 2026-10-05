// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// BIND-6b: ToArrow and FromArrow, where the generator emits them.
//
// A message whose fields are all mapped scalars gets both; any other gets its Schema
// and waits for BIND-6c, because a ToArrow that skipped a field would build a batch its
// own Schema disagrees with. Both halves are asserted: Reading round-trips, and Player
// has no conversion yet. When 6c lands, the second half is the case that must change.
using System;
using System.Collections.Generic;

using Apache.Arrow;
using Apache.Arrow.Types;

using Fletcher.Gen.Integration.ProtocDotnet;

using Xunit;

namespace Eiva.Fletcher.ProtocDotnet;

public sealed class ConversionTests
{
    private static readonly Reading Full = new()
    {
        Flag = true,
        Small = int.MinValue,
        Big = long.MaxValue,
        Count = uint.MaxValue,
        Huge = ulong.MaxValue,
        Ratio = 1.5f,
        Value = Math.PI,
        Name = "héllo ✓",
        Note = "note",
        Blob = new byte[] { 0, 1, 255 },
        Extra = new byte[] { 9 },
        Color = Color.DarkBlue,
        Tint = Color.Red,
        Maybe = 42,
        MaybeValue = -0.25,
        MaybeFlag = false,
    };

    // Every optional field null, every required one at its proto default.
    private static readonly Reading Empty = new();

    [Fact]
    public void ReadingRoundTripsThroughToArrowAndFromArrow()
    {
        var rows = new List<Reading> { Full, Empty, new() { Name = "", Extra = System.Array.Empty<byte>(), Tint = Color.Unspecified } };

        using RecordBatch batch = Reading.ToArrow(rows);
        StructArray array = AsStruct(batch);

        Assert.Equal(rows.Count, batch.Length);
        for (int i = 0; i < rows.Count; i++)
            Assert.Equal(Describe(rows[i]), Describe(Reading.FromArrow(array, i)));
    }

    [Fact]
    public void ToArrowCarriesTheGeneratedSchema()
    {
        using RecordBatch batch = Reading.ToArrow(new[] { Full });
        Assert.Same(Reading.Schema, batch.Schema);
    }

    [Fact]
    public void NullsArriveAsNullsNotAsDefaults()
    {
        using RecordBatch batch = Reading.ToArrow(new[] { Empty });
        StructArray array = AsStruct(batch);

        Reading back = Reading.FromArrow(array, 0);
        Assert.Null(back.Note);
        Assert.Null(back.Extra);
        Assert.Null(back.Tint);
        Assert.Null(back.Maybe);
        Assert.Null(back.MaybeValue);
        Assert.Null(back.MaybeFlag);
        // A required field is never null, even read from a defaulted row.
        Assert.Equal("", back.Name);
        Assert.Empty(back.Blob);
    }

    [Fact]
    public void AnEmptySequenceIsAnEmptyBatch()
    {
        using RecordBatch batch = Reading.ToArrow(System.Array.Empty<Reading>());
        Assert.Equal(0, batch.Length);
        Assert.Equal(Reading.Schema.FieldsList.Count, batch.ColumnCount);
    }

    [Fact]
    public void NullArgumentsAreRefused()
    {
        Assert.Throws<ArgumentNullException>(() => Reading.ToArrow(null!));
        Assert.Throws<ArgumentNullException>(() => Reading.FromArrow(null!, 0));
    }

    [Fact]
    public void AMessageWithUnmappedFieldsHasSchemaButNoConversionYet()
    {
        // BIND-6c: Player holds a list, a struct and a map. This is the case that has to
        // change when 6c generates their conversion.
        Assert.NotNull(Player.Schema);
        Assert.Null(typeof(Player).GetMethod("ToArrow"));
        Assert.Null(typeof(Player).GetMethod("FromArrow"));
        // A nested scalar-only message converts already.
        Assert.NotNull(typeof(Player_Stats).GetMethod("ToArrow"));
    }

    private static StructArray AsStruct(RecordBatch batch) =>
        new(new StructType(batch.Schema.FieldsList), batch.Length, batch.Arrays, ArrowBuffer.Empty, 0);

    private static string Describe(Reading r) => string.Join(" | ",
        r.Flag, r.Small, r.Big, r.Count, r.Huge, r.Ratio, r.Value, r.Name,
        r.Note ?? "<null>", Convert.ToHexString(r.Blob), r.Extra is null ? "<null>" : Convert.ToHexString(r.Extra),
        r.Color, r.Tint?.ToString() ?? "<null>", r.Maybe?.ToString() ?? "<null>",
        r.MaybeValue?.ToString() ?? "<null>", r.MaybeFlag?.ToString() ?? "<null>");
}
