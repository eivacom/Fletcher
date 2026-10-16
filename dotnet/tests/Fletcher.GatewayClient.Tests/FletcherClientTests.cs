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
        var gateway = new FakeGateway();
        FletcherClient client = await Connected(gateway);
        int delivered = 0;
        ulong id = await client.SubscribeAsync("t", RowSchema(), (row, _) => { Interlocked.Increment(ref delivered); row.Dispose(); });

        await client.DisposeAsync();
        gateway.Deliver(id, new Envelope(EncodeRow(1, "late")));
        await Task.Delay(100);

        Assert.Equal(0, delivered);
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
        // D-BIND-110: twenty calls started at once still reach the gateway one by one.
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

    // ── Helpers ──

    private static FletcherClient Client(FakeGateway gateway) =>
        new(new FletcherClientOptions { Uri = new Uri("ws://fake:9090") }, gateway.Transport);

    private static async Task<FletcherClient> Connected(FakeGateway gateway)
    {
        FletcherClient client = Client(gateway);
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
