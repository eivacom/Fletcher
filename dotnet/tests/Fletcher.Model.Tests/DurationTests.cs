// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// Duration is Timestamp's reasoning without an epoch or a zone: the Arrow value is
// kept exactly, and the only loss is the nanosecond-to-tick floor in a call you
// make. Expectations are literals; see TimestampTests for why.
using System;

using Apache.Arrow.Types;

using Xunit;

namespace Eiva.Fletcher.Model.Tests;

public class DurationTests
{
    [Theory]
    [InlineData(1L, TimeUnit.Second, 10_000_000L)]
    [InlineData(1L, TimeUnit.Millisecond, 10_000L)]
    [InlineData(1L, TimeUnit.Microsecond, 10L)]
    [InlineData(100L, TimeUnit.Nanosecond, 1L)]
    [InlineData(-2L, TimeUnit.Second, -20_000_000L)]
    public void ConvertsToTheExpectedNumberOfTicks(long value, TimeUnit unit, long ticks)
    {
        Assert.Equal(new TimeSpan(ticks), new Duration(value, unit).ToTimeSpan());
    }

    [Theory]
    [InlineData(99L, 0L)]
    [InlineData(-1L, -1L)]
    [InlineData(-100L, -1L)]
    [InlineData(-101L, -2L)]
    public void NanosecondsFloorToATickTowardNegativeInfinity(long nanoseconds, long ticks)
    {
        Assert.Equal(new TimeSpan(ticks), new Duration(nanoseconds, TimeUnit.Nanosecond).ToTimeSpan());
    }

    [Fact]
    public void ANanosecondValueThatIsNotATickMultipleLosesExactlyTheLastTwoDigits()
    {
        var back = Duration.FromTimeSpan(new Duration(1_234_567_891L, TimeUnit.Nanosecond).ToTimeSpan(), TimeUnit.Nanosecond);

        Assert.Equal(1_234_567_800L, back.Value);
    }

    [Theory]
    [InlineData(TimeUnit.Second, 90L)]
    [InlineData(TimeUnit.Millisecond, 1_500L)]
    [InlineData(TimeUnit.Microsecond, -2_500_001L)]
    [InlineData(TimeUnit.Nanosecond, 3_000_000_100L)]
    public void ValuesAtTickPrecisionRoundTripExactly(TimeUnit unit, long value)
    {
        var original = new Duration(value, unit);

        Assert.Equal(original, Duration.FromTimeSpan(original.ToTimeSpan(), unit));
    }

    [Fact]
    public void FromTimeSpanFloorsToACoarserUnit()
    {
        var oneTickBackwards = new TimeSpan(-1);

        Assert.Equal(-1L, Duration.FromTimeSpan(oneTickBackwards, TimeUnit.Second).Value);
        Assert.Equal(-100L, Duration.FromTimeSpan(oneTickBackwards, TimeUnit.Nanosecond).Value);
    }

    [Fact]
    public void EqualityIsStructuralSoOneSecondIsNotAThousandMilliseconds()
    {
        var seconds = new Duration(1L, TimeUnit.Second);
        var millis = new Duration(1_000L, TimeUnit.Millisecond);

        Assert.NotEqual(seconds, millis);
        Assert.Equal(seconds.ToTimeSpan(), millis.ToTimeSpan());
    }

    [Fact]
    public void ASpanTooLargeForATimeSpanThrowsInsteadOfWrapping()
    {
        Assert.Throws<OverflowException>(() => new Duration(long.MaxValue, TimeUnit.Second).ToTimeSpan());
        Assert.Throws<OverflowException>(() => new Duration(long.MinValue, TimeUnit.Millisecond).ToTimeSpan());
    }

    [Fact]
    public void ATimeSpanTooLargeForANanosecondCountThrows()
    {
        Assert.Throws<OverflowException>(() => Duration.FromTimeSpan(TimeSpan.MaxValue, TimeUnit.Nanosecond));
        Assert.Throws<OverflowException>(() => Duration.FromTimeSpan(TimeSpan.MinValue, TimeUnit.Nanosecond));
    }

    [Theory]
    [InlineData(2L, TimeUnit.Second, TimeUnit.Millisecond, 2_000L)]
    [InlineData(-9L, TimeUnit.Microsecond, TimeUnit.Nanosecond, -9_000L)]
    [InlineData(6_000L, TimeUnit.Microsecond, TimeUnit.Millisecond, 6L)]
    public void WithUnitRecountsExactly(long value, TimeUnit from, TimeUnit to, long expected)
    {
        Assert.Equal(new Duration(expected, to), new Duration(value, from).WithUnit(to));
    }

    [Fact]
    public void WithUnitRefusesToDropARemainderOrToOverflow()
    {
        Assert.Throws<ArgumentException>(() => new Duration(1_001L, TimeUnit.Microsecond).WithUnit(TimeUnit.Millisecond));
        Assert.Throws<OverflowException>(() => new Duration(long.MinValue / 2, TimeUnit.Millisecond).WithUnit(TimeUnit.Nanosecond));
    }

    [Fact]
    public void DefaultIsZeroSeconds()
    {
        Assert.Equal(TimeSpan.Zero, default(Duration).ToTimeSpan());
        Assert.Equal(TimeUnit.Second, default(Duration).Unit);
    }
}
