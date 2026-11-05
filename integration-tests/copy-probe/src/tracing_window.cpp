// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The source-tracing window and D-BIND-61's scoring rule. The argument is in
// the header.

#include "fletcher/copy_probe/tracing_window.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace fletcher {
namespace copy_probe {

TracingWindow::TracingWindow(uint8_t* slot, size_t slot_bytes)
    // Capacity 0 from the first byte: the window is full before anything is
    // written, so the very first append already reaches a slow path.
    : WriteBuffer(slot, 0), slot_(slot), slot_bytes_(slot_bytes) {}

void TracingWindow::Require(size_t len) const {
    if (len > slot_bytes_ - pos_) {
        throw std::overflow_error("TracingWindow: the row does not fit the probe's slot");
    }
}

void TracingWindow::Reconcile() {
    const size_t end = writes_.empty() ? 0 : writes_.back().offset + writes_.back().len;
    if (pos_ == end) return;
    // Only a refill for AppendInPlace restores `pos_` below what was recorded, and
    // only its writer's commit can move it back up - never past the room that
    // refill made, which is `capacity_`. Anything else means bytes arrived that no
    // slow path saw, and a trace missing them must not be scored.
    if (!fault_.empty()) return;
    if (writes_.empty() || writes_.back().source != 0 || pos_ < writes_.back().offset ||
        pos_ > end) {
        fault_ = "TracingWindow: the window's position (" + std::to_string(pos_) +
                 ") does not agree with its records (last ends at " + std::to_string(end) + ")";
        return;
    }
    TracedWrite& last = writes_.back();
    last.len = pos_ - last.offset;
    if (last.len == 0) writes_.pop_back();
    // Full again, so the next write is seen.
    capacity_ = pos_;
}

void TracingWindow::AppendSlow(const uint8_t* data, size_t len) {
    Reconcile();
    Require(len);
    std::memcpy(data_ + pos_, data, len);
    writes_.push_back(TracedWrite{At(data), pos_, len});
    pos_ += len;
    capacity_ = pos_;
}

void TracingWindow::AppendZerosSlow(size_t len) {
    Reconcile();
    Require(len);
    std::memset(data_ + pos_, 0, len);
    // Sourceless: a real AppendZeros, or the room AppendInPlace asks for before it
    // lends the span. The two are the same call on this side of the window.
    writes_.push_back(TracedWrite{0, pos_, len});
    pos_ += len;
    capacity_ = pos_;
}

PublishTrace TracingWindow::Seal() {
    Reconcile();
    PublishTrace trace;
    trace.base = At(data_);
    trace.len = pos_;
    trace.fault = fault_;
    if (trace.fault.empty() && data_ != slot_) {
        trace.fault = "TracingWindow: the window moved off its slot";
    }
    trace.writes = std::move(writes_);
    writes_.clear();
    return trace;
}

std::string ScoreProducedFromSource(const PublishTrace& trace, const uint8_t* expected,
                                    size_t expected_len, CopyLedger& ledger) {
    if (!trace.fault.empty()) return trace.fault;
    if (expected == nullptr || expected_len == 0) {
        return "ScoreProducedFromSource: the caller named no payload to trace";
    }
    const auto* window = reinterpret_cast<const uint8_t*>(trace.base);
    const uint8_t* const found =
        std::search(window, window + trace.len, expected, expected + expected_len);
    if (found == window + trace.len) {
        return "ScoreProducedFromSource: the payload is not in the published row";
    }
    const size_t at = static_cast<size_t>(found - window);

    ledger.encode_base = trace.base;
    ledger.encode_len = trace.len;
    ledger.produced_len = expected_len;

    for (const TracedWrite& write : trace.writes) {
        if (at < write.offset || at >= write.offset + write.len) continue;
        if (write.source != 0) {
            // The payload was copied in from somewhere. Only from the caller's own
            // buffer, as one run of exactly the payload, is that the encode itself.
            ledger.produced_at = write.source + (at - write.offset);
            ledger.produced_in_window =
                write.source == At(expected) && write.offset == at && write.len == expected_len;
        } else {
            // No source to name, so the only honest address is where it landed -
            // and nothing says it came from the caller's buffer.
            ledger.produced_at = trace.base + at;
            ledger.produced_in_window = false;
        }
        return {};
    }
    return "ScoreProducedFromSource: no record covers the payload";
}

}  // namespace copy_probe
}  // namespace fletcher
