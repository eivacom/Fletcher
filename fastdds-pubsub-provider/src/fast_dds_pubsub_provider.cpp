// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// Targets eProsima Fast DDS 2.14.x (fast-dds/2.14.3 from Conan Center).
//
// The custom TopicDataType serialises encoded row bytes + Attachments
// directly into the DDS payload buffer via WriteBuffer on publish, and
// delivers raw row bytes to the subscriber callback — no Arrow C++
// dependency.  The envelope wire format is:
//   [ROW_LEN:4][ROW_DATA][ATTACH_COUNT:4][attachments...]
// wrapped in a CDR-LE octet sequence.

#include "fletcher/fastdds_pubsub_provider/fast_dds_pubsub_provider.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/publisher/DataWriter.hpp>
#include <fastdds/dds/publisher/Publisher.hpp>
#include <fastdds/dds/subscriber/DataReader.hpp>
#include <fastdds/dds/subscriber/DataReaderListener.hpp>
#include <fastdds/dds/subscriber/SampleInfo.hpp>
#include <fastdds/dds/subscriber/Subscriber.hpp>
#include <fastdds/dds/topic/Topic.hpp>
#include <fastdds/dds/topic/TopicDataType.hpp>
#include <fastdds/dds/topic/TypeSupport.hpp>
#include <fletcher/core/envelope.hpp>
#include <fletcher/core/write_buffer.hpp>
#include <fletcher/pubsub/internal/segments.hpp>
#include <fletcher/pubsub/schema_ipc.hpp>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

using namespace eprosima::fastdds::dds;

namespace fletcher {
namespace {

// -----------------------------------------------------------------------
// Raw-bytes DDS type — used only for the companion schema topic.
// -----------------------------------------------------------------------

struct RawBytes {
    std::vector<uint8_t> data;
};

class RawBytesTopicType : public TopicDataType {
   public:
    explicit RawBytesTopicType(uint32_t max_payload) {
        setName("SchemaBytes");
        m_typeSize = 4 + 4 + max_payload;
        m_isGetKeyDefined = false;
    }

    bool serialize(void* data, eprosima::fastrtps::rtps::SerializedPayload_t* payload) override {
        auto* d = static_cast<RawBytes*>(data);
        // CDR LE encapsulation (4) + sequence header (4) + data.
        uint32_t len = static_cast<uint32_t>(d->data.size());
        uint32_t total = 4 + 4 + len;
        if (total > payload->max_size) return false;
        payload->encapsulation = CDR_LE;
        uint8_t hdr[] = {0x00, 0x01, 0x00, 0x00};
        std::memcpy(payload->data, hdr, 4);
        std::memcpy(payload->data + 4, &len, 4);
        std::memcpy(payload->data + 8, d->data.data(), len);
        payload->length = total;
        return true;
    }

    bool deserialize(eprosima::fastrtps::rtps::SerializedPayload_t* payload, void* data) override {
        auto* d = static_cast<RawBytes*>(data);
        if (payload->length < 8) return false;
        uint32_t len = 0;
        std::memcpy(&len, payload->data + 4, 4);
        if (8 + len > payload->length) return false;
        d->data.assign(payload->data + 8, payload->data + 8 + len);
        return true;
    }

    std::function<uint32_t()> getSerializedSizeProvider(void*) override {
        uint32_t sz = static_cast<uint32_t>(m_typeSize);
        return [sz]() { return sz; };
    }

    void* createData() override { return new RawBytes(); }
    void deleteData(void* data) override { delete static_cast<RawBytes*>(data); }

    bool getKey(void*, eprosima::fastrtps::rtps::InstanceHandle_t*, bool) override { return false; }
};

// -----------------------------------------------------------------------
// Transport data — carries RowEncoder on publish, raw bytes on subscribe.
// -----------------------------------------------------------------------

struct TransportData {
    // Publish path — encoder writes row bytes directly into
    // the DDS payload buffer via FixedWriteBuffer.
    PubSubProvider::RowEncoder encoder;
    const Attachments* attachments = nullptr;

    // Subscribe path (decoded in-place by deserialize, moved by listener).
    std::vector<uint8_t> decoded_row;
    Attachments decoded_attachments;
};

// -----------------------------------------------------------------------
// DDS TopicDataType — encodes row bytes directly into DDS payload.
// -----------------------------------------------------------------------

class FletcherTopicType : public TopicDataType {
   public:
    explicit FletcherTopicType(uint32_t max_payload) {
        setName("fletcher");
        m_typeSize = 4 + 4 + max_payload;
        m_isGetKeyDefined = false;
    }

    bool serialize(void* data, eprosima::fastrtps::rtps::SerializedPayload_t* payload) override {
        auto* d = static_cast<TransportData*>(data);
        try {
            FixedWriteBuffer buf(payload->data, payload->max_size);

            // CDR little-endian encapsulation header.
            payload->encapsulation = CDR_LE;
            const uint8_t cdr_header[] = {0x00, 0x01, 0x00, 0x00};
            buf.Append(cdr_header, 4);

            // CDR octet-sequence: uint32 length placeholder.
            size_t seq_len_pos = buf.WriteLengthPlaceholder();
            size_t seq_start = buf.Position();

            // Envelope: [ROW_LEN:4][ROW_DATA][ATTACH_COUNT:4][attachments...]
            size_t row_len_pos = buf.WriteLengthPlaceholder();
            size_t row_start = buf.Position();

            // Row bytes written directly by the encoder.
            d->encoder(buf);
            buf.PatchU32(row_len_pos, static_cast<uint32_t>(buf.Position() - row_start));

            // Attachments.
            const auto& att = *d->attachments;
            buf.AppendFixed(static_cast<uint32_t>(att.size()));
            for (const auto& [key, blob] : att) {
                buf.AppendFixed(static_cast<uint32_t>(key.size()));
                buf.Append(reinterpret_cast<const uint8_t*>(key.data()), key.size());
                uint32_t blob_len = blob ? static_cast<uint32_t>(blob->size()) : 0;
                buf.AppendFixed(blob_len);
                if (blob_len > 0) buf.Append(blob->data(), blob_len);
            }

            // Patch CDR sequence length.
            buf.PatchU32(seq_len_pos, static_cast<uint32_t>(buf.Position() - seq_start));

            payload->length = static_cast<uint32_t>(buf.Position());
            return true;
        } catch (...) {
            payload->length = 0;
            return false;
        }
    }

    bool deserialize(eprosima::fastrtps::rtps::SerializedPayload_t* payload, void* data) override {
        auto* d = static_cast<TransportData*>(data);
        if (payload->length < 8) return false;

        // Skip 4-byte CDR encapsulation, read 4-byte sequence length.
        uint32_t data_size = 0;
        std::memcpy(&data_size, payload->data + 4, sizeof(data_size));
        if (8 + data_size > payload->length) return false;

        const uint8_t* ptr = payload->data + 8;
        size_t total = data_size;
        if (total < 4) return false;

        uint32_t row_len;
        std::memcpy(&row_len, ptr, 4);
        if (4 + row_len > total) return false;

        // Deliver raw row bytes — no decoding, no Arrow dependency.
        d->decoded_row.assign(ptr + 4, ptr + 4 + row_len);

        // Parse attachments in-place.
        d->decoded_attachments.clear();
        size_t pos = 4 + row_len;
        if (pos + 4 <= total) {
            uint32_t att_count;
            std::memcpy(&att_count, ptr + pos, 4);
            pos += 4;
            for (uint32_t i = 0; i < att_count; ++i) {
                if (pos + 4 > total) return false;
                uint32_t key_len;
                std::memcpy(&key_len, ptr + pos, 4);
                pos += 4;
                if (pos + key_len > total) return false;
                std::string key(reinterpret_cast<const char*>(ptr + pos), key_len);
                pos += key_len;
                if (pos + 4 > total) return false;
                uint32_t blob_len;
                std::memcpy(&blob_len, ptr + pos, 4);
                pos += 4;
                if (pos + blob_len > total) return false;
                auto blob =
                    std::make_shared<const std::vector<uint8_t>>(ptr + pos, ptr + pos + blob_len);
                pos += blob_len;
                d->decoded_attachments[std::move(key)] = std::move(blob);
            }
        }
        return true;
    }

    std::function<uint32_t()> getSerializedSizeProvider(void* /*data*/) override {
        uint32_t sz = static_cast<uint32_t>(m_typeSize);
        return [sz]() { return sz; };
    }

    void* createData() override { return new TransportData(); }

    void deleteData(void* data) override { delete static_cast<TransportData*>(data); }

    bool getKey(void* /*data*/, eprosima::fastrtps::rtps::InstanceHandle_t* /*handle*/,
                bool /*force_md5*/) override {
        return false;
    }
};

// -----------------------------------------------------------------------
// DataReaderListener — delivers raw row bytes to subscriber callback.
// -----------------------------------------------------------------------

class SubscriptionListener : public DataReaderListener {
   public:
    SubscriptionListener(PubSubProvider::SubscribeCallback cb, SharedSchema schema)
        : callback_(std::move(cb)), schema_(std::move(schema)), schema_ready_(schema_ != nullptr) {}

    void on_data_available(DataReader* reader) override {
        TransportData data;
        SampleInfo info;
        while (reader->take_next_sample(&data, &info) == ReturnCode_t::RETCODE_OK) {
            if (!info.valid_data) continue;
            bool buffered = false;
            SharedSchema schema;
            {
                std::lock_guard<std::mutex> lk(mu_);
                if (!schema_ready_) {
                    // Hold the sample until the schema arrives, so the callback
                    // is never invoked with a null schema (subscriber-first
                    // ordering: a data sample can arrive before the schema does).
                    pending_.push_back(
                        {std::move(data.decoded_row), std::move(data.decoded_attachments)});
                    buffered = true;
                } else {
                    schema = schema_;
                }
            }
            if (buffered) continue;
            callback_(data.decoded_row.data(), data.decoded_row.size(), schema,
                      std::move(data.decoded_attachments));
        }
    }

    // Supplies the schema once known (from the companion __schema channel),
    // then flushes buffered samples in order. Must run OUTSIDE any provider
    // lock — it invokes the user callback, which may call back into the
    // provider.
    void SetSchema(SharedSchema schema) {
        std::vector<PendingSample> flush;
        SharedSchema sch;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (schema_ready_) return;
            schema_ = std::move(schema);
            schema_ready_ = true;
            sch = schema_;
            flush = std::move(pending_);
            pending_.clear();
        }
        for (auto& s : flush) {
            callback_(s.row.data(), s.row.size(), sch, std::move(s.att));
        }
    }

   private:
    struct PendingSample {
        std::vector<uint8_t> row;
        Attachments att;
    };
    PubSubProvider::SubscribeCallback callback_;
    std::mutex mu_;
    SharedSchema schema_;
    bool schema_ready_ = false;
    std::vector<PendingSample> pending_;
};

// Per-subscription schema handoff. The promise is resolved by the SchemaListener
// (on a FastDDS thread) when the companion __schema sample arrives; the caller
// gets the shared_future. Guarded by its OWN mutex — NEVER the provider mutex —
// so this FastDDS-thread callback can never contend with the provider lock the
// application thread holds while inside a FastDDS API (which would invert with
// FastDDS' internal subscriber mutex and deadlock).
struct SchemaChannel {
    std::mutex m;
    std::promise<SharedSchema> promise;
    std::shared_future<SharedSchema> future;
    bool resolved = false;

    void Resolve(SharedSchema schema) {
        std::lock_guard<std::mutex> lk(m);
        if (resolved) return;
        promise.set_value(std::move(schema));
        resolved = true;
    }
    void Break(std::exception_ptr error) {
        std::lock_guard<std::mutex> lk(m);
        if (resolved) return;
        promise.set_exception(std::move(error));
        resolved = true;
    }
};

// DataReaderListener for the companion __schema topic. Fires once when the
// retained schema sample arrives and forwards the deserialised schema to the
// callback installed by Subscribe (which resolves the subscription's schema
// future and flushes buffered data samples).
class SchemaListener : public DataReaderListener {
   public:
    explicit SchemaListener(std::function<void(SharedSchema)> on_schema)
        : on_schema_(std::move(on_schema)) {}

    void on_data_available(DataReader* reader) override {
        RawBytes raw;
        SampleInfo info;
        while (reader->take_next_sample(&raw, &info) == ReturnCode_t::RETCODE_OK) {
            if (!info.valid_data) continue;
            bool expected = false;
            if (fired_.compare_exchange_strong(expected, true)) {
                OwnedSchema owned = DeserializeSchemaIpc(raw.data.data(), raw.data.size());
                on_schema_(MakeSharedSchema(std::move(owned)));
            }
        }
    }

   private:
    std::function<void(SharedSchema)> on_schema_;
    std::atomic<bool> fired_{false};
};

}  // anonymous namespace

// -----------------------------------------------------------------------
// Impl — hides all Fast DDS types behind the pimpl wall.
// -----------------------------------------------------------------------

struct FastDDSPubSubProvider::Impl {
    struct TopicState {
        Topic* topic = nullptr;
        DataWriter* writer = nullptr;
        DataReader* reader = nullptr;
        std::unique_ptr<SubscriptionListener> listener;
        // Companion schema topic (publisher side).
        Topic* schema_topic = nullptr;
        DataWriter* schema_writer = nullptr;
        // Companion schema channel (subscriber side): a persistent reader +
        // listener that resolves schema_promise asynchronously when the schema
        // arrives — so Subscribe works subscriber-first (before any publisher).
        DataReader* schema_reader = nullptr;
        std::unique_ptr<SchemaListener> schema_listener;
        std::shared_ptr<SchemaChannel> schema_channel;
        // Schema (nanoarrow ArrowSchema).
        OwnedSchema schema;
        bool is_publisher = false;
    };

    uint32_t max_payload = 0;
    DomainParticipant* participant = nullptr;
    Publisher* publisher = nullptr;
    Subscriber* subscriber = nullptr;
    TypeSupport type_support;
    TypeSupport schema_type_support;
    std::mutex mu;
    std::map<std::string, TopicState> topics;

    // Provider-instance defaults, captured at construction.
    DataWriterQos default_writer_qos;
    DataReaderQos default_reader_qos;

    // Per-topic QoS overrides — keyed by joined topic name.
    std::unordered_map<std::string, DataWriterQos> topic_writer_qos;
    std::unordered_map<std::string, DataReaderQos> topic_reader_qos;

    // Resolve writer QoS for a topic: per-topic override → instance default.
    DataWriterQos ResolveWriterQos(const std::string& name) const {
        auto it = topic_writer_qos.find(name);
        if (it != topic_writer_qos.end()) {
            return it->second;
        }
        return default_writer_qos;
    }

    DataReaderQos ResolveReaderQos(const std::string& name) const {
        auto it = topic_reader_qos.find(name);
        if (it != topic_reader_qos.end()) {
            return it->second;
        }
        return default_reader_qos;
    }
};

// -----------------------------------------------------------------------
// Construction / destruction
// -----------------------------------------------------------------------

FastDDSPubSubProvider::FastDDSPubSubProvider(FastDDSProviderOptions options)
    : impl_(std::make_unique<Impl>()) {
    impl_->max_payload = options.max_payload_bytes;
    impl_->default_writer_qos = std::move(options.default_writer_qos);
    impl_->default_reader_qos = std::move(options.default_reader_qos);
    impl_->topic_writer_qos = std::move(options.topic_writer_qos);
    impl_->topic_reader_qos = std::move(options.topic_reader_qos);

    DomainParticipantQos pqos = PARTICIPANT_QOS_DEFAULT;
    pqos.name("FletcherParticipant");
    impl_->participant =
        DomainParticipantFactory::get_instance()->create_participant(options.domain_id, pqos);
    if (!impl_->participant)
        throw std::runtime_error("FastDDS: failed to create DomainParticipant");

    impl_->type_support.reset(new FletcherTopicType(options.max_payload_bytes));
    impl_->type_support.register_type(impl_->participant);

    impl_->schema_type_support.reset(new RawBytesTopicType(options.max_payload_bytes));
    impl_->schema_type_support.register_type(impl_->participant);

    impl_->publisher = impl_->participant->create_publisher(PUBLISHER_QOS_DEFAULT);
    if (!impl_->publisher) throw std::runtime_error("FastDDS: failed to create Publisher");

    impl_->subscriber = impl_->participant->create_subscriber(SUBSCRIBER_QOS_DEFAULT);
    if (!impl_->subscriber) throw std::runtime_error("FastDDS: failed to create Subscriber");
}

FastDDSPubSubProvider::~FastDDSPubSubProvider() {
    if (!impl_ || !impl_->participant) return;

    for (auto& [name, ts] : impl_->topics) {
        // Delete the schema reader first: it stops the schema listener (which
        // resolves the schema channel) before the rest is torn down.
        if (ts.schema_reader) impl_->subscriber->delete_datareader(ts.schema_reader);
        if (ts.schema_writer) impl_->publisher->delete_datawriter(ts.schema_writer);
        if (ts.writer) impl_->publisher->delete_datawriter(ts.writer);
        if (ts.reader) impl_->subscriber->delete_datareader(ts.reader);
        if (ts.schema_topic) impl_->participant->delete_topic(ts.schema_topic);
        if (ts.topic) impl_->participant->delete_topic(ts.topic);
    }
    impl_->topics.clear();

    if (impl_->publisher) impl_->participant->delete_publisher(impl_->publisher);
    if (impl_->subscriber) impl_->participant->delete_subscriber(impl_->subscriber);

    DomainParticipantFactory::get_instance()->delete_participant(impl_->participant);
}

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
    // on this provider — receive it via TRANSIENT_LOCAL. Done exactly once: a
    // repeated CreateTopic, or one issued after a Subscribe already opened the
    // __schema reader, must not create a duplicate writer.
    if (schema && !ts.schema_writer) {
        ts.schema = OwnedSchema::DeepCopy(schema.get());

        std::string schema_name = name + "/__schema";
        // The __schema topic may already exist (a subscriber-first reader
        // created it to await the schema); reuse it.
        if (!ts.schema_topic) {
            ts.schema_topic = impl_->participant->create_topic(
                schema_name, impl_->schema_type_support.get_type_name(), TOPIC_QOS_DEFAULT);
            if (!ts.schema_topic)
                throw std::runtime_error("FastDDS: failed to create schema topic: " + schema_name);
        }

        DataWriterQos wqos = DATAWRITER_QOS_DEFAULT;
        wqos.reliability().kind = RELIABLE_RELIABILITY_QOS;
        wqos.history().kind = KEEP_LAST_HISTORY_QOS;
        wqos.history().depth = 1;
        wqos.durability().kind = TRANSIENT_LOCAL_DURABILITY_QOS;

        ts.schema_writer = impl_->publisher->create_datawriter(ts.schema_topic, wqos);
        if (!ts.schema_writer)
            throw std::runtime_error("FastDDS: failed to create schema DataWriter for: " +
                                     schema_name);

        RawBytes raw;
        raw.data = SerializeSchemaIpc(schema.get());
        ts.schema_writer->write(&raw);
    }
}

void FastDDSPubSubProvider::Publish(const std::vector<std::string>& topic_segments,
                                    RowEncoder encoder, const Attachments& attachments) {
    std::string name = internal::JoinSegments(topic_segments);
    std::lock_guard lock(impl_->mu);

    auto it = impl_->topics.find(name);
    if (it == impl_->topics.end()) throw std::runtime_error("FastDDS: unknown topic: " + name);

    auto& ts = it->second;

    // Lazily create the DataWriter on first publish. QoS is resolved
    // from per-topic override → instance default at this point.
    if (!ts.writer) {
        DataWriterQos wqos = impl_->ResolveWriterQos(name);
        ts.writer = impl_->publisher->create_datawriter(ts.topic, wqos);
        if (!ts.writer)
            throw std::runtime_error("FastDDS: failed to create DataWriter for: " + name);
    }

    // Encoder writes row bytes directly into the DDS payload buffer
    // via FixedWriteBuffer — no intermediate copy.
    TransportData transport;
    transport.encoder = std::move(encoder);
    transport.attachments = &attachments;
    ts.writer->write(&transport);
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

    // Fresh schema channel for this subscription (its own mutex; see SchemaChannel).
    ts.schema_channel = std::make_shared<SchemaChannel>();
    ts.schema_channel->future = ts.schema_channel->promise.get_future().share();

    // Data DataReader. The schema may not be known yet (subscriber-first); the
    // listener then buffers samples until it arrives, so the callback is never
    // invoked with a null schema.
    SharedSchema initial =
        ts.schema ? MakeSharedSchema(OwnedSchema::DeepCopy(ts.schema.get())) : nullptr;
    ts.listener = std::make_unique<SubscriptionListener>(std::move(callback), std::move(initial));

    DataReaderQos rqos = impl_->ResolveReaderQos(name);
    ts.reader = impl_->subscriber->create_datareader(ts.topic, rqos, ts.listener.get());
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

        DataReaderQos sqos = DATAREADER_QOS_DEFAULT;
        sqos.reliability().kind = RELIABLE_RELIABILITY_QOS;
        sqos.history().kind = KEEP_LAST_HISTORY_QOS;
        sqos.history().depth = 1;
        sqos.durability().kind = TRANSIENT_LOCAL_DURABILITY_QOS;

        SubscriptionListener* data_listener = ts.listener.get();
        // The schema handoff uses the channel's OWN mutex (captured by shared_ptr),
        // NOT impl_->mu. on_schema runs on a FastDDS listener thread; if it took
        // impl_->mu it would invert with the application thread that holds
        // impl_->mu while inside a FastDDS API (create_datareader, etc.), which
        // holds FastDDS' internal subscriber mutex → deadlock. Keeping it off
        // impl_->mu means the provider lock can be held safely across FastDDS calls.
        std::shared_ptr<SchemaChannel> chan = ts.schema_channel;
        auto on_schema = [chan, data_listener](SharedSchema sch) {
            chan->Resolve(sch);                        // resolve the future (channel mutex)
            data_listener->SetSchema(std::move(sch));  // flush buffered samples
        };
        ts.schema_listener = std::make_unique<SchemaListener>(std::move(on_schema));
        ts.schema_reader =
            impl_->subscriber->create_datareader(ts.schema_topic, sqos, ts.schema_listener.get());
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
    std::shared_ptr<SchemaChannel> chan;
    {
        std::lock_guard lock(impl_->mu);
        auto it = impl_->topics.find(name);
        if (it == impl_->topics.end()) return;

        auto& ts = it->second;
        schema_reader = ts.schema_reader;
        ts.schema_reader = nullptr;
        data_reader = ts.reader;
        ts.reader = nullptr;
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

    // No callbacks can be running now; drop the listeners.
    std::lock_guard lock(impl_->mu);
    auto it = impl_->topics.find(name);
    if (it != impl_->topics.end()) {
        it->second.listener.reset();
        it->second.schema_listener.reset();
    }
}

}  // namespace fletcher
