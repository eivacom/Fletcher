// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The subscribe tier, and the one file in this binding where a mistake is a
// use-after-free on a transport thread rather than a failing test.
//
// ── THE LIFETIME PROBLEM, stated before the code (D-BIND-18) ────────────────
// A subscription hands native a GCHandle to managed state and a function pointer
// into this file. Native then calls that pointer from a TRANSPORT THREAD, at
// moments nobody chooses. Freeing the GCHandle too early means the next delivery
// dereferences a freed handle; never freeing it means every subscription in the
// process leaks its handler for the life of the program.
//
// What makes it solvable is one guarantee from the seam: NO INVOCATION BEGINS
// AFTER `Unsubscribe` RETURNS. So the rule is:
//
//   * the thunk increments an in-flight count on entry and decrements on exit;
//   * `Unsubscribe` calls native FIRST, then sets `retired`;
//   * WHOEVER BRINGS THE COUNT TO ZERO AFTER RETIREMENT FREES THE HANDLE -
//     the cancelling thread if no delivery is running, the last delivery
//     otherwise.
//
// Both paths matter. The ordinary one blocks: native's `Unsubscribe` waits for
// an in-flight delivery, so by the time `retired` is set the count is already
// zero and the canceller frees. The re-entrant one does NOT block - a handler
// cancelling its own subscription cannot wait for the frame it is already in -
// so the count is 1, the canceller frees nothing, and the thunk's own exit does
// it. Exactly one of them wins, and `TryFree`'s interlocked exchange is what
// makes "exactly one" true rather than likely.
//
// ── What is NOT in this slice ───────────────────────────────────────────────
// D-BIND-18 also requires a thread-static "inside my own thunk" marker, a
// managed refusal of `Dispose` from a handler, a managed refusal of a
// synchronous `Subscribe` to a new topic from a handler, and
// `DispatchAfterDelivery`. Those are 4c-ii. They are a REFUSAL layer above the
// lifetime core below, and separating them keeps the part where a bug is
// memory-unsafe reviewable on its own.
using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Threading;

using Eiva.Fletcher.Interop;

namespace Eiva.Fletcher;

/// <summary>Receives one delivered row.</summary>
/// <param name="row">The encoded row, BORROWED for this call only.</param>
/// <param name="schema">The topic's schema, borrowed. Call Retain to keep it.</param>
/// <param name="attachments">Sidecar metadata, borrowed.</param>
/// <remarks>
/// EVERY PARAMETER IS A REF STRUCT OR BORROWED, and that is deliberate (N-3): an
/// <c>async</c> handler cannot compile against this signature, because a
/// <see cref="ReadOnlySpan{T}"/> and an <see cref="AttachmentsView"/> cannot live
/// across an await. An async handler would return at its first await while the
/// transport thread carried on and freed everything it was still reading - so the
/// compiler refuses the shape rather than leaving it to be discovered in
/// production.
///
/// A handler MUST NOT THROW across this boundary. If it does, the thunk absorbs
/// the exception and counts it: an exception reaching a transport's C frames is a
/// process termination, not an unwind.
/// </remarks>
public delegate void RowHandler(ReadOnlySpan<byte> row, SchemaHandle schema, AttachmentsView attachments);

/// <summary>A live subscription. Dispose to cancel it.</summary>
/// <remarks>
/// A handle rather than a <c>ulong</c>, because an id is meaningful only to the
/// subscriber that issued it: handing a raw number to a different subscriber
/// silently addresses THAT instance's subscription with the same number, or does
/// nothing at all. A typed handle makes that mistake unrepresentable.
/// </remarks>
public sealed class Subscription : IDisposable
{
    private readonly Subscriber _owner;

    internal Subscription(Subscriber owner, ulong id)
    {
        _owner = owner;
        Id = id;
    }

    internal ulong Id { get; }

    /// <summary>Whether this subscription has not been cancelled.</summary>
    public bool IsLive => !Retired;

    internal bool Retired { get; private set; }

    internal void MarkRetired() => Retired = true;

    /// <summary>Cancel the subscription.</summary>
    /// <remarks>
    /// Cancelling something already cancelled is a NO-OP rather than an error -
    /// the seam's rule, forwarded - so teardown may call this unconditionally.
    /// </remarks>
    public void Dispose() => _owner.Unsubscribe(this);
}

/// <summary>What a subscribe call returns.</summary>
public readonly struct SubscribeResult
{
    internal SubscribeResult(Subscription subscription, SchemaArrival schema)
    {
        Subscription = subscription;
        Schema = schema;
    }

    /// <summary>The subscription, for cancelling.</summary>
    public Subscription Subscription { get; }

    /// <summary>
    /// The topic's schema, as a waitable arrival: a subscriber may subscribe
    /// before any publisher exists.
    /// </summary>
    public SchemaArrival Schema { get; }
}

/// <summary>Subscribes to topics over one provider.</summary>
public sealed unsafe class Subscriber : IDisposable
{
    private readonly SubscriberHandle _handle;
    private readonly object _gate = new();
    private long _absorbed;
    private bool _disposed;

    /// <summary>Create a subscriber over <paramref name="provider"/>.</summary>
    public Subscriber(PubSubProviderHandle provider)
    {
        ArgumentNullException.ThrowIfNull(provider);

        FlError err = default;
        int status = SubscriberHandle.Create(provider.Handle, out SubscriberHandle handle, ref err);
        Errors.ThrowIfFailed(status, ref err);
        _handle = handle;
    }

    /// <summary>How many handler failures this subscriber's thunk has absorbed.</summary>
    /// <remarks>
    /// Containment without a count is a SILENT wrong answer, and this counter is
    /// the whole difference. A handler that throws does not reach the transport -
    /// where an escaping exception terminates the process - and does not reach the
    /// caller either, because there is no caller: a delivery arrives on a
    /// transport thread with nobody waiting on it. So the count is the only report
    /// there can be. Monotonic, so a test may bracket an operation and assert the
    /// delta.
    /// </remarks>
    public ulong AbsorbedCallbackFailures => (ulong)Interlocked.Read(ref _absorbed);

    /// <summary>Raised when a handler throws. Never rethrown into the transport.</summary>
    /// <remarks>
    /// The counter says how many; this says which. An exception thrown BY a
    /// handler for this event is itself absorbed and counted, because the
    /// alternative is an exception reaching the C frames this whole design exists
    /// to keep them out of.
    /// </remarks>
    public event EventHandler<HandlerFaultedEventArgs>? HandlerFaulted;

    /// <summary>Subscribe to a topic. NEVER BLOCKS.</summary>
    public SubscribeResult Subscribe(TopicPath topic, RowHandler handler)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        ArgumentNullException.ThrowIfNull(handler);

        byte* buffer = stackalloc byte[TopicPath.MaxJoinedBytes];
        FlStr* segments = stackalloc FlStr[topic.Segments.Count];
        FlTopic native = MarshalTopic(topic, buffer, segments);

        var state = new SubscriptionState(this, handler);

        // Allocated BEFORE the subscription can deliver, and freed by whoever
        // retires it last. If the subscribe itself fails there is no delivery and
        // no retirement, so the handle is freed right here.
        state.Self = GCHandle.Alloc(state);

        FlError err = default;
        int status = NativeMethods.fl_subscriber_subscribe(
            _handle,
            native,
            (nint)(delegate* unmanaged[Cdecl]<nint, ulong, byte*, nuint, FlSchema*, nint, void>)&DeliveryThunk,
            GCHandle.ToIntPtr(state.Self),
            out ulong id,
            out SchemaArrivalHandle arrival,
            ref err);

        if (status != (int)FletcherStatus.Ok)
        {
            state.Self.Free();
            Errors.ThrowIfFailed(status, ref err);
        }

        var subscription = new Subscription(this, id);
        state.Subscription = subscription;

        lock (_gate)
        {
            _live[id] = state;
        }

        return new SubscribeResult(subscription, new SchemaArrival(arrival));
    }

    private readonly System.Collections.Generic.Dictionary<ulong, SubscriptionState> _live = [];

    /// <summary>Cancel a subscription.</summary>
    /// <remarks>
    /// THE ORDER HERE IS THE WHOLE DESIGN. Native is called first, and on its
    /// return the seam guarantees no further invocation BEGINS. Only then is the
    /// state retired - so a delivery still running (the re-entrant case, which
    /// native does not wait for) is the one that frees the handle on its way out,
    /// and a subscription with nothing in flight is freed by this thread.
    /// </remarks>
    public void Unsubscribe(Subscription subscription)
    {
        ArgumentNullException.ThrowIfNull(subscription);

        if (_disposed || subscription.Retired)
        {
            // Cancelling something that is not live is a no-op, not an error: a
            // foreign-runtime finaliser cannot let an exception escape.
            return;
        }

        SubscriptionState? state;
        lock (_gate)
        {
            _live.TryGetValue(subscription.Id, out state);
            _live.Remove(subscription.Id);
        }

        FlError err = default;
        int status = NativeMethods.fl_subscriber_unsubscribe(_handle, subscription.Id, ref err);
        subscription.MarkRetired();

        // Retired AFTER the native call returns, never before: retiring first
        // would let the last in-flight delivery free the handle while native was
        // still holding the pointer to it.
        state?.Retire();

        Errors.ThrowIfFailed(status, ref err);
    }

    /// <summary>Release the subscriber, cancelling everything still live.</summary>
    /// <remarks>
    /// REQUIRES QUIESCENCE, and this is the clause a managed wrapper cannot fully
    /// honour: from inside a delivery on this same subscriber the seam's answer is
    /// process termination, by design. Refusing that in managed code, before it
    /// can reach native, is D-BIND-18's job and belongs to 4c-ii.
    /// </remarks>
    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;

        SubscriptionState[] remaining;
        lock (_gate)
        {
            remaining = [.. _live.Values];
            _live.Clear();
        }

        foreach (SubscriptionState state in remaining)
        {
            FlError err = default;
            // Deliberately not checked: teardown cancels everything it can, and a
            // refusal on one subscription must not leave the rest subscribed.
            NativeMethods.fl_subscriber_unsubscribe(_handle, state.Id, ref err);
            NativeMethods.fl_error_dispose(ref err);
            state.Subscription?.MarkRetired();
            state.Retire();
        }

        _handle.Dispose();
    }

    internal void ReportAbsorbed(Exception exception, ulong subscriptionId)
    {
        Interlocked.Increment(ref _absorbed);

        try
        {
            HandlerFaulted?.Invoke(this, new HandlerFaultedEventArgs(exception, subscriptionId));
        }
        catch (Exception)
        {
            // A subscriber to the event threw. Counted already; nothing else can
            // be done, and NOTHING may leave this frame.
            Interlocked.Increment(ref _absorbed);
        }
    }

    /// <summary>A topic's segments, laid into one stack buffer. See Publisher.</summary>
    private static FlTopic MarshalTopic(TopicPath topic, byte* buffer, FlStr* segments)
    {
        byte[][] utf8 = topic.Utf8Segments;
        int offset = 0;

        for (int i = 0; i < utf8.Length; i++)
        {
            byte[] segment = utf8[i];
            segment.AsSpan().CopyTo(new Span<byte>(buffer + offset, segment.Length));
            segments[i] = new FlStr { Data = (nint)(buffer + offset), Len = (nuint)segment.Length };
            offset += segment.Length;
        }

        return new FlTopic { Segments = (nint)segments, Count = (nuint)utf8.Length };
    }

    /// <summary>The delivery hook native calls. NOTHING may escape it.</summary>
    /// <remarks>
    /// Runs on a TRANSPORT THREAD. An exception leaving an
    /// <c>[UnmanagedCallersOnly]</c> method is a fail-fast rather than an unwind,
    /// so every path below is inside a catch - including the one that records the
    /// failure, because recording can itself throw.
    /// </remarks>
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void DeliveryThunk(
        nint ctx, ulong subscriptionId, byte* data, nuint length, FlSchema* schema, nint attachments)
    {
        SubscriptionState? state = null;

        try
        {
            state = (SubscriptionState?)GCHandle.FromIntPtr(ctx).Target;
            if (state is null || !state.TryEnter())
            {
                return;
            }

            try
            {
                // BORROWED, all three. The schema handle is the non-owning form:
                // the shim holds the reference for this call, so a handler that
                // disposes what it was handed drops nothing, and one that wants to
                // keep it calls Retain.
                var schemaHandle = new SchemaHandle(*schema, owned: false);
                var view = new AttachmentsView(attachments);
                var row = new ReadOnlySpan<byte>(data, checked((int)length));

                state.Handler(row, schemaHandle, view);
            }
            catch (Exception exception)
            {
                state.Owner.ReportAbsorbed(exception, subscriptionId);
            }
            finally
            {
                state.Exit();
            }
        }
        catch (Exception)
        {
            // Reaching here means the bookkeeping itself failed - a freed handle,
            // an allocation failure recording the fault. There is nowhere to
            // report it and the one thing that must not happen is an exception
            // leaving this frame.
        }
    }

    /// <summary>Per-subscription lifetime bookkeeping (D-BIND-18).</summary>
    private sealed class SubscriptionState
    {
        private int _inFlight;
        private int _retired;
        private int _freed;

        internal SubscriptionState(Subscriber owner, RowHandler handler)
        {
            Owner = owner;
            Handler = handler;
        }

        internal Subscriber Owner { get; }

        internal RowHandler Handler { get; }

        internal GCHandle Self;

        internal Subscription? Subscription { get; set; }

        internal ulong Id => Subscription?.Id ?? 0;

        /// <summary>Claim a delivery slot, unless the subscription is already retired.</summary>
        /// <remarks>
        /// The retired check is ADVISORY and the increment is what matters. The
        /// seam already guarantees no invocation begins after Unsubscribe returns,
        /// so a delivery that gets here is one that began before - the check only
        /// short-circuits a handler call whose subscription the caller has already
        /// let go of.
        /// </remarks>
        internal bool TryEnter()
        {
            if (Volatile.Read(ref _retired) != 0)
            {
                return false;
            }

            Interlocked.Increment(ref _inFlight);
            return true;
        }

        /// <summary>Leave a delivery. The last one out after retirement frees.</summary>
        internal void Exit()
        {
            if (Interlocked.Decrement(ref _inFlight) == 0 && Volatile.Read(ref _retired) != 0)
            {
                TryFree();
            }
        }

        /// <summary>Retire the subscription, freeing if nothing is in flight.</summary>
        internal void Retire()
        {
            Volatile.Write(ref _retired, 1);

            if (Volatile.Read(ref _inFlight) == 0)
            {
                TryFree();
            }
        }

        /// <summary>Free the GCHandle exactly once, whoever gets here first.</summary>
        /// <remarks>
        /// The interlocked exchange is what makes "exactly one" true rather than
        /// merely likely: a retiring thread and a departing delivery can both
        /// observe zero-and-retired, and freeing a GCHandle twice is an
        /// InvalidOperationException on a transport thread - which is to say, a
        /// crash with no useful stack.
        /// </remarks>
        private void TryFree()
        {
            if (Interlocked.Exchange(ref _freed, 1) == 0 && Self.IsAllocated)
            {
                Self.Free();
            }
        }
    }
}

/// <summary>Carries a handler failure the thunk absorbed.</summary>
public sealed class HandlerFaultedEventArgs : EventArgs
{
    internal HandlerFaultedEventArgs(Exception exception, ulong subscriptionId)
    {
        Exception = exception;
        SubscriptionId = subscriptionId;
    }

    /// <summary>What the handler threw.</summary>
    public Exception Exception { get; }

    /// <summary>Which subscription's delivery it was.</summary>
    public ulong SubscriptionId { get; }
}
