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
//                             feeding OrderedDelivery — plus the reader statuses logged and the
//                             predicate that decides which flow a reader QoS admits
//   sample_writer.hpp         the two publish flows, SampleWriter / LoanableSampleWriter
//   data_writer_listener.hpp  the writer statuses logged, and the status masks for both ends
//   schema_channel.hpp        the __schema handoff: promise + listener
//   ordered_delivery.hpp      single-drainer FIFO preserving writer order across that handoff
//
// No Arrow C++ dependency anywhere in the path: rows arrive as encoded bytes and leave as encoded
// bytes.

#include "fletcher/fastdds_pubsub_provider/fast_dds_pubsub_provider.hpp"

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
#include <fletcher/core/write_buffer.hpp>
#include <fletcher/pubsub/internal/segments.hpp>
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
    struct TopicState {
        Topic* topic = nullptr;
        DataWriter* writer = nullptr;
        DataReader* reader = nullptr;
        std::unique_ptr<internal::DataReaderListenerBase> listener;
        // Companion schema topic (publisher side).
        Topic* schema_topic = nullptr;
        DataWriter* schema_writer = nullptr;
        // Companion schema channel (subscriber side): a persistent reader +
        // listener that resolves schema_promise asynchronously when the schema
        // arrives — so Subscribe works subscriber-first (before any publisher).
        DataReader* schema_reader = nullptr;
        std::unique_ptr<internal::SchemaListener> schema_listener;
        std::shared_ptr<internal::SchemaChannel> schema_channel;
        // Schema (nanoarrow ArrowSchema).
        OwnedSchema schema;
        // The same schema as Arrow IPC bytes, kept from the announcement so a re-declaration is a
        // byte compare rather than another encode of what was already encoded once.
        std::vector<uint8_t> schema_ipc;
        bool is_publisher = false;
    };

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

    // Which publish flow Publish uses, fixed at construction from
    // FastDDSProviderOptions::loan_publish. Stateless, so one instance serves every topic and
    // every thread.
    std::unique_ptr<internal::SampleWriterBase> sample_writer;

    // Shared by every DataWriter this provider creates; carries no per-topic state either.
    internal::DataWriterListener data_writer_listener;

    // Provider-instance defaults, captured at construction.
    DataWriterQos default_writer_qos;
    DataReaderQos default_reader_qos;

    // Per-topic QoS overrides — keyed by joined topic name.
    std::unordered_map<std::string, DataWriterQos> topic_writer_qos;
    std::unordered_map<std::string, DataReaderQos> topic_reader_qos;

    // Resolve writer QoS for a topic: per-topic override → instance default.
    const DataWriterQos& ResolveWriterQos(const std::string& name) const {
        auto it = topic_writer_qos.find(name);
        if (it != topic_writer_qos.end()) {
            return it->second;
        }
        return default_writer_qos;
    }

    const DataReaderQos& ResolveReaderQos(const std::string& name) const {
        auto it = topic_reader_qos.find(name);
        if (it != topic_reader_qos.end()) {
            return it->second;
        }
        return default_reader_qos;
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

FastDDSPubSubProvider::FastDDSPubSubProvider(FastDDSProviderOptions options)
    : impl_(std::make_unique<Impl>()) {
    impl_->default_writer_qos = std::move(options.default_writer_qos);
    impl_->default_reader_qos = std::move(options.default_reader_qos);
    impl_->topic_writer_qos = std::move(options.topic_writer_qos);
    impl_->topic_reader_qos = std::move(options.topic_reader_qos);
    // Before the participant, so an unusable bound throws having created nothing.
    if (!IsPayloadBound(options.max_payload_bytes)) {
        throw std::invalid_argument(
            "FastDDS: max_payload_bytes " + std::to_string(options.max_payload_bytes) +
            " cannot bound a payload; it must be a multiple of 4 between " +
            std::to_string(kMinPayloadBytes) + " and " + std::to_string(kMaxPayloadBytes));
    }
    const uint32_t bound = options.max_payload_bytes;
    impl_->payload_bytes = bound;

    // What the bound costs against the caller's limits is Fast DDS's call, not checked here.
    if (options.loan_publish) {
        impl_->sample_writer = std::make_unique<internal::LoanableSampleWriter>(bound);
    } else {
        impl_->sample_writer = std::make_unique<internal::SampleWriter>();
    }

    DomainParticipantQos pqos = PARTICIPANT_QOS_DEFAULT;
    pqos.name("FletcherParticipant");
    impl_->participant =
        DomainParticipantFactory::get_instance()->create_participant(options.domain_id, pqos);
    if (!impl_->participant)
        throw std::runtime_error("FastDDS: failed to create DomainParticipant");

    impl_->type_support.reset(new internal::FletcherSamplePubSubType(bound));
    if (impl_->type_support.register_type(impl_->participant) != RETCODE_OK)
        throw std::runtime_error("FastDDS: failed to register the data type");

    impl_->schema_type_support.reset(new internal::RawBytesPubSubType(options.max_schema_bytes));
    if (impl_->schema_type_support.register_type(impl_->participant) != RETCODE_OK)
        throw std::runtime_error("FastDDS: failed to register the schema type");

    impl_->publisher = impl_->participant->create_publisher(PUBLISHER_QOS_DEFAULT);
    if (!impl_->publisher) throw std::runtime_error("FastDDS: failed to create Publisher");

    impl_->subscriber = impl_->participant->create_subscriber(SUBSCRIBER_QOS_DEFAULT);
    if (!impl_->subscriber) throw std::runtime_error("FastDDS: failed to create Subscriber");
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
    std::string name = internal::JoinSegments(topic_segments);
    std::lock_guard lock(impl_->mu);

    // Idempotent, mirroring the in-process reference provider
    // (InProcessProvider::CreateTopic): declaring a topic never fails on an
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
        if (!ts.topic) throw std::runtime_error("FastDDS: failed to create topic: " + name);
    }

    // Announce the schema on the companion __schema channel so that
    // late-joining subscribers — and a subscriber-first reader already waiting
    // on this provider — receive it via TRANSIENT_LOCAL.
    if (schema) {
        std::vector<uint8_t> ipc = SerializeSchemaIpc(schema.get());

        if (ts.schema_writer) {
            // A publisher already announced a schema for this topic. Idempotent
            // for an identical schema (fan-in / re-declaration); a different one
            // is a genuine conflict that must not be silently dropped. Compared against the bytes
            // that announcement sent, not against a fresh encode of the stored schema.
            if (ts.schema && ipc != ts.schema_ipc) {
                throw std::runtime_error(
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
                throw std::runtime_error("FastDDS: failed to create schema topic: " + schema_name);
        }

        ts.schema_writer = impl_->publisher->create_datawriter(
            ts.schema_topic, internal::MakeSchemaChannelWriterQos());
        if (!ts.schema_writer)
            throw std::runtime_error("FastDDS: failed to create schema DataWriter for: " +
                                     schema_name);

        internal::RawBytes raw;
        raw.data = std::move(ipc);
        // The one write whose failure is invisible from the outside: subscribers learn the schema
        // only from this sample, so a dropped one leaves every subscriber of this topic waiting
        // forever on a future that never resolves.
        if (ts.schema_writer->write(&raw) != RETCODE_OK) {
            // Undo the half-announcement before throwing. The caller is being told to retry, and a
            // retry short-circuits on a non-null schema_writer — so one left behind here turns
            // every later CreateTopic for this topic into a silent no-op and makes the failure
            // permanent, which is the very thing the paragraph above says must not happen.
            impl_->publisher->delete_datawriter(ts.schema_writer);
            ts.schema_writer = nullptr;
            throw std::runtime_error("FastDDS: failed to announce the schema for: " + name);
        }

        // Recorded only once the announcement is out, so a failed one leaves nothing behind for a
        // retry to match against and nothing for it to short-circuit on.
        ts.schema = OwnedSchema::DeepCopy(schema.get());
        ts.schema_ipc = std::move(raw.data);
    }
}

void FastDDSPubSubProvider::Publish(const std::vector<std::string>& topic_segments,
                                    const RowEncoder& encoder, const Attachments& attachments) {
    // Reused per thread: the joined name is only a lookup key and dies with the call, and a fresh
    // std::string here was a malloc and a free on every publish. Publish holds the mutex shared, so
    // a scratch buffer on the provider would be a data race; one per thread is not.
    static thread_local std::string name;
    internal::JoinSegmentsInto(name, topic_segments);

    // Shared, so publishes to different topics run concurrently. Held for the whole call, which is
    // what keeps the topic and its writer alive underneath the write.
    std::shared_lock lock(impl_->mu);

    auto it = impl_->topics.find(name);
    if (it == impl_->topics.end()) throw std::runtime_error("FastDDS: unknown topic: " + name);

    auto& ts = it->second;

    // Lazily create the DataWriter on first publish. QoS is resolved from per-topic override →
    // instance default at this point. Creating it mutates the topic state, so this one step needs
    // the lock exclusively, and another thread may have won the race in between — hence the
    // re-check. Dropping the lock cannot invalidate `ts`: std::unordered_map guarantees references
    // to elements survive a rehash, only erase invalidates them, and nothing erases outside the
    // destructor.
    if (!ts.writer) {
        lock.unlock();
        {
            std::unique_lock exclusive(impl_->mu);
            if (!ts.writer) {
                const DataWriterQos& wqos = impl_->ResolveWriterQos(name);
                ts.writer = impl_->publisher->create_datawriter(
                    ts.topic, wqos, &impl_->data_writer_listener, internal::WriterStatusMask());
                if (!ts.writer)
                    throw std::runtime_error("FastDDS: failed to create DataWriter for: " + name);
            }
        }
        lock.lock();
    }

    // Which of the two publish flows this provider uses was decided at construction from
    // loan_publish; see internal/sample_writer.hpp. Stateless either way, so one instance serves
    // every topic and every thread.
    impl_->sample_writer->Write(ts.writer, encoder, attachments);
}

SubscriptionResult FastDDSPubSubProvider::Subscribe(const std::vector<std::string>& topic_segments,
                                                    SubscribeCallback callback) {
    std::string name = internal::JoinSegments(topic_segments);
    std::lock_guard lock(impl_->mu);

    auto& ts = impl_->topics[name];
    if (ts.reader) throw std::runtime_error("FastDDS: already subscribed to: " + name);

    // Data DDS topic.
    if (!ts.topic) {
        ts.topic = impl_->participant->create_topic(name, impl_->type_support.get_type_name(),
                                                    TOPIC_QOS_DEFAULT);
        if (!ts.topic) throw std::runtime_error("FastDDS: failed to create topic: " + name);
    }

    // Fresh schema channel for this subscription (its own mutex; see internal::SchemaChannel).
    ts.schema_channel = std::make_shared<internal::SchemaChannel>();
    ts.schema_channel->future = ts.schema_channel->promise.get_future().share();

    // Data DataReader. The schema may not be known yet (subscriber-first); the
    // listener then buffers samples until it arrives, so the callback is never
    // invoked with a null schema.
    SharedSchema initial =
        ts.schema ? MakeSharedSchema(OwnedSchema::DeepCopy(ts.schema.get())) : nullptr;
    const DataReaderQos& rqos = impl_->ResolveReaderQos(name);

    // The reader's own QoS decides the flow; the backlog bound is its history depth.
    const int32_t backlog_bound =
        rqos.history().kind == KEEP_LAST_HISTORY_QOS && rqos.history().depth > 0
            ? rqos.history().depth
            : rqos.resource_limits().max_samples;
    // Non-positive means the reader asked for no limit — LENGTH_UNLIMITED, or a caller's -1 idiom,
    // which used to reach OrderedDelivery as SIZE_MAX by sign conversion. Say "unbounded" outright.
    const size_t max_queued = backlog_bound > 0 ? static_cast<size_t>(backlog_bound) : 0;
    if (internal::CanLoanSamples(rqos)) {
        ts.listener = std::make_unique<internal::LoanableDataReaderListener>(
            impl_->payload_bytes, std::move(callback), std::move(initial), max_queued);
    } else {
        EPROSIMA_LOG_INFO(FLETCHER_SUBSCRIPTION,
                          "reader on '" << name
                                        << "' reads through deserialised copies: its history "
                                           "memory policy is not PREALLOCATED, so payload nodes "
                                           "cannot be read in place");
        ts.listener = std::make_unique<internal::DataReaderListener>(
            std::move(callback), std::move(initial), max_queued);
    }

    ts.reader = impl_->subscriber->create_datareader(ts.topic, rqos, ts.listener.get(),
                                                     internal::ReaderStatusMask());
    if (!ts.reader) throw std::runtime_error("FastDDS: failed to create DataReader for: " + name);

    if (ts.schema) {
        // Schema already known on this provider (publisher-side / cached):
        // resolve the future immediately. Non-blocking.
        ts.schema_channel->Resolve(MakeSharedSchema(OwnedSchema::DeepCopy(ts.schema.get())));
    } else {
        // Subscriber-side: acquire the schema asynchronously from the
        // companion __schema channel via a persistent reader + listener.
        // Subscribe neither blocks nor throws if no publisher exists yet —
        // the schema (and any buffered data) is delivered once one appears.
        std::string schema_name = name + "/__schema";
        if (!ts.schema_topic) {
            ts.schema_topic = impl_->participant->create_topic(
                schema_name, impl_->schema_type_support.get_type_name(), TOPIC_QOS_DEFAULT);
            if (!ts.schema_topic)
                throw std::runtime_error("FastDDS: failed to create schema topic: " + schema_name);
        }

        internal::DataReaderListenerBase* data_listener = ts.listener.get();
        // The schema handoff uses the channel's OWN mutex (captured by shared_ptr),
        // NOT impl_->mu. on_schema runs on a FastDDS listener thread; if it took
        // impl_->mu it would invert with the application thread that holds
        // impl_->mu while inside a FastDDS API (create_datareader, etc.), which
        // holds FastDDS' internal subscriber mutex → deadlock. Keeping it off
        // impl_->mu means the provider lock can be held safely across FastDDS calls.
        std::shared_ptr<internal::SchemaChannel> chan = ts.schema_channel;
        auto on_schema = [chan, data_listener](SharedSchema sch) {
            chan->Resolve(sch);                        // resolve the future (channel mutex)
            data_listener->SetSchema(std::move(sch));  // flush buffered samples
        };
        ts.schema_listener = std::make_unique<internal::SchemaListener>(std::move(on_schema));
        ts.schema_reader = impl_->subscriber->create_datareader(
            ts.schema_topic, internal::MakeSchemaChannelReaderQos(), ts.schema_listener.get(),
            internal::SchemaReaderStatusMask());
        if (!ts.schema_reader)
            throw std::runtime_error("FastDDS: failed to create schema DataReader for: " +
                                     schema_name);
    }

    return {ts.schema_channel->future};
}

void FastDDSPubSubProvider::Unsubscribe(const std::vector<std::string>& topic_segments) {
    std::string name = internal::JoinSegments(topic_segments);

    DataReader* schema_reader = nullptr;
    DataReader* data_reader = nullptr;
    std::shared_ptr<internal::SchemaChannel> chan;
    // The listeners are taken out of the topic state here, under the lock, with the readers
    // that use them, and destroyed at the end of this function — after both readers are gone.
    //
    // They used to be reset by a second lookup after the deletes, which is a use-after-free waiting
    // to happen: a Subscribe on the same topic racing this call sees `reader == nullptr`, adds a
    // *new* listener, and the second lookup then destroys that one while its reader is live. Moving
    // them out here means this call can only ever destroy the listeners it detached.
    std::unique_ptr<internal::DataReaderListenerBase> listener;
    std::unique_ptr<internal::SchemaListener> schema_listener;
    {
        std::lock_guard lock(impl_->mu);
        auto it = impl_->topics.find(name);
        if (it == impl_->topics.end()) return;

        auto& ts = it->second;
        schema_reader = ts.schema_reader;
        ts.schema_reader = nullptr;
        data_reader = ts.reader;
        ts.reader = nullptr;
        listener = std::move(ts.listener);
        schema_listener = std::move(ts.schema_listener);
        chan = ts.schema_channel;
    }

    // If the schema never arrived, break the promise so a waiting get() does
    // not block forever (channel's own mutex, not the provider lock).
    if (chan) {
        chan->Break(std::make_exception_ptr(
            std::runtime_error("FastDDS: unsubscribed before schema arrived: " + name)));
    }

    // Delete the readers OUTSIDE the lock: their listener callbacks (the
    // schema listener in particular) acquire the provider mutex. Deleting the
    // schema reader first waits for any in-flight schema delivery to finish.
    if (schema_reader) impl_->subscriber->delete_datareader(schema_reader);
    if (data_reader) impl_->subscriber->delete_datareader(data_reader);

    // Both readers are gone, so no callback can still be running: `listener` and `schema_listener`
    // die here, at the end of scope, and nothing else can be looking at them.
}

}  // namespace fletcher
