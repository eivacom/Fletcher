// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// BIND-6d: the GENERATED publisher/subscriber pair, against the real native runtime.
//
// The classes under test are not written here: they are the plugin's output for
// protoc/tests/golden/csharp_pair.proto, committed as goldens and compiled into this
// assembly (the plugin's own suite fails if they drift from its output by a byte).
// So these cases test what a consumer's protoc call produces, over the shim, and run
// on both target frameworks - which is also the proof that the generated handler
// delegate compiles for net8.0, where Action<Reading, AttachmentsView> does not
// (D-BIND-79).
//
// `inprocess` delivers SYNCHRONOUSLY on the publishing thread, so every assertion runs
// straight after a publish with no wait. A wait would hide a delivery that never
// happened.
using System;
using System.Buffers;
using System.Collections.Generic;
using System.Linq;
using System.Text;

using Apache.Arrow;
using Apache.Arrow.Types;

using Eiva.Fletcher.Model;

using Fletcher.Gen.Golden.Pair;

using Xunit;

using PbTimestamp = Fletcher.Gen.Google.Protobuf.Timestamp;

namespace Eiva.Fletcher.Tests;

public sealed class GeneratedPairTests : IDisposable
{
    private readonly PubSubProviderHandle _provider;

    public GeneratedPairTests()
    {
        _provider = ProviderRegistry.Create(ProviderSelector.Parse("inprocess"), new ProviderConfig());
    }

    public void Dispose() => _provider.Dispose();

    // Every kind the pair carries, at values a default would not reproduce.
    private static Reading Full(int id) => new()
    {
        Id = id,
        Name = "héllo ✓ " + id,
        Value = -0.5,
        Mood = Mood.Happy,
        Samples = { 3, -1, int.MaxValue },
        Tags = { new("b", 2), new("a", long.MinValue), new("b", 3) },
        Stamp = new Stamp { By = "x" },
        History = { new Stamp { By = "h1" }, new Stamp { By = "h2" } },
        At = new Timestamp(1_700_000_000_123_456_789L, TimeUnit.Nanosecond),
    };

    private static string Describe(Reading r) => string.Join(" | ",
        r.Id, r.Name, r.Value?.ToString(System.Globalization.CultureInfo.InvariantCulture) ?? "<null>", r.Mood,
        "[" + string.Join(",", r.Samples) + "]",
        "{" + string.Join(",", r.Tags.Select(t => t.Key + "=" + t.Value)) + "}",
        r.Stamp?.By ?? "<null>",
        "[" + string.Join(",", r.History.Select(h => h.By)) + "]",
        // The instant, in the column's unit: a value written in another unit comes back
        // recounted (6c-2), so the unit itself is not part of what must survive.
        r.At.WithUnit(TimeUnit.Nanosecond).Value + " ns");

    [Fact]
    public void TheTopicIsCppsForm()
    {
        // D-BIND-72 point 6 and D-BIND-77: the package is one segment, dots kept, so a
        // C# and a C++ publisher of one method name the same topic.
        Assert.Equal(new[] { "golden.pair", "Telemetry", "Report" }, Telemetry_ReportTopic.Segments);
        Assert.Equal("golden.pair/Telemetry/Report", Telemetry_ReportTopic.Key);
        Assert.Equal(TopicPath.Of("golden.pair", "Telemetry", "Report"), Telemetry_ReportPublisher.Topic);
        Assert.Equal(Telemetry_ReportPublisher.Topic, Telemetry_ReportSubscriber.Topic);
        Assert.Equal(Telemetry_ReportTopic.Key, Telemetry_ReportPublisher.TopicKey);
        Assert.Same(Reading.Schema, Telemetry_ReportPublisher.Schema);
    }

    [Fact]
    public void AGeneratedPublishArrivesWholeAtTheGeneratedSubscriber()
    {
        using var subscriber = new Telemetry_ReportSubscriber(_provider);
        var seen = new List<string>();
        using Subscription subscription = subscriber.Subscribe((row, attachments) => seen.Add(Describe(row)));
        using var publisher = new Telemetry_ReportPublisher(_provider);

        Reading sent = Full(1);
        publisher.Publish(sent);

        Assert.Equal(new[] { Describe(sent) }, seen);
    }

    [Fact]
    public void AnEmptyRowArrivesAsAnEmptyRowNotAsAFullOne()
    {
        // The message field absent, the optional null, every collection empty: the
        // defaults a decoder could invent are all distinguishable from what was sent.
        using var subscriber = new Telemetry_ReportSubscriber(_provider);
        Reading? got = null;
        using Subscription subscription = subscriber.Subscribe((row, attachments) => got = row);
        using var publisher = new Telemetry_ReportPublisher(_provider);

        publisher.Publish(new Reading());

        Assert.NotNull(got);
        Assert.Null(got!.Stamp);
        Assert.Null(got.Value);
        Assert.Empty(got.Samples);
        Assert.Empty(got.Tags);
        Assert.Empty(got.History);
        Assert.Equal("", got.Name);
    }

    [Fact]
    public void TheBatchFormDeliversEveryRowInOrder()
    {
        using var subscriber = new Telemetry_ReportSubscriber(_provider);
        var seen = new List<string>();
        using Subscription subscription = subscriber.Subscribe((row, attachments) => seen.Add(Describe(row)));
        using var publisher = new Telemetry_ReportPublisher(_provider);

        Reading[] sent = { Full(1), new Reading { Id = 2 }, Full(3) };
        publisher.Publish(sent);

        Assert.Equal(sent.Select(Describe), seen);
    }

    [Fact]
    public void AnEmptyBatchPublishesNothing()
    {
        using var subscriber = new Telemetry_ReportSubscriber(_provider);
        int deliveries = 0;
        using Subscription subscription = subscriber.Subscribe((row, attachments) => deliveries++);
        using var publisher = new Telemetry_ReportPublisher(_provider);

        publisher.Publish(System.Array.Empty<Reading>());

        Assert.Equal(0, deliveries);
    }

    [Fact]
    public void AttachmentsTravelWithTheirRow()
    {
        using var subscriber = new Telemetry_ReportSubscriber(_provider);
        string? value = null;
        using Subscription subscription = subscriber.Subscribe((row, attachments) =>
        {
            // Borrowed for the call: copied out before returning.
            if (attachments.TryFind(Encoding.UTF8.GetBytes("frame"), out ReadOnlySpan<byte> bytes))
            {
                value = Encoding.UTF8.GetString(bytes);
            }
        });
        using var publisher = new Telemetry_ReportPublisher(_provider);
        using var attachments = new AttachmentsBuilder();
        attachments.Set("frame", Encoding.UTF8.GetBytes("jpeg-bytes"));

        publisher.Publish(Full(1), attachments);

        Assert.Equal("jpeg-bytes", value);
    }

    [Fact]
    public void ThePairPublishesTheCodecsOwnEncodingOfToArrow()
    {
        // The pair adds no encoding of its own: a raw subscriber on the same topic sees the
        // bytes the codec writes for Reading.ToArrow of the same row (D-BIND-1).
        using var raw = new Subscriber(_provider);
        byte[]? bytes = null;
        SubscribeResult result = raw.Subscribe(Telemetry_ReportPublisher.Topic, (row, schema, attachments) => bytes = row.ToArray());
        result.Schema.Dispose();
        using var publisher = new Telemetry_ReportPublisher(_provider);

        Reading sent = Full(7);
        publisher.Publish(sent);

        using RecordBatch batch = Reading.ToArrow(new[] { sent });
        using var codec = new FletcherCodec(Reading.Schema);
        using BoundRows rows = codec.Bind(batch);
        var expected = new ArrayBufferWriter<byte>();
        codec.Encode(rows, 0, expected);
        Assert.Equal(expected.WrittenSpan.ToArray(), bytes);
        result.Subscription.Dispose();
    }

    [Fact]
    public void AHandlerThatThrowsIsAbsorbedAndReported()
    {
        using var subscriber = new Telemetry_ReportSubscriber(_provider);
        Exception? reported = null;
        subscriber.HandlerFaulted += (_, e) => reported = e.Exception;
        using Subscription subscription = subscriber.Subscribe((row, attachments) => throw new InvalidOperationException("boom"));
        using var publisher = new Telemetry_ReportPublisher(_provider);

        publisher.Publish(Full(1));

        Assert.Equal(1UL, subscriber.AbsorbedCallbackFailures);
        Assert.IsType<InvalidOperationException>(reported);
    }

    [Fact]
    public void NothingArrivesAfterUnsubscribe()
    {
        using var subscriber = new Telemetry_ReportSubscriber(_provider);
        int deliveries = 0;
        Subscription subscription = subscriber.Subscribe((row, attachments) => deliveries++);
        using var publisher = new Telemetry_ReportPublisher(_provider);

        publisher.Publish(Full(1));
        subscriber.Unsubscribe(subscription);
        publisher.Publish(Full(2));

        Assert.Equal(1, deliveries);
    }

    [Fact]
    public void NullArgumentsAreRefused()
    {
        Assert.Throws<ArgumentNullException>(() => new Telemetry_ReportPublisher(null!));
        Assert.Throws<ArgumentNullException>(() => new Telemetry_ReportSubscriber(null!));
        using var publisher = new Telemetry_ReportPublisher(_provider);
        Assert.Throws<ArgumentNullException>(() => publisher.Publish((Reading)null!));
        Assert.Throws<ArgumentNullException>(() => publisher.Publish((IEnumerable<Reading>)null!));
        using var subscriber = new Telemetry_ReportSubscriber(_provider);
        Assert.Throws<ArgumentNullException>(() => subscriber.Subscribe(null!));
    }

    [Fact]
    public void APairWhoseRowsCarryAnotherFilesMessageRoundTrips()
    {
        // BIND-6e: Marks holds `repeated google.protobuf.Timestamp`, which the IR maps as a
        // list of timestamp.proto's struct, so each element is the class generated from that
        // file (a golden too) and its column is built by that class's public ToArrow
        // (D-BIND-80). The native codec encodes and decodes it like any list of structs.
        using var subscriber = new Telemetry_MarkSubscriber(_provider);
        var seen = new List<string>();
        using Subscription subscription = subscriber.Subscribe((row, attachments) =>
            seen.Add(string.Join(",", row.At.Select(t => t.Seconds + ":" + t.Nanos))));
        using var publisher = new Telemetry_MarkPublisher(_provider);

        Marks[] sent =
        {
            new() { At = { new PbTimestamp { Seconds = 1_700_000_000, Nanos = 123_456_789 },
                           new PbTimestamp { Seconds = long.MinValue, Nanos = -1 } } },
            new(),
            new() { At = { new PbTimestamp { Seconds = -1, Nanos = 999_999_999 } } },
        };
        publisher.Publish(sent);

        Assert.Equal(new[] { "1700000000:123456789," + long.MinValue + ":-1", "", "-1:999999999" }, seen);
        Assert.Equal("golden.pair/Telemetry/Mark", Telemetry_MarkTopic.Key);
    }
}
