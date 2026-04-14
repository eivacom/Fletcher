#include <catch2/catch_all.hpp>

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
                     OwnedSchema schema) override {
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
        if (it != callbacks_.end())
            it->second(buf.data(), buf.size(), attachments);
    }

    SubscriptionResult Subscribe(const std::vector<std::string>& segments,
                                 SubscribeCallback callback) override {
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
    REQUIRE(len == 5);  // 1 byte null bitfield + 4 bytes int32
    int32_t value;
    std::memcpy(&value, data + 1, sizeof(value));
    return value;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_CASE("Driver: CreateTopic delegates to provider") {
    auto mock = std::make_shared<MockProvider>();
    Driver driver(mock);

    driver.CreateTopic(kTopic, TestSchema());
    REQUIRE(mock->topics_created.size() == 1);
    CHECK(mock->topics_created[0] == "test/topic");
}

TEST_CASE("Driver: CreateTopic rejects duplicates") {
    auto mock = std::make_shared<MockProvider>();
    Driver driver(mock);

    driver.CreateTopic(kTopic, TestSchema());
    CHECK_THROWS_AS(driver.CreateTopic(kTopic, TestSchema()), std::runtime_error);
}

TEST_CASE("Driver: Publish delegates to provider") {
    auto mock = std::make_shared<MockProvider>();
    Driver driver(mock);

    driver.CreateTopic(kTopic, TestSchema());

    int32_t received_value = 0;
    driver.Subscribe(kTopic, [&](const uint8_t* data, size_t len, Attachments) {
        received_value = DecodeTestRow(data, len);
    });

    driver.Publish(kTopic, MakeTestEncoder(42));

    CHECK(received_value == 42);
}

TEST_CASE("Driver: Subscribe returns unique IDs") {
    auto mock = std::make_shared<MockProvider>();
    Driver driver(mock);
    driver.CreateTopic(kTopic, TestSchema());

    auto r1 = driver.Subscribe(kTopic, [](const uint8_t*, size_t, Attachments) {});
    auto r2 = driver.Subscribe(kTopic, [](const uint8_t*, size_t, Attachments) {});

    CHECK(r1.subscription_id != r2.subscription_id);
}

TEST_CASE("Driver: Subscribe to unknown topic auto-registers") {
    auto mock = std::make_shared<MockProvider>();
    Driver driver(mock);

    // Subscribing to an unknown topic should succeed (subscriber-only process).
    auto result = driver.Subscribe({"no", "such"},
                                   [](const uint8_t*, size_t, Attachments) {});
    CHECK(result.subscription_id > 0);
    CHECK(driver.HasTopic({"no", "such"}));
}

TEST_CASE("Driver: Subscribe returns schema from provider") {
    auto mock = std::make_shared<MockProvider>();
    Driver driver(mock);

    driver.CreateTopic(kTopic, TestSchema());

    auto result = driver.Subscribe(kTopic,
                                   [](const uint8_t*, size_t, Attachments) {});
    REQUIRE(result.schema.valid());
    // Verify structure: one child named "x" with int32 format.
    CHECK(result.schema->n_children == 1);
    CHECK(std::string(result.schema->children[0]->name) == "x");
    CHECK(std::string(result.schema->children[0]->format) == "i");  // int32
}

TEST_CASE("Driver: multi-subscriber fan-out") {
    auto mock = std::make_shared<MockProvider>();
    Driver driver(mock);
    driver.CreateTopic(kTopic, TestSchema());

    int count_a = 0, count_b = 0;
    driver.Subscribe(kTopic, [&](const uint8_t*, size_t, Attachments) { count_a++; });
    driver.Subscribe(kTopic, [&](const uint8_t*, size_t, Attachments) { count_b++; });

    driver.Publish(kTopic, MakeTestEncoder(1));

    CHECK(count_a == 1);
    CHECK(count_b == 1);

    driver.Publish(kTopic, MakeTestEncoder(2));
    CHECK(count_a == 2);
    CHECK(count_b == 2);
}

TEST_CASE("Driver: Unsubscribe removes specific subscriber") {
    auto mock = std::make_shared<MockProvider>();
    Driver driver(mock);
    driver.CreateTopic(kTopic, TestSchema());

    int count_a = 0, count_b = 0;
    auto ra = driver.Subscribe(kTopic, [&](const uint8_t*, size_t, Attachments) { count_a++; });
    driver.Subscribe(kTopic, [&](const uint8_t*, size_t, Attachments) { count_b++; });

    driver.Publish(kTopic, MakeTestEncoder(1));
    CHECK(count_a == 1);
    CHECK(count_b == 1);

    driver.Unsubscribe(ra.subscription_id);
    driver.Publish(kTopic, MakeTestEncoder(2));
    CHECK(count_a == 1);  // no longer incremented
    CHECK(count_b == 2);
}

TEST_CASE("Driver: Unsubscribe last subscriber unsubscribes from provider") {
    auto mock = std::make_shared<MockProvider>();
    Driver driver(mock);
    driver.CreateTopic(kTopic, TestSchema());

    auto r = driver.Subscribe(kTopic, [](const uint8_t*, size_t, Attachments) {});
    CHECK(mock->unsubscribe_count == 0);

    driver.Unsubscribe(r.subscription_id);
    CHECK(mock->unsubscribe_count == 1);
}

TEST_CASE("Driver: Unsubscribe with remaining subscribers keeps provider subscription") {
    auto mock = std::make_shared<MockProvider>();
    Driver driver(mock);
    driver.CreateTopic(kTopic, TestSchema());

    auto r1 = driver.Subscribe(kTopic, [](const uint8_t*, size_t, Attachments) {});
    driver.Subscribe(kTopic, [](const uint8_t*, size_t, Attachments) {});

    driver.Unsubscribe(r1.subscription_id);
    CHECK(mock->unsubscribe_count == 0);  // still have one subscriber
}

TEST_CASE("Driver: Unsubscribe unknown ID throws") {
    auto mock = std::make_shared<MockProvider>();
    Driver driver(mock);

    CHECK_THROWS_AS(driver.Unsubscribe(999), std::runtime_error);
}

TEST_CASE("Driver: ListTopics") {
    auto mock = std::make_shared<MockProvider>();
    Driver driver(mock);

    CHECK(driver.ListTopics().empty());

    driver.CreateTopic({"a", "b"}, TestSchema());
    driver.CreateTopic({"c", "d"}, TestSchema());

    auto topics = driver.ListTopics();
    CHECK(topics.size() == 2);

    // Sort for deterministic comparison.
    std::sort(topics.begin(), topics.end());
    CHECK(topics[0] == "a/b");
    CHECK(topics[1] == "c/d");
}

TEST_CASE("Driver: HasTopic") {
    auto mock = std::make_shared<MockProvider>();
    Driver driver(mock);

    CHECK_FALSE(driver.HasTopic(kTopic));

    driver.CreateTopic(kTopic, TestSchema());
    CHECK(driver.HasTopic(kTopic));
    CHECK_FALSE(driver.HasTopic({"other"}));
}

TEST_CASE("Driver: null provider throws") {
    CHECK_THROWS_AS(Driver(nullptr), std::invalid_argument);
}

TEST_CASE("Driver: publish with attachments fans out correctly") {
    auto mock = std::make_shared<MockProvider>();
    Driver driver(mock);
    driver.CreateTopic(kTopic, TestSchema());

    int32_t received_value = 0;
    Attachments received_att;
    driver.Subscribe(kTopic, [&](const uint8_t* data, size_t len, Attachments att) {
        received_value = DecodeTestRow(data, len);
        received_att = std::move(att);
    });

    auto blob = std::make_shared<const std::vector<uint8_t>>(
        std::vector<uint8_t>{0xDE, 0xAD});

    driver.Publish(kTopic, MakeTestEncoder(99), {{"img", blob}});

    CHECK(received_value == 99);
    REQUIRE(received_att.count("img") == 1);
    CHECK(*received_att.at("img") == *blob);
}
