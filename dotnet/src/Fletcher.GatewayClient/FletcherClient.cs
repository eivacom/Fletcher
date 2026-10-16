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
// ── The receive loop ────────────────────────────────────────────────────────
// One task reads the connection. A text frame is a reply and completes the request in
// flight; a binary frame is a MESSAGE and is delivered to its subscription's handler, on
// this loop, with the envelope lent from the receive buffer (D-BIND-113). Requests complete
// with RunContinuationsAsynchronously, so a caller's continuation never runs on the loop
// and cannot stall it.
//
// ── Subscribing ─────────────────────────────────────────────────────────────
// The subscription is registered BY THE LOOP when the `subscribed` reply is read, not by
// the caller afterwards: the gateway may deliver a retained row for the new subscription
// before its reply (ws_session.cpp posts both), so rows for an id not yet known that arrive
// while a subscribe is in flight are kept (copied, bounded) and replayed on registration.
//
// ── What ends the connection ────────────────────────────────────────────────
// The gateway closing it, the socket failing, a frame the client cannot parse, or
// DisposeAsync. The request in flight then fails with a GatewayException naming the cause,
// later calls fail the same way, and every subscription ends. There is no reconnect, as in
// the TypeScript client.
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
/// reads its reply before sending the next request, so no later call is answered with it.
/// </para>
/// </remarks>
public sealed class FletcherClient : IAsyncDisposable
{
    /// <summary>Rows kept for a subscription the client does not know yet, while a subscribe is in flight.</summary>
    internal static readonly int MaxEarlyRows = 1024;

    private static readonly TimeSpan CloseGrace = TimeSpan.FromSeconds(2);

    private readonly FletcherClientOptions _options;
    private readonly IGatewayTransport _transport;
    private readonly SemaphoreSlim _gate = new(1, 1);
    private readonly object _sync = new();
    private readonly ConcurrentDictionary<ulong, Subscription> _subscriptions = new();
    private readonly CancellationTokenSource _stop = new();
    private readonly List<(ulong Id, byte[] Envelope)> _early = new();

    private Pending? _pending;
    private Task? _receiveLoop;
    private Exception? _ended;
    private int _state;
    private long _absorbed;
    private long _undecodable;

    /// <summary>A client for the gateway at <see cref="FletcherClientOptions.Uri"/>; call <see cref="ConnectAsync"/> next.</summary>
    /// <exception cref="ArgumentNullException"><paramref name="options"/> is null.</exception>
    public FletcherClient(FletcherClientOptions options)
        : this(options, new WebSocketTransport())
    {
    }

    internal FletcherClient(FletcherClientOptions options, IGatewayTransport transport)
    {
        ArgumentNullException.ThrowIfNull(options);
        ArgumentNullException.ThrowIfNull(transport);
        _options = options;
        _transport = transport;
    }

    /// <summary>Raised when a subscription's handler throws, or a row sent to it does not decode.</summary>
    /// <remarks>Raised on the receive loop. An exception from a subscriber to this event is ignored.</remarks>
    public event EventHandler<GatewayHandlerFaultedEventArgs>? HandlerFaulted;

    /// <summary>How many times a subscription's handler has thrown.</summary>
    public ulong AbsorbedHandlerFailures => (ulong)Interlocked.Read(ref _absorbed);

    /// <summary>How many rows did not decode against their subscription's schema, and were dropped.</summary>
    public ulong UndecodableRows => (ulong)Interlocked.Read(ref _undecodable);

    /// <summary>Open the connection.</summary>
    /// <exception cref="InvalidOperationException">The client was connected before, or disposed.</exception>
    public async Task ConnectAsync(CancellationToken cancellationToken = default)
    {
        if (Interlocked.CompareExchange(ref _state, 1, 0) != 0)
        {
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

        _receiveLoop = Task.Run(ReceiveLoopAsync, CancellationToken.None);
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
            new Pending(typeof(TopicCreatedReply)), cancellationToken).ConfigureAwait(false);
    }

    /// <summary>Subscribe to a topic; every row published to it reaches <paramref name="handler"/>.</summary>
    /// <param name="topic">The topic's key.</param>
    /// <param name="schema">
    /// The schema to decode rows with: a generated message's <c>Schema</c>. Null uses the schema
    /// the gateway sends back, which it has only when a publisher announced one.
    /// </param>
    /// <param name="handler">Receives each row on the receive loop (see <see cref="GatewayRowHandler"/>).</param>
    /// <param name="cancellationToken">Stops the wait; see the type's remarks on cancellation.</param>
    /// <returns>The subscription's id, for <see cref="UnsubscribeAsync"/>.</returns>
    /// <exception cref="ArgumentException"><paramref name="schema"/> has a type the wire format does not carry.</exception>
    /// <exception cref="InvalidOperationException">
    /// No schema was given and the gateway had none, or had one the wire format does not carry.
    /// The gateway-side subscription is released before this is thrown.
    /// </exception>
    /// <exception cref="GatewayException">The gateway refused it, or the connection ended.</exception>
    public async Task<ulong> SubscribeAsync(string topic, Schema? schema, GatewayRowHandler handler, CancellationToken cancellationToken = default)
    {
        CheckTopic(topic);
        ArgumentNullException.ThrowIfNull(handler);
        GatewayRowCodec? codec = schema is null ? null : new GatewayRowCodec(schema);

        var pending = new Pending(typeof(SubscribedReply)) { Handler = handler, Codec = codec };
        try
        {
            var reply = (SubscribedReply)await RequestAsync(Text(ControlMessages.BuildSubscribe(topic)), WebSocketMessageType.Text,
                pending, cancellationToken).ConfigureAwait(false);
            return reply.SubscriptionId;
        }
        catch (NoSchemaException e)
        {
            try
            {
                await RequestAsync(Text(ControlMessages.BuildUnsubscribe(e.SubscriptionId)), WebSocketMessageType.Text,
                    new Pending(typeof(UnsubscribedReply)), CancellationToken.None).ConfigureAwait(false);
            }
            catch (GatewayException)
            {
                // The subscription is released on the gateway's side when the connection ends anyway.
            }

            throw new InvalidOperationException(e.Message, e.InnerException);
        }
    }

    /// <summary>End a subscription. Its handler is not called again once this has started.</summary>
    /// <exception cref="GatewayException">The gateway refused it, or the connection ended.</exception>
    public async Task UnsubscribeAsync(ulong subscriptionId, CancellationToken cancellationToken = default)
    {
        _subscriptions.TryRemove(subscriptionId, out _);
        await RequestAsync(Text(ControlMessages.BuildUnsubscribe(subscriptionId)), WebSocketMessageType.Text,
            new Pending(typeof(UnsubscribedReply)), cancellationToken).ConfigureAwait(false);
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
            new Pending(typeof(TopicsListReply)), cancellationToken).ConfigureAwait(false);
        return reply.Topics;
    }

    /// <summary>Close the connection. A request in flight fails; every subscription ends.</summary>
    public async ValueTask DisposeAsync()
    {
        if (Interlocked.Exchange(ref _state, 2) == 2)
        {
            return;
        }

        // The reason first: the gateway answers our close with its own, and the loop would
        // otherwise record that as "the gateway closed the connection".
        End(new ObjectDisposedException(nameof(FletcherClient)));

        if (_receiveLoop is not null)
        {
            // Send our close and let the loop read the gateway's; give up after a grace period.
            try
            {
                using var grace = new CancellationTokenSource(CloseGrace);
                await _transport.CloseAsync(grace.Token).ConfigureAwait(false);
                await _receiveLoop.WaitAsync(CloseGrace).ConfigureAwait(false);
            }
            catch (Exception e) when (e is TimeoutException or OperationCanceledException or WebSocketException or ObjectDisposedException)
            {
            }

            _stop.Cancel();
            try
            {
                await _receiveLoop.ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
            }
        }

        await _transport.DisposeAsync().ConfigureAwait(false);
        _stop.Dispose();
    }

    // ───────────────────────────── Requests ─────────────────────────────

    private Task PublishRowAsync(string topic, Envelope envelope, CancellationToken cancellationToken) =>
        RequestAsync(GatewayFrames.BuildPublish(topic, envelope), WebSocketMessageType.Binary,
            new Pending(typeof(PublishedReply)), cancellationToken);

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

            _pending = pending;
        }

        try
        {
            // Never cancel a send: cancelling ClientWebSocket.SendAsync aborts the connection.
            await _transport.SendAsync(frame, type, CancellationToken.None).ConfigureAwait(false);
        }
        catch (Exception e)
        {
            var failure = new GatewayException("FletcherClient: the request could not be sent", e);
            End(failure);
            throw failure;
        }

        return await pending.Task.WaitAsync(cancellationToken).ConfigureAwait(false);
    }

    private void ThrowIfUnusable()
    {
        if (Volatile.Read(ref _state) == 0)
        {
            throw new InvalidOperationException("FletcherClient: call ConnectAsync first.");
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
                switch (frame.Type)
                {
                    case WebSocketMessageType.Close:
                        End(new GatewayException("FletcherClient: the gateway closed the connection"));
                        return;
                    case WebSocketMessageType.Text:
                        OnReply(ControlMessages.ParseReply(Encoding.UTF8.GetString(frame.Payload.Span)));
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
        catch (Exception e)
        {
            End(new GatewayException("FletcherClient: the connection to the gateway failed: " + e.Message, e));
        }
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

        if (reply is SubscribedReply subscribed && pending.Handler is not null)
        {
            Register(pending, subscribed);
            return;
        }

        Finish(pending, reply);
    }

    private void Register(Pending pending, SubscribedReply reply)
    {
        GatewayRowCodec? codec = pending.Codec;
        string? why = null;
        if (codec is null && reply.Schema is null)
        {
            why = "FletcherClient: no schema for topic '" + reply.Topic + "': pass one to SubscribeAsync, or have its " +
                "publisher announce one with CreateTopicAsync";
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

        var subscription = new Subscription(codec, pending.Handler!);
        _subscriptions[reply.SubscriptionId] = subscription;
        foreach ((ulong id, byte[] envelope) in _early)
        {
            if (id == reply.SubscriptionId)
            {
                Deliver(subscription, id, Envelope.Deserialize(envelope));
            }
        }

        _early.Clear();
        Finish(pending, reply);
    }

    private void OnMessage(ReadOnlyMemory<byte> frame)
    {
        (ulong id, ReadOnlyMemory<byte> envelopeBytes) = GatewayFrames.ParseMessage(frame);
        Envelope envelope = Envelope.Deserialize(envelopeBytes);

        if (_subscriptions.TryGetValue(id, out Subscription? subscription))
        {
            Deliver(subscription, id, envelope);
            return;
        }

        // Not yet registered: keep it if a subscribe is in flight (the reply may follow it).
        // Otherwise it belongs to a subscription already ended, and is dropped.
        bool subscribing;
        lock (_sync)
        {
            subscribing = _pending?.Handler is not null;
        }

        if (subscribing && _early.Count < MaxEarlyRows)
        {
            _early.Add((id, envelopeBytes.ToArray()));
        }
    }

    private void Deliver(Subscription subscription, ulong id, Envelope envelope)
    {
        RecordBatch row;
        try
        {
            row = subscription.Codec.Decode(envelope.Row.Span);
        }
        catch (InvalidDataException e)
        {
            Interlocked.Increment(ref _undecodable);
            Report(e, id);
            return;
        }

        try
        {
            subscription.Handler(row, envelope);
        }
        catch (Exception e)
        {
            Interlocked.Increment(ref _absorbed);
            Report(e, id);
        }
    }

    private void Report(Exception exception, ulong id)
    {
        try
        {
            HandlerFaulted?.Invoke(this, new GatewayHandlerFaultedEventArgs(exception, id));
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

    // The connection is over: record why, fail the request in flight, end every subscription.
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

        _subscriptions.Clear();
        if (pending is not null)
        {
            _gate.Release();
            pending.Fail(Ended());
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

    private sealed record Subscription(GatewayRowCodec Codec, GatewayRowHandler Handler);

    // A request in flight: what reply it expects and, for a subscribe, what to register.
    private sealed class Pending
    {
        private readonly TaskCompletionSource<GatewayReply> _completion = new(TaskCreationOptions.RunContinuationsAsynchronously);

        public Pending(Type expected)
        {
            Expected = expected;
        }

        public Type Expected { get; }

        public GatewayRowHandler? Handler { get; init; }

        public GatewayRowCodec? Codec { get; init; }

        public Task<GatewayReply> Task => _completion.Task;

        public void Complete(GatewayReply reply) => _completion.TrySetResult(reply);

        public void Fail(Exception failure) => _completion.TrySetException(failure);
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
