// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// D-BIND-55: the two members whose observation is PROCESS-WIDE - the finaliser
// on an owned `SchemaHandle`, seen through a static hook, and
// `Diagnostics.AbsorbedTotal`, a static counter.
//
// A process-wide number cannot be bracketed by a test that runs in parallel with
// others (xUnit parallelises across classes), so these run in a collection that
// disables parallelisation: xUnit runs it on its own, after the parallel ones.
// That is what lets the counter test assert an EXACT delta rather than "at least".
using System;
using System.Collections.Concurrent;
using System.Runtime.CompilerServices;

using Apache.Arrow;

using Xunit;

namespace Eiva.Fletcher.Tests;

[CollectionDefinition(Name, DisableParallelization = true)]
public sealed class ProcessWideCollection
{
    public const string Name = "Process-wide state";
}

[Collection(ProcessWideCollection.Name)]
public sealed class ProcessWideTests : IDisposable
{
    private readonly PubSubProviderHandle _provider;
    private readonly Publisher _publisher;
    private readonly Subscriber _subscriber;
    private readonly ConcurrentBag<nint> _finalised = new();

    public ProcessWideTests()
    {
        _provider = ProviderRegistry.Create(ProviderSelector.Parse("inprocess"), new ProviderConfig());
        _publisher = new Publisher(_provider);
        _subscriber = new Subscriber(_provider);
        SchemaHandle.ReleasedByFinalizerForTest = _finalised.Add;
    }

    public void Dispose()
    {
        SchemaHandle.ReleasedByFinalizerForTest = null;
        _subscriber.Dispose();
        _publisher.Dispose();
        _provider.Dispose();
        GC.SuppressFinalize(this);
    }

    private static void CollectEverything()
    {
        GC.Collect();
        GC.WaitForPendingFinalizers();
        GC.Collect();
    }

    private TopicPath Declared(string name)
    {
        TopicPath topic = TopicPath.Of("bind", "processwide", name, Guid.NewGuid().ToString("N"));
        _publisher.CreateTopic(topic, CodecFixtures.Scalar().Schema);
        return topic;
    }

    /// <summary>Takes an OWNED reference and drops it undisposed; returns its owner.</summary>
    /// <remarks>
    /// Its own frame, and not inlined, so nothing in the caller keeps the handle
    /// reachable when the collector runs.
    /// </remarks>
    [MethodImpl(MethodImplOptions.NoInlining)]
    private nint LeakAnOwnedHandle(TopicPath topic)
    {
        SubscribeResult result = _subscriber.Subscribe(topic, (_, _, _) => { });
        using (result.Schema)
        {
            SchemaHandle owned = result.Schema.Wait(TimeSpan.Zero).Schema!;
            nint owner = owned.OwnerForTest;
            Assert.NotEqual(0, owner);
            result.Subscription.Dispose();
            return owner;
        }
    }

    /// <summary>An owned handle nobody disposed is released by its finaliser.</summary>
    /// <remarks>Fails against a SchemaHandle with no finaliser: the hook never sees it.</remarks>
    [Fact]
    public void AnUndisposedOwnedSchemaIsReleasedByTheFinaliser()
    {
        nint owner = LeakAnOwnedHandle(Declared("owned"));

        CollectEverything();

        Assert.Contains(owner, _finalised);
    }

    /// <summary>A disposed handle is not released a second time by its finaliser.</summary>
    /// <remarks>Fails against a Dispose that does not suppress finalisation.</remarks>
    [Fact]
    public void ADisposedSchemaIsNotReleasedAgain()
    {
        nint owner = DisposeAnOwnedHandle(Declared("disposed"));

        CollectEverything();

        Assert.DoesNotContain(owner, _finalised);
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private nint DisposeAnOwnedHandle(TopicPath topic)
    {
        SubscribeResult result = _subscriber.Subscribe(topic, (_, _, _) => { });
        using (result.Schema)
        {
            SchemaHandle owned = result.Schema.Wait(TimeSpan.Zero).Schema!;
            nint owner = owned.OwnerForTest;
            owned.Dispose();
            result.Subscription.Dispose();
            return owner;
        }
    }

    /// <summary>The schema a delivery LENDS a handler is never released by a finaliser.</summary>
    /// <remarks>
    /// Releasing it would drop the reference the shim holds for the call, and the
    /// schema would die under the provider still delivering on it. Fails against a
    /// finaliser that does not distinguish borrowed from owned.
    /// </remarks>
    [Fact]
    public void ABorrowedSchemaIsNeverReleasedByAFinaliser()
    {
        nint owner = DeliverOnceAndDropTheBorrowedHandle(Declared("borrowed"));
        Assert.NotEqual(0, owner);

        CollectEverything();

        Assert.DoesNotContain(owner, _finalised);
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private nint DeliverOnceAndDropTheBorrowedHandle(TopicPath topic)
    {
        RecordBatch batch = CodecFixtures.Scalar();
        using var codec = new FletcherCodec(batch.Schema);
        using BoundRows rows = codec.Bind(batch);

        nint owner = 0;
        SubscribeResult result = _subscriber.Subscribe(topic, (_, schema, _) => owner = schema.OwnerForTest);
        result.Schema.Dispose();

        _publisher.Publish(topic, rows, 0);
        result.Subscription.Dispose();
        return owner;
    }

    /// <summary>AbsorbedTotal moves by exactly what the Subscribers absorbed.</summary>
    /// <remarks>
    /// Two routes absorb: a handler that throws, and then a HandlerFaulted listener
    /// that throws too, which counts again. The process total must track the
    /// per-instance counter through both. Fails against a Subscriber that does not
    /// report to Diagnostics.
    /// </remarks>
    [Fact]
    public void AbsorbedTotalMovesByExactlyWhatTheSubscribersAbsorbed()
    {
        RecordBatch batch = CodecFixtures.Scalar();
        using var codec = new FletcherCodec(batch.Schema);
        using BoundRows rows = codec.Bind(batch);
        TopicPath topic = Declared("absorbed");

        ulong before = Diagnostics.AbsorbedTotal;

        SubscribeResult result = _subscriber.Subscribe(topic, (_, _, _) => throw new InvalidOperationException("handler"));
        result.Schema.Dispose();

        _publisher.Publish(topic, rows, 0);
        _publisher.Publish(topic, rows, 1);

        _subscriber.HandlerFaulted += (_, _) => throw new InvalidOperationException("listener");
        _publisher.Publish(topic, rows, 0);

        result.Subscription.Dispose();

        Assert.Equal(4UL, _subscriber.AbsorbedCallbackFailures);
        Assert.Equal(_subscriber.AbsorbedCallbackFailures, Diagnostics.AbsorbedTotal - before);
    }
}
