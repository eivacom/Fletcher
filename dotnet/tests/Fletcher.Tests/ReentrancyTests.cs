// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// BIND-4c-ii: what a handler may and may not do from inside its own delivery.
//
// ── Why these refusals are managed rather than native ───────────────────────
// Two of the three acts below have no usable answer once they reach native.
// Disposing a subscriber from a handler ENDS THE PROCESS - the seam's destructor
// reaches the provider's door, is refused with kReentrantCall, and rethrows out
// of a noexcept destructor, which is the designed answer to a forbidden act
// rather than an accident. Subscribing to a new topic is refused with
// kReentrantCall from inside a transport callback, where the caller is a handler
// with nowhere to put a status. Refusing both in managed code turns a process
// death and an opaque status into an exception naming the route that works.
//
// The third act is NOT refused, and testing that is as important as testing the
// refusals: Unsubscribe from inside a handler is the seam's documented carve-out
// and is served. A binding that refused it would break the ordinary "cancel
// myself when I see the last message" pattern.
//
// ── The row this file exists for ────────────────────────────────────────────
// `ASelfCancellingHandlerIsServedAndTheDeliveryFreesTheHandle` reaches the
// branch 4c-i could not: the re-entrant path where native does NOT wait, the
// in-flight count is still 1 when the subscription retires, and the DEPARTING
// DELIVERY frees the GCHandle rather than the canceller. That is the branch
// where a bug is a use-after-free on a transport thread, and until now it was
// reasoned about and never executed.
using System;
using System.Collections.Generic;
using System.Threading.Tasks;

using Apache.Arrow;

using Eiva.Fletcher;

using Xunit;

namespace Eiva.Fletcher.Tests;

public class ReentrancyTests : IDisposable
{
    private readonly PubSubProviderHandle _provider;
    private readonly Publisher _publisher;
    private readonly Subscriber _subscriber;
    private readonly RecordBatch _batch = CodecFixtures.Scalar();
    private readonly FletcherCodec _codec;
    private readonly BoundRows _rows;

    public ReentrancyTests()
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
        TopicPath topic = TopicPath.Of("bind", name);
        _publisher.CreateTopic(topic, _batch.Schema);
        return topic;
    }

    /// <summary>The exceptions the thunk absorbed, in order.</summary>
    private List<Exception> Absorbed()
    {
        var faults = new List<Exception>();
        _subscriber.HandlerFaulted += (_, args) => faults.Add(args.Exception);
        return faults;
    }

    [Fact]
    public void ASelfCancellingHandlerIsServedAndTheDeliveryFreesTheHandle()
    {
        // THE BRANCH 4c-i COULD NOT REACH. A cancellation from inside a delivery
        // does not wait - it cannot wait for the frame it is already in - so when
        // the subscription retires the in-flight count is still 1 and the
        // canceller frees nothing. The departing delivery does it instead.
        //
        // AND THE BRANCH IS ASSERTED, not assumed. Both paths end with the handle
        // freed and delivery stopped, so a test checking only those outcomes
        // passes whichever one ran - which would make this row look like coverage
        // of the dangerous branch while actually covering the safe one. The
        // counters say which. The loop is deliberate on top of that: repeating is
        // what would surface a double free or a handle released while native still
        // held it.
        TopicPath topic = Declare("selfcancel");
        List<Exception> faults = Absorbed();

        long deliveryBefore = _subscriber.FreedByDeliveryCount;
        long cancellerBefore = _subscriber.FreedByCancellerCount;

        const int Rounds = 25;
        for (int round = 0; round < Rounds; round++)
        {
            int delivered = 0;
            SubscribeResult result = default;

            result = _subscriber.Subscribe(topic, (_, _, _) =>
            {
                delivered++;

                // The carve-out: served, not refused, and it does not block.
                result.Subscription.Dispose();
            });
            result.Schema.Dispose();

            _publisher.Publish(topic, _rows, 0);
            Assert.Equal(1, delivered);
            Assert.False(result.Subscription.IsLive);

            // No further delivery begins for a cancelled subscription.
            _publisher.Publish(topic, _rows, 1);
            Assert.Equal(1, delivered);
        }

        Assert.Empty(faults);

        // EVERY round freed on the delivery's way out, and none on the
        // canceller's. If the seam ever started draining a re-entrant cancel, this
        // is the assertion that would notice - and the lifetime rule would need
        // rereading rather than the test relaxing.
        Assert.Equal(Rounds, _subscriber.FreedByDeliveryCount - deliveryBefore);
        Assert.Equal(0, _subscriber.FreedByCancellerCount - cancellerBefore);
    }

    [Fact]
    public void AnOrdinaryCancellationFreesOnTheCancellingThreadInstead()
    {
        // The other branch, asserted the same way, so the pair is a real contrast
        // rather than one test and an assumption. Nothing is in flight here, so
        // native's Unsubscribe returns with the count already at zero and the
        // cancelling thread frees.
        TopicPath topic = Declare("ordinarycancel");

        long deliveryBefore = _subscriber.FreedByDeliveryCount;
        long cancellerBefore = _subscriber.FreedByCancellerCount;

        SubscribeResult result = _subscriber.Subscribe(topic, (_, _, _) => { });
        result.Schema.Dispose();
        _publisher.Publish(topic, _rows, 0);

        result.Subscription.Dispose();

        Assert.Equal(1, _subscriber.FreedByCancellerCount - cancellerBefore);
        Assert.Equal(0, _subscriber.FreedByDeliveryCount - deliveryBefore);
    }

    [Fact]
    public void DisposingTheSubscriberFromInsideAHandlerIsRefusedInManagedCode()
    {
        // Reaching native here would END THE PROCESS rather than fail a test, so
        // the refusal is the only thing standing between a handler's mistake and a
        // dead service. The exception is raised inside the handler, so it comes
        // back through the absorbed-failure channel rather than to a caller -
        // there is no caller on a transport thread.
        TopicPath topic = Declare("selfdispose");
        List<Exception> faults = Absorbed();

        SubscribeResult result = _subscriber.Subscribe(topic, (_, _, _) => _subscriber.Dispose());
        result.Schema.Dispose();

        _publisher.Publish(topic, _rows, 0);

        Exception fault = Assert.Single(faults);
        Assert.IsType<InvalidOperationException>(fault);
        Assert.Contains("quiescence", fault.Message, StringComparison.Ordinal);
        Assert.Contains("DispatchAfterDelivery", fault.Message, StringComparison.Ordinal);

        // AND THE SUBSCRIBER IS STILL USABLE: the refusal happened before any
        // teardown began, so it did not leave a half-disposed object behind. A
        // guard placed after `_disposed = true` would pass the assertion above and
        // fail this one.
        result.Subscription.Dispose();

        SubscribeResult again = _subscriber.Subscribe(Declare("stillworks"), (_, _, _) => { });
        again.Schema.Dispose();
        Assert.True(again.Subscription.IsLive);
        again.Subscription.Dispose();
    }

    [Fact]
    public void SubscribingToANewTopicFromInsideAHandlerIsRefusedAndNamesTheRoute()
    {
        TopicPath topic = Declare("resubscribe");
        TopicPath other = Declare("somewhereelse");
        List<Exception> faults = Absorbed();

        SubscribeResult result = _subscriber.Subscribe(topic, (_, _, _) =>
        {
            SubscribeResult nested = _subscriber.Subscribe(other, (_, _, _) => { });
            nested.Schema.Dispose();
        });
        result.Schema.Dispose();

        _publisher.Publish(topic, _rows, 0);

        Exception fault = Assert.Single(faults);
        Assert.IsType<InvalidOperationException>(fault);
        Assert.Contains("DispatchAfterDelivery", fault.Message, StringComparison.Ordinal);
        Assert.Contains("bind/somewhereelse", fault.Message, StringComparison.Ordinal);

        result.Subscription.Dispose();
    }

    [Fact]
    public void SubscribingToATopicThisSubscriberAlreadyHoldsIsServedFromInsideAHandler()
    {
        // THE OTHER HALF, and the reason the managed check asks about the TOPIC
        // rather than simply refusing every re-entrant Subscribe. Joining a topic
        // this Subscriber has already subscribed is answered from the cached
        // arrival and touches no provider, so the seam serves it - and a binding
        // that refused it would be stricter than the seam for no reason, which is
        // the unguarded direction D-BIND-20's note warns about.
        TopicPath topic = Declare("already");
        List<Exception> faults = Absorbed();

        SubscribeResult first = _subscriber.Subscribe(topic, (_, _, _) => { });
        first.Schema.Dispose();

        Subscription? joined = null;
        SubscribeResult outer = _subscriber.Subscribe(topic, (_, _, _) =>
        {
            if (joined is null)
            {
                SubscribeResult nested = _subscriber.Subscribe(topic, (_, _, _) => { });
                nested.Schema.Dispose();
                joined = nested.Subscription;
            }
        });
        outer.Schema.Dispose();

        _publisher.Publish(topic, _rows, 0);

        Assert.Empty(faults);
        Assert.NotNull(joined);
        Assert.True(joined!.IsLive);

        joined.Dispose();
        outer.Subscription.Dispose();
        first.Subscription.Dispose();
    }

    [Fact]
    public async Task DispatchAfterDeliveryRunsWorkThatWouldBeRefusedInline()
    {
        // The route the refusals name, doing the thing they refuse. The work runs
        // on the thread pool, where the seam's rule does not apply - a call from
        // ANOTHER thread is served and simply blocks until the delivery in flight
        // has returned.
        //
        // Note what this test does NOT do: await inside the handler. That is a
        // deadlock by construction, because the work waits at the provider's door
        // for a delivery that is waiting for the work.
        TopicPath topic = Declare("dispatch");
        TopicPath target = Declare("deferred");
        List<Exception> faults = Absorbed();

        Task? deferred = null;
        Subscription? made = null;

        SubscribeResult result = _subscriber.Subscribe(topic, (_, _, _) =>
        {
            deferred ??= _subscriber.DispatchAfterDelivery(() =>
            {
                SubscribeResult nested = _subscriber.Subscribe(target, (_, _, _) => { });
                nested.Schema.Dispose();
                made = nested.Subscription;
                return Task.CompletedTask;
            });
        });
        result.Schema.Dispose();

        _publisher.Publish(topic, _rows, 0);

        Assert.NotNull(deferred);
        await deferred!;

        Assert.Empty(faults);
        Assert.NotNull(made);
        Assert.True(made!.IsLive);

        made.Dispose();
        result.Subscription.Dispose();
    }

    [Fact]
    public async Task AFailureInDeferredWorkLandsOnTheTaskRatherThanTheAbsorbedCount()
    {
        // Deferred work HAS a caller who can observe it, which is exactly what a
        // delivery does not have. So its failure belongs on the task, and counting
        // it as an absorbed handler failure would file it under the one channel
        // that exists because nobody is listening.
        Task work = _subscriber.DispatchAfterDelivery(
            () => throw new InvalidOperationException("deferred work failed"));

        InvalidOperationException thrown =
            await Assert.ThrowsAsync<InvalidOperationException>(async () => await work);

        Assert.Equal("deferred work failed", thrown.Message);
        Assert.Equal(0UL, _subscriber.AbsorbedCallbackFailures);
    }

    [Fact]
    public void TheMarkerIsRestoredSoTheSameActsAreAllowedAfterTheHandlerReturns()
    {
        // The marker is per-THREAD and saved/restored rather than cleared. If it
        // leaked, the first delivery on a pool thread would poison that thread for
        // every later call - and the failure would look like an unrelated refusal
        // much later, on work that never went near a handler.
        TopicPath topic = Declare("restored");
        List<Exception> faults = Absorbed();

        SubscribeResult result = _subscriber.Subscribe(topic, (_, _, _) => { });
        result.Schema.Dispose();

        _publisher.Publish(topic, _rows, 0);
        Assert.Empty(faults);

        // Both acts are refused INSIDE a handler and must be ordinary outside one,
        // on the very thread that just ran the delivery.
        SubscribeResult after = _subscriber.Subscribe(Declare("afterwards"), (_, _, _) => { });
        after.Schema.Dispose();
        Assert.True(after.Subscription.IsLive);

        after.Subscription.Dispose();
        result.Subscription.Dispose();

        var throwaway = new Subscriber(_provider);
        throwaway.Dispose();
    }
}
