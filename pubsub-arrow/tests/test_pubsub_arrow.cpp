// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include <arrow/api.h>
#include <gtest/gtest.h>

#include <cstring>
#include <fletcher/core/write_buffer.hpp>
#include <fletcher/pubsub/provider.hpp>
#include <fletcher/pubsub_arrow/publisher_arrow.hpp>
#include <fletcher/pubsub_arrow/subscriber_arrow.hpp>
#include <unordered_map>
#include <vector>

using namespace fletcher;

// ---------------------------------------------------------------------------
// Mock provider (nanoarrow interface)
// ---------------------------------------------------------------------------

class MockProvider : public PubSubProvider {
   public:
    void CreateTopic(const std::vector<std::string>& segments, OwnedSchema schema) override {
        std::string key = Join(segments);
        topics_created.push_back(key);
        if (schema) {
            schemas_[key] = OwnedSchema::DeepCopy(schema.get());
        }
    }

    void Publish(const std::vector<std::string>& segments, RowEncoder encoder,
                 const Attachments& attachments) override {
        std::string key = Join(segments);

        std::vector<uint8_t> buf;
        VectorWriteBuffer wb(buf);
        encoder(wb);

        auto it = callbacks_.find(key);
        if (it != callbacks_.end()) {
            SharedSchema sp;
            auto sit = schemas_.find(key);
            if (sit != schemas_.end()) {
                sp = MakeSharedSchema(OwnedSchema::DeepCopy(sit->second.get()));
            }
            it->second(buf.data(), buf.size(), sp, attachments);
        }
    }

    SubscriptionResult Subscribe(const std::vector<std::string>& segments,
                                 SubscribeCallback callback) override {
        std::string key = Join(segments);
        callbacks_[key] = std::move(callback);
        auto it = schemas_.find(key);
        OwnedSchema schema;
        if (it != schemas_.end()) {
            schema = OwnedSchema::DeepCopy(it->second.get());
        }
        return {std::move(schema)};
    }

    void Unsubscribe(const std::vector<std::string>& segments) override {
        callbacks_.erase(Join(segments));
    }

    std::vector<std::string> topics_created;

   private:
    std::unordered_map<std::string, SubscribeCallback> callbacks_;
    std::unordered_map<std::string, OwnedSchema> schemas_;

    static std::string Join(const std::vector<std::string>& segs) {
        std::string out;
        for (size_t i = 0; i < segs.size(); ++i) {
            if (i > 0) {
                out += '/';
            }
            out += segs[i];
        }
        return out;
    }
};

static auto TestSchema() {
    return arrow::schema({
        arrow::field("x", arrow::int32()),
        arrow::field("name", arrow::utf8()),
    });
}

static const std::vector<std::string> kTopic = {"test", "topic"};

// ---------------------------------------------------------------------------
// PublisherArrow tests
// ---------------------------------------------------------------------------

TEST(PublisherArrowTest, CreateTopicConvertsArrowSchema) {
    auto mock = std::make_shared<MockProvider>();
    PublisherArrow pub(mock);

    pub.CreateTopic(kTopic, TestSchema());
    ASSERT_EQ(mock->topics_created.size(), 1);
    EXPECT_EQ(mock->topics_created[0], "test/topic");
}

TEST(PublisherArrowTest, ListTopicsAndHasTopic) {
    auto mock = std::make_shared<MockProvider>();
    PublisherArrow pub(mock);

    EXPECT_TRUE(pub.ListTopics().empty());
    EXPECT_FALSE(pub.HasTopic(kTopic));

    pub.CreateTopic(kTopic, TestSchema());
    EXPECT_TRUE(pub.HasTopic(kTopic));

    std::vector<std::string> topics = pub.ListTopics();
    ASSERT_EQ(topics.size(), 1);
    EXPECT_EQ(topics[0], "test/topic");
}

// ---------------------------------------------------------------------------
// SubscriberArrow tests
// ---------------------------------------------------------------------------

TEST(SubscriberArrowTest, SubscribeReturnsArrowSchema) {
    auto mock = std::make_shared<MockProvider>();
    PublisherArrow pub(mock);
    SubscriberArrow sub(mock);

    pub.CreateTopic(kTopic, TestSchema());
    SubscriberArrow::SubscribeResult result = sub.Subscribe(kTopic, [](ArrowRow, Attachments) {});

    ASSERT_NE(result.schema, nullptr);
    EXPECT_EQ(result.schema->num_fields(), 2);
    EXPECT_EQ(result.schema->field(0)->name(), "x");
    EXPECT_TRUE(result.schema->field(0)->type()->Equals(*arrow::int32()));
    EXPECT_EQ(result.schema->field(1)->name(), "name");
    EXPECT_TRUE(result.schema->field(1)->type()->Equals(*arrow::utf8()));
}

TEST(PubSubArrowTest, PublishSubscribeRoundtripWithArrowRow) {
    auto mock = std::make_shared<MockProvider>();
    PublisherArrow pub(mock);
    SubscriberArrow sub(mock);

    pub.CreateTopic(kTopic, TestSchema());

    ArrowRow received;
    sub.Subscribe(kTopic, [&](ArrowRow row, Attachments) { received = std::move(row); });

    ArrowRow sent = {
        std::make_shared<arrow::Int32Scalar>(42),
        std::make_shared<arrow::StringScalar>("hello"),
    };
    pub.Publish(kTopic, sent);

    ASSERT_EQ(received.size(), 2);
    EXPECT_EQ(static_cast<const arrow::Int32Scalar&>(*received[0]).value, 42);
    EXPECT_EQ(static_cast<const arrow::StringScalar&>(*received[1]).value->ToString(), "hello");
}

TEST(PubSubArrowTest, PublishWithAttachments) {
    auto mock = std::make_shared<MockProvider>();
    PublisherArrow pub(mock);
    SubscriberArrow sub(mock);

    pub.CreateTopic(kTopic, TestSchema());

    Attachments received_att;
    sub.Subscribe(kTopic, [&](ArrowRow, Attachments att) { received_att = std::move(att); });

    auto blob = std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{0xDE, 0xAD});

    ArrowRow row = {
        std::make_shared<arrow::Int32Scalar>(1),
        std::make_shared<arrow::StringScalar>("a"),
    };
    pub.Publish(kTopic, row, {{"img", blob}});

    ASSERT_EQ(received_att.count("img"), 1);
    EXPECT_EQ(*received_att.at("img"), *blob);
}

TEST(PubSubArrowTest, PublishDirectPassthrough) {
    auto mock = std::make_shared<MockProvider>();
    PublisherArrow pub(mock);
    SubscriberArrow sub(mock);

    std::shared_ptr<arrow::Schema> schema = arrow::schema({arrow::field("x", arrow::int32())});
    pub.CreateTopic(kTopic, schema);

    ArrowRow received;
    sub.Subscribe(kTopic, [&](ArrowRow row, Attachments) { received = std::move(row); });

    pub.PublishDirect(kTopic, [](WriteBuffer& buf) {
        buf.AppendByte(0x00);
        int32_t val = 99;
        buf.Append(reinterpret_cast<const uint8_t*>(&val), sizeof(val));
    });

    ASSERT_EQ(received.size(), 1);
    EXPECT_EQ(static_cast<const arrow::Int32Scalar&>(*received[0]).value, 99);
}

TEST(PubSubArrowTest, Unsubscribe) {
    auto mock = std::make_shared<MockProvider>();
    PublisherArrow pub(mock);
    SubscriberArrow sub(mock);

    pub.CreateTopic(kTopic, TestSchema());

    int count = 0;
    SubscriberArrow::SubscribeResult result =
        sub.Subscribe(kTopic, [&](ArrowRow, Attachments) { count++; });

    ArrowRow row = {
        std::make_shared<arrow::Int32Scalar>(1),
        std::make_shared<arrow::StringScalar>("a"),
    };
    pub.Publish(kTopic, row);
    EXPECT_EQ(count, 1);

    sub.Unsubscribe(result.subscription_id);
    pub.Publish(kTopic, row);
    EXPECT_EQ(count, 1);
}
