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
#include <nanoarrow/nanoarrow.h>

#include <chrono>
#include <fletcher/arrow_bridge/codec.hpp>
#include <fletcher/core/write_buffer.hpp>
#include <fletcher/pubsub/internal/schema_check.hpp>
#include <fletcher/pubsub/owned_schema.hpp>
#include <fletcher/pubsub/provider.hpp>
#include <fletcher/pubsub/schema_ipc.hpp>
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

// A one-field schema that cannot match Telemetry's four fields — used to prove the generated
// Subscribe / SubscribeInPlace refuse a topic that already announces something else.
OwnedSchema MismatchedSchema() {
    OwnedSchema schema;
    ArrowSchemaInit(schema.get());
    ArrowSchemaSetTypeStruct(schema.get(), 1);
    ArrowSchemaSetName(schema->children[0], "not_telemetry");
    ArrowSchemaSetType(schema->children[0], NANOARROW_TYPE_INT32);
    return schema;
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

    // Recorded rather than refused: this mock DOES support a profile and bound.
    void CreateTopic(const std::vector<std::string>& segments,
                     TopicDeclaration declaration) override {
        last_create_options = TopicOptions{.profile = declaration.profile,
                                           .max_payload_bytes = declaration.max_payload_bytes};
        created_topics.push_back({segments, std::move(declaration.schema)});
    }

    void Publish(const std::vector<std::string>& segments, const RowEncoder& encoder,
                 const Attachments& attachments) override {
        VectorWriteBuffer wb;
        encoder(wb);
        std::vector<uint8_t> buf = wb.Finish();
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

    // The generated Subscriber wrapper goes through the OwnedSchema overload, which reaches the
    // provider's `Subscribe` with a non-empty `request.check` on the topic's first opening. This
    // mock supports it directly rather than refusing kNotSupported, so the generated classes can
    // be exercised end to end. A request with an empty `check` behaves like an ordinary
    // subscription (provider.hpp's own rule).
    SubscriptionResult Subscribe(const std::vector<std::string>& segments,
                                 SubscriptionRequest request) override {
        last_subscribe_options = TopicOptions{.profile = request.profile};
        SharedSchema announced;
        for (const auto& ct : created_topics) {
            if (ct.segments == segments) {
                announced = MakeSharedSchema(OwnedSchema::DeepCopy(ct.schema.get()));
                break;
            }
        }
        if (request.check) {
            ++subscribe_checked_count;
            if (!fletcher::internal::RunSchemaCheck(this, request.check, announced)) {
                throw PubSubError(PubSubStatus::kSchemaConflict, "mock: check rejected the schema");
            }
        }
        subscribers[segments] = std::move(request.callback);
        return {fletcher::SchemaArrival::Ready(announced)};
    }

    void Unsubscribe(const std::vector<std::string>& segments) override {
        subscribers.erase(segments);
    }

    TopicOptions last_create_options;
    TopicOptions last_subscribe_options;
    int subscribe_checked_count = 0;
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

TEST(PubSubProtoTest, PublisherConstructionWithoutOptionsReachesProviderWithEmptyOptions) {
    auto mock = std::make_shared<MockPubSubProvider>();
    fletcher_gen::integration::pubsub::TelemetryFeed_TelemetryStreamPublisher pub(mock);

    // The generated wrapper always passes a TopicOptions; the option-less constructor still
    // fills TopicDeclaration's profile/bound fields, just from a default-constructed (empty) one.
    EXPECT_TRUE(mock->last_create_options.empty());
}

TEST(PubSubProtoTest, PublisherConstructionWithOptionsReachesProvider) {
    auto mock = std::make_shared<MockPubSubProvider>();
    fletcher::TopicOptions options{.profile = "latest", .max_payload_bytes = 4096};
    fletcher_gen::integration::pubsub::TelemetryFeed_TelemetryStreamPublisher pub(mock, options);

    EXPECT_EQ(mock->last_create_options.profile, "latest");
    EXPECT_EQ(mock->last_create_options.max_payload_bytes, 4096u);
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

TEST(PubSubProtoTest, SubscribeCarryingAProfileReachesProvider) {
    auto mock = std::make_shared<MockPubSubProvider>();
    fletcher_gen::integration::pubsub::TelemetryFeed_TelemetryStreamPublisher pub(mock);
    fletcher_gen::integration::pubsub::TelemetryFeed_TelemetryStreamSubscriber sub(mock);

    sub.Subscribe([](fletcher_gen::integration::pubsub::Telemetry, Attachments) {},
                  fletcher::TopicOptions{.profile = "latest_reader"});

    EXPECT_EQ(mock->last_subscribe_options.profile, "latest_reader");
}

TEST(PubSubProtoTest, SubscribeInPlaceWithOptionsReachesProvider) {
    auto mock = std::make_shared<MockPubSubProvider>();
    fletcher_gen::integration::pubsub::TelemetryFeed_TelemetryStreamPublisher pub(mock);
    fletcher_gen::integration::pubsub::TelemetryFeed_TelemetryStreamSubscriber sub(mock);

    sub.SubscribeInPlace(
        [](const fletcher_gen::integration::pubsub::Telemetry&, const Attachments&) {},
        fletcher::TopicOptions{.profile = "x"});

    EXPECT_EQ(mock->last_subscribe_options.profile, "x");
}

TEST(PubSubProtoTest, SubscribeInPlaceReusesOneRowWithoutCarryingValuesOver) {
    auto mock = std::make_shared<MockPubSubProvider>();
    fletcher_gen::integration::pubsub::TelemetryFeed_TelemetryStreamPublisher pub(mock);
    fletcher_gen::integration::pubsub::TelemetryFeed_TelemetryStreamSubscriber sub(mock);

    std::vector<const void*> addresses;
    std::vector<std::string> names;
    std::vector<int32_t> ids;
    sub.SubscribeInPlace(
        [&](const fletcher_gen::integration::pubsub::Telemetry& msg, const Attachments&) {
            addresses.push_back(&msg);
            names.push_back(std::string(msg.metric_name()));
            ids.push_back(msg.device_id());
        });

    fletcher_gen::integration::pubsub::Telemetry row;
    pub.Publish(row.set_device_id(1).set_value(1.0).set_timestamp(10LL).set_metric_name("cpu"));
    pub.Publish(row.set_device_id(2).set_value(2.0).set_timestamp(20LL).set_metric_name("io"));

    ASSERT_EQ(ids.size(), 2u);
    EXPECT_EQ(ids[0], 1);
    EXPECT_EQ(ids[1], 2);
    // The second sample's shorter name must not leave any of the first behind.
    EXPECT_EQ(names[0], "cpu");
    EXPECT_EQ(names[1], "io");
    EXPECT_EQ(addresses[0], addresses[1]) << "SubscribeInPlace must decode into one reused row";
}

TEST(PubSubProtoTest, UnsubscribeStopsDelivery) {
    auto mock = std::make_shared<MockPubSubProvider>();
    fletcher_gen::integration::pubsub::TelemetryFeed_TelemetryStreamPublisher pub(mock);
    fletcher_gen::integration::pubsub::TelemetryFeed_TelemetryStreamSubscriber sub(mock);

    int count = 0;
    uint64_t sub_id =
        sub.Subscribe([&](fletcher_gen::integration::pubsub::Telemetry, Attachments) { ++count; })
            .subscription_id;

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
    sub.Subscribe([&](fletcher_gen::integration::pubsub::Telemetry msg, const Attachments& att) {
        received = std::move(msg);
        received_att = std::move(att);
    });

    fletcher_gen::integration::pubsub::Telemetry row;
    row.set_device_id(42).set_value(3.14).set_timestamp(1000LL).set_metric_name("cpu");

    const std::vector<uint8_t> payload{0xDE, 0xAD, 0xBE, 0xEF};
    fletcher::Blob blob{payload};
    fletcher::Attachments sent;
    sent.Set("image", blob);
    pub.Publish(row, sent);

    EXPECT_EQ(received.device_id(), 42);
    ASSERT_EQ(received_att.size(), 1u);
    const fletcher::Blob* arrived = received_att.Find("image");
    ASSERT_NE(arrived, nullptr);
    // The generated publisher hands the blob through; a Blob copy retains the
    // owner, so the delivered bytes are the very bytes published (§3.2).
    EXPECT_EQ(arrived->data(), blob.data());
    EXPECT_EQ(arrived->size(), payload.size());
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

// ── Generated Subscribe schema check ───────────────────────────────

TEST(PubSubProtoTest, SubscribeGoesThroughTheSchemaCheck) {
    auto mock = std::make_shared<MockPubSubProvider>();
    fletcher_gen::integration::pubsub::TelemetryFeed_TelemetryStreamPublisher pub(mock);
    fletcher_gen::integration::pubsub::TelemetryFeed_TelemetryStreamSubscriber sub(mock);

    fletcher::Subscriber::SubscribeResult result =
        sub.Subscribe([](fletcher_gen::integration::pubsub::Telemetry, Attachments) {});

    EXPECT_EQ(mock->subscribe_checked_count, 1);
    EXPECT_NE(result.subscription_id, 0u);

    SharedSchema schema;
    EXPECT_EQ(result.schema.Wait(std::chrono::milliseconds(0), &schema), PubSubStatus::kOk);
    EXPECT_TRUE(schema);
}

TEST(PubSubProtoTest, GeneratedSubscribeRefusesAMismatchedSchema) {
    auto mock = std::make_shared<MockPubSubProvider>();
    using Subscriber = fletcher_gen::integration::pubsub::TelemetryFeed_TelemetryStreamSubscriber;
    mock->CreateTopic(Subscriber::TopicSegments(), {MismatchedSchema()});
    Subscriber sub(mock);

    try {
        sub.Subscribe([](fletcher_gen::integration::pubsub::Telemetry, Attachments) {});
        FAIL() << "expected a PubSubError(kSchemaConflict)";
    } catch (const PubSubError& e) {
        EXPECT_EQ(e.status(), PubSubStatus::kSchemaConflict);
    }
    // A rejected check leaves nothing behind: no delivery is registered, and
    // `subscribe_checked_count` — the number of Subscribe calls whose request carried a check —
    // is exactly one (not once per constructor-vs-call, not zero because of an early return).
    EXPECT_EQ(mock->subscribers.count(Subscriber::TopicSegments()), 0u);
    EXPECT_EQ(mock->subscribe_checked_count, 1);
}

TEST(PubSubProtoTest, GeneratedSubscribeInPlaceRefusesAMismatchedSchema) {
    auto mock = std::make_shared<MockPubSubProvider>();
    using Subscriber = fletcher_gen::integration::pubsub::TelemetryFeed_TelemetryStreamSubscriber;
    mock->CreateTopic(Subscriber::TopicSegments(), {MismatchedSchema()});
    Subscriber sub(mock);

    try {
        sub.SubscribeInPlace(
            [](const fletcher_gen::integration::pubsub::Telemetry&, const Attachments&) {});
        FAIL() << "expected a PubSubError(kSchemaConflict)";
    } catch (const PubSubError& e) {
        EXPECT_EQ(e.status(), PubSubStatus::kSchemaConflict);
    }
    // A rejected check leaves nothing behind: no delivery is registered, and
    // `subscribe_checked_count` — the number of Subscribe calls whose request carried a check —
    // is exactly one.
    EXPECT_EQ(mock->subscribers.count(Subscriber::TopicSegments()), 0u);
    EXPECT_EQ(mock->subscribe_checked_count, 1);
}
