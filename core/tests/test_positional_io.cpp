// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include <gtest/gtest.h>

#include <algorithm>
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

TEST(PositionalIoTest, AllNullListAcceptedWhenBitfieldFits) {
    // Regression (PR #98 review): a list of all-null elements has no payloads,
    // so its count can legitimately exceed the remaining byte count. The bound
    // is on the null-bitfield size (ceil(count/8)), not the raw count.
    // 1-field row: [row bitfield 0x00] [list COUNT=20 LE] [elem bitfield: 3B].
    std::vector<uint8_t> buf = {0x00, 0x14, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x0F};
    PositionalReader r(buf.data(), buf.size(), 1);
    auto hdr = r.ReadListHeader();  // previously threw: count 20 > remaining 3
    EXPECT_EQ(hdr.count, 20u);
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

// --- AppendTrailingUint64Field: stamp a trailing system column (e.g. _ingest_offset) ---

TEST(AppendTrailingUint64FieldTest, PureAppendWhenBitfieldDoesNotGrow) {
    auto row = EncodeTwoInts(7, -3);  // 2 fields -> 1-byte bitfield; 2 % 8 != 0
    const uint64_t offset = 0x0102030405060708ULL;

    auto out = AppendTrailingUint64Field(row, /*base_num_fields=*/2, offset);

    // Bitfield stays 1 byte, so the result is the original row + 8 trailing bytes.
    ASSERT_EQ(out.size(), row.size() + sizeof(uint64_t));
    EXPECT_TRUE(std::equal(row.begin(), row.end(), out.begin()));

    // Round-trips as a 3-field row: int32, int32, uint64.
    PositionalReader r(out.data(), out.size(), 3);
    EXPECT_FALSE(r.IsNull(0));
    EXPECT_EQ(r.ReadInt32(), 7);
    EXPECT_EQ(r.ReadInt32(), -3);
    EXPECT_FALSE(r.IsNull(2));
    EXPECT_EQ(r.ReadUint64(), offset);
    EXPECT_NO_THROW(r.VerifyFullyConsumed());
}

TEST(AppendTrailingUint64FieldTest, GrowsBitfieldWhenBaseIsMultipleOfEight) {
    std::vector<uint8_t> row;
    {
        VectorWriteBuffer wb(row);
        PositionalWriter w(wb, 8);  // 8 fields -> 1-byte bitfield
        for (int32_t i = 0; i < 8; ++i) w.WriteInt32(i * 10);
    }
    ASSERT_EQ(row.size(), 1u + 8u * sizeof(int32_t));

    const uint64_t offset = 42;
    auto out = AppendTrailingUint64Field(row, /*base_num_fields=*/8, offset);

    // 8 -> 9 fields grows the bitfield by one byte; that inserted byte is zero
    // (the new field is non-null) and existing bytes are otherwise preserved.
    ASSERT_EQ(out.size(), row.size() + 1u + sizeof(uint64_t));
    EXPECT_EQ(out[1], 0u);  // inserted bitfield byte, just after the original 1-byte bitfield

    PositionalReader r(out.data(), out.size(), 9);
    for (int i = 0; i < 8; ++i) {
        EXPECT_FALSE(r.IsNull(i));
        EXPECT_EQ(r.ReadInt32(), i * 10);
    }
    EXPECT_FALSE(r.IsNull(8));
    EXPECT_EQ(r.ReadUint64(), offset);
    EXPECT_NO_THROW(r.VerifyFullyConsumed());
}

TEST(AppendTrailingUint64FieldTest, PreservesExistingNullBits) {
    std::vector<uint8_t> row;
    {
        VectorWriteBuffer wb(row);
        PositionalWriter w(wb, 3);
        w.WriteInt32(11);  // field 0 (non-null)
        w.SetNull(1);      // field 1 null (no payload)
        w.WriteInt32(33);  // field 2 (non-null)
    }

    const uint64_t offset = 0xFEDCBA9876543210ULL;  // high bit set, exercises all 8 bytes
    auto out = AppendTrailingUint64Field(row, /*base_num_fields=*/3, offset);

    PositionalReader r(out.data(), out.size(), 4);
    EXPECT_FALSE(r.IsNull(0));
    EXPECT_EQ(r.ReadInt32(), 11);
    EXPECT_TRUE(r.IsNull(1));  // null bit survived the splice
    EXPECT_FALSE(r.IsNull(2));
    EXPECT_EQ(r.ReadInt32(), 33);
    EXPECT_FALSE(r.IsNull(3));
    EXPECT_EQ(r.ReadUint64(), offset);
    EXPECT_NO_THROW(r.VerifyFullyConsumed());
}

// A stray spare bit in the last bitfield byte (beyond the valid field range) must
// not leak into the appended field's null bit — the splice clears it explicitly.
TEST(AppendTrailingUint64FieldTest, ClearsStraySpareBitOnAppendedField) {
    auto row = EncodeTwoInts(7, -3);          // 2 fields -> 1-byte bitfield; bits 0,1 used
    row[0] |= static_cast<uint8_t>(1u << 2);  // set the spare bit at position 2 (field index 2)

    const uint64_t offset = 0x1122334455667788ULL;
    auto out = AppendTrailingUint64Field(row, /*base_num_fields=*/2, offset);

    PositionalReader r(out.data(), out.size(), 3);
    EXPECT_FALSE(r.IsNull(2));  // appended field decodes as non-null despite the stray bit
    EXPECT_EQ(r.ReadInt32(), 7);
    EXPECT_EQ(r.ReadInt32(), -3);
    EXPECT_EQ(r.ReadUint64(), offset);
    EXPECT_NO_THROW(r.VerifyFullyConsumed());
}

TEST(AppendTrailingUint64FieldTest, RejectsRowShorterThanBitfield) {
    std::vector<uint8_t> empty;  // a 9-field base needs a 2-byte bitfield
    EXPECT_THROW(AppendTrailingUint64Field(empty, 9, uint64_t{0}), std::invalid_argument);
}

TEST(AppendTrailingUint64FieldTest, RejectsNegativeBaseFieldCount) {
    std::vector<uint8_t> row = {0x00};
    EXPECT_THROW(AppendTrailingUint64Field(row, -1, uint64_t{0}), std::invalid_argument);
}

// --- ReadTrailingUint64Field: the no-schema fast path (read the last 8 bytes) ---

TEST(ReadTrailingUint64FieldTest, RoundTripsWithoutBitfieldGrowth) {
    auto row = EncodeTwoInts(7, -3);  // 2 fields, no bitfield growth on append
    const uint64_t offset = 0x0102030405060708ULL;
    auto out = AppendTrailingUint64Field(row, 2, offset);
    EXPECT_EQ(ReadTrailingUint64Field(out), offset);
}

TEST(ReadTrailingUint64FieldTest, RoundTripsAcrossBitfieldGrowth) {
    std::vector<uint8_t> row;
    {
        VectorWriteBuffer wb(row);
        PositionalWriter w(wb, 8);  // append grows the bitfield (8 -> 9 fields)
        for (int32_t i = 0; i < 8; ++i) w.WriteInt32(i * 10);
    }
    const uint64_t offset = 0xFFFFFFFFFFFFFFFFULL;  // max uint64 round-trips intact
    auto out = AppendTrailingUint64Field(row, 8, offset);
    EXPECT_EQ(ReadTrailingUint64Field(out), offset);
}

TEST(ReadTrailingUint64FieldTest, RejectsRowShorterThanUint64) {
    std::vector<uint8_t> tiny = {0x00, 0x01, 0x02};  // < 8 bytes
    EXPECT_THROW(ReadTrailingUint64Field(tiny), std::invalid_argument);
}
