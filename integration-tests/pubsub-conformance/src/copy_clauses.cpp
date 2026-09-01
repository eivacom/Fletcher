// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The `CopyAccounting` suite: zero-copy stops being prose (§8) and becomes a
// test that fails when it stops being true.
//
// Seven ctest entries, all in-process, all in milliseconds. The table of what
// each one is lives in README.md.
//
// The control is not decoration. PDA-DEC-1's lesson was that a guard nobody
// made go red is a guard nobody has measured, so `StagingIsCaught` runs the same
// capture through the same `Judge()` against a provider that deliberately
// copies, and demands the numbers 1 and 2. Stub the instrument to always report
// zero and that test fails; make the instrument inert (a null or stale window
// base) and the three real subjects fail instead. There is no state in which
// every test in this file passes and the instrument is not working.

#include <gtest/gtest.h>

#include <cstddef>
#include <iostream>
#include <memory>
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

/// Publish the refill cost as a number, per the owner's 2026-09-01 ruling —
/// permitted, but never silent. RecordProperty puts it in the JUnit XML; the
/// stream puts it where `ctest -V` and a failing run can see it.
void PublishRefillCost(const std::string& tag, const CopyVerdict& verdict) {
    ::testing::Test::RecordProperty("refill_moves_" + tag, static_cast<int>(verdict.refill_moves));
    ::testing::Test::RecordProperty("refill_bytes_" + tag, static_cast<int>(verdict.refill_bytes));
    std::cout << "[ copy     ] " << tag << ": refill_moves=" << verdict.refill_moves
              << " refill_bytes=" << verdict.refill_bytes << std::endl;
}

}  // namespace

/// Everything that must hold before a verdict may be read at all.
///
/// Order matters and is the point: a publish that threw, a delivery that never
/// happened, or bytes that arrived GARBLED must each fail as themselves.
/// Otherwise "nothing arrived" reads as "no copies" (rung-2 item 8) and
/// corruption reads as a copy (rung-1 item 4).
///
/// A macro rather than a helper so `ASSERT_` aborts the TEST, not a helper frame.
#define COPY_MUST_DELIVER_CLEANLY(trip, expected_row_bytes, expected_attachments)                 \
    do {                                                                                          \
        ASSERT_TRUE((trip).error.empty()) << "publish threw: " << (trip).error;                   \
        ASSERT_EQ((trip).ledger.deliveries, static_cast<size_t>(1))                               \
            << "expected exactly ONE delivery before any verdict is read; got "                   \
            << (trip).ledger.deliveries;                                                          \
        ASSERT_EQ((trip).ledger.delivered_len, static_cast<size_t>(expected_row_bytes));          \
        ASSERT_TRUE((trip).ledger.row_content_ok)                                                 \
            << "the row arrived GARBLED, which is a different failure from a copy";               \
        ASSERT_EQ((trip).ledger.attachments.size(), static_cast<size_t>(expected_attachments));   \
        for (const ::fletcher::conformance::AttachmentTrace& trace : (trip).ledger.attachments) { \
            ASSERT_TRUE(trace.content_ok) << "attachment '" << trace.key                          \
                                          << "' arrived GARBLED, which is a different "           \
                                             "failure from a copy";                               \
        }                                                                                         \
    } while (false)

/// Value-parameterised over the registered subjects, so the subject list is
/// compile-time and a subject that stopped being registered is a MISSING entry
/// in `ctest -N` rather than a skip nobody reads. There is no `GTEST_SKIP` in
/// this file.
class CopyAccounting : public ::testing::TestWithParam<CopySubject> {};

// ── The forcing test ────────────────────────────────────────────────
//
// Legs 1 and 2 together: the row, at both sizes, and two attachments. The bytes
// a subscriber sees are the same bytes, at the same address, the publisher wrote.
TEST_P(CopyAccounting, PublishAndReceivePerformNoPayloadCopies) {
    for (size_t row_bytes : {kSmallRowBytes, kLargeRowBytes}) {
        const std::string tag = GetParam().label + "/" + std::to_string(row_bytes) + "B";
        SCOPED_TRACE(tag);

        std::unique_ptr<CopyRunner> runner = GetParam()();
        ASSERT_NE(runner, nullptr);

        // Two attachments, never zero: `attachment_copies == 0` must not be
        // satisfiable by an empty map (rung-2 item 10).
        const Attachments attachments = MakeCopyAttachments();
        ASSERT_EQ(attachments.size(), kAttachmentCount);
        ASSERT_GE(attachments.size(), static_cast<size_t>(2));

        RoundTrip trip =
            RunRoundTrip(*runner, FreshTopic("CopyAccounting"), row_bytes, attachments);
        COPY_MUST_DELIVER_CLEANLY(trip, row_bytes, kAttachmentCount);

        const CopyVerdict verdict = Judge(trip.ledger);

        EXPECT_EQ(verdict.row_copies, static_cast<size_t>(0))
            << "the delivered row is not the encode window: encode_base="
            << static_cast<const void*>(trip.ledger.encode_base) << " (" << trip.ledger.encode_len
            << " B) vs delivered=" << static_cast<const void*>(trip.ledger.delivered_data) << " ("
            << trip.ledger.delivered_len << " B)";

        EXPECT_EQ(verdict.attachment_copies, static_cast<size_t>(0))
            << "a delivered attachment's bytes are not the published bytes";

        // Refill relocation is PERMITTED and its cost is published, never
        // failed — spec §3.1 clause 1, owner ruling 2026-09-01. Every OTHER
        // byte movement is what the two expectations above forbid.
        PublishRefillCost(tag, verdict);
    }
}

INSTANTIATE_TEST_SUITE_P(CopySubjects, CopyAccounting,
                         ::testing::ValuesIn(CopyAccountingSubjects()));

// ── The live negative control ───────────────────────────────────────
//
// A provider that copies, scored by the SAME Judge() through the SAME capture.
// This is the test that fails if the instrument is stubbed, always-zero, or
// deleted — and it is deliberately NOT in the parameterised list, so rung-1
// item 2 ("every registered subject faces the same numbers") stays true.
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

// ── Leg 3: the one receive-side copy today's Blob forces ────────────
//
// PINNED AT EXACTLY ONE (owner ruling 2026-09-01, "Pin at one"). A provider
// holding payload bytes in memory it owns — where a transport's loaned sample
// would be — cannot publish them as a `Blob` without copying, because
// `shared_ptr<const vector<uint8_t>>` cannot alias foreign memory (§3.2).
//
// **PDA-DEC-3 owns removing that limitation, and when it does THIS TEST GOES
// RED ON PURPOSE.** That is not a regression: it is the tripwire that stops the
// fix landing silently or half-landing. Update the number here as part of that
// change; do not delete the test.
TEST(CopyAccounting, BorrowedAttachmentCostsExactlyOneCopy) {
    RoundTrip trip = RunBorrowedAttachmentRoundTrip(FreshTopic("CopyAccountingBorrowed"));
    COPY_MUST_DELIVER_CLEANLY(trip, kSmallRowBytes, static_cast<size_t>(1));

    const CopyVerdict verdict = Judge(trip.ledger);
    EXPECT_EQ(verdict.row_copies, static_cast<size_t>(0)) << "the row leg is unaffected by §3.2";
    EXPECT_EQ(verdict.attachment_copies, static_cast<size_t>(1))
        << "spec §3.2 forces exactly one copy of borrowed transport memory today. If this is 0, "
           "PDA-DEC-3 has landed and this test must be updated to demand 0 — see the comment "
           "above. If it is >1, something copies the blob a SECOND time and that is a defect.";
}

// ── The refill counter is live ──────────────────────────────────────
//
// Refill relocation is permitted and published (2026-09-01 ruling), which is
// only meaningful if the counter can be non-zero. `VectorWriteBuffer` has a
// kChunk of 256 and reallocates at 512/1536/3584 under sub-kChunk appends, so
// the 4 KiB row at `kAppendChunk`-sized appends must relocate; the arena-backed
// `FixedWriteBuffer` must not. Both directions are asserted, so neither an
// always-zero nor an always-nonzero counter survives.
TEST(CopyAccounting, RefillMovementIsCountedNotFailed) {
    std::unique_ptr<CopyRunner> growable = SubjectNamed("InProcessLoopback")();
    RoundTrip grown = RunRoundTrip(*growable, FreshTopic("CopyAccountingRefill"), kLargeRowBytes,
                                   MakeCopyAttachments());
    COPY_MUST_DELIVER_CLEANLY(grown, kLargeRowBytes, kAttachmentCount);
    const CopyVerdict grown_verdict = Judge(grown.ledger);
    PublishRefillCost("InProcessLoopback/4096B", grown_verdict);

    EXPECT_EQ(grown_verdict.row_copies, static_cast<size_t>(0))
        << "a refill must preserve the bytes verbatim and the FINAL window is what is delivered";
    EXPECT_GT(grown_verdict.refill_moves, static_cast<size_t>(0))
        << "a growable buffer relocated nothing writing 4 KiB in " << kAppendChunk
        << "-byte appends — either the append granularity stopped forcing it or the refill "
           "counter is inert";
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
}

// ── The pure function, without a provider ───────────────────────────
TEST(CopyAccounting, JudgeArithmeticIsSound) {
    std::vector<uint8_t> window(128);
    const uint8_t* base = window.data();

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

    // DEBT-2, the shape strict equality exists to catch: an in-place memmove
    // down to the window base delivers `encode_base` with a SHORTER length.
    // Every payload byte moved; memcmp of the delivered range still passes.
    // Containment would score this 0.
    CopyLedger memmoved = clean;
    memmoved.delivered_len = 32;
    EXPECT_EQ(Judge(memmoved).row_copies, static_cast<size_t>(1))
        << "an identity-preserving in-place move must not score as zero copies";

    // Nothing delivered is never "no copies".
    CopyLedger nothing = clean;
    nothing.delivered_data = nullptr;
    EXPECT_EQ(Judge(nothing).row_copies, static_cast<size_t>(1));

    CopyLedger blobs = clean;
    blobs.attachments = {
        AttachmentTrace{"same", base, 16, base, 16, true},
        AttachmentTrace{"elsewhere", base, 16, base + 16, 16, true},
        AttachmentTrace{"absent", base, 16, nullptr, 0, false},
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
