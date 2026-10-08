// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The probe provider and its control variants, moved here from
// pubsub-conformance's `copy_accounting.cpp` by D-BIND-62. `kTracing` is new
// (D-BIND-61); the other three behave exactly as they did there.

#include "fletcher/copy_probe/seam_probe_provider.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace fletcher {
namespace copy_probe {

/// A fixed arena of slots. The slots are MEMBERS, so P5's liveness precondition
/// holds by construction rather than by argument. Slots rotate so the loaned
/// bytes and the row of one publish never share an address.
class Arena {
   public:
    static constexpr size_t kSlots = 4;

    uint8_t* NextSlot() {
        uint8_t* base = slots_[cursor_].data();
        cursor_ = (cursor_ + 1) % kSlots;
        return base;
    }

    /// Scribbles on the way out, so "the owner died and the bytes happen to still
    /// be there" is not a way to pass. Without this the ownership leg would rely
    /// on an allocator not reusing the block - which is luck, not a measurement.
    ~Arena() {
        for (auto& slot : slots_) slot.fill(0xDD);
    }

   private:
    std::array<std::array<uint8_t, SeamProbeProvider::kSlotBytes>, kSlots> slots_{};
    size_t cursor_ = 0;
};

namespace {

std::string JoinTopic(const std::vector<std::string>& topic) {
    std::string out;
    for (const std::string& segment : topic) {
        if (!out.empty()) out += '/';
        out += segment;
    }
    return out;
}

/// A growable window owned by THIS HARNESS. Every refill allocates the
/// replacement while the old block is still held, so relocation is
/// unconditional and observable (README, refill).
class GrowableProbeBuffer : public WriteBuffer {
   public:
    GrowableProbeBuffer() : WriteBuffer(nullptr, 0) {}

   private:
    static constexpr size_t kStep = 128;

    void AppendSlow(const uint8_t* data, size_t len) override {
        Grow(len);
        std::memcpy(data_ + pos_, data, len);
        pos_ += len;
    }

    void AppendZerosSlow(size_t len) override {
        Grow(len);
        std::memset(data_ + pos_, 0, len);
        pos_ += len;
    }

    void Grow(size_t len) {
        std::vector<uint8_t> next(pos_ + std::max<size_t>(len, kStep));
        if (pos_ > 0) std::memcpy(next.data(), buf_.data(), pos_);
        buf_ = std::move(next);
        data_ = buf_.data();
        capacity_ = buf_.size();
    }

    std::vector<uint8_t> buf_;
};

/// A schema-less transport passes null throughout (spec §7 clause 1); this
/// oracle measures bytes, not schemas.
const SharedSchema& NoSchema() {
    static const SharedSchema kNone{};
    return kNone;
}

}  // namespace

SeamProbeProvider::SeamProbeProvider(ProbeMode mode)
    : arena_(std::make_shared<Arena>()), mode_(mode) {}

SeamProbeProvider::~SeamProbeProvider() = default;

void SeamProbeProvider::CreateTopic(const std::vector<std::string>&, OwnedSchema) {}

void SeamProbeProvider::Publish(const std::vector<std::string>& topic_segments,
                                const RowEncoder& encoder, const Attachments& attachments) {
    SubscribeCallback* cb = Callback(topic_segments);

    if (mode_ == ProbeMode::kGrowable) {
        // The buffer outlives the callback, so P5 holds here too.
        GrowableProbeBuffer buffer;
        encoder(buffer);
        if (cb != nullptr) (*cb)(buffer.Data(), buffer.Position(), NoSchema(), attachments);
        return;
    }

    uint8_t* slot = arena_->NextSlot();
    size_t written = 0;
    if (mode_ == ProbeMode::kTracing) {
        TracingWindow buffer(slot, kSlotBytes);
        // Cleared first, so a publish that throws out of its encoder cannot leave
        // the PREVIOUS publish's trace behind to be scored as this one's.
        last_trace_ = PublishTrace{};
        encoder(buffer);
        last_trace_ = buffer.Seal();
        written = buffer.Position();
    } else {
        FixedWriteBuffer buffer(slot, kSlotBytes);
        encoder(buffer);
        written = buffer.Position();
    }
    if (cb == nullptr) return;

    if (mode_ != ProbeMode::kStaging && loan_len_ == 0) {
        (*cb)(slot, written, NoSchema(), attachments);
        return;
    }

    // The caller's set, copied SHALLOWLY: copying shared_ptrs moves no
    // payload byte, so it is not a copy under this oracle's definition (P3).
    Attachments delivered = attachments;
    if (loan_len_ > 0) {
        // Where §3.2 USED to bite, and now does not: `Blob` is an owner plus
        // a span, so bytes this provider already holds cross where they lie.
        // The owner is the arena itself - a real one, so the blob keeps
        // those bytes alive past the delivery exactly as a transport loan
        // handle would.
        delivered.Set(loan_key_, Blob(arena_, loan_base_, loan_len_));
    }

    if (mode_ != ProbeMode::kStaging) {
        (*cb)(slot, written, NoSchema(), delivered);
        return;
    }

    // The control's whole job: move every payload byte to a second address
    // while keeping the content identical, so `memcmp` cannot tell the
    // difference and only provenance can.
    const std::vector<uint8_t> staged(slot, slot + written);
    Attachments deep;
    for (size_t i = 0; i < delivered.size(); ++i) {
        const Blob& blob = delivered.ValueAt(i);
        deep.Set(std::string(delivered.KeyAt(i)),
                 blob.empty() ? blob
                              : Blob(std::vector<uint8_t>(blob.data(), blob.data() + blob.size())));
    }
    (*cb)(staged.data(), staged.size(), NoSchema(), deep);
}

SubscriptionResult SeamProbeProvider::Subscribe(const std::vector<std::string>& topic_segments,
                                                SubscribeCallback callback) {
    callbacks_[JoinTopic(topic_segments)] = std::move(callback);
    // Schema-less by construction (§7 clause 1): kOk with a null schema.
    return {SchemaArrival::Ready(nullptr)};
}

void SeamProbeProvider::Unsubscribe(const std::vector<std::string>& topic_segments) {
    callbacks_.erase(JoinTopic(topic_segments));
}

const uint8_t* SeamProbeProvider::LoanForDelivery(std::string key,
                                                  const std::vector<uint8_t>& payload) {
    if (payload.size() > kSlotBytes) {
        throw std::overflow_error("SeamProbeProvider::LoanForDelivery: slot overflow");
    }
    uint8_t* base = arena_->NextSlot();
    std::memcpy(base, payload.data(), payload.size());
    loan_key_ = std::move(key);
    loan_base_ = base;
    loan_len_ = payload.size();
    return base;
}

SeamProbeProvider::SubscribeCallback* SeamProbeProvider::Callback(
    const std::vector<std::string>& topic_segments) {
    auto it = callbacks_.find(JoinTopic(topic_segments));
    if (it == callbacks_.end() || !it->second) return nullptr;
    return &it->second;
}

}  // namespace copy_probe
}  // namespace fletcher
