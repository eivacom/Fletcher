// schema-transport integration test.
//
// Drives the FastDDSPubSubProvider's `/__schema` companion-topic
// mechanism through three timing scenarios that the per-provider unit
// tests don't cover. Each scenario verifies that a subscriber resolves
// the schema correctly — the AC for US #17021 is specifically about
// schema delivery, not data delivery. The pubsub-arrow + fastdds
// integration test (#36) covers the data path on the happy timing
// order, and TRANSIENT_LOCAL data retention to late joiners is a known
// flaky property of FastDDS that is out of scope for this suite.

#include <pubsub_arrow/pubsub_arrow.hpp>
#include <fast_dds_pubsub_provider.hpp>

#include <arrow/api.h>
#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace fletcher;
using namespace std::chrono_literals;

namespace {

// High domain ids keep each test isolated from other DDS traffic and
// from each other (each test uses its own domain so stray endpoints
// from one test cannot match a reader in the next).
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
// Test 1: Late join — subscriber connects after publisher published
// data. The schema must still arrive via /__schema TRANSIENT_LOCAL.
// ─────────────────────────────────────────────────────────────────────
TEST(SchemaTransportTest, SchemaArrivesAtLateJoiningSubscriber) {
    auto pub_provider = std::make_shared<FastDDSPubSubProvider>(kLateJoinDomain);

    PubSubArrow pub(pub_provider);
    const auto schema = SensorSchema();
    const std::vector<std::string> topic{"sensor", "late-join"};

    // Realistic late-join setup: publisher creates the topic, writes a
    // sample, and only afterwards does the subscriber come online.
    pub.CreateTopic(topic, schema);
    pub.Publish(topic, SensorRow(42, 23.5, "before-sub"));

    auto sub_provider = std::make_shared<FastDDSPubSubProvider>(kLateJoinDomain);
    PubSubArrow sub(sub_provider);

    auto result = sub.Subscribe(topic, [](ArrowRow, Attachments) {});

    ASSERT_NE(result.schema, nullptr)
        << "schema must be delivered to late joiner via /__schema TRANSIENT_LOCAL";
    EXPECT_TRUE(result.schema->Equals(*schema, /*check_metadata=*/false));

    sub.Unsubscribe(result.subscription_id);
}

// ─────────────────────────────────────────────────────────────────────
// Test 2: Subscribe before publish — Subscribe polls /__schema and
// waits for CreateTopic to come up.
// ─────────────────────────────────────────────────────────────────────
TEST(SchemaTransportTest, SubscribeWaitsUntilPublisherCreatesTopic) {
    auto pub_provider = std::make_shared<FastDDSPubSubProvider>(kEarlySubDomain);
    auto sub_provider = std::make_shared<FastDDSPubSubProvider>(kEarlySubDomain);

    PubSubArrow pub(pub_provider);
    PubSubArrow sub(sub_provider);

    const auto schema = SensorSchema();
    const std::vector<std::string> topic{"sensor", "early-sub"};

    PubSubArrow::SubscribeResult sub_result;
    std::string subscribe_error;

    // Subscribe runs in a background thread because it internally polls
    // /__schema for up to 5 s — we don't want it to block CreateTopic.
    std::thread sub_thread([&] {
        try {
            sub_result = sub.Subscribe(topic, [](ArrowRow, Attachments) {});
        } catch (const std::exception& e) {
            subscribe_error = e.what();
        }
    });

    // Give Subscribe time to begin polling /__schema before the
    // publisher writes anything to it.
    std::this_thread::sleep_for(500ms);

    pub.CreateTopic(topic, schema);

    sub_thread.join();
    ASSERT_TRUE(subscribe_error.empty()) << "Subscribe threw: " << subscribe_error;
    ASSERT_NE(sub_result.schema, nullptr)
        << "schema must arrive at the early subscriber via polling";
    EXPECT_TRUE(sub_result.schema->Equals(*schema, /*check_metadata=*/false));

    sub.Unsubscribe(sub_result.subscription_id);
}

// ─────────────────────────────────────────────────────────────────────
// Test 3: Publisher restart — the original publisher exits, a new one
// takes over the same topic, and a subscriber that joins after the
// takeover must still resolve the schema.
// ─────────────────────────────────────────────────────────────────────
TEST(SchemaTransportTest, SchemaSurvivesPublisherRestart) {
    const auto schema = SensorSchema();
    const std::vector<std::string> topic{"sensor", "restart"};

    // First publisher creates the topic, then is destroyed (its
    // provider, DataWriter, and TRANSIENT_LOCAL history all go away
    // when this scope exits).
    {
        auto pub1_provider = std::make_shared<FastDDSPubSubProvider>(kRestartDomain);
        PubSubArrow pub1(pub1_provider);
        pub1.CreateTopic(topic, schema);
    }

    // Brief settle so DDS discovery can publish pub1's leave message
    // and other participants can drop pub1's endpoints before pub2
    // arrives. Without this, sub may match against pub1's stale
    // discovery state and never receive from pub2.
    std::this_thread::sleep_for(500ms);

    // Second publisher takes over on the same DDS domain with the same
    // topic + schema.
    auto pub2_provider = std::make_shared<FastDDSPubSubProvider>(kRestartDomain);
    PubSubArrow pub2(pub2_provider);
    pub2.CreateTopic(topic, schema);

    // Subscriber joins after the takeover. The schema must arrive via
    // pub2's /__schema writer.
    auto sub_provider = std::make_shared<FastDDSPubSubProvider>(kRestartDomain);
    PubSubArrow sub(sub_provider);

    auto result = sub.Subscribe(topic, [](ArrowRow, Attachments) {});

    ASSERT_NE(result.schema, nullptr)
        << "schema must be served by the surviving publisher (pub2)";
    EXPECT_TRUE(result.schema->Equals(*schema, /*check_metadata=*/false));

    sub.Unsubscribe(result.subscription_id);
}
