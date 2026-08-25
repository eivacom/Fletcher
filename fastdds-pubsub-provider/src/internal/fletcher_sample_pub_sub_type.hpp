// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The data-channel DDS type over FletcherSample. Serialises a row into the DDS payload for the
// non-loaned publish path, and reports the boundedness and plainness Fast DDS gates data-sharing
// and loans on — see internal/fletcher_sample.hpp for why those two are true.
#ifndef FLETCHER_FASTDDS_PUBSUB_PROVIDER_INTERNAL_FLETCHER_SAMPLE_PUB_SUB_TYPE_HPP_
#define FLETCHER_FASTDDS_PUBSUB_PROVIDER_INTERNAL_FLETCHER_SAMPLE_PUB_SUB_TYPE_HPP_

#include <fastcdr/Cdr.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fastcdr/CdrEncoding.hpp>
#include <fastdds/dds/topic/TopicDataType.hpp>
#include <fastdds/rtps/common/SerializedPayload.hpp>
#include <fletcher/core/write_buffer.hpp>
#include <new>
#include <string>

#include "envelope_codec.hpp"
#include "fletcher_sample.hpp"
#include "transport_data.hpp"

namespace fletcher {
namespace internal {

// A DDS-XTypes representation id is the encoding algorithm flag with the endianness bit set, so it
// is spelled with fastcdr's own constants rather than as a literal: PLAIN_CDR | LITTLE_ENDIANNESS
// is 0x01, which is exactly Fast DDS's `CDR_LE`, and PLAIN_CDR2 | LITTLE_ENDIANNESS is 0x07. Both
// are header-only enums, so this costs nothing at run time — unlike routing the bytes through a
// Cdr. See README "Measured decisions".
constexpr uint8_t RepresentationId(eprosima::fastcdr::EncodingAlgorithmFlag encoding) {
    return static_cast<uint8_t>(encoding) |
           static_cast<uint8_t>(eprosima::fastcdr::Cdr::LITTLE_ENDIANNESS);
}

static_assert(RepresentationId(eprosima::fastcdr::EncodingAlgorithmFlag::PLAIN_CDR) == CDR_LE);

template <uint32_t N>
class FletcherSamplePubSubType : public eprosima::fastdds::dds::TopicDataType {
   public:
    // The alias fastddsgen puts on every generated type support.
    using type = FletcherSample<N>;

    FletcherSamplePubSubType() {
        // N rides in the type name so that two providers on different bounds fail to match at
        // discovery instead of exchanging samples one of them cannot hold. A plain type's size is
        // part of its identity; this is the nearest thing to the type consistency an IDL type would
        // get for free.
        set_name("fletcher_" + std::to_string(N));
        // Sized the way a generated type support sizes itself (DeliveryMechanismsPubSubTypes.cxx):
        // the type, padded for possible submessage alignment, plus the encapsulation. The
        // padding term is zero here because sizeof(type) is a multiple of 4, which
        // fletcher_sample.hpp's static_assert on N guarantees — it is kept because it is fastcdr
        // that decides that, not this file.
        uint32_t type_size = static_cast<uint32_t>(sizeof(type));
        type_size += static_cast<uint32_t>(eprosima::fastcdr::Cdr::alignment(type_size, 4));
        max_serialized_type_size =
            type_size + eprosima::fastdds::rtps::SerializedPayload_t::representation_header_size;
        is_compute_key_provided = false;
    }

    // Bounded unlocks data-sharing; plain additionally unlocks loans at both ends. Both are
    // properties of FletcherSample<N> rather than claims made here — it is a fixed-size,
    // padding-free, trivially copyable struct, which is what its static_asserts pin down. Fast DDS
    // only needs to know they are *allowed*: whether data-sharing engages is its own decision
    // (DataSharingQosPolicy stays at AUTO, which is why no QoS here ever mentions it), and whether
    // the publish side loans is FastDDSProviderOptions::loan_publish. The cost of being bounded is
    // PREALLOCATED payload pools, which N sizes.
    bool is_bounded() const override { return true; }

    // Dispatched per representation and answered by a constexpr layout check, which is the shape
    // fastddsgen emits (DeliveryMechanismsPubSubTypes.hpp). Identical for both XCDR versions here —
    // a FINAL struct of a uint32 and an octet array carries no DHEADER and no optional members, so
    // nothing differs between v1 and v2.
    bool is_plain(
        eprosima::fastdds::dds::DataRepresentationId_t data_representation) const override {
        if (data_representation ==
            eprosima::fastdds::dds::DataRepresentationId_t::XCDR2_DATA_REPRESENTATION) {
            return is_plain_xcdrv2_impl();
        }
        return is_plain_xcdrv1_impl();
    }

    // Lets Fast DDS hand out a loan under CONSTRUCTED_LOAN_INITIALIZATION
    // (DataWriterImpl.cpp). Nothing here asks for that kind — LoanableSampleWriter
    // overwrites `length` and the body it uses — but the type supports it either way.
    bool construct_sample(void* memory) const override {
        new (memory) type();
        return true;
    }

    bool serialize(const void* const data, eprosima::fastdds::rtps::SerializedPayload_t& payload,
                   eprosima::fastdds::dds::DataRepresentationId_t data_representation) override {
        const auto* d = static_cast<const PublishData*>(data);
        constexpr uint32_t kHeader =
            eprosima::fastdds::rtps::SerializedPayload_t::representation_header_size;

        // The non-loaned publish path. It writes the same layout the loaned path does — a
        // FletcherSample<N> behind a CDR encapsulation header — but **truncated after the bytes in
        // use**: `payload.length` stops at the end of the body instead of covering the whole
        // fixed-size `body` array. Fast DDS never reads past `length` (nor does this type's own
        // reader), so the tail padding a plain type would nominally carry is simply not sent, which
        // is what keeps a small row small on the wire when loan_publish is off.
        if (payload.max_size < kHeader + kEnvelopeLengthPrefix) {
            payload.length = 0;
            return false;
        }
        // Bound the write by the buffer Fast DDS actually gave us rather than by the size
        // max_serialized_type_size implies it has, and never past what a reader's FletcherSample<N>
        // can hold.
        const uint32_t kFraming = kHeader + static_cast<uint32_t>(kEnvelopeLengthPrefix);
        const uint32_t capacity = std::min<uint32_t>(N, payload.max_size - kFraming);

        try {
            // The body goes down first: the length that precedes it is not known until it is
            // written, and it is exactly FletcherSample<N>::length in position and width.
            FixedWriteBuffer buf(payload.data + kHeader + kEnvelopeLengthPrefix, capacity);
            EncodeEnvelopeBody(buf, *d->encoder, *d->attachments);
            const auto body_size = static_cast<uint32_t>(buf.Position());
            const uint32_t length =
                kHeader + static_cast<uint32_t>(kEnvelopeLengthPrefix) + body_size;

            // Then the encapsulation, placed here rather than driven through a Cdr, but out of
            // fastcdr's own constants. Byte 1 is the representation id for the version asked for,
            // which is the selection a generated type support makes from this same argument
            // (DeliveryMechanismsPubSubTypes.cxx). Byte 3 is the low octet of the DDS-XTypes
            // options field, carrying how far short of a 4-byte boundary the payload ends —
            // Cdr::set_dds_cdr_options computes it with exactly this call.
            //
            // The body is the same under both versions: a FINAL struct of a uint32 and an octet
            // array carries no DHEADER under PLAIN_CDR2 and needs no extra alignment. That these
            // eight bytes are byte for byte what fastcdr would have written is held by
            // FletcherWireFormat.FastCdrReproducesTheBytesExactly and by bench_pub_sub_type's
            // fastcdr arm, which compares both representations.
            const uint8_t representation_id =
                data_representation ==
                        eprosima::fastdds::dds::DataRepresentationId_t::XCDR2_DATA_REPRESENTATION
                    ? RepresentationId(eprosima::fastcdr::EncodingAlgorithmFlag::PLAIN_CDR2)
                    : RepresentationId(eprosima::fastcdr::EncodingAlgorithmFlag::PLAIN_CDR);
            const uint8_t encapsulation[4] = {
                0x00, representation_id, 0x00,
                static_cast<uint8_t>(eprosima::fastcdr::Cdr::alignment(length, 4))};
            std::memcpy(payload.data, encapsulation, sizeof(encapsulation));
            std::memcpy(payload.data + kHeader, &body_size, sizeof(body_size));

            payload.encapsulation = CDR_LE;
            payload.length = length;
            return true;
        } catch (...) {
            payload.length = 0;
            return false;
        }
    }

    // Only reached by a reader that asks for deserialised samples. Fletcher's own does not — it
    // reads FletcherSample through a loan — but TopicDataType requires this, and a peer using
    // take_next_sample on this topic gets the same rows out of it.
    bool deserialize(eprosima::fastdds::rtps::SerializedPayload_t& payload, void* data) override {
        auto* d = static_cast<ReceivedData*>(data);
        const uint32_t header =
            eprosima::fastdds::rtps::SerializedPayload_t::representation_header_size;
        if (payload.length < header + kEnvelopeLengthPrefix) return false;

        // Read the encapsulation rather than skipping past it. The envelope's own length fields are
        // host-order little-endian (envelope_codec.hpp), so a big-endian or parameter-list payload
        // is not something this type can parse and has to be refused rather than misread. A
        // generated type reaches the same place through Cdr::read_encapsulation, and likewise
        // reports on the payload what it found (DeliveryMechanismsPubSubTypes.cxx).
        const uint8_t representation_id = payload.data[1];
        if (representation_id !=
                RepresentationId(eprosima::fastcdr::EncodingAlgorithmFlag::PLAIN_CDR) &&
            representation_id !=
                RepresentationId(eprosima::fastcdr::EncodingAlgorithmFlag::PLAIN_CDR2)) {
            return false;
        }
        payload.encapsulation = CDR_LE;

        // Past the encapsulation header sits FletcherSample: its length, then that many body bytes.
        // Bounded by what actually arrived, since a serialised sample stops after the bytes in use.
        uint32_t length = 0;
        std::memcpy(&length, payload.data + header, sizeof(length));
        if (length > payload.length - header - kEnvelopeLengthPrefix) return false;

        const uint8_t* row = nullptr;
        uint32_t row_len = 0;
        if (!ParseEnvelopeBody(payload.data + header + kEnvelopeLengthPrefix, length, row, row_len,
                               d->decoded_attachments)) {
            return false;
        }

        // Deliver raw row bytes — no decoding, no Arrow dependency.
        d->decoded_row.assign(row, row + row_len);
        return true;
    }

    uint32_t calculate_serialized_size(
        const void* const /*data*/,
        eprosima::fastdds::dds::DataRepresentationId_t /*data_representation*/) override {
        // Dead under any PREALLOCATED* memory policy, which a bounded type always gets: Fast DDS
        // takes pool_config_.payload_initial_size instead (DataWriterImpl.cpp). Still has to
        // be honest, because a caller may set a different endpoint().history_memory_policy.
        return static_cast<uint32_t>(max_serialized_type_size);
    }

    // A received sample, not a PublishData: the only caller in Fast DDS 3.4 is the reader's
    // SampleLoanManager, which makes these to hand to deserialize() — and only for a non-plain
    // type, so for this one they are never called at all (SampleLoanManager.hpp).
    void* create_data() override { return new ReceivedData(); }

    void delete_data(void* data) override { delete static_cast<ReceivedData*>(data); }

    bool compute_key(eprosima::fastdds::rtps::SerializedPayload_t& /*payload*/,
                     eprosima::fastdds::rtps::InstanceHandle_t& /*handle*/,
                     bool /*force_md5*/) override {
        return false;
    }
    bool compute_key(const void* const /*data*/,
                     eprosima::fastdds::rtps::InstanceHandle_t& /*handle*/,
                     bool /*force_md5*/) override {
        return false;
    }

   private:
    static constexpr bool is_plain_xcdrv1_impl() { return FletcherSampleIsPlain<N>(); }
    static constexpr bool is_plain_xcdrv2_impl() { return FletcherSampleIsPlain<N>(); }
};

}  // namespace internal
}  // namespace fletcher

#endif  // FLETCHER_FASTDDS_PUBSUB_PROVIDER_INTERNAL_FLETCHER_SAMPLE_PUB_SUB_TYPE_HPP_
