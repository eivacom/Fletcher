// Targets eProsima Fast DDS 2.14.x (fast-dds/2.14.3 from Conan Center).
//
// The custom TopicDataType serialises ArrowRow + Attachments directly
// into the DDS payload buffer via WriteBuffer on publish, and decodes
// directly from the DDS payload buffer on subscribe — no intermediate
// buffer copies on either path.  The envelope wire format is:
//   [ROW_LEN:4][ROW_DATA][ATTACH_COUNT:4][attachments...]
// wrapped in a CDR-LE octet sequence.

#include "fast_dds_pubsub_provider.hpp"

#include <pubsub/envelope.hpp>
#include <write_buffer.hpp>

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

#include <cstring>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>

using namespace eprosima::fastdds::dds;

namespace fletcher {
namespace {

// -----------------------------------------------------------------------
// Transport data — dual-purpose type for publish and subscribe.
//
// On publish: holds pointers to the ArrowRow/Attachments/RowCodec.
//   serialize() encodes directly into the DDS payload buffer.
// On subscribe: codec pointer is set by the listener before
//   take_next_sample; deserialize() decodes in-place from the DDS buffer.
// -----------------------------------------------------------------------

struct TransportData {
    // Publish path (set by Publish, read by serialize).
    const ArrowRow*     row = nullptr;
    const Attachments*  attachments = nullptr;
    const RowCodec*     codec = nullptr;

    // PublishDirect path — encoder writes row bytes directly into
    // the DDS payload buffer, bypassing RowCodec and ArrowRow entirely.
    PubSubProvider::RowEncoder encoder;

    // Subscribe path (decoded in-place by deserialize, moved by listener).
    ArrowRow    decoded_row;
    Attachments decoded_attachments;
};

// -----------------------------------------------------------------------
// DDS TopicDataType — encodes ArrowRow directly into DDS payload.
// -----------------------------------------------------------------------

class ArrowRowTopicType : public TopicDataType {
 public:
    explicit ArrowRowTopicType(uint32_t max_payload) {
        setName("ArrowRow");
        m_typeSize = 4 + 4 + max_payload;
        m_isGetKeyDefined = false;
    }

    bool serialize(
        void* data,
        eprosima::fastrtps::rtps::SerializedPayload_t* payload) override {
        auto* d = static_cast<TransportData*>(data);

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
        if (d->encoder) {
            // PublishDirect path — row bytes written directly by caller.
            d->encoder(buf);
        } else {
            // Publish path — encode via RowCodec from ArrowRow.
            d->codec->EncodeRow(*d->row, buf);
        }
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

        // Decode directly from the DDS payload buffer — no intermediate copy.
        const uint8_t* ptr = payload->data + 8;
        size_t total = data_size;
        if (total < 4) return false;

        uint32_t row_len;
        std::memcpy(&row_len, ptr, 4);
        if (4 + row_len > total) return false;

        d->decoded_row = d->codec->DecodeRow(ptr + 4, row_len);

        // Parse attachments in-place.
        d->decoded_attachments.clear();
        size_t pos = 4 + row_len;
        if (pos + 4 <= total) {
            uint32_t att_count;
            std::memcpy(&att_count, ptr + pos, 4);
            pos += 4;
            for (uint32_t i = 0; i < att_count && pos + 4 <= total; ++i) {
                uint32_t key_len;
                std::memcpy(&key_len, ptr + pos, 4);
                pos += 4;
                if (pos + key_len > total) break;
                std::string key(reinterpret_cast<const char*>(ptr + pos), key_len);
                pos += key_len;
                if (pos + 4 > total) break;
                uint32_t blob_len;
                std::memcpy(&blob_len, ptr + pos, 4);
                pos += 4;
                if (pos + blob_len > total) break;
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
// DataReaderListener — decodes received data and delivers ArrowRow.
// -----------------------------------------------------------------------

class SubscriptionListener : public DataReaderListener {
 public:
    SubscriptionListener(PubSubProvider::SubscribeCallback cb,
                         RowCodec* codec)
        : callback_(std::move(cb)), codec_(codec) {}

    void on_data_available(DataReader* reader) override {
        TransportData data;
        data.codec = codec_;  // stash so deserialize() can decode in-place
        SampleInfo info;
        while (reader->take_next_sample(&data, &info) == ReturnCode_t::RETCODE_OK) {
            if (!info.valid_data) continue;
            callback_(std::move(data.decoded_row),
                      std::move(data.decoded_attachments));
        }
    }

 private:
    PubSubProvider::SubscribeCallback callback_;
    RowCodec* codec_;
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
        std::unique_ptr<RowCodec> codec;
    };

    uint32_t             max_payload = 0;
    DomainParticipant*   participant = nullptr;
    Publisher*           publisher   = nullptr;
    Subscriber*          subscriber  = nullptr;
    TypeSupport          type_support;
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
    pqos.name("ArrowRowParticipant");
    impl_->participant = DomainParticipantFactory::get_instance()
                             ->create_participant(domain_id, pqos);
    if (!impl_->participant)
        throw std::runtime_error("FastDDS: failed to create DomainParticipant");

    impl_->type_support.reset(new ArrowRowTopicType(max_payload_bytes));
    impl_->type_support.register_type(impl_->participant);

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
        if (ts.writer)
            impl_->publisher->delete_datawriter(ts.writer);
        if (ts.reader)
            impl_->subscriber->delete_datareader(ts.reader);
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
// PubSubProvider interface
// -----------------------------------------------------------------------

void FastDDSPubSubProvider::CreateTopic(
    const std::vector<std::string>& topic_segments,
    std::shared_ptr<arrow::Schema> schema) {
    std::string name = JoinSegments(topic_segments);
    std::lock_guard lock(impl_->mu);

    if (impl_->topics.count(name))
        throw std::runtime_error("FastDDS: topic already exists: " + name);

    auto* topic = impl_->participant->create_topic(
        name, impl_->type_support.get_type_name(), TOPIC_QOS_DEFAULT);
    if (!topic)
        throw std::runtime_error("FastDDS: failed to create topic: " + name);

    auto& ts = impl_->topics[name];
    ts.topic = topic;
    if (schema)
        ts.codec = std::make_unique<RowCodec>(std::move(schema));
}

void FastDDSPubSubProvider::Publish(
    const std::vector<std::string>& topic_segments,
    const ArrowRow& row,
    const Attachments& attachments) {
    std::string name = JoinSegments(topic_segments);
    std::lock_guard lock(impl_->mu);

    auto it = impl_->topics.find(name);
    if (it == impl_->topics.end())
        throw std::runtime_error("FastDDS: unknown topic: " + name);

    auto& ts = it->second;
    if (!ts.codec)
        throw std::runtime_error("FastDDS: topic has no schema (cannot encode): " + name);

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

    // Set up TransportData with pointers — serialize() will encode
    // directly into the DDS payload buffer via FixedWriteBuffer.
    TransportData transport;
    transport.row = &row;
    transport.attachments = &attachments;
    transport.codec = ts.codec.get();
    ts.writer->write(&transport);
}

void FastDDSPubSubProvider::PublishDirect(
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

    // Zero-copy path: encoder writes row bytes directly into the
    // DDS payload buffer — no ArrowRow, no RowCodec, no intermediate copy.
    TransportData transport;
    transport.encoder = std::move(encoder);
    transport.attachments = &attachments;
    ts.writer->write(&transport);
}

void FastDDSPubSubProvider::Subscribe(
    const std::vector<std::string>& topic_segments,
    SubscribeCallback callback) {
    std::string name = JoinSegments(topic_segments);
    std::lock_guard lock(impl_->mu);

    auto it = impl_->topics.find(name);
    if (it == impl_->topics.end())
        throw std::runtime_error("FastDDS: unknown topic: " + name);

    auto& ts = it->second;
    if (ts.reader)
        throw std::runtime_error("FastDDS: already subscribed to: " + name);
    if (!ts.codec)
        throw std::runtime_error("FastDDS: topic has no schema (cannot decode): " + name);

    ts.listener = std::make_unique<SubscriptionListener>(
        std::move(callback), ts.codec.get());

    DataReaderQos rqos = DATAREADER_QOS_DEFAULT;
    rqos.reliability().kind = RELIABLE_RELIABILITY_QOS;
    rqos.history().kind     = KEEP_ALL_HISTORY_QOS;
    rqos.durability().kind  = TRANSIENT_LOCAL_DURABILITY_QOS;

    ts.reader = impl_->subscriber->create_datareader(
        ts.topic, rqos, ts.listener.get());
    if (!ts.reader)
        throw std::runtime_error(
            "FastDDS: failed to create DataReader for: " + name);
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
