// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// BIND-5a: the Arrow tier's subscriber, batch-first (D-BIND-25, D-BIND-59).
//
// ── What it is, in terms of what is below it ────────────────────────────────
// A Subscriber whose handler COPIES each borrowed row and its attachments into
// a pending window, and a batcher that decodes the window into one RecordBatch
// per flush - N rows per native call - and hands it to the caller's handler.
// It mirrors C++ `SubscriberArrow`'s batched Subscribe, rule for rule:
//
//   * a window flushes at MaxRows (RowLimit), when its deadline passes
//     (Timeout), or on Unsubscribe/Dispose (Closing);
//   * the deadline is armed by the window's FIRST event, row or drop, and
//     anchored to the PREVIOUS flush: a steady stream flushes every Timeout, and
//     a stream idle longer than one window is delivered at once;
//   * a row that fails to decode is counted in RowsDropped and contributes
//     neither a row nor an attachment - its attachment's identity lived in it;
//   * a window of only dropped rows still delivers a zero-row batch, so the loss
//     is reported rather than silent;
//   * a schema the codec cannot open drops every row for the subscription's
//     life, and each report carries a NULL batch;
//   * every row that arrives either reaches a batch or is counted in
//     RowsDropped (D-BIND-66): the intake and the flush contain their own
//     failures, as C++'s AddRow does, and none reaches the inner Subscriber or
//     the timer thread;
//   * a window never holds more than it can decode (D-BIND-63): a row that would
//     take it past the ceiling flushes it first, as RowLimit, and a row that
//     alone exceeds the ceiling is dropped and counted.
//
// ── The ceiling, and how it differs from C++'s ─────────────────────────────
// C++'s batcher flushes early when an append would overflow an Arrow 32-bit
// offset: 2^31-2 bytes (or elements) in one column's builder
// (`BatchCapacityExceeded`). This batcher holds RAW rows until flush, so it
// bounds the raw window instead - which bounds every column's value bytes too,
// since a column's bytes are a subset of the rows'. The number is C++'s, lowered
// to `Array.MaxLength` (55 bytes less), the most one managed buffer can hold. So
// the same topic splits at the same place give or take the rows' framing bytes,
// never later than C++ would. What it does NOT bound, as C++ does, is ELEMENT
// counts: a list of bit-packed booleans can hold 2^31 elements in 256 MiB. That
// window fails to decode and is dropped whole, and reported - not silent.
//
// Per-row delivery is the same thing with MaxRows = 1 (D-BIND-59): a row
// arrives as a one-row batch, flushed on the delivery thread as it lands.
//
// ── Decoding N rows at a time, and a corrupt one among them ─────────────────
// The window is decoded with ONE fl_decode_rows call. The format is
// self-delimiting, so a corrupt row fails the whole call; only then is each row
// decoded alone to find the bad ones, which are counted dropped, and the good
// ones are decoded together again. The common case is one native call per
// batch; the failure case costs a second pass and keeps attachments aligned.
//
// ── Threads, and what a handler may assume (D-BIND-64) ─────────────────────
// A RowLimit flush runs on the transport's delivery thread (inside the
// delivery, so the seam's re-entrancy rules bind the handler there), a Timeout
// flush on a timer thread, a Closing flush on the thread that unsubscribes.
// Handler calls are NOT serialised, exactly as in C++: those three may run the
// handler at once, and batches may complete out of order. Each window is still
// cut in order, under the state lock, so no row is in two batches and none is
// skipped; a handler must be thread-safe. (5a held a lock across the handler to
// promise one call at a time. It deadlocked: a timer-thread handler that
// cancelled waited, in the native drain, for a delivery that was itself waiting
// for that lock.)
//
// What IS promised: no handler call runs after Unsubscribe or Dispose returns.
// A delivery-thread flush is covered by the native cancel, which drains the
// delivery it runs in; a timer flush on another thread is waited for by Stop,
// as C++'s Stop joins its timer thread - unless Stop runs ON that flush, as a
// handler that cancels itself does. A limit shared with C++: a delivery-thread
// handler that cancels while a timer-thread handler also cancels can still
// deadlock, each cancel waiting for the other's flush.
//
// THE HANDLER OWNS WHAT IT IS HANDED. The batch is the codec's, handed over as
// FletcherCodec.DecodeBatch hands it - the caller disposes it - and each
// AttachmentsBuilder is a managed copy it may keep, read or republish with. A
// batch disposed after the handler returned would make a kept reference a
// use-after-free; one the caller forgets is only a leak.
//
// A handler that throws is absorbed, counted in AbsorbedCallbackFailures and
// Diagnostics.AbsorbedTotal, and raised as HandlerFaulted (D-BIND-19 rule 5):
// none of the three threads may be taken down by user code.
//
// ── Dictionaries ────────────────────────────────────────────────────────────
// Delivered as their VALUE type (FletcherCodec.DecodedSchema): re-folding into a
// DictionaryArray is deferred to the DICT round (D-BIND-8). C++'s BatchDecoder
// re-folds; that difference is by ruling.
using System;
using System.Buffers;
using System.Collections.Generic;
using System.Diagnostics;
using System.Threading;

using Apache.Arrow;

namespace Eiva.Fletcher;

/// <summary>Receives one flushed batch.</summary>
/// <param name="batch">
/// The decoded rows, which the handler OWNS and disposes. Zero rows when the window
/// held only dropped rows; NULL only when the topic's schema could not be opened, in
/// which case every row is dropped for the life of the subscription.
/// </param>
/// <param name="attachments">Row <c>i</c>'s attachments at index <c>i</c>, owned copies.</param>
/// <param name="status">Why the batch was flushed, and how many rows were lost since the last one.</param>
public delegate void RecordBatchHandler(RecordBatch? batch, IReadOnlyList<AttachmentsBuilder> attachments, BatchStatus status);

/// <summary>Why a batch was flushed.</summary>
public enum BatchReason
{
    /// <summary><see cref="BatchOptions.MaxRows"/> was reached.</summary>
    RowLimit,

    /// <summary>The window's deadline passed.</summary>
    Timeout,

    /// <summary>The subscription is being torn down.</summary>
    Closing,
}

/// <summary>Why a batch was flushed and whether rows were lost.</summary>
public readonly struct BatchStatus
{
    internal BatchStatus(BatchReason reason, long rowsDropped)
    {
        Reason = reason;
        RowsDropped = rowsDropped;
    }

    /// <summary>Why this batch was flushed.</summary>
    public BatchReason Reason { get; }

    /// <summary>Rows lost since the previous flush; 0 means all good.</summary>
    public long RowsDropped { get; }
}

/// <summary>How a batched subscription flushes.</summary>
public sealed class BatchOptions
{
    /// <summary>Flush at this many rows. Values below 1 mean 1.</summary>
    public long MaxRows { get; init; } = 8000;

    /// <summary>...or when this long has passed since the previous flush. <see cref="Timeout.InfiniteTimeSpan"/> for never.</summary>
    public TimeSpan Timeout { get; init; } = TimeSpan.FromMinutes(1);
}

/// <summary>A subscriber that delivers RecordBatches (D-BIND-25).</summary>
public sealed class SubscriberArrow : IDisposable
{
    /// <summary>
    /// The most one window may hold, in raw row bytes (D-BIND-63): C++'s Arrow
    /// builder limit, 2^31-2, lowered to what one managed buffer can hold.
    /// </summary>
    internal static readonly long DefaultWindowByteCeiling = Math.Min((1L << 31) - 2, System.Array.MaxLength);

    private readonly Subscriber _subscriber;
    private readonly object _gate = new();
    private readonly Dictionary<ulong, Batcher> _batchers = [];
    private readonly long _windowByteCeiling;
    private long _absorbed;
    private bool _disposed;

    /// <summary>Create an Arrow subscriber over a provider.</summary>
    public SubscriberArrow(PubSubProviderHandle provider)
        : this(provider, DefaultWindowByteCeiling)
    {
    }

    /// <summary>
    /// With a lower window ceiling, so the split can be tested without
    /// allocating gigabytes. Not public: the ceiling is C++'s, not a knob.
    /// </summary>
    internal SubscriberArrow(PubSubProviderHandle provider, long windowByteCeiling)
    {
        ArgumentNullException.ThrowIfNull(provider);
        ArgumentOutOfRangeException.ThrowIfLessThan(windowByteCeiling, 1);
        ArgumentOutOfRangeException.ThrowIfGreaterThan(windowByteCeiling, DefaultWindowByteCeiling);
        _subscriber = new Subscriber(provider);
        _windowByteCeiling = windowByteCeiling;
    }

    /// <summary>Batch handlers that threw, absorbed by this subscriber.</summary>
    public ulong AbsorbedCallbackFailures => (ulong)Interlocked.Read(ref _absorbed);

    /// <summary>Raised for each absorbed batch-handler failure.</summary>
    public event EventHandler<HandlerFaultedEventArgs>? HandlerFaulted;

    /// <summary>Per-row Arrow delivery: every row arrives as a one-row batch (D-BIND-59).</summary>
    /// <remarks>
    /// Exactly <see cref="SubscribeBatched"/> with <see cref="BatchOptions.MaxRows"/> = 1:
    /// each row is flushed on the delivery thread as it lands.
    /// </remarks>
    public SubscribeResult Subscribe(TopicPath topic, RecordBatchHandler handler, TopicOptions? topicOptions = null)
        => SubscribeBatched(topic, handler, new BatchOptions { MaxRows = 1 }, topicOptions);

    /// <summary>Batched RecordBatch delivery - the primary shape (D-BIND-25).</summary>
    /// <exception cref="ArgumentOutOfRangeException">
    /// <see cref="BatchOptions.Timeout"/> is negative and not <see cref="Timeout.InfiniteTimeSpan"/>.
    /// </exception>
    /// <remarks>
    /// Disposing the returned <see cref="Subscription"/> is <see cref="Unsubscribe"/>:
    /// the pending window is flushed with <see cref="BatchReason.Closing"/> first.
    /// </remarks>
    public SubscribeResult SubscribeBatched(
        TopicPath topic, RecordBatchHandler handler, BatchOptions? options = null, TopicOptions? topicOptions = null)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        ArgumentNullException.ThrowIfNull(handler);

        options ??= new BatchOptions();
        if (options.Timeout < TimeSpan.Zero && options.Timeout != Timeout.InfiniteTimeSpan)
        {
            throw new ArgumentOutOfRangeException(
                nameof(options), options.Timeout,
                "a negative Timeout is refused: use Timeout.InfiniteTimeSpan for a window with no deadline");
        }

        int maxRows = (int)Math.Clamp(options.MaxRows, 1, int.MaxValue);
        var batcher = new Batcher(this, handler, maxRows, options.Timeout, _windowByteCeiling);

        SubscribeResult result = _subscriber.Subscribe(topic, batcher.OnRow, topicOptions);
        batcher.SubscriptionId = result.Subscription.Id;
        result.Subscription.Canceller = Unsubscribe;

        lock (_gate)
        {
            _batchers[result.Subscription.Id] = batcher;
        }

        return result;
    }

    /// <summary>Flush the subscription's pending window as <see cref="BatchReason.Closing"/>, then cancel it.</summary>
    /// <remarks>
    /// That order is C++'s: the handler sees the last partial window before the
    /// data subscription goes, and rows that land in between are ignored. When it
    /// returns, no handler call for the subscription is still running (D-BIND-64).
    /// Called from inside the subscription's own handler it is served: the closing
    /// flush delivers whatever the window holds by then, which can call the handler
    /// again, nested, as C++ does.
    /// </remarks>
    public void Unsubscribe(Subscription subscription)
    {
        ArgumentNullException.ThrowIfNull(subscription);

        Batcher? batcher;
        lock (_gate)
        {
            _batchers.Remove(subscription.Id, out batcher);
        }

        batcher?.Stop();
        _subscriber.Unsubscribe(subscription);
    }

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

    /// <summary>Flush every pending window as <see cref="BatchReason.Closing"/>, then tear down.</summary>
    /// <exception cref="InvalidOperationException">
    /// Called from inside a delivery on this subscriber's provider - refused BEFORE
    /// any window is flushed, so a refused Dispose leaves nothing half torn down.
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

        List<Batcher> batchers;
        lock (_gate)
        {
            batchers = [.. _batchers.Values];
            _batchers.Clear();
        }

        foreach (Batcher batcher in batchers)
        {
            batcher.Stop();
        }

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

    /// <summary>One subscription's pending window, its deadline and its codec.</summary>
    private sealed class Batcher
    {
        private static readonly long MinWindowTicks = Stopwatch.Frequency / 1000;

        private readonly SubscriberArrow _owner;
        private readonly RecordBatchHandler _handler;
        private readonly int _maxRows;
        private readonly TimeSpan _timeout;
        private readonly long _byteCeiling;

        // State, under _gate. Never held while the handler runs.
        private readonly object _gate = new();
        private ArrayBufferWriter<byte> _rows = new();
        private List<int> _ends = [];
        private List<AttachmentsBuilder> _attachments = [];
        private long _dropped;
        private FletcherCodec? _codec;
        private bool _undecodable;
        private Timer? _timer;
        private bool _hasDeadline;
        private long _deadline;
        private long _lastFlush = Stopwatch.GetTimestamp();
        private bool _stopped;

        // Flushes past their cut and not yet returned. The codec is disposed by
        // whichever of Stop and the last of these comes last, so a flush on another
        // thread never decodes with a disposed codec - and nobody waits for one.
        private int _activeFlushes;

        // The managed thread running a Timeout flush, or 0. Stop waits for it to
        // finish unless Stop is running on it (D-BIND-64).
        private int _timerFlushThread;

        internal Batcher(SubscriberArrow owner, RecordBatchHandler handler, int maxRows, TimeSpan timeout, long byteCeiling)
        {
            _owner = owner;
            _handler = handler;
            _maxRows = maxRows;
            _timeout = timeout;
            _byteCeiling = byteCeiling;
        }

        internal ulong SubscriptionId { get; set; }

        /// <summary>The Subscriber's handler: copy the borrowed row, and flush at the row limit.</summary>
        internal void OnRow(ReadOnlySpan<byte> row, SchemaHandle schema, AttachmentsView view)
        {
            // The intake owns its failures (D-BIND-66): a row it cannot copy is a
            // counted drop, never an exception into the inner Subscriber, which would
            // absorb it where no count of this tier sees it.
            AttachmentsBuilder? attachments = CopyAttachments(view);

            bool full;
            while (true)
            {
                lock (_gate)
                {
                    if (_stopped)
                    {
                        return;
                    }

                    if (_codec is null && !_undecodable)
                    {
                        Resolve(schema);
                    }

                    // Alone past the ceiling: no window could decode it, so it is
                    // dropped and counted - C++'s "the row alone exceeds the budget".
                    // A row whose attachments could not be copied goes the same way.
                    if (_undecodable || row.Length > _byteCeiling || attachments is null)
                    {
                        _dropped++;
                        ArmDeadline();
                        return;
                    }

                    // Fits once the pending window is out of the way: flush it first,
                    // outside the gate, then look again - another delivery may have
                    // added to the new window in between (D-BIND-63).
                    if (_rows.WrittenCount + (long)row.Length <= _byteCeiling)
                    {
                        try
                        {
                            row.CopyTo(_rows.GetSpan(row.Length));
                        }
                        catch (Exception)
                        {
                            // The copy is the only allocation that grows with the data;
                            // a failed one is a lost row, reported as one - never an
                            // absorbed handler fault (D-BIND-63 rule 4, D-BIND-66).
                            _dropped++;
                            ArmDeadline();
                            return;
                        }

                        _rows.Advance(row.Length);
                        _ends.Add(_rows.WrittenCount);
                        _attachments.Add(attachments);
                        ArmDeadline();
                        full = _ends.Count >= _maxRows;
                        break;
                    }
                }

                Flush(BatchReason.RowLimit);
            }

            if (full)
            {
                Flush(BatchReason.RowLimit);
            }
        }

        /// <summary>The delivery's attachments as owned copies, or null if they could not be copied.</summary>
        private static AttachmentsBuilder? CopyAttachments(AttachmentsView view)
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
                return null;
            }
        }

        /// <summary>The codec, from the first delivery's schema - so a subscriber that subscribed first still decodes.</summary>
        private void Resolve(SchemaHandle schema)
        {
            try
            {
                _codec = schema.IsNull ? null : new FletcherCodec(schema.ToArrowSchema());
            }
            catch (Exception)
            {
                // Not only the codec's own refusal: Apache.Arrow's importer refuses
                // with types of its own. Any of them means this schema cannot be
                // decoded, reported as such, rather than a row lost on every delivery
                // with nothing counted (BIND-5 review B6).
                _codec = null;
            }

            _undecodable = _codec is null;
        }

        /// <summary>Armed by the window's first event; anchored to the previous flush.</summary>
        private void ArmDeadline()
        {
            if (_hasDeadline || _timeout == Timeout.InfiniteTimeSpan)
            {
                return;
            }

            long now = Stopwatch.GetTimestamp();
            long anchored = _lastFlush + (long)(_timeout.TotalSeconds * Stopwatch.Frequency);
            _deadline = Math.Max(now + MinWindowTicks, anchored);
            _hasDeadline = true;

            _timer ??= new Timer(static state => ((Batcher)state!).OnDeadline(), this, System.Threading.Timeout.Infinite, System.Threading.Timeout.Infinite);
            _timer.Change(DueIn(now), System.Threading.Timeout.InfiniteTimeSpan);
        }

        private TimeSpan DueIn(long now) =>
            TimeSpan.FromSeconds(Math.Max(0, _deadline - now) / (double)Stopwatch.Frequency);

        private void OnDeadline()
        {
            lock (_gate)
            {
                if (_stopped || !_hasDeadline)
                {
                    return;
                }

                long now = Stopwatch.GetTimestamp();
                if (now < _deadline)
                {
                    // Woken early (timer granularity): go back to sleep for the rest.
                    _timer?.Change(DueIn(now), System.Threading.Timeout.InfiniteTimeSpan);
                    return;
                }

                _timerFlushThread = Environment.CurrentManagedThreadId;
            }

            try
            {
                Flush(BatchReason.Timeout);
            }
            finally
            {
                lock (_gate)
                {
                    _timerFlushThread = 0;
                    Monitor.PulseAll(_gate);
                }
            }
        }

        /// <summary>Stop taking rows and deliver the pending window as Closing. Idempotent.</summary>
        internal void Stop()
        {
            lock (_gate)
            {
                if (_stopped)
                {
                    return;
                }

                _stopped = true;
            }

            // The window as it stands goes out as Closing. A flush already running on
            // another thread keeps what it cut; nothing waits for it here.
            Flush(BatchReason.Closing);

            lock (_gate)
            {
                _timer?.Dispose();
                _timer = null;

                // A Timeout flush on another thread may still be in its handler: C++'s
                // Stop joins its timer thread for the same reason. Not when this IS
                // that flush - a handler cancelling itself would wait for itself.
                int self = Environment.CurrentManagedThreadId;
                while (_timerFlushThread != 0 && _timerFlushThread != self)
                {
                    Monitor.Wait(_gate);
                }

                DisposeCodecIfIdle();
            }
        }

        /// <summary>Under the gate: the codec goes once stopped and no flush still holds it.</summary>
        private void DisposeCodecIfIdle()
        {
            // Every batch already handed out was imported on its own, so nothing
            // delivered holds on to the codec - only a flush still decoding does.
            if (_stopped && _activeFlushes == 0)
            {
                _codec?.Dispose();
                _codec = null;
            }
        }

        private void Flush(BatchReason reason)
        {
            ArrayBufferWriter<byte> rows;
            int[] ends;
            List<AttachmentsBuilder> attachments;
            long dropped;
            FletcherCodec? codec;

            // The cut, under the gate: this is what keeps windows in order and whole
            // while their handlers run concurrently (D-BIND-64).
            lock (_gate)
            {
                _hasDeadline = false;
                _lastFlush = Stopwatch.GetTimestamp();

                if (_ends.Count == 0 && _dropped == 0)
                {
                    return;
                }

                // Swapped out, not copied: the window's own buffer goes to the decode, so
                // a flush allocates no second copy of up to ~2 GiB under the gate
                // (review M-D1), and the cut itself cannot fail on one (review B5).
                rows = _rows;
                ends = [.. _ends];
                attachments = _attachments;
                dropped = _dropped;
                codec = _codec;

                _rows = new ArrayBufferWriter<byte>();
                _ends = [];
                _attachments = [];
                _dropped = 0;
                _activeFlushes++;
            }

            try
            {
                Deliver(reason, codec, rows, ends, attachments, dropped);
            }
            finally
            {
                lock (_gate)
                {
                    _activeFlushes--;
                    DisposeCodecIfIdle();
                }
            }
        }

        /// <summary>Decode one cut window and hand it to the handler, with no lock held.</summary>
        private void Deliver(
            BatchReason reason, FletcherCodec? codec, ArrayBufferWriter<byte> rows, int[] ends, List<AttachmentsBuilder> attachments, long dropped)
        {
            RecordBatch? batch = null;
            List<AttachmentsBuilder> kept = attachments;
            if (codec is not null)
            {
                try
                {
                    batch = Decode(codec, rows.WrittenMemory, ends, attachments, ref dropped, out kept);
                }
                catch (Exception)
                {
                    // Whatever failed - the decode, its fallback's allocation, the import -
                    // the window's rows are lost, and counted as lost (D-BIND-66). Rows the
                    // fallback already counted are in `dropped`; the rest are `kept`.
                    dropped += kept.Count;
                    kept = [];
                    batch = EmptyBatch(codec);
                }
            }

            try
            {
                _handler(batch, kept, new BatchStatus(reason, dropped));
            }
            catch (Exception e)
            {
                _owner.ReportAbsorbed(e, SubscriptionId);
            }
        }

        /// <summary>
        /// A zero-row batch: what a decodable topic's window of only lost rows
        /// delivers. NULL stays reserved for a schema that cannot be opened at all
        /// (review B7); only if even this fails does the handler see NULL.
        /// </summary>
        private static RecordBatch? EmptyBatch(FletcherCodec codec)
        {
            try
            {
                return codec.DecodeBatch(ReadOnlySpan<byte>.Empty, 0);
            }
            catch (Exception)
            {
                return null;
            }
        }

        /// <summary>One native call for the window; a second pass only when a row in it is corrupt.</summary>
        private static RecordBatch Decode(
            FletcherCodec codec, ReadOnlyMemory<byte> window, int[] ends, List<AttachmentsBuilder> attachments,
            ref long dropped, out List<AttachmentsBuilder> kept)
        {
            ReadOnlySpan<byte> rows = window.Span;
            try
            {
                kept = attachments;
                return codec.DecodeBatch(rows, ends.Length);
            }
            catch (FletcherFormatException)
            {
                var good = new ArrayBufferWriter<byte>(rows.Length);
                kept = new List<AttachmentsBuilder>(ends.Length);
                int start = 0;
                for (int i = 0; i < ends.Length; i++)
                {
                    ReadOnlySpan<byte> one = rows.Slice(start, ends[i] - start);
                    start = ends[i];
                    try
                    {
                        codec.DecodeBatch(one, 1).Dispose();
                    }
                    catch (FletcherFormatException)
                    {
                        dropped++;
                        continue;
                    }

                    one.CopyTo(good.GetSpan(one.Length));
                    good.Advance(one.Length);
                    kept.Add(attachments[i]);
                }

                return codec.DecodeBatch(good.WrittenSpan, kept.Count);
            }
        }
    }
}
