// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <vector>

#include "publish_frame.hpp"

using fletcher::gateway::ParsePublishFrame;

// ---------------------------------------------------------------------------
// Happy path
// ---------------------------------------------------------------------------

TEST(ParsePublishFrameTest, ExtractsTopicAndEnvelope) {
    // [TOPIC_LEN:2 LE = 5]["hello"][envelope: 0xDE 0xAD 0xBE 0xEF]
    const std::vector<uint8_t> frame = {
        0x05, 0x00, 'h', 'e', 'l', 'l', 'o', 0xDE, 0xAD, 0xBE, 0xEF,
    };
    auto parts = ParsePublishFrame(frame.data(), frame.size());

    EXPECT_EQ(parts.topic, "hello");
    ASSERT_EQ(parts.envelope_size, 4u);
    EXPECT_EQ(parts.envelope_data[0], 0xDE);
    EXPECT_EQ(parts.envelope_data[3], 0xEF);
}

TEST(ParsePublishFrameTest, EmptyEnvelopeIsAllowed) {
    // Caller may still want a publish with an empty payload — that's
    // the envelope's responsibility to reject, not the frame parser's.
    const std::vector<uint8_t> frame = {
        0x03, 0x00, 'a', '/', 'b',
    };
    auto parts = ParsePublishFrame(frame.data(), frame.size());

    EXPECT_EQ(parts.topic, "a/b");
    EXPECT_EQ(parts.envelope_size, 0u);
}

TEST(ParsePublishFrameTest, EmptyTopicIsAllowed) {
    // The frame parser doesn't validate topic semantics — split-into-
    // segments downstream is what decides whether the empty topic is
    // meaningful.
    const std::vector<uint8_t> frame = {
        0x00,
        0x00,
        0x42,
        0x43,
    };
    auto parts = ParsePublishFrame(frame.data(), frame.size());

    EXPECT_TRUE(parts.topic.empty());
    ASSERT_EQ(parts.envelope_size, 2u);
    EXPECT_EQ(parts.envelope_data[0], 0x42);
}

TEST(ParsePublishFrameTest, ReadsTopicLengthAsLittleEndian) {
    // TOPIC_LEN = 0x0001 in LE = 1 byte. Catches accidental BE swap.
    const std::vector<uint8_t> frame = {
        0x01,
        0x00,
        'x',
        0xAA,
    };
    auto parts = ParsePublishFrame(frame.data(), frame.size());

    EXPECT_EQ(parts.topic, "x");
    ASSERT_EQ(parts.envelope_size, 1u);
    EXPECT_EQ(parts.envelope_data[0], 0xAA);
}

// ---------------------------------------------------------------------------
// Bounds-check edge cases — these are the reason this parser is its
// own unit-testable function. The integration test only sends valid
// frames.
// ---------------------------------------------------------------------------

TEST(ParsePublishFrameTest, ThrowsOnEmptyFrame) {
    const std::vector<uint8_t> frame;
    EXPECT_THROW(ParsePublishFrame(frame.data(), frame.size()), std::invalid_argument);
}

TEST(ParsePublishFrameTest, ThrowsWhenTopicLenFieldIsTruncated) {
    // Only one byte — not enough to read the 2-byte TOPIC_LEN.
    const std::vector<uint8_t> frame = {0x05};
    EXPECT_THROW(ParsePublishFrame(frame.data(), frame.size()), std::invalid_argument);
}

TEST(ParsePublishFrameTest, ThrowsWhenTopicRunsOffEndOfFrame) {
    // Claim TOPIC_LEN = 10 but only provide 3 bytes after the length.
    const std::vector<uint8_t> frame = {
        0x0A, 0x00, 'a', 'b', 'c',
    };
    EXPECT_THROW(ParsePublishFrame(frame.data(), frame.size()), std::invalid_argument);
}

TEST(ParsePublishFrameTest, ThrowsWhenTopicLenIsMaxAndFrameTooSmall) {
    // TOPIC_LEN = 0xFFFF (max uint16). A malicious client could send
    // this with a small frame to try to provoke an out-of-bounds read.
    const std::vector<uint8_t> frame = {
        0xFF,
        0xFF,
        'a',
    };
    EXPECT_THROW(ParsePublishFrame(frame.data(), frame.size()), std::invalid_argument);
}

TEST(ParsePublishFrameTest, TopicLenExactlyFillingFrameIsAccepted) {
    // TOPIC_LEN = 3, frame size = 5 → topic fills frame, envelope empty.
    // Boundary between "valid" and "truncated".
    const std::vector<uint8_t> frame = {
        0x03, 0x00, 'a', 'b', 'c',
    };
    auto parts = ParsePublishFrame(frame.data(), frame.size());

    EXPECT_EQ(parts.topic, "abc");
    EXPECT_EQ(parts.envelope_size, 0u);
}
