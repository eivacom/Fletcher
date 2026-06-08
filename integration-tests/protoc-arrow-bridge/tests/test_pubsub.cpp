// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// pubsub.proto — generated Publisher / Subscriber classes.
//
// The plugin emits typed Publisher / Subscriber wrappers around the
// fletcher::PubSubProvider interface. These tests use a
// MockPubSubProvider (in-process, no DDS) to exercise:
//
// - Publisher creates a topic with the right schema on construction
// - Publish encodes a typed message and delivers bytes to the provider
// - Subscriber receives typed messages decoded from delivered bytes
// - Unsubscribe stops delivery
// - Attachments flow through

#include <arrow/api.h>
#include <arrow/c/bridge.h>
#include <gtest/gtest.h>

#include <fletcher/arrow_bridge/codec.hpp>
#include <fletcher/core/write_buffer.hpp>
#include <fletcher/pubsub/owned_schema.hpp>
#include <fletcher/pubsub/provider.hpp>
#include <map>
#include <memory>
#include <vector>

#include "pubsub.fletcher.pb.h"

using namespace fletcher;

namespace {

std::shared_ptr<arrow::Schema> ImportNano(OwnedSchema nano) {
    auto result = arrow::ImportSchema(nano.get());
    if (!result.ok()) {
        ADD_FAILURE() << "ImportSchema failed: " << result.status();
        return nullptr;
    }
    return *result;
}

// Minimal in-process provider that records calls and delivers published
// rows back to subscribers synchronously. No DDS, no transport.
class MockPubSubProvider : public PubSubProvider {
   public:
    struct CreatedTopic {
        std::vector<std::string> segments;
        OwnedSchema schema;
    };

    struct PublishedMsg {
        std::vector<std::string> segments;
        std::vector<uint8_t> encoded;
        Attachments attachments;
    };

    std::vector<CreatedTopic> created_topics;
    std::vector<PublishedMsg> published;
    std::map<std::vector<std::string>, SubscribeCallback> subscribers;

    void CreateTopic(const std::vector<std::string>& segments, OwnedSchema schema) override {
        created_topics.push_back({segments, std::move(schema)});
    }

    void Publish(const std::vector<std::string>& segments, RowEncoder encoder,
                 const Attachments& attachments) override {
        std::vector<uint8_t> buf;
        VectorWriteBuffer wb(buf);
        encoder(wb);
        SharedSchema sp;
        for (const auto& ct : created_topics) {
            if (ct.segments == segments) {
                sp = MakeSharedSchema(OwnedSchema::DeepCopy(ct.schema.get()));
                break;
            }
        }
        auto it = subscribers.find(segments);
        if (it != subscribers.end()) {
            it->second(buf.data(), buf.size(), sp, attachments);
        }
        published.push_back({segments, std::move(buf), attachments});
    }

    SubscriptionResult Subscribe(const std::vector<std::string>& segments,
                                 SubscribeCallback callback) override {
        subscribers[segments] = std::move(callback);
        for (const auto& ct : created_topics) {
            if (ct.segments == segments) {
                return {MakeReadySchemaFuture(
                    MakeSharedSchema(OwnedSchema::DeepCopy(ct.schema.get())))};
            }
        }
        return {MakeReadySchemaFuture(nullptr)};
    }

    void Unsubscribe(const std::vector<std::string>& segments) override {
        subscribers.erase(segments);
    }
};

}  // namespace

// ── Publisher tests ──────────────────────────────────────────────────────

TEST(PubSubProtoTest, PublisherConstructionCreatesTopicWithCorrectSchema) {
    auto mock = std::make_shared<MockPubSubProvider>();
    fletcher_gen::integration::pubsub::TelemetryFeed_TelemetryStreamPublisher pub(mock);

    ASSERT_EQ(mock->created_topics.size(), 1u);
    EXPECT_EQ(mock->created_topics[0].segments,
              (std::vector<std::string>{"integration.pubsub", "TelemetryFeed", "TelemetryStream"}));
    auto schema = ImportNano(OwnedSchema::DeepCopy(mock->created_topics[0].schema.get()));
    EXPECT_EQ(schema->num_fields(), 4);
}

TEST(PubSubProtoTest, PublishEncodesAndDeliversToProvider) {
    auto mock = std::make_shared<MockPubSubProvider>();
    fletcher_gen::integration::pubsub::TelemetryFeed_TelemetryStreamPublisher pub(mock);

    fletcher_gen::integration::pubsub::Telemetry row;
    row.set_device_id(42).set_value(3.14).set_timestamp(1000LL).set_metric_name("cpu");
    pub.Publish(row);

    ASSERT_EQ(mock->published.size(), 1u);
    EXPECT_EQ(mock->published[0].segments,
              (std::vector<std::string>{"integration.pubsub", "TelemetryFeed", "TelemetryStream"}));
    EXPECT_FALSE(mock->published[0].encoded.empty());
}

TEST(PubSubProtoTest, MultiplePublishesAccumulate) {
    auto mock = std::make_shared<MockPubSubProvider>();
    fletcher_gen::integration::pubsub::TelemetryFeed_TelemetryStreamPublisher pub(mock);

    fletcher_gen::integration::pubsub::Telemetry r1, r2, r3;
    r1.set_device_id(1).set_value(1.0).set_timestamp(100LL).set_metric_name("a");
    r2.set_device_id(2).set_value(2.0).set_timestamp(200LL).set_metric_name("b");
    r3.set_device_id(3).set_value(3.0).set_timestamp(300LL).set_metric_name("c");

    pub.Publish(r1);
    pub.Publish(r2);
    pub.Publish(r3);

    EXPECT_EQ(mock->published.size(), 3u);
}

// ── Subscriber tests ─────────────────────────────────────────────────────

TEST(PubSubProtoTest, SubscriberConstructionDoesNotCreateTopic) {
    auto mock = std::make_shared<MockPubSubProvider>();
    fletcher_gen::integration::pubsub::TelemetryFeed_TelemetryStreamSubscriber sub(mock);

    // Subscriber-only processes don't call CreateTopic; the schema is
    // discovered from the provider when Subscribe() is called.
    EXPECT_TRUE(mock->created_topics.empty());
}

TEST(PubSubProtoTest, SubscriberReceivesTypedMessageFromPublishedRows) {
    auto mock = std::make_shared<MockPubSubProvider>();
    fletcher_gen::integration::pubsub::TelemetryFeed_TelemetryStreamPublisher pub(mock);
    fletcher_gen::integration::pubsub::TelemetryFeed_TelemetryStreamSubscriber sub(mock);

    fletcher_gen::integration::pubsub::Telemetry received;
    sub.Subscribe([&](fletcher_gen::integration::pubsub::Telemetry msg, Attachments) {
        received = std::move(msg);
    });

    fletcher_gen::integration::pubsub::Telemetry row;
    row.set_device_id(42).set_value(3.14).set_timestamp(1000LL).set_metric_name("cpu");
    pub.Publish(row);

    EXPECT_EQ(received.device_id(), 42);
    EXPECT_DOUBLE_EQ(received.value(), 3.14);
    EXPECT_EQ(received.timestamp(), 1000LL);
    EXPECT_EQ(received.metric_name(), "cpu");
}

TEST(PubSubProtoTest, UnsubscribeStopsDelivery) {
    auto mock = std::make_shared<MockPubSubProvider>();
    fletcher_gen::integration::pubsub::TelemetryFeed_TelemetryStreamPublisher pub(mock);
    fletcher_gen::integration::pubsub::TelemetryFeed_TelemetryStreamSubscriber sub(mock);

    int count = 0;
    uint64_t sub_id =
        sub.Subscribe([&](fletcher_gen::integration::pubsub::Telemetry, Attachments) { ++count; });

    fletcher_gen::integration::pubsub::Telemetry row;
    row.set_device_id(1).set_value(0.0).set_timestamp(0LL).set_metric_name("x");

    pub.Publish(row);
    EXPECT_EQ(count, 1);

    sub.Unsubscribe(sub_id);
    pub.Publish(row);
    EXPECT_EQ(count, 1);  // no delivery after unsubscribe
}

TEST(PubSubProtoTest, PublishWithAttachmentsDeliversBlobToSubscriber) {
    auto mock = std::make_shared<MockPubSubProvider>();
    fletcher_gen::integration::pubsub::TelemetryFeed_TelemetryStreamPublisher pub(mock);
    fletcher_gen::integration::pubsub::TelemetryFeed_TelemetryStreamSubscriber sub(mock);

    fletcher_gen::integration::pubsub::Telemetry received;
    Attachments received_att;
    sub.Subscribe([&](fletcher_gen::integration::pubsub::Telemetry msg, Attachments att) {
        received = std::move(msg);
        received_att = std::move(att);
    });

    fletcher_gen::integration::pubsub::Telemetry row;
    row.set_device_id(42).set_value(3.14).set_timestamp(1000LL).set_metric_name("cpu");

    auto blob =
        std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{0xDE, 0xAD, 0xBE, 0xEF});
    pub.Publish(row, {{"image", blob}});

    EXPECT_EQ(received.device_id(), 42);
    ASSERT_EQ(received_att.size(), 1u);
    ASSERT_EQ(received_att.count("image"), 1u);
    EXPECT_EQ(*received_att.at("image"), (std::vector<uint8_t>{0xDE, 0xAD, 0xBE, 0xEF}));
}

TEST(PubSubProtoTest, PublishWithoutAttachmentsHasEmptyAttachments) {
    auto mock = std::make_shared<MockPubSubProvider>();
    fletcher_gen::integration::pubsub::TelemetryFeed_TelemetryStreamPublisher pub(mock);

    fletcher_gen::integration::pubsub::Telemetry row;
    row.set_device_id(1).set_value(0.0).set_timestamp(0LL).set_metric_name("x");
    pub.Publish(row);

    ASSERT_EQ(mock->published.size(), 1u);
    EXPECT_TRUE(mock->published[0].attachments.empty());
    EXPECT_FALSE(mock->published[0].encoded.empty());
}
