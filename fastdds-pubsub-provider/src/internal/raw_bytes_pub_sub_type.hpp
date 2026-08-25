// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// DDS type for the companion __schema channel: a length-prefixed blob of Arrow IPC schema bytes.
#ifndef FLETCHER_FASTDDS_PUBSUB_PROVIDER_INTERNAL_RAW_BYTES_PUB_SUB_TYPE_HPP_
#define FLETCHER_FASTDDS_PUBSUB_PROVIDER_INTERNAL_RAW_BYTES_PUB_SUB_TYPE_HPP_

#include <fastcdr/Cdr.h>
#include <fastcdr/FastBuffer.h>

#include <cstdint>
#include <fastdds/dds/topic/TopicDataType.hpp>
#include <fastdds/rtps/common/SerializedPayload.hpp>

#include "transport_data.hpp"

namespace fletcher {
namespace internal {

class RawBytesPubSubType : public eprosima::fastdds::dds::TopicDataType {
   public:
    explicit RawBytesPubSubType(uint32_t max_payload) {
        set_name("SchemaBytes");
        max_serialized_type_size = 4 + 4 + max_payload;
        is_compute_key_provided = false;
    }

    bool serialize(
        const void* const data, eprosima::fastdds::rtps::SerializedPayload_t& payload,
        eprosima::fastdds::dds::DataRepresentationId_t /*data_representation*/) override {
        const auto* d = static_cast<const RawBytes*>(data);

        // Nothing here is Fletcher-specific — the sample *is* a sequence<octet> — so fastcdr does
        // all of it, writing straight into the DDS buffer. Same shape fastddsgen emits.
        eprosima::fastcdr::FastBuffer fastbuffer(reinterpret_cast<char*>(payload.data),
                                                 payload.max_size);
        eprosima::fastcdr::Cdr ser(fastbuffer, eprosima::fastcdr::Cdr::DEFAULT_ENDIAN,
                                   eprosima::fastcdr::CdrVersion::XCDRv1);
        payload.encapsulation =
            ser.endianness() == eprosima::fastcdr::Cdr::BIG_ENDIANNESS ? CDR_BE : CDR_LE;
        ser.set_encoding_flag(eprosima::fastcdr::EncodingAlgorithmFlag::PLAIN_CDR);

        try {
            ser.serialize_encapsulation();
            ser << d->data;
            ser.set_dds_cdr_options({0, 0});
            payload.length = static_cast<uint32_t>(ser.get_serialized_data_length());
            return true;
        } catch (...) {
            payload.length = 0;
            return false;
        }
    }

    bool deserialize(eprosima::fastdds::rtps::SerializedPayload_t& payload, void* data) override {
        auto* d = static_cast<RawBytes*>(data);

        eprosima::fastcdr::FastBuffer fastbuffer(reinterpret_cast<char*>(payload.data),
                                                 payload.length);
        eprosima::fastcdr::Cdr des(fastbuffer, eprosima::fastcdr::Cdr::DEFAULT_ENDIAN,
                                   eprosima::fastcdr::CdrVersion::XCDRv1);
        try {
            des.read_encapsulation();
            des >> d->data;
            return true;
        } catch (...) {
            return false;
        }
    }

    uint32_t calculate_serialized_size(
        const void* const /*data*/,
        eprosima::fastdds::dds::DataRepresentationId_t /*data_representation*/) override {
        // The real size is in hand here and would cost nothing to return, but it would not change
        // what gets allocated: the pool floors every node at max_serialized_type_size regardless.
        // See FletcherSamplePubSubType::calculate_serialized_size.
        return static_cast<uint32_t>(max_serialized_type_size);
    }

    void* create_data() override { return new RawBytes(); }
    void delete_data(void* data) override { delete static_cast<RawBytes*>(data); }

    bool compute_key(eprosima::fastdds::rtps::SerializedPayload_t& /*payload*/,
                     eprosima::fastdds::rtps::InstanceHandle_t& /*handle*/, bool) override {
        return false;
    }
    bool compute_key(const void* const /*data*/,
                     eprosima::fastdds::rtps::InstanceHandle_t& /*handle*/, bool) override {
        return false;
    }
};

}  // namespace internal
}  // namespace fletcher

#endif  // FLETCHER_FASTDDS_PUBSUB_PROVIDER_INTERNAL_RAW_BYTES_PUB_SUB_TYPE_HPP_
