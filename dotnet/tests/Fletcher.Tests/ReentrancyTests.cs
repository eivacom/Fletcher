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
        long deferredBefore = _subscriber.FreedDeferredCount;

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

        // A self-cancel also queues D-BIND-50's deferred free, and it must never be
        // the branch that wins here: its cancel waits for the drain, the drain
        // includes this delivery's gate, and the delivery frees on its way out
        // before releasing that gate. Race-free by the seam's guarantee, so a
        // deferred free counted here would be an ordering bug, not flakiness.
        Assert.Equal(0, _subscriber.FreedDeferredCount - deferredBefore);
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

        // A subscriber over the SAME provider, disposed on the thread that just
        // ran a delivery. Since D-BIND-51 the refusal is keyed on the provider, so
        // a delivery frame that leaked on this thread would refuse THIS dispose -
        // which the first version, keyed on the Subscriber object, could not see.
        var throwaway = new Subscriber(_provider);
        throwaway.Dispose();
    }

    // ── D-BIND-50: a carve-out cancel frees only after a drain ──────────────

    [Fact]
    public async Task CancellingASiblingFromInsideAHandlerDefersTheFreeUntilADrainHasBeenWaitedFor()
    {
        // THE BUG, and why this test asserts a BRANCH rather than a crash. A
        // handler that cancels a sibling gets no drain from native - a cancel from
        // inside a delivery cannot wait - and the first version freed the
        // sibling's GCHandle on the spot because its in-flight count read zero.
        // That count is incremented inside the thunk, AFTER the seam's gate, so a
        // sibling delivery between the two on another thread read a freed handle.
        // That window lives in native code and the reverse-P/Invoke transition,
        // where managed code cannot hold it open on demand - so the test pins the
        // RULE that closes it instead: on the carve-out nothing is freed inline,
        // and the free arrives later, from the deferred branch, once a second
        // cancel has waited for the drain. The first version fails the first
        // assertion (it freed on the canceller branch).
        TopicPath trigger = Declare("siblingtrigger");
        TopicPath idle = Declare("siblingidle");
        List<Exception> faults = Absorbed();

        int siblingDelivered = 0;
        SubscribeResult sibling = _subscriber.Subscribe(idle, (_, _, _) => siblingDelivered++);
        sibling.Schema.Dispose();

        long deliveryBefore = _subscriber.FreedByDeliveryCount;
        long cancellerBefore = _subscriber.FreedByCancellerCount;
        long deferredBefore = _subscriber.FreedDeferredCount;

        SubscribeResult canceller = _subscriber.Subscribe(trigger, (_, _, _) => sibling.Subscription.Dispose());
        canceller.Schema.Dispose();

        _publisher.Publish(trigger, _rows, 0);

        Assert.Empty(faults);
        Assert.False(sibling.Subscription.IsLive);

        // Not freed inline, on either immediate branch. Independent of when the
        // deferred work runs, so this assertion has no race in it.
        Assert.Equal(0, _subscriber.FreedByCancellerCount - cancellerBefore);
        Assert.Equal(0, _subscriber.FreedByDeliveryCount - deliveryBefore);

        // Freed exactly once, by the deferred branch.
        Assert.True(
            await Eventually(() => _subscriber.FreedDeferredCount - deferredBefore == 1),
            "the carve-out's deferred free never ran");
        Assert.Equal(1, _subscriber.FreedDeferredCount - deferredBefore);

        // And the cancelled sibling stays cancelled.
        _publisher.Publish(idle, _rows, 0);
        Assert.Equal(0, siblingDelivered);

        canceller.Subscription.Dispose();
    }

    // ── D-BIND-51: the refusal is keyed on the provider ─────────────────────

    [Fact]
    public void DisposingAnotherSubscriberOnTheSameProviderFromAHandlerIsRefused()
    {
        // THE CASE THE SEAM'S OWN COMMENT NAMES: "a handler on subscriber X
        // destroying subscriber Y over the same provider violates it". The first
        // version asked only "is this thread inside a delivery on THIS object", so
        // it let this through - and Y's live subscription then left
        // provider_subscribed set, the native destructor reached the provider's
        // door, and the kReentrantCall it rethrew out of a noexcept destructor
        // TERMINATED THE PROCESS. A regression here does not fail this test; it
        // kills the test host, which is louder.
        using var other = new Subscriber(_provider);
        SubscribeResult held = other.Subscribe(Declare("otherheld"), (_, _, _) => { });
        held.Schema.Dispose();

        TopicPath topic = Declare("disposeother");
        List<Exception> faults = Absorbed();

        SubscribeResult result = _subscriber.Subscribe(topic, (_, _, _) => other.Dispose());
        result.Schema.Dispose();

        _publisher.Publish(topic, _rows, 0);

        Exception fault = Assert.Single(faults);
        Assert.IsType<InvalidOperationException>(fault);
        Assert.Contains("provider", fault.Message, StringComparison.Ordinal);
        Assert.Contains("DispatchAfterDelivery", fault.Message, StringComparison.Ordinal);

        // Refused before any teardown, so the other subscriber is intact.
        Assert.True(held.Subscription.IsLive);

        result.Subscription.Dispose();
    }

    [Fact]
    public void ANestedDeliveryCannotHideAnOuterFrameOnTheSameProvider()
    {
        // The second route to the same termination, and the reason the marker is
        // a stack. A handler on provider P publishes on an in-process provider Q,
        // which delivers SYNCHRONOUSLY: this thread now holds a frame on P and,
        // inside it, a frame on Q. Q's handler disposes a subscriber over P. The
        // first version compared only the innermost frame (Q's subscriber) and
        // passed; the seam asks about every frame on the thread.
        using PubSubProviderHandle q = ProviderRegistry.Create(ProviderSelector.Parse("inprocess"), new ProviderConfig());
        using var qPublisher = new Publisher(q);
        using var qSubscriber = new Subscriber(q);
        TopicPath qTopic = TopicPath.Of("bind", "nestedq");
        qPublisher.CreateTopic(qTopic, _batch.Schema);

        using var victim = new Subscriber(_provider);
        SubscribeResult victimHeld = victim.Subscribe(Declare("nestedvictim"), (_, _, _) => { });
        victimHeld.Schema.Dispose();

        var qFaults = new List<Exception>();
        qSubscriber.HandlerFaulted += (_, args) => qFaults.Add(args.Exception);
        SubscribeResult inner = qSubscriber.Subscribe(qTopic, (_, _, _) => victim.Dispose());
        inner.Schema.Dispose();

        List<Exception> outerFaults = Absorbed();
        TopicPath outer = Declare("nestedouter");
        SubscribeResult outerSub = _subscriber.Subscribe(outer, (_, _, _) => qPublisher.Publish(qTopic, _rows, 0));
        outerSub.Schema.Dispose();

        _publisher.Publish(outer, _rows, 0);

        Assert.Empty(outerFaults);
        Exception fault = Assert.Single(qFaults);
        Assert.IsType<InvalidOperationException>(fault);
        Assert.True(victimHeld.Subscription.IsLive);

        inner.Subscription.Dispose();
        outerSub.Subscription.Dispose();
    }

    [Fact]
    public void DisposingASubscriberOnADifferentProviderFromAHandlerIsServed()
    {
        // The refusal is the seam's question and no wider. Destroying a subscriber
        // over provider Q from inside a delivery on provider P enters Q's door,
        // not P's, and the seam serves it - so the managed layer must too.
        // Refusing it would be this binding inventing a limit.
        using PubSubProviderHandle q = ProviderRegistry.Create(ProviderSelector.Parse("inprocess"), new ProviderConfig());
        using var qPublisher = new Publisher(q);
        var elsewhere = new Subscriber(q);
        TopicPath qTopic = TopicPath.Of("bind", "crossprovider");
        qPublisher.CreateTopic(qTopic, _batch.Schema);
        SubscribeResult held = elsewhere.Subscribe(qTopic, (_, _, _) => { });
        held.Schema.Dispose();

        List<Exception> faults = Absorbed();
        TopicPath topic = Declare("disposeelsewhere");
        SubscribeResult result = _subscriber.Subscribe(topic, (_, _, _) => elsewhere.Dispose());
        result.Schema.Dispose();

        _publisher.Publish(topic, _rows, 0);

        Assert.Empty(faults);
        Assert.Throws<ObjectDisposedException>(() => elsewhere.Subscribe(qTopic, (_, _, _) => { }));

        result.Subscription.Dispose();
    }

    [Fact]
    public void SubscribingThroughAnotherSubscriberOnTheSameProviderIsRefusedInManagedCode()
    {
        // The Subscribe half of D-BIND-51. The first version refused only through
        // the handler's OWN subscriber; through another subscriber over the same
        // provider the call reached native and came back as a kReentrantCall
        // status - safe, but not the managed refusal naming the route that works.
        using var other = new Subscriber(_provider);
        TopicPath fresh = Declare("otherfresh");
        TopicPath topic = Declare("subscribeother");
        List<Exception> faults = Absorbed();

        SubscribeResult result = _subscriber.Subscribe(topic, (_, _, _) =>
        {
            SubscribeResult nested = other.Subscribe(fresh, (_, _, _) => { });
            nested.Schema.Dispose();
        });
        result.Schema.Dispose();

        _publisher.Publish(topic, _rows, 0);

        Exception fault = Assert.Single(faults);
        Assert.IsType<InvalidOperationException>(fault);
        Assert.Contains("DispatchAfterDelivery", fault.Message, StringComparison.Ordinal);

        result.Subscription.Dispose();
    }

    /// <summary>Poll <paramref name="condition"/> until it holds or a generous deadline passes.</summary>
    private static async Task<bool> Eventually(Func<bool> condition)
    {
        DateTime deadline = DateTime.UtcNow + TimeSpan.FromSeconds(10);
        while (!condition())
        {
            if (DateTime.UtcNow > deadline)
            {
                return false;
            }

            await Task.Delay(10);
        }

        return true;
    }
}
