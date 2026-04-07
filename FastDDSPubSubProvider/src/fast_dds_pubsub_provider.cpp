// Targets eProsima Fast DDS 2.14.x (fast-dds/2.14.3 from Conan Center).
//
// The custom TopicDataType (RawBytesTopicType) serialises an Envelope —
// an EncodedRow with optional key/value attachments — as a CDR-LE octet
// sequence: 4-byte encapsulation header, 4-byte uint32 length, then
// the SerializeEnvelope() payload bytes.

#include "fast_dds_pubsub_provider.hpp"

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
// Raw-bytes DDS type — transports EncodedRow as an opaque octet sequence.
// -----------------------------------------------------------------------

struct RawBytes {
    std::vector<uint8_t> data;
};

class RawBytesTopicType : public TopicDataType {
 public:
    explicit RawBytesTopicType(uint32_t max_payload) {
        setName("ArrowRow");
        // Encapsulation (4) + CDR sequence header (4) + payload.
        m_typeSize = 4 + 4 + max_payload;
        m_isGetKeyDefined = false;
    }

    bool serialize(
        void* data,
        eprosima::fastrtps::rtps::SerializedPayload_t* payload) override {
        auto* raw = static_cast<RawBytes*>(data);
        auto data_size = static_cast<uint32_t>(raw->data.size());
        uint32_t total = 4 + 4 + data_size;

        if (total > payload->max_size) return false;

        // CDR little-endian encapsulation header.
        payload->encapsulation = CDR_LE;
        payload->data[0] = 0x00;
        payload->data[1] = 0x01;
        payload->data[2] = 0x00;
        payload->data[3] = 0x00;

        // CDR octet-sequence: uint32 length followed by raw bytes.
        std::memcpy(payload->data + 4, &data_size, sizeof(data_size));
        if (data_size > 0) {
            std::memcpy(payload->data + 8, raw->data.data(), data_size);
        }
        payload->length = total;
        return true;
    }

    bool deserialize(
        eprosima::fastrtps::rtps::SerializedPayload_t* payload,
        void* data) override {
        auto* raw = static_cast<RawBytes*>(data);
        if (payload->length < 8) return false;

        // Skip 4-byte encapsulation, read 4-byte length.
        uint32_t data_size = 0;
        std::memcpy(&data_size, payload->data + 4, sizeof(data_size));
        if (8 + data_size > payload->length) return false;

        raw->data.resize(data_size);
        if (data_size > 0) {
            std::memcpy(raw->data.data(), payload->data + 8, data_size);
        }
        return true;
    }

    std::function<uint32_t()> getSerializedSizeProvider(
        void* data) override {
        auto* raw = static_cast<RawBytes*>(data);
        uint32_t sz = 4 + 4 + static_cast<uint32_t>(raw->data.size());
        return [sz]() { return sz; };
    }

    void* createData() override { return new RawBytes(); }

    void deleteData(void* data) override {
        delete static_cast<RawBytes*>(data);
    }

    bool getKey(
        void* /*data*/,
        eprosima::fastrtps::rtps::InstanceHandle_t* /*handle*/,
        bool /*force_md5*/) override {
        return false;
    }
};

// -----------------------------------------------------------------------
// DataReaderListener — dispatches received rows to a user callback.
// -----------------------------------------------------------------------

class SubscriptionListener : public DataReaderListener {
 public:
    explicit SubscriptionListener(PubSubProvider::SubscribeCallback cb)
        : callback_(std::move(cb)) {}

    void on_data_available(DataReader* reader) override {
        RawBytes raw;
        SampleInfo info;
        while (reader->take_next_sample(&raw, &info) == ReturnCode_t::RETCODE_OK) {
            if (info.valid_data) {
                callback_(DeserializeEnvelope(raw.data));
            }
        }
    }

 private:
    PubSubProvider::SubscribeCallback callback_;
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

    // --- Domain participant ---
    DomainParticipantQos pqos = PARTICIPANT_QOS_DEFAULT;
    pqos.name("ArrowRowParticipant");
    impl_->participant = DomainParticipantFactory::get_instance()
                             ->create_participant(domain_id, pqos);
    if (!impl_->participant)
        throw std::runtime_error("FastDDS: failed to create DomainParticipant");

    // --- Register the raw-bytes type ---
    impl_->type_support.reset(new RawBytesTopicType(max_payload_bytes));
    impl_->type_support.register_type(impl_->participant);

    // --- Publisher / subscriber (shared across all topics) ---
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
    std::shared_ptr<arrow::Schema> /*schema*/) {
    std::string name = JoinSegments(topic_segments);
    std::lock_guard lock(impl_->mu);

    if (impl_->topics.count(name))
        throw std::runtime_error("FastDDS: topic already exists: " + name);

    auto* topic = impl_->participant->create_topic(
        name, impl_->type_support.get_type_name(), TOPIC_QOS_DEFAULT);
    if (!topic)
        throw std::runtime_error("FastDDS: failed to create topic: " + name);

    impl_->topics[name].topic = topic;
}

void FastDDSPubSubProvider::Publish(
    const std::vector<std::string>& topic_segments,
    const Envelope& envelope) {
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

    RawBytes raw_bytes;
    raw_bytes.data = SerializeEnvelope(envelope);
    ts.writer->write(&raw_bytes);
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

    ts.listener = std::make_unique<SubscriptionListener>(std::move(callback));

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
