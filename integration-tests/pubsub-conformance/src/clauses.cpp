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
// Clause 2 (CallbackNeverSeesNullSchema) lives in clauses_carried.cpp, which is
// linked only into the schema-CARRYING subjects' binaries: the axis gate is
// applied at link/instantiation, so on a schema-less subject the clause is
// absent from the ctest list rather than present and skipped. There is no
// GTEST_SKIP anywhere in this suite.

#include <algorithm>
#include <chrono>
#include <fletcher/core/internal/status_name.hpp>
#include <optional>
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

}  // namespace conformance
}  // namespace fletcher
