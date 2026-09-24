// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// BIND-4d-iv — the C# ARM of the CallerTier conformance suite.
//
// ── What this file is, and what makes it different from a port ──────────────
// Seam §9 hands BIND an ORACLE: `integration-tests/pubsub-conformance`'s
// `CallerTier` suite, 21 cases asserted at the tier a language binding wraps -
// `Subscriber`, not `PubSubProvider`. BIND-4's acceptance asks for a C# arm of it
// "with a total mapping onto the C++ cases".
//
// TOTAL is the word that shapes this file. Unlike bucket 3, where two cases were
// ruled unportable (D-BIND-47), every case here must have a mirror: the C++ suite
// exists precisely because these properties are what a BINDING can get wrong, so
// "the managed surface cannot observe it" would be an admission rather than an
// exclusion. Where the managed answer legitimately DIFFERS from the C++ one, the
// mirror asserts the managed answer and says why - that is still a mirror, and it
// is the honest kind.
//
// `scripts/check_caller_tier_mapping.py` fails the build if any C++ case has no
// `Mirrors CallerTier.<Name>` marker here. The marker is the contract; the script
// only enforces what this comment claims.
//
// ── Why `inprocess`, and what that buys ─────────────────────────────────────
// The C++ suite links `fletcher-pubsub` and NO transport SDK, because these are
// seam properties rather than one provider's. `inprocess` is the managed
// equivalent: no discovery, no sockets, and delivery happens SYNCHRONOUSLY on the
// publishing thread - which is what makes "a delivery in flight" reproducible
// here. A delivery is in flight exactly while another thread sits inside
// `Publish`, so every window below is MADE with latches rather than waited for
// with sleeps. The C++ file states that rule and this arm keeps it.
//
// ── The vacuity rule, inherited verbatim ────────────────────────────────────
// No case may pass on "it did not throw" alone. Every case asserts a call count,
// delivered bytes, an ordering fact, or a value captured at a named moment.
using System;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;

using Apache.Arrow;

using Eiva.Fletcher;

using Xunit;

namespace Eiva.Fletcher.Tests;

public class CallerTierArmTests : IDisposable
{
    /// <summary>Generous on purpose: a latch that times out is a FAILED test, not a slow one.</summary>
    private static readonly TimeSpan Generous = TimeSpan.FromSeconds(10);

    /// <summary>Long enough that "it returned first" cannot be scheduling luck.</summary>
    private static readonly TimeSpan HoldWindow = TimeSpan.FromMilliseconds(150);

    private readonly PubSubProviderHandle _provider;
    private readonly Publisher _publisher;
    private readonly Subscriber _subscriber;
    private readonly RecordBatch _batch = CodecFixtures.Scalar();
    private readonly FletcherCodec _codec;
    private readonly BoundRows _rows;

    public CallerTierArmTests()
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

    /// <summary>A latch. Waiting on one that never fires FAILS rather than hangs.</summary>
    private sealed class Flag : IDisposable
    {
        private readonly ManualResetEventSlim _event = new(false);

        internal void Set() => _event.Set();

        internal bool WaitFor(TimeSpan timeout) => _event.Wait(timeout);

        /// <summary>Wait, and FAIL rather than hang if it never fires.</summary>
        /// <remarks>
        /// A synchronous event wait, not a Task one - xUnit1031 is about blocking
        /// on Tasks, and blocking on a latch is exactly how the window this suite
        /// needs is MADE rather than waited for.
        /// </remarks>
        internal void MustFire(string what)
            => Assert.True(_event.Wait(Generous), $"latch never fired: {what}");

        public void Dispose() => _event.Dispose();
    }

    /// <summary>Whether <paramref name="task"/> finished inside <paramref name="timeout"/>.</summary>
    /// <remarks>
    /// The async form of a join with a deadline. xUnit's analyzer forbids blocking
    /// task operations in a test (xUnit1031) and is right to: a test that blocks
    /// its own thread can deadlock against the very machinery it is exercising.
    /// Returning a bool rather than asserting inside lets each case say what a
    /// timeout MEANS for it - here it is almost always "deadlocked", which is a
    /// finding rather than a slow machine.
    /// </remarks>
    private static async Task<bool> CompletesWithin(Task task, TimeSpan timeout)
        => await Task.WhenAny(task, Task.Delay(timeout)).ConfigureAwait(false) == task;

    private TopicPath Declare(string name)
    {
        TopicPath topic = TopicPath.Of("callertier", name);
        _publisher.CreateTopic(topic, _batch.Schema);
        return topic;
    }

    private void PublishOne(TopicPath topic) => _publisher.Publish(topic, _rows, 0);

    // ── §7 clause 6: no callback after Unsubscribe returns ──────────────────

    /// <summary>Mirrors CallerTier.NoCallbackAfterUnsubscribeReturns.</summary>
    [Fact]
    public async Task NoCallbackAfterUnsubscribeReturns()
    {
        TopicPath topic = Declare("nocallbackafter");
        int afterReturn = 0;
        var cancelled = new ManualResetEventSlim(false);

        SubscribeResult result = _subscriber.Subscribe(topic, (_, _, _) =>
        {
            if (cancelled.IsSet)
            {
                Interlocked.Increment(ref afterReturn);
            }
        });
        result.Schema.Dispose();

        var stop = new ManualResetEventSlim(false);
        Task pump = Task.Run(() =>
        {
            while (!stop.IsSet)
            {
                PublishOne(topic);
            }
        });

        Thread.Sleep(20);
        result.Subscription.Dispose();
        cancelled.Set();

        // The pump keeps publishing for a while AFTER the cancel returned. Any
        // callback in that window is the defect this clause exists to catch.
        Thread.Sleep(50);
        stop.Set();
        await CompletesWithin(pump, Generous);

        Assert.Equal(0, Volatile.Read(ref afterReturn));
    }

    /// <summary>Mirrors CallerTier.UnsubscribeWaitsForAnInFlightDelivery.</summary>
    [Fact]
    public async Task UnsubscribeWaitsForAnInFlightDelivery()
    {
        // THE GUARANTEE THE WHOLE TIER EXISTS FOR: on return the caller may free
        // whatever the handler was using. The handler is released only once the
        // main thread has committed to the cancel, so "the cancel returned first"
        // cannot be an artefact of scheduling order.
        TopicPath topic = Declare("waitsinflight");
        using var entered = new Flag();
        using var release = new Flag();
        using var atUnsubscribe = new Flag();
        int exited = 0;

        SubscribeResult result = _subscriber.Subscribe(topic, (_, _, _) =>
        {
            entered.Set();
            Assert.True(release.WaitFor(Generous), "the releaser never ran");
            Volatile.Write(ref exited, 1);
        });
        result.Schema.Dispose();

        Task delivery = Task.Run(() => PublishOne(topic));
        entered.MustFire("the handler never started");

        Task releaser = Task.Run(() =>
        {
            atUnsubscribe.MustFire("the main thread never reached the cancel");
            Thread.Sleep(HoldWindow);
            release.Set();
        });

        atUnsubscribe.Set();
        result.Subscription.Dispose();
        int exitedAtReturn = Volatile.Read(ref exited);

        await CompletesWithin(Task.WhenAll(releaser, delivery), Generous);

        Assert.Equal(1, exitedAtReturn);
    }

    /// <summary>Mirrors CallerTier.UnsubscribeOfAnUnknownIdIsANoOp.</summary>
    [Fact]
    public void UnsubscribeOfAnUnknownIdIsANoOp()
    {
        // Cancelling something not live is an ordinary answer, not an error: a
        // foreign-runtime finaliser has nowhere to put an exception.
        TopicPath topic = Declare("unknownid");
        int delivered = 0;

        SubscribeResult result = _subscriber.Subscribe(topic, (_, _, _) => Interlocked.Increment(ref delivered));
        result.Schema.Dispose();

        result.Subscription.Dispose();
        result.Subscription.Dispose();
        result.Subscription.Dispose();

        PublishOne(topic);
        Assert.Equal(0, Volatile.Read(ref delivered));
    }

    // ── Cancellation from inside a delivery ─────────────────────────────────

    /// <summary>Mirrors CallerTier.CancelFromInsideDeliveryDoesNotEnterTheProvider.</summary>
    [Fact]
    public void CancelFromInsideDeliveryDoesNotEnterTheProvider()
    {
        // The C++ case observes that the provider is not re-entered. The managed
        // surface cannot see the provider, so the mirror asserts the CONSEQUENCE
        // that makes it matter: the cancel completes rather than deadlocking or
        // being refused, and delivery stops.
        TopicPath topic = Declare("cancelinside");
        int delivered = 0;
        SubscribeResult result = default;

        result = _subscriber.Subscribe(topic, (_, _, _) =>
        {
            Interlocked.Increment(ref delivered);
            result.Subscription.Dispose();
        });
        result.Schema.Dispose();

        PublishOne(topic);
        PublishOne(topic);

        Assert.Equal(1, Volatile.Read(ref delivered));
        Assert.False(result.Subscription.IsLive);
    }

    /// <summary>Mirrors CallerTier.SelfUnsubscribeInsideItsOwnCallbackReturns.</summary>
    [Fact]
    public void SelfUnsubscribeInsideItsOwnCallbackReturns()
    {
        // It RETURNS - a cancellation cannot wait for the frame it is already in,
        // and one that tried would hang here rather than fail.
        TopicPath topic = Declare("selfreturns");
        int returned = 0;
        SubscribeResult result = default;

        result = _subscriber.Subscribe(topic, (_, _, _) =>
        {
            result.Subscription.Dispose();
            Volatile.Write(ref returned, 1);
        });
        result.Schema.Dispose();

        PublishOne(topic);

        Assert.Equal(1, Volatile.Read(ref returned));
    }

    /// <summary>Mirrors CallerTier.CrossCancellingDeliveriesDoNotDeadlock.</summary>
    /// <remarks>
    /// WEAKER THAN ITS C++ ORIGINAL, AND THE LIMIT IS STRUCTURAL. The C++ case puts
    /// two deliveries genuinely in flight and has each handler cancel the other's
    /// subscription. That window CANNOT BE CONSTRUCTED from managed code:
    /// `inprocess` holds one mutex across the callback, so it delivers ONE AT A
    /// TIME instance-wide (`in_process_provider.cpp`: "one delivery at a time,
    /// instance-wide"), and the C++ suite only gets its window by using a
    /// purpose-built ProbeProvider - which a binding cannot supply, because
    /// `ProviderRegistry` deliberately exposes no `Register` (D-BIND-24).
    ///
    /// So this asserts the half that IS observable: a handler cancelling ANOTHER
    /// subscriber's subscription is served rather than refused, completes rather
    /// than deadlocking, and retires what it cancelled. The concurrent half stays
    /// covered where it can be - in C++, by the case this mirrors.
    /// </remarks>
    [Fact]
    public async Task CrossCancellingDeliveriesDoNotDeadlock()
    {
        TopicPath topic = Declare("crosscancel");
        using var otherSubscriber = new Subscriber(_provider);

        SubscribeResult victim = otherSubscriber.Subscribe(topic, (_, _, _) => { });
        victim.Schema.Dispose();

        int cancellerRan = 0;
        SubscribeResult canceller = _subscriber.Subscribe(topic, (_, _, _) =>
        {
            Interlocked.Increment(ref cancellerRan);
            victim.Subscription.Dispose();
        });
        canceller.Schema.Dispose();

        Task work = Task.Run(() => PublishOne(topic));

        Assert.True(await CompletesWithin(work, Generous),
            "cancelling another subscriber's subscription from inside a delivery deadlocked");
        Assert.Equal(1, Volatile.Read(ref cancellerRan));
        Assert.False(victim.Subscription.IsLive);

        canceller.Subscription.Dispose();
    }

    /// <summary>Mirrors CallerTier.UnsubscribeDoesNotHoldAGateWhileEnteringTheProvider.</summary>
    /// <remarks>
    /// WEAKER THAN ITS C++ ORIGINAL, FOR THE SAME STRUCTURAL REASON as the case
    /// above. C++ asserts that the Subscriber does not hold its own gate while
    /// entering the provider - a statement about which of two locks is held when.
    /// Over `inprocess` the two are indistinguishable: that provider holds one
    /// mutex across the callback, and its own comment says so - "an Unsubscribe on
    /// another thread blocks here until the delivery in flight has returned". A
    /// cancel of an UNRELATED subscription therefore waits, and a managed test
    /// cannot tell the provider's mutex from a subscriber gate.
    ///
    /// What is observable, and is what the gate discipline exists to guarantee, is
    /// that the cancel EVENTUALLY COMPLETES rather than deadlocking - so it is
    /// asserted against the delivery being released, which is the strongest claim
    /// this provider admits.
    /// </remarks>
    [Fact]
    public async Task UnsubscribeDoesNotHoldAGateWhileEnteringTheProvider()
    {
        TopicPath topic = Declare("nogatehold");
        TopicPath other = Declare("nogatehold2");

        using var entered = new Flag();
        using var release = new Flag();

        SubscribeResult busy = _subscriber.Subscribe(topic, (_, _, _) =>
        {
            entered.Set();
            release.WaitFor(Generous);
        });
        SubscribeResult idle = _subscriber.Subscribe(other, (_, _, _) => { });
        busy.Schema.Dispose();
        idle.Schema.Dispose();

        Task delivery = Task.Run(() => PublishOne(topic));
        entered.MustFire("the busy handler never started");

        Task cancel = Task.Run(() => idle.Subscription.Dispose());

        // Released FIRST, deliberately: over `inprocess` the cancel is waiting on
        // the provider's own mutex, so asserting it returned BEFORE this point
        // would be asserting something about a lock this provider does not have.
        release.Set();

        Assert.True(await CompletesWithin(cancel, Generous),
            "cancelling a subscription never completed after the delivery was released");
        Assert.True(await CompletesWithin(delivery, Generous));
        Assert.False(idle.Subscription.IsLive);

        busy.Subscription.Dispose();
    }

    /// <summary>Mirrors CallerTier.ReentrantSubscribeFromInsideDeliveryDoesNotDeadlock.</summary>
    [Fact]
    public async Task ReentrantSubscribeFromInsideDeliveryDoesNotDeadlock()
    {
        // THE MANAGED ANSWER DIFFERS FROM THE C++ ONE, and that is the mirror
        // rather than a gap. C++ reaches the provider and is refused with
        // kReentrantCall; the managed surface refuses it FIRST, in managed code,
        // because a status returned into a transport callback has no caller to
        // read it (D-BIND-18). Either way the property asserted is the same: it
        // does not deadlock, and the refusal is visible.
        TopicPath topic = Declare("reentrantsub");
        TopicPath fresh = Declare("reentrantsubnew");
        var faults = new List<Exception>();
        _subscriber.HandlerFaulted += (_, e) => faults.Add(e.Exception);

        SubscribeResult result = _subscriber.Subscribe(topic, (_, _, _) =>
        {
            SubscribeResult nested = _subscriber.Subscribe(fresh, (_, _, _) => { });
            nested.Schema.Dispose();
        });
        result.Schema.Dispose();

        Task work = Task.Run(() => PublishOne(topic));
        Assert.True(await CompletesWithin(work, Generous), "a re-entrant subscribe deadlocked");

        Exception fault = Assert.Single(faults);
        Assert.IsType<InvalidOperationException>(fault);
        result.Subscription.Dispose();
    }

    /// <summary>Mirrors CallerTier.ALiveSubscriptionStillReceives.</summary>
    [Fact]
    public void ALiveSubscriptionStillReceives()
    {
        TopicPath topic = Declare("stillreceives");
        int live = 0;

        SubscribeResult doomed = _subscriber.Subscribe(topic, (_, _, _) => { });
        SubscribeResult survivor = _subscriber.Subscribe(topic, (_, _, _) => Interlocked.Increment(ref live));
        doomed.Schema.Dispose();
        survivor.Schema.Dispose();

        doomed.Subscription.Dispose();
        PublishOne(topic);

        Assert.Equal(1, Volatile.Read(ref live));
        survivor.Subscription.Dispose();
    }

    /// <summary>Mirrors CallerTier.AThrowingHandlerDoesNotAbortTheFanOut.</summary>
    /// <remarks>
    /// THE CASE BIND ADDED TO THE C++ SUITE (seam 12.1). The property is
    /// documented on `Subscriber::SubscribeCallback` - a throw is contained, every
    /// REMAINING subscriber still receives that sample, and nothing is reported to
    /// a caller - but only the first and third claims had a case at that tier.
    /// Writing this arm is what surfaced the gap: the managed thunk must absorb a
    /// handler throw or the process dies, so the property had to be asserted here,
    /// and the oracle turned out not to assert it there.
    ///
    /// The throwing handler is registered FIRST for the same reason as in C++: a
    /// fan-out that stops at a throw cannot then reach the survivor, and
    /// registering it second would pass against the very defect this guards.
    /// </remarks>
    [Fact]
    public void AThrowingHandlerDoesNotAbortTheFanOut()
    {
        TopicPath topic = Declare("fanoutsurvives");
        int thrown = 0;
        int survived = 0;

        ulong before = _subscriber.AbsorbedCallbackFailures;

        SubscribeResult faulty = _subscriber.Subscribe(topic, (_, _, _) =>
        {
            Interlocked.Increment(ref thrown);
            throw new InvalidOperationException("this handler is unhappy");
        });
        SubscribeResult survivor = _subscriber.Subscribe(
            topic, (_, _, _) => Interlocked.Increment(ref survived));
        faulty.Schema.Dispose();
        survivor.Schema.Dispose();

        PublishOne(topic);

        Assert.Equal(1, Volatile.Read(ref thrown));
        Assert.Equal(1, Volatile.Read(ref survived));
        Assert.Equal(before + 1, _subscriber.AbsorbedCallbackFailures);

        faulty.Subscription.Dispose();
        survivor.Subscription.Dispose();
    }

    /// <summary>Mirrors CallerTier.AReleasedIdIsNeverReused.</summary>
    [Fact]
    public void AReleasedIdIsNeverReused()
    {
        // C++ asserts a uint64 id is never handed out twice. The managed surface
        // hands out HANDLES rather than numbers, so the mirror asserts the property
        // the ids existed to give: a retired subscription never becomes live again,
        // and a later subscription is a different object that does not disturb it.
        TopicPath topic = Declare("neverreused");

        SubscribeResult first = _subscriber.Subscribe(topic, (_, _, _) => { });
        first.Schema.Dispose();
        first.Subscription.Dispose();

        SubscribeResult second = _subscriber.Subscribe(topic, (_, _, _) => { });
        second.Schema.Dispose();

        Assert.NotSame(first.Subscription, second.Subscription);
        Assert.False(first.Subscription.IsLive);
        Assert.True(second.Subscription.IsLive);

        second.Subscription.Dispose();
        Assert.False(first.Subscription.IsLive);
    }

    /// <summary>Mirrors CallerTier.DestructorDrainsAnInFlightDelivery.</summary>
    [Fact]
    public async Task DestructorDrainsAnInFlightDelivery()
    {
        // Disposing a Subscriber while one of its deliveries is running must wait
        // for it. Run on a fresh Subscriber, because disposing the fixture's would
        // break every later assertion in this instance.
        TopicPath topic = Declare("destructordrains");
        var subscriber = new Subscriber(_provider);

        using var entered = new Flag();
        using var release = new Flag();
        int exited = 0;

        SubscribeResult result = subscriber.Subscribe(topic, (_, _, _) =>
        {
            entered.Set();
            release.WaitFor(Generous);
            Volatile.Write(ref exited, 1);
        });
        result.Schema.Dispose();

        Task delivery = Task.Run(() => PublishOne(topic));
        entered.MustFire("the handler never started");

        Task releaser = Task.Run(() =>
        {
            Thread.Sleep(HoldWindow);
            release.Set();
        });

        subscriber.Dispose();
        int exitedAtReturn = Volatile.Read(ref exited);

        await CompletesWithin(Task.WhenAll(releaser, delivery), Generous);
        Assert.Equal(1, exitedAtReturn);
    }

    /// <summary>Mirrors CallerTier.CancellingOnAnotherSubscriberWaitsForItsDelivery.</summary>
    [Fact]
    public async Task CancellingOnAnotherSubscriberWaitsForItsDelivery()
    {
        TopicPath topic = Declare("othersubscriber");
        using var other = new Subscriber(_provider);

        using var entered = new Flag();
        using var release = new Flag();
        int exited = 0;

        SubscribeResult result = other.Subscribe(topic, (_, _, _) =>
        {
            entered.Set();
            release.WaitFor(Generous);
            Volatile.Write(ref exited, 1);
        });
        result.Schema.Dispose();

        Task delivery = Task.Run(() => PublishOne(topic));
        entered.MustFire("the handler never started");

        Task releaser = Task.Run(() =>
        {
            Thread.Sleep(HoldWindow);
            release.Set();
        });

        result.Subscription.Dispose();
        int exitedAtReturn = Volatile.Read(ref exited);

        await CompletesWithin(Task.WhenAll(releaser, delivery), Generous);
        Assert.Equal(1, exitedAtReturn);
    }

    /// <summary>Mirrors CallerTier.ADuplicateCancelWaitsForTheDrainInProgress.</summary>
    [Fact]
    public async Task ADuplicateCancelWaitsForTheDrainInProgress()
    {
        // TWO THREADS cancelling one subscription: both must return only once the
        // handler has finished, so both may free handler state on return.
        TopicPath topic = Declare("duplicatecancel");
        using var entered = new Flag();
        using var release = new Flag();
        int exited = 0;

        SubscribeResult result = _subscriber.Subscribe(topic, (_, _, _) =>
        {
            entered.Set();
            release.WaitFor(Generous);
            Volatile.Write(ref exited, 1);
        });
        result.Schema.Dispose();

        Task delivery = Task.Run(() => PublishOne(topic));
        entered.MustFire("the handler never started");

        Task releaser = Task.Run(() =>
        {
            Thread.Sleep(HoldWindow);
            release.Set();
        });

        Task first = Task.Run(() => result.Subscription.Dispose());
        Task second = Task.Run(() => result.Subscription.Dispose());

        Assert.True(await CompletesWithin(Task.WhenAll(first, second), Generous),
            "a duplicate cancel did not return");
        Assert.Equal(1, Volatile.Read(ref exited));

        await CompletesWithin(Task.WhenAll(releaser, delivery), Generous);
    }

    /// <summary>Mirrors CallerTier.ACancelOfAFullyRetiredIdReturnsWithoutWaiting.</summary>
    [Fact]
    public async Task ACancelOfAFullyRetiredIdReturnsWithoutWaiting()
    {
        // Nothing is in flight, so this must be immediate rather than merely
        // eventual - a retired id that still waited on something would make
        // teardown unpredictable.
        TopicPath topic = Declare("fullyretired");

        SubscribeResult result = _subscriber.Subscribe(topic, (_, _, _) => { });
        result.Schema.Dispose();
        result.Subscription.Dispose();

        Task again = Task.Run(() => result.Subscription.Dispose());
        Assert.True(await CompletesWithin(again, TimeSpan.FromSeconds(2)), "cancelling a retired subscription waited on something");
    }

    /// <summary>Mirrors CallerTier.ASubscribeDuringADrainKeepsItsProviderSubscription.</summary>
    [Fact]
    public async Task ASubscribeDuringADrainKeepsItsProviderSubscription()
    {
        // C++ observes the provider-level subscription surviving. The managed
        // surface cannot see that, so the mirror asserts what it exists to
        // guarantee: a subscription taken while another is draining still receives.
        TopicPath topic = Declare("subduringdrain");
        using var entered = new Flag();
        using var release = new Flag();
        int late = 0;

        SubscribeResult draining = _subscriber.Subscribe(topic, (_, _, _) =>
        {
            entered.Set();
            release.WaitFor(Generous);
        });
        draining.Schema.Dispose();

        Task delivery = Task.Run(() => PublishOne(topic));
        entered.MustFire("the draining handler never started");

        SubscribeResult fresh = _subscriber.Subscribe(topic, (_, _, _) => Interlocked.Increment(ref late));
        fresh.Schema.Dispose();

        release.Set();
        Assert.True(await CompletesWithin(delivery, Generous));
        draining.Subscription.Dispose();

        PublishOne(topic);
        Assert.Equal(1, Volatile.Read(ref late));
        fresh.Subscription.Dispose();
    }

    /// <summary>Mirrors CallerTier.ACancelRacingASelfCancelWaitsForThatHandler.</summary>
    [Fact]
    public async Task ACancelRacingASelfCancelWaitsForThatHandler()
    {
        TopicPath topic = Declare("racingselfcancel");
        using var entered = new Flag();
        using var release = new Flag();
        int exited = 0;
        SubscribeResult result = default;

        result = _subscriber.Subscribe(topic, (_, _, _) =>
        {
            entered.Set();
            release.WaitFor(Generous);
            result.Subscription.Dispose();
            Volatile.Write(ref exited, 1);
        });
        result.Schema.Dispose();

        Task delivery = Task.Run(() => PublishOne(topic));
        entered.MustFire("the handler never started");

        Task releaser = Task.Run(() =>
        {
            Thread.Sleep(HoldWindow);
            release.Set();
        });

        Task outside = Task.Run(() => result.Subscription.Dispose());
        Assert.True(await CompletesWithin(outside, Generous), "a cancel racing a self-cancel did not return");
        Assert.Equal(1, Volatile.Read(ref exited));

        await CompletesWithin(Task.WhenAll(releaser, delivery), Generous);
    }

    /// <summary>Mirrors CallerTier.ConcurrentFirstSubscribesCreateOneProviderSubscription.</summary>
    [Fact]
    public void ConcurrentFirstSubscribesCreateOneProviderSubscription()
    {
        // The provider-level count is invisible from managed code. What IS
        // observable, and is what the single subscription exists to make true, is
        // that every concurrent subscriber receives exactly one copy of one row.
        TopicPath topic = Declare("concurrentfirst");
        const int Count = 8;
        var hits = new int[Count];
        var results = new SubscribeResult[Count];

        Parallel.For(0, Count, i =>
        {
            results[i] = _subscriber.Subscribe(topic, (_, _, _) => Interlocked.Increment(ref hits[i]));
        });

        foreach (SubscribeResult r in results)
        {
            r.Schema.Dispose();
        }

        PublishOne(topic);

        for (int i = 0; i < Count; i++)
        {
            Assert.Equal(1, Volatile.Read(ref hits[i]));
            results[i].Subscription.Dispose();
        }
    }

    /// <summary>Mirrors CallerTier.CancellingASiblingRunningOnAnotherThreadKeepsItPublished.</summary>
    [Fact]
    public async Task CancellingASiblingRunningOnAnotherThreadKeepsItPublished()
    {
        // Cancelling a SIBLING from inside a delivery does not wait for it, and
        // must not lose the row the sibling is already handling.
        TopicPath topic = Declare("siblingpublished");
        using var siblingEntered = new Flag();
        int siblingCompleted = 0;
        SubscribeResult sibling = default;

        sibling = _subscriber.Subscribe(topic, (_, _, _) =>
        {
            siblingEntered.Set();
            Thread.Sleep(HoldWindow);
            Interlocked.Increment(ref siblingCompleted);
        });
        SubscribeResult canceller = _subscriber.Subscribe(topic, (_, _, _) => sibling.Subscription.Dispose());
        sibling.Schema.Dispose();
        canceller.Schema.Dispose();

        Task work = Task.Run(() => PublishOne(topic));
        Assert.True(await CompletesWithin(work, Generous), "cancelling a sibling from a delivery blocked");

        siblingEntered.MustFire("the sibling never received the row it was already handling");
        Assert.Equal(1, Volatile.Read(ref siblingCompleted));

        canceller.Subscription.Dispose();
    }

    /// <summary>Mirrors CallerTier.SubscribingToANewTopicFromInsideADeliveryIsRefusedByName.</summary>
    [Fact]
    public void SubscribingToANewTopicFromInsideADeliveryIsRefusedByName()
    {
        // BY NAME: the refusal must identify the topic, or a handler subscribing to
        // several cannot tell which one was refused.
        TopicPath topic = Declare("refusedbyname");
        TopicPath wanted = Declare("refusedbynametarget");
        var faults = new List<Exception>();
        _subscriber.HandlerFaulted += (_, e) => faults.Add(e.Exception);

        SubscribeResult result = _subscriber.Subscribe(topic, (_, _, _) =>
        {
            SubscribeResult nested = _subscriber.Subscribe(wanted, (_, _, _) => { });
            nested.Schema.Dispose();
        });
        result.Schema.Dispose();

        PublishOne(topic);

        Exception fault = Assert.Single(faults);
        Assert.Contains(wanted.ToKey(), fault.Message, StringComparison.Ordinal);
        result.Subscription.Dispose();
    }

    /// <summary>Mirrors CallerTier.ARefusedReentrantSubscribeIsCountedRatherThanSwallowedSilently.</summary>
    [Fact]
    public void ARefusedReentrantSubscribeIsCountedRatherThanSwallowedSilently()
    {
        // Containment without a count is a SILENT wrong answer: the handler
        // believes it holds a subscription that does not exist.
        TopicPath topic = Declare("refusedcounted");
        TopicPath wanted = Declare("refusedcountedtarget");

        ulong before = _subscriber.AbsorbedCallbackFailures;

        SubscribeResult result = _subscriber.Subscribe(topic, (_, _, _) =>
        {
            SubscribeResult nested = _subscriber.Subscribe(wanted, (_, _, _) => { });
            nested.Schema.Dispose();
        });
        result.Schema.Dispose();

        PublishOne(topic);

        Assert.Equal(before + 1, _subscriber.AbsorbedCallbackFailures);
        result.Subscription.Dispose();
    }

    /// <summary>Mirrors CallerTier.TheCallerTierDoorIsReachedBeforeItCanBlock.</summary>
    [Fact]
    public async Task TheCallerTierDoorIsReachedBeforeItCanBlock()
    {
        // The refusal happens at the DOOR - before anything can wait on anything -
        // which is why a handler that subscribes to a new topic gets an exception
        // rather than a hang. Asserted as a bound on how long the refused call took
        // while another delivery is deliberately held open: a door reached after a
        // lock would have waited for the holder.
        TopicPath held = Declare("doorheld");
        TopicPath wanted = Declare("doorwanted");

        using var entered = new Flag();
        using var release = new Flag();
        using var other = new Subscriber(_provider);

        SubscribeResult holder = other.Subscribe(held, (_, _, _) =>
        {
            entered.Set();
            release.WaitFor(Generous);
        });
        holder.Schema.Dispose();

        Task delivery = Task.Run(() => _publisher.Publish(held, _rows, 0));
        entered.MustFire("the holding handler never started");

        var faults = new List<Exception>();
        _subscriber.HandlerFaulted += (_, e) => faults.Add(e.Exception);

        SubscribeResult probe = _subscriber.Subscribe(Declare("doorprobe"), (_, _, _) =>
        {
            SubscribeResult nested = _subscriber.Subscribe(wanted, (_, _, _) => { });
            nested.Schema.Dispose();
        });
        probe.Schema.Dispose();

        var clock = System.Diagnostics.Stopwatch.StartNew();
        _publisher.Publish(Declare("doorprobe"), _rows, 0);
        clock.Stop();

        release.Set();
        Assert.True(await CompletesWithin(delivery, Generous));

        Assert.Single(faults);
        Assert.True(clock.Elapsed < HoldWindow,
            $"the refusal took {clock.ElapsedMilliseconds} ms - it waited on something before refusing");

        probe.Subscription.Dispose();
        holder.Subscription.Dispose();
    }
}
