// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// BIND-4c-i: the delivery path, and THE MANAGED END-TO-END the round has owed
// since BIND-2.
//
// Every earlier slice could only test one direction. The publisher's suite could
// say a topic was declared and a call did not throw; the codec's could say what
// bytes it produced. This file is the first place a C# publisher's row is read
// back by a C# subscriber and compared against the codec's own answer - so it is
// the first test in the round that could catch a wire-format disagreement
// introduced by the BINDING rather than by the codec.
//
// `inprocess` delivers SYNCHRONOUSLY on the publishing thread, which is why every
// assertion below runs straight after a publish with no wait. A wait would hide a
// delivery that never happened.
using System;
using System.Buffers;
using System.Collections.Generic;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

using Apache.Arrow;

using Eiva.Fletcher;

using Xunit;

namespace Eiva.Fletcher.Tests;

public class SubscriberTests : IDisposable
{
    private readonly PubSubProviderHandle _provider;
    private readonly Publisher _publisher;
    private readonly Subscriber _subscriber;

    public SubscriberTests()
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

    /// <summary>What one delivery carried, COPIED out of the borrowed views.</summary>
    /// <remarks>
    /// The copying is the point rather than convenience: every parameter a handler
    /// receives is borrowed for the duration of the call, so a test asserting
    /// against them after the frame returned would be testing a promise the ABI
    /// never made - and would pass or fail on allocator luck.
    /// </remarks>
    private sealed class Seen
    {
        internal byte[] Row = [];
        internal bool HadSchema;
    }

    private static byte[] EncodeRow(FletcherCodec codec, BoundRows rows, int row)
    {
        var buffer = new ArrayBufferWriter<byte>();
        codec.Encode(rows, row, buffer);
        return buffer.WrittenSpan.ToArray();
    }

    [Fact]
    public void ARowPublishedFromCSharpArrivesAtCSharpAsTheCodecEncodedIt()
    {
        // THE END-TO-END. A binding that mangled the row on either side - a bad
        // span length, an off-by-one in the window, a topic that did not match -
        // shows up here and nowhere earlier.
        RecordBatch batch = CodecFixtures.Scalar();
        using var codec = new FletcherCodec(batch.Schema);
        using BoundRows rows = codec.Bind(batch);

        TopicPath topic = TopicPath.Of("bind", "e2e");
        _publisher.CreateTopic(topic, batch.Schema);

        var seen = new List<Seen>();
        SubscribeResult result = _subscriber.Subscribe(topic, (row, schema, attachments) =>
        {
            seen.Add(new Seen { Row = row.ToArray(), HadSchema = !schema.IsNull });
        });

        using (result.Schema)
        {
            _publisher.Publish(topic, rows, 0);

            Assert.Single(seen);
            Assert.Equal(EncodeRow(codec, rows, 0), seen[0].Row);
            Assert.True(seen[0].HadSchema);

            _publisher.Publish(topic, rows, 1);
            Assert.Equal(2, seen.Count);
            Assert.Equal(EncodeRow(codec, rows, 1), seen[1].Row);

            result.Subscription.Dispose();
        }
    }

    [Fact]
    public void TheArrivalCarriesTheDeclaredSchemaAndItImportsAsArrow()
    {
        // D-BIND-46's reason for existing, exercised: ToArrowSchema needs a deep
        // copy from the shim, because importing the SHARED pointer would run the
        // Arrow release callback on a schema the provider is still delivering on.
        RecordBatch batch = CodecFixtures.Scalar();
        TopicPath topic = TopicPath.Of("bind", "schema");
        _publisher.CreateTopic(topic, batch.Schema);

        SubscribeResult result = _subscriber.Subscribe(topic, (_, _, _) => { });

        using (result.Schema)
        {
            SchemaWaitResult wait = result.Schema.Wait(TimeSpan.Zero);

            Assert.Equal(FletcherStatus.Ok, wait.Status);
            Assert.True(wait.HasSchema);
            Assert.NotNull(wait.Schema);

            using (wait.Schema)
            {
                Assert.False(wait.Schema!.IsNull);

                Schema imported = wait.Schema.ToArrowSchema();
                Assert.Equal(batch.Schema.FieldsList.Count, imported.FieldsList.Count);
                Assert.Equal("id", imported.FieldsList[0].Name);
            }
        }

        result.Subscription.Dispose();
    }

    [Fact]
    public void AnImportedSchemaOutlivesTheHandleItCameFrom()
    {
        // The copy is INDEPENDENT. If it shared structure with the shared schema,
        // reading it after the handle went away would be a use-after-free - and a
        // shallow copy would pass an assertion that only read the field count.
        RecordBatch batch = CodecFixtures.Scalar();
        TopicPath topic = TopicPath.Of("bind", "outlive");
        _publisher.CreateTopic(topic, batch.Schema);

        SubscribeResult result = _subscriber.Subscribe(topic, (_, _, _) => { });
        Schema imported;

        using (result.Schema)
        {
            SchemaWaitResult wait = result.Schema.Wait(TimeSpan.Zero);
            imported = wait.Schema!.ToArrowSchema();
            wait.Schema.Dispose();
        }

        result.Subscription.Dispose();

        Assert.Equal("id", imported.FieldsList[0].Name);
        Assert.Equal("id", imported.GetFieldByName("id")!.Name);
    }

    [Fact]
    public void AttachmentsCrossTheDeliveryAndAreReadableByKey()
    {
        RecordBatch batch = CodecFixtures.Scalar();
        using var codec = new FletcherCodec(batch.Schema);
        using BoundRows rows = codec.Bind(batch);

        TopicPath topic = TopicPath.Of("bind", "sidecar");
        _publisher.CreateTopic(topic, batch.Schema);

        using var attachments = new AttachmentsBuilder();
        attachments.Set("trace", [0xAB, 0xCD]);
        attachments.Set("origin", "unit"u8.ToArray());

        var found = new List<(string Key, byte[] Value)>();
        byte[]? byKey = null;

        SubscribeResult result = _subscriber.Subscribe(topic, (_, _, view) =>
        {
            for (int i = 0; i < view.Count; i++)
            {
                found.Add((Encoding.UTF8.GetString(view.KeyAt(i)), view.ValueAt(i).ToArray()));
            }

            if (view.TryFind("trace"u8, out ReadOnlySpan<byte> value))
            {
                byKey = value.ToArray();
            }
        });

        using (result.Schema)
        {
            _publisher.Publish(topic, rows, 0, attachments);
        }

        // Ordered by key BYTES, which is the seam's order and not insertion order.
        Assert.Equal(2, found.Count);
        Assert.Equal("origin", found[0].Key);
        Assert.Equal("trace", found[1].Key);
        Assert.Equal(new byte[] { 0xAB, 0xCD }, byKey);

        result.Subscription.Dispose();
    }

    [Fact]
    public void ARetainedSchemaOutlivesTheDelivery()
    {
        // Everything a handler receives is borrowed for the call. Keeping the
        // schema means retaining it - which is what D-BIND-43 added the call for.
        RecordBatch batch = CodecFixtures.Scalar();
        using var codec = new FletcherCodec(batch.Schema);
        using BoundRows rows = codec.Bind(batch);

        TopicPath topic = TopicPath.Of("bind", "retain");
        _publisher.CreateTopic(topic, batch.Schema);

        SchemaHandle? kept = null;
        SubscribeResult result = _subscriber.Subscribe(topic, (_, schema, _) => kept = schema.Retain());

        using (result.Schema)
        {
            _publisher.Publish(topic, rows, 0);
        }

        result.Subscription.Dispose();

        Assert.NotNull(kept);
        Assert.False(kept!.IsNull);
        Assert.Equal("id", kept.ToArrowSchema().FieldsList[0].Name);
        kept.Dispose();
    }

    [Fact]
    public void CancellingStopsDeliveryAndCancellingTwiceIsANoOp()
    {
        RecordBatch batch = CodecFixtures.Scalar();
        using var codec = new FletcherCodec(batch.Schema);
        using BoundRows rows = codec.Bind(batch);

        TopicPath topic = TopicPath.Of("bind", "cancel");
        _publisher.CreateTopic(topic, batch.Schema);

        int delivered = 0;
        SubscribeResult result = _subscriber.Subscribe(topic, (_, _, _) => delivered++);
        result.Schema.Dispose();

        _publisher.Publish(topic, rows, 0);
        Assert.Equal(1, delivered);

        Assert.True(result.Subscription.IsLive);
        result.Subscription.Dispose();
        Assert.False(result.Subscription.IsLive);

        _publisher.Publish(topic, rows, 1);
        Assert.Equal(1, delivered);

        // A finaliser in a foreign runtime cannot let an exception escape, so
        // cancelling something already cancelled must be a no-op.
        result.Subscription.Dispose();
    }

    [Fact]
    public void AThrowingHandlerIsAbsorbedCountedAndReported()
    {
        // D-BIND-19 RULE 5. There is no caller to throw to - a delivery arrives on
        // a transport thread with nobody waiting - and an exception reaching the C
        // frames terminates the process. So it is absorbed; the count and the
        // event are the only report there can be, and silence would be the actual
        // defect.
        RecordBatch batch = CodecFixtures.Scalar();
        using var codec = new FletcherCodec(batch.Schema);
        using BoundRows rows = codec.Bind(batch);

        TopicPath topic = TopicPath.Of("bind", "faulty");
        _publisher.CreateTopic(topic, batch.Schema);

        var reported = new List<Exception>();
        _subscriber.HandlerFaulted += (_, args) => reported.Add(args.Exception);

        ulong before = _subscriber.AbsorbedCallbackFailures;
        SubscribeResult result = _subscriber.Subscribe(
            topic, (_, _, _) => throw new InvalidOperationException("handler is unhappy"));
        result.Schema.Dispose();

        // The publish itself SUCCEEDS: the handler's failure is not the
        // publisher's, and the row did go out.
        _publisher.Publish(topic, rows, 0);

        Assert.Equal(before + 1, _subscriber.AbsorbedCallbackFailures);
        Assert.Single(reported);
        Assert.Equal("handler is unhappy", reported[0].Message);

        result.Subscription.Dispose();
    }

    [Fact]
    public void OneFaultyHandlerDoesNotStopAnother()
    {
        // The fan-out must not abort at the first failure, or one bad subscriber
        // silently starves every one registered after it.
        RecordBatch batch = CodecFixtures.Scalar();
        using var codec = new FletcherCodec(batch.Schema);
        using BoundRows rows = codec.Bind(batch);

        TopicPath topic = TopicPath.Of("bind", "fanout");
        _publisher.CreateTopic(topic, batch.Schema);

        int good = 0;
        SubscribeResult bad = _subscriber.Subscribe(topic, (_, _, _) => throw new InvalidOperationException("no"));
        SubscribeResult ok = _subscriber.Subscribe(topic, (_, _, _) => good++);
        bad.Schema.Dispose();
        ok.Schema.Dispose();

        _publisher.Publish(topic, rows, 0);

        Assert.Equal(1, good);
        Assert.Equal(1UL, _subscriber.AbsorbedCallbackFailures);

        bad.Subscription.Dispose();
        ok.Subscription.Dispose();
    }

    [Fact]
    public void TwoSubscriptionsToOneTopicEachGetTheirOwnDelivery()
    {
        RecordBatch batch = CodecFixtures.Scalar();
        using var codec = new FletcherCodec(batch.Schema);
        using BoundRows rows = codec.Bind(batch);

        TopicPath topic = TopicPath.Of("bind", "two");
        _publisher.CreateTopic(topic, batch.Schema);

        int first = 0;
        int second = 0;
        SubscribeResult a = _subscriber.Subscribe(topic, (_, _, _) => first++);
        SubscribeResult b = _subscriber.Subscribe(topic, (_, _, _) => second++);
        a.Schema.Dispose();
        b.Schema.Dispose();

        _publisher.Publish(topic, rows, 0);
        Assert.Equal(1, first);
        Assert.Equal(1, second);

        a.Subscription.Dispose();
        _publisher.Publish(topic, rows, 1);
        Assert.Equal(1, first);
        Assert.Equal(2, second);

        b.Subscription.Dispose();
    }

    [Fact]
    public void ANegativeWaitIsRefusedAndInfiniteIsAccepted()
    {
        // D-BIND-20 in code: .NET's own spelling of "forever" maps to the seam's
        // unbounded form, and every OTHER negative is refused here rather than
        // being given a meaning one binding invented.
        RecordBatch batch = CodecFixtures.Scalar();
        TopicPath topic = TopicPath.Of("bind", "timeout");
        _publisher.CreateTopic(topic, batch.Schema);

        SubscribeResult result = _subscriber.Subscribe(topic, (_, _, _) => { });

        using (result.Schema)
        {
            Assert.Throws<ArgumentOutOfRangeException>(
                () => result.Schema.Wait(TimeSpan.FromMilliseconds(-5)));

            // The schema is already there, so this returns at once rather than
            // testing anyone's patience.
            SchemaWaitResult wait = result.Schema.Wait(Timeout.InfiniteTimeSpan);
            Assert.Equal(FletcherStatus.Ok, wait.Status);
            wait.Schema?.Dispose();
        }

        result.Subscription.Dispose();
    }

    [Fact]
    public void DisposingTheSubscriberCancelsWhatIsStillLive()
    {
        RecordBatch batch = CodecFixtures.Scalar();
        using var codec = new FletcherCodec(batch.Schema);
        using BoundRows rows = codec.Bind(batch);

        PubSubProviderHandle provider =
            ProviderRegistry.Create(ProviderSelector.Parse("inprocess"), new ProviderConfig());
        using var publisher = new Publisher(provider);
        var subscriber = new Subscriber(provider);

        TopicPath topic = TopicPath.Of("bind", "teardown");
        publisher.CreateTopic(topic, batch.Schema);

        int delivered = 0;
        SubscribeResult result = subscriber.Subscribe(topic, (_, _, _) => delivered++);
        result.Schema.Dispose();

        publisher.Publish(topic, rows, 0);
        Assert.Equal(1, delivered);

        subscriber.Dispose();
        subscriber.Dispose();

        publisher.Publish(topic, rows, 1);
        Assert.Equal(1, delivered);

        provider.Dispose();
    }

    [Fact]
    public void OnASchemaLessTransportTheArrivalAnswersOkWithNoSchema()
    {
        // THE DISTINCTION §7 EXISTS TO PROTECT, and this row was first written
        // asserting the WRONG half of it. `inprocess` defaults to
        // schema_carriage=as_declared: the schema is not carried on the wire at
        // all, the declaration is its only source. So subscribing to a topic
        // nobody declared answers Ok WITH A NULL SCHEMA - "this transport carries
        // no schemas; bring your own" - and not Pending.
        //
        // Reading that as a failure would refuse to work on a transport behaving
        // exactly as designed; reading it as "not yet" would wait forever. The
        // next row covers the outcome that really is "not yet".
        TopicPath topic = TopicPath.Of("bind", "nobody");

        SubscribeResult result = _subscriber.Subscribe(topic, (_, _, _) => { });

        using (result.Schema)
        {
            SchemaWaitResult wait = result.Schema.Wait(TimeSpan.Zero);

            Assert.Equal(FletcherStatus.Ok, wait.Status);
            Assert.Null(wait.Schema);
            Assert.False(wait.HasSchema);
        }

        result.Subscription.Dispose();
    }

    [Fact]
    public void OnASchemaCarryingTransportAnUnannouncedTopicIsPending()
    {
        // The outcome the previous row does NOT cover. With the schema carried on
        // the wire, a topic no publisher has announced has a schema that has not
        // arrived yet - which is Pending, an outcome rather than a failure, and
        // the caller may ask again.
        //
        // Two providers are needed to cover both because one provider cannot
        // produce both answers: whether a schema is carried is a property of the
        // transport, not of the topic.
        byte[] document = Encoding.UTF8.GetBytes("schema_carriage=carried");
        using PubSubProviderHandle carrying = ProviderRegistry.Create(
            ProviderSelector.Parse("inprocess"), new ProviderConfig { Document = document });
        using var subscriber = new Subscriber(carrying);

        SubscribeResult result = subscriber.Subscribe(TopicPath.Of("bind", "unannounced"), (_, _, _) => { });

        using (result.Schema)
        {
            SchemaWaitResult wait = result.Schema.Wait(TimeSpan.Zero);

            Assert.Equal(FletcherStatus.Pending, wait.Status);
            Assert.Null(wait.Schema);
            Assert.False(wait.HasSchema);
        }

        result.Subscription.Dispose();
    }

    [Fact]
    public async Task AnInfiniteWaitActuallyWaitsUntilTheSchemaArrives()
    {
        // BIND-4's bullet asks that `Wait(Timeout.InfiniteTimeSpan)` WAITS. The
        // only earlier test of it waited on a schema that already existed, so it
        // returned at once - true of a build that mapped Infinite to "do not wait"
        // just as much as of a correct one (BIND-4 conformance review, bullet 11).
        // Here nothing has announced the topic, so the wait has nothing to return
        // until a publisher declares it: still running after a pause, and done -
        // with the schema - once the declaration lands.
        byte[] document = Encoding.UTF8.GetBytes("schema_carriage=carried");
        using PubSubProviderHandle carrying = ProviderRegistry.Create(
            ProviderSelector.Parse("inprocess"), new ProviderConfig { Document = document });
        using var subscriber = new Subscriber(carrying);
        using var publisher = new Publisher(carrying);

        TopicPath topic = TopicPath.Of("bind", "infinitewait");
        SubscribeResult result = subscriber.Subscribe(topic, (_, _, _) => { });

        using (result.Schema)
        {
            Task<SchemaWaitResult> waiting = Task.Run(() => result.Schema.Wait(Timeout.InfiniteTimeSpan));

            Task first = await Task.WhenAny(waiting, Task.Delay(TimeSpan.FromMilliseconds(200)));
            Assert.NotSame(waiting, first);

            publisher.CreateTopic(topic, CodecFixtures.Scalar().Schema);

            Task done = await Task.WhenAny(waiting, Task.Delay(TimeSpan.FromSeconds(10)));
            Assert.Same(waiting, done);

            SchemaWaitResult wait = await waiting;
            Assert.Equal(FletcherStatus.Ok, wait.Status);
            Assert.True(wait.HasSchema);
            wait.Schema?.Dispose();
        }

        result.Subscription.Dispose();
    }
}
