#include <gtest/gtest.h>

#include <pubsub/driver.hpp>
#include <pubsub/write_buffer.hpp>

#include <nanoarrow/nanoarrow.h>

#include <algorithm>
#include <cstring>
#include <vector>

using namespace fletcher;

// ---------------------------------------------------------------------------
// Minimal in-process mock provider for testing the Driver.
// ---------------------------------------------------------------------------

class MockProvider : public PubSub {
 public:
    void CreateTopic(const std::vector<std::string>& segments,
                     OwnedSchema schema,
                     std::any /*config*/) override {
        std::string key = Join(segments);
        topics_created.push_back(key);
        if (schema)
            schemas_[key] = OwnedSchema::DeepCopy(schema.get());
    }

    void Publish(const std::vector<std::string>& segments,
                 RowEncoder encoder,
                 const Attachments& attachments) override {
        std::string key = Join(segments);

        // Encode into a temporary buffer.
        std::vector<uint8_t> buf;
        VectorWriteBuffer wb(buf);
        encoder(wb);

        auto it = callbacks_.find(key);
        if (it != callbacks_.end()) {
            SharedSchema sp;
            auto sit = schemas_.find(key);
            if (sit != schemas_.end())
                sp = MakeSharedSchema(OwnedSchema::DeepCopy(sit->second.get()));
            it->second(buf.data(), buf.size(), sp, attachments);
        }
    }

    SubscriptionResult Subscribe(const std::vector<std::string>& segments,
                                 SubscribeCallback callback,
                                 std::any /*config*/) override {
        std::string key = Join(segments);
        callbacks_[key] = std::move(callback);
        auto it = schemas_.find(key);
        OwnedSchema schema;
        if (it != schemas_.end())
            schema = OwnedSchema::DeepCopy(it->second.get());
        return {std::move(schema)};
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
            if (i > 0) out += '/';
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

static const std::vector<std::string> kTopic = {"test", "topic"};

/// Encode a test row as positional format: [null_bitfield(1 byte)] [int32 LE].
static PubSub::RowEncoder MakeTestEncoder(int32_t x) {
    return [x](WriteBuffer& buf) {
        buf.AppendByte(0x00);          // null bitfield: 1 field, not null
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
// Tests
// ---------------------------------------------------------------------------

TEST(DriverTest, CreateTopicDelegatesToProvider) {
    auto mock = std::make_shared<MockProvider>();
    Driver driver(mock);

    driver.CreateTopic(kTopic, TestSchema());
    ASSERT_EQ(mock->topics_created.size(), 1u);
    EXPECT_EQ(mock->topics_created[0], "test/topic");
}

TEST(DriverTest, CreateTopicRejectsDuplicates) {
    auto mock = std::make_shared<MockProvider>();
    Driver driver(mock);

    driver.CreateTopic(kTopic, TestSchema());
    EXPECT_THROW(driver.CreateTopic(kTopic, TestSchema()), std::runtime_error);
}

TEST(DriverTest, PublishDelegatesToProvider) {
    auto mock = std::make_shared<MockProvider>();
    Driver driver(mock);

    driver.CreateTopic(kTopic, TestSchema());

    int32_t received_value = 0;
    driver.Subscribe(kTopic, [&](const uint8_t* data, size_t len, SharedSchema, Attachments) {
        received_value = DecodeTestRow(data, len);
    });

    driver.Publish(kTopic, MakeTestEncoder(42));

    EXPECT_EQ(received_value, 42);
}

TEST(DriverTest, SubscribeReturnsUniqueIds) {
    auto mock = std::make_shared<MockProvider>();
    Driver driver(mock);
    driver.CreateTopic(kTopic, TestSchema());

    auto r1 = driver.Subscribe(kTopic, [](const uint8_t*, size_t, SharedSchema, Attachments) {});
    auto r2 = driver.Subscribe(kTopic, [](const uint8_t*, size_t, SharedSchema, Attachments) {});

    EXPECT_NE(r1.subscription_id, r2.subscription_id);
}

TEST(DriverTest, SubscribeToUnknownTopicAutoRegisters) {
    auto mock = std::make_shared<MockProvider>();
    Driver driver(mock);

    // Subscribing to an unknown topic should succeed (subscriber-only process).
    auto result = driver.Subscribe({"no", "such"},
                                   [](const uint8_t*, size_t, SharedSchema, Attachments) {});
    EXPECT_GT(result.subscription_id, 0u);
    EXPECT_TRUE(driver.HasTopic({"no", "such"}));
}

TEST(DriverTest, SubscribeReturnsSchemaFromProvider) {
    auto mock = std::make_shared<MockProvider>();
    Driver driver(mock);

    driver.CreateTopic(kTopic, TestSchema());

    auto result = driver.Subscribe(kTopic,
                                   [](const uint8_t*, size_t, SharedSchema, Attachments) {});
    ASSERT_TRUE(result.schema.valid());
    // Verify structure: one child named "x" with int32 format.
    EXPECT_EQ(result.schema->n_children, 1);
    EXPECT_EQ(std::string(result.schema->children[0]->name), "x");
    EXPECT_EQ(std::string(result.schema->children[0]->format), "i");  // int32
}

TEST(DriverTest, MultiSubscriberFanOut) {
    auto mock = std::make_shared<MockProvider>();
    Driver driver(mock);
    driver.CreateTopic(kTopic, TestSchema());

    int count_a = 0, count_b = 0;
    driver.Subscribe(kTopic, [&](const uint8_t*, size_t, SharedSchema, Attachments) { count_a++; });
    driver.Subscribe(kTopic, [&](const uint8_t*, size_t, SharedSchema, Attachments) { count_b++; });

    driver.Publish(kTopic, MakeTestEncoder(1));

    EXPECT_EQ(count_a, 1);
    EXPECT_EQ(count_b, 1);

    driver.Publish(kTopic, MakeTestEncoder(2));
    EXPECT_EQ(count_a, 2);
    EXPECT_EQ(count_b, 2);
}

TEST(DriverTest, UnsubscribeRemovesSpecificSubscriber) {
    auto mock = std::make_shared<MockProvider>();
    Driver driver(mock);
    driver.CreateTopic(kTopic, TestSchema());

    int count_a = 0, count_b = 0;
    auto ra = driver.Subscribe(kTopic, [&](const uint8_t*, size_t, SharedSchema, Attachments) { count_a++; });
    driver.Subscribe(kTopic, [&](const uint8_t*, size_t, SharedSchema, Attachments) { count_b++; });

    driver.Publish(kTopic, MakeTestEncoder(1));
    EXPECT_EQ(count_a, 1);
    EXPECT_EQ(count_b, 1);

    driver.Unsubscribe(ra.subscription_id);
    driver.Publish(kTopic, MakeTestEncoder(2));
    EXPECT_EQ(count_a, 1);  // no longer incremented
    EXPECT_EQ(count_b, 2);
}

TEST(DriverTest, UnsubscribeLastSubscriberUnsubscribesFromProvider) {
    auto mock = std::make_shared<MockProvider>();
    Driver driver(mock);
    driver.CreateTopic(kTopic, TestSchema());

    auto r = driver.Subscribe(kTopic, [](const uint8_t*, size_t, SharedSchema, Attachments) {});
    EXPECT_EQ(mock->unsubscribe_count, 0);

    driver.Unsubscribe(r.subscription_id);
    EXPECT_EQ(mock->unsubscribe_count, 1);
}

TEST(DriverTest, UnsubscribeWithRemainingSubscribersKeepsProviderSubscription) {
    auto mock = std::make_shared<MockProvider>();
    Driver driver(mock);
    driver.CreateTopic(kTopic, TestSchema());

    auto r1 = driver.Subscribe(kTopic, [](const uint8_t*, size_t, SharedSchema, Attachments) {});
    driver.Subscribe(kTopic, [](const uint8_t*, size_t, SharedSchema, Attachments) {});

    driver.Unsubscribe(r1.subscription_id);
    EXPECT_EQ(mock->unsubscribe_count, 0);  // still have one subscriber
}

TEST(DriverTest, UnsubscribeUnknownIdThrows) {
    auto mock = std::make_shared<MockProvider>();
    Driver driver(mock);

    EXPECT_THROW(driver.Unsubscribe(999), std::runtime_error);
}

TEST(DriverTest, ListTopics) {
    auto mock = std::make_shared<MockProvider>();
    Driver driver(mock);

    EXPECT_TRUE(driver.ListTopics().empty());

    driver.CreateTopic({"a", "b"}, TestSchema());
    driver.CreateTopic({"c", "d"}, TestSchema());

    auto topics = driver.ListTopics();
    EXPECT_EQ(topics.size(), 2u);

    // Sort for deterministic comparison.
    std::sort(topics.begin(), topics.end());
    EXPECT_EQ(topics[0], "a/b");
    EXPECT_EQ(topics[1], "c/d");
}

TEST(DriverTest, HasTopic) {
    auto mock = std::make_shared<MockProvider>();
    Driver driver(mock);

    EXPECT_FALSE(driver.HasTopic(kTopic));

    driver.CreateTopic(kTopic, TestSchema());
    EXPECT_TRUE(driver.HasTopic(kTopic));
    EXPECT_FALSE(driver.HasTopic({"other"}));
}

TEST(DriverTest, NullProviderThrows) {
    EXPECT_THROW(Driver(nullptr), std::invalid_argument);
}

TEST(DriverTest, PublishWithAttachmentsFansOutCorrectly) {
    auto mock = std::make_shared<MockProvider>();
    Driver driver(mock);
    driver.CreateTopic(kTopic, TestSchema());

    int32_t received_value = 0;
    Attachments received_att;
    driver.Subscribe(kTopic, [&](const uint8_t* data, size_t len, SharedSchema, Attachments att) {
        received_value = DecodeTestRow(data, len);
        received_att = std::move(att);
    });

    auto blob = std::make_shared<const std::vector<uint8_t>>(
        std::vector<uint8_t>{0xDE, 0xAD});

    driver.Publish(kTopic, MakeTestEncoder(99), {{"img", blob}});

    EXPECT_EQ(received_value, 99);
    ASSERT_EQ(received_att.count("img"), 1u);
    EXPECT_EQ(*received_att.at("img"), *blob);
}
