// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// NOTHING eProsima may appear in this header. That is not a style rule: the CMake target links
// fast-dds PRIVATE and the Conan recipe drops `transitive_headers`, so `test_package` compiles
// with **no Fast DDS include directories at all** and any surviving `<fastdds/...>` here is a
// compile error there. It is the machine check for the 2026-08-31 configuration ruling —
// "Fletcher never learns DDS vocabulary" (PDA-DEC-6 §5).
#ifndef FLETCHER_INCLUDE_FAST_DDS_PUBSUB_PROVIDER_HPP_
#define FLETCHER_INCLUDE_FAST_DDS_PUBSUB_PROVIDER_HPP_

#include <cstdint>
// The bound advice below tells a caller to write `kPayloadBytes<N>`, so the header owes them the
// declaration: nothing else here pulls it in, and an out-of-tree TU that takes the advice would
// otherwise not compile (review 4a F7). Fletcher's own header - no eProsima, so the machine check
// above is unaffected. `test_package/src/example.cpp` writes the idiom, so this include cannot be
// dropped again in silence.
#include <fletcher/pubsub/payload_bound.hpp>
#include <fletcher/pubsub/provider.hpp>
#include <fletcher/pubsub/provider_registry.hpp>
#include <memory>
#include <string>
#include <vector>

namespace fletcher {

/// Make Fast DDS selectable as `"fastdds"` (spec §4 clause 4).
///
/// Idempotence is NOT offered: a second call is refused by
/// `ProviderRegistry::Register` (`kInvalidArgument`) — a registry means one
/// transport per name for its whole life.
void RegisterFastDDSProvider(ProviderRegistry& registry);

/// PubSubProvider transport backed by eProsima Fast DDS.
///
/// ── How it is configured (spec §4.1, owner rulings 2026-08-31 / 2026-09-02) ──
/// `ProviderConfig` and nothing else. There are no runtime setters: QoS is fixed
/// at construction, which is what stops "QoS set after the DataWriter exists"
/// bugs.
///
///  - `domain_id` — the DDS domain, used exactly as given.
///  - `max_payload_bytes` — the row payload ceiling; **0 means unset** and
///    resolves to 65536. The bound is part of the registered DDS type name, so
///    two endpoints on different bounds do not discover each other at all. A
///    value `IsPayloadBound` rejects is refused with
///    `PubSubError(kInvalidArgument)` before the participant exists. Write it
///    as `kPayloadBytes<N>` to be told at compile time instead.
///  - `document` — **a Fast DDS XML profiles document, as text** (owner ruling
///    2026-09-02: the setting holds the XML itself, never a filename; the
///    gateway's `--provider-config FILE` is where reading a file lives). Fast
///    DDS parses it; Fletcher gains no parser (locked decision 8). An empty
///    document means "Fletcher's built-in profile everywhere", which is what
///    every caller got before this existed.
///
/// ── What the document may say ───────────────────────────────────────────────
/// Reserved profile names, and what each role falls back to when the document
/// does not name one:
///
///  - **participant** — `fletcher_participant`, which a non-empty document
///    MUST define; otherwise Fast DDS's default, named `FletcherParticipant`.
///  - **data writer on topic `T`** — the profile named `T` (the `/`-joined
///    topic), then `fletcher_writer`, then Fletcher's built-in writer profile.
///  - **data reader on topic `T`** — the profile named `T`, then
///    `fletcher_reader`, then Fletcher's built-in reader profile.
///  - **the internal `__schema` channel** — no profile name is ever consulted
///    for it; it keeps its own fixed QoS.
///
/// **A supplied profile is that endpoint's WHOLE quality-of-service.** Anything
/// it leaves out takes *Fast DDS's* default, not Fletcher's: there is no merge
/// and no floor (owner ruling 2026-09-02). The XML API returns a filled QoS and
/// cannot report which policies a document mentioned, so an overlay rule would
/// rest on a fact the substrate does not expose. The README publishes Fletcher's
/// own profile as the copy-paste starting point.
///
/// The two settings a QoS profile cannot express live as vendor properties in
/// the anchor's `<rtps><propertiesPolicy>`: `fletcher.loan_publish` (`true` /
/// `false`) and `fletcher.max_schema_bytes` (a positive integer). Both are
/// consumed and stripped before the participant is created; every other property
/// reaches Fast DDS untouched, which is what security plugins need.
///
/// ── Refused, all `kInvalidArgument` ─────────────────────────────────────────
/// In the constructor, before the participant exists: a non-empty document that
/// Fast DDS cannot parse, or that does not define `fletcher_participant`; an
/// unknown or unparseable `fletcher.*` property; a non-zero `<domainId>` in the
/// anchor disagreeing with `config.domain_id`; an unusable
/// `max_payload_bytes`.
///
///  - **Later, not at construction:** a `fletcher.*` property placed in a
///    writer or reader profile instead of the anchor is read by nobody, so it
///    is refused rather than left inert — but only once the profile is
///    resolved. For `fletcher_writer` / `fletcher_reader` that is still inside
///    the constructor (after the Publisher and Subscriber exist); for a profile
///    named after a topic it is that topic's **first endpoint use** — the first
///    `Publish` or `Subscribe`, the first moment the name is known. **A
///    provider that constructed is therefore not a provider whose every profile
///    has been checked.**
///
/// The companion schema channel (`__schema` topic) always uses RELIABLE +
/// KEEP_LAST(depth=1) + TRANSIENT_LOCAL and is not configurable — a
/// Fletcher-internal implementation detail, so no profile name is consulted for
/// it.
class FastDDSPubSubProvider : public PubSubProvider {
   public:
    explicit FastDDSPubSubProvider(const ProviderConfig& config = {});

    /// Destruction precondition: the caller must ensure the provider is
    /// quiescent — no thread executing or about to enter a public API on this
    /// instance, and no provider callback still in flight that can re-enter it.
    /// The destructor tears down DDS entities and invalidates all internal
    /// state; it is not a synchronization boundary for concurrent use.
    ~FastDDSPubSubProvider() override;

    FastDDSPubSubProvider(const FastDDSPubSubProvider&) = delete;
    FastDDSPubSubProvider& operator=(const FastDDSPubSubProvider&) = delete;

    void CreateTopic(const std::vector<std::string>& topic_segments, OwnedSchema schema) override;

    void Publish(const std::vector<std::string>& topic_segments, const RowEncoder& encoder,
                 const Attachments& attachments = {}) override;

    // [[nodiscard]] is NOT inherited from the PubSubProvider base declaration and
    // the diagnostic keys off the STATIC type at the call site, so the annotation
    // must be repeated on every concrete override or it never fires where
    // applications actually call (#56).
    [[nodiscard]] SubscriptionResult Subscribe(const std::vector<std::string>& topic_segments,
                                               SubscribeCallback callback) override;

    void Unsubscribe(const std::vector<std::string>& topic_segments) override;

    /// The payload bound in force — `ProviderConfig::max_payload_bytes` exactly as given, or
    /// 65536 if it was 0 (unset). An unsupported one never gets past the constructor. It is the
    /// number in the registered type name, and the size a row has to fit.
    uint32_t PayloadBytes() const;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace fletcher

#endif  // FLETCHER_INCLUDE_FAST_DDS_PUBSUB_PROVIDER_HPP_
