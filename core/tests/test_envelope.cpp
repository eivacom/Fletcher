// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include <gtest/gtest.h>

#include <fletcher/core/envelope.hpp>
#include <memory>
#include <vector>

using namespace fletcher;

namespace {

// Deserialization now needs an OWNER for the bytes it parses: the attachments it
// produces alias that buffer instead of copying out of it (§3.2). Wrapping the
// serialized bytes in shared storage is what every real receive path does.
std::shared_ptr<const std::vector<uint8_t>> Owned(std::vector<uint8_t> bytes) {
    return std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
}

std::vector<uint8_t> Bytes(const Blob& blob) {
    return std::vector<uint8_t>(blob.data(), blob.data() + blob.size());
}

}  // namespace

// ---------------------------------------------------------------------------
// SerializeEnvelope / DeserializeEnvelope roundtrips
// ---------------------------------------------------------------------------

TEST(EnvelopeTest, RoundtripWithNoAttachments) {
    Envelope env;
    env.row = {0x01, 0x02, 0x03, 0x04};

    auto serialized = SerializeEnvelope(env);
    auto owner = Owned(std::move(serialized));
    auto restored = DeserializeEnvelope(owner);

    EXPECT_EQ(restored.row, env.row);
    EXPECT_TRUE(restored.attachments.empty());
}

TEST(EnvelopeTest, RoundtripWithOneAttachment) {
    const std::vector<uint8_t> payload{0xDE, 0xAD, 0xBE, 0xEF};
    Blob blob{payload};

    Envelope env;
    env.row = {0xAA, 0xBB};
    env.attachments["image"] = blob;

    auto serialized = SerializeEnvelope(env);
    auto owner = Owned(std::move(serialized));
    auto restored = DeserializeEnvelope(owner);

    EXPECT_EQ(restored.row, env.row);
    ASSERT_EQ(restored.attachments.size(), 1u);
    ASSERT_EQ(restored.attachments.count("image"), 1u);
    EXPECT_EQ(Bytes(restored.attachments.at("image")), payload);

    // The re-anchor: the restored blob does not merely COMPARE equal, it points
    // INTO the buffer that was parsed. That is what §3.2's owner-plus-span buys
    // and what the old copy-out-of-the-buffer parser could not do.
    const uint8_t* base = owner->data();
    const uint8_t* at = restored.attachments.at("image").data();
    EXPECT_GE(at, base);
    EXPECT_LT(at, base + owner->size());
}

TEST(EnvelopeTest, RoundtripWithMultipleAttachments) {
    const std::vector<uint8_t> a{0x01, 0x02};
    const std::vector<uint8_t> b{0x03, 0x04, 0x05};
    Blob blob_a{a};
    Blob blob_b{b};
    Blob blob_c{};  // empty is null data and zero size (§3.2 clause 5)

    Envelope env;
    env.row = {0xFF};
    env.attachments["a"] = blob_a;
    env.attachments["b"] = blob_b;
    env.attachments["empty"] = blob_c;

    auto serialized = SerializeEnvelope(env);
    auto owner = Owned(std::move(serialized));
    auto restored = DeserializeEnvelope(owner);

    EXPECT_EQ(restored.row, env.row);
    ASSERT_EQ(restored.attachments.size(), 3u);
    EXPECT_EQ(Bytes(restored.attachments.at("a")), a);
    EXPECT_EQ(Bytes(restored.attachments.at("b")), b);
    EXPECT_TRUE(restored.attachments.at("empty").empty());
    EXPECT_EQ(restored.attachments.at("empty").data(), nullptr);
}

TEST(EnvelopeTest, RoundtripWithLargeBlob) {
    std::vector<uint8_t> big(1'100'000, 0x42);
    Blob blob{std::move(big)};

    Envelope env;
    env.row = {0x00};
    env.attachments["big"] = blob;

    auto serialized = SerializeEnvelope(env);
    auto owner = Owned(std::move(serialized));
    auto restored = DeserializeEnvelope(owner);

    EXPECT_EQ(restored.row, env.row);
    ASSERT_EQ(restored.attachments.count("big"), 1u);
    EXPECT_EQ(restored.attachments.at("big").size(), 1'100'000u);
    EXPECT_EQ(restored.attachments.at("big").data()[0], 0x42);
}

TEST(EnvelopeTest, EmptyRowWithAttachments) {
    const std::vector<uint8_t> payload{0x01};
    Blob blob{payload};

    Envelope env;
    // row is empty
    env.attachments["data"] = blob;

    auto serialized = SerializeEnvelope(env);
    auto owner = Owned(std::move(serialized));
    auto restored = DeserializeEnvelope(owner);

    EXPECT_TRUE(restored.row.empty());
    ASSERT_EQ(restored.attachments.size(), 1u);
    EXPECT_EQ(Bytes(restored.attachments.at("data")), payload);
}

TEST(EnvelopeTest, CompletelyEmptyEnvelope) {
    Envelope env;

    auto serialized = SerializeEnvelope(env);
    auto owner = Owned(std::move(serialized));
    auto restored = DeserializeEnvelope(owner);

    EXPECT_TRUE(restored.row.empty());
    EXPECT_TRUE(restored.attachments.empty());
}

// ---------------------------------------------------------------------------
// Error handling
// ---------------------------------------------------------------------------

TEST(EnvelopeTest, ThrowsOnTruncatedBuffer) {
    EXPECT_THROW(static_cast<void>(DeserializeEnvelope(Owned({0x01, 0x02}))),
                 std::invalid_argument);
}

TEST(EnvelopeTest, ThrowsOnTruncatedRowData) {
    // Claim row_len=100 but only provide 4 bytes.
    std::vector<uint8_t> buf = {0x64, 0x00, 0x00, 0x00,   // row_len = 100
                                0x01, 0x02, 0x03, 0x04};  // only 4 bytes
    EXPECT_THROW(static_cast<void>(DeserializeEnvelope(Owned(buf))), std::invalid_argument);
}

TEST(EnvelopeTest, ThrowsOnTruncatedAttachmentKey) {
    // Valid row (len=1, data=0xFF), attach_count=1, key_len=100 but no key data.
    std::vector<uint8_t> buf = {0x01, 0x00, 0x00, 0x00,   // row_len = 1
                                0xFF,                     // row data
                                0x01, 0x00, 0x00, 0x00,   // attach_count = 1
                                0x64, 0x00, 0x00, 0x00};  // key_len = 100 (truncated)
    EXPECT_THROW(static_cast<void>(DeserializeEnvelope(Owned(buf))), std::invalid_argument);
}
