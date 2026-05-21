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

#include <fletcher/pubsub/schema_ipc.hpp>
#include <fletcher/core/envelope.hpp>
#include <fletcher/core/write_buffer.hpp>

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

#include <chrono>
#include <cstring>
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

    bool serialize(
        void* data,
        eprosima::fastrtps::rtps::SerializedPayload_t* payload) override {
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

    bool deserialize(
        eprosima::fastrtps::rtps::SerializedPayload_t* payload,
        void* data) override {
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

    bool getKey(void*, eprosima::fastrtps::rtps::InstanceHandle_t*, bool) override {
        return false;
    }
};

// -----------------------------------------------------------------------
// Transport data — carries RowEncoder on publish, raw bytes on subscribe.
// -----------------------------------------------------------------------

struct TransportData {
    // Publish path — encoder writes row bytes directly into
    // the DDS payload buffer via FixedWriteBuffer.
    PubSub::RowEncoder encoder;
    const Attachments* attachments = nullptr;

    // Subscribe path (decoded in-place by deserialize, moved by listener).
    std::vector<uint8_t> decoded_row;
    Attachments          decoded_attachments;
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

    bool serialize(
        void* data,
        eprosima::fastrtps::rtps::SerializedPayload_t* payload) override {
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

    bool deserialize(
        eprosima::fastrtps::rtps::SerializedPayload_t* payload,
        void* data) override {
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
                auto blob = std::make_shared<const std::vector<uint8_t>>(
                    ptr + pos, ptr + pos + blob_len);
                pos += blob_len;
                d->decoded_attachments[std::move(key)] = std::move(blob);
            }
        }
        return true;
    }

    std::function<uint32_t()> getSerializedSizeProvider(
        void* /*data*/) override {
        uint32_t sz = static_cast<uint32_t>(m_typeSize);
        return [sz]() { return sz; };
    }

    void* createData() override { return new TransportData(); }

    void deleteData(void* data) override {
        delete static_cast<TransportData*>(data);
    }

    bool getKey(
        void* /*data*/,
        eprosima::fastrtps::rtps::InstanceHandle_t* /*handle*/,
        bool /*force_md5*/) override {
        return false;
    }
};

// -----------------------------------------------------------------------
// DataReaderListener — delivers raw row bytes to subscriber callback.
// -----------------------------------------------------------------------

class SubscriptionListener : public DataReaderListener {
 public:
    SubscriptionListener(PubSub::SubscribeCallback cb, SharedSchema schema)
        : callback_(std::move(cb)), schema_(std::move(schema)) {}

    void on_data_available(DataReader* reader) override {
        TransportData data;
        SampleInfo info;
        while (reader->take_next_sample(&data, &info) == ReturnCode_t::RETCODE_OK) {
            if (!info.valid_data) continue;
            callback_(data.decoded_row.data(),
                      data.decoded_row.size(),
                      schema_,
                      std::move(data.decoded_attachments));
        }
    }

 private:
    PubSub::SubscribeCallback callback_;
    SharedSchema schema_;
};

// -----------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------

std::string JoinSegments(const std::vector<std::string>& segs) {
    if (segs.empty()) return {};
    std::string out = segs[0];
    for (size_t i = 1; i < segs.size(); ++i) {
        out += '/';
        out += segs[i];
    }
    return out;
}

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
        // Schema (nanoarrow ArrowSchema).
        OwnedSchema schema;
        bool is_publisher = false;
    };

    uint32_t             max_payload = 0;
    DomainParticipant*   participant = nullptr;
    Publisher*           publisher   = nullptr;
    Subscriber*          subscriber  = nullptr;
    TypeSupport          type_support;
    TypeSupport          schema_type_support;
    std::mutex           mu;
    std::map<std::string, TopicState> topics;
};

// -----------------------------------------------------------------------
// Construction / destruction
// -----------------------------------------------------------------------

FastDDSPubSubProvider::FastDDSPubSubProvider(uint32_t domain_id,
                                             uint32_t max_payload_bytes)
    : impl_(std::make_unique<Impl>()) {
    impl_->max_payload = max_payload_bytes;

    DomainParticipantQos pqos = PARTICIPANT_QOS_DEFAULT;
    pqos.name("FletcherParticipant");
    impl_->participant = DomainParticipantFactory::get_instance()
                             ->create_participant(domain_id, pqos);
    if (!impl_->participant)
        throw std::runtime_error("FastDDS: failed to create DomainParticipant");

    impl_->type_support.reset(new FletcherTopicType(max_payload_bytes));
    impl_->type_support.register_type(impl_->participant);

    impl_->schema_type_support.reset(new RawBytesTopicType(max_payload_bytes));
    impl_->schema_type_support.register_type(impl_->participant);

    impl_->publisher =
        impl_->participant->create_publisher(PUBLISHER_QOS_DEFAULT);
    if (!impl_->publisher)
        throw std::runtime_error("FastDDS: failed to create Publisher");

    impl_->subscriber =
        impl_->participant->create_subscriber(SUBSCRIBER_QOS_DEFAULT);
    if (!impl_->subscriber)
        throw std::runtime_error("FastDDS: failed to create Subscriber");
}

FastDDSPubSubProvider::~FastDDSPubSubProvider() {
    if (!impl_ || !impl_->participant) return;

    for (auto& [name, ts] : impl_->topics) {
        if (ts.schema_writer)
            impl_->publisher->delete_datawriter(ts.schema_writer);
        if (ts.writer)
            impl_->publisher->delete_datawriter(ts.writer);
        if (ts.reader)
            impl_->subscriber->delete_datareader(ts.reader);
        if (ts.schema_topic)
            impl_->participant->delete_topic(ts.schema_topic);
        if (ts.topic)
            impl_->participant->delete_topic(ts.topic);
    }
    impl_->topics.clear();

    if (impl_->publisher)
        impl_->participant->delete_publisher(impl_->publisher);
    if (impl_->subscriber)
        impl_->participant->delete_subscriber(impl_->subscriber);

    DomainParticipantFactory::get_instance()
        ->delete_participant(impl_->participant);
}

// -----------------------------------------------------------------------
// PubSub interface
// -----------------------------------------------------------------------

void FastDDSPubSubProvider::CreateTopic(
    const std::vector<std::string>& topic_segments,
    OwnedSchema schema,
    std::any /*config*/) {
    std::string name = JoinSegments(topic_segments);
    std::lock_guard lock(impl_->mu);

    if (impl_->topics.count(name))
        throw std::runtime_error("FastDDS: topic already exists: " + name);

    // Create the data topic.
    auto* topic = impl_->participant->create_topic(
        name, impl_->type_support.get_type_name(), TOPIC_QOS_DEFAULT);
    if (!topic)
        throw std::runtime_error("FastDDS: failed to create topic: " + name);

    auto& ts = impl_->topics[name];
    ts.topic = topic;
    ts.is_publisher = true;

    // Create companion __schema topic and eagerly publish the schema
    // so that late-joining subscribers receive it via TRANSIENT_LOCAL.
    if (schema) {
        ts.schema = OwnedSchema::DeepCopy(schema.get());

        std::string schema_name = name + "/__schema";
        auto* stopic = impl_->participant->create_topic(
            schema_name, impl_->schema_type_support.get_type_name(), TOPIC_QOS_DEFAULT);
        if (!stopic)
            throw std::runtime_error(
                "FastDDS: failed to create schema topic: " + schema_name);
        ts.schema_topic = stopic;

        DataWriterQos wqos = DATAWRITER_QOS_DEFAULT;
        wqos.reliability().kind = RELIABLE_RELIABILITY_QOS;
        wqos.history().kind     = KEEP_LAST_HISTORY_QOS;
        wqos.history().depth    = 1;
        wqos.durability().kind  = TRANSIENT_LOCAL_DURABILITY_QOS;

        ts.schema_writer = impl_->publisher->create_datawriter(stopic, wqos);
        if (!ts.schema_writer)
            throw std::runtime_error(
                "FastDDS: failed to create schema DataWriter for: " + schema_name);

        RawBytes raw;
        raw.data = SerializeSchemaIpc(schema.get());
        ts.schema_writer->write(&raw);
    }
}

void FastDDSPubSubProvider::Publish(
    const std::vector<std::string>& topic_segments,
    RowEncoder encoder,
    const Attachments& attachments) {
    std::string name = JoinSegments(topic_segments);
    std::lock_guard lock(impl_->mu);

    auto it = impl_->topics.find(name);
    if (it == impl_->topics.end())
        throw std::runtime_error("FastDDS: unknown topic: " + name);

    auto& ts = it->second;

    // Lazily create the DataWriter on first publish.
    if (!ts.writer) {
        DataWriterQos wqos = DATAWRITER_QOS_DEFAULT;
        wqos.reliability().kind = RELIABLE_RELIABILITY_QOS;
        wqos.history().kind     = KEEP_ALL_HISTORY_QOS;
        wqos.durability().kind  = TRANSIENT_LOCAL_DURABILITY_QOS;

        ts.writer = impl_->publisher->create_datawriter(ts.topic, wqos);
        if (!ts.writer)
            throw std::runtime_error(
                "FastDDS: failed to create DataWriter for: " + name);
    }

    // Encoder writes row bytes directly into the DDS payload buffer
    // via FixedWriteBuffer — no intermediate copy.
    TransportData transport;
    transport.encoder = std::move(encoder);
    transport.attachments = &attachments;
    ts.writer->write(&transport);
}

SubscriptionResult FastDDSPubSubProvider::Subscribe(
    const std::vector<std::string>& topic_segments,
    SubscribeCallback callback,
    std::any /*config*/) {
    std::string name = JoinSegments(topic_segments);
    OwnedSchema schema;

    // --- Phase 1: read schema from companion topic (outside lock) ---
    {
        std::lock_guard lock(impl_->mu);

        auto it = impl_->topics.find(name);
        if (it != impl_->topics.end() && it->second.schema) {
            // Publisher-side or previously resolved: schema already cached.
            schema = OwnedSchema::DeepCopy(it->second.schema.get());
        }
    }

    if (!schema) {
        // Subscriber-side: read from the companion __schema topic.
        std::string schema_name = name + "/__schema";

        // Find or create the DDS topic for the schema channel.
        auto* stopic = impl_->participant->create_topic(
            schema_name, impl_->schema_type_support.get_type_name(), TOPIC_QOS_DEFAULT);
        if (!stopic)
            throw std::runtime_error(
                "FastDDS: failed to find/create schema topic: " + schema_name);

        DataReaderQos rqos = DATAREADER_QOS_DEFAULT;
        rqos.reliability().kind = RELIABLE_RELIABILITY_QOS;
        rqos.history().kind     = KEEP_LAST_HISTORY_QOS;
        rqos.history().depth    = 1;
        rqos.durability().kind  = TRANSIENT_LOCAL_DURABILITY_QOS;

        auto* schema_reader = impl_->subscriber->create_datareader(stopic, rqos);
        if (!schema_reader)
            throw std::runtime_error(
                "FastDDS: failed to create schema DataReader for: " + schema_name);

        // Poll for the retained schema sample (TRANSIENT_LOCAL delivers it
        // once the DataWriter is matched).
        RawBytes raw;
        SampleInfo info;
        constexpr int kMaxRetries = 50;
        constexpr int kRetryMs = 100;
        bool got_schema = false;
        for (int attempt = 0; attempt < kMaxRetries; ++attempt) {
            if (schema_reader->take_next_sample(&raw, &info) ==
                    ReturnCode_t::RETCODE_OK && info.valid_data) {
                schema = DeserializeSchemaIpc(raw.data.data(), raw.data.size());
                got_schema = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(kRetryMs));
        }

        // Clean up the temporary schema reader.
        impl_->subscriber->delete_datareader(schema_reader);
        impl_->participant->delete_topic(stopic);

        if (!got_schema)
            throw std::runtime_error(
                "FastDDS: timed out waiting for schema on: " + schema_name);
    }

    // --- Phase 2: register topic and create data DataReader (under lock) ---
    std::lock_guard lock(impl_->mu);

    auto& ts = impl_->topics[name];
    if (!ts.schema && schema)
        ts.schema = OwnedSchema::DeepCopy(schema.get());

    // Create or find the data DDS topic if not already present.
    if (!ts.topic) {
        ts.topic = impl_->participant->create_topic(
            name, impl_->type_support.get_type_name(), TOPIC_QOS_DEFAULT);
        if (!ts.topic)
            throw std::runtime_error("FastDDS: failed to create topic: " + name);
    }

    if (ts.reader)
        throw std::runtime_error("FastDDS: already subscribed to: " + name);

    ts.listener = std::make_unique<SubscriptionListener>(
        std::move(callback),
        ts.schema ? MakeSharedSchema(OwnedSchema::DeepCopy(ts.schema.get()))
                  : nullptr);

    DataReaderQos rqos = DATAREADER_QOS_DEFAULT;
    rqos.reliability().kind = RELIABLE_RELIABILITY_QOS;
    rqos.history().kind     = KEEP_ALL_HISTORY_QOS;
    rqos.durability().kind  = TRANSIENT_LOCAL_DURABILITY_QOS;

    ts.reader = impl_->subscriber->create_datareader(
        ts.topic, rqos, ts.listener.get());
    if (!ts.reader)
        throw std::runtime_error(
            "FastDDS: failed to create DataReader for: " + name);

    OwnedSchema result_schema;
    if (ts.schema)
        result_schema = OwnedSchema::DeepCopy(ts.schema.get());
    return {std::move(result_schema)};
}

void FastDDSPubSubProvider::Unsubscribe(
    const std::vector<std::string>& topic_segments) {
    std::string name = JoinSegments(topic_segments);
    std::lock_guard lock(impl_->mu);

    auto it = impl_->topics.find(name);
    if (it == impl_->topics.end())
        throw std::runtime_error("FastDDS: unknown topic: " + name);

    auto& ts = it->second;
    if (ts.reader) {
        impl_->subscriber->delete_datareader(ts.reader);
        ts.reader = nullptr;
        ts.listener.reset();
    }
}

}  // namespace fletcher
