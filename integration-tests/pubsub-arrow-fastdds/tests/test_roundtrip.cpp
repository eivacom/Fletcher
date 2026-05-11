// pubsub-arrow + fastdds-pubsub-provider integration test.
//
// Drives the PubSubArrow adapter on top of a real FastDDSPubSubProvider in
// the same process (publisher and subscriber on separate DomainParticipants
// but the same DDS domain) and verifies that ArrowRow instances + their
// schema round-trip across the adapter + DDS path.
//
// Each component has unit tests against a mock provider; only this test
// covers the seam between them.

#include <pubsub_arrow/pubsub_arrow.hpp>
#include <fast_dds_pubsub_provider.hpp>

#include <arrow/api.h>
#include <gtest/gtest.h>

#include <atomic>
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

// Use a high domain id to keep this test isolated from any other DDS
// traffic on the host running CI.
constexpr uint32_t kTestDomain = 137;

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

TEST(PubSubArrowFastDdsTest, SchemaAndRowDeliveredAcrossDdsBoundary) {
    auto pub_provider = std::make_shared<FastDDSPubSubProvider>(kTestDomain);
    auto sub_provider = std::make_shared<FastDDSPubSubProvider>(kTestDomain);

    PubSubArrow pub(pub_provider);
    PubSubArrow sub(sub_provider);

    const auto schema = SensorSchema();
    const std::vector<std::string> topic{"sensor", "feed"};

    pub.CreateTopic(topic, schema);

    std::mutex mu;
    std::condition_variable cv;
    bool received = false;
    ArrowRow rx_row;

    auto result = sub.Subscribe(topic, [&](ArrowRow row, Attachments) {
        std::lock_guard<std::mutex> lk(mu);
        rx_row = std::move(row);
        received = true;
        cv.notify_all();
    });

    // Subscribe internally polls until the schema arrives via the companion
    // /__schema topic, so once it returns the subscriber knows the schema.
    ASSERT_NE(result.schema, nullptr);
    EXPECT_TRUE(result.schema->Equals(*schema, /*check_metadata=*/false));

    // Give DDS time to complete writer/reader matching on the data topic
    // before publishing — otherwise the row may be dropped despite RELIABLE.
    std::this_thread::sleep_for(500ms);

    pub.Publish(topic, SensorRow(42, 23.5, "alpha"));

    {
        std::unique_lock<std::mutex> lk(mu);
        ASSERT_TRUE(cv.wait_for(lk, 5s, [&] { return received; }))
            << "subscriber callback never fired within 5 s";
    }

    ASSERT_EQ(rx_row.size(), 3u);
    EXPECT_EQ(std::static_pointer_cast<arrow::Int32Scalar>(rx_row[0])->value, 42);
    EXPECT_EQ(std::static_pointer_cast<arrow::DoubleScalar>(rx_row[1])->value, 23.5);
    EXPECT_EQ(
        std::static_pointer_cast<arrow::StringScalar>(rx_row[2])->ToString(),
        "alpha");

    sub.Unsubscribe(result.subscription_id);
}

TEST(PubSubArrowFastDdsTest, MultipleRowsDeliveredInOrder) {
    auto pub_provider = std::make_shared<FastDDSPubSubProvider>(kTestDomain);
    auto sub_provider = std::make_shared<FastDDSPubSubProvider>(kTestDomain);

    PubSubArrow pub(pub_provider);
    PubSubArrow sub(sub_provider);

    const auto schema = SensorSchema();
    const std::vector<std::string> topic{"sensor", "stream"};

    pub.CreateTopic(topic, schema);

    std::mutex mu;
    std::condition_variable cv;
    std::vector<int32_t> received_ids;

    constexpr int kRowCount = 5;

    auto result = sub.Subscribe(topic, [&](ArrowRow row, Attachments) {
        std::lock_guard<std::mutex> lk(mu);
        received_ids.push_back(
            std::static_pointer_cast<arrow::Int32Scalar>(row[0])->value);
        cv.notify_all();
    });

    ASSERT_NE(result.schema, nullptr);

    std::this_thread::sleep_for(500ms);

    for (int i = 0; i < kRowCount; ++i) {
        pub.Publish(topic, SensorRow(i, static_cast<double>(i), "row-" + std::to_string(i)));
    }

    {
        std::unique_lock<std::mutex> lk(mu);
        ASSERT_TRUE(cv.wait_for(lk, 10s, [&] { return received_ids.size() == kRowCount; }))
            << "expected " << kRowCount << " rows, got " << received_ids.size();
    }

    for (int i = 0; i < kRowCount; ++i) {
        EXPECT_EQ(received_ids[i], i) << "row " << i << " out of order";
    }

    sub.Unsubscribe(result.subscription_id);
}
