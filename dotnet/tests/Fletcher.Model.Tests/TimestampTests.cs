// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// Timestamp keeps the Arrow value exactly and loses precision only in a call you
// make (D-BIND-26). Every expectation below is written out as a literal, so a case
// reads as the arithmetic it claims; none recomputes the answer with the code under
// test. The cases that matter most are the two directions of the nanosecond tick
// division: they are not mirror images, and a floor that rounds toward zero would
// pass every positive case.
using System;

using Apache.Arrow.Types;

using Xunit;

namespace Eiva.Fletcher.Model.Tests;

public class TimestampTests
{
    private static readonly DateTimeOffset Epoch = new(1970, 1, 1, 0, 0, 0, TimeSpan.Zero);

    [Fact]
    public void DefaultIsTheEpochInSeconds()
    {
        Assert.Equal(Epoch, default(Timestamp).ToDateTimeOffset());
        Assert.Equal(TimeUnit.Second, default(Timestamp).Unit);
    }

    [Theory]
    [InlineData(1L, TimeUnit.Second, 1L * 10_000_000)]
    [InlineData(1L, TimeUnit.Millisecond, 10_000L)]
    [InlineData(1L, TimeUnit.Microsecond, 10L)]
    [InlineData(100L, TimeUnit.Nanosecond, 1L)]
    [InlineData(-1L, TimeUnit.Second, -10_000_000L)]
    [InlineData(-3L, TimeUnit.Microsecond, -30L)]
    public void ConvertsToTheExpectedNumberOfTicksAfterTheEpoch(long value, TimeUnit unit, long ticks)
    {
        Assert.Equal(Epoch.AddTicks(ticks), new Timestamp(value, unit).ToDateTimeOffset());
    }

    [Theory]
    [InlineData(99L, 0L)]
    [InlineData(199L, 1L)]
    [InlineData(-1L, -1L)]
    [InlineData(-100L, -1L)]
    [InlineData(-101L, -2L)]
    public void NanosecondsFloorToATickTowardNegativeInfinity(long nanoseconds, long ticks)
    {
        Assert.Equal(Epoch.AddTicks(ticks), new Timestamp(nanoseconds, TimeUnit.Nanosecond).ToDateTimeOffset());
    }

    [Fact]
    public void ANanosecondValueThatIsNotATickMultipleDoesNotSurviveTheRoundTrip()
    {
        // The loss D-BIND-26 exists to keep OUT of the model: this is the one place
        // it is allowed, and it must be exactly the two digits, no more.
        var original = new Timestamp(1_700_000_000_123_456_789L, TimeUnit.Nanosecond);

        var back = Timestamp.FromDateTimeOffset(original.ToDateTimeOffset(), TimeUnit.Nanosecond);

        Assert.Equal(1_700_000_000_123_456_700L, back.Value);
        Assert.NotEqual(original, back);
    }

    [Fact]
    public void ANanosecondValueThatIsATickMultipleSurvivesTheRoundTrip()
    {
        var original = new Timestamp(1_700_000_000_123_456_700L, TimeUnit.Nanosecond, "Europe/Copenhagen");

        var back = Timestamp.FromDateTimeOffset(original.ToDateTimeOffset(), TimeUnit.Nanosecond, "Europe/Copenhagen");

        Assert.Equal(original, back);
    }

    [Theory]
    [InlineData(TimeUnit.Second, 1_700_000_000L)]
    [InlineData(TimeUnit.Millisecond, 1_700_000_000_123L)]
    [InlineData(TimeUnit.Microsecond, 1_700_000_000_123_456L)]
    public void CoarserUnitsRoundTripExactly(TimeUnit unit, long value)
    {
        var original = new Timestamp(value, unit);

        Assert.Equal(original, Timestamp.FromDateTimeOffset(original.ToDateTimeOffset(), unit));
    }

    [Fact]
    public void FromDateTimeOffsetFloorsToACoarserUnit()
    {
        // One tick before the epoch is not second zero: a floor puts it in second -1.
        var oneTickBefore = Epoch.AddTicks(-1);

        Assert.Equal(-1L, Timestamp.FromDateTimeOffset(oneTickBefore, TimeUnit.Second).Value);
        Assert.Equal(-1L, Timestamp.FromDateTimeOffset(oneTickBefore, TimeUnit.Millisecond).Value);
        Assert.Equal(-1L, Timestamp.FromDateTimeOffset(oneTickBefore, TimeUnit.Microsecond).Value);
        Assert.Equal(-100L, Timestamp.FromDateTimeOffset(oneTickBefore, TimeUnit.Nanosecond).Value);
    }

    [Fact]
    public void FromDateTimeOffsetKeepsTheInstantNotTheOffset()
    {
        var plusTwo = new DateTimeOffset(1970, 1, 1, 2, 0, 0, TimeSpan.FromHours(2));

        Assert.Equal(0L, Timestamp.FromDateTimeOffset(plusTwo, TimeUnit.Second).Value);
    }

    [Fact]
    public void TheTimeZoneIsMetadataAndDoesNotMoveTheInstant()
    {
        var utc = new Timestamp(1_000L, TimeUnit.Second);
        var zoned = new Timestamp(1_000L, TimeUnit.Second, "Asia/Tokyo");

        Assert.Equal(utc.ToDateTimeOffset(), zoned.ToDateTimeOffset());
        Assert.Equal(TimeSpan.Zero, zoned.ToDateTimeOffset().Offset);
        Assert.Equal("Asia/Tokyo", zoned.TimeZone);
    }

    [Fact]
    public void EqualityIsStructuralSoOneSecondIsNotAThousandMilliseconds()
    {
        var seconds = new Timestamp(1L, TimeUnit.Second);
        var millis = new Timestamp(1_000L, TimeUnit.Millisecond);

        Assert.NotEqual(seconds, millis);
        Assert.Equal(seconds.ToDateTimeOffset(), millis.ToDateTimeOffset());
        Assert.NotEqual(new Timestamp(1L, TimeUnit.Second, "UTC"), seconds);
    }

    [Theory]
    [InlineData(long.MaxValue, TimeUnit.Second)]
    [InlineData(long.MinValue, TimeUnit.Second)]
    [InlineData(300_000_000_000L, TimeUnit.Second)]
    [InlineData(-62_135_596_801L, TimeUnit.Second)]
    public void AnInstantOutsideDateTimeOffsetThrowsInsteadOfWrapping(long value, TimeUnit unit)
    {
        Assert.Throws<OverflowException>(() => new Timestamp(value, unit).ToDateTimeOffset());
    }

    [Fact]
    public void TheLargestNanosecondTimestampStillConverts()
    {
        // 2262-04-11: the far end of a 64-bit nanosecond count, well inside DateTimeOffset.
        var date = new Timestamp(long.MaxValue, TimeUnit.Nanosecond).ToDateTimeOffset();

        Assert.Equal(2262, date.Year);
    }

    [Fact]
    public void ADateTimeOffsetTooFarForANanosecondCountThrows()
    {
        Assert.Throws<OverflowException>(() => Timestamp.FromDateTimeOffset(DateTimeOffset.MaxValue, TimeUnit.Nanosecond));
        Assert.Throws<OverflowException>(() => Timestamp.FromDateTimeOffset(DateTimeOffset.MinValue, TimeUnit.Nanosecond));
    }

    [Fact]
    public void AnUnknownUnitIsRefused()
    {
        Assert.Throws<ArgumentOutOfRangeException>(() => new Timestamp(1L, (TimeUnit)99).ToDateTimeOffset());
    }

    [Theory]
    [InlineData(3L, TimeUnit.Second, TimeUnit.Nanosecond, 3_000_000_000L)]
    [InlineData(3L, TimeUnit.Millisecond, TimeUnit.Microsecond, 3_000L)]
    [InlineData(-7L, TimeUnit.Microsecond, TimeUnit.Nanosecond, -7_000L)]
    [InlineData(5_000_000_000L, TimeUnit.Nanosecond, TimeUnit.Second, 5L)]
    [InlineData(-4_000L, TimeUnit.Millisecond, TimeUnit.Second, -4L)]
    [InlineData(42L, TimeUnit.Millisecond, TimeUnit.Millisecond, 42L)]
    public void WithUnitRecountsExactly(long value, TimeUnit from, TimeUnit to, long expected)
    {
        var recounted = new Timestamp(value, from, "Europe/Copenhagen").WithUnit(to);

        Assert.Equal(new Timestamp(expected, to, "Europe/Copenhagen"), recounted);
    }

    [Theory]
    [InlineData(1_500L, TimeUnit.Millisecond, TimeUnit.Second)]
    [InlineData(-1L, TimeUnit.Nanosecond, TimeUnit.Microsecond)]
    [InlineData(123_456_789L, TimeUnit.Nanosecond, TimeUnit.Millisecond)]
    public void WithUnitRefusesToDropARemainder(long value, TimeUnit from, TimeUnit to)
    {
        // The step that puts a value into a column of fixed unit must not be where a digit
        // is lost: D-BIND-26 allows that only in a conversion the caller names.
        Assert.Throws<ArgumentException>(() => new Timestamp(value, from).WithUnit(to));
    }

    [Fact]
    public void WithUnitRefusesToOverflowAFinerUnit()
    {
        Assert.Throws<OverflowException>(() => new Timestamp(long.MaxValue / 10, TimeUnit.Second).WithUnit(TimeUnit.Nanosecond));
    }
}
