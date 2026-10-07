// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// A span of time that does not lose a digit on its way through C# (D-BIND-26).
// The reasoning is Timestamp's: TimeSpan counts 100 ns ticks, an Arrow nanosecond
// duration does not, so the type keeps the value and the unit and the lossy step
// is a call you make.
using System;

using Apache.Arrow.Types;

namespace Eiva.Fletcher.Model;

/// <summary>An Arrow duration: a count of <paramref name="Unit"/>s, kept exactly.</summary>
/// <param name="Value">The count, in <paramref name="Unit"/>s. Negative for a span running backwards.</param>
/// <param name="Unit">The unit <paramref name="Value"/> is counted in.</param>
/// <remarks>
/// Equality is STRUCTURAL, as for <see cref="Timestamp"/>: one second and 1000
/// milliseconds are the same span and are NOT equal.
/// </remarks>
public readonly record struct Duration(long Value, TimeUnit Unit)
{
    /// <summary>Converts to a <see cref="TimeSpan"/>.</summary>
    /// <returns>
    /// The span. Second, millisecond and microsecond values convert exactly. A
    /// nanosecond value is floored to a 100 ns tick, so a negative nanosecond value
    /// that is not a multiple of 100 rounds away from zero.
    /// </returns>
    /// <exception cref="OverflowException">The span does not fit a <see cref="TimeSpan"/>.</exception>
    public TimeSpan ToTimeSpan() => new(UnitMath.ToTicks(Value, Unit));

    /// <summary>The same span counted in <paramref name="unit"/>, exactly.</summary>
    /// <remarks>
    /// How a value enters an Arrow column of a fixed unit, as for <see cref="Timestamp.WithUnit"/>:
    /// a finer unit is always exact, a coarser one is refused rather than rounded.
    /// </remarks>
    /// <exception cref="ArgumentException"><paramref name="unit"/> is coarser and the value is not a whole number of it.</exception>
    /// <exception cref="OverflowException">The count in a finer <paramref name="unit"/> does not fit in 64 bits.</exception>
    public Duration WithUnit(TimeUnit unit) => new(UnitMath.Recount(Value, Unit, unit), unit);

    /// <summary>Builds a duration counting <paramref name="unit"/>s from a <see cref="TimeSpan"/>.</summary>
    /// <param name="value">The span.</param>
    /// <param name="unit">The unit to count in. A unit coarser than 100 ns floors the sub-unit part.</param>
    /// <exception cref="OverflowException">The span does not fit a 64-bit count of <paramref name="unit"/>s.</exception>
    public static Duration FromTimeSpan(TimeSpan value, TimeUnit unit) =>
        new(UnitMath.FromTicks(value.Ticks, unit), unit);
}
