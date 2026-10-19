// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// BIND-8b: FletcherClient against the real gateway exe, once per provider.
//
// What the in-memory FakeGateway cannot prove, a live gateway does:
//
// * rows of a GENERATED message with a nested message, a Timestamp, a repeated field and a
//   map cross a real WebSocket and a real provider and come back field for field, through
//   the model's ToArrow / FromArrow (D-BIND-108);
// * create_topic's schemaIpc (D-BIND-112), written by Apache.Arrow in C#, is read by the
//   gateway's nanoarrow DeserializeSchemaIpc: a second client re-declaring the topic with the
//   same generated schema is accepted (the gateway's conflict check compares the two
//   declarations' Arrow IPC byte for byte), a different schema is refused, and a subscriber
//   that brings no schema decodes with the one the gateway hands back;
// * the gateway's real refusals reach the call that caused them (D-BIND-110), its real
//   `published` acknowledgements pace a batch (D-BIND-111), and attachments survive the trip.
//
// Every test uses its own topic, so the cases do not see each other's rows. Over Fast DDS
// the publisher and subscriber share one gateway process, so delivery stays inside it and
// the data-sharing defect the gateway-fastdds-ts lane guards (two processes) does not apply.
using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

using Apache.Arrow;
using Apache.Arrow.Types;

using Eiva.Fletcher.GatewayClient;

using Fletcher.Gen.Gateway.Endtoend;

using Xunit;

using Timestamp = Eiva.Fletcher.Model.Timestamp;

namespace Eiva.Fletcher.GatewayDotnetEndToEnd;

public sealed class InProcessEndToEndTests : EndToEndTests, IClassFixture<InProcessGateway>
{
    public InProcessEndToEndTests(InProcessGateway gateway)
        : base(gateway)
    {
    }
}

public sealed class FastDdsEndToEndTests : EndToEndTests, IClassFixture<FastDdsGateway>
{
    public FastDdsEndToEndTests(FastDdsGateway gateway)
        : base(gateway)
    {
    }
}

public abstract class EndToEndTests
{
    private readonly GatewayProcess _gateway;

    protected EndToEndTests(GatewayProcess gateway)
    {
        _gateway = gateway;
    }

    [Fact]
    public async Task GeneratedRowsRoundTripFieldForField()
    {
        string topic = Topic();
        await using FletcherClient publisher = await Connect();
        await using FletcherClient subscriber = await Connect();
        await publisher.CreateTopicAsync(topic, Fix.Schema);
        var received = new BlockingCollection<Fix>();
        await subscriber.SubscribeAsync(topic, Fix.Schema, (row, _) => { using (row) { received.Add(Fix.FromArrow(AsStruct(row), 0)); } });

        Fix[] sent = { MakeFix(1, "north"), MakeFix(2, "Æøå"), MakeFix(3, string.Empty) };
        using RecordBatch batch = Fix.ToArrow(sent);
        await publisher.PublishAsync(topic, batch);

        foreach (Fix expected in sent)
        {
            Assert.True(received.TryTake(out var actual, _gateway.RoundTrip), $"row {expected.Id} did not arrive over {_gateway.Provider}");
            AssertSame(expected, actual!);
        }
    }

    [Fact]
    public async Task TheGeneratedGatewayPairRoundTripsTypedRows()
    {
        // BIND-8c: the pair csharp_gateway generates for FixFeed.Report, over ONE borrowed client
        // per side (D-BIND-115), on the method's own topic key.
        await using FletcherClient publisherClient = await Connect();
        await using FletcherClient subscriberClient = await Connect();
        FixFeed_ReportGatewayPublisher publisher = await FixFeed_ReportGatewayPublisher.CreateAsync(publisherClient);
        var received = new BlockingCollection<Fix>();
        ulong id = await new FixFeed_ReportGatewaySubscriber(subscriberClient).SubscribeAsync((row, _) => received.Add(row));

        Fix[] sent = { MakeFix(11, "typed"), MakeFix(12, "pair") };
        await publisher.PublishAsync(sent);

        foreach (Fix expected in sent)
        {
            Assert.True(received.TryTake(out var actual, _gateway.RoundTrip), $"row {expected.Id} did not arrive over {_gateway.Provider}");
            AssertSame(expected, actual!);
        }

        await subscriberClient.UnsubscribeAsync(id);
    }

    [Fact]
    public async Task ASecondDeclarationWithTheSameGeneratedSchemaIsAcceptedAndADifferentOneRefused()
    {
        // D-BIND-112 at the real gateway: C#'s schemaIpc decodes, through nanoarrow, to a
        // declaration equal to itself; the JSON path's would not have.
        string topic = Topic();
        await using FletcherClient first = await Connect();
        await using FletcherClient second = await Connect();

        await first.CreateTopicAsync(topic, Fix.Schema);
        await second.CreateTopicAsync(topic, Fix.Schema);

        GatewayException conflict = await Assert.ThrowsAsync<GatewayException>(() => second.CreateTopicAsync(topic, Position.Schema));
        Assert.Contains("conflict", conflict.Message, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public async Task ASubscriberWithoutASchemaDecodesWithTheOneThePublisherAnnounced()
    {
        string topic = Topic();
        await using FletcherClient publisher = await Connect();
        await using FletcherClient subscriber = await Connect();
        await publisher.CreateTopicAsync(topic, Fix.Schema);
        var received = new BlockingCollection<Fix>();

        await subscriber.SubscribeAsync(topic, schema: null, (row, _) => { using (row) { received.Add(Fix.FromArrow(AsStruct(row), 0)); } });
        using RecordBatch batch = Fix.ToArrow(new[] { MakeFix(7, "announced") });
        await publisher.PublishAsync(topic, batch);

        Assert.True(received.TryTake(out var actual, _gateway.RoundTrip));
        AssertSame(MakeFix(7, "announced"), actual!);
    }

    [Fact]
    public async Task AttachmentsArriveWithTheirRow()
    {
        string topic = Topic();
        await using FletcherClient publisher = await Connect();
        await using FletcherClient subscriber = await Connect();
        await publisher.CreateTopicAsync(topic, Fix.Schema);
        var notes = new BlockingCollection<string>();
        await subscriber.SubscribeAsync(topic, Fix.Schema, (row, envelope) =>
        {
            row.Dispose();
            notes.Add(envelope.TryFind("note", out ReadOnlyMemory<byte> note) ? Encoding.UTF8.GetString(note.Span) : "<none>");
        });

        using RecordBatch batch = Fix.ToArrow(new[] { MakeFix(1, "a") });
        await publisher.PublishAsync(topic, batch, 0, new[] { new KeyValuePair<string, ReadOnlyMemory<byte>>("note", Encoding.UTF8.GetBytes("attached")) });

        Assert.True(notes.TryTake(out string? received, _gateway.RoundTrip));
        Assert.Equal("attached", received);
    }

    [Fact]
    public async Task TheGatewaysRefusalReachesTheCallThatCausedItAndTheConnectionGoesOn()
    {
        await using FletcherClient client = await Connect();

        // An empty segment is refused by the gateway, not repaired (as the TS lane pins it).
        await Assert.ThrowsAsync<GatewayException>(() => client.CreateTopicAsync("a//b"));

        string topic = Topic();
        await client.CreateTopicAsync(topic);
        Assert.Contains(topic, await client.ListTopicsAsync());
    }

    [Fact]
    public async Task UnsubscribingStopsDelivery()
    {
        string topic = Topic();
        await using FletcherClient publisher = await Connect();
        await using FletcherClient subscriber = await Connect();
        await publisher.CreateTopicAsync(topic, Fix.Schema);
        var received = new BlockingCollection<int>();
        ulong id = await subscriber.SubscribeAsync(topic, Fix.Schema, (row, _) => { using (row) { received.Add(Fix.FromArrow(AsStruct(row), 0).Id); } });

        using (RecordBatch before = Fix.ToArrow(new[] { MakeFix(1, "before") }))
        {
            await publisher.PublishAsync(topic, before);
        }

        Assert.True(received.TryTake(out int first, _gateway.RoundTrip));
        Assert.Equal(1, first);

        await subscriber.UnsubscribeAsync(id);
        using (RecordBatch after = Fix.ToArrow(new[] { MakeFix(2, "after") }))
        {
            await publisher.PublishAsync(topic, after);
        }

        Assert.False(received.TryTake(out _, TimeSpan.FromSeconds(1)));
    }

    // ── Helpers ──

    private async Task<FletcherClient> Connect()
    {
        var client = new FletcherClient(new FletcherClientOptions { Uri = _gateway.Uri });
        await client.ConnectAsync();
        return client;
    }

    private static string Topic() => "gateway.endtoend/FixFeed/" + Guid.NewGuid().ToString("N");

    private static Fix MakeFix(int id, string label) => new()
    {
        Id = id,
        Label = label,
        At = new Position { Lat = 55.676 + id, Lon = 12.568 - id },
        When = new Timestamp(1_700_000_000_000_000_000L + id, TimeUnit.Nanosecond),
        Readings = { 1.5 * id, -2.25, 0.0 },
        Tags = { new KeyValuePair<string, int>("hdg", 90 * id), new KeyValuePair<string, int>("sat", 12) },
    };

    private static void AssertSame(Fix expected, Fix actual)
    {
        Assert.Equal(expected.Id, actual.Id);
        Assert.Equal(expected.Label, actual.Label);
        Assert.Equal(expected.At.Lat, actual.At.Lat);
        Assert.Equal(expected.At.Lon, actual.At.Lon);
        Assert.Equal(expected.When, actual.When);
        Assert.Equal(expected.Readings, actual.Readings);
        Assert.Equal(expected.Tags, actual.Tags);
    }

    private static StructArray AsStruct(RecordBatch batch) =>
        new(new StructType(batch.Schema.FieldsList), batch.Length, batch.Arrays, ArrowBuffer.Empty, 0);
}
