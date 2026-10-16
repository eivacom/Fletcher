// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
using System;

using Apache.Arrow;

namespace Eiva.Fletcher.GatewayClient;

/// <summary>Receives one row of a subscription (D-BIND-113).</summary>
/// <param name="row">
/// The row, decoded into a one-row batch over the subscription's schema (dictionary fields as
/// their value type). The handler OWNS it and disposes it, as a native
/// <c>SubscriberArrow</c> handler owns its batch.
/// </param>
/// <param name="envelope">
/// The row's envelope: its encoded bytes and its attachments. BORROWED: a window onto the
/// client's receive buffer, valid only until the handler returns. Copy what must outlive it.
/// </param>
/// <remarks>
/// Called on the client's receive loop, one message at a time, so a subscription's rows
/// arrive in order and never concurrently. Two rules follow. A slow handler holds the loop,
/// and with it every reply on the connection, so a <c>PublishAsync</c> elsewhere waits.
/// And a handler must not BLOCK on a call to the same client (<c>.Wait()</c>,
/// <c>.Result</c>): that call's reply is read by the loop the handler is holding. Starting
/// a call without waiting for it is fine. A handler that throws is counted and reported
/// through <see cref="FletcherClient.HandlerFaulted"/>; the subscription goes on.
/// </remarks>
public delegate void GatewayRowHandler(RecordBatch row, Envelope envelope);

/// <summary>A subscription's handler threw, or a row it was sent could not be decoded.</summary>
public sealed class GatewayHandlerFaultedEventArgs : EventArgs
{
    internal GatewayHandlerFaultedEventArgs(Exception exception, ulong subscriptionId)
    {
        Exception = exception;
        SubscriptionId = subscriptionId;
    }

    /// <summary>
    /// What the handler threw, or, for a row that did not decode against the subscription's
    /// schema, an <see cref="System.IO.InvalidDataException"/>.
    /// </summary>
    public Exception Exception { get; }

    /// <summary>The subscription the row belonged to.</summary>
    public ulong SubscriptionId { get; }
}
