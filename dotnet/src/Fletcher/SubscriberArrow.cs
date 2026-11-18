// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// BIND-5a, switched onto domain 7 by D10C-7: the Arrow tier's subscriber (D-BIND-25; d10).
//
// ── What it is, in terms of what is below it ────────────────────────────────
// Two kinds of subscription, and one kind per topic on one object (ruling :575 (6)):
//
//   * PER ROW (Subscribe): the native per-row path, on both builds of the shim.
//     Each row offered is one handler call: a one-row batch, or a null batch with
//     RowsDropped = 1 (RowDelivery; d10 L7, L9; BC-16).
//   * BATCHED (SubscribeBatched): domain 7's batches, through the server build's
//     fl_subscriber_subscribe_batches (d10 L4, L6). C# holds no window, row count,
//     deadline, timer or batch thread: each window domain 7 closes arrives on its
//     batch thread, one at a time, in window order, and every close of an accepted
//     subscription ends with exactly one Closing, empty when idle (d7 L6, L9, L12).
//     On the edge build the call is refused: the shim's NotSupported, plus one
//     sentence naming Eiva.Fletcher.Server (d10 L3, D29).
//
// ── The batch thunk ─────────────────────────────────────────────────────────
// One function per window: MOVE the borrowed Arrow array and schema into structs
// C# allocated (copy each, then NULL the source's release: binding.h, fl_batch),
// import them, copy each row's attachments, call the handler, and count a throw
// (AbsorbedCallbackFailures, HandlerFaulted; d10 L9). The handler OWNS what it is
// handed and may keep it past the call (d10 L8). A window C# cannot take over (out
// of memory, an import refusal) still arrives, as a null batch with its rows added
// to RowsDropped, never silently (d9 L18's rule, one layer up), with no error (ruling
// :669). A window the SHIM could not hand over arrives the same way, carrying the shim's
// failure in BatchStatus.Failure: copied as given, never disposed, for the shim frees it
// after the callback (ruling :551 (3); d10 L3, L8, L9). A dictionary column
// arrives as domain 7 delivers it, re-folded; C# converts nothing (ruling :551 (2)).
//
// ── Lifetime ────────────────────────────────────────────────────────────────
// The thunk's context is a GCHandle to a BatchDelivery. A refused subscribe never
// delivers, so the subscribing frame frees it; once native has accepted, ONLY the
// Closing batch frees it - the subscription's last delivery, exactly once on every
// close path. Unsubscribe and Dispose from outside a handler return after the
// Closing; from inside the handler they return at once and the Closing follows it.
//
// ── A quiet close keeps its topic (D10C-7 D5) ───────────────────────────────
// A close made inside a row delivery on the provider leaves the transport
// subscription open and quiet until the subscriber is disposed or the topic is
// subscribed again (pubsub subscriber.hpp). Its claim then stays, with a count of
// 0: the same kind may take it again; the other kind, which would silently replace
// the quiet subscription, is refused.
using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Threading;

using Apache.Arrow;
using Apache.Arrow.C;

using Eiva.Fletcher.Interop;

namespace Eiva.Fletcher;

/// <summary>Receives one flushed batch.</summary>
/// <param name="batch">
/// The decoded rows, which the handler OWNS, may keep, and disposes. Zero rows when the
/// window held only dropped rows. An idle Closing is empty: zero rows, or NULL if no row has yet
/// carried a schema, so there is nothing to build a batch from (d7 L6). NULL when no batch could be built
/// for this window - no usable schema yet, a schema that cannot be opened (then for the
/// subscription's life), or a window that could not be finished or handed over - with
/// RowsDropped counting the window's rows; a failed hand-over also carries its failure in
/// <see cref="BatchStatus.Failure"/>. Per row (<see cref="SubscriberArrow.Subscribe"/>):
/// NULL when this one row could not be decoded, with RowsDropped = 1.
/// </param>
/// <param name="attachments">Row <c>i</c>'s attachments at index <c>i</c>, owned copies.</param>
/// <param name="status">Why the batch was flushed, and how many rows were lost since the last one.</param>
public delegate void RecordBatchHandler(RecordBatch? batch, IReadOnlyList<AttachmentsBuilder> attachments, BatchStatus status);

/// <summary>Why a batch was flushed.</summary>
public enum BatchReason
{
    /// <summary>
    /// <see cref="BatchOptions.MaxRows"/> was reached; per row, on every call: the window is
    /// the one row offered, delivered or dropped.
    /// </summary>
    RowLimit = 1,

    /// <summary>The window's deadline passed.</summary>
    Timeout = 2,

    /// <summary>The subscription is being torn down.</summary>
    Closing = 3,
}

/// <summary>Why a batch was flushed and whether rows were lost.</summary>
public readonly struct BatchStatus
{
    internal BatchStatus(BatchReason reason, long rowsDropped, FletcherException? failure = null)
    {
        Reason = reason;
        RowsDropped = rowsDropped;
        Failure = failure;
    }

    /// <summary>Why this batch was flushed.</summary>
    public BatchReason Reason { get; }

    /// <summary>Rows lost since the previous flush; 0 means all good.</summary>
    public long RowsDropped { get; }

    /// <summary>
    /// Null, except on a window the server shim could not hand over: then the batch is null, the
    /// window's rows are in <see cref="RowsDropped"/>, and this is the shim's failure, its status,
    /// origin (<see cref="FletcherOrigin.Handover"/>) and message as the shim gave them (d10 L9).
    /// A window C# itself could not take over carries none: its rows are counted, nothing more.
    /// </summary>
    public FletcherException? Failure { get; }
}

/// <summary>How a batched subscription flushes.</summary>
public sealed class BatchOptions
{
    /// <summary>Flush at this many rows. Below 1 is refused by domain 7 (<see cref="FletcherStatus.InvalidArgument"/>).</summary>
    public long MaxRows { get; init; } = 8000;

    /// <summary>
    /// ...or when this long has passed since the previous flush. <see cref="Timeout.InfiniteTimeSpan"/>
    /// for never; a negative one is refused by domain 7 (<see cref="FletcherStatus.InvalidArgument"/>).
    /// </summary>
    public TimeSpan Timeout { get; init; } = TimeSpan.FromMinutes(1);
}

/// <summary>A subscriber that delivers RecordBatches (D-BIND-25).</summary>
public sealed class SubscriberArrow : IDisposable
{
    private readonly Subscriber _subscriber;
    private readonly object _gate = new();
    // D10C-6: the kind each topic is held in (with a count of that kind), and the topic each
    // live subscription holds, so a release is keyed by the subscription and happens once. A
    // count of 0 is a quiet close's topic, still held for its kind (D10C-7 D5).
    private readonly Dictionary<TopicPath, (bool PerRow, int Count)> _claims = [];
    private readonly Dictionary<ulong, TopicPath> _claimed = [];
    private long _absorbed;
    private bool _disposed;

    /// <summary>Create an Arrow subscriber over a provider.</summary>
    public SubscriberArrow(PubSubProviderHandle provider)
    {
        ArgumentNullException.ThrowIfNull(provider);
        _subscriber = new Subscriber(provider);
    }

    /// <summary>Batch handlers that threw, absorbed by this subscriber.</summary>
    public ulong AbsorbedCallbackFailures => (ulong)Interlocked.Read(ref _absorbed);

    /// <summary>Raised for each absorbed batch-handler failure.</summary>
    public event EventHandler<HandlerFaultedEventArgs>? HandlerFaulted;

    /// <summary>Per-row Arrow delivery: each row offered is exactly one handler call (d10 L7, L9; BC-16).</summary>
    /// <remarks>
    /// The native per-row path, with no window, count or timer, so it works on every shim.
    /// A row that decodes arrives on the delivery thread as a one-row batch with its own
    /// attachments; one that cannot be decoded - malformed, schema-less, or of a schema the
    /// codec cannot open - arrives alone as a null batch with RowsDropped = 1 and no
    /// attachments. The reason is <see cref="BatchReason.RowLimit"/> on every call, and
    /// nothing is delivered after <see cref="Unsubscribe"/>.
    /// </remarks>
    /// <exception cref="InvalidOperationException">
    /// This object already holds a <see cref="SubscribeBatched"/> subscription to the topic (ruling
    /// :575 (6)); refused before anything is subscribed.
    /// </exception>
    public SubscribeResult Subscribe(TopicPath topic, RecordBatchHandler handler, TopicOptions? topicOptions = null)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        ArgumentNullException.ThrowIfNull(handler);

        var delivery = new RowDelivery(this, handler);
        SubscribeResult result = SubscribeClaimed(
            topic, perRow: true, () => _subscriber.Subscribe(topic, delivery.OnRow, topicOptions, delivery));
        delivery.SubscriptionId = result.Subscription.Id;
        result.Subscription.Canceller = Unsubscribe;
        return result;
    }

    /// <summary>Batched RecordBatch delivery - the primary shape (D-BIND-25): domain 7's batches.</summary>
    /// <remarks>
    /// Needs the server build of the shim (the Eiva.Fletcher.Server package). The options cross as
    /// given (d10 L3): domain 7 answers a row limit below 1 or a negative timeout. Batches arrive on
    /// domain 7's batch thread, one at a time, in window order; the first may arrive before this
    /// returns. Disposing the returned <see cref="Subscription"/> is <see cref="Unsubscribe"/>.
    /// </remarks>
    /// <exception cref="FletcherException">
    /// <see cref="FletcherStatus.NotSupported"/> on the edge build of the shim, its message naming the
    /// server package; <see cref="FletcherStatus.InvalidArgument"/> for options domain 7 refuses.
    /// Nothing is delivered after a refusal.
    /// </exception>
    /// <exception cref="InvalidOperationException">
    /// This object already holds a per-row <see cref="Subscribe"/> subscription to the topic (ruling
    /// :575 (6)); refused before anything is subscribed.
    /// </exception>
    public unsafe SubscribeResult SubscribeBatched(
        TopicPath topic, RecordBatchHandler handler, BatchOptions? options = null, TopicOptions? topicOptions = null)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        ArgumentNullException.ThrowIfNull(handler);

        options ??= new BatchOptions();
        long maxRows = options.MaxRows;
        long timeoutMs = SchemaArrival.ToNativeMilliseconds(options.Timeout);

        // Built before the claim (D10C-6 DEBT-1). Native holds `self` from the instant it answers OK
        // until its Closing, so this frame frees it only while `accepted` is false (review DEBT-2).
        var delivery = new BatchDelivery(this, handler);
        GCHandle self = GCHandle.Alloc(delivery);
        bool accepted = false;
        SubscribeResult result;
        try
        {
            result = SubscribeClaimed(
                topic,
                perRow: false,
                () => _subscriber.SubscribeBatches(
                    topic, maxRows, timeoutMs, topicOptions,
                    (nint)(delegate* unmanaged[Cdecl]<nint, FlBatch*, void>)&BatchThunk, GCHandle.ToIntPtr(self), ref accepted),
                () => accepted);
        }
        catch when (!accepted)
        {
            self.Free();
            throw;
        }

        delivery.Subscription = result.Subscription;
        result.Subscription.Canceller = Unsubscribe;
        return result;
    }

    /// <summary>The one place a subscription takes, keeps or gives back its topic's claim (ruling :575 (6)).</summary>
    /// <remarks>
    /// The claim comes before the inner subscribe and is released if that throws before native
    /// accepted (<paramref name="accepted"/>; a per-row subscribe passes none); the subscription is
    /// recorded straight after it returns, outside the <c>try</c>, so the claim lasts exactly as long
    /// as the native subscription.
    /// </remarks>
    private SubscribeResult SubscribeClaimed(
        TopicPath topic, bool perRow, Func<SubscribeResult> subscribe, Func<bool>? accepted = null)
    {
        _ = topic.ToKey(); // a defaulted topic is refused here, before it can be claimed
        Claim(topic, perRow);
        SubscribeResult result;
        try
        {
            result = subscribe();
        }
        catch when (accepted?.Invoke() != true)
        {
            Release(topic, quiet: false);
            throw;
        }

        lock (_gate)
        {
            _claimed[result.Subscription.Id] = topic;
        }

        return result;
    }

    /// <summary>Take the topic for one kind, or refuse: the two kinds never share a topic on this object.</summary>
    /// <remarks>
    /// Under the lock and before any subscribe, so two threads racing the kinds see exactly one win.
    /// The same kind again is not refused: only the mix is (L3, ruling :575 (6)). A topic held at a
    /// count of 0 by a quiet close is the same: its kind takes it again, the other is refused (D5).
    /// </remarks>
    private void Claim(TopicPath topic, bool perRow)
    {
        lock (_gate)
        {
            if (!_claims.TryGetValue(topic, out (bool PerRow, int Count) held))
            {
                _claims[topic] = (perRow, 1);
            }
            else if (held.PerRow != perRow)
            {
                throw new InvalidOperationException(
                    $"cannot subscribe to '{topic}' {(perRow ? "per-row" : "batched")}: this SubscriberArrow " +
                    $"already holds a {(held.PerRow ? "per-row" : "batched")} subscription to it, which a later one " +
                    "of the other kind would silently replace. " +
                    (held.Count == 0
                        ? "That one was closed inside a delivery, so its transport subscription stays open until " +
                          "this SubscriberArrow is disposed or the topic is subscribed again per that kind"
                        : "Unsubscribe that one first"));
            }
            else
            {
                _claims[topic] = (perRow, held.Count + 1);
            }
        }
    }

    /// <summary>Give back one subscription's share of its topic; a quiet close keeps the topic at 0 (D5).</summary>
    private void Release(TopicPath topic, bool quiet)
    {
        lock (_gate)
        {
            if (_claims.TryGetValue(topic, out (bool PerRow, int Count) held))
            {
                if (held.Count <= 1 && quiet)
                {
                    _claims[topic] = (held.PerRow, 0);
                }
                else if (held.Count <= 1)
                {
                    _claims.Remove(topic);
                }
                else
                {
                    _claims[topic] = (held.PerRow, held.Count - 1);
                }
            }
        }
    }

    /// <summary>End a subscription of either kind, and give back its topic's claim.</summary>
    /// <remarks>
    /// A batched one ends with its Closing batch (d7 L9): from outside its handler this returns after
    /// the Closing; from inside the handler it returns at once and the Closing follows the handler. A
    /// close made inside a row delivery on this provider leaves the topic held for its kind (D5).
    /// Closing ANOTHER batched subscription from inside a batch handler waits for that one's Closing,
    /// so two handlers that close each other's subscriptions hang (binding.h,
    /// fl_subscriber_unsubscribe_batches).
    /// </remarks>
    public void Unsubscribe(Subscription subscription)
    {
        ArgumentNullException.ThrowIfNull(subscription);

        // Before anything is closed: ids are per-subscriber counters, so another
        // subscriber's subscription #1 would otherwise stop THIS one's #1 (review B9).
        if (!subscription.BelongsTo(_subscriber))
        {
            throw new ArgumentException(
                "the subscription was issued by a different subscriber; cancel it on the one that issued it, " +
                "or dispose it", nameof(subscription));
        }

        // Asked before the close (D5): only a close inside a row delivery leaves the transport quiet.
        bool quiet = _subscriber.InDeliveryOnThisProvider;

        // The ABI's id rule: bit 63 set is a batched id, never a row id (binding.h).
        if ((subscription.Id & _batchedIdBit) != 0)
        {
            _subscriber.UnsubscribeBatches(subscription);
        }
        else
        {
            _subscriber.Unsubscribe(subscription);
        }

        // Only once the close has returned: a failed one keeps the claim, so a retry releases it.
        TopicPath claimed;
        bool held;
        lock (_gate)
        {
            held = _claimed.Remove(subscription.Id, out claimed);
        }

        if (held)
        {
            Release(claimed, quiet);
        }
    }

    private const ulong _batchedIdBit = 1UL << 63;

    /// <summary>Watch a topic's schema without its data - <see cref="Subscriber.SubscribeSchema"/>, forwarded.</summary>
    public SchemaArrival SubscribeSchema(TopicPath topic)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        return _subscriber.SubscribeSchema(topic);
    }

    /// <summary>Release one watch - <see cref="Subscriber.UnsubscribeSchema"/>, forwarded.</summary>
    public void UnsubscribeSchema(TopicPath topic)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        _subscriber.UnsubscribeSchema(topic);
    }

    /// <summary>End every subscription - each batched one with its Closing batch - then tear down.</summary>
    /// <exception cref="InvalidOperationException">
    /// Called from inside a row delivery on this subscriber's provider - refused BEFORE
    /// anything is closed, so a refused Dispose leaves nothing half torn down.
    /// </exception>
    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        if (_subscriber.InDeliveryOnThisProvider)
        {
            throw new InvalidOperationException(
                "a SubscriberArrow cannot be disposed inside a delivery on its own provider: tearing down its " +
                "subscriptions would enter the provider from its own delivery frame. Use DispatchAfterDelivery on " +
                "a Subscriber, or dispose after the handler returns.");
        }

        _disposed = true;
        _subscriber.Dispose();
    }

    private void ReportAbsorbed(Exception exception, ulong subscriptionId)
    {
        Interlocked.Increment(ref _absorbed);
        Diagnostics.CountAbsorbed();

        try
        {
            HandlerFaulted?.Invoke(this, new HandlerFaultedEventArgs(exception, subscriptionId));
        }
        catch (Exception)
        {
            // A listener threw. Counted already; nothing may leave this frame.
            Interlocked.Increment(ref _absorbed);
            Diagnostics.CountAbsorbed();
        }
    }

    /// <summary>A delivery's attachments as an owned copy, empty keys included; a partial copy is disposed and the failure rethrown.</summary>
    private static AttachmentsBuilder CopyAttachments(AttachmentsView view)
    {
        var copy = new AttachmentsBuilder();
        try
        {
            for (int i = 0; i < view.Count; i++)
            {
                copy.AppendDelivered(view.KeyAt(i), view.ValueAt(i));
            }

            return copy;
        }
        catch (Exception)
        {
            copy.Dispose();
            throw;
        }
    }

    /// <summary>One per-row subscription: its handler and its codec decision (d10 L7, L9; BC-16).</summary>
    /// <remarks>
    /// The inner Subscriber disposes it at the subscription's one free, after which no
    /// delivery of it can run, so the codec outlives every decode - an in-handler
    /// Unsubscribe included - with no count of its own (D-BIND-50).
    /// </remarks>
    private sealed class RowDelivery : IDisposable
    {
        private readonly SubscriberArrow _owner;
        private readonly RecordBatchHandler _handler;

        // Under _gate, never held across a decode or the handler.
        private readonly object _gate = new();
        private FletcherCodec? _codec;
        private bool _decided;

        internal RowDelivery(SubscriberArrow owner, RecordBatchHandler handler)
        {
            _owner = owner;
            _handler = handler;
        }

        internal ulong SubscriptionId { get; set; }

        /// <summary>THE classify-and-count site: every row offered reaches exactly one handler call.</summary>
        internal void OnRow(ReadOnlySpan<byte> row, SchemaHandle schema, AttachmentsView view)
        {
            AttachmentsBuilder? attachments = null;
            RecordBatch? batch = null;
            IReadOnlyList<AttachmentsBuilder> delivered = [];
            try
            {
                attachments = CopyAttachments(view);
                FletcherCodec? codec = Codec(schema);
                if (codec is not null)
                {
                    // Domain 2's answer: a bad or trailing byte is refused, never repaired.
                    batch = codec.Decode(row);
                    delivered = [attachments];
                }
            }
            catch (Exception)
            {
                // Refused, unopenable, or an allocation that failed: this row is one drop (D30).
                batch?.Dispose();
                batch = null;
            }

            if (batch is null)
            {
                attachments?.Dispose();
            }

            var status = new BatchStatus(BatchReason.RowLimit, batch is null ? 1 : 0);
            try
            {
                _handler(batch, delivered, status);
            }
            catch (Exception e)
            {
                _owner.ReportAbsorbed(e, SubscriptionId);
            }
        }

        /// <summary>
        /// The codec, decided once by the first row that carries a schema (d7 L8, L13; ruling
        /// :258 (3)): a schema-less row decides nothing; a schema the codec cannot open
        /// decides "none" for the subscription's life.
        /// </summary>
        private FletcherCodec? Codec(SchemaHandle schema)
        {
            lock (_gate)
            {
                if (!_decided && !schema.IsNull)
                {
                    _decided = true;
                    try
                    {
                        _codec = new FletcherCodec(schema.ToArrowSchema());
                    }
                    catch (Exception)
                    {
                        _codec = null;
                    }
                }

                return _codec;
            }
        }

        public void Dispose()
        {
            lock (_gate)
            {
                _decided = true;
                _codec?.Dispose();
                _codec = null;
            }
        }
    }

    /// <summary>
    /// THE batch thunk (D10C-7 D3): move, import, copy the attachments, call, count a throw.
    /// NOTHING may escape it (d10 L10). The only free after an accepted subscribe is here, at
    /// the Closing batch - the subscription's last delivery (binding.h, fl_subscriber_destroy).
    /// </summary>
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static unsafe void BatchThunk(nint ctx, FlBatch* batch)
    {
        try
        {
            GCHandle self = GCHandle.FromIntPtr(ctx);
            try
            {
                ((BatchDelivery)self.Target!).Deliver(batch);
            }
            finally
            {
                if (batch->Reason == (int)BatchReason.Closing)
                {
                    // A racing Dispose can deliver the Closing before the subscribing thread has
                    // set the Subscription (review DEBT-2): retire it only if it is there.
                    ((BatchDelivery)self.Target!).Subscription?.MarkRetired();
                    self.Free();
                }
            }
        }
        catch (Exception)
        {
            // Bookkeeping failed - nowhere to report it, and nothing may leave this frame.
        }
    }

    /// <summary>One batched subscription: its handler, and its Subscription once the subscribe returned.</summary>
    private sealed class BatchDelivery
    {
        private readonly SubscriberArrow _owner;
        private readonly RecordBatchHandler _handler;
        private Subscription? _subscription;

        internal BatchDelivery(SubscriberArrow owner, RecordBatchHandler handler)
        {
            _owner = owner;
            _handler = handler;
        }

        /// <summary>Set by the subscribing thread; a batch may arrive before it is (residue b).</summary>
        internal Subscription? Subscription
        {
            get => Volatile.Read(ref _subscription);
            set => Volatile.Write(ref _subscription, value);
        }

        /// <summary>Take one window over and hand it to the handler, which then owns it.</summary>
        /// <remarks>
        /// The array and schema are MOVED out of the shim's borrowed structs before they are
        /// imported (copy, then NULL the source's release), so nothing the handler keeps points
        /// into memory the shim reclaims when this returns (d10 L8). A window that cannot be taken
        /// over arrives as no batch, its rows counted as dropped (residue a); a null array is
        /// domain 7's (or the shim's) "no batch", passed through as given.
        /// </remarks>
        internal unsafe void Deliver(FlBatch* source)
        {
            // Nothing that can fail runs before the take-over's `try`: a failure anywhere in it, out
            // of memory included, still reaches the handler as this window, a null batch with its
            // rows counted (residue (a); d10 L9) - a Closing too.
            RecordBatch? batch = null;
            IReadOnlyList<AttachmentsBuilder> delivered = System.Array.Empty<AttachmentsBuilder>();
            List<AttachmentsBuilder>? attachments = null;
            long dropped = source->RowsDropped;
            if (source->Array != 0)
            {
                CArrowArray* array = null;
                CArrowSchema* schema = null;
                try
                {
                    // The move's NULL goes where Apache.Arrow's own struct keeps `release` (Arrow
                    // C Data Interface, "Moving an array"); the field is internal to the library.
                    nint arrayRelease = Marshal.OffsetOf<CArrowArray>("release");
                    nint schemaRelease = Marshal.OffsetOf<CArrowSchema>("release");
                    array = CArrowArray.Create();
                    schema = CArrowSchema.Create();
                    attachments = new List<AttachmentsBuilder>(checked((int)source->NumRows));
                    *array = *(CArrowArray*)source->Array;
                    *(nint*)((byte*)source->Array + arrayRelease) = 0;
                    *schema = *(CArrowSchema*)source->Schema;
                    *(nint*)((byte*)source->Schema + schemaRelease) = 0;

                    Schema imported = CArrowSchemaImporter.ImportSchema(schema);
                    batch = CArrowArrayImporter.ImportRecordBatch(array, imported);
                    nint* sets = (nint*)source->Attachments;
                    for (long i = 0; i < source->NumRows; i++)
                    {
                        attachments.Add(CopyAttachments(new AttachmentsView(sets[i])));
                    }

                    delivered = attachments;
                }
                catch (Exception)
                {
                    batch?.Dispose();
                    batch = null;
                    foreach (AttachmentsBuilder copied in attachments ?? [])
                    {
                        copied.Dispose();
                    }

                    dropped += source->NumRows;
                }
                finally
                {
                    // Only what was allocated; each releases its struct only if the import has not
                    // moved it out, so there is one release.
                    if (schema != null)
                    {
                        CArrowSchema.Free(schema);
                    }

                    if (array != null)
                    {
                        CArrowArray.Free(array);
                    }
                }
            }

            // A failed hand-over's failure, copied as given (d10 L3) and never disposed: it is
            // borrowed for this call, and the shim frees it when the call returns (d10 L8). A copy
            // that throws (out of memory) costs the window only its failure (brief D10C-10, "Handled"; DEBT-3).
            FletcherException? failure = null;
            if (source->Failure != 0)
            {
                try
                {
                    failure = Errors.ToException(((FlError*)source->Failure)->Status, in *(FlError*)source->Failure);
                }
                catch (Exception)
                {
                    // Delivered without its failure; its rows are already counted.
                }
            }

            var status = new BatchStatus((BatchReason)source->Reason, dropped, failure);
            try
            {
                _handler(batch, delivered, status);
            }
            catch (Exception e)
            {
                _owner.ReportAbsorbed(e, Subscription?.Id ?? 0);
            }
        }
    }
}
