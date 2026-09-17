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
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "internal/envelope_codec.hpp"
#include "internal/fletcher_sample.hpp"
#include "internal/fletcher_sample_pub_sub_type.hpp"
#include "internal/qos_defaults.hpp"
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

// The companion __schema channel's TopicDataType: the same plain sample as the data channel, under
// a different registered name (design owner-approved 2026-09-14). One TopicDataType
// implementation, so its claims about itself are the data type's, checked once here rather than
// duplicated per bound.
TEST(SchemaBytesPubSubTypeTest, NameBoundAndPlainness) {
    fletcher::internal::SchemaBytesPubSubType schema_type(kTestPayloadBytes);
    FletcherSamplePubSubType data_type(kTestPayloadBytes);

    EXPECT_EQ(schema_type.get_name(), fletcher::kSchemaTypeName);
    EXPECT_TRUE(schema_type.is_bounded());
    EXPECT_TRUE(schema_type.is_plain(kXcdr1));
    EXPECT_TRUE(schema_type.is_plain(kXcdr2));
    EXPECT_EQ(schema_type.max_serialized_type_size, data_type.max_serialized_type_size);
}

// The schema rides as a row -- the IPC bytes -- plus one attachment, the publisher's payload
// bound, through the same envelope the data channel uses: [u32 length][u32 row_len][ipc
// bytes][u32 attachment_count = 1][attachment]. This pins the shape the XRCE side must produce on
// the wire.
TEST(SchemaBytesPubSubTypeTest, AnIpcBlobRoundTripsAsARowWithItsPayloadBound) {
    fletcher::internal::SchemaBytesPubSubType type(kTestPayloadBytes);
    const std::vector<uint8_t> blob = Row(300, 0x77);
    fletcher::Attachments att;
    att.Set(fletcher::kSchemaPayloadBoundKey, fletcher::Blob(std::vector<uint8_t>{0, 0, 1, 0}));
    Publishing publishing(blob, att);

    SerializedPayload_t payload(type.max_serialized_type_size);
    ASSERT_TRUE(type.serialize(&publishing.data, payload, kXcdr1));

    // The body's own row-length prefix, right after the sample length.
    EXPECT_EQ(ReadU32(payload.data + kHeader + kLengthPrefix), blob.size());
    // kHeader + kLengthPrefix frame the sample; then the body: a 4-byte row_len, the row itself, a
    // 4-byte attachment count, and the one 29-byte `max_payload_bytes` attachment (4-byte key_len +
    // 17-byte key + 4-byte blob_len + 4-byte blob).
    EXPECT_EQ(payload.length, kHeader + kLengthPrefix + 4 + blob.size() + 4 + 29);

    ReceivedData received;
    ASSERT_TRUE(type.deserialize(payload, &received));
    EXPECT_EQ(received.decoded_row, blob);
    ASSERT_EQ(received.decoded_attachments.size(), static_cast<size_t>(1));
    const fletcher::Blob* found =
        received.decoded_attachments.Find(fletcher::kSchemaPayloadBoundKey);
    ASSERT_NE(found, nullptr);
    ASSERT_EQ(found->size(), 4u);
    uint32_t bound = 0;
    std::memcpy(&bound, found->data(), sizeof(bound));
    EXPECT_EQ(bound, 65536u);
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

// P18: the guard at the top of serialize() now accounts for the row's OWN 4-byte length prefix
// (EncodeEnvelopeBody's ROW_LEN) as well as the sample's — even an EMPTY row needs it. A buffer one
// byte too small for that must be refused quietly, before the encoder ever runs, rather than
// reaching it and failing with the "oversized row" diagnostic, which is the wrong story for a
// buffer that could never have held anything.
TEST(FletcherSamplePubSubTypeTest, ABufferTooSmallForAnEmptyRowIsRefusedQuietly) {
    FletcherSamplePubSubType type(kTestPayloadBytes);
    const std::vector<uint8_t> row = Row(0);
    Publishing publishing(row);
    SerializedPayload_t payload(kHeader + kLengthPrefix + 4 - 1);

    EXPECT_FALSE(type.serialize(&publishing.data, payload, kXcdr1));
    EXPECT_EQ(payload.length, 0u);
}

TEST(FletcherSamplePubSubTypeTest, AnOversizedRowFailsAndEmptiesThePayload) {
    FletcherSamplePubSubType type(kTestPayloadBytes);
    const std::vector<uint8_t> row = Row(kTestPayloadBytes);  // + the envelope: too big.
    Publishing publishing(row);
    SerializedPayload_t payload(type.max_serialized_type_size);

    EXPECT_FALSE(type.serialize(&publishing.data, payload, kXcdr1));
    EXPECT_EQ(payload.length, 0u);
}

// T1 (cycle 2), the exact-fit edge P18's guard leaves untested. `capacity` in serialize() is
// `min(payload_bytes_, payload.max_size - kFraming)`, and the payload below reserves
// `max_serialized_type_size == kFraming + payload_bytes_`, so here `capacity == kTestPayloadBytes`
// exactly. EncodeEnvelopeBody's body framing for a ZERO-attachment row is 8 bytes — a 4-byte
// ROW_LEN placeholder plus a 4-byte ATTACH_COUNT of zero (envelope_codec.hpp) — not the CDR
// encapsulation header or the sample length prefix, both of which `kFraming` already accounts
// for separately. So `body_size = 8 + row.size()`, and a row of `bound - 8` bytes is the largest
// that fits `capacity` at all.
TEST(FletcherSamplePubSubTypeTest, ARowOfExactlyTheAvailableCapacitySerializesAndRoundTrips) {
    FletcherSamplePubSubType type(kTestPayloadBytes);
    const std::vector<uint8_t> row = Row(kTestPayloadBytes - 8, 0x37);
    Publishing publishing(row);
    SerializedPayload_t payload(type.max_serialized_type_size);

    ASSERT_TRUE(type.serialize(&publishing.data, payload, kXcdr1));

    ReceivedData received;
    ASSERT_TRUE(type.deserialize(payload, &received));
    EXPECT_EQ(received.decoded_row, row);
    EXPECT_TRUE(received.decoded_attachments.empty());
}

// The other edge: one byte more than the exact fit above cannot fit `capacity` at all, so
// EncodeEnvelopeBody's overflow throws inside serialize()'s try block and is turned into a quiet
// `false` (the "capacity outcome of a bounded type" catch(std::overflow_error) arm), the same
// refusal AnOversizedRowFailsAndEmptiesThePayload pins for a row far past the bound.
TEST(FletcherSamplePubSubTypeTest,
     ARowOneByteOverTheAvailableCapacityIsRefusedAndEmptiesThePayload) {
    FletcherSamplePubSubType type(kTestPayloadBytes);
    const std::vector<uint8_t> row = Row(kTestPayloadBytes - 8 + 1, 0x37);
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
    sent.Set("sidecar", fletcher::Blob{std::vector<uint8_t>{1, 2, 3}});
    Publishing publishing(row, sent);

    SerializedPayload_t payload(type.max_serialized_type_size);
    ASSERT_TRUE(type.serialize(&publishing.data, payload, kXcdr1));

    ReceivedData received;
    ASSERT_TRUE(type.deserialize(payload, &received));
    EXPECT_EQ(received.decoded_row, row);
    const fletcher::Blob* found = received.decoded_attachments.Find("sidecar");
    ASSERT_NE(found, nullptr);
    const fletcher::Blob& sidecar = *found;
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

// ── PDA-DEC-AG2: the body's attachment order is part of the format ───
//
// `EncodeEnvelopeBody` is the second of the two encoders that put attachments on
// the wire (the first is core's `SerializeEnvelope`, asserted in the conformance
// harness, which links no transport SDK and cannot reach this one). The owner's
// 2026-09-06 ruling authorised moving these bytes for attachment ordering, so
// the claim is asserted ON THE BYTES rather than on the container.
TEST(EnvelopeCodecTest, TheSameBodyIsEncodedToTheSameBytes) {
    const std::vector<uint8_t> row = Row(8);
    auto encoder = [&row](fletcher::WriteBuffer& buf) { buf.Append(row.data(), row.size()); };

    const std::vector<std::pair<std::string, fletcher::Blob>> entries{
        {"zulu", fletcher::Blob{std::vector<uint8_t>{0x01, 0x02}}},
        {"alpha", fletcher::Blob{std::vector<uint8_t>{0x03}}},
        {"mike", fletcher::Blob{std::vector<uint8_t>{0x04, 0x05, 0x06}}},
        {"bravo", fletcher::Blob{std::vector<uint8_t>{0x07}}},
    };
    auto encode_in_order = [&](const std::vector<size_t>& order) {
        fletcher::Attachments attachments;
        for (const size_t i : order) attachments.Set(entries[i].first, entries[i].second);
        fletcher::VectorWriteBuffer buf;
        fletcher::internal::EncodeEnvelopeBody(buf, encoder, attachments);
        return buf.Finish();
    };

    const std::vector<uint8_t> reference = encode_in_order({0, 1, 2, 3});
    EXPECT_EQ(encode_in_order({3, 2, 1, 0}), reference)
        << "the same body built in a different order encoded to different bytes";
    EXPECT_EQ(encode_in_order({1, 3, 0, 2}), reference)
        << "the same body built in a different order encoded to different bytes";

    // Round-tripping through the codec's own parser recovers the same sequence,
    // ascending — the order a decoder on another machine is told to expect.
    auto owner = std::make_shared<const std::vector<uint8_t>>(reference);
    const uint8_t* decoded_row = nullptr;
    uint32_t decoded_row_len = 0;
    fletcher::Attachments decoded;
    ASSERT_TRUE(fletcher::internal::ParseEnvelopeBody(owner, owner->data(), owner->size(),
                                                      decoded_row, decoded_row_len, decoded));
    ASSERT_EQ(decoded.size(), static_cast<size_t>(4));
    std::vector<std::string> keys;
    for (size_t i = 0; i < decoded.size(); ++i) keys.emplace_back(decoded.KeyAt(i));
    EXPECT_EQ(keys, (std::vector<std::string>{"alpha", "bravo", "mike", "zulu"}))
        << "the encoded key order is not ascending unsigned-byte order";
}

// ── PDA-DEC-AG2 / owner ruling 2026-09-06: refused on arrival ────────
//
// A wire-supplied attachment key carrying a zero byte is a SIXTH malformation,
// dropped exactly like the five `ParseEnvelopeBody` already refuses. Two things
// are asserted, and the second is the one that matters: the refusal is a
// `return false`, so NOTHING is thrown — this function runs inside Fast DDS's
// own `deserialize()` and `on_data_available` frames, where an escaping
// exception is a process termination rather than an unwind (§5.3). Routing this
// through `Attachments::Set` instead would throw there, and this test is what
// catches that.
TEST(EnvelopeCodecTest, AnArrivingAttachmentKeyWithAZeroByteIsDroppedAsMalformed) {
    auto append_u32 = [](std::vector<uint8_t>& out, uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8) {
            out.push_back(static_cast<uint8_t>((value >> shift) & 0xFFu));
        }
    };
    // A body no local encoder can produce: the zero byte is in the wire bytes.
    auto body_with_key = [&](const std::string& key) {
        std::vector<uint8_t> body;
        append_u32(body, 1);  // row_len
        body.push_back(0xFF);
        append_u32(body, 1);  // attachment count
        append_u32(body, static_cast<uint32_t>(key.size()));
        body.insert(body.end(), key.begin(), key.end());
        append_u32(body, 0);  // blob_len
        return body;
    };

    std::string nul_bearing = "a";
    nul_bearing.push_back(static_cast<char>(0));
    nul_bearing += "b";

    const std::vector<uint8_t> malformed = body_with_key(nul_bearing);
    auto owner = std::make_shared<const std::vector<uint8_t>>(malformed);
    const uint8_t* row = nullptr;
    uint32_t row_len = 0;
    fletcher::Attachments attachments;

    bool accepted = true;
    EXPECT_NO_THROW({
        accepted = fletcher::internal::ParseEnvelopeBody(owner, owner->data(), owner->size(), row,
                                                         row_len, attachments);
    }) << "the arrival refusal THREW inside a path that runs in a transport callback";
    EXPECT_FALSE(accepted) << "a key carrying a zero byte was accepted off the wire";
    EXPECT_EQ(attachments.size(), static_cast<size_t>(0))
        << "the refused key was stored before the sample was dropped";

    // The bound on the narrowing: the identical body with a clean key parses,
    // so a parser that refused every attachment is not green above.
    const std::vector<uint8_t> clean = body_with_key("axb");
    auto clean_owner = std::make_shared<const std::vector<uint8_t>>(clean);
    ASSERT_TRUE(fletcher::internal::ParseEnvelopeBody(
        clean_owner, clean_owner->data(), clean_owner->size(), row, row_len, attachments));
    ASSERT_EQ(attachments.size(), static_cast<size_t>(1));
    EXPECT_EQ(attachments.KeyAt(0), "axb");
}

// T2 (cycle 2): one malformation per row, each refused by `ParseEnvelopeBody` with a quiet
// `false` that leaves `attachments` empty — never a throw, for the same reason the zero-byte-key
// refusal above is a `return false` rather than an exception: this parse runs inside Fast DDS's
// own `deserialize()`/`on_data_available` frames.
//
// This belongs HERE rather than in core/tests/test_envelope.cpp, where the brief first pointed:
// core's own decoder, `fletcher::DeserializeEnvelope` (core/include/fletcher/core/envelope.hpp),
// is a DIFFERENT function that THROWS `std::invalid_argument` on every one of these same
// malformations instead of returning `false` — see EnvelopeTest.ThrowsOnTruncatedBuffer and
// neighbours in that file. `ParseEnvelopeBody`, the bool-returning decoder these four rows are
// actually about, is private to this component (`fastdds-pubsub-provider/src/internal/`);
// `fletcher-core` is a header-only base library this component depends ON, never the reverse
// (core/conanfile.py, fastdds-pubsub-provider/conanfile.py), so core's tests have no include path
// to it at all.
TEST(EnvelopeCodecTest, MalformedBodiesAreRefusedAndLeaveAttachmentsEmpty) {
    auto append_u32 = [](std::vector<uint8_t>& out, uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8) {
            out.push_back(static_cast<uint8_t>((value >> shift) & 0xFFu));
        }
    };

    struct Case {
        std::string name;
        std::vector<uint8_t> body;
        bool supply_owner;
    };
    std::vector<Case> cases;

    // att_count bomb: claims far more attachments than the buffer could name entries for at all
    // (the bound check is `att_count > (total - pos) / 8`, and here there is nothing left).
    {
        std::vector<uint8_t> body;
        append_u32(body, 0);           // row_len = 0
        append_u32(body, UINT32_MAX);  // att_count: bomb
        cases.push_back({"att_count bomb", body, true});
    }

    // key_len past the buffer: att_count is within bounds (1 <= 8/8), but the key it names cannot
    // fit in what remains.
    {
        std::vector<uint8_t> body;
        append_u32(body, 0);               // row_len = 0
        append_u32(body, 1);               // att_count = 1
        append_u32(body, 100);             // key_len = 100
        body.insert(body.end(), 4, 0x00);  // pads the buffer without supplying a key
        cases.push_back({"key_len past the buffer", body, true});
    }

    // blob_len past the buffer: the key fits (3 <= 7 remaining), the blob it names does not.
    {
        std::vector<uint8_t> body;
        append_u32(body, 0);  // row_len = 0
        append_u32(body, 1);  // att_count = 1
        append_u32(body, 3);  // key_len = 3
        body.insert(body.end(), {'a', 'b', 'c'});
        append_u32(body, 100);  // blob_len = 100, nothing follows
        cases.push_back({"blob_len past the buffer", body, true});
    }

    // blob_len > 0 with no owner: an otherwise well-formed body — the bound checks all pass —
    // refused only because the caller supplied no shared owner for the bytes the resulting Blob
    // would have to alias.
    {
        std::vector<uint8_t> body;
        append_u32(body, 0);  // row_len = 0
        append_u32(body, 1);  // att_count = 1
        append_u32(body, 3);  // key_len = 3
        body.insert(body.end(), {'a', 'b', 'c'});
        append_u32(body, 1);  // blob_len = 1
        body.push_back(0xAB);
        cases.push_back({"blob_len > 0 with no owner", body, false});
    }

    for (const Case& c : cases) {
        SCOPED_TRACE(c.name);
        std::shared_ptr<const std::vector<uint8_t>> owner =
            c.supply_owner ? std::make_shared<const std::vector<uint8_t>>(c.body) : nullptr;
        const uint8_t* data = c.body.data();
        const uint8_t* row = nullptr;
        uint32_t row_len = 0;
        // Pre-populated so a `false` return that left it untouched could not pass by accident:
        // `AttachmentsWireBuilder`'s constructor clears it, and the destructor clears it again if
        // `Finish()` never ran (fletcher/core/types.hpp) — both must actually have fired.
        fletcher::Attachments attachments;
        attachments.Set("stale", fletcher::Blob{std::vector<uint8_t>{0x01}});

        const bool accepted = fletcher::internal::ParseEnvelopeBody(owner, data, c.body.size(), row,
                                                                    row_len, attachments);

        EXPECT_FALSE(accepted);
        EXPECT_EQ(attachments.size(), static_cast<size_t>(0));
    }
}

// The same refusal reached through `deserialize()` itself — the frame ruling
// 2026-09-06 is actually about. A sample whose body carries such a key is
// rejected by the type, and the rejection is a `false` return rather than an
// exception crossing a Fast DDS frame.
TEST(FletcherSamplePubSubTypeTest, ASampleWithAZeroByteAttachmentKeyIsRejected) {
    FletcherSamplePubSubType type(kTestPayloadBytes);
    const std::vector<uint8_t> row = Row(16);

    std::string nul_bearing = "a";
    nul_bearing.push_back(static_cast<char>(0));
    nul_bearing += "b";

    // The sample is BUILT past the local refusal — `Attachments::Set` would
    // refuse this key — so the bytes are assembled by hand, which is exactly the
    // position a foreign or hostile producer is in.
    fletcher::Attachments clean;
    clean.Set("axb", fletcher::Blob{std::vector<uint8_t>{0x01}});
    Publishing publishing(row, clean);
    SerializedPayload_t payload(type.max_serialized_type_size);
    ASSERT_TRUE(type.serialize(&publishing.data, payload, kXcdr1));

    // Overwrite the key bytes in place: same length, one byte replaced by zero.
    // Any other byte of the sample is untouched, so nothing but the key differs.
    uint8_t* found = nullptr;
    for (uint32_t i = 0; i + 3 <= payload.length; ++i) {
        if (std::memcmp(payload.data + i, "axb", 3) == 0) {
            found = payload.data + i;
            break;
        }
    }
    ASSERT_NE(found, nullptr) << "the key was not found in the serialized sample";
    found[1] = 0;

    ReceivedData received;
    bool accepted = true;
    EXPECT_NO_THROW({ accepted = type.deserialize(payload, &received); })
        << "deserialize() let an exception escape into a Fast DDS frame";
    EXPECT_FALSE(accepted) << "a sample carrying a zero-byte attachment key was accepted";
}

}  // namespace
