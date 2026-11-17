// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// BIND-5a - the `test_pubsub_arrow` file set, ported over `inprocess`.
//
// ── The mapping, total by construction ─────────────────────────────────────
// `pubsub-arrow/tests/test_pubsub_arrow.cpp` (33 cases at D-BIND-57's re-derivation).
// `U` = this file; `T` = the transport lane (TopicOptionsOverFastDdsTests.cs), for
// what only a provider with a schema channel can answer. PublisherArrow folds into
// Publisher (D-BIND-25), so its cases are answered by the Publisher C# ships.
//
//   PublisherArrowTest.CreateTopicConvertsArrowSchema -> PublisherTests.ATopicIsDeclaredUnderTheNameTheSeamJoins (4b)
//   PublisherArrowTest.ListTopics                     -> PublisherTests.ATopicIsDeclaredUnderTheNameTheSeamJoins (4b)
//   PublisherArrowTest.CreateTopicForwardsTopicOptions -> T BothFieldsReachTheProvider (D-BIND-57)
//   PublisherArrowTest.PublishTypeMismatchThrowsToCaller -> U RowsOfAnotherSchemaAreRefusedAndNothingIsPublished (D-BIND-60)
//   PublisherArrowTest.PublishAfterATypeMismatchStillDelivers -> U APublishAfterARefusedOneStillDelivers
//   PublisherArrowTest.PublishTwiceReusesScratchAndDeliversBothRows -> U TwoPublishesDeliverBothRows
//   PublisherArrowTest.RedeclaringATopicKeepsTheCodec -> U ARedeclaredTopicStillDecodes
//   SubscriberArrowTest.SubscribeReturnsArrowSchema    -> U SubscribeReturnsTheDeclaredSchema
//   SubscriberArrowTest.SubscribeSchemaYieldsAnImportableSchemaWithoutADataSubscription
//                                                      -> T ASubscriberArrowSchemaWatchYieldsTheDeclaredSchema
//   SubscriberArrowTest.SubscribeSchemaOnATransportWithoutOneThrowsNotSupported
//                                                      -> U ASchemaWatchIsNotSupportedWithoutASchemaChannel
//   SubscriberArrowTest.SubscribeForwardsTopicOptions  -> U TopicOptionsReachTheProviderThroughBothForms
//   SubscriberArrowTest.SubscribeWithOptionsOnATransportWithoutOneThrowsNotSupported
//                                                      -> U TopicOptionsReachTheProviderThroughBothForms
//   SubscriberArrowBatchTest.SubscribeForwardsTopicOptions -> U TopicOptionsReachTheProviderThroughBothForms
//   PubSubArrowTest.PublishSubscribeRoundtripWithArrowRow -> U ARowRoundTripsAsAOneRowBatch
//   PubSubArrowTest.PublishWithAttachments             -> U AttachmentsArriveWithTheirRow (bytes; the
//                                                         ADDRESS claim is D-BIND-58's, BIND-5b)
//   PubSubArrowTest.PublishDirectPassthrough           -> U ARawRowDecodesLikeAnyOther
//   PubSubArrowTest.Unsubscribe                        -> U UnsubscribingStopsDelivery
//   SubscriberArrowBatchTest.FlushesAtRowLimit         -> U FlushesAtTheRowLimit
//   SubscriberArrowBatchTest.FlushesAtTimeout          -> U FlushesAtTheTimeout
//   SubscriberArrowBatchTest.ClosingFlushOnUnsubscribe -> U UnsubscribingFlushesThePartialWindow
//   SubscriberArrowBatchTest.AttachmentsAlignWithRows  -> U AttachmentsAlignWithTheirRows
//   SubscriberArrowBatchTest.DroppedRowReportedAndAttachmentDiscarded
//                                                      -> U ADroppedRowIsReportedAndItsAttachmentDiscarded
//   SubscriberArrowBatchTest.OnlyDroppedRowsStillDeliversEmptyBatch
//                                                      -> U OnlyDroppedRowsStillDeliverAnEmptyBatch
//   SubscriberArrowBatchTest.DictionaryColumnRefoldedToDictionaryArray
//                                                      -> U BatchesAreWellFormed (its dictionary column, re-folded
//                                                         as domain 7 delivers it: ruling :551 (2), d10 D24)
//   SubscriberArrowBatchTest.DictionaryColumnPreservesNulls -> U ADictionaryColumnKeepsItsNulls (same ruling)
//   SubscriberArrowBatchTest.CorruptRowIsCountedDroppedAndBatchStaysAligned
//                                                      -> U ACorruptRowIsDroppedAndTheBatchStaysAligned
//   SubscriberArrowBatchTest.FixedSizeListWithNamedItemArrivesNonNull
//                                                      -> U AFixedSizeListArrivesNonNull
//   SubscriberArrowBatchTest.NestedDictionarySchemaReportsEveryRowDropped
//                                                      -> U ASchemaTheCodecCannotOpenDropsEveryRow (C#'s codec
//                                                         DECODES nested dictionaries, D-BIND-39, so the property
//                                                         is shown on a schema it refuses: a dictionary of structs)
//   SubscriberArrowBatchTest.FinishFailureIsReportedNotFatal -> EXCLUDED, see below
//   SubscriberArrowBatchTest.UnsubscribeFromCallbackDuringRowLimitFlushStopsDeliveryCleanly
//                                                      -> U UnsubscribingFromTheHandlerDuringARowLimitFlushStopsDelivery
//   SubscriberArrowBatchTest.UnsubscribeFromInsideATimeoutFlushIsSafe
//                                                      -> U UnsubscribingFromInsideATimeoutFlushIsSafe
//   SubscriberArrowBatchTest.BatchesAreValidArrow      -> U BatchesAreWellFormed (C++'s four-column schema;
//                                                         Apache.Arrow has no ValidateFull, so its checks are
//                                                         done by hand - review T-D2)
//   SubscriberArrowBatchTest.ReuseAcrossWindows        -> U WindowsFollowOneAnother
//
// ── ONE CASE IS EXCLUDED ────────────────────────────────────────────────────
// `FinishFailureIsReportedNotFatal` forces C++'s BatchDecoder to fail at Finish(),
// when 200 distinct values overflow an int8 dictionary index during RE-FOLDING.
// Since D10C-7 C#'s batches ARE domain 7's (d10 L4), so that case is domain 7's own
// and C# adds nothing to it; the neighbouring property - a window that cannot be
// decoded is reported, never fatal - is ASchemaTheCodecCannotOpenDropsEveryRow's and
// ACorruptRowIsDroppedAndTheBatchStaysAligned's. So the file set is 33: 32 mapped,
// 1 excluded.
//
// ── Two legs (D10C-7; the ladder's ground rules) ────────────────────────────
// Every batched subject runs on the leg this process is: the SERVER leg when the
// server shim's file is beside the tests (d10 L5's own fact, design P5), else the
// EDGE leg, where the subject is asserted as the BC-10 truth table instead
// (EdgeRefuses). Per-row subjects run unchanged on both.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Threading;

using Apache.Arrow;
using Apache.Arrow.C;
using Apache.Arrow.Types;

using Eiva.Fletcher.Testing;

using Xunit;

namespace Eiva.Fletcher.Tests;

public sealed class SubscriberArrowTests : IDisposable
{
    private static readonly Schema TwoColumns = new(
    [
        new Field("x", Int32Type.Default, nullable: true),
        new Field("name", StringType.Default, nullable: true),
    ], metadata: null);

    private readonly PubSubProviderHandle _provider;
    private readonly Publisher _publisher;
    private readonly SubscriberArrow _subscriber;
    private readonly Sink _sink = new();

    public SubscriberArrowTests()
    {
        _provider = ProviderRegistry.Create(ProviderSelector.Parse("inprocess"), new ProviderConfig());
        _publisher = new Publisher(_provider);
        _subscriber = new SubscriberArrow(_provider);
    }

    public void Dispose()
    {
        _subscriber.Dispose();
        _publisher.Dispose();
        _provider.Dispose();
        _sink.Dispose();
    }

    private static TopicPath Topic(string name) => TopicPath.Of("bind5", "arrow", name);

    private TopicPath Declared(string name)
    {
        TopicPath topic = Topic(name);
        _publisher.CreateTopic(topic, TwoColumns);
        return topic;
    }

    private static RecordBatch Row(int x, string name) => new(
        TwoColumns,
        [new Int32Array.Builder().Append(x).Build(), new StringArray.Builder().Append(name).Build()],
        length: 1);

    private void Publish(TopicPath topic, int x, string name, AttachmentsBuilder? attachments = null)
    {
        using RecordBatch batch = Row(x, name);
        using var codec = new FletcherCodec(TwoColumns);
        using BoundRows rows = codec.Bind(batch);
        _publisher.Publish(topic, rows, 0, attachments);
    }

    private static BatchOptions Options(long maxRows, TimeSpan timeout) => new() { MaxRows = maxRows, Timeout = timeout };

    private static readonly TimeSpan Long = TimeSpan.FromMinutes(10);

    /// <summary>How long a delivery that must come is waited for.</summary>
    private static readonly TimeSpan Await = TimeSpan.FromSeconds(10);

    /// <summary>The fixed window over which "nothing delivered" is asserted (one-sided; ladder ground rules).</summary>
    private static readonly TimeSpan Quiet = TimeSpan.FromMilliseconds(300);

    // ── The two legs (D10C-7) ───────────────────────────────────────────────

    /// <summary>
    /// The server leg: the server shim's file is beside the tests, so it is the one loaded
    /// (d10 L5; NativeLoaderTests.TheLoadedShimIsTheServerExactlyWhenItIsBesideTheTests).
    /// Read from outside, never from NativeMethods (design P5).
    /// </summary>
    internal static bool ServerLeg { get; } =
        File.Exists(Path.Combine(AppContext.BaseDirectory, LoadedShim.ServerFileName));

    /// <summary>
    /// The edge build's refusal of a batched entry point, verbatim (d9 L12, L16; the one door,
    /// <c>c-abi/src/binding.cpp</c> <c>RequireServerBuild</c>).
    /// </summary>
    internal const string EdgeShimText =
        "fl_subscriber_subscribe_batches: the edge build of the shim carries no batcher; load the server build " +
        "(fletcher-c-abi-server) for batched subscriptions";

    /// <summary>The server package a C# app adds for batches (ruling :605 (1); d10 L15).</summary>
    internal const string ServerPackage = "Eiva.Fletcher.Server";

    /// <summary>
    /// The edge leg's truth table for one batched subject (BC-10; d10 L3, L6, D29; ruling :551 (1),
    /// :575 (2)): <paramref name="subscribe"/> throws <see cref="FletcherException"/>
    /// (<see cref="FletcherStatus.NotSupported"/>, origin <see cref="FletcherOrigin.Seam"/>) whose
    /// message is the shim's text verbatim followed by exactly one sentence naming the server
    /// package; the handler is never called, and a later publish to the topic delivers nothing.
    /// </summary>
    /// <param name="subscribe">The batched subscribe under test, given the handler to pass.</param>
    /// <param name="publish">A publish to the subscribed topic; the default is one TwoColumns row.</param>
    private void EdgeRefuses(TopicPath topic, Func<RecordBatchHandler, SubscribeResult> subscribe, Action? publish = null)
    {
        Assert.False(ServerLeg, "the edge truth table was asked on the server leg");
        using var never = new Sink();

        FletcherException refused = Assert.Throws<FletcherException>(() => subscribe(never.Handler).Schema.Dispose());
        Assert.Equal(FletcherStatus.NotSupported, refused.Status);
        Assert.Equal(FletcherOrigin.Seam, refused.Origin);
        Assert.StartsWith(EdgeShimText, refused.Message, StringComparison.Ordinal);
        string added = refused.Message[EdgeShimText.Length..].Trim();
        Assert.True(
            added.Contains(ServerPackage, StringComparison.Ordinal)
                && added.EndsWith('.')
                && !added[..^1].Replace(ServerPackage, string.Empty, StringComparison.Ordinal).Contains('.', StringComparison.Ordinal),
            $"after the shim's text the binding adds exactly one sentence naming {ServerPackage}; it added '{added}'");

        (publish ?? (() => Publish(topic, 1, "edge")))();
        Assert.False(never.WaitFor(1, Quiet), "a refused batched subscribe called its handler");
    }

    // ── Publisher (PublisherArrow folds into it) ────────────────────────────

    /// <summary>Mirrors PublisherArrowTest.PublishTypeMismatchThrowsToCaller.</summary>
    /// <remarks>
    /// THE CASE THAT FOUND D-BIND-60. `x` as float32 is the same WIDTH as the declared
    /// int32, so before the shim checked the rows' schema these bytes were delivered
    /// and decoded, silently, into the wrong values. Now nothing is published.
    /// </remarks>
    [Fact]
    public void RowsOfAnotherSchemaAreRefusedAndNothingIsPublished()
    {
        TopicPath topic = Declared("mismatch");
        _subscriber.Subscribe(topic, _sink.Handler).Schema.Dispose();

        var floaty = new Schema([new Field("x", FloatType.Default, nullable: true), new Field("name", StringType.Default, nullable: true)], metadata: null);
        using RecordBatch batch = new(floaty, [new FloatArray.Builder().Append(1.5f).Build(), new StringArray.Builder().Append("a").Build()], length: 1);
        using var codec = new FletcherCodec(floaty);
        using BoundRows rows = codec.Bind(batch);

        FletcherException refused = Assert.Throws<FletcherException>(() => _publisher.Publish(topic, rows, 0));
        Assert.Equal(FletcherStatus.InvalidArgument, refused.Status);
        Assert.Contains("wire layout", refused.Message, StringComparison.Ordinal);
        Assert.Empty(_sink.Snapshot());
    }

    /// <summary>Mirrors PublisherArrowTest.PublishAfterATypeMismatchStillDelivers.</summary>
    [Fact]
    public void APublishAfterARefusedOneStillDelivers()
    {
        TopicPath topic = Declared("afterrefusal");
        _subscriber.Subscribe(topic, _sink.Handler).Schema.Dispose();

        var floaty = new Schema([new Field("x", FloatType.Default, nullable: true), new Field("name", StringType.Default, nullable: true)], metadata: null);
        using (RecordBatch bad = new(floaty, [new FloatArray.Builder().Append(1f).Build(), new StringArray.Builder().Append("a").Build()], length: 1))
        using (var codec = new FletcherCodec(floaty))
        using (BoundRows rows = codec.Bind(bad))
        {
            Assert.Throws<FletcherException>(() => _publisher.Publish(topic, rows, 0));
        }

        Publish(topic, 7, "good");

        Delivery only = Assert.Single(_sink.Snapshot());
        Assert.Equal(7, ((Int32Array)only.Batch!.Column(0)).GetValue(0));
    }

    /// <summary>Mirrors PublisherArrowTest.PublishTwiceReusesScratchAndDeliversBothRows.</summary>
    [Fact]
    public void TwoPublishesDeliverBothRows()
    {
        TopicPath topic = Declared("twice");
        _subscriber.Subscribe(topic, _sink.Handler).Schema.Dispose();

        Publish(topic, 1, "one");
        Publish(topic, 2, "two");

        List<Delivery> seen = _sink.Snapshot();
        Assert.Equal(2, seen.Count);
        Assert.Equal("one", ((StringArray)seen[0].Batch!.Column(1)).GetString(0));
        Assert.Equal("two", ((StringArray)seen[1].Batch!.Column(1)).GetString(0));
    }

    /// <summary>Mirrors PublisherArrowTest.RedeclaringATopicKeepsTheCodec.</summary>
    [Fact]
    public void ARedeclaredTopicStillDecodes()
    {
        TopicPath topic = Declared("redeclared");
        _publisher.CreateTopic(topic, TwoColumns);
        _subscriber.Subscribe(topic, _sink.Handler).Schema.Dispose();

        Publish(topic, 3, "three");

        Delivery only = Assert.Single(_sink.Snapshot());
        Assert.Equal(3, ((Int32Array)only.Batch!.Column(0)).GetValue(0));
        Assert.Equal("three", ((StringArray)only.Batch!.Column(1)).GetString(0));
    }

    // ── SubscriberArrow ─────────────────────────────────────────────────────

    /// <summary>Mirrors SubscriberArrowTest.SubscribeReturnsArrowSchema.</summary>
    [Fact]
    public void SubscribeReturnsTheDeclaredSchema()
    {
        SubscribeResult result = _subscriber.Subscribe(Declared("schema"), _sink.Handler);
        using (result.Schema)
        {
            SchemaWaitResult wait = result.Schema.Wait(TimeSpan.Zero);
            using (wait.Schema)
            {
                Schema schema = wait.Schema!.ToArrowSchema();
                Assert.Equal(2, schema.FieldsList.Count);
                Assert.Equal("x", schema.FieldsList[0].Name);
                Assert.IsType<Int32Type>(schema.FieldsList[0].DataType);
                Assert.Equal("name", schema.FieldsList[1].Name);
                Assert.IsType<StringType>(schema.FieldsList[1].DataType);
            }
        }
    }

    /// <summary>Mirrors SubscriberArrowTest.SubscribeSchemaOnATransportWithoutOneThrowsNotSupported.</summary>
    [Fact]
    public void ASchemaWatchIsNotSupportedWithoutASchemaChannel()
    {
        FletcherException refused = Assert.Throws<FletcherException>(() => _subscriber.SubscribeSchema(Topic("watch")));
        Assert.Equal(FletcherStatus.NotSupported, refused.Status);
    }

    /// <summary>
    /// Mirrors SubscriberArrowTest.SubscribeForwardsTopicOptions,
    /// SubscriberArrowTest.SubscribeWithOptionsOnATransportWithoutOneThrowsNotSupported and
    /// SubscriberArrowBatchTest.SubscribeForwardsTopicOptions.
    /// </summary>
    /// <remarks>
    /// `inprocess` knows no options, so a profile REACHING it is refused NotSupported -
    /// and a SubscriberArrow that dropped the options would subscribe without error.
    /// Both forms, per-row and batched. This shows the options ARRIVE; that they arrive
    /// UNCHANGED - the C++ cases' `last_subscribe_options == options` - is shown over
    /// Fast DDS, in the transport lane's ASubscriberArrowForwardsItsReaderProfileOnBothForms
    /// (review T-D3).
    ///
    /// D10C-7 (audit 0a T7, FORBIDDEN-BY-TOLERANCE): the edge build answers NotSupported for the
    /// BUILD, so "NotSupported" alone cannot tell a provider's refusal of the options from the
    /// build's refusal of the call. Server leg: the provider's refusal, carrying neither the edge
    /// build's text nor the package sentence (design D2: appended only when the build is Edge).
    /// Edge leg: the truth table.
    /// </remarks>
    [Fact]
    public void TopicOptionsReachTheProviderThroughBothForms()
    {
        var profiled = new TopicOptions { Profile = "x" };

        FletcherException perRow = Assert.Throws<FletcherException>(
            () => _subscriber.Subscribe(Topic("optrow"), _sink.Handler, profiled));
        Assert.Equal(FletcherStatus.NotSupported, perRow.Status);

        TopicPath topic = Declared("optbatch");
        if (!ServerLeg)
        {
            EdgeRefuses(topic, handler => _subscriber.SubscribeBatched(topic, handler, null, profiled));
            return;
        }

        FletcherException batched = Assert.Throws<FletcherException>(
            () => _subscriber.SubscribeBatched(topic, _sink.Handler, null, profiled));
        Assert.Equal(FletcherStatus.NotSupported, batched.Status);
        Assert.DoesNotContain(EdgeShimText, batched.Message, StringComparison.Ordinal);
        Assert.DoesNotContain(ServerPackage, batched.Message, StringComparison.Ordinal);
    }

    /// <summary>
    /// The edge refusal on its own (D10C-7; d10 L3 D29, L6; BC-10; ruling :551 (1), :575 (2)): on the
    /// edge shim a batched subscribe, with default or explicit options, is the truth table and
    /// leaves no claim on its topic, so a per-row subscription to it is then accepted and delivers
    /// (FWD-1). On the server shim the same calls are accepted.
    /// </summary>
    [Fact]
    public void ABatchedSubscribeOnTheEdgeShimIsRefusedNamingTheServerPackage()
    {
        TopicPath topic = Declared("edgerefusal");
        if (ServerLeg)
        {
            SubscribeResult accepted = _subscriber.SubscribeBatched(topic, _sink.Handler);
            accepted.Schema.Dispose();
            accepted.Subscription.Dispose();
            return;
        }

        EdgeRefuses(topic, handler => _subscriber.SubscribeBatched(topic, handler));
        EdgeRefuses(topic, handler => _subscriber.SubscribeBatched(topic, handler, Options(1, Long)));

        _subscriber.Subscribe(topic, _sink.Handler).Schema.Dispose();
        Publish(topic, 3, "perrow");
        Assert.True(_sink.WaitFor(1, Await), "a per-row subscription after the refused batched ones did not deliver");
        Assert.Equal(3, ((Int32Array)_sink.Snapshot()[0].Batch!.Column(0)).GetValue(0));
    }

    // ── Round trips ─────────────────────────────────────────────────────────

    /// <summary>Mirrors PubSubArrowTest.PublishSubscribeRoundtripWithArrowRow.</summary>
    /// <remarks>
    /// RowLimit is the reason on every per-row call: d10 L2 lets C# carry only a reason
    /// the C surface publishes, and of those only "the window is full" is true of a
    /// window of one row (Closing would claim the last delivery, Timeout a deadline).
    /// </remarks>
    [Fact]
    public void ARowRoundTripsAsAOneRowBatch()
    {
        TopicPath topic = Declared("roundtrip");
        _subscriber.Subscribe(topic, _sink.Handler).Schema.Dispose();

        Publish(topic, 42, "hello");

        Delivery only = Assert.Single(_sink.Snapshot());
        Assert.Equal(1, only.Rows);
        Assert.Equal(BatchReason.RowLimit, only.Status.Reason);
        Assert.Equal(42, ((Int32Array)only.Batch!.Column(0)).GetValue(0));
        Assert.Equal("hello", ((StringArray)only.Batch!.Column(1)).GetString(0));
    }

    /// <summary>Mirrors PubSubArrowTest.PublishWithAttachments.</summary>
    /// <remarks>
    /// Weaker, and by ruling: the C++ case checks the delivered blob IS the published
    /// one, by address. A SubscriberArrow hands the handler owned COPIES (D-BIND-59),
    /// so this checks the bytes; the zero-copy delivery view is D-BIND-58's claim,
    /// measured by the copy oracle at BIND-5b.
    /// </remarks>
    [Fact]
    public void AttachmentsArriveWithTheirRow()
    {
        TopicPath topic = Declared("attachments");
        _subscriber.Subscribe(topic, _sink.Handler).Schema.Dispose();

        using var sent = new AttachmentsBuilder();
        sent.Set("img", [1, 2, 3, 4]);
        Publish(topic, 1, "a", sent);

        Delivery only = Assert.Single(_sink.Snapshot());
        AttachmentsBuilder arrived = Assert.Single(only.Attachments);
        Assert.True(arrived.TryFind("img"u8, out ReadOnlySpan<byte> value));
        Assert.Equal(new byte[] { 1, 2, 3, 4 }, value.ToArray());
    }

    /// <summary>Mirrors PubSubArrowTest.PublishDirectPassthrough.</summary>
    [Fact]
    public void ARawRowDecodesLikeAnyOther()
    {
        TopicPath topic = Declared("raw");
        _subscriber.Subscribe(topic, _sink.Handler).Schema.Dispose();

        byte[] encoded;
        using (RecordBatch batch = Row(99, "raw"))
        using (var codec = new FletcherCodec(TwoColumns))
        using (BoundRows rows = codec.Bind(batch))
        {
            var buffer = new System.Buffers.ArrayBufferWriter<byte>();
            codec.Encode(rows, 0, buffer);
            encoded = buffer.WrittenSpan.ToArray();
        }

        _publisher.PublishRaw(topic, encoded);

        Delivery only = Assert.Single(_sink.Snapshot());
        Assert.Equal(99, ((Int32Array)only.Batch!.Column(0)).GetValue(0));
    }

    /// <summary>Mirrors PubSubArrowTest.Unsubscribe.</summary>
    [Fact]
    public void UnsubscribingStopsDelivery()
    {
        TopicPath topic = Declared("unsubscribe");
        SubscribeResult result = _subscriber.Subscribe(topic, _sink.Handler);
        result.Schema.Dispose();

        Publish(topic, 1, "a");
        Assert.Single(_sink.Snapshot());

        result.Subscription.Dispose();
        Publish(topic, 2, "b");
        Assert.Single(_sink.Snapshot());
    }

    // ── One object, one kind per topic (D10C-6) ─────────────────────────────
    //
    // Ruling :575 (6); d10 L3's ruled refusal; d9 D33. One SubscriberArrow refuses a
    // per-row and a batched subscription to the same topic, in either order, because
    // once both share one native subscriber the later would silently replace the
    // earlier. The scope is this object only: the same kind twice, other topics, and
    // other subscribers are not refused (L3: no rule without a ruling).

    private static SubscribeResult SubscribeKind(SubscriberArrow on, bool perRow, TopicPath topic, RecordBatchHandler handler) =>
        perRow
            ? on.Subscribe(topic, handler)
            : on.SubscribeBatched(topic, handler, Options(1, Long));

    /// <summary>
    /// The second kind on a topic is refused by name, the first keeps delivering, and the
    /// other kind is accepted once the first is gone.
    /// </summary>
    /// <remarks>
    /// The last arm is the control: a refusal that left its claim behind, or a close that
    /// never released it, fails there. The release is `Unsubscribe` when the row came
    /// first and `Subscription.Dispose` when the batched one did, so both routes are shown.
    /// The refused handler is waited on one-sidedly: absence is shown by a short timeout.
    ///
    /// D10C-7, the edge leg (FWD-1, FWD-1b; design D2's check order, claim first, then native):
    /// row first, the batched call is the same-topic refusal there too, and once the row is
    /// gone it is the truth table; batched first, it is the truth table and leaves no claim,
    /// so the per-row subscription is accepted and delivers.
    /// </remarks>
    [Theory]
    [InlineData(true)]
    [InlineData(false)]
    public void ARowAndABatchedSubscriptionToOneTopicAreRefusedWhicheverComesSecond(bool rowFirst)
    {
        TopicPath topic = Declared(rowFirst ? "mixrowfirst" : "mixbatchedfirst");
        using var refused = new Sink();

        if (!ServerLeg && !rowFirst)
        {
            EdgeRefuses(topic, handler => SubscribeKind(_subscriber, perRow: false, topic, handler));
            SubscribeResult perRow = SubscribeKind(_subscriber, perRow: true, topic, refused.Handler);
            perRow.Schema.Dispose();
            Publish(topic, 6, "taken");
            Assert.True(refused.WaitFor(1, Await), "the per-row subscription after the refused batched one did not deliver");
            Assert.Equal(6, ((Int32Array)refused.Snapshot()[0].Batch!.Column(0)).GetValue(0));
            perRow.Subscription.Dispose();
            return;
        }

        SubscribeResult held = SubscribeKind(_subscriber, perRow: rowFirst, topic, _sink.Handler);
        held.Schema.Dispose();

        InvalidOperationException refusal = Assert.Throws<InvalidOperationException>(
            () => SubscribeKind(_subscriber, perRow: !rowFirst, topic, refused.Handler));
        Assert.Contains(topic.ToString(), refusal.Message, StringComparison.Ordinal);
        // Every message names both kinds (the one refused and the one held), so only the phrase
        // that names the HELD kind discriminates (D10C-6 review DEBT-3).
        Assert.Contains(rowFirst ? "holds a per-row" : "holds a batched", refusal.Message, StringComparison.Ordinal);

        Publish(topic, 5, "kept");
        Assert.True(_sink.WaitFor(1, Await), "the first subscription stopped delivering");
        Assert.Equal(5, ((Int32Array)_sink.Snapshot()[0].Batch!.Column(0)).GetValue(0));
        Assert.False(refused.WaitFor(1, Quiet), "the refused handler was called");

        if (rowFirst)
        {
            _subscriber.Unsubscribe(held.Subscription);
        }
        else
        {
            held.Subscription.Dispose();
        }

        if (!ServerLeg)
        {
            EdgeRefuses(topic, handler => SubscribeKind(_subscriber, perRow: false, topic, handler));
            return;
        }

        SubscribeResult accepted = SubscribeKind(_subscriber, perRow: !rowFirst, topic, refused.Handler);
        accepted.Schema.Dispose();
        Publish(topic, 6, "taken");
        Assert.True(refused.WaitFor(1, Await), "the accepted subscription did not deliver");
        Assert.Equal(6, ((Int32Array)refused.Snapshot()[0].Batch!.Column(0)).GetValue(0));
        accepted.Subscription.Dispose();
    }

    /// <summary>
    /// Guard: the refusal is the mix only. The same kind twice on one topic, and a row and a
    /// batched subscription to different topics, are not refused.
    /// </summary>
    /// <remarks>D10C-7 (FWD-1b): the batched arms are the server leg's; on the edge leg each is the truth table.</remarks>
    [Fact]
    public void TheSameKindTwiceAndDifferentTopicsAreNotRefused()
    {
        using var second = new Sink();
        TopicPath rows = Declared("samekindrow");
        TopicPath batches = Declared("samekindbatched");
        TopicPath apart1 = Declared("apartrow");
        TopicPath apart2 = Declared("apartbatched");

        SubscribeKind(_subscriber, perRow: true, rows, _sink.Handler).Schema.Dispose();
        SubscribeKind(_subscriber, perRow: true, rows, second.Handler).Schema.Dispose();
        SubscribeKind(_subscriber, perRow: true, apart1, _sink.Handler).Schema.Dispose();
        if (!ServerLeg)
        {
            EdgeRefuses(batches, handler => SubscribeKind(_subscriber, perRow: false, batches, handler));
            EdgeRefuses(apart2, handler => SubscribeKind(_subscriber, perRow: false, apart2, handler));
            return;
        }

        SubscribeKind(_subscriber, perRow: false, batches, _sink.Handler).Schema.Dispose();
        SubscribeKind(_subscriber, perRow: false, batches, second.Handler).Schema.Dispose();
        SubscribeKind(_subscriber, perRow: false, apart2, _sink.Handler).Schema.Dispose();
    }

    /// <summary>
    /// Guard: the scope is one object. A row subscription here and a batched one on another
    /// SubscriberArrow over the same provider, in either order, do not refuse each other:
    /// the transport's rule between separate subscribers is domain 3/4's (d10 L3).
    /// </summary>
    /// <remarks>D10C-7 (FWD-1b): both arms are the server leg's; on the edge leg the batched call is the truth table.</remarks>
    [Theory]
    [InlineData(true)]
    [InlineData(false)]
    public void TwoArrowSubscribersOnOneProviderDoNotRefuseEachOther(bool rowOnThisOneFirst)
    {
        using var other = new SubscriberArrow(_provider);
        using var otherSink = new Sink();
        TopicPath topic = Declared(rowOnThisOneFirst ? "twoobjectsrowfirst" : "twoobjectsbatchedfirst");
        SubscriberArrow batchedOn = rowOnThisOneFirst ? other : _subscriber;
        if (!ServerLeg)
        {
            SubscribeKind(rowOnThisOneFirst ? _subscriber : other, perRow: true, topic, _sink.Handler).Schema.Dispose();
            EdgeRefuses(topic, handler => SubscribeKind(batchedOn, perRow: false, topic, handler));
            return;
        }

        SubscribeKind(_subscriber, perRow: rowOnThisOneFirst, topic, _sink.Handler).Schema.Dispose();
        SubscribeKind(other, perRow: !rowOnThisOneFirst, topic, otherSink.Handler).Schema.Dispose();
    }

    // ── Per row, on the native per-row path (D10C-3) ────────────────────────
    //
    // d10 L7, L9, L11, D20, D30, D32; register BC-16; ruling :575 (3). A per-row
    // subscription has no window: each row offered is exactly one handler call,
    // carrying that row alone or a null batch with RowsDropped = 1 - never skipped,
    // never folded into another call. These rely on inprocess delivering before
    // Publish returns, as the whole file does.

    private enum RowKind
    {
        Good,
        OneByte,
        Truncated,
    }

    private static byte[] Bytes(RowKind kind, int i) => kind switch
    {
        RowKind.Good => Encoded(i, "r" + i),
        RowKind.OneByte => [0x00],
        _ => Encoded(i, "r" + i)[..^3],
    };

    /// <summary>A schema C#'s codec refuses by name: a dictionary whose value type is a struct.</summary>
    private static Schema DictionaryOfStructs()
    {
        var nested = new StructType([new Field("x", Int32Type.Default, nullable: true)]);
        return new Schema(
            [new Field("category", new DictionaryType(Int32Type.Default, nested, ordered: false), nullable: true)],
            metadata: null);
    }

    /// <summary>
    /// BC-16's conservation, over three ways a per-row row goes bad: <c>mixed</c>, malformed
    /// raw rows (one byte, and an encoding cut short) among good ones on a declared topic;
    /// <c>subscribedFirst</c>, a subscription made before the declaration on the default
    /// inprocess loopback, so no row carries a schema; <c>unopenable</c>, a schema the codec
    /// refuses (a dictionary of structs, as ASchemaTheCodecCannotOpenDropsEveryRow).
    /// </summary>
    /// <remarks>
    /// Every handler call is one row with RowsDropped = 0 - its value in publish order and
    /// exactly its own attachments - or a null batch with RowsDropped = 1 and no attachments
    /// (ruling :575 (3), d10 D30); every reason is RowLimit (d10 L2, see
    /// ARowRoundTripsAsAOneRowBatch); calls = rows offered = Σ rows + Σ RowsDropped; and
    /// nothing arrives after Unsubscribe.
    /// </remarks>
    [Theory]
    [InlineData("mixed")]
    [InlineData("subscribedFirst")]
    [InlineData("unopenable")]
    public void PerRowDeliversEachRowAloneOrCountsItAsOneDrop(string arm)
    {
        TopicPath topic = Topic("perrow" + arm);
        Schema declared = arm == "unopenable" ? DictionaryOfStructs() : TwoColumns;
        SubscribeResult result;
        if (arm == "subscribedFirst")
        {
            result = _subscriber.Subscribe(topic, _sink.Handler);
            _publisher.CreateTopic(topic, declared);
        }
        else
        {
            _publisher.CreateTopic(topic, declared);
            result = _subscriber.Subscribe(topic, _sink.Handler);
        }

        result.Schema.Dispose();

        // The offer, seeded so a failure replays. Only the mixed arm sends malformed
        // rows; in the other two not even a well-formed row can be decoded.
        const int Offered = 16;
        var random = new Random(20261115);
        var kinds = new RowKind[Offered];
        var blobs = new byte[]?[Offered];
        for (int i = 0; i < Offered; i++)
        {
            int roll = random.Next(5);
            kinds[i] = arm != "mixed" || roll >= 2 ? RowKind.Good : roll == 0 ? RowKind.OneByte : RowKind.Truncated;
            blobs[i] = random.Next(2) == 0 ? [(byte)i, (byte)(i * 7)] : null;
        }

        // The fixture, checked: domain 2's decode refuses exactly the rows meant to be
        // malformed, and the mixed arm holds both kinds, each followed by a good row.
        using (var reference = new FletcherCodec(TwoColumns))
        {
            for (int i = 0; i < Offered; i++)
            {
                byte[] bytes = Bytes(kinds[i], i);
                if (kinds[i] == RowKind.Good)
                {
                    reference.Decode(bytes).Dispose();
                }
                else
                {
                    Assert.ThrowsAny<Exception>(() => reference.Decode(bytes).Dispose());
                }
            }
        }

        // ...and the unopenable arm's schema really is one the codec refuses, so its
        // drops cannot come from the rows' bytes alone.
        if (arm == "unopenable")
        {
            Assert.ThrowsAny<Exception>(() => new FletcherCodec(declared).Dispose());
        }

        if (arm == "mixed")
        {
            foreach (RowKind bad in new[] { RowKind.OneByte, RowKind.Truncated })
            {
                bool followed = false;
                for (int i = 0; i + 1 < Offered; i++)
                {
                    followed |= kinds[i] == bad && kinds[i + 1] == RowKind.Good;
                }

                Assert.True(followed, $"the seed gives no {bad} row followed by a good one");
            }
        }

        for (int i = 0; i < Offered; i++)
        {
            using AttachmentsBuilder? attachments = blobs[i] is null ? null : new AttachmentsBuilder();
            attachments?.Set("blob", blobs[i]);
            _publisher.PublishRaw(topic, Bytes(kinds[i], i), attachments);
        }

        List<Delivery> seen = _sink.Snapshot();

        // Each call alone: one row and no drop, or no row and one drop.
        for (int c = 0; c < seen.Count; c++)
        {
            Delivery d = seen[c];
            Assert.True(
                (d.Rows == 1 && d.Status.RowsDropped == 0) || (d.Batch is null && d.Status.RowsDropped == 1),
                $"call {c} carries {d.Rows} row(s) and {d.Status.RowsDropped} drop(s); a per-row call is one row or one drop");
            Assert.Equal(BatchReason.RowLimit, d.Status.Reason);
        }

        // One call per row offered, and the books balance.
        Assert.Equal(Offered, seen.Count);
        long rows = 0;
        long dropped = 0;
        foreach (Delivery d in seen)
        {
            rows += Math.Max(0, d.Rows);
            dropped += d.Status.RowsDropped;
        }

        Assert.Equal(Offered, rows + dropped);

        // Call i is row i: its value and exactly its attachments, or its drop.
        for (int i = 0; i < Offered; i++)
        {
            Delivery d = seen[i];
            if (arm == "mixed" && kinds[i] == RowKind.Good)
            {
                Assert.Equal(i, ((Int32Array)d.Batch!.Column(0)).GetValue(0));
                Assert.Equal("r" + i, ((StringArray)d.Batch!.Column(1)).GetString(0));
                AttachmentsBuilder arrived = Assert.Single(d.Attachments);
                if (blobs[i] is null)
                {
                    Assert.Equal(0, arrived.Count);
                }
                else
                {
                    Assert.Equal(1, arrived.Count);
                    Assert.True(arrived.TryFind("blob"u8, out ReadOnlySpan<byte> value));
                    Assert.Equal(blobs[i], value.ToArray());
                }
            }
            else
            {
                Assert.Null(d.Batch);
                Assert.Equal(1, d.Status.RowsDropped);
                Assert.Empty(d.Attachments);
            }
        }

        _subscriber.Unsubscribe(result.Subscription);
        _publisher.PublishRaw(topic, Bytes(RowKind.Good, Offered));
        Assert.Equal(Offered, _sink.Snapshot().Count);
    }

    /// <summary>
    /// Disposing the SubscriberArrow from inside its own per-row delivery is refused before
    /// anything is torn down (d10 L11, Findings (c)): the subscription stays live, the next
    /// row still arrives, and Dispose from outside the delivery then works. A declared guard:
    /// green before and after D10C-3.
    /// </summary>
    [Fact]
    public void DisposingInsideAPerRowDeliveryIsRefusedAndNothingIsTornDown()
    {
        TopicPath topic = Declared("disposeinside");
        Exception? refused = null;
        int calls = 0;
        SubscribeResult result = _subscriber.Subscribe(topic, (batch, attachments, status) =>
        {
            if (++calls == 1)
            {
                refused = Record.Exception(() => _subscriber.Dispose());
            }

            _sink.Handler(batch, attachments, status);
        });
        result.Schema.Dispose();

        Publish(topic, 1, "a");

        Assert.IsType<InvalidOperationException>(refused);
        Assert.True(result.Subscription.IsLive);

        Publish(topic, 2, "b");
        List<Delivery> seen = _sink.Snapshot();
        Assert.Equal(2, seen.Count);
        Assert.Equal(2, ((Int32Array)seen[1].Batch!.Column(0)).GetValue(0));
        Assert.Equal(0UL, _subscriber.AbsorbedCallbackFailures);

        _subscriber.Dispose();
        Assert.False(result.Subscription.IsLive);
    }

    /// <summary>
    /// A per-row dictionary column arrives as its value type (d10 D32, ruling :575 (5)): the
    /// per-row decode hands over the values, in order, and C# converts nothing. A different
    /// subject from the batched dictionary column (ADictionaryColumnKeepsItsNulls,
    /// BatchesAreWellFormed), which follows domain 7 from rung 7. A declared guard: green before
    /// and after D10C-3.
    /// </summary>
    [Fact]
    public void APerRowDictionaryColumnArrivesAsItsValueType()
    {
        using RecordBatch source = CodecFixtures.Dictionary();
        TopicPath topic = Topic("perrowdictionary");
        _publisher.CreateTopic(topic, source.Schema);
        _subscriber.Subscribe(topic, _sink.Handler).Schema.Dispose();

        using var codec = new FletcherCodec(source.Schema);
        using BoundRows rows = codec.Bind(source);
        _publisher.Publish(topic, rows);

        List<Delivery> seen = _sink.Snapshot();
        Assert.Equal(3, seen.Count);
        string[] expected = ["gamma", "alpha", "beta"];
        for (int i = 0; i < 3; i++)
        {
            Assert.Equal(1, seen[i].Rows);
            var category = Assert.IsType<StringArray>(seen[i].Batch!.Column(1));
            Assert.Equal(expected[i], category.GetString(0));
        }
    }

    /// <summary>
    /// A leg (mechanism, not the proof of a promise): a per-row handler that unsubscribes
    /// itself is served - no fault, nothing after, and a later Dispose is clean.
    /// </summary>
    /// <remarks>
    /// The inner Subscriber frees a self-cancelled subscription on the departing delivery's
    /// way out (TryFree's delivery branch), never on D-BIND-50's deferred branch, which only
    /// a sibling's cancel reaches (ReentrancyTests). The two counts bracket which branch freed;
    /// they do not observe the codec's release, which
    /// ReentrancyTests.TheReleaseRunsExactlyOnceWhereverTheSubscriptionIsFreed does.
    /// </remarks>
    [Fact]
    public void APerRowHandlerThatUnsubscribesItselfStopsCleanly()
    {
        TopicPath topic = Declared("perrowselfcancel");
        var inner = (Subscriber)typeof(SubscriberArrow)
            .GetField("_subscriber", BindingFlags.NonPublic | BindingFlags.Instance)!
            .GetValue(_subscriber)!;
        long byDelivery = inner.FreedByDeliveryCount;
        long deferred = inner.FreedDeferredCount;
        int calls = 0;
        Exception? failure = null;
        Subscription? self = null;
        SubscribeResult result = _subscriber.Subscribe(topic, (batch, _, _) =>
        {
            batch?.Dispose();
            calls++;
            failure = Record.Exception(() => _subscriber.Unsubscribe(self!));
        });
        self = result.Subscription;
        result.Schema.Dispose();

        Publish(topic, 1, "a");
        Publish(topic, 2, "b");

        Assert.Equal(1, calls);
        Assert.Null(failure);
        Assert.False(self.IsLive);
        Assert.Equal(0UL, _subscriber.AbsorbedCallbackFailures);
        Assert.Equal(1, inner.FreedByDeliveryCount - byDelivery);
        Assert.Equal(0, inner.FreedDeferredCount - deferred);

        Assert.Null(Record.Exception(() => _subscriber.Dispose()));
        Assert.Equal(1, calls);
    }

    // ── Batching ────────────────────────────────────────────────────────────
    //
    // D10C-7 (rung 7): on the server leg every batched subject is domain 7's, reached
    // through the server shim. Each window arrives on domain 7's batch thread, so a test
    // reads results after WAITING for delivery, never straight after Publish (ruling
    // :140, "must wait for delivery instead"; d10 L13). On the edge leg each subject is
    // the truth table (EdgeRefuses).

    // ── Subscribed before the topic is declared (D-BIND-146) ────────────────
    // The seam fixes a subscription's schema mode when Subscribe returns (provider.hpp,
    // spec §7 clause 1). On a schema-CARRYING transport (Fast DDS, inprocess with
    // schema_carriage=carried) the schema arrives with the data, so a subscriber that
    // subscribed first still decodes: conformance clause 1, SchemaBeforeDataAcrossHandoff,
    // and the first test below. The default inprocess loopback (as_declared) instead
    // answers a subscription made before the declaration with no schema, for its whole
    // life; SubscriberArrow then cannot decode, and every row is dropped and counted (the
    // second test). Found by the BIND-9a review, after the package README's batch snippet
    // received a null window over the default inprocess.

    [Fact]
    public void OnACarryingTransportASubscriberThatSubscribedFirstStillDecodes()
    {
        using PubSubProviderHandle carrying = ProviderRegistry.Create(
            ProviderSelector.Parse("inprocess"),
            new ProviderConfig { Document = System.Text.Encoding.UTF8.GetBytes("schema_carriage=carried") });
        using var publisher = new Publisher(carrying);
        using var subscriber = new SubscriberArrow(carrying);
        TopicPath topic = Topic("carriedfirst");
        void PublishThree()
        {
            publisher.CreateTopic(topic, TwoColumns);
            for (int i = 0; i < 3; i++)
            {
                using RecordBatch batch = Row(i, "late");
                using var codec = new FletcherCodec(TwoColumns);
                using BoundRows rows = codec.Bind(batch);
                publisher.Publish(topic, rows, 0, null);
            }
        }

        if (!ServerLeg)
        {
            EdgeRefuses(topic, handler => subscriber.SubscribeBatched(topic, handler, Options(3, Long)), PublishThree);
            return;
        }

        subscriber.SubscribeBatched(topic, _sink.Handler, Options(3, Long)).Schema.Dispose();
        PublishThree();

        Assert.True(_sink.WaitFor(1, Await), "no batch arrived");
        Delivery only = Assert.Single(_sink.Snapshot());
        Assert.Equal(3, only.Rows);
        Assert.Equal(0, only.Status.RowsDropped);
        Assert.Equal(BatchReason.RowLimit, only.Status.Reason);
    }

    [Fact]
    public void OnTheDefaultInProcessLoopbackASubscriberThatSubscribedFirstDropsAndCountsEveryRow()
    {
        TopicPath topic = Topic("declaredlater");
        if (!ServerLeg)
        {
            EdgeRefuses(topic, handler => _subscriber.SubscribeBatched(topic, handler, Options(3, Long)), () =>
            {
                _publisher.CreateTopic(topic, TwoColumns);
                Publish(topic, 1, "late");
            });
            return;
        }

        SubscribeResult result = _subscriber.SubscribeBatched(topic, _sink.Handler, Options(3, Long));
        result.Schema.Dispose();
        _publisher.CreateTopic(topic, TwoColumns);

        for (int i = 0; i < 3; i++)
        {
            Publish(topic, i, "late");
        }

        Assert.Empty(_sink.Snapshot());
        result.Subscription.Dispose();

        Delivery closing = Assert.Single(_sink.Snapshot());
        Assert.Null(closing.Batch);
        Assert.Equal(3, closing.Status.RowsDropped);
        Assert.Equal(BatchReason.Closing, closing.Status.Reason);
    }

    /// <summary>Mirrors SubscriberArrowBatchTest.FlushesAtRowLimit.</summary>
    [Fact]
    public void FlushesAtTheRowLimit()
    {
        TopicPath topic = Declared("rowlimit");
        if (!ServerLeg)
        {
            EdgeRefuses(topic, handler => _subscriber.SubscribeBatched(topic, handler, Options(3, Long)));
            return;
        }

        _subscriber.SubscribeBatched(topic, _sink.Handler, Options(3, Long)).Schema.Dispose();

        for (int i = 0; i < 3; i++)
        {
            Publish(topic, i, "n");
        }

        Assert.True(_sink.WaitFor(1, Await), "no batch at the row limit");
        Delivery only = Assert.Single(_sink.Snapshot());
        Assert.Equal(3, only.Rows);
        Assert.Equal(3, only.Attachments.Count);
        Assert.Equal(0, only.Status.RowsDropped);
        Assert.Equal(BatchReason.RowLimit, only.Status.Reason);
    }

    /// <summary>Mirrors SubscriberArrowBatchTest.FlushesAtTimeout.</summary>
    [Fact]
    public void FlushesAtTheTimeout()
    {
        TopicPath topic = Declared("timeout");
        if (!ServerLeg)
        {
            EdgeRefuses(topic, handler => _subscriber.SubscribeBatched(topic, handler, Options(100_000, TimeSpan.FromMilliseconds(100))));
            return;
        }

        _subscriber.SubscribeBatched(topic, _sink.Handler, Options(100_000, TimeSpan.FromMilliseconds(100))).Schema.Dispose();

        Publish(topic, 1, "a");
        Publish(topic, 2, "b");

        Assert.True(_sink.WaitFor(1, TimeSpan.FromSeconds(5)), "no timeout flush within 5 s of a 100 ms window");
        Delivery only = Assert.Single(_sink.Snapshot());
        Assert.Equal(2, only.Rows);
        Assert.Equal(0, only.Status.RowsDropped);
        Assert.Equal(BatchReason.Timeout, only.Status.Reason);
    }

    /// <summary>
    /// The control for every one-sided timing arm in this file (ladder ground rules): a 10 ms
    /// timeout gives a <see cref="BatchReason.Timeout"/> batch within 5 s. A "nothing delivered
    /// over a fixed window" arm is only evidence while this one is green. (The ladder's 2 s, widened
    /// so a loaded shared runner does not fail it; review 4b.)
    /// </summary>
    [Fact]
    public void ATenMillisecondTimeoutClosesTheWindowWithinFiveSeconds()
    {
        TopicPath topic = Declared("tenms");
        if (!ServerLeg)
        {
            EdgeRefuses(topic, handler => _subscriber.SubscribeBatched(topic, handler, Options(100_000, TimeSpan.FromMilliseconds(10))));
            return;
        }

        _subscriber.SubscribeBatched(topic, _sink.Handler, Options(100_000, TimeSpan.FromMilliseconds(10))).Schema.Dispose();

        Publish(topic, 1, "a");

        Assert.True(_sink.WaitFor(1, TimeSpan.FromSeconds(5)), "no Timeout batch within 5 s of a 10 ms window");
        Delivery first = _sink.Snapshot()[0];
        Assert.Equal(BatchReason.Timeout, first.Status.Reason);
        Assert.Equal(1, first.Rows);
    }

    /// <summary>Mirrors SubscriberArrowBatchTest.ClosingFlushOnUnsubscribe.</summary>
    [Fact]
    public void UnsubscribingFlushesThePartialWindow()
    {
        TopicPath topic = Declared("closing");
        if (!ServerLeg)
        {
            EdgeRefuses(topic, handler => _subscriber.SubscribeBatched(topic, handler, Options(100_000, Long)));
            return;
        }

        SubscribeResult result = _subscriber.SubscribeBatched(topic, _sink.Handler, Options(100_000, Long));
        result.Schema.Dispose();

        Publish(topic, 7, "x");
        Publish(topic, 8, "y");
        Assert.Empty(_sink.Snapshot());

        result.Subscription.Dispose();

        Delivery only = Assert.Single(_sink.Snapshot());
        Assert.Equal(2, only.Rows);
        Assert.Equal(BatchReason.Closing, only.Status.Reason);
    }

    /// <summary>
    /// D10C-7 (d7 L6, L9; ruling :484; design P3): a close with nothing pending still ends with
    /// exactly one Closing delivery - empty - and nothing after it, on each way a C# caller closes:
    /// <c>Unsubscribe</c>, the subscription's <c>Dispose</c>, and the <c>SubscriberArrow</c>'s
    /// <c>Dispose</c>.
    /// </summary>
    /// <remarks>
    /// "Empty" is d7 L6's either-or, "zero-row, or null if there is no decoder": domain 7 decides
    /// its decoder from the first row that carries a schema (d7 L8, L13), so a subscription that
    /// never saw a row closes with a null batch and RowsDropped = 0. Either form is accepted here;
    /// which one is domain 7's to choose.
    /// </remarks>
    [Theory]
    [InlineData("unsubscribe")]
    [InlineData("subscription")]
    [InlineData("subscriber")]
    public void AnIdleCloseDeliversOneEmptyClosing(string closer)
    {
        TopicPath topic = Declared("idleclose" + closer);
        if (!ServerLeg)
        {
            EdgeRefuses(topic, handler => _subscriber.SubscribeBatched(topic, handler, Options(100_000, Long)));
            return;
        }

        SubscribeResult result = _subscriber.SubscribeBatched(topic, _sink.Handler, Options(100_000, Long));
        result.Schema.Dispose();
        Assert.False(_sink.WaitFor(1, Quiet), "an idle window delivered before the close");

        switch (closer)
        {
            case "unsubscribe":
                _subscriber.Unsubscribe(result.Subscription);
                break;
            case "subscription":
                result.Subscription.Dispose();
                break;
            default:
                _subscriber.Dispose();
                break;
        }

        Delivery closing = Assert.Single(_sink.Snapshot());
        Assert.Equal(BatchReason.Closing, closing.Status.Reason);
        Assert.True(closing.Batch is null || closing.Rows == 0, $"the idle Closing carried {closing.Rows} row(s)");
        Assert.Empty(closing.Attachments);
        Assert.Equal(0, closing.Status.RowsDropped);

        Publish(topic, 1, "after");
        Assert.False(_sink.WaitFor(2, Quiet), "a delivery after the Closing");
    }

    /// <summary>Mirrors SubscriberArrowBatchTest.AttachmentsAlignWithRows.</summary>
    [Fact]
    public void AttachmentsAlignWithTheirRows()
    {
        TopicPath topic = Declared("align");
        if (!ServerLeg)
        {
            EdgeRefuses(topic, handler => _subscriber.SubscribeBatched(topic, handler, Options(2, Long)));
            return;
        }

        _subscriber.SubscribeBatched(topic, _sink.Handler, Options(2, Long)).Schema.Dispose();

        using var sent = new AttachmentsBuilder();
        sent.Set("img", [9]);
        Publish(topic, 1, "a", sent);
        Publish(topic, 2, "b");

        Assert.True(_sink.WaitFor(1, Await), "no batch at the row limit");
        Delivery only = Assert.Single(_sink.Snapshot());
        Assert.Equal(2, only.Attachments.Count);
        Assert.True(only.Attachments[0].TryFind("img"u8, out _));
        Assert.Equal(0, only.Attachments[1].Count);
    }

    /// <summary>Mirrors SubscriberArrowBatchTest.DroppedRowReportedAndAttachmentDiscarded.</summary>
    [Fact]
    public void ADroppedRowIsReportedAndItsAttachmentDiscarded()
    {
        TopicPath topic = Declared("dropped");
        if (!ServerLeg)
        {
            EdgeRefuses(topic, handler => _subscriber.SubscribeBatched(topic, handler, Options(100_000, Long)));
            return;
        }

        SubscribeResult result = _subscriber.SubscribeBatched(topic, _sink.Handler, Options(100_000, Long));
        result.Schema.Dispose();

        using var good = new AttachmentsBuilder();
        good.Set("good", [1]);
        using var orphan = new AttachmentsBuilder();
        orphan.Set("orphan", [2]);

        Publish(topic, 1, "good", good);
        _publisher.PublishRaw(topic, [0x00], orphan);
        result.Subscription.Dispose();

        Delivery only = Assert.Single(_sink.Snapshot());
        Assert.Equal(1, only.Rows);
        AttachmentsBuilder kept = Assert.Single(only.Attachments);
        Assert.True(kept.TryFind("good"u8, out _));
        Assert.Equal(1, only.Status.RowsDropped);
    }

    /// <summary>Mirrors SubscriberArrowBatchTest.OnlyDroppedRowsStillDeliversEmptyBatch.</summary>
    [Fact]
    public void OnlyDroppedRowsStillDeliverAnEmptyBatch()
    {
        TopicPath topic = Declared("onlydropped");
        if (!ServerLeg)
        {
            EdgeRefuses(topic, handler => _subscriber.SubscribeBatched(topic, handler, Options(100_000, TimeSpan.FromMilliseconds(100))));
            return;
        }

        _subscriber.SubscribeBatched(topic, _sink.Handler, Options(100_000, TimeSpan.FromMilliseconds(100))).Schema.Dispose();

        _publisher.PublishRaw(topic, [0x00]);

        Assert.True(_sink.WaitFor(1, TimeSpan.FromSeconds(5)), "no report of the dropped row");
        Delivery only = Assert.Single(_sink.Snapshot());
        Assert.NotNull(only.Batch);
        Assert.Equal(0, only.Rows);
        Assert.Empty(only.Attachments);
        Assert.Equal(1, only.Status.RowsDropped);
        Assert.Equal(BatchReason.Timeout, only.Status.Reason);
    }

    /// <summary>The value at <paramref name="row"/> of a dictionary column, through its indices; null when the row is.</summary>
    private static string? ValueAt(DictionaryArray column, int row) =>
        column.IsNull(row)
            ? null
            : ((StringArray)column.Dictionary).GetString(checked((int)Convert.ToInt64(
                ((IArrowArray)column.Indices) switch
                {
                    Int8Array i8 => (object?)i8.GetValue(row),
                    Int16Array i16 => i16.GetValue(row),
                    Int32Array i32 => i32.GetValue(row),
                    Int64Array i64 => i64.GetValue(row),
                    UInt8Array u8 => u8.GetValue(row),
                    UInt16Array u16 => u16.GetValue(row),
                    UInt32Array u32 => u32.GetValue(row),
                    UInt64Array u64 => u64.GetValue(row),
                    _ => throw new InvalidOperationException($"a dictionary index of type {column.Indices.Data.DataType.Name}"),
                },
                System.Globalization.CultureInfo.InvariantCulture)));

    /// <summary>Mirrors SubscriberArrowBatchTest.DictionaryColumnPreservesNulls.</summary>
    /// <remarks>
    /// D10C-7 (audit 0a T21; ruling :551 (2); d10 D24): a batched dictionary column arrives
    /// re-folded, as domain 7 delivers it - a <see cref="DictionaryArray"/> - with its null and
    /// its values kept. C# converts nothing; the index width is domain 7's to choose.
    /// </remarks>
    [Fact]
    public void ADictionaryColumnKeepsItsNulls()
    {
        var type = new DictionaryType(Int32Type.Default, StringType.Default, ordered: false);
        var schema = new Schema([new Field("category", type, nullable: true)], metadata: null);
        StringArray values = new StringArray.Builder().Append("x").Build();
        Int32Array indices = new Int32Array.Builder().Append(0).AppendNull().Append(0).Build();
        using RecordBatch source = new(schema, [new DictionaryArray(type, indices, values)], length: 3);

        TopicPath topic = Topic("dictnulls");
        _publisher.CreateTopic(topic, schema);
        using var codec = new FletcherCodec(schema);
        using BoundRows rows = codec.Bind(source);
        if (!ServerLeg)
        {
            EdgeRefuses(topic, handler => _subscriber.SubscribeBatched(topic, handler, Options(3, Long)), () => _publisher.Publish(topic, rows));
            return;
        }

        _subscriber.SubscribeBatched(topic, _sink.Handler, Options(3, Long)).Schema.Dispose();
        _publisher.Publish(topic, rows);

        Assert.True(_sink.WaitFor(1, Await), "no batch at the row limit");
        var category = Assert.IsType<DictionaryArray>(Assert.Single(_sink.Snapshot()).Batch!.Column(0));
        Assert.Equal(3, category.Length);
        Assert.Equal(1, category.NullCount);
        Assert.Equal("x", ValueAt(category, 0));
        Assert.True(category.IsNull(1));
        Assert.Equal("x", ValueAt(category, 2));
    }

    /// <summary>Mirrors SubscriberArrowBatchTest.CorruptRowIsCountedDroppedAndBatchStaysAligned.</summary>
    /// <remarks>
    /// C++'s case exactly (review T-D5): a list column, and a corrupt row made from
    /// row A's own encoding truncated by 3 bytes, so the fixed 4-byte int32 underruns
    /// on read - a row that starts well and fails part way, not a one-byte stub. With
    /// max_rows = 2 the corrupt row never counts toward the limit, and A and B come back
    /// with THEIR attachments.
    /// </remarks>
    [Fact]
    public void ACorruptRowIsDroppedAndTheBatchStaysAligned()
    {
        var schema = new Schema(
        [
            new Field("tags", new ListType(StringType.Default), nullable: true),
            new Field("x", Int32Type.Default, nullable: true),
        ], metadata: null);
        TopicPath topic = Topic("corrupt");
        _publisher.CreateTopic(topic, schema);

        using var codec = new FletcherCodec(schema);
        RecordBatch RowOf(int x, params string[] items)
        {
            var tags = new ListArray.Builder(StringType.Default);
            tags.Append();
            foreach (string item in items)
            {
                ((StringArray.Builder)tags.ValueBuilder).Append(item);
            }

            return new RecordBatch(schema, [tags.Build(), new Int32Array.Builder().Append(x).Build()], length: 1);
        }

        using RecordBatch rowA = RowOf(1, "a", "b");
        using RecordBatch rowB = RowOf(2, "c");
        using BoundRows boundA = codec.Bind(rowA);
        using BoundRows boundB = codec.Bind(rowB);
        if (!ServerLeg)
        {
            EdgeRefuses(topic, handler => _subscriber.SubscribeBatched(topic, handler, Options(2, Long)), () => _publisher.Publish(topic, boundA, 0));
            return;
        }

        _subscriber.SubscribeBatched(topic, _sink.Handler, Options(2, Long)).Schema.Dispose();
        var encodedA = new System.Buffers.ArrayBufferWriter<byte>();
        codec.Encode(boundA, 0, encodedA);
        byte[] truncated = encodedA.WrittenSpan[..^3].ToArray();

        using var a = new AttachmentsBuilder();
        a.Set("blob", [0xAA]);
        using var b = new AttachmentsBuilder();
        b.Set("blob", [0xBB]);

        _publisher.Publish(topic, boundA, 0, a);
        _publisher.PublishRaw(topic, truncated);
        Assert.Empty(_sink.Snapshot());
        _publisher.Publish(topic, boundB, 0, b);  // reaches max_rows

        Assert.True(_sink.WaitFor(1, Await), "no batch at the row limit");
        Delivery only = Assert.Single(_sink.Snapshot());
        Assert.Equal(2, only.Rows);
        Assert.Equal(1, only.Status.RowsDropped);
        Assert.Equal(BatchReason.RowLimit, only.Status.Reason);
        Assert.Equal(1, ((Int32Array)only.Batch!.Column(1)).GetValue(0));
        Assert.Equal(2, ((Int32Array)only.Batch!.Column(1)).GetValue(1));
        Assert.True(only.Attachments[0].TryFind("blob"u8, out ReadOnlySpan<byte> gotA));
        Assert.True(only.Attachments[1].TryFind("blob"u8, out ReadOnlySpan<byte> gotB));
        Assert.Equal(new byte[] { 0xAA }, gotA.ToArray());
        Assert.Equal(new byte[] { 0xBB }, gotB.ToArray());
    }

    /// <summary>Mirrors SubscriberArrowBatchTest.FixedSizeListWithNamedItemArrivesNonNull.</summary>
    [Fact]
    public void AFixedSizeListArrivesNonNull()
    {
        var type = new FixedSizeListType(new Field("element", FloatType.Default, nullable: true), listSize: 3);
        var schema = new Schema([new Field("vector", type, nullable: true)], metadata: null);
        var list = new FixedSizeListArray(type, 1,
            new FloatArray.Builder().Append(1f).Append(2f).Append(3f).Build(), ArrowBuffer.Empty, nullCount: 0);
        using RecordBatch source = new(schema, [list], length: 1);

        TopicPath topic = Topic("fixedsize");
        _publisher.CreateTopic(topic, schema);
        using var codec = new FletcherCodec(schema);
        using BoundRows rows = codec.Bind(source);
        if (!ServerLeg)
        {
            EdgeRefuses(topic, handler => _subscriber.SubscribeBatched(topic, handler, Options(1, Long)), () => _publisher.Publish(topic, rows, 0));
            return;
        }

        _subscriber.SubscribeBatched(topic, _sink.Handler, Options(1, Long)).Schema.Dispose();
        _publisher.Publish(topic, rows, 0);

        Assert.True(_sink.WaitFor(1, Await), "no batch at the row limit");
        var column = Assert.IsType<FixedSizeListArray>(Assert.Single(_sink.Snapshot()).Batch!.Column(0));
        Assert.True(column.IsValid(0), "the list itself arrived null");
        var values = (FloatArray)column.Values;
        Assert.Equal([1f, 2f, 3f], [values.GetValue(0)!.Value, values.GetValue(1)!.Value, values.GetValue(2)!.Value]);
    }

    /// <summary>Mirrors SubscriberArrowBatchTest.NestedDictionarySchemaReportsEveryRowDropped.</summary>
    /// <remarks>
    /// The property: a schema the codec cannot open drops EVERY row for the life of
    /// the subscription, reported with a NULL batch rather than silently. C++ shows
    /// it on a nested dictionary; here it is shown on a dictionary whose value type is a
    /// struct, which domain 7's decoder refuses too (d7 L7, L8).
    /// </remarks>
    [Fact]
    public void ASchemaTheCodecCannotOpenDropsEveryRow()
    {
        var nested = new StructType([new Field("x", Int32Type.Default, nullable: true)]);
        var schema = new Schema([new Field("category", new DictionaryType(Int32Type.Default, nested, ordered: false), nullable: true)], metadata: null);
        TopicPath topic = Topic("undecodable");
        _publisher.CreateTopic(topic, schema);
        if (!ServerLeg)
        {
            EdgeRefuses(topic, handler => _subscriber.SubscribeBatched(topic, handler, Options(100_000, Long)), () => _publisher.PublishRaw(topic, [0x00]));
            return;
        }

        SubscribeResult result = _subscriber.SubscribeBatched(topic, _sink.Handler, Options(100_000, Long));
        result.Schema.Dispose();

        for (int i = 0; i < 3; i++)
        {
            _publisher.PublishRaw(topic, [0x00]);
        }

        result.Subscription.Dispose();

        Delivery only = Assert.Single(_sink.Snapshot());
        Assert.Null(only.Batch);
        Assert.Equal(3, only.Status.RowsDropped);
        Assert.Empty(only.Attachments);
    }

    /// <summary>Mirrors SubscriberArrowBatchTest.UnsubscribeFromCallbackDuringRowLimitFlushStopsDeliveryCleanly.</summary>
    /// <remarks>
    /// D10C-7 (audit 0a T25; ruling :484; d7 L9): an Unsubscribe from inside the handler returns
    /// at once, and the subscription still ends with exactly one Closing - empty, since the
    /// window was just delivered - after the handler returns; then nothing.
    /// </remarks>
    [Fact]
    public void UnsubscribingFromTheHandlerDuringARowLimitFlushStopsDelivery()
    {
        TopicPath topic = Declared("selfcancel");
        if (!ServerLeg)
        {
            EdgeRefuses(topic, handler => _subscriber.SubscribeBatched(topic, handler, Options(1, Long)));
            return;
        }

        int calls = 0;
        Subscription? self = null;
        SubscribeResult result = _subscriber.SubscribeBatched(topic, (batch, attachments, status) =>
        {
            _sink.Handler(batch, attachments, status);
            if (Interlocked.Increment(ref calls) == 1)
            {
                _subscriber.Unsubscribe(self!);
            }
        }, Options(1, Long));
        self = result.Subscription;
        result.Schema.Dispose();

        Publish(topic, 1, "a");

        Assert.True(_sink.WaitFor(2, Await), "the in-handler Unsubscribe was not followed by a Closing delivery");
        List<Delivery> seen = _sink.Snapshot();
        Assert.Equal(BatchReason.RowLimit, seen[0].Status.Reason);
        Assert.Equal(1, seen[0].Rows);
        Assert.Equal(BatchReason.Closing, seen[1].Status.Reason);
        Assert.Equal(0, seen[1].Rows);
        Assert.Equal(0, seen[1].Status.RowsDropped);

        Publish(topic, 2, "b");
        Publish(topic, 3, "c");
        Assert.False(_sink.WaitFor(3, Quiet), "a delivery after the Closing");
        Assert.Equal(0UL, _subscriber.AbsorbedCallbackFailures);
    }

    /// <summary>Mirrors SubscriberArrowBatchTest.UnsubscribeFromInsideATimeoutFlushIsSafe.</summary>
    /// <remarks>
    /// D10C-7 (audit 0a T26): the handler also receives the subscription's Closing (ruling :484)
    /// and ignores it, rather than unsubscribing a closed subscription again.
    /// </remarks>
    [Fact]
    public void UnsubscribingFromInsideATimeoutFlushIsSafe()
    {
        TopicPath topic = Declared("timercancel");
        if (!ServerLeg)
        {
            EdgeRefuses(topic, handler => _subscriber.SubscribeBatched(topic, handler, Options(100, TimeSpan.FromMilliseconds(20))));
            return;
        }

        using var done = new ManualResetEventSlim(false);
        Subscription? self = null;
        Exception? failure = null;
        SubscribeResult result = _subscriber.SubscribeBatched(topic, (batch, _, status) =>
        {
            batch?.Dispose();
            if (status.Reason == BatchReason.Closing)
            {
                return;
            }

            try
            {
                _subscriber.Unsubscribe(self!);
            }
            catch (Exception e)
            {
                failure = e;
            }

            done.Set();
        }, Options(100, TimeSpan.FromMilliseconds(20)));
        self = result.Subscription;
        result.Schema.Dispose();

        Publish(topic, 1, "a");

        Assert.True(done.Wait(TimeSpan.FromSeconds(5)), "the timeout flush never ran");
        Assert.Null(failure);
        Assert.True(SpinWait.SpinUntil(() => !self.IsLive, Await), "the subscription is still live");
    }

    // ── Dispose from inside a batch handler (D10C-9, rung 9; d10 L11; ruling :174 (1)) ──

    /// <summary>
    /// Holds the first window's handler while <paramref name="queued"/> more rows are published behind
    /// it, then has it run <paramref name="close"/> and raise a flag when that returns. The flag is
    /// waited for at most <paramref name="limit"/>, so a close that waits for its own Closing (which the
    /// blocked batch thread could never deliver) is a failed assertion, not a hang; when it did return,
    /// the harness waits for <paramref name="deliveries"/> deliveries.
    /// </summary>
    /// <remarks>
    /// The handler runs on domain 7's batch thread, where no delivery frame is open, so the
    /// close is the in-callback close the shim serves, not the row-delivery refusal. The events are
    /// not disposed: a control's slow handler outlives this frame.
    /// </remarks>
    private (bool Returned, List<Delivery> Seen) CloseInsideTheFirstHandler(
        string name, BatchOptions options, int queued, Action<SubscriberArrow> close, TimeSpan limit, int deliveries)
    {
        TopicPath topic = Declared(name);
        var closer = new SubscriberArrow(_provider);
        try
        {
            var entered = new ManualResetEventSlim(false);
            var release = new ManualResetEventSlim(false);
            var returned = new ManualResetEventSlim(false);
            int calls = 0;
            closer.SubscribeBatched(topic, (batch, attachments, status) =>
            {
                _sink.Handler(batch, attachments, status);
                if (Interlocked.Increment(ref calls) == 1)
                {
                    entered.Set();
                    release.Wait(Await);
                    close(closer);
                    returned.Set();
                }
            }, options).Schema.Dispose();

            Publish(topic, 1, "a");
            Assert.True(entered.Wait(Await), "the first window never arrived");
            for (int x = 2; x < 2 + queued; x++)
            {
                Publish(topic, x, "queued");
            }

            release.Set();
            bool done = returned.Wait(limit);
            if (done)
            {
                _sink.WaitFor(deliveries, Await);
            }

            return (done, _sink.Snapshot());
        }
        finally
        {
            closer.Dispose();
        }
    }

    /// <summary>
    /// A batch handler that disposes its own SubscriberArrow is served (d10 L11; ruling :174 (1); d7 L9;
    /// V14's row-limit case and its timeout twin): Dispose returns inside the handler, the queued
    /// row-limit windows still arrive, then exactly one Closing, then nothing. A declared guard:
    /// green before and after D10C-9.
    /// </summary>
    /// <remarks>
    /// The timeout case holds a 1-row timeout window (MaxRows = 2, so the first window can be one),
    /// queues 4 rows as two full windows. Control: TheDisposeReturnedFlagCanFail. Edge leg: the BC-10 truth table.
    /// </remarks>
    [Theory]
    [InlineData("rowlimit")]
    [InlineData("timeout")]
    public void DisposingTheSubscriberInsideABatchHandlerIsServed(string window)
    {
        bool timeout = window == "timeout";
        // 200 ms, not less: rows 2-5 are published while the first handler is held, and must all arrive
        // inside one window's deadline of the first flush for the windows to be exactly [1], [2,3], [4,5].
        BatchOptions options = timeout ? Options(2, TimeSpan.FromMilliseconds(200)) : Options(1, Long);
        if (!ServerLeg)
        {
            TopicPath edge = Declared("disposebatch" + window);
            EdgeRefuses(edge, handler => _subscriber.SubscribeBatched(edge, handler, options));
            return;
        }

        (bool returned, List<Delivery> seen) = CloseInsideTheFirstHandler(
            "disposebatch" + window, options, queued: timeout ? 4 : 2, closer => closer.Dispose(), Await, deliveries: 4);

        Assert.True(returned, "Dispose inside a batch handler did not return: it waited for its own Closing");
        Assert.Equal(
            [timeout ? BatchReason.Timeout : BatchReason.RowLimit, BatchReason.RowLimit, BatchReason.RowLimit, BatchReason.Closing],
            seen.Select(d => d.Status.Reason).ToArray());
        int[] firstX = timeout ? [1, 2, 4] : [1, 2, 3];
        for (int i = 0; i < 3; i++)
        {
            Assert.Equal(firstX[i], ((Int32Array)seen[i].Batch!.Column(0)).GetValue(0));
        }

        Assert.Equal(1, seen[0].Rows);
        Assert.Equal(timeout ? 2 : 1, seen[1].Rows);
        Assert.Equal(timeout ? 2 : 1, seen[2].Rows);
        Assert.True(seen[3].Batch is null || seen[3].Rows == 0, "the Closing carried rows");

        Publish(Topic("disposebatch" + window), 99, "after");
        Assert.False(_sink.WaitFor(5, Quiet), "a delivery after the Closing");
    }

    /// <summary>
    /// Control for DisposingTheSubscriberInsideABatchHandlerIsServed: the same harness, with a stand-in
    /// that does not return within the limit (a close waiting for its own Closing), reports
    /// Returned = false, so the guard's assertion fails instead of the test hanging.
    /// </summary>
    [Fact]
    public void TheDisposeReturnedFlagCanFail()
    {
        if (!ServerLeg)
        {
            TopicPath edge = Declared("disposeflag");
            EdgeRefuses(edge, handler => _subscriber.SubscribeBatched(edge, handler, Options(1, Long)));
            return;
        }

        (bool returned, _) = CloseInsideTheFirstHandler(
            "disposeflag", Options(1, Long), queued: 2, _ => Thread.Sleep(1500), TimeSpan.FromMilliseconds(200), deliveries: 4);

        Assert.False(returned, "the flag said a close that outlived the limit had returned");
    }

    // ── The framed decode (D-BIND-68) ───────────────────────────────────────

    private static byte[] Encoded(int x, string name)
    {
        using RecordBatch batch = Row(x, name);
        using var codec = new FletcherCodec(TwoColumns);
        using BoundRows rows = codec.Bind(batch);
        var buffer = new System.Buffers.ArrayBufferWriter<byte>();
        codec.Encode(rows, 0, buffer);
        return buffer.WrittenSpan.ToArray();
    }

    /// <summary>
    /// A corrupt row does not count toward MaxRows, as in C++'s
    /// CorruptRowIsCountedDroppedAndBatchStaysAligned (max_rows = 2): the RowLimit
    /// batch holds two GOOD rows and reports the one dropped (review B3).
    /// </summary>
    [Fact]
    public void ACorruptRowDoesNotCountTowardMaxRows()
    {
        TopicPath topic = Declared("corruptlimit");
        if (!ServerLeg)
        {
            EdgeRefuses(topic, handler => _subscriber.SubscribeBatched(topic, handler, Options(2, Long)));
            return;
        }

        _subscriber.SubscribeBatched(topic, _sink.Handler, Options(2, Long)).Schema.Dispose();

        using var a = new AttachmentsBuilder();
        a.Set("row", "A"u8);
        using var bad = new AttachmentsBuilder();
        bad.Set("row", "X"u8);
        using var b = new AttachmentsBuilder();
        b.Set("row", "B"u8);

        _publisher.PublishRaw(topic, Encoded(1, "first"), a);
        _publisher.PublishRaw(topic, [0x00], bad);
        Assert.Empty(_sink.Snapshot());  // one good row: the window waits for another
        _publisher.PublishRaw(topic, Encoded(2, "second"), b);

        Assert.True(_sink.WaitFor(1, Await), "no batch at the row limit");
        Delivery only = Assert.Single(_sink.Snapshot());
        Assert.Equal(BatchReason.RowLimit, only.Status.Reason);
        Assert.Equal(2, only.Rows);
        Assert.Equal(1, only.Status.RowsDropped);
        Assert.Equal("first", ((StringArray)only.Batch!.Column(1)).GetString(0));
        Assert.Equal("second", ((StringArray)only.Batch!.Column(1)).GetString(1));
        Assert.True(only.Attachments[0].TryFind("row"u8, out ReadOnlySpan<byte> first));
        Assert.True(only.Attachments[1].TryFind("row"u8, out ReadOnlySpan<byte> second));
        Assert.Equal("A"u8.ToArray(), first.ToArray());
        Assert.Equal("B"u8.ToArray(), second.ToArray());
    }

    /// <summary>
    /// Two valid rows split across two messages at a non-boundary: each message is
    /// malformed alone and the two are valid together. C++ decodes per message and
    /// drops both; so does the framed decode, where the whole-window decode paired
    /// the rows with the wrong messages (review B4).
    /// </summary>
    [Fact]
    public void RowsSplitAcrossMessagesAreDroppedNotMisaligned()
    {
        TopicPath topic = Declared("split");
        if (!ServerLeg)
        {
            EdgeRefuses(topic, handler => _subscriber.SubscribeBatched(topic, handler, Options(100, Long)));
            return;
        }

        SubscribeResult result = _subscriber.SubscribeBatched(topic, _sink.Handler, Options(100, Long));
        result.Schema.Dispose();

        byte[] joined = [.. Encoded(1, "aaaa"), .. Encoded(2, "bbbb")];
        int cut = Encoded(1, "aaaa").Length + 3;
        _publisher.PublishRaw(topic, joined.AsSpan(0, cut));
        _publisher.PublishRaw(topic, joined.AsSpan(cut));
        result.Subscription.Dispose();

        Delivery only = Assert.Single(_sink.Snapshot());
        Assert.Equal(0, only.Rows);
        Assert.Equal(2, only.Status.RowsDropped);
        Assert.Empty(only.Attachments);
    }

    // ── Arguments a caller can get wrong (review B8, B9) ────────────────────

    /// <summary>The review's B9 reproduction on this tier: refused, and the owner's #1 keeps delivering.</summary>
    [Fact]
    public void ASubscriptionFromAnotherSubscriberArrowIsRefused()
    {
        TopicPath mine = Declared("ownermine");
        TopicPath theirs = Declared("ownertheirs");
        using var other = new SubscriberArrow(_provider);
        SubscribeResult own = _subscriber.Subscribe(mine, _sink.Handler);
        SubscribeResult foreign = other.Subscribe(theirs, (batch, _, _) => batch?.Dispose());
        own.Schema.Dispose();
        foreign.Schema.Dispose();
        Assert.Equal(own.Subscription.Id, foreign.Subscription.Id);

        Assert.Throws<ArgumentException>(() => _subscriber.Unsubscribe(foreign.Subscription));

        Publish(mine, 1, "still");
        Assert.Single(_sink.Snapshot());
        Assert.True(own.Subscription.IsLive);
    }

    /// <summary>A very long finite timeout never fires early (d7 L5): the window waits, and closes on Unsubscribe.</summary>
    /// <remarks>
    /// Re-anchored at D10C-9 (audit 0a T31; ruling :536 (3), "the 49.7-day cap goes"): no limit exists to
    /// sit at, so the two long values are 100 years and TimeSpan.MaxValue, each used as given.
    /// Its edge leg is the truth table from rung 7.
    /// </remarks>
    [Theory]
    [InlineData("100years")]
    [InlineData("TimeSpan.MaxValue")]
    public void AWindowWithAVeryLongTimeoutWaits(string which)
    {
        TimeSpan timeout = which == "100years" ? TimeSpan.FromDays(36525) : TimeSpan.MaxValue;
        TopicPath topic = Declared("longtimeout" + which.Length);
        if (!ServerLeg)
        {
            EdgeRefuses(topic, handler => _subscriber.SubscribeBatched(topic, handler, Options(100, timeout)));
            return;
        }

        SubscribeResult result = _subscriber.SubscribeBatched(topic, _sink.Handler, Options(100, timeout));
        result.Schema.Dispose();

        Publish(topic, 1, "a");
        Thread.Sleep(200);
        Assert.Empty(_sink.Snapshot());

        result.Subscription.Dispose();
        Assert.Equal(BatchReason.Closing, Assert.Single(_sink.Snapshot()).Status.Reason);
    }

    // ── Every row arrives or is counted (D-BIND-66) ─────────────────────────

    /// <summary>
    /// An EMPTY attachment key, which C++ allows, crosses: the row arrives with it.
    /// The intake copied through the public Set, which refuses one, and every such
    /// row was lost uncounted (BIND-5 review B6).
    /// </summary>
    [Fact]
    public void ARowWithAnEmptyAttachmentKeyArrives()
    {
        TopicPath topic = Declared("emptykey");
        _subscriber.Subscribe(topic, _sink.Handler).Schema.Dispose();

        using var sent = new AttachmentsBuilder();
        sent.AppendDelivered(ReadOnlySpan<byte>.Empty, [7, 8]);
        Publish(topic, 1, "a", sent);

        Delivery only = Assert.Single(_sink.Snapshot());
        Assert.Equal(1, only.Rows);
        Assert.Equal(0, only.Status.RowsDropped);
        Assert.True(Assert.Single(only.Attachments).TryFind(ReadOnlySpan<byte>.Empty, out ReadOnlySpan<byte> value));
        Assert.Equal(new byte[] { 7, 8 }, value.ToArray());
        Assert.Equal(0UL, _subscriber.AbsorbedCallbackFailures);
    }

    /// <summary>
    /// The batched twin of ARowWithAnEmptyAttachmentKeyArrives (D10C-7; audit 0a; design D3 step 2):
    /// a row whose attachment has an EMPTY key reaches a batched handler with that attachment,
    /// copied by the binding's one attachment-copy function, and nothing is dropped or absorbed.
    /// </summary>
    [Fact]
    public void ABatchedRowWithAnEmptyAttachmentKeyArrives()
    {
        TopicPath topic = Declared("emptykeybatched");
        if (!ServerLeg)
        {
            EdgeRefuses(topic, handler => _subscriber.SubscribeBatched(topic, handler, Options(1, Long)));
            return;
        }

        _subscriber.SubscribeBatched(topic, _sink.Handler, Options(1, Long)).Schema.Dispose();

        using var sent = new AttachmentsBuilder();
        sent.AppendDelivered(ReadOnlySpan<byte>.Empty, [7, 8]);
        Publish(topic, 1, "a", sent);

        Assert.True(_sink.WaitFor(1, Await), "no batch at the row limit");
        Delivery only = _sink.Snapshot()[0];
        Assert.Equal(1, only.Rows);
        Assert.Equal(0, only.Status.RowsDropped);
        Assert.True(Assert.Single(only.Attachments).TryFind(ReadOnlySpan<byte>.Empty, out ReadOnlySpan<byte> value));
        Assert.Equal(new byte[] { 7, 8 }, value.ToArray());
        Assert.Equal(0UL, _subscriber.AbsorbedCallbackFailures);
    }

    // ── One at a time, in window order, on one thread (d10 L13; d7 L12) ─────

    /// <summary>
    /// d7's EveryCallbackRunsOnTheBatchThreadInWindowOrder, replayed from C# (D10C-7, ladder rung 7;
    /// BC-9; d10 L13, L4): max_rows = 2, a second thread publishes 6 rows, and the first handler
    /// call holds until the publisher reports every Publish returned - which a Publish that ran or
    /// waited on a handler can never report. Then the windows arrive as x = 0, 2, 4, two rows each,
    /// and the close ends them with one empty Closing; every call on one thread that is neither
    /// the publisher's nor the closer's.
    /// </summary>
    [Fact]
    public void EveryBatchArrivesOnOneThreadInWindowOrder()
    {
        TopicPath topic = Declared("onethread");
        if (!ServerLeg)
        {
            EdgeRefuses(topic, handler => _subscriber.SubscribeBatched(topic, handler, Options(2, Long)));
            return;
        }

        TimeSpan hold = TimeSpan.FromSeconds(2);
        using var entered = new ManualResetEventSlim(false);
        using var allPublished = new ManualResetEventSlim(false);
        using var firstReturned = new ManualResetEventSlim(false);
        bool releasedByPublisher = false;
        var threads = new List<int>();
        SubscribeResult result = _subscriber.SubscribeBatched(topic, (batch, attachments, status) =>
        {
            lock (threads)
            {
                threads.Add(Environment.CurrentManagedThreadId);
            }

            _sink.Handler(batch, attachments, status);
            if (!entered.IsSet)
            {
                entered.Set();
                Volatile.Write(ref releasedByPublisher, allPublished.Wait(hold));
                firstReturned.Set();
            }
        }, Options(2, Long));
        result.Schema.Dispose();

        int publisherThread = 0;
        bool enteredBeforeRest = false;
        var publisher = new Thread(() =>
        {
            publisherThread = Environment.CurrentManagedThreadId;
            Publish(topic, 0, "n");
            Publish(topic, 1, "n");  // the first full window
            enteredBeforeRest = entered.Wait(Await);
            for (int i = 2; i < 6; i++)
            {
                Publish(topic, i, "n");
            }

            allPublished.Set();
        })
        { IsBackground = true };
        publisher.Start();
        Assert.True(publisher.Join(Await + Await), "the publisher never finished");
        Assert.True(firstReturned.Wait(Await), "the first handler call never returned");

        Assert.True(Volatile.Read(ref releasedByPublisher),
            "the publisher could not finish publishing while the first handler call was held: Publish ran or waited on a batch handler");
        Assert.True(enteredBeforeRest, "the first window was not delivered while the publisher waited");
        Assert.True(_sink.WaitFor(3, Await), "fewer than three windows arrived");
        result.Subscription.Dispose();

        List<Delivery> seen = _sink.Snapshot();
        Assert.Equal(4, seen.Count);
        for (int i = 0; i < 3; i++)
        {
            Assert.Equal(BatchReason.RowLimit, seen[i].Status.Reason);
            Assert.Equal(2, seen[i].Rows);
            Assert.Equal(2 * i, ((Int32Array)seen[i].Batch!.Column(0)).GetValue(0));
            Assert.Equal(0, seen[i].Status.RowsDropped);
        }

        Assert.Equal(BatchReason.Closing, seen[3].Status.Reason);
        Assert.Equal(0, seen[3].Rows);

        int[] on;
        lock (threads)
        {
            on = [.. threads];
        }

        Assert.All(on, id => Assert.Equal(on[0], id));
        Assert.NotEqual(publisherThread, on[0]);
        Assert.NotEqual(Environment.CurrentManagedThreadId, on[0]);
    }

    /// <summary>
    /// D10C-7 design D3 step 2, d10 L8: a batch the handler KEEPS reads intact after the handler
    /// returned, after later windows were delivered, and after the Closing - the handler owns it,
    /// so nothing it points into is reclaimed under it. The SERVER arm is a guard: green before the
    /// switch (the managed batcher's batches are owned too); after it, it catches a batch imported
    /// without first being moved out of the shim's borrowed struct. The edge arm is the truth table
    /// (D2's forcing arm), red until the switch.
    /// </summary>
    [Fact]
    public void AKeptBatchReadsIntactAfterClosing()
    {
        TopicPath topic = Declared("kept");
        if (!ServerLeg)
        {
            EdgeRefuses(topic, handler => _subscriber.SubscribeBatched(topic, handler, Options(2, Long)));
            return;
        }

        SubscribeResult result = _subscriber.SubscribeBatched(topic, _sink.Handler, Options(2, Long));
        result.Schema.Dispose();

        // Five rows at two a window: two full windows, then a Closing that carries the fifth,
        // so this guard does not lean on the idle Closing (AnIdleCloseDeliversOneEmptyClosing's).
        for (int i = 0; i < 5; i++)
        {
            Publish(topic, i, "kept" + new string('k', i * 40));
        }

        Assert.True(_sink.WaitFor(2, Await), "fewer than two windows arrived");
        result.Subscription.Dispose();
        GC.Collect();
        GC.WaitForPendingFinalizers();
        GC.Collect();

        List<Delivery> seen = _sink.Snapshot();
        Assert.Equal(3, seen.Count);
        Assert.Equal(BatchReason.Closing, seen[2].Status.Reason);
        int expected = 0;
        foreach (Delivery d in seen)
        {
            var x = (Int32Array)d.Batch!.Column(0);
            var name = (StringArray)d.Batch!.Column(1);
            for (int r = 0; r < d.Rows; r++, expected++)
            {
                Assert.Equal(expected, x.GetValue(r));
                Assert.Equal("kept" + new string('k', expected * 40), name.GetString(r));
            }
        }

        Assert.Equal(5, expected);
    }

    /// <summary>
    /// A leg (mechanism, design P2; d10 L8): the batch thunk, called directly with a batch C#
    /// exported, MOVES the array and schema out of the borrowed structs - each source's
    /// <c>release</c> is NULL when it returns - and the batch the handler kept reads intact after
    /// the source structs are freed, with each row's attachments copied. Runs on both legs: it
    /// calls no shim entry point. Not the proof of a promise; AKeptBatchReadsIntactAfterClosing is.
    /// </summary>
    [Fact]
    public unsafe void TheBatchThunkMovesTheBatchOutOfTheBorrowedStructs()
    {
        using RecordBatch exported = new(TwoColumns,
            [new Int32Array.Builder().Append(4).Append(5).Build(), new StringArray.Builder().Append("moved").Append("kept").Build()],
            length: 2);
        CArrowArray* array = CArrowArray.Create();
        CArrowSchema* schema = CArrowSchema.Create();
        using var first = new AttachmentsBuilder();
        first.Set("row", [1]);
        using var second = new AttachmentsBuilder();
        nint[] sets = [first.Build().DangerousGetHandle(), second.Build().DangerousGetHandle()];

        RecordBatch? kept = null;
        IReadOnlyList<AttachmentsBuilder> keptAttachments = [];
        RecordBatchHandler handler = (batch, attachments, _) =>
        {
            kept = batch;
            keptAttachments = attachments;
        };
        Type deliveryType = typeof(SubscriberArrow).GetNestedType("BatchDelivery", BindingFlags.NonPublic)!;
        object delivery = Activator.CreateInstance(
            deliveryType, BindingFlags.NonPublic | BindingFlags.Instance, null, [_subscriber, handler], null)!;
        GCHandle self = GCHandle.Alloc(delivery);
        var thunk = (delegate* unmanaged[Cdecl]<nint, Eiva.Fletcher.Interop.FlBatch*, void>)typeof(SubscriberArrow)
            .GetMethod("BatchThunk", BindingFlags.NonPublic | BindingFlags.Static)!.MethodHandle.GetFunctionPointer();
        try
        {
            CArrowSchemaExporter.ExportSchema(TwoColumns, schema);
            CArrowArrayExporter.ExportRecordBatch(exported, array);
            fixed (nint* setPointers = sets)
            {
                var batch = new Eiva.Fletcher.Interop.FlBatch
                {
                    Reason = (int)BatchReason.RowLimit,
                    NumRows = 2,
                    Array = (nint)array,
                    Schema = (nint)schema,
                    Attachments = (nint)setPointers,
                };
                thunk(GCHandle.ToIntPtr(self), &batch);
            }

            // The move: each source's `release` NULL, read where Apache.Arrow's struct keeps it.
            Assert.Equal(nint.Zero, *(nint*)((byte*)array + Marshal.OffsetOf<CArrowArray>("release")));
            Assert.Equal(nint.Zero, *(nint*)((byte*)schema + Marshal.OffsetOf<CArrowSchema>("release")));
        }
        finally
        {
            CArrowArray.Free(array);
            CArrowSchema.Free(schema);
            self.Free();
        }

        Assert.NotNull(kept);
        using (kept)
        {
            Assert.Equal(2, kept!.Length);
            Assert.Equal(5, ((Int32Array)kept.Column(0)).GetValue(1));
            Assert.Equal("moved", ((StringArray)kept.Column(1)).GetString(0));
            Assert.Equal(2, keptAttachments.Count);
            Assert.True(keptAttachments[0].TryFind("row"u8, out ReadOnlySpan<byte> value));
            Assert.Equal(new byte[] { 1 }, value.ToArray());
            Assert.Equal(0, keptAttachments[1].Count);
        }

        foreach (AttachmentsBuilder copy in keptAttachments)
        {
            copy.Dispose();
        }

        Assert.Equal(0UL, _subscriber.AbsorbedCallbackFailures);
    }

    /// <summary>
    /// A leg (mechanism, design residue (a); d10 L9; review 4a F1): a window the thunk cannot take over
    /// still reaches the handler - a Closing here - as a null batch with no attachments, its rows added
    /// to RowsDropped and its reason kept, never lost. The failure is the design's own example: a
    /// window longer than one managed batch holds (more than int.MaxValue rows), refused before
    /// anything is moved, so the shim still owns, and releases, the array.
    /// </summary>
    [Fact]
    public unsafe void AWindowTheThunkCannotTakeOverArrivesAsANullBatchWithItsRowsCounted()
    {
        using RecordBatch exported = new(TwoColumns,
            [new Int32Array.Builder().Append(4).Append(5).Build(), new StringArray.Builder().Append("a").Append("b").Build()],
            length: 2);
        CArrowArray* array = CArrowArray.Create();
        CArrowSchema* schema = CArrowSchema.Create();
        using var first = new AttachmentsBuilder();
        using var second = new AttachmentsBuilder();
        nint[] sets = [first.Build().DangerousGetHandle(), second.Build().DangerousGetHandle()];

        var seen = new List<Delivery>();
        RecordBatchHandler handler = (batch, attachments, status) => seen.Add(new Delivery(batch, attachments, status));
        Type deliveryType = typeof(SubscriberArrow).GetNestedType("BatchDelivery", BindingFlags.NonPublic)!;
        object delivery = Activator.CreateInstance(
            deliveryType, BindingFlags.NonPublic | BindingFlags.Instance, null, [_subscriber, handler], null)!;
        nint ctx = GCHandle.ToIntPtr(GCHandle.Alloc(delivery));  // a Closing: the thunk frees it
        var thunk = (delegate* unmanaged[Cdecl]<nint, Eiva.Fletcher.Interop.FlBatch*, void>)typeof(SubscriberArrow)
            .GetMethod("BatchThunk", BindingFlags.NonPublic | BindingFlags.Static)!.MethodHandle.GetFunctionPointer();
        try
        {
            CArrowSchemaExporter.ExportSchema(TwoColumns, schema);
            CArrowArrayExporter.ExportRecordBatch(exported, array);
            fixed (nint* setPointers = sets)
            {
                var batch = new Eiva.Fletcher.Interop.FlBatch
                {
                    Reason = (int)BatchReason.Closing,
                    RowsDropped = 3,
                    NumRows = (long)int.MaxValue + 1,
                    Array = (nint)array,
                    Schema = (nint)schema,
                    Attachments = (nint)setPointers,
                };
                thunk(ctx, &batch);
            }
        }
        finally
        {
            CArrowArray.Free(array);
            CArrowSchema.Free(schema);
        }

        Delivery only = Assert.Single(seen);
        Assert.Null(only.Batch);
        Assert.Empty(only.Attachments);
        Assert.Equal(BatchReason.Closing, only.Status.Reason);
        Assert.Equal(3 + (long)int.MaxValue + 1, only.Status.RowsDropped);
        // C#'s own take-over failure carries a count only, no error (ruling :669; d10 L9).
        Assert.Null(only.Status.Failure);
        Assert.Equal(0UL, _subscriber.AbsorbedCallbackFailures);
    }

    // ── A failed hand-over's failure, in the thunk (D10C-10, rung 11) ───────
    //
    // Legs: the mechanism under the external proof in Fletcher.Handover.Tests
    // (AWindowTheShimCannotHandOverReachesTheHandlerWithItsFailure), each driving the batch thunk
    // directly with a crafted fl_batch whose `failure` is set, as d9 hands over a window it could
    // not export: no array, no schema, no rows, the window's rows in rows_dropped.

    /// <summary>The batch thunk as native calls it.</summary>
    private static unsafe delegate* unmanaged[Cdecl]<nint, Eiva.Fletcher.Interop.FlBatch*, void> Thunk =>
        (delegate* unmanaged[Cdecl]<nint, Eiva.Fletcher.Interop.FlBatch*, void>)typeof(SubscriberArrow)
            .GetMethod("BatchThunk", BindingFlags.NonPublic | BindingFlags.Static)!.MethodHandle.GetFunctionPointer();

    /// <summary>A BatchDelivery over <paramref name="handler"/>, held by a GCHandle the caller (or a Closing) frees.</summary>
    private GCHandle NewDelivery(RecordBatchHandler handler)
    {
        Type deliveryType = typeof(SubscriberArrow).GetNestedType("BatchDelivery", BindingFlags.NonPublic)!;
        object delivery = Activator.CreateInstance(
            deliveryType, BindingFlags.NonPublic | BindingFlags.Instance, null, [_subscriber, handler], null)!;
        return GCHandle.Alloc(delivery);
    }

    /// <summary>
    /// A leg (rung 11; d10 L3, L8; ruling :551 (3), :512): the failure on a window's <c>fl_batch</c>
    /// reaches the handler's <see cref="BatchStatus.Failure"/> AS GIVEN. Its status and origin are an
    /// unusual pair (PayloadTooLarge, Seam), so a hard-coded Internal or Handover turns this red; its
    /// message is multi-byte UTF-8 with an escape sequence and no NUL, followed in memory by bytes
    /// that are not part of it, so a decode that scans instead of using the length turns it red. The
    /// "unchanged" half the external test cannot spell (its message differs by C++ runtime).
    /// </summary>
    [Fact]
    public unsafe void TheBatchThunkForwardsAFailureAsGiven()
    {
        byte[] message = System.Text.Encoding.UTF8.GetBytes("hånd-över fejlede: 失败 \u001b[31m✗\u001b[0m 🚀");
        const int Beyond = 16;
        byte* bytes = (byte*)NativeMemory.Alloc((nuint)(message.Length + Beyond));
        var seen = new List<Delivery>();
        GCHandle self = NewDelivery((batch, attachments, status) => seen.Add(new Delivery(batch, attachments, status)));
        try
        {
            message.CopyTo(new Span<byte>(bytes, message.Length));
            new Span<byte>(bytes + message.Length, Beyond).Fill((byte)'X');
            var failure = new Eiva.Fletcher.Interop.FlError
            {
                Status = (int)FletcherStatus.PayloadTooLarge,
                Origin = (int)FletcherOrigin.Seam,
                Message = (nint)bytes,
                MessageLen = (nuint)message.Length,
            };
            var batch = new Eiva.Fletcher.Interop.FlBatch
            {
                Reason = (int)BatchReason.Timeout,
                RowsDropped = 4,
                Failure = (nint)(&failure),
            };
            Thunk(GCHandle.ToIntPtr(self), &batch);
        }
        finally
        {
            self.Free();
            NativeMemory.Free(bytes);
        }

        Delivery only = Assert.Single(seen);
        Assert.Null(only.Batch);
        Assert.Empty(only.Attachments);
        Assert.Equal(BatchReason.Timeout, only.Status.Reason);
        Assert.Equal(4, only.Status.RowsDropped);
        FletcherException forwarded = Assert.IsType<FletcherException>(only.Status.Failure);
        Assert.Equal(FletcherStatus.PayloadTooLarge, forwarded.Status);
        Assert.Equal(FletcherOrigin.Seam, forwarded.Origin);
        Assert.Equal(message, System.Text.Encoding.UTF8.GetBytes(forwarded.Message));
        Assert.Equal(0UL, _subscriber.AbsorbedCallbackFailures);
    }

    /// <summary>
    /// A leg (rung 11; d10 L8 "never disposes a batch's failure"; d9 L2; ruling :512 "borrowed for
    /// the callback"; step-0a audit (a)): the thunk leaves the borrowed <c>fl_error</c> as it found
    /// it, because the shim frees it after the callback returns (<c>batches_server.cpp</c>). The
    /// failure is a real one the shim allocated (a refused codec open, as HandleTests does), and
    /// <c>fl_error_dispose</c> zeroes what it frees (<c>binding.cpp</c>), so an IN-PLACE dispose of
    /// the borrowed struct turns this red. Why a leg: if C# did dispose it, nobody outside could
    /// see it, since the shim's own later dispose of a zeroed struct is a no-op.
    /// </summary>
    /// <remarks>
    /// A dispose of a struct COPY (<c>var e = *f; fl_error_dispose(ref e)</c>) frees the message
    /// and leaves <c>*f</c> intact, so it is outside the predicate above (review DEBT-1); the shim
    /// would then free the message a second time. The scan below covers that case: the thunk's
    /// file, <c>SubscriberArrow.cs</c>, names <c>fl_error_dispose</c> nowhere, and the decoder's,
    /// <c>Errors.cs</c>, names it once, in <c>ThrowIfFailed</c>'s finally, which the thunk never
    /// reaches (step-0b audit note 2). The bare name, so no spelling of the call escapes (4a F1).
    /// </remarks>
    [Fact]
    public unsafe void TheBatchThunkNeverDisposesTheBorrowedFailure()
    {
        Eiva.Fletcher.Interop.FlError failure = default;
        int refused = Eiva.Fletcher.Interop.NativeMethods.fl_codec_open(
            nint.Zero, out Eiva.Fletcher.Interop.CodecHandle codec, ref failure);
        codec.Dispose();
        var seen = new List<Delivery>();
        GCHandle self = NewDelivery((batch, attachments, status) => seen.Add(new Delivery(batch, attachments, status)));
        try
        {
            Assert.NotEqual(0, refused);
            Assert.NotEqual(nint.Zero, failure.Message);
            Eiva.Fletcher.Interop.FlError before = failure;
            var batch = new Eiva.Fletcher.Interop.FlBatch
            {
                Reason = (int)BatchReason.RowLimit,
                RowsDropped = 2,
                Failure = (nint)(&failure),
            };
            Thunk(GCHandle.ToIntPtr(self), &batch);

            Assert.Single(seen);
            Assert.Equal(before.Status, failure.Status);
            Assert.Equal(before.Origin, failure.Origin);
            Assert.Equal(before.Message, failure.Message);
            Assert.Equal(before.MessageLen, failure.MessageLen);
        }
        finally
        {
            self.Free();
            Eiva.Fletcher.Interop.NativeMethods.fl_error_dispose(ref failure);
        }

        string src = typeof(SubscriberArrowTests).Assembly.GetCustomAttributes<AssemblyMetadataAttribute>()
            .FirstOrDefault(a => a.Key == "FletcherDotnetSrc")?.Value ?? string.Empty;
        string thunkFile = Path.Combine(src, "Fletcher", "SubscriberArrow.cs");
        string errorsFile = Path.Combine(src, "Fletcher", "Errors.cs");
        Assert.True(File.Exists(thunkFile) && File.Exists(errorsFile), $"the baked source folder '{src}' lacks the scanned files.");
        Assert.DoesNotContain("fl_error_dispose", File.ReadAllText(thunkFile), StringComparison.Ordinal);
        string errors = File.ReadAllText(errorsFile);
        int disposes = errors.Split("fl_error_dispose").Length - 1;
        Assert.True(disposes == 1 && errors.Contains("finally", StringComparison.Ordinal) && errors.Contains("throw ToException(", StringComparison.Ordinal),
            $"Errors.cs disposes an fl_error {disposes} times; exactly once, in ThrowIfFailed's finally, is the rule");
    }

    /// <summary>
    /// A leg (rung 11, the design's handled residue; review DEBT-3; d10 L9; brief D10C-10, "Handled"): when
    /// copying the shim's failure throws, the window still arrives, once - a null batch, its rows
    /// counted, its reason kept - with no failure attached. The failure crafted here has a
    /// message longer than one managed string holds (more than int.MaxValue bytes), so the copy
    /// throws deterministically before reading a byte. Red if the copy is not contained: its throw
    /// would escape the delivery and the thunk's outer catch would swallow the window - a Closing
    /// here, the subscription's last delivery.
    /// </summary>
    [Fact]
    public unsafe void ACopiedFailureThatCannotBeDecodedStillDeliversTheWindow()
    {
        byte unread = (byte)'x';
        var seen = new List<Delivery>();
        nint ctx = GCHandle.ToIntPtr(NewDelivery((batch, attachments, status) => seen.Add(new Delivery(batch, attachments, status))));
        var failure = new Eiva.Fletcher.Interop.FlError
        {
            Status = (int)FletcherStatus.Internal,
            Origin = (int)FletcherOrigin.Handover,
            Message = (nint)(&unread),
            MessageLen = (nuint)int.MaxValue + 1,
        };
        var batch = new Eiva.Fletcher.Interop.FlBatch
        {
            Reason = (int)BatchReason.Closing,
            RowsDropped = 5,
            Failure = (nint)(&failure),
        };
        Thunk(ctx, &batch);  // a Closing: the thunk frees the context

        Delivery only = Assert.Single(seen);
        Assert.Null(only.Batch);
        Assert.Empty(only.Attachments);
        Assert.Equal(BatchReason.Closing, only.Status.Reason);
        Assert.Equal(5, only.Status.RowsDropped);
        Assert.Null(only.Status.Failure);
        Assert.Equal(0UL, _subscriber.AbsorbedCallbackFailures);
    }

    // ── A handler that cancels, and Unsubscribe's wait (D-BIND-64, revised) ─
    //
    // No C++ mirror: these pin what the BIND-5 review found. The first is the review's
    // B2 reproduction, kept as a regression test. D-BIND-64's "not serialised" is revised
    // by ruling :551 (4): batches reach a handler one at a time, in order (d10 L13).

    /// <summary>A timer-thread handler that cancels while a delivery fills the window does not deadlock.</summary>
    [Fact]
    public void ATimerHandlerThatCancelsDuringADeliveryDoesNotDeadlock()
    {
        TopicPath topic = Declared("timercancelrace");
        if (!ServerLeg)
        {
            EdgeRefuses(topic, handler => _subscriber.SubscribeBatched(topic, handler, Options(2, TimeSpan.FromMilliseconds(20))));
            return;
        }

        using var inHandler = new ManualResetEventSlim(false);
        using var done = new ManualResetEventSlim(false);
        Subscription? self = null;
        int timeouts = 0;
        SubscribeResult result = _subscriber.SubscribeBatched(topic, (batch, _, status) =>
        {
            batch?.Dispose();
            if (status.Reason == BatchReason.Timeout && Interlocked.Increment(ref timeouts) == 1)
            {
                inHandler.Set();
                Thread.Sleep(300);  // long enough for the delivery below to fill the window
                _subscriber.Unsubscribe(self!);
                done.Set();
            }
        }, Options(2, TimeSpan.FromMilliseconds(20)));
        self = result.Subscription;
        result.Schema.Dispose();

        Publish(topic, 1, "a");
        Assert.True(inHandler.Wait(TimeSpan.FromSeconds(5)), "the timeout flush never ran");
        // A thread of its own, not the pool: the timer's callbacks already run there.
        var delivery = new Thread(() =>
        {
            Publish(topic, 2, "b");
            Publish(topic, 3, "c");
        })
        { IsBackground = true };
        delivery.Start();

        Assert.True(done.Wait(TimeSpan.FromSeconds(10)), "the cancel from the timer thread deadlocked");
        Assert.True(delivery.Join(TimeSpan.FromSeconds(10)), "the delivery never returned");
    }

    /// <summary>Unsubscribe returns only once a timer-thread handler still running elsewhere has finished.</summary>
    [Fact]
    public void UnsubscribeWaitsForATimerHandlerStillRunning()
    {
        TopicPath topic = Declared("waitstimer");
        if (!ServerLeg)
        {
            EdgeRefuses(topic, handler => _subscriber.SubscribeBatched(topic, handler, Options(100, TimeSpan.FromMilliseconds(20))));
            return;
        }

        using var inHandler = new ManualResetEventSlim(false);
        int finished = 0;
        SubscribeResult result = _subscriber.SubscribeBatched(topic, (batch, _, status) =>
        {
            batch?.Dispose();
            if (status.Reason == BatchReason.Timeout)
            {
                inHandler.Set();
                Thread.Sleep(300);
                Volatile.Write(ref finished, 1);
            }
        }, Options(100, TimeSpan.FromMilliseconds(20)));
        result.Schema.Dispose();

        Publish(topic, 1, "a");
        Assert.True(inHandler.Wait(TimeSpan.FromSeconds(5)), "the timeout flush never ran");

        _subscriber.Unsubscribe(result.Subscription);

        Assert.Equal(1, Volatile.Read(ref finished));
    }

    /// <summary>Mirrors SubscriberArrowBatchTest.BatchesAreValidArrow.</summary>
    /// <remarks>
    /// C++'s schema - int32, utf8, a dictionary and a list of utf8 - so the decode
    /// builds a variable-length child and a dictionary column as well as scalars
    /// (review T-D2). Weaker in one way, by necessity: Apache.Arrow for .NET has no
    /// ValidateFull, so the shape it would check is checked by hand - every column the
    /// batch's length, the list's offsets monotonic and ending at its values' length -
    /// and every value is read back. D10C-7 (audit 0a T36; ruling :551 (2); d10 D24): the
    /// dictionary column arrives re-folded, a <see cref="DictionaryArray"/>, as domain 7
    /// delivers it.
    /// </remarks>
    [Fact]
    public void BatchesAreWellFormed()
    {
        var category = new DictionaryType(Int32Type.Default, StringType.Default, ordered: false);
        var schema = new Schema(
        [
            new Field("x", Int32Type.Default, nullable: true),
            new Field("name", StringType.Default, nullable: true),
            new Field("category", category, nullable: true),
            new Field("tags", new ListType(StringType.Default), nullable: true),
        ], metadata: null);
        TopicPath topic = Topic("wellformed");
        _publisher.CreateTopic(topic, schema);

        using var codec = new FletcherCodec(schema);
        void PublishRow(int i)
        {
            var tags = new ListArray.Builder(StringType.Default);
            tags.Append();
            ((StringArray.Builder)tags.ValueBuilder).Append("t" + i);
            using RecordBatch row = new(schema,
            [
                new Int32Array.Builder().Append(i).Build(),
                new StringArray.Builder().Append("n" + i).Build(),
                new DictionaryArray(category, new Int32Array.Builder().Append(i % 2).Build(),
                    new StringArray.Builder().Append("red").Append("blue").Build()),
                tags.Build(),
            ], length: 1);
            using BoundRows rows = codec.Bind(row);
            _publisher.Publish(topic, rows, 0);
        }

        if (!ServerLeg)
        {
            EdgeRefuses(topic, handler => _subscriber.SubscribeBatched(topic, handler, Options(5, Long)), () => PublishRow(0));
            return;
        }

        _subscriber.SubscribeBatched(topic, _sink.Handler, Options(5, Long)).Schema.Dispose();
        for (int i = 0; i < 5; i++)
        {
            PublishRow(i);
        }

        Assert.True(_sink.WaitFor(1, Await), "no batch at the row limit");
        RecordBatch batch = Assert.Single(_sink.Snapshot()).Batch!;
        Assert.Equal(5, batch.Length);
        Assert.All(batch.Arrays, column => Assert.Equal(5, column.Length));

        var listed = (ListArray)batch.Column(3);
        var offsets = listed.ValueOffsets;
        Assert.Equal(0, offsets[0]);
        for (int r = 0; r < 5; r++)
        {
            Assert.True(offsets[r + 1] >= offsets[r], "the list's offsets are not monotonic");
        }

        Assert.Equal(listed.Values.Length, offsets[5]);

        var folded = Assert.IsType<DictionaryArray>(batch.Column(2));
        for (int i = 0; i < 5; i++)
        {
            Assert.Equal(i, ((Int32Array)batch.Column(0)).GetValue(i));
            Assert.Equal("n" + i, ((StringArray)batch.Column(1)).GetString(i));
            Assert.Equal(i % 2 == 0 ? "red" : "blue", ValueAt(folded, i));
            Assert.Equal("t" + i, ((StringArray)listed.Values).GetString(offsets[i]));
        }
    }

    /// <summary>Mirrors SubscriberArrowBatchTest.ReuseAcrossWindows.</summary>
    [Fact]
    public void WindowsFollowOneAnother()
    {
        TopicPath topic = Declared("windows");
        if (!ServerLeg)
        {
            EdgeRefuses(topic, handler => _subscriber.SubscribeBatched(topic, handler, Options(2, Long)));
            return;
        }

        SubscribeResult result = _subscriber.SubscribeBatched(topic, _sink.Handler, Options(2, Long));
        result.Schema.Dispose();

        for (int i = 0; i < 5; i++)
        {
            Publish(topic, i, "n" + i);
        }

        result.Subscription.Dispose();

        List<Delivery> seen = _sink.Snapshot();
        Assert.Equal([2, 2, 1], [seen[0].Rows, seen[1].Rows, seen[2].Rows]);
        Assert.Equal([BatchReason.RowLimit, BatchReason.RowLimit, BatchReason.Closing], [seen[0].Status.Reason, seen[1].Status.Reason, seen[2].Status.Reason]);
        Assert.All(seen, d => Assert.Equal(0, d.Status.RowsDropped));

        // The reuse C++ checks: each window starts where the last one ended - no row
        // re-delivered, none skipped - so the values run 0..4 across all three
        // (review T-D1).
        int expected = 0;
        foreach (Delivery d in seen)
        {
            var x = (Int32Array)d.Batch!.Column(0);
            for (int r = 0; r < x.Length; r++)
            {
                Assert.Equal(expected++, x.GetValue(r));
            }
        }

        Assert.Equal(5, expected);
    }

    /// <summary>
    /// D10C-7 (review 4b S3; d7 L9; binding.h, fl_subscriber_unsubscribe_batches): closing ANOTHER batched
    /// subscription from inside a batch handler waits for that one's Closing - it arrives, once, before
    /// the call returns - and nothing reaches it after. One direction only: two handlers closing each
    /// other hang, which Unsubscribe's docs state and no test exercises.
    /// </summary>
    [Fact]
    public void ClosingASiblingFromABatchHandlerWaitsForItsClosing()
    {
        TopicPath closer = Declared("siblingcloser");
        TopicPath closed = Declared("siblingclosed");
        if (!ServerLeg)
        {
            EdgeRefuses(closed, handler => _subscriber.SubscribeBatched(closed, handler, Options(1, Long)));
            return;
        }

        using var siblingSink = new Sink();
        SubscribeResult sibling = _subscriber.SubscribeBatched(closed, siblingSink.Handler, Options(1, Long));
        sibling.Schema.Dispose();
        using var returned = new ManualResetEventSlim(false);
        int closingsBeforeReturn = -1;
        Exception? failure = null;
        _subscriber.SubscribeBatched(closer, (batch, _, status) =>
        {
            batch?.Dispose();
            if (status.Reason == BatchReason.RowLimit && !returned.IsSet)
            {
                failure = Record.Exception(() => _subscriber.Unsubscribe(sibling.Subscription));
                closingsBeforeReturn = siblingSink.Snapshot().Count(d => d.Status.Reason == BatchReason.Closing);
                returned.Set();
            }
        }, Options(1, Long)).Schema.Dispose();

        Publish(closer, 1, "close");

        Assert.True(returned.Wait(Await), "closing a sibling from a batch handler did not return");
        Assert.Null(failure);
        Assert.Equal(1, closingsBeforeReturn);
        Publish(closed, 2, "after");
        Assert.False(siblingSink.WaitFor(2, Quiet), "a delivery reached the sibling after its Closing");
        Assert.False(sibling.Subscription.IsLive);
    }

    // ── A quiet close keeps its topic (D10C-7 D5; FWD-1, FWD-1c) ────────────

    /// <summary>
    /// D5's forcing test (D10C-7 design D5; FWD-1c; ruling :575 (6)), red before D5 on both legs
    /// because the claim was released at the close: a subscription closed from inside a ROW
    /// delivery on the provider leaves the transport subscription open and quiet (subscriber.hpp,
    /// owner ruling 2026-09-05), so the topic stays held for that kind: the other kind is refused
    /// by name, and the same kind again is accepted and receives rows.
    /// </summary>
    /// <remarks>
    /// The close is made from a per-row handler on another topic of this same object. Per-row
    /// first runs on both legs: the claim is checked before the shim is asked (design D2), so the
    /// edge leg refuses the batched call by the claim too. Batched first is the server leg's; on
    /// the edge leg it is the truth table.
    /// </remarks>
    [Theory]
    [InlineData(true)]
    [InlineData(false)]
    public void AQuietCloseKeepsTheTopicForItsKind(bool perRowHeld)
    {
        TopicPath topic = Declared(perRowHeld ? "quietrow" : "quietbatched");
        TopicPath trigger = Declared(perRowHeld ? "quiettriggerrow" : "quiettriggerbatched");
        if (!perRowHeld && !ServerLeg)
        {
            EdgeRefuses(topic, handler => SubscribeKind(_subscriber, perRow: false, topic, handler));
            return;
        }

        using var again = new Sink();
        SubscribeResult held = SubscribeKind(_subscriber, perRowHeld, topic, _sink.Handler);
        held.Schema.Dispose();

        Exception? closeFailure = null;
        bool closedInside = false;
        _subscriber.Subscribe(trigger, (batch, _, _) =>
        {
            batch?.Dispose();
            if (!closedInside)
            {
                closedInside = true;
                closeFailure = Record.Exception(() => _subscriber.Unsubscribe(held.Subscription));
            }
        }).Schema.Dispose();
        Publish(trigger, 0, "close");
        Assert.True(closedInside, "the trigger's row delivery did not run");
        Assert.Null(closeFailure);

        InvalidOperationException refusal = Assert.Throws<InvalidOperationException>(
            () => SubscribeKind(_subscriber, !perRowHeld, topic, again.Handler));
        Assert.Contains(topic.ToString(), refusal.Message, StringComparison.Ordinal);
        Assert.Contains(perRowHeld ? "holds a per-row" : "holds a batched", refusal.Message, StringComparison.Ordinal);

        SubscribeResult reopened = SubscribeKind(_subscriber, perRowHeld, topic, again.Handler);
        reopened.Schema.Dispose();
        Publish(topic, 9, "again");
        Assert.True(again.WaitFor(1, Await), "the same kind, subscribed again after a quiet close, received nothing");
        Assert.Equal(9, ((Int32Array)again.Snapshot()[0].Batch!.Column(0)).GetValue(0));
    }

    /// <summary>A handler that throws is absorbed and counted, and the next batch still arrives.</summary>
    /// <remarks>
    /// No C++ mirror: the C++ tier swallows a throwing callback silently. The binding
    /// counts it and raises it (D-BIND-19 rule 5), as a Subscriber does.
    /// </remarks>
    [Fact]
    public void AHandlerThatThrowsIsAbsorbedCountedAndRaised()
    {
        TopicPath topic = Declared("throws");
        int faults = 0;
        _subscriber.HandlerFaulted += (_, _) => faults++;
        int calls = 0;
        _subscriber.Subscribe(topic, (batch, _, _) =>
        {
            batch?.Dispose();
            if (++calls == 1)
            {
                throw new InvalidOperationException("first batch");
            }
        }).Schema.Dispose();

        Publish(topic, 1, "a");
        Publish(topic, 2, "b");

        Assert.Equal(2, calls);
        Assert.Equal(1UL, _subscriber.AbsorbedCallbackFailures);
        Assert.Equal(1, faults);
    }

    /// <summary>
    /// The batched twin of AHandlerThatThrowsIsAbsorbedCountedAndRaised (D10C-7; d10 L9, V13;
    /// design D3 step 5): a batched handler that throws is absorbed, counted once and raised once
    /// with its own exception, and the next batch still arrives.
    /// </summary>
    [Fact]
    public void ABatchedHandlerThatThrowsIsAbsorbedCountedAndRaised()
    {
        TopicPath topic = Declared("throwsbatched");
        if (!ServerLeg)
        {
            EdgeRefuses(topic, handler => _subscriber.SubscribeBatched(topic, handler, Options(1, Long)));
            return;
        }

        var faults = new List<Exception>();
        _subscriber.HandlerFaulted += (_, e) =>
        {
            lock (faults)
            {
                faults.Add(e.Exception);
            }
        };
        int calls = 0;
        _subscriber.SubscribeBatched(topic, (batch, attachments, status) =>
        {
            _sink.Handler(batch, attachments, status);
            if (Interlocked.Increment(ref calls) == 1)
            {
                throw new InvalidOperationException("first batch");
            }
        }, Options(1, Long)).Schema.Dispose();

        Publish(topic, 1, "a");
        Publish(topic, 2, "b");

        Assert.True(_sink.WaitFor(2, Await), "the batch after the one whose handler threw did not arrive");
        Assert.Equal(2, ((Int32Array)_sink.Snapshot()[1].Batch!.Column(0)).GetValue(0));
        Assert.Equal(1UL, _subscriber.AbsorbedCallbackFailures);
        lock (faults)
        {
            Exception raised = Assert.Single(faults);
            Assert.Equal("first batch", Assert.IsType<InvalidOperationException>(raised).Message);
        }
    }

    // ── The sink ────────────────────────────────────────────────────────────

    internal sealed record Delivery(RecordBatch? Batch, IReadOnlyList<AttachmentsBuilder> Attachments, BatchStatus Status)
    {
        public int Rows => Batch?.Length ?? -1;
    }

    /// <summary>Collects deliveries and disposes the batches it was handed, which it owns.</summary>
    internal sealed class Sink : IDisposable
    {
        private readonly object _gate = new();
        private readonly List<Delivery> _deliveries = [];

        internal RecordBatchHandler Handler => (batch, attachments, status) =>
        {
            lock (_gate)
            {
                _deliveries.Add(new Delivery(batch, attachments, status));
                Monitor.PulseAll(_gate);
            }
        };

        internal List<Delivery> Snapshot()
        {
            lock (_gate)
            {
                return [.. _deliveries];
            }
        }

        internal bool WaitFor(int count, TimeSpan timeout)
        {
            DateTime until = DateTime.UtcNow + timeout;
            lock (_gate)
            {
                while (_deliveries.Count < count)
                {
                    TimeSpan left = until - DateTime.UtcNow;
                    if (left <= TimeSpan.Zero || !Monitor.Wait(_gate, left))
                    {
                        return _deliveries.Count >= count;
                    }
                }

                return true;
            }
        }

        public void Dispose()
        {
            lock (_gate)
            {
                foreach (Delivery d in _deliveries)
                {
                    d.Batch?.Dispose();
                }
            }
        }
    }
}
