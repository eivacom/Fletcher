// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// D-BIND-57 - BUCKET 3's #128 cases, the half that needs a provider which KNOWS
// per-topic options and has a schema channel: Fast DDS. The mapping for all of
// bucket 3 is in dotnet/tests/Fletcher.Tests/BucketThreeConformanceTests.cs.
//
// ── Why here and not in the unit lane ───────────────────────────────────────
// The C++ cases use a mock that accepts any option; the only providers C# can
// select that accept one are real transports, and real transports live in this
// lane (see CrossTransportTests' header for why). `inprocess` answers the
// DEFAULT bodies, and those mirrors are in the unit lane.
//
// ── Why only the profiles the provider's DEFAULT document defines ───────────
// Fast DDS profile names are process-wide, so every Fast DDS provider in one
// process shares one byte-identical document or an empty one (which becomes the
// provider's default) - a different document is refused at construction (seam
// §4.1, as landed by PDA-DEC-6). The other rows in this process run on the
// default, so these do too, and use the named pairs it defines
// (`fastdds-pubsub-provider/src/qos_defaults.cpp`): `latest` and `store_latest`,
// each a <data_writer> and a <data_reader> profile of the same name.
//
// ── How each refusal is made UNAMBIGUOUS ───────────────────────────────────
// A C++ case asserts kInvalidArgument AND that the provider was called once, so
// it knows the refusal was the caller tier's field-wise check. C# cannot count
// provider calls. So the refused value here is one the provider would ACCEPT on
// its own - a valid bound, a defined profile - leaving the field-wise check as
// the only thing that can refuse it.
using System;
using System.Threading;

using Apache.Arrow;
using Apache.Arrow.Types;

using Xunit;

namespace Eiva.Fletcher.TransportConformance;

[Collection(XrceAgentCollection.Name)]
public sealed class TopicOptionsOverFastDdsTests : IDisposable
{
    // RELIABLE, VOLATILE, KEEP_LAST 1 at both ends - compatible with the default
    // RELIABLE writer and reader, so a row still flows.
    private const string Writer = "latest";
    private const string Reader = "latest";

    // A second profile the document defines, so a "different" value is one the
    // provider would accept on its own.
    private const string OtherReader = "store_latest";

    private readonly PubSubProviderHandle _provider;
    private readonly Publisher _publisher;
    private readonly Subscriber _subscriber;

    public TopicOptionsOverFastDdsTests()
    {
        _provider = ProviderRegistry.Create(ProviderSelector.Parse("fastdds"), new ProviderConfig());
        _publisher = new Publisher(_provider);
        _subscriber = new Subscriber(_provider);
    }

    public void Dispose()
    {
        _subscriber.Dispose();
        _publisher.Dispose();
        _provider.Dispose();
    }

    private static readonly Schema OneColumn =
        new([new Field("id", Int32Type.Default, nullable: true)], metadata: null);

    private static TopicPath Unique(string name) => TopicPath.Of("bind4", "options", name, Guid.NewGuid().ToString("N"));

    private static void Refused(FletcherStatus expected, Action action) =>
        Assert.Equal(expected, Assert.Throws<FletcherException>(action).Status);

    // ── Publisher ───────────────────────────────────────────────────────────

    /// <summary>Mirrors PublisherTest.PublisherForwardsTopicOptions.</summary>
    /// <remarks>
    /// The C++ case reads the options back out of a mock. The managed observation
    /// is their EFFECT: a per-topic bound below the provider's refuses a row the
    /// provider alone would carry, and a profile the document does not define is
    /// refused by the provider - which it can only be if it reached it.
    /// </remarks>
    [Fact]
    public void BothFieldsReachTheProvider()
    {
        var schema = new Schema([new Field("payload", StringType.Default, nullable: false)], metadata: null);
        TopicPath bounded = Unique("bound");
        _publisher.CreateTopic(bounded, schema, new TopicOptions { MaxPayloadBytes = 128 });

        using (RecordBatch large = new(schema, [new StringArray.Builder().Append(new string('x', 512)).Build()], length: 1))
        using (var codec = new FletcherCodec(schema))
        using (BoundRows rows = codec.Bind(large))
        {
            Refused(FletcherStatus.PayloadTooLarge, () => _publisher.Publish(bounded, rows, 0));
        }

        _publisher.CreateTopic(Unique("known"), OneColumn, new TopicOptions { Profile = Writer });
        Refused(FletcherStatus.InvalidArgument,
            () => _publisher.CreateTopic(Unique("unknown"), OneColumn, new TopicOptions { Profile = "no_such_profile" }));
    }

    /// <summary>Mirrors PublisherTest.PublisherRefusesARedeclarationWithDifferentOptions.</summary>
    [Fact]
    public void ARedeclarationThatChangesAFieldIsRefused()
    {
        TopicPath topic = Unique("different");
        _publisher.CreateTopic(topic, OneColumn, new TopicOptions { MaxPayloadBytes = 4096 });

        // 8192 is a bound the provider would take; only the field-wise check refuses it.
        Refused(FletcherStatus.InvalidArgument,
            () => _publisher.CreateTopic(topic, OneColumn, new TopicOptions { MaxPayloadBytes = 8192 }));
    }

    /// <summary>Mirrors PublisherTest.PublisherAcceptsARedeclarationWithEmptyOptions.</summary>
    [Fact]
    public void ARedeclarationWithEmptyOptionsIsTheSameDeclaration()
    {
        TopicPath topic = Unique("empty");
        _publisher.CreateTopic(topic, OneColumn, new TopicOptions { MaxPayloadBytes = 4096 });

        _publisher.CreateTopic(topic, OneColumn, new TopicOptions());
        _publisher.CreateTopic(topic, OneColumn);
    }

    /// <summary>Mirrors PublisherTest.PublisherAcceptsARedeclarationNamingASubsetOfTheOptions.</summary>
    [Fact]
    public void ARedeclarationNamingASubsetOfTheFieldsIsTheSameDeclaration()
    {
        TopicPath topic = Unique("subset");
        _publisher.CreateTopic(topic, OneColumn, new TopicOptions { Profile = Writer, MaxPayloadBytes = 4096 });

        _publisher.CreateTopic(topic, OneColumn, new TopicOptions { Profile = Writer });
    }

    /// <summary>Mirrors PublisherTest.PublisherRefusesANewFieldOnARedeclaration.</summary>
    [Fact]
    public void ARedeclarationThatAddsAFieldIsRefused()
    {
        TopicPath topic = Unique("newfield");
        _publisher.CreateTopic(topic, OneColumn, new TopicOptions { MaxPayloadBytes = 4096 });

        // The profile is one the document defines, so only the field-wise rule -
        // a non-empty field against an empty stored one - can refuse it.
        Refused(FletcherStatus.InvalidArgument,
            () => _publisher.CreateTopic(topic, OneColumn, new TopicOptions { Profile = Writer, MaxPayloadBytes = 4096 }));
    }

    // ── Subscriber ──────────────────────────────────────────────────────────

    /// <summary>Mirrors SubscriberTest.SubscriberForwardsTopicOptionsOnTheFirstSubscription.</summary>
    /// <remarks>
    /// Observed by effect, as the publisher mirror is: a defined reader profile
    /// delivers, an undefined one is refused. The writer takes the SAME profile
    /// name: the named profiles are pairs (`qos_defaults.cpp`), and a named reader
    /// against the default writer is not what they are for - `store_latest`'s
    /// TRANSIENT_LOCAL reader never matches a VOLATILE writer, and even `latest`,
    /// compatible on paper, was measured delivering nothing when subscribed first.
    /// </remarks>
    [Fact]
    public void AReaderProfileReachesTheProvider()
    {
        TopicPath topic = Unique("reader");
        using var seen = new ManualResetEventSlim(false);
        SubscribeResult result = _subscriber.Subscribe(topic, (_, _, _) => seen.Set(), new TopicOptions { Profile = Reader });
        _publisher.CreateTopic(topic, OneColumn, new TopicOptions { Profile = Writer });

        using (result.Schema)
        using (RecordBatch batch = new(OneColumn, [new Int32Array.Builder().Append(1).Build()], length: 1))
        using (var codec = new FletcherCodec(OneColumn))
        using (BoundRows rows = codec.Bind(batch))
        {
            Assert.True(CrossTransportTests.PublishUntilSeen(() => _publisher.Publish(topic, rows, 0), seen),
                "no row reached a subscription opened with the document's own reader profile");
        }

        result.Subscription.Dispose();
        Refused(FletcherStatus.InvalidArgument,
            () => _subscriber.Subscribe(Unique("noreader"), (_, _, _) => { }, new TopicOptions { Profile = "no_such_profile" }));
    }

    /// <summary>Mirrors SubscriberTest.SubscriberRefusesDifferentOptionsOnALiveTopic.</summary>
    /// <remarks>
    /// Both profiles are ones the document defines, so only the field-wise check
    /// can refuse the second.
    /// </remarks>
    [Fact]
    public void ASecondSubscriptionWithADifferentProfileIsRefused()
    {
        TopicPath topic = Unique("subdifferent");
        SubscribeResult first = _subscriber.Subscribe(topic, (_, _, _) => { }, new TopicOptions { Profile = Reader });
        first.Schema.Dispose();

        Refused(FletcherStatus.InvalidArgument,
            () => _subscriber.Subscribe(topic, (_, _, _) => { }, new TopicOptions { Profile = OtherReader }));
        first.Subscription.Dispose();
    }

    /// <summary>Mirrors SubscriberTest.SubscriberSecondSubscriptionWithEmptyOptionsSharesTheFirst.</summary>
    [Fact]
    public void ASecondSubscriptionWithEmptyOptionsJoinsTheFirst()
    {
        TopicPath topic = Unique("subshare");
        using var firstSeen = new ManualResetEventSlim(false);
        using var secondSeen = new ManualResetEventSlim(false);
        SubscribeResult first = _subscriber.Subscribe(topic, (_, _, _) => firstSeen.Set(), new TopicOptions { Profile = Reader });
        SubscribeResult second = _subscriber.Subscribe(topic, (_, _, _) => secondSeen.Set(), new TopicOptions());
        _publisher.CreateTopic(topic, OneColumn, new TopicOptions { Profile = Writer });

        using (first.Schema)
        using (second.Schema)
        using (RecordBatch batch = new(OneColumn, [new Int32Array.Builder().Append(2).Build()], length: 1))
        using (var codec = new FletcherCodec(OneColumn))
        using (BoundRows rows = codec.Bind(batch))
        {
            Assert.True(CrossTransportTests.PublishUntilSeen(() => _publisher.Publish(topic, rows, 0), secondSeen),
                "the joining subscription saw nothing");
            Assert.True(firstSeen.IsSet, "the first subscription saw nothing");
        }

        second.Subscription.Dispose();
        first.Subscription.Dispose();
    }

    /// <summary>Mirrors SubscriberTest.SubscriberRefusesANewFieldOnASecondSubscription.</summary>
    /// <remarks>
    /// The C++ case adds a BOUND, which a conforming provider refuses on any
    /// subscription, so it cannot be refused for the field-wise reason alone.
    /// The same rule is asserted unambiguously the other way round: a DEFINED
    /// profile added to a subscription whose topic already has an empty one.
    /// </remarks>
    [Fact]
    public void AFieldAddedToALiveSubscriptionIsRefused()
    {
        TopicPath topic = Unique("subnewfield");
        SubscribeResult first = _subscriber.Subscribe(topic, (_, _, _) => { }, new TopicOptions());
        first.Schema.Dispose();

        Refused(FletcherStatus.InvalidArgument,
            () => _subscriber.Subscribe(topic, (_, _, _) => { }, new TopicOptions { Profile = Reader }));
        first.Subscription.Dispose();
    }

    // ── The schema watch ────────────────────────────────────────────────────

    /// <summary>Mirrors SubscriberTest.SchemaWatchesAreCountedPerTopic.</summary>
    /// <remarks>
    /// The C++ case counts the provider's watch releases. The managed observation
    /// of the same count is the arrival: it stays pending until the LAST release,
    /// then answers SubscriptionEnded, and a further release is a no-op.
    /// </remarks>
    [Fact]
    public void AWatchIsReleasedOnlyByItsLastRelease()
    {
        TopicPath topic = Unique("watchcount");
        using SchemaArrival first = _subscriber.SubscribeSchema(topic);
        using SchemaArrival second = _subscriber.SubscribeSchema(topic);
        Assert.Equal(FletcherStatus.Pending, first.Wait(TimeSpan.Zero).Status);

        _subscriber.UnsubscribeSchema(topic);
        Assert.Equal(FletcherStatus.Pending, first.Wait(TimeSpan.Zero).Status);

        _subscriber.UnsubscribeSchema(topic);
        Assert.Equal(FletcherStatus.SubscriptionEnded, first.Wait(TimeSpan.Zero).Status);
        Assert.Equal(FletcherStatus.SubscriptionEnded, second.Wait(TimeSpan.Zero).Status);

        _subscriber.UnsubscribeSchema(topic);
    }

    /// <summary>Mirrors SubscriberTest.DestructorReleasesOutstandingSchemaWatches.</summary>
    /// <remarks>
    /// The C++ case counts the release a destructor makes. The managed observation:
    /// an arrival still pending when its subscriber is disposed is answered
    /// SubscriptionEnded, never left pending forever.
    /// </remarks>
    [Fact]
    public void DisposingTheSubscriberReleasesAnOutstandingWatch()
    {
        using var subscriber = new Subscriber(_provider);
        using SchemaArrival arrival = subscriber.SubscribeSchema(Unique("watchdispose"));
        Assert.Equal(FletcherStatus.Pending, arrival.Wait(TimeSpan.Zero).Status);

        subscriber.Dispose();

        Assert.Equal(FletcherStatus.SubscriptionEnded, arrival.Wait(TimeSpan.Zero).Status);
    }
}
