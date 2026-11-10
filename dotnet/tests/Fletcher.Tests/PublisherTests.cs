// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// BIND-4b: the publish tier.
//
// ── WHAT THESE ROWS CAN AND CANNOT PROVE, said up front ─────────────────────
// There is no managed subscriber yet (4c), so nothing here can read back the
// bytes a publish put on the wire. Claiming otherwise would be the vacuity this
// round keeps catching, so the limit is stated instead: what is verified here is
// the CROSSING - that topics are declared under the name the seam joins, that
// the writer is handed a window at least as large as it asked for, that the
// callback discipline returns a caller's own exception rather than a rewritten
// one, and that every refusal lands on the right side of the boundary.
//
// The bytes themselves already have an oracle one level down:
// `BindingEntryPoints.TheFusedPublishRunsOverTheWholeChain` compares what
// arrives against what the codec encodes, in C++, through the same entry points
// these rows call. What is owed to 4c is the MANAGED end-to-end.
using System;
using System.Collections.Generic;
using System.Text;

using Apache.Arrow;

using Eiva.Fletcher;

using Xunit;

namespace Eiva.Fletcher.Tests;

public class PublisherTests : IDisposable
{
    private readonly PubSubProviderHandle _provider;
    private readonly Publisher _publisher;

    public PublisherTests()
    {
        _provider = ProviderRegistry.Create(ProviderSelector.Parse("inprocess"), new ProviderConfig());
        _publisher = new Publisher(_provider);
    }

    public void Dispose()
    {
        _publisher.Dispose();
        _provider.Dispose();
        GC.SuppressFinalize(this);
    }

    private static (FletcherCodec Codec, BoundRows Rows) Bind()
    {
        RecordBatch batch = CodecFixtures.Scalar();
        var codec = new FletcherCodec(batch.Schema);
        return (codec, codec.Bind(batch));
    }

    [Fact]
    public void ATopicIsDeclaredUnderTheNameTheSeamJoins()
    {
        RecordBatch batch = CodecFixtures.Scalar();
        _publisher.CreateTopic(TopicPath.Of("bind", "rows"), batch.Schema);

        IReadOnlyList<string> topics = _publisher.ListTopics();

        Assert.Single(topics);
        Assert.Equal("bind/rows", topics[0]);
    }

    [Fact]
    public void TheFusedPathPublishesARowOfABoundBatch()
    {
        (FletcherCodec codec, BoundRows rows) = Bind();
        using (codec)
        using (rows)
        {
            TopicPath topic = TopicPath.Of("bind", "fused");
            _publisher.CreateTopic(topic, rows.Schema);

            _publisher.Publish(topic, rows, 0);
            _publisher.Publish(topic, rows, 1);
        }
    }

    [Fact]
    public void ARowOutsideTheBoundBatchIsRefusedInManagedCode()
    {
        (FletcherCodec codec, BoundRows rows) = Bind();
        using (codec)
        using (rows)
        {
            TopicPath topic = TopicPath.Of("bind", "range");
            _publisher.CreateTopic(topic, rows.Schema);

            // Refused before the crossing, so the caller gets an argument
            // exception naming their parameter rather than a codec refusal.
            Assert.Throws<ArgumentOutOfRangeException>(() => _publisher.Publish(topic, rows, 99));
        }
    }

    [Fact]
    public void TheWholeBatchGoesInOneCrossing()
    {
        (FletcherCodec codec, BoundRows rows) = Bind();
        using (codec)
        using (rows)
        {
            TopicPath topic = TopicPath.Of("bind", "batch");
            _publisher.CreateTopic(topic, rows.Schema);

            _publisher.Publish(topic, rows);
        }
    }

    [Fact]
    public void AttachmentsPerRowMustHaveExactlyOneEntryPerRow()
    {
        (FletcherCodec codec, BoundRows rows) = Bind();
        using (codec)
        using (rows)
        {
            TopicPath topic = TopicPath.Of("bind", "perrow");
            _publisher.CreateTopic(topic, rows.Schema);

            using var one = new AttachmentsBuilder();
            one.Set("k", [1]);

            // The batch has two rows, so a one-entry table would silently pair the
            // wrong attachments with the wrong row - or read off the end.
            ArgumentException refused = Assert.Throws<ArgumentException>(
                () => _publisher.Publish(topic, rows, [one]));
            Assert.Contains("one entry per row", refused.Message, StringComparison.Ordinal);
        }
    }

    [Fact]
    public void APerRowTableOfTheRightLengthIsAccepted()
    {
        (FletcherCodec codec, BoundRows rows) = Bind();
        using (codec)
        using (rows)
        {
            TopicPath topic = TopicPath.Of("bind", "perrow2");
            _publisher.CreateTopic(topic, rows.Schema);

            using var first = new AttachmentsBuilder();
            using var second = new AttachmentsBuilder();
            first.Set("a", [1]);
            second.Set("b", [2]);

            _publisher.Publish(topic, rows, [first, second]);
        }
    }

    [Fact]
    public void TheWriterIsHandedAWindowAtLeastAsLargeAsItAskedFor()
    {
        // The one property of the writer path observable without a subscriber, and
        // it is the load-bearing one: `room` is the WHOLE remaining window and is
        // always at least `min_bytes`, so a variable-length producer never needs a
        // second crossing to ask how much is left.
        TopicPath topic = TopicPath.Of("bind", "writer");
        RecordBatch batch = CodecFixtures.Scalar();
        _publisher.CreateTopic(topic, batch.Schema);

        int observed = -1;
        _publisher.Publish(topic, destination =>
        {
            observed = destination.Length;
            destination[0] = 0x2A;
            return 1;
        }, minBytes: 64);

        Assert.True(observed >= 64, $"the writer was handed {observed} bytes for a 64-byte request");
    }

    [Fact]
    public void AWritersOwnExceptionComesBackAsItselfRatherThanAsAFletcherException()
    {
        // D-BIND-19 RULE 3, and the reason the thunk records rather than throws: a
        // managed caller's contract is not rewritten by having crossed a boundary.
        // The writer's InvalidDataException arrives as an InvalidDataException,
        // with its own message, not as a FletcherException carrying a status.
        TopicPath topic = TopicPath.Of("bind", "throwing");
        RecordBatch batch = CodecFixtures.Scalar();
        _publisher.CreateTopic(topic, batch.Schema);

        var thrown = Assert.Throws<System.IO.InvalidDataException>(
            () => _publisher.Publish(topic, _ => throw new System.IO.InvalidDataException("row 7 is malformed"), 16));

        Assert.Equal("row 7 is malformed", thrown.Message);
    }

    [Fact]
    public void AWriterReportingZeroIsGivenARealMessageRatherThanASilentFailure()
    {
        // 0 is the ABI's signal that a thunk captured an exception, so a writer
        // returning it honestly would be indistinguishable from one that threw.
        // Converted into an exception that says so.
        TopicPath topic = TopicPath.Of("bind", "zero");
        RecordBatch batch = CodecFixtures.Scalar();
        _publisher.CreateTopic(topic, batch.Schema);

        var thrown = Assert.Throws<InvalidOperationException>(
            () => _publisher.Publish(topic, _ => 0, 16));

        Assert.Contains("at least one byte", thrown.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void AWriterClaimingMoreThanTheWindowHoldsIsRefused()
    {
        TopicPath topic = TopicPath.Of("bind", "overrun");
        RecordBatch batch = CodecFixtures.Scalar();
        _publisher.CreateTopic(topic, batch.Schema);

        var thrown = Assert.Throws<InvalidOperationException>(
            () => _publisher.Publish(topic, destination => destination.Length + 1, 16));

        Assert.Contains("bytes written into a", thrown.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void AZeroMinimumIsRefusedBeforeTheCrossing()
    {
        TopicPath topic = TopicPath.Of("bind", "nomin");
        Assert.Throws<ArgumentOutOfRangeException>(() => _publisher.Publish(topic, _ => 1, 0));
    }

    [Fact]
    public void PublishRawSendsBytesAlreadyInHand()
    {
        TopicPath topic = TopicPath.Of("bind", "raw");
        RecordBatch batch = CodecFixtures.Scalar();
        _publisher.CreateTopic(topic, batch.Schema);

        _publisher.PublishRaw(topic, [1, 2, 3, 4]);
    }

    [Fact]
    public void AnEmptyRawRowIsRefusedNamingTheArgument()
    {
        // The seam refuses a min_bytes of 0 anyway - "a fill of no bytes names
        // nothing" - so this is about WHICH message the caller gets.
        TopicPath topic = TopicPath.Of("bind", "rawempty");
        ArgumentException refused = Assert.Throws<ArgumentException>(
            () => _publisher.PublishRaw(topic, ReadOnlySpan<byte>.Empty));

        Assert.Equal("row", refused.ParamName);
    }

    [Fact]
    public void PublishingWithAttachmentsDoesNotEmptyTheCallersBuilder()
    {
        // D-BIND-44, as a behaviour rather than as a comment. The native builder
        // empties itself on build, so a publish that sealed the caller's builder
        // would leave it with nothing - and the SECOND row would carry no
        // attachments at all, silently.
        TopicPath topic = TopicPath.Of("bind", "keeps");
        RecordBatch batch = CodecFixtures.Scalar();
        _publisher.CreateTopic(topic, batch.Schema);

        using var attachments = new AttachmentsBuilder();
        attachments.Set("trace", [9]);

        _publisher.PublishRaw(topic, [1], attachments);
        Assert.Equal(1, attachments.Count);

        _publisher.PublishRaw(topic, [2], attachments);
        Assert.Equal(1, attachments.Count);
    }

    [Fact]
    public void AMalformedTopicIsRefusedInManagedCodeBeforeAnythingCrosses()
    {
        Assert.Throws<ArgumentException>(() => TopicPath.Of("bind", "__reserved"));
    }

    [Fact]
    public void DisposingTwiceIsSafeAndUsingADisposedPublisherIsRefused()
    {
        PubSubProviderHandle provider =
            ProviderRegistry.Create(ProviderSelector.Parse("inprocess"), new ProviderConfig());
        var publisher = new Publisher(provider);

        publisher.Dispose();
        publisher.Dispose();

        Assert.Throws<ObjectDisposedException>(() => publisher.ListTopics());
        provider.Dispose();
    }

    [Fact]
    public void ThePublisherKeepsItsProviderAliveWhateverOrderTheyAreReleasedIn()
    {
        // The seam requires a provider to outlive everything built on it, and .NET
        // does not order finalization. PublisherHandle.Create takes a SafeHandle
        // reference for exactly this, so releasing the provider first is survivable
        // rather than a use-after-free.
        PubSubProviderHandle provider =
            ProviderRegistry.Create(ProviderSelector.Parse("inprocess"), new ProviderConfig());
        var publisher = new Publisher(provider);

        provider.Dispose();

        RecordBatch batch = CodecFixtures.Scalar();
        publisher.CreateTopic(TopicPath.Of("bind", "outlives"), batch.Schema);
        Assert.Single(publisher.ListTopics());

        publisher.Dispose();
    }
}
