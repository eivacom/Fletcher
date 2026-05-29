// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include <gtest/gtest.h>

#include <cstdint>
#include <fletcher/core/positional_io.hpp>
#include <fletcher/core/write_buffer.hpp>
#include <stdexcept>
#include <vector>

using namespace fletcher;

namespace {

// Encode a 2-field row of two int32s and return the raw bytes.
std::vector<uint8_t> EncodeTwoInts(int32_t a, int32_t b) {
    std::vector<uint8_t> buf;
    VectorWriteBuffer wb(buf);
    PositionalWriter w(wb, 2);
    w.WriteInt32(a);
    w.WriteInt32(b);
    return buf;
}

}  // namespace

TEST(PositionalIoTest, RoundtripTwoInts) {
    auto buf = EncodeTwoInts(7, -3);
    PositionalReader r(buf.data(), buf.size(), 2);
    EXPECT_FALSE(r.IsNull(0));
    EXPECT_EQ(r.ReadInt32(), 7);
    EXPECT_EQ(r.ReadInt32(), -3);
    EXPECT_NO_THROW(r.VerifyFullyConsumed());
}

// --- Phase 1 hardening: malformed input must throw, not over-read ---

TEST(PositionalIoTest, ReadUnderrunThrows) {
    auto buf = EncodeTwoInts(7, -3);
    buf.pop_back();  // truncate the last int32 by one byte
    PositionalReader r(buf.data(), buf.size(), 2);
    EXPECT_EQ(r.ReadInt32(), 7);
    EXPECT_THROW(r.ReadInt32(), std::invalid_argument);
}

TEST(PositionalIoTest, VerifyFullyConsumedRejectsTrailingBytes) {
    auto buf = EncodeTwoInts(7, -3);
    buf.push_back(0xAB);  // padding / corruption after a valid row
    PositionalReader r(buf.data(), buf.size(), 2);
    r.ReadInt32();
    r.ReadInt32();
    EXPECT_THROW(r.VerifyFullyConsumed(), std::invalid_argument);
}

TEST(PositionalIoTest, OversizedListCountRejected) {
    // 1-field row: [bitfield = 0x00 (present)] [list COUNT = 0xFFFFFFFF].
    std::vector<uint8_t> buf = {0x00, 0xFF, 0xFF, 0xFF, 0xFF};
    PositionalReader r(buf.data(), buf.size(), 1);
    EXPECT_THROW(r.ReadListHeader(), std::invalid_argument);
}

TEST(PositionalIoTest, OversizedMapCountRejected) {
    std::vector<uint8_t> buf = {0x00, 0xFF, 0xFF, 0xFF, 0xFF};
    PositionalReader r(buf.data(), buf.size(), 1);
    EXPECT_THROW(r.ReadMapCount(), std::invalid_argument);
}

TEST(PositionalIoTest, ConstructorRejectsTooShortBitfield) {
    // A 9-field row needs a 2-byte null bitfield; give it only 1 byte.
    std::vector<uint8_t> buf = {0x00};
    EXPECT_THROW(PositionalReader(buf.data(), buf.size(), 9), std::invalid_argument);
}
