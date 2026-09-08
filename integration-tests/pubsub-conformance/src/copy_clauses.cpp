// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The `CopyAccounting` suite: zero-copy stops being prose (§8) and becomes a
// test that fails when it stops being true. Eleven ctest entries, all
// in-process, all in milliseconds. What each one is, and why the mechanism is
// provenance rather than counting, is in README.md.

#include <gtest/gtest.h>

#include <cstddef>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "fletcher/conformance/copy_accounting.hpp"
#include "fletcher/conformance/fixtures.hpp"

namespace fletcher {
namespace conformance {
namespace {

/// Look a subject up by label rather than by index: a reordered registry must
/// not silently point a test at a different provider.
const CopySubject& SubjectNamed(const std::string& label) {
    for (const CopySubject& subject : CopyAccountingSubjects()) {
        if (subject.label == label) return subject;
    }
    throw std::runtime_error("CopyAccounting: no subject named " + label);
}

const AttachmentTrace& TraceNamed(const CopyLedger& ledger, const std::string& key) {
    for (const AttachmentTrace& trace : ledger.attachments) {
        if (trace.key == key) return trace;
    }
    throw std::runtime_error("CopyAccounting: no attachment trace named " + key);
}

std::string Hex(Address address) {
    std::ostringstream out;
    out << "0x" << std::hex << address;
    return out.str();
}

/// Publish the refill cost as a number, per the owner's 2026-09-01 ruling —
/// permitted, but never silent. RecordProperty puts it in the JUnit XML; the
/// stream puts it where `ctest -V` and a failing run can see it.
void PublishRefillCost(const std::string& tag, const CopyVerdict& verdict) {
    ::testing::Test::RecordProperty("refill_moves_" + tag, std::to_string(verdict.refill_moves));
    ::testing::Test::RecordProperty("refill_bytes_" + tag, std::to_string(verdict.refill_bytes));
    std::cout << "[ copy     ] " << tag << ": refill_moves=" << verdict.refill_moves
              << " refill_bytes=" << verdict.refill_bytes << std::endl;
}

}  // namespace

/// Everything that must hold before a verdict may be read at all. Order is the
/// point: a publish that threw, a delivery that never happened, an encode
/// window that did not survive to the callback (P5), and bytes that arrived
/// GARBLED or MISSING must each fail as themselves — otherwise "nothing
/// arrived" reads as "no copies" and corruption reads as a copy. A macro, not a
/// helper, so `ASSERT_` aborts the TEST rather than a helper frame.
#define COPY_MUST_DELIVER_CLEANLY(trip, expected_row_bytes, expected_attachments)                 \
    do {                                                                                          \
        ASSERT_TRUE((trip).error.empty()) << "publish threw: " << (trip).error;                   \
        ASSERT_EQ((trip).ledger.deliveries, static_cast<size_t>(1))                               \
            << "expected exactly ONE delivery before any verdict is read; got "                   \
            << (trip).ledger.deliveries;                                                          \
        ASSERT_TRUE((trip).ledger.window_intact)                                                  \
            << "P5 VIOLATED: the encode window no longer held the row when the callback ran, "    \
               "so this subject frees, recycles or pools it before delivery and address "         \
               "provenance is unsound for it — it must not be registered (see README)";           \
        ASSERT_EQ((trip).ledger.delivered_len, static_cast<size_t>(expected_row_bytes));          \
        ASSERT_TRUE((trip).ledger.row_content_ok)                                                 \
            << "the row arrived GARBLED, which is a different failure from a copy";               \
        ASSERT_EQ((trip).ledger.delivered_attachments, static_cast<size_t>(expected_attachments)) \
            << "the DELIVERY carried a different number of attachments than were published";      \
        for (const ::fletcher::conformance::AttachmentTrace& t : (trip).ledger.attachments) {     \
            ASSERT_NE(t.delivered_data, static_cast<::fletcher::conformance::Address>(0))         \
                << "attachment '" << t.key << "' arrived MISSING";                                \
            ASSERT_TRUE(t.content_ok) << "attachment '" << t.key                                  \
                                      << "' arrived GARBLED, which is a different "               \
                                         "failure from a copy";                                   \
        }                                                                                         \
    } while (false)

/// The producer half of the same discipline: a leg whose sampler never ran must
/// fail AS ITSELF rather than be read at all. Same place, same reason as
/// COPY_MUST_DELIVER_CLEANLY's "arrived MISSING" (design review A1-DEBT-4). The
/// verdict itself carries no number for such a leg — `encode_copies` is empty —
/// so this reads as the named failure and not as a `bad_optional_access` from
/// somewhere below.
#define COPY_MUST_HAVE_PRODUCED(trip, expected_row_bytes)                                      \
    do {                                                                                       \
        ASSERT_NE((trip).ledger.produced_at, static_cast<::fletcher::conformance::Address>(0)) \
            << "the producer sampler never ran, so there is no `encode_copies` to read — "   \
               "this leg proves nothing either way";                                           \
        ASSERT_EQ((trip).ledger.produced_len, static_cast<size_t>(expected_row_bytes))         \
            << "the producer wrote a different number of bytes than the row it was asked "     \
               "for, so the address it recorded is not the row's";                             \
    } while (false)

/// Value-parameterised over the registered subjects, so a subject that stopped
/// being registered is a MISSING `ctest -N` entry rather than a skip nobody
/// reads. There is no `GTEST_SKIP` in this file.
class CopyAccounting : public ::testing::TestWithParam<CopySubject> {};

// ── The forcing test ────────────────────────────────────────────────
//
// Legs 1 and 2: the row, at both sizes, and two attachments. The bytes a
// subscriber sees are the same bytes, at the same address, the publisher wrote.
TEST_P(CopyAccounting, PublishAndReceivePerformNoPayloadCopies) {
    for (size_t row_bytes : {kSmallRowBytes, kLargeRowBytes}) {
        const std::string tag = GetParam().label + "/" + std::to_string(row_bytes) + "B";
        SCOPED_TRACE(tag);

        std::unique_ptr<CopyRunner> runner = GetParam()();
        ASSERT_NE(runner, nullptr);

        // Two attachments, never zero: `attachment_copies == 0` must not be
        // satisfiable by an empty map.
        const Attachments attachments = MakeCopyAttachments();
        ASSERT_EQ(attachments.size(), kAttachmentCount);
        ASSERT_GE(attachments.size(), static_cast<size_t>(2));

        RoundTrip trip =
            RunRoundTrip(*runner, FreshTopic("CopyAccounting"), row_bytes, attachments);
        COPY_MUST_DELIVER_CLEANLY(trip, row_bytes, kAttachmentCount);

        const CopyVerdict verdict = Judge(trip.ledger);

        EXPECT_EQ(verdict.row_copies, static_cast<size_t>(0))
            << "the delivered row is not the encode window: encode_base="
            << Hex(trip.ledger.encode_base) << " (" << trip.ledger.encode_len
            << " B) vs delivered=" << Hex(trip.ledger.delivered_data) << " ("
            << trip.ledger.delivered_len << " B)";

        EXPECT_EQ(verdict.attachment_copies, static_cast<size_t>(0))
            << "a delivered attachment's bytes are not the published bytes";

        // Refill relocation is PERMITTED and its cost is published, never
        // failed — spec §3.1 clause 1, owner ruling 2026-09-01.
        PublishRefillCost(tag, verdict);
    }
}

// ── The forcing test for PDA-DEC-A1 ─────────────────────────────────
//
// The CLIENT's half of §8, which the oracle could not see until this item: the
// bytes a producer composes are the bytes the subscriber reads, at one address,
// with nothing copied in between. `AppendInPlace` lends the window's write
// cursor, so a producer that uses it scores `encode_copies == 0` — and the same
// publish still scores `row_copies == 0`, because the two are consecutive halves
// of one path rather than two views of the same half.
//
// What green claims, exactly: the INTERFACE permits an uncopied send, measured
// with a stand-in producer. Not that a C#/Rust binding achieves it — none exists
// to measure (owner ruling 2026-09-04). `StagingProducerIsCaught` below is the
// live negative control that stops this greening on a stuck sampler.
TEST_P(CopyAccounting, InPlaceEncodeWritesIntoTheDeliveredWindow) {
    for (size_t row_bytes : {kSmallRowBytes, kLargeRowBytes}) {
        const std::string tag = GetParam().label + "/" + std::to_string(row_bytes) + "B";
        SCOPED_TRACE(tag);

        std::unique_ptr<CopyRunner> runner = GetParam()();
        ASSERT_NE(runner, nullptr);

        RoundTrip trip = RunProducerRoundTrip(*runner, FreshTopic("CopyAccountingInPlace"),
                                              row_bytes, ProducerMode::kInPlace);
        COPY_MUST_DELIVER_CLEANLY(trip, row_bytes, static_cast<size_t>(0));
        COPY_MUST_HAVE_PRODUCED(trip, row_bytes);

        const CopyVerdict verdict = Judge(trip.ledger);

        ASSERT_TRUE(verdict.encode_copies.has_value())
            << "the verdict carries no producer number, so nothing here is a measurement";
        EXPECT_EQ(*verdict.encode_copies, static_cast<size_t>(0))
            << "the producer did not write into the delivered window: produced_at="
            << Hex(trip.ledger.produced_at) << " vs the window cursor it was lent. A client "
            << "handed a WriteBuffer must be able to compose the row IN it — otherwise §8's "
            << "row property is unreachable from a language binding and the copy is invisible";

        EXPECT_EQ(verdict.row_copies, static_cast<size_t>(0))
            << "the delivered row is not the encode window: encode_base="
            << Hex(trip.ledger.encode_base) << " (" << trip.ledger.encode_len
            << " B) vs delivered=" << Hex(trip.ledger.delivered_data) << " ("
            << trip.ledger.delivered_len << " B)";

        // No PublishRefillCost here, deliberately. This leg's producer composes
        // one row into a window that starts empty, so there are no bytes below
        // the lend point for a refill to move and the number would be a
        // STRUCTURAL zero rather than a measurement — publishing it would put a
        // figure nobody measured beside figures somebody did. The refill
        // sampling itself is unchanged in shape (it runs in `EncodeProduced`),
        // and `RefillMovementIsCountedNotFailed` is the entry that measures it.
    }
}

INSTANTIATE_TEST_SUITE_P(CopySubjects, CopyAccounting,
                         ::testing::ValuesIn(CopyAccountingSubjects()));

// ── The producer-side live negative control ─────────────────────────
//
// The blindness itself, pinned as a test. A producer that composes its row in
// its own vector and hands it over with one `Append` — exactly the workaround a
// language binding was forced into before `AppendInPlace` existed — pays one
// whole-row copy. The PROVIDER half is spotless for it: `row_copies == 0`,
// because the staged bytes are copied into the window BEFORE the window base is
// sampled. So `row_copies` alone was green over a lost property, which is why
// this control and the forcing test are one item and not two.
//
// It is also the sampler's other direction: a sampler stuck TRUE reddens here,
// a sampler stuck FALSE reddens the three forcing entries above.
TEST(CopyAccounting, StagingProducerIsCaught) {
    const CopySubject positive = SubjectNamed("SeamProbe");
    std::unique_ptr<CopyRunner> runner = positive();
    ASSERT_NE(runner, nullptr);

    RoundTrip trip = RunProducerRoundTrip(*runner, FreshTopic("CopyAccountingStagedProducer"),
                                          kSmallRowBytes, ProducerMode::kStaged);
    COPY_MUST_DELIVER_CLEANLY(trip, kSmallRowBytes, static_cast<size_t>(0));
    COPY_MUST_HAVE_PRODUCED(trip, kSmallRowBytes);

    const CopyVerdict verdict = Judge(trip.ledger);
    ASSERT_TRUE(verdict.encode_copies.has_value())
        << "the verdict carries no producer number, so nothing here is a measurement";
    EXPECT_EQ(*verdict.encode_copies, static_cast<size_t>(1))
        << "the instrument did not see a whole-row staging copy it was handed on a plate — it "
           "is inert, stubbed or always-zero, and every encode_copies==0 above is worthless";
    EXPECT_EQ(verdict.row_copies, static_cast<size_t>(0))
        << "the provider half must stay CLEAN here. If it does not, this control is measuring "
           "the provider rather than the producer, and it no longer demonstrates that "
           "row_copies was green over a copy it could not see";
}

// ── The live negative control ───────────────────────────────────────
//
// A provider that copies, scored by the SAME Judge() through the SAME capture:
// the test that fails if the instrument is stubbed, always-zero or deleted.
// Deliberately NOT in the parameterised list.
TEST(CopyAccounting, StagingIsCaught) {
    const CopySubject control = StagingControlSubject();
    std::unique_ptr<CopyRunner> runner = control();
    ASSERT_NE(runner, nullptr);

    const Attachments attachments = MakeCopyAttachments();
    RoundTrip trip =
        RunRoundTrip(*runner, FreshTopic("CopyAccountingControl"), kSmallRowBytes, attachments);

    // The control's bytes arrive INTACT — it copies faithfully. So this passes,
    // and the verdict below is the only thing that can tell the difference.
    COPY_MUST_DELIVER_CLEANLY(trip, kSmallRowBytes, kAttachmentCount);

    const CopyVerdict verdict = Judge(trip.ledger);
    EXPECT_EQ(verdict.row_copies, static_cast<size_t>(1))
        << "the instrument did not see a row copy it was handed on a plate — it is inert, "
           "stubbed or always-zero, and every green in this file is worthless";
    EXPECT_EQ(verdict.attachment_copies, kAttachmentCount)
        << "the instrument did not see " << kAttachmentCount
        << " deep-copied attachments it was handed on a plate";
}

// ── Leg 3: borrowed memory now costs NOTHING ────────────────────────
//
// **This pin was 1 and is now 0, and the change is the point.** The owner's
// 2026-09-01 ruling pinned the §3.2 copy at exactly one so that removing it
// would turn this test RED and force the stage that removed it to come back
// here — silence being how such a fix gets forgotten or half-landed. PDA-DEC-3
// removed it: `Blob` became an owner plus a span, so a provider hands over bytes
// it already holds where they lie. The tripwire fired as designed (the
// static_assert in copy_accounting.hpp stopped the BUILD first), and this is the
// deliberate, visible update it demanded.
//
// Everything else about the leg is unchanged: the PROVIDER, not this test, must
// produce the `Blob`; a caller-owned blob rides the same publish and must cross
// untouched; and the number stays three-valued, because the identical leg against
// a deep-copying provider still scores 2.
TEST(CopyAccounting, BorrowedAttachmentCostsNoCopies) {
    RoundTrip trip = RunBorrowedAttachmentRoundTrip(FreshTopic("CopyAccountingBorrowed"));
    COPY_MUST_DELIVER_CLEANLY(trip, kSmallRowBytes, static_cast<size_t>(2));

    const AttachmentTrace& owned = TraceNamed(trip.ledger, "owned");
    EXPECT_EQ(owned.delivered_data, owned.published_data)
        << "the provider copied a CALLER-OWNED blob (" << Hex(owned.published_data) << " -> "
        << Hex(owned.delivered_data) << "); only borrowed memory may cost a copy";

    const AttachmentTrace& loaned = TraceNamed(trip.ledger, "loaned");
    EXPECT_EQ(loaned.delivered_data, loaned.published_data)
        << "the provider copied its own arena bytes (" << Hex(loaned.published_data) << " -> "
        << Hex(loaned.delivered_data)
        << ") instead of handing them over where they lay; §3.2's owner-plus-span is what "
           "removed that copy and this number is what guards it";

    const CopyVerdict verdict = Judge(trip.ledger);
    EXPECT_EQ(verdict.row_copies, static_cast<size_t>(0)) << "the row leg is unaffected by §3.2";
    EXPECT_EQ(verdict.attachment_copies, static_cast<size_t>(0))
        << "the seam still costs a copy to carry memory it does not own";

    // Standing proof that the "1" above is a measurement of the provider and
    // not an arithmetic constant of the harness: the identical leg, against a
    // provider that copies everything, scores 2.
    RoundTrip copying = RunBorrowedAttachmentRoundTrip(FreshTopic("CopyAccountingBorrowed"), true);
    COPY_MUST_DELIVER_CLEANLY(copying, kSmallRowBytes, static_cast<size_t>(2));
    EXPECT_EQ(Judge(copying.ledger).attachment_copies, static_cast<size_t>(2))
        << "a provider that deep-copies every blob still scored 1 — the leg is measuring the "
           "harness, not the provider";
}

// ── The refill counter is live ──────────────────────────────────────
//
// Permitted and published (2026-09-01 ruling), which is only meaningful if the
// counter can be non-zero. Both directions are asserted against HARNESS-OWNED
// buffers, so neither an always-zero nor an always-nonzero counter survives and
// pre-sizing a provider's send buffer (an improvement) cannot turn this red.
TEST(CopyAccounting, RefillMovementIsCountedNotFailed) {
    const CopySubject growable_subject = GrowableControlSubject();
    std::unique_ptr<CopyRunner> growable = growable_subject();
    RoundTrip grown = RunRoundTrip(*growable, FreshTopic("CopyAccountingRefill"), kLargeRowBytes,
                                   MakeCopyAttachments());
    COPY_MUST_DELIVER_CLEANLY(grown, kLargeRowBytes, kAttachmentCount);
    const CopyVerdict grown_verdict = Judge(grown.ledger);
    PublishRefillCost("GrowableProbe/4096B", grown_verdict);

    EXPECT_EQ(grown_verdict.row_copies, static_cast<size_t>(0))
        << "a refill must preserve the bytes verbatim and the FINAL window is what is delivered";
    EXPECT_GT(grown_verdict.refill_moves, static_cast<size_t>(0))
        << "a window that relocates on every refill reported none — the refill counter is inert";
    EXPECT_GT(grown_verdict.refill_bytes, static_cast<size_t>(0));

    std::unique_ptr<CopyRunner> fixed = SubjectNamed("SeamProbe")();
    RoundTrip flat = RunRoundTrip(*fixed, FreshTopic("CopyAccountingRefill"), kLargeRowBytes,
                                  MakeCopyAttachments());
    COPY_MUST_DELIVER_CLEANLY(flat, kLargeRowBytes, kAttachmentCount);
    const CopyVerdict flat_verdict = Judge(flat.ledger);
    PublishRefillCost("SeamProbe/4096B", flat_verdict);

    EXPECT_EQ(flat_verdict.refill_moves, static_cast<size_t>(0))
        << "a fixed-capacity buffer cannot refill (§3.1 clause 4), so a non-zero count here "
           "means the counter fires on something other than relocation";
    EXPECT_EQ(flat_verdict.refill_bytes, static_cast<size_t>(0));
}

// ── The pure function, without a provider ───────────────────────────
TEST(CopyAccounting, JudgeArithmeticIsSound) {
    std::vector<uint8_t> window(128);
    const auto base = reinterpret_cast<Address>(window.data());

    CopyLedger clean;
    clean.encode_base = base;
    clean.encode_len = 64;
    clean.deliveries = 1;
    clean.delivered_data = base;
    clean.delivered_len = 64;
    clean.produced_at = base;
    clean.produced_len = 64;
    clean.produced_in_window = true;
    EXPECT_EQ(Judge(clean).row_copies, static_cast<size_t>(0));
    EXPECT_EQ(Judge(clean).encode_copies, std::optional<size_t>(0));

    // The two halves are INDEPENDENT, and this is what says so: a producer that
    // staged its row still delivers a spotless provider half, so a build that
    // folded encode_copies into row_copies (or read one off the other) fails
    // here without needing a provider.
    CopyLedger staged = clean;
    staged.produced_in_window = false;
    staged.produced_at = base + 64;
    EXPECT_EQ(Judge(staged).encode_copies, std::optional<size_t>(1));
    EXPECT_EQ(Judge(staged).row_copies, static_cast<size_t>(0))
        << "staging on the CLIENT side must not be charged to the provider half";

    // A second address is a copy.
    CopyLedger moved = clean;
    moved.delivered_data = base + 64;
    EXPECT_EQ(Judge(moved).row_copies, static_cast<size_t>(1));
    // …and the mirror of the staged case: a PROVIDER that copied a row the client
    // composed in place leaves the producer half clean.
    EXPECT_EQ(Judge(moved).encode_copies, std::optional<size_t>(0))
        << "a copy on the provider half must not be charged to the client half";

    // An UNSAMPLED leg carries no producer verdict at all. Every pre-existing
    // leg is one, and this is the row that stops `encode_copies` defaulting into
    // "the client copied the row" for all of them (code review S4, compliance
    // F3): the ledger declares the state unreadable, and the verdict now makes
    // it unreadable rather than leaving a macro to remember.
    CopyLedger unsampled = clean;
    unsampled.produced_at = 0;
    unsampled.produced_len = 0;
    unsampled.produced_in_window = false;
    EXPECT_FALSE(Judge(unsampled).encode_copies.has_value())
        << "a leg whose producer was never sampled must carry NO producer number, not 1";
    EXPECT_EQ(Judge(unsampled).row_copies, static_cast<size_t>(0))
        << "an unsampled producer must not disturb the provider half";

    // The shape strict equality exists to catch: an in-place memmove down to
    // the window base, delivering `encode_base` with a SHORTER length.
    CopyLedger memmoved = clean;
    memmoved.delivered_len = 32;
    EXPECT_EQ(Judge(memmoved).row_copies, static_cast<size_t>(1))
        << "an identity-preserving in-place move must not score as zero copies";

    // Nothing delivered is never "no copies".
    CopyLedger nothing = clean;
    nothing.delivered_data = 0;
    EXPECT_EQ(Judge(nothing).row_copies, static_cast<size_t>(1));

    CopyLedger blobs = clean;
    blobs.attachments = {
        AttachmentTrace{"same", base, 16, base, 16, true},
        AttachmentTrace{"elsewhere", base, 16, base + 16, 16, true},
        AttachmentTrace{"absent", base, 16, 0, 0, false},
    };
    EXPECT_EQ(Judge(blobs).attachment_copies, static_cast<size_t>(2));

    // Refill numbers are reported verbatim, never folded into a copy count.
    CopyLedger refilled = clean;
    refilled.refill_moves = 3;
    refilled.refill_bytes = 5632;
    const CopyVerdict verdict = Judge(refilled);
    EXPECT_EQ(verdict.row_copies, static_cast<size_t>(0));
    EXPECT_EQ(verdict.refill_moves, static_cast<size_t>(3));
    EXPECT_EQ(verdict.refill_bytes, static_cast<size_t>(5632));
}

}  // namespace conformance
}  // namespace fletcher
