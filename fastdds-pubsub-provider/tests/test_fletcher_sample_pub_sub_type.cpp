// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The DDS type on its own — no participant, no writer, no reader. Every other test in this suite
// reaches serialize()/deserialize() through a live provider, which exercises them but cannot state
// what they produce; these do.
#include <fastcdr/Cdr.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <fastdds/rtps/common/SerializedPayload.hpp>
#include <fletcher/core/types.hpp>
#include <fletcher/core/write_buffer.hpp>
#include <stdexcept>
#include <string>
#include <vector>

#include "internal/envelope_codec.hpp"
#include "internal/fletcher_sample.hpp"
#include "internal/fletcher_sample_pub_sub_type.hpp"
#include "internal/payload_binding.hpp"
#include "internal/transport_data.hpp"

namespace {

using eprosima::fastdds::rtps::SerializedPayload_t;
using fletcher::internal::PublishData;
using fletcher::internal::ReceivedData;

// The bound these tests work at. The type is templated on it — one plain type per power of two
// between fletcher::kMinPayloadBytes and kMaxPayloadBytes — and every property below holds for all
// of them, so they are pinned down once here rather than repeated per bound. What differs between
// bounds is the type name and the sample size, which BoundsAreDistinctTypes covers directly.
constexpr uint32_t kTestPayloadBytes = 64 * 1024;
using FletcherSample = fletcher::internal::FletcherSample<kTestPayloadBytes>;
using FletcherSamplePubSubType = fletcher::internal::FletcherSamplePubSubType<kTestPayloadBytes>;

constexpr auto kXcdr1 = eprosima::fastdds::dds::DataRepresentationId_t::XCDR_DATA_REPRESENTATION;
constexpr auto kXcdr2 = eprosima::fastdds::dds::DataRepresentationId_t::XCDR2_DATA_REPRESENTATION;

// Offsets into the payload: the CDR encapsulation, then FletcherSample::length, then the body.
constexpr uint32_t kHeader = SerializedPayload_t::representation_header_size;
constexpr uint32_t kLengthPrefix = 4;

const fletcher::Attachments kNoAttachments;

std::vector<uint8_t> Row(size_t size, uint8_t fill = 0xAB) {
    return std::vector<uint8_t>(size, fill);
}

// Owns the encoder, because PublishData only borrows one.
struct Publishing {
    explicit Publishing(const std::vector<uint8_t>& row,
                        const fletcher::Attachments& attachments = kNoAttachments)
        : encoder([&row](fletcher::WriteBuffer& buf) { buf.Append(row.data(), row.size()); }) {
        data.encoder = &encoder;
        data.attachments = &attachments;
    }

    fletcher::PubSubProvider::RowEncoder encoder;
    PublishData data;
};

uint32_t ReadU32(const uint8_t* at) {
    uint32_t value = 0;
    std::memcpy(&value, at, sizeof(value));
    return value;
}

// ---------------------------------------------------------------------------
// What the type claims about itself
// ---------------------------------------------------------------------------

TEST(FletcherSamplePubSubTypeTest, ClaimsBoundedAndPlainForBothRepresentations) {
    FletcherSamplePubSubType type;
    EXPECT_TRUE(type.is_bounded());
    EXPECT_TRUE(type.is_plain(kXcdr1));
    EXPECT_TRUE(type.is_plain(kXcdr2));
}

TEST(FletcherSamplePubSubTypeTest, NameCarriesThePayloadBound) {
    FletcherSamplePubSubType type;
    EXPECT_EQ(type.get_name(), "fletcher_" + std::to_string(kTestPayloadBytes));
    EXPECT_EQ(type.max_serialized_type_size, kHeader + sizeof(FletcherSample));
}

// Two bounds are two DDS types, not one type configured two ways. That is what turns a bound
// mismatch into a discovery-time non-match instead of a silent per-sample drop, and it is why the
// bound can be a runtime option at all: each one is still plain.
TEST(FletcherSamplePubSubTypeTest, BoundsAreDistinctTypes) {
    fletcher::internal::FletcherSamplePubSubType<fletcher::kMinPayloadBytes> small;
    fletcher::internal::FletcherSamplePubSubType<fletcher::kMaxPayloadBytes> large;

    EXPECT_EQ(small.get_name(), "fletcher_4096");
    EXPECT_EQ(large.get_name(), "fletcher_8388608");
    EXPECT_LT(small.max_serialized_type_size, large.max_serialized_type_size);
    EXPECT_TRUE(small.is_plain(kXcdr1));
    EXPECT_TRUE(large.is_plain(kXcdr1));
}

// Asserted rather than EXPECTed, because being a compile error is the point: these would fail the
// build if the rule moved, which is what `kPayloadBytes<N>` gives a caller at its own call site.
TEST(FletcherSamplePubSubTypeTest, ThePayloadBoundSetIsPowersOfTwoInRange) {
    static_assert(fletcher::PayloadBound<fletcher::kMinPayloadBytes>);
    static_assert(fletcher::PayloadBound<64 * 1024>);
    static_assert(fletcher::PayloadBound<fletcher::kMaxPayloadBytes>);
    static_assert(!fletcher::PayloadBound<100'000>, "not a power of two");
    static_assert(!fletcher::PayloadBound<2048>, "below the floor");
    static_assert(!fletcher::PayloadBound<16 * 1024 * 1024>, "above the ceiling");
}

// The same rule for a bound that arrives as a number rather than as a template argument. Nothing is
// rounded: an unsupported one is refused, not adjusted to the nearest that would work.
TEST(FletcherSamplePubSubTypeTest, OnlyASupportedBoundGetsABinding) {
    using fletcher::internal::MakePayloadBinding;

    EXPECT_EQ(MakePayloadBinding(kTestPayloadBytes)->Bytes(), kTestPayloadBytes);
    EXPECT_EQ(MakePayloadBinding(fletcher::kMinPayloadBytes)->Bytes(), fletcher::kMinPayloadBytes);
    EXPECT_EQ(MakePayloadBinding(fletcher::kMaxPayloadBytes)->Bytes(), fletcher::kMaxPayloadBytes);

    EXPECT_THROW(MakePayloadBinding(100'000), std::invalid_argument);
    EXPECT_THROW(MakePayloadBinding(2048), std::invalid_argument);
    EXPECT_THROW(MakePayloadBinding(fletcher::kMaxPayloadBytes * 2), std::invalid_argument);
}

// ---------------------------------------------------------------------------
// serialize
// ---------------------------------------------------------------------------

TEST(FletcherSamplePubSubTypeTest, WireLayoutIsEncapsulationThenLengthThenBody) {
    FletcherSamplePubSubType type;
    const std::vector<uint8_t> row = Row(214);
    Publishing publishing(row);
    SerializedPayload_t payload(type.max_serialized_type_size);
    ASSERT_TRUE(type.serialize(&publishing.data, payload, kXcdr1));

    // [0] is the dummy octet, [1] the representation id, [2..3] the options field whose low octet
    // is the trailing-padding count.
    EXPECT_EQ(payload.data[0], 0x00);
    EXPECT_EQ(payload.data[1], CDR_LE);
    EXPECT_EQ(payload.data[2], 0x00);
    EXPECT_EQ(payload.data[3], eprosima::fastcdr::Cdr::alignment(payload.length, 4));
    EXPECT_EQ(payload.encapsulation, CDR_LE);

    // Then FletcherSample::length, and the envelope it counts: the row length, the row, and the
    // attachment count.
    const uint32_t body_size = ReadU32(payload.data + kHeader);
    EXPECT_EQ(payload.length, kHeader + kLengthPrefix + body_size);
    EXPECT_EQ(body_size, 4 + row.size() + 4);
    EXPECT_EQ(ReadU32(payload.data + kHeader + kLengthPrefix), row.size());
    EXPECT_EQ(0, std::memcmp(payload.data + kHeader + kLengthPrefix + 4, row.data(), row.size()));
}

TEST(FletcherSamplePubSubTypeTest, StopsAfterTheBytesInUse) {
    FletcherSamplePubSubType type;
    const std::vector<uint8_t> row = Row(10);
    Publishing publishing(row);
    SerializedPayload_t payload(type.max_serialized_type_size);
    ASSERT_TRUE(type.serialize(&publishing.data, payload, kXcdr1));

    // A plain type nominally carries its whole fixed-size body; this one truncates, which is what
    // keeps a small row small when loan_publish is off.
    EXPECT_EQ(payload.length, kHeader + kLengthPrefix + 4 + row.size() + 4);
    EXPECT_LT(payload.length, type.max_serialized_type_size);
}

TEST(FletcherSamplePubSubTypeTest, PaddingCountFollowsThePayloadLength) {
    FletcherSamplePubSubType type;
    for (size_t size : {1u, 2u, 3u, 4u, 5u, 214u, 4096u}) {
        const std::vector<uint8_t> row = Row(size);
        Publishing publishing(row);
        SerializedPayload_t payload(type.max_serialized_type_size);
        ASSERT_TRUE(type.serialize(&publishing.data, payload, kXcdr1)) << "size " << size;
        EXPECT_EQ(payload.data[3], eprosima::fastcdr::Cdr::alignment(payload.length, 4))
            << "size " << size;
    }
}

TEST(FletcherSamplePubSubTypeTest, Xcdr2ChangesOnlyTheRepresentationId) {
    FletcherSamplePubSubType type;
    const std::vector<uint8_t> row = Row(214);
    Publishing publishing(row);

    SerializedPayload_t one(type.max_serialized_type_size);
    SerializedPayload_t two(type.max_serialized_type_size);
    ASSERT_TRUE(type.serialize(&publishing.data, one, kXcdr1));
    ASSERT_TRUE(type.serialize(&publishing.data, two, kXcdr2));

    EXPECT_EQ(one.data[1], CDR_LE);
    EXPECT_EQ(two.data[1], fletcher::internal::RepresentationId(
                               eprosima::fastcdr::EncodingAlgorithmFlag::PLAIN_CDR2));
    ASSERT_EQ(one.length, two.length);
    EXPECT_EQ(0, std::memcmp(one.data + 2, two.data + 2, one.length - 2))
        << "a FINAL struct of a uint32 and an octet array has no DHEADER under PLAIN_CDR2, so only "
           "the id may differ";
}

TEST(FletcherSamplePubSubTypeTest, AnOversizedRowFailsAndEmptiesThePayload) {
    FletcherSamplePubSubType type;
    const std::vector<uint8_t> row = Row(kTestPayloadBytes);  // + the envelope: too big.
    Publishing publishing(row);
    SerializedPayload_t payload(type.max_serialized_type_size);

    EXPECT_FALSE(type.serialize(&publishing.data, payload, kXcdr1));
    EXPECT_EQ(payload.length, 0u);
}

// The property both publish flows rest on: whichever wrote the sample, a reader cannot tell.
TEST(FletcherSamplePubSubTypeTest, LoanedAndSerialisedBodiesAreIdentical) {
    FletcherSamplePubSubType type;
    const std::vector<uint8_t> row = Row(214);
    Publishing publishing(row);

    SerializedPayload_t payload(type.max_serialized_type_size);
    ASSERT_TRUE(type.serialize(&publishing.data, payload, kXcdr1));

    FletcherSample loaned{};
    fletcher::FixedWriteBuffer buf(loaned.body, kTestPayloadBytes);
    fletcher::internal::EncodeEnvelopeBody(buf, publishing.encoder, kNoAttachments);
    loaned.length = static_cast<uint32_t>(buf.Position());

    EXPECT_EQ(loaned.length, ReadU32(payload.data + kHeader));
    EXPECT_EQ(0, std::memcmp(loaned.body, payload.data + kHeader + kLengthPrefix, loaned.length));
}

// ---------------------------------------------------------------------------
// deserialize
// ---------------------------------------------------------------------------

TEST(FletcherSamplePubSubTypeTest, RoundTripsTheRow) {
    FletcherSamplePubSubType type;
    const std::vector<uint8_t> row = Row(214, 0x5A);
    Publishing publishing(row);
    SerializedPayload_t payload(type.max_serialized_type_size);
    ASSERT_TRUE(type.serialize(&publishing.data, payload, kXcdr1));

    ReceivedData received;
    ASSERT_TRUE(type.deserialize(payload, &received));
    EXPECT_EQ(received.decoded_row, row);
    EXPECT_TRUE(received.decoded_attachments.empty());
}

TEST(FletcherSamplePubSubTypeTest, RoundTripsAttachments) {
    FletcherSamplePubSubType type;
    const std::vector<uint8_t> row = Row(32);
    fletcher::Attachments sent;
    sent["sidecar"] = std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{1, 2, 3});
    Publishing publishing(row, sent);

    SerializedPayload_t payload(type.max_serialized_type_size);
    ASSERT_TRUE(type.serialize(&publishing.data, payload, kXcdr1));

    ReceivedData received;
    ASSERT_TRUE(type.deserialize(payload, &received));
    EXPECT_EQ(received.decoded_row, row);
    ASSERT_EQ(received.decoded_attachments.count("sidecar"), 1u);
    EXPECT_EQ(*received.decoded_attachments.at("sidecar"), std::vector<uint8_t>({1, 2, 3}));
}

TEST(FletcherSamplePubSubTypeTest, RoundTripsAnXcdr2Payload) {
    FletcherSamplePubSubType type;
    const std::vector<uint8_t> row = Row(64);
    Publishing publishing(row);
    SerializedPayload_t payload(type.max_serialized_type_size);
    ASSERT_TRUE(type.serialize(&publishing.data, payload, kXcdr2));

    ReceivedData received;
    ASSERT_TRUE(type.deserialize(payload, &received));
    EXPECT_EQ(received.decoded_row, row);
}

TEST(FletcherSamplePubSubTypeTest, RefusesAnEncapsulationItCannotParse) {
    FletcherSamplePubSubType type;
    const std::vector<uint8_t> row = Row(64);
    Publishing publishing(row);
    SerializedPayload_t payload(type.max_serialized_type_size);
    ASSERT_TRUE(type.serialize(&publishing.data, payload, kXcdr1));

    ReceivedData received;
    // The envelope's own length fields are host-order little-endian, so big-endian CDR and the
    // parameter-list encodings are not misread — they are refused.
    for (uint8_t id : {uint8_t{CDR_BE}, uint8_t{PL_CDR_BE}, uint8_t{PL_CDR_LE}, uint8_t{0x06}}) {
        payload.data[1] = id;
        EXPECT_FALSE(type.deserialize(payload, &received)) << "id " << static_cast<int>(id);
    }
}

TEST(FletcherSamplePubSubTypeTest, RefusesAPayloadShorterThanItsHeader) {
    FletcherSamplePubSubType type;
    const std::vector<uint8_t> row = Row(64);
    Publishing publishing(row);
    SerializedPayload_t payload(type.max_serialized_type_size);
    ASSERT_TRUE(type.serialize(&publishing.data, payload, kXcdr1));

    ReceivedData received;
    const uint32_t full = payload.length;
    payload.length = kHeader + kLengthPrefix - 1;
    EXPECT_FALSE(type.deserialize(payload, &received));

    // And a length field claiming more than arrived: the parse is bounded by what the writer said
    // it wrote, clamped to what is actually there.
    payload.length = full;
    const uint32_t lie = full;
    std::memcpy(payload.data + kHeader, &lie, sizeof(lie));
    EXPECT_FALSE(type.deserialize(payload, &received));
}

// create_data/delete_data serve the reader, so they make the received half of the sample. Fast DDS
// only calls them for a non-plain type, so for this one they are never reached — but they have to
// be the right type if they are.
TEST(FletcherSamplePubSubTypeTest, CreateDataMakesSomethingDeserializeCanFill) {
    FletcherSamplePubSubType type;
    const std::vector<uint8_t> row = Row(16);
    Publishing publishing(row);
    SerializedPayload_t payload(type.max_serialized_type_size);
    ASSERT_TRUE(type.serialize(&publishing.data, payload, kXcdr1));

    void* sample = type.create_data();
    ASSERT_NE(sample, nullptr);
    ASSERT_TRUE(type.deserialize(payload, sample));
    EXPECT_EQ(static_cast<ReceivedData*>(sample)->decoded_row, row);
    type.delete_data(sample);
}

TEST(FletcherSamplePubSubTypeTest, IsNotKeyed) {
    FletcherSamplePubSubType type;
    EXPECT_FALSE(type.is_compute_key_provided);

    eprosima::fastdds::rtps::InstanceHandle_t handle;
    const std::vector<uint8_t> row = Row(16);
    Publishing publishing(row);
    SerializedPayload_t payload(type.max_serialized_type_size);
    ASSERT_TRUE(type.serialize(&publishing.data, payload, kXcdr1));

    EXPECT_FALSE(type.compute_key(payload, handle, false));
    EXPECT_FALSE(type.compute_key(&publishing.data, handle, false));
}

}  // namespace
