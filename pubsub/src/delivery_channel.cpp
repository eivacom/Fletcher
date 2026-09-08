// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include "fletcher/pubsub/delivery_channel.hpp"

#include <fletcher/core/internal/delivery_frame.hpp>
#include <fletcher/core/status.hpp>
#include <utility>

namespace fletcher {
namespace {

// Every absorption in this BINARY -- a function-local static, so one instance per
// linked module, one per process only because the tree links everything into one.
// Same scope as delivery_frame.hpp's P1, and its STOP-AND-ASK covers this counter
// too. Not a diagnostic and not a ledger: one
// integer, so a conformance clause that can only see a ProviderSubject can still
// assert that a throw was ABSORBED rather than merely unobserved.
std::atomic<uint64_t>& AbsorbedTotalCounter() noexcept {
    static std::atomic<uint64_t> total{0};
    return total;
}

}  // namespace

DeliveryChannel::DeliveryChannel(const PubSubProvider* provider,
                                 PubSubProvider::SubscribeCallback callback)
    : DeliveryChannel(RawToken{}, static_cast<const void*>(provider), std::move(callback)) {}

DeliveryChannel::DeliveryChannel(RawToken, const void* provider_token,
                                 PubSubProvider::SubscribeCallback callback)
    : callback_(std::move(callback)),
      provider_token_(provider_token),
      absorbed_(std::make_shared<std::atomic<uint64_t>>(0)) {
    // A null token would be indistinguishable from a default-constructed
    // channel's, so a door asking about the provider would match a frame this
    // channel pushed for nobody. No in-tree caller does it; refused rather than
    // documented, because the failure it would cause is a wrong ANSWER from a
    // door, not a crash anyone would trace back to here.
    if (provider_token == nullptr) {
        throw PubSubError(PubSubStatus::kInvalidArgument,
                          "DeliveryChannel: provider token must not be null");
    }
}

void DeliveryChannel::Deliver(const uint8_t* data, size_t len, const SharedSchema& schema,
                              const Attachments& attachments) const noexcept {
    if (!callback_) return;
    try {
        // The frame scope is closed INSIDE the try so that the callback's own
        // exception is absorbed with the frame already popped — the sweep must
        // not run with this delivery still on the stack.
        //
        // It does NOT protect the sweep. `~DeliveryScope` is implicitly
        // noexcept, so a throw from the deferred work terminates at the
        // destructor boundary and never reaches this catch; the containment for
        // that lives inside the destructor itself, per item (see
        // core/.../delivery_frame.hpp). Stated because the opposite was written
        // here first and is easy to believe.
        {
            internal::DeliveryScope frame(provider_token_);
            callback_(data, len, schema, attachments);
        }
    } catch (...) {
        // Spec §5.3, owner ruling 2026-09-05: the failure is contained and
        // reported where it happened. It is not the publisher's failure and the
        // publisher cannot act on it, so nothing propagates — which is also what
        // makes "a subscriber's exception reaching a publisher's status"
        // unrepresentable rather than merely discriminated: no callback frame
        // reaches a translator at all.
        if (absorbed_) absorbed_->fetch_add(1, std::memory_order_relaxed);
        AbsorbedTotalCounter().fetch_add(1, std::memory_order_relaxed);
    }
}

uint64_t DeliveryChannel::AbsorbedCount() const noexcept {
    return absorbed_ ? absorbed_->load(std::memory_order_relaxed) : 0;
}

uint64_t DeliveryChannel::AbsorbedTotal() noexcept {
    return AbsorbedTotalCounter().load(std::memory_order_relaxed);
}

}  // namespace fletcher
