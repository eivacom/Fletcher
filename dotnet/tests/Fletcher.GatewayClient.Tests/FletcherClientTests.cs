// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// BIND-8b: FletcherClient against an in-memory gateway (FakeGateway.cs).
//
// First gateway-client-ts/test/client.test.ts's six cases, one for one (Bucket 2's last
// TypeScript file): connect resolves, a failed connect rejects, closing rejects the request
// in flight, closing clears subscriptions, closing twice is safe, a request before connect
// is refused. Then what the C# client promises beyond them, each a ruling or a stated
// default of 8b: one request in flight (D-BIND-110), a refusal tied to its call, cancelling
// without the late reply reaching the next call, publishing row by row on acknowledgement
// (D-BIND-111), create_topic's schemaIpc (D-BIND-112), delivery to a handler on the loop
// with the envelope lent and the batch given (D-BIND-113), string topics (D-BIND-114), the
// subscribe races, and a handler that throws.
//
// A live gateway is integration-tests/gateway-dotnet-end-to-end's job.
using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

using Apache.Arrow;
using Apache.Arrow.Types;

using Xunit;

namespace Eiva.Fletcher.GatewayClient.Tests;

public sealed class FletcherClientTests
{
    private static readonly TimeSpan Wait = TimeSpan.FromSeconds(10);

    // ── gateway-client-ts test/client.test.ts ──

    [Fact]
    public async Task ConnectResolvesWhenTheConnectionOpens()
    {
        var gateway = new FakeGateway();
        await using FletcherClient client = Client(gateway);

        await client.ConnectAsync().WaitAsync(Wait);
    }

    [Fact]
    public async Task ConnectRejectsWhenTheConnectionFails()
    {
        var gateway = new FakeGateway();
        gateway.Transport.ConnectFailure = new System.Net.WebSockets.WebSocketException("connection failed");
        await using FletcherClient client = Client(gateway);

        await Assert.ThrowsAsync<System.Net.WebSockets.WebSocketException>(() => client.ConnectAsync());
        await Assert.ThrowsAsync<GatewayException>(() => client.CreateTopicAsync("t"));
    }

    [Fact]
    public async Task ClosingRejectsTheRequestInFlight()
    {
        var gateway = new FakeGateway { HoldReplies = true };
        FletcherClient client = await Connected(gateway);

        Task pending = client.CreateTopicAsync("test/topic");
        await WaitUntil(() => gateway.Received.Count == 1);
        await client.DisposeAsync();

        await Assert.ThrowsAsync<ObjectDisposedException>(() => pending.WaitAsync(Wait));
    }

    [Fact]
    public async Task ClosingClearsSubscriptions()
    {
        // Review B5 (BIND-8e): this used to deliver a row after DisposeAsync, which no loop reads
        // whether or not the subscriptions were cleared; it now looks at what the client holds.
        var gateway = new FakeGateway();
        FletcherClient client = await Connected(gateway);
        await client.SubscribeAsync("t", RowSchema(), (row, _) => row.Dispose());
        await client.SubscribeAsync("u", RowSchema(), (row, _) => row.Dispose());
        Assert.Equal(2, client.SubscriptionCount);

        await client.DisposeAsync();

        Assert.Equal(0, client.SubscriptionCount);
    }

    [Fact]
    public async Task ClosingTwiceIsSafe()
    {
        var gateway = new FakeGateway();
        FletcherClient client = await Connected(gateway);

        await client.DisposeAsync();
        await client.DisposeAsync();

        Assert.True(gateway.Transport.Disposed);
    }

    [Fact]
    public async Task ARequestBeforeConnectIsRefused()
    {
        await using FletcherClient client = Client(new FakeGateway());

        InvalidOperationException e = await Assert.ThrowsAsync<InvalidOperationException>(() => client.CreateTopicAsync("t"));
        Assert.Contains("ConnectAsync first", e.Message, StringComparison.Ordinal);
    }

    // ── Topics and schemas ──

    [Fact]
    public async Task CreateTopicAnnouncesTheSchemaAsSchemaIpc()
    {
        var gateway = new FakeGateway();
        await using FletcherClient client = await Connected(gateway);

        await client.CreateTopicAsync("fleet/fix", RowSchema());

        Schema? declared = gateway.DeclaredSchema("fleet/fix");
        Assert.NotNull(declared);
        Assert.Equal(new[] { "id", "label" }, declared!.FieldsList.Select(f => f.Name));
        Assert.False(declared.FieldsList[0].IsNullable);
        Assert.Equal("1", declared.FieldsList[0].Metadata[ControlMessages.FieldNumberKey]);
        Assert.Contains("schemaIpc", Encoding.UTF8.GetString(gateway.Received.Single().Frame), StringComparison.Ordinal);
    }

    [Fact]
    public async Task ListTopicsReturnsTheGatewaysTopics()
    {
        var gateway = new FakeGateway();
        await using FletcherClient client = await Connected(gateway);
        await client.CreateTopicAsync("b");
        await client.CreateTopicAsync("a");

        Assert.Equal(new[] { "a", "b" }, await client.ListTopicsAsync());
    }

    [Fact]
    public async Task ATopicTheGatewayCannotRouteIsRefusedBeforeSending()
    {
        var gateway = new FakeGateway();
        await using FletcherClient client = await Connected(gateway);

        await Assert.ThrowsAsync<ArgumentException>(() => client.CreateTopicAsync(string.Empty));
        await Assert.ThrowsAsync<ArgumentException>(() => client.CreateTopicAsync(new string('x', GatewayFrames.MaxTopicBytes + 1)));
        Assert.Empty(gateway.Received);
    }

    // ── Publishing and subscribing ──

    [Fact]
    public async Task APublishedRowReachesTheSubscriberDecodedWithItsAttachments()
    {
        var gateway = new FakeGateway();
        await using FletcherClient client = await Connected(gateway);
        var rows = new BlockingCollection<(int Id, string Label, string Attachment)>();
        await client.SubscribeAsync("fleet/fix", RowSchema(), (row, envelope) =>
        {
            using (row)
            {
                Assert.True(envelope.TryFind("note", out ReadOnlyMemory<byte> note));
                rows.Add((((Int32Array)row.Column(0)).GetValue(0)!.Value, ((StringArray)row.Column(1)).GetString(0), Encoding.UTF8.GetString(note.Span)));
            }
        });

        using RecordBatch batch = Batch((42, "north"));
        await client.PublishAsync("fleet/fix", batch, 0,
            new[] { new KeyValuePair<string, ReadOnlyMemory<byte>>("note", Encoding.UTF8.GetBytes("hello")) });

        Assert.True(rows.TryTake(out var received, Wait));
        Assert.Equal((42, "north", "hello"), received);
    }

    [Fact]
    public async Task ABatchIsPublishedRowByRowEachOnItsAcknowledgement()
    {
        var gateway = new FakeGateway();
        await using FletcherClient client = await Connected(gateway);
        var ids = new BlockingCollection<int>();
        await client.SubscribeAsync("t", RowSchema(), (row, _) => { using (row) { ids.Add(((Int32Array)row.Column(0)).GetValue(0)!.Value); } });

        using RecordBatch batch = Batch((1, "a"), (2, "b"), (3, "c"));
        await client.PublishAsync("t", batch);

        Assert.Equal(3, gateway.Received.Count(r => r.Action == "publish"));
        Assert.Equal(new[] { 1, 2, 3 }, Enumerable.Range(0, 3).Select(_ => { Assert.True(ids.TryTake(out int id, Wait)); return id; }));
    }

    [Fact]
    public async Task OneRequestIsInFlightAtATime()
    {
        // D-BIND-110: twenty calls started at once still reach the gateway one by one. The fake
        // answers inside the send, so this holds even for a gate released on send; that the gate
        // is held until the REPLY is read is ACancelledCallsLateReplyIsNotGivenToTheNextCall's.
        var gateway = new FakeGateway();
        await using FletcherClient client = await Connected(gateway);
        using RecordBatch batch = Batch((1, "a"));

        await Task.WhenAll(Enumerable.Range(0, 20).Select(i => i % 2 == 0
            ? client.CreateTopicAsync("t" + i)
            : client.PublishAsync("t", batch)));

        Assert.Equal(1, gateway.MaxOutstanding);
        Assert.Equal(1, gateway.Transport.MaxConcurrentSends);
        Assert.Equal(20, gateway.Received.Count);
    }

    [Fact]
    public async Task TheNextRequestIsNotSentWhileThePreviousSendIsStillReturning()
    {
        // A reply can be read before its send has returned (the socket reports completion
        // after the frame left). The gate is released on the reply, so the next request's send
        // must still wait for the previous send: ClientWebSocket refuses two at once.
        var gateway = new FakeGateway();
        await using FletcherClient client = await Connected(gateway);
        var sendsReturn = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        gateway.Transport.SendCompletionGate = sendsReturn.Task;

        Task first = client.CreateTopicAsync("first");
        await WaitUntil(() => gateway.Received.Count == 1);
        Task second = client.CreateTopicAsync("second");
        await Task.Delay(200); // ample for a second send to start, were it allowed to

        Assert.Single(gateway.Received);
        sendsReturn.SetResult();
        await Task.WhenAll(first, second).WaitAsync(Wait);
        Assert.Equal(2, gateway.Received.Count);
        Assert.Equal(1, gateway.Transport.MaxConcurrentSends);
    }

    [Fact]
    public async Task DisposingWaitsForASendStillRunningBeforeClosing()
    {
        // The close is a write too: a dispose while a publish's send is still returning must
        // not close over it.
        var gateway = new FakeGateway();
        FletcherClient client = await Connected(gateway);
        var sendsReturn = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        gateway.Transport.SendCompletionGate = sendsReturn.Task;
        using RecordBatch batch = Batch((1, "a"));

        Task publish = client.PublishAsync("t", batch);
        await WaitUntil(() => gateway.Received.Count == 1);
        Task dispose = client.DisposeAsync().AsTask();
        await Task.Delay(200); // ample for a close to start, were it allowed to

        Assert.Equal(0, gateway.Transport.Closes);
        sendsReturn.SetResult();
        await dispose.WaitAsync(Wait);
        AssertPublishEnded(await Record.ExceptionAsync(() => publish.WaitAsync(Wait)));
        Assert.Equal(1, gateway.Transport.Closes);
        Assert.Equal(1, gateway.Transport.MaxConcurrentSends);
    }

    [Fact]
    public async Task DisposingDoesNotWaitForeverForASendThatNeverReturns()
    {
        // Bounded by the close grace: the close is skipped and the socket dropped instead.
        var gateway = new FakeGateway();
        FletcherClient client = await Connected(gateway);
        var never = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        gateway.Transport.SendCompletionGate = never.Task;
        using RecordBatch batch = Batch((1, "a"));

        Task publish = client.PublishAsync("t", batch);
        await WaitUntil(() => gateway.Received.Count == 1);
        await client.DisposeAsync().AsTask().WaitAsync(Wait);

        Assert.Equal(0, gateway.Transport.Closes);
        Assert.True(gateway.Transport.Disposed);
        never.SetResult();
        AssertPublishEnded(await Record.ExceptionAsync(() => publish.WaitAsync(Wait)));
    }

    [Fact]
    public async Task ARefusalFailsTheCallItAnswersAndTheNextCallWorks()
    {
        var gateway = new FakeGateway();
        await using FletcherClient client = await Connected(gateway);
        using RecordBatch batch = Batch((1, "a"));

        gateway.RefuseNext = "publish: truncated topic";
        GatewayException e = await Assert.ThrowsAsync<GatewayException>(() => client.PublishAsync("t", batch));
        Assert.Equal("publish: truncated topic", e.Message);

        await client.CreateTopicAsync("t");
        Assert.Contains("t", await client.ListTopicsAsync());
    }

    [Fact]
    public async Task ACancelledCallsLateReplyIsNotGivenToTheNextCall()
    {
        var gateway = new FakeGateway { HoldReplies = true };
        await using FletcherClient client = await Connected(gateway);

        using var cancel = new CancellationTokenSource();
        Task<IReadOnlyList<string>> first = client.ListTopicsAsync(cancel.Token);
        await WaitUntil(() => gateway.Received.Count(r => r.Action == "list_topics") == 1);
        cancel.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => first);

        // The next request waits for the first one's reply to be consumed, then gets its own.
        Task second = client.CreateTopicAsync("next");
        await Task.Delay(50);
        Assert.Equal(0, gateway.Received.Count(r => r.Action == "create_topic" && Encoding.UTF8.GetString(r.Frame).Contains("next", StringComparison.Ordinal)));
        gateway.ReleaseHeld();
        await second.WaitAsync(Wait);
        Assert.Contains("next", await client.ListTopicsAsync());
    }

    [Fact]
    public async Task SubscribingWithoutASchemaUsesTheOneThePublisherAnnounced()
    {
        var gateway = new FakeGateway();
        await using FletcherClient client = await Connected(gateway);
        await client.CreateTopicAsync("t", RowSchema());
        var labels = new BlockingCollection<string>();

        await client.SubscribeAsync("t", schema: null, (row, _) => { using (row) { labels.Add(((StringArray)row.Column(1)).GetString(0)); } });
        using RecordBatch batch = Batch((5, "from-gateway-schema"));
        await client.PublishAsync("t", batch);

        Assert.True(labels.TryTake(out string? label, Wait));
        Assert.Equal("from-gateway-schema", label);
    }

    [Fact]
    public async Task SubscribingWithNoSchemaAnywhereFailsAndReleasesTheSubscription()
    {
        var gateway = new FakeGateway();
        await using FletcherClient client = await Connected(gateway);

        InvalidOperationException e = await Assert.ThrowsAsync<InvalidOperationException>(
            () => client.SubscribeAsync("bare", schema: null, (row, _) => row.Dispose()));

        Assert.Contains("no schema", e.Message, StringComparison.Ordinal);
        Assert.Empty(gateway.Subscriptions);
        Assert.Contains(gateway.Received, r => r.Action == "unsubscribe");
    }

    [Fact]
    public async Task ARowDeliveredBeforeTheSubscribedReplyIsNotLost()
    {
        // ws_session.cpp posts a retained row and the subscribed reply separately, so the row
        // can come first; the client keeps it until the reply names its subscription.
        var gateway = new FakeGateway();
        gateway.RetainedBeforeSubscribed.Add(new Envelope(EncodeRow(9, "retained")));
        await using FletcherClient client = await Connected(gateway);
        var labels = new BlockingCollection<string>();

        await client.SubscribeAsync("t", RowSchema(), (row, _) => { using (row) { labels.Add(((StringArray)row.Column(1)).GetString(0)); } });

        Assert.True(labels.TryTake(out string? label, Wait));
        Assert.Equal("retained", label);
    }

    [Fact]
    public async Task UnsubscribingStopsDelivery()
    {
        var gateway = new FakeGateway();
        await using FletcherClient client = await Connected(gateway);
        int delivered = 0;
        ulong id = await client.SubscribeAsync("t", RowSchema(), (row, _) => { Interlocked.Increment(ref delivered); row.Dispose(); });

        await client.UnsubscribeAsync(id);
        gateway.Deliver(id, new Envelope(EncodeRow(1, "after")));
        await client.ListTopicsAsync();

        Assert.Equal(0, delivered);
        Assert.Empty(gateway.Subscriptions);
    }

    [Fact]
    public async Task AThrowingHandlerIsCountedAndReportedAndTheSubscriptionGoesOn()
    {
        var gateway = new FakeGateway();
        await using FletcherClient client = await Connected(gateway);
        var faults = new BlockingCollection<GatewayHandlerFaultedEventArgs>();
        client.HandlerFaulted += (_, e) => faults.Add(e);
        var good = new BlockingCollection<int>();
        ulong id = await client.SubscribeAsync("t", RowSchema(), (row, _) =>
        {
            using (row)
            {
                int value = ((Int32Array)row.Column(0)).GetValue(0)!.Value;
                if (value == 1)
                {
                    throw new InvalidOperationException("handler bug");
                }

                good.Add(value);
            }
        });

        using RecordBatch batch = Batch((1, "bad"), (2, "good"));
        await client.PublishAsync("t", batch);

        Assert.True(faults.TryTake(out GatewayHandlerFaultedEventArgs? fault, Wait));
        Assert.Equal("handler bug", fault!.Exception.Message);
        Assert.Equal(GatewayFaultReason.HandlerThrew, fault.Reason);
        Assert.Equal(id, fault.SubscriptionId);
        Assert.True(good.TryTake(out int value, Wait));
        Assert.Equal(2, value);
        Assert.Equal(1ul, client.AbsorbedHandlerFailures);
    }

    [Fact]
    public async Task ARowThatDoesNotDecodeIsDroppedAndReported()
    {
        var gateway = new FakeGateway();
        await using FletcherClient client = await Connected(gateway);
        var faults = new BlockingCollection<GatewayHandlerFaultedEventArgs>();
        client.HandlerFaulted += (_, e) => faults.Add(e);
        ulong id = await client.SubscribeAsync("t", RowSchema(), (row, _) => row.Dispose());

        gateway.Deliver(id, new Envelope(new byte[] { 0x00, 0x01 }));

        Assert.True(faults.TryTake(out GatewayHandlerFaultedEventArgs? fault, Wait));
        Assert.IsType<System.IO.InvalidDataException>(fault!.Exception);
        Assert.Equal(GatewayFaultReason.RowUndecodable, fault.Reason);
        Assert.Equal(1ul, client.UndecodableRows);
        Assert.Equal(0ul, client.AbsorbedHandlerFailures);
    }

    [Fact]
    public async Task TheGatewayClosingFailsTheRequestInFlightAndEveryLaterCall()
    {
        var gateway = new FakeGateway { HoldReplies = true };
        await using FletcherClient client = await Connected(gateway);

        Task pending = client.CreateTopicAsync("t");
        await WaitUntil(() => gateway.Received.Count == 1);
        gateway.Close();

        GatewayException e = await Assert.ThrowsAsync<GatewayException>(() => pending.WaitAsync(Wait));
        Assert.Contains("closed the connection", e.InnerException!.Message, StringComparison.Ordinal);
        await Assert.ThrowsAsync<GatewayException>(() => client.ListTopicsAsync());
    }

    // ── BIND-8e: the review's blockers and rulings ──

    [Fact]
    public async Task ACancelledSubscribeLeavesNoSubscriptionBehind()
    {
        // Review B2: cancelled after the subscribe was sent, the call returns at once, and the
        // reply that follows used to register the handler under an id the caller never got.
        var gateway = new FakeGateway { HoldReplies = true };
        await using FletcherClient client = await Connected(gateway);
        int delivered = 0;
        using var cancel = new CancellationTokenSource();

        Task<ulong> subscribe = client.SubscribeAsync("t", RowSchema(), (row, _) => { Interlocked.Increment(ref delivered); row.Dispose(); }, cancel.Token);
        await WaitUntil(() => gateway.Subscriptions.Count == 1);
        ulong id = gateway.Subscriptions.Single();
        cancel.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => subscribe);

        gateway.ReleaseHeld();
        await WaitUntil(() => gateway.Subscriptions.Count == 0);
        gateway.Deliver(id, new Envelope(EncodeRow(1, "orphan")));
        await client.ListTopicsAsync();

        Assert.Equal(0, delivered);
        Assert.Equal(0, client.SubscriptionCount);
    }

    [Fact]
    public async Task CompletionFaultsWhenTheGatewayClosesTheConnection()
    {
        // D-BIND-121: a subscriber-only app learns that its subscriptions are gone.
        var gateway = new FakeGateway();
        await using FletcherClient client = await Connected(gateway);
        await client.SubscribeAsync("t", RowSchema(), (row, _) => row.Dispose());
        Assert.False(client.Completion.IsCompleted);

        gateway.Close();

        GatewayException e = await Assert.ThrowsAsync<GatewayException>(() => client.Completion.WaitAsync(Wait));
        Assert.Contains("closed the connection", e.Message, StringComparison.Ordinal);
        Assert.Equal(0, client.SubscriptionCount);
    }

    [Fact]
    public async Task CompletionCompletesOnDispose()
    {
        var gateway = new FakeGateway();
        FletcherClient client = await Connected(gateway);

        await client.DisposeAsync();

        await client.Completion.WaitAsync(Wait);
        Assert.True(client.Completion.IsCompletedSuccessfully);
    }

    [Fact]
    public async Task AReplyThatCannotBeParsedFailsOnlyItsRequest()
    {
        // D-BIND-124: an unknown reply type fails the request in flight; the connection stays.
        var gateway = new FakeGateway { NextReplyText = "{\"type\":\"from_a_newer_gateway\"}" };
        await using FletcherClient client = await Connected(gateway);

        GatewayException e = await Assert.ThrowsAsync<GatewayException>(() => client.ListTopicsAsync());
        Assert.Contains("list_topics", e.Message, StringComparison.Ordinal);

        await client.CreateTopicAsync("still-up");
        Assert.Contains("still-up", await client.ListTopicsAsync());
        Assert.False(client.Completion.IsCompleted);
    }

    [Fact]
    public async Task AReplyNobodyAskedForIsIgnored()
    {
        var gateway = new FakeGateway();
        await using FletcherClient client = await Connected(gateway);
        var seen = new TaskCompletionSource();
        ulong id = await client.SubscribeAsync("t", RowSchema(), (row, _) => { row.Dispose(); seen.TrySetResult(); });

        // With no request in flight; the row after them shows when the loop has read both.
        gateway.Transport.ToClient(System.Net.WebSockets.WebSocketMessageType.Text, Encoding.UTF8.GetBytes("{\"type\":\"surprise\"}"));
        gateway.Transport.ToClient(System.Net.WebSockets.WebSocketMessageType.Text, Encoding.UTF8.GetBytes("not json"));
        gateway.Deliver(id, new Envelope(EncodeRow(1, "after")));
        await seen.Task.WaitAsync(Wait);

        await client.CreateTopicAsync("t");
        Assert.False(client.Completion.IsCompleted);
    }

    [Fact]
    public async Task AnUnreadableGatewaySchemaDoesNotStopASubscriberThatBroughtItsOwn()
    {
        // D-BIND-124: a native publisher's schema this client cannot read used to end the
        // connection, even for a subscriber that never needed it.
        var gateway = new FakeGateway { SchemaIpcOverride = "AAAA" };
        await using FletcherClient client = await Connected(gateway);
        await client.CreateTopicAsync("t", RowSchema());
        var labels = new BlockingCollection<string>();

        await client.SubscribeAsync("t", RowSchema(), (row, _) => { using (row) { labels.Add(((StringArray)row.Column(1)).GetString(0)); } });
        using RecordBatch batch = Batch((1, "own-schema"));
        await client.PublishAsync("t", batch);

        Assert.True(labels.TryTake(out string? label, Wait));
        Assert.Equal("own-schema", label);
    }

    [Fact]
    public async Task AnUnreadableGatewaySchemaWithNoOwnSchemaFailsTheSubscribeAndSaysWhy()
    {
        var gateway = new FakeGateway { SchemaIpcOverride = "AAAA" };
        await using FletcherClient client = await Connected(gateway);
        await client.CreateTopicAsync("t", RowSchema());

        InvalidOperationException e = await Assert.ThrowsAsync<InvalidOperationException>(
            () => client.SubscribeAsync("t", schema: null, (row, _) => row.Dispose()));

        Assert.Contains("cannot be read", e.Message, StringComparison.Ordinal);
        Assert.Empty(gateway.Subscriptions);
        Assert.False(client.Completion.IsCompleted);
    }

    [Fact]
    public async Task AMessageAboveMaxMessageBytesEndsTheConnectionNamingTheSize()
    {
        // D-BIND-126.
        var gateway = new FakeGateway();
        await using FletcherClient client = await Connected(gateway, new FletcherClientOptions { Uri = FakeUri, MaxMessageBytes = 1024 });
        ulong id = await client.SubscribeAsync("t", RowSchema(), (row, _) => row.Dispose());

        gateway.Deliver(id, new Envelope(EncodeRow(1, new string('x', 2048))));

        GatewayException e = await Assert.ThrowsAsync<GatewayException>(() => client.Completion.WaitAsync(Wait));
        Assert.Contains("1024", e.Message, StringComparison.Ordinal);
        Assert.True(gateway.Transport.Aborted, "a connection the client ended was not dropped");
    }

    [Fact]
    public async Task ConfigureWebSocketRunsBeforeTheSocketConnects()
    {
        // D-BIND-126, on the real transport: the hook sees the socket's options first.
        var seen = new TaskCompletionSource<System.Net.WebSockets.ClientWebSocketOptions>();
        await using var client = new FletcherClient(new FletcherClientOptions
        {
            Uri = new Uri("ws://127.0.0.1:1"),
            ConfigureWebSocket = o =>
            {
                o.SetRequestHeader("Authorization", "Bearer test");
                seen.TrySetResult(o);
            },
        });

        await Assert.ThrowsAnyAsync<Exception>(() => client.ConnectAsync());
        Assert.True(seen.Task.IsCompleted, "ConfigureWebSocket was not called");
    }

    [Fact]
    public async Task AReplyThatDoesNotComeInTimeEndsTheConnection()
    {
        // D-BIND-127: the request fails, Completion names it, and the socket is dropped.
        var gateway = new FakeGateway { HoldReplies = true };
        await using FletcherClient client = await Connected(gateway, new FletcherClientOptions { Uri = FakeUri, ReplyTimeout = TimeSpan.FromMilliseconds(200) });

        await Assert.ThrowsAsync<GatewayException>(() => client.CreateTopicAsync("t").WaitAsync(Wait));

        GatewayException e = await Assert.ThrowsAsync<GatewayException>(() => client.Completion.WaitAsync(Wait));
        Assert.Contains("no reply to create_topic", e.Message, StringComparison.Ordinal);
        Assert.True(gateway.Transport.Aborted);
    }

    [Fact]
    public async Task ARowForNoSubscriptionIsCountedAsDropped()
    {
        // D-BIND-132: rows for a subscription the client does not hold were dropped uncounted.
        var gateway = new FakeGateway();
        await using FletcherClient client = await Connected(gateway);

        gateway.Deliver(999, new Envelope(EncodeRow(1, "nobody")));
        await client.ListTopicsAsync();

        await WaitUntil(() => client.DroppedRows == 1);
        Assert.Equal(0ul, client.UndecodableRows);
    }

    [Fact]
    public async Task ACallWhileConnectIsRunningIsRefusedAndTheClientStaysUsable()
    {
        // Review DEBT: such a call used to send on the unopened socket and end the client for good.
        var gateway = new FakeGateway();
        var open = new TaskCompletionSource();
        gateway.Transport.ConnectGate = open.Task;
        await using FletcherClient client = Client(gateway);

        Task connect = client.ConnectAsync();
        InvalidOperationException e = await Assert.ThrowsAsync<InvalidOperationException>(() => client.ListTopicsAsync());
        Assert.Contains("has not completed", e.Message, StringComparison.Ordinal);

        open.SetResult();
        await connect.WaitAsync(Wait);
        await client.CreateTopicAsync("t");
    }

    [Fact]
    public async Task AHandlerMayDisposeItsClient()
    {
        // Review DEBT: DisposeAsync from a handler waited for the loop the handler was running on.
        var gateway = new FakeGateway();
        FletcherClient client = await Connected(gateway);
        var disposed = new TaskCompletionSource<TimeSpan>();
        ulong id = await client.SubscribeAsync("t", RowSchema(), (row, _) =>
        {
            row.Dispose();
            var clock = System.Diagnostics.Stopwatch.StartNew();
            client.DisposeAsync().AsTask().GetAwaiter().GetResult();
            disposed.TrySetResult(clock.Elapsed);
        });

        gateway.Deliver(id, new Envelope(EncodeRow(1, "bye")));

        // Prompt, not merely bounded: waiting on its own loop would cost the close grace (2 s).
        TimeSpan took = await disposed.Task.WaitAsync(Wait);
        Assert.True(took < TimeSpan.FromSeconds(1), $"DisposeAsync from a handler took {took}");
        await client.Completion.WaitAsync(Wait);
    }

    // ── Helpers ──

    private static readonly Uri FakeUri = new("ws://fake:9090");

    private static FletcherClient Client(FakeGateway gateway, FletcherClientOptions? options = null) =>
        new(options ?? new FletcherClientOptions { Uri = FakeUri }, gateway.Transport);

    private static async Task<FletcherClient> Connected(FakeGateway gateway, FletcherClientOptions? options = null)
    {
        FletcherClient client = Client(gateway, options);
        await client.ConnectAsync();
        return client;
    }

    // As the generator writes a schema: proto3 non-null scalars with their field numbers.
    private static Schema RowSchema() => new Schema.Builder()
        .Field(f => f.Name("id").DataType(Int32Type.Default).Nullable(false).Metadata(ControlMessages.FieldNumberKey, "1"))
        .Field(f => f.Name("label").DataType(StringType.Default).Nullable(false).Metadata(ControlMessages.FieldNumberKey, "2"))
        .Build();

    private static RecordBatch Batch(params (int Id, string Label)[] rows) => new(
        RowSchema(),
        new IArrowArray[]
        {
            new Int32Array.Builder().AppendRange(rows.Select(r => r.Id)).Build(),
            new StringArray.Builder().AppendRange(rows.Select(r => r.Label)).Build(),
        },
        rows.Length);

    private static byte[] EncodeRow(int id, string label)
    {
        using RecordBatch batch = Batch((id, label));
        var codec = new GatewayRowCodec(batch.Schema);
        return codec.Encode(codec.Bind(batch), 0);
    }

    // A publish whose send outlived a dispose either finished (its reply was read first) or
    // failed as disposed; it never hangs and never fails otherwise.
    private static void AssertPublishEnded(Exception? outcome) =>
        Assert.True(outcome is null or ObjectDisposedException, "the publish ended with " + outcome);

    private static async Task WaitUntil(Func<bool> condition)
    {
        DateTime deadline = DateTime.UtcNow + Wait;
        while (!condition())
        {
            if (DateTime.UtcNow > deadline)
            {
                throw new TimeoutException("condition not reached");
            }

            await Task.Delay(10);
        }
    }
}
