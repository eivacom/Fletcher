// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// BIND-6b and 6c-2: ToArrow and FromArrow, where the generator emits them.
//
// Every message whose fields all convert gets both: scalars since 6b; lists, nested
// messages, maps and the lossless Timestamp and Duration since 6c-2. Each is held to a
// round trip, and every batch to the Arrow IPC stream format, which checks what
// RecordBatch's constructor does not: that each column has its field's layout.
using System;
using System.Collections.Generic;
using System.Linq;

using Apache.Arrow;
using Apache.Arrow.Types;

using Eiva.Fletcher.Model;

using Fletcher.Gen.Integration.ProtocDotnet;
using Fletcher.Gen.Integration.SharedTypes;

using Xunit;

using PbTimestamp = Fletcher.Gen.Google.Protobuf.Timestamp;

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

    // BIND-6c-2: lists, a nested message, a map, and a list of messages.
    private static readonly Player Busy = new()
    {
        Id = 7,
        Player_ = "named after its class",
        Class = -1,
        Clone_ = true,
        Schema_ = "s",
        Mode = Player_Mode._2D,
        Scores = { 3, -1, int.MaxValue },
        Stats = new Player_Stats { Goals = 12 },
        // Entry order and a duplicate key survive (D-BIND-75).
        Tags = { new("b", 2), new("a", 1), new("b", 3) },
        History = { new Player_Stats { Goals = 1 }, new Player_Stats { Goals = 2 } },
    };

    [Fact]
    public void PlayerRoundTripsListsAStructAMapAndAListOfStructs()
    {
        // The second row has every collection empty and the message absent, the third
        // only a list, so offsets that start mid-buffer are read back too.
        var rows = new List<Player> { Busy, new(), new() { Scores = { 5 }, History = { new() } } };

        using RecordBatch batch = Player.ToArrow(rows);
        StructArray array = AsStruct(batch);

        Assert.Equal(rows.Count, batch.Length);
        for (int i = 0; i < rows.Count; i++)
            Assert.Equal(Describe(rows[i]), Describe(Player.FromArrow(array, i)));
    }

    [Fact]
    public void AnAbsentMessageReadsBackAsNullNotAsADefaultInstance()
    {
        using RecordBatch batch = Player.ToArrow(new[] { new Player() });
        Assert.Null(Player.FromArrow(AsStruct(batch), 0).Stats);
        Assert.Equal(1, batch.Column(Player.Schema.GetFieldIndex("stats", StringComparer.Ordinal)).NullCount);
    }

    [Fact]
    public void TimedRoundTripsAndKeepsEveryNanosecond()
    {
        // D-BIND-26: the column is nanoseconds and so is the value, so all nine digits
        // come back; DateTime would have kept seven.
        var rows = new List<Timed>
        {
            new()
            {
                At = new Timestamp(1_700_000_000_123_456_789L, TimeUnit.Nanosecond),
                Took = new Duration(-1L, TimeUnit.Nanosecond),
                Boxed = 4,
                MaybeAt = new Timestamp(-5L, TimeUnit.Nanosecond),
            },
            new(),
        };

        using RecordBatch batch = Timed.ToArrow(rows);
        StructArray array = AsStruct(batch);

        Timed first = Timed.FromArrow(array, 0);
        Assert.Equal(rows[0].At, first.At);
        Assert.Equal(rows[0].Took, first.Took);
        Assert.Equal(4, first.Boxed);
        Assert.Equal(rows[0].MaybeAt, first.MaybeAt);

        Timed second = Timed.FromArrow(array, 1);
        Assert.Null(second.Boxed);
        Assert.Null(second.MaybeAt);
        Assert.Equal(new Timestamp(0L, TimeUnit.Nanosecond), second.At);
    }

    [Fact]
    public void AValueInACoarserUnitIsRecountedIntoTheColumnsUnitNotTruncated()
    {
        // The column is nanoseconds; a value written in seconds comes back as the same
        // instant counted in nanoseconds. Structural equality says they differ, the instant
        // says they do not.
        var written = new Timestamp(5L, TimeUnit.Second);
        using RecordBatch batch = Timed.ToArrow(new[] { new Timed { At = written } });

        Timestamp back = Timed.FromArrow(AsStruct(batch), 0).At;

        Assert.Equal(new Timestamp(5_000_000_000L, TimeUnit.Nanosecond), back);
        Assert.Equal(written.ToDateTimeOffset(), back.ToDateTimeOffset());
    }

    [Fact]
    public void EveryBatchSurvivesTheArrowIpcStreamFormat()
    {
        // RecordBatch's constructor does not check that a column matches its field. Writing
        // the batch as an Arrow IPC stream and reading it back does: a column built with the
        // wrong layout or type fails here, before the C++ codec ever sees it.
        Check(Reading.ToArrow(new[] { Full, Empty }), b => Reading.FromArrow(AsStruct(b), 0).Name, Full.Name);
        Check(Player.ToArrow(new[] { Busy, new Player() }), b => Describe(Player.FromArrow(AsStruct(b), 0)), Describe(Busy));
        Check(
            Timed.ToArrow(new[] { new Timed { At = new Timestamp(9L, TimeUnit.Nanosecond) } }),
            b => Timed.FromArrow(AsStruct(b), 0).At.Value,
            9L);
        Check(Route.ToArrow(new[] { new Route(), Trip }), b => Describe(Route.FromArrow(AsStruct(b), 1)), Describe(Trip));

        static void Check<T>(RecordBatch batch, Func<RecordBatch, T> read, T expected)
        {
            using (batch)
            {
                using var stream = new System.IO.MemoryStream();
                using (var writer = new Apache.Arrow.Ipc.ArrowStreamWriter(stream, batch.Schema, leaveOpen: true))
                {
                    writer.WriteRecordBatch(batch);
                    writer.WriteEnd();
                }

                stream.Position = 0;
                using var reader = new Apache.Arrow.Ipc.ArrowStreamReader(stream);
                using RecordBatch back = reader.ReadNextRecordBatch();
                Assert.Equal(batch.Length, back.Length);
                Assert.Equal(expected, read(back));
            }
        }
    }

    // BIND-6e: every place a type from another file and assembly can sit, at values a
    // default would not reproduce. Pos, Fix and Quality come from shared.proto and the
    // marks' element from timestamp.proto, all compiled into ProtocDotnetShared.
    private static readonly Route Trip = new()
    {
        Start = new Pos { Lat = 55.67, Lon = 12.57, Label = "start" },
        Legs = { new Pos { Lat = -90, Lon = 180 }, new Pos { Lat = double.Epsilon, Label = "" } },
        Stops = { new("b", new Pos { Lon = 1 }), new("a", new Pos { Lat = 2 }), new("b", new Pos { Label = "dup" }) },
        Quality = Quality.Poor,
        Checks = { Quality.Good, Quality.Unspecified, Quality.Poor },
        Fix = new Fix { Pos = null, Trail = { new Pos { Lat = 1, Lon = 2 } }, Quality = Quality.Good },
        Marks = { new PbTimestamp { Seconds = long.MinValue, Nanos = -1 }, new PbTimestamp { Seconds = 1_700_000_000, Nanos = 999_999_999 } },
    };

    [Fact]
    public void RouteRoundTripsTypesFromAnotherFileAndAssembly()
    {
        // D-BIND-80: Route's columns for Pos, Fix and Timestamp are built by those classes'
        // public ToArrow, since their internal ToArrowColumns is out of reach from this
        // assembly. An empty Route between two full ones shows no row bleeds into the next.
        Route[] sent = { Trip, new Route(), Trip };
        using RecordBatch batch = Route.ToArrow(sent);
        StructArray rows = AsStruct(batch);

        Assert.Equal(sent.Select(Describe), Enumerable.Range(0, sent.Length).Select(i => Describe(Route.FromArrow(rows, i))));
        Route empty = Route.FromArrow(rows, 1);
        Assert.Null(empty.Start);
        Assert.Null(empty.Fix);
        Assert.Empty(empty.Legs);
        Assert.Empty(empty.Marks);
    }

    private static string Describe(Route r) => string.Join(" | ",
        Describe(r.Start),
        "[" + string.Join(",", r.Legs.Select(p => Describe(p))) + "]",
        "{" + string.Join(",", r.Stops.Select(s => s.Key + "=" + Describe(s.Value))) + "}",
        r.Quality,
        "[" + string.Join(",", r.Checks) + "]",
        r.Fix is null ? "<null>" : Describe(r.Fix.Pos) + "/[" + string.Join(",", r.Fix.Trail.Select(p => Describe(p))) + "]/" + r.Fix.Quality,
        "[" + string.Join(",", r.Marks.Select(m => m.Seconds + ":" + m.Nanos)) + "]");

    private static string Describe(Pos? p) => p is null
        ? "<null>"
        : string.Join(";", p.Lat.ToString("R", System.Globalization.CultureInfo.InvariantCulture),
            p.Lon.ToString("R", System.Globalization.CultureInfo.InvariantCulture), p.Label ?? "<null>");

    private static StructArray AsStruct(RecordBatch batch) =>
        new(new StructType(batch.Schema.FieldsList), batch.Length, batch.Arrays, ArrowBuffer.Empty, 0);

    private static string Describe(Player p) => string.Join(" | ",
        p.Id, p.Player_, p.Class, p.Clone_, p.Schema_, p.Mode,
        "[" + string.Join(",", p.Scores) + "]",
        p.Stats is null ? "<null>" : p.Stats.Goals.ToString(System.Globalization.CultureInfo.InvariantCulture),
        "{" + string.Join(",", p.Tags.Select(t => t.Key + "=" + t.Value)) + "}",
        "[" + string.Join(",", p.History.Select(h => h.Goals)) + "]");

    private static string Describe(Reading r) => string.Join(" | ",
        r.Flag, r.Small, r.Big, r.Count, r.Huge, r.Ratio, r.Value, r.Name,
        r.Note ?? "<null>", Convert.ToHexString(r.Blob), r.Extra is null ? "<null>" : Convert.ToHexString(r.Extra),
        r.Color, r.Tint?.ToString() ?? "<null>", r.Maybe?.ToString() ?? "<null>",
        r.MaybeValue?.ToString() ?? "<null>", r.MaybeFlag?.ToString() ?? "<null>");
}
