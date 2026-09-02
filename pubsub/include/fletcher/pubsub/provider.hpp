// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#ifndef FLETCHER_INCLUDE_PUBSUB_PROVIDER_HPP_
#define FLETCHER_INCLUDE_PUBSUB_PROVIDER_HPP_

#include <cstdint>
#include <fletcher/core/status.hpp>
#include <fletcher/core/types.hpp>
#include <fletcher/core/write_buffer.hpp>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "fletcher/pubsub/owned_schema.hpp"
#include "fletcher/pubsub/schema_arrival.hpp"

namespace fletcher {

/// Result returned by PubSubProvider::Subscribe.
///
/// `schema` is a waitable handle for the topic's schema. Subscribe never blocks
/// to obtain it: the arrival is answered once the schema is known — immediately
/// for providers that already hold it, or asynchronously when a publisher
/// announces it (late-joining subscribers).
///
/// What it answers WITH follows the transport's schema mode, and the two are
/// never mixed within one subscription (§7 clause 1 of
/// docs/pubsub-interface-spec.md):
///  - a schema-**carrying** transport answers kOk with a non-null SharedSchema;
///  - a transport that carries no schemas — the in-process loopback in its
///    default mode, where the client brings its own — answers kOk with **null**.
///
/// A subscription torn down before either happens is answered
/// kSubscriptionEnded, so a consumer that waits never hangs and never mistakes
/// "this subscription is over" for "this transport has no schemas". See
/// SchemaArrival for the whole outcome set.
struct SubscriptionResult {
    SchemaArrival schema;
};

/// Abstract transport provider for pub/sub.
///
/// The provider deals with raw byte buffers and nanoarrow schemas —
/// no Apache Arrow C++ dependency.  Higher-level wrappers (Publisher,
/// Subscriber, PublisherArrow, SubscriberArrow) add convenience APIs
/// on top of this interface.
///
/// Topic names are represented as a list of string segments so that
/// the provider can join them with any separator it prefers. **An empty segment
/// list is illegal** and is refused with PubSubStatus::kInvalidArgument by every
/// method that takes one — there is no default topic and no recovery (§3.5).
/// C form: a pointer-and-count of pointer-and-length pairs, borrowed for the
/// duration of the call; a callee that keeps a segment copies its bytes. As
/// everywhere in this vocabulary, the C form is conceptual — no layout
/// compatibility is implied and each boundary constructs its own.
///
/// **Failure.** Every method here reports failure by throwing PubSubError,
/// which carries a stable numbered PubSubStatus (spec §5.1, see
/// fletcher/core/status.hpp). A provider translates at its own entry points, so
/// no untyped exception — not even std::bad_alloc — leaves the seam.
///
/// Provider semantics: ONE callback per topic. Fan-out (multiple
/// local subscribers to the same topic) is handled by the Subscriber
/// class, not by providers.
///
/// Provider-specific configuration (e.g. QoS) is supplied when the provider is
/// created, through `ProviderConfig` — a typed core of exactly
/// `{max_payload_bytes, domain_id}` plus an opaque document in the provider's
/// own format, which Fletcher transports and never reads (§4.1, §4.2, see
/// provider_registry.hpp). There is no per-call config parameter and no
/// protocol-typed Options struct at this seam. The in-process loopback
/// (PDA-DEC-5) and Fast DDS (PDA-DEC-6) are configured this way — Fast DDS by
/// its own native XML QoS profiles document, so no eProsima type is nameable
/// from here. XRCE still takes its own options struct until PDA-DEC-7 retires
/// `XrceConfig` the same way.
class PubSubProvider {
   public:
    virtual ~PubSubProvider() = default;

    /// Declares a topic and its schema; called on the publisher side. The
    /// schema describes the Arrow structure of rows on this topic and its
    /// ownership is transferred to the provider. Subscribers do not call this —
    /// they learn the schema out-of-band (see Subscribe). Re-declaring a topic
    /// with an identical schema is idempotent (so several publishers may share
    /// one topic); a provider **must** reject a re-declaration with a
    /// conflicting schema, by throwing (spec §7 clause 3 — "may" became "must"
    /// with the 2026-09-01 ruling, so a provider that silently overwrote the
    /// declared schema is now non-conforming).
    virtual void CreateTopic(const std::vector<std::string>& topic_segments,
                             OwnedSchema schema) = 0;

    /// Callback that encodes a row directly into a WriteBuffer.
    using RowEncoder = std::function<void(WriteBuffer&)>;

    /// Publish by writing the encoded row directly into the provider's
    /// buffer.  The provider supplies a WriteBuffer; the encoder writes
    /// into it.
    virtual void Publish(const std::vector<std::string>& topic_segments, const RowEncoder& encoder,
                         const Attachments& attachments = {}) = 0;

    /// Callback signature for Subscribe — delivers raw encoded row bytes,
    /// the topic's schema, and any sidecar attachments.
    /// `schema` and `attachments` are **borrowed for the duration of the call**, like `data`. A
    /// callback that wants to keep either one copies it — SharedSchema is a shared_ptr, so a copy
    /// keeps the schema alive for as long as that copy lives.
    ///
    /// They were passed by value until it was measured. An empty `Attachments` is an
    /// `unordered_map`, and MSVC allocates a sentinel node in its default constructor, so every
    /// delivery on every topic built and destroyed one whether or not the sample carried any:
    /// **~110 ns per sample against 1.4 ns for the call itself**, more than the whole rest of the
    /// delivery path. See "Measured decisions" in the FastDDS provider README.
    ///
    /// Delivery contract every provider must uphold:
    ///  - **Schema before data.** The callback is never invoked with a null
    ///    schema. A subscriber may Subscribe before any publisher exists; the
    ///    schema then arrives asynchronously, and the provider buffers data
    ///    that arrives ahead of it and delivers that data only once the schema
    ///    is known. A transport that carries no schemas for a topic - the
    ///    gateway's in-process loopback in its default mode, where the client
    ///    brings its own - passes null throughout instead. **The two are never
    ///    mixed within one subscription**: whichever a subscription's first
    ///    delivery would carry is what every delivery on it carries, for its
    ///    whole life.
    ///  - **Per-writer order.** Samples from a single writer reach the callback
    ///    in the order they were published. This holds across the schema
    ///    handoff too: the buffered pre-schema backlog is delivered before —
    ///    and never interleaved with — samples that arrive live afterwards.
    ///  - **One callback at a time.** Never two deliveries in flight for the same
    ///    subscription, though the thread they arrive on may differ between samples. A
    ///    provider that fans out from several threads must serialise them itself.
    using SubscribeCallback =
        std::function<void(const uint8_t* data, size_t len, const SharedSchema& schema,
                           const Attachments& attachments)>;

    /// Subscribe to a named topic. **Never blocks**: a subscriber may subscribe
    /// before any publisher exists, and the returned SubscriptionResult carries
    /// a waitable *arrival* for the schema rather than the schema itself — see
    /// SubscriptionResult for what it answers with, and when. Delivery obeys
    /// the SubscribeCallback contract above (schema-before-data, per-writer
    /// order).
    ///
    /// **A subscription's schema mode is fixed when Subscribe returns** and is
    /// exactly what its SchemaArrival reports: a declaration made after a
    /// subscription exists never reaches that subscription. §7 clause 1's "never
    /// mix" is a property of the subscription, so a client can decode one stream
    /// one way for its whole life.
    ///
    /// `data` may point into a buffer the transport owns rather than a copy of one — that is a
    /// provider-local optimisation, not part of this contract. Either way the pointer is only
    /// valid for the duration of the call and was never owned by the callback.
    [[nodiscard]] virtual SubscriptionResult Subscribe(
        const std::vector<std::string>& topic_segments, SubscribeCallback callback) = 0;

    /// Remove a previously registered subscription. **Once this returns, no
    /// further callback runs for that topic** (§7 clause 6): a provider that
    /// delivers from its own thread must not let a delivery already in flight
    /// outlive the call. Unsubscribing a topic with no subscription is a no-op,
    /// not an error, so it is safe to call unconditionally on teardown.
    virtual void Unsubscribe(const std::vector<std::string>& topic_segments) = 0;
};

}  // namespace fletcher

#endif  // FLETCHER_INCLUDE_PUBSUB_PROVIDER_HPP_
