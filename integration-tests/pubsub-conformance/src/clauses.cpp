// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The delivery contract of docs/pubsub-interface-spec.md §7 (plus §6 clause 1),
// encoded ONCE and run against every subject. Each clause names its authority.
//
// A clause body sees only a ProviderSubject: no PubSubProvider&, no Publish, no
// CreateTopic. That is what makes an in-process shortcut on a cross-process
// subject unrepresentable rather than a review risk.
//
// Every clause declares its Collector first and its ScopedSubscription second,
// so the subscription always dies before the storage its callback writes into.
// See ScopedSubscription: a trailing Unsubscribe cannot do that job, because an
// ASSERT_ failure path never reaches it.
//
// PDA-DEC-AG1 appends five more clauses at the bottom of this file, covering
// §5.3 and §6 clause 6 — what a MISBEHAVING callback does. They are numbered in
// their own AG1-n series in the comments, because §7's twelve are already
// numbered here.
//
// Clause 2 (CallbackNeverSeesNullSchema) lives in clauses_carried.cpp, which is
// linked only into the schema-CARRYING subjects' binaries: the axis gate is
// applied at link/instantiation, so on a schema-less subject the clause is
// absent from the ctest list rather than present and skipped. There is no
// GTEST_SKIP anywhere in this suite.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <fletcher/core/internal/status_name.hpp>
#include <fletcher/core/status.hpp>
#include <fletcher/pubsub/delivery_channel.hpp>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "fletcher/conformance/suite.hpp"

namespace fletcher {
namespace conformance {
namespace {

std::vector<uint32_t> Range(uint32_t first, uint32_t last) {
    std::vector<uint32_t> out;
    for (uint32_t i = first; i <= last; ++i) {
        out.push_back(i);
    }
    return out;
}

}  // namespace

// ── Clause 1 (§7 clause 1 + clause 2) ───────────────────────────────
// The forcing test. A subscriber that joins before any publisher exists still
// receives every row, in order, with the schema always in hand — and never a
// live sample ahead of one buffered before the schema arrived.
TEST_P(ProviderConformance, SchemaBeforeDataAcrossHandoff) {
    const Topic topic = Fresh("handoff");
    constexpr uint32_t kRows = 5;

    Collector collector;
    ScopedSubscription sub(Subject(), topic, collector.Callback());

    CONF_MUST_DECLARE(topic, DataSchema());
    // No-op except on Fast DDS: waits for the reader to match the writer just
    // declared before the immediate publish below (subject.hpp).
    Subject().AwaitDataMatched(topic, RemainingBudget());
    for (uint32_t seq = 1; seq <= kRows; ++seq) {
        CONF_MUST_PUBLISH(topic, seq);
    }

    ASSERT_TRUE(collector.WaitForCount(kRows, Deadline()))
        << "only " << collector.Count() << " of " << kRows << " rows arrived";
    EXPECT_EQ(collector.Foreign(), 0u) << "a non-row payload reached the data callback";
    EXPECT_EQ(collector.Seqs(), Range(1, kRows)) << "per-writer order broke across the handoff";
    for (const Collector::Delivery& d : collector.Snapshot()) {
        EXPECT_EQ(d.had_schema, Carried())
            << "row " << d.seq << " carried " << (d.had_schema ? "a" : "no")
            << " schema, which is not this transport's mode";
    }
}

// ── Clause 3 (§7 clause 1, last sentence) ───────────────────────────
// Schema-carrying and schema-less are the two modes, and a transport is in
// exactly one of them for every delivery: "must never mix the two".
TEST_P(ProviderConformance, SchemaModeIsUniformNeverMixed) {
    const Topic topic = Fresh("uniform_mode");

    CONF_MUST_DECLARE(topic, DataSchema());
    Collector collector;
    ScopedSubscription sub(Subject(), topic, collector.Callback());
    Subject().AwaitDataMatched(topic, RemainingBudget());

    for (uint32_t seq = 1; seq <= 3; ++seq) {
        CONF_MUST_PUBLISH(topic, seq);
    }
    ASSERT_TRUE(collector.WaitForCount(3, Deadline())) << "first batch never arrived";
    for (uint32_t seq = 4; seq <= 6; ++seq) {
        CONF_MUST_PUBLISH(topic, seq);
    }
    ASSERT_TRUE(collector.WaitForCount(6, Deadline())) << "second batch never arrived";

    const std::vector<Collector::Delivery> got = collector.Snapshot();
    for (const Collector::Delivery& d : got) {
        EXPECT_EQ(d.had_schema, got.front().had_schema)
            << "row " << d.seq << " mixed the two schema modes within one subscription";
        EXPECT_EQ(d.had_schema, Carried());
    }
}

// ── Clause 4 (§7 clause 2) ──────────────────────────────────────────
// Samples from a single writer reach the callback in publish order.
TEST_P(ProviderConformance, PerWriterOrderIsMonotonic) {
    const Topic topic = Fresh("writer_order");
    constexpr uint32_t kRows = 20;

    CONF_MUST_DECLARE(topic, DataSchema());
    Collector collector;
    ScopedSubscription sub(Subject(), topic, collector.Callback());
    Subject().AwaitDataMatched(topic, RemainingBudget());

    for (uint32_t seq = 1; seq <= kRows; ++seq) {
        CONF_MUST_PUBLISH(topic, seq);
    }
    ASSERT_TRUE(collector.WaitForCount(kRows, Deadline()))
        << "only " << collector.Count() << " of " << kRows << " rows arrived";
    EXPECT_EQ(collector.Seqs(), Range(1, kRows));
}

// ── Clause 5 (§7 clause 2, the handoff half) ────────────────────────
// The buffered pre-schema backlog is delivered before, and never interleaved
// with, samples arriving live afterwards.
TEST_P(ProviderConformance, BacklogNeverInterleavesWithLiveSamples) {
    const Topic topic = Fresh("no_interleave");

    Collector collector;
    ScopedSubscription sub(Subject(), topic, collector.Callback());

    CONF_MUST_DECLARE(topic, DataSchema());
    Subject().AwaitDataMatched(topic, RemainingBudget());
    for (uint32_t seq = 1; seq <= 3; ++seq) {
        CONF_MUST_PUBLISH(topic, seq);
    }
    // Wait until delivery has demonstrably started, so the rows below really are
    // "live afterwards" rather than part of the same burst.
    ASSERT_TRUE(collector.WaitForCount(1, Deadline())) << "nothing arrived at all";
    for (uint32_t seq = 4; seq <= 6; ++seq) {
        CONF_MUST_PUBLISH(topic, seq);
    }

    ASSERT_TRUE(collector.WaitForCount(6, Deadline()))
        << "only " << collector.Count() << " of 6 rows arrived";
    EXPECT_EQ(collector.Seqs(), Range(1, 6)) << "a live sample overtook a buffered one";
}

// ── Clause 6 (locked decision 12 + §7 clause 1 "buffered and delivered") ──
// All or nothing. A transport either replays every row retained before the
// subscriber existed, or none of them. Partial delivery fails under BOTH trait
// values, which is the point: the shipped receive-side data-sharing defect
// delivered "often just the newest sample".
//
// NOTHING is published after Subscribe, and that is load-bearing rather than
// incidental. An earlier version of this clause published one live "sentinel"
// row after subscribing and waited for it, to give the dropping case a
// deterministic end. Because that row comes from the SAME writer under
// RELIABLE + KEEP_ALL, a reliable reader cannot release it while seqs 1..N are
// missing, so waiting on it FORCED NACK/repair of exactly the gap this clause
// exists to observe: the clause then measured a backlog the transport had been
// compelled to repair, not the one it replayed at match time. Measured, not
// theorised — with the receive-side data-sharing defect deliberately restored,
// the sentinel version passed 12/12 while integration-tests/gateway-fastdds-ts
// (which publishes nothing after its rows) failed with the documented
// signature. So the wait here is bounded by the clause's own deadline instead,
// the way clauses 9 and 11 already bound theirs.
TEST_P(ProviderConformance, LateJoinerBacklogIsAllOrNothing) {
    const Topic topic = Fresh("late_joiner");
    constexpr uint32_t kBacklog = 5;

    CONF_MUST_DECLARE(topic, DataSchema());
    for (uint32_t seq = 1; seq <= kBacklog; ++seq) {
        CONF_MUST_PUBLISH(topic, seq);
    }

    // The late joiner. No publish of any kind past this line.
    Collector collector;
    ScopedSubscription sub(Subject(), topic, collector.Callback());

    if (Retains()) {
        EXPECT_TRUE(collector.WaitForCount(kBacklog, Deadline()))
            << "a retaining transport replayed only " << collector.Count() << " of " << kBacklog
            << " retained rows within the clause budget — partial is never acceptable";
    } else {
        EXPECT_FALSE(collector.WaitForCount(1, SettleDeadline()))
            << "a dropping transport replayed a row published before the subscriber existed";
    }

    // All or nothing, asserted independently of which wait ran: no count between
    // 0 and N passes under either trait value.
    const std::vector<uint32_t> seqs = collector.Seqs();
    const auto replayed = static_cast<uint32_t>(
        std::count_if(seqs.begin(), seqs.end(), [](uint32_t s) { return s <= kBacklog; }));
    EXPECT_EQ(replayed, Retains() ? kBacklog : 0u)
        << "replayed " << replayed << " of " << kBacklog
        << " rows published before the subscriber existed; this transport's retention trait "
           "allows only all or none";
    EXPECT_EQ(collector.Foreign(), 0u) << "a non-row payload reached the data callback";
}

// ── Clause 7 (§7 clause 3, first half) ──────────────────────────────
// Re-declaring with an identical schema is idempotent, so several publishers
// may share one topic. Asserted as "no observable change": the second
// declaration reports no failure and the topic still delivers.
TEST_P(ProviderConformance, IdenticalRedeclarationIsIdempotent) {
    const Topic topic = Fresh("redeclare_same");

    // The real schema on every subject: this clause is about the declaration,
    // not about delivery.
    CONF_MUST_DECLARE(topic, SchemaId::kA);
    CONF_MUST_DECLARE(topic, SchemaId::kA);

    Collector collector;
    ScopedSubscription sub(Subject(), topic, collector.Callback());
    Subject().AwaitDataMatched(topic, RemainingBudget());
    CONF_MUST_PUBLISH(topic, 1);
    ASSERT_TRUE(collector.WaitForCount(1, Deadline()))
        << "the topic stopped delivering after an identical re-declaration";
    EXPECT_EQ(collector.Seqs(), Range(1, 1));
}

// ── Clause 8 (§7 clause 3, second half — AMENDED in this PR) ────────
// A conflicting re-declaration MUST be rejected (owner ruling 2026-09-01; the
// spec's "may" became "must" in the same change). Asserts only THAT the
// PROVIDER refused: the seam has no exception taxonomy yet, so asserting which
// failure would be inventing one here.
//
// `refused()`, not "not ok". A dead peer, an expired deadline or a garbled reply
// are Outcome::kHarnessFailure and must NOT satisfy this clause — a negative
// clause that passes when the harness breaks is worse than no clause, and this
// is the clause the loopback and XRCE conflict fixes exist to satisfy.
TEST_P(ProviderConformance, ConflictingRedeclarationIsRejected) {
    const Topic topic = Fresh("redeclare_conflict");

    CONF_MUST_DECLARE(topic, SchemaId::kA);
    const Reply reply = Subject().DeclareTopic(topic, SchemaId::kB);
    EXPECT_TRUE(reply.refused())
        << "re-declaring struct<seq:int32> as struct<seq:int32,extra:float64> was not refused by "
           "the provider; outcome was "
        << (reply.ok() ? "accepted" : "a harness failure") << ": " << reply.detail;
}

// ── Clause 9 (§7 clause 4) ──────────────────────────────────────────
// One callback per topic per instance. Cardinality only: exactly one delivery
// across two registrations, without asserting which registration wins — §7
// states the cardinality, not a winner.
TEST_P(ProviderConformance, OneCallbackPerTopicPerInstance) {
    const Topic topic = Fresh("one_callback");

    CONF_MUST_DECLARE(topic, DataSchema());
    Collector collector;  // shared, so the count is across BOTH registrations
    ScopedSubscription first(Subject(), topic, collector.Callback());
    // std::optional so the second subscription lives to the end of the clause
    // rather than to the end of the try block, and still tears down before the
    // collector.
    std::optional<ScopedSubscription> second;
    try {
        second.emplace(Subject(), topic, collector.Callback());
    } catch (const std::exception&) {
        // Refusing the second registration is one legal way to hold the
        // cardinality; replacing the first is the other.
    }
    Subject().AwaitDataMatched(topic, RemainingBudget());

    CONF_MUST_PUBLISH(topic, 1);
    ASSERT_TRUE(collector.WaitForCount(1, Deadline())) << "the row reached no callback at all";
    EXPECT_FALSE(collector.WaitForCount(2, SettleDeadline()))
        << "one published row was delivered twice on one instance";
    EXPECT_EQ(collector.Count(), 1u);
}

// ── Clause 10 (§7 clause 5) ─────────────────────────────────────────
// Subscribe never blocks; a late joiner gets the schema asynchronously.
TEST_P(ProviderConformance, SubscribeNeverBlocksSchemaArrivesLater) {
    const Topic topic = Fresh("late_schema");

    Collector collector;
    const auto before = std::chrono::steady_clock::now();
    ScopedSubscription sub(Subject(), topic, collector.Callback());
    const auto elapsed = std::chrono::steady_clock::now() - before;
    EXPECT_LT(elapsed, kSubscribeCeiling)
        << "Subscribe on a topic no publisher has declared took "
        << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() << " ms";

    CONF_MUST_DECLARE(topic, DataSchema());
    Subject().AwaitDataMatched(topic, RemainingBudget());
    CONF_MUST_PUBLISH(topic, 1);
    ASSERT_TRUE(collector.WaitForSeq(1, Deadline())) << "the row never arrived";

    SharedSchema arrived;
    const PubSubStatus status = sub.Schema().Wait(RemainingBudget(), &arrived);
    if (Carried()) {
        ASSERT_EQ(status, PubSubStatus::kOk)
            << "the schema arrival never answered kOk: " << internal::PubSubStatusName(status)
            << " (" << sub.Schema().Message() << ")";
        EXPECT_NE(arrived, nullptr);
    } else {
        // §7 clause 1's sanctioned schema-less transport: kOk with a NULL schema,
        // answered rather than left pending, so a waiter never hangs — and
        // distinct from kSubscriptionEnded, which would mean something else
        // entirely.
        ASSERT_EQ(status, PubSubStatus::kOk) << internal::PubSubStatusName(status);
        EXPECT_EQ(arrived, nullptr);
    }
}

// ── Clause 11 (§7 clause 6) ─────────────────────────────────────────
// After Unsubscribe returns, no further callback for that topic.
TEST_P(ProviderConformance, NoDeliveryAfterUnsubscribeReturns) {
    const Topic topic = Fresh("after_unsubscribe");

    CONF_MUST_DECLARE(topic, DataSchema());
    Collector collector;
    ScopedSubscription sub(Subject(), topic, collector.Callback());
    Subject().AwaitDataMatched(topic, RemainingBudget());

    for (uint32_t seq = 1; seq <= 3; ++seq) {
        CONF_MUST_PUBLISH(topic, seq);
    }
    ASSERT_TRUE(collector.WaitForCount(3, Deadline())) << "delivery never started";

    // The behaviour under test, so it is an explicit call here rather than the
    // scope exit. The scope exit still runs and is an idempotent no-op.
    Subject().Unsubscribe(topic);
    for (uint32_t seq = 4; seq <= 8; ++seq) {
        CONF_MUST_PUBLISH(topic, seq);
    }
    EXPECT_FALSE(collector.WaitForCount(4, SettleDeadline()))
        << "a callback ran after Unsubscribe returned";
    EXPECT_EQ(collector.Count(), 3u);
}

// ── Clause 12 (§6 clause 1) ─────────────────────────────────────────
// Delivery is serialized per subscription: never two deliveries in flight for
// one subscription, though the thread may differ between samples.
//
// Honesty note (also in the README): on a cross-process subject the peer
// protocol is one request/reply at a time, so two concurrent publishes are
// impossible and this clause is an OBSERVATION there, not a proof. On the
// in-process subjects PublishRow is a direct call, so the two publishing
// threads below are genuinely concurrent and this is a real assertion.
TEST_P(ProviderConformance, DeliveryIsSerializedPerSubscription) {
    const Topic topic = Fresh("serialized_delivery");
    constexpr uint32_t kRows = 8;

    CONF_MUST_DECLARE(topic, DataSchema());
    Collector collector;
    // Widen the window a delivery occupies so a second one has something to
    // overlap with. A busy wait, not a sleep: stalling a provider thread is not
    // what is under test. Set before Subscribe, so no provider thread can read
    // it concurrently with this write.
    collector.SetHoldWindow(std::chrono::microseconds(500));
    ScopedSubscription sub(Subject(), topic, collector.Callback());
    Subject().AwaitDataMatched(topic, RemainingBudget());

    Reply reply_a;
    Reply reply_b;
    std::thread a([&] {
        for (uint32_t seq = 1; seq <= kRows / 2; ++seq) {
            Reply r = Subject().PublishRow(topic, seq);
            if (!r.ok() && reply_a.ok()) {
                reply_a = r;
            }
        }
    });
    std::thread b([&] {
        for (uint32_t seq = kRows / 2 + 1; seq <= kRows; ++seq) {
            Reply r = Subject().PublishRow(topic, seq);
            if (!r.ok() && reply_b.ok()) {
                reply_b = r;
            }
        }
    });
    a.join();
    b.join();
    ASSERT_TRUE(reply_a.ok()) << "publish failed: " << reply_a.detail;
    ASSERT_TRUE(reply_b.ok()) << "publish failed: " << reply_b.detail;

    ASSERT_TRUE(collector.WaitForCount(kRows, Deadline()))
        << "only " << collector.Count() << " of " << kRows << " rows arrived";
    EXPECT_EQ(collector.MaxInFlight(), 1u)
        << "two deliveries were in flight at once on one subscription";
}

// ── PDA-DEC-AG1 — the misbehaving callback (§5.3, §6 clauses 5 and 6) ─
//
// One question, five clauses: what happens when a subscriber's delivery callback
// misbehaves — by failing, or by calling back into the seam from inside itself?
//
// They are deliberately MUTUALLY IRREDUCIBLE, because this item absorbed two
// that were separate (a throwing callback, and re-entrancy), and a grouping is
// only legitimate if neither mechanism can green the other's control:
//
//   * AG1-2 has NO exception anywhere on its path — the refusal is caught and
//     compared inside the callback's own frame — so absorption cannot green it;
//   * AG1-3 has NO re-entry on its path, so the door check is not on it, and its
//     absorbed-count assertion can be satisfied by nothing but the absorption;
//   * AG1-1, the forcing test, needs BOTH;
//   * AG1-6 needs THE DOOR, and only the door: it asserts that the other three
//     methods refuse too, which absorption cannot produce and which a door on
//     `Unsubscribe` alone does not reach.
//
// Each of them reddens by HANGING on at least one subject when its mechanism is
// absent, so the per-target ctest TIMEOUT is load-bearing here exactly as it is
// for the CallerTier deadlock controls: an uncapped hang is not a red.

namespace {

// What a callback recorded, as a number. Two sentinels outside the enum, so "not
// refused at all" and "something that is not a seam error" stay distinguishable
// from every real status instead of collapsing into one failure.
constexpr int32_t kNothingRecorded = -1;
constexpr int32_t kReturnedWithoutThrowing = -2;
constexpr int32_t kNonSeamException = -3;

std::string StatusText(int32_t recorded) {
    switch (recorded) {
        case kNothingRecorded:
            return "nothing — the callback never got there";
        case kReturnedWithoutThrowing:
            return "no refusal at all — the call was SERVED";
        case kNonSeamException:
            return "an exception that is not a PubSubError";
        default:
            return internal::PubSubStatusName(static_cast<PubSubStatus>(recorded));
    }
}

// Issue `Unsubscribe` on this instance and report what came back.
//
// The catch and the comparison are INSIDE the callback's own frame, deliberately:
// that is what keeps every exception off the re-entrancy clauses' paths, so no
// catch anywhere else in the tree can be the thing that makes them pass.
int32_t RecordReentrantUnsubscribe(ProviderSubject& subject, const Topic& topic) {
    try {
        subject.Unsubscribe(topic);
    } catch (const PubSubError& e) {
        return static_cast<int32_t>(e.status());
    } catch (...) {
        return kNonSeamException;
    }
    return kReturnedWithoutThrowing;
}

// A one-shot flag with a bounded wait, so a signal that never comes is a named
// assertion rather than the target's TIMEOUT.
class Latch {
   public:
    void Set() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            set_ = true;
        }
        cv_.notify_all();
    }
    [[nodiscard]] bool WaitUntil(std::chrono::steady_clock::time_point deadline) {
        std::unique_lock<std::mutex> lock(mu_);
        return cv_.wait_until(lock, deadline, [this] { return set_; });
    }

   private:
    std::mutex mu_;
    std::condition_variable cv_;
    bool set_ = false;
};

}  // namespace

// ── Clause AG1-1 (§5.3 + §6 clause 6) — THE FORCING TEST ────────────
//
// One handler that both fails AND calls back in. The call-back-in is refused by
// a distinct named error; the failure is contained; and the publisher on the
// other side completes normally, as does the next unrelated caller.
//
// Owner ruling 2026-09-05 ("nothing — the publish succeeds") is the second half.
// Today the loopback lets `std::overflow_error` unwind out of the callback into
// the PUBLISHER's TranslateSeamFailure, where §5.1's normative mapping turns it
// into `kPayloadTooLarge` — a subscriber's bug reported to an unrelated
// publisher as a payload problem it neither caused nor can act on.
//
// Honesty, in the shape clause 12 already uses on itself: on the DDS subjects
// delivery is asynchronous, so `PublishRow` has returned before the handler runs
// and the `ok()` below is an observation there rather than a proof. On the
// loopback the publish IS the delivery, and that is the subject the mis-charging
// lives on. The named-refusal half is a real assertion on all five subjects.
TEST_P(ProviderConformance, HostileCallbackNeitherEscapesNorIsChargedElsewhere) {
    const Topic topic = Fresh("hostile");
    const Topic after = Fresh("hostile_after");
    CONF_MUST_DECLARE(topic, DataSchema());

    std::atomic<int32_t> recorded{kNothingRecorded};
    std::atomic<int> entered{0};
    Latch handled;

    ScopedSubscription sub(Subject(), topic,
                           [&](const uint8_t*, size_t, const SharedSchema&, const Attachments&) {
                               if (entered.fetch_add(1) != 0) return;
                               recorded.store(RecordReentrantUnsubscribe(Subject(), topic));
                               handled.Set();
                               // …and then fails, in the very frame the refusal
                               // was just handed to.
                               throw std::overflow_error("a hostile handler");
                           });
    Subject().AwaitDataMatched(topic, RemainingBudget());

    const Reply published = Subject().PublishRow(topic, 1);
    EXPECT_TRUE(published.ok()) << "a subscriber's handler failure was charged to the publisher: "
                                << published.detail;

    ASSERT_TRUE(handled.WaitUntil(Deadline())) << "the handler never ran, so nothing was tested";
    EXPECT_EQ(recorded.load(), static_cast<int32_t>(PubSubStatus::kReentrantCall))
        << "cancelling from inside a delivery on this instance answered with "
        << StatusText(recorded.load());

    // The seam is not wedged and the failure did not travel: an unrelated caller
    // afterwards succeeds.
    EXPECT_TRUE(Subject().DeclareTopic(after, DataSchema()).ok());
    EXPECT_TRUE(Subject().PublishRow(after, 1).ok());
}

// ── Clause AG1-2 (§6 clause 6) — live negative control for the refusal ─
//
// The same re-entrant cancel with NOTHING thrown anywhere on the path, so the
// absorption half of this item cannot be what makes it pass. Remove the door and
// it reddens by hanging on the loopback and on Fast DDS, or by recording a
// SERVED call on XRCE — and none of those three is an exception.
TEST_P(ProviderConformance, ReentrantCallIsRefusedWithoutAnyThrow) {
    const Topic topic = Fresh("reentrant_only");
    CONF_MUST_DECLARE(topic, DataSchema());

    std::atomic<int32_t> recorded{kNothingRecorded};
    std::atomic<int> entered{0};
    Latch handled;

    ScopedSubscription sub(Subject(), topic,
                           [&](const uint8_t*, size_t, const SharedSchema&, const Attachments&) {
                               if (entered.fetch_add(1) != 0) return;
                               recorded.store(RecordReentrantUnsubscribe(Subject(), topic));
                               handled.Set();
                           });
    Subject().AwaitDataMatched(topic, RemainingBudget());

    CONF_MUST_PUBLISH(topic, 1);
    ASSERT_TRUE(handled.WaitUntil(Deadline())) << "the handler never ran, so nothing was tested";
    EXPECT_EQ(recorded.load(), static_cast<int32_t>(PubSubStatus::kReentrantCall))
        << "cancelling from inside a delivery on this instance answered with "
        << StatusText(recorded.load());
}

// ── Clause AG1-3 (§5.3) — live negative control for the absorption ──
//
// A handler that throws and NEVER re-enters, so the door check is not on this
// path and cannot be what makes it pass.
//
// ABSORBED, not merely unobserved. Deleting the per-provider catch blocks removed
// the only log line a failing handler produced, and `pubsub/src` has no logging
// facility at all, so the count is the observable that replaces it — and
// asserting it is what stops "nothing was reported" being satisfied by "nothing
// happened".
TEST_P(ProviderConformance, ThrowingCallbackIsAbsorbedWithoutReentering) {
    const Topic topic = Fresh("throwing_only");
    CONF_MUST_DECLARE(topic, DataSchema());

    std::atomic<int> entered{0};
    Latch threw;

    ScopedSubscription sub(Subject(), topic,
                           [&](const uint8_t*, size_t, const SharedSchema&, const Attachments&) {
                               if (entered.fetch_add(1) != 0) return;
                               threw.Set();
                               throw std::overflow_error("a failing handler");
                           });
    Subject().AwaitDataMatched(topic, RemainingBudget());

    const uint64_t before = DeliveryChannel::AbsorbedTotal();
    const Reply published = Subject().PublishRow(topic, 1);
    EXPECT_TRUE(published.ok()) << "a subscriber's handler failure was charged to the publisher: "
                                << published.detail;

    ASSERT_TRUE(threw.WaitUntil(Deadline())) << "the handler never ran, so nothing was tested";
    // The latch is set BEFORE the throw — it has to be — so the count is waited
    // for rather than read once. Bounded by this clause's own deadline.
    while (DeliveryChannel::AbsorbedTotal() < before + 1 &&
           std::chrono::steady_clock::now() < Deadline()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_EQ(DeliveryChannel::AbsorbedTotal(), before + 1)
        << "the handler's failure was not absorbed at the dispatch site: a count that never moved "
           "means it went somewhere else, or nowhere at all";

    // Not wedged: the seam still takes the next row.
    EXPECT_TRUE(Subject().PublishRow(topic, 2).ok());
}

// ── Clause AG1-4 — not-too-wide control, THREAD axis ────────────────
//
// The refusal is per THREAD, not per instance. A second thread issuing a seam
// call while a delivery is in flight is not re-entrancy and must not be refused;
// it waits for the drain, exactly as §7 clause 6 requires of every provider.
//
// It asserts only that the call was ISSUED and did not see `kReentrantCall` —
// deliberately NOT that it completed while the delivery was still running, which
// the loopback makes impossible by design -- it holds its instance mutex across
// dispatch -- and which would therefore be a guaranteed false red under the
// target's TIMEOUT. The
// anti-widening force rests on the DDS subjects, where it does complete
// concurrently.
TEST_P(ProviderConformance, AnotherThreadIsNotRefusedDuringADelivery) {
    const Topic topic = Fresh("other_thread");
    const Topic idle = Fresh("other_thread_idle");
    CONF_MUST_DECLARE(topic, DataSchema());

    Latch in_delivery;
    Latch may_return;
    std::atomic<int> entered{0};

    ScopedSubscription sub(Subject(), topic,
                           [&](const uint8_t*, size_t, const SharedSchema&, const Attachments&) {
                               if (entered.fetch_add(1) != 0) return;
                               in_delivery.Set();
                               (void)may_return.WaitUntil(SettleDeadline());
                           });
    Subject().AwaitDataMatched(topic, RemainingBudget());

    std::thread publisher([&] { (void)Subject().PublishRow(topic, 1); });
    // However this clause exits — including through the ASSERT below — the parked
    // handler is released and the publishing thread is joined. A joinable thread
    // destroyed on an early return is a std::terminate, not a red.
    struct ReleaseAndJoin {
        Latch& latch;
        std::thread& thread;
        ~ReleaseAndJoin() {
            latch.Set();
            if (thread.joinable()) thread.join();
        }
    } cleanup{may_return, publisher};

    ASSERT_TRUE(in_delivery.WaitUntil(Deadline())) << "no delivery ever started";

    std::atomic<int32_t> from_other_thread{kNothingRecorded};
    std::thread other(
        [&] { from_other_thread.store(RecordReentrantUnsubscribe(Subject(), idle)); });
    // Released first and joined after: the other thread may legitimately BLOCK
    // until the handler returns — that is the loopback's gate, and the drain
    // every provider owes — and blocking is not being refused.
    may_return.Set();
    other.join();

    EXPECT_NE(from_other_thread.load(), static_cast<int32_t>(PubSubStatus::kReentrantCall))
        << "a call from a SECOND thread during a delivery was refused as re-entrant; the refusal "
           "has been widened from per-thread to per-instance";
}

// ── Clause AG1-6 — the METHOD axis of §6 clause 6: refused UNIFORMLY, and LOUDLY
//
// Owner ruling 2026-09-05 ("re-entry is refused on every protocol") replaced the
// ruling this clause was first written to: the earlier one permitted
// `CreateTopic`, `Publish` and `Subscribe` from inside a delivery, on the claim
// they already worked on Fast DDS and XRCE. They do not — a Fast DDS listener
// callback holds the RTPS reader mutex and each of them HANGS there, probed one
// at a time. The clause is re-aimed, not deleted, and it now pins BOTH failure
// directions at once:
//
//   * **no protocol may silently HANG** — every call must return, which the
//     per-clause ctest TIMEOUT enforces and no in-body assertion can; and
//   * **no protocol may silently SERVE** — each call must come back refused, by
//     the NAME `kReentrantCall`, not merely "not ok" and not with a success.
//
// Measured with the doors disabled and nothing else changed, the three providers
// give three different wrong answers, which is why one clause has to cover both
// directions: the loopback answers `kInternal` (MSVC's "resource deadlock would
// occur" from re-locking the mutex held across dispatch), Fast DDS HANGS and is
// killed by the TIMEOUT, and XRCE SERVES all three calls and reports success.
// One clause, three defects, no per-provider branch.
//
// WHY IT IS STILL INDEPENDENT of clauses AG1-1 and AG1-2, which is the property
// the charter constraint rests on. Those two pin `Unsubscribe` alone, and a door
// on `Unsubscribe` alone greens both of them — and leaves this clause red on
// every subject. Against AG1-3 it pins the opposite edge: a refusal widened from
// per-thread to per-instance would green this clause and RED AG1-3, and a
// refusal narrowed back to one method greens AG1-3 and reds this. No single
// mechanism greens all three; each needs the door to be exactly as wide as the
// spec says and no wider.
//
// WHICH SUBJECTS CARRY WHICH PART. `Subscribe` reaches the provider under test
// directly on all six subjects, so its assertion is unconditional. On a peer
// subject `DeclareTopic` and `PublishRow` go over the pipe to a child process and
// are NOT re-entrant — a callback reaching a second provider instance is
// expressly not re-entrancy (§6 clause 6) — so asserting a refusal there would
// assert a bug. They are asserted where they are genuinely re-entrant, which
// `publishes_into_subject_instance` names structurally rather than by matching on
// a label.
//
// `SubscribeSchema`/`UnsubscribeSchema`/`DeclareTopicWith`/`SubscribeWith` beside `Subscribe`: all
// five are LOCAL-ONLY on every subject (subject.hpp), never routed over the peer pipe, so their
// assertion is unconditional on all six subjects too — the same reason `Subscribe`'s is.
// `DeclareTopicWith` and `SubscribeWith` are passed a request with EVERY optional field non-empty
// (`profile`, `max_payload_bytes`, `check`), so what they pin is door-before-field-refusal: the
// re-entrancy door answers `kReentrantCall` before any of those fields is ever looked at, on a
// provider that could otherwise have answered `kNotSupported` or `kInvalidArgument` for one of
// them.
TEST_P(ProviderConformance, EveryProviderMethodIsRefusedFromInsideADelivery) {
    const bool reentrant_publish = GetParam().publishes_into_subject_instance;

    const Topic driver = Fresh("refused_driver");
    const Topic derived = Fresh("refused_derived");
    const Topic watched = Fresh("refused_watched");
    const Topic derived_local = Fresh("refused_derived_local");
    CONF_MUST_DECLARE(driver, DataSchema());

    Reply declare_reply = Reply::HarnessFailure("the handler never got there");
    Reply publish_reply = Reply::HarnessFailure("the handler never got there");
    std::atomic<int32_t> subscribe_status{kNothingRecorded};
    std::atomic<int32_t> subscribe_schema_status{kNothingRecorded};
    std::atomic<int32_t> unsubscribe_schema_status{kNothingRecorded};
    std::atomic<int32_t> declare_with_status{kNothingRecorded};
    std::atomic<int32_t> subscribe_with_status{kNothingRecorded};
    std::atomic<int> entered{0};
    Latch handled;

    ScopedSubscription driver_sub(
        Subject(), driver, [&](const uint8_t*, size_t, const SharedSchema&, const Attachments&) {
            if (entered.fetch_add(1) != 0) return;
            declare_reply = Subject().DeclareTopic(derived, DataSchema());
            publish_reply = Subject().PublishRow(derived, 77);
            try {
                SubscriptionResult opened = Subject().Subscribe(
                    watched,
                    [](const uint8_t*, size_t, const SharedSchema&, const Attachments&) {});
                (void)opened;
                subscribe_status.store(kReturnedWithoutThrowing);
            } catch (const PubSubError& e) {
                subscribe_status.store(static_cast<int32_t>(e.status()));
            } catch (...) {
                subscribe_status.store(kNonSeamException);
            }
            try {
                SchemaArrival opened = Subject().SubscribeSchema(watched);
                (void)opened;
                subscribe_schema_status.store(kReturnedWithoutThrowing);
            } catch (const PubSubError& e) {
                subscribe_schema_status.store(static_cast<int32_t>(e.status()));
            } catch (...) {
                subscribe_schema_status.store(kNonSeamException);
            }
            try {
                Subject().UnsubscribeSchema(watched);
                unsubscribe_schema_status.store(kReturnedWithoutThrowing);
            } catch (const PubSubError& e) {
                unsubscribe_schema_status.store(static_cast<int32_t>(e.status()));
            } catch (...) {
                unsubscribe_schema_status.store(kNonSeamException);
            }
            try {
                Subject().DeclareTopicWith(derived_local,
                                           {.schema = MakeConformanceSchema(DataSchema()),
                                            .profile = "x",
                                            .max_payload_bytes = 1024});
                declare_with_status.store(kReturnedWithoutThrowing);
            } catch (const PubSubError& e) {
                declare_with_status.store(static_cast<int32_t>(e.status()));
            } catch (...) {
                declare_with_status.store(kNonSeamException);
            }
            try {
                SubscriptionResult opened = Subject().SubscribeWith(
                    watched, {.callback = [](const uint8_t*, size_t, const SharedSchema&,
                                             const Attachments&) {},
                              .profile = "x",
                              .check = [](const SharedSchema&) { return true; }});
                (void)opened;
                subscribe_with_status.store(kReturnedWithoutThrowing);
            } catch (const PubSubError& e) {
                subscribe_with_status.store(static_cast<int32_t>(e.status()));
            } catch (...) {
                subscribe_with_status.store(kNonSeamException);
            }
            handled.Set();
        });
    Subject().AwaitDataMatched(driver, RemainingBudget());

    CONF_MUST_PUBLISH(driver, 1);
    ASSERT_TRUE(handled.WaitUntil(Deadline())) << "the handler never ran, so nothing was tested";

    // `Subscribe`, `SubscribeSchema` and `UnsubscribeSchema` all carry force on
    // every subject — the subscriber side is always this process and this
    // instance (subject.hpp), peer or not — and each status is asserted by
    // NAME, so a provider that refuses for some other reason does not green
    // this.
    EXPECT_EQ(subscribe_status.load(), static_cast<int32_t>(PubSubStatus::kReentrantCall))
        << "Subscribe from inside a delivery answered with " << StatusText(subscribe_status.load())
        << "; §6 clause 6 refuses every seam method on every provider";
    EXPECT_EQ(subscribe_schema_status.load(), static_cast<int32_t>(PubSubStatus::kReentrantCall))
        << "SubscribeSchema from inside a delivery answered with "
        << StatusText(subscribe_schema_status.load())
        << "; §6 clause 6 refuses the schema-only methods too";
    EXPECT_EQ(unsubscribe_schema_status.load(), static_cast<int32_t>(PubSubStatus::kReentrantCall))
        << "UnsubscribeSchema from inside a delivery answered with "
        << StatusText(unsubscribe_schema_status.load())
        << "; §6 clause 6 refuses the schema-only methods too";
    EXPECT_EQ(declare_with_status.load(), static_cast<int32_t>(PubSubStatus::kReentrantCall))
        << "DeclareTopicWith from inside a delivery answered with "
        << StatusText(declare_with_status.load())
        << "; §6 clause 6 refuses the door before any field of a non-empty request is looked at";
    EXPECT_EQ(subscribe_with_status.load(), static_cast<int32_t>(PubSubStatus::kReentrantCall))
        << "SubscribeWith from inside a delivery answered with "
        << StatusText(subscribe_with_status.load())
        << "; §6 clause 6 refuses the door before any field of a non-empty request is looked at";

    if (reentrant_publish) {
        // `refused()`, never "!ok()": Reply's third outcome exists precisely so a
        // clause asserting a refusal cannot be satisfied by a harness failure.
        EXPECT_TRUE(declare_reply.refused())
            << "CreateTopic from inside a delivery on the subject's own instance was not refused: "
            << declare_reply.detail;
        EXPECT_TRUE(publish_reply.refused())
            << "Publish from inside a delivery on the subject's own instance was not refused: "
            << publish_reply.detail;
    } else {
        // Not re-entrancy at all — a different instance in a different process.
        // Asserted rather than skipped, so the clause still says something here:
        // the refusal must not have leaked across the instance boundary.
        EXPECT_TRUE(declare_reply.ok())
            << "CreateTopic on a PEER instance was refused from inside a delivery; the refusal has "
               "leaked past the instance it belongs to: "
            << declare_reply.detail;
        EXPECT_TRUE(publish_reply.ok())
            << "Publish on a PEER instance was refused from inside a delivery; the refusal has "
               "leaked past the instance it belongs to: "
            << publish_reply.detail;
    }

    // The refusal must leave the subscription usable rather than half-torn: an
    // ordinary publish after the handler returns is still delivered.
    Collector after;
    ScopedSubscription after_sub(Subject(), derived, after.Callback());
    CONF_MUST_DECLARE(derived, DataSchema());
    Subject().AwaitDataMatched(derived, RemainingBudget());
    CONF_MUST_PUBLISH(derived, 78);
    EXPECT_TRUE(after.WaitForSeq(78, Deadline()))
        << "the provider stopped serving after refusing a re-entrant call";
}

// ── Checked subscribe (provider.hpp's own `SubscriptionRequest::check` contract) ────
//
// The `check` field's contract, doc'd beside it in provider.hpp: it runs once, gates whether the
// data side opens, and never delivers a row it rejected. What "later" means differs by whether the
// schema was already known when SubscribeWith was called with a non-empty `check` (synchronous:
// throws) or arrives after (asynchronous: the arrival fails kSchemaConflict) — every clause below
// runs the asynchronous shape, since it opens the checked subscription before CONF_MUST_DECLARE.

namespace {

// A checked subscribe has no ScopedSubscription-equivalent constructor (suite.hpp's
// ScopedSubscription always calls Subscribe, never SubscribeWith with a `check`), so each clause
// below unsubscribes through its own guard, the same shape as ScopedSubscription's destructor: a
// collector still alive when a delivery thread calls into it is the bug that type exists to make
// unrepresentable, on every exit path including an ASSERT_ failure. Declare it AFTER the Collector
// it feeds, for the same reason.
class CheckedSubscriptionGuard {
   public:
    CheckedSubscriptionGuard(ProviderSubject& subject, Topic topic)
        : subject_(subject), topic_(std::move(topic)) {}
    ~CheckedSubscriptionGuard() { Run(); }
    // Idempotent, so it is also safe to call explicitly before destruction —
    // ARejectedCheckedSubscriptionLeavesTheTopicReusable needs the topic freed mid-test, before a
    // fresh subscription reuses it, and the destructor's own call then finds nothing left to do.
    void Run() {
        if (ran_) return;
        ran_ = true;
        try {
            subject_.Unsubscribe(topic_);
        } catch (...) {
        }
    }
    CheckedSubscriptionGuard(const CheckedSubscriptionGuard&) = delete;
    CheckedSubscriptionGuard& operator=(const CheckedSubscriptionGuard&) = delete;

   private:
    ProviderSubject& subject_;
    Topic topic_;
    bool ran_ = false;
};

}  // namespace

// A rejecting check never lets a row cross, on the two paths a rejection reaches the caller: a
// synchronous throw (this clause's harness order — the checked subscribe before the declaration —
// never takes it, but a provider is free to learn of the conflict earlier and throw anyway) or the
// arrival failing kSchemaConflict. A schema-less transport runs no check at all and delivers like
// a plain Subscribe.
TEST_P(ProviderConformance, ACheckedSubscriptionThatRejectsNeverDelivers) {
    const Topic topic = Fresh("checked_reject");
    std::atomic<int> check_calls{0};
    Collector collector;
    std::optional<CheckedSubscriptionGuard> guard;

    bool threw = false;
    PubSubStatus thrown_status = PubSubStatus::kOk;
    SubscriptionResult result;
    try {
        result = Subject().SubscribeWith(
            topic, {.callback = collector.Callback(), .check = [&](const SharedSchema&) {
                        check_calls.fetch_add(1);
                        return false;
                    }});
        guard.emplace(Subject(), topic);
    } catch (const PubSubError& e) {
        threw = true;
        thrown_status = e.status();
    }

    CONF_MUST_DECLARE(topic, DataSchema());
    // `AwaitDataMatched` only for Absent, deliberately: on Fast DDS (always Carried) a rejected
    // checked subscription never opens its data reader (provider.hpp), so waiting for it to match a
    // writer would wait out the whole clause budget for a match that structurally cannot happen —
    // the row is asserted absent regardless of match status there. The schema-less (Absent) branch
    // calls it, matching every other delivery clause's own convention, since its row IS expected to
    // arrive.
    if (!Carried()) {
        Subject().AwaitDataMatched(topic, RemainingBudget());
    }
    CONF_MUST_PUBLISH(topic, 1);

    if (Carried()) {
        if (threw) {
            EXPECT_EQ(thrown_status, PubSubStatus::kSchemaConflict)
                << "a rejecting checked subscribe threw "
                << internal::PubSubStatusName(thrown_status) << ", not kSchemaConflict";
        } else {
            SharedSchema schema;
            const PubSubStatus wait_status = result.schema.Wait(RemainingBudget(), &schema);
            EXPECT_EQ(wait_status, PubSubStatus::kSchemaConflict)
                << "a rejecting checked subscription's arrival answered "
                << internal::PubSubStatusName(wait_status) << ", not kSchemaConflict";
        }
        EXPECT_EQ(check_calls.load(), 1)
            << "the rejecting check ran " << check_calls.load() << " times, not exactly once";

        // Fence: a second, plain subscription on a fresh topic, declared, matched and published,
        // proves the transport is still delivering before the negative below is read.
        const Topic fence_topic = Fresh("checked_reject_fence");
        Collector fence_collector;
        ScopedSubscription fence_sub(Subject(), fence_topic, fence_collector.Callback());
        CONF_MUST_DECLARE(fence_topic, DataSchema());
        Subject().AwaitDataMatched(fence_topic, RemainingBudget());
        CONF_MUST_PUBLISH(fence_topic, 1);
        ASSERT_TRUE(fence_collector.WaitForSeq(1, Deadline()))
            << "the fence row never arrived; the harness itself is stalled";

        EXPECT_EQ(collector.Count(), 0u)
            << "a row was delivered to a checked subscription its own check rejected";
    } else {
        EXPECT_FALSE(threw) << "a schema-less transport refused the checked subscribe outright: "
                            << internal::PubSubStatusName(thrown_status);
        EXPECT_EQ(check_calls.load(), 0) << "a schema-less transport ran the check anyway";
        EXPECT_TRUE(collector.WaitForSeq(1, Deadline()))
            << "the row never reached a schema-less transport's unchecked delivery";
    }
}

// An accepting check is indistinguishable from a plain Subscribe from there: every row is
// delivered, and the check ran exactly once regardless of how many rows follow.
TEST_P(ProviderConformance, ACheckedSubscriptionThatAcceptsDeliversLikeSubscribe) {
    const Topic topic = Fresh("checked_accept");
    std::atomic<int> check_calls{0};
    std::atomic<bool> saw_non_null{false};
    Collector collector;

    SubscriptionResult result = Subject().SubscribeWith(
        topic, {.callback = collector.Callback(), .check = [&](const SharedSchema& announced) {
                    check_calls.fetch_add(1);
                    if (announced != nullptr) saw_non_null.store(true);
                    return true;
                }});
    (void)result;
    CheckedSubscriptionGuard guard(Subject(), topic);

    CONF_MUST_DECLARE(topic, DataSchema());
    Subject().AwaitDataMatched(topic, RemainingBudget());
    CONF_MUST_PUBLISH(topic, 1);
    CONF_MUST_PUBLISH(topic, 2);

    ASSERT_TRUE(collector.WaitForCount(2, Deadline()))
        << "only " << collector.Count()
        << " of 2 rows arrived on an accepting checked subscription";

    if (Carried()) {
        EXPECT_EQ(check_calls.load(), 1)
            << "the accepting check ran " << check_calls.load() << " times, not exactly once";
        EXPECT_TRUE(saw_non_null.load())
            << "the check never saw a non-null schema on a carrying transport";
    } else {
        EXPECT_EQ(check_calls.load(), 0) << "a schema-less transport ran the check anyway";
    }
}

// The check itself is framed exactly like a delivery (provider.hpp: "the provider frames the call
// with internal::DeliveryScope"), so a seam call from inside it is refused kReentrantCall — the
// same door an ordinary callback is refused at.
TEST_P(ProviderConformance, ACheckCannotEnterTheProvider) {
    const Topic topic = Fresh("checked_reentrant");
    std::atomic<int32_t> recorded{kNothingRecorded};
    Collector collector;

    SubscriptionResult result = Subject().SubscribeWith(
        topic, {.callback = collector.Callback(), .check = [&](const SharedSchema&) {
                    try {
                        Subject().Unsubscribe(Fresh("never_subscribed"));
                        recorded.store(kReturnedWithoutThrowing);
                    } catch (const PubSubError& e) {
                        recorded.store(static_cast<int32_t>(e.status()));
                    } catch (...) {
                        recorded.store(kNonSeamException);
                    }
                    return true;
                }});
    (void)result;
    CheckedSubscriptionGuard guard(Subject(), topic);

    CONF_MUST_DECLARE(topic, DataSchema());
    Subject().AwaitDataMatched(topic, RemainingBudget());
    CONF_MUST_PUBLISH(topic, 1);
    ASSERT_TRUE(collector.WaitForSeq(1, Deadline())) << "the row never arrived";

    if (Carried()) {
        EXPECT_EQ(recorded.load(), static_cast<int32_t>(PubSubStatus::kReentrantCall))
            << "a checked subscription's check called back into the provider and got "
            << StatusText(recorded.load());
    } else {
        EXPECT_EQ(recorded.load(), kNothingRecorded)
            << "a schema-less transport ran the check anyway, recording "
            << StatusText(recorded.load());
    }
}

// Unsubscribe is the one documented way out of a rejected checked subscription (provider.hpp: "the
// subscription stays registered until Unsubscribe"), and once called the topic is fully reusable: a
// plain Subscribe afterwards declares and delivers exactly as if nothing had been registered there
// before. On the in-process provider a second Subscribe on a topic that already carries a
// registration replaces it regardless of Unsubscribe (the in-process provider's `Subscribe`, in its
// `if (slot.schema)` branch, installs a fresh channel unconditionally), so that subject cannot show
// Unsubscribe made the difference — only that the topic stays usable either way. Fast DDS's
// rejected checked subscription never opens a data reader at all (provider.hpp), so there the
// resubscribe really is a fresh registration and Unsubscribe's role is the one this clause pins. On
// XRCE a rejected checked subscription keeps its data reader until Unsubscribe deletes the reader
// and its subscriber, and the resubscribe recreates both.
TEST_P(ProviderConformance, ARejectedCheckedSubscriptionLeavesTheTopicReusable) {
    const Topic topic = Fresh("checked_reject_reuse");
    Collector collector;
    std::optional<CheckedSubscriptionGuard> guard;

    bool threw = false;
    PubSubStatus thrown_status = PubSubStatus::kOk;
    SubscriptionResult result;
    try {
        result = Subject().SubscribeWith(
            topic,
            {.callback = collector.Callback(), .check = [](const SharedSchema&) { return false; }});
        guard.emplace(Subject(), topic);
    } catch (const PubSubError& e) {
        threw = true;
        thrown_status = e.status();
    }

    CONF_MUST_DECLARE(topic, DataSchema());
    // Only for Absent, for the same reason ACheckedSubscriptionThatRejectsNeverDelivers's own
    // publish is: a rejected checked subscription never opens a data reader on Fast DDS, so waiting
    // for a match here would wait out the whole clause budget on the Carried subjects.
    if (!Carried()) {
        Subject().AwaitDataMatched(topic, RemainingBudget());
    }
    CONF_MUST_PUBLISH(topic, 1);

    if (Carried()) {
        if (threw) {
            EXPECT_EQ(thrown_status, PubSubStatus::kSchemaConflict)
                << "a rejecting checked subscribe threw "
                << internal::PubSubStatusName(thrown_status) << ", not kSchemaConflict";
        } else {
            SharedSchema schema;
            const PubSubStatus wait_status = result.schema.Wait(RemainingBudget(), &schema);
            EXPECT_EQ(wait_status, PubSubStatus::kSchemaConflict)
                << "a rejecting checked subscription's arrival answered "
                << internal::PubSubStatusName(wait_status) << ", not kSchemaConflict";
        }
    } else {
        EXPECT_FALSE(threw) << "a schema-less transport refused the checked subscribe outright: "
                            << internal::PubSubStatusName(thrown_status);
        EXPECT_TRUE(collector.WaitForSeq(1, Deadline()))
            << "the row never reached a schema-less transport's unchecked delivery";
    }

    if (!threw) {
        guard->Run();  // the freeing act this clause pins
    }

    Collector after;
    ScopedSubscription resub(Subject(), topic, after.Callback());
    Subject().AwaitDataMatched(topic, RemainingBudget());
    CONF_MUST_PUBLISH(topic, 2);
    EXPECT_TRUE(after.WaitForSeq(2, Deadline()))
        << "the topic did not accept a plain Subscribe after the rejected checked subscription was "
           "unsubscribed";
}

}  // namespace conformance
}  // namespace fletcher
