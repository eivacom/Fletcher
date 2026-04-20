#include <gtest/gtest.h>

#include <core/envelope.hpp>

using namespace fletcher;

// ---------------------------------------------------------------------------
// SerializeEnvelope / DeserializeEnvelope roundtrips
// ---------------------------------------------------------------------------

TEST(EnvelopeTest, RoundtripWithNoAttachments) {
    Envelope env;
    env.row = {0x01, 0x02, 0x03, 0x04};

    auto serialized = SerializeEnvelope(env);
    auto restored   = DeserializeEnvelope(serialized);

    EXPECT_EQ(restored.row, env.row);
    EXPECT_TRUE(restored.attachments.empty());
}

TEST(EnvelopeTest, RoundtripWithOneAttachment) {
    auto blob = std::make_shared<const std::vector<uint8_t>>(
        std::vector<uint8_t>{0xDE, 0xAD, 0xBE, 0xEF});

    Envelope env;
    env.row = {0xAA, 0xBB};
    env.attachments["image"] = blob;

    auto serialized = SerializeEnvelope(env);
    auto restored   = DeserializeEnvelope(serialized);

    EXPECT_EQ(restored.row, env.row);
    ASSERT_EQ(restored.attachments.size(), 1u);
    ASSERT_EQ(restored.attachments.count("image"), 1u);
    EXPECT_EQ(*restored.attachments.at("image"), *blob);
}

TEST(EnvelopeTest, RoundtripWithMultipleAttachments) {
    auto blob_a = std::make_shared<const std::vector<uint8_t>>(
        std::vector<uint8_t>{0x01, 0x02});
    auto blob_b = std::make_shared<const std::vector<uint8_t>>(
        std::vector<uint8_t>{0x03, 0x04, 0x05});
    auto blob_c = std::make_shared<const std::vector<uint8_t>>(
        std::vector<uint8_t>{});

    Envelope env;
    env.row = {0xFF};
    env.attachments["a"] = blob_a;
    env.attachments["b"] = blob_b;
    env.attachments["empty"] = blob_c;

    auto serialized = SerializeEnvelope(env);
    auto restored   = DeserializeEnvelope(serialized);

    EXPECT_EQ(restored.row, env.row);
    ASSERT_EQ(restored.attachments.size(), 3u);
    EXPECT_EQ(*restored.attachments.at("a"), *blob_a);
    EXPECT_EQ(*restored.attachments.at("b"), *blob_b);
    EXPECT_TRUE(restored.attachments.at("empty")->empty());
}

TEST(EnvelopeTest, RoundtripWithLargeBlob) {
    std::vector<uint8_t> big(1'100'000, 0x42);
    auto blob = std::make_shared<const std::vector<uint8_t>>(std::move(big));

    Envelope env;
    env.row = {0x00};
    env.attachments["big"] = blob;

    auto serialized = SerializeEnvelope(env);
    auto restored   = DeserializeEnvelope(serialized);

    EXPECT_EQ(restored.row, env.row);
    ASSERT_EQ(restored.attachments.count("big"), 1u);
    EXPECT_EQ(restored.attachments.at("big")->size(), 1'100'000u);
    EXPECT_EQ(restored.attachments.at("big")->front(), 0x42);
}

TEST(EnvelopeTest, EmptyRowWithAttachments) {
    auto blob = std::make_shared<const std::vector<uint8_t>>(
        std::vector<uint8_t>{0x01});

    Envelope env;
    // row is empty
    env.attachments["data"] = blob;

    auto serialized = SerializeEnvelope(env);
    auto restored   = DeserializeEnvelope(serialized);

    EXPECT_TRUE(restored.row.empty());
    ASSERT_EQ(restored.attachments.size(), 1u);
    EXPECT_EQ(*restored.attachments.at("data"), *blob);
}

TEST(EnvelopeTest, CompletelyEmptyEnvelope) {
    Envelope env;

    auto serialized = SerializeEnvelope(env);
    auto restored   = DeserializeEnvelope(serialized);

    EXPECT_TRUE(restored.row.empty());
    EXPECT_TRUE(restored.attachments.empty());
}

// ---------------------------------------------------------------------------
// Error handling
// ---------------------------------------------------------------------------

TEST(EnvelopeTest, ThrowsOnTruncatedBuffer) {
    std::vector<uint8_t> tiny = {0x01, 0x02};
    EXPECT_THROW(DeserializeEnvelope(tiny), std::invalid_argument);
}

TEST(EnvelopeTest, ThrowsOnTruncatedRowData) {
    // Claim row_len=100 but only provide 4 bytes.
    std::vector<uint8_t> buf = {0x64, 0x00, 0x00, 0x00,  // row_len = 100
                                 0x01, 0x02, 0x03, 0x04}; // only 4 bytes
    EXPECT_THROW(DeserializeEnvelope(buf), std::invalid_argument);
}

TEST(EnvelopeTest, ThrowsOnTruncatedAttachmentKey) {
    // Valid row (len=1, data=0xFF), attach_count=1, key_len=100 but no key data.
    std::vector<uint8_t> buf = {
        0x01, 0x00, 0x00, 0x00,  // row_len = 1
        0xFF,                     // row data
        0x01, 0x00, 0x00, 0x00,  // attach_count = 1
        0x64, 0x00, 0x00, 0x00}; // key_len = 100 (truncated)
    EXPECT_THROW(DeserializeEnvelope(buf), std::invalid_argument);
}
