// schema-transport integration test.
//
// Drives the FastDDSPubSubProvider's `/__schema` companion-topic
// mechanism through three timing scenarios that the per-provider unit
// tests don't cover:
//
//  1. Subscriber joins *after* publisher created the topic and
//     published a data sample. TRANSIENT_LOCAL durability on both the
//     /__schema topic and the data topic should still deliver the
//     schema + the retained data sample to the late joiner.
//
//  2. Subscriber starts subscribing *before* the publisher has created
//     the topic. Subscribe internally polls /__schema for up to 5 s, so
//     it should return successfully once CreateTopic fires.
//
//  3. Publisher 1 creates the topic and publishes a row, then exits
//     (its provider is destroyed). A new publisher takes over the same
//     topic on the same DDS domain and publishes another row. A
//     subscriber that joins after the takeover should receive the
//     schema from the new publisher's /__schema writer.

#include <pubsub_arrow/pubsub_arrow.hpp>
#include <fast_dds_pubsub_provider.hpp>

#include <arrow/api.h>
#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace fletcher;
using namespace std::chrono_literals;

namespace {

// High domain ids keep each test isolated from other DDS traffic and
// from each other (each test uses its own domain so a stray retained
// sample from one test cannot match a reader in the next).
constexpr uint32_t kLateJoinDomain   = 141;
constexpr uint32_t kEarlySubDomain   = 142;
constexpr uint32_t kRestartDomain    = 143;

std::shared_ptr<arrow::Schema> SensorSchema() {
    return arrow::schema({
        arrow::field("sensor_id",   arrow::int32(),   false),
        arrow::field("temperature", arrow::float64(), false),
        arrow::field("label",       arrow::utf8(),    false),
    });
}

ArrowRow SensorRow(int32_t id, double temp, const std::string& label) {
    return {
        std::make_shared<arrow::Int32Scalar>(id),
        std::make_shared<arrow::DoubleScalar>(temp),
        std::make_shared<arrow::StringScalar>(label),
    };
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────
// Test 1: Late join — subscriber connects after publisher publishes.
// ─────────────────────────────────────────────────────────────────────
TEST(SchemaTransportTest, SubscriberJoiningAfterPublishStillGetsSchemaAndData) {
    auto pub_provider = std::make_shared<FastDDSPubSubProvider>(kLateJoinDomain);

    PubSubArrow pub(pub_provider);
    const auto schema = SensorSchema();
    const std::vector<std::string> topic{"sensor", "late-join"};

    pub.CreateTopic(topic, schema);
    pub.Publish(topic, SensorRow(42, 23.5, "before-sub"));

    // Subscriber joins on a separate provider after the publish has
    // already happened. TRANSIENT_LOCAL on the schema topic AND the
    // data topic should both still deliver.
    auto sub_provider = std::make_shared<FastDDSPubSubProvider>(kLateJoinDomain);

    std::mutex mu;
    std::condition_variable cv;
    bool got_row = false;
    ArrowRow rx_row;

    PubSubArrow sub(sub_provider);

    auto result = sub.Subscribe(topic, [&](ArrowRow row, Attachments) {
        std::lock_guard<std::mutex> lk(mu);
        rx_row = std::move(row);
        got_row = true;
        cv.notify_all();
    });

    ASSERT_NE(result.schema, nullptr)
        << "schema must be delivered to late joiner via /__schema TRANSIENT_LOCAL";
    EXPECT_TRUE(result.schema->Equals(*schema, /*check_metadata=*/false));

    {
        std::unique_lock<std::mutex> lk(mu);
        ASSERT_TRUE(cv.wait_for(lk, 5s, [&] { return got_row; }))
            << "TRANSIENT_LOCAL data sample must reach the late-joining subscriber";
    }
    sub.Unsubscribe(result.subscription_id);

    ASSERT_EQ(rx_row.size(), 3u);
    EXPECT_EQ(std::static_pointer_cast<arrow::Int32Scalar>(rx_row[0])->value, 42);
    EXPECT_EQ(
        std::static_pointer_cast<arrow::StringScalar>(rx_row[2])->ToString(),
        "before-sub");
}

// ─────────────────────────────────────────────────────────────────────
// Test 2: Subscribe before publish — Subscribe polls /__schema and
// waits for CreateTopic.
// ─────────────────────────────────────────────────────────────────────
TEST(SchemaTransportTest, SubscribeWaitsUntilPublisherCreatesTopic) {
    auto pub_provider = std::make_shared<FastDDSPubSubProvider>(kEarlySubDomain);
    auto sub_provider = std::make_shared<FastDDSPubSubProvider>(kEarlySubDomain);

    PubSubArrow pub(pub_provider);
    PubSubArrow sub(sub_provider);

    const auto schema = SensorSchema();
    const std::vector<std::string> topic{"sensor", "early-sub"};

    std::mutex mu;
    std::condition_variable cv;
    bool got_row = false;
    ArrowRow rx_row;
    PubSubArrow::SubscribeResult sub_result;
    std::string subscribe_error;

    // Subscribe runs in a background thread because it internally polls
    // /__schema for up to 5 s — we don't want it to block CreateTopic.
    std::thread sub_thread([&] {
        try {
            sub_result = sub.Subscribe(topic, [&](ArrowRow row, Attachments) {
                std::lock_guard<std::mutex> lk(mu);
                rx_row = std::move(row);
                got_row = true;
                cv.notify_all();
            });
        } catch (const std::exception& e) {
            subscribe_error = e.what();
        }
    });

    // Give Subscribe time to begin polling /__schema before the
    // publisher writes anything to it.
    std::this_thread::sleep_for(500ms);

    pub.CreateTopic(topic, schema);
    pub.Publish(topic, SensorRow(99, 12.5, "after-sub-armed"));

    sub_thread.join();
    ASSERT_TRUE(subscribe_error.empty()) << "Subscribe threw: " << subscribe_error;
    ASSERT_NE(sub_result.schema, nullptr)
        << "schema must arrive at the early subscriber via polling";
    EXPECT_TRUE(sub_result.schema->Equals(*schema, /*check_metadata=*/false));

    {
        std::unique_lock<std::mutex> lk(mu);
        ASSERT_TRUE(cv.wait_for(lk, 5s, [&] { return got_row; }));
    }
    sub.Unsubscribe(sub_result.subscription_id);

    ASSERT_EQ(rx_row.size(), 3u);
    EXPECT_EQ(std::static_pointer_cast<arrow::Int32Scalar>(rx_row[0])->value, 99);
}

// ─────────────────────────────────────────────────────────────────────
// Test 3: Publisher restart — original publisher exits, a new one takes
// over the same topic, and a subscriber that joins afterwards still
// gets the schema.
// ─────────────────────────────────────────────────────────────────────
TEST(SchemaTransportTest, NewPublisherCanResumeSchemaForLateSubscribers) {
    const auto schema = SensorSchema();
    const std::vector<std::string> topic{"sensor", "restart"};

    // First publisher creates the topic, publishes a row, then is
    // destroyed (its provider, DataWriter, and TRANSIENT_LOCAL history
    // all go away).
    {
        auto pub1_provider = std::make_shared<FastDDSPubSubProvider>(kRestartDomain);
        PubSubArrow pub1(pub1_provider);
        pub1.CreateTopic(topic, schema);
        pub1.Publish(topic, SensorRow(1, 1.0, "from-pub1"));
    }

    // Second publisher takes over on the same DDS domain with the same
    // topic + schema.
    auto pub2_provider = std::make_shared<FastDDSPubSubProvider>(kRestartDomain);
    PubSubArrow pub2(pub2_provider);
    pub2.CreateTopic(topic, schema);
    pub2.Publish(topic, SensorRow(2, 2.0, "from-pub2"));

    // Subscriber joins after the takeover. The schema must arrive via
    // pub2's /__schema writer (pub1's history is gone).
    auto sub_provider = std::make_shared<FastDDSPubSubProvider>(kRestartDomain);
    PubSubArrow sub(sub_provider);

    std::mutex mu;
    std::condition_variable cv;
    bool got_row = false;
    ArrowRow rx_row;

    auto result = sub.Subscribe(topic, [&](ArrowRow row, Attachments) {
        std::lock_guard<std::mutex> lk(mu);
        rx_row = std::move(row);
        got_row = true;
        cv.notify_all();
    });

    ASSERT_NE(result.schema, nullptr)
        << "schema must be served by the surviving publisher (pub2)";
    EXPECT_TRUE(result.schema->Equals(*schema, /*check_metadata=*/false));

    {
        std::unique_lock<std::mutex> lk(mu);
        ASSERT_TRUE(cv.wait_for(lk, 5s, [&] { return got_row; }))
            << "subscriber must receive at least one row from pub2";
    }
    sub.Unsubscribe(result.subscription_id);

    ASSERT_EQ(rx_row.size(), 3u);
    // The sample we receive must come from pub2 — pub1's history is
    // destroyed with its provider, so pub2's row (id=2) is the only
    // sample on a live TRANSIENT_LOCAL writer at the time the
    // subscriber matches.
    EXPECT_EQ(std::static_pointer_cast<arrow::Int32Scalar>(rx_row[0])->value, 2);
}
