// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The one seam between FletcherClient and the socket.
//
// The client needs four things of a connection: open it, send one whole text or binary
// message, receive one whole message, close it. Behind this interface the production
// transport is a ClientWebSocket; the tests put an in-memory gateway there, which is how
// gateway-client-ts tests its client too (a mock WebSocket), and what 8a's frame parsers
// were written for.
using System;
using System.Net.WebSockets;
using System.Threading;
using System.Threading.Tasks;

namespace Eiva.Fletcher.GatewayClient;

/// <summary>One whole message received from the gateway.</summary>
/// <remarks>
/// <see cref="Payload"/> is valid only until the next <see cref="IGatewayTransport.ReceiveAsync"/>:
/// the transport reuses its buffer, which is what lets an envelope be lent to a handler
/// without a copy.
/// </remarks>
internal readonly struct ReceivedFrame
{
    public ReceivedFrame(WebSocketMessageType type, ReadOnlyMemory<byte> payload)
    {
        Type = type;
        Payload = payload;
    }

    public WebSocketMessageType Type { get; }

    public ReadOnlyMemory<byte> Payload { get; }
}

/// <summary>A connection to a gateway, whole messages at a time.</summary>
/// <remarks>
/// At most one send and one receive may be outstanding at once; the client guarantees it.
/// </remarks>
internal interface IGatewayTransport : IAsyncDisposable
{
    public Task ConnectAsync(Uri uri, CancellationToken cancellationToken);

    /// <summary>Send one whole message.</summary>
    public Task SendAsync(ReadOnlyMemory<byte> message, WebSocketMessageType type, CancellationToken cancellationToken);

    /// <summary>Receive one whole message; <see cref="WebSocketMessageType.Close"/> when the gateway closed.</summary>
    public ValueTask<ReceivedFrame> ReceiveAsync(CancellationToken cancellationToken);

    /// <summary>Close the connection politely, if it is still open.</summary>
    public Task CloseAsync(CancellationToken cancellationToken);
}

/// <summary>The production transport: <see cref="ClientWebSocket"/>.</summary>
internal sealed class WebSocketTransport : IGatewayTransport
{
    private readonly ClientWebSocket _socket = new();
    private byte[] _buffer = new byte[64 * 1024];

    public Task ConnectAsync(Uri uri, CancellationToken cancellationToken) => _socket.ConnectAsync(uri, cancellationToken);

    public Task SendAsync(ReadOnlyMemory<byte> message, WebSocketMessageType type, CancellationToken cancellationToken) =>
        _socket.SendAsync(message, type, endOfMessage: true, cancellationToken).AsTask();

    public async ValueTask<ReceivedFrame> ReceiveAsync(CancellationToken cancellationToken)
    {
        int length = 0;
        while (true)
        {
            if (length == _buffer.Length)
            {
                System.Array.Resize(ref _buffer, _buffer.Length * 2);
            }

            ValueWebSocketReceiveResult result = await _socket.ReceiveAsync(_buffer.AsMemory(length), cancellationToken).ConfigureAwait(false);
            if (result.MessageType == WebSocketMessageType.Close)
            {
                return new ReceivedFrame(WebSocketMessageType.Close, ReadOnlyMemory<byte>.Empty);
            }

            length += result.Count;
            if (result.EndOfMessage)
            {
                return new ReceivedFrame(result.MessageType, _buffer.AsMemory(0, length));
            }
        }
    }

    public async Task CloseAsync(CancellationToken cancellationToken)
    {
        // CloseOutputAsync, not CloseAsync: CloseAsync waits to RECEIVE the gateway's close,
        // and the client's receive loop already has a receive outstanding, which is where
        // that close arrives.
        if (_socket.State is WebSocketState.Open or WebSocketState.CloseReceived)
        {
            await _socket.CloseOutputAsync(WebSocketCloseStatus.NormalClosure, "client closed", cancellationToken).ConfigureAwait(false);
        }
    }

    public ValueTask DisposeAsync()
    {
        _socket.Dispose();
        return ValueTask.CompletedTask;
    }
}
