// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include <gtest/gtest.h>
#include <uxr/client/client.h>

#include <cstdint>
#include <cstring>
#include <fletcher/core/envelope.hpp>
#include <fletcher/core/status.hpp>
#include <fletcher/core/write_buffer.hpp>
#include <string>
#include <vector>

#include "fletcher/xrcedds_pubsub_provider/xrce_dds_pubsub_provider.hpp"
#include "internal/xrce_test_hook.hpp"

using namespace fletcher;

// ---------------------------------------------------------------------------
// A cancel issued from inside a delivery is REFUSED BY NAME, and the flush that
// delivery belongs to still completes.
//
// REPLACED IN PLACE by PDA-DEC-AG1. The case here used to be
// `ReentrantUnsubscribeNoUseAfterFree`, and it pinned the behaviour that
// "cancelling from inside a handler works on XRCE" — this provider's recursive
// `mu` let the call straight through, while the loopback deadlocked and Fast DDS
// self-waited on reader deletion. Owner ruling 2026-09-05 ends that divergence:
// `Unsubscribe` on this instance from inside a delivery on this thread is refused
// with `kReentrantCall` on all three. That is a deliberate behaviour change and
// this test now asserts the new answer rather than the old one.
//
// The scenario (built and driven by the internal test hook against the real
// Impl::OnTopic schema-flush path): a topic has a subscription and two buffered
// pending envelopes; a synthesized schema sample triggers the flush; the first
// delivery attempts the cancel — meeting the same door the real Unsubscribe
// opens with — and then performs the real in-place TopicState reset, leaving the
// map node live.
//
// #62 residual (HARD-4) rides along, and is why `DeliveryChannel` is COPYABLE:
// OnTopic snapshots the channel, schema and pending list into locals before
// invoking user code, so both envelopes are delivered from local copies
// (delivery_count == 2) across a reset that would otherwise destroy the
// std::function currently executing.
// ---------------------------------------------------------------------------
TEST(XrceProviderTest, ReentrantUnsubscribeIsRefusedAndTheFlushSurvives) {
    EXPECT_NO_THROW({
        auto result = fletcher::xrce::test::RunReentrantUnsubscribeSchemaFlushScenario();
        EXPECT_EQ(result.delivery_count, 2);
        EXPECT_EQ(result.refusal_status,
                  static_cast<int32_t>(fletcher::PubSubStatus::kReentrantCall))
            << "a cancel issued from inside a delivery on this instance must be refused by "
               "name; this provider used to serve it";
    });
}

// ---------------------------------------------------------------------------
// RETIRED here, replaced in test_xrce_document.cpp (PDA-DEC-7):
//
//   XrceProviderTest.DefaultValues / .CustomValues set a field on the retired
//   `XrceConfig` struct and read it straight back, which asserts that C++
//   assignment works. Their subject no longer exists. What replaces them is
//   strictly stronger: `XrceConfig.PublishedDefaultsAreExact` parses the
//   default document out of README.md on disk and compares it WHOLE-STRUCT to
//   the code's defaults, and `XrceConfig.EveryKeySetNonDefaultLandsWholeStruct`
//   sets all four keys away from their defaults and compares whole-struct, so a
//   reader that accepts a value and then discards it cannot stay green.
//
//   XrceProviderTest.ConstructorThrowsWithoutAgent asserted only that
//   something deriving from `std::runtime_error` came out - true of every
//   `PubSubError` whatever its status. It is replaced by
//   `XrceConfig.AgentUnreachableIsATransportFailure`, which asserts
//   `kTransportFailure` and goes through `RegisterXrceProvider` + `Create`.
//
//   XrceProviderTest.SerialTransportNotImplemented likewise becomes
//   `XrceConfig.SerialIsRefusedAsUnsupported`, asserting `kNotSupported` -
//   distinct from a typo's `kInvalidArgument`.
// ---------------------------------------------------------------------------

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
// Envelope format compatibility (unit test, no Agent needed)
// ---------------------------------------------------------------------------

TEST(XrceProviderTest, EnvelopeXrceUsesSameWireFormatAsFastDds) {
    // Verify that the envelope serialization used by XRCE provider
    // produces the exact same bytes as the shared implementation.
    Envelope env;
    env.row = {0x01, 0x02, 0x03};

    const std::vector<uint8_t> payload{0xDE, 0xAD};
    env.attachments.Set("sensor", Blob{payload});

    // Parsing needs an OWNER for the bytes now: the restored attachments alias
    // them rather than being copied out (§3.2).
    auto wire = std::make_shared<const std::vector<uint8_t>>(SerializeEnvelope(env));
    auto restored = DeserializeEnvelope(wire);

    EXPECT_EQ(restored.row, env.row);
    ASSERT_EQ(restored.attachments.size(), 1);
    const Blob* found = restored.attachments.Find("sensor");
    ASSERT_NE(found, nullptr);
    const Blob& sensor = *found;
    ASSERT_EQ(sensor.size(), payload.size());
    EXPECT_EQ(std::memcmp(sensor.data(), payload.data(), payload.size()), 0);
    EXPECT_GE(sensor.data(), wire->data());
    EXPECT_LT(sensor.data(), wire->data() + wire->size());
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

// The companion __schema channel rides the same envelope as the data channel, as a row plus one
// attachment -- the announcing publisher's payload bound under key `kSchemaPayloadBoundKey`, a
// 4-byte little-endian uint32: [ROW_LEN:4][ipc bytes][ATTACH_COUNT:4 = 1][KEY_LEN:4][key:17]
// [BLOB_LEN:4][blob:4]. This pins the shape the Fast DDS side produces and requires, independently
// of xrce_dds_pubsub_provider.cpp's own construction of it, the way EnvelopeWireFormatLayout above
// pins the data channel's.
TEST(XrceProviderTest, SchemaEnvelopeCarriesThePayloadBoundAttachment) {
    const std::vector<uint8_t> ipc = {0x01, 0x02, 0x03, 0x04, 0x05};
    constexpr uint32_t kBound = 65536;  // {0, 0, 1, 0} little-endian

    Envelope schema_env;
    schema_env.row = ipc;
    schema_env.attachments.Set(kSchemaPayloadBoundKey, Blob(std::vector<uint8_t>{0, 0, 1, 0}));
    auto wire = SerializeEnvelope(schema_env);

    // 29 = 4 (key length) + 17 (key bytes) + 4 (blob length) + 4 (blob).
    ASSERT_EQ(wire.size(), 4 + ipc.size() + 4 + 29);

    uint32_t row_len;
    std::memcpy(&row_len, wire.data(), 4);
    EXPECT_EQ(row_len, ipc.size());

    EXPECT_EQ(0, std::memcmp(wire.data() + 4, ipc.data(), ipc.size()));

    size_t pos = 4 + ipc.size();
    uint32_t attach_count;
    std::memcpy(&attach_count, wire.data() + pos, 4);
    EXPECT_EQ(attach_count, 1u);
    pos += 4;

    uint32_t key_len;
    std::memcpy(&key_len, wire.data() + pos, 4);
    EXPECT_EQ(key_len, 17u);
    pos += 4;

    const std::string key(reinterpret_cast<const char*>(wire.data() + pos), key_len);
    EXPECT_EQ(key, "max_payload_bytes");
    pos += key_len;

    uint32_t blob_len;
    std::memcpy(&blob_len, wire.data() + pos, 4);
    EXPECT_EQ(blob_len, 4u);
    pos += 4;

    EXPECT_EQ(wire[pos], 0);
    EXPECT_EQ(wire[pos + 1], 0);
    EXPECT_EQ(wire[pos + 2], 1);
    EXPECT_EQ(wire[pos + 3], 0);

    // And the round trip a receiver performs: DeserializeEnvelope recovers the IPC bytes as the
    // row, plus the one attachment -- the check that tells an undecodable schema from a
    // well-formed one.
    Envelope restored = DeserializeEnvelope(wire.data(), wire.size());
    EXPECT_EQ(restored.row, ipc);
    ASSERT_EQ(restored.attachments.size(), 1u);
    const Blob* bound_blob = restored.attachments.Find(kSchemaPayloadBoundKey);
    ASSERT_NE(bound_blob, nullptr);
    ASSERT_EQ(bound_blob->size(), 4u);
    uint32_t bound = 0;
    std::memcpy(&bound, bound_blob->data(), sizeof(bound));
    EXPECT_EQ(bound, kBound);
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
