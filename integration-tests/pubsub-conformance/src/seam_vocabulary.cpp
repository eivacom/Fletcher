// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The `SeamVocabulary` suite: the crossing vocabulary's own assertions —
// ownership (§3.2), schema arrival (§3.4) and the status taxonomy (§5.1).
//
// It is NOT a third copy of the copy-accounting oracle. The one leg that needs
// address provenance borrows `CopyAccounting`'s instrument outright, so there is
// exactly one scoring path in this harness and a broken instrument cannot green
// a vocabulary claim. Everything else here is about what the TYPES make
// representable, which no provider-parameterised clause can reach.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <fletcher/core/status.hpp>
#include <fletcher/core/write_buffer.hpp>
#include <fletcher/pubsub/in_process_provider.hpp>
#include <fletcher/pubsub/schema_arrival.hpp>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "fletcher/conformance/copy_accounting.hpp"
#include "fletcher/conformance/fixtures.hpp"

namespace fletcher {
namespace conformance {
namespace {

std::string Hex(Address address) {
    std::ostringstream out;
    out << "0x" << std::hex << address;
    return out.str();
}

const AttachmentTrace& TraceNamed(const CopyLedger& ledger, const std::string& key) {
    for (const AttachmentTrace& trace : ledger.attachments) {
        if (trace.key == key) return trace;
    }
    throw std::runtime_error("SeamVocabulary: no attachment trace named " + key);
}

}  // namespace

// ── §3.2 — the seam carries memory Fletcher did not allocate ────────
//
// THE FORCING TEST. A provider holds payload bytes in memory it owns — the
// stand-in for a transport's loaned sample — and hands them to the subscriber
// WHERE THEY LIE. Two independent claims, both required:
//
//   1. Provenance: the delivered blob's `data()` IS the provider's own address,
//      not a second address holding equal bytes. `memcmp` cannot tell those
//      apart; only the address can.
//   2. Ownership is real: a callee that keeps the blob past the borrow window
//      still reads those same bytes at that same address afterwards. A span
//      with no owner would satisfy (1) and fail (2).
//
// The negative control runs the IDENTICAL leg against a provider that copies:
// it must still score copies, or the instrument is inert and this green is
// worthless.
TEST(SeamVocabulary, BorrowedTransportMemoryCrossesWithoutCopy) {
    RoundTrip trip = RunBorrowedAttachmentRoundTrip(FreshTopic("SeamVocabularyBorrowed"));
    ASSERT_TRUE(trip.error.empty()) << "the round trip failed: " << trip.error;
    ASSERT_EQ(trip.ledger.deliveries, static_cast<size_t>(1))
        << "no delivery happened, so nothing below is evidence of anything";
    ASSERT_EQ(trip.ledger.delivered_attachments, static_cast<size_t>(2))
        << "the delivery did not carry both attachments";

    const AttachmentTrace& loaned = TraceNamed(trip.ledger, "loaned");
    ASSERT_TRUE(loaned.content_ok) << "the borrowed bytes arrived garbled, so provenance is moot";
    EXPECT_EQ(loaned.delivered_data, loaned.published_data)
        << "the seam copied borrowed transport memory: the provider held it at "
        << Hex(loaned.published_data) << " and the subscriber saw it at "
        << Hex(loaned.delivered_data) << ". §3.2's owner-plus-span is what removes this copy";

    const AttachmentTrace& owned = TraceNamed(trip.ledger, "owned");
    EXPECT_EQ(owned.delivered_data, owned.published_data)
        << "a CALLER-owned blob was copied as well; that path was already zero-copy";

    // Ownership, not just aliasing: read the kept blob after the callback and
    // after Unsubscribe.
    EXPECT_EQ(trip.ledger.retained_data, loaned.published_data)
        << "a blob kept past the delivery no longer names the provider's bytes";
    EXPECT_TRUE(trip.ledger.retained_content_ok)
        << "a blob kept past the delivery no longer reads back the published bytes — its owner "
           "does not keep [data, data+size) alive, which §3.2 requires of every Blob";

    const CopyVerdict verdict = Judge(trip.ledger);
    EXPECT_EQ(verdict.attachment_copies, static_cast<size_t>(0))
        << "the seam still costs a copy to carry memory it does not own";
    EXPECT_EQ(verdict.row_copies, static_cast<size_t>(0)) << "the row leg is unaffected by §3.2";

    // The instrument is live: the same leg, against a provider that copies
    // everything, must NOT score zero.
    RoundTrip copying = RunBorrowedAttachmentRoundTrip(FreshTopic("SeamVocabularyBorrowed"), true);
    ASSERT_TRUE(copying.error.empty()) << "the control round trip failed: " << copying.error;
    EXPECT_EQ(Judge(copying.ledger).attachment_copies, static_cast<size_t>(2))
        << "a provider that deep-copies every blob scored "
        << Judge(copying.ledger).attachment_copies
        << " — the instrument is inert and the zero above means nothing";
}

// ── §3.4 — the outcome no happy path reaches ────────────────────────
//
// A subscription torn down before its schema arrives is a LEGITIMATE path, so
// it must be reported, not refused — and it must be distinguishable from "this
// transport carries no schemas at all", which demands the opposite handling at
// a subscriber (§7 clause 1). The old shared_future could not express it: a
// broken promise surfaced as a THROWN get(), and a bool-returning wait could
// not tell the two apart at all.
TEST(SeamVocabulary, AbandonedSubscriptionReportsNoSchemaWillArrive) {
    SharedSchema out;

    {
        auto [arrival, resolver] = SchemaArrival::Create();
        // Nothing has happened yet: pending, and the out param is untouched.
        EXPECT_EQ(arrival.Wait(std::chrono::milliseconds(0), &out), PubSubStatus::kPending);
        EXPECT_EQ(out, nullptr);
    }
    // The resolver died unresolved. That IS the third terminal outcome.
    SchemaArrival abandoned;
    {
        auto [arrival, resolver] = SchemaArrival::Create();
        abandoned = arrival;
    }
    EXPECT_EQ(abandoned.Wait(std::chrono::milliseconds(0), &out), PubSubStatus::kSubscriptionEnded)
        << "an abandoned subscription must say so, not hang and not report success";
    EXPECT_EQ(out, nullptr) << "*out is untouched for every outcome except kOk";

    // Even the unbounded wait returns, which is what makes it safe to offer.
    EXPECT_EQ(abandoned.Wait(std::chrono::milliseconds::max(), &out),
              PubSubStatus::kSubscriptionEnded);

    // ...and it is NOT the same answer as a schema-less transport's.
    SchemaArrival schemaless = SchemaArrival::Ready(nullptr);
    EXPECT_EQ(schemaless.Wait(std::chrono::milliseconds(0), &out), PubSubStatus::kOk)
        << "kOk + null is how a transport says it carries no schemas; conflating it with "
           "kSubscriptionEnded is the silent-wrong-slot-decoding failure §7 names";
    EXPECT_EQ(out, nullptr);
}

// ── §3.2 rung 1 — an unowned blob is unrepresentable ────────────────
//
// Not "discouraged": there is no view-only constructor, so §3.2 clause 1's "a
// callee that keeps it takes its own reference" is exactly true and a C boundary
// can implement "keep it" as retain(owner) with nothing left to check.
TEST(SeamVocabulary, BlobRefusesBytesNothingOwns) {
    const auto arena = std::make_shared<const std::vector<uint8_t>>(8, 0xAB);

    EXPECT_THROW(Blob(nullptr, arena->data(), arena->size()), PubSubError)
        << "a Blob was built over bytes with no owner keeping them alive";
    EXPECT_THROW(Blob(arena, nullptr, 4), PubSubError)
        << "a Blob claimed four bytes at a null address";

    try {
        Blob unowned(nullptr, arena->data(), arena->size());
        FAIL() << "expected a refusal";
    } catch (const PubSubError& e) {
        EXPECT_EQ(e.status(), PubSubStatus::kInvalidArgument)
            << "refused, but with a status a binding cannot map back to the cause";
    }

    // Empty is null data and zero size, and is perfectly legal (§3.2 clause 5).
    const Blob empty;
    EXPECT_EQ(empty.data(), nullptr);
    EXPECT_EQ(empty.size(), 0u);
    EXPECT_TRUE(empty.empty());
}

// ── §5.1 — one error type, one stable number ────────────────────────
//
// The numbers themselves are pinned by static_assert in status.hpp, where a
// reorder fails the build. What this covers is the behaviour a static_assert
// cannot: that a FAILURE can never be constructed carrying a success code, or a
// wait outcome.
TEST(SeamVocabulary, ErrorRefusesEveryNonFailureStatus) {
    EXPECT_EQ(PubSubError(PubSubStatus::kOk, "boom").status(), PubSubStatus::kInternal)
        << "a failure that carries kOk lets a C boundary report it as success — silently";
    EXPECT_EQ(PubSubError(PubSubStatus::kPending, "boom").status(), PubSubStatus::kInternal);
    EXPECT_EQ(PubSubError(PubSubStatus::kSubscriptionEnded, "boom").status(),
              PubSubStatus::kInternal)
        << "kPending and kSubscriptionEnded are wait OUTCOMES, never thrown";

    // A real cause survives untouched, message and all.
    const PubSubError conflict(PubSubStatus::kSchemaConflict, "topic T");
    EXPECT_EQ(conflict.status(), PubSubStatus::kSchemaConflict);
    EXPECT_STREQ(conflict.what(), "topic T");
    // It still catches as what every existing site catches.
    EXPECT_NO_THROW({
        try {
            throw conflict;
        } catch (const std::runtime_error&) {
        }
    });
}

// ── §3.4 rung 1/2 — the two ways to blur the outcome, both refused ──
TEST(SeamVocabulary, ResolverRefusesNullAndWaitRefusesNegativeTimeout) {
    SharedSchema out;

    auto [arrival, resolver] = SchemaArrival::Create();
    EXPECT_THROW(std::move(resolver).Resolve(nullptr), PubSubError)
        << "a carrying provider resolved with null, which is kOk+null — the meaning reserved "
           "for a transport that carries no schemas at all";

    // A negative timeout silently polled before it was refused, so "negative
    // means forever" could have been invented by one boundary and not the other.
    SchemaArrival ready = SchemaArrival::Ready(nullptr);
    EXPECT_THROW(static_cast<void>(ready.Wait(std::chrono::milliseconds(-1), &out)), PubSubError);
    try {
        static_cast<void>(ready.Wait(std::chrono::milliseconds(-1), &out));
    } catch (const PubSubError& e) {
        EXPECT_EQ(e.status(), PubSubStatus::kInvalidArgument);
    }
}

// ── §7 clause 1, per subscription (review debt C2-1) ────────────────
//
// The loopback in its default mode carries whatever a publisher declared ON THIS
// INSTANCE. It used to hand that schema to whatever subscription happened to be
// live, so a subscription that had already been told "no schema" silently began
// receiving one mid-stream — the null-to-non-null flip §7 clause 1 forbids, whose
// failure mode is a client decoding one stream two ways with no signal.
//
// **No conformance subject reaches this path.** `InProcessLocal` carries the
// harness's subject axis kAbsent, so CONF_MUST_DECLARE never hands the loopback a
// real schema; the only caller that does is the gateway, which has no subject.
// That is why the rule is asserted here, directly.
TEST(SeamVocabulary, LaterDeclarationNeverReachesALiveSubscription) {
    auto provider = std::make_shared<InProcessPubSubProvider>();
    const Topic topic = FreshTopic("SeamVocabularyLatch");

    std::vector<bool> had_schema;
    SubscriptionResult first = provider->Subscribe(
        topic, [&](const uint8_t*, size_t, const SharedSchema& schema, const Attachments&) {
            had_schema.push_back(schema != nullptr);
        });

    // Fixed when Subscribe returned, and it was fixed at "no schema".
    SharedSchema out;
    ASSERT_EQ(first.schema.Wait(std::chrono::milliseconds(0), &out), PubSubStatus::kOk);
    ASSERT_EQ(out, nullptr) << "nothing had been declared, so the answer is kOk + null";

    provider->Publish(topic, [](WriteBuffer& buf) { buf.AppendByte(0x01); });

    // A declaration lands AFTER the subscription exists.
    provider->CreateTopic(topic, MakeConformanceSchema(SchemaId::kA));
    provider->Publish(topic, [](WriteBuffer& buf) { buf.AppendByte(0x02); });

    ASSERT_EQ(had_schema.size(), 2u) << "both rows should have been delivered";
    EXPECT_FALSE(had_schema[0]);
    EXPECT_FALSE(had_schema[1])
        << "a declaration made after this subscription existed reached it anyway: the "
           "subscription flipped from no-schema to schema mid-stream, which §7 clause 1 forbids "
           "per subscription";

    // The arrival it was handed still says what its deliveries say. One answer,
    // for the life of the subscription.
    out = nullptr;
    EXPECT_EQ(first.schema.Wait(std::chrono::milliseconds(0), &out), PubSubStatus::kOk);
    EXPECT_EQ(out, nullptr);

    // A NEW subscription does see the declaration — the rule is "later
    // declarations reach only new subscriptions", not "declarations are lost".
    provider->Unsubscribe(topic);
    SubscriptionResult second = provider->Subscribe(
        topic, [](const uint8_t*, size_t, const SharedSchema&, const Attachments&) {});
    out = nullptr;
    EXPECT_EQ(second.schema.Wait(std::chrono::milliseconds(0), &out), PubSubStatus::kOk);
    EXPECT_NE(out, nullptr) << "a subscription created after the declaration must see it";
}

}  // namespace conformance
}  // namespace fletcher
