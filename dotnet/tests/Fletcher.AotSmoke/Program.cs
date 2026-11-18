// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// BIND-6e, D-BIND-81: what a NativeAOT consumer of the generated pair does, end to end.
//
// Each row crosses everything a trimmer could break: the generated ToArrow and FromArrow
// (lists, a map, a nested message, a lossless Timestamp, and Marks' list of a class from
// another file, built through its public ToArrow, D-BIND-80), the codec behind the C ABI, the
// provider, and the generated subscriber's delegate. The exit code is the verdict: 0 only
// when exactly the rows that were sent come back.
//
// BIND-9b-3 (D-BIND-88): `Fletcher.AotSmoke fastdds` is the same crossing over a real transport,
// so a delivery enters AOT-compiled code on a Fast DDS thread. That takes TWO processes: Fast
// DDS hands a sample written in the same process to the reader on the writer's own thread
// (intraprocess delivery), which is what `inprocess` already covers. So this process relaunches
// itself as `fastdds-subscribe <topic>`, and republishes one row on that GUID topic every 100 ms
// until the child exits, because a sample written before discovery matched is lost. Each side
// has a deadline; the child's exit code is the verdict.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Linq;
using System.Threading;

using Apache.Arrow;
using Apache.Arrow.Types;

using Eiva.Fletcher;
using Eiva.Fletcher.GatewayClient;
using Eiva.Fletcher.Model;

using Fletcher.Gen.Golden.Pair;

using PbTimestamp = Fletcher.Gen.Google.Protobuf.Timestamp;

TimeSpan fastDdsDeadline = TimeSpan.FromSeconds(30);
switch (args)
{
    case []:
        break;
    case ["fastdds"]:
        return RunFastDdsPublisher();
    case ["fastdds-subscribe", string topicSegment]:
        return RunFastDdsSubscriber(topicSegment);
    default:
        Console.Error.WriteLine("usage: Fletcher.AotSmoke [fastdds]");
        return 2;
}

using PubSubProviderHandle provider = ProviderRegistry.Create(ProviderSelector.Parse("inprocess"), new ProviderConfig());

var seen = new List<string>();
using var readings = new Telemetry_ReportSubscriber(provider);
using Subscription readingSubscription = readings.Subscribe((row, attachments) =>
    seen.Add(Describe(row)));
using var marks = new Telemetry_MarkSubscriber(provider);
using Subscription marksSubscription = marks.Subscribe((row, attachments) =>
    seen.Add("marks " + string.Join(',', row.At.ConvertAll(t => $"{t.Seconds}:{t.Nanos}"))));

using (var publisher = new Telemetry_ReportPublisher(provider))
{
    publisher.Publish(MakeReading());
}

using (var publisher = new Telemetry_MarkPublisher(provider))
{
    publisher.Publish(new[] { new Marks { At = { new PbTimestamp { Seconds = 5, Nanos = 6 }, new PbTimestamp { Seconds = -1, Nanos = 0 } } } });
}

// ── BIND-8e (review of BIND-8): the gateway client and its generated pair under NativeAOT ──
// No gateway runs here, so the managed codec and the schema IPC reader are reached directly
// (InternalsVisibleTo), and the generated gateway publisher is driven on a client that never
// connected: it writes its schemaIpc, then is refused, which is the path a trimmer could break.
using (RecordBatch gatewayBatch = Reading.ToArrow(new[] { MakeReading() }))
{
    var codec = new GatewayRowCodec(Reading.Schema);
    byte[] encoded = codec.Encode(codec.Bind(gatewayBatch), 0);
    using RecordBatch decoded = codec.Decode(encoded);
    var asStruct = new StructArray(new StructType(decoded.Schema.FieldsList), decoded.Length, decoded.Arrays, ArrowBuffer.Empty, 0);
    seen.Add("gateway " + Describe(Reading.FromArrow(asStruct, 0)));
}

// BIND-8f (D-BIND-133): a batched subscription's window, cut at MaxRows and decoded by one
// DecodeBatch, driven through its batcher directly as the receive loop would.
await using (var batchClient = new FletcherClient(new FletcherClientOptions { Uri = new Uri("ws://127.0.0.1:9") }))
{
    var codec = new GatewayRowCodec(Reading.Schema);
    var batcher = new GatewayBatcher(batchClient, 1, codec, (batch, envelopes, status) =>
    {
        using (batch)
        {
            seen.Add($"batched {batch.Length} {envelopes.Count} {status.Reason} {status.RowsDropped}");
        }
    }, new GatewayBatchOptions { MaxRows = 2, Timeout = System.Threading.Timeout.InfiniteTimeSpan });
    using RecordBatch rows = Reading.ToArrow(new[] { MakeReading(), MakeReading() });
    BoundBatch bound = codec.Bind(rows);
    for (int i = 0; i < 2; ++i)
    {
        batcher.Add(new Envelope(codec.Encode(bound, i)).Serialize());
    }
}

Schema readBack = ControlMessages.ReadSchemaIpc(Convert.ToBase64String(ControlMessages.WriteSchemaIpc(Reading.Schema)));
seen.Add("schemaIpc " + (readBack.FieldsList.Count == Reading.Schema.FieldsList.Count));

await using (var gatewayClient = new FletcherClient(new FletcherClientOptions { Uri = new Uri("ws://127.0.0.1:9") }))
{
    try
    {
        await Telemetry_ReportGatewayPublisher.CreateAsync(gatewayClient);
        seen.Add("an unconnected client declared a topic");
    }
    catch (InvalidOperationException)
    {
        seen.Add("unconnected refused");
    }
}

string[] expected =
{
    "7 aot 0.5 Happy 1,-2 2 x 1 1700000000123456789",
    "marks 5:6,-1:0",
    "gateway 7 aot 0.5 Happy 1,-2 2 x 1 1700000000123456789",
    "batched 2 2 RowLimit 0",
    "schemaIpc True",
    "unconnected refused",
};

foreach (string line in seen)
{
    Console.WriteLine(line);
}

if (!seen.SequenceEqual(expected))
{
    Console.Error.WriteLine("AotSmoke: expected" + Environment.NewLine + string.Join(Environment.NewLine, expected));
    return 1;
}

Console.WriteLine("AotSmoke: OK");
return 0;

static Reading MakeReading() => new()
{
    Id = 7,
    Name = "aot",
    Value = 0.5,
    Mood = Mood.Happy,
    Samples = { 1, -2 },
    Tags = { new("a", 1), new("a", 2) },
    Stamp = new Stamp { By = "x" },
    History = { new Stamp { By = "h" } },
    At = new Timestamp(1_700_000_000_123_456_789L, TimeUnit.Nanosecond),
};

static string Describe(Reading row) =>
    $"{row.Id} {row.Name} {row.Value} {row.Mood} {string.Join(',', row.Samples)} "
    + $"{row.Tags.Count} {row.Stamp?.By} {row.History.Count} {row.At.WithUnit(TimeUnit.Nanosecond).Value}";

// The runtime's Publisher and Subscriber with the generated row class, as the generated pair
// uses them: the generated classes fix their topic, and this run needs a GUID one.
int RunFastDdsPublisher()
{
    string topicSegment = "aotsmoke-" + Guid.NewGuid().ToString("N");
    using Process child = Process.Start(new ProcessStartInfo(Environment.ProcessPath!)
    {
        ArgumentList = { "fastdds-subscribe", topicSegment },
        UseShellExecute = false,
    })!;

    using PubSubProviderHandle provider = ProviderRegistry.Create(ProviderSelector.Parse("fastdds"), new ProviderConfig());
    using var codec = new FletcherCodec(Reading.Schema);
    using var publisher = new Publisher(provider);
    TopicPath topic = TopicPath.Of(topicSegment);
    publisher.CreateTopic(topic, Reading.Schema);
    using RecordBatch batch = Reading.ToArrow(new[] { MakeReading() });
    using BoundRows bound = codec.Bind(batch);

    var clock = Stopwatch.StartNew();
    int sent = 0;
    while (!child.WaitForExit(100))
    {
        if (clock.Elapsed > fastDdsDeadline)
        {
            child.Kill();
            Console.Error.WriteLine($"AotSmoke fastdds: no row reached the subscriber in {fastDdsDeadline.TotalSeconds} s ({sent} published on {topicSegment})");
            return 1;
        }

        publisher.Publish(topic, bound, 0);
        ++sent;
    }

    Console.WriteLine($"AotSmoke fastdds: {sent} published on {topicSegment}, subscriber exited {child.ExitCode}");
    return child.ExitCode == 0 ? 0 : 1;
}

int RunFastDdsSubscriber(string topicSegment)
{
    int mainThread = Environment.CurrentManagedThreadId;
    using PubSubProviderHandle provider = ProviderRegistry.Create(ProviderSelector.Parse("fastdds"), new ProviderConfig());
    using var codec = new FletcherCodec(Reading.Schema);
    using var subscriber = new Subscriber(provider);
    subscriber.HandlerFaulted += (_, e) => Console.Error.WriteLine($"AotSmoke fastdds-subscribe: the handler threw: {e.Exception}");

    string? first = null;
    int deliveryThread = 0;
    using var arrived = new ManualResetEventSlim();
    SubscribeResult result = subscriber.Subscribe(TopicPath.Of(topicSegment), (bytes, schema, attachments) =>
    {
        using RecordBatch batch = codec.Decode(bytes);
        var columns = new StructArray(new StructType(batch.Schema.FieldsList), batch.Length, batch.Arrays, ArrowBuffer.Empty, 0);
        string row = Describe(Reading.FromArrow(columns, 0));
        if (Interlocked.CompareExchange(ref first, row, null) is null)
        {
            deliveryThread = Environment.CurrentManagedThreadId;
            arrived.Set();
        }
    });
    result.Schema.Dispose();
    using Subscription subscription = result.Subscription;

    // Longer than the publisher's, so it is the publisher that reports a lost row.
    if (!arrived.Wait(fastDdsDeadline + TimeSpan.FromSeconds(5)))
    {
        Console.Error.WriteLine("AotSmoke fastdds-subscribe: no row arrived");
        return 1;
    }

    string expected = Describe(MakeReading());
    Console.WriteLine($"fastdds {first} (delivered on thread {deliveryThread}, main thread {mainThread})");
    if (first != expected || deliveryThread == mainThread)
    {
        Console.Error.WriteLine($"AotSmoke fastdds-subscribe: expected \"{expected}\" on a transport thread");
        return 1;
    }

    Console.WriteLine("AotSmoke fastdds: OK");
    return 0;
}
