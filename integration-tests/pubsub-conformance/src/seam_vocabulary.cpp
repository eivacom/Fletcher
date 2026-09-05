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
#include <cstring>
#include <fletcher/core/status.hpp>
#include <fletcher/core/write_buffer.hpp>
#include <fletcher/pubsub/in_process_provider.hpp>
#include <fletcher/pubsub/schema_arrival.hpp>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
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

    // Ownership, not just aliasing. The two claims below are read after the
    // callback, after Unsubscribe, and after the PROVIDER ITSELF HAS BEEN
    // DESTROYED — so the Blob's own owner is the only thing that can still be
    // keeping the arena alive. Read with the provider still up they would pass
    // for a span with no owner at all, which is the case they claim to
    // distinguish; the arena also scribbles 0xDD over its slots on destruction,
    // so "the bytes happened to survive" is not a way to pass either.
    ASSERT_TRUE(trip.ledger.subject_released)
        << "the probe outlived the round trip, so the two ownership assertions below would hold "
           "whether or not the Blob owns anything — the guard is vacuous, not green";
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
    // Pre-loaded with a NON-null schema, deliberately. "*out is untouched" is a
    // claim about writing, and against a null `out` it passes whether Wait leaves
    // it alone or writes null into it — which is no test at all.
    const SharedSchema sentinel = MakeSharedSchema(MakeConformanceSchema(SchemaId::kA));
    ASSERT_NE(sentinel, nullptr);
    SharedSchema out = sentinel;

    {
        auto [arrival, resolver] = SchemaArrival::Create();
        // Nothing has happened yet: pending, and the out param is untouched.
        EXPECT_EQ(arrival.Wait(std::chrono::milliseconds(0), &out), PubSubStatus::kPending);
        EXPECT_EQ(out, sentinel) << "*out was written on a non-kOk outcome";
    }
    // The resolver died unresolved. That IS the third terminal outcome.
    SchemaArrival abandoned;
    {
        auto [arrival, resolver] = SchemaArrival::Create();
        abandoned = arrival;
    }
    EXPECT_EQ(abandoned.Wait(std::chrono::milliseconds(0), &out), PubSubStatus::kSubscriptionEnded)
        << "an abandoned subscription must say so, not hang and not report success";
    EXPECT_EQ(out, sentinel) << "*out is untouched for every outcome except kOk";

    // Even an unbounded wait returns on an ended subscription, which is what makes
    // an unbounded wait safe to offer at all. Note what this does and does NOT
    // say: a settled arrival short-circuits before the timeout is looked at, so
    // this asserts "a settled arrival answers immediately whatever you ask for",
    // not anything about the waiting machinery.
    //
    // There is deliberately NO assertion here about the huge-finite-timeout clamp
    // in Wait(). One was written and is removed: it called Wait on this same
    // settled arrival, so it returned in ~0 us without ever consulting the
    // timeout, and it passed with the clamp mutated away. The corrected version —
    // a genuinely pending arrival settled from another thread — cannot falsify the
    // clamp either, because MSVC 14.44's wait_for clamps the deadline internally,
    // so it blocks for the same time with the clamp present or absent. The clamp
    // guards standard libraries that overflow instead; see schema_arrival.cpp.
    // **Do not "restore coverage" here.** A third guard that passes for a reason
    // other than the one it states is worse than no guard: it spends the suite's
    // credibility, which is the only thing it has.
    EXPECT_EQ(abandoned.Wait(std::chrono::milliseconds::max(), &out),
              PubSubStatus::kSubscriptionEnded);

    // ...and it is NOT the same answer as a schema-less transport's.
    SchemaArrival schemaless = SchemaArrival::Ready(nullptr);
    EXPECT_EQ(schemaless.Wait(std::chrono::milliseconds(0), &out), PubSubStatus::kOk)
        << "kOk + null is how a transport says it carries no schemas; conflating it with "
           "kSubscriptionEnded is the silent-wrong-slot-decoding failure §7 names";
    EXPECT_EQ(out, nullptr) << "kOk WRITES *out, including the null of a schema-less transport — "
                               "a caller must not be left reading a stale schema";
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

// ── §3.1 clause 6 — the window has a WRITE end, through the base ────
//
// The representability claim, and the one no `CopyAccounting` entry can make: a
// caller holding nothing but a `WriteBuffer&` — which is all a language binding
// is ever handed, since `Publish` inverts and gives it the reference and nothing
// else — can obtain writable space, be told how much, fill it, and commit what
// it wrote. Before this member the only way to advance the position was to
// supply the bytes from somewhere else, so the whole-row copy was structural.
//
// It runs through a plain `WriteBuffer&`, deliberately: a member reachable only
// on a concrete subclass would be useless to the callers this exists to serve.
TEST(SeamVocabulary, AWriteBufferReferenceCanBeFilledInPlace) {
    VectorWriteBuffer owned;
    WriteBuffer& buffer = owned;  // all a binding ever sees

    // A variable-length row: a length placeholder, a body of a size the producer
    // only discovers while writing, then a back-patch. This is the shape that
    // proves the capability is COMPLETE with one member — no `Capacity()`, no
    // non-const `Data()`, no Reserve/Commit pair.
    const size_t length_at = buffer.WriteLengthPlaceholder();
    const uint8_t* lent = nullptr;
    size_t lent_room = 0;

    buffer.AppendInPlace(64, [&](uint8_t* dst, size_t room) -> size_t {
        lent = dst;
        lent_room = room;
        size_t written = 0;
        while (written < 37) {
            dst[written] = static_cast<uint8_t>(written);
            ++written;
        }
        return written;
    });
    buffer.PatchU32(length_at, 37);

    EXPECT_NE(lent, nullptr) << "the writer was never invoked";
    EXPECT_GE(lent_room, static_cast<size_t>(64))
        << "the producer must be TOLD how much space it has — it has no other way to ask";
    ASSERT_EQ(buffer.Position(), sizeof(uint32_t) + 37);

    const std::vector<uint8_t> row = owned.Finish();
    ASSERT_EQ(row.size(), sizeof(uint32_t) + 37);
    uint32_t declared = 0;
    std::memcpy(&declared, row.data(), sizeof(declared));
    EXPECT_EQ(declared, 37u);
    for (size_t i = 0; i < 37; ++i) EXPECT_EQ(row[sizeof(uint32_t) + i], static_cast<uint8_t>(i));

    // The two refusals a binding must be able to map back to a cause. Both are
    // kInvalidArgument: they are "the caller broke this call's contract", which
    // is what that number already means — A1 appends no status.
    VectorWriteBuffer fresh;
    WriteBuffer& target = fresh;
    try {
        target.AppendInPlace(0, [](uint8_t*, size_t) -> size_t { return 0; });
        FAIL() << "a fill of no bytes names nothing and must be refused";
    } catch (const PubSubError& e) {
        EXPECT_EQ(e.status(), PubSubStatus::kInvalidArgument);
    }
    try {
        target.AppendInPlace(8, [](uint8_t*, size_t room) -> size_t { return room + 1; });
        FAIL() << "a writer reporting more than it was lent must be refused";
    } catch (const PubSubError& e) {
        EXPECT_EQ(e.status(), PubSubStatus::kInvalidArgument);
    }
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
    try {
        std::move(resolver).Resolve(nullptr);
        ADD_FAILURE() << "a carrying provider resolved with null, which is kOk+null — the meaning "
                         "reserved for a transport that carries no schemas at all";
    } catch (const PubSubError& e) {
        EXPECT_EQ(e.status(), PubSubStatus::kInvalidArgument);
    }
    // The refusal is TERMINAL, and the header says so: the token is consumed and
    // the arrival settles at kInternal, so a waiter learns of the provider bug
    // instead of blocking on an arrival that will never be resolved again.
    EXPECT_EQ(arrival.Wait(std::chrono::milliseconds(0), &out), PubSubStatus::kInternal);
    EXPECT_FALSE(arrival.Message().empty());

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

// ── §3.5 rung 2 — an empty topic names no topic ─────────────────────
//
// One check, no default topic, no recovery. It is a BEHAVIOUR change as well as
// a new rule: `JoinSegments({})` used to return `""`, a perfectly legal topic
// key, so an empty segment list silently published to and subscribed from a
// topic named "". It is reachable from outside the process — the gateway's
// `SplitTopic("")` yields an empty vector — which is why it is refused at the
// door rather than trusted not to happen.
//
// Asserted on every one of the four methods: the check lives in one place
// (`internal::RequireSegments`), and this is what says all four still route
// through it.
TEST(SeamVocabulary, EmptyTopicSegmentListIsRefusedAtEveryEntryPoint) {
    InProcessPubSubProvider provider;
    const Topic none;

    auto refused = [](auto&& call) {
        try {
            call();
        } catch (const PubSubError& e) {
            return e.status() == PubSubStatus::kInvalidArgument;
        } catch (...) {
            return false;
        }
        return false;
    };

    EXPECT_TRUE(refused([&] { provider.CreateTopic(none, MakeConformanceSchema(SchemaId::kA)); }))
        << "CreateTopic accepted an empty topic";
    EXPECT_TRUE(refused([&] {
        provider.Publish(none, [](WriteBuffer& buf) { buf.AppendByte(0x01); });
    })) << "Publish accepted an empty topic";
    EXPECT_TRUE(refused([&] {
        static_cast<void>(provider.Subscribe(
            none, [](const uint8_t*, size_t, const SharedSchema&, const Attachments&) {}));
    })) << "Subscribe accepted an empty topic";
    EXPECT_TRUE(refused([&] { provider.Unsubscribe(none); }))
        << "Unsubscribe accepted an empty topic";

    // And a one-segment topic is still perfectly ordinary — the refusal is of
    // EMPTY, not of short.
    EXPECT_NO_THROW(provider.CreateTopic({"solo"}, OwnedSchema{}));
}

// ── §3.5 rung 2 — the segment list IS the topic (PDA-DEC-A5) ─────────
//
// The seam identifies a topic by a segment LIST; every provider identifies it
// by the single joined byte string `internal::JoinSegments` produces. Nothing
// made that map injective or faithful, so four segment shapes broke it:
//
//   1. a segment containing a NUL — XRCE hands the joined name to
//      `uxr_buffer_create_topic_bin` as a `const char*`, which has no length
//      form, so the name reaching the wire was TRUNCATED at the first zero
//      byte and two different topics arrived as one;
//   2. a segment containing `/` — `{"a/b"}` and `{"a","b"}` joined to the same
//      name in all three providers, so one silently received the other's rows;
//   3. an empty segment — `{""}` reproduces the empty name that §3.5's
//      empty-LIST rule already refuses, one level down, and `{"a",""}` names
//      `"a/"`;
//   4. a segment beginning `__` — both DDS providers derive a companion topic
//      `name + "/__schema"`, so `{"a","__schema"}` landed on the schema channel
//      of `{"a"}`. The PREFIX is reserved rather than the one literal name, so
//      every future provider-derived companion is out of reach by construction
//      (owner ruling 2026-09-04).
//
// This is the SIBLING of the empty-list case above and is deliberately beside
// it: same provider, same four methods, same shape of assertion. Both rules
// live in `internal::RequireSegments`, and asserting all four methods is what
// says the door is still the one door every entry point routes through.
//
// The peer subjects are excluded by construction, not by omission:
// `PeerSubject::RejectUnsendableTopic` makes all of these unsendable over the
// harness pipe, so a parameterised clause would score the HARNESS's door. The
// cross-provider half of this claim lives as `TopicNames.AmbiguousSegmentsAreRefused`
// in the Fast DDS and XRCE subject binaries, which construct a real provider
// directly. See README.md.
TEST(SeamVocabulary, AmbiguousTopicSegmentsAreRefusedAtEveryEntryPoint) {
    InProcessPubSubProvider provider;

    auto refused = [](auto&& call) {
        try {
            call();
        } catch (const PubSubError& e) {
            return e.status() == PubSubStatus::kInvalidArgument;
        } catch (...) {
            return false;
        }
        return false;
    };

    // Spelled through `push_back` because `std::string("a\0b")` stops at the
    // zero byte and would silently become a DIFFERENT rule's row.
    std::string nul_bearing = "a";
    nul_bearing.push_back('\0');
    nul_bearing += "b";

    const std::vector<std::pair<Topic, std::string>> kRefused = {
        {Topic{nul_bearing}, "a segment carrying a NUL"},
        {Topic{"a/b"}, "a segment carrying the separator"},
        {Topic{"a", "b/c"}, "a later segment carrying the separator"},
        {Topic{""}, "an empty segment"},
        {Topic{"a", ""}, "a trailing empty segment"},
        {Topic{"a", "__schema"}, "a segment in the reserved `__` namespace"},
        {Topic{"__anything"}, "any segment in the reserved `__` namespace"},
    };

    for (const auto& entry : kRefused) {
        const Topic& topic = entry.first;
        const std::string& why = entry.second;

        EXPECT_TRUE(refused([&] {
            provider.CreateTopic(topic, MakeConformanceSchema(SchemaId::kA));
        })) << "CreateTopic accepted "
            << why;
        EXPECT_TRUE(refused([&] {
            provider.Publish(topic, [](WriteBuffer& buf) { buf.AppendByte(0x01); });
        })) << "Publish accepted "
            << why;
        EXPECT_TRUE(refused([&] {
            static_cast<void>(provider.Subscribe(
                topic, [](const uint8_t*, size_t, const SharedSchema&, const Attachments&) {}));
        })) << "Subscribe accepted "
            << why;
        EXPECT_TRUE(refused([&] { provider.Unsubscribe(topic); }))
            << "Unsubscribe accepted " << why;
    }

    // The bound on the narrowing. A dot, a space, a hyphen and a SINGLE leading
    // underscore are not wrong and must still work — the safe-charset option
    // was rejected for exactly this reason. Without these rows a build that
    // refused every topic would be green above.
    EXPECT_NO_THROW(provider.CreateTopic({"vessel.bow", "depth-raw"}, OwnedSchema{}));
    EXPECT_NO_THROW(provider.CreateTopic({"_private", "two words"}, OwnedSchema{}));
}

}  // namespace conformance
}  // namespace fletcher
