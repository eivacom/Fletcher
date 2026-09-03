// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The DDS type this provider shipped before FletcherSample, kept alive here as the baseline arm of
// bench_pub_sub_type.cpp. Lifted verbatim from src/fast_dds_pubsub_provider.cpp at f779c2f, where
// it was a file-local class; the only edits are the rename from FletcherTopicType, the namespace,
// the include block, and taking internal::PublishData / internal::ReceivedData from
// src/internal/transport_data.hpp in place of the single combined struct that sat beside it in
// that file (same fields, split by direction).
//
// What it is, in one line: an unbounded, non-plain type carrying a self-framing envelope behind a
// hand-written CDR encapsulation. Unbounded means Fast DDS never offered it data-sharing, and
// non-plain means it never offered either end a loan.
#ifndef FLETCHER_FASTDDS_PUBSUB_PROVIDER_BENCHMARKS_LEGACY_FLETCHER_TOPIC_TYPE_HPP_
#define FLETCHER_FASTDDS_PUBSUB_PROVIDER_BENCHMARKS_LEGACY_FLETCHER_TOPIC_TYPE_HPP_

#include <cstdint>
#include <cstring>
#include <fastdds/dds/topic/TopicDataType.hpp>
#include <fastdds/rtps/common/SerializedPayload.hpp>
#include <fletcher/core/types.hpp>
#include <fletcher/core/write_buffer.hpp>
#include <memory>
#include <string>
#include <vector>

#include "transport_data.hpp"

namespace fletcher {
namespace benchmarks {

class LegacyFletcherTopicType : public eprosima::fastdds::dds::TopicDataType {
   public:
    explicit LegacyFletcherTopicType(uint32_t max_payload) {
        set_name("fletcher");
        max_serialized_type_size = 4 + 4 + max_payload;
        is_compute_key_provided = false;
    }

    bool serialize(
        const void* const data, eprosima::fastdds::rtps::SerializedPayload_t& payload,
        eprosima::fastdds::dds::DataRepresentationId_t /*data_representation*/) override {
        const auto* d = static_cast<const internal::PublishData*>(data);
        try {
            FixedWriteBuffer buf(payload.data, payload.max_size);

            // CDR little-endian encapsulation header.
            payload.encapsulation = CDR_LE;
            const uint8_t cdr_header[] = {0x00, 0x01, 0x00, 0x00};
            buf.Append(cdr_header, 4);

            // CDR octet-sequence: uint32 length placeholder.
            size_t seq_len_pos = buf.WriteLengthPlaceholder();
            size_t seq_start = buf.Position();

            // Envelope: [ROW_LEN:4][ROW_DATA][ATTACH_COUNT:4][attachments...]
            size_t row_len_pos = buf.WriteLengthPlaceholder();
            size_t row_start = buf.Position();

            // Row bytes written directly by the encoder.
            (*d->encoder)(buf);
            buf.PatchU32(row_len_pos, static_cast<uint32_t>(buf.Position() - row_start));

            // Attachments.
            const auto& att = *d->attachments;
            buf.AppendFixed(static_cast<uint32_t>(att.size()));
            for (const auto& [key, blob] : att) {
                buf.AppendFixed(static_cast<uint32_t>(key.size()));
                buf.Append(reinterpret_cast<const uint8_t*>(key.data()), key.size());
                uint32_t blob_len = blob ? static_cast<uint32_t>(blob->size()) : 0;
                buf.AppendFixed(blob_len);
                if (blob_len > 0) buf.Append(blob->data(), blob_len);
            }

            // Patch CDR sequence length.
            buf.PatchU32(seq_len_pos, static_cast<uint32_t>(buf.Position() - seq_start));

            payload.length = static_cast<uint32_t>(buf.Position());
            return true;
        } catch (...) {
            payload.length = 0;
            return false;
        }
    }

    bool deserialize(eprosima::fastdds::rtps::SerializedPayload_t& payload, void* data) override {
        auto* d = static_cast<internal::ReceivedData*>(data);
        if (payload.length < 8) return false;

        // Skip 4-byte CDR encapsulation, read 4-byte sequence length.
        uint32_t data_size = 0;
        std::memcpy(&data_size, payload.data + 4, sizeof(data_size));
        if (8 + data_size > payload.length) return false;

        const uint8_t* ptr = payload.data + 8;
        size_t total = data_size;
        if (total < 4) return false;

        uint32_t row_len;
        std::memcpy(&row_len, ptr, 4);
        if (4 + row_len > total) return false;

        // Deliver raw row bytes — no decoding, no Arrow dependency.
        d->decoded_row.assign(ptr + 4, ptr + 4 + row_len);

        // Parse attachments in-place.
        d->decoded_attachments.clear();
        size_t pos = 4 + row_len;
        if (pos + 4 <= total) {
            uint32_t att_count;
            std::memcpy(&att_count, ptr + pos, 4);
            pos += 4;
            for (uint32_t i = 0; i < att_count; ++i) {
                if (pos + 4 > total) return false;
                uint32_t key_len;
                std::memcpy(&key_len, ptr + pos, 4);
                pos += 4;
                if (pos + key_len > total) return false;
                std::string key(reinterpret_cast<const char*>(ptr + pos), key_len);
                pos += key_len;
                if (pos + 4 > total) return false;
                uint32_t blob_len;
                std::memcpy(&blob_len, ptr + pos, 4);
                pos += 4;
                if (pos + blob_len > total) return false;
                auto blob =
                    std::make_shared<const std::vector<uint8_t>>(ptr + pos, ptr + pos + blob_len);
                pos += blob_len;
                d->decoded_attachments[std::move(key)] = std::move(blob);
            }
        }
        return true;
    }

    uint32_t calculate_serialized_size(
        const void* const /*data*/,
        eprosima::fastdds::dds::DataRepresentationId_t /*data_representation*/) override {
        return static_cast<uint32_t>(max_serialized_type_size);
    }

    void* create_data() override { return new internal::ReceivedData(); }

    void delete_data(void* data) override { delete static_cast<internal::ReceivedData*>(data); }

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
};

}  // namespace benchmarks
}  // namespace fletcher

#endif  // FLETCHER_FASTDDS_PUBSUB_PROVIDER_BENCHMARKS_LEGACY_FLETCHER_TOPIC_TYPE_HPP_
