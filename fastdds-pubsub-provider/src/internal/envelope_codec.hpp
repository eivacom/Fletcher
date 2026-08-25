// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The contents of FletcherSample::body: the encoded row and its attachments. Shared by the
// serialised and the loaned publish paths, which differ only in who writes the length.
#ifndef FLETCHER_FASTDDS_PUBSUB_PROVIDER_INTERNAL_ENVELOPE_CODEC_HPP_
#define FLETCHER_FASTDDS_PUBSUB_PROVIDER_INTERNAL_ENVELOPE_CODEC_HPP_

#include <bit>
#include <cstdint>
#include <cstring>
#include <fletcher/core/types.hpp>
#include <fletcher/core/write_buffer.hpp>
#include <fletcher/pubsub/provider.hpp>
#include <string>
#include <utility>

namespace fletcher {
namespace internal {

// Every length inside the envelope is a raw host-order uint32 — deliberate, it keeps the codec to
// a memcpy on the hot path, but it means the body is not self-describing the way the CDR
// encapsulation wrapped around it is. That header advertises CDR_LE or CDR_BE; these fields would
// silently disagree with it between hosts of different endianness. Fletcher is little-endian only.
// Fail the build rather than the wire if that stops being true.
static_assert(std::endian::native == std::endian::little,
              "Fletcher's envelope stores lengths in host order and assumes little-endian; a "
              "big-endian target needs byte swaps in EncodeEnvelopeBody and ParseEnvelope");

// Width of FletcherSample::length, which prefixes the body in both layouts — as the struct member
// when the sample is loaned, and as the field fastcdr writes at the same offset when it is
// serialised.
constexpr size_t kEnvelopeLengthPrefix = 4;

// Writes [ROW_LEN:4][ROW_DATA][ATTACH_COUNT:4][attachments...] into FletcherSample::body. Nothing
// here frames itself: the byte count goes in FletcherSample::length, which the loaned path assigns
// as a struct member and the serialised path hands to fastcdr. Throws (FixedWriteBuffer) if the
// target buffer cannot hold the result.
inline void EncodeEnvelopeBody(WriteBuffer& buf, const PubSubProvider::RowEncoder& encoder,
                               const Attachments& attachments) {
    size_t row_len_pos = buf.WriteLengthPlaceholder();
    size_t row_start = buf.Position();
    encoder(buf);
    buf.PatchU32(row_len_pos, static_cast<uint32_t>(buf.Position() - row_start));

    buf.AppendFixed(static_cast<uint32_t>(attachments.size()));
    for (const auto& [key, blob] : attachments) {
        buf.AppendFixed(static_cast<uint32_t>(key.size()));
        buf.Append(reinterpret_cast<const uint8_t*>(key.data()), key.size());
        uint32_t blob_len = blob ? static_cast<uint32_t>(blob->size()) : 0;
        buf.AppendFixed(blob_len);
        if (blob_len > 0) buf.Append(blob->data(), blob_len);
    }
}

// Reverse of EncodeEnvelopeBody. `ptr` points at FletcherSample::body and `total` is
// FletcherSample::length — the real byte count, so nothing here reads past what the writer wrote.
// Callers must have clamped `total` to the size of the body they hold. `row` is returned as a view
// into `ptr` (no copy); attachment blobs are copied, as Attachments owns them.
inline bool ParseEnvelopeBody(const uint8_t* ptr, size_t total, const uint8_t*& row,
                              uint32_t& row_len, Attachments& attachments) {
    if (total < 4) return false;
    std::memcpy(&row_len, ptr, 4);
    if (4 + static_cast<size_t>(row_len) > total) return false;
    row = ptr + 4;

    attachments.clear();
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
            attachments.insert_or_assign(std::move(key), std::move(blob));
        }
    }
    return true;
}

}  // namespace internal
}  // namespace fletcher

#endif  // FLETCHER_FASTDDS_PUBSUB_PROVIDER_INTERNAL_ENVELOPE_CODEC_HPP_
