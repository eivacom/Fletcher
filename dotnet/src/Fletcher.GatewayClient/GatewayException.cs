// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
using System;

namespace Eiva.Fletcher.GatewayClient;

/// <summary>
/// The gateway refused a request (its <c>error</c> reply), or the connection to it ended
/// while the request was in flight.
/// </summary>
/// <remarks>
/// The gateway's <c>error</c> reply carries no request id. The client keeps one request in
/// flight at a time, so the refusal always belongs to the call that receives it
/// (D-BIND-110).
/// </remarks>
public sealed class GatewayException : Exception
{
    /// <summary>A refusal, with the gateway's message.</summary>
    public GatewayException(string message)
        : base(message)
    {
    }

    /// <summary>A refusal or a lost connection, with its cause.</summary>
    public GatewayException(string message, Exception innerException)
        : base(message, innerException)
    {
    }
}
