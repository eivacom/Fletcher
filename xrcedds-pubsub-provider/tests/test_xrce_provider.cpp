// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include <gtest/gtest.h>
#include <uxr/client/client.h>

#include <cstring>
#include <fletcher/core/envelope.hpp>
#include <fletcher/core/write_buffer.hpp>
#include <string>
#include <vector>

#include "fletcher/xrcedds_pubsub_provider/xrce_dds_pubsub_provider.hpp"
#include "internal/xrce_test_hook.hpp"

using namespace fletcher;

// ---------------------------------------------------------------------------
// #62 residual (HARD-4) — re-entrant Unsubscribe from inside a callback must
// not use-after-free / throw std::bad_function_call in OnTopic.
//
// The scenario (built and driven by the internal test hook against the real
// Impl::OnTopic schema-flush path): a topic has a callback and two buffered
// pending envelopes; a synthesized schema sample triggers the flush; the first
// delivery re-enters Unsubscribe on the same topic, which performs the real
// in-place TopicState reset (ts.callback = nullptr; ts.pending.clear()) leaving
// the map node live.
//
// PRE-fix: the flush loop keeps reading the live ts.pending / ts.callback across
// the reset — the second iteration reads a destroyed Envelope and then invokes
// the now-null ts.callback, throwing std::bad_function_call (delivery_count < 2,
// never returned). POST-fix: OnTopic snapshots callback, schema, and pending
// into locals before invoking user code, so both envelopes are delivered from
// local copies (delivery_count == 2, no throw).
// ---------------------------------------------------------------------------
TEST(XrceProviderTest, ReentrantUnsubscribeNoUseAfterFree) {
    EXPECT_NO_THROW({
        auto result = fletcher::xrce::test::RunReentrantUnsubscribeSchemaFlushScenario();
        EXPECT_EQ(result.delivery_count, 2);
    });
}

// ---------------------------------------------------------------------------
// JoinSegments — mirrors the internal helper; tested via public API behavior
// ---------------------------------------------------------------------------

TEST(XrceProviderTest, DefaultValues) {
    XrceConfig cfg;
    EXPECT_EQ(cfg.transport, XrceTransport::kUdp);
    EXPECT_EQ(cfg.agent_ip, "127.0.0.1");
    EXPECT_EQ(cfg.agent_port, 2018);
    EXPECT_EQ(cfg.max_payload, 512);
    EXPECT_EQ(cfg.stream_history, 4);
    EXPECT_EQ(cfg.run_loop_ms, 10);
    EXPECT_EQ(cfg.session_key, 0xAABBCCDD);
}

TEST(XrceProviderTest, CustomValues) {
    XrceConfig cfg;
    cfg.transport = XrceTransport::kTcp;
    cfg.agent_ip = "192.168.1.100";
    cfg.agent_port = 3000;
    cfg.max_payload = 1024;
    cfg.stream_history = 8;
    cfg.run_loop_ms = 50;
    cfg.session_key = 0x12345678;

    EXPECT_EQ(cfg.transport, XrceTransport::kTcp);
    EXPECT_EQ(cfg.agent_ip, "192.168.1.100");
    EXPECT_EQ(cfg.agent_port, 3000);
    EXPECT_EQ(cfg.max_payload, 1024);
    EXPECT_EQ(cfg.stream_history, 8);
    EXPECT_EQ(cfg.run_loop_ms, 50);
    EXPECT_EQ(cfg.session_key, 0x12345678);
}

// ---------------------------------------------------------------------------
// XRCE object ID allocation
// ---------------------------------------------------------------------------

TEST(XrceProviderTest, ObjectIdTypeEncoding) {
    // Verify the XRCE object ID packing works as expected.
    auto id = uxr_object_id(0x0010, UXR_PARTICIPANT_ID);
    EXPECT_EQ(id.id, 0x0010);
    EXPECT_EQ(id.type, UXR_PARTICIPANT_ID);

    auto id2 = uxr_object_id(0x0010, UXR_TOPIC_ID);
    EXPECT_EQ(id2.id, 0x0010);
    EXPECT_EQ(id2.type, UXR_TOPIC_ID);
}

TEST(XrceProviderTest, DifferentTypesWithSameBase) {
    uint16_t base = 42;
    auto part = uxr_object_id(base, UXR_PARTICIPANT_ID);
    auto topic = uxr_object_id(base, UXR_TOPIC_ID);
    auto pub = uxr_object_id(base, UXR_PUBLISHER_ID);
    auto dw = uxr_object_id(base, UXR_DATAWRITER_ID);
    auto sub = uxr_object_id(base, UXR_SUBSCRIBER_ID);
    auto dr = uxr_object_id(base, UXR_DATAREADER_ID);

    // All share the same numeric base.
    EXPECT_EQ(part.id, base);
    EXPECT_EQ(topic.id, base);
    EXPECT_EQ(pub.id, base);
    EXPECT_EQ(dw.id, base);
    EXPECT_EQ(sub.id, base);
    EXPECT_EQ(dr.id, base);

    // All have distinct type tags.
    EXPECT_EQ(part.type, UXR_PARTICIPANT_ID);
    EXPECT_EQ(topic.type, UXR_TOPIC_ID);
    EXPECT_EQ(pub.type, UXR_PUBLISHER_ID);
    EXPECT_EQ(dw.type, UXR_DATAWRITER_ID);
    EXPECT_EQ(sub.type, UXR_SUBSCRIBER_ID);
    EXPECT_EQ(dr.type, UXR_DATAREADER_ID);
}

// ---------------------------------------------------------------------------
// Constructor without Agent — should throw
// ---------------------------------------------------------------------------

TEST(XrceProviderTest, ConstructorThrowsWithoutAgent) {
    XrceConfig cfg;
    cfg.agent_ip = "127.0.0.1";
    cfg.agent_port = 19999;  // No agent expected on this port.
    cfg.connect_timeout_ms = 200;

    EXPECT_THROW(XrceDDSPubSubProvider(cfg), std::runtime_error);
}

TEST(XrceProviderTest, SerialTransportNotImplemented) {
    XrceConfig cfg;
    cfg.transport = XrceTransport::kSerial;
    cfg.connect_timeout_ms = 200;

    EXPECT_THROW(XrceDDSPubSubProvider(cfg), std::runtime_error);
}

// ---------------------------------------------------------------------------
// Envelope format compatibility (unit test, no Agent needed)
// ---------------------------------------------------------------------------

TEST(XrceProviderTest, EnvelopeXrceUsesSameWireFormatAsFastDds) {
    // Verify that the envelope serialization used by XRCE provider
    // produces the exact same bytes as the shared implementation.
    Envelope env;
    env.row = {0x01, 0x02, 0x03};

    auto blob = std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{0xDE, 0xAD});
    env.attachments["sensor"] = blob;

    auto wire = SerializeEnvelope(env);
    auto restored = DeserializeEnvelope(wire);

    EXPECT_EQ(restored.row, env.row);
    ASSERT_EQ(restored.attachments.size(), 1);
    EXPECT_EQ(*restored.attachments.at("sensor"), *blob);
}

TEST(XrceProviderTest, EnvelopeWireFormatLayout) {
    Envelope env;
    env.row = {0xAA, 0xBB, 0xCC};
    // No attachments.

    auto wire = SerializeEnvelope(env);

    // Expected layout: [ROW_LEN:4][ROW_DATA:3][ATTACH_COUNT:4]
    ASSERT_EQ(wire.size(), 4 + 3 + 4);

    // ROW_LEN = 3 (little-endian).
    uint32_t row_len;
    std::memcpy(&row_len, wire.data(), 4);
    EXPECT_EQ(row_len, 3);

    // Row data.
    EXPECT_EQ(wire[4], 0xAA);
    EXPECT_EQ(wire[5], 0xBB);
    EXPECT_EQ(wire[6], 0xCC);

    // ATTACH_COUNT = 0.
    uint32_t attach_count;
    std::memcpy(&attach_count, wire.data() + 7, 4);
    EXPECT_EQ(attach_count, 0);
}

// ---------------------------------------------------------------------------
// QoS struct defaults
// ---------------------------------------------------------------------------

TEST(XrceProviderTest, QosStructInitialization) {
    uxrQoS_t qos{};
    qos.reliability = UXR_RELIABILITY_RELIABLE;
    qos.durability = UXR_DURABILITY_TRANSIENT_LOCAL;
    qos.history = UXR_HISTORY_KEEP_LAST;
    qos.depth = 1;

    EXPECT_EQ(qos.reliability, UXR_RELIABILITY_RELIABLE);
    EXPECT_EQ(qos.durability, UXR_DURABILITY_TRANSIENT_LOCAL);
    EXPECT_EQ(qos.history, UXR_HISTORY_KEEP_LAST);
    EXPECT_EQ(qos.depth, 1);
}
