// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The managed WebSocket client for the Fletcher gateway (BIND-8b).
//
// A port of gateway-client-ts's FletcherClient, method for method, over Arrow batches rather
// than plain objects (D-BIND-108). The protocol is gateway/src/ws_session.cpp's; the frames,
// the envelope and the row codec are 8a's.
//
// ── One request in flight (D-BIND-110) ──────────────────────────────────────
// The gateway's `error` reply carries no request id, so the client lets ONE control request
// (publishes included, D-BIND-111) be outstanding at a time: a gate is taken before the
// frame is sent and released when ITS reply has been read. A caller that cancels stops
// waiting at once, but the gate stays held until the reply arrives and is consumed, so the
// next request can never be handed the previous one's answer. Rows to subscribers are not
// requests and are never held back by the gate.
//
// ── One write at a time ─────────────────────────────────────────────────────
// ClientWebSocket allows ONE write at a time (SendAsync or CloseOutputAsync) and throws on a
// second. The gate does not ensure it: it is released when a reply is READ, and a reply can be
// read before its send has returned. So every write takes a second lock, _write: a request's
// send (never cancelled, like the send itself) and DisposeAsync's close (bounded by the close
// grace; a send that never returns leaves the socket to be dropped instead).
//
// ── The receive loop ────────────────────────────────────────────────────────
// One task reads the connection. A text frame is a reply and completes the request in
// flight; a binary frame is a MESSAGE and is delivered to its subscription's handler, on
// this loop, with the envelope lent from the receive buffer (D-BIND-113). Requests complete
// with RunContinuationsAsynchronously, so a caller's continuation never runs on the loop
// and cannot stall it. A reply the client cannot parse fails only the request in flight
// (D-BIND-124).
//
// ── Subscribing ─────────────────────────────────────────────────────────────
// The subscription is registered BY THE LOOP when the `subscribed` reply is read, not by
// the caller afterwards: the gateway may deliver a retained row for the new subscription
// before its reply (ws_session.cpp posts both), so rows for an id not yet known that arrive
// while a subscribe is in flight are kept (copied, bounded) and replayed on registration.
// A subscribe whose caller stopped waiting before the reply was read is not registered: the
// loop releases it on the gateway instead, since its caller never got the id to (review B2).
// A batched subscription (D-BIND-133) hands its rows to a GatewayBatcher, which gathers them
// into windows and delivers each as one batch; see GatewayBatch.cs.
//
// ── What ends the connection ────────────────────────────────────────────────
// The gateway closing it, the socket failing, a message above MaxMessageBytes, a reply that
// does not come within ReplyTimeout, or DisposeAsync. The request in flight then fails with
// a GatewayException naming the cause, later calls fail the same way, every subscription
// ends, and Completion completes (faulted with the cause, unless disposed). There is no
// reconnect, as in the TypeScript client (D-BIND-121).
using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.IO;
using System.Net.WebSockets;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

using Apache.Arrow;

namespace Eiva.Fletcher.GatewayClient;

/// <summary>A connection to a Fletcher gateway: topics, subscriptions and publishing over WebSocket.</summary>
/// <remarks>
/// <para>
/// Needs no native assets: the wire format is encoded and decoded in managed code. Every method
/// may be called from any thread; requests are sent one at a time in the order they take the
/// connection, and a <see cref="GatewayException"/> from the gateway belongs to the call that
/// receives it.
/// </para>
/// <para>
/// Topics are the protocol's <c>string</c> keys, segments joined with <c>/</c> (D-BIND-114); a
/// generated gateway pair passes its topic's <c>Key</c>.
/// </para>
/// <para>
/// <b>Cancellation.</b> A token cancelled before the request is sent cancels it. Once it has
/// been sent, the call stops waiting and throws <see cref="OperationCanceledException"/> at
/// once, but the request still reaches the gateway and takes effect, and the client still
/// reads its reply before sending the next request, so no later call is answered with it. A
/// cancelled <see cref="SubscribeAsync"/> leaves no subscription behind: the client releases
/// it when its reply arrives.
/// </para>
/// <para>
/// <b>The end of the connection.</b> <see cref="Completion"/> completes when the connection
/// ends; the client never reconnects. To carry on, await it, then connect a new client and
/// subscribe again.
/// </para>
/// </remarks>
public sealed class FletcherClient : IAsyncDisposable
{
    /// <summary>Rows kept for a subscription the client does not know yet, while a subscribe is in flight.</summary>
    internal static readonly int MaxEarlyRows = 1024;

    private static readonly TimeSpan CloseGrace = TimeSpan.FromSeconds(2);

    // The client whose handler this thread is running, so DisposeAsync called from inside a
    // handler does not wait for the loop it is running on.
    [ThreadStatic]
    private static FletcherClient? _delivering;

    private readonly FletcherClientOptions _options;
    private readonly IGatewayTransport _transport;
    private readonly SemaphoreSlim _gate = new(1, 1);
    private readonly SemaphoreSlim _write = new(1, 1);
    private readonly object _sync = new();
    private readonly ConcurrentDictionary<ulong, Subscription> _subscriptions = new();
    private readonly CancellationTokenSource _stop = new();
    private readonly List<(ulong Id, byte[] Envelope)> _early = new();
    private readonly TaskCompletionSource _completion = new(TaskCreationOptions.RunContinuationsAsynchronously);

    private Pending? _pending;
    private Task? _receiveLoop;
    private Exception? _ended;
    private int _state;
    private volatile bool _connected;
    private long _absorbed;
    private long _undecodable;
    private long _dropped;

    /// <summary>A client for the gateway at <see cref="FletcherClientOptions.Uri"/>; call <see cref="ConnectAsync"/> next.</summary>
    /// <exception cref="ArgumentNullException"><paramref name="options"/> is null.</exception>
    public FletcherClient(FletcherClientOptions options)
        : this(options, new WebSocketTransport(options ?? throw new ArgumentNullException(nameof(options))))
    {
    }

    internal FletcherClient(FletcherClientOptions options, IGatewayTransport transport)
    {
        ArgumentNullException.ThrowIfNull(options);
        ArgumentNullException.ThrowIfNull(transport);
        if (options.MaxMessageBytes <= 0)
        {
            throw new ArgumentOutOfRangeException(nameof(options), options.MaxMessageBytes, "MaxMessageBytes must be positive.");
        }

        if (options.ReplyTimeout is { } timeout && timeout <= TimeSpan.Zero)
        {
            throw new ArgumentOutOfRangeException(nameof(options), timeout, "ReplyTimeout must be positive, or null for none.");
        }

        _options = options;
        _transport = transport;
    }

    /// <summary>Raised when a subscription's handler throws, or a row sent to it does not decode.</summary>
    /// <remarks>
    /// Raised on the receive loop; <see cref="GatewayHandlerFaultedEventArgs.Reason"/> says which.
    /// An exception from a subscriber to this event is ignored.
    /// </remarks>
    public event EventHandler<GatewayHandlerFaultedEventArgs>? HandlerFaulted;

    /// <summary>
    /// Completes when the connection ends (D-BIND-121): successfully after
    /// <see cref="DisposeAsync"/>, faulted with a <see cref="GatewayException"/> naming the cause
    /// otherwise (the gateway closed it, the socket failed, a message was too large, a reply did
    /// not come in time). Every subscription has ended by then; the client does not reconnect.
    /// </summary>
    public Task Completion => _completion.Task;

    /// <summary>How many times a subscription's handler has thrown.</summary>
    public ulong AbsorbedHandlerFailures => (ulong)Interlocked.Read(ref _absorbed);

    /// <summary>How many rows did not decode against their subscription's schema, and were dropped.</summary>
    public ulong UndecodableRows => (ulong)Interlocked.Read(ref _undecodable);

    /// <summary>
    /// How many rows arrived for no subscription the client holds and were dropped (D-BIND-132):
    /// rows for a subscription already ended, or not yet registered beyond the
    /// <see cref="MaxEarlyRows"/> kept while a subscribe is in flight.
    /// </summary>
    public ulong DroppedRows => (ulong)Interlocked.Read(ref _dropped);

    /// <summary>How many subscriptions the client holds; for tests.</summary>
    internal int SubscriptionCount => _subscriptions.Count;

    /// <summary>Open the connection.</summary>
    /// <exception cref="InvalidOperationException">The client was connected before, or disposed.</exception>
    public async Task ConnectAsync(CancellationToken cancellationToken = default)
    {
        if (Interlocked.CompareExchange(ref _state, 1, 0) != 0)
        {
            if (Volatile.Read(ref _state) == 2)
            {
                throw new ObjectDisposedException(nameof(FletcherClient));
            }

            throw new InvalidOperationException("FletcherClient: ConnectAsync may be called once, on a new client.");
        }

        try
        {
            await _transport.ConnectAsync(_options.Uri, cancellationToken).ConfigureAwait(false);
        }
        catch (Exception e)
        {
            End(new GatewayException("FletcherClient: could not connect to " + _options.Uri, e));
            throw;
        }

        Volatile.Write(ref _receiveLoop, Task.Run(ReceiveLoopAsync, CancellationToken.None));
        _connected = true;
    }

    /// <summary>Create a topic, announcing <paramref name="schema"/> to later subscribers when given.</summary>
    /// <param name="topic">The topic's key.</param>
    /// <param name="schema">
    /// The topic's schema, sent as its Arrow IPC bytes (D-BIND-112), so the topic is declared
    /// exactly as a native publisher of the same schema declares it. Null routes bytes only.
    /// </param>
    /// <param name="cancellationToken">Stops the wait; see the type's remarks on cancellation.</param>
    /// <exception cref="GatewayException">The gateway refused it, or the connection ended.</exception>
    public async Task CreateTopicAsync(string topic, Schema? schema = null, CancellationToken cancellationToken = default)
    {
        CheckTopic(topic);
        await RequestAsync(Text(ControlMessages.BuildCreateTopic(topic, schema)), WebSocketMessageType.Text,
            new Pending(typeof(TopicCreatedReply), "create_topic"), cancellationToken).ConfigureAwait(false);
    }

    /// <summary>Subscribe to a topic; every row published to it reaches <paramref name="handler"/>.</summary>
    /// <param name="topic">The topic's key.</param>
    /// <param name="schema">
    /// The schema to decode rows with: a generated message's <c>Schema</c>. Null uses the schema
    /// the gateway sends back, which it has only when a publisher announced one.
    /// </param>
    /// <param name="handler">
    /// Receives each row on the receive loop. Synchronous: do not pass an <c>async</c> lambda
    /// (see <see cref="GatewayRowHandler"/>, D-BIND-125).
    /// </param>
    /// <param name="cancellationToken">
    /// Stops the wait; see the type's remarks on cancellation. A subscribe cancelled after it was
    /// sent is released by the client when its reply arrives, never left running.
    /// </param>
    /// <returns>The subscription's id, for <see cref="UnsubscribeAsync"/>.</returns>
    /// <exception cref="ArgumentException"><paramref name="schema"/> has a type the wire format does not carry.</exception>
    /// <exception cref="InvalidOperationException">
    /// No schema was given and the gateway had none, or had one this client cannot read or the
    /// wire format does not carry. The gateway-side subscription is released before this is thrown.
    /// </exception>
    /// <exception cref="GatewayException">The gateway refused it, or the connection ended.</exception>
    public async Task<ulong> SubscribeAsync(string topic, Schema? schema, GatewayRowHandler handler, CancellationToken cancellationToken = default)
    {
        CheckTopic(topic);
        ArgumentNullException.ThrowIfNull(handler);
        GatewayRowCodec? codec = schema is null ? null : new GatewayRowCodec(schema);
        return await SubscribeCoreAsync(topic, new Pending(typeof(SubscribedReply), "subscribe") { Handler = handler, Codec = codec },
            cancellationToken).ConfigureAwait(false);
    }

    /// <summary>
    /// Subscribe to a topic with batched delivery (D-BIND-133): rows are gathered into windows and
    /// each window reaches <paramref name="handler"/> as one <see cref="RecordBatch"/>, as a native
    /// <c>SubscriberArrow.SubscribeBatched</c> delivers them.
    /// </summary>
    /// <param name="topic">The topic's key.</param>
    /// <param name="schema">The schema to decode rows with, or null for the gateway's; as <see cref="SubscribeAsync"/>.</param>
    /// <param name="handler">
    /// Receives each window. Synchronous, never called concurrently with itself (see
    /// <see cref="GatewayBatchHandler"/>).
    /// </param>
    /// <param name="options">When a window flushes; null for the defaults (8000 rows, one minute).</param>
    /// <param name="cancellationToken">Stops the wait; as <see cref="SubscribeAsync"/>.</param>
    /// <returns>The subscription's id, for <see cref="UnsubscribeAsync"/>, which flushes the pending window first.</returns>
    /// <exception cref="ArgumentOutOfRangeException">
    /// <see cref="GatewayBatchOptions.Timeout"/> is negative or above <see cref="GatewayBatchOptions.MaxTimeout"/>,
    /// and not <see cref="Timeout.InfiniteTimeSpan"/>.
    /// </exception>
    /// <exception cref="ArgumentException"><paramref name="schema"/> has a type the wire format does not carry.</exception>
    /// <exception cref="InvalidOperationException">As <see cref="SubscribeAsync"/>.</exception>
    /// <exception cref="GatewayException">The gateway refused it, or the connection ended.</exception>
    public async Task<ulong> SubscribeBatchedAsync(string topic, Schema? schema, GatewayBatchHandler handler,
        GatewayBatchOptions? options = null, CancellationToken cancellationToken = default)
    {
        CheckTopic(topic);
        ArgumentNullException.ThrowIfNull(handler);
        options ??= new GatewayBatchOptions();
        options.Validate();
        GatewayRowCodec? codec = schema is null ? null : new GatewayRowCodec(schema);
        return await SubscribeCoreAsync(topic,
            new Pending(typeof(SubscribedReply), "subscribe") { BatchHandler = handler, BatchOptions = options, Codec = codec },
            cancellationToken).ConfigureAwait(false);
    }

    private async Task<ulong> SubscribeCoreAsync(string topic, Pending pending, CancellationToken cancellationToken)
    {
        try
        {
            var reply = (SubscribedReply)await RequestAsync(Text(ControlMessages.BuildSubscribe(topic)), WebSocketMessageType.Text,
                pending, cancellationToken).ConfigureAwait(false);
            return reply.SubscriptionId;
        }
        catch (NoSchemaException e)
        {
            await ReleaseOnGatewayAsync(e.SubscriptionId).ConfigureAwait(false);
            throw new InvalidOperationException(e.Message, e);
        }
    }

    /// <summary>End a subscription. Its handler is not called for rows read after this call starts.</summary>
    /// <remarks>
    /// A row already being delivered on the receive loop when this is called may still be in its
    /// handler. Removed from the client at once, so a failed or cancelled unsubscribe still stops
    /// delivery; the gateway's side then ends with the connection. A batched subscription's
    /// pending window is flushed first, as <see cref="GatewayBatchReason.Closing"/>, on this thread
    /// unless its handler is already running elsewhere.
    /// </remarks>
    /// <exception cref="GatewayException">The gateway refused it, or the connection ended.</exception>
    public async Task UnsubscribeAsync(ulong subscriptionId, CancellationToken cancellationToken = default)
    {
        if (_subscriptions.TryRemove(subscriptionId, out Subscription? removed))
        {
            removed.Batcher?.Close();
        }

        await RequestAsync(Text(ControlMessages.BuildUnsubscribe(subscriptionId)), WebSocketMessageType.Text,
            new Pending(typeof(UnsubscribedReply), "unsubscribe"), cancellationToken).ConfigureAwait(false);
    }

    /// <summary>Publish every row of <paramref name="batch"/> to <paramref name="topic"/>.</summary>
    /// <remarks>
    /// One frame per row, each awaiting the gateway's <c>published</c> before the next is sent
    /// (D-BIND-111), so a refusal is tied to its row and the rows before it were published.
    /// </remarks>
    /// <exception cref="ArgumentException">The batch has a type the wire format does not carry, or a value it cannot.</exception>
    /// <exception cref="GatewayException">The gateway refused a row, or the connection ended.</exception>
    public async Task PublishAsync(string topic, RecordBatch batch, CancellationToken cancellationToken = default)
    {
        CheckTopic(topic);
        ArgumentNullException.ThrowIfNull(batch);
        var codec = new GatewayRowCodec(batch.Schema);
        BoundBatch bound = codec.Bind(batch);
        for (int row = 0; row < bound.Length; ++row)
        {
            cancellationToken.ThrowIfCancellationRequested();
            await PublishRowAsync(topic, new Envelope(codec.Encode(bound, row)), cancellationToken).ConfigureAwait(false);
        }
    }

    /// <summary>Publish row <paramref name="row"/> of <paramref name="batch"/> with attachments.</summary>
    /// <param name="topic">The topic's key.</param>
    /// <param name="batch">The rows; only <paramref name="row"/> is sent.</param>
    /// <param name="row">The row to send.</param>
    /// <param name="attachments">The row's attachments, or null. Keys are UTF-8 on the wire; a repeated key keeps its last value.</param>
    /// <param name="cancellationToken">Stops the wait; see the type's remarks on cancellation.</param>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="row"/> is outside the batch.</exception>
    /// <exception cref="GatewayException">The gateway refused it, or the connection ended.</exception>
    public async Task PublishAsync(string topic, RecordBatch batch, int row,
        IEnumerable<KeyValuePair<string, ReadOnlyMemory<byte>>>? attachments, CancellationToken cancellationToken = default)
    {
        CheckTopic(topic);
        ArgumentNullException.ThrowIfNull(batch);
        var codec = new GatewayRowCodec(batch.Schema);
        byte[] bytes = codec.Encode(codec.Bind(batch), row);
        Envelope envelope = attachments is null ? new Envelope(bytes) : new Envelope(bytes, attachments);
        await PublishRowAsync(topic, envelope, cancellationToken).ConfigureAwait(false);
    }

    /// <summary>The topics the gateway knows.</summary>
    /// <exception cref="GatewayException">The gateway refused it, or the connection ended.</exception>
    public async Task<IReadOnlyList<string>> ListTopicsAsync(CancellationToken cancellationToken = default)
    {
        var reply = (TopicsListReply)await RequestAsync(Text(ControlMessages.BuildListTopics()), WebSocketMessageType.Text,
            new Pending(typeof(TopicsListReply), "list_topics"), cancellationToken).ConfigureAwait(false);
        return reply.Topics;
    }

    /// <summary>Close the connection. A request in flight fails; every subscription ends; <see cref="Completion"/> completes.</summary>
    /// <remarks>Safe to call from a handler: it then does not wait for the receive loop it is running on.</remarks>
    public async ValueTask DisposeAsync()
    {
        if (Interlocked.Exchange(ref _state, 2) == 2)
        {
            return;
        }

        // The reason first: the gateway answers our close with its own, and the loop would
        // otherwise record that as "the gateway closed the connection".
        End(new ObjectDisposedException(nameof(FletcherClient)));

        bool fromLoop = ReferenceEquals(_delivering, this);
        Task? loop = Volatile.Read(ref _receiveLoop);
        if (loop is not null)
        {
            // Send our close and let the loop read the gateway's; give up after a grace period.
            try
            {
                using var grace = new CancellationTokenSource(CloseGrace);

                // A close is a write: not while a send is still running (a publish in flight).
                await _write.WaitAsync(grace.Token).ConfigureAwait(false);
                try
                {
                    await _transport.CloseAsync(grace.Token).ConfigureAwait(false);
                }
                finally
                {
                    _write.Release();
                }

                if (!fromLoop)
                {
                    await loop.WaitAsync(CloseGrace).ConfigureAwait(false);
                }
            }
            catch (Exception e) when (e is TimeoutException or OperationCanceledException or WebSocketException
                or ObjectDisposedException or InvalidOperationException)
            {
            }

            _stop.Cancel();
            if (!fromLoop)
            {
                try
                {
                    // Bounded: a handler stuck in I/O must not hang the dispose for ever.
                    await loop.WaitAsync(CloseGrace).ConfigureAwait(false);
                }
                catch (Exception e) when (e is OperationCanceledException or TimeoutException)
                {
                }
            }
        }

        await _transport.DisposeAsync().ConfigureAwait(false);
        if (!fromLoop)
        {
            _stop.Dispose();
        }
    }

    // ───────────────────────────── Requests ─────────────────────────────

    private Task PublishRowAsync(string topic, Envelope envelope, CancellationToken cancellationToken) =>
        RequestAsync(GatewayFrames.BuildPublish(topic, envelope), WebSocketMessageType.Binary,
            new Pending(typeof(PublishedReply), "publish"), cancellationToken);

    private async Task<GatewayReply> RequestAsync(ReadOnlyMemory<byte> frame, WebSocketMessageType type, Pending pending,
        CancellationToken cancellationToken)
    {
        ThrowIfUnusable();
        await _gate.WaitAsync(cancellationToken).ConfigureAwait(false);
        lock (_sync)
        {
            if (_ended is not null)
            {
                _gate.Release();
                throw Ended();
            }

            // A token cancelled while this call waited for the gate: not sent at all.
            if (cancellationToken.IsCancellationRequested)
            {
                _gate.Release();
                cancellationToken.ThrowIfCancellationRequested();
            }

            _pending = pending;
        }

        try
        {
            // Never cancel a send: cancelling ClientWebSocket.SendAsync aborts the connection.
            // The previous request's send may still be returning although its reply was read.
            await _write.WaitAsync(CancellationToken.None).ConfigureAwait(false);
            try
            {
                await _transport.SendAsync(frame, type, CancellationToken.None).ConfigureAwait(false);
            }
            finally
            {
                _write.Release();
            }
        }
        catch (Exception e)
        {
            var failure = new GatewayException("FletcherClient: the request could not be sent", e);
            End(failure);
            throw failure;
        }

        if (_options.ReplyTimeout is { } timeout)
        {
            _ = WatchReplyAsync(pending, timeout);
        }

        try
        {
            return await pending.Task.WaitAsync(cancellationToken).ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            // Abandoned before the loop read the reply: the loop then releases what the reply
            // created (review B2). Read already: the reply is this call's, so return it.
            if (pending.TryAbandon())
            {
                throw;
            }

            return await pending.Task.ConfigureAwait(false);
        }
    }

    // D-BIND-127: a reply that does not come in time ends the connection, independently of the
    // caller (who may have stopped waiting), so the gate cannot stay held for good.
    private async Task WatchReplyAsync(Pending pending, TimeSpan timeout)
    {
        try
        {
            Task finished = await Task.WhenAny(pending.Task, Task.Delay(timeout, _stop.Token)).ConfigureAwait(false);
            if (finished != pending.Task && !_stop.IsCancellationRequested)
            {
                End(new GatewayException("FletcherClient: no reply to " + pending.Action + " within " + timeout.TotalSeconds + " s"));
            }
        }
        catch (ObjectDisposedException)
        {
        }
    }

    // Release a gateway-side subscription the caller holds no id for, through the gate.
    private async Task ReleaseOnGatewayAsync(ulong subscriptionId)
    {
        try
        {
            await RequestAsync(Text(ControlMessages.BuildUnsubscribe(subscriptionId)), WebSocketMessageType.Text,
                new Pending(typeof(UnsubscribedReply), "unsubscribe"), CancellationToken.None).ConfigureAwait(false);
        }
        catch (Exception e) when (e is GatewayException or ObjectDisposedException or InvalidOperationException)
        {
            // Released on the gateway's side when the connection ends anyway.
        }
    }

    private void ThrowIfUnusable()
    {
        if (!_connected)
        {
            lock (_sync)
            {
                if (_ended is not null)
                {
                    throw Ended();
                }
            }

            throw new InvalidOperationException(Volatile.Read(ref _state) == 0
                ? "FletcherClient: call ConnectAsync first."
                : "FletcherClient: ConnectAsync has not completed; await it before other calls.");
        }

        lock (_sync)
        {
            if (_ended is not null)
            {
                throw Ended();
            }
        }
    }

    private Exception Ended() => _ended is ObjectDisposedException disposed
        ? new ObjectDisposedException(nameof(FletcherClient), disposed)
        : new GatewayException("FletcherClient: the connection to the gateway has ended", _ended!);

    // ───────────────────────────── The receive loop ─────────────────────────────

    private async Task ReceiveLoopAsync()
    {
        try
        {
            while (true)
            {
                ReceivedFrame frame = await _transport.ReceiveAsync(_stop.Token).ConfigureAwait(false);
                if (frame.Payload.Length > _options.MaxMessageBytes)
                {
                    throw new MessageTooLargeException(_options.MaxMessageBytes);
                }

                switch (frame.Type)
                {
                    case WebSocketMessageType.Close:
                        End(new GatewayException("FletcherClient: the gateway closed the connection"));
                        return;
                    case WebSocketMessageType.Text:
                        OnText(frame.Payload);
                        break;
                    default:
                        OnMessage(frame.Payload);
                        break;
                }
            }
        }
        catch (OperationCanceledException) when (_stop.IsCancellationRequested)
        {
            End(new ObjectDisposedException(nameof(FletcherClient)));
        }
        catch (MessageTooLargeException e)
        {
            End(new GatewayException(e.Message, e));
        }
        catch (Exception e)
        {
            End(new GatewayException("FletcherClient: the connection to the gateway failed: " + e.Message, e));
        }
    }

    // D-BIND-124: a reply that does not parse fails the request in flight, or is ignored when
    // none is; the connection stays up.
    private void OnText(ReadOnlyMemory<byte> payload)
    {
        GatewayReply reply;
        try
        {
            reply = ControlMessages.ParseReply(Encoding.UTF8.GetString(payload.Span));
        }
        catch (Exception e) when (e is InvalidDataException or DecoderFallbackException)
        {
            Pending? waiting;
            lock (_sync)
            {
                waiting = _pending;
            }

            if (waiting is not null)
            {
                Finish(waiting, failure: new GatewayException("FletcherClient: the gateway's reply to " + waiting.Action +
                    " could not be read: " + e.Message, e));
            }

            return;
        }

        OnReply(reply);
    }

    private void OnReply(GatewayReply reply)
    {
        Pending? pending;
        lock (_sync)
        {
            pending = _pending;
        }

        // A reply nothing asked for: the gateway sends none unasked, so ignore it (as the
        // TypeScript client does) rather than end the connection over it.
        if (pending is null)
        {
            return;
        }

        if (reply is ErrorReply error)
        {
            Finish(pending, failure: new GatewayException(error.Message));
            return;
        }

        if (reply.GetType() != pending.Expected)
        {
            Finish(pending, failure: new GatewayException("FletcherClient: the gateway answered " + reply.GetType().Name +
                " to a request expecting " + pending.Expected.Name));
            return;
        }

        if (reply is SubscribedReply subscribed && pending.Subscribes)
        {
            // Review B2: the caller stopped waiting before this reply was read, so it never
            // gets the id; the subscription is released instead of registered.
            if (!pending.TryClaim())
            {
                _early.Clear();
                Finish(pending, reply);
                _ = ReleaseOnGatewayAsync(subscribed.SubscriptionId);
                return;
            }

            Register(pending, subscribed);
            return;
        }

        pending.TryClaim();
        Finish(pending, reply);
    }

    private void Register(Pending pending, SubscribedReply reply)
    {
        GatewayRowCodec? codec = pending.Codec;
        string? why = null;
        if (codec is null && reply.Schema is null)
        {
            why = reply.SchemaError is null
                ? "FletcherClient: no schema for topic '" + reply.Topic + "': pass one to SubscribeAsync, or have its " +
                    "publisher announce one with CreateTopicAsync"
                : "FletcherClient: the gateway's schema for topic '" + reply.Topic + "' cannot be read (" + reply.SchemaError +
                    "): pass one to SubscribeAsync";
        }
        else if (codec is null)
        {
            try
            {
                codec = new GatewayRowCodec(reply.Schema!);
            }
            catch (ArgumentException e)
            {
                why = "FletcherClient: the gateway's schema for topic '" + reply.Topic + "' cannot be decoded: " + e.Message;
            }
        }

        if (codec is null)
        {
            _early.Clear();
            Finish(pending, failure: new NoSchemaException(why!, reply.SubscriptionId));
            return;
        }

        var subscription = pending.BatchHandler is null
            ? new Subscription(codec, pending.Handler, null)
            : new Subscription(codec, null,
                new GatewayBatcher(this, reply.SubscriptionId, codec, pending.BatchHandler, pending.BatchOptions!));
        _subscriptions[reply.SubscriptionId] = subscription;
        foreach ((ulong id, byte[] envelope) in _early)
        {
            if (id == reply.SubscriptionId)
            {
                DeliverBytes(subscription, id, envelope);
            }
            else
            {
                Interlocked.Increment(ref _dropped);
            }
        }

        _early.Clear();
        Finish(pending, reply);
    }

    private void OnMessage(ReadOnlyMemory<byte> frame)
    {
        ulong id;
        ReadOnlyMemory<byte> envelopeBytes;
        try
        {
            (id, envelopeBytes) = GatewayFrames.ParseMessage(frame);
        }
        catch (InvalidDataException)
        {
            // Not even a subscription id: no subscription to report it to.
            Interlocked.Increment(ref _dropped);
            return;
        }

        if (_subscriptions.TryGetValue(id, out Subscription? subscription))
        {
            DeliverBytes(subscription, id, envelopeBytes);
            return;
        }

        // Not yet registered: keep it if a subscribe is in flight (the reply may follow it).
        // Otherwise it belongs to a subscription already ended, and is dropped (D-BIND-132).
        bool subscribing;
        lock (_sync)
        {
            subscribing = _pending?.Subscribes == true;
        }

        if (subscribing && _early.Count < MaxEarlyRows)
        {
            _early.Add((id, envelopeBytes.ToArray()));
        }
        else
        {
            Interlocked.Increment(ref _dropped);
        }
    }

    // One row for a subscription: whatever about its bytes does not decode is counted and
    // reported for that row alone, never the end of the connection.
    private void DeliverBytes(Subscription subscription, ulong id, ReadOnlyMemory<byte> envelopeBytes)
    {
        if (subscription.Batcher is { } batcher)
        {
            batcher.Add(envelopeBytes);
            return;
        }

        Envelope envelope;
        RecordBatch row;
        try
        {
            envelope = Envelope.Deserialize(envelopeBytes);
            row = subscription.Codec.Decode(envelope.Row.Span);
        }
        catch (Exception e)
        {
            Interlocked.Increment(ref _undecodable);
            Report(e, id, GatewayFaultReason.RowUndecodable);
            return;
        }

        FletcherClient? outer = _delivering;
        _delivering = this;
        try
        {
            subscription.Handler!(row, envelope);
        }
        catch (Exception e)
        {
            Interlocked.Increment(ref _absorbed);
            Report(e, id, GatewayFaultReason.HandlerThrew);
        }
        finally
        {
            _delivering = outer;
        }
    }

    // Run a batched subscription's handler, as DeliverBytes runs a row's: an exception is
    // counted and reported, never thrown on.
    internal void RunHandler(ulong id, Action call)
    {
        FletcherClient? outer = _delivering;
        _delivering = this;
        try
        {
            call();
        }
        catch (Exception e)
        {
            Interlocked.Increment(ref _absorbed);
            Report(e, id, GatewayFaultReason.HandlerThrew);
        }
        finally
        {
            _delivering = outer;
        }
    }

    internal void OnRowUndecodable(Exception exception, ulong id)
    {
        Interlocked.Increment(ref _undecodable);
        Report(exception, id, GatewayFaultReason.RowUndecodable);
    }

    internal void OnRowDropped() => Interlocked.Increment(ref _dropped);

    private void Report(Exception exception, ulong id, GatewayFaultReason reason)
    {
        try
        {
            HandlerFaulted?.Invoke(this, new GatewayHandlerFaultedEventArgs(exception, id, reason));
        }
        catch (Exception)
        {
            // The loop must survive whatever an event subscriber does.
        }
    }

    // Complete the request in flight and release the gate for the next one.
    private void Finish(Pending pending, GatewayReply? reply = null, Exception? failure = null)
    {
        lock (_sync)
        {
            if (!ReferenceEquals(_pending, pending))
            {
                return;
            }

            _pending = null;
        }

        _gate.Release();
        if (failure is not null)
        {
            pending.Fail(failure);
        }
        else
        {
            pending.Complete(reply!);
        }
    }

    // The connection is over: record why, fail the request in flight, end every subscription,
    // complete Completion; a connection the client ended itself is dropped at once.
    private void End(Exception reason)
    {
        Pending? pending;
        lock (_sync)
        {
            if (_ended is not null)
            {
                return;
            }

            _ended = reason;
            pending = _pending;
            _pending = null;
        }

        if (pending is not null)
        {
            _gate.Release();
            pending.Fail(Ended());
        }

        // Every subscription ends; a batched one flushes its pending window first (D-BIND-133).
        foreach (ulong id in _subscriptions.Keys)
        {
            if (_subscriptions.TryRemove(id, out Subscription? removed))
            {
                removed.Batcher?.Close();
            }
        }

        if (reason is ObjectDisposedException)
        {
            _completion.TrySetResult();
        }
        else
        {
            // Dropped BEFORE Completion says so: whoever awaits it finds the socket gone.
            try
            {
                _transport.Abort();
            }
            catch (Exception)
            {
                // Already closed or disposed: nothing to drop.
            }

            _completion.TrySetException(reason as GatewayException ?? new GatewayException("FletcherClient: the connection ended", reason));

            // Observed here, so an app that never awaits Completion sees no unobserved-task noise.
            _ = _completion.Task.Exception;
        }
    }

    private static void CheckTopic(string topic)
    {
        ArgumentNullException.ThrowIfNull(topic);
        if (topic.Length == 0)
        {
            throw new ArgumentException("A topic must not be empty.", nameof(topic));
        }

        if (Encoding.UTF8.GetByteCount(topic) > GatewayFrames.MaxTopicBytes)
        {
            throw new ArgumentException("A topic must fit a PUBLISH frame: at most " + GatewayFrames.MaxTopicBytes + " UTF-8 bytes.", nameof(topic));
        }
    }

    private static byte[] Text(string json) => Encoding.UTF8.GetBytes(json);

    // Exactly one of Handler (per row) and Batcher (batched) is set.
    private sealed record Subscription(GatewayRowCodec Codec, GatewayRowHandler? Handler, GatewayBatcher? Batcher);

    // A request in flight: what reply it expects and, for a subscribe, what to register.
    private sealed class Pending
    {
        private readonly TaskCompletionSource<GatewayReply> _completion = new(TaskCreationOptions.RunContinuationsAsynchronously);
        private int _claim; // 0 open, 1 the loop handled the reply, 2 the caller abandoned it

        public Pending(Type expected, string action)
        {
            Expected = expected;
            Action = action;
        }

        public Type Expected { get; }

        /// <summary>The request's action, for messages.</summary>
        public string Action { get; }

        public GatewayRowHandler? Handler { get; init; }

        public GatewayBatchHandler? BatchHandler { get; init; }

        public GatewayBatchOptions? BatchOptions { get; init; }

        /// <summary>A subscribe: its reply registers a subscription.</summary>
        public bool Subscribes => Handler is not null || BatchHandler is not null;

        public GatewayRowCodec? Codec { get; init; }

        public Task<GatewayReply> Task => _completion.Task;

        /// <summary>The loop takes the reply; false when the caller had already abandoned it.</summary>
        public bool TryClaim() => Interlocked.CompareExchange(ref _claim, 1, 0) != 2;

        /// <summary>The caller stops waiting; false when the loop had already taken the reply.</summary>
        public bool TryAbandon() => Interlocked.CompareExchange(ref _claim, 2, 0) != 1;

        public void Complete(GatewayReply reply) => _completion.TrySetResult(reply);

        public void Fail(Exception failure)
        {
            _completion.TrySetException(failure);

            // A caller that stopped waiting never observes it; mark it observed here.
            _ = _completion.Task.Exception;
        }
    }

    // A subscribe the gateway accepted but the client cannot decode: SubscribeAsync releases it.
    private sealed class NoSchemaException : Exception
    {
        public NoSchemaException(string message, ulong subscriptionId)
            : base(message)
        {
            SubscriptionId = subscriptionId;
        }

        public ulong SubscriptionId { get; }
    }
}
