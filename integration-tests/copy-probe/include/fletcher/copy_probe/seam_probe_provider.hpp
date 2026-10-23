// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The instrumented provider the copy-accounting ledger is built around. Moved
// here from pubsub-conformance's `copy_accounting.cpp`, where it was private to
// one translation unit, by D-BIND-62 - so that the binding shim's probe variant
// (`fletcher-c-abi-probe`) registers THIS class rather than a copy of it.
#ifndef FLETCHER_COPY_PROBE_SEAM_PROBE_PROVIDER_HPP_
#define FLETCHER_COPY_PROBE_SEAM_PROBE_PROVIDER_HPP_

#include <cstddef>
#include <cstdint>
#include <fletcher/pubsub/provider.hpp>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "fletcher/copy_probe/tracing_window.hpp"

namespace fletcher {
namespace copy_probe {

class Arena;

/// One class, four behaviours, so no control can drift from the thing it
/// controls: `kZeroCopy` delivers the arena slot itself (positive control - a
/// red there means the measurement is wrong), `kStaging` stages the row and
/// deep-copies every blob (negative control), `kGrowable` encodes into a
/// harness-owned growable window (refill control), and `kTracing` encodes into a
/// `TracingWindow` over the slot and delivers the slot itself, recording where
/// every byte came from (D-BIND-61).
enum class ProbeMode { kZeroCopy, kStaging, kGrowable, kTracing };

class SeamProbeProvider : public PubSubProvider {
   public:
    /// Slot size, and so the largest row the probe takes: above the oracle's
    /// 4 KiB large row, and a throw rather than a truncation beyond it.
    static constexpr size_t kSlotBytes = 8192;

    explicit SeamProbeProvider(ProbeMode mode);
    ~SeamProbeProvider() override;

    void CreateTopic(const std::vector<std::string>& topic_segments, OwnedSchema schema) override;
    void Publish(const std::vector<std::string>& topic_segments, const RowEncoder& encoder,
                 const Attachments& attachments) override;
    [[nodiscard]] SubscriptionResult Subscribe(const std::vector<std::string>& topic_segments,
                                               SubscribeCallback callback) override;
    void Unsubscribe(const std::vector<std::string>& topic_segments) override;

    /// Park `payload` in an arena slot - memory this provider owns and a real
    /// transport would have been LOANED - and make every later `Publish`
    /// deliver those bytes under `key`. Returns the arena base: the address the
    /// delivered blob would carry if the seam could carry borrowed memory.
    const uint8_t* LoanForDelivery(std::string key, const std::vector<uint8_t>& payload);

    /// What `LoanForDelivery` parked, for a scorer that did not call it itself
    /// (the probe shim's `fl_test_probe_score`). Null and 0 before any loan.
    const std::string& LoanedKey() const { return loan_key_; }
    const uint8_t* LoanedBase() const { return loan_base_; }
    size_t LoanedLen() const { return loan_len_; }

    /// `kTracing` only: what the last `Publish` left in the window. Its bytes
    /// live in the arena, which this provider owns, so they stay readable until
    /// the slot is reused (`kSlots` publishes later) or the provider dies.
    const PublishTrace& LastTrace() const { return last_trace_; }

   private:
    SubscribeCallback* Callback(const std::vector<std::string>& topic_segments);

    // Held by shared_ptr because a Blob handed over from it must be able to OWN
    // it: that is the whole §3.2 contract a transport loan has to satisfy, and
    // the probe has to satisfy it too or it is not standing in for one.
    std::shared_ptr<Arena> arena_;
    ProbeMode mode_;
    std::unordered_map<std::string, SubscribeCallback> callbacks_;
    std::string loan_key_;
    const uint8_t* loan_base_ = nullptr;
    size_t loan_len_ = 0;
    PublishTrace last_trace_;
};

}  // namespace copy_probe
}  // namespace fletcher

#endif  // FLETCHER_COPY_PROBE_SEAM_PROBE_PROVIDER_HPP_
