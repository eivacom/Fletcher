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
#include <exception>
#include <fletcher/core/envelope.hpp>
#include <fletcher/core/write_buffer.hpp>
#include <fletcher/pubsub/internal/segments.hpp>
#include <fletcher/pubsub/schema_ipc.hpp>
#include <future>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef FLETCHER_BUILD_TESTS
#include "internal/xrce_test_hook.hpp"
#endif

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

        // Subscriber-first support. Subscribe is non-blocking and resolves the
        // schema asynchronously through the companion __schema reader (see
        // OnTopic): schema_promise is fulfilled when the schema arrives, and
        // data that arrives before the schema is buffered in `pending` so the
        // callback is never invoked with a null schema.
        std::promise<SharedSchema> schema_promise;
        std::shared_future<SharedSchema> schema_future;
        bool schema_resolved = false;
        std::vector<Envelope> pending;
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

    // Demux for companion __schema readers: object_id.id → topic name. Kept
    // separate from reader_to_topic so OnTopic can tell a schema sample from a
    // data sample by which map the reader id lands in.
    std::map<uint16_t, std::string> schema_reader_to_topic;

    // Background run-loop.
    std::atomic<bool> running{false};
    std::thread run_thread;

    uint16_t AllocId() { return next_id++; }

    // Static callback — dispatches to per-topic subscriber.
    static void OnTopic(uxrSession* /*session*/, uxrObjectId object_id, uint16_t /*request_id*/,
                        uxrStreamId /*stream_id*/, struct ucdrBuffer* ub, uint16_t length,
                        void* args);

#ifdef FLETCHER_BUILD_TESTS
    // Test seam (issue #62 residual, HARD-4). Builds a real re-entrant-Unsubscribe
    // scenario and drives the real OnTopic() above. Defined out-of-line so it has
    // full access to Impl / TopicState / OnTopic. The free hook declared in
    // xrce_test_hook.hpp (namespace fletcher::xrce::test) cannot name this private
    // pimpl type, so it forwards here through a file-scope trampoline that
    // test_hook_registered_ wires up at static-init time (see end of file). This
    // keeps the installed public header byte-for-byte untouched.
    static xrce::test::ReentrantUnsubscribeResult RunReentrantUnsubscribeScenario();
    static const bool test_hook_registered_;
#endif
};

void XrceDDSPubSubProvider::Impl::OnTopic(uxrSession* /*session*/, uxrObjectId object_id,
                                          uint16_t /*request_id*/, uxrStreamId /*stream_id*/,
                                          struct ucdrBuffer* ub, uint16_t length, void* args) {
    auto* impl = static_cast<Impl*>(args);
    std::lock_guard lock(impl->mu);

    // Read the payload from the ucdrBuffer. MicroXRCEAgent strips the
    // CDR encapsulation header, so the first 4 bytes here are the OMG
    // CDR `sequence<octet>` length field that other DDS peers (e.g.
    // FastDDSPubSubProvider) put on the DDS bus. Skip + validate before
    // decoding.
    std::vector<uint8_t> payload(length);
    ucdr_deserialize_array_uint8_t(ub, payload.data(), length);

    if (payload.size() < 4) return;
    uint32_t seq_len = 0;
    std::memcpy(&seq_len, payload.data(), 4);
    if (static_cast<size_t>(4) + seq_len > payload.size()) return;
    const uint8_t* body = payload.data() + 4;

    // Companion __schema reader? Resolve the schema future, set the per-topic
    // shared schema, and flush any data buffered before the schema arrived
    // (subscriber-first). This runs on the same thread that already holds
    // impl->mu (the session pump), so the recursive lock above is re-entered
    // safely — there are no separate listener threads as in the FastDDS path.
    auto sit = impl->schema_reader_to_topic.find(object_id.id);
    if (sit != impl->schema_reader_to_topic.end()) {
        // Snapshot everything the callbacks need into locals BEFORE invoking any
        // user code (issue #62 residual). A callback that re-enters Unsubscribe()
        // on this topic performs an in-place TopicState reset — ts.callback =
        // nullptr; ts.pending.clear() — while leaving the map node live. The old
        // code kept iterating the live ts.pending and calling the live ts.callback
        // across that reset: the second iteration read a destroyed Envelope and
        // then invoked the now-null ts.callback (std::bad_function_call). Copying
        // to locals makes the in-flight flush independent of the live TopicState.
        // Dispatch stays under impl->mu (single recursive-mutex pump model).
        PubSubProvider::SubscribeCallback callback;
        SharedSchema schema_for_callbacks;
        std::vector<Envelope> pending;
        {
            auto tit = impl->topics.find(sit->second);
            if (tit == impl->topics.end()) return;
            auto& ts = tit->second;
            if (ts.schema_resolved) return;  // __schema is KEEP_LAST(1); ignore repeats.

            // Malformed __schema sample must not throw out of the XRCE session
            // callback thread (which could terminate the process); ignore it and
            // let the retained TRANSIENT_LOCAL/KEEP_LAST(1) sample be redelivered.
            OwnedSchema schema;
            try {
                schema = DeserializeSchemaIpc(body, seq_len);
            } catch (...) {
                return;
            }
            if (!schema) return;
            ts.schema = OwnedSchema::DeepCopy(schema.get());
            ts.shared_schema = MakeSharedSchema(std::move(schema));
            ts.schema_promise.set_value(ts.shared_schema);
            ts.schema_resolved = true;

            callback = ts.callback;
            schema_for_callbacks = ts.shared_schema;
            pending = std::move(ts.pending);
            ts.pending.clear();
        }

        // Deliver from locals only — do NOT read or write ts / ts.* past this
        // point; a re-entrant Unsubscribe may reset the live TopicState.
        if (callback) {
            for (auto& env : pending) {
                callback(env.row.data(), env.row.size(), schema_for_callbacks,
                         std::move(env.attachments));
            }
        }
        return;
    }

    // Otherwise it is a data reader.
    auto rit = impl->reader_to_topic.find(object_id.id);
    if (rit == impl->reader_to_topic.end()) return;
    auto tit = impl->topics.find(rit->second);
    if (tit == impl->topics.end() || !tit->second.callback) return;

    // Snapshot the callback and schema into locals before invoking user code
    // (issue #62 residual): a callback that re-enters Unsubscribe() resets the
    // live TopicState (ts.callback = nullptr) in place, which would otherwise
    // self-destruct the executing std::function. `envelope` is already a local.
    // Dispatch stays under impl->mu (single recursive-mutex pump model).
    PubSubProvider::SubscribeCallback callback;
    SharedSchema schema_for_callback;
    Envelope envelope;
    {
        auto& ts = tit->second;

        // DeserializeEnvelope throws std::invalid_argument on a malformed/truncated
        // sample. That must not escape the XRCE session callback thread (which could
        // tear down the session pump / process), so drop the bad sample and keep the
        // session alive. std::invalid_argument is the only type it throws; anything
        // else is unexpected and is left to propagate.
        try {
            envelope = DeserializeEnvelope(body, seq_len);
        } catch (const std::invalid_argument&) {
            return;
        }
        if (!ts.shared_schema) {
            // Subscriber-first: the schema has not arrived yet. Buffer the sample;
            // it is flushed in order when the __schema sample resolves the future.
            ts.pending.push_back(std::move(envelope));
            return;
        }
        callback = ts.callback;
        schema_for_callback = ts.shared_schema;
    }

    // Deliver from locals only — do NOT read or write ts / ts.* past this point.
    if (callback) {
        callback(envelope.row.data(), envelope.row.size(), schema_for_callback,
                 std::move(envelope.attachments));
    }
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
    std::string name = internal::JoinSegments(topic_segments);
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
    std::string name = internal::JoinSegments(topic_segments);
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
    std::string name = internal::JoinSegments(topic_segments);
    std::lock_guard lock(impl_->mu);

    auto& ts = impl_->topics[name];
    if (ts.has_reader) throw std::runtime_error("XRCE: already subscribed to: " + name);

    // Subscribe is non-blocking and never throws when no publisher exists yet
    // (subscriber-first). The schema is delivered asynchronously: data that
    // arrives before it is buffered (see OnTopic) so the first callback is
    // never invoked with a null schema. A fresh promise/future is wired up for
    // this subscription.
    ts.schema_promise = std::promise<SharedSchema>();
    ts.schema_future = ts.schema_promise.get_future().share();
    ts.schema_resolved = false;

    // If no participant yet (subscriber-side), create one + the data topic.
    // Publisher-side topics already did this in CreateTopic.
    if (ts.participant_id.type == UXR_INVALID_ID) {
        uint16_t base = impl_->AllocId();
        ts.participant_id = uxr_object_id(base, UXR_PARTICIPANT_ID);
        ts.topic_id = uxr_object_id(base, UXR_TOPIC_ID);

        uint16_t req_part = uxr_buffer_create_participant_bin(
            &impl_->session, impl_->reliable_out, ts.participant_id, impl_->config.domain_id,
            name.c_str(), UXR_REPLACE);
        WaitForStatus(&impl_->session, req_part, "subscriber participant");

        uint16_t req_topic =
            uxr_buffer_create_topic_bin(&impl_->session, impl_->reliable_out, ts.topic_id,
                                        ts.participant_id, name.c_str(), "fletcher", UXR_REPLACE);
        WaitForStatus(&impl_->session, req_topic, "subscriber topic");
    }

    if (ts.schema) {
        // Schema already known on this provider (publisher-side / cached):
        // resolve the future immediately.
        ts.shared_schema = MakeSharedSchema(OwnedSchema::DeepCopy(ts.schema.get()));
        ts.schema_promise.set_value(ts.shared_schema);
        ts.schema_resolved = true;
    } else if (ts.schema_reader_id.type == UXR_INVALID_ID) {
        // Subscriber-side: create a persistent companion __schema reader and
        // route its samples to OnTopic, which resolves the schema future when a
        // publisher announces the schema. No poll, no callback swap, no throw —
        // so a subscriber can subscribe before any publisher exists.
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

        // Route this schema reader's samples to OnTopic for async resolution.
        impl_->schema_reader_to_topic[ts.schema_reader_id.id] = name;

        // Request continuous schema delivery; the retained TRANSIENT_LOCAL
        // sample arrives once a publisher announces the schema.
        uxrDeliveryControl schema_delivery{};
        schema_delivery.max_samples = UXR_MAX_SAMPLES_UNLIMITED;
        uxr_buffer_request_data(&impl_->session, impl_->reliable_out, ts.schema_reader_id,
                                impl_->reliable_in, &schema_delivery);
    }

    // --- Data subscription ---
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
    impl_->reader_to_topic[ts.reader_id.id] = name;

    // Request continuous data delivery ONCE. A single READ_DATA with
    // max_samples=UNLIMITED establishes a standing stream on the Agent that
    // delivers samples as they arrive — including from a writer that matches
    // later (subscriber-first). It must NOT be re-issued: re-requesting makes
    // the Agent stop+restart the read, dropping samples during the gap.
    uxrDeliveryControl delivery{};
    delivery.max_samples = UXR_MAX_SAMPLES_UNLIMITED;
    uxr_buffer_request_data(&impl_->session, impl_->reliable_out, ts.reader_id, impl_->reliable_in,
                            &delivery);

    // Non-blocking: hand back the schema future. It is already satisfied for
    // publisher-side/cached topics, and resolves asynchronously otherwise.
    return {ts.schema_future};
}

void XrceDDSPubSubProvider::Unsubscribe(const std::vector<std::string>& topic_segments) {
    std::string name = internal::JoinSegments(topic_segments);
    std::lock_guard lock(impl_->mu);

    auto it = impl_->topics.find(name);
    if (it == impl_->topics.end()) return;

    auto& ts = it->second;

    // If the schema never arrived, break the promise so a consumer blocked on
    // the future wakes up instead of waiting forever (and the promise is not
    // destroyed unsatisfied).
    if (!ts.schema_resolved) {
        try {
            ts.schema_promise.set_exception(std::make_exception_ptr(
                std::runtime_error("XRCE: unsubscribed before schema arrived: " + name)));
        } catch (const std::future_error&) {
            // No future was ever handed out, or it was already satisfied.
        }
        ts.schema_resolved = true;
    }

    if (ts.schema_reader_id.type != UXR_INVALID_ID) {
        uxr_buffer_cancel_data(&impl_->session, impl_->reliable_out, ts.schema_reader_id);
        uxr_buffer_delete_entity(&impl_->session, impl_->reliable_out, ts.schema_reader_id);
        impl_->schema_reader_to_topic.erase(ts.schema_reader_id.id);
        ts.schema_reader_id.type = UXR_INVALID_ID;
    }

    // Delete the companion __schema subscriber + topic that Subscribe created
    // (from a fresh id base). Without this, repeated subscribe/unsubscribe
    // cycles leak XRCE entities on the Agent until the session is destroyed.
    if (ts.schema_subscriber_id.type != UXR_INVALID_ID) {
        uxr_buffer_delete_entity(&impl_->session, impl_->reliable_out, ts.schema_subscriber_id);
        ts.schema_subscriber_id.type = UXR_INVALID_ID;
    }
    if (ts.schema_topic_id.type != UXR_INVALID_ID) {
        uxr_buffer_delete_entity(&impl_->session, impl_->reliable_out, ts.schema_topic_id);
        ts.schema_topic_id.type = UXR_INVALID_ID;
    }

    if (ts.has_reader) {
        uxr_buffer_cancel_data(&impl_->session, impl_->reliable_out, ts.reader_id);
        uxr_buffer_delete_entity(&impl_->session, impl_->reliable_out, ts.reader_id);
        impl_->reader_to_topic.erase(ts.reader_id.id);
        ts.has_reader = false;
        // In-place reset — the map node stays live; OnTopic must not depend on
        // these fields across a user callback (see the copy-to-locals fix above).
        ts.callback = nullptr;
        ts.pending.clear();
    }
}

// -----------------------------------------------------------------------
// Test seam (issue #62 residual, HARD-4) — compiled only under
// FLETCHER_BUILD_TESTS. Drives the real Impl::OnTopic() schema-flush path
// through a re-entrant-Unsubscribe scenario. See src/internal/xrce_test_hook.hpp.
// -----------------------------------------------------------------------
#ifdef FLETCHER_BUILD_TESTS

namespace xrce::test {
namespace {
// File-scope trampoline. The free hook forwards through this pointer, which the
// non-inline static data member Impl::test_hook_registered_ wires to the
// Impl-scoped scenario at static-init (a non-inline non-local static's dynamic
// init strongly-happens-before the first odr-use of any non-inline function in
// this TU — i.e. before the hook can run).
ReentrantUnsubscribeResult (*g_run_reentrant_scenario)() = nullptr;
}  // namespace
}  // namespace xrce::test

const bool XrceDDSPubSubProvider::Impl::test_hook_registered_ = [] {
    xrce::test::g_run_reentrant_scenario =
        &XrceDDSPubSubProvider::Impl::RunReentrantUnsubscribeScenario;
    return true;
}();

xrce::test::ReentrantUnsubscribeResult
XrceDDSPubSubProvider::Impl::RunReentrantUnsubscribeScenario() {
    xrce::test::ReentrantUnsubscribeResult result;

    // Default-constructed Impl: no transport/session/run-loop is started, so no
    // network or Agent is involved. We populate internal state by hand and drive
    // the real OnTopic() directly.
    Impl impl;

    const std::string topic_name = "reentrant/topic";
    const uint16_t schema_reader_id = 7;

    // Route a schema sample carrying this reader id to `topic_name`'s
    // schema-flush path.
    impl.schema_reader_to_topic[schema_reader_id] = topic_name;

    auto& ts = impl.topics[topic_name];
    ts.schema_resolved = false;

    // Two buffered pending envelopes (subscriber-first backlog). No attachments.
    auto make_env = [](uint8_t tag) {
        Envelope env;
        env.row = {tag, 0x00, 0x00, 0x00};
        return env;
    };
    ts.pending.push_back(make_env(0x01));
    ts.pending.push_back(make_env(0x02));

    // Callback: count deliveries and, on the FIRST delivery, reproduce EXACTLY
    // what the real Unsubscribe() does to a live TopicState — an in-place reset
    // (ts.callback = nullptr; ts.pending.clear()) with the map node left live.
    // Pre-fix this self-nulls the executing std::function and invalidates the
    // pending iteration; post-fix the flush runs off local copies and is immune.
    Impl* impl_ptr = &impl;
    ts.callback = [impl_ptr, topic_name, &result](const uint8_t*, size_t, SharedSchema,
                                                  Attachments) {
        result.delivery_count += 1;
        if (result.delivery_count == 1) {
            auto& live = impl_ptr->topics[topic_name];
            live.callback = nullptr;  // real Unsubscribe in-place reset
            live.pending.clear();     // real Unsubscribe in-place reset
        }
    };

    // Synthesize a schema sample exactly as the wire path presents it to
    // OnTopic: IPC schema bytes wrapped in the CDR sequence<octet> length prefix
    // (the Agent has already stripped the CDR encapsulation header).
    OwnedSchema schema;
    ArrowSchemaInit(schema.get());
    ArrowSchemaSetTypeStruct(schema.get(), 1);
    ArrowSchemaSetName(schema->children[0], "x");
    ArrowSchemaSetType(schema->children[0], NANOARROW_TYPE_INT32);

    std::vector<uint8_t> ipc = SerializeSchemaIpc(schema.get());
    const uint32_t ipc_len = static_cast<uint32_t>(ipc.size());
    std::vector<uint8_t> wire;
    wire.resize(sizeof(ipc_len));
    std::memcpy(wire.data(), &ipc_len, sizeof(ipc_len));
    wire.insert(wire.end(), ipc.begin(), ipc.end());

    ucdrBuffer ub;
    ucdr_init_buffer(&ub, wire.data(), wire.size());

    uxrObjectId object_id = uxr_object_id(schema_reader_id, UXR_DATAREADER_ID);

    // Drive the REAL schema-flush path. Pre-fix: throws std::bad_function_call on
    // the 2nd pending iteration (ts.callback nulled by the re-entrant reset).
    // Post-fix: delivers both envelopes from local copies (delivery_count == 2).
    Impl::OnTopic(nullptr, object_id, /*request_id=*/0, impl.reliable_in, &ub,
                  static_cast<uint16_t>(wire.size()), &impl);

    return result;
}

namespace xrce::test {
ReentrantUnsubscribeResult RunReentrantUnsubscribeSchemaFlushScenario() {
    return g_run_reentrant_scenario();
}
}  // namespace xrce::test

#endif  // FLETCHER_BUILD_TESTS

}  // namespace fletcher
