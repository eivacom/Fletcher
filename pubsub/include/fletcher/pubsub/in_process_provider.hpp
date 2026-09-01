// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#ifndef FLETCHER_INCLUDE_PUBSUB_IN_PROCESS_PROVIDER_HPP_
#define FLETCHER_INCLUDE_PUBSUB_IN_PROCESS_PROVIDER_HPP_

#include <memory>
#include <string>
#include <vector>

#include "fletcher/pubsub/provider.hpp"

namespace fletcher {

/// In-process loopback transport: a publish is delivered synchronously to a
/// subscriber on the SAME provider instance, and nowhere else.
///
/// Lifted out of the gateway, which is where it used to live as a private class,
/// so that the conformance suite — and later the provider registry — can reach
/// it. It is the reference implementation of the delivery contract in
/// `provider.hpp` for a transport that carries no schemas of its own: unless a
/// publisher declared one through `CreateTopic`, the callback's schema is null
/// throughout, which §7 clause 1 of docs/pubsub-interface-spec.md explicitly
/// sanctions (the gateway's clients bring their own schema).
///
/// Topics are created on first subscribe or publish, so no pre-registration is
/// needed. A publish to a topic nobody subscribed to is dropped: there is no
/// retention, so a subscriber that joins after a publish never sees it.
///
/// Threading: one delivery at a time, dispatched under the instance mutex, so a
/// callback must not re-enter the provider (the mutex is non-recursive, so one
/// that does deadlocks rather than corrupting state).
class InProcessPubSubProvider : public PubSubProvider {
   public:
    InProcessPubSubProvider();
    ~InProcessPubSubProvider() override;

    InProcessPubSubProvider(const InProcessPubSubProvider&) = delete;
    InProcessPubSubProvider& operator=(const InProcessPubSubProvider&) = delete;

    /// Declares the topic and caches the schema, which subscribers get back
    /// through `SubscriptionResult` and on every delivery. Re-declaring with an
    /// identical schema is idempotent; a conflicting one throws
    /// `std::runtime_error` (spec §7 clause 3).
    void CreateTopic(const std::vector<std::string>& topic_segments, OwnedSchema schema) override;

    void Publish(const std::vector<std::string>& topic_segments, const RowEncoder& encoder,
                 const Attachments& attachments = {}) override;

    // [[nodiscard]] is NOT inherited from the PubSubProvider base declaration and the diagnostic
    // keys off the STATIC type at the call site, so the annotation must be repeated on every
    // concrete override or it never fires where applications actually call (#56).
    [[nodiscard]] SubscriptionResult Subscribe(const std::vector<std::string>& topic_segments,
                                               SubscribeCallback callback) override;

    void Unsubscribe(const std::vector<std::string>& topic_segments) override;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace fletcher

#endif  // FLETCHER_INCLUDE_PUBSUB_IN_PROCESS_PROVIDER_HPP_
