// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include <gtest/gtest.h>
#include <nanoarrow/nanoarrow.h>

#include <algorithm>
#include <cstring>
#include <fletcher/core/write_buffer.hpp>
#include <fletcher/pubsub/provider.hpp>
#include <fletcher/pubsub/publisher.hpp>
#include <fletcher/pubsub/subscriber.hpp>
#include <unordered_map>
#include <vector>

using namespace fletcher;

// ---------------------------------------------------------------------------
// Minimal in-process mock provider for testing Publisher/Subscriber.
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
        SharedSchema schema;
        if (it != schemas_.end()) {
            schema = MakeSharedSchema(OwnedSchema::DeepCopy(it->second.get()));
        }
        return {MakeReadySchemaFuture(std::move(schema))};
    }

    void Unsubscribe(const std::vector<std::string>& segments) override {
        callbacks_.erase(Join(segments));
        unsubscribe_count++;
    }

    std::vector<std::string> topics_created;
    int unsubscribe_count = 0;

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

/// Build a nanoarrow schema: struct{ x: int32 }.
static OwnedSchema TestSchema() {
    OwnedSchema schema;
    ArrowSchemaInit(schema.get());
    ArrowSchemaSetTypeStruct(schema.get(), 1);
    ArrowSchemaSetName(schema->children[0], "x");
    ArrowSchemaSetType(schema->children[0], NANOARROW_TYPE_INT32);
    return schema;
}

/// A schema that differs from TestSchema() (different field name + type) so a
/// re-declaration with it is a genuine conflict.
static OwnedSchema TestSchemaB() {
    OwnedSchema schema;
    ArrowSchemaInit(schema.get());
    ArrowSchemaSetTypeStruct(schema.get(), 1);
    ArrowSchemaSetName(schema->children[0], "y");
    ArrowSchemaSetType(schema->children[0], NANOARROW_TYPE_DOUBLE);
    return schema;
}

static const std::vector<std::string> kTopic = {"test", "topic"};

/// Encode a test row as positional format: [null_bitfield(1 byte)] [int32 LE].
static PubSubProvider::RowEncoder MakeTestEncoder(int32_t x) {
    return [x](WriteBuffer& buf) {
        buf.AppendByte(0x00);  // null bitfield: 1 field, not null
        buf.AppendFixed<int32_t>(x);
    };
}

/// Decode the int32 value from a positional-encoded test row.
static int32_t DecodeTestRow(const uint8_t* data, size_t len) {
    EXPECT_EQ(len, 5u);  // 1 byte null bitfield + 4 bytes int32
    int32_t value;
    std::memcpy(&value, data + 1, sizeof(value));
    return value;
}

// ---------------------------------------------------------------------------
// Publisher tests
// ---------------------------------------------------------------------------

TEST(PublisherTest, CreateTopicDelegatesToProvider) {
    auto mock = std::make_shared<MockProvider>();
    Publisher publisher(mock);

    publisher.CreateTopic(kTopic, TestSchema());
    ASSERT_EQ(mock->topics_created.size(), 1u);
    EXPECT_EQ(mock->topics_created[0], "test/topic");
}

TEST(PublisherTest, CreateTopicIsIdempotentForSameSchema) {
    auto mock = std::make_shared<MockProvider>();
    Publisher publisher(mock);

    publisher.CreateTopic(kTopic, TestSchema());
    // Re-declaring with an identical schema is a no-op (lets several publishers
    // share one topic) and does not reach the provider a second time.
    EXPECT_NO_THROW(publisher.CreateTopic(kTopic, TestSchema()));
    EXPECT_EQ(mock->topics_created.size(), 1u);
}

TEST(PublisherTest, CreateTopicRejectsConflictingSchema) {
    auto mock = std::make_shared<MockProvider>();
    Publisher publisher(mock);

    publisher.CreateTopic(kTopic, TestSchema());
    EXPECT_THROW(publisher.CreateTopic(kTopic, TestSchemaB()), std::runtime_error);
}

TEST(PublisherTest, ListTopics) {
    auto mock = std::make_shared<MockProvider>();
    Publisher publisher(mock);

    EXPECT_TRUE(publisher.ListTopics().empty());

    publisher.CreateTopic({"a", "b"}, TestSchema());
    publisher.CreateTopic({"c", "d"}, TestSchema());

    std::vector<std::string> topics = publisher.ListTopics();
    EXPECT_EQ(topics.size(), 2u);

    std::sort(topics.begin(), topics.end());
    EXPECT_EQ(topics[0], "a/b");
    EXPECT_EQ(topics[1], "c/d");
}

TEST(PublisherTest, NullProviderThrows) { EXPECT_THROW(Publisher(nullptr), std::invalid_argument); }

// ---------------------------------------------------------------------------
// Subscriber tests
// ---------------------------------------------------------------------------

TEST(SubscriberTest, PublishDelegatesToProvider) {
    auto mock = std::make_shared<MockProvider>();
    Publisher publisher(mock);
    Subscriber subscriber(mock);

    publisher.CreateTopic(kTopic, TestSchema());

    int32_t received_value = 0;
    (void)subscriber.Subscribe(
        kTopic, [&](uint64_t, const uint8_t* data, size_t len, SharedSchema, Attachments) {
            received_value = DecodeTestRow(data, len);
        });

    publisher.Publish(kTopic, MakeTestEncoder(42));

    EXPECT_EQ(received_value, 42);
}

TEST(SubscriberTest, SubscribeReturnsUniqueIds) {
    auto mock = std::make_shared<MockProvider>();
    Publisher publisher(mock);
    Subscriber subscriber(mock);
    publisher.CreateTopic(kTopic, TestSchema());

    Subscriber::SubscribeResult r1 = subscriber.Subscribe(
        kTopic, [](uint64_t, const uint8_t*, size_t, SharedSchema, Attachments) {});
    Subscriber::SubscribeResult r2 = subscriber.Subscribe(
        kTopic, [](uint64_t, const uint8_t*, size_t, SharedSchema, Attachments) {});

    EXPECT_NE(r1.subscription_id, r2.subscription_id);
}

TEST(SubscriberTest, SubscribeToUnknownTopicSucceeds) {
    auto mock = std::make_shared<MockProvider>();
    Subscriber subscriber(mock);

    // Subscribing to an unknown topic should succeed (subscriber-only process).
    Subscriber::SubscribeResult result = subscriber.Subscribe(
        {"no", "such"}, [](uint64_t, const uint8_t*, size_t, SharedSchema, Attachments) {});
    EXPECT_GT(result.subscription_id, 0u);
}

TEST(SubscriberTest, SubscribeReturnsSchemaFromProvider) {
    auto mock = std::make_shared<MockProvider>();
    Publisher publisher(mock);
    Subscriber subscriber(mock);

    publisher.CreateTopic(kTopic, TestSchema());

    Subscriber::SubscribeResult result = subscriber.Subscribe(
        kTopic, [](uint64_t, const uint8_t*, size_t, SharedSchema, Attachments) {});
    ASSERT_TRUE(result.schema.valid());
    SharedSchema sch = result.schema.get();
    ASSERT_TRUE(sch);
    EXPECT_EQ(sch->n_children, 1);
    EXPECT_EQ(std::string(sch->children[0]->name), "x");
    EXPECT_EQ(std::string(sch->children[0]->format), "i");
}

TEST(SubscriberTest, MultiSubscriberFanOut) {
    auto mock = std::make_shared<MockProvider>();
    Publisher publisher(mock);
    Subscriber subscriber(mock);
    publisher.CreateTopic(kTopic, TestSchema());

    int count_a = 0;
    int count_b = 0;
    (void)subscriber.Subscribe(
        kTopic, [&](uint64_t, const uint8_t*, size_t, SharedSchema, Attachments) { count_a++; });
    (void)subscriber.Subscribe(
        kTopic, [&](uint64_t, const uint8_t*, size_t, SharedSchema, Attachments) { count_b++; });

    publisher.Publish(kTopic, MakeTestEncoder(1));

    EXPECT_EQ(count_a, 1);
    EXPECT_EQ(count_b, 1);

    publisher.Publish(kTopic, MakeTestEncoder(2));
    EXPECT_EQ(count_a, 2);
    EXPECT_EQ(count_b, 2);
}

TEST(SubscriberTest, UnsubscribeRemovesSpecificSubscriber) {
    auto mock = std::make_shared<MockProvider>();
    Publisher publisher(mock);
    Subscriber subscriber(mock);
    publisher.CreateTopic(kTopic, TestSchema());

    int count_a = 0;
    int count_b = 0;
    Subscriber::SubscribeResult ra = subscriber.Subscribe(
        kTopic, [&](uint64_t, const uint8_t*, size_t, SharedSchema, Attachments) { count_a++; });
    (void)subscriber.Subscribe(
        kTopic, [&](uint64_t, const uint8_t*, size_t, SharedSchema, Attachments) { count_b++; });

    publisher.Publish(kTopic, MakeTestEncoder(1));
    EXPECT_EQ(count_a, 1);
    EXPECT_EQ(count_b, 1);

    subscriber.Unsubscribe(ra.subscription_id);
    publisher.Publish(kTopic, MakeTestEncoder(2));
    EXPECT_EQ(count_a, 1);
    EXPECT_EQ(count_b, 2);
}

TEST(SubscriberTest, UnsubscribeLastSubscriberUnsubscribesFromProvider) {
    auto mock = std::make_shared<MockProvider>();
    Publisher publisher(mock);
    Subscriber subscriber(mock);
    publisher.CreateTopic(kTopic, TestSchema());

    Subscriber::SubscribeResult r = subscriber.Subscribe(
        kTopic, [](uint64_t, const uint8_t*, size_t, SharedSchema, Attachments) {});
    EXPECT_EQ(mock->unsubscribe_count, 0);

    subscriber.Unsubscribe(r.subscription_id);
    EXPECT_EQ(mock->unsubscribe_count, 1);
}

TEST(SubscriberTest, UnsubscribeWithRemainingSubscribersKeepsProviderSubscription) {
    auto mock = std::make_shared<MockProvider>();
    Publisher publisher(mock);
    Subscriber subscriber(mock);
    publisher.CreateTopic(kTopic, TestSchema());

    Subscriber::SubscribeResult r1 = subscriber.Subscribe(
        kTopic, [](uint64_t, const uint8_t*, size_t, SharedSchema, Attachments) {});
    (void)subscriber.Subscribe(kTopic,
                               [](uint64_t, const uint8_t*, size_t, SharedSchema, Attachments) {});

    subscriber.Unsubscribe(r1.subscription_id);
    EXPECT_EQ(mock->unsubscribe_count, 0);
}

TEST(SubscriberTest, UnsubscribeUnknownIdThrows) {
    auto mock = std::make_shared<MockProvider>();
    Subscriber subscriber(mock);

    EXPECT_THROW(subscriber.Unsubscribe(999), std::runtime_error);
}

TEST(SubscriberTest, NullProviderThrows) {
    EXPECT_THROW(Subscriber(nullptr), std::invalid_argument);
}

TEST(SubscriberTest, PublishWithAttachmentsFansOutCorrectly) {
    auto mock = std::make_shared<MockProvider>();
    Publisher publisher(mock);
    Subscriber subscriber(mock);
    publisher.CreateTopic(kTopic, TestSchema());

    int32_t received_value = 0;
    Attachments received_att;
    (void)subscriber.Subscribe(
        kTopic, [&](uint64_t, const uint8_t* data, size_t len, SharedSchema, Attachments att) {
            received_value = DecodeTestRow(data, len);
            received_att = std::move(att);
        });

    auto blob = std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{0xDE, 0xAD});

    publisher.Publish(kTopic, MakeTestEncoder(99), {{"img", blob}});

    EXPECT_EQ(received_value, 99);
    ASSERT_EQ(received_att.count("img"), 1u);
    EXPECT_EQ(*received_att.at("img"), *blob);
}
