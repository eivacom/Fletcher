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
// ── AND THE PATH THE FIRST VERSION GOT WRONG (D-BIND-50) ────────────────────
// "No invocation begins after Unsubscribe returns" is true at the seam, where
// "begins" means PASSING THE GATE. The count above is incremented LATER, inside
// this file's thunk. On the blocking path the difference is invisible - native
// drained. On the re-entrant path it is not: a handler that cancels a SIBLING
// subscription whose delivery is between its gate and `TryEnter` on another
// thread sees a count of zero, and freeing on it frees a handle that thread is
// about to read. So a cancel from inside a delivery on this Subscriber does not
// free inline. It retires, and a thread-pool cancel of the same id - from
// outside any delivery, where the seam makes it wait for the same drain - frees
// afterwards. The self-cancel case still usually frees on the thunk's exit;
// either way exactly one free happens, and which branch ran is counted.
//
// ── The refusal layer above it (4c-ii) ──────────────────────────────────────
// Two things a handler can do have no safe answer at all once they reach native,
// so they are refused HERE, in managed code, while a managed exception is still
// possible:
//
//   Subscriber.Dispose() from a handler   native's answer is PROCESS TERMINATION.
//                                         Not a refusal - the seam's destructor
//                                         reaches the provider's door, is refused
//                                         with kReentrantCall, and rethrows out
//                                         of a noexcept destructor. By design,
//                                         because the alternative is a silently
//                                         leaked transport subscription.
//
//   Subscribe(new topic) from a handler   refused natively with kReentrantCall,
//                                         because a topic this Subscriber has not
//                                         subscribed before needs a PROVIDER-level
//                                         subscription and the provider cannot be
//                                         entered from inside its own delivery.
//
// A thread-static stack of delivery frames is what makes both detectable, and
// the question asked of it is the seam's: is this thread inside a delivery on
// this Subscriber's PROVIDER, at any depth (D-BIND-51)? The first version kept
// one marker naming the innermost Subscriber and compared it with `this`, which
// let a handler on subscriber X dispose subscriber Y over the same provider -
// and the seam's answer to that is the same process termination. Note what is
// NOT refused - `Unsubscribe`
// from a handler is the seam's documented carve-out and is served, and a
// Subscribe to a topic this Subscriber ALREADY holds is answered from the cached
// arrival without touching the provider. Which of the two a Subscribe gets
// depends on the data, not on the call, so the managed check asks the same
// question the seam does.
//
// `DispatchAfterDelivery` is the sanctioned route for work that must touch the
// seam: it runs off the delivery thread, where the seam's own blocking makes the
// call correct rather than refused.
using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;

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
    /// <summary>Every Subscriber whose thunk this thread is inside, outermost first.</summary>
    /// <remarks>
    /// <para>
    /// Thread-static because the question is about a THREAD's call stack, not
    /// about the object: two threads may be inside two deliveries on the same
    /// Subscriber, and neither is inside the other's.
    /// </para>
    /// <para>
    /// A STACK, NOT A SINGLE MARKER (D-BIND-51). The first version kept only the
    /// innermost Subscriber, and the seam's questions are about EVERY frame: a
    /// handler that publishes on an in-process provider which delivers
    /// synchronously to another handler puts two frames on this thread, and the
    /// outer one's provider is still one that must not be entered. An array and a
    /// depth rather than a linked list of frames, because this is the delivery hot
    /// path and a push must not allocate; the array grows only when nesting goes
    /// deeper than it has before on this thread.
    /// </para>
    /// </remarks>
    [ThreadStatic]
    private static Subscriber?[]? _frames;

    [ThreadStatic]
    private static int _depth;

    private long _freedByDelivery;
    private long _freedByCanceller;
    private long _freedDeferred;

    /// <summary>How many GCHandles a DEPARTING DELIVERY has freed (the re-entrant path).</summary>
    /// <remarks>
    /// <para>
    /// Exists so a test can bracket an operation and assert WHICH branch of the
    /// lifetime rule ran. See <c>SubscriptionState.TryFree</c>: both branches end
    /// in the same observable state - handle freed, delivery stopped - so without
    /// these the re-entrant path cannot be distinguished from the ordinary one by
    /// any assertion a test could make, and a row claiming to cover it would prove
    /// nothing.
    /// </para>
    /// <para>
    /// PER INSTANCE, and the first version was static, which was wrong for the
    /// same reason the seam scopes <c>AbsorbedCallbackFailures</c> per Subscriber:
    /// a process-wide counter answers a question nobody asked. A test bracketing
    /// one was measuring every other subscriber in the process too, and with xUnit
    /// running classes in parallel it read cancellations that belonged to another
    /// test - passing or failing on scheduling. Scoped here, the bracket means
    /// what it says.
    /// </para>
    /// </remarks>
    internal long FreedByDeliveryCount => Interlocked.Read(ref _freedByDelivery);

    /// <summary>How many a CANCELLING THREAD has freed (the ordinary path).</summary>
    internal long FreedByCancellerCount => Interlocked.Read(ref _freedByCanceller);

    /// <summary>How many the DEFERRED free has released (the carve-out path, D-BIND-50).</summary>
    internal long FreedDeferredCount => Interlocked.Read(ref _freedDeferred);

    /// <summary>Record which branch released a subscription's handle.</summary>
    internal void RecordFree(FreeBranch branch)
    {
        switch (branch)
        {
            case FreeBranch.Delivery:
                Interlocked.Increment(ref _freedByDelivery);
                break;
            case FreeBranch.Canceller:
                Interlocked.Increment(ref _freedByCanceller);
                break;
            default:
                Interlocked.Increment(ref _freedDeferred);
                break;
        }
    }

    /// <summary>Is this thread inside a delivery on THIS Subscriber, at any depth?</summary>
    /// <remarks>The seam's carve-out question - <c>InsideDeliveryOn(identity)</c> - asked here.</remarks>
    private bool InDeliveryOnThis()
    {
        Subscriber?[]? frames = _frames;
        for (int i = 0; i < _depth; i++)
        {
            if (ReferenceEquals(frames![i], this))
            {
                return true;
            }
        }

        return false;
    }

    /// <summary>Is this thread inside a delivery on THIS Subscriber's PROVIDER, at any depth?</summary>
    /// <remarks>
    /// The seam's door question - <c>InsideDeliveryOn(provider)</c> - asked here
    /// (D-BIND-51). Any Subscriber over the same provider counts, because the
    /// provider is what refuses to be entered, not the Subscriber.
    /// </remarks>
    private bool InDeliveryOnProvider()
    {
        Subscriber?[]? frames = _frames;
        for (int i = 0; i < _depth; i++)
        {
            if (ReferenceEquals(frames![i]!._provider, _provider))
            {
                return true;
            }
        }

        return false;
    }

    private static void PushFrame(Subscriber owner)
    {
        Subscriber?[] frames = _frames ??= new Subscriber?[4];
        if (_depth == frames.Length)
        {
            Array.Resize(ref frames, frames.Length * 2);
            _frames = frames;
        }

        frames[_depth++] = owner;
    }

    private static void PopFrame()
    {
        _frames![--_depth] = null;
    }

    private readonly SubscriberHandle _handle;
    private readonly ProviderHandle _provider;
    private readonly object _gate = new();
    private readonly System.Collections.Generic.HashSet<string> _known = new(StringComparer.Ordinal);
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

        // The provider's IDENTITY, for the refusal layer (D-BIND-51). One
        // ProviderHandle exists per native provider, so a reference comparison is
        // the seam's "same provider instance" question exactly.
        _provider = provider.Handle;
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
    public SubscribeResult Subscribe(TopicPath topic, RowHandler handler) => Subscribe(topic, handler, null);

    /// <summary>Subscribe with per-topic options (D-BIND-57).</summary>
    /// <remarks>
    /// The options apply to the FIRST provider-level subscription on the topic;
    /// a later subscription joins it and shares them, checked field by field (see
    /// <see cref="TopicOptions"/>). A subscription carries no payload bound, so a
    /// non-zero <see cref="TopicOptions.MaxPayloadBytes"/> is
    /// <see cref="FletcherStatus.InvalidArgument"/>. The two-argument form is this
    /// one with null options, which mean empty.
    /// </remarks>
    public SubscribeResult Subscribe(TopicPath topic, RowHandler handler, TopicOptions? options)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        ArgumentNullException.ThrowIfNull(handler);

        // REFUSED HERE BECAUSE THE SEAM WOULD REFUSE IT THERE, and a managed
        // exception is worth more than a kReentrantCall from inside a transport
        // callback. The question is the seam's own: a topic this Subscriber has
        // already subscribed is answered from the cached arrival and touches no
        // provider, so it is SERVED; one it has not needs a provider-level
        // subscription, and the provider cannot be entered from inside its own
        // delivery frame - a delivery on ANY Subscriber over this provider, at any
        // depth on this thread (D-BIND-51).
        string key = topic.ToKey();
        if (InDeliveryOnProvider())
        {
            bool known;
            lock (_gate)
            {
                known = _known.Contains(key);
            }

            if (!known)
            {
                throw new InvalidOperationException(
                    $"Subscribe to '{key}' cannot run inside a delivery on this subscriber's provider: " +
                    "the topic has not been subscribed before, so it needs a provider-level subscription, " +
                    "and the provider cannot be entered from inside its own delivery. Use " +
                    "DispatchAfterDelivery to defer the call past this handler's return.");
            }
        }

        byte* buffer = stackalloc byte[TopicPath.MaxJoinedBytes];
        FlStr* segments = stackalloc FlStr[topic.Segments.Count];
        FlTopic native = MarshalTopic(topic, buffer, segments);

        var state = new SubscriptionState(this, handler);

        // Allocated BEFORE the subscription can deliver, and freed by whoever
        // retires it last. If the subscribe itself fails there is no delivery and
        // no retirement, so the handle is freed right here.
        state.Self = GCHandle.Alloc(state);

        byte[] profile = options?.Profile is { Length: > 0 } p ? System.Text.Encoding.UTF8.GetBytes(p) : [];
        FlError err = default;
        int status;
        ulong id;
        SchemaArrivalHandle arrival;
        fixed (byte* profileBytes = profile)
        {
            var nativeOptions = new FlTopicOptions
            {
                Profile = new FlStr { Data = (nint)profileBytes, Len = (nuint)profile.Length },
                MaxPayloadBytes = options?.MaxPayloadBytes ?? 0,
            };

            status = NativeMethods.fl_subscriber_subscribe_with_options(
                _handle,
                native,
                (nint)(delegate* unmanaged[Cdecl]<nint, ulong, byte*, nuint, FlSchema*, nint, void>)&DeliveryThunk,
                GCHandle.ToIntPtr(state.Self),
                in nativeOptions,
                out id,
                out arrival,
                ref err);
        }

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

            // Remembered for the subscriber's LIFETIME, not until the last
            // subscription on it is cancelled. That mirrors the seam: a
            // provider-level subscription survives the last local unsubscribe and
            // is reused if the topic is subscribed again, so re-entering on it
            // stays legal after the topic has gone quiet.
            _known.Add(key);
        }

        return new SubscribeResult(subscription, new SchemaArrival(arrival));
    }

    private readonly System.Collections.Generic.Dictionary<ulong, SubscriptionState> _live = [];

    /// <summary>Watch a topic's schema without subscribing to its data (D-BIND-52, D-BIND-57).</summary>
    /// <remarks>
    /// <para>
    /// The seam's <c>SubscribeSchema</c>, forwarded: it never blocks, and the
    /// arrival resolves when a publisher announces the topic, exactly as the one
    /// <see cref="Subscribe(TopicPath, RowHandler)"/> returns. There is no
    /// subscription to cancel - a watch delivers nothing - so it is COUNTED per
    /// Subscriber and idempotent per topic, and the last
    /// <see cref="UnsubscribeSchema"/> (or <see cref="Dispose"/>) releases it.
    /// A data unsubscribe does not.
    /// </para>
    /// <para>
    /// A transport with no out-of-band schema channel answers
    /// <see cref="FletcherStatus.NotSupported"/>: only Fast DDS has one today.
    /// </para>
    /// </remarks>
    /// <exception cref="InvalidOperationException">
    /// Called from inside a delivery on this subscriber's provider. The seam refuses
    /// that ALWAYS - unlike <c>Subscribe</c>, whose answer depends on whether the
    /// provider must be entered - so it is refused here first (D-BIND-51).
    /// </exception>
    public SchemaArrival SubscribeSchema(TopicPath topic)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);

        if (InDeliveryOnProvider())
        {
            throw new InvalidOperationException(
                $"SubscribeSchema on '{topic.ToKey()}' cannot run inside a delivery on this subscriber's " +
                "provider: the seam refuses a schema watch there always. Use DispatchAfterDelivery to " +
                "defer the call past this handler's return.");
        }

        byte* buffer = stackalloc byte[TopicPath.MaxJoinedBytes];
        FlStr* segments = stackalloc FlStr[topic.Segments.Count];
        FlTopic native = MarshalTopic(topic, buffer, segments);

        FlError err = default;
        int status = NativeMethods.fl_subscriber_subscribe_schema(_handle, native, out SchemaArrivalHandle arrival, ref err);
        if (status != (int)FletcherStatus.Ok)
        {
            arrival.Dispose();
            Errors.ThrowIfFailed(status, ref err);
        }

        return new SchemaArrival(arrival);
    }

    /// <summary>Release one of this subscriber's watches on <paramref name="topic"/>.</summary>
    /// <remarks>
    /// A no-op for a topic this subscriber does not watch. Only the LAST release
    /// enters the provider, and a still-pending arrival then reports
    /// <see cref="FletcherStatus.SubscriptionEnded"/>. From inside a delivery on
    /// this subscriber's provider that last release is refused by the seam as
    /// <see cref="FletcherStatus.ReentrantCall"/> and the watch stays counted;
    /// this tier cannot tell a last release from an earlier one, so it passes the
    /// seam's answer through rather than guessing.
    /// </remarks>
    public void UnsubscribeSchema(TopicPath topic)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);

        byte* buffer = stackalloc byte[TopicPath.MaxJoinedBytes];
        FlStr* segments = stackalloc FlStr[topic.Segments.Count];
        FlTopic native = MarshalTopic(topic, buffer, segments);

        FlError err = default;
        int status = NativeMethods.fl_subscriber_unsubscribe_schema(_handle, native, ref err);
        Errors.ThrowIfFailed(status, ref err);
    }

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

        if (_disposed)
        {
            // Cancelling on a released subscriber is a no-op, not an error: a
            // foreign-runtime finaliser cannot let an exception escape.
            return;
        }

        // A subscription that is ALREADY retired still reaches native, and that is
        // not waste. Native's cancel of a fully cancelled id is a no-op - the seam
        // makes it one - but its cancel of an id that is still RETIRING waits for
        // that retirement's drain. The first version short-circuited on `Retired`,
        // so a thread cancelling a subscription whose handler had just cancelled
        // itself (the carve-out, which does not drain) returned while that handler
        // was still running - the "second exception to a promise that has one" the
        // seam's ruling of 2026-09-04 removed, reintroduced one tier up. Found when
        // `ACancelRacingASelfCancelWaitsForThatHandler` was made to mirror its C++
        // case faithfully (BIND-4 review, B5).

        SubscriptionState? state;
        lock (_gate)
        {
            _live.TryGetValue(subscription.Id, out state);
            _live.Remove(subscription.Id);
        }

        // Asked BEFORE the native call, which does not change the answer: whether
        // this thread is inside a delivery on this Subscriber is what decides
        // whether native is about to drain or about to return early.
        bool carveOut = InDeliveryOnThis();

        FlError err = default;
        int status = NativeMethods.fl_subscriber_unsubscribe(_handle, subscription.Id, ref err);
        subscription.MarkRetired();

        // Retired AFTER the native call returns, never before: retiring first
        // would let the last in-flight delivery free the handle while native was
        // still holding the pointer to it.
        if (state is not null)
        {
            if (carveOut)
            {
                RetireAfterDrain(state);
            }
            else
            {
                state.Retire();
            }
        }

        Errors.ThrowIfFailed(status, ref err);
    }

    /// <summary>The carve-out's retirement: free only once a drain has been waited for (D-BIND-50).</summary>
    /// <remarks>
    /// <para>
    /// WHY NOT FREE HERE. Native returned WITHOUT draining - a cancel from inside a
    /// delivery on this Subscriber cannot wait - and its promise is only that no
    /// invocation BEGINS afterwards, where "begins" means passing the seam's gate.
    /// The in-flight counter is incremented later, inside the thunk, so a SIBLING
    /// subscription whose delivery is between its gate and <c>TryEnter</c> on
    /// another thread reads as zero in flight. Freeing on that zero is a freed
    /// GCHandle read on a transport thread.
    /// </para>
    /// <para>
    /// WHAT WAITS INSTEAD. A second cancel of the same id, from a thread-pool thread
    /// - outside any delivery - "waits for the same drain" (subscriber.hpp): it
    /// returns only once the delivery holding the gate has finished, thunk and all.
    /// Then nothing can still be reading the handle, and it is freed. A delivery
    /// that DID reach <c>TryEnter</c> may still free it first, on its way out; the
    /// interlocked exchange in <c>TryFree</c> makes that race harmless.
    /// </para>
    /// <para>
    /// A reference is held on the subscriber's handle across the second cancel, so
    /// a racing <c>Dispose</c> postpones the native destroy rather than pulling the
    /// handle out from under the call. If <c>Dispose</c> already won, the destroy
    /// has drained everything - destruction requires quiescence - and the handle
    /// is simply freed.
    /// </para>
    /// </remarks>
    private void RetireAfterDrain(SubscriptionState state)
    {
        state.MarkRetiredWithoutFreeing();
        ulong id = state.Id;

        ThreadPool.UnsafeQueueUserWorkItem(
            static work =>
            {
                (Subscriber owner, SubscriptionState retiring, ulong subscriptionId) = work;
                bool added = false;
                try
                {
                    owner._handle.DangerousAddRef(ref added);
                    FlError drainErr = default;
                    NativeMethods.fl_subscriber_unsubscribe(owner._handle, subscriptionId, ref drainErr);
                    NativeMethods.fl_error_dispose(ref drainErr);
                }
                catch (ObjectDisposedException)
                {
                    // Dispose won the race: the native destroy drained every
                    // delivery before it returned, so the free below is safe.
                }
                finally
                {
                    if (added)
                    {
                        owner._handle.DangerousRelease();
                    }

                    retiring.FreeAfterDrain();
                }
            },
            (this, state, id),
            preferLocal: false);
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
        // THE ONE REFUSAL THAT PREVENTS A PROCESS TERMINATION RATHER THAN AN
        // ERROR. Reaching native here ends the program: the seam's destructor
        // enters the provider's door, is refused with kReentrantCall, and rethrows
        // out of a noexcept destructor. That is the designed answer to a forbidden
        // act - the alternative is to leak the transport subscription with no
        // signal - so the shim must not soften it and this wrapper must not reach
        // it. Checked BEFORE the disposed short-circuit, because a handler
        // disposing its own subscriber is a bug whether or not someone else
        // already disposed it.
        //
        // PER PROVIDER, over every frame on this thread (D-BIND-51). The seam
        // terminates for a handler on subscriber X destroying subscriber Y over
        // the same provider - not only for X destroying itself - and a nested
        // delivery must not hide an outer frame on that provider.
        if (InDeliveryOnProvider())
        {
            throw new InvalidOperationException(
                "Subscriber.Dispose() cannot run inside a delivery on this subscriber's provider: " +
                "destroying a subscriber requires quiescence - no delivery on its provider may be in " +
                "flight on this thread - and the native answer to doing it from a handler is process " +
                "termination, by design. Hand the subscriber to whatever owns its lifetime and let the handler return, or " +
                "use DispatchAfterDelivery.");
        }

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

    /// <summary>Run <paramref name="work"/> off the delivery thread.</summary>
    /// <returns>A task that completes when the work does, carrying any failure.</returns>
    /// <remarks>
    /// <para>
    /// THE SANCTIONED ROUTE for work that must touch the seam from a handler. The
    /// two refusals above name it, and this is what makes them actionable rather
    /// than merely correct: a handler that needs to subscribe, or to dispose its
    /// subscriber, hands the work here and returns.
    /// </para>
    /// <para>
    /// It runs on the thread pool, which is what makes the deferred call LEGAL
    /// rather than refused: the seam's rule is about re-entering a provider on the
    /// SAME THREAD, and a call from another thread is served - it simply blocks
    /// until the delivery in flight has returned. So the work may start before
    /// this delivery finishes and will wait at the provider's door, which is
    /// correct and costs nothing but a parked pool thread.
    /// </para>
    /// <para>
    /// <b>DO NOT AWAIT OR WAIT ON THE RETURNED TASK INSIDE THE HANDLER.</b> That
    /// is a deadlock and not a subtle one: the work blocks at the provider until
    /// the delivery returns, and the delivery cannot return because it is waiting
    /// for the work. The task is for the code that owns the handler, not for the
    /// handler.
    /// </para>
    /// <para>
    /// The failure lands on the task rather than in
    /// <see cref="AbsorbedCallbackFailures"/>: deferred work has a caller who can
    /// observe it, which is exactly what a delivery does not have.
    /// </para>
    /// </remarks>
    public Task DispatchAfterDelivery(Func<Task> work)
    {
        ArgumentNullException.ThrowIfNull(work);
        return Task.Run(work);
    }

    internal void ReportAbsorbed(Exception exception, ulong subscriptionId)
    {
        Interlocked.Increment(ref _absorbed);
        Diagnostics.CountAbsorbed();

        try
        {
            HandlerFaulted?.Invoke(this, new HandlerFaultedEventArgs(exception, subscriptionId));
        }
        catch (Exception)
        {
            // A subscriber to the event threw. Counted already; nothing else can
            // be done, and NOTHING may leave this frame.
            Interlocked.Increment(ref _absorbed);
            Diagnostics.CountAbsorbed();
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

            // PUSHED AND POPPED, never cleared. A handler that reaches a delivery
            // on another subscriber leaves an outer frame whose answer is still
            // "inside that one" - clearing would tell the outer frame it was safe
            // to dispose itself, and keeping only the innermost frame (the first
            // version) hid the outer frame's provider (D-BIND-51).
            PushFrame(state.Owner);

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
                // The frame comes off BEFORE Exit, so that the departing delivery
                // is not still advertising itself as in-flight while it frees.
                PopFrame();
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
        /// <remarks>
        /// Safe on every path, the carve-out included: a delivery that reached
        /// <c>TryEnter</c> holds this subscription's native gate until the thunk
        /// has returned, and the gate serialises this subscription's deliveries, so
        /// no other delivery of it can be between its gate and <c>TryEnter</c>
        /// while this one frees.
        /// </remarks>
        internal void Exit()
        {
            if (Interlocked.Decrement(ref _inFlight) == 0 && Volatile.Read(ref _retired) != 0)
            {
                TryFree(FreeBranch.Delivery);
            }
        }

        /// <summary>Retire the subscription, freeing if nothing is in flight.</summary>
        /// <remarks>
        /// ONLY after a cancel that drained. On the carve-out, zero in flight does
        /// not mean nothing is reading the handle - see
        /// <c>Subscriber.RetireAfterDrain</c> (D-BIND-50).
        /// </remarks>
        internal void Retire()
        {
            Volatile.Write(ref _retired, 1);

            if (Volatile.Read(ref _inFlight) == 0)
            {
                TryFree(FreeBranch.Canceller);
            }
        }

        /// <summary>The carve-out's first half: retired, and NOT freed on a zero count.</summary>
        internal void MarkRetiredWithoutFreeing() => Volatile.Write(ref _retired, 1);

        /// <summary>The carve-out's second half: a drain has been waited for, so free.</summary>
        internal void FreeAfterDrain() => TryFree(FreeBranch.Deferred);

        /// <summary>Free the GCHandle exactly once, whoever gets here first.</summary>
        /// <remarks>
        /// The interlocked exchange is what makes "exactly one" true rather than
        /// merely likely: a retiring thread and a departing delivery can both
        /// observe zero-and-retired, and freeing a GCHandle twice is an
        /// InvalidOperationException on a transport thread - which is to say, a
        /// crash with no useful stack.
        /// </remarks>
        private void TryFree(FreeBranch branch)
        {
            if (Interlocked.Exchange(ref _freed, 1) != 0)
            {
                return;
            }

            // WHICH BRANCH FREED IS RECORDED, because otherwise no test can tell
            // them apart. A self-cancelling handler and an ordinary cancellation
            // both end with the handle freed and delivery stopped, so a test
            // asserting only those outcomes passes whichever path ran - and the
            // re-entrant path is the one where a bug is a use-after-free on a
            // transport thread. Counting is the cheapest way to make the claim
            // checkable rather than merely plausible.
            Owner.RecordFree(branch);

            if (Self.IsAllocated)
            {
                Self.Free();
            }
        }
    }
}

/// <summary>Which branch of the lifetime rule released a subscription's GCHandle.</summary>
internal enum FreeBranch
{
    /// <summary>The last delivery out after retirement (the re-entrant path).</summary>
    Delivery,

    /// <summary>The cancelling thread, after a cancel that drained (the ordinary path).</summary>
    Canceller,

    /// <summary>The thread-pool free after a carve-out cancel waited for the drain (D-BIND-50).</summary>
    Deferred,
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
