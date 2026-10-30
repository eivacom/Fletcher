// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// BIND-8f: the gateway client's batched subscription (D-BIND-133).
//
// ── What it mirrors ─────────────────────────────────────────────────────────
// The native SubscriberArrow.SubscribeBatched (D-BIND-25) and C++'s batched
// SubscriberArrow::Subscribe, with their names and defaults, in this package's own types
// (it cannot reference Eiva.Fletcher, D-BIND-120):
//
//   * a window flushes at MaxRows (RowLimit), when its deadline passes (Timeout), or on
//     UnsubscribeAsync, DisposeAsync or the end of the connection (Closing);
//   * the deadline is armed by the window's FIRST event, row or drop, and anchored to the
//     PREVIOUS flush: a steady stream flushes every Timeout, and a row after an idle spell
//     longer than one window is delivered at once;
//   * a row that fails to decode is counted in RowsDropped and contributes neither a row nor
//     an envelope; a window of only dropped rows still delivers a zero-row batch;
//   * a window never holds more than it can decode (D-BIND-63): a row that would take it past
//     the ceiling flushes it first, as RowLimit, and a row that alone exceeds it is dropped.
//
// ── Where it differs, by ruling ─────────────────────────────────────────────
// MaxRows counts rows RECEIVED, not rows decoded: a window with a bad row delivers one row
// fewer rather than waiting for one more (native puts the good rows back). And one
// subscription's handler never runs concurrently with itself: a cut window goes into a
// queue, and whichever thread finds no one delivering drains it, one handler call at a time,
// with no lock held across the handler (native lets its three threads run the handler at
// once, D-BIND-64). A RowLimit window is cut on the receive loop, a Timeout window on a timer
// thread, a Closing window on the thread that unsubscribes or ends the connection; each is
// delivered by that thread unless another is already delivering for the subscription.
//
// ── Decoding ────────────────────────────────────────────────────────────────
// The window keeps each row as received (the envelope copied out of the receive buffer, so
// the handler may keep it) and decodes them in one DecodeBatch at the flush. A window that
// fails is decoded again row by row to find the bad rows, and the good ones are decoded
// together, so a bad row costs only its own window a second pass.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Threading;

using Apache.Arrow;

namespace Eiva.Fletcher.GatewayClient;

/// <summary>Receives one flushed window of a batched subscription (D-BIND-133).</summary>
/// <param name="batch">
/// The window's rows, decoded over the subscription's schema (dictionary fields as their value
/// type). The handler OWNS it and disposes it. Zero rows when the window held only dropped rows.
/// </param>
/// <param name="envelopes">
/// Row <c>i</c>'s envelope at index <c>i</c>: its encoded bytes and its attachments. OWNED copies,
/// unlike <see cref="GatewayRowHandler"/>'s borrowed one: the handler may keep them.
/// </param>
/// <param name="status">Why the window was flushed, and how many rows it dropped.</param>
/// <remarks>
/// <para>
/// One subscription's handler is never called concurrently with itself, and its windows arrive
/// in order; it may be called on the receive loop, a timer thread, or the thread that
/// unsubscribes or ends the connection. While it runs on the receive loop it holds the loop, so
/// the rules of <see cref="GatewayRowHandler"/> apply: keep it short, and do not BLOCK on a
/// call to the same client.
/// </para>
/// <para>
/// <b>The handler is synchronous; do not pass an <c>async</c> lambda (D-BIND-125).</b> Hand what
/// you need to your own queue instead. A handler that throws is counted and reported through
/// <see cref="FletcherClient.HandlerFaulted"/>; the subscription goes on.
/// </para>
/// </remarks>
public delegate void GatewayBatchHandler(RecordBatch batch, IReadOnlyList<Envelope> envelopes, GatewayBatchStatus status);

/// <summary>Why a window was flushed.</summary>
public enum GatewayBatchReason
{
    /// <summary><see cref="GatewayBatchOptions.MaxRows"/> was reached, or the window's byte ceiling would have been passed.</summary>
    RowLimit,

    /// <summary>The window's deadline passed.</summary>
    Timeout,

    /// <summary>The subscription is ending: unsubscribed, disposed, or the connection ended.</summary>
    Closing,
}

/// <summary>Why a window was flushed and whether rows were lost.</summary>
public readonly struct GatewayBatchStatus
{
    internal GatewayBatchStatus(GatewayBatchReason reason, long rowsDropped)
    {
        Reason = reason;
        RowsDropped = rowsDropped;
    }

    /// <summary>Why this window was flushed.</summary>
    public GatewayBatchReason Reason { get; }

    /// <summary>Rows lost since the previous flush; 0 means all good.</summary>
    public long RowsDropped { get; }
}

/// <summary>How a batched subscription flushes; the native <c>BatchOptions</c>'s members and defaults.</summary>
public sealed class GatewayBatchOptions
{
    /// <summary>
    /// The most one window may hold, in raw row bytes (D-BIND-63): C++'s Arrow builder limit,
    /// 2^31-2, lowered to what one managed buffer can hold.
    /// </summary>
    internal static readonly long DefaultWindowByteCeiling = Math.Min((1L << 31) - 2, System.Array.MaxLength);

    /// <summary>Flush at this many rows received. Values below 1 mean 1.</summary>
    public long MaxRows { get; init; } = 8000;

    /// <summary>
    /// ...or when this long has passed since the previous flush. <see cref="System.Threading.Timeout.InfiniteTimeSpan"/>
    /// for never; otherwise zero up to <see cref="MaxTimeout"/>.
    /// </summary>
    public TimeSpan Timeout { get; init; } = TimeSpan.FromMinutes(1);

    /// <summary>The longest finite <see cref="Timeout"/>: the most a timer can wait, about 49.7 days.</summary>
    public static TimeSpan MaxTimeout { get; } = TimeSpan.FromMilliseconds(uint.MaxValue - 1);

    /// <summary>The window's byte ceiling; lowered by tests only.</summary>
    internal long WindowByteCeiling { get; init; } = DefaultWindowByteCeiling;

    internal void Validate()
    {
        if (Timeout != System.Threading.Timeout.InfiniteTimeSpan && (Timeout < TimeSpan.Zero || Timeout > MaxTimeout))
        {
            throw new ArgumentOutOfRangeException(
                "options", Timeout,
                "a Timeout must be between zero and GatewayBatchOptions.MaxTimeout (about 49.7 days, the most a timer can " +
                "wait): use Timeout.InfiniteTimeSpan for a window with no deadline");
        }
    }
}

// One batched subscription's window, its timer and its delivery queue.
internal sealed class GatewayBatcher
{
    private readonly FletcherClient _client;
    private readonly ulong _id;
    private readonly GatewayRowCodec _codec;
    private readonly GatewayBatchHandler _handler;
    private readonly int _maxRows;
    private readonly TimeSpan _timeout;
    private readonly long _ceiling;
    private readonly object _lock = new();
    private readonly Queue<Window> _queue = new();

    private List<ReadOnlyMemory<byte>> _rows = new();
    private List<Envelope> _envelopes = new();
    private long _bytes;
    private long _dropped;
    private bool _armed;
    private long _lastFlush = Stopwatch.GetTimestamp();
    private Timer? _timer;
    private bool _closed;
    private bool _draining;

    public GatewayBatcher(FletcherClient client, ulong id, GatewayRowCodec codec, GatewayBatchHandler handler, GatewayBatchOptions options)
    {
        _client = client;
        _id = id;
        _codec = codec;
        _handler = handler;
        _maxRows = (int)Math.Clamp(options.MaxRows, 1, int.MaxValue);
        _timeout = options.Timeout;
        _ceiling = options.WindowByteCeiling;
    }

    /// <summary>A row for this subscription, its envelope lent from the receive buffer; called on the receive loop.</summary>
    public void Add(ReadOnlyMemory<byte> envelopeBytes)
    {
        Envelope envelope;
        try
        {
            envelope = Envelope.Deserialize(envelopeBytes.ToArray());
        }
        catch (Exception e)
        {
            _client.OnRowUndecodable(e, _id);
            Drop();
            return;
        }

        if (envelope.Row.Length > _ceiling)
        {
            _client.OnRowUndecodable(new System.IO.InvalidDataException("FletcherClient: a row of " + envelope.Row.Length +
                " bytes is larger than a batch window can hold (" + _ceiling + " bytes)"), _id);
            Drop();
            return;
        }

        lock (_lock)
        {
            if (_closed)
            {
                _client.OnRowDropped();
                return;
            }

            if (_rows.Count > 0 && _bytes + envelope.Row.Length > _ceiling)
            {
                Cut(GatewayBatchReason.RowLimit);
            }

            bool due = Arm();
            _rows.Add(envelope.Row);
            _envelopes.Add(envelope);
            _bytes += envelope.Row.Length;
            if (_rows.Count >= _maxRows)
            {
                Cut(GatewayBatchReason.RowLimit);
            }
            else if (due)
            {
                Cut(GatewayBatchReason.Timeout);
            }
        }

        Drain();
    }

    /// <summary>End the subscription: the pending window is flushed as Closing, later rows are dropped.</summary>
    public void Close()
    {
        lock (_lock)
        {
            if (_closed)
            {
                return;
            }

            _closed = true;
            Cut(GatewayBatchReason.Closing);
        }

        Drain();
    }

    // A row that will not reach a batch: counted in the window, which it arms.
    private void Drop()
    {
        lock (_lock)
        {
            if (_closed)
            {
                return;
            }

            bool due = Arm();
            ++_dropped;
            if (due)
            {
                Cut(GatewayBatchReason.Timeout);
            }
        }

        Drain();
    }

    // The window's first event starts its deadline, anchored to the previous flush; true when
    // that deadline has already passed. Under _lock.
    private bool Arm()
    {
        if (_armed)
        {
            return false;
        }

        _armed = true;
        if (_timeout == System.Threading.Timeout.InfiniteTimeSpan)
        {
            return false;
        }

        TimeSpan left = _timeout - Stopwatch.GetElapsedTime(_lastFlush);
        if (left <= TimeSpan.Zero)
        {
            return true;
        }

        // One timer per window, carrying it, so a timer for a window already cut does nothing.
        List<ReadOnlyMemory<byte>> window = _rows;
        _timer = new Timer(OnTimer, window, left, System.Threading.Timeout.InfiniteTimeSpan);
        return false;
    }

    private void OnTimer(object? window)
    {
        lock (_lock)
        {
            if (_closed || !ReferenceEquals(window, _rows))
            {
                return;
            }

            Cut(GatewayBatchReason.Timeout);
        }

        Drain();
    }

    // Queue the window for delivery and start the next one. Under _lock.
    private void Cut(GatewayBatchReason reason)
    {
        _timer?.Dispose();
        _timer = null;
        _armed = false;
        _lastFlush = Stopwatch.GetTimestamp();
        if (_rows.Count == 0 && _dropped == 0)
        {
            return;
        }

        _queue.Enqueue(new Window(_rows, _envelopes, _dropped, reason));
        _rows = new List<ReadOnlyMemory<byte>>();
        _envelopes = new List<Envelope>();
        _bytes = 0;
        _dropped = 0;
    }

    // Deliver queued windows one at a time, unless another thread already is.
    private void Drain()
    {
        while (true)
        {
            Window window;
            lock (_lock)
            {
                if (_draining || _queue.Count == 0)
                {
                    return;
                }

                _draining = true;
                window = _queue.Dequeue();
            }

            try
            {
                Deliver(window);
            }
            finally
            {
                lock (_lock)
                {
                    _draining = false;
                }
            }
        }
    }

    private void Deliver(Window window)
    {
        IReadOnlyList<ReadOnlyMemory<byte>> rows = window.Rows;
        IReadOnlyList<Envelope> envelopes = window.Envelopes;
        long dropped = window.Dropped;
        RecordBatch batch;
        try
        {
            batch = _codec.DecodeBatch(rows);
        }
        catch (Exception)
        {
            // A bad row among them: find it, then decode the good ones together.
            var goodRows = new List<ReadOnlyMemory<byte>>(rows.Count);
            var goodEnvelopes = new List<Envelope>(rows.Count);
            for (int i = 0; i < rows.Count; ++i)
            {
                try
                {
                    _codec.Decode(rows[i].Span).Dispose();
                    goodRows.Add(rows[i]);
                    goodEnvelopes.Add(envelopes[i]);
                }
                catch (Exception e)
                {
                    ++dropped;
                    _client.OnRowUndecodable(e, _id);
                }
            }

            envelopes = goodEnvelopes;
            try
            {
                batch = _codec.DecodeBatch(goodRows);
            }
            catch (Exception e)
            {
                // Rows that decode alone but not together: never seen, but the delivering
                // thread (the receive loop, often) must not be taken down by it.
                dropped += goodRows.Count;
                _client.OnRowUndecodable(e, _id);
                batch = _codec.DecodeBatch(System.Array.Empty<ReadOnlyMemory<byte>>());
                envelopes = System.Array.Empty<Envelope>();
            }
        }

        GatewayBatchStatus status = new(window.Reason, dropped);
        _client.RunHandler(_id, () => _handler(batch, envelopes, status));
    }

    private sealed record Window(List<ReadOnlyMemory<byte>> Rows, List<Envelope> Envelopes, long Dropped, GatewayBatchReason Reason);
}
