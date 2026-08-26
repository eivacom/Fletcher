// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The data-channel DDS type over the sample layout. Serialises a row into the DDS payload for the
// non-loaned publish path, and reports the boundedness and plainness Fast DDS gates data-sharing
// and loans on — see internal/fletcher_sample.hpp for why those two are true.
#ifndef FLETCHER_FASTDDS_PUBSUB_PROVIDER_INTERNAL_FLETCHER_SAMPLE_PUB_SUB_TYPE_HPP_
#define FLETCHER_FASTDDS_PUBSUB_PROVIDER_INTERNAL_FLETCHER_SAMPLE_PUB_SUB_TYPE_HPP_

#include <fastcdr/Cdr.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fastcdr/CdrEncoding.hpp>
#include <fastdds/dds/log/Log.hpp>
#include <fastdds/dds/topic/TopicDataType.hpp>
#include <fastdds/rtps/common/SerializedPayload.hpp>
#include <fletcher/core/write_buffer.hpp>
#include <fletcher/pubsub/payload_bound.hpp>
#include <string>

#include "envelope_codec.hpp"
#include "fletcher_sample.hpp"
#include "transport_data.hpp"

namespace fletcher {
namespace internal {

// A representation id is the encoding flag with the endianness bit set; both enums fold here.
constexpr uint8_t RepresentationId(eprosima::fastcdr::EncodingAlgorithmFlag encoding) {
    return static_cast<uint8_t>(encoding) |
           static_cast<uint8_t>(eprosima::fastcdr::Cdr::LITTLE_ENDIANNESS);
}

static_assert(RepresentationId(eprosima::fastcdr::EncodingAlgorithmFlag::PLAIN_CDR) == CDR_LE);

class FletcherSamplePubSubType : public eprosima::fastdds::dds::TopicDataType {
   public:
    // Precondition: the provider constructor rejects an unusable bound before creating anything.
    explicit FletcherSamplePubSubType(uint32_t payload_bytes) : payload_bytes_(payload_bytes) {
        assert(IsPayloadBound(payload_bytes));
        set_name(FletcherTypeName(payload_bytes));
        // A generated support also adds Cdr::alignment(type_size, 4), always zero for this layout.
        max_serialized_type_size =
            SampleSize(payload_bytes) +
            eprosima::fastdds::rtps::SerializedPayload_t::representation_header_size;
    }

    // Bounded permits data-sharing, plain permits loans; Fast DDS decides whether either engages.
    bool is_bounded() const override { return true; }

    // Plain for any bound IsPayloadBound admits, and identical under both XCDR versions.
    bool is_plain(
        eprosima::fastdds::dds::DataRepresentationId_t /*data_representation*/) const override {
        return true;
    }

    // A zeroed length is an empty sample and bounds every read, so the body needs no memset.
    bool construct_sample(void* memory) const override {
        WriteSampleLength(static_cast<uint8_t*>(memory), 0);
        return true;
    }

    bool serialize(const void* const data, eprosima::fastdds::rtps::SerializedPayload_t& payload,
                   eprosima::fastdds::dds::DataRepresentationId_t data_representation) override {
        const auto* d = static_cast<const PublishData*>(data);
        constexpr uint32_t kHeader =
            eprosima::fastdds::rtps::SerializedPayload_t::representation_header_size;

        // Truncated after the bytes in use, so a small row stays small on the wire.
        if (payload.max_size < kHeader + kSampleLengthPrefix) {
            payload.length = 0;
            return false;
        }
        // Bounded by the buffer Fast DDS actually gave us, and by what a reader's sample holds.
        constexpr uint32_t kFraming = kHeader + kSampleLengthPrefix;
        const uint32_t capacity = std::min<uint32_t>(payload_bytes_, payload.max_size - kFraming);

        try {
            // Body first: the length that precedes it is not known until it is written.
            FixedWriteBuffer buf(SampleBody(payload.data + kHeader), capacity);
            EncodeEnvelopeBody(buf, *d->encoder, *d->attachments);
            const auto body_size = static_cast<uint32_t>(buf.Position());
            const uint32_t length = kFraming + body_size;

            // Byte 1 is the representation id; byte 3 the shortfall from a 4-byte boundary.
            const uint8_t representation_id =
                data_representation ==
                        eprosima::fastdds::dds::DataRepresentationId_t::XCDR2_DATA_REPRESENTATION
                    ? RepresentationId(eprosima::fastcdr::EncodingAlgorithmFlag::PLAIN_CDR2)
                    : RepresentationId(eprosima::fastcdr::EncodingAlgorithmFlag::PLAIN_CDR);
            const uint8_t encapsulation[4] = {
                0x00, representation_id, 0x00,
                static_cast<uint8_t>(eprosima::fastcdr::Cdr::alignment(length, 4))};
            std::memcpy(payload.data, encapsulation, sizeof(encapsulation));
            WriteSampleLength(payload.data + kHeader, body_size);

            payload.encapsulation = CDR_LE;
            payload.length = length;
            return true;
        } catch (const std::exception& e) {
            // A false return reaches the caller as a code that cannot distinguish the cause.
            payload.length = 0;
            EPROSIMA_LOG_ERROR(FLETCHER_PUBLICATION,
                               "serialize failed for " << get_name() << ": " << e.what());
            return false;
        } catch (...) {
            payload.length = 0;
            EPROSIMA_LOG_ERROR(FLETCHER_PUBLICATION,
                               "serialize failed for " << get_name() << ": non-std exception");
            return false;
        }
    }

    // For a peer using take_next_sample; Fletcher's own reader goes through a loan.
    bool deserialize(eprosima::fastdds::rtps::SerializedPayload_t& payload, void* data) override {
        auto* d = static_cast<ReceivedData*>(data);
        const uint32_t header =
            eprosima::fastdds::rtps::SerializedPayload_t::representation_header_size;
        if (payload.length < header + kSampleLengthPrefix) return false;

        // Host-order lengths, so a big-endian or parameter-list payload has to be refused.
        const uint8_t representation_id = payload.data[1];
        if (representation_id !=
                RepresentationId(eprosima::fastcdr::EncodingAlgorithmFlag::PLAIN_CDR) &&
            representation_id !=
                RepresentationId(eprosima::fastcdr::EncodingAlgorithmFlag::PLAIN_CDR2)) {
            return false;
        }
        payload.encapsulation = CDR_LE;

        // Bounded by what arrived, since a serialised sample stops after the bytes in use.
        const uint32_t length = ReadSampleLength(payload.data + header);
        if (length > payload.length - header - kSampleLengthPrefix) return false;

        const uint8_t* row = nullptr;
        uint32_t row_len = 0;
        if (!ParseEnvelopeBody(SampleBody(payload.data + header), length, row, row_len,
                               d->decoded_attachments)) {
            return false;
        }

        // Deliver raw row bytes — no decoding, no Arrow dependency.
        d->decoded_row.assign(row, row + row_len);
        return true;
    }

    // Unused while the pool is PREALLOCATED*, which a caller's memory policy can change.
    uint32_t calculate_serialized_size(
        const void* const /*data*/,
        eprosima::fastdds::dds::DataRepresentationId_t /*data_representation*/) override {
        return static_cast<uint32_t>(max_serialized_type_size);
    }

    // Never reached: SampleLoanManager only calls these for a non-plain type.
    void* create_data() override { return new ReceivedData(); }

    void delete_data(void* data) override { delete static_cast<ReceivedData*>(data); }

    // Keyless; is_compute_key_provided stays false, gating check_allocation_consistency.
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
    uint32_t payload_bytes_;
};

}  // namespace internal
}  // namespace fletcher

#endif  // FLETCHER_FASTDDS_PUBSUB_PROVIDER_INTERNAL_FLETCHER_SAMPLE_PUB_SUB_TYPE_HPP_
