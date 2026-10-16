// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
using System;

namespace Eiva.Fletcher.GatewayClient;

/// <summary>How a <see cref="FletcherClient"/> reaches its gateway.</summary>
public sealed class FletcherClientOptions
{
    /// <summary>The gateway's WebSocket address, for example <c>ws://localhost:9090</c>.</summary>
    public required Uri Uri { get; init; }
}
