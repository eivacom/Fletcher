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
                     std::shared_ptr<arrow::Schema> /*schema*/) override {
        std::string key = Join(segments);
        topics_created.push_back(key);
    }

    void Publish(const std::vector<std::string>& segments,
                 const Envelope& envelope) override {
        std::string key = Join(segments);
        auto it = callbacks_.find(key);
        if (it != callbacks_.end())
            it->second(envelope);
    }

    void Subscribe(const std::vector<std::string>& segments,
                   SubscribeCallback callback) override {
        callbacks_[Join(segments)] = std::move(callback);
    }

    void Unsubscribe(const std::vector<std::string>& segments) override {
        callbacks_.erase(Join(segments));
        unsubscribe_count++;
    }

    std::vector<std::string> topics_created;
    int unsubscribe_count = 0;

 private:
    std::unordered_map<std::string, SubscribeCallback> callbacks_;

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

    // Subscribe so the mock has a callback to verify delivery.
    Envelope received;
    driver.Subscribe(kTopic, [&](const Envelope& env) { received = env; });

    Envelope sent;
    sent.row = {0x01, 0x02, 0x03};
    driver.Publish(kTopic, sent);

    CHECK(received.row == sent.row);
}

TEST_CASE("Driver: Subscribe returns unique IDs") {
    auto mock = std::make_shared<MockProvider>();
    Driver driver(mock);
    driver.CreateTopic(kTopic, TestSchema());

    auto id1 = driver.Subscribe(kTopic, [](const Envelope&) {});
    auto id2 = driver.Subscribe(kTopic, [](const Envelope&) {});

    CHECK(id1 != id2);
}

TEST_CASE("Driver: Subscribe to unknown topic throws") {
    auto mock = std::make_shared<MockProvider>();
    Driver driver(mock);

    CHECK_THROWS_AS(
        driver.Subscribe({"no", "such"}, [](const Envelope&) {}),
        std::runtime_error);
}

TEST_CASE("Driver: multi-subscriber fan-out") {
    auto mock = std::make_shared<MockProvider>();
    Driver driver(mock);
    driver.CreateTopic(kTopic, TestSchema());

    int count_a = 0, count_b = 0;
    driver.Subscribe(kTopic, [&](const Envelope&) { count_a++; });
    driver.Subscribe(kTopic, [&](const Envelope&) { count_b++; });

    Envelope env;
    env.row = {0xAA};
    driver.Publish(kTopic, env);

    CHECK(count_a == 1);
    CHECK(count_b == 1);

    driver.Publish(kTopic, env);
    CHECK(count_a == 2);
    CHECK(count_b == 2);
}

TEST_CASE("Driver: Unsubscribe removes specific subscriber") {
    auto mock = std::make_shared<MockProvider>();
    Driver driver(mock);
    driver.CreateTopic(kTopic, TestSchema());

    int count_a = 0, count_b = 0;
    auto id_a = driver.Subscribe(kTopic, [&](const Envelope&) { count_a++; });
    driver.Subscribe(kTopic, [&](const Envelope&) { count_b++; });

    Envelope env;
    env.row = {0xBB};
    driver.Publish(kTopic, env);
    CHECK(count_a == 1);
    CHECK(count_b == 1);

    driver.Unsubscribe(id_a);
    driver.Publish(kTopic, env);
    CHECK(count_a == 1);  // no longer incremented
    CHECK(count_b == 2);
}

TEST_CASE("Driver: Unsubscribe last subscriber unsubscribes from provider") {
    auto mock = std::make_shared<MockProvider>();
    Driver driver(mock);
    driver.CreateTopic(kTopic, TestSchema());

    auto id = driver.Subscribe(kTopic, [](const Envelope&) {});
    CHECK(mock->unsubscribe_count == 0);

    driver.Unsubscribe(id);
    CHECK(mock->unsubscribe_count == 1);
}

TEST_CASE("Driver: Unsubscribe with remaining subscribers keeps provider subscription") {
    auto mock = std::make_shared<MockProvider>();
    Driver driver(mock);
    driver.CreateTopic(kTopic, TestSchema());

    auto id1 = driver.Subscribe(kTopic, [](const Envelope&) {});
    driver.Subscribe(kTopic, [](const Envelope&) {});

    driver.Unsubscribe(id1);
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

    Envelope received;
    driver.Subscribe(kTopic, [&](const Envelope& env) { received = env; });

    auto blob = std::make_shared<const std::vector<uint8_t>>(
        std::vector<uint8_t>{0xDE, 0xAD});

    Envelope sent;
    sent.row = {0x01};
    sent.attachments["img"] = blob;
    driver.Publish(kTopic, sent);

    CHECK(received.row == sent.row);
    REQUIRE(received.attachments.count("img") == 1);
    CHECK(*received.attachments.at("img") == *blob);
}
