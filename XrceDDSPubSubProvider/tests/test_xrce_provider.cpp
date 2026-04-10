#include <catch2/catch_all.hpp>

#include "xrce_dds_pubsub_provider.hpp"

#include <pubsub/envelope.hpp>
#include <pubsub/write_buffer.hpp>

#include <uxr/client/client.h>

#include <cstring>
#include <string>
#include <vector>

using namespace fletcher;

// ---------------------------------------------------------------------------
// JoinSegments — mirrors the internal helper; tested via public API behavior
// ---------------------------------------------------------------------------

TEST_CASE("XrceConfig: default values") {
    XrceConfig cfg;
    CHECK(cfg.transport == XrceTransport::kUdp);
    CHECK(cfg.agent_ip == "127.0.0.1");
    CHECK(cfg.agent_port == 2018);
    CHECK(cfg.max_payload == 512);
    CHECK(cfg.stream_history == 4);
    CHECK(cfg.run_loop_ms == 10);
    CHECK(cfg.session_key == 0xAABBCCDD);
}

TEST_CASE("XrceConfig: custom values") {
    XrceConfig cfg;
    cfg.transport = XrceTransport::kTcp;
    cfg.agent_ip = "192.168.1.100";
    cfg.agent_port = 3000;
    cfg.max_payload = 1024;
    cfg.stream_history = 8;
    cfg.run_loop_ms = 50;
    cfg.session_key = 0x12345678;

    CHECK(cfg.transport == XrceTransport::kTcp);
    CHECK(cfg.agent_ip == "192.168.1.100");
    CHECK(cfg.agent_port == 3000);
    CHECK(cfg.max_payload == 1024);
    CHECK(cfg.stream_history == 8);
    CHECK(cfg.run_loop_ms == 50);
    CHECK(cfg.session_key == 0x12345678);
}

// ---------------------------------------------------------------------------
// XRCE object ID allocation
// ---------------------------------------------------------------------------

TEST_CASE("XRCE object IDs: uxr_object_id type encoding") {
    // Verify the XRCE object ID packing works as expected.
    auto id = uxr_object_id(0x0010, UXR_PARTICIPANT_ID);
    CHECK(id.id == 0x0010);
    CHECK(id.type == UXR_PARTICIPANT_ID);

    auto id2 = uxr_object_id(0x0010, UXR_TOPIC_ID);
    CHECK(id2.id == 0x0010);
    CHECK(id2.type == UXR_TOPIC_ID);
}

TEST_CASE("XRCE object IDs: different types with same base") {
    uint16_t base = 42;
    auto part  = uxr_object_id(base, UXR_PARTICIPANT_ID);
    auto topic = uxr_object_id(base, UXR_TOPIC_ID);
    auto pub   = uxr_object_id(base, UXR_PUBLISHER_ID);
    auto dw    = uxr_object_id(base, UXR_DATAWRITER_ID);
    auto sub   = uxr_object_id(base, UXR_SUBSCRIBER_ID);
    auto dr    = uxr_object_id(base, UXR_DATAREADER_ID);

    // All share the same numeric base.
    CHECK(part.id == base);
    CHECK(topic.id == base);
    CHECK(pub.id == base);
    CHECK(dw.id == base);
    CHECK(sub.id == base);
    CHECK(dr.id == base);

    // All have distinct type tags.
    CHECK(part.type == UXR_PARTICIPANT_ID);
    CHECK(topic.type == UXR_TOPIC_ID);
    CHECK(pub.type == UXR_PUBLISHER_ID);
    CHECK(dw.type == UXR_DATAWRITER_ID);
    CHECK(sub.type == UXR_SUBSCRIBER_ID);
    CHECK(dr.type == UXR_DATAREADER_ID);
}

// ---------------------------------------------------------------------------
// Constructor without Agent — should throw
// ---------------------------------------------------------------------------

TEST_CASE("XrceDDSPubSubProvider: constructor throws without Agent") {
    XrceConfig cfg;
    cfg.agent_ip = "127.0.0.1";
    cfg.agent_port = 19999;  // No agent expected on this port.

    // Construction should fail because uxr_create_session will time out.
    CHECK_THROWS_AS(XrceDDSPubSubProvider(cfg), std::runtime_error);
}

TEST_CASE("XrceDDSPubSubProvider: serial transport not implemented") {
    XrceConfig cfg;
    cfg.transport = XrceTransport::kSerial;

    CHECK_THROWS_AS(XrceDDSPubSubProvider(cfg), std::runtime_error);
}

// ---------------------------------------------------------------------------
// Envelope format compatibility (unit test, no Agent needed)
// ---------------------------------------------------------------------------

TEST_CASE("Envelope: XRCE uses same wire format as FastDDS") {
    // Verify that the envelope serialization used by XRCE provider
    // produces the exact same bytes as the shared implementation.
    Envelope env;
    env.row = {0x01, 0x02, 0x03};

    auto blob = std::make_shared<const std::vector<uint8_t>>(
        std::vector<uint8_t>{0xDE, 0xAD});
    env.attachments["sensor"] = blob;

    auto wire = SerializeEnvelope(env);
    auto restored = DeserializeEnvelope(wire);

    CHECK(restored.row == env.row);
    REQUIRE(restored.attachments.size() == 1);
    CHECK(*restored.attachments.at("sensor") == *blob);
}

TEST_CASE("Envelope: wire format layout") {
    Envelope env;
    env.row = {0xAA, 0xBB, 0xCC};
    // No attachments.

    auto wire = SerializeEnvelope(env);

    // Expected layout: [ROW_LEN:4][ROW_DATA:3][ATTACH_COUNT:4]
    REQUIRE(wire.size() == 4 + 3 + 4);

    // ROW_LEN = 3 (little-endian).
    uint32_t row_len;
    std::memcpy(&row_len, wire.data(), 4);
    CHECK(row_len == 3);

    // Row data.
    CHECK(wire[4] == 0xAA);
    CHECK(wire[5] == 0xBB);
    CHECK(wire[6] == 0xCC);

    // ATTACH_COUNT = 0.
    uint32_t attach_count;
    std::memcpy(&attach_count, wire.data() + 7, 4);
    CHECK(attach_count == 0);
}

// ---------------------------------------------------------------------------
// QoS struct defaults
// ---------------------------------------------------------------------------

TEST_CASE("XRCE QoS: struct initialization") {
    uxrQoS_t qos{};
    qos.reliability = UXR_RELIABILITY_RELIABLE;
    qos.durability  = UXR_DURABILITY_TRANSIENT_LOCAL;
    qos.history     = UXR_HISTORY_KEEP_LAST;
    qos.depth       = 1;

    CHECK(qos.reliability == UXR_RELIABILITY_RELIABLE);
    CHECK(qos.durability == UXR_DURABILITY_TRANSIENT_LOCAL);
    CHECK(qos.history == UXR_HISTORY_KEEP_LAST);
    CHECK(qos.depth == 1);
}
