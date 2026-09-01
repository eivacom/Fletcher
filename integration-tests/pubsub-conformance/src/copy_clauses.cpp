// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The `CopyAccounting` suite: zero-copy stops being prose (§8) and becomes a
// test that fails when it stops being true. Seven ctest entries, all
// in-process, all in milliseconds. What each one is, and why the mechanism is
// provenance rather than counting, is in README.md.

#include <gtest/gtest.h>

#include <cstddef>
#include <iostream>
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

INSTANTIATE_TEST_SUITE_P(CopySubjects, CopyAccounting,
                         ::testing::ValuesIn(CopyAccountingSubjects()));

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
    EXPECT_EQ(Judge(clean).row_copies, static_cast<size_t>(0));

    // A second address is a copy.
    CopyLedger moved = clean;
    moved.delivered_data = base + 64;
    EXPECT_EQ(Judge(moved).row_copies, static_cast<size_t>(1));

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
