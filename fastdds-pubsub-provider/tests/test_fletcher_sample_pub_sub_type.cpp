// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The DDS type on its own — no participant, no writer, no reader. Every other test in this suite
// reaches serialize()/deserialize() through a live provider, which exercises them but cannot state
// what they produce; these do.
#include <fastcdr/Cdr.h>
#include <fastcdr/FastBuffer.h>
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
#include "internal/qos_defaults.hpp"
#include "internal/raw_bytes_pub_sub_type.hpp"
#include "internal/transport_data.hpp"

namespace {

using eprosima::fastdds::rtps::SerializedPayload_t;
using fletcher::internal::PublishData;
using fletcher::internal::ReceivedData;

// The bound these tests work at; every property below holds for any bound the rule admits.
constexpr uint32_t kTestPayloadBytes = 64 * 1024;
using fletcher::internal::FletcherSamplePubSubType;
using fletcher::internal::SampleSize;

constexpr auto kXcdr1 = eprosima::fastdds::dds::DataRepresentationId_t::XCDR_DATA_REPRESENTATION;
constexpr auto kXcdr2 = eprosima::fastdds::dds::DataRepresentationId_t::XCDR2_DATA_REPRESENTATION;

// Offsets into the payload: the CDR encapsulation, then the sample's length, then the body.
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

// The reference arm for FastCdrReproducesTheBytesExactly: fastcdr places the framing instead.
class FastCdrFramedPubSubType : public FletcherSamplePubSubType {
   public:
    using FletcherSamplePubSubType::FletcherSamplePubSubType;

    bool serialize(const void* const data, SerializedPayload_t& payload,
                   eprosima::fastdds::dds::DataRepresentationId_t data_representation) override {
        const auto* d = static_cast<const PublishData*>(data);
        const bool xcdr1 = data_representation == kXcdr1;

        eprosima::fastcdr::FastBuffer fastbuffer(reinterpret_cast<char*>(payload.data),
                                                 payload.max_size);
        eprosima::fastcdr::Cdr ser(
            fastbuffer, eprosima::fastcdr::Cdr::DEFAULT_ENDIAN,
            xcdr1 ? eprosima::fastcdr::CdrVersion::XCDRv1 : eprosima::fastcdr::CdrVersion::XCDRv2);
        payload.encapsulation =
            ser.endianness() == eprosima::fastcdr::Cdr::BIG_ENDIANNESS ? CDR_BE : CDR_LE;
        ser.set_encoding_flag(xcdr1 ? eprosima::fastcdr::EncodingAlgorithmFlag::PLAIN_CDR
                                    : eprosima::fastcdr::EncodingAlgorithmFlag::PLAIN_CDR2);

        try {
            ser.serialize_encapsulation();

            // The save-state / serialize / jump sequence fastcdr uses for an XCDR2 DHEADER.
            const eprosima::fastcdr::Cdr::state length_state = ser.get_state();
            ser.serialize(static_cast<uint32_t>(0));

            const size_t body_offset = ser.get_serialized_data_length();
            fletcher::FixedWriteBuffer buf(payload.data + body_offset, kTestPayloadBytes);
            fletcher::internal::EncodeEnvelopeBody(buf, *d->encoder, *d->attachments);
            const auto body_size = static_cast<uint32_t>(buf.Position());

            ser.set_state(length_state);
            ser.serialize(body_size);
            ser.jump(body_size);
            ser.set_dds_cdr_options({0, 0});

            payload.length = static_cast<uint32_t>(ser.get_serialized_data_length());
            return true;
        } catch (...) {
            payload.length = 0;
            return false;
        }
    }
};

// ---------------------------------------------------------------------------
// What the type claims about itself
// ---------------------------------------------------------------------------

TEST(FletcherSamplePubSubTypeTest, ClaimsBoundedAndPlainForBothRepresentations) {
    FletcherSamplePubSubType type(kTestPayloadBytes);
    EXPECT_TRUE(type.is_bounded());
    EXPECT_TRUE(type.is_plain(kXcdr1));
    EXPECT_TRUE(type.is_plain(kXcdr2));
}

TEST(FletcherSamplePubSubTypeTest, NameCarriesThePayloadBound) {
    FletcherSamplePubSubType type(kTestPayloadBytes);
    EXPECT_EQ(type.get_name(), "fletcher_" + std::to_string(kTestPayloadBytes));
    EXPECT_EQ(type.max_serialized_type_size, kHeader + SampleSize(kTestPayloadBytes));
}

// Two bounds are two DDS types, which is what makes a mismatch a discovery-time non-match.
TEST(FletcherSamplePubSubTypeTest, BoundsAreDistinctTypes) {
    FletcherSamplePubSubType small(fletcher::kMinPayloadBytes);
    FletcherSamplePubSubType large(kTestPayloadBytes);

    EXPECT_EQ(small.get_name(), "fletcher_4");
    EXPECT_EQ(large.get_name(), "fletcher_65536");
    EXPECT_NE(small.get_name(), large.get_name());
    EXPECT_LT(small.max_serialized_type_size, large.max_serialized_type_size);
    EXPECT_TRUE(small.is_plain(kXcdr1));
    EXPECT_TRUE(large.is_plain(kXcdr1));
}

// static_assert because being a compile error is the point: kPayloadBytes<N> gives callers this.
TEST(FletcherSamplePubSubTypeTest, ThePayloadBoundRuleIsFourByteAlignment) {
    static_assert(fletcher::PayloadBound<fletcher::kMinPayloadBytes>);
    static_assert(fletcher::PayloadBound<64 * 1024>);
    static_assert(fletcher::PayloadBound<100'000>, "4-aligned, so usable, power of two or not");
    static_assert(fletcher::PayloadBound<fletcher::kMaxPayloadBytes>);
    static_assert(!fletcher::PayloadBound<100'001>, "not a multiple of 4");
    static_assert(!fletcher::PayloadBound<0>, "not positive");
    static_assert(!fletcher::PayloadBound<2>, "positive, but cannot frame a sample");

    // The same expression for a runtime bound, which is what the provider constructor rejects on.
    EXPECT_TRUE(fletcher::IsPayloadBound(kTestPayloadBytes));
    EXPECT_TRUE(fletcher::IsPayloadBound(100'000));
    EXPECT_FALSE(fletcher::IsPayloadBound(100'001));
    EXPECT_FALSE(fletcher::IsPayloadBound(0));
    EXPECT_FALSE(fletcher::IsPayloadBound(fletcher::kMaxPayloadBytes + 4));
}

// ---------------------------------------------------------------------------
// serialize
// ---------------------------------------------------------------------------

TEST(FletcherSamplePubSubTypeTest, WireLayoutIsEncapsulationThenLengthThenBody) {
    FletcherSamplePubSubType type(kTestPayloadBytes);
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

    // Then the sample's length, and the envelope it counts.
    const uint32_t body_size = ReadU32(payload.data + kHeader);
    EXPECT_EQ(payload.length, kHeader + kLengthPrefix + body_size);
    EXPECT_EQ(body_size, 4 + row.size() + 4);
    EXPECT_EQ(ReadU32(payload.data + kHeader + kLengthPrefix), row.size());
    EXPECT_EQ(0, std::memcmp(payload.data + kHeader + kLengthPrefix + 4, row.data(), row.size()));
}

TEST(FletcherSamplePubSubTypeTest, StopsAfterTheBytesInUse) {
    FletcherSamplePubSubType type(kTestPayloadBytes);
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
    FletcherSamplePubSubType type(kTestPayloadBytes);
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
    FletcherSamplePubSubType type(kTestPayloadBytes);
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

// Writing the encapsulation by hand is only licensed while it matches fastcdr byte for byte.
TEST(FletcherSamplePubSubTypeTest, FastCdrReproducesTheBytesExactly) {
    FletcherSamplePubSubType shipped(kTestPayloadBytes);
    FastCdrFramedPubSubType reference(kTestPayloadBytes);
    const std::vector<uint8_t> row = Row(214);
    Publishing publishing(row);

    SerializedPayload_t shipped_xcdr1(shipped.max_serialized_type_size);
    SerializedPayload_t reference_xcdr1(reference.max_serialized_type_size);
    ASSERT_TRUE(shipped.serialize(&publishing.data, shipped_xcdr1, kXcdr1));
    ASSERT_TRUE(reference.serialize(&publishing.data, reference_xcdr1, kXcdr1));

    ASSERT_EQ(reference_xcdr1.length, shipped_xcdr1.length);
    EXPECT_EQ(0, std::memcmp(reference_xcdr1.data, shipped_xcdr1.data, shipped_xcdr1.length))
        << "the hand-written XCDR1 framing diverged from fastcdr";

    // Again under XCDR2, which keeps both hand-written representation ids honest.
    SerializedPayload_t shipped_xcdr2(shipped.max_serialized_type_size);
    SerializedPayload_t reference_xcdr2(reference.max_serialized_type_size);
    ASSERT_TRUE(shipped.serialize(&publishing.data, shipped_xcdr2, kXcdr2));
    ASSERT_TRUE(reference.serialize(&publishing.data, reference_xcdr2, kXcdr2));

    ASSERT_EQ(reference_xcdr2.length, shipped_xcdr2.length);
    EXPECT_EQ(0, std::memcmp(reference_xcdr2.data, shipped_xcdr2.data, shipped_xcdr2.length))
        << "the hand-written XCDR2 framing diverged from fastcdr";
    EXPECT_NE(shipped_xcdr2.data[1], shipped_xcdr1.data[1])
        << "XCDR2 must not reuse the XCDR1 representation id";
}

// One wire shape for both channels, though they are framed by different code.
TEST(FletcherSamplePubSubTypeTest, TheSchemaChannelFramesTheSameShapeAsTheDataChannel) {
    FletcherSamplePubSubType data_type(kTestPayloadBytes);
    const std::vector<uint8_t> row = Row(214);
    Publishing publishing(row);

    SerializedPayload_t framed(data_type.max_serialized_type_size);
    ASSERT_TRUE(data_type.serialize(&publishing.data, framed, kXcdr1));

    // The same envelope bytes through the schema channel's non-plain sequence<octet>.
    const uint32_t body_size = framed.length - kHeader - kLengthPrefix;
    fletcher::internal::RawBytes raw;
    raw.data.assign(framed.data + kHeader + kLengthPrefix,
                    framed.data + kHeader + kLengthPrefix + body_size);

    fletcher::internal::RawBytesPubSubType schema_type(kTestPayloadBytes);
    SerializedPayload_t via_fastcdr(schema_type.max_serialized_type_size);
    ASSERT_TRUE(schema_type.serialize(&raw, via_fastcdr, kXcdr1));

    ASSERT_EQ(framed.length, via_fastcdr.length);
    EXPECT_EQ(0, std::memcmp(framed.data, via_fastcdr.data, framed.length))
        << "the hand-framed data channel and the fastcdr-framed schema channel disagree about the "
           "same wire shape";
}

// Bounded but not plain, and construct_sample must keep answering false for a non-plain type.
TEST(FletcherSamplePubSubTypeTest, TheSchemaChannelIsBoundedButNotPlain) {
    fletcher::internal::RawBytesPubSubType schema_type(kTestPayloadBytes);
    EXPECT_TRUE(schema_type.is_bounded());
    EXPECT_FALSE(schema_type.is_plain(kXcdr1));
    EXPECT_FALSE(schema_type.is_plain(kXcdr2));
    EXPECT_FALSE(schema_type.construct_sample(nullptr));
    EXPECT_EQ(4u + 4u + kTestPayloadBytes, schema_type.max_serialized_type_size);

    // 4 + 100'001 = 100'005, padded to 100'008, plus the 4-byte encapsulation.
    fletcher::internal::RawBytesPubSubType odd_type(100'001);
    EXPECT_EQ(100'012u, odd_type.max_serialized_type_size);

    // Saturates rather than wraps: 32-bit arithmetic would have reported eight bytes.
    fletcher::internal::RawBytesPubSubType absurd_type(UINT32_MAX);
    EXPECT_EQ(UINT32_MAX, absurd_type.max_serialized_type_size);

    // And the size of an actual sample comes out of fastcdr, not out of that ceiling.
    fletcher::internal::RawBytes sample;
    sample.data.assign(214, 0xAB);
    EXPECT_EQ(4u + 4u + 214u, schema_type.calculate_serialized_size(&sample, kXcdr1));

    // The data channel claims all three, which is what earns it loans.
    FletcherSamplePubSubType data_type(kTestPayloadBytes);
    EXPECT_TRUE(data_type.is_bounded());
    EXPECT_TRUE(data_type.is_plain(kXcdr1));
    EXPECT_TRUE(data_type.is_plain(kXcdr2));
}

// The pool is sized for the one sample the channel can hold, which is why bounded is affordable.
TEST(FletcherSamplePubSubTypeTest, TheSchemaChannelPoolIsSizedForOneSample) {
    const auto wqos = fletcher::internal::MakeSchemaChannelWriterQos();
    EXPECT_EQ(eprosima::fastdds::dds::KEEP_LAST_HISTORY_QOS, wqos.history().kind);
    EXPECT_EQ(1, wqos.history().depth);
    EXPECT_EQ(1, wqos.resource_limits().max_samples);
    EXPECT_EQ(1, wqos.resource_limits().allocated_samples);

    const auto rqos = fletcher::internal::MakeSchemaChannelReaderQos();
    EXPECT_EQ(eprosima::fastdds::dds::KEEP_LAST_HISTORY_QOS, rqos.history().kind);
    EXPECT_EQ(1, rqos.history().depth);
    EXPECT_EQ(1, rqos.resource_limits().max_samples);
    EXPECT_EQ(1, rqos.resource_limits().allocated_samples);
}

TEST(FletcherSamplePubSubTypeTest, AnOversizedRowFailsAndEmptiesThePayload) {
    FletcherSamplePubSubType type(kTestPayloadBytes);
    const std::vector<uint8_t> row = Row(kTestPayloadBytes);  // + the envelope: too big.
    Publishing publishing(row);
    SerializedPayload_t payload(type.max_serialized_type_size);

    EXPECT_FALSE(type.serialize(&publishing.data, payload, kXcdr1));
    EXPECT_EQ(payload.length, 0u);
}

// The property both publish flows rest on: whichever wrote the sample, a reader cannot tell.
TEST(FletcherSamplePubSubTypeTest, LoanedAndSerialisedBodiesAreIdentical) {
    FletcherSamplePubSubType type(kTestPayloadBytes);
    const std::vector<uint8_t> row = Row(214);
    Publishing publishing(row);

    SerializedPayload_t payload(type.max_serialized_type_size);
    ASSERT_TRUE(type.serialize(&publishing.data, payload, kXcdr1));

    // What the loaned flow does, with a plain buffer standing in for the payload Fast DDS lends.
    std::vector<uint8_t> loaned(SampleSize(kTestPayloadBytes));
    fletcher::FixedWriteBuffer buf(fletcher::internal::SampleBody(loaned.data()),
                                   kTestPayloadBytes);
    fletcher::internal::EncodeEnvelopeBody(buf, publishing.encoder, kNoAttachments);
    fletcher::internal::WriteSampleLength(loaned.data(), static_cast<uint32_t>(buf.Position()));

    const uint32_t length = fletcher::internal::ReadSampleLength(loaned.data());
    EXPECT_EQ(length, ReadU32(payload.data + kHeader));
    EXPECT_EQ(0, std::memcmp(fletcher::internal::SampleBody(loaned.data()),
                             payload.data + kHeader + kLengthPrefix, length));
}

// ---------------------------------------------------------------------------
// deserialize
// ---------------------------------------------------------------------------

TEST(FletcherSamplePubSubTypeTest, RoundTripsTheRow) {
    FletcherSamplePubSubType type(kTestPayloadBytes);
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
    FletcherSamplePubSubType type(kTestPayloadBytes);
    const std::vector<uint8_t> row = Row(32);
    fletcher::Attachments sent;
    sent["sidecar"] = fletcher::Blob{std::vector<uint8_t>{1, 2, 3}};
    Publishing publishing(row, sent);

    SerializedPayload_t payload(type.max_serialized_type_size);
    ASSERT_TRUE(type.serialize(&publishing.data, payload, kXcdr1));

    ReceivedData received;
    ASSERT_TRUE(type.deserialize(payload, &received));
    EXPECT_EQ(received.decoded_row, row);
    ASSERT_EQ(received.decoded_attachments.count("sidecar"), 1u);
    const fletcher::Blob& sidecar = received.decoded_attachments.at("sidecar");
    ASSERT_EQ(sidecar.size(), 3u);
    EXPECT_EQ(std::vector<uint8_t>(sidecar.data(), sidecar.data() + sidecar.size()),
              std::vector<uint8_t>({1, 2, 3}));
    // The decoded attachment aliases the ONE body copy ReceivedData now owns
    // (§3.2), instead of being a copy of its own.
    ASSERT_NE(received.body, nullptr);
    EXPECT_GE(sidecar.data(), received.body->data());
    EXPECT_LT(sidecar.data(), received.body->data() + received.body->size());
}

TEST(FletcherSamplePubSubTypeTest, RoundTripsAnXcdr2Payload) {
    FletcherSamplePubSubType type(kTestPayloadBytes);
    const std::vector<uint8_t> row = Row(64);
    Publishing publishing(row);
    SerializedPayload_t payload(type.max_serialized_type_size);
    ASSERT_TRUE(type.serialize(&publishing.data, payload, kXcdr2));

    ReceivedData received;
    ASSERT_TRUE(type.deserialize(payload, &received));
    EXPECT_EQ(received.decoded_row, row);
}

TEST(FletcherSamplePubSubTypeTest, RefusesAnEncapsulationItCannotParse) {
    FletcherSamplePubSubType type(kTestPayloadBytes);
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
    FletcherSamplePubSubType type(kTestPayloadBytes);
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
    FletcherSamplePubSubType type(kTestPayloadBytes);
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
    FletcherSamplePubSubType type(kTestPayloadBytes);
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
