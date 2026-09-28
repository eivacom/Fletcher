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
//                                                      -> U ADictionaryColumnArrivesAsItsValueType (DIFFERENT BY
//                                                         RULING: re-folding is deferred to DICT, D-BIND-8)
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
//   SubscriberArrowBatchTest.BatchesAreValidArrow      -> U BatchesAreWellFormed (weaker: Apache.Arrow has no
//                                                         ValidateFull; lengths and values are checked instead)
//   SubscriberArrowBatchTest.ReuseAcrossWindows        -> U WindowsFollowOneAnother
//
// ── ONE CASE IS EXCLUDED ────────────────────────────────────────────────────
// `FinishFailureIsReportedNotFatal` forces C++'s BatchDecoder to fail at Finish(),
// when 200 distinct values overflow an int8 dictionary index during RE-FOLDING. C#
// does not re-fold (D-BIND-8): the column arrives as its value type, so there is
// no Finish() to fail and nothing to construct. The neighbouring property - a
// window that cannot be decoded is reported, never fatal - is
// ASchemaTheCodecCannotOpenDropsEveryRow's and ACorruptRowIsDroppedAndTheBatchStaysAligned's.
// So the file set is 33: 32 mapped, 1 excluded.
using System;
using System.Collections.Generic;
using System.Threading;

using Apache.Arrow;
using Apache.Arrow.Types;

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
        Assert.Contains("declared with", refused.Message, StringComparison.Ordinal);
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
    /// Both forms, per-row and batched.
    /// </remarks>
    [Fact]
    public void TopicOptionsReachTheProviderThroughBothForms()
    {
        var profiled = new TopicOptions { Profile = "x" };

        FletcherException perRow = Assert.Throws<FletcherException>(
            () => _subscriber.Subscribe(Topic("optrow"), _sink.Handler, profiled));
        FletcherException batched = Assert.Throws<FletcherException>(
            () => _subscriber.SubscribeBatched(Topic("optbatch"), _sink.Handler, null, profiled));

        Assert.Equal(FletcherStatus.NotSupported, perRow.Status);
        Assert.Equal(FletcherStatus.NotSupported, batched.Status);
    }

    // ── Round trips ─────────────────────────────────────────────────────────

    /// <summary>Mirrors PubSubArrowTest.PublishSubscribeRoundtripWithArrowRow.</summary>
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

    // ── Batching ────────────────────────────────────────────────────────────

    /// <summary>Mirrors SubscriberArrowBatchTest.FlushesAtRowLimit.</summary>
    [Fact]
    public void FlushesAtTheRowLimit()
    {
        TopicPath topic = Declared("rowlimit");
        _subscriber.SubscribeBatched(topic, _sink.Handler, Options(3, Long)).Schema.Dispose();

        for (int i = 0; i < 3; i++)
        {
            Publish(topic, i, "n");
        }

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
        _subscriber.SubscribeBatched(topic, _sink.Handler, Options(100_000, TimeSpan.FromMilliseconds(100))).Schema.Dispose();

        Publish(topic, 1, "a");
        Publish(topic, 2, "b");

        Assert.True(_sink.WaitFor(1, TimeSpan.FromSeconds(5)), "no timeout flush within 5 s of a 100 ms window");
        Delivery only = Assert.Single(_sink.Snapshot());
        Assert.Equal(2, only.Rows);
        Assert.Equal(0, only.Status.RowsDropped);
        Assert.Equal(BatchReason.Timeout, only.Status.Reason);
    }

    /// <summary>Mirrors SubscriberArrowBatchTest.ClosingFlushOnUnsubscribe.</summary>
    [Fact]
    public void UnsubscribingFlushesThePartialWindow()
    {
        TopicPath topic = Declared("closing");
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

    /// <summary>Mirrors SubscriberArrowBatchTest.AttachmentsAlignWithRows.</summary>
    [Fact]
    public void AttachmentsAlignWithTheirRows()
    {
        TopicPath topic = Declared("align");
        _subscriber.SubscribeBatched(topic, _sink.Handler, Options(2, Long)).Schema.Dispose();

        using var sent = new AttachmentsBuilder();
        sent.Set("img", [9]);
        Publish(topic, 1, "a", sent);
        Publish(topic, 2, "b");

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

    /// <summary>Mirrors SubscriberArrowBatchTest.DictionaryColumnRefoldedToDictionaryArray.</summary>
    /// <remarks>
    /// DIFFERENT BY RULING: C++'s BatchDecoder re-folds into a DictionaryArray; C#
    /// delivers the VALUE type, and re-folding is deferred to DICT (D-BIND-8). What
    /// both owe is the values, in order.
    /// </remarks>
    [Fact]
    public void ADictionaryColumnArrivesAsItsValueType()
    {
        RecordBatch source = CodecFixtures.Dictionary();
        TopicPath topic = Topic("dictionary");
        _publisher.CreateTopic(topic, source.Schema);
        _subscriber.SubscribeBatched(topic, _sink.Handler, Options(3, Long)).Schema.Dispose();

        using var codec = new FletcherCodec(source.Schema);
        using BoundRows rows = codec.Bind(source);
        _publisher.Publish(topic, rows);

        Delivery only = Assert.Single(_sink.Snapshot());
        var category = Assert.IsType<StringArray>(only.Batch!.Column(1));
        Assert.Equal(["gamma", "alpha", "beta"], [category.GetString(0), category.GetString(1), category.GetString(2)]);
    }

    /// <summary>Mirrors SubscriberArrowBatchTest.DictionaryColumnPreservesNulls.</summary>
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
        _subscriber.SubscribeBatched(topic, _sink.Handler, Options(3, Long)).Schema.Dispose();

        using var codec = new FletcherCodec(schema);
        using BoundRows rows = codec.Bind(source);
        _publisher.Publish(topic, rows);

        var category = Assert.IsType<StringArray>(Assert.Single(_sink.Snapshot()).Batch!.Column(0));
        Assert.Equal("x", category.GetString(0));
        Assert.True(category.IsNull(1));
        Assert.Equal("x", category.GetString(2));
    }

    /// <summary>Mirrors SubscriberArrowBatchTest.CorruptRowIsCountedDroppedAndBatchStaysAligned.</summary>
    /// <remarks>
    /// The case the second decoding pass exists for: one native call fails on the
    /// corrupt row, each row is then decoded alone, and the good ones come back
    /// together with THEIR attachments, not a neighbour's.
    /// </remarks>
    [Fact]
    public void ACorruptRowIsDroppedAndTheBatchStaysAligned()
    {
        TopicPath topic = Declared("corrupt");
        SubscribeResult result = _subscriber.SubscribeBatched(topic, _sink.Handler, Options(100_000, Long));
        result.Schema.Dispose();

        using var a = new AttachmentsBuilder();
        a.Set("row", "A"u8);
        using var bad = new AttachmentsBuilder();
        bad.Set("row", "X"u8);
        using var b = new AttachmentsBuilder();
        b.Set("row", "B"u8);

        Publish(topic, 1, "first", a);
        _publisher.PublishRaw(topic, [0x00], bad);
        Publish(topic, 2, "second", b);
        result.Subscription.Dispose();

        Delivery only = Assert.Single(_sink.Snapshot());
        Assert.Equal(2, only.Rows);
        Assert.Equal(1, only.Status.RowsDropped);
        Assert.Equal("first", ((StringArray)only.Batch!.Column(1)).GetString(0));
        Assert.Equal("second", ((StringArray)only.Batch!.Column(1)).GetString(1));
        Assert.True(only.Attachments[0].TryFind("row"u8, out ReadOnlySpan<byte> first));
        Assert.True(only.Attachments[1].TryFind("row"u8, out ReadOnlySpan<byte> second));
        Assert.Equal("A"u8.ToArray(), first.ToArray());
        Assert.Equal("B"u8.ToArray(), second.ToArray());
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
        _subscriber.SubscribeBatched(topic, _sink.Handler, Options(1, Long)).Schema.Dispose();

        using var codec = new FletcherCodec(schema);
        using BoundRows rows = codec.Bind(source);
        _publisher.Publish(topic, rows, 0);

        var column = Assert.IsType<FixedSizeListArray>(Assert.Single(_sink.Snapshot()).Batch!.Column(0));
        Assert.True(column.IsValid(0), "the list itself arrived null");
        var values = (FloatArray)column.Values;
        Assert.Equal([1f, 2f, 3f], [values.GetValue(0)!.Value, values.GetValue(1)!.Value, values.GetValue(2)!.Value]);
    }

    /// <summary>Mirrors SubscriberArrowBatchTest.NestedDictionarySchemaReportsEveryRowDropped.</summary>
    /// <remarks>
    /// The property: a schema the codec cannot open drops EVERY row for the life of
    /// the subscription, reported with a NULL batch rather than silently. C++ shows
    /// it on a nested dictionary; C#'s codec decodes those (D-BIND-39), so it is shown
    /// here on one the codec refuses by name - a dictionary whose value type is a struct.
    /// </remarks>
    [Fact]
    public void ASchemaTheCodecCannotOpenDropsEveryRow()
    {
        var nested = new StructType([new Field("x", Int32Type.Default, nullable: true)]);
        var schema = new Schema([new Field("category", new DictionaryType(Int32Type.Default, nested, ordered: false), nullable: true)], metadata: null);
        TopicPath topic = Topic("undecodable");
        _publisher.CreateTopic(topic, schema);
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
    [Fact]
    public void UnsubscribingFromTheHandlerDuringARowLimitFlushStopsDelivery()
    {
        TopicPath topic = Declared("selfcancel");
        int delivered = 0;
        Subscription? self = null;
        SubscribeResult result = _subscriber.SubscribeBatched(topic, (batch, _, _) =>
        {
            batch?.Dispose();
            delivered++;
            _subscriber.Unsubscribe(self!);
        }, Options(1, Long));
        self = result.Subscription;
        result.Schema.Dispose();

        Publish(topic, 1, "a");
        Assert.Equal(1, delivered);

        Publish(topic, 2, "b");
        Publish(topic, 3, "c");
        Assert.Equal(1, delivered);
    }

    /// <summary>Mirrors SubscriberArrowBatchTest.UnsubscribeFromInsideATimeoutFlushIsSafe.</summary>
    [Fact]
    public void UnsubscribingFromInsideATimeoutFlushIsSafe()
    {
        TopicPath topic = Declared("timercancel");
        using var done = new ManualResetEventSlim(false);
        Subscription? self = null;
        Exception? failure = null;
        SubscribeResult result = _subscriber.SubscribeBatched(topic, (batch, _, _) =>
        {
            batch?.Dispose();
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
        Assert.False(self.IsLive);
    }

    /// <summary>Mirrors SubscriberArrowBatchTest.BatchesAreValidArrow.</summary>
    /// <remarks>
    /// Weaker: Apache.Arrow for .NET has no ValidateFull, so the batch's shape is
    /// checked by hand - every column the batch's length, every value where it was put.
    /// </remarks>
    [Fact]
    public void BatchesAreWellFormed()
    {
        TopicPath topic = Declared("wellformed");
        _subscriber.SubscribeBatched(topic, _sink.Handler, Options(5, Long)).Schema.Dispose();

        for (int i = 0; i < 5; i++)
        {
            Publish(topic, i, "t" + i);
        }

        RecordBatch batch = Assert.Single(_sink.Snapshot()).Batch!;
        Assert.Equal(5, batch.Length);
        Assert.All(batch.Arrays, column => Assert.Equal(5, column.Length));
        for (int i = 0; i < 5; i++)
        {
            Assert.Equal(i, ((Int32Array)batch.Column(0)).GetValue(i));
            Assert.Equal("t" + i, ((StringArray)batch.Column(1)).GetString(i));
        }
    }

    /// <summary>Mirrors SubscriberArrowBatchTest.ReuseAcrossWindows.</summary>
    [Fact]
    public void WindowsFollowOneAnother()
    {
        TopicPath topic = Declared("windows");
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

    // ── The sink ────────────────────────────────────────────────────────────

    private sealed record Delivery(RecordBatch? Batch, IReadOnlyList<AttachmentsBuilder> Attachments, BatchStatus Status)
    {
        public int Rows => Batch?.Length ?? -1;
    }

    /// <summary>Collects deliveries and disposes the batches it was handed, which it owns.</summary>
    private sealed class Sink : IDisposable
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
