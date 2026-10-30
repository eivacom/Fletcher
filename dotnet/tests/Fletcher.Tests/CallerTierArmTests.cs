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
        // THE C++ SHAPE, EXACTLY (rewritten after the BIND-4 review, D16). The
        // first version pumped rows and cancelled after a fixed sleep, with no
        // proof the handler ever ran and no window it could be wrong in. The
        // window this clause exists for is precise: a fan-out snapshot is loaded,
        // its FIRST entry is parked, and the SECOND is cancelled before the loop
        // reaches it. `inprocess` delivers on the publishing thread in
        // registration order, so that window is made here with a latch.
        TopicPath topic = Declare("nocallbackafter");
        using var firstEntered = new Flag();
        using var releaseFirst = new Flag();
        int secondCalls = 0;
        int releaseTimedOut = 0;

        SubscribeResult first = _subscriber.Subscribe(topic, (_, _, _) =>
        {
            firstEntered.Set();
            if (!releaseFirst.WaitFor(Generous))
            {
                Volatile.Write(ref releaseTimedOut, 1);
            }
        });
        SubscribeResult second = _subscriber.Subscribe(topic, (_, _, _) => Interlocked.Increment(ref secondCalls));
        first.Schema.Dispose();
        second.Schema.Dispose();

        Task delivery = Task.Run(() => PublishOne(topic));
        firstEntered.MustFire("the first handler never started");
        Assert.Equal(0, Volatile.Read(ref secondCalls));

        // The snapshot holding `second` is loaded and the loop has not reached it.
        // `first` stays live, so the topic is not emptied and the provider is not
        // entered: this cancel is the Subscriber tier's alone.
        second.Subscription.Dispose();
        releaseFirst.Set();
        Assert.True(await CompletesWithin(delivery, Generous), "the delivery never finished");

        Assert.Equal(0, Volatile.Read(ref releaseTimedOut));
        Assert.Equal(0, Volatile.Read(ref secondCalls));
        first.Subscription.Dispose();
    }

    /// <summary>Mirrors CallerTier.UnsubscribeWaitsForAnInFlightDelivery.</summary>
    [Fact]
    public async Task UnsubscribeWaitsForAnInFlightDelivery()
    {
        // THE GUARANTEE THE WHOLE TIER EXISTS FOR: on return the caller may free
        // whatever the handler was using. The handler is released only once the
        // main thread has committed to the cancel, so "the cancel returned first"
        // cannot be an artefact of scheduling order.
        //
        // THE SIBLING IS THE POINT (BIND-4 review, B6). Cancelling the topic's
        // ONLY subscription empties it, and the last cancel then enters the
        // provider - whose mutex `inprocess` holds across the delivery. That
        // mutex alone would make the cancel wait, so a Subscriber tier with no
        // drain at all passed. The sibling keeps the topic non-empty, so the only
        // thing that can make this cancel wait is the tier it is named after.
        TopicPath topic = Declare("waitsinflight");
        using var entered = new Flag();
        using var release = new Flag();
        using var atUnsubscribe = new Flag();
        int exited = 0;
        int releaseTimedOut = 0;

        SubscribeResult result = _subscriber.Subscribe(topic, (_, _, _) =>
        {
            entered.Set();
            if (!release.WaitFor(Generous))
            {
                Volatile.Write(ref releaseTimedOut, 1);
            }

            Volatile.Write(ref exited, 1);
        });
        SubscribeResult sibling = _subscriber.Subscribe(topic, (_, _, _) => { });
        result.Schema.Dispose();
        sibling.Schema.Dispose();

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

        Assert.True(await CompletesWithin(Task.WhenAll(releaser, delivery), Generous));

        Assert.Equal(0, Volatile.Read(ref releaseTimedOut));
        Assert.Equal(1, exitedAtReturn);
        sibling.Subscription.Dispose();
    }

    /// <summary>Mirrors CallerTier.UnsubscribeOfAnUnknownIdIsANoOp.</summary>
    [Fact]
    public void UnsubscribeOfAnUnknownIdIsANoOp()
    {
        // Cancelling something not live is an ordinary answer, not an error: a
        // foreign-runtime finaliser has nowhere to put an exception.
        //
        // WHAT THE MANAGED SURFACE CAN AND CANNOT SAY. The C++ case cancels an id
        // never issued (`gone + 4096`, `0`); a managed caller cannot, because a
        // `Subscription` is a typed handle and an arbitrary id is unrepresentable
        // by design. What it CAN do is cancel the same subscription repeatedly -
        // and since the review (B5) every repeat reaches native rather than
        // stopping at a managed short-circuit, so this exercises the seam's
        // idempotence for real. The second half is the one that matters: the
        // no-op must disturb a LIVE subscription on another topic not at all.
        TopicPath liveTopic = Declare("unknownidlive");
        TopicPath goneTopic = Declare("unknownidgone");
        int liveCalls = 0;
        int goneCalls = 0;

        SubscribeResult live = _subscriber.Subscribe(liveTopic, (_, _, _) => Interlocked.Increment(ref liveCalls));
        SubscribeResult gone = _subscriber.Subscribe(goneTopic, (_, _, _) => Interlocked.Increment(ref goneCalls));
        live.Schema.Dispose();
        gone.Schema.Dispose();

        gone.Subscription.Dispose();
        gone.Subscription.Dispose();
        gone.Subscription.Dispose();

        PublishOne(liveTopic);
        PublishOne(goneTopic);
        Assert.Equal(1, Volatile.Read(ref liveCalls));
        Assert.Equal(0, Volatile.Read(ref goneCalls));
        Assert.True(live.Subscription.IsLive);

        live.Subscription.Dispose();
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
        // THE SAME TOPIC, AS IN C++ (rewritten after the BIND-4 review, B5). The
        // first version subscribed to a FRESH topic and asserted a refusal - a
        // duplicate of `SubscribingToANewTopicFromInsideADeliveryIsRefusedByName`
        // - and claimed C++ was refused too, which it is not. The C++ case
        // re-subscribes to the topic whose delivery is running: the seam SERVES
        // that (the provider-level subscription already exists), the new
        // subscription does not join the fan-out already in progress, and it
        // receives the next row. The managed door asks the same question
        // (`Subscriber.Subscribe` serves a topic this Subscriber holds), so the
        // managed answer here is the C++ answer, not a different one.
        TopicPath topic = Declare("reentrantsub");
        var faults = new List<Exception>();
        _subscriber.HandlerFaulted += (_, e) => faults.Add(e.Exception);

        int outerCalls = 0;
        int addedCalls = 0;
        Subscription? added = null;

        SubscribeResult result = _subscriber.Subscribe(topic, (_, _, _) =>
        {
            if (Interlocked.Increment(ref outerCalls) != 1)
            {
                return;
            }

            SubscribeResult nested = _subscriber.Subscribe(topic, (_, _, _) => Interlocked.Increment(ref addedCalls));
            nested.Schema.Dispose();
            added = nested.Subscription;
        });
        result.Schema.Dispose();

        Task work = Task.Run(() => PublishOne(topic));
        Assert.True(await CompletesWithin(work, Generous), "a re-entrant subscribe deadlocked");

        Assert.Empty(faults);
        Assert.NotNull(added);
        Assert.True(added!.IsLive, "the re-entrant Subscribe never produced a live subscription");
        Assert.Equal(0, Volatile.Read(ref addedCalls));

        PublishOne(topic);
        Assert.Equal(1, Volatile.Read(ref addedCalls));

        added.Dispose();
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
        // THE IDS THEMSELVES, AS IN C++ (rewritten after the BIND-4 review, B5).
        // The first version compared two freshly allocated handle objects and a
        // managed flag - neither can fail. The native id is what a stale handle
        // would address, so it is what must never come back: read here through the
        // test assembly's InternalsVisibleTo, 32 subscribe/cancel rounds, strictly
        // increasing and never repeated.
        TopicPath topic = Declare("neverreused");
        var seen = new HashSet<ulong>();
        ulong previous = 0;

        for (int i = 0; i < 32; i++)
        {
            SubscribeResult result = _subscriber.Subscribe(topic, (_, _, _) => { });
            result.Schema.Dispose();
            ulong id = result.Subscription.Id;

            Assert.True(seen.Add(id), $"id {id} was handed out twice");
            Assert.True(id > previous, $"ids stopped increasing at iteration {i}");
            previous = id;

            result.Subscription.Dispose();
        }

        Assert.Equal(32, seen.Count);
    }

    /// <summary>Mirrors CallerTier.DestructorDrainsAnInFlightDelivery.</summary>
    /// <remarks>
    /// WEAKER THAN ITS C++ ORIGINAL, AND THE LIMIT IS STRUCTURAL (BIND-4 review,
    /// B6). Disposing a Subscriber cancels EVERY subscription it holds, so its last
    /// cancel on the topic always enters the provider - and `inprocess` holds its
    /// mutex across the delivery, so that entry waits for the handler whether or
    /// not the Subscriber tier drained first. A sibling cannot keep the provider out
    /// of it here, as it does in the other drain mirrors: a sibling on THIS
    /// Subscriber is cancelled by the same Dispose, and one on another Subscriber
    /// over the same `inprocess` instance would contend for its single per-topic
    /// callback slot. So this asserts the promise a caller relies on - Dispose does
    /// not return while a handler runs - and not WHICH layer keeps it; the C++ case
    /// pins the layer.
    /// </remarks>
    [Fact]
    public async Task DestructorDrainsAnInFlightDelivery()
    {
        // Run on a fresh Subscriber, because disposing the fixture's would break
        // every later assertion in this instance.
        TopicPath topic = Declare("destructordrains");
        var subscriber = new Subscriber(_provider);

        using var entered = new Flag();
        using var release = new Flag();
        using var atDispose = new Flag();
        int exited = 0;
        int releaseTimedOut = 0;

        SubscribeResult result = subscriber.Subscribe(topic, (_, _, _) =>
        {
            entered.Set();
            if (!release.WaitFor(Generous))
            {
                Volatile.Write(ref releaseTimedOut, 1);
            }

            Volatile.Write(ref exited, 1);
        });
        result.Schema.Dispose();

        Task delivery = Task.Run(() => PublishOne(topic));
        entered.MustFire("the handler never started");

        // The hold window starts only once the main thread has committed to the
        // dispose (D16): a releaser that slept first would let a descheduled main
        // thread reach Dispose after the handler had already been released.
        Task releaser = Task.Run(() =>
        {
            atDispose.MustFire("the main thread never reached the dispose");
            Thread.Sleep(HoldWindow);
            release.Set();
        });

        atDispose.Set();
        subscriber.Dispose();
        int exitedAtReturn = Volatile.Read(ref exited);

        Assert.True(await CompletesWithin(Task.WhenAll(releaser, delivery), Generous));
        Assert.Equal(0, Volatile.Read(ref releaseTimedOut));
        Assert.Equal(1, exitedAtReturn);
    }

    /// <summary>Mirrors CallerTier.CancellingOnAnotherSubscriberWaitsForItsDelivery.</summary>
    [Fact]
    public async Task CancellingOnAnotherSubscriberWaitsForItsDelivery()
    {
        // THE C++ PROPERTY, MADE CONSTRUCTIBLE (rewritten after the BIND-4 review,
        // B5). C++: X's HANDLER cancels a subscription on Y while Y's delivery is
        // parked, and must wait - the skip is keyed on the subscriber, and X is not
        // inside a delivery on Y. The first version cancelled from the test thread,
        // so a process-wide depth predicate - the regression the case guards -
        // stayed green. Here X's handler really runs inside a delivery while Y's is
        // parked on another thread, which `inprocess` allows only across TWO
        // instances (it delivers one at a time per instance). The seam's predicate
        // is per Subscriber, so two providers change nothing it asks - and Y keeps a
        // sibling on its topic, so Q's provider is never entered and its mutex
        // cannot stand in for Y's drain (B6).
        using PubSubProviderHandle q = ProviderRegistry.Create(ProviderSelector.Parse("inprocess"), new ProviderConfig());
        using var qPublisher = new Publisher(q);
        using var y = new Subscriber(q);
        TopicPath yTopic = TopicPath.Of("callertier", "othersubscribery");
        qPublisher.CreateTopic(yTopic, _batch.Schema);
        TopicPath xTopic = Declare("othersubscriberx");

        using var yEntered = new Flag();
        using var releaseY = new Flag();
        using var xAtCancel = new Flag();
        int yExited = 0;
        int yExitedAtReturn = -1;
        int releaseTimedOut = 0;

        SubscribeResult ySub = y.Subscribe(yTopic, (_, _, _) =>
        {
            yEntered.Set();
            if (!releaseY.WaitFor(Generous))
            {
                Volatile.Write(ref releaseTimedOut, 1);
            }

            Volatile.Write(ref yExited, 1);
        });
        SubscribeResult ySibling = y.Subscribe(yTopic, (_, _, _) => { });
        ySub.Schema.Dispose();
        ySibling.Schema.Dispose();

        SubscribeResult xSub = _subscriber.Subscribe(xTopic, (_, _, _) =>
        {
            xAtCancel.Set();
            ySub.Subscription.Dispose();
            Volatile.Write(ref yExitedAtReturn, Volatile.Read(ref yExited));
        });
        xSub.Schema.Dispose();

        Task yDelivery = Task.Run(() => qPublisher.Publish(yTopic, _rows, 0));
        yEntered.MustFire("Y's handler never started");

        Task releaser = Task.Run(() =>
        {
            xAtCancel.MustFire("X's handler never reached the cancel");
            Thread.Sleep(HoldWindow);
            releaseY.Set();
        });

        PublishOne(xTopic); // runs X's handler on this thread

        Assert.True(await CompletesWithin(Task.WhenAll(releaser, yDelivery), Generous));
        Assert.Equal(0, Volatile.Read(ref releaseTimedOut));
        Assert.Equal(1, Volatile.Read(ref yExitedAtReturn));

        xSub.Subscription.Dispose();
        ySibling.Subscription.Dispose();
    }

    /// <summary>Mirrors CallerTier.ADuplicateCancelWaitsForTheDrainInProgress.</summary>
    [Fact]
    public async Task ADuplicateCancelWaitsForTheDrainInProgress()
    {
        // TWO THREADS cancelling one subscription: both must return only once the
        // handler has finished, so both may free handler state on return.
        //
        // THE C++ SHAPE (rewritten after the BIND-4 review, B6 and D16). The first
        // version asserted `exited` only after BOTH cancels had returned, so a
        // duplicate that returned at once was invisible; and it cancelled the
        // topic's only subscription, so the provider's mutex did the waiting. Now
        // the winner is proved to be draining before the duplicate starts, the
        // duplicate's own return is what is checked, and a sibling keeps the
        // provider out of it.
        TopicPath topic = Declare("duplicatecancel");
        using var entered = new Flag();
        using var release = new Flag();
        using var winnerAtCancel = new Flag();
        using var duplicateAtCancel = new Flag();
        int exited = 0;
        int winnerReturned = 0;
        int releaseTimedOut = 0;

        SubscribeResult result = _subscriber.Subscribe(topic, (_, _, _) =>
        {
            entered.Set();
            if (!release.WaitFor(Generous))
            {
                Volatile.Write(ref releaseTimedOut, 1);
            }

            Volatile.Write(ref exited, 1);
        });
        SubscribeResult sibling = _subscriber.Subscribe(topic, (_, _, _) => { });
        result.Schema.Dispose();
        sibling.Schema.Dispose();

        Task delivery = Task.Run(() => PublishOne(topic));
        entered.MustFire("the handler never started");

        Task winner = Task.Run(() =>
        {
            winnerAtCancel.Set();
            result.Subscription.Dispose();
            Volatile.Write(ref winnerReturned, 1);
        });
        winnerAtCancel.MustFire("the winning cancel never started");

        // Let the winner get into the drain, and prove it is still there, so a
        // short wait cannot green this case without reaching the branch it names.
        Thread.Sleep(100);
        Assert.Equal(0, Volatile.Read(ref winnerReturned));

        Task releaser = Task.Run(() =>
        {
            duplicateAtCancel.MustFire("the duplicate cancel never started");
            Thread.Sleep(HoldWindow);
            release.Set();
        });

        duplicateAtCancel.Set();
        result.Subscription.Dispose(); // the duplicate
        int duplicateSawItExit = Volatile.Read(ref exited);

        Assert.True(await CompletesWithin(Task.WhenAll(winner, releaser, delivery), Generous));
        Assert.Equal(0, Volatile.Read(ref releaseTimedOut));
        Assert.Equal(1, duplicateSawItExit);
        sibling.Subscription.Dispose();
    }

    /// <summary>Mirrors CallerTier.ACancelOfAFullyRetiredIdReturnsWithoutWaiting.</summary>
    [Fact]
    public async Task ACancelOfAFullyRetiredIdReturnsWithoutWaiting()
    {
        // THE OTHER HALF OF THE DUPLICATE-CANCEL RULING, AS IN C++ (rewritten
        // after the BIND-4 review, B5): "being cancelled right now" waits, "fully
        // cancelled" does not - and the way to tell them apart is to cancel a
        // fully retired subscription WHILE an unrelated delivery is parked, and see
        // that the cancel does not wait for it. The first version had nothing in
        // flight and stopped at a managed short-circuit before native; since the
        // review a repeat cancel reaches native, so this is the seam's no-op
        // branch, observed.
        TopicPath retiredTopic = Declare("fullyretired");
        TopicPath busyTopic = Declare("fullyretiredbusy");

        SubscribeResult retired = _subscriber.Subscribe(retiredTopic, (_, _, _) => { });
        retired.Schema.Dispose();
        retired.Subscription.Dispose(); // completes: now fully cancelled

        using var otherEntered = new Flag();
        using var releaseOther = new Flag();
        int otherExited = 0;
        SubscribeResult other = _subscriber.Subscribe(busyTopic, (_, _, _) =>
        {
            otherEntered.Set();
            releaseOther.WaitFor(Generous);
            Volatile.Write(ref otherExited, 1);
        });
        other.Schema.Dispose();

        Task delivery = Task.Run(() => PublishOne(busyTopic));
        otherEntered.MustFire("the unrelated handler never started");

        retired.Subscription.Dispose();
        Assert.Equal(0, Volatile.Read(ref otherExited));

        releaseOther.Set();
        Assert.True(await CompletesWithin(delivery, Generous));
        Assert.Equal(1, Volatile.Read(ref otherExited));
        other.Subscription.Dispose();
    }

    /// <summary>Mirrors CallerTier.ASubscribeDuringADrainKeepsItsProviderSubscription.</summary>
    [Fact]
    public async Task ASubscribeDuringADrainKeepsItsProviderSubscription()
    {
        // C++ observes the provider-level subscription surviving. The managed
        // surface cannot see that, so the mirror asserts what it exists to
        // guarantee: a subscription taken while another is draining still receives.
        //
        // WITH A REAL DRAIN WINDOW (rewritten after the BIND-4 review, B5). The
        // first version subscribed the newcomer while no cancel was running at all,
        // and cancelled the draining subscription only after its delivery had
        // finished - there was no drain to land in. Now the topic's LAST
        // subscription is cancelled on its own thread while its handler is parked,
        // that cancel is proved to be still blocked, and only then does the
        // newcomer subscribe. Over `inprocess` the newcomer's own Subscribe may
        // wait for the delivery too, so it runs on its own thread as well.
        TopicPath topic = Declare("subduringdrain");
        using var parked = new Flag();
        using var releaseParked = new Flag();
        using var cancellerAtCancel = new Flag();
        int cancellerReturned = 0;
        int newcomerCalls = 0;

        SubscribeResult draining = _subscriber.Subscribe(topic, (_, _, _) =>
        {
            parked.Set();
            releaseParked.WaitFor(Generous);
        });
        draining.Schema.Dispose();

        Task delivery = Task.Run(() => PublishOne(topic));
        parked.MustFire("the draining handler never started");

        Task canceller = Task.Run(() =>
        {
            cancellerAtCancel.Set();
            draining.Subscription.Dispose();
            Volatile.Write(ref cancellerReturned, 1);
        });
        cancellerAtCancel.MustFire("the canceller never started");
        Thread.Sleep(100);
        Assert.Equal(0, Volatile.Read(ref cancellerReturned));

        Task<SubscribeResult> newcomer = Task.Run(
            () => _subscriber.Subscribe(topic, (_, _, _) => Interlocked.Increment(ref newcomerCalls)));

        releaseParked.Set();
        Assert.True(await CompletesWithin(Task.WhenAll(canceller, newcomer, delivery), Generous));
        SubscribeResult fresh = await newcomer;
        fresh.Schema.Dispose();

        PublishOne(topic);
        Assert.Equal(1, Volatile.Read(ref newcomerCalls));
        fresh.Subscription.Dispose();
    }

    /// <summary>Mirrors CallerTier.ACancelRacingASelfCancelWaitsForThatHandler.</summary>
    [Fact]
    public async Task ACancelRacingASelfCancelWaitsForThatHandler()
    {
        // THE C++ ORDER, WHICH IS THE WHOLE CASE (rewritten after the BIND-4
        // review, B5). The handler cancels ITSELF FIRST - the carve-out, which does
        // not drain - and only THEN parks. A cancel of the same subscription from
        // another thread must still wait for that handler: the carve-out belongs to
        // the handler's own frame, not to every other caller. The first version
        // parked BEFORE self-cancelling, so the carve-out never ran while the other
        // cancel was waiting.
        //
        // MADE FAITHFUL, THIS CASE FOUND A DEFECT: the managed `Unsubscribe`
        // returned at once for a subscription already marked retired, so the other
        // thread's cancel came back while the handler was still running. A repeat
        // cancel now reaches native, which waits for the retirement in progress.
        // The sibling keeps the topic non-empty, so the provider is never entered
        // and its mutex cannot do the waiting instead (B6).
        TopicPath topic = Declare("racingselfcancel");
        using var entered = new Flag();
        using var release = new Flag();
        using var otherAtCancel = new Flag();
        int exited = 0;
        int releaseTimedOut = 0;
        SubscribeResult result = default;

        SubscribeResult sibling = _subscriber.Subscribe(topic, (_, _, _) => { });
        result = _subscriber.Subscribe(topic, (_, _, _) =>
        {
            result.Subscription.Dispose(); // the carve-out: does not drain
            entered.Set();
            if (!release.WaitFor(Generous))
            {
                Volatile.Write(ref releaseTimedOut, 1);
            }

            Volatile.Write(ref exited, 1);
        });
        sibling.Schema.Dispose();
        result.Schema.Dispose();

        Task delivery = Task.Run(() => PublishOne(topic));
        entered.MustFire("the handler never started");

        Task releaser = Task.Run(() =>
        {
            otherAtCancel.MustFire("the other thread never reached its cancel");
            Thread.Sleep(HoldWindow);
            release.Set();
        });

        otherAtCancel.Set();
        result.Subscription.Dispose(); // another thread, the same subscription
        int otherSawItExit = Volatile.Read(ref exited);

        Assert.True(await CompletesWithin(Task.WhenAll(releaser, delivery), Generous));
        Assert.Equal(0, Volatile.Read(ref releaseTimedOut));
        Assert.Equal(1, otherSawItExit);
        sibling.Subscription.Dispose();
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
    /// <remarks>
    /// WEAKER THAN ITS C++ ORIGINAL, AND THE LIMIT IS STRUCTURAL (BIND-4 review,
    /// B5). The C++ window is a sibling's callback PARKED on one thread while a
    /// handler on the same Subscriber, on another thread, cancels it - two
    /// deliveries in flight on ONE Subscriber. A Subscriber sits over one provider,
    /// and `inprocess` delivers one at a time per instance, so that window cannot
    /// be built from managed code, exactly as for
    /// <see cref="CrossCancellingDeliveriesDoNotDeadlock"/>. The first version
    /// claimed it anyway: sibling and canceller ran in one fan-out on one thread,
    /// the sibling had finished before it was cancelled, and its assertions were
    /// true of any build.
    ///
    /// What IS observable: a handler cancels a sibling on ANOTHER topic (a spare
    /// keeps that topic non-empty, as in C++); a third thread, never inside a
    /// delivery, then cancels the same sibling and returns; and the sibling never
    /// receives again. The managed half of the lifetime rule this case exists for -
    /// the carve-out defers its free until a drain has been waited for - is pinned
    /// by <c>ReentrancyTests.CancellingASiblingFromInsideAHandlerDefersTheFreeUntilADrainHasBeenWaitedFor</c>
    /// (D-BIND-50).
    /// </remarks>
    [Fact]
    public async Task CancellingASiblingRunningOnAnotherThreadKeepsItPublished()
    {
        TopicPath siblingTopic = Declare("siblingpublished");
        TopicPath cancellerTopic = Declare("siblingpublishedcanceller");
        int siblingCalls = 0;
        int handlerCancelled = 0;

        SubscribeResult spare = _subscriber.Subscribe(siblingTopic, (_, _, _) => { });
        SubscribeResult sibling = _subscriber.Subscribe(siblingTopic, (_, _, _) => Interlocked.Increment(ref siblingCalls));
        SubscribeResult canceller = _subscriber.Subscribe(cancellerTopic, (_, _, _) =>
        {
            sibling.Subscription.Dispose();
            Volatile.Write(ref handlerCancelled, 1);
        });
        spare.Schema.Dispose();
        sibling.Schema.Dispose();
        canceller.Schema.Dispose();

        PublishOne(cancellerTopic);
        Assert.Equal(1, Volatile.Read(ref handlerCancelled));
        Assert.False(sibling.Subscription.IsLive);

        Task third = Task.Run(() => sibling.Subscription.Dispose());
        Assert.True(await CompletesWithin(third, Generous), "a third thread's cancel of the sibling never returned");

        PublishOne(siblingTopic);
        Assert.Equal(0, Volatile.Read(ref siblingCalls));

        canceller.Subscription.Dispose();
        spare.Subscription.Dispose();
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
        // THE C++ INTERLEAVING (rewritten after the BIND-4 review, B5). Thread B
        // is inside a Subscribe to topic W, which over `inprocess` cannot finish
        // while a delivery holds the provider's mutex; a handler running inside that
        // very delivery then subscribes to W too. With the door BEFORE the wait, the
        // handler is refused by name and returns, the delivery ends, and B
        // completes. With the door behind the wait, the handler waits on B and B on
        // the handler.
        //
        // The first version declared a topic INSIDE the held window - `CreateTopic`
        // takes the same mutex - so its test thread blocked until the holder's latch
        // timed out (10 s, every run) and its stopwatch then timed a refusal with
        // nothing held. Every topic is declared up front now, and the handler's
        // latch result is checked rather than ignored.
        TopicPath trigger = Declare("doortrigger");
        TopicPath wanted = Declare("doorwanted");

        var faults = new List<Exception>();
        _subscriber.HandlerFaulted += (_, e) => faults.Add(e.Exception);

        using var handlerRunning = new Flag();
        using var bStarted = new Flag();
        int bParkedInTime = 0;
        long refusalMs = -1;

        SubscribeResult probe = _subscriber.Subscribe(trigger, (_, _, _) =>
        {
            handlerRunning.Set();

            // B is started only now, so its Subscribe lands while this delivery
            // holds the provider. Give it a moment to reach the provider's door.
            if (bStarted.WaitFor(Generous))
            {
                Volatile.Write(ref bParkedInTime, 1);
            }

            Thread.Sleep(50);

            var clock = System.Diagnostics.Stopwatch.StartNew();
            try
            {
                SubscribeResult nested = _subscriber.Subscribe(wanted, (_, _, _) => { });
                nested.Schema.Dispose();
            }
            finally
            {
                // Timed whether it returned or threw: the claim is about how long
                // the call took to answer, and a refusal answers by throwing.
                Interlocked.Exchange(ref refusalMs, clock.ElapsedMilliseconds);
            }
        });
        probe.Schema.Dispose();

        Task<SubscribeResult> b = Task.Run(() =>
        {
            handlerRunning.MustFire("the trigger handler never ran");
            bStarted.Set();
            return _subscriber.Subscribe(wanted, (_, _, _) => { });
        });

        Task delivery = Task.Run(() => PublishOne(trigger));

        Assert.True(await CompletesWithin(Task.WhenAll(delivery, b), Generous),
            "the delivery or B never finished: the door is behind a blocking wait again");
        Assert.Equal(1, Volatile.Read(ref bParkedInTime));

        Exception fault = Assert.Single(faults);
        Assert.IsType<InvalidOperationException>(fault);
        Assert.Contains(wanted.ToKey(), fault.Message, StringComparison.Ordinal);

        // The refusal did not wait for B, nor for the delivery it ran inside.
        long answeredIn = Interlocked.Read(ref refusalMs);
        Assert.True(answeredIn >= 0, "the nested Subscribe never ran");
        Assert.True(answeredIn < HoldWindow.TotalMilliseconds,
            $"the refusal took {answeredIn} ms - it waited on something before refusing");

        SubscribeResult bResult = await b;
        bResult.Schema.Dispose();
        bResult.Subscription.Dispose();
        probe.Subscription.Dispose();
    }
}
