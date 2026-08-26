// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// DDS type for the companion __schema channel: a length-prefixed blob of Arrow IPC schema bytes.
#ifndef FLETCHER_FASTDDS_PUBSUB_PROVIDER_INTERNAL_RAW_BYTES_PUB_SUB_TYPE_HPP_
#define FLETCHER_FASTDDS_PUBSUB_PROVIDER_INTERNAL_RAW_BYTES_PUB_SUB_TYPE_HPP_

#include <fastcdr/Cdr.h>
#include <fastcdr/FastBuffer.h>

#include <algorithm>
#include <cstdint>
#include <exception>
#include <fastcdr/CdrSizeCalculator.hpp>
#include <fastdds/dds/log/Log.hpp>
#include <fastdds/dds/topic/TopicDataType.hpp>
#include <fastdds/rtps/common/SerializedPayload.hpp>
#include <fletcher/pubsub/payload_bound.hpp>

#include "transport_data.hpp"

namespace fletcher {
namespace internal {

class RawBytesPubSubType : public eprosima::fastdds::dds::TopicDataType {
   public:
    explicit RawBytesPubSubType(uint32_t max_payload) {
        set_name(kSchemaTypeName);
        // 64-bit and saturated: max_schema_bytes is uncapped, and 32-bit `4 + max_payload` wraps.
        uint64_t type_size = uint64_t{4} + max_payload;  // sequence length, then the octets
        type_size += eprosima::fastcdr::Cdr::alignment(static_cast<size_t>(type_size), 4);
        type_size += eprosima::fastdds::rtps::SerializedPayload_t::representation_header_size;
        max_serialized_type_size = static_cast<uint32_t>(std::min<uint64_t>(type_size, UINT32_MAX));
    }

    // Bounded is all data-sharing asks for; the QoS pins the pool to the one sample this holds.
    bool is_bounded() const override { return true; }

    // Never plain: the std::vector behind a sequence is not its own CDR layout.
    bool is_plain(
        eprosima::fastdds::dds::DataRepresentationId_t /*data_representation*/) const override {
        return false;
    }

    bool serialize(
        const void* const data, eprosima::fastdds::rtps::SerializedPayload_t& payload,
        eprosima::fastdds::dds::DataRepresentationId_t /*data_representation*/) override {
        // Always XCDR1: this channel's QoS is Fletcher-fixed and offers nothing else.
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
        } catch (const std::exception& e) {
            // A schema past max_schema_bytes lands here, and only here is its size known.
            payload.length = 0;
            EPROSIMA_LOG_ERROR(FLETCHER_SCHEMA, "schema serialize failed ("
                                                    << d->data.size() << " bytes, channel bound "
                                                    << max_serialized_type_size
                                                    << "): " << e.what());
            return false;
        } catch (...) {
            payload.length = 0;
            EPROSIMA_LOG_ERROR(FLETCHER_SCHEMA, "schema serialize failed: non-std exception");
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
        } catch (const std::exception& e) {
            // SchemaListener retries the next sample, so the reason exists only here.
            EPROSIMA_LOG_ERROR(FLETCHER_SCHEMA, "schema deserialize failed ("
                                                    << payload.length << " bytes): " << e.what());
            return false;
        } catch (...) {
            EPROSIMA_LOG_ERROR(FLETCHER_SCHEMA, "schema deserialize failed: non-std exception");
            return false;
        }
    }

    uint32_t calculate_serialized_size(
        const void* const data,
        eprosima::fastdds::dds::DataRepresentationId_t /*data_representation*/) override {
        // Through fastcdr rather than the type's maximum; nothing on the current path calls it.
        try {
            const auto* d = static_cast<const RawBytes*>(data);
            eprosima::fastcdr::CdrSizeCalculator calculator(eprosima::fastcdr::CdrVersion::XCDRv1);
            size_t current_alignment{0};
            return static_cast<uint32_t>(
                       calculator.calculate_serialized_size(d->data, current_alignment)) +
                   eprosima::fastdds::rtps::SerializedPayload_t::representation_header_size;
        } catch (const std::exception& e) {
            // The type maximum, not 0: understating would hand out a buffer nothing fits in.
            EPROSIMA_LOG_ERROR(FLETCHER_SCHEMA, "schema size calculation failed: " << e.what());
            return max_serialized_type_size;
        }
    }

    void* create_data() override { return new RawBytes(); }
    void delete_data(void* data) override { delete static_cast<RawBytes*>(data); }

    // Keyless; is_compute_key_provided stays false, as on the data channel.
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
