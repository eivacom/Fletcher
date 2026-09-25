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
// `WaitAsync` is that helper (D-BIND-55): it runs this Wait in short slices on
// the thread pool, so every outcome it returns is one a Wait returned.
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
using System.Diagnostics;
using System.Threading;
using System.Threading.Tasks;

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

    /// <summary>Whether this is the schema-less transport's answer: Ok, and no schema.</summary>
    /// <remarks>
    /// NOT the negation of <see cref="HasSchema"/>, and that is why it exists
    /// (D-BIND-55). <c>!HasSchema</c> is also true for Pending and for
    /// SubscriptionEnded, so a caller who reads it as "this transport carries no
    /// schemas" makes the one mistake seam §7 exists to prevent: on a
    /// schema-carrying transport the two demand opposite handling, and guessing
    /// wrong decodes into the wrong slot silently rather than failing.
    /// </remarks>
    public bool IsSchemaless => Status == FletcherStatus.Ok && Schema is null;
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
        long milliseconds = ToMilliseconds(timeout);

        FlError err = default;
        int status = NativeMethods.fl_schema_arrival_wait(_handle, milliseconds, out FlSchema schema, ref err);

        // Pending and SubscriptionEnded are OUTCOMES. Handing them to
        // ThrowIfFailed would turn "not yet" into an exception, which is the
        // shape D-BIND-19 rule 4 exists to refuse.
        switch ((FletcherStatus)status)
        {
            case FletcherStatus.Ok:
                // Both Ok shapes. A schema-less transport's answer comes back as a
                // managed NULL Schema, with IsSchemaless true: nothing to dispose
                // and nothing went wrong.
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

    /// <summary>Wait up to <paramref name="timeout"/> for the schema without blocking the caller.</summary>
    /// <exception cref="ArgumentOutOfRangeException">
    /// As <see cref="Wait"/>, and thrown HERE, on the caller's stack, rather than
    /// through the task.
    /// </exception>
    /// <exception cref="ObjectDisposedException">The arrival is already disposed.</exception>
    /// <remarks>
    /// <para>
    /// THE HELPER D-BIND-22 ALLOWS, built above <see cref="Wait"/> and never beside
    /// it (D-BIND-55). It calls Wait in slices of at most <see cref="AsyncSlice"/>
    /// on a thread-pool thread and checks <paramref name="cancellationToken"/>
    /// between them, so it has Wait's outcomes and no others: Ok with or without a
    /// schema, Pending when the timeout elapses first, SubscriptionEnded, and a
    /// provider failure, which faults the task with the same exception Wait throws.
    /// </para>
    /// <para>
    /// Cancellation ends the task as CANCELLED within about one slice
    /// (<see cref="OperationCanceledException"/>, managed-only - D-BIND-19 rule 4).
    /// It cancels nothing native: the arrival stays waitable, and a later wait sees
    /// whatever arrived meanwhile. Disposing the arrival while a wait runs ends it
    /// with <see cref="ObjectDisposedException"/> at the next slice.
    /// </para>
    /// <para>
    /// THE COST, stated rather than buried: one thread-pool thread is occupied for
    /// as long as the wait runs, because the primitive underneath blocks. An
    /// application waiting on many topics at once pays one thread for each.
    /// </para>
    /// </remarks>
    public Task<SchemaWaitResult> WaitAsync(TimeSpan timeout, CancellationToken cancellationToken = default)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);

        // Validated for its exception, not its value: a refused timeout belongs on
        // the caller's stack, not inside a faulted task they may never await.
        _ = ToMilliseconds(timeout);

        if (cancellationToken.IsCancellationRequested)
        {
            return Task.FromCanceled<SchemaWaitResult>(cancellationToken);
        }

        return Task.Run(() => WaitInSlices(timeout, cancellationToken), cancellationToken);
    }

    /// <summary>The longest single Wait that WaitAsync makes, which bounds how late a cancellation lands.</summary>
    internal static readonly TimeSpan AsyncSlice = TimeSpan.FromMilliseconds(50);

    private SchemaWaitResult WaitInSlices(TimeSpan timeout, CancellationToken cancellationToken)
    {
        bool forever = timeout == Timeout.InfiniteTimeSpan;
        var clock = Stopwatch.StartNew();

        while (true)
        {
            cancellationToken.ThrowIfCancellationRequested();

            TimeSpan slice = AsyncSlice;
            if (!forever)
            {
                TimeSpan left = timeout - clock.Elapsed;
                if (left < slice)
                {
                    slice = left < TimeSpan.Zero ? TimeSpan.Zero : left;
                }
            }

            SchemaWaitResult result = Wait(slice);

            // Only "not yet" goes round again. Every other outcome is Wait's own
            // answer and is returned as it came; a failure has already thrown.
            if (result.Status != FletcherStatus.Pending)
            {
                return result;
            }

            if (!forever && clock.Elapsed >= timeout)
            {
                return result;
            }
        }
    }

    /// <summary>D-BIND-20's mapping, in one place for both waits.</summary>
    private static long ToMilliseconds(TimeSpan timeout)
    {
        if (timeout == Timeout.InfiniteTimeSpan)
        {
            return long.MaxValue;
        }

        if (timeout < TimeSpan.Zero)
        {
            throw new ArgumentOutOfRangeException(
                nameof(timeout), timeout,
                "a negative timeout is refused: use Timeout.InfiniteTimeSpan to wait without a deadline");
        }

        double total = timeout.TotalMilliseconds;
        return total >= long.MaxValue ? long.MaxValue : (long)total;
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
