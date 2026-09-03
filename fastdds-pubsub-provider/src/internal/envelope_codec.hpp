// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The contents of a sample's body: the encoded row and its attachments. Shared by the
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
              "big-endian target needs byte swaps in EncodeEnvelopeBody and ParseEnvelopeBody");

// Writes [ROW_LEN:4][ROW_DATA][ATTACH_COUNT:4][attachments...]; throws if the buffer is too small.
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

// Reverse of EncodeEnvelopeBody. `row` views `ptr`; attachment blobs are copied.
inline bool ParseEnvelopeBody(const uint8_t* ptr, size_t total, const uint8_t*& row,
                              uint32_t& row_len, Attachments& attachments) {
    if (total < 4) return false;
    std::memcpy(&row_len, ptr, 4);
    // Checks subtract, never add: `pos + len` can wrap on a 32-bit size_t. pos <= total throughout.
    if (row_len > total - 4) return false;
    row = ptr + 4;

    attachments.clear();
    size_t pos = 4 + row_len;
    if (total - pos >= 4) {
        uint32_t att_count;
        std::memcpy(&att_count, ptr + pos, 4);
        pos += 4;
        // Bounds the loop before it allocates: a corrupt count could claim 2^32 attachments.
        if (att_count > (total - pos) / 8) return false;
        for (uint32_t i = 0; i < att_count; ++i) {
            if (total - pos < 4) return false;
            uint32_t key_len;
            std::memcpy(&key_len, ptr + pos, 4);
            pos += 4;
            if (key_len > total - pos) return false;
            std::string key(reinterpret_cast<const char*>(ptr + pos), key_len);
            pos += key_len;
            if (total - pos < 4) return false;
            uint32_t blob_len;
            std::memcpy(&blob_len, ptr + pos, 4);
            pos += 4;
            if (blob_len > total - pos) return false;
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
