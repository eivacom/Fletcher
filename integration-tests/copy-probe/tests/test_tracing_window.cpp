// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The source-tracing window and D-BIND-61's scoring rule, against producers
// written here, whose every byte movement is known. The binding legs that use
// this instrument (c-abi's probe shim, the C# copy-oracle tests) can only be as
// right as these are: each case is one way a row can reach the window, and the
// verdict it must get.
//
// The row shape is the codec's for one binary field - a null bitfield byte
// (`AppendZeros`, then patched), a u32 length (`AppendFixed`), then the bytes
// (`Append`) - so the window sees what a real encode puts through it.

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <fletcher/copy_probe/ledger.hpp>
#include <fletcher/copy_probe/seam_probe_provider.hpp>
#include <fletcher/copy_probe/tracing_window.hpp>
#include <memory>
#include <stdexcept>
#include <vector>

namespace fletcher::copy_probe {
namespace {

constexpr size_t kSlot = 256;

std::vector<uint8_t> Payload(size_t len) {
    std::vector<uint8_t> out(len);
    for (size_t i = 0; i < len; ++i) out[i] = static_cast<uint8_t>((i * 31u + 7u) & 0xFFu);
    return out;
}

/// What the codec does for one non-null binary field, reading `bytes`.
void EncodeBinaryRow(WriteBuffer& out, const uint8_t* bytes, size_t len) {
    out.AppendZeros(1);
    out.PatchByte(0, 0x00);
    out.AppendFixed(static_cast<uint32_t>(len));
    out.Append(bytes, len);
}

/// Seal the window, score it, and judge it.
CopyVerdict Score(TracingWindow& window, const std::vector<uint8_t>& payload, CopyLedger& ledger) {
    const PublishTrace trace = window.Seal();
    EXPECT_EQ(trace.fault, "");
    EXPECT_EQ(ScoreProducedFromSource(trace, payload.data(), payload.size(), ledger), "");
    return Judge(ledger);
}

TEST(TracingWindow, APayloadAppendedFromTheCallersBufferIsNotACopy) {
    std::array<uint8_t, kSlot> slot{};
    TracingWindow window(slot.data(), slot.size());
    const std::vector<uint8_t> payload = Payload(40);

    EncodeBinaryRow(window, payload.data(), payload.size());

    CopyLedger ledger;
    const CopyVerdict verdict = Score(window, payload, ledger);
    EXPECT_EQ(ledger.produced_at, At(payload.data()));
    EXPECT_TRUE(ledger.produced_in_window);
    ASSERT_TRUE(verdict.encode_copies.has_value());
    EXPECT_EQ(*verdict.encode_copies, 0u);
    EXPECT_EQ(ledger.encode_base, At(slot.data()));
    EXPECT_EQ(ledger.encode_len, 1u + 4u + payload.size());
    EXPECT_EQ(verdict.refill_moves, 0u);
}

/// The copied-export control's shape: the same encode, reading a second copy.
TEST(TracingWindow, APayloadAppendedFromACopyIsACopyAndNamesTheCopy) {
    std::array<uint8_t, kSlot> slot{};
    TracingWindow window(slot.data(), slot.size());
    const std::vector<uint8_t> payload = Payload(40);
    const std::vector<uint8_t> copied = payload;

    EncodeBinaryRow(window, copied.data(), copied.size());

    CopyLedger ledger;
    const CopyVerdict verdict = Score(window, payload, ledger);
    EXPECT_EQ(ledger.produced_at, At(copied.data()));
    EXPECT_FALSE(ledger.produced_in_window);
    EXPECT_EQ(verdict.encode_copies.value(), 1u);
}

/// The staging control's shape: the row composed elsewhere, then lent in whole.
TEST(TracingWindow, APayloadThatArrivesThroughALentSpanIsACopy) {
    std::array<uint8_t, kSlot> slot{};
    TracingWindow window(slot.data(), slot.size());
    const std::vector<uint8_t> payload = Payload(40);

    std::array<uint8_t, kSlot> staged{};
    FixedWriteBuffer staging(staged.data(), staged.size());
    EncodeBinaryRow(staging, payload.data(), payload.size());
    const size_t row = staging.Position();
    window.AppendInPlace(row, [&](uint8_t* dst, size_t) {
        std::memcpy(dst, staged.data(), row);
        return row;
    });

    CopyLedger ledger;
    const CopyVerdict verdict = Score(window, payload, ledger);
    EXPECT_FALSE(ledger.produced_in_window);
    // No source to name, so the address is where it landed in the window.
    EXPECT_EQ(ledger.produced_at, At(slot.data() + 5));
    EXPECT_EQ(verdict.encode_copies.value(), 1u);
}

/// A patched bitfield is still sourceless zeros, and does not disturb the
/// payload's verdict: provenance is per record, not per row.
TEST(TracingWindow, RecordsTileTheWindowInOrder) {
    std::array<uint8_t, kSlot> slot{};
    TracingWindow window(slot.data(), slot.size());
    const std::vector<uint8_t> payload = Payload(12);

    EncodeBinaryRow(window, payload.data(), payload.size());
    const PublishTrace trace = window.Seal();

    ASSERT_EQ(trace.fault, "");
    ASSERT_EQ(trace.writes.size(), 3u);
    EXPECT_EQ(trace.writes[0].source, 0u);
    EXPECT_EQ(trace.writes[0].len, 1u);
    EXPECT_NE(trace.writes[1].source, 0u);
    EXPECT_EQ(trace.writes[1].offset, 1u);
    EXPECT_EQ(trace.writes[1].len, 4u);
    EXPECT_EQ(trace.writes[2].source, At(payload.data()));
    EXPECT_EQ(trace.writes[2].offset, 5u);
    EXPECT_EQ(trace.len, 17u);
}

/// The blind spot the header names, pinned: a writer that used less than it was
/// lent leaves room an inline append fills unseen. The bytes are folded into the
/// sourceless record - so the verdict errs towards a copy, never away from one.
TEST(TracingWindow, AnAppendIntoLeftoverLentRoomReadsAsSourceless) {
    std::array<uint8_t, kSlot> slot{};
    TracingWindow window(slot.data(), slot.size());
    const std::vector<uint8_t> payload = Payload(8);

    window.AppendInPlace(16, [](uint8_t* dst, size_t) {
        std::memset(dst, 0xAB, 4);
        return size_t{4};
    });
    window.Append(payload.data(), payload.size());  // fits the leftover 12: inline, unseen

    CopyLedger ledger;
    const CopyVerdict verdict = Score(window, payload, ledger);
    EXPECT_FALSE(ledger.produced_in_window);
    EXPECT_EQ(verdict.encode_copies.value(), 1u);
}

TEST(TracingWindow, AnAppendAfterALentSpanIsSeenAgain) {
    std::array<uint8_t, kSlot> slot{};
    TracingWindow window(slot.data(), slot.size());
    const std::vector<uint8_t> payload = Payload(8);

    window.AppendInPlace(4, [](uint8_t* dst, size_t) {
        std::memset(dst, 0xAB, 4);
        return size_t{4};
    });
    window.Append(payload.data(), payload.size());

    CopyLedger ledger;
    EXPECT_EQ(Score(window, payload, ledger).encode_copies.value(), 0u);
}

/// Stands in for a future `WriteBuffer` member that writes inline without
/// consulting the capacity - the one change that would blind this window. No
/// member can do that today, so it is done by hand.
class BypassedWindow : public TracingWindow {
   public:
    using TracingWindow::TracingWindow;

    void WriteUnseen(size_t len) {
        std::memset(data_ + pos_, 0x42, len);
        pos_ += len;
    }

    void Move(uint8_t* elsewhere) { data_ = elsewhere; }
};

/// D-BIND-61's loud-failure rule: bytes no slow path saw FAULT the trace, and a
/// faulted trace leaves the producer half unsampled - never a zero-copy verdict.
TEST(TracingWindow, BytesWrittenPastTheRecordsFaultTheTrace) {
    std::array<uint8_t, kSlot> slot{};
    BypassedWindow window(slot.data(), slot.size());
    const std::vector<uint8_t> payload = Payload(8);
    window.Append(payload.data(), payload.size());
    window.WriteUnseen(4);

    const PublishTrace trace = window.Seal();
    EXPECT_NE(trace.fault, "");
    CopyLedger ledger;
    EXPECT_EQ(ScoreProducedFromSource(trace, payload.data(), payload.size(), ledger), trace.fault);
    EXPECT_FALSE(Judge(ledger).encode_copies.has_value());
}

/// The same, before any write was recorded at all - the case with no record to
/// compare against.
TEST(TracingWindow, BytesWrittenBeforeAnyRecordFaultTheTrace) {
    std::array<uint8_t, kSlot> slot{};
    BypassedWindow window(slot.data(), slot.size());
    window.WriteUnseen(4);
    EXPECT_NE(window.Seal().fault, "");
}

/// ...and an unseen write followed by a seen one: the fault sticks, so a later
/// record cannot paper over the hole.
TEST(TracingWindow, AFaultSurvivesTheWritesAfterIt) {
    std::array<uint8_t, kSlot> slot{};
    BypassedWindow window(slot.data(), slot.size());
    const std::vector<uint8_t> payload = Payload(8);
    window.WriteUnseen(4);
    window.Append(payload.data(), payload.size());
    EXPECT_NE(window.Seal().fault, "");
}

TEST(TracingWindow, AWindowThatMovedOffItsSlotFaults) {
    std::array<uint8_t, kSlot> slot{};
    std::array<uint8_t, kSlot> other{};
    BypassedWindow window(slot.data(), slot.size());
    window.Move(other.data());
    EXPECT_NE(window.Seal().fault, "");
}

TEST(TracingWindow, ARowPastTheSlotThrows) {
    std::array<uint8_t, 8> slot{};
    TracingWindow window(slot.data(), slot.size());
    const std::vector<uint8_t> payload = Payload(16);
    EXPECT_THROW(window.Append(payload.data(), payload.size()), std::overflow_error);
}

/// A payload the row does not hold is unscorable, and the producer half stays
/// unsampled - so `encode_copies` is EMPTY, never a manufactured number.
TEST(TracingWindow, APayloadNotInTheRowLeavesTheLegUnsampled) {
    std::array<uint8_t, kSlot> slot{};
    TracingWindow window(slot.data(), slot.size());
    const std::vector<uint8_t> payload = Payload(8);
    const std::vector<uint8_t> other(8, 0x5A);
    EncodeBinaryRow(window, other.data(), other.size());

    CopyLedger ledger;
    EXPECT_NE(ScoreProducedFromSource(window.Seal(), payload.data(), payload.size(), ledger), "");
    EXPECT_EQ(ledger.produced_at, 0u);
    EXPECT_FALSE(Judge(ledger).encode_copies.has_value());
}

/// The provider end to end: the delivered row IS the traced window, and the
/// trace is the one `Publish` just made.
TEST(SeamProbeProviderTracing, DeliversTheTracedWindowItself) {
    auto provider = std::make_shared<SeamProbeProvider>(ProbeMode::kTracing);
    const std::vector<uint8_t> payload = Payload(40);
    const std::vector<std::string> topic = {"probe", "trace"};

    CopyLedger ledger;
    (void)provider->Subscribe(
        topic, [&](const uint8_t* data, size_t len, const SharedSchema&, const Attachments&) {
            ++ledger.deliveries;
            ledger.delivered_data = At(data);
            ledger.delivered_len = len;
        });
    provider->Publish(
        topic, [&](WriteBuffer& out) { EncodeBinaryRow(out, payload.data(), payload.size()); },
        Attachments{});

    ASSERT_EQ(ledger.deliveries, 1u);
    ASSERT_EQ(
        ScoreProducedFromSource(provider->LastTrace(), payload.data(), payload.size(), ledger), "");
    const CopyVerdict verdict = Judge(ledger);
    EXPECT_EQ(verdict.row_copies, 0u);
    EXPECT_EQ(verdict.encode_copies.value(), 0u);
}

}  // namespace
}  // namespace fletcher::copy_probe
