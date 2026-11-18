// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// A point in time that does not lose a digit on its way through C# (D-BIND-26).
//
// DateTime and DateTimeOffset count 100 ns ticks. An Arrow nanosecond timestamp
// would lose its last two digits on every round trip, silently, and the loss
// would surface as a wire-visible difference between a C# and a C++ publisher.
// So the generated type keeps the Arrow value, the unit it is counted in and the
// timezone the schema carries; the lossy step is a conversion you CALL, named
// for what it does, and documented where it loses.
using System;

using Apache.Arrow.Types;

namespace Eiva.Fletcher.Model;

/// <summary>An Arrow timestamp: a count of <paramref name="Unit"/>s since the Unix epoch, kept exactly.</summary>
/// <param name="Value">The count, in <paramref name="Unit"/>s, since 1970-01-01T00:00:00Z. Negative before it.</param>
/// <param name="Unit">The unit <paramref name="Value"/> is counted in.</param>
/// <param name="TimeZone">The timezone the schema declares, or <see langword="null"/> for none. Metadata only: the instant is the same in every zone.</param>
/// <remarks>
/// Equality is STRUCTURAL: the same value, unit and zone. One second and 1000
/// milliseconds are the same instant and are NOT equal, because two values that
/// compare equal must also be the same Arrow value on the wire. Compare the
/// instants with <see cref="ToDateTimeOffset"/> when that is what you mean.
/// </remarks>
public readonly record struct Timestamp(long Value, TimeUnit Unit, string? TimeZone = null)
{
    /// <summary>Converts to a <see cref="DateTimeOffset"/> at UTC.</summary>
    /// <returns>
    /// The instant, with a zero offset. Second, millisecond and microsecond values
    /// convert exactly. A nanosecond value is floored to a 100 ns tick, so
    /// <c>FromDateTimeOffset(ToDateTimeOffset(x), Nanosecond)</c> equals <c>x</c>
    /// only when <c>x</c> is a multiple of 100 ns.
    /// </returns>
    /// <exception cref="OverflowException">The instant is outside 0001-01-01 to 9999-12-31.</exception>
    public DateTimeOffset ToDateTimeOffset()
    {
        long ticks = checked(UnitMath.ToTicks(Value, Unit) + DateTime.UnixEpoch.Ticks);
        if (ticks < DateTime.MinValue.Ticks || ticks > DateTime.MaxValue.Ticks)
        {
            throw new OverflowException("The timestamp is outside the range of DateTimeOffset.");
        }

        return new DateTimeOffset(ticks, TimeSpan.Zero);
    }

    /// <summary>The same instant counted in <paramref name="unit"/>, exactly; the timezone is kept.</summary>
    /// <remarks>
    /// This is how a value enters an Arrow column of a fixed unit: generated <c>ToArrow</c>
    /// calls it with the column's unit. A finer unit is always exact; a coarser one is
    /// refused rather than rounded when the value has a remainder.
    /// </remarks>
    /// <exception cref="ArgumentException"><paramref name="unit"/> is coarser and the value is not a whole number of it.</exception>
    /// <exception cref="OverflowException">The count in a finer <paramref name="unit"/> does not fit in 64 bits.</exception>
    public Timestamp WithUnit(TimeUnit unit) => new(UnitMath.Recount(Value, Unit, unit), unit, TimeZone);

    /// <summary>Builds a timestamp counting <paramref name="unit"/>s from a <see cref="DateTimeOffset"/>.</summary>
    /// <param name="value">The instant. Its offset is ignored: only the UTC instant is kept.</param>
    /// <param name="unit">The unit to count in. A unit coarser than 100 ns floors the sub-unit part.</param>
    /// <param name="timeZone">The timezone metadata to carry, or <see langword="null"/>.</param>
    /// <exception cref="OverflowException">The instant does not fit a 64-bit count of <paramref name="unit"/>s.</exception>
    public static Timestamp FromDateTimeOffset(DateTimeOffset value, TimeUnit unit, string? timeZone = null) =>
        new(UnitMath.FromTicks(value.UtcTicks - DateTime.UnixEpoch.Ticks, unit), unit, timeZone);
}
