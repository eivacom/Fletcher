// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// Targets eProsima Micro XRCE-DDS Client 2.4.x.
//
// Unlike the FastDDS provider which has per-subscription listeners, the
// XRCE-DDS client provides a single global on_topic callback per session.
// Incoming data is demultiplexed using a reader_id → topic_name map.
//
// The envelope wire format is identical to the FastDDS provider:
//   [ROW_LEN:4][ROW_DATA][ATTACH_COUNT:4][attachments...]
// using SerializeEnvelope/DeserializeEnvelope from pubsub/envelope.hpp.
//
// On the XRCE wire, the envelope is wrapped in an OMG-CDR
// `sequence<octet>` length prefix (uint32) before being handed to
// `uxr_buffer_topic`. MicroXRCEAgent's TopicPubSubType then prepends
// a 4-byte CDR-LE encapsulation header on the DDS bus, producing the
// spec-correct framing `[CDR-header :4][seq_len :4][envelope]` that
// FastDDS peers expect for an IDL `struct { sequence<octet> data; }`.

#include "fletcher/xrcedds_pubsub_provider/xrce_dds_pubsub_provider.hpp"

#include <uxr/client/client.h>

#include <atomic>
#include <bit>
#include <cstring>
#include <fletcher/core/envelope.hpp>
#include <fletcher/core/write_buffer.hpp>
#include <fletcher/pubsub/schema_ipc.hpp>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fletcher {

// The CDR `sequence<octet>` length field we prepend before
// `uxr_buffer_topic` (and strip on receive) is written via
// `std::memcpy` of a host-order `uint32_t`. Agent's CDR-LE
// encapsulation header pins the on-the-wire endianness to little.
// Fail the build loudly on a hypothetical big-endian host rather
// than producing silently corrupt wire bytes at runtime.
static_assert(std::endian::native == std::endian::little,
              "xrcedds-pubsub-provider requires a little-endian host: "
              "the CDR sequence<octet> length prefix is encoded by "
              "raw memcpy and the Agent's CDR-LE framing assumes LE.");

namespace {

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
// Impl — hides all XRCE-DDS types behind the pimpl wall.
// -----------------------------------------------------------------------

struct XrceDDSPubSubProvider::Impl {
    struct TopicState {
        // XRCE entity IDs for data topic.
        uxrObjectId participant_id{};
        uxrObjectId topic_id{};
        uxrObjectId publisher_id{};
        uxrObjectId writer_id{};
        uxrObjectId subscriber_id{};
        uxrObjectId reader_id{};

        // Companion schema topic entities.
        uxrObjectId schema_topic_id{};
        uxrObjectId schema_publisher_id{};
        uxrObjectId schema_writer_id{};
        uxrObjectId schema_subscriber_id{};
        uxrObjectId schema_reader_id{};

        OwnedSchema schema;
        SharedSchema shared_schema;  // for callback delivery
        bool is_publisher = false;
        bool has_reader = false;
        PubSubProvider::SubscribeCallback callback;
    };

    XrceConfig config;

    // XRCE session and transport.
    uxrSession session{};
    uxrUDPTransport udp_transport{};
    uxrTCPTransport tcp_transport{};
    uxrCommunication* comm = nullptr;

    // Reliable output/input streams.
    uxrStreamId reliable_out{};
    uxrStreamId reliable_in{};

    // Buffers for reliable streams.
    std::vector<uint8_t> output_buffer;
    std::vector<uint8_t> input_buffer;

    // Monotonic XRCE object ID counter (per type).
    uint16_t next_id = 1;

    // Recursive because OnTopic, invoked from inside uxr_run_session_*
    // calls, also wants to lock — and the calling API methods already
    // hold the lock when they pump the session for their own status
    // replies. The same coarse lock serialises all uxr_* session access
    // between API-method callers and the background run-loop below,
    // closing the issue-#41 race where the run-loop swallowed status
    // replies the API method was waiting for.
    std::recursive_mutex mu;
    std::map<std::string, TopicState> topics;

    // Demux: datareader object_id.id → topic name.
    std::map<uint16_t, std::string> reader_to_topic;

    // Background run-loop.
    std::atomic<bool> running{false};
    std::thread run_thread;

    uint16_t AllocId() { return next_id++; }

    // Static callback — dispatches to per-topic subscriber.
    static void OnTopic(uxrSession* /*session*/, uxrObjectId object_id, uint16_t /*request_id*/,
                        uxrStreamId /*stream_id*/, struct ucdrBuffer* ub, uint16_t length,
                        void* args);
};

void XrceDDSPubSubProvider::Impl::OnTopic(uxrSession* /*session*/, uxrObjectId object_id,
                                          uint16_t /*request_id*/, uxrStreamId /*stream_id*/,
                                          struct ucdrBuffer* ub, uint16_t length, void* args) {
    auto* impl = static_cast<Impl*>(args);
    std::lock_guard lock(impl->mu);

    auto rit = impl->reader_to_topic.find(object_id.id);
    if (rit == impl->reader_to_topic.end()) return;

    auto tit = impl->topics.find(rit->second);
    if (tit == impl->topics.end() || !tit->second.callback) return;

    // Read the payload from the ucdrBuffer. MicroXRCEAgent strips the
    // CDR encapsulation header, so the first 4 bytes here are the OMG
    // CDR `sequence<octet>` length field that other DDS peers (e.g.
    // FastDDSPubSubProvider) put on the DDS bus. Skip + validate before
    // decoding the envelope.
    std::vector<uint8_t> payload(length);
    ucdr_deserialize_array_uint8_t(ub, payload.data(), length);

    if (payload.size() < 4) return;
    uint32_t seq_len = 0;
    std::memcpy(&seq_len, payload.data(), 4);
    if (static_cast<size_t>(4) + seq_len > payload.size()) return;

    auto envelope = DeserializeEnvelope(payload.data() + 4, seq_len);
    tit->second.callback(envelope.row.data(), envelope.row.size(), tit->second.shared_schema,
                         std::move(envelope.attachments));
}

// -----------------------------------------------------------------------
// Construction / destruction
// -----------------------------------------------------------------------

XrceDDSPubSubProvider::XrceDDSPubSubProvider(const XrceConfig& config)
    : impl_(std::make_unique<Impl>()) {
    impl_->config = config;

    // Size reliable stream buffers.
    // history must be power of 2; buffer size = MTU * history.
    uint16_t history = config.stream_history;
    size_t mtu = UXR_CONFIG_UDP_TRANSPORT_MTU;
    impl_->output_buffer.resize(mtu * history);
    impl_->input_buffer.resize(mtu * history);

    // Initialize transport.
    std::string port_str = std::to_string(config.agent_port);

    switch (config.transport) {
        case XrceTransport::kUdp:
            if (!uxr_init_udp_transport(&impl_->udp_transport, UXR_IPv4, config.agent_ip.c_str(),
                                        port_str.c_str()))
                throw std::runtime_error("XRCE: failed to init UDP transport");
            impl_->comm = &impl_->udp_transport.comm;
            break;

        case XrceTransport::kTcp:
            if (!uxr_init_tcp_transport(&impl_->tcp_transport, UXR_IPv4, config.agent_ip.c_str(),
                                        port_str.c_str()))
                throw std::runtime_error("XRCE: failed to init TCP transport");
            impl_->comm = &impl_->tcp_transport.comm;
            break;

        case XrceTransport::kSerial:
            throw std::runtime_error("XRCE: serial transport not implemented");
    }

    // Initialize session.
    uxr_init_session(&impl_->session, impl_->comm, config.session_key);
    uxr_set_topic_callback(&impl_->session, Impl::OnTopic, impl_.get());

    // uxr_create_session_retries takes a retry COUNT; each attempt waits
    // ~1000 ms internally. Convert the ms budget to a count (minimum 0 = 1 attempt).
    constexpr int kMsPerAttempt = 1000;
    size_t retries =
        static_cast<size_t>((std::max)(0, (config.connect_timeout_ms - 1) / kMsPerAttempt));
    if (!uxr_create_session_retries(&impl_->session, retries))
        throw std::runtime_error("XRCE: failed to create session (is the Agent running?)");

    // Create reliable streams.
    impl_->reliable_out = uxr_create_output_reliable_stream(
        &impl_->session, impl_->output_buffer.data(), impl_->output_buffer.size(), history);

    impl_->reliable_in = uxr_create_input_reliable_stream(
        &impl_->session, impl_->input_buffer.data(), impl_->input_buffer.size(), history);

    // Start background run-loop. Holds impl_->mu only during the
    // session pump itself and sleeps a fixed quantum between iterations
    // so concurrent API methods are guaranteed a window to acquire the
    // lock for their own create/wait sequences. std::this_thread::yield
    // was insufficient — on a fully loaded scheduler the run-loop would
    // immediately reacquire after release and starve API methods of the
    // mutex for tens of seconds. A 5 ms sleep gives subscribers ~33%
    // duty-cycle pump coverage (5 ms gap + 10 ms pump) while keeping the
    // worst-case API-thread mutex-wait below the pump interval.
    impl_->running = true;
    impl_->run_thread = std::thread([this]() {
        while (impl_->running.load(std::memory_order_relaxed)) {
            {
                std::lock_guard lock(impl_->mu);
                uxr_run_session_time(&impl_->session, static_cast<int>(impl_->config.run_loop_ms));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });
}

XrceDDSPubSubProvider::~XrceDDSPubSubProvider() {
    if (!impl_) return;

    // Stop run-loop.
    impl_->running = false;
    if (impl_->run_thread.joinable()) impl_->run_thread.join();

    // Delete session (cleans up all entities on the Agent).
    uxr_delete_session(&impl_->session);

    // Close transport.
    switch (impl_->config.transport) {
        case XrceTransport::kUdp:
            uxr_close_udp_transport(&impl_->udp_transport);
            break;
        case XrceTransport::kTcp:
            uxr_close_tcp_transport(&impl_->tcp_transport);
            break;
        case XrceTransport::kSerial:
            break;
    }
}

// -----------------------------------------------------------------------
// Helper: wait for entity creation status
// -----------------------------------------------------------------------

namespace {

void WaitForStatus(uxrSession* session, uint16_t request_id, const char* entity_desc) {
    uint8_t status = 0;
    if (!uxr_run_session_until_all_status(session, 1000, &request_id, &status, 1) ||
        (status != UXR_STATUS_OK && status != UXR_STATUS_OK_MATCHED)) {
        throw std::runtime_error(std::string("XRCE: failed to create ") + entity_desc +
                                 " (status=" + std::to_string(status) + ")");
    }
}

void WaitForStatuses(uxrSession* session, const uint16_t* requests, uint8_t* statuses, size_t count,
                     const char* desc) {
    if (!uxr_run_session_until_all_status(session, 1000, requests, statuses, count)) {
        throw std::runtime_error(std::string("XRCE: timeout waiting for ") + desc);
    }
    for (size_t i = 0; i < count; ++i) {
        if (statuses[i] != UXR_STATUS_OK && statuses[i] != UXR_STATUS_OK_MATCHED) {
            throw std::runtime_error(std::string("XRCE: failed to create ") + desc + " (status[" +
                                     std::to_string(i) + "]=" + std::to_string(statuses[i]) + ")");
        }
    }
}

}  // anonymous namespace

// -----------------------------------------------------------------------
// PubSubProvider interface
// -----------------------------------------------------------------------

void XrceDDSPubSubProvider::CreateTopic(const std::vector<std::string>& topic_segments,
                                        OwnedSchema schema) {
    std::string name = JoinSegments(topic_segments);
    std::lock_guard lock(impl_->mu);

    if (impl_->topics.count(name)) throw std::runtime_error("XRCE: topic already exists: " + name);

    auto& ts = impl_->topics[name];
    ts.is_publisher = true;

    // Allocate XRCE entity IDs.
    uint16_t base = impl_->AllocId();
    ts.participant_id = uxr_object_id(base, UXR_PARTICIPANT_ID);
    ts.topic_id = uxr_object_id(base, UXR_TOPIC_ID);
    ts.publisher_id = uxr_object_id(base, UXR_PUBLISHER_ID);
    ts.writer_id = uxr_object_id(base, UXR_DATAWRITER_ID);

    // Create participant on the configured DDS domain.
    uint16_t req_part =
        uxr_buffer_create_participant_bin(&impl_->session, impl_->reliable_out, ts.participant_id,
                                          impl_->config.domain_id, name.c_str(), UXR_REPLACE);
    WaitForStatus(&impl_->session, req_part, "participant");

    // Create topic.
    uint16_t req_topic =
        uxr_buffer_create_topic_bin(&impl_->session, impl_->reliable_out, ts.topic_id,
                                    ts.participant_id, name.c_str(), "fletcher", UXR_REPLACE);
    WaitForStatus(&impl_->session, req_topic, "topic");

    // Create publisher + data writer.
    uint16_t req_pub = uxr_buffer_create_publisher_bin(
        &impl_->session, impl_->reliable_out, ts.publisher_id, ts.participant_id, UXR_REPLACE);

    uxrQoS_t data_qos{};
    data_qos.reliability = UXR_RELIABILITY_RELIABLE;
    data_qos.durability = UXR_DURABILITY_TRANSIENT_LOCAL;
    data_qos.history = UXR_HISTORY_KEEP_ALL;
    data_qos.depth = 16;

    uint16_t req_dw =
        uxr_buffer_create_datawriter_bin(&impl_->session, impl_->reliable_out, ts.writer_id,
                                         ts.publisher_id, ts.topic_id, data_qos, UXR_REPLACE);

    uint16_t reqs[] = {req_pub, req_dw};
    uint8_t statuses[2]{};
    WaitForStatuses(&impl_->session, reqs, statuses, 2, "publisher+writer");

    // Companion __schema topic: publish schema IPC bytes so subscribers
    // can discover the schema.
    if (schema) {
        ts.schema = OwnedSchema::DeepCopy(schema.get());

        uint16_t schema_base = impl_->AllocId();
        ts.schema_topic_id = uxr_object_id(schema_base, UXR_TOPIC_ID);
        ts.schema_publisher_id = uxr_object_id(schema_base, UXR_PUBLISHER_ID);
        ts.schema_writer_id = uxr_object_id(schema_base, UXR_DATAWRITER_ID);

        std::string schema_name = name + "/__schema";

        uint16_t req_st = uxr_buffer_create_topic_bin(
            &impl_->session, impl_->reliable_out, ts.schema_topic_id, ts.participant_id,
            schema_name.c_str(), "SchemaBytes", UXR_REPLACE);
        WaitForStatus(&impl_->session, req_st, "schema topic");

        uint16_t req_sp =
            uxr_buffer_create_publisher_bin(&impl_->session, impl_->reliable_out,
                                            ts.schema_publisher_id, ts.participant_id, UXR_REPLACE);

        uxrQoS_t schema_qos{};
        schema_qos.reliability = UXR_RELIABILITY_RELIABLE;
        schema_qos.durability = UXR_DURABILITY_TRANSIENT_LOCAL;
        schema_qos.history = UXR_HISTORY_KEEP_LAST;
        schema_qos.depth = 1;

        uint16_t req_sw = uxr_buffer_create_datawriter_bin(
            &impl_->session, impl_->reliable_out, ts.schema_writer_id, ts.schema_publisher_id,
            ts.schema_topic_id, schema_qos, UXR_REPLACE);

        uint16_t schema_reqs[] = {req_sp, req_sw};
        uint8_t schema_statuses[2]{};
        WaitForStatuses(&impl_->session, schema_reqs, schema_statuses, 2,
                        "schema publisher+writer");

        // Publish schema bytes, wrapped in the CDR `sequence<octet>`
        // length prefix the Agent will forward to FastDDS peers.
        auto ipc_bytes = SerializeSchemaIpc(schema.get());
        const uint32_t ipc_len = static_cast<uint32_t>(ipc_bytes.size());
        std::vector<uint8_t> wire;
        wire.reserve(sizeof(ipc_len) + ipc_bytes.size());
        wire.resize(sizeof(ipc_len));
        std::memcpy(wire.data(), &ipc_len, sizeof(ipc_len));
        wire.insert(wire.end(), ipc_bytes.begin(), ipc_bytes.end());

        uxr_buffer_topic(&impl_->session, impl_->reliable_out, ts.schema_writer_id, wire.data(),
                         static_cast<uint32_t>(wire.size()));
        uxr_run_session_until_confirm_delivery(&impl_->session, 1000);
    }
}

void XrceDDSPubSubProvider::Publish(const std::vector<std::string>& topic_segments,
                                    RowEncoder encoder, const Attachments& attachments) {
    std::string name = JoinSegments(topic_segments);
    std::lock_guard lock(impl_->mu);

    auto it = impl_->topics.find(name);
    if (it == impl_->topics.end()) throw std::runtime_error("XRCE: unknown topic: " + name);

    auto& ts = it->second;

    // Encode row bytes into a local buffer.
    std::vector<uint8_t> row_bytes;
    VectorWriteBuffer row_buf(row_bytes);
    encoder(row_buf);

    // Serialize the full envelope.
    Envelope env;
    env.row = std::move(row_bytes);
    env.attachments = attachments;
    auto envelope = SerializeEnvelope(env);

    // Wrap the envelope in an OMG-CDR `sequence<octet>` length prefix so
    // that, once MicroXRCEAgent's TopicPubSubType prepends the CDR-LE
    // encapsulation header on the DDS side, the bytes on the bus match
    // the spec-correct format that other DDS peers (e.g. FastDDS) use.
    const uint32_t envelope_len = static_cast<uint32_t>(envelope.size());
    std::vector<uint8_t> wire;
    wire.reserve(sizeof(envelope_len) + envelope.size());
    wire.resize(sizeof(envelope_len));
    std::memcpy(wire.data(), &envelope_len, sizeof(envelope_len));
    wire.insert(wire.end(), envelope.begin(), envelope.end());

    // Write into the XRCE output stream.
    uxr_buffer_topic(&impl_->session, impl_->reliable_out, ts.writer_id, wire.data(),
                     static_cast<uint32_t>(wire.size()));
}

SubscriptionResult XrceDDSPubSubProvider::Subscribe(const std::vector<std::string>& topic_segments,
                                                    SubscribeCallback callback) {
    std::string name = JoinSegments(topic_segments);
    OwnedSchema schema;

    // --- Phase 1: try to read schema from companion topic ---
    {
        std::lock_guard lock(impl_->mu);
        auto it = impl_->topics.find(name);
        if (it != impl_->topics.end() && it->second.schema) {
            schema = OwnedSchema::DeepCopy(it->second.schema.get());
        }
    }

    if (!schema) {
        // Need to create temporary entities to read the schema topic.
        // We reuse the same participant or create a new one.
        std::lock_guard lock(impl_->mu);

        auto& ts = impl_->topics[name];

        // If no participant yet (subscriber-side), create one.
        if (ts.participant_id.type == UXR_INVALID_ID) {
            uint16_t base = impl_->AllocId();
            ts.participant_id = uxr_object_id(base, UXR_PARTICIPANT_ID);
            ts.topic_id = uxr_object_id(base, UXR_TOPIC_ID);

            uint16_t req_part = uxr_buffer_create_participant_bin(
                &impl_->session, impl_->reliable_out, ts.participant_id, impl_->config.domain_id,
                name.c_str(), UXR_REPLACE);
            WaitForStatus(&impl_->session, req_part, "subscriber participant");

            uint16_t req_topic = uxr_buffer_create_topic_bin(&impl_->session, impl_->reliable_out,
                                                             ts.topic_id, ts.participant_id,
                                                             name.c_str(), "fletcher", UXR_REPLACE);
            WaitForStatus(&impl_->session, req_topic, "subscriber topic");
        }

        // Create schema reader entities.
        uint16_t schema_base = impl_->AllocId();
        ts.schema_topic_id = uxr_object_id(schema_base, UXR_TOPIC_ID);
        ts.schema_subscriber_id = uxr_object_id(schema_base, UXR_SUBSCRIBER_ID);
        ts.schema_reader_id = uxr_object_id(schema_base, UXR_DATAREADER_ID);

        std::string schema_name = name + "/__schema";

        uint16_t req_st = uxr_buffer_create_topic_bin(
            &impl_->session, impl_->reliable_out, ts.schema_topic_id, ts.participant_id,
            schema_name.c_str(), "SchemaBytes", UXR_REPLACE);
        WaitForStatus(&impl_->session, req_st, "schema topic (sub)");

        uint16_t req_ss = uxr_buffer_create_subscriber_bin(&impl_->session, impl_->reliable_out,
                                                           ts.schema_subscriber_id,
                                                           ts.participant_id, UXR_REPLACE);

        uxrQoS_t schema_qos{};
        schema_qos.reliability = UXR_RELIABILITY_RELIABLE;
        schema_qos.durability = UXR_DURABILITY_TRANSIENT_LOCAL;
        schema_qos.history = UXR_HISTORY_KEEP_LAST;
        schema_qos.depth = 1;

        uint16_t req_sr = uxr_buffer_create_datareader_bin(
            &impl_->session, impl_->reliable_out, ts.schema_reader_id, ts.schema_subscriber_id,
            ts.schema_topic_id, schema_qos, UXR_REPLACE);

        uint16_t reqs[] = {req_ss, req_sr};
        uint8_t statuses[2]{};
        WaitForStatuses(&impl_->session, reqs, statuses, 2, "schema subscriber+reader");

        // Temporarily install a schema-reading callback.
        // We save/restore the topic callback args to pass schema data back.
        struct SchemaCapture {
            OwnedSchema* schema;
            uint16_t reader_id;
        };
        SchemaCapture capture{&schema, ts.schema_reader_id.id};

        auto schema_cb = [](uxrSession*, uxrObjectId obj_id, uint16_t, uxrStreamId,
                            struct ucdrBuffer* ub, uint16_t length, void* args) {
            auto* cap = static_cast<SchemaCapture*>(args);
            if (obj_id.id != cap->reader_id) return;
            std::vector<uint8_t> data(length);
            ucdr_deserialize_array_uint8_t(ub, data.data(), length);
            // Skip the CDR `sequence<octet>` length prefix added by
            // the publisher side (mirrors Publish/OnTopic wrapping).
            if (data.size() < 4) return;
            uint32_t seq_len = 0;
            std::memcpy(&seq_len, data.data(), 4);
            if (static_cast<size_t>(4) + seq_len > data.size()) return;
            *(cap->schema) = DeserializeSchemaIpc(data.data() + 4, seq_len);
        };

        // Temporarily swap the topic callback.
        uxr_set_topic_callback(&impl_->session, schema_cb, &capture);

        // Request schema data.
        uxrDeliveryControl delivery{};
        delivery.max_samples = 1;
        uxr_buffer_request_data(&impl_->session, impl_->reliable_out, ts.schema_reader_id,
                                impl_->reliable_in, &delivery);

        // Poll for schema (up to 5 seconds).
        constexpr int kMaxRetries = 50;
        for (int i = 0; i < kMaxRetries && !schema; ++i) {
            uxr_run_session_time(&impl_->session, 100);
        }

        // Restore the normal topic callback.
        uxr_set_topic_callback(&impl_->session, Impl::OnTopic, impl_.get());

        // Clean up temporary schema reader.
        uxr_buffer_delete_entity(&impl_->session, impl_->reliable_out, ts.schema_reader_id);
        uxr_buffer_delete_entity(&impl_->session, impl_->reliable_out, ts.schema_subscriber_id);

        if (schema) ts.schema = OwnedSchema::DeepCopy(schema.get());
    }

    // --- Phase 2: create data subscription ---
    std::lock_guard lock(impl_->mu);
    auto& ts = impl_->topics[name];

    if (ts.has_reader) throw std::runtime_error("XRCE: already subscribed to: " + name);

    // Create subscriber + data reader if needed.
    if (ts.subscriber_id.type == UXR_INVALID_ID) {
        uint16_t sub_base = impl_->AllocId();
        ts.subscriber_id = uxr_object_id(sub_base, UXR_SUBSCRIBER_ID);
        ts.reader_id = uxr_object_id(sub_base, UXR_DATAREADER_ID);

        uint16_t req_sub = uxr_buffer_create_subscriber_bin(
            &impl_->session, impl_->reliable_out, ts.subscriber_id, ts.participant_id, UXR_REPLACE);

        uxrQoS_t data_qos{};
        data_qos.reliability = UXR_RELIABILITY_RELIABLE;
        data_qos.durability = UXR_DURABILITY_TRANSIENT_LOCAL;
        data_qos.history = UXR_HISTORY_KEEP_ALL;
        data_qos.depth = 16;

        uint16_t req_dr =
            uxr_buffer_create_datareader_bin(&impl_->session, impl_->reliable_out, ts.reader_id,
                                             ts.subscriber_id, ts.topic_id, data_qos, UXR_REPLACE);

        uint16_t reqs[] = {req_sub, req_dr};
        uint8_t statuses[2]{};
        WaitForStatuses(&impl_->session, reqs, statuses, 2, "data subscriber+reader");
    }

    ts.callback = std::move(callback);
    ts.has_reader = true;
    if (ts.schema && !ts.shared_schema)
        ts.shared_schema = MakeSharedSchema(OwnedSchema::DeepCopy(ts.schema.get()));
    impl_->reader_to_topic[ts.reader_id.id] = name;

    // Request continuous data delivery.
    uxrDeliveryControl delivery{};
    delivery.max_samples = UXR_MAX_SAMPLES_UNLIMITED;
    uxr_buffer_request_data(&impl_->session, impl_->reliable_out, ts.reader_id, impl_->reliable_in,
                            &delivery);

    OwnedSchema result_schema;
    if (ts.schema) result_schema = OwnedSchema::DeepCopy(ts.schema.get());
    return {std::move(result_schema)};
}

void XrceDDSPubSubProvider::Unsubscribe(const std::vector<std::string>& topic_segments) {
    std::string name = JoinSegments(topic_segments);
    std::lock_guard lock(impl_->mu);

    auto it = impl_->topics.find(name);
    if (it == impl_->topics.end()) return;

    auto& ts = it->second;
    if (ts.has_reader) {
        uxr_buffer_cancel_data(&impl_->session, impl_->reliable_out, ts.reader_id);
        uxr_buffer_delete_entity(&impl_->session, impl_->reliable_out, ts.reader_id);
        impl_->reader_to_topic.erase(ts.reader_id.id);
        ts.has_reader = false;
        ts.callback = nullptr;
    }
}

}  // namespace fletcher
