// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The arithmetic Timestamp and Duration share: moving between an Arrow time unit
// and the 100 ns tick that DateTime, DateTimeOffset and TimeSpan count in.
//
// A tick is coarser than a nanosecond and finer than every other Arrow unit, so
// the two directions are not alike. Second, millisecond and microsecond values
// become ticks EXACTLY; a nanosecond value is divided by 100 and rounds toward
// negative infinity (floor), so -1 ns is one tick BEFORE the instant rather than
// the instant itself. Going the other way, a finer unit is exact and a coarser
// one floors again. Every overflow throws rather than wrapping: a value that does
// not fit is a defect to report, and a silent wrap is a wrong time.
using System;

using Apache.Arrow.Types;

namespace Eiva.Fletcher.Model;

internal static class UnitMath
{
    // static readonly, not const: .editorconfig names a private const like a private
    // instance field (_underscore) and a private static readonly in PascalCase.
    private static readonly long TicksPerSecond = 10_000_000L;

    /// <summary>How many ticks one <paramref name="unit"/> is, for the units at least as coarse as a tick.</summary>
    private static long TicksPerUnit(TimeUnit unit) => unit switch
    {
        TimeUnit.Second => TicksPerSecond,
        TimeUnit.Millisecond => TicksPerSecond / 1_000L,
        TimeUnit.Microsecond => TicksPerSecond / 1_000_000L,
        _ => throw new ArgumentOutOfRangeException(nameof(unit), unit, "Not an Arrow time unit."),
    };

    /// <summary>The tick count of <paramref name="value"/> counted in <paramref name="unit"/>.</summary>
    /// <exception cref="OverflowException">The tick count does not fit in a <see cref="long"/>.</exception>
    internal static long ToTicks(long value, TimeUnit unit) =>
        unit == TimeUnit.Nanosecond ? FloorDivide(value, 100L) : checked(value * TicksPerUnit(unit));

    /// <summary><paramref name="ticks"/> counted in <paramref name="unit"/>, floored when the unit is coarser than a tick.</summary>
    /// <exception cref="OverflowException">The result does not fit in a <see cref="long"/>.</exception>
    internal static long FromTicks(long ticks, TimeUnit unit) =>
        unit == TimeUnit.Nanosecond ? checked(ticks * 100L) : FloorDivide(ticks, TicksPerUnit(unit));

    /// <summary>How many nanoseconds one <paramref name="unit"/> is.</summary>
    private static long NanosecondsPerUnit(TimeUnit unit) => unit switch
    {
        TimeUnit.Second => 1_000_000_000L,
        TimeUnit.Millisecond => 1_000_000L,
        TimeUnit.Microsecond => 1_000L,
        TimeUnit.Nanosecond => 1L,
        _ => throw new ArgumentOutOfRangeException(nameof(unit), unit, "Not an Arrow time unit."),
    };

    /// <summary><paramref name="value"/> counted in <paramref name="from"/>, recounted in <paramref name="to"/> EXACTLY.</summary>
    /// <remarks>
    /// Unlike the tick conversions above, this one never rounds: it is what puts a value into
    /// an Arrow column of a fixed unit, and a column that silently dropped digits would be the
    /// loss D-BIND-26 exists to prevent. A finer unit multiplies; a coarser one divides only
    /// when nothing is left over.
    /// </remarks>
    /// <exception cref="ArgumentException">A coarser unit cannot hold the value without losing its remainder.</exception>
    /// <exception cref="OverflowException">A finer unit's count does not fit in a <see cref="long"/>.</exception>
    internal static long Recount(long value, TimeUnit from, TimeUnit to)
    {
        long fromNs = NanosecondsPerUnit(from);
        long toNs = NanosecondsPerUnit(to);
        if (fromNs >= toNs)
        {
            return checked(value * (fromNs / toNs));
        }

        long ratio = toNs / fromNs;
        if (value % ratio != 0)
        {
            throw new ArgumentException(
                $"{value} {from} is not a whole number of {to}: recounting it would lose precision.", nameof(value));
        }

        return value / ratio;
    }

    private static long FloorDivide(long dividend, long divisor)
    {
        long quotient = dividend / divisor;
        return dividend % divisor < 0 ? quotient - 1 : quotient;
    }
}
