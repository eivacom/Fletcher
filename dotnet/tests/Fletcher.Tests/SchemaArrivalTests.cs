// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// D-BIND-55: the two members the public surface promised on the arrival and
// BIND-4 did not ship - `SchemaWaitResult.IsSchemaless` and
// `SchemaArrival.WaitAsync`.
//
// Every outcome below is produced by a real provider rather than built by hand.
// `inprocess` gives all three shapes from one binary, and which one depends on its
// configuration: by default it carries no schema on the wire, so an undeclared
// topic answers Ok with NO schema; with `schema_carriage=carried` the same topic
// answers Pending until a publisher declares it.
using System;
using System.Diagnostics;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

using Xunit;

namespace Eiva.Fletcher.Tests;

public sealed class SchemaArrivalTests
{
    private static readonly byte[] Carried = Encoding.UTF8.GetBytes("schema_carriage=carried");

    private static PubSubProviderHandle Provider(bool carried) => ProviderRegistry.Create(
        ProviderSelector.Parse("inprocess"),
        carried ? new ProviderConfig { Document = Carried } : new ProviderConfig());

    private static TopicPath Unique(string name) => TopicPath.Of("bind", "arrival", name, Guid.NewGuid().ToString("N"));

    public enum Shape
    {
        SchemaArrived,
        Schemaless,
        Pending,
    }

    /// <summary>IsSchemaless is true for exactly one outcome, and it is not !HasSchema.</summary>
    /// <remarks>
    /// The Pending row is the one that matters: HasSchema is false there too, so a
    /// caller who read <c>!HasSchema</c> as "this transport carries no schemas" would
    /// be wrong on it, silently. An <c>IsSchemaless => !HasSchema</c> fails here.
    /// </remarks>
    [Theory]
    [InlineData(Shape.SchemaArrived, false, true)]
    [InlineData(Shape.Schemaless, true, false)]
    [InlineData(Shape.Pending, false, false)]
    public void IsSchemalessIsTrueOnlyForTheSchemalessAnswer(Shape shape, bool schemaless, bool hasSchema)
    {
        using PubSubProviderHandle provider = Provider(carried: shape == Shape.Pending);
        using var publisher = new Publisher(provider);
        using var subscriber = new Subscriber(provider);

        TopicPath topic = Unique("shape");
        if (shape == Shape.SchemaArrived)
        {
            publisher.CreateTopic(topic, CodecFixtures.Scalar().Schema);
        }

        SubscribeResult result = subscriber.Subscribe(topic, (_, _, _) => { });
        using (result.Schema)
        {
            SchemaWaitResult wait = result.Schema.Wait(TimeSpan.Zero);
            using (wait.Schema)
            {
                Assert.Equal(shape == Shape.Pending ? FletcherStatus.Pending : FletcherStatus.Ok, wait.Status);
                Assert.Equal(schemaless, wait.IsSchemaless);
                Assert.Equal(hasSchema, wait.HasSchema);
            }
        }

        result.Subscription.Dispose();
    }

    [Fact]
    public async Task WaitAsyncReturnsASchemaThatIsAlreadyThere()
    {
        using PubSubProviderHandle provider = Provider(carried: false);
        using var publisher = new Publisher(provider);
        using var subscriber = new Subscriber(provider);

        TopicPath topic = Unique("there");
        publisher.CreateTopic(topic, CodecFixtures.Scalar().Schema);
        SubscribeResult result = subscriber.Subscribe(topic, (_, _, _) => { });

        using (result.Schema)
        {
            SchemaWaitResult wait = await result.Schema.WaitAsync(TimeSpan.FromSeconds(10));
            using (wait.Schema)
            {
                Assert.True(wait.HasSchema);
                Assert.Equal("id", wait.Schema!.ToArrowSchema().FieldsList[0].Name);
            }
        }

        result.Subscription.Dispose();
    }

    [Fact]
    public async Task WaitAsyncAnswersTheSchemalessTransportAsWaitDoes()
    {
        using PubSubProviderHandle provider = Provider(carried: false);
        using var subscriber = new Subscriber(provider);

        SubscribeResult result = subscriber.Subscribe(Unique("schemaless"), (_, _, _) => { });
        using (result.Schema)
        {
            SchemaWaitResult wait = await result.Schema.WaitAsync(Timeout.InfiniteTimeSpan);
            Assert.True(wait.IsSchemaless);
            Assert.Null(wait.Schema);
        }

        result.Subscription.Dispose();
    }

    /// <summary>An infinite WaitAsync keeps waiting past a slice, and ends when the schema lands.</summary>
    /// <remarks>
    /// Fails against a helper that returns after one slice: that one completes,
    /// Pending, long before the declaration below.
    /// </remarks>
    [Fact]
    public async Task AnInfiniteWaitAsyncWaitsAcrossSlicesUntilTheSchemaArrives()
    {
        using PubSubProviderHandle provider = Provider(carried: true);
        using var publisher = new Publisher(provider);
        using var subscriber = new Subscriber(provider);

        TopicPath topic = Unique("infinite");
        SubscribeResult result = subscriber.Subscribe(topic, (_, _, _) => { });

        using (result.Schema)
        {
            Task<SchemaWaitResult> waiting = result.Schema.WaitAsync(Timeout.InfiniteTimeSpan);

            // Several slices long, so a single-slice helper has visibly given up.
            Task first = await Task.WhenAny(waiting, Task.Delay(SchemaArrival.AsyncSlice * 6));
            Assert.NotSame(waiting, first);

            publisher.CreateTopic(topic, CodecFixtures.Scalar().Schema);

            Task done = await Task.WhenAny(waiting, Task.Delay(TimeSpan.FromSeconds(10)));
            Assert.Same(waiting, done);

            SchemaWaitResult wait = await waiting;
            using (wait.Schema)
            {
                Assert.True(wait.HasSchema);
            }
        }

        result.Subscription.Dispose();
    }

    /// <summary>A finite WaitAsync answers Pending, and not before its timeout.</summary>
    [Fact]
    public async Task AFiniteWaitAsyncAnswersPendingAtItsTimeoutAndNotBefore()
    {
        using PubSubProviderHandle provider = Provider(carried: true);
        using var subscriber = new Subscriber(provider);

        SubscribeResult result = subscriber.Subscribe(Unique("finite"), (_, _, _) => { });
        using (result.Schema)
        {
            TimeSpan timeout = SchemaArrival.AsyncSlice * 6;
            var clock = Stopwatch.StartNew();

            SchemaWaitResult wait = await result.Schema.WaitAsync(timeout);

            Assert.Equal(FletcherStatus.Pending, wait.Status);
            Assert.False(wait.IsSchemaless);

            // Millisecond truncation in the native wait can land a hair early, never
            // a slice early.
            Assert.True(clock.Elapsed >= timeout - TimeSpan.FromMilliseconds(20),
                $"answered Pending after {clock.Elapsed.TotalMilliseconds} ms of a {timeout.TotalMilliseconds} ms wait");
        }

        result.Subscription.Dispose();
    }

    /// <summary>Cancelling ends the task as cancelled, promptly, and cancels nothing native.</summary>
    /// <remarks>
    /// Fails against a helper that ignores the token: that one waits forever, and
    /// the bounded wait below catches it rather than the test run hanging - the
    /// subscription's disposal at the end then ends the stray wait with
    /// SubscriptionEnded.
    /// </remarks>
    [Fact]
    public async Task CancellingAWaitAsyncEndsItAsCancelledAndLeavesTheArrivalWaitable()
    {
        using PubSubProviderHandle provider = Provider(carried: true);
        using var subscriber = new Subscriber(provider);

        SubscribeResult result = subscriber.Subscribe(Unique("cancel"), (_, _, _) => { });
        using (result.Schema)
        {
            using var cancel = new CancellationTokenSource();
            Task<SchemaWaitResult> waiting = result.Schema.WaitAsync(Timeout.InfiniteTimeSpan, cancel.Token);

            await Task.Delay(SchemaArrival.AsyncSlice * 2);
            cancel.Cancel();

            Task done = await Task.WhenAny(waiting, Task.Delay(TimeSpan.FromSeconds(5)));
            Assert.Same(waiting, done);
            Assert.True(waiting.IsCanceled, $"the wait ended {waiting.Status}, not Canceled");
            await Assert.ThrowsAnyAsync<OperationCanceledException>(() => waiting);

            // Nothing native was cancelled: the same arrival still answers.
            Assert.Equal(FletcherStatus.Pending, result.Schema.Wait(TimeSpan.Zero).Status);
        }

        result.Subscription.Dispose();
    }

    [Fact]
    public void AnAlreadyCancelledTokenIsCancelledAtOnce()
    {
        using PubSubProviderHandle provider = Provider(carried: true);
        using var subscriber = new Subscriber(provider);

        SubscribeResult result = subscriber.Subscribe(Unique("precancelled"), (_, _, _) => { });
        using (result.Schema)
        {
            Task<SchemaWaitResult> waiting = result.Schema.WaitAsync(Timeout.InfiniteTimeSpan, new CancellationToken(true));
            Assert.True(waiting.IsCanceled);
        }

        result.Subscription.Dispose();
    }

    /// <summary>A refused timeout, or a disposed arrival, throws on the CALLER's stack.</summary>
    /// <remarks>
    /// Not through the task: a caller who never awaits would otherwise never learn
    /// the timeout was refused. Fails against a helper that validates inside the
    /// thread-pool work.
    /// </remarks>
    [Fact]
    public void ARefusedTimeoutAndADisposedArrivalThrowSynchronously()
    {
        using PubSubProviderHandle provider = Provider(carried: true);
        using var subscriber = new Subscriber(provider);

        SubscribeResult result = subscriber.Subscribe(Unique("refused"), (_, _, _) => { });

        // A statement lambda that DISCARDS the task: the assertion is that the call
        // itself throws, before any task exists.
        Assert.Throws<ArgumentOutOfRangeException>(() => { _ = result.Schema.WaitAsync(TimeSpan.FromMilliseconds(-5)); });

        result.Schema.Dispose();
        Assert.Throws<ObjectDisposedException>(() => { _ = result.Schema.WaitAsync(TimeSpan.Zero); });

        result.Subscription.Dispose();
    }
}
