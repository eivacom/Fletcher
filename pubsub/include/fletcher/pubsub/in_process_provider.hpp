// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#ifndef FLETCHER_INCLUDE_PUBSUB_IN_PROCESS_PROVIDER_HPP_
#define FLETCHER_INCLUDE_PUBSUB_IN_PROCESS_PROVIDER_HPP_

#include <memory>
#include <string>
#include <vector>

#include "fletcher/pubsub/provider.hpp"
#include "fletcher/pubsub/provider_registry.hpp"

namespace fletcher {

/// Make the loopback selectable as `"inprocess"` (spec §4 clause 4).
///
/// Idempotence is NOT offered: a second call is refused by
/// `ProviderRegistry::Register` (`kInvalidArgument`) — a registry means one
/// transport per name for its whole life, and this registration is no
/// exception.
void RegisterInProcessProvider(ProviderRegistry& registry);

/// In-process loopback transport: a publish is delivered synchronously to a
/// subscriber on the SAME provider instance, and nowhere else.
///
/// Lifted out of the gateway, which is where it used to live as a private class,
/// so that the conformance suite and the provider registry can reach it. It is
/// the reference implementation of the delivery contract in `provider.hpp`, in
/// either of §7 clause 1's two schema modes.
///
/// The mode is chosen by the single `schema_carriage` key in `config.document`
/// (`as_declared`, the default — today's gateway behaviour — or `carried`,
/// schema-before-data). It is the ONLY route: there is no second constructor
/// and no accessor, so an object with an undecided mode never exists and a
/// caller cannot pick the mode any other way (spec §4.1, §4.2, locked
/// decision 8). An unrecognised document entry — unknown key, unknown value,
/// an entry with no `=`, or a duplicate key — is refused in the constructor
/// with `PubSubError(kInvalidArgument)`, quoting the offending entry. The
/// typed core (`max_payload_bytes`, `domain_id`) is ignored, exactly as today's
/// gateway documents `--domain-id` as ignored by this provider.
///
/// Topics are created on first subscribe or publish, so no pre-registration is
/// needed in the default mode. A publish to a topic nobody subscribed to is
/// dropped: there is no retention, so a subscriber that joins after a publish
/// never sees it.
///
/// Threading: one delivery at a time, dispatched under the instance mutex, so a
/// callback must not re-enter the provider (the mutex is non-recursive, so one
/// that does deadlocks rather than corrupting state).
class InProcessPubSubProvider : public PubSubProvider {
   public:
    explicit InProcessPubSubProvider(const ProviderConfig& config = {});
    ~InProcessPubSubProvider() override;

    InProcessPubSubProvider(const InProcessPubSubProvider&) = delete;
    InProcessPubSubProvider& operator=(const InProcessPubSubProvider&) = delete;

    /// Declares the topic and caches the schema, which subscribers that
    /// subscribe AFTERWARDS get back through `SubscriptionResult` and on every
    /// delivery. Re-declaring with an identical schema is idempotent; a
    /// conflicting one throws `PubSubError(kSchemaConflict)` (spec §7 clause 3).
    /// In `kCarried` a declaration with no schema is refused
    /// (`kInvalidArgument`): a schema-carrying transport has nothing to carry.
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
