// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include <gtest/gtest.h>

#include <fletcher/core/envelope.hpp>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace fletcher;

namespace {

// Deserialization now needs an OWNER for the bytes it parses: the attachments it
// produces alias that buffer instead of copying out of it (§3.2). Wrapping the
// serialized bytes in shared storage is what every real receive path does.
std::shared_ptr<const std::vector<uint8_t>> Owned(std::vector<uint8_t> bytes) {
    return std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
}

// The sealed container answers "absent" with a null pointer rather than a
// throw (§3.2). A test that dereferenced that null would crash instead of
// failing, so absence is turned back into a loud failure here.
const Blob& Must(const Attachments& attachments, std::string_view key) {
    const Blob* found = attachments.Find(key);
    if (found == nullptr)
        throw std::runtime_error(std::string("no attachment named ") + std::string(key));
    return *found;
}

std::vector<uint8_t> Bytes(const Blob& blob) {
    return std::vector<uint8_t>(blob.data(), blob.data() + blob.size());
}

// Little-endian, the envelope's only integer encoding. Used by the hand-built
// bodies below, which exist precisely to put bytes on the wire that no local
// encoder would produce.
void AppendU32(std::vector<uint8_t>& out, uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<uint8_t>((value >> shift) & 0xFFu));
    }
}

// Eight digits, zero padded, so decimal order and unsigned-byte order agree and
// every key is the same length: the sort is then over the digits alone.
std::string FixedWidthKey(uint32_t value) {
    std::string digits = std::to_string(value);
    return std::string(8 - digits.size(), '0') + digits;
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
    env.attachments.Set("image", blob);

    auto serialized = SerializeEnvelope(env);
    auto owner = Owned(std::move(serialized));
    auto restored = DeserializeEnvelope(owner);

    EXPECT_EQ(restored.row, env.row);
    ASSERT_EQ(restored.attachments.size(), 1u);
    ASSERT_NE(restored.attachments.Find("image"), nullptr);
    EXPECT_EQ(Bytes(Must(restored.attachments, "image")), payload);

    // The re-anchor: the restored blob does not merely COMPARE equal, it points
    // INTO the buffer that was parsed. That is what §3.2's owner-plus-span buys
    // and what the old copy-out-of-the-buffer parser could not do.
    const uint8_t* base = owner->data();
    const uint8_t* at = Must(restored.attachments, "image").data();
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
    env.attachments.Set("a", blob_a);
    env.attachments.Set("b", blob_b);
    env.attachments.Set("empty", blob_c);

    auto serialized = SerializeEnvelope(env);
    auto owner = Owned(std::move(serialized));
    auto restored = DeserializeEnvelope(owner);

    EXPECT_EQ(restored.row, env.row);
    ASSERT_EQ(restored.attachments.size(), 3u);
    EXPECT_EQ(Bytes(Must(restored.attachments, "a")), a);
    EXPECT_EQ(Bytes(Must(restored.attachments, "b")), b);
    EXPECT_TRUE(Must(restored.attachments, "empty").empty());
    EXPECT_EQ(Must(restored.attachments, "empty").data(), nullptr);
}

TEST(EnvelopeTest, RoundtripWithLargeBlob) {
    std::vector<uint8_t> big(1'100'000, 0x42);
    Blob blob{std::move(big)};

    Envelope env;
    env.row = {0x00};
    env.attachments.Set("big", blob);

    auto serialized = SerializeEnvelope(env);
    auto owner = Owned(std::move(serialized));
    auto restored = DeserializeEnvelope(owner);

    EXPECT_EQ(restored.row, env.row);
    ASSERT_NE(restored.attachments.Find("big"), nullptr);
    EXPECT_EQ(Must(restored.attachments, "big").size(), 1'100'000u);
    EXPECT_EQ(Must(restored.attachments, "big").data()[0], 0x42);
}

TEST(EnvelopeTest, EmptyRowWithAttachments) {
    const std::vector<uint8_t> payload{0x01};
    Blob blob{payload};

    Envelope env;
    // row is empty
    env.attachments.Set("data", blob);

    auto serialized = SerializeEnvelope(env);
    auto owner = Owned(std::move(serialized));
    auto restored = DeserializeEnvelope(owner);

    EXPECT_TRUE(restored.row.empty());
    ASSERT_EQ(restored.attachments.size(), 1u);
    EXPECT_EQ(Bytes(Must(restored.attachments, "data")), payload);
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

// ---------------------------------------------------------------------------
// Decode cost: a wire-supplied attachment count is not a quadratic term
// ---------------------------------------------------------------------------
//
// `Attachments::Set` keeps a sorted vector, so keys arriving in DESCENDING order
// cost O(k) element moves each — O(k^2) for one envelope. Measured on MSVC 19.4
// /O2 with the decoders routed through `Set`: k = 7 000 took 57 ms, k = 60 000
// took 4.2 s, k = 200 000 took 58 s. Nothing bounds k on this path —
// `DeserializeEnvelope` reads the count off the wire and has no count check at
// all, and `gateway/src/ws_session.cpp` calls it on a client frame Beast caps at
// 16 MiB. So the decoders do not use `Set`: they build through
// `internal::AttachmentsWireBuilder`, which appends in arrival order and sorts
// once, O(k log k) whatever order the producer chose.
//
// This row is a TIMEOUT test by construction. `tests/CMakeLists.txt` gives every
// row in this suite a 30 s ctest TIMEOUT, and the quadratic build of this exact
// body took ~58 s, so a regression reddens as a timeout instead of hanging a CI
// job. Keep the count and that timeout in step if either moves.
TEST(EnvelopeTest, ADescendingAttachmentKeyOrderDoesNotMakeDecodeQuadratic) {
    constexpr uint32_t kCount = 200000;

    std::vector<uint8_t> buf;
    buf.reserve(static_cast<size_t>(kCount) * 16 + 8);
    AppendU32(buf, 0);       // row_len = 0
    AppendU32(buf, kCount);  // attachment count
    for (uint32_t i = 0; i < kCount; ++i) {
        // Fixed width, so decimal order IS unsigned-byte order: the highest key
        // arrives first and every subsequent one sorts before everything already
        // present — the worst case for a sorted-vector insert.
        const std::string key = FixedWidthKey(kCount - 1 - i);
        AppendU32(buf, static_cast<uint32_t>(key.size()));
        buf.insert(buf.end(), key.begin(), key.end());
        AppendU32(buf, 0);  // blob_len = 0
    }

    Envelope parsed = DeserializeEnvelope(Owned(std::move(buf)));

    ASSERT_EQ(parsed.attachments.size(), static_cast<size_t>(kCount));
    // Arrival order is not the published order: the set comes out ascending
    // whatever order the wire chose, which is what makes the bulk build a
    // complexity change rather than a behaviour change.
    EXPECT_EQ(parsed.attachments.KeyAt(0), FixedWidthKey(0));
    EXPECT_EQ(parsed.attachments.KeyAt(kCount - 1), FixedWidthKey(kCount - 1));
    for (size_t i = 1; i < parsed.attachments.size(); ++i) {
        ASSERT_LT(parsed.attachments.KeyAt(i - 1), parsed.attachments.KeyAt(i))
            << "the published sequence is not ascending at index " << i;
    }
    EXPECT_NE(parsed.attachments.Find(FixedWidthKey(12345)), nullptr);
}

// The one semantic the bulk build could quietly change. `Set` replaced in place,
// so a duplicate key on the wire resolved LAST-WINS; a sort that is not stable,
// or a dedupe that keeps the first of an equal run, would silently pick the
// other one. Unchanged from the `unordered_map` this container replaced, where
// `insert_or_assign` gave the same answer.
TEST(EnvelopeTest, ADuplicateAttachmentKeyOnTheWireResolvesLastWins) {
    std::vector<uint8_t> buf;
    AppendU32(buf, 0);  // row_len = 0
    AppendU32(buf, 3);  // attachment count
    const std::vector<std::pair<std::string, uint8_t>> wire = {
        {"k", 0x11}, {"a", 0x22}, {"k", 0x33}};
    for (const auto& [key, byte] : wire) {
        AppendU32(buf, static_cast<uint32_t>(key.size()));
        buf.insert(buf.end(), key.begin(), key.end());
        AppendU32(buf, 1);
        buf.push_back(byte);
    }

    Envelope parsed = DeserializeEnvelope(Owned(std::move(buf)));

    ASSERT_EQ(parsed.attachments.size(), static_cast<size_t>(2));
    EXPECT_EQ(parsed.attachments.KeyAt(0), "a");
    EXPECT_EQ(parsed.attachments.KeyAt(1), "k");
    ASSERT_EQ(Must(parsed.attachments, "k").size(), static_cast<size_t>(1));
    EXPECT_EQ(Must(parsed.attachments, "k").data()[0], 0x33)
        << "a duplicate wire key did not resolve last-wins";
}
