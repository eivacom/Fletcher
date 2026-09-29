// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#ifndef FLETCHER_INCLUDE_PUBSUB_INTERNAL_SCHEMA_CHECK_HPP_
#define FLETCHER_INCLUDE_PUBSUB_INTERNAL_SCHEMA_CHECK_HPP_

#include <fletcher/core/internal/delivery_frame.hpp>

#include "fletcher/pubsub/provider.hpp"

namespace fletcher {
namespace internal {

/// Runs a subscription's schema check the way every provider must: inside a delivery frame for
/// `provider_token`, so a seam call from inside the check is refused kReentrantCall at the door;
/// never for a null schema (a schema-less transport has nothing to check); and with a throw
/// absorbed as `false`. `provider_token` is the same value the provider's doors hand
/// `RefuseIfInsideDeliveryOn`: a provider passes `static_cast<const PubSubProvider*>(this)`, and a
/// test hook with no provider object passes whatever token its `DeliveryChannel::RawToken` carries.
[[nodiscard]] inline bool RunSchemaCheck(const void* provider_token,
                                         const PubSubProvider::SchemaCheck& check,
                                         const SharedSchema& announced) noexcept {
    if (!announced) return true;
    DeliveryScope frame(provider_token);
    try {
        return check(announced);
    } catch (...) {
        return false;
    }
}

}  // namespace internal
}  // namespace fletcher

#endif  // FLETCHER_INCLUDE_PUBSUB_INTERNAL_SCHEMA_CHECK_HPP_
