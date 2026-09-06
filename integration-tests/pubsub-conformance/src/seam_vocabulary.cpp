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
#include <fletcher/core/envelope.hpp>
#include <fletcher/core/status.hpp>
#include <fletcher/core/write_buffer.hpp>
#include <fletcher/pubsub/in_process_provider.hpp>
#include <fletcher/pubsub/schema_arrival.hpp>
#include <initializer_list>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
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

// ── §3.2 — the attachment set has a PUBLISHED FORM ──────────────────
//
// THE FORCING TEST for A6. Until PDA-DEC-AG2 `Attachments` was an
// `unordered_map`, which published no form at all: the sequence entries came out
// in — and were written onto the wire in — `std::hash`'s order, a property of the
// BUILD rather than of the value. A separately built binary, or a language
// binding, could not reproduce it from the value it held.
//
// The stand-in boundary below is deliberately impoverished: it is shown ONLY
// `size()`, `KeyAt()` and `ValueAt()` — everything a C#/Rust binding would be
// given — and it never sorts. It flattens what it is shown into bytes, and
// rebuilds a set from those bytes through `Set` alone. Three claims, all
// required, and the third is the one that was false before:
//
//   1. Round trip: the rebuilt set publishes a byte-identical form.
//   2. Construction order is not part of the value: the same entries added in
//      several different orders publish the identical form.
//   3. The form is the ORDER RULE, not merely "some fixed order": the key
//      sequence is ascending in unsigned-byte order, which is the rule a
//      boundary is told and can check without reproducing a collation.
//
// Claim 3 is what a build-dependent enumeration fails.
// `AnAlteredAttachmentSetPublishesADifferentForm` below is the live negative
// control: without it, a `PublishedForm` returning a constant would green every
// line here.

namespace {

void AppendU32(std::vector<uint8_t>& out, uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<uint8_t>((value >> shift) & 0xFFu));
    }
}

uint32_t ReadU32(const std::vector<uint8_t>& in, size_t& pos) {
    uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
        value |= static_cast<uint32_t>(in.at(pos + static_cast<size_t>(i))) << (8 * i);
    }
    pos += 4;
    return value;
}

// Everything the seam publishes about an attachment set, and nothing else. A
// boundary holding these bytes holds the whole value.
std::vector<uint8_t> PublishedForm(const Attachments& attachments) {
    std::vector<uint8_t> out;
    AppendU32(out, static_cast<uint32_t>(attachments.size()));
    for (size_t i = 0; i < attachments.size(); ++i) {
        const std::string_view key = attachments.KeyAt(i);
        const Blob& value = attachments.ValueAt(i);
        AppendU32(out, static_cast<uint32_t>(key.size()));
        out.insert(out.end(), key.begin(), key.end());
        AppendU32(out, static_cast<uint32_t>(value.size()));
        out.insert(out.end(), value.data(), value.data() + value.size());
    }
    return out;
}

// The far side of the stand-in boundary. It reads the sequence in the order it
// was handed and never sorts — a binding is not asked to reproduce a collation.
Attachments RebuildFromPublishedForm(const std::vector<uint8_t>& form) {
    Attachments rebuilt;
    size_t pos = 0;
    const uint32_t count = ReadU32(form, pos);
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t key_len = ReadU32(form, pos);
        std::string key(reinterpret_cast<const char*>(form.data() + pos), key_len);
        pos += key_len;
        const uint32_t value_len = ReadU32(form, pos);
        std::vector<uint8_t> bytes(form.begin() + static_cast<ptrdiff_t>(pos),
                                   form.begin() + static_cast<ptrdiff_t>(pos + value_len));
        pos += value_len;
        rebuilt.Set(std::move(key), value_len == 0 ? Blob() : Blob(std::move(bytes)));
    }
    return rebuilt;
}

Blob BlobOf(std::initializer_list<uint8_t> bytes) { return Blob(std::vector<uint8_t>(bytes)); }

// One set of entries, four keys, added in a caller-chosen order.
Attachments EntriesInOrder(const std::vector<size_t>& order) {
    const std::vector<std::pair<std::string, Blob>> entries{
        {"zulu", BlobOf({0x01, 0x02})},
        {"alpha", BlobOf({0x03})},
        {"mike", BlobOf({0x04, 0x05, 0x06})},
        {"bravo", BlobOf({0x07})},
    };
    Attachments attachments;
    for (const size_t i : order) attachments.Set(entries[i].first, entries[i].second);
    return attachments;
}

std::string KeySequence(const Attachments& attachments) {
    std::string out;
    for (size_t i = 0; i < attachments.size(); ++i) {
        if (i != 0) out += ' ';
        out += std::string(attachments.KeyAt(i));
    }
    return out;
}

}  // namespace

TEST(SeamVocabulary, AnAttachmentSetIsReconstructibleFromItsPublishedFormAlone) {
    const Attachments original = EntriesInOrder({0, 1, 2, 3});
    const std::vector<uint8_t> form = PublishedForm(original);

    // Claim 1 — the boundary's rebuild publishes the same form. It was shown
    // nothing but the sequence, so anything it fails to reproduce was never in
    // the published form to begin with.
    const Attachments rebuilt = RebuildFromPublishedForm(form);
    EXPECT_EQ(PublishedForm(rebuilt), form)
        << "a set rebuilt from its published form does not publish that form again; original keys ["
        << KeySequence(original) << "], rebuilt keys [" << KeySequence(rebuilt) << "]";

    // Claim 2 — construction order is not part of the value. Four orders,
    // including the reverse and one that overwrites a key it already set.
    const std::vector<std::vector<size_t>> orders{
        {0, 1, 2, 3},
        {3, 2, 1, 0},
        {1, 3, 0, 2},
        {2, 0, 3, 1, 0},
    };
    for (const std::vector<size_t>& order : orders) {
        const Attachments other = EntriesInOrder(order);
        EXPECT_EQ(PublishedForm(other), form)
            << "the same entries added in a different order publish a different form: keys ["
            << KeySequence(other) << "] against [" << KeySequence(original) << "]";
    }

    // Claim 3 — the order is the RULE, not merely stable within this build:
    // ascending unsigned-byte order of the key bytes, which is what §3.2
    // publishes and what a boundary is told to expect.
    EXPECT_EQ(KeySequence(original), "alpha bravo mike zulu")
        << "the published sequence is not in ascending unsigned-byte order of the key";

    // And it is a BYTE order, not a collation: a key that is a prefix of another
    // sorts first, and a byte above 0x7f sorts after every ASCII key. Written as
    // literals rather than derived, so this does not compare a rule with itself.
    Attachments byte_order;
    byte_order.Set("ab", Blob());
    byte_order.Set("\xC3\xA9", Blob());  // U+00E9, two bytes >= 0x80
    byte_order.Set("a", Blob());
    byte_order.Set("Z", Blob());
    EXPECT_EQ(KeySequence(byte_order), std::string("Z a ab \xC3\xA9"))
        << "the order is not over unsigned key bytes";

    // Claim 4 — the enumeration has a BOUND, and it is the same one for both
    // halves of the pair. Without this the positional form ships a rung nothing
    // exercises: an index outside [0, size()) must be refused with
    // kInvalidArgument — no clamping, no sentinel entry, no index that is valid
    // only sometimes — and `size()` itself is the first such index, which is the
    // off-by-one a boundary walking `i <= size()` would hit.
    for (const size_t outside : {original.size(), original.size() + 1, ~size_t{0}}) {
        PubSubStatus key_status = PubSubStatus::kOk;
        try {
            static_cast<void>(original.KeyAt(outside));
            ADD_FAILURE() << "KeyAt accepted index " << outside << " with size " << original.size();
        } catch (const PubSubError& error) {
            key_status = error.status();
        }
        EXPECT_EQ(key_status, PubSubStatus::kInvalidArgument);

        PubSubStatus value_status = PubSubStatus::kOk;
        try {
            static_cast<void>(original.ValueAt(outside));
            ADD_FAILURE() << "ValueAt accepted index " << outside << " with size "
                          << original.size();
        } catch (const PubSubError& error) {
            value_status = error.status();
        }
        EXPECT_EQ(value_status, PubSubStatus::kInvalidArgument);
    }
    // The bound on THAT narrowing: the last valid index is still valid, so a
    // container refusing every index is not green above.
    EXPECT_NO_THROW(static_cast<void>(original.KeyAt(original.size() - 1)));
    EXPECT_NO_THROW(static_cast<void>(original.ValueAt(original.size() - 1)));

    // And on an EMPTY set every index is outside, including zero.
    const Attachments none;
    EXPECT_THROW(static_cast<void>(none.KeyAt(0)), PubSubError);
}

// ── The live negative control for the test above ────────────────────
//
// Four mutations, each the smallest that keeps the set the same size where it
// can. Without these, a `PublishedForm` that dropped the values, or the keys, or
// returned a constant, would satisfy every line of the positive test.
TEST(SeamVocabulary, AnAlteredAttachmentSetPublishesADifferentForm) {
    const Attachments original = EntriesInOrder({0, 1, 2, 3});
    const std::vector<uint8_t> form = PublishedForm(original);

    // 1. One key byte changed.
    Attachments key_changed;
    key_changed.Set("zulu", BlobOf({0x01, 0x02}));
    key_changed.Set("alpha", BlobOf({0x03}));
    key_changed.Set("mikf", BlobOf({0x04, 0x05, 0x06}));  // mike -> mikf
    key_changed.Set("bravo", BlobOf({0x07}));
    EXPECT_NE(PublishedForm(key_changed), form) << "a changed key byte publishes the same form";

    // 2. One value byte changed.
    Attachments value_changed;
    value_changed.Set("zulu", BlobOf({0x01, 0x02}));
    value_changed.Set("alpha", BlobOf({0x03}));
    value_changed.Set("mike", BlobOf({0x04, 0x05, 0x07}));  // 0x06 -> 0x07
    value_changed.Set("bravo", BlobOf({0x07}));
    EXPECT_NE(PublishedForm(value_changed), form) << "a changed value byte publishes the same form";

    // 3. Two values swapped between their keys. Same keys, same multiset of
    //    values — only the pairing differs, which a key-only form would miss.
    Attachments swapped;
    swapped.Set("zulu", BlobOf({0x07}));
    swapped.Set("alpha", BlobOf({0x03}));
    swapped.Set("mike", BlobOf({0x04, 0x05, 0x06}));
    swapped.Set("bravo", BlobOf({0x01, 0x02}));
    EXPECT_NE(PublishedForm(swapped), form) << "swapping two values publishes the same form";

    // 4. One entry dropped.
    const Attachments dropped = EntriesInOrder({0, 1, 2});
    EXPECT_NE(PublishedForm(dropped), form) << "dropping an entry publishes the same form";
}

// ── §3.2 rung 2 — a label that cannot survive the trip is refused ────
//
// TWO LEGS, refused by different mechanisms on purpose.
//
// The ATTACH leg (owner ruling 2026-09-06, "refuse it when attached"): `Set`
// refuses a NUL-bearing key with `kInvalidArgument`. A boundary marshalling a
// key as a NUL-terminated string truncates it silently, so a key of three bytes
// with a zero in the middle and its one-byte prefix would be one attachment
// abroad, and one would overwrite the other.
//
// The ARRIVAL leg (owner ruling 2026-09-06, extending the above to the receive
// side): a wire-supplied key is refused by the WIRE checks, not by `Set` —
// `DeserializeEnvelope` raises `std::invalid_argument`, the type its own
// contract note reserves for a wire fault, and NEVER `PubSubError`, which that
// note reserves for a caller fault. That distinction is the point of this leg:
// it goes red on the exception TYPE if a later change routes decode through
// `Set`, which would label a wire fault a caller fault and put an exception into
// a transport frame.
TEST(SeamVocabulary, AnAttachmentKeyThatWouldTruncateIsRefused) {
    std::string nul_bearing = "a";
    nul_bearing.push_back(static_cast<char>(0));
    nul_bearing += "b";
    ASSERT_EQ(nul_bearing.size(), static_cast<size_t>(3));

    // ── Leg 1: attached locally.
    Attachments attachments;
    PubSubStatus attach_status = PubSubStatus::kOk;
    try {
        attachments.Set(nul_bearing, Blob());
        ADD_FAILURE() << "Set accepted a key carrying a zero byte";
    } catch (const PubSubError& error) {
        attach_status = error.status();
    }
    EXPECT_EQ(attach_status, PubSubStatus::kInvalidArgument);
    EXPECT_EQ(attachments.size(), static_cast<size_t>(0)) << "the refused key was stored anyway";

    // The bound on the narrowing, so a build that refused every key is not green
    // above: a key with no zero byte is accepted, INCLUDING an empty one — no
    // key-length bound and no empty-key refusal were adopted, because neither
    // was measured and an empty key survives a pointer-plus-length boundary.
    EXPECT_NO_THROW(attachments.Set("", Blob()));
    EXPECT_NO_THROW(attachments.Set(std::string(1024, 'k'), Blob()));

    // ── Leg 2: arriving from the wire.
    //
    // A hand-built envelope body, so the zero byte is in the BYTES rather than
    // in a key some local code constructed. Nothing above builds this: it is
    // what a foreign or hostile producer puts on the wire.
    std::vector<uint8_t> wire;
    AppendU32(wire, 1);  // row_len
    wire.push_back(0xFF);
    AppendU32(wire, 1);  // attachment count
    AppendU32(wire, static_cast<uint32_t>(nul_bearing.size()));
    wire.insert(wire.end(), nul_bearing.begin(), nul_bearing.end());
    AppendU32(wire, 0);  // blob_len

    auto owner = std::make_shared<const std::vector<uint8_t>>(wire);
    EXPECT_THROW(static_cast<void>(DeserializeEnvelope(owner)), std::invalid_argument)
        << "a wire-supplied key carrying a zero byte was not refused as malformed";

    // ...and specifically NOT as a caller fault. `PubSubError` derives from
    // `std::runtime_error` and `std::invalid_argument` from `std::logic_error`,
    // so these two catch clauses are disjoint and this cannot pass by accident.
    try {
        static_cast<void>(DeserializeEnvelope(owner));
        ADD_FAILURE() << "the malformed body was accepted";
    } catch (const PubSubError& error) {
        ADD_FAILURE() << "a wire fault was reported as a caller fault (status "
                      << static_cast<int32_t>(error.status())
                      << "): decode was routed through Attachments::Set";
    } catch (const std::invalid_argument&) {
        SUCCEED();
    }

    // The bound on THIS narrowing: the identical body with a clean key parses.
    // Without it, a decoder that refused every attachment would be green above.
    std::vector<uint8_t> clean;
    AppendU32(clean, 1);
    clean.push_back(0xFF);
    AppendU32(clean, 1);
    AppendU32(clean, 3);
    clean.insert(clean.end(), {'a', 'x', 'b'});
    AppendU32(clean, 0);
    auto clean_owner = std::make_shared<const std::vector<uint8_t>>(clean);
    Envelope parsed = DeserializeEnvelope(clean_owner);
    ASSERT_EQ(parsed.attachments.size(), static_cast<size_t>(1));
    EXPECT_EQ(parsed.attachments.KeyAt(0), "axb");
}

// ── Ruling 53's wire bytes, measured on the wire (AG2-DEBT-2) ────────
//
// The ruling that authorised this change is about the BYTES, so this reads them
// off `SerializeEnvelope`'s output rather than off the container. The Fast DDS
// codec's identical claim is asserted in that provider's own suite, where
// `EncodeEnvelopeBody` is reachable; this file's binary links no transport SDK.
TEST(SeamVocabulary, TheSameMessagePublishesTheSameWireBytes) {
    auto envelope_of = [](const std::vector<size_t>& order) {
        Envelope env;
        env.row = {0x10, 0x20};
        env.attachments = EntriesInOrder(order);
        return SerializeEnvelope(env);
    };

    const std::vector<uint8_t> reference = envelope_of({0, 1, 2, 3});
    EXPECT_EQ(envelope_of({3, 2, 1, 0}), reference)
        << "the same message built in a different order produced different wire bytes";
    EXPECT_EQ(envelope_of({1, 3, 0, 2}), reference)
        << "the same message built in a different order produced different wire bytes";

    // And the emitted sequence is ascending — the property that is a function of
    // the VALUE rather than of this build's hash. Before AG2 these bytes came out
    // in `std::hash` order, which on this toolchain is neither ascending nor
    // derivable from the value.
    size_t pos = 0;
    const uint32_t row_len = ReadU32(reference, pos);
    pos += row_len;
    const uint32_t count = ReadU32(reference, pos);
    ASSERT_EQ(count, static_cast<uint32_t>(4));
    std::vector<std::string> emitted;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t key_len = ReadU32(reference, pos);
        emitted.emplace_back(reinterpret_cast<const char*>(reference.data() + pos), key_len);
        pos += key_len;
        pos += ReadU32(reference, pos);
    }
    EXPECT_EQ(emitted, (std::vector<std::string>{"alpha", "bravo", "mike", "zulu"}))
        << "the wire key order is not ascending unsigned-byte order";
}

}  // namespace conformance
}  // namespace fletcher
