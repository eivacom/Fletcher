// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// NOTHING eProsima may appear in this header. That is not a style rule: the CMake target links
// fast-dds PRIVATE and the Conan recipe drops `transitive_headers`, so `test_package` compiles
// with **no Fast DDS include directories at all** and any surviving `<fastdds/...>` here is a
// compile error there. It is the machine check for the rule that Fletcher never learns DDS
// vocabulary.
#ifndef FLETCHER_INCLUDE_FAST_DDS_PUBSUB_PROVIDER_HPP_
#define FLETCHER_INCLUDE_FAST_DDS_PUBSUB_PROVIDER_HPP_

#include <cstdint>
// The bound advice below tells a caller to write `kPayloadBytes<N>`, so the header owes them the
// declaration: nothing else here pulls it in, and an out-of-tree TU that takes the advice would
// otherwise not compile. Fletcher's own header - no eProsima, so the machine check
// above is unaffected. `test_package/src/example.cpp` writes the idiom, so this include cannot be
// dropped again in silence.
#include <fletcher/pubsub/payload_bound.hpp>
#include <fletcher/pubsub/provider.hpp>
#include <fletcher/pubsub/provider_registry.hpp>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fletcher {

/// Make Fast DDS selectable as `"fastdds"`.
///
/// Idempotence is NOT offered: a second call is refused by
/// `ProviderRegistry::Register` (`kInvalidArgument`) — a registry means one
/// transport per name for its whole life.
///
/// A registry builds providers from a `ProviderConfig` alone, so a provider
/// reached through one observes no statuses. `FastDDSStatusListener` needs the
/// two-argument constructor below, which means constructing the provider
/// directly.
void RegisterFastDDSProvider(ProviderRegistry& registry);

/// Endpoint and discovery status, translated out of DDS types. Non-owning and optional: a provider
/// built without one observes nothing, which is what every caller gets by default.
///
/// Every method is a `noexcept` no-op here — override the ones worth hearing about.
/// `FastDDSLoggingStatusListener` below is the ready-made subclass that logs them all.
///
/// ── Threading contract ──────────────────────────────────────────────────────
/// A callback runs on a Fast DDS thread (data and writer statuses alike, and
/// discovery), on an application thread that is inside `CreateTopic` /
/// `Publish` / `Subscribe` of **any** provider in this process, with that
/// provider's mutex held, or on the schema thread. The schema thread is where
/// every `__schema` reader status (`OnMatched`, `OnDeadlineMissed`,
/// `OnLivelinessChanged`, `OnIncompatibleQos`, `OnSampleLost`,
/// `OnSampleRejected`, on that endpoint only) is dispatched on every wake — it
/// reads `get_status_changes()` and the matching `get_*_status()` getter for
/// each changed bit, the same pattern as eiva-ddsbus's `WaitsetDataReader` —
/// and it is also where that thread creates a topic's data reader, already
/// enabled, once its schema and payload bound arrive: endpoint matching between
/// participants that have already discovered each other runs synchronously
/// inside `create_datareader` / `create_datawriter`, including between two
/// participants in one process; participant discovery itself still runs over the
/// transport. Inside `Subscribe`, that same call runs under both the provider
/// mutex and the schema thread's lock; on the schema thread it runs under that
/// lock alone. `CreateTopic`'s data-writer creation runs under both locks too.
/// DATA reader statuses, by contrast, arrive through Fast DDS's own listener
/// dispatch, the same as writer statuses and discovery. One
/// `DataWriterListener` and one `ParticipantListener` instance is shared by
/// every endpoint a provider owns, so two of these calls can be in flight on two
/// different threads at once — nothing here serialises callbacks against each
/// other, only each one against the provider mutex, or the schema thread's own
/// lock, it happens to be running under. So an override
///
///  - **must not call into any provider.** The provider mutex is a
///    non-recursive `std::shared_mutex`; re-entering deadlocks. So is the
///    lock a status dispatched from the schema thread runs under.
///  - **must not block**, and must not wait on the thread destroying a
///    provider: `~FastDDSPubSubProvider` waits for in-flight callbacks.
///  - **must not throw.** Every method is `noexcept`, so an override has to be
///    spelled `noexcept override` too (the compiler refuses one that is not) and
///    a throw inside it terminates. Fletcher does not catch here the way it does
///    for a subscription callback: one bad sample must not stop a stream, but a
///    throwing status callback is a defect worth stopping on.
///
/// Calls can arrive until `~FastDDSPubSubProvider` returns, so the listener must
/// outlive the provider — the same rule as every listener handed to Fast DDS.
class FastDDSStatusListener {
   public:
    virtual ~FastDDSStatusListener() = default;

    /// Which endpoint the status is about. `topic` is the joined Fletcher topic;
    /// `is_schema_channel` marks the companion `<topic>/__schema` endpoint, whose statuses are
    /// about the schema handoff rather than the caller's rows. The view aliases a name the
    /// transport owns and is valid for the duration of the call only.
    struct Endpoint {
        std::string_view topic;
        bool is_schema_channel;
        bool is_writer;
    };

    virtual void OnMatched(Endpoint /*endpoint*/, int32_t /*current_count*/,
                           int32_t /*change*/) noexcept {}
    // `policy_id` is DDS's `QosPolicyId_t`, narrowed to a plain integer so this DDS-free header
    // names no eProsima type; it identifies which QoS policy the two ends disagreed on.
    virtual void OnIncompatibleQos(Endpoint /*endpoint*/, uint32_t /*policy_id*/,
                                   uint32_t /*total_count*/) noexcept {}
    virtual void OnDeadlineMissed(Endpoint /*endpoint*/, uint32_t /*total_count*/) noexcept {}
    // Readers only.
    virtual void OnLivelinessChanged(Endpoint /*endpoint*/, int32_t /*alive_count*/,
                                     int32_t /*not_alive_count*/) noexcept {}
    // Writers only.
    virtual void OnLivelinessLost(Endpoint /*endpoint*/, uint32_t /*total_count*/) noexcept {}
    // Readers only.
    virtual void OnSampleLost(Endpoint /*endpoint*/, uint32_t /*total_count*/) noexcept {}
    // Readers only. `reason` is DDS's `SampleRejectedStatusKind`, narrowed the same way as
    // `policy_id` above.
    virtual void OnSampleRejected(Endpoint /*endpoint*/, int32_t /*reason*/,
                                  uint32_t /*total_count*/) noexcept {}
    // Writers only.
    virtual void OnUnacknowledgedSampleRemoved(Endpoint /*endpoint*/) noexcept {}

    /// Discovery of REMOTE entities, which is a strictly wider net than matching. A remote writer
    /// whose `type_name` carries another bound (`fletcher_65536` against `fletcher_8192`, say) is
    /// no longer a subscriber-side mismatch: a subscriber's own reader is created at whatever
    /// bound the publisher it follows announced, so it has none of its own to disagree with. What
    /// a differing `type_name` shows here is either publisher-vs-publisher information (a second
    /// publisher announcing another bound for a topic already resolved is logged and never
    /// matches the first), or the sign that this instance already holds the topic, as a
    /// publisher, at another bound. `alive` is false once the entity is removed or dropped.
    /// Fletcher's own companion `<topic>/__schema` endpoints are not reported.
    virtual void OnParticipantDiscovered(std::string_view /*name*/, bool /*alive*/) noexcept {}
    virtual void OnWriterDiscovered(std::string_view /*topic*/, std::string_view /*type_name*/,
                                    bool /*alive*/) noexcept {}
    virtual void OnReaderDiscovered(std::string_view /*topic*/, std::string_view /*type_name*/,
                                    bool /*alive*/) noexcept {}
};

/// The lines the provider used to log itself, as an opt-in listener — one for one, at the levels
/// it used. Pass one of these to restore the diagnostics a provider built without a listener no
/// longer prints.
///
/// Bodies live in `src/status_listener.cpp`, so the log lines cost a consumer nothing to declare.
/// Derive from this rather than from `FastDDSStatusListener` to add behaviour and keep the line:
/// call the base from the override, and leave alone whatever should just keep logging.
class FastDDSLoggingStatusListener : public FastDDSStatusListener {
   public:
    void OnMatched(Endpoint endpoint, int32_t current_count, int32_t change) noexcept override;
    void OnIncompatibleQos(Endpoint endpoint, uint32_t policy_id,
                           uint32_t total_count) noexcept override;
    void OnDeadlineMissed(Endpoint endpoint, uint32_t total_count) noexcept override;
    void OnLivelinessChanged(Endpoint endpoint, int32_t alive_count,
                             int32_t not_alive_count) noexcept override;
    void OnLivelinessLost(Endpoint endpoint, uint32_t total_count) noexcept override;
    void OnSampleLost(Endpoint endpoint, uint32_t total_count) noexcept override;
    void OnSampleRejected(Endpoint endpoint, int32_t reason,
                          uint32_t total_count) noexcept override;
    void OnUnacknowledgedSampleRemoved(Endpoint endpoint) noexcept override;
};

/// PubSubProvider transport backed by eProsima Fast DDS.
///
/// ── How it is configured ──
/// `ProviderConfig` and nothing else. There are no runtime setters: QoS is fixed
/// at construction, which is what stops "QoS set after the DataWriter exists"
/// bugs.
///
///  - `domain_id` — the DDS domain, used exactly as given.
///  - `max_payload_bytes` — governs this provider's PUBLISHERS only: the row
///    payload ceiling, and the type name (`fletcher_<bound>`) `CreateTopic`
///    registers. **0 means unset** and resolves to 65536. A subscriber takes
///    its bound from the publisher it follows, announced on `__schema`, not
///    from this field. A value `IsPayloadBound` rejects is refused with
///    `PubSubError(kInvalidArgument)` before the participant exists. Write it
///    as `kPayloadBytes<N>` to be told at compile time instead.
///  - `document` — **a Fast DDS XML profiles document, as text** (the setting
///    holds the XML itself, never a filename; the gateway's
///    `--provider-config FILE` is where reading a file lives). Fast
///    DDS parses it; Fletcher gains no parser. **Empty** —
///    replaced by the provider's own default document (the README's "The
///    published starting point"), so there is exactly one path: the document
///    — supplied or default — is loaded ONCE into Fast DDS's own process-wide
///    profile registry, which then decides every endpoint's QoS. Every
///    provider in one process must therefore carry the same bytes; an empty
///    document and a different non-empty document collide, and the second is
///    refused, the same as any two different documents.
///
/// ── What the document may say ───────────────────────────────────────────────
///
///  - **participant** — `fletcher_participant`, which the document MUST
///    define — mandatory even for the provider's own default document, so a
///    document that would otherwise silently register nothing is refused
///    instead of running on Fast DDS's own defaults unnoticed.
///  - **the default writer / reader QoS** — the document's
///    `<data_writer is_default_profile="true">` / `<data_reader
///    is_default_profile="true">` profile, if it has one; otherwise Fast DDS's
///    own default (Fast DDS seeds the Publisher's / Subscriber's default QoS
///    from it when each is created).
///  - **a per-topic override on topic `T`** — the profile named `T` (the
///    `/`-joined topic), ahead of the default above.
///  - **the internal `__schema` channel** — no profile name is ever consulted
///    for it; it keeps its own fixed QoS, bounded at the fixed
///    `kSchemaPayloadBytes` (`pubsub/include/fletcher/pubsub/payload_bound.hpp`),
///    and its sample carries one attachment — the publisher's `max_payload_bytes`.
///
/// **A supplied profile is that endpoint's WHOLE quality-of-service.** Anything
/// it leaves out takes *Fast DDS's* default, not Fletcher's: there is no merge
/// and no floor. The XML API returns a filled QoS and
/// cannot report which policies a document mentioned, so an overlay rule would
/// rest on a fact the substrate does not expose. The README publishes Fletcher's
/// own profile as the copy-paste starting point.
///
/// The document carries no vendor properties: `Publish` always goes through the
/// regular (non-loaned) path. `LoanableSampleWriter` stays in the tree, compiled
/// and unit-tested, but is not selectable by a document or any other
/// configuration.
///
/// ── Refused, all `kInvalidArgument` ─────────────────────────────────────────
/// In the constructor, before the participant exists: a non-empty document that
/// Fast DDS cannot parse, or that does not define `fletcher_participant`; a
/// second, different document loaded by another provider in this process
/// (Fast DDS profile names are process-wide; byte-identical copies are fine —
/// an empty document collides the same way, since it is replaced by the
/// provider's own default document before this rule applies); a non-zero
/// `<domainId>` in the anchor disagreeing with `config.domain_id`; an unusable
/// `max_payload_bytes`. `Publish` on a topic this provider never `CreateTopic`d
/// — including one it only `Subscribe`d to — is `kTopicNotDeclared`.
///
/// `CreateTopicWithOptions` and `SubscribeWithOptions` (`TopicOptions`, below) add three more, all
/// `kInvalidArgument`: a `profile` naming no `<data_writer>` / `<data_reader>` profile the document
/// defines, quoting the name; a re-declaration of an already-declared topic with a different
/// non-empty profile or a different `max_payload_bytes`; and a non-zero `max_payload_bytes` on
/// `SubscribeWithOptions`, which always follows its publisher's announced bound instead.
///
/// The companion schema channel (`__schema` topic) always uses RELIABLE +
/// KEEP_LAST(depth=1) + TRANSIENT_LOCAL, bounded at the fixed
/// `kSchemaPayloadBytes`, and is not configurable — a Fletcher-internal
/// implementation detail, so no profile name is consulted for it. Its sample
/// carries the schema row plus one attachment, the publisher's
/// `max_payload_bytes`, which is how a subscriber learns the bound to open its
/// data reader at.
class FastDDSPubSubProvider : public PubSubProvider {
   public:
    explicit FastDDSPubSubProvider(const ProviderConfig& config = {});

    /// The same construction, plus an observer for endpoint and discovery status. The pointer is
    /// non-owning and may be null (identical to the constructor above); a non-null one must outlive
    /// this provider, because calls arrive until `~FastDDSPubSubProvider` returns. Read
    /// `FastDDSStatusListener` for what an override may and may not do.
    FastDDSPubSubProvider(const ProviderConfig& config, FastDDSStatusListener* status_listener);

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
    // applications actually call.
    [[nodiscard]] SubscriptionResult Subscribe(const std::vector<std::string>& topic_segments,
                                               SubscribeCallback callback) override;

    void Unsubscribe(const std::vector<std::string>& topic_segments) override;

    /// `CreateTopic` with per-topic options (`TopicOptions`, pubsub/provider.hpp).
    /// `options.profile` selects a `<data_writer>` profile by name for THIS topic's writer, ahead
    /// of the document's per-topic-name lookup and its default profile; a name the document does
    /// not define is `kInvalidArgument`, quoting it. `options.max_payload_bytes` is this topic's
    /// own publisher bound — the type name this topic's writer registers and the bound it announces
    /// on `__schema` — in place of the provider's own bound for this topic only; `PayloadBytes()`
    /// itself is unchanged and still answers the provider's own configured bound. Zero means "this
    /// topic follows the provider's own bound", the same as `CreateTopic`. A bound
    /// `IsPayloadBound` rejects is `kInvalidArgument`, quoting it, checked before any lock.
    /// Re-declaring an already-declared topic with a different non-empty profile, or a different
    /// non-zero bound, is `kInvalidArgument`; an identical re-declaration (or one with empty
    /// options, which names neither field) is the same idempotent no-op `CreateTopic` is.
    /// `CreateTopic` is a one-line delegation to this with `TopicOptions{}`, so every one of its
    /// refusals is this method's.
    void CreateTopicWithOptions(const std::vector<std::string>& topic_segments, OwnedSchema schema,
                                const TopicOptions& options) override;

    /// `Subscribe` with per-topic options. `options.profile` selects a `<data_reader>` profile by
    /// name for this subscription's reader, resolved the moment `Subscribe` runs, before any lock,
    /// so an unknown name is refused synchronously rather than surfacing later when the schema
    /// thread opens the reader. `options.max_payload_bytes` is always `kInvalidArgument`: a
    /// subscription follows whatever bound its publisher announces on `__schema` and never carries
    /// one of its own. `Subscribe` is a one-line delegation to this with `TopicOptions{}`.
    //
    // [[nodiscard]] is NOT inherited from the PubSubProvider base declaration and the diagnostic
    // keys off the STATIC type at the call site, so the annotation must be repeated here too (see
    // Subscribe above) or it never fires where applications actually call.
    [[nodiscard]] SubscriptionResult SubscribeWithOptions(
        const std::vector<std::string>& topic_segments, SubscribeCallback callback,
        const TopicOptions& options) override;

    /// Both optional seam methods are served here: the `__schema` channel this provider already
    /// runs for every subscription IS the schema-only subscription, so a watch is that channel
    /// with no data reader beside it. Read `PubSubProvider::SubscribeSchema` for the contract; the
    /// README's `## Usage` has the catalog recipe.
    [[nodiscard]] SchemaArrival SubscribeSchema(
        const std::vector<std::string>& topic_segments) override;

    void UnsubscribeSchema(const std::vector<std::string>& topic_segments) override;

    /// The bound for topics declared WITHOUT a `TopicOptions::max_payload_bytes` override (see
    /// `CreateTopicWithOptions`) — `ProviderConfig::max_payload_bytes` exactly as given, or 65536
    /// if it was 0 (unset). An unsupported one never gets past the constructor. It is the bound
    /// such a topic's writer registers in its type name and the size a published row has to fit;
    /// it says nothing about what this provider's subscriptions use, since a subscriber's reader
    /// is created at the bound its publisher announces.
    [[nodiscard]] uint32_t PayloadBytes() const noexcept;

    /// The XML document the provider loads when `ProviderConfig::document` is empty: one
    /// `<data_writer is_default_profile="true">` and one `<data_reader is_default_profile="true">`
    /// profile (RELIABLE, VOLATILE, KEEP_LAST 25) plus the participant anchor, and five named
    /// `<data_writer>`/`<data_reader>` pairs, selectable through `TopicOptions::profile` on
    /// `CreateTopic` and `Subscribe` with no document of your own: `fire_and_forget` (BEST_EFFORT,
    /// VOLATILE, KEEP_LAST 1) drops a lagging sample rather than retransmit it; `latest` (RELIABLE,
    /// VOLATILE, KEEP_LAST 1) sends the newest value, resent if lost, and never replays to a late
    /// subscriber; `store_latest` (RELIABLE, TRANSIENT_LOCAL, KEEP_LAST 1) replays the last value
    /// to a late subscriber; `store_history` (RELIABLE, TRANSIENT_LOCAL, KEEP_LAST 25) replays the
    /// last 25; `lossless` (RELIABLE, VOLATILE, KEEP_ALL, an infinite `max_blocking_time`)
    /// delivers every sample in order and blocks the writer rather than drop one. Start a custom
    /// document from
    /// this text and add named profiles inside `<profiles>`; a custom document replaces all of
    /// this one, so copy the pairs you keep. The two `is_default_profile` profiles are what every
    /// topic without a profile of its own runs on.
    [[nodiscard]] static const char* DefaultProfilesDocument() noexcept;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace fletcher

#endif  // FLETCHER_INCLUDE_FAST_DDS_PUBSUB_PROVIDER_HPP_
