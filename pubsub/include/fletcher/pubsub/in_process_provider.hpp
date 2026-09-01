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
/// `provider.hpp`, in either of two schema modes chosen at construction (see
/// SchemaCarriage).
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
    /// Which of §7 clause 1's two schema modes this instance is in. Fixed at
    /// construction, so "never mix the two" is a property of the object.
    ///
    /// Named SchemaCarriage rather than SchemaMode because the conformance
    /// harness already has a `SchemaMode` and would shadow this one where it
    /// matters most — in the subject registration that chooses between them.
    ///
    /// Forward note for PDA-DEC-5: this must eventually arrive through §4.1's
    /// typed-core-plus-opaque-document, NOT as a second construction API, or
    /// decision 3's "one creation signature" is breached by this argument.
    enum class SchemaCarriage {
        /// **What the loopback has always done, and the default** — so the
        /// gateway is unchanged and keeps sending `schemaIpc`. The topic carries
        /// whatever schema a publisher declared **on this instance**, and null
        /// for a topic nobody declared.
        ///
        /// A subscription's answer is fixed when `Subscribe` returns: a
        /// declaration made afterwards never reaches it, and a later subscriber
        /// gets the new one. Without that a live subscription silently flipped
        /// from null to non-null mid-stream, which §7 clause 1 forbids and whose
        /// failure mode is a client decoding one stream two ways with no signal.
        kAsDeclared,
        /// Schema-before-data, the mode a schema-carrying transport is in. Every
        /// delivery carries a non-null schema, and the guarantee is upheld by
        /// **refusal rather than by buffering**: `CreateTopic` requires a real
        /// schema and `Publish` to an undeclared topic is kTopicNotDeclared. A
        /// single-instance loopback delivers synchronously under one lock, so
        /// there is no window in which a sample can arrive ahead of the schema
        /// for a subscription to buffer.
        kCarried,
    };

    explicit InProcessPubSubProvider(SchemaCarriage carriage = SchemaCarriage::kAsDeclared);
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
