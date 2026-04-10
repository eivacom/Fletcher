#include <catch2/catch_all.hpp>

#include <pubsub/driver.hpp>

#include <arrow/api.h>

#include <atomic>
#include <thread>
#include <vector>

using namespace fletcher;

// ---------------------------------------------------------------------------
// Minimal in-process mock provider for testing the Driver.
// ---------------------------------------------------------------------------

class MockProvider : public PubSubProvider {
 public:
    void CreateTopic(const std::vector<std::string>& segments,
                     std::shared_ptr<arrow::Schema> schema) override {
        std::string key = Join(segments);
        topics_created.push_back(key);
        if (schema) schemas_[key] = schema;
    }

    void Publish(const std::vector<std::string>& segments,
                 const ArrowRow& row,
                 const Attachments& attachments) override {
        std::string key = Join(segments);
        auto it = callbacks_.find(key);
        if (it != callbacks_.end())
            it->second(row, attachments);
    }

    SubscriptionResult Subscribe(const std::vector<std::string>& segments,
                                 SubscribeCallback callback) override {
        callbacks_[Join(segments)] = std::move(callback);
        auto it = schemas_.find(Join(segments));
        return {it != schemas_.end() ? it->second : nullptr};
    }

    void Unsubscribe(const std::vector<std::string>& segments) override {
        callbacks_.erase(Join(segments));
        unsubscribe_count++;
    }

    std::vector<std::string> topics_created;
    int unsubscribe_count = 0;

 private:
    std::unordered_map<std::string, SubscribeCallback> callbacks_;
    std::unordered_map<std::string, std::shared_ptr<arrow::Schema>> schemas_;

    static std::string Join(const std::vector<std::string>& segs) {
        std::string out;
        for (size_t i = 0; i < segs.size(); ++i) {
            if (i > 0) out += '/';
            out += segs[i];
        }
        return out;
    }
};

static auto TestSchema() {
    return arrow::schema({arrow::field("x", arrow::int32())});
}

static const std::vector<std::string> kTopic = {"test", "topic"};

static ArrowRow MakeTestRow(int32_t x) {
    return { std::make_shared<arrow::Int32Scalar>(x) };
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

    ArrowRow received;
    driver.Subscribe(kTopic, [&](ArrowRow row, Attachments) { received = std::move(row); });

    auto sent = MakeTestRow(42);
    driver.Publish(kTopic, sent);

    REQUIRE(received.size() == 1);
    CHECK(static_cast<const arrow::Int32Scalar&>(*received[0]).value == 42);
}

TEST_CASE("Driver: Subscribe returns unique IDs") {
    auto mock = std::make_shared<MockProvider>();
    Driver driver(mock);
    driver.CreateTopic(kTopic, TestSchema());

    auto r1 = driver.Subscribe(kTopic, [](ArrowRow, Attachments) {});
    auto r2 = driver.Subscribe(kTopic, [](ArrowRow, Attachments) {});

    CHECK(r1.subscription_id != r2.subscription_id);
}

TEST_CASE("Driver: Subscribe to unknown topic auto-registers") {
    auto mock = std::make_shared<MockProvider>();
    Driver driver(mock);

    // Subscribing to an unknown topic should succeed (subscriber-only process).
    auto result = driver.Subscribe({"no", "such"}, [](ArrowRow, Attachments) {});
    CHECK(result.subscription_id > 0);
    CHECK(driver.HasTopic({"no", "such"}));
}

TEST_CASE("Driver: Subscribe returns schema from provider") {
    auto mock = std::make_shared<MockProvider>();
    Driver driver(mock);

    auto schema = TestSchema();
    driver.CreateTopic(kTopic, schema);

    auto result = driver.Subscribe(kTopic, [](ArrowRow, Attachments) {});
    REQUIRE(result.schema != nullptr);
    CHECK(result.schema->Equals(*schema));
}

TEST_CASE("Driver: multi-subscriber fan-out") {
    auto mock = std::make_shared<MockProvider>();
    Driver driver(mock);
    driver.CreateTopic(kTopic, TestSchema());

    int count_a = 0, count_b = 0;
    driver.Subscribe(kTopic, [&](ArrowRow, Attachments) { count_a++; });
    driver.Subscribe(kTopic, [&](ArrowRow, Attachments) { count_b++; });

    driver.Publish(kTopic, MakeTestRow(1));

    CHECK(count_a == 1);
    CHECK(count_b == 1);

    driver.Publish(kTopic, MakeTestRow(2));
    CHECK(count_a == 2);
    CHECK(count_b == 2);
}

TEST_CASE("Driver: Unsubscribe removes specific subscriber") {
    auto mock = std::make_shared<MockProvider>();
    Driver driver(mock);
    driver.CreateTopic(kTopic, TestSchema());

    int count_a = 0, count_b = 0;
    auto ra = driver.Subscribe(kTopic, [&](ArrowRow, Attachments) { count_a++; });
    driver.Subscribe(kTopic, [&](ArrowRow, Attachments) { count_b++; });

    driver.Publish(kTopic, MakeTestRow(1));
    CHECK(count_a == 1);
    CHECK(count_b == 1);

    driver.Unsubscribe(ra.subscription_id);
    driver.Publish(kTopic, MakeTestRow(2));
    CHECK(count_a == 1);  // no longer incremented
    CHECK(count_b == 2);
}

TEST_CASE("Driver: Unsubscribe last subscriber unsubscribes from provider") {
    auto mock = std::make_shared<MockProvider>();
    Driver driver(mock);
    driver.CreateTopic(kTopic, TestSchema());

    auto r = driver.Subscribe(kTopic, [](ArrowRow, Attachments) {});
    CHECK(mock->unsubscribe_count == 0);

    driver.Unsubscribe(r.subscription_id);
    CHECK(mock->unsubscribe_count == 1);
}

TEST_CASE("Driver: Unsubscribe with remaining subscribers keeps provider subscription") {
    auto mock = std::make_shared<MockProvider>();
    Driver driver(mock);
    driver.CreateTopic(kTopic, TestSchema());

    auto r1 = driver.Subscribe(kTopic, [](ArrowRow, Attachments) {});
    driver.Subscribe(kTopic, [](ArrowRow, Attachments) {});

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

    ArrowRow received_row;
    Attachments received_att;
    driver.Subscribe(kTopic, [&](ArrowRow row, Attachments att) {
        received_row = std::move(row);
        received_att = std::move(att);
    });

    auto blob = std::make_shared<const std::vector<uint8_t>>(
        std::vector<uint8_t>{0xDE, 0xAD});

    driver.Publish(kTopic, MakeTestRow(99), {{"img", blob}});

    REQUIRE(received_row.size() == 1);
    CHECK(static_cast<const arrow::Int32Scalar&>(*received_row[0]).value == 99);
    REQUIRE(received_att.count("img") == 1);
    CHECK(*received_att.at("img") == *blob);
}
