// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The managed end of D-BIND-19: one numbered error type, the native message
// verbatim, and one place that decides what to throw.
using System;

namespace Eiva.Fletcher;

/// <summary>The seam's status taxonomy, number for number.</summary>
/// <remarks>
/// The numbers are the seam's own and are NEVER renumbered, reordered, reused or
/// removed at any version — a number that has reached an application cannot be
/// taken back. <see cref="Pending"/> and <see cref="SubscriptionEnded"/> are wait
/// OUTCOMES rather than failures, and never reach an exception: they are returned
/// as values (D-BIND-19 rule 4).
/// </remarks>
public enum FletcherStatus
{
    /// <summary>Success.</summary>
    Ok = 0,

    /// <summary>Something the seam refuses to interpret.</summary>
    InvalidArgument = 1,

    /// <summary>A topic re-declared with a provably different schema.</summary>
    SchemaConflict = 2,

    /// <summary>The topic has not been declared on this instance.</summary>
    TopicNotDeclared = 3,

    /// <summary>The encoded sample does not fit the transport's payload bound.</summary>
    PayloadTooLarge = 4,

    /// <summary>The transport refused or failed.</summary>
    TransportFailure = 5,

    /// <summary>This provider does not implement the requested behaviour.</summary>
    NotSupported = 6,

    /// <summary>The catch-all: anything with no better home.</summary>
    Internal = 7,

    /// <summary>A wait OUTCOME, never a failure: not available yet.</summary>
    Pending = 8,

    /// <summary>A wait OUTCOME, never a failure: it will never arrive.</summary>
    SubscriptionEnded = 9,

    /// <summary>The seam was re-entered from inside a delivery callback.</summary>
    ReentrantCall = 10,
}

/// <summary>Which containment site produced a failure.</summary>
/// <remarks>
/// Not a seam concept — BIND's own, and it exists because the number alone cannot
/// separate two things a caller must: an <see cref="FletcherStatus.InvalidArgument"/>
/// from the seam ("no such provider name") and one from the positional reader on a
/// truncated buffer. It is what lets the throw site raise a typed
/// <see cref="FletcherFormatException"/> for the second (D-BIND-15).
/// </remarks>
public enum FletcherOrigin
{
    /// <summary>No failure.</summary>
    None = 0,

    /// <summary>The seam: a PubSubError or a std::exception.</summary>
    Seam = 1,

    /// <summary>Malformed bytes: the positional reader, envelope parsing.</summary>
    Codec = 2,

    /// <summary>A caller-supplied writer or handler reported failure.</summary>
    Callback = 3,
}

/// <summary>A failure that crossed the binding ABI.</summary>
/// <remarks>
/// Carries the native message VERBATIM. That is not politeness: a registry
/// refusal lists every registered provider, and the difference between that and
/// "invalid argument" is a five-second fix versus an afternoon.
/// </remarks>
public class FletcherException : Exception
{
    /// <summary>Create a failure with a status, an origin and the native message.</summary>
    public FletcherException(FletcherStatus status, FletcherOrigin origin, string message)
        : base(message)
    {
        Status = status;
        Origin = origin;
    }

    /// <summary>Create a failure with a status and message only.</summary>
    public FletcherException(FletcherStatus status, string message)
        : this(status, FletcherOrigin.Seam, message)
    {
    }

    /// <summary>Required by the exception design guidelines.</summary>
    public FletcherException()
        : this(FletcherStatus.Internal, FletcherOrigin.None, "Fletcher failed")
    {
    }

    /// <summary>Required by the exception design guidelines.</summary>
    public FletcherException(string message)
        : this(FletcherStatus.Internal, FletcherOrigin.None, message)
    {
    }

    /// <summary>Required by the exception design guidelines.</summary>
    public FletcherException(string message, Exception innerException)
        : base(message, innerException)
    {
        Status = FletcherStatus.Internal;
        Origin = FletcherOrigin.None;
    }

    /// <summary>The seam status number this failure carries.</summary>
    public FletcherStatus Status { get; }

    /// <summary>Which containment site produced it.</summary>
    public FletcherOrigin Origin { get; }
}

/// <summary>Malformed input: an <see cref="FletcherStatus.InvalidArgument"/> whose origin is the codec.</summary>
/// <remarks>
/// A separate type because the two are acted on differently. A caller can retry
/// or reconfigure after a transport failure; malformed bytes mean the data is
/// wrong, and the HARD rounds put specific messages into the positional reader
/// precisely so that this case says which byte and why (D-BIND-15). Catching
/// <see cref="FletcherException"/> still catches this. Other codec-origin
/// failures are NOT this type: a row too large for its window is valid data and
/// a <see cref="FletcherStatus.PayloadTooLarge"/>, and a codec-origin
/// <see cref="FletcherStatus.Internal"/> is a defect (D-BIND-54). Both arrive as
/// a plain <see cref="FletcherException"/> whose <see cref="FletcherException.Origin"/>
/// is still <see cref="FletcherOrigin.Codec"/>.
/// </remarks>
public sealed class FletcherFormatException : FletcherException
{
    /// <summary>Create a malformed-input failure.</summary>
    public FletcherFormatException(FletcherStatus status, string message)
        : base(status, FletcherOrigin.Codec, message)
    {
    }

    /// <summary>Required by the exception design guidelines.</summary>
    public FletcherFormatException()
        : base(FletcherStatus.InvalidArgument, FletcherOrigin.Codec, "malformed input")
    {
    }

    /// <summary>Required by the exception design guidelines.</summary>
    public FletcherFormatException(string message)
        : base(FletcherStatus.InvalidArgument, FletcherOrigin.Codec, message)
    {
    }

    /// <summary>Required by the exception design guidelines.</summary>
    public FletcherFormatException(string message, Exception innerException)
        : base(message, innerException)
    {
    }
}
