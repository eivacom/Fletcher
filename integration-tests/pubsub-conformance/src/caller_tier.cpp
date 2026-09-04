// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The `CallerTier` suite: §7 clause 6 and cancellation idempotence, asserted at
// the tier §9 hands BIND-C#/BIND-Rust — `Subscriber`, not `PubSubProvider`.
//
// A FIFTH suite in this harness, for the same reason the copy oracle is a second
// one: what it asserts is unreachable from a provider-parameterised clause.
// `ProviderConformance` encodes §7's clauses and constructs no `Subscriber` at
// all (`src/clauses.cpp`), so every clause it proves is proved one tier below the
// one a language binding wraps. That gap is the whole item: `Subscriber` used to
// invoke a callback AFTER `Unsubscribe` returned, and to throw when asked to
// cancel something already gone, and nothing here could see either.
//
// Its binary links `fletcher-pubsub` and NO transport SDK. That narrow link line
// is itself the guard that these are seam properties and not one provider's.
//
// The subject is the probe provider below: it stores the provider-level callback
// and hands the test the trigger, so every case is driven by latches rather than
// sleeps — the timing window the defect lives in is MADE, not waited for.
//
// Vacuity rule for this file: no case may pass on "it did not throw" alone.
// Every case asserts a call count, delivered bytes, or an ordering fact.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <fletcher/core/status.hpp>
#include <fletcher/core/types.hpp>
#include <fletcher/pubsub/owned_schema.hpp>
#include <fletcher/pubsub/provider.hpp>
#include <fletcher/pubsub/subscriber.hpp>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fletcher {
namespace conformance {
namespace {

// How long a handshake may take before we call it a hung build. Generous: this
// bound exists so a WRONG build fails as an assertion with a name on it rather
// than as a bare ctest timeout. The two deadlock controls deliberately have no
// such bound — a build that reinstates the ABBA cycle hangs, and the target's
// ctest TIMEOUT is what reports it.
constexpr auto kGenerous = std::chrono::milliseconds(10000);

// How long a delivery is held inside its callback while the cancellation runs.
// The cancel must outlast it; a build that does not wait returns first.
constexpr auto kHoldWindow = std::chrono::milliseconds(200);

const std::vector<std::string> kT1{"caller", "tier", "one"};
const std::vector<std::string> kT2{"caller", "tier", "two"};

std::string Join(const std::vector<std::string>& segments) {
    std::string key;
    for (const std::string& segment : segments) {
        if (!key.empty()) key.push_back('/');
        key += segment;
    }
    return key;
}

// A row with a caller-chosen marker byte, so "it was delivered" is an assertion
// about bytes rather than about a counter that anything could have bumped.
std::vector<uint8_t> Row(uint8_t marker) { return {marker, 'R', 'O', 'W'}; }

// ── One-shot flag, waited on with a bound ───────────────────────────
class Flag {
   public:
    void Set() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            set_ = true;
        }
        cv_.notify_all();
    }
    [[nodiscard]] bool WaitFor(std::chrono::milliseconds budget) {
        std::unique_lock<std::mutex> lock(mu_);
        return cv_.wait_for(lock, budget, [this] { return set_; });
    }

   private:
    std::mutex mu_;
    std::condition_variable cv_;
    bool set_ = false;
};

// ── The probe provider ──────────────────────────────────────────────
//
// Stores the multiplex callback `Subscriber` registers and delivers on the
// thread the test chooses. It copies the callback out from under its own lock
// before invoking, so the probe can never be the thing that deadlocks — the only
// locks in play during a delivery are the Subscriber's own, which is what this
// suite is about.
//
// `wait_for_inflight_on_unsubscribe` turns it into the OTHER kind of provider:
// one that honours §7 clause 6 on its own side by refusing to return while a
// delivery is in flight, exactly as `provider.hpp` requires and as Fast DDS does
// (`fast_dds_pubsub_provider.cpp:570-574`). Only the edge-B control turns it on.
class ProbeProvider : public PubSubProvider {
   public:
    void CreateTopic(const std::vector<std::string>&, OwnedSchema) override {}
    void Publish(const std::vector<std::string>&, const RowEncoder&, const Attachments&) override {}

    [[nodiscard]] SubscriptionResult Subscribe(const std::vector<std::string>& segments,
                                               SubscribeCallback callback) override {
        // Deliberately OUTSIDE the probe's own lock: a real provider's Subscribe
        // does its work without holding anything of ours, and serialising it here
        // would make the concurrent-first-Subscribe case vacuous.
        if (subscribe_delay.count() > 0) std::this_thread::sleep_for(subscribe_delay);
        std::lock_guard<std::mutex> lock(mu_);
        callbacks_[Join(segments)] = std::move(callback);
        ++subscribe_calls;
        return SubscriptionResult{SchemaArrival::Ready(nullptr)};
    }

    void Unsubscribe(const std::vector<std::string>& segments) override {
        std::unique_lock<std::mutex> lock(mu_);
        callbacks_.erase(Join(segments));
        ++unsubscribe_calls;
        if (wait_for_inflight_on_unsubscribe) {
            cv_.wait(lock, [this] { return in_flight_ == 0; });
        }
    }

    // The trigger. Runs the fan-out on the calling thread.
    void Deliver(const std::vector<std::string>& segments, const std::vector<uint8_t>& row) {
        SubscribeCallback callback;
        {
            std::lock_guard<std::mutex> lock(mu_);
            auto it = callbacks_.find(Join(segments));
            if (it == callbacks_.end()) return;
            callback = it->second;
            ++in_flight_;
        }
        callback(row.data(), row.size(), SharedSchema{}, Attachments{});
        {
            std::lock_guard<std::mutex> lock(mu_);
            --in_flight_;
        }
        cv_.notify_all();
    }

    bool wait_for_inflight_on_unsubscribe = false;
    // How long provider->Subscribe takes. Fast DDS's create_datareader is far
    // above the 50 us at which the unserialised window was measured 400/400.
    std::chrono::milliseconds subscribe_delay{0};
    std::atomic<int> subscribe_calls{0};
    std::atomic<int> unsubscribe_calls{0};

   private:
    std::mutex mu_;
    std::condition_variable cv_;
    std::unordered_map<std::string, SubscribeCallback> callbacks_;
    int in_flight_ = 0;
};

using Sub = Subscriber::SubscribeCallback;

// ── Primary 1 — §7 clause 6, the half a snapshot fan-out cannot keep ─
//
// A cancellation that returns while its callback sits in a snapshot already
// loaded is the defect: the entry is invoked afterwards, and the caller — told
// by the seam that no further callback runs — has already freed the state that
// callback touches. `pubsub/tests/test_publisher_subscriber.cpp` asserted this
// as intended behaviour until this item.
TEST(CallerTier, NoCallbackAfterUnsubscribeReturns) {
    auto probe = std::make_shared<ProbeProvider>();
    Subscriber subscriber(probe);

    Flag first_entered;
    Flag release_first;
    std::atomic<int> second_calls{0};

    const uint64_t first = subscriber
                               .Subscribe(kT1, Sub{[&](uint64_t, const uint8_t*, size_t,
                                                       const SharedSchema&, const Attachments&) {
                                              first_entered.Set();
                                              ASSERT_TRUE(release_first.WaitFor(kGenerous));
                                          }})
                               .subscription_id;
    const uint64_t second =
        subscriber
            .Subscribe(kT1, Sub{[&](uint64_t, const uint8_t*, size_t, const SharedSchema&,
                                    const Attachments&) { ++second_calls; }})
            .subscription_id;

    std::thread delivery([&] { probe->Deliver(kT1, Row(1)); });
    ASSERT_TRUE(first_entered.WaitFor(kGenerous));
    ASSERT_EQ(second_calls.load(), 0) << "the fan-out reached the second entry too early: this "
                                         "case needs the delivery parked ahead of it";

    // The window the defect lives in: the snapshot holding `second` is loaded and
    // the loop has not reached it yet.
    subscriber.Unsubscribe(second);
    release_first.Set();
    delivery.join();

    EXPECT_EQ(second_calls.load(), 0)
        << "a callback ran after its own Unsubscribe returned — spec §7 clause 6";
    subscriber.Unsubscribe(first);
}

// ── Primary 2 — the wait the owner ruled for (2026-09-04) ───────────
//
// "Once cancel returns, that handler is not running and will not run again, so
// the application may immediately free whatever the handler was using."
TEST(CallerTier, UnsubscribeWaitsForAnInFlightDelivery) {
    auto probe = std::make_shared<ProbeProvider>();
    Subscriber subscriber(probe);

    Flag entered;
    Flag release;
    Flag main_at_unsubscribe;
    std::atomic<bool> exited{false};

    const uint64_t id = subscriber
                            .Subscribe(kT1, Sub{[&](uint64_t, const uint8_t*, size_t,
                                                    const SharedSchema&, const Attachments&) {
                                           entered.Set();
                                           ASSERT_TRUE(release.WaitFor(kGenerous));
                                           exited.store(true, std::memory_order_release);
                                       }})
                            .subscription_id;

    std::thread delivery([&] { probe->Deliver(kT1, Row(1)); });
    ASSERT_TRUE(entered.WaitFor(kGenerous));

    // The handler is freed only once the main thread has committed to the cancel,
    // so "the cancel returned first" cannot be an artefact of scheduling order.
    std::thread releaser([&] {
        ASSERT_TRUE(main_at_unsubscribe.WaitFor(kGenerous));
        std::this_thread::sleep_for(kHoldWindow);
        release.Set();
    });

    main_at_unsubscribe.Set();
    subscriber.Unsubscribe(id);
    const bool exited_at_return = exited.load(std::memory_order_acquire);

    releaser.join();
    delivery.join();

    EXPECT_TRUE(exited_at_return)
        << "Unsubscribe returned while that subscription's callback was still running: the caller "
           "may not free handler state, which is the guarantee this item exists to deliver";
}

// ── Primary 3 — idempotence (owner ruling 2026-09-04) ───────────────
//
// A C#/Rust finaliser cancels unconditionally and cannot let an error escape.
TEST(CallerTier, UnsubscribeOfAnUnknownIdIsANoOp) {
    auto probe = std::make_shared<ProbeProvider>();
    Subscriber subscriber(probe);

    std::atomic<int> live_calls{0};
    const uint64_t live =
        subscriber
            .Subscribe(kT1, Sub{[&](uint64_t, const uint8_t*, size_t, const SharedSchema&,
                                    const Attachments&) { ++live_calls; }})
            .subscription_id;
    const uint64_t gone = subscriber
                              .Subscribe(kT2, Sub{[](uint64_t, const uint8_t*, size_t,
                                                     const SharedSchema&, const Attachments&) {}})
                              .subscription_id;
    subscriber.Unsubscribe(gone);

    EXPECT_NO_THROW(subscriber.Unsubscribe(gone)) << "cancelling twice";
    EXPECT_NO_THROW(subscriber.Unsubscribe(gone + 4096)) << "cancelling an id never issued";
    EXPECT_NO_THROW(subscriber.Unsubscribe(0)) << "cancelling id 0, which is never issued";

    // The no-op must be a no-op: "accepted and does nothing", not "quietly
    // cancelled something else".
    probe->Deliver(kT1, Row(7));
    EXPECT_EQ(live_calls.load(), 1) << "a no-op cancellation disturbed a live subscription";
    subscriber.Unsubscribe(live);
}

// ── Control — the self shape the owner's carve-out names ────────────
//
// Without the per-Subscriber delivery-depth scope this self-deadlocks on its own
// non-recursive gate and dies on the target's ctest TIMEOUT. The sibling keeps
// the topic non-empty on purpose: what a PROVIDER does with a re-entrant
// Unsubscribe is premise P5's, owned by A3, and this suite claims nothing
// whatever about it.
TEST(CallerTier, SelfUnsubscribeInsideItsOwnCallbackReturns) {
    auto probe = std::make_shared<ProbeProvider>();
    Subscriber subscriber(probe);

    std::atomic<int> sibling_calls{0};
    std::atomic<int> self_calls{0};

    (void)subscriber.Subscribe(kT1, Sub{[&](uint64_t, const uint8_t*, size_t, const SharedSchema&,
                                            const Attachments&) { ++sibling_calls; }});
    (void)subscriber.Subscribe(
        kT1, Sub{[&](uint64_t id, const uint8_t*, size_t, const SharedSchema&, const Attachments&) {
            ++self_calls;
            subscriber.Unsubscribe(id);
        }});

    probe->Deliver(kT1, Row(1));
    probe->Deliver(kT1, Row(2));

    EXPECT_EQ(self_calls.load(), 1) << "the self-cancelled entry was invoked again";
    EXPECT_EQ(sibling_calls.load(), 2) << "the gate silenced a subscription that is still live";
}

// ── Control — edge A: gate → gate is forbidden, not handled ─────────
//
// Two deliveries on two topics of one Subscriber, each callback cancelling the
// other's subscription. Ordinary on Fast DDS, which runs a listener per reader.
// Any build that lets a thread already inside a delivery block on a gate forms
// the ABBA cycle and HANGS here — the target's ctest TIMEOUT is the report.
TEST(CallerTier, CrossCancellingDeliveriesDoNotDeadlock) {
    auto probe = std::make_shared<ProbeProvider>();
    Subscriber subscriber(probe);

    // A spare live entry per topic, so neither cancellation empties its topic and
    // neither reaches provider->Unsubscribe (P5 again).
    (void)subscriber.Subscribe(
        kT1, Sub{[](uint64_t, const uint8_t*, size_t, const SharedSchema&, const Attachments&) {}});
    (void)subscriber.Subscribe(
        kT2, Sub{[](uint64_t, const uint8_t*, size_t, const SharedSchema&, const Attachments&) {}});

    Flag in_one;
    Flag in_two;
    std::atomic<uint64_t> id_one{0};
    std::atomic<uint64_t> id_two{0};
    std::atomic<int> one_calls{0};
    std::atomic<int> two_calls{0};

    id_one = subscriber
                 .Subscribe(kT1, Sub{[&](uint64_t, const uint8_t*, size_t, const SharedSchema&,
                                         const Attachments&) {
                                ++one_calls;
                                in_one.Set();
                                ASSERT_TRUE(in_two.WaitFor(kGenerous));
                                subscriber.Unsubscribe(id_two.load());
                            }})
                 .subscription_id;
    id_two = subscriber
                 .Subscribe(kT2, Sub{[&](uint64_t, const uint8_t*, size_t, const SharedSchema&,
                                         const Attachments&) {
                                ++two_calls;
                                in_two.Set();
                                ASSERT_TRUE(in_one.WaitFor(kGenerous));
                                subscriber.Unsubscribe(id_one.load());
                            }})
                 .subscription_id;

    std::thread one([&] { probe->Deliver(kT1, Row(1)); });
    std::thread two([&] { probe->Deliver(kT2, Row(2)); });
    one.join();
    two.join();

    ASSERT_EQ(one_calls.load(), 1);
    ASSERT_EQ(two_calls.load(), 1);

    probe->Deliver(kT1, Row(3));
    probe->Deliver(kT2, Row(4));
    EXPECT_EQ(one_calls.load(), 1) << "a cross-cancelled subscription was invoked again";
    EXPECT_EQ(two_calls.load(), 1) << "a cross-cancelled subscription was invoked again";
}

// ── Control — edge B: no gate is held while the provider is entered ──
//
// The design's live check for this edge was `ProviderConformance` against Fast
// DDS, which constructs no `Subscriber` at all (A4-DEBT-6), so edge B had no
// control anywhere. This is one, and it needs no transport: the probe is
// switched into the shape `provider.hpp` requires of every provider — its
// Unsubscribe refuses to return while a delivery is in flight.
//
// The shape: a delivery is parked inside an entry that has ALREADY cancelled
// itself, so the cancellation of the last remaining entry empties the topic and
// enters the provider while the fan-out loop still has that entry's gate ahead
// of it. A build that scopes the barrier as a function-scoped lock_guard holds
// that gate across the provider call, the provider waits for the delivery, and
// the delivery blocks on the gate. HANGS — the ctest TIMEOUT is the report.
TEST(CallerTier, UnsubscribeDoesNotHoldAGateWhileEnteringTheProvider) {
    auto probe = std::make_shared<ProbeProvider>();
    probe->wait_for_inflight_on_unsubscribe = true;
    Subscriber subscriber(probe);

    Flag parked;
    Flag release_parked;
    Flag main_at_unsubscribe;
    std::atomic<int> last_calls{0};

    (void)subscriber.Subscribe(
        kT1, Sub{[&](uint64_t id, const uint8_t*, size_t, const SharedSchema&, const Attachments&) {
            subscriber.Unsubscribe(id);  // from inside: does not wait
            parked.Set();
            ASSERT_TRUE(release_parked.WaitFor(kGenerous));
        }});
    const uint64_t last =
        subscriber
            .Subscribe(kT1, Sub{[&](uint64_t, const uint8_t*, size_t, const SharedSchema&,
                                    const Attachments&) { ++last_calls; }})
            .subscription_id;

    std::thread delivery([&] { probe->Deliver(kT1, Row(1)); });
    ASSERT_TRUE(parked.WaitFor(kGenerous));

    std::thread releaser([&] {
        ASSERT_TRUE(main_at_unsubscribe.WaitFor(kGenerous));
        std::this_thread::sleep_for(kHoldWindow);
        release_parked.Set();
    });

    main_at_unsubscribe.Set();
    subscriber.Unsubscribe(last);  // empties the topic → enters the provider

    releaser.join();
    delivery.join();

    EXPECT_EQ(last_calls.load(), 0) << "the retired last entry was invoked by the parked delivery";
    EXPECT_EQ(probe->unsubscribe_calls.load(), 1) << "the provider was never entered at all, so "
                                                     "this case controlled nothing";
}

// ── Control — the mu ↔ gate edge, in the permitted direction ────────
TEST(CallerTier, ReentrantSubscribeFromInsideDeliveryDoesNotDeadlock) {
    auto probe = std::make_shared<ProbeProvider>();
    Subscriber subscriber(probe);

    std::atomic<int> outer_calls{0};
    std::atomic<int> added_calls{0};
    std::atomic<uint64_t> added_id{0};

    (void)subscriber.Subscribe(
        kT1, Sub{[&](uint64_t, const uint8_t*, size_t, const SharedSchema&, const Attachments&) {
            if (outer_calls.fetch_add(1) != 0) return;
            added_id =
                subscriber
                    .Subscribe(kT1, Sub{[&](uint64_t, const uint8_t*, size_t, const SharedSchema&,
                                            const Attachments&) { ++added_calls; }})
                    .subscription_id;
        }});

    probe->Deliver(kT1, Row(1));
    EXPECT_NE(added_id.load(), 0u) << "the re-entrant Subscribe never returned an id";
    EXPECT_EQ(added_calls.load(), 0) << "a subscription created mid-delivery joined the delivery "
                                        "already in progress";

    probe->Deliver(kT1, Row(2));
    EXPECT_EQ(added_calls.load(), 1) << "a subscription created mid-delivery never received";
}

// ── Control — the gate did not simply silence delivery ──────────────
TEST(CallerTier, ALiveSubscriptionStillReceives) {
    auto probe = std::make_shared<ProbeProvider>();
    Subscriber subscriber(probe);

    std::vector<uint8_t> seen;
    const uint64_t dropped =
        subscriber
            .Subscribe(kT1, Sub{[](uint64_t, const uint8_t*, size_t, const SharedSchema&,
                                   const Attachments&) {}})
            .subscription_id;
    (void)subscriber.Subscribe(
        kT1, Sub{[&](uint64_t, const uint8_t* data, size_t len, const SharedSchema&,
                     const Attachments&) { seen.assign(data, data + len); }});

    subscriber.Unsubscribe(dropped);
    probe->Deliver(kT1, Row(9));

    EXPECT_EQ(seen, Row(9)) << "the surviving subscription received nothing, or the wrong bytes";
}

// ── Pins premise P4 — ids are never reused ──────────────────────────
//
// "Unknown id is a no-op" is only safe while a released id can never come back:
// otherwise the no-op silently becomes "cancels a stranger".
TEST(CallerTier, AReleasedIdIsNeverReused) {
    auto probe = std::make_shared<ProbeProvider>();
    Subscriber subscriber(probe);

    std::set<uint64_t> seen;
    uint64_t previous = 0;
    for (int i = 0; i < 32; ++i) {
        const uint64_t id = subscriber
                                .Subscribe(kT1, Sub{[](uint64_t, const uint8_t*, size_t,
                                                       const SharedSchema&, const Attachments&) {}})
                                .subscription_id;
        EXPECT_TRUE(seen.insert(id).second) << "id " << id << " was handed out twice";
        EXPECT_GT(id, previous) << "ids stopped increasing at iteration " << i;
        previous = id;
        subscriber.Unsubscribe(id);
    }
    EXPECT_EQ(seen.size(), 32u);
}

// ── ~Subscriber drains too, or the guarantee has a teardown hole ────
TEST(CallerTier, DestructorDrainsAnInFlightDelivery) {
    auto probe = std::make_shared<ProbeProvider>();

    Flag entered;
    Flag release;
    Flag main_at_destroy;
    std::atomic<bool> exited{false};
    std::thread delivery;
    std::thread releaser;

    {
        Subscriber subscriber(probe);
        (void)subscriber.Subscribe(kT1, Sub{[&](uint64_t, const uint8_t*, size_t,
                                                const SharedSchema&, const Attachments&) {
                                       entered.Set();
                                       ASSERT_TRUE(release.WaitFor(kGenerous));
                                       exited.store(true, std::memory_order_release);
                                   }});

        delivery = std::thread([&] { probe->Deliver(kT1, Row(1)); });
        ASSERT_TRUE(entered.WaitFor(kGenerous));

        releaser = std::thread([&] {
            ASSERT_TRUE(main_at_destroy.WaitFor(kGenerous));
            std::this_thread::sleep_for(kHoldWindow);
            release.Set();
        });
        main_at_destroy.Set();
    }
    const bool exited_at_return = exited.load(std::memory_order_acquire);

    releaser.join();
    delivery.join();

    EXPECT_TRUE(exited_at_return)
        << "~Subscriber returned while a callback was still running: the same use-after-free, "
           "reached through teardown instead of through cancellation";
}

// ── Control on the per-Subscriber scope of the depth predicate ──────
//
// The mechanism the owner ruled on (2026-09-04, "one subscriber object"), and
// the one nothing else in this suite can see: every other case constructs ONE
// `Subscriber`, so widening the predicate back to process-wide leaves them all
// green while reinstating the use-after-free the ruling was given to remove.
//
// X's handler cancels a subscription on Y while Y's delivery is parked. Because
// the skip is keyed on the subscriber, X is NOT inside a delivery on Y, so it
// takes Y's barrier and waits — which is what makes the published sentence true
// for Y's caller.
TEST(CallerTier, CancellingOnAnotherSubscriberWaitsForItsDelivery) {
    auto probe = std::make_shared<ProbeProvider>();
    Subscriber x(probe);
    Subscriber y(probe);

    Flag y_entered;
    Flag release_y;
    Flag x_at_cancel;
    std::atomic<bool> y_exited{false};
    std::atomic<bool> y_exited_at_return{false};

    const uint64_t y_id = y.Subscribe(kT2, Sub{[&](uint64_t, const uint8_t*, size_t,
                                                   const SharedSchema&, const Attachments&) {
                                          y_entered.Set();
                                          ASSERT_TRUE(release_y.WaitFor(kGenerous));
                                          y_exited.store(true, std::memory_order_release);
                                      }})
                              .subscription_id;

    (void)x.Subscribe(
        kT1, Sub{[&](uint64_t, const uint8_t*, size_t, const SharedSchema&, const Attachments&) {
            ASSERT_TRUE(y_entered.WaitFor(kGenerous));
            x_at_cancel.Set();
            y.Unsubscribe(y_id);
            y_exited_at_return.store(y_exited.load(std::memory_order_acquire),
                                     std::memory_order_release);
        }});

    std::thread y_delivery([&] { probe->Deliver(kT2, Row(1)); });
    ASSERT_TRUE(y_entered.WaitFor(kGenerous));

    std::thread releaser([&] {
        ASSERT_TRUE(x_at_cancel.WaitFor(kGenerous));
        std::this_thread::sleep_for(kHoldWindow);
        release_y.Set();
    });

    probe->Deliver(kT1, Row(2));  // runs X's handler on this thread

    releaser.join();
    y_delivery.join();

    EXPECT_TRUE(y_exited_at_return.load(std::memory_order_acquire))
        << "a handler on one Subscriber cancelled on ANOTHER and did not wait: that caller reads "
           "the published sentence, frees its handler state, and the handler is still running";
}

// ── A duplicate cancel waits for the same drain (ruling 2026-09-04) ─
//
// Two threads cancelling one id: the loser used to take the no-op branch and
// return while the handler was still running — a second, unpublished exception
// to the frozen promise, reachable only under a race. It now waits for the
// winner's drain.
TEST(CallerTier, ADuplicateCancelWaitsForTheDrainInProgress) {
    auto probe = std::make_shared<ProbeProvider>();
    Subscriber subscriber(probe);

    Flag entered;
    Flag release;
    Flag winner_at_unsubscribe;
    Flag duplicate_at_unsubscribe;
    std::atomic<bool> exited{false};

    const uint64_t id = subscriber
                            .Subscribe(kT1, Sub{[&](uint64_t, const uint8_t*, size_t,
                                                    const SharedSchema&, const Attachments&) {
                                           entered.Set();
                                           ASSERT_TRUE(release.WaitFor(kGenerous));
                                           exited.store(true, std::memory_order_release);
                                       }})
                            .subscription_id;

    std::thread delivery([&] { probe->Deliver(kT1, Row(1)); });
    ASSERT_TRUE(entered.WaitFor(kGenerous));

    std::atomic<bool> winner_returned{false};
    std::thread winner([&] {
        winner_at_unsubscribe.Set();
        subscriber.Unsubscribe(id);
        winner_returned.store(true, std::memory_order_release);
    });
    ASSERT_TRUE(winner_at_unsubscribe.WaitFor(kGenerous));
    // Let the winner get past its critical section and into the drain, which is
    // where the id stops being live and starts being "retiring".
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    // ...and prove it is still there, so a short sleep cannot green this case
    // without ever reaching the branch it names.
    ASSERT_FALSE(winner_returned.load(std::memory_order_acquire))
        << "the winner had already finished draining: this case never exercised the duplicate "
           "branch";

    std::thread releaser([&] {
        ASSERT_TRUE(duplicate_at_unsubscribe.WaitFor(kGenerous));
        std::this_thread::sleep_for(kHoldWindow);
        release.Set();
    });

    duplicate_at_unsubscribe.Set();
    subscriber.Unsubscribe(id);  // the duplicate
    const bool duplicate_saw_it_exit = exited.load(std::memory_order_acquire);

    winner.join();
    releaser.join();
    delivery.join();

    EXPECT_TRUE(duplicate_saw_it_exit)
        << "the duplicate cancel returned while the handler was still running, so its caller may "
           "not free handler state after all — a second exception to a promise that has one";
}

// ── ...and the OTHER half of the same ruling: a fully cancelled id is
//    a silent no-op that does NOT wait for anything ────────────────
//
// Written as its own case because these two branches are what a reader cannot
// tell apart from the code alone: "being cancelled right now" waits, "unknown or
// already fully cancelled" does not.
TEST(CallerTier, ACancelOfAFullyRetiredIdReturnsWithoutWaiting) {
    auto probe = std::make_shared<ProbeProvider>();
    Subscriber subscriber(probe);

    const uint64_t retired =
        subscriber
            .Subscribe(kT1, Sub{[](uint64_t, const uint8_t*, size_t, const SharedSchema&,
                                   const Attachments&) {}})
            .subscription_id;
    subscriber.Unsubscribe(retired);  // completes: `retired` is now fully cancelled

    Flag other_entered;
    Flag release_other;
    std::atomic<bool> other_exited{false};
    (void)subscriber.Subscribe(
        kT2, Sub{[&](uint64_t, const uint8_t*, size_t, const SharedSchema&, const Attachments&) {
            other_entered.Set();
            ASSERT_TRUE(release_other.WaitFor(kGenerous));
            other_exited.store(true, std::memory_order_release);
        }});

    std::thread delivery([&] { probe->Deliver(kT2, Row(1)); });
    ASSERT_TRUE(other_entered.WaitFor(kGenerous));

    EXPECT_NO_THROW(subscriber.Unsubscribe(retired)) << "cancelling a fully cancelled id";
    EXPECT_FALSE(other_exited.load(std::memory_order_acquire))
        << "a no-op cancel waited for an unrelated delivery: the two branches have been collapsed "
           "into one";
    EXPECT_NO_THROW(subscriber.Unsubscribe(retired + 100000)) << "cancelling an id never issued";
    EXPECT_FALSE(other_exited.load(std::memory_order_acquire));

    release_other.Set();
    delivery.join();
}

// ── B1: a Subscribe landing in the drain window survives it ─────────
//
// The drain makes the provider-level transition window as long as a handler
// runs. Publishing `provider_subscribed = false` before draining meant a
// newcomer subscribing in that window registered a fresh provider subscription
// which the drainer then tore down — and nothing repaired it, so the newcomer
// held a valid id and received nothing, ever, with no error anywhere. Reachable
// in the gateway, where one `Subscriber` is shared by every WS session.
TEST(CallerTier, ASubscribeDuringADrainKeepsItsProviderSubscription) {
    auto probe = std::make_shared<ProbeProvider>();
    Subscriber subscriber(probe);

    Flag parked;
    Flag release_parked;
    Flag canceller_at_unsubscribe;

    const uint64_t parked_id =
        subscriber
            .Subscribe(kT1, Sub{[&](uint64_t, const uint8_t*, size_t, const SharedSchema&,
                                    const Attachments&) {
                           parked.Set();
                           ASSERT_TRUE(release_parked.WaitFor(kGenerous));
                       }})
            .subscription_id;

    std::thread delivery([&] { probe->Deliver(kT1, Row(1)); });
    ASSERT_TRUE(parked.WaitFor(kGenerous));

    // Cancels the topic's LAST subscription and then blocks in the drain for as
    // long as the parked handler takes.
    std::atomic<bool> canceller_returned{false};
    std::thread canceller([&] {
        canceller_at_unsubscribe.Set();
        subscriber.Unsubscribe(parked_id);
        canceller_returned.store(true, std::memory_order_release);
    });
    ASSERT_TRUE(canceller_at_unsubscribe.WaitFor(kGenerous));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ASSERT_FALSE(canceller_returned.load(std::memory_order_acquire))
        << "the cancellation had already completed: the newcomer below would not land in the "
           "drain window at all";

    // The newcomer, landing squarely inside the drain window.
    std::atomic<int> newcomer_calls{0};
    const uint64_t newcomer =
        subscriber
            .Subscribe(kT1, Sub{[&](uint64_t, const uint8_t*, size_t, const SharedSchema&,
                                    const Attachments&) { ++newcomer_calls; }})
            .subscription_id;
    ASSERT_NE(newcomer, 0u);

    release_parked.Set();
    canceller.join();
    delivery.join();

    probe->Deliver(kT1, Row(5));
    EXPECT_EQ(newcomer_calls.load(), 1)
        << "a subscription created during a drain receives nothing: its provider-level "
           "subscription was torn down by the drain it landed in, and nothing repairs it";
    EXPECT_EQ(probe->unsubscribe_calls.load(), 0)
        << "the topic was released at the provider while it still had a live subscriber";
}

// ── The carve-out must not un-publish the drain for everyone else ───
//
// A handler cancels its OWN subscription: the carve-out skips the barrier,
// because a cancellation cannot wait for the frame it is already in. That skip is
// the one authorised exception, and it belongs to *that* caller. It must not also
// hide the drain from a cancel arriving on another thread, which is covered by no
// exception at all — that thread reads the published sentence, frees its handler
// state, and the handler is still running.
//
// The sibling keeps the topic non-empty so the provider is never re-entered (P5).
TEST(CallerTier, ACancelRacingASelfCancelWaitsForThatHandler) {
    auto probe = std::make_shared<ProbeProvider>();
    Subscriber subscriber(probe);

    Flag entered;
    Flag release;
    Flag other_at_cancel;
    std::atomic<bool> exited{false};

    (void)subscriber.Subscribe(
        kT1, Sub{[](uint64_t, const uint8_t*, size_t, const SharedSchema&, const Attachments&) {}});
    const uint64_t self_id =
        subscriber
            .Subscribe(kT1, Sub{[&](uint64_t id, const uint8_t*, size_t, const SharedSchema&,
                                    const Attachments&) {
                           subscriber.Unsubscribe(id);  // the carve-out: skips the barrier
                           entered.Set();
                           ASSERT_TRUE(release.WaitFor(kGenerous));
                           exited.store(true, std::memory_order_release);
                       }})
            .subscription_id;

    std::thread delivery([&] { probe->Deliver(kT1, Row(1)); });
    ASSERT_TRUE(entered.WaitFor(kGenerous));

    std::thread releaser([&] {
        ASSERT_TRUE(other_at_cancel.WaitFor(kGenerous));
        std::this_thread::sleep_for(kHoldWindow);
        release.Set();
    });

    other_at_cancel.Set();
    subscriber.Unsubscribe(self_id);  // another thread, the same id
    const bool other_saw_it_exit = exited.load(std::memory_order_acquire);

    releaser.join();
    delivery.join();

    EXPECT_TRUE(other_saw_it_exit)
        << "a cancel from outside the handler returned while that handler was still running: the "
           "self-cancel un-published the drain, so the promise has a second exception";
}

// ── RB1: two first-Subscribes on one topic register once ────────────
//
// `EnsureProviderSubscription` releases `mu` across `provider->Subscribe`, so
// without serialisation both callers read `provider_subscribed == false` and both
// register — measured 400/400 duplicate provider subscriptions at 50 us of
// provider work, which is the outcome under contention rather than a race. Fast
// DDS then refuses the loser with `kInvalidArgument` on a valid Subscribe; the
// loopback replaces the slot and reports `kSubscriptionEnded` to a live
// subscriber's arrival. Reachable in the gateway, where one `Subscriber` is
// shared by every WS session.
TEST(CallerTier, ConcurrentFirstSubscribesCreateOneProviderSubscription) {
    auto probe = std::make_shared<ProbeProvider>();
    probe->subscribe_delay = std::chrono::milliseconds(50);
    Subscriber subscriber(probe);

    Flag go;
    std::atomic<int> first_calls{0};
    std::atomic<int> second_calls{0};

    std::thread one([&] {
        ASSERT_TRUE(go.WaitFor(kGenerous));
        (void)subscriber.Subscribe(kT1,
                                   Sub{[&](uint64_t, const uint8_t*, size_t, const SharedSchema&,
                                           const Attachments&) { ++first_calls; }});
    });
    std::thread two([&] {
        ASSERT_TRUE(go.WaitFor(kGenerous));
        (void)subscriber.Subscribe(kT1,
                                   Sub{[&](uint64_t, const uint8_t*, size_t, const SharedSchema&,
                                           const Attachments&) { ++second_calls; }});
    });
    go.Set();
    one.join();
    two.join();

    EXPECT_EQ(probe->subscribe_calls.load(), 1)
        << "both callers registered a provider-level subscription for one topic";

    // Vacuity guard: serialising must not have cost either of them its delivery.
    probe->Deliver(kT1, Row(3));
    EXPECT_EQ(first_calls.load(), 1);
    EXPECT_EQ(second_calls.load(), 1);
}

}  // namespace
}  // namespace conformance
}  // namespace fletcher
