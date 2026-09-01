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
// Clause 2 (CallbackNeverSeesNullSchema) lives in clauses_carried.cpp, which is
// linked only into the schema-CARRYING subjects' binaries: the axis gate is
// applied at link/instantiation, so on a schema-less subject the clause is
// absent from the ctest list rather than present and skipped. There is no
// GTEST_SKIP anywhere in this suite.

#include <algorithm>
#include <chrono>
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
    SubscriptionResult sub = Subject().Subscribe(topic, collector.Callback());
    (void)sub;

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
    Subject().Unsubscribe(topic);
}

// ── Clause 3 (§7 clause 1, last sentence) ───────────────────────────
// Schema-carrying and schema-less are the two modes, and a transport is in
// exactly one of them for every delivery: "must never mix the two".
TEST_P(ProviderConformance, SchemaModeIsUniformNeverMixed) {
    const Topic topic = Fresh("uniform_mode");

    CONF_MUST_DECLARE(topic, DataSchema());
    Collector collector;
    SubscriptionResult sub = Subject().Subscribe(topic, collector.Callback());
    (void)sub;

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
    Subject().Unsubscribe(topic);
}

// ── Clause 4 (§7 clause 2) ──────────────────────────────────────────
// Samples from a single writer reach the callback in publish order.
TEST_P(ProviderConformance, PerWriterOrderIsMonotonic) {
    const Topic topic = Fresh("writer_order");
    constexpr uint32_t kRows = 20;

    CONF_MUST_DECLARE(topic, DataSchema());
    Collector collector;
    SubscriptionResult sub = Subject().Subscribe(topic, collector.Callback());
    (void)sub;

    for (uint32_t seq = 1; seq <= kRows; ++seq) {
        CONF_MUST_PUBLISH(topic, seq);
    }
    ASSERT_TRUE(collector.WaitForCount(kRows, Deadline()))
        << "only " << collector.Count() << " of " << kRows << " rows arrived";
    EXPECT_EQ(collector.Seqs(), Range(1, kRows));
    Subject().Unsubscribe(topic);
}

// ── Clause 5 (§7 clause 2, the handoff half) ────────────────────────
// The buffered pre-schema backlog is delivered before, and never interleaved
// with, samples arriving live afterwards.
TEST_P(ProviderConformance, BacklogNeverInterleavesWithLiveSamples) {
    const Topic topic = Fresh("no_interleave");

    Collector collector;
    SubscriptionResult sub = Subject().Subscribe(topic, collector.Callback());
    (void)sub;

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
    Subject().Unsubscribe(topic);
}

// ── Clause 6 (locked decision 12 + §7 clause 1 "buffered and delivered") ──
// All or nothing. A transport either replays every row retained before the
// subscriber existed, or none of them. Partial delivery fails under BOTH trait
// values, which is the point: the shipped receive-side data-sharing defect
// delivered "often just the newest sample".
TEST_P(ProviderConformance, LateJoinerBacklogIsAllOrNothing) {
    const Topic topic = Fresh("late_joiner");
    constexpr uint32_t kBacklog = 5;
    constexpr uint32_t kSentinel = 1000;

    CONF_MUST_DECLARE(topic, DataSchema());
    for (uint32_t seq = 1; seq <= kBacklog; ++seq) {
        CONF_MUST_PUBLISH(topic, seq);
    }

    // The late joiner.
    Collector collector;
    SubscriptionResult sub = Subject().Subscribe(topic, collector.Callback());
    (void)sub;

    // One live row after subscribing. Per-writer order (clause 4) puts the whole
    // retained backlog ahead of it, so its arrival is a bounded, deterministic
    // end to the wait — for the retaining and the dropping case alike.
    CONF_MUST_PUBLISH(topic, kSentinel);
    ASSERT_TRUE(collector.WaitForSeq(kSentinel, Deadline()))
        << "the live row after Subscribe never arrived; nothing can be concluded";

    const std::vector<uint32_t> seqs = collector.Seqs();
    const auto replayed = static_cast<uint32_t>(
        std::count_if(seqs.begin(), seqs.end(), [](uint32_t s) { return s <= kBacklog; }));
    if (Retains()) {
        EXPECT_EQ(replayed, kBacklog) << "a retaining transport replayed " << replayed << " of "
                                      << kBacklog << " retained rows — partial is never acceptable";
    } else {
        EXPECT_EQ(replayed, 0u) << "a dropping transport replayed " << replayed
                                << " rows published before the subscriber existed";
    }
    Subject().Unsubscribe(topic);
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
    SubscriptionResult sub = Subject().Subscribe(topic, collector.Callback());
    (void)sub;
    CONF_MUST_PUBLISH(topic, 1);
    ASSERT_TRUE(collector.WaitForCount(1, Deadline()))
        << "the topic stopped delivering after an identical re-declaration";
    EXPECT_EQ(collector.Seqs(), Range(1, 1));
    Subject().Unsubscribe(topic);
}

// ── Clause 8 (§7 clause 3, second half — AMENDED in this PR) ────────
// A conflicting re-declaration MUST be rejected (owner ruling 2026-09-01; the
// spec's "may" became "must" in the same change). Asserts only THAT the call
// failed: the seam has no exception taxonomy yet, so asserting which failure
// would be inventing one here.
TEST_P(ProviderConformance, ConflictingRedeclarationIsRejected) {
    const Topic topic = Fresh("redeclare_conflict");

    CONF_MUST_DECLARE(topic, SchemaId::kA);
    const std::optional<std::string> err = Subject().DeclareTopic(topic, SchemaId::kB);
    EXPECT_TRUE(err.has_value())
        << "re-declaring struct<seq:int32> as struct<seq:int32,extra:float64> was accepted";
}

// ── Clause 9 (§7 clause 4) ──────────────────────────────────────────
// One callback per topic per instance. Cardinality only: exactly one delivery
// across two registrations, without asserting which registration wins — §7
// states the cardinality, not a winner.
TEST_P(ProviderConformance, OneCallbackPerTopicPerInstance) {
    const Topic topic = Fresh("one_callback");

    CONF_MUST_DECLARE(topic, DataSchema());
    Collector collector;  // shared, so the count is across BOTH registrations
    SubscriptionResult first = Subject().Subscribe(topic, collector.Callback());
    (void)first;
    try {
        SubscriptionResult second = Subject().Subscribe(topic, collector.Callback());
        (void)second;
    } catch (const std::exception&) {
        // Refusing the second registration is one legal way to hold the
        // cardinality; replacing the first is the other.
    }

    CONF_MUST_PUBLISH(topic, 1);
    ASSERT_TRUE(collector.WaitForCount(1, Deadline())) << "the row reached no callback at all";
    EXPECT_FALSE(collector.WaitForCount(2, SettleDeadline()))
        << "one published row was delivered twice on one instance";
    EXPECT_EQ(collector.Count(), 1u);
    Subject().Unsubscribe(topic);
}

// ── Clause 10 (§7 clause 5) ─────────────────────────────────────────
// Subscribe never blocks; a late joiner gets the schema asynchronously.
TEST_P(ProviderConformance, SubscribeNeverBlocksSchemaArrivesLater) {
    const Topic topic = Fresh("late_schema");

    Collector collector;
    const auto before = std::chrono::steady_clock::now();
    SubscriptionResult sub = Subject().Subscribe(topic, collector.Callback());
    const auto elapsed = std::chrono::steady_clock::now() - before;
    EXPECT_LT(elapsed, kSubscribeCeiling)
        << "Subscribe on a topic no publisher has declared took "
        << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() << " ms";

    CONF_MUST_DECLARE(topic, DataSchema());
    CONF_MUST_PUBLISH(topic, 1);
    ASSERT_TRUE(collector.WaitForSeq(1, Deadline())) << "the row never arrived";

    if (Carried()) {
        ASSERT_EQ(sub.schema.wait_until(Deadline()), std::future_status::ready)
            << "the schema future never resolved";
        EXPECT_NE(sub.schema.get(), nullptr);
    } else {
        // §7 clause 1's sanctioned schema-less transport: null throughout, and
        // resolved rather than left pending, so a waiter never hangs.
        ASSERT_EQ(sub.schema.wait_until(Deadline()), std::future_status::ready);
        EXPECT_EQ(sub.schema.get(), nullptr);
    }
    Subject().Unsubscribe(topic);
}

// ── Clause 11 (§7 clause 6) ─────────────────────────────────────────
// After Unsubscribe returns, no further callback for that topic.
TEST_P(ProviderConformance, NoDeliveryAfterUnsubscribeReturns) {
    const Topic topic = Fresh("after_unsubscribe");

    CONF_MUST_DECLARE(topic, DataSchema());
    Collector collector;
    SubscriptionResult sub = Subject().Subscribe(topic, collector.Callback());
    (void)sub;

    for (uint32_t seq = 1; seq <= 3; ++seq) {
        CONF_MUST_PUBLISH(topic, seq);
    }
    ASSERT_TRUE(collector.WaitForCount(3, Deadline())) << "delivery never started";

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
    // what is under test.
    collector.SetHoldWindow(std::chrono::microseconds(500));
    SubscriptionResult sub = Subject().Subscribe(topic, collector.Callback());
    (void)sub;

    std::optional<std::string> err_a;
    std::optional<std::string> err_b;
    std::thread a([&] {
        for (uint32_t seq = 1; seq <= kRows / 2; ++seq) {
            if (auto e = Subject().PublishRow(topic, seq); e.has_value() && !err_a.has_value()) {
                err_a = e;
            }
        }
    });
    std::thread b([&] {
        for (uint32_t seq = kRows / 2 + 1; seq <= kRows; ++seq) {
            if (auto e = Subject().PublishRow(topic, seq); e.has_value() && !err_b.has_value()) {
                err_b = e;
            }
        }
    });
    a.join();
    b.join();
    ASSERT_FALSE(err_a.has_value()) << "publish failed: " << *err_a;
    ASSERT_FALSE(err_b.has_value()) << "publish failed: " << *err_b;

    ASSERT_TRUE(collector.WaitForCount(kRows, Deadline()))
        << "only " << collector.Count() << " of " << kRows << " rows arrived";
    EXPECT_EQ(collector.MaxInFlight(), 1u)
        << "two deliveries were in flight at once on one subscription";
    Subject().Unsubscribe(topic);
}

}  // namespace conformance
}  // namespace fletcher
