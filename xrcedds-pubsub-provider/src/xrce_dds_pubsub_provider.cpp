// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// Targets eProsima Micro XRCE-DDS Client 3.0.x.
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
#include <fletcher/pubsub/internal/schema_conflict.hpp>
#include <fletcher/pubsub/internal/segments.hpp>
#include <fletcher/pubsub/schema_ipc.hpp>
#include <map>
#include <mutex>
#include <optional>
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
        // The declared schema as Arrow IPC bytes - the only form a conflict
        // check needs. Set together with is_publisher, once the announcement is
        // out, so a failed CreateTopic leaves nothing for a retry to match
        // against. Uses the same comparison as Publisher and the in-process
        // provider rather than a third one.
        internal::DeclaredSchema declared;
        bool is_publisher = false;
        bool has_reader = false;
        PubSubProvider::SubscribeCallback callback;

        // Subscriber-first support. Subscribe is non-blocking and resolves the
        // schema asynchronously through the companion __schema reader (see
        // OnTopic): the arrival is resolved when the schema arrives, and
        // data that arrives before the schema is buffered in `pending` so the
        // callback is never invoked with a null schema.
        //
        // The arrival, and the ONE thing that can end it. There is deliberately no
        // separate resolved-flag beside these: the optional IS the flag —
        // engaged means "this subscription is still waiting", and whichever path
        // consumes the token is the one that settled the question. Fast DDS's
        // SchemaChannel dropped its equivalent flag for the same reason; two sources
        // of truth for one fact is one too many.
        SchemaArrival schema_arrival;
        std::optional<SchemaResolver> schema_resolver;
        std::vector<Envelope> pending;
    };

    XrceConfig config;

    // Must be byte-identical to what a FastDDS peer registers, or the endpoints never match.
    std::string type_name;

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
    //
    // Shared storage rather than a plain local: the attachments parsed out of it
    // ALIAS these bytes (spec 3.2), and a Blob that outlives this call - one
    // buffered in `pending`, or one a callback keeps - needs an owner that
    // outlives it too. One allocation per sample, exactly as before; what
    // disappears is the copy this path used to make of every attachment.
    auto payload = std::make_shared<std::vector<uint8_t>>(length);
    ucdr_deserialize_array_uint8_t(ub, payload->data(), length);

    if (payload->size() < 4) return;
    uint32_t seq_len = 0;
    std::memcpy(&seq_len, payload->data(), 4);
    if (static_cast<size_t>(4) + seq_len > payload->size()) return;
    const uint8_t* body = payload->data() + 4;
    const std::shared_ptr<const void> body_owner = payload;

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
            // __schema is KEEP_LAST(1)/TRANSIENT_LOCAL, so repeats are expected; a
            // consumed token means this subscription's schema question is already
            // settled (arrived, or the subscription ended), and no live subscription
            // means there is nobody to tell either way.
            if (!ts.schema_resolver.has_value()) return;

            // NOTHING in the schema-resolution sequence below may throw out of
            // the XRCE session callback thread. OnTopic is invoked from inside
            // uxr_run_session_time(), so unwinding would cross C frames from the
            // XRCE client library: UB on MSVC and process termination in
            // practice (H-INV-3 / HARD locked decision #3).
            //
            // The guard covers the WHOLE sequence, not just the parse, because
            // every step can throw:
            //   * DeserializeSchemaIpc — malformed/truncated __schema sample;
            //   * OwnedSchema::DeepCopy — throws on a failed deep copy (#54);
            //   * MakeSharedSchema — allocates;
            //   * resolving the arrival — allocation, and a refusal if the
            //     schema were somehow null.
            // On any failure the token is NOT consumed and we return, so the
            // retained TRANSIENT_LOCAL/KEEP_LAST(1) __schema sample is
            // redelivered and resolution is retried.
            try {
                OwnedSchema schema = DeserializeSchemaIpc(body, seq_len);
                if (!schema) return;
                ts.schema = OwnedSchema::DeepCopy(schema.get());
                ts.shared_schema = MakeSharedSchema(std::move(schema));
                std::optional<SchemaResolver> token = std::move(ts.schema_resolver);
                ts.schema_resolver.reset();
                // Resolved while holding impl_->mu, unlike the other two providers,
                // and that is this provider's model rather than an oversight: XRCE
                // has a single recursive-mutex session pump, OnTopic is already
                // entered with the lock held, and it dispatches user callbacks under
                // it a few lines below. Safe because a waiter never takes impl_->mu
                // in order to wait — SchemaArrival has its own mutex — so there is no
                // inversion to have. Fast DDS resolves outside its lock because its
                // listener runs on a foreign thread that would otherwise invert with
                // the Fast DDS subscriber mutex.
                std::move(*token).Resolve(ts.shared_schema);
            } catch (...) {
                return;
            }

            callback = ts.callback;
            schema_for_callbacks = ts.shared_schema;
            pending = std::move(ts.pending);
            ts.pending.clear();
        }

        // Deliver from locals only — do NOT read or write ts / ts.* past this
        // point; a re-entrant Unsubscribe may reset the live TopicState.
        if (callback) {
            for (auto& env : pending) {
                // Borrowed: a callback that keeps the attachments copies them, per the contract.
                callback(env.row.data(), env.row.size(), schema_for_callbacks, env.attachments);
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

        // Same H-INV-3 constraint as the schema path above: nothing here may throw
        // out of the XRCE session callback thread, because OnTopic is invoked from
        // inside uxr_run_session_time() and unwinding would cross the XRCE client's
        // C frames (UB on MSVC, process termination in practice).
        //
        // DeserializeEnvelope throws std::invalid_argument on a malformed/truncated
        // sample, and buffering a pending sample allocates (std::bad_alloc). The
        // guard is a catch-all rather than `catch (const std::invalid_argument&)`:
        // letting an "unexpected" type propagate from here is not a safe fallback,
        // it is the UB this invariant exists to prevent. Drop the sample and keep
        // the session pump alive; a well-formed sample is redelivered.
        try {
            envelope = DeserializeEnvelope(body_owner, body, seq_len);
            if (!ts.shared_schema) {
                // Subscriber-first: the schema has not arrived yet. Buffer the sample;
                // it is flushed in order when the __schema sample resolves the future.
                ts.pending.push_back(std::move(envelope));
                return;
            }
        } catch (...) {
            return;
        }
        callback = ts.callback;
        schema_for_callback = ts.shared_schema;
    }

    // Deliver from locals only — do NOT read or write ts / ts.* past this point.
    if (callback) {
        callback(envelope.row.data(), envelope.row.size(), schema_for_callback,
                 envelope.attachments);
    }
}

// -----------------------------------------------------------------------
// Construction / destruction
// -----------------------------------------------------------------------

XrceDDSPubSubProvider::XrceDDSPubSubProvider(const XrceConfig& config)
    : impl_(std::make_unique<Impl>()) {
    impl_->config = config;

    // The bound is part of the registered type name, so an unusable one matches nothing.
    if (!IsPayloadBound(config.payload_bound)) {
        throw std::invalid_argument(
            "XRCE: payload_bound " + std::to_string(config.payload_bound) +
            " is not a bound a Fletcher DDS type can carry; it must be a multiple of 4 between " +
            std::to_string(kMinPayloadBytes) + " and " + std::to_string(kMaxPayloadBytes));
    }
    impl_->type_name = FletcherTypeName(config.payload_bound);

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
        throw PubSubError(PubSubStatus::kTransportFailure,
                          std::string("XRCE: failed to create ") + entity_desc +
                              " (status=" + std::to_string(status) + ")");
    }
}

void WaitForStatuses(uxrSession* session, const uint16_t* requests, uint8_t* statuses, size_t count,
                     const char* desc) {
    if (!uxr_run_session_until_all_status(session, 1000, requests, statuses, count)) {
        throw PubSubError(PubSubStatus::kTransportFailure,
                          std::string("XRCE: timeout waiting for ") + desc);
    }
    for (size_t i = 0; i < count; ++i) {
        if (statuses[i] != UXR_STATUS_OK && statuses[i] != UXR_STATUS_OK_MATCHED) {
            throw PubSubError(PubSubStatus::kTransportFailure,
                              std::string("XRCE: failed to create ") + desc + " (status[" +
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
    // Every seam entry point translates, so the only exception that can leave this
    // provider is a PubSubError carrying a stable number (spec §5.1).
    TranslateSeamFailure([&] {
        std::string name = internal::JoinSegments(topic_segments);

        // Encoded before the lock, so the locked section is a byte compare rather
        // than an IPC encode every concurrent CreateTopic queues behind.
        internal::DeclaredSchema incoming = internal::DeclaredSchema::Encode(schema.get());

        std::lock_guard lock(impl_->mu);

        auto& ts = impl_->topics[name];

        // Re-declaration is idempotent for an identical schema (so several
        // publishers may share one topic) and REFUSED for a conflicting one -
        // spec section 7 clause 3, tightened from "may be rejected" to "must be
        // rejected".
        //
        // This whole block used to be a throw on any existing topic state, which
        // refused BOTH: an identical re-declaration the contract calls idempotent,
        // and - because Subscribe creates the topic state lazily - every
        // subscriber-first declaration, so a subscriber on this instance could
        // never be joined by a publisher on it. Both are the same mistake: the
        // presence of topic state was read as "already declared".
        if (ts.is_publisher) {
            if (incoming.ConflictsWith(ts.declared)) {
                throw PubSubError(
                    PubSubStatus::kSchemaConflict,
                    "XRCE: topic already declared with a conflicting schema: " + name);
            }
            return;  // identical (or non-comparable) re-declaration - no-op
        }

        // Allocate XRCE entity IDs. The participant and the data topic may already
        // exist because a subscriber joined first (Subscribe creates them the same
        // way); reuse them, and add only the publisher side.
        uint16_t base = impl_->AllocId();
        if (ts.participant_id.type == UXR_INVALID_ID) {
            ts.participant_id = uxr_object_id(base, UXR_PARTICIPANT_ID);
            ts.topic_id = uxr_object_id(base, UXR_TOPIC_ID);

            // Create participant on the configured DDS domain.
            uint16_t req_part = uxr_buffer_create_participant_bin(
                &impl_->session, impl_->reliable_out, ts.participant_id, impl_->config.domain_id,
                name.c_str(), UXR_REPLACE);
            WaitForStatus(&impl_->session, req_part, "participant");

            // Create topic.
            uint16_t req_topic = uxr_buffer_create_topic_bin(
                &impl_->session, impl_->reliable_out, ts.topic_id, ts.participant_id, name.c_str(),
                impl_->type_name.c_str(), UXR_REPLACE);
            WaitForStatus(&impl_->session, req_topic, "topic");
        }
        ts.publisher_id = uxr_object_id(base, UXR_PUBLISHER_ID);
        ts.writer_id = uxr_object_id(base, UXR_DATAWRITER_ID);

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
            ts.schema_publisher_id = uxr_object_id(schema_base, UXR_PUBLISHER_ID);
            ts.schema_writer_id = uxr_object_id(schema_base, UXR_DATAWRITER_ID);

            // The __schema topic may already exist too - a subscriber-first reader
            // created it to await the schema. Reuse it rather than replacing the id
            // that reader is attached to.
            if (ts.schema_topic_id.type == UXR_INVALID_ID) {
                ts.schema_topic_id = uxr_object_id(schema_base, UXR_TOPIC_ID);

                std::string schema_name = name + "/__schema";
                uint16_t req_st = uxr_buffer_create_topic_bin(
                    &impl_->session, impl_->reliable_out, ts.schema_topic_id, ts.participant_id,
                    schema_name.c_str(), kSchemaTypeName, UXR_REPLACE);
                WaitForStatus(&impl_->session, req_st, "schema topic");
            }

            uint16_t req_sp = uxr_buffer_create_publisher_bin(&impl_->session, impl_->reliable_out,
                                                              ts.schema_publisher_id,
                                                              ts.participant_id, UXR_REPLACE);

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

        // Recorded only once the announcement is out, so a failed declaration
        // leaves nothing for a retry to match against and nothing to
        // short-circuit on. Mirrors the FastDDS provider.
        ts.declared = std::move(incoming);
        ts.is_publisher = true;
    });
}

void XrceDDSPubSubProvider::Publish(const std::vector<std::string>& topic_segments,
                                    const RowEncoder& encoder, const Attachments& attachments) {
    // Every seam entry point translates, so the only exception that can leave this
    // provider is a PubSubError carrying a stable number (spec §5.1).
    TranslateSeamFailure([&] {
        std::string name = internal::JoinSegments(topic_segments);
        std::lock_guard lock(impl_->mu);

        auto it = impl_->topics.find(name);
        if (it == impl_->topics.end())
            throw PubSubError(PubSubStatus::kTopicNotDeclared, "XRCE: unknown topic: " + name);

        auto& ts = it->second;

        // Topic state exists is NOT the same as declared for publishing. Subscribe
        // creates the entry lazily, and CreateTopic now takes a reference to it
        // before anything that can throw, so an entry can exist with no DataWriter
        // behind it: writer_id is then the default-constructed (invalid) object id.
        // uxr_buffer_topic reports nothing to a caller, so publishing through it
        // sent the row to an id that does not exist on the Agent and returned
        // success — silent data loss. Refuse instead.
        if (!ts.is_publisher) {
            throw PubSubError(PubSubStatus::kTopicNotDeclared,
                              "XRCE: topic not declared for publishing (call CreateTopic "
                              "first): " +
                                  name);
        }

        // Encode row bytes into a local buffer.
        VectorWriteBuffer row_buf;
        encoder(row_buf);

        // Serialize the full envelope.
        Envelope env;
        env.row = row_buf.Finish();
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
    });
}

SubscriptionResult XrceDDSPubSubProvider::Subscribe(const std::vector<std::string>& topic_segments,
                                                    SubscribeCallback callback) {
    // Every seam entry point translates, so the only exception that can leave this
    // provider is a PubSubError carrying a stable number (spec §5.1).
    return TranslateSeamFailure([&]() -> SubscriptionResult {
        std::string name = internal::JoinSegments(topic_segments);
        std::lock_guard lock(impl_->mu);

        auto& ts = impl_->topics[name];
        if (ts.has_reader)
            throw PubSubError(PubSubStatus::kInvalidArgument,
                              "XRCE: already subscribed to: " + name);

        // Subscribe is non-blocking and never throws when no publisher exists yet
        // (subscriber-first). The schema is delivered asynchronously: data that
        // arrives before it is buffered (see OnTopic) so the first callback is
        // never invoked with a null schema. A fresh arrival and its single-use
        // resolver are wired up for this subscription.
        auto arrival_pair = SchemaArrival::Create();
        ts.schema_arrival = std::move(arrival_pair.first);
        ts.schema_resolver.emplace(std::move(arrival_pair.second));

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

            uint16_t req_topic = uxr_buffer_create_topic_bin(
                &impl_->session, impl_->reliable_out, ts.topic_id, ts.participant_id, name.c_str(),
                impl_->type_name.c_str(), UXR_REPLACE);
            WaitForStatus(&impl_->session, req_topic, "subscriber topic");
        }

        if (ts.schema) {
            // Schema already known on this provider (publisher-side / cached):
            // answer the arrival immediately.
            ts.shared_schema = MakeSharedSchema(OwnedSchema::DeepCopy(ts.schema.get()));
            std::optional<SchemaResolver> token = std::move(ts.schema_resolver);
            ts.schema_resolver.reset();
            std::move(*token).Resolve(ts.shared_schema);
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
                schema_name.c_str(), kSchemaTypeName, UXR_REPLACE);
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

            uint16_t req_sub =
                uxr_buffer_create_subscriber_bin(&impl_->session, impl_->reliable_out,
                                                 ts.subscriber_id, ts.participant_id, UXR_REPLACE);

            uxrQoS_t data_qos{};
            data_qos.reliability = UXR_RELIABILITY_RELIABLE;
            data_qos.durability = UXR_DURABILITY_TRANSIENT_LOCAL;
            data_qos.history = UXR_HISTORY_KEEP_ALL;
            data_qos.depth = 16;

            uint16_t req_dr = uxr_buffer_create_datareader_bin(&impl_->session, impl_->reliable_out,
                                                               ts.reader_id, ts.subscriber_id,
                                                               ts.topic_id, data_qos, UXR_REPLACE);

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
        uxr_buffer_request_data(&impl_->session, impl_->reliable_out, ts.reader_id,
                                impl_->reliable_in, &delivery);

        // Non-blocking: hand back the schema future. It is already satisfied for
        // publisher-side/cached topics, and resolves asynchronously otherwise.
        return {ts.schema_arrival};
    });
}

void XrceDDSPubSubProvider::Unsubscribe(const std::vector<std::string>& topic_segments) {
    // Every seam entry point translates, so the only exception that can leave this
    // provider is a PubSubError carrying a stable number (spec §5.1).
    TranslateSeamFailure([&] {
        std::string name = internal::JoinSegments(topic_segments);
        std::lock_guard lock(impl_->mu);

        auto it = impl_->topics.find(name);
        if (it == impl_->topics.end()) return;

        auto& ts = it->second;

        // If the schema never arrived, end the arrival so a waiter wakes instead of
        // waiting forever. Dropping the token unresolved IS the outcome —
        // kSubscriptionEnded. It used to be a broken promise a waiting get() threw
        // out of; it is now a value a binding reads, and one it cannot confuse with
        // "this transport carries no schemas at all". Idempotent: a token already
        // consumed by a resolution is simply absent.
        ts.schema_resolver.reset();

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
        // The __schema TOPIC is shared between the two sides of one instance: a
        // publisher declaring a topic a subscriber already created reuses this id
        // rather than replacing it (see CreateTopic), and its schema DataWriter is
        // attached to it. Deleting it here would leave that writer pointing at a
        // topic that no longer exists on the Agent, with is_publisher already set so
        // no later CreateTopic repairs it — the retained TRANSIENT_LOCAL schema
        // sample then silently stops reaching any later subscriber. So only the side
        // that owns it deletes it. Publisher-side entities are torn down with the
        // session in the destructor, as they always were.
        if (!ts.is_publisher && ts.schema_topic_id.type != UXR_INVALID_ID) {
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
    });
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
    // The scenario is "a schema arrives for a live subscription", so the topic must
    // hold an unconsumed resolver — that optional is what OnTopic's schema branch
    // gates on.
    auto arrival_pair = SchemaArrival::Create();
    ts.schema_arrival = std::move(arrival_pair.first);
    ts.schema_resolver.emplace(std::move(arrival_pair.second));

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
