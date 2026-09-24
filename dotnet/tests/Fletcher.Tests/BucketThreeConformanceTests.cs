// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// BIND-4d-ii — BUCKET 3 ported: pub/sub semantics, over `inprocess`.
//
// ── What a "port" is here, and what it is not ───────────────────────────────
// There is ONE codec and one seam, so these are CONFORMANCE tests of the
// binding, not parity tests between two implementations (tracker, bucket table).
// A ported case asks "does the managed surface expose the same behaviour the C++
// case pins", and answers it through whatever the managed surface can actually
// observe - which is sometimes a different observation of the same fact.
//
// ── The mapping, total by construction ─────────────────────────────────────
// Each C++ case below names its C# mirror. Cases already covered by an earlier
// slice are named rather than duplicated: a second test of the same property is
// not more coverage, it is one more thing to keep true.
//
//   pubsub/tests/test_publisher_subscriber.cpp (18)
//     CreateTopicDelegatesToProvider          -> PublisherTests.ATopicIsDeclaredUnderTheNameTheSeamJoins (4b)
//     CreateTopicIsIdempotentForSameSchema    -> RedeclaringATopicWithTheSameSchemaIsIdempotent
//     CreateTopicRejectsConflictingSchema     -> RedeclaringATopicWithADifferentSchemaIsASchemaConflict
//     ListTopics                              -> PublisherTests.ATopicIsDeclaredUnderTheNameTheSeamJoins (4b)
//     NullProviderThrows (Publisher)          -> APublisherNeedsAProvider
//     PublishDelegatesToProvider              -> SubscriberTests.ARowPublishedFromCSharpArrivesAtCSharpAsTheCodecEncodedIt (4c)
//     SubscribeReturnsUniqueIds               -> EverySubscriptionIsADistinctHandle
//     SubscribeToUnknownTopicSucceeds         -> SubscriberTests.OnASchemaLessTransportTheArrivalAnswersOkWithNoSchema (4c)
//     SubscribeReturnsSchemaFromProvider      -> SubscriberTests.TheArrivalCarriesTheDeclaredSchemaAndItImportsAsArrow (4c)
//     MultiSubscriberFanOut                   -> SubscriberTests.TwoSubscriptionsToOneTopicEachGetTheirOwnDelivery (4c)
//     UnsubscribeRemovesSpecificSubscriber    -> SubscriberTests.TwoSubscriptionsToOneTopicEachGetTheirOwnDelivery (4c)
//     UnsubscribeUnknownIdIsANoOp             -> SubscriberTests.CancellingStopsDeliveryAndCancellingTwiceIsANoOp (4c)
//     NullProviderThrows (Subscriber)         -> ASubscriberNeedsAProvider
//     PublishWithAttachmentsFansOutCorrectly  -> AttachmentsReachEverySubscriberInTheFanOut
//     FanOutRunsCallbacksInSubscriptionOrder  -> TheFanOutRunsHandlersInSubscriptionOrder
//     UnsubscribeFromInsideCallbackTakesEffectImmediately
//                                             -> ReentrancyTests.ASelfCancellingHandlerIsServedAndTheDeliveryFreesTheHandle (4c-ii)
//     UnsubscribeLastSubscriberUnsubscribesFromProvider        -> EXCLUDED, see below
//     UnsubscribeWithRemainingSubscribersKeepsProviderSubscription -> EXCLUDED, see below
//
//   pubsub/tests/test_segments.cpp (5)
//     SegmentsThatAliasOrTruncateAreRefused   -> TheRefusedShapesAndTheAcceptedOnesBesideThem
//     JoinIsInvertible                        -> TheJoinIsInvertibleOverACorpus
//     RefusalReachesAllFourEntryPoints        -> NoEntryPointCanBeHandedAnUnvalidatedTopic
//     AcceptedNamesJoinToTheSameBytesAsBefore -> AcceptedNamesJoinToTheSameBytesAsBefore
//     NamesThatWouldTruncateOnTheWireAreRefused -> TheCapIsTheFastDdsCeilingLessTheCompanionSuffix
//
// ── TWO CASES ARE EXCLUDED (D-BIND-47), the reason structural not awkward ───
// `UnsubscribeLastSubscriberUnsubscribesFromProvider` and
// `UnsubscribeWithRemainingSubscribersKeepsProviderSubscription` both assert
// `mock->unsubscribe_count`: the fan-out's PROVIDER-LEVEL bookkeeping, which
// decides when the last local cancellation releases the transport subscription.
//
// That bookkeeping lives in the C++ `fletcher::Subscriber`. The managed
// `Subscriber` WRAPS it through `fl_subscriber_*` and reimplements none of it, so
// porting these would run the same C++ code a second time and call it a test of
// the binding. The managed surface also has no view of provider-level
// subscriptions at all - deliberately, since `ProviderRegistry` exposes no
// registration and no enumeration (D-BIND-24) - so there is no observation to
// make even if it were the binding's to make.
//
// Same reasoning the bucket table already uses for the 24 provider-internal Fast
// DDS cases. It moves bucket 3's portable count from 23 to 21 and the round's
// documented exclusions from 24 to 26; the tracker is corrected in the same
// commit rather than left disagreeing with this file.
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;

using Apache.Arrow;
using Apache.Arrow.Types;

using Eiva.Fletcher;

using Xunit;

namespace Eiva.Fletcher.Tests;

public class BucketThreePublisherTests : IDisposable
{
    private readonly PubSubProviderHandle _provider;
    private readonly Publisher _publisher;
    private readonly RecordBatch _batch = CodecFixtures.Scalar();

    public BucketThreePublisherTests()
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

    /// <summary>Mirrors PublisherTest.CreateTopicIsIdempotentForSameSchema.</summary>
    [Fact]
    public void RedeclaringATopicWithTheSameSchemaIsIdempotent()
    {
        TopicPath topic = TopicPath.Of("bucket3", "idempotent");

        _publisher.CreateTopic(topic, _batch.Schema);
        _publisher.CreateTopic(topic, _batch.Schema);

        // The C++ case counts the provider's declarations through a mock. The
        // managed observation of the same fact is the publisher's own list: two
        // declarations of one topic are one topic, not two.
        Assert.Single(_publisher.ListTopics());
        Assert.Equal("bucket3/idempotent", _publisher.ListTopics()[0]);
    }

    /// <summary>Mirrors PublisherTest.CreateTopicRejectsConflictingSchema.</summary>
    [Fact]
    public void RedeclaringATopicWithADifferentSchemaIsASchemaConflict()
    {
        TopicPath topic = TopicPath.Of("bucket3", "conflict");
        _publisher.CreateTopic(topic, _batch.Schema);

        var different = new Schema(
            [new Field("something_else", StringType.Default, nullable: true)], metadata: null);

        FletcherException refused =
            Assert.Throws<FletcherException>(() => _publisher.CreateTopic(topic, different));

        // THE NUMBER, not merely that it threw. A conflicting re-declaration must
        // carry the same status at the binding as it does at the seam, or a C#
        // caller cannot branch on it the way a C++ one does.
        Assert.Equal(FletcherStatus.SchemaConflict, refused.Status);
    }

    /// <summary>Mirrors PublisherTest.NullProviderThrows.</summary>
    [Fact]
    public void APublisherNeedsAProvider()
        => Assert.Throws<ArgumentNullException>(() => new Publisher(null!));
}

public class BucketThreeSubscriberTests : IDisposable
{
    private readonly PubSubProviderHandle _provider;
    private readonly Publisher _publisher;
    private readonly Subscriber _subscriber;
    private readonly RecordBatch _batch = CodecFixtures.Scalar();
    private readonly FletcherCodec _codec;
    private readonly BoundRows _rows;

    public BucketThreeSubscriberTests()
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

    private TopicPath Declare(string name)
    {
        TopicPath topic = TopicPath.Of("bucket3", name);
        _publisher.CreateTopic(topic, _batch.Schema);
        return topic;
    }

    /// <summary>Mirrors SubscriberTest.NullProviderThrows.</summary>
    [Fact]
    public void ASubscriberNeedsAProvider()
        => Assert.Throws<ArgumentNullException>(() => new Subscriber(null!));

    /// <summary>Mirrors SubscriberTest.SubscribeReturnsUniqueIds.</summary>
    [Fact]
    public void EverySubscriptionIsADistinctHandle()
    {
        TopicPath topic = Declare("uniqueids");

        SubscribeResult a = _subscriber.Subscribe(topic, (_, _, _) => { });
        SubscribeResult b = _subscriber.Subscribe(topic, (_, _, _) => { });
        a.Schema.Dispose();
        b.Schema.Dispose();

        // The C++ case compares uint64 ids. The managed surface hands out HANDLES
        // rather than numbers (an id is meaningful only to the subscriber that
        // issued it), so the property is stated about the handles - and each must
        // address its own subscription, which the cancel below is what proves.
        Assert.NotSame(a.Subscription, b.Subscription);
        Assert.True(a.Subscription.IsLive);
        Assert.True(b.Subscription.IsLive);

        a.Subscription.Dispose();
        Assert.False(a.Subscription.IsLive);
        Assert.True(b.Subscription.IsLive);

        b.Subscription.Dispose();
    }

    /// <summary>Mirrors SubscriberTest.FanOutRunsCallbacksInSubscriptionOrder.</summary>
    [Fact]
    public void TheFanOutRunsHandlersInSubscriptionOrder()
    {
        TopicPath topic = Declare("order");
        var order = new List<int>();
        var live = new List<Subscription>();

        for (int i = 0; i < 8; i++)
        {
            int index = i;
            SubscribeResult result = _subscriber.Subscribe(topic, (_, _, _) => order.Add(index));
            result.Schema.Dispose();
            live.Add(result.Subscription);
        }

        _publisher.Publish(topic, _rows, 0);

        // Subscription order, not any order. Eight rather than two because a
        // fan-out that happened to reverse would look correct with two.
        Assert.Equal(Enumerable.Range(0, 8), order);

        foreach (Subscription subscription in live)
        {
            subscription.Dispose();
        }
    }

    /// <summary>Mirrors SubscriberTest.PublishWithAttachmentsFansOutCorrectly.</summary>
    [Fact]
    public void AttachmentsReachEverySubscriberInTheFanOut()
    {
        // 4c already proved attachments cross A delivery. What this adds is that
        // every subscriber on the topic sees them - a fan-out that handed the set
        // to the first handler and an empty one to the rest would pass that test
        // and fail this.
        TopicPath topic = Declare("fanoutatts");

        using var attachments = new AttachmentsBuilder();
        attachments.Set("trace", [7, 8]);

        var seen = new List<byte[]>();
        var live = new List<Subscription>();

        for (int i = 0; i < 3; i++)
        {
            SubscribeResult result = _subscriber.Subscribe(topic, (_, _, view) =>
            {
                seen.Add(view.TryFind("trace"u8, out ReadOnlySpan<byte> value) ? value.ToArray() : []);
            });
            result.Schema.Dispose();
            live.Add(result.Subscription);
        }

        _publisher.Publish(topic, _rows, 0, attachments);

        Assert.Equal(3, seen.Count);
        Assert.All(seen, one => Assert.Equal(new byte[] { 7, 8 }, one));

        foreach (Subscription subscription in live)
        {
            subscription.Dispose();
        }
    }
}

public class BucketThreeSegmentsTests
{
    /// <summary>
    /// The harness's OWN inverse of the join, deliberately not product code.
    /// </summary>
    /// <remarks>
    /// An oracle that called a product `Split` would be comparing the product with
    /// itself. Splits on every '/', keeping empty pieces, so a trailing or doubled
    /// separator is visible rather than tidied away.
    /// </remarks>
    private static string[] Split(string joined) => joined.Split('/');

    /// <summary>Mirrors Segments.JoinIsInvertible.</summary>
    [Fact]
    public void TheJoinIsInvertibleOverACorpus()
    {
        // THE ORACLE. The seam identifies a topic by a segment LIST and every
        // provider by the joined byte string, so the map has to be injective or
        // two distinct topics become one - which is not a crash but a subscriber
        // receiving another topic's rows.
        string[][] corpus =
        [
            ["a"],
            ["a", "b"],
            ["a", "b", "c"],
            ["ab"],
            ["a_b"],
            ["a-b"],
            ["_private"],
            ["a__b"],
            ["ä"],
            ["telemetry", "vehicle", "pose"],
        ];

        var names = new Dictionary<string, string[]>(StringComparer.Ordinal);

        foreach (string[] segments in corpus)
        {
            TopicPath topic = TopicPath.Of(segments);
            string joined = topic.ToKey();

            Assert.Equal(segments, Split(joined));

            // No two ACCEPTED lists may land on one name. {"a","b"} and {"a/b"}
            // used to be the counterexample; the latter is refused now, which is
            // what makes the claim true rather than merely untested.
            Assert.False(names.ContainsKey(joined),
                $"'{joined}' is the joined name of two different segment lists");
            names[joined] = segments;
        }

        Assert.Equal(corpus.Length, names.Count);
    }

    /// <summary>Mirrors Segments.SegmentsThatAliasOrTruncateAreRefused.</summary>
    [Fact]
    public void TheRefusedShapesAndTheAcceptedOnesBesideThem()
    {
        // The accepted shapes are beside the refused ones ON PURPOSE: without
        // them the narrowing is open-ended, and a binding that refused everything
        // would pass a refusal-only test.
        Assert.Throws<ArgumentException>(() => TopicPath.Of("a/b"));
        Assert.Throws<ArgumentException>(() => TopicPath.Of("a\0b"));
        Assert.Throws<ArgumentException>(() => TopicPath.Of(""));
        Assert.Throws<ArgumentException>(() => TopicPath.Of("__schema"));

        Assert.Equal("a/b", TopicPath.Of("a", "b").ToKey());
        Assert.Equal("ab", TopicPath.Of("ab").ToKey());
        Assert.Equal("_private", TopicPath.Of("_private").ToKey());
        Assert.Equal("a__b", TopicPath.Of("a__b").ToKey());
    }

    /// <summary>Mirrors Segments.AcceptedNamesJoinToTheSameBytesAsBefore.</summary>
    [Fact]
    public void AcceptedNamesJoinToTheSameBytesAsBefore()
    {
        // THE OVER-REACH CONTROL. Narrowing the accepted set must not move the
        // wire bytes of any name that was already accepted - otherwise every
        // deployed topic silently renames itself.
        (string[] Segments, string Expected)[] goldens =
        [
            (["a"], "a"),
            (["a", "b"], "a/b"),
            (["telemetry", "vehicle", "pose"], "telemetry/vehicle/pose"),
            (["_private", "x"], "_private/x"),
        ];

        foreach ((string[] segments, string expected) in goldens)
        {
            TopicPath topic = TopicPath.Of(segments);
            Assert.Equal(expected, topic.ToKey());
            Assert.Equal(Encoding.UTF8.GetBytes(expected), Encoding.UTF8.GetBytes(topic.ToKey()));
        }
    }

    /// <summary>Mirrors Segments.NamesThatWouldTruncateOnTheWireAreRefused.</summary>
    [Fact]
    public void TheCapIsTheFastDdsCeilingLessTheCompanionSuffix()
    {
        // 255 is Fast DDS's announced ceiling; 246 is that less the 9 bytes of
        // "/__schema", so the companion channel each DDS provider derives survives
        // intact too. Bounded at 255 the data name would survive and its companion
        // would truncate back onto it - moving the collision to the hidden channel
        // rather than closing it.
        Assert.Equal(246, TopicPath.Of(new string('a', 246)).ToKey().Length);
        Assert.Throws<ArgumentException>(() => TopicPath.Of(new string('a', 247)));

        // And the separators count toward it.
        Assert.Throws<ArgumentException>(() => TopicPath.Of(new string('a', 123), new string('b', 123)));
    }

    /// <summary>Mirrors Segments.RefusalReachesAllFourEntryPoints.</summary>
    [Fact]
    public void NoEntryPointCanBeHandedAnUnvalidatedTopic()
    {
        // THE C++ CASE AND THIS ONE PROVE THE SAME PROPERTY BY DIFFERENT MEANS,
        // and the difference is worth stating rather than hiding. C++ drives four
        // provider methods with a bad SEGMENT LIST and checks each refuses,
        // because there the check could have been written at one caller. In C#
        // there is no such thing as an unvalidated topic to hand anywhere:
        // `TopicPath.Of` is the only constructor and every entry point takes a
        // `TopicPath`, so the type system does what the C++ case has to assert.
        //
        // What remains reachable is the DEFAULTED struct - the one hole a private
        // constructor cannot close - so the property is stated about that: every
        // entry point refuses it, none of them silently treats it as a topic.
        using PubSubProviderHandle provider =
            ProviderRegistry.Create(ProviderSelector.Parse("inprocess"), new ProviderConfig());
        using var publisher = new Publisher(provider);
        using var subscriber = new Subscriber(provider);

        RecordBatch batch = CodecFixtures.Scalar();
        using var codec = new FletcherCodec(batch.Schema);
        using BoundRows rows = codec.Bind(batch);

        TopicPath unvalidated = default;

        Assert.Throws<InvalidOperationException>(() => publisher.CreateTopic(unvalidated, batch.Schema));
        Assert.Throws<InvalidOperationException>(() => publisher.Publish(unvalidated, rows, 0));
        Assert.Throws<InvalidOperationException>(() => publisher.PublishRaw(unvalidated, [1, 2, 3]));
        Assert.Throws<InvalidOperationException>(() => subscriber.Subscribe(unvalidated, (_, _, _) => { }));
    }
}
