// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// A human-readable name for a PubSubStatus — for diagnostics only.
//
// INTERNAL, and the reason is worth stating: **the NUMBER is the contract.**
// A name function has no product caller and never should have one — nothing in
// Fletcher branches on it, and a C boundary carries the integer, not this
// string. Its only users are tests and harnesses, which want a readable failure
// message instead of a four-byte hex dump. Putting it here says so, and keeps it
// out of the surface two later rounds derive their C types from.
//
// Cross-package by design, following pubsub/…/internal/segments.hpp: an internal
// header a sibling package may include is still not public API.
#ifndef FLETCHER_INCLUDE_CORE_INTERNAL_STATUS_NAME_HPP_
#define FLETCHER_INCLUDE_CORE_INTERNAL_STATUS_NAME_HPP_

#include "fletcher/core/status.hpp"

namespace fletcher {
namespace internal {

[[nodiscard]] inline const char* PubSubStatusName(PubSubStatus status) noexcept {
    switch (status) {
        case PubSubStatus::kOk:
            return "ok";
        case PubSubStatus::kInvalidArgument:
            return "invalid_argument";
        case PubSubStatus::kSchemaConflict:
            return "schema_conflict";
        case PubSubStatus::kTopicNotDeclared:
            return "topic_not_declared";
        case PubSubStatus::kPayloadTooLarge:
            return "payload_too_large";
        case PubSubStatus::kTransportFailure:
            return "transport_failure";
        case PubSubStatus::kNotSupported:
            return "not_supported";
        case PubSubStatus::kInternal:
            return "internal";
        case PubSubStatus::kPending:
            return "pending";
        case PubSubStatus::kSubscriptionEnded:
            return "subscription_ended";
    }
    return "internal";
}

}  // namespace internal
}  // namespace fletcher

#endif  // FLETCHER_INCLUDE_CORE_INTERNAL_STATUS_NAME_HPP_
