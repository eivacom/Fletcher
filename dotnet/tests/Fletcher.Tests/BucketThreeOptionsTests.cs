// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// D-BIND-57 - BUCKET 3's #128 cases, the half that runs over `inprocess`.
//
// #128 added 26 cases to `pubsub/tests/test_publisher_subscriber.cpp`: per-topic
// options and the schema-only watch. It reached this branch AFTER bucket 3 was
// ported, and nothing re-derived the matrix, so they went unported through two
// BIND-4 closes. The whole mapping - these, the Fast DDS half in the transport
// suite, and the three exclusions - is in BucketThreeConformanceTests.cs.
//
// ── Why `inprocess` answers these at all ────────────────────────────────────
// `inprocess` overrides none of the seam's option or watch methods, so it runs
// the seam's DEFAULT bodies - the ones these C++ cases pin through a data-only
// mock. What C# observes is the status that comes back, which is the part a
// binding could get wrong (the options' marshalling, the containment). What it
// cannot observe is how many times the provider was called: the C++ cases'
// `*_count` assertions are the provider tier's bookkeeping, and these mirrors are
// declared weaker for that reason where they drop one.
using System;

using Apache.Arrow;

using Eiva.Fletcher;

using Xunit;

namespace Eiva.Fletcher.Tests;

public sealed class BucketThreeOptionsTests : IDisposable
{
    private readonly PubSubProviderHandle _provider;
    private readonly Publisher _publisher;
    private readonly Subscriber _subscriber;
    private readonly RecordBatch _batch = CodecFixtures.Scalar();
    private readonly FletcherCodec _codec;
    private readonly BoundRows _rows;

    public BucketThreeOptionsTests()
    {
        _provider = ProviderRegistry.Create(ProviderSelector.Parse("inprocess"), new ProviderConfig());
        _publisher = new Publisher(_provider);
        _subscriber = new Subscriber(_provider);
        _codec = new FletcherCodec(_batch.Schema);
        _rows = _codec.Bind(_batch);
    }

    public void Dispose()
    {
        _rows.Dispose();
        _codec.Dispose();
        _subscriber.Dispose();
        _publisher.Dispose();
        _provider.Dispose();
        GC.SuppressFinalize(this);
    }

    private static TopicPath Topic(string name) => TopicPath.Of("bucket3", "options", name);

    private static readonly TopicOptions Profiled = new() { Profile = "x" };

    /// <summary>Mirrors SubscriberTest.DefaultCreateTopicWithOptionsDelegatesWhenEmpty.</summary>
    /// <remarks>Weaker: the C++ case counts the provider's plain CreateTopic; here the topic is listed.</remarks>
    [Fact]
    public void EmptyOptionsDeclareATopicOnAProviderThatKnowsNone()
    {
        _publisher.CreateTopic(Topic("empty"), _batch.Schema, new TopicOptions());
        Assert.Contains("bucket3/options/empty", _publisher.ListTopics());
    }

    /// <summary>Mirrors SubscriberTest.DefaultCreateTopicWithOptionsRefusesNonEmptyWithNotSupported.</summary>
    [Fact]
    public void NonEmptyOptionsAreNotSupportedByAProviderThatKnowsNone()
    {
        FletcherException refused = Assert.Throws<FletcherException>(
            () => _publisher.CreateTopic(Topic("profiled"), _batch.Schema, Profiled));

        Assert.Equal(FletcherStatus.NotSupported, refused.Status);
        Assert.DoesNotContain("bucket3/options/profiled", _publisher.ListTopics());
    }

    /// <summary>Mirrors SubscriberTest.DefaultSubscribeWithOptionsDelegatesWhenEmpty.</summary>
    /// <remarks>Weaker: the C++ case counts the provider's plain Subscribe; here the row arrives.</remarks>
    [Fact]
    public void EmptyOptionsSubscribeOnAProviderThatKnowsNone()
    {
        TopicPath topic = Topic("subscribe");
        _publisher.CreateTopic(topic, _batch.Schema);

        int delivered = 0;
        SubscribeResult result = _subscriber.Subscribe(topic, (_, _, _) => delivered++, new TopicOptions());
        result.Schema.Dispose();

        _publisher.Publish(topic, _rows, 0);
        Assert.Equal(1, delivered);
        result.Subscription.Dispose();
    }

    /// <summary>Mirrors SubscriberTest.DefaultSubscribeWithOptionsRefusesNonEmptyWithNotSupported.</summary>
    [Fact]
    public void ANonEmptyProfileOnASubscriptionIsNotSupportedByAProviderThatKnowsNone()
    {
        FletcherException refused = Assert.Throws<FletcherException>(
            () => _subscriber.Subscribe(Topic("subprofiled"), (_, _, _) => { }, Profiled));

        Assert.Equal(FletcherStatus.NotSupported, refused.Status);
    }

    /// <summary>Mirrors SubscriberTest.DefaultSubscribeWithOptionsRefusesABoundAsInvalidArgument.</summary>
    /// <remarks>
    /// INVALID_ARGUMENT and not NOT_SUPPORTED, from a provider that supports
    /// nothing: a subscriber follows its publisher's bound, so asking for one is a
    /// caller error, and the seam says so before it asks whether options are
    /// supported at all.
    /// </remarks>
    [Fact]
    public void ASubscriptionCarriesNoPayloadBound()
    {
        FletcherException refused = Assert.Throws<FletcherException>(
            () => _subscriber.Subscribe(Topic("bounded"), (_, _, _) => { }, new TopicOptions { MaxPayloadBytes = 8192 }));

        Assert.Equal(FletcherStatus.InvalidArgument, refused.Status);
    }

    /// <summary>Mirrors SubscriberTest.DefaultOptionsMethodsAreRefusedFromInsideADelivery.</summary>
    /// <remarks>
    /// The C++ case calls the provider's two methods from inside a delivery and
    /// expects kReentrantCall from both. The managed surface reaches them through
    /// the caller tier: a CreateTopic comes back as the seam's
    /// <see cref="FletcherStatus.ReentrantCall"/>, and a Subscribe to a topic this
    /// subscriber has never subscribed is refused in managed code first, as every
    /// such Subscribe is (D-BIND-51).
    /// </remarks>
    [Fact]
    public void TheOptionFormsAreRefusedFromInsideADelivery()
    {
        TopicPath topic = Topic("reentrant");
        _publisher.CreateTopic(topic, _batch.Schema);

        FletcherStatus? create = null;
        Exception? subscribe = null;
        SubscribeResult result = _subscriber.Subscribe(topic, (_, _, _) =>
        {
            try
            {
                _publisher.CreateTopic(Topic("fromhandler"), _batch.Schema, new TopicOptions());
            }
            catch (FletcherException e)
            {
                create = e.Status;
            }

            try
            {
                _subscriber.Subscribe(Topic("fromhandler2"), (_, _, _) => { }, new TopicOptions());
            }
            catch (InvalidOperationException e)
            {
                subscribe = e;
            }
        });
        result.Schema.Dispose();

        _publisher.Publish(topic, _rows, 0);

        Assert.Equal(FletcherStatus.ReentrantCall, create);
        Assert.NotNull(subscribe);
        Assert.Contains("DispatchAfterDelivery", subscribe!.Message, StringComparison.Ordinal);
        result.Subscription.Dispose();
    }
}

public sealed class BucketThreeSchemaWatchTests : IDisposable
{
    private readonly PubSubProviderHandle _provider;
    private readonly Publisher _publisher;
    private readonly Subscriber _subscriber;
    private readonly RecordBatch _batch = CodecFixtures.Scalar();

    public BucketThreeSchemaWatchTests()
    {
        _provider = ProviderRegistry.Create(ProviderSelector.Parse("inprocess"), new ProviderConfig());
        _publisher = new Publisher(_provider);
        _subscriber = new Subscriber(_provider);
    }

    public void Dispose()
    {
        _subscriber.Dispose();
        _publisher.Dispose();
        _provider.Dispose();
        GC.SuppressFinalize(this);
    }

    /// <summary>Mirrors SubscriberTest.DefaultProviderRefusesSchemaOnlyWithNotSupported.</summary>
    /// <remarks>
    /// `inprocess` has no out-of-band schema channel. The message is the
    /// transport's own, and releasing what was never watched is served.
    /// </remarks>
    [Fact]
    public void ASchemaWatchIsNotSupportedWithoutASchemaChannel()
    {
        TopicPath topic = TopicPath.Of("bucket3", "watch", "unsupported");

        FletcherException refused = Assert.Throws<FletcherException>(() => _subscriber.SubscribeSchema(topic));
        Assert.Equal(FletcherStatus.NotSupported, refused.Status);

        _subscriber.UnsubscribeSchema(topic);
    }

    /// <summary>Mirrors SubscriberTest.UnsubscribeSchemaWithoutAWatchDoesNotForward.</summary>
    /// <remarks>Weaker: the C++ case counts that nothing reached the provider; here it is served, twice.</remarks>
    [Fact]
    public void ReleasingAWatchThatWasNeverTakenIsANoOp()
    {
        TopicPath topic = TopicPath.Of("bucket3", "watch", "never");
        _subscriber.UnsubscribeSchema(topic);
        _subscriber.UnsubscribeSchema(topic);
    }

    /// <summary>
    /// Mirrors SubscriberTest.DefaultSchemaMethodsAreRefusedFromInsideADelivery and
    /// SubscriberTest.SubscribeSchemaFromInsideADeliveryThroughTheMockProviderIsRefused.
    /// </summary>
    /// <remarks>
    /// A watch from inside a delivery on the provider is refused ALWAYS - the
    /// seam's caller-tier rule, raised here in managed code before anything
    /// reaches native (D-BIND-51, D-BIND-57). The release half of the first C++
    /// case calls the PROVIDER's method, which C# cannot reach; the caller tier's
    /// release of an unwatched topic never enters the provider, so it is served,
    /// and that is what is asserted.
    /// </remarks>
    [Fact]
    public void AWatchFromInsideADeliveryIsRefusedAndAnUnwatchedReleaseIsServed()
    {
        TopicPath topic = TopicPath.Of("bucket3", "watch", "reentrant");
        _publisher.CreateTopic(topic, _batch.Schema);
        using var codec = new FletcherCodec(_batch.Schema);
        using BoundRows rows = codec.Bind(_batch);

        Exception? watch = null;
        Exception? release = null;
        SubscribeResult result = _subscriber.Subscribe(topic, (_, _, _) =>
        {
            watch = Record(() => _subscriber.SubscribeSchema(TopicPath.Of("bucket3", "watch", "fromhandler")).Dispose());
            release = Record(() => _subscriber.UnsubscribeSchema(TopicPath.Of("bucket3", "watch", "fromhandler")));
        });
        result.Schema.Dispose();

        _publisher.Publish(topic, rows, 0);

        Assert.IsType<InvalidOperationException>(watch);
        Assert.Contains("DispatchAfterDelivery", watch!.Message, StringComparison.Ordinal);
        Assert.Null(release);
        result.Subscription.Dispose();
    }

    private static Exception? Record(Action action)
    {
        try
        {
            action();
            return null;
        }
        catch (Exception e) when (e is InvalidOperationException or FletcherException)
        {
            return e;
        }
    }
}
