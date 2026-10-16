// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// An in-memory gateway behind FletcherClient's transport seam (BIND-8b).
//
// gateway-client-ts tests its client against a mock WebSocket; this is the C# equivalent,
// one level richer: it answers requests as gateway/src/ws_session.cpp does (topic_created,
// subscribed with a decimal-string id and the announced schema as schemaIpc, unsubscribed,
// published, topics_list, error with no request id), and delivers each PUBLISH to the
// topic's subscribers as a MESSAGE frame, as the in-process provider would. It reads and
// writes frames with 8a's parsers and builders, which is what they were written for.
//
// Switches let a test make it misbehave in the ways a real gateway can: refuse the next
// request, hold a reply back until released, deliver a retained row BEFORE the subscribed
// reply (ws_session.cpp posts both), close the connection, or count how many requests were
// ever outstanding at once (D-BIND-110 says one).
using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Net.WebSockets;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Channels;
using System.Threading.Tasks;

using Apache.Arrow;

namespace Eiva.Fletcher.GatewayClient.Tests;

internal sealed class FakeGateway
{
    private readonly object _sync = new();
    private readonly Dictionary<string, Schema?> _topics = new(StringComparer.Ordinal);
    private readonly Dictionary<ulong, string> _subscriptions = new();
    private readonly List<(WebSocketMessageType Type, byte[] Payload)> _held = new();
    private ulong _nextId = 7;
    private int _outstanding;

    public FakeTransport Transport { get; }

    public FakeGateway()
    {
        Transport = new FakeTransport(this);
    }

    /// <summary>Every request the client sent, as (action or "publish", the raw frame).</summary>
    public ConcurrentQueue<(string Action, byte[] Frame)> Received { get; } = new();

    /// <summary>The most requests the client ever had awaiting a reply at once.</summary>
    public int MaxOutstanding { get; private set; }

    /// <summary>The next request is answered with this error.</summary>
    public string? RefuseNext { get; set; }

    /// <summary>While set, replies are held instead of sent; <see cref="ReleaseHeld"/> sends them.</summary>
    public bool HoldReplies { get; set; }

    /// <summary>On subscribe, deliver these envelopes to the new subscription BEFORE the subscribed reply.</summary>
    public List<Envelope> RetainedBeforeSubscribed { get; } = new();

    /// <summary>The schema the topic was declared with, decoded from schemaIpc.</summary>
    public Schema? DeclaredSchema(string topic)
    {
        lock (_sync)
        {
            return _topics.TryGetValue(topic, out Schema? schema) ? schema : null;
        }
    }

    public void DeclareTopic(string topic, Schema? schema)
    {
        lock (_sync)
        {
            _topics[topic] = schema;
        }
    }

    /// <summary>Send a MESSAGE frame for a subscription, as if a publisher elsewhere had published.</summary>
    public void Deliver(ulong subscriptionId, Envelope envelope) =>
        Transport.ToClient(WebSocketMessageType.Binary, GatewayFrames.BuildMessage(subscriptionId, envelope));

    public void ReleaseHeld()
    {
        List<(WebSocketMessageType, byte[])> held;
        lock (_sync)
        {
            HoldReplies = false;
            held = _held.ToList();
            _held.Clear();
        }

        foreach ((WebSocketMessageType type, byte[] payload) in held)
        {
            Reply(type, payload);
        }
    }

    public void Close() => Transport.ToClient(WebSocketMessageType.Close, System.Array.Empty<byte>());

    public IReadOnlyCollection<ulong> Subscriptions
    {
        get
        {
            lock (_sync)
            {
                return _subscriptions.Keys.ToList();
            }
        }
    }

    // A frame from the client: one request.
    internal void OnClientFrame(WebSocketMessageType type, byte[] frame)
    {
        lock (_sync)
        {
            MaxOutstanding = Math.Max(MaxOutstanding, ++_outstanding);
        }

        if (type == WebSocketMessageType.Binary)
        {
            Received.Enqueue(("publish", frame));
            OnPublish(frame);
            return;
        }

        using JsonDocument json = JsonDocument.Parse(frame);
        string action = json.RootElement.GetProperty("action").GetString()!;
        Received.Enqueue((action, frame));

        if (TakeRefusal() is string refusal)
        {
            ReplyText("{\"type\":\"error\",\"message\":" + JsonSerializer.Serialize(refusal) + "}");
            return;
        }

        JsonElement root = json.RootElement;
        switch (action)
        {
            case "create_topic":
                {
                    string topic = root.GetProperty("topic").GetString()!;
                    Schema? schema = root.TryGetProperty("schemaIpc", out JsonElement ipc) ? ControlMessages.ReadSchemaIpc(ipc.GetString()!) : null;
                    DeclareTopic(topic, schema);
                    ReplyText("{\"type\":\"topic_created\"}");
                    break;
                }

            case "subscribe":
                {
                    string topic = root.GetProperty("topic").GetString()!;
                    ulong id;
                    Schema? schema;
                    lock (_sync)
                    {
                        id = _nextId++;
                        _subscriptions[id] = topic;
                        _topics.TryGetValue(topic, out schema);
                    }

                    foreach (Envelope retained in RetainedBeforeSubscribed)
                    {
                        Transport.ToClient(WebSocketMessageType.Binary, GatewayFrames.BuildMessage(id, retained));
                    }

                    string reply = "{\"type\":\"subscribed\",\"subId\":\"" + id.ToString(CultureInfo.InvariantCulture) +
                        "\",\"topic\":" + JsonSerializer.Serialize(topic);
                    if (schema is not null)
                    {
                        reply += ",\"schemaIpc\":\"" + Convert.ToBase64String(ControlMessages.WriteSchemaIpc(schema)) + "\"";
                    }

                    ReplyText(reply + "}");
                    break;
                }

            case "unsubscribe":
                {
                    ulong id = ulong.Parse(root.GetProperty("subId").GetString()!, CultureInfo.InvariantCulture);
                    lock (_sync)
                    {
                        _subscriptions.Remove(id);
                    }

                    ReplyText("{\"type\":\"unsubscribed\"}");
                    break;
                }

            case "list_topics":
                {
                    string[] topics;
                    lock (_sync)
                    {
                        topics = _topics.Keys.OrderBy(t => t, StringComparer.Ordinal).ToArray();
                    }

                    ReplyText("{\"type\":\"topics_list\",\"topics\":" + JsonSerializer.Serialize(topics) + "}");
                    break;
                }

            default:
                ReplyText("{\"type\":\"error\",\"message\":\"unknown action: " + action + "\"}");
                break;
        }
    }

    private void OnPublish(byte[] frame)
    {
        if (TakeRefusal() is string refusal)
        {
            ReplyText("{\"type\":\"error\",\"message\":" + JsonSerializer.Serialize(refusal) + "}");
            return;
        }

        (string topic, ReadOnlyMemory<byte> envelopeBytes) = GatewayFrames.ParsePublish(frame);
        Envelope envelope = Envelope.Deserialize(envelopeBytes.ToArray());
        List<ulong> subscribers;
        lock (_sync)
        {
            subscribers = _subscriptions.Where(s => s.Value == topic).Select(s => s.Key).ToList();
        }

        // As the in-process provider: delivered to every subscriber, then acknowledged.
        foreach (ulong id in subscribers)
        {
            Deliver(id, envelope);
        }

        ReplyText("{\"type\":\"published\"}");
    }

    private string? TakeRefusal()
    {
        lock (_sync)
        {
            string? refusal = RefuseNext;
            RefuseNext = null;
            return refusal;
        }
    }

    private void ReplyText(string json) => Reply(WebSocketMessageType.Text, Encoding.UTF8.GetBytes(json));

    private void Reply(WebSocketMessageType type, byte[] payload)
    {
        lock (_sync)
        {
            if (HoldReplies)
            {
                _held.Add((type, payload));
                return;
            }

            --_outstanding;
        }

        Transport.ToClient(type, payload);
    }
}

/// <summary>The client's side of the in-memory connection.</summary>
internal sealed class FakeTransport : IGatewayTransport
{
    private readonly FakeGateway _gateway;
    private readonly Channel<(WebSocketMessageType Type, byte[] Payload)> _toClient =
        Channel.CreateUnbounded<(WebSocketMessageType, byte[])>(new UnboundedChannelOptions { SingleReader = true });

    private int _sends;

    public FakeTransport(FakeGateway gateway)
    {
        _gateway = gateway;
    }

    /// <summary>Make ConnectAsync fail with this.</summary>
    public Exception? ConnectFailure { get; set; }

    /// <summary>The most sends ever outstanding at once (the transport allows one).</summary>
    public int MaxConcurrentSends { get; private set; }

    public bool Disposed { get; private set; }

    public void ToClient(WebSocketMessageType type, byte[] payload) => _toClient.Writer.TryWrite((type, payload));

    public Task ConnectAsync(Uri uri, CancellationToken cancellationToken) =>
        ConnectFailure is null ? Task.CompletedTask : Task.FromException(ConnectFailure);

    public async Task SendAsync(ReadOnlyMemory<byte> message, WebSocketMessageType type, CancellationToken cancellationToken)
    {
        int now = Interlocked.Increment(ref _sends);
        MaxConcurrentSends = Math.Max(MaxConcurrentSends, now);
        await Task.Yield();
        try
        {
            _gateway.OnClientFrame(type, message.ToArray());
        }
        finally
        {
            Interlocked.Decrement(ref _sends);
        }
    }

    public async ValueTask<ReceivedFrame> ReceiveAsync(CancellationToken cancellationToken)
    {
        (WebSocketMessageType type, byte[] payload) = await _toClient.Reader.ReadAsync(cancellationToken).ConfigureAwait(false);
        return new ReceivedFrame(type, payload);
    }

    public Task CloseAsync(CancellationToken cancellationToken)
    {
        // The gateway answers a close with its own.
        ToClient(WebSocketMessageType.Close, System.Array.Empty<byte>());
        return Task.CompletedTask;
    }

    public ValueTask DisposeAsync()
    {
        Disposed = true;
        return ValueTask.CompletedTask;
    }
}
