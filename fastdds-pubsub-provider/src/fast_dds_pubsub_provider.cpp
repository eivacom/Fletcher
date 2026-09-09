// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// Targets eProsima Fast DDS 3.4.x (fast-dds/3.4.0 from Conan Center). Comments across this
// provider cite upstream by file and symbol against that tree — no line numbers, which a point
// release invalidates without anything noticing.
//
// This file holds the provider itself: the pimpl, the participant/publisher/subscriber lifecycle,
// and the four PubSubProvider methods. Everything they compose lives one per header in internal/:
//
//   profile_document.hpp      the provider's configuration document: the reserved profile names,
//                             the resolution ladders, the anchor and the two `fletcher.*`
//                             properties -- everything Fast DDS's own XML API is asked for
//   qos_defaults.hpp          Fletcher's built-in profiles, the fallback when the document names
//                             no profile for a role (never a floor UNDER one)
//   fletcher_sample.hpp       the plain sample Fast DDS lends at both ends: its layout, and what
//                             makes it plain for a given bound
//   transport_data.hpp        the sample types handed to Fast DDS on the serialising paths
//   envelope_codec.hpp        the contents of a sample's body
//   fletcher_sample_pub_sub_type.hpp
//                             the data-channel TopicDataType over that layout — and what it
//                             reports about itself, which is what Fast DDS gates data-sharing and
//                             loans on
//   raw_bytes_pub_sub_type.hpp
//                             the companion __schema channel's TopicDataType
//   data_reader_listener.hpp  the two read flows, DataReaderListener / LoanableDataReaderListener,
//                             feeding OrderedDelivery — plus the reader statuses forwarded and the
//                             predicate that decides which flow a reader QoS admits
//   sample_writer.hpp         the two publish flows, SampleWriter / LoanableSampleWriter
//   data_writer_listener.hpp  the writer statuses forwarded, and the status masks for both ends
//   participant_listener.hpp  discovery of remote entities, forwarded
//   status_endpoint.hpp       what all three forwarding listeners share: a Fast DDS endpoint
//                             translated into the public FastDDSStatusListener::Endpoint
//   schema_channel.hpp        the __schema handoff: promise + listener
//   ordered_delivery.hpp      single-drainer FIFO preserving writer order across that handoff
//
// No Arrow C++ dependency anywhere in the path: rows arrive as encoded bytes and leave as encoded
// bytes.

#include "fletcher/fastdds_pubsub_provider/fast_dds_pubsub_provider.hpp"

#include <chrono>
#include <cstdint>
#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/log/Log.hpp>
#include <fastdds/dds/publisher/DataWriter.hpp>
#include <fastdds/dds/publisher/Publisher.hpp>
#include <fastdds/dds/subscriber/DataReader.hpp>
#include <fastdds/dds/subscriber/Subscriber.hpp>
#include <fastdds/dds/topic/Topic.hpp>
#include <fastdds/dds/topic/TypeSupport.hpp>
#include <fletcher/core/internal/delivery_frame.hpp>
#include <fletcher/core/write_buffer.hpp>
#include <fletcher/pubsub/delivery_channel.hpp>
#include <fletcher/pubsub/internal/segments.hpp>
#include <fletcher/pubsub/payload_bound.hpp>
#include <fletcher/pubsub/schema_ipc.hpp>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "internal/data_reader_listener.hpp"
#include "internal/data_writer_listener.hpp"
#include "internal/envelope_codec.hpp"
#include "internal/fletcher_sample_pub_sub_type.hpp"
#include "internal/ordered_delivery.hpp"
#include "internal/participant_listener.hpp"
#include "internal/profile_document.hpp"
#include "internal/qos_defaults.hpp"
#include "internal/raw_bytes_pub_sub_type.hpp"
#include "internal/sample_writer.hpp"
#include "internal/schema_channel.hpp"
#include "internal/transport_data.hpp"

using namespace eprosima::fastdds::dds;

namespace fletcher {

// -----------------------------------------------------------------------
// Impl — hides all Fast DDS types behind the pimpl wall.
// -----------------------------------------------------------------------

struct FastDDSPubSubProvider::Impl {
    // The three listeners that forward statuses all need the observer at construction, so it
    // arrives here rather than being assigned afterwards.
    explicit Impl(FastDDSStatusListener* listener)
        : status_listener(listener),
          data_writer_listener(listener),
          participant_listener(listener) {}

    struct TopicState {
        Topic* topic = nullptr;
        DataWriter* writer = nullptr;
        DataReader* reader = nullptr;
        std::unique_ptr<internal::DataReaderListenerBase> listener;
        // Companion schema topic (publisher side).
        Topic* schema_topic = nullptr;
        DataWriter* schema_writer = nullptr;
        // Companion schema channel (subscriber side): a persistent reader + listener that
        // resolves schema_channel asynchronously when the schema arrives — so Subscribe works
        // subscriber-first (before any publisher), and SubscribeSchema works with no data reader
        // at all. Opened by EnsureSchemaChannel; one per topic, shared by a watch and the data
        // subscription.
        DataReader* schema_reader = nullptr;
        std::unique_ptr<internal::SchemaListener> schema_listener;
        std::shared_ptr<internal::SchemaChannel> schema_channel;
        // A SubscribeSchema watch is outstanding, so the channel outlives the data subscription:
        // Unsubscribe re-arms it (RearmSchemaWatch) instead of breaking it.
        bool schema_watch = false;
        // Schema (nanoarrow ArrowSchema).
        OwnedSchema schema;
        // The same schema as Arrow IPC bytes, kept from the announcement so a re-declaration is a
        // byte compare rather than another encode of what was already encoded once.
        std::vector<uint8_t> schema_ipc;
        bool is_publisher = false;
    };

    // Non-owning and possibly null: the application's observer for endpoint and discovery status
    // (public header). Handed to every listener this provider creates; null means nothing is
    // observed, which is what construction from a `ProviderConfig` alone gives.
    FastDDSStatusListener* status_listener = nullptr;

    DomainParticipant* participant = nullptr;
    Publisher* publisher = nullptr;
    Subscriber* subscriber = nullptr;
    TypeSupport type_support;
    TypeSupport schema_type_support;
    // Shared for Publish, exclusive for everything that mutates `topics` or the endpoints in it.
    // DataWriter::write is itself thread safe, so a shared lock is enough to keep the topic and its
    // writer alive for the duration of the call, and publishes to different topics then run
    // concurrently instead of serialising on this mutex. See README "Measured decisions".
    std::shared_mutex mu;
    // Unordered because Publish looks a topic up by name on every sample and a hash beats the
    // std::map this was: log-n string comparisons per publish bought nothing. Reference stability
    // across Publish's lock drop is unchanged — rehashing invalidates iterators, never references
    // or pointers to elements.
    std::unordered_map<std::string, TopicState> topics;

    // The registered type and both loaned flows are built from this one number.
    uint32_t payload_bytes = 0;

    // Which publish flow Publish uses, fixed at construction from the document's
    // `fletcher.loan_publish` property. Stateless, so one instance serves every topic and every
    // thread.
    std::unique_ptr<internal::SampleWriterBase> sample_writer;

    // Shared by every DataWriter this provider creates; carries no per-topic state either.
    internal::DataWriterListener data_writer_listener;

    // Installed on the participant with StatusMask::none() (internal/participant_listener.hpp),
    // unconditionally: the discovery callbacks are not mask-gated, and with a null observer each
    // one is a single branch at discovery rate. `~Impl` deletes the participant in its body, so
    // this member is still alive when the last callback returns.
    internal::ParticipantListener participant_listener;

    // The Fast DDS XML profiles document, copied at construction. Held as text and re-parsed per
    // endpoint by Fast DDS itself: `get_*_qos_from_xml` registers nothing process-wide, which is
    // what lets two instances in one process carry different documents under the SAME profile
    // names (spec §4 clause 3). Empty means "Fletcher's built-in profile everywhere".
    std::string document;

    // Resolve writer QoS for a topic: the topic-named profile, then `fletcher_writer`, then
    // Fletcher's built-in. By VALUE, not by reference: each call parses the document afresh, and
    // a resolved profile is the whole QoS for that endpoint rather than an overlay on a cached
    // one (internal/profile_document.hpp).
    DataWriterQos ResolveWriterQos(const std::string& name) const {
        return internal::ResolveWriterQos(*publisher, document, name);
    }

    DataReaderQos ResolveReaderQos(const std::string& name) const {
        return internal::ResolveReaderQos(*subscriber, document, name);
    }

    // Opens the topic's schema channel if it has none. Called with `mu` held exclusively.
    //
    // Resolved on the spot when this provider published the topic; otherwise fed by a __schema
    // reader whose listener resolves the channel on a Fast DDS thread. That handoff uses the
    // channel's OWN mutex, NOT `mu`: if it took `mu` it would invert with an application thread
    // that holds `mu` while inside a Fast DDS API (create_datareader, etc.), which holds Fast
    // DDS' internal subscriber mutex → deadlock. Keeping it off `mu` is what lets `mu` be held
    // across the Fast DDS calls below.
    void EnsureSchemaChannel(TopicState& ts, const std::string& name) {
        if (ts.schema_channel) return;

        auto chan = std::make_shared<internal::SchemaChannel>();
        if (ts.schema) {
            // Publisher-side / cached: nothing to wait for. Resolved here rather than by the
            // caller, because a channel this function has not created yet cannot be resolved.
            chan->Resolve(MakeSharedSchema(OwnedSchema::DeepCopy(ts.schema.get())));
            ts.schema_channel = std::move(chan);
            return;
        }

        OpenSchemaReader(ts, name, chan);
        ts.schema_channel = std::move(chan);
    }

    // The __schema reader and listener that feed `chan`. Called with `mu` held exclusively;
    // throws with `ts` untouched if either endpoint cannot be created. See EnsureSchemaChannel
    // for why holding `mu` across these Fast DDS calls is safe.
    void OpenSchemaReader(TopicState& ts, const std::string& name,
                          const std::shared_ptr<internal::SchemaChannel>& chan) {
        std::string schema_name = name + "/__schema";
        if (!ts.schema_topic) {
            ts.schema_topic = participant->create_topic(
                schema_name, schema_type_support.get_type_name(), TOPIC_QOS_DEFAULT);
            if (!ts.schema_topic)
                throw PubSubError(PubSubStatus::kTransportFailure,
                                  "FastDDS: failed to create schema topic: " + schema_name);
        }

        // The lambda captures the channel and nothing else: what the schema is handed on to is the
        // channel's continuation, installed by Subscribe, so this listener outlives any one data
        // subscription without pointing at it.
        auto schema_listener = std::make_unique<internal::SchemaListener>(
            [chan](SharedSchema sch) { chan->Resolve(std::move(sch)); }, status_listener);
        DataReader* schema_reader = subscriber->create_datareader(
            ts.schema_topic, internal::MakeSchemaChannelReaderQos(), schema_listener.get(),
            internal::SchemaReaderStatusMask());
        if (!schema_reader)
            throw PubSubError(PubSubStatus::kTransportFailure,
                              "FastDDS: failed to create schema DataReader for: " + schema_name);

        // Stored only now, so a throw above leaves nothing half-open for a retry to trip on.
        ts.schema_listener = std::move(schema_listener);
        ts.schema_reader = schema_reader;
    }

    // Puts a kept watch's channel back after a released data subscription took the schema reader
    // down with it, and opens a fresh reader for it when the schema is still pending. Called
    // WITHOUT `mu` held, after both readers are gone.
    void RearmSchemaWatch(const std::string& name, std::shared_ptr<internal::SchemaChannel> chan) {
        // The continuation points at the data listener that dies with the subscription being
        // released. Dropping it HERE is safe and nowhere else is: the schema reader whose listener
        // could have run it is already deleted, and the new one below does not exist yet. Getting
        // this window wrong is what crashed the branch this is ported from (SEH 0xc0000005 — a
        // kept continuation pointing at a destroyed data listener).
        chan->Withdraw();

        std::lock_guard lock(mu);
        auto it = topics.find(name);
        // Released, the watch cleared, or a Subscribe raced in and opened a channel of its own
        // while the caller was unlocked: end the orphan so a waiter reads kSubscriptionEnded
        // instead of waiting forever on a channel nothing feeds any more.
        if (it == topics.end() || !it->second.schema_watch || it->second.schema_channel) {
            chan->Break();
            return;
        }
        SharedSchema already;
        if (chan->arrival.Wait(std::chrono::milliseconds(0), &already) != PubSubStatus::kOk) {
            // Still pending, so it needs a reader again. A channel that has its schema does not:
            // it ignores a second Resolve, so a new reader could only re-deliver what the arrival
            // already holds.
            try {
                OpenSchemaReader(it->second, name, chan);
            } catch (const std::exception& e) {
                // The release has already happened, so this is reported THROUGH the arrival
                // rather than out of it — and as a fault, not Break()'s kSubscriptionEnded: a
                // waiter must not read a transport failure as a normal release.
                chan->Fail(PubSubStatus::kTransportFailure, e.what());
                // The arrival has failed, so nothing is watching any more: a flag left set here
                // would have every later Unsubscribe re-arm a __schema reader for nobody.
                it->second.schema_watch = false;
                return;
            }
        }
        it->second.schema_channel = std::move(chan);
    }

    // Teardown lives here, not in ~FastDDSPubSubProvider, so a constructor that throws part-way
    // still releases whatever it had already created — a throwing constructor means the outer
    // destructor never runs, but impl_ still unwinds.
    ~Impl() {
        if (!participant) return;

        for (auto& [name, ts] : topics) {
            // Delete the schema reader first: it stops the schema listener (which
            // resolves the schema channel) before the rest is torn down.
            if (ts.schema_reader) subscriber->delete_datareader(ts.schema_reader);
            if (ts.schema_writer) publisher->delete_datawriter(ts.schema_writer);
            if (ts.writer) publisher->delete_datawriter(ts.writer);
            if (ts.reader) subscriber->delete_datareader(ts.reader);
            if (ts.schema_topic) participant->delete_topic(ts.schema_topic);
            if (ts.topic) participant->delete_topic(ts.topic);
        }
        topics.clear();

        if (publisher) participant->delete_publisher(publisher);
        if (subscriber) participant->delete_subscriber(subscriber);

        DomainParticipantFactory::get_instance()->delete_participant(participant);
    }
};

// -----------------------------------------------------------------------
// Construction / destruction
// -----------------------------------------------------------------------

// A registry factory is handed a `ProviderConfig` and nothing else, so a provider reached through
// one observes no statuses. Passing a listener means constructing the provider directly.
void RegisterFastDDSProvider(ProviderRegistry& registry) {
    registry.Register("fastdds", [](const ProviderConfig& config) {
        return std::make_shared<FastDDSPubSubProvider>(config);
    });
}

// The bound `max_payload_bytes == 0` (unset, spec §4.1) resolves to. Bit-for-bit the retired
// options struct's default, and deliberately so: the bound is part of the registered DDS type
// name, so a different number silently stops endpoints discovering each other (locked
// decision 13).
namespace {
constexpr uint32_t kDefaultPayloadBytes = 64 * 1024;
}  // namespace

FastDDSPubSubProvider::FastDDSPubSubProvider(const ProviderConfig& config)
    : FastDDSPubSubProvider(config, nullptr) {}

FastDDSPubSubProvider::FastDDSPubSubProvider(const ProviderConfig& config,
                                             FastDDSStatusListener* status_listener)
    : impl_(std::make_unique<Impl>(status_listener)) {
    impl_->document = config.document;

    // Everything the document decides about the PARTICIPANT — the payload bound, the anchor's
    // `fletcher.*` properties, the domain — is settled below, before the participant exists
    // (rung-2, spec §5.1). It is not the whole document: the misplaced-`fletcher.*` refusal needs
    // a Publisher and a Subscriber and so runs at the end of this constructor, and for a profile
    // named after a topic it runs on that topic's first endpoint, out of `Publish` / `Subscribe`.
    // So a constructed provider is a provider whose participant configuration is good, not one
    // whose every profile has been read (review 4c F8; the same list is in the public header).
    // Refusals are `kInvalidArgument`: reached through a factory, a `std::invalid_argument` would
    // arrive at the caller as `kInternal`, which tells an operator nothing.
    const uint32_t bound =
        config.max_payload_bytes == 0 ? kDefaultPayloadBytes : config.max_payload_bytes;
    if (!IsPayloadBound(bound)) {
        throw PubSubError(PubSubStatus::kInvalidArgument,
                          "FastDDS: max_payload_bytes " + std::to_string(bound) +
                              " cannot bound a payload; it must be a multiple of 4 between " +
                              std::to_string(kMinPayloadBytes) + " and " +
                              std::to_string(kMaxPayloadBytes));
    }
    impl_->payload_bytes = bound;

    DomainParticipantQos pqos;
    internal::FletcherProperties properties;
    internal::ResolveParticipantQos(impl_->document, config.domain_id, pqos, properties);

    // What the bound costs against the caller's limits is Fast DDS's call, not checked here.
    if (properties.loan_publish) {
        impl_->sample_writer = std::make_unique<internal::LoanableSampleWriter>(bound);
    } else {
        impl_->sample_writer = std::make_unique<internal::SampleWriter>();
    }

    // StatusMask::none() is load-bearing, not tidiness: a participant listener that holds the
    // `data_on_readers` bit is handed every reader's data INSTEAD of the reader's own listener, so
    // the default all() here would silently take over the data path
    // (internal/participant_listener.hpp has the source citations). Discovery is dispatched
    // regardless of the mask, which is why none() still delivers it.
    impl_->participant = DomainParticipantFactory::get_instance()->create_participant(
        config.domain_id, pqos, &impl_->participant_listener, StatusMask::none());
    if (!impl_->participant)
        throw PubSubError(PubSubStatus::kTransportFailure,
                          "FastDDS: failed to create DomainParticipant");

    impl_->type_support.reset(new internal::FletcherSamplePubSubType(bound));
    if (impl_->type_support.register_type(impl_->participant) != RETCODE_OK)
        throw PubSubError(PubSubStatus::kTransportFailure,
                          "FastDDS: failed to register the data type");

    impl_->schema_type_support.reset(new internal::RawBytesPubSubType(properties.max_schema_bytes));
    if (impl_->schema_type_support.register_type(impl_->participant) != RETCODE_OK)
        throw PubSubError(PubSubStatus::kTransportFailure,
                          "FastDDS: failed to register the schema type");

    impl_->publisher = impl_->participant->create_publisher(PUBLISHER_QOS_DEFAULT);
    if (!impl_->publisher)
        throw PubSubError(PubSubStatus::kTransportFailure, "FastDDS: failed to create Publisher");

    impl_->subscriber = impl_->participant->create_subscriber(SUBSCRIBER_QOS_DEFAULT);
    if (!impl_->subscriber)
        throw PubSubError(PubSubStatus::kTransportFailure, "FastDDS: failed to create Subscriber");

    // A `fletcher.*` property in a WRITER or READER profile is read by nobody, so it is refused
    // rather than left inert (review 4a F5). The two role profiles are checked here, as late as
    // the Publisher and Subscriber allow and still before any endpoint exists; a profile named
    // after a topic is checked when that topic is created, which is the first moment its name is
    // known (internal/profile_document.hpp). `~Impl` releases what this constructor already
    // built when this throws.
    internal::RefuseMisplacedFletcherPropertiesInRoleProfiles(*impl_->publisher, *impl_->subscriber,
                                                              impl_->document);
}

// Destruction precondition (issue #63): no thread may be executing or about to
// enter a public provider API on this instance, and no provider callback may
// still be in flight if that callback can re-enter this provider. Teardown
// invalidates all TopicState pointers; it is NOT a synchronization boundary for
// concurrent use of the provider object.
//
// Teardown deliberately does NOT take impl_->mu (HARD-4 locked decision #4).
// DDS deletion calls such as delete_datareader() may block waiting for in-flight
// listener callbacks to finish. If teardown held impl_->mu while such a deletion
// waits, and a callback that re-enters the provider tried to take impl_->mu, the
// two would deadlock (hold-and-wait). This mirrors Unsubscribe(), which extracts
// state under the lock and deletes readers OUTSIDE it. A lock cannot cure
// use-*during*-destruction UB; the quiescence precondition above is the real
// contract. ~Impl (above) honours this — it takes no lock.
//
// Teardown lives in ~Impl so a throwing constructor still releases what it built.
FastDDSPubSubProvider::~FastDDSPubSubProvider() = default;

uint32_t FastDDSPubSubProvider::PayloadBytes() const { return impl_->payload_bytes; }

// -----------------------------------------------------------------------
// PubSubProvider interface
// -----------------------------------------------------------------------

void FastDDSPubSubProvider::CreateTopic(const std::vector<std::string>& topic_segments,
                                        OwnedSchema schema) {
    // Every seam entry point translates, so the only exception that can leave this
    // provider is a PubSubError carrying a stable number (spec §5.1).
    TranslateSeamFailure([&] {
        // The door, BEFORE any lock (spec §6 clause 6, owner ruling 2026-09-05,
        // "Re-entry is refused on every protocol"). A Fast DDS listener callback
        // runs with the RTPS reader mutex held, and this call HANGS if it is let
        // through — probed, not assumed. Refused by name instead.
        //
        // "Before any lock" is not decoration: with this check one line lower,
        // after `impl_->mu`, the refusal never runs and the call hangs on the
        // mutex exactly as it did with no door at all. That is a measured
        // mistake, not a hypothetical one.
        internal::RefuseIfInsideDeliveryOn(static_cast<const PubSubProvider*>(this), "CreateTopic");

        std::string name = internal::JoinSegments(topic_segments);
        std::lock_guard lock(impl_->mu);

        // Idempotent, mirroring the in-process reference provider
        // (InProcessPubSubProvider::CreateTopic): declaring a topic never fails on an
        // existing one. The topic state may already exist because a subscriber
        // joined first (subscriber-first) and lazily created it without a schema,
        // or because a publisher already declared it. Attach the publisher side
        // and announce the schema exactly once.
        auto& ts = impl_->topics[name];
        ts.is_publisher = true;

        // The data topic may already exist (created by a prior Subscribe); reuse it.
        if (!ts.topic) {
            ts.topic = impl_->participant->create_topic(name, impl_->type_support.get_type_name(),
                                                        TOPIC_QOS_DEFAULT);
            if (!ts.topic)
                throw PubSubError(PubSubStatus::kTransportFailure,
                                  "FastDDS: failed to create topic: " + name);
        }

        // Announce the schema on the companion __schema channel so that
        // late-joining subscribers — and a subscriber-first reader already waiting
        // on this provider — receive it via TRANSIENT_LOCAL.
        if (schema) {
            std::vector<uint8_t> ipc = SerializeSchemaIpc(schema.get());

            if (ts.schema_writer) {
                // A publisher already announced a schema for this topic. Idempotent
                // for an identical schema (fan-in / re-declaration); a different one
                // is a genuine conflict that must not be silently dropped. Compared against the
                // bytes that announcement sent, not against a fresh encode of the stored schema.
                if (ts.schema && ipc != ts.schema_ipc) {
                    throw PubSubError(
                        PubSubStatus::kSchemaConflict,
                        "FastDDS: topic already declared with a conflicting schema: " + name);
                }
                return;
            }

            // First schema announcement (possibly attaching to a topic state a
            // subscriber-first reader already created).
            std::string schema_name = name + "/__schema";
            // The __schema topic may already exist (a subscriber-first reader
            // created it to await the schema); reuse it.
            if (!ts.schema_topic) {
                ts.schema_topic = impl_->participant->create_topic(
                    schema_name, impl_->schema_type_support.get_type_name(), TOPIC_QOS_DEFAULT);
                if (!ts.schema_topic)
                    throw PubSubError(PubSubStatus::kTransportFailure,
                                      "FastDDS: failed to create schema topic: " + schema_name);
            }

            ts.schema_writer = impl_->publisher->create_datawriter(
                ts.schema_topic, internal::MakeSchemaChannelWriterQos());
            if (!ts.schema_writer)
                throw PubSubError(
                    PubSubStatus::kTransportFailure,
                    "FastDDS: failed to create schema DataWriter for: " + schema_name);

            internal::RawBytes raw;
            raw.data = std::move(ipc);
            // The one write whose failure is invisible from the outside: subscribers learn the
            // schema only from this sample, so a dropped one leaves every subscriber of this topic
            // waiting forever on a future that never resolves.
            if (ts.schema_writer->write(&raw) != RETCODE_OK) {
                // Undo the half-announcement before throwing. The caller is being told to retry,
                // and a retry short-circuits on a non-null schema_writer — so one left behind here
                // turns every later CreateTopic for this topic into a silent no-op and makes the
                // failure permanent, which is the very thing the paragraph above says must not
                // happen.
                impl_->publisher->delete_datawriter(ts.schema_writer);
                ts.schema_writer = nullptr;
                throw PubSubError(PubSubStatus::kTransportFailure,
                                  "FastDDS: failed to announce the schema for: " + name);
            }

            // Recorded only once the announcement is out, so a failed one leaves nothing behind for
            // a retry to match against and nothing for it to short-circuit on.
            ts.schema = OwnedSchema::DeepCopy(schema.get());
            ts.schema_ipc = std::move(raw.data);
        }
    });
}

void FastDDSPubSubProvider::Publish(const std::vector<std::string>& topic_segments,
                                    const RowEncoder& encoder, const Attachments& attachments) {
    // Every seam entry point translates, so the only exception that can leave this
    // provider is a PubSubError carrying a stable number (spec §5.1).
    TranslateSeamFailure([&] {
        // The door, before any lock (spec §6 clause 6, owner ruling 2026-09-05,
        // "Re-entry is refused on every protocol"). A Fast DDS listener callback
        // runs with the RTPS reader mutex held, and this call HANGS if it is let
        // through — probed, not assumed. Refused by name instead.
        internal::RefuseIfInsideDeliveryOn(static_cast<const PubSubProvider*>(this), "Publish");

        // Reused per thread: the joined name is only a lookup key and dies with the call, and a
        // fresh std::string here was a malloc and a free on every publish. Publish holds the mutex
        // shared, so a scratch buffer on the provider would be a data race; one per thread is not.
        static thread_local std::string name;
        internal::JoinSegmentsInto(name, topic_segments);

        // Shared, so publishes to different topics run concurrently. Held for the whole call, which
        // is what keeps the topic and its writer alive underneath the write.
        std::shared_lock lock(impl_->mu);

        auto it = impl_->topics.find(name);
        if (it == impl_->topics.end())
            throw PubSubError(PubSubStatus::kTopicNotDeclared, "FastDDS: unknown topic: " + name);

        auto& ts = it->second;

        // Lazily create the DataWriter on first publish. QoS is resolved from per-topic override →
        // instance default at this point. Creating it mutates the topic state, so this one step
        // needs the lock exclusively, and another thread may have won the race in between — hence
        // the re-check. Dropping the lock cannot invalidate `ts`: std::unordered_map guarantees
        // references to elements survive a rehash, only erase invalidates them, and nothing erases
        // outside the destructor.
        if (!ts.writer) {
            lock.unlock();
            {
                std::unique_lock exclusive(impl_->mu);
                if (!ts.writer) {
                    const DataWriterQos wqos = impl_->ResolveWriterQos(name);
                    ts.writer = impl_->publisher->create_datawriter(
                        ts.topic, wqos, &impl_->data_writer_listener, internal::WriterStatusMask());
                    if (!ts.writer)
                        throw PubSubError(PubSubStatus::kTransportFailure,
                                          "FastDDS: failed to create DataWriter for: " + name);
                }
            }
            lock.lock();
        }

        // Which of the two publish flows this provider uses was decided at construction from
        // loan_publish; see internal/sample_writer.hpp. Stateless either way, so one instance
        // serves every topic and every thread.
        impl_->sample_writer->Write(ts.writer, encoder, attachments);
    });
}

SubscriptionResult FastDDSPubSubProvider::Subscribe(const std::vector<std::string>& topic_segments,
                                                    SubscribeCallback callback) {
    // Every seam entry point translates, so the only exception that can leave this
    // provider is a PubSubError carrying a stable number (spec §5.1).
    return TranslateSeamFailure([&]() -> SubscriptionResult {
        // The door, before any lock (spec §6 clause 6, owner ruling 2026-09-05,
        // "Re-entry is refused on every protocol"). A Fast DDS listener callback
        // runs with the RTPS reader mutex held, and this call HANGS if it is let
        // through — probed, not assumed. Refused by name instead.
        internal::RefuseIfInsideDeliveryOn(static_cast<const PubSubProvider*>(this), "Subscribe");

        std::string name = internal::JoinSegments(topic_segments);
        std::unique_lock lock(impl_->mu);

        auto& ts = impl_->topics[name];
        if (ts.reader)
            throw PubSubError(PubSubStatus::kInvalidArgument,
                              "FastDDS: already subscribed to: " + name);

        // Data DDS topic.
        if (!ts.topic) {
            ts.topic = impl_->participant->create_topic(name, impl_->type_support.get_type_name(),
                                                        TOPIC_QOS_DEFAULT);
            if (!ts.topic)
                throw PubSubError(PubSubStatus::kTransportFailure,
                                  "FastDDS: failed to create topic: " + name);
        }

        // The schema channel: opened here, or already open from a SubscribeSchema, in which case
        // its reader keeps serving and a schema it has already delivered starts the data listener
        // off with no pre-schema buffering at all. Reused rather than replaced — a fresh channel
        // per Subscribe would strand a watch's arrival on a reader nothing feeds any more.
        impl_->EnsureSchemaChannel(ts, name);

        // Data DataReader. The schema may not be known yet (subscriber-first); the
        // listener then buffers samples until it arrives, so the callback is never
        // invoked with a null schema.
        SharedSchema initial;
        static_cast<void>(ts.schema_channel->arrival.Wait(std::chrono::milliseconds(0), &initial));
        const bool have_schema = initial != nullptr;
        const DataReaderQos rqos = impl_->ResolveReaderQos(name);

        // The reader's own QoS decides the flow; the backlog bound is its history depth.
        const int32_t backlog_bound =
            rqos.history().kind == KEEP_LAST_HISTORY_QOS && rqos.history().depth > 0
                ? rqos.history().depth
                : rqos.resource_limits().max_samples;
        // Non-positive means the reader asked for no limit — LENGTH_UNLIMITED, or a caller's -1
        // idiom, which used to reach OrderedDelivery as SIZE_MAX by sign conversion. Say
        // "unbounded" outright.
        const size_t max_queued = backlog_bound > 0 ? static_cast<size_t>(backlog_bound) : 0;
        if (internal::CanLoanSamples(rqos)) {
            ts.listener = std::make_unique<internal::LoanableDataReaderListener>(
                impl_->payload_bytes, DeliveryChannel(this, std::move(callback)),
                std::move(initial), max_queued, impl_->status_listener);
        } else {
            EPROSIMA_LOG_INFO(FLETCHER_SUBSCRIPTION,
                              "reader on '"
                                  << name
                                  << "' reads through deserialised copies: its history "
                                     "memory policy is not PREALLOCATED, so payload nodes "
                                     "cannot be read in place");
            ts.listener = std::make_unique<internal::DataReaderListener>(
                DeliveryChannel(this, std::move(callback)), std::move(initial), max_queued,
                impl_->status_listener);
        }

        if (!have_schema) {
            // Registered BEFORE the reader exists: if the schema lands between the poll above and
            // here, the continuation fires inline on an empty queue and runs no user code under
            // impl_->mu. Once the reader is live it fires from the schema listener's thread
            // instead. The handoff uses the channel's OWN mutex (captured by shared_ptr), NOT
            // impl_->mu: taking impl_->mu on a Fast DDS listener thread would invert with the
            // application thread that holds impl_->mu while inside a Fast DDS API
            // (create_datareader, etc.), which holds Fast DDS' internal subscriber mutex ->
            // deadlock. Keeping it off impl_->mu is what lets the provider lock be held across
            // the Fast DDS call below.
            ts.schema_channel->OnResolved([data_listener = ts.listener.get()](SharedSchema sch) {
                data_listener->SetSchema(std::move(sch));  // flush buffered samples
            });
        }

        ts.reader = impl_->subscriber->create_datareader(ts.topic, rqos, ts.listener.get(),
                                                         internal::ReaderStatusMask());
        if (!ts.reader) {
            // The schema side is released exactly as Unsubscribe releases it, so the watch flag
            // and the channel can never disagree: a watch keeps its channel (re-armed below,
            // which is also what Withdraws the continuation pointing at the listener about to
            // die), anything else goes with the failed subscription. Everything comes out of the
            // topic state under the lock, so this call can only ever destroy what it detached.
            const bool keep_watch = ts.schema_watch;
            DataReader* schema_reader = ts.schema_reader;
            ts.schema_reader = nullptr;
            std::unique_ptr<internal::SchemaListener> schema_listener =
                std::move(ts.schema_listener);
            std::shared_ptr<internal::SchemaChannel> chan = std::move(ts.schema_channel);
            std::unique_ptr<internal::DataReaderListenerBase> listener = std::move(ts.listener);
            lock.unlock();

            if (!keep_watch) chan->Break();
            // Outside the lock: deleting the schema reader waits out an in-flight schema delivery
            // — and the continuation it runs on `listener` — so both listeners die in the unwind
            // below with nothing still calling into them.
            if (schema_reader) impl_->subscriber->delete_datareader(schema_reader);
            if (keep_watch) impl_->RearmSchemaWatch(name, std::move(chan));

            throw PubSubError(PubSubStatus::kTransportFailure,
                              "FastDDS: failed to create DataReader for: " + name);
        }

        return {ts.schema_channel->arrival};
    });
}

void FastDDSPubSubProvider::Unsubscribe(const std::vector<std::string>& topic_segments) {
    // Every seam entry point translates, so the only exception that can leave this
    // provider is a PubSubError carrying a stable number (spec §5.1).
    TranslateSeamFailure([&] {
        // The door, before any lock (spec §6 clause 6, owner ruling 2026-09-05).
        // Issued from inside a delivery on this instance, the delete_datareader
        // below waits for the very delivery this thread is executing — a
        // self-wait, and a hang under the suite's TIMEOUT. Refused by name
        // instead -- as are the other three, at their own doors above.
        internal::RefuseIfInsideDeliveryOn(static_cast<const PubSubProvider*>(this), "Unsubscribe");

        std::string name = internal::JoinSegments(topic_segments);

        DataReader* schema_reader = nullptr;
        DataReader* data_reader = nullptr;
        std::shared_ptr<internal::SchemaChannel> chan;
        bool keep_watch = false;
        // The listeners are taken out of the topic state here, under the lock, with the readers
        // that use them, and destroyed at the end of this function — after both readers are gone.
        //
        // They used to be reset by a second lookup after the deletes, which is a use-after-free
        // waiting to happen: a Subscribe on the same topic racing this call sees `reader ==
        // nullptr`, adds a *new* listener, and the second lookup then destroys that one while its
        // reader is live. Moving them out here means this call can only ever destroy the listeners
        // it detached.
        std::unique_ptr<internal::DataReaderListenerBase> listener;
        std::unique_ptr<internal::SchemaListener> schema_listener;
        {
            std::lock_guard lock(impl_->mu);
            auto it = impl_->topics.find(name);
            if (it == impl_->topics.end()) return;

            auto& ts = it->second;
            data_reader = ts.reader;
            ts.reader = nullptr;
            listener = std::move(ts.listener);
            keep_watch = ts.schema_watch;

            // The schema side comes out with the data side — unless a watch is keeping it and
            // there is no data listener whose SetSchema the channel could still be about to call,
            // in which case this call has nothing to do with the schema side and leaves it open.
            // Moved OUT of the slot, not copied: a channel left in a topic state after this
            // function breaks it would hand the next Subscribe an arrival already at
            // kSubscriptionEnded.
            if (!keep_watch || listener) {
                schema_reader = ts.schema_reader;
                ts.schema_reader = nullptr;
                schema_listener = std::move(ts.schema_listener);
                chan = std::move(ts.schema_channel);
            }
        }

        // With no watch the channel goes with the subscription: if the schema never arrived, end
        // the arrival so a waiter wakes with kSubscriptionEnded instead of blocking forever
        // (channel's own mutex, not the provider lock). With one, the channel is put back at the
        // end of this function instead — its arrival keeps waiting, or keeps the schema it has.
        if (chan && !keep_watch) {
            chan->Break();
        }

        // Delete the readers OUTSIDE the lock: their listener callbacks (the
        // schema listener in particular) acquire the provider mutex. Deleting the
        // schema reader first waits for any in-flight schema delivery to finish.
        if (schema_reader) impl_->subscriber->delete_datareader(schema_reader);
        if (data_reader) impl_->subscriber->delete_datareader(data_reader);

        // Both readers are gone, so no callback can still be running: `listener` and
        // `schema_listener` die at the end of scope, and nothing else can be looking at them.
        // Only now — with nothing left that could still call into them — is it safe to give a
        // kept watch its channel back, because that is where its continuation is Withdrawn.
        if (chan && keep_watch) impl_->RearmSchemaWatch(name, std::move(chan));
    });
}

SchemaArrival FastDDSPubSubProvider::SubscribeSchema(
    const std::vector<std::string>& topic_segments) {
    // Every seam entry point translates, so the only exception that can leave this
    // provider is a PubSubError carrying a stable number (spec §5.1).
    return TranslateSeamFailure([&]() -> SchemaArrival {
        // The door, before any lock (spec §6 clause 6, owner ruling 2026-09-05), as at the four
        // doors above: a Fast DDS listener callback runs with the RTPS reader mutex held, and the
        // create_datareader below HANGS if it is let through. Refused by name instead.
        internal::RefuseIfInsideDeliveryOn(static_cast<const PubSubProvider*>(this),
                                           "SubscribeSchema");

        std::string name = internal::JoinSegments(topic_segments);
        std::lock_guard lock(impl_->mu);

        // Idempotent per topic: EnsureSchemaChannel opens nothing a Subscribe or an earlier watch
        // has already opened, and the flag is what makes the channel outlive a data Unsubscribe.
        // Set only after the channel exists, so a throwing EnsureSchemaChannel cannot leave a
        // watch flagged on a topic with no channel.
        auto& ts = impl_->topics[name];
        impl_->EnsureSchemaChannel(ts, name);
        ts.schema_watch = true;
        return ts.schema_channel->arrival;
    });
}

void FastDDSPubSubProvider::UnsubscribeSchema(const std::vector<std::string>& topic_segments) {
    // Every seam entry point translates, so the only exception that can leave this
    // provider is a PubSubError carrying a stable number (spec §5.1).
    TranslateSeamFailure([&] {
        // The door, before any lock (spec §6 clause 6, owner ruling 2026-09-05). Issued from
        // inside a delivery on this instance, the delete_datareader below waits for the very
        // delivery this thread is executing — a self-wait, and a hang under the suite's TIMEOUT.
        internal::RefuseIfInsideDeliveryOn(static_cast<const PubSubProvider*>(this),
                                           "UnsubscribeSchema");

        std::string name = internal::JoinSegments(topic_segments);

        DataReader* schema_reader = nullptr;
        std::shared_ptr<internal::SchemaChannel> chan;
        std::unique_ptr<internal::SchemaListener> schema_listener;
        {
            std::lock_guard lock(impl_->mu);
            auto it = impl_->topics.find(name);
            if (it == impl_->topics.end()) return;

            auto& ts = it->second;
            if (!ts.schema_watch) return;
            ts.schema_watch = false;
            // A live data subscription shares the channel; its endpoints go when that is
            // unsubscribed. Clearing the flag is the whole of this call then.
            if (ts.reader) return;
            schema_reader = ts.schema_reader;
            ts.schema_reader = nullptr;
            schema_listener = std::move(ts.schema_listener);
            chan = std::move(ts.schema_channel);
        }

        if (chan) {
            chan->Break();
        }
        // Outside the lock, like Unsubscribe: deleting the reader waits out an in-flight schema
        // delivery, and `schema_listener` dies at the end of scope, after it.
        if (schema_reader) impl_->subscriber->delete_datareader(schema_reader);
    });
}

}  // namespace fletcher
