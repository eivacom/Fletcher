// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// BIND-8c: the GENERATED gateway pair, compiled from the plugin's golden
// (protoc/tests/golden/csharp_pair.fletcher.gateway.cs, held to the plugin byte for byte by
// test_csharp_visitor), against the in-memory gateway.
//
// What the pair adds over FletcherClient is typing, so that is what these cases hold: a
// publisher created on a borrowed client declares its topic with the message's schema
// (D-BIND-112, D-BIND-115), typed rows go out through the model's ToArrow and come back through
// its FromArrow with every kind csharp_pair.proto carries (scalars, an optional, an enum, a
// list, a map, a nested message, a list of messages, a Timestamp; and, on the second method,
// another file's message), attachments ride along, a batch is one frame per row, and an
// unsubscribed handler hears nothing. One client serves both pairs and both methods.
//
// The live round trip is integration-tests/gateway-dotnet-end-to-end's.
using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

using Apache.Arrow.Types;

using Fletcher.Gen.Golden.Pair;

using Xunit;

using ProtoTimestamp = Fletcher.Gen.Google.Protobuf.Timestamp;
using Timestamp = Eiva.Fletcher.Model.Timestamp;

namespace Eiva.Fletcher.GatewayClient.Tests;

public sealed class GeneratedGatewayPairTests
{
    private static readonly TimeSpan Wait = TimeSpan.FromSeconds(10);

    [Fact]
    public async Task CreatingAPublisherDeclaresTheTopicWithTheMessagesSchema()
    {
        var gateway = new FakeGateway();
        await using FletcherClient client = await Connected(gateway);

        await Telemetry_ReportGatewayPublisher.CreateAsync(client);

        Assert.Equal("golden/pair/Telemetry/Report", Telemetry_ReportGatewayPublisher.TopicKey);
        global::Apache.Arrow.Schema? declared = gateway.DeclaredSchema(Telemetry_ReportGatewayPublisher.TopicKey);
        Assert.NotNull(declared);
        Assert.Equal(Reading.Schema.FieldsList.Select(f => (f.Name, f.DataType.TypeId, f.IsNullable)),
            declared!.FieldsList.Select(f => (f.Name, f.DataType.TypeId, f.IsNullable)));
    }

    [Fact]
    public async Task ATypedRowRoundTripsWithEveryKindTheMessageCarries()
    {
        var gateway = new FakeGateway();
        await using FletcherClient client = await Connected(gateway);
        Telemetry_ReportGatewayPublisher publisher = await Telemetry_ReportGatewayPublisher.CreateAsync(client);
        var subscriber = new Telemetry_ReportGatewaySubscriber(client);
        var received = new BlockingCollection<(Reading Row, string Note)>();
        await subscriber.SubscribeAsync((row, envelope) =>
            received.Add((row, envelope.TryFind("note", out ReadOnlyMemory<byte> note) ? Encoding.UTF8.GetString(note.Span) : "<none>")));

        Reading sent = MakeReading(7);
        await publisher.PublishAsync(sent, new[] { new KeyValuePair<string, ReadOnlyMemory<byte>>("note", Encoding.UTF8.GetBytes("typed")) });

        Assert.True(received.TryTake(out var got, Wait));
        AssertSame(sent, got.Row);
        Assert.Equal("typed", got.Note);
    }

    [Fact]
    public async Task ABatchOfRowsIsPublishedOneFrameEachAndArrivesInOrder()
    {
        var gateway = new FakeGateway();
        await using FletcherClient client = await Connected(gateway);
        Telemetry_ReportGatewayPublisher publisher = await Telemetry_ReportGatewayPublisher.CreateAsync(client);
        var ids = new BlockingCollection<int>();
        await new Telemetry_ReportGatewaySubscriber(client).SubscribeAsync((row, _) => ids.Add(row.Id));

        await publisher.PublishAsync(new[] { MakeReading(1), MakeReading(2), MakeReading(3) });
        await publisher.PublishAsync(Array.Empty<Reading>());

        Assert.Equal(3, gateway.Received.Count(r => r.Action == "publish"));
        Assert.Equal(new[] { 1, 2, 3 }, Enumerable.Range(0, 3).Select(_ => { Assert.True(ids.TryTake(out int id, Wait)); return id; }));
    }

    [Fact]
    public async Task AMessageFromAnotherFileTravelsThroughItsPair()
    {
        // Telemetry.Mark carries Marks, whose list holds timestamp.proto's generated class.
        var gateway = new FakeGateway();
        await using FletcherClient client = await Connected(gateway);
        Telemetry_MarkGatewayPublisher publisher = await Telemetry_MarkGatewayPublisher.CreateAsync(client);
        var received = new BlockingCollection<Marks>();
        await new Telemetry_MarkGatewaySubscriber(client).SubscribeAsync((row, _) => received.Add(row));

        var sent = new Marks { At = { new ProtoTimestamp { Seconds = 1_700_000_000, Nanos = 5 }, new ProtoTimestamp { Seconds = -1, Nanos = 0 } } };
        await publisher.PublishAsync(sent);

        Assert.True(received.TryTake(out Marks? got, Wait));
        Assert.Equal(sent.At.Select(t => (t.Seconds, t.Nanos)), got!.At.Select(t => (t.Seconds, t.Nanos)));
    }

    [Fact]
    public async Task OneClientServesBothMethodsAndAnUnsubscribedHandlerHearsNothing()
    {
        var gateway = new FakeGateway();
        await using FletcherClient client = await Connected(gateway);
        Telemetry_ReportGatewayPublisher reports = await Telemetry_ReportGatewayPublisher.CreateAsync(client);
        Telemetry_MarkGatewayPublisher marks = await Telemetry_MarkGatewayPublisher.CreateAsync(client);
        var subscriber = new Telemetry_ReportGatewaySubscriber(client);
        int heard = 0;
        ulong id = await subscriber.SubscribeAsync((_, _) => heard++);

        await subscriber.UnsubscribeAsync(id);
        await reports.PublishAsync(MakeReading(1));
        await marks.PublishAsync(new Marks());

        Assert.Equal(0, heard);
        Assert.Equal(new[] { Telemetry_MarkGatewayPublisher.TopicKey, Telemetry_ReportGatewayPublisher.TopicKey }, await client.ListTopicsAsync());
    }

    [Fact]
    public async Task ThePairRefusesANullClientRowOrHandler()
    {
        var gateway = new FakeGateway();
        await using FletcherClient client = await Connected(gateway);
        Telemetry_ReportGatewayPublisher publisher = await Telemetry_ReportGatewayPublisher.CreateAsync(client);

        await Assert.ThrowsAsync<ArgumentNullException>(() => Telemetry_ReportGatewayPublisher.CreateAsync(null!));
        Assert.Throws<ArgumentNullException>(() => new Telemetry_ReportGatewaySubscriber(null!));
        await Assert.ThrowsAsync<ArgumentNullException>(() => publisher.PublishAsync((Reading)null!));
        await Assert.ThrowsAsync<ArgumentNullException>(() => new Telemetry_ReportGatewaySubscriber(client).SubscribeAsync(null!));
    }

    private static async Task<FletcherClient> Connected(FakeGateway gateway)
    {
        var client = new FletcherClient(new FletcherClientOptions { Uri = new Uri("ws://fake:9090") }, gateway.Transport);
        await client.ConnectAsync();
        return client;
    }

    private static Reading MakeReading(int id) => new()
    {
        Id = id,
        Name = "reading-" + id,
        Value = id % 2 == 0 ? null : 1.5 * id,
        Mood = id % 2 == 0 ? Mood.Happy : Mood.Unspecified,
        Samples = { id, -id, 0 },
        Tags = { new KeyValuePair<string, long>("a", id), new KeyValuePair<string, long>("b", long.MaxValue) },
        Stamp = new Stamp { By = "pair" },
        History = { new Stamp { By = "h1" }, new Stamp { By = "Æøå" } },
        At = new Timestamp(1_700_000_000_000_000_000L + id, TimeUnit.Nanosecond),
    };

    private static void AssertSame(Reading expected, Reading actual)
    {
        Assert.Equal(expected.Id, actual.Id);
        Assert.Equal(expected.Name, actual.Name);
        Assert.Equal(expected.Value, actual.Value);
        Assert.Equal(expected.Mood, actual.Mood);
        Assert.Equal(expected.Samples, actual.Samples);
        Assert.Equal(expected.Tags, actual.Tags);
        Assert.Equal(expected.Stamp.By, actual.Stamp.By);
        Assert.Equal(expected.History.Select(h => h.By), actual.History.Select(h => h.By));
        Assert.Equal(expected.At, actual.At);
    }
}
