// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
using System;
using System.Net.WebSockets;

namespace Eiva.Fletcher.GatewayClient;

/// <summary>How a <see cref="FletcherClient"/> reaches its gateway.</summary>
public sealed class FletcherClientOptions
{
    /// <summary>The default <see cref="MaxMessageBytes"/>: 64 MiB.</summary>
    public const int DefaultMaxMessageBytes = 64 * 1024 * 1024;

    /// <summary>The gateway's WebSocket address, for example <c>ws://localhost:9090</c>.</summary>
    public required Uri Uri { get; init; }

    /// <summary>
    /// Called with the socket's options before it connects: request headers and authentication,
    /// a proxy, client certificates and server-certificate validation, keep-alive (D-BIND-126).
    /// </summary>
    public Action<ClientWebSocketOptions>? ConfigureWebSocket { get; init; }

    /// <summary>
    /// The largest message the client accepts from the gateway. A larger one ends the connection,
    /// with <see cref="FletcherClient.Completion"/> naming the size, rather than growing the
    /// receive buffer without bound (D-BIND-126).
    /// </summary>
    public int MaxMessageBytes { get; init; } = DefaultMaxMessageBytes;

    /// <summary>
    /// How long a request may wait for its reply; null (the default) waits indefinitely. When it
    /// runs out the connection ends, with <see cref="FletcherClient.Completion"/> naming the
    /// request: one request is in flight at a time, so a lost reply would otherwise hold every
    /// later call (D-BIND-127).
    /// </summary>
    public TimeSpan? ReplyTimeout { get; init; }
}
