// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// Waiting for a topic's schema: ONE mechanism, and five outcomes.
//
// ── Why this is not a Task and never will be (D-BIND-22) ────────────────────
// Nothing resembling a future crosses the binding ABI - not a shared_future, not
// a promise, not a callback that fires once. A future has no C form, so a
// binding-side bridge (a thread parked on a wait, calling back into managed)
// would be a SECOND waiting mechanism with different semantics: no Pending, no
// "Ok with no schema", a cancelled task instead of SubscriptionEnded. C# and
// Rust would each invent a different one. So the blocking Wait below is the
// primitive; an async helper may be built ABOVE it, never beside it.
//
// ── The five outcomes, and the two that are values rather than failures ─────
//   Ok + schema            the schema arrived.
//   Ok + null              RESERVED: this transport carries no schemas at all.
//                          NOT an error, NOT "not yet", and nothing to release.
//   Pending                not yet, within the timeout asked for. Ask again.
//   SubscriptionEnded      no schema will EVER arrive. Terminal.
//   anything else          the provider failed to produce one - and THAT throws.
//
// Confusing the second with the fourth is the failure seam §7 exists to prevent:
// on a schema-carrying transport they demand opposite handling, and the failure
// mode for guessing wrong is silent wrong-slot decoding rather than a crash.
// That is why Pending and SubscriptionEnded are returned as values here and only
// a genuine provider failure throws (D-BIND-19 rule 4).
using System;
using System.Threading;

using Eiva.Fletcher.Interop;

namespace Eiva.Fletcher;

/// <summary>The outcome of waiting for a topic's schema.</summary>
public readonly struct SchemaWaitResult
{
    internal SchemaWaitResult(FletcherStatus status, SchemaHandle? schema)
    {
        Status = status;
        Schema = schema;
    }

    /// <summary>Which of the outcomes this is.</summary>
    public FletcherStatus Status { get; }

    /// <summary>
    /// The schema, when <see cref="Status"/> is <see cref="FletcherStatus.Ok"/>.
    /// </summary>
    /// <remarks>
    /// Non-null on Ok from a schema-carrying transport, and the caller DISPOSES
    /// it. Null on Ok means the transport carries no schemas at all - there is
    /// nothing to dispose and nothing went wrong. Null on every other status.
    /// </remarks>
    public SchemaHandle? Schema { get; }

    /// <summary>Whether the schema arrived, on a transport that carries one.</summary>
    public bool HasSchema => Status == FletcherStatus.Ok && Schema is not null;
}

/// <summary>A waitable handle for a topic's schema.</summary>
public sealed class SchemaArrival : IDisposable
{
    private readonly SchemaArrivalHandle _handle;
    private bool _disposed;

    internal SchemaArrival(SchemaArrivalHandle handle) => _handle = handle;

    /// <summary>Wait up to <paramref name="timeout"/> for the schema.</summary>
    /// <exception cref="ArgumentOutOfRangeException">
    /// <paramref name="timeout"/> is negative and is not
    /// <see cref="Timeout.InfiniteTimeSpan"/>.
    /// </exception>
    /// <remarks>
    /// D-BIND-20's mapping, in code rather than in a comment:
    /// <see cref="Timeout.InfiniteTimeSpan"/> becomes the seam's unbounded form,
    /// and every OTHER negative is refused here, in managed code, before the call.
    /// The seam refuses negatives too - the point is that "negative means forever"
    /// must not be inventable by one binding and not another, so C# maps the one
    /// spelling .NET already has and rejects the rest rather than guessing.
    ///
    /// Spelling "forever" as a large finite number is NOT equivalent: a deadline
    /// computed as now + timeout overflows well below <see cref="long.MaxValue"/>
    /// and would return immediately, turning a wait into a poll.
    /// </remarks>
    public SchemaWaitResult Wait(TimeSpan timeout)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);

        long milliseconds;
        if (timeout == Timeout.InfiniteTimeSpan)
        {
            milliseconds = long.MaxValue;
        }
        else if (timeout < TimeSpan.Zero)
        {
            throw new ArgumentOutOfRangeException(
                nameof(timeout), timeout,
                "a negative timeout is refused: use Timeout.InfiniteTimeSpan to wait without a deadline");
        }
        else
        {
            double total = timeout.TotalMilliseconds;
            milliseconds = total >= long.MaxValue ? long.MaxValue : (long)total;
        }

        FlError err = default;
        int status = NativeMethods.fl_schema_arrival_wait(_handle, milliseconds, out FlSchema schema, ref err);

        // Pending and SubscriptionEnded are OUTCOMES. Handing them to
        // ThrowIfFailed would turn "not yet" into an exception, which is the
        // shape D-BIND-19 rule 4 exists to refuse.
        switch ((FletcherStatus)status)
        {
            case FletcherStatus.Ok:
                // Written on BOTH Ok shapes. A schema-less transport's null comes
                // back as a SchemaHandle whose IsNull is true, rather than as a
                // managed null, so a caller that dereferences without checking
                // gets a clear answer instead of a NullReferenceException.
                NativeMethods.fl_error_dispose(ref err);
                return new SchemaWaitResult(FletcherStatus.Ok,
                    schema.Schema == 0 ? null : new SchemaHandle(schema));

            case FletcherStatus.Pending:
            case FletcherStatus.SubscriptionEnded:
                NativeMethods.fl_error_dispose(ref err);
                return new SchemaWaitResult((FletcherStatus)status, null);

            default:
                Errors.ThrowIfFailed(status, ref err);
                throw new InvalidOperationException("unreachable: ThrowIfFailed did not throw on a failure");
        }
    }

    /// <summary>Release the arrival handle.</summary>
    /// <remarks>
    /// Does NOT cancel the subscription behind it, and does not release a schema
    /// already handed out by <see cref="Wait"/> - this end holds a copy of a
    /// copyable arrival, so letting go changes nothing at the other.
    /// </remarks>
    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        _handle.Dispose();
    }
}
