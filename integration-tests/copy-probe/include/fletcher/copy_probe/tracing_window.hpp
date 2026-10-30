// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The SOURCE-TRACING window (D-BIND-61): an encode window that sees where every
// byte written into it came from.
//
// ── Why it exists ───────────────────────────────────────────────────────────
// `encode_copies` asks whether the producer wrote the row straight into the
// window. The ledger learns that from the PRODUCER, which reports where it
// composed the row. A binding's fused publish has no such producer: the codec
// encodes field by field inside the shim, so a correct publish and one that
// encoded into managed memory first and pushed the bytes through a raw publish
// are indistinguishable by every number the ledger had.
//
// What does distinguish them is where the window's payload bytes came FROM. The
// codec writes a variable-length field with `Append(data, len)`, `data` pointing
// into the Arrow buffer the caller's export shares; a staged row enters as one
// `AppendInPlace` fill whose contents the window never sees the source of; an
// export that copied its buffers appends from a second address.
//
// ── How it sees ─────────────────────────────────────────────────────────────
// `WriteBuffer::Append` is inline and reaches the virtual `AppendSlow(src, len)`
// only when the window is full. This window is kept EXACTLY full between writes -
// `capacity == pos`, over a fixed slot that never moves - so every `Append`,
// `AppendByte` and `AppendFixed` arrives at `AppendSlow` WITH ITS SOURCE, and
// every `AppendZeros` at `AppendZerosSlow`. Nothing relocates, so no refill is
// ever counted and every byte stays where it was written.
//
// ── What it cannot see, and which way that fails ────────────────────────────
// `AppendInPlace` makes room through `AppendZerosSlow` - the same call a real
// `AppendZeros` makes - and then lends the span. Both are recorded as SOURCELESS
// bytes: the window can tell neither apart nor what the lent span was filled
// from. A lent span the writer used only part of is truncated to what it
// committed, and a write that then landed in the leftover room took the inline
// path unseen and is folded into the same sourceless record. Every blind spot
// therefore reads as "no source", which scores as a copy: this window can report
// a copy that did not happen, never hide one that did. A zero-copy verdict needs
// a RECORDED `Append` from the caller's own address.
#ifndef FLETCHER_COPY_PROBE_TRACING_WINDOW_HPP_
#define FLETCHER_COPY_PROBE_TRACING_WINDOW_HPP_

#include <cstddef>
#include <cstdint>
#include <fletcher/core/write_buffer.hpp>
#include <string>
#include <vector>

#include "fletcher/copy_probe/ledger.hpp"

namespace fletcher {
namespace copy_probe {

/// One run of bytes written into the window.
struct TracedWrite {
    /// `source != 0`: copied in by `Append` from that address. `source == 0`:
    /// SOURCELESS - zeros, or a span lent to an in-place writer.
    Address source = 0;
    size_t offset = 0;
    size_t len = 0;
};

/// Everything one encode left in the window.
struct PublishTrace {
    Address base = 0;
    size_t len = 0;
    /// Tile `[0, len)` exactly, in order. By construction: every record is pushed
    /// at the position the previous one ended, after `Reconcile` has checked that
    /// the window agrees.
    std::vector<TracedWrite> writes;
    /// Non-empty when the window's position disagreed with its records - bytes
    /// arrived that no slow path saw - or the window moved off its slot. A faulted
    /// trace is never scored, so a write this window did not see makes a leg RED
    /// rather than vacuously green (D-BIND-61).
    std::string fault;
};

/// Not `final`, for one reason: the only way to exercise the fault path is to
/// write past the window's back, which no `WriteBuffer` member can do today - so
/// the package's tests derive from it and do it by hand, standing in for the
/// future inline append the guard exists for.
class TracingWindow : public WriteBuffer {
   public:
    /// Writes into `slot`, which must outlive this window and every delivery of
    /// its bytes (P5). Throws `std::overflow_error` past `slot_bytes`, as
    /// `FixedWriteBuffer` does, so an undersized slot fails loudly.
    TracingWindow(uint8_t* slot, size_t slot_bytes);

    /// Reconcile the last record with what was committed, check that the records
    /// tile the window, and hand the trace over. Call once, after the encoder
    /// returned.
    PublishTrace Seal();

   private:
    void AppendSlow(const uint8_t* data, size_t len) override;
    void AppendZerosSlow(size_t len) override;

    /// A lent span commits `pos0 + used` after the refill recorded `min_bytes`,
    /// so the last record can end past `pos_`; it is truncated here, before the
    /// next record or the seal. Any OTHER disagreement between `pos_` and the
    /// records - above all `pos_` past their end, bytes no slow path saw - faults.
    void Reconcile();
    /// Room for `len` more bytes at `pos_`, or a throw.
    void Require(size_t len) const;

    uint8_t* const slot_;
    const size_t slot_bytes_;
    std::vector<TracedWrite> writes_;
    std::string fault_;
};

/// D-BIND-61's scoring rule, filling the PRODUCER half of `ledger` from a trace.
///
/// The caller names where its payload lives - `expected` and `expected_len`, the
/// bytes of the one variable-length field in its OWN buffer, still live. The
/// payload is found in the window by content and the record that wrote it
/// decides:
///   * one recorded `Append` from `expected`, exactly the payload -> in window;
///   * an `Append` from anywhere else -> `produced_at` is that source;
///   * sourceless bytes -> `produced_at` is where the payload sits in the window,
///     with nothing to say it came from the caller's buffer.
/// Also fills `encode_base`/`encode_len`. Returns a non-empty reason when the
/// trace cannot be scored (a fault, or the payload is not in the row); the
/// ledger's producer half is then left unsampled, so the leg fails as itself.
std::string ScoreProducedFromSource(const PublishTrace& trace, const uint8_t* expected,
                                    size_t expected_len, CopyLedger& ledger);

}  // namespace copy_probe
}  // namespace fletcher

#endif  // FLETCHER_COPY_PROBE_TRACING_WINDOW_HPP_
