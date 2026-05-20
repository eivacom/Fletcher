// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#ifndef FLETCHER_INCLUDE_CORE_ENVELOPE_HPP_
#define FLETCHER_INCLUDE_CORE_ENVELOPE_HPP_

#include "fletcher/core/types.hpp"

#include <bit>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace fletcher {

static_assert(std::endian::native == std::endian::little,
              "Envelope wire format assumes little-endian host");

// An encoded row bundled with optional attachments.
struct Envelope {
    EncodedRow   row;
    Attachments  attachments;
};

// ---------------------------------------------------------------------------
// Envelope wire format (little-endian):
//
//   [ROW_LEN       : 4 bytes]  uint32_t
//   [ROW_DATA      : ROW_LEN bytes]
//   [ATTACH_COUNT  : 4 bytes]  uint32_t
//   For each attachment:
//     [KEY_LEN     : 4 bytes]  uint32_t
//     [KEY          : KEY_LEN bytes]  UTF-8
//     [BLOB_LEN    : 4 bytes]  uint32_t
//     [BLOB         : BLOB_LEN bytes]
// ---------------------------------------------------------------------------

inline std::vector<uint8_t> SerializeEnvelope(const Envelope& env) {
    // Pre-compute total size.
    size_t total = 4 + env.row.size() + 4;
    for (const auto& [key, blob] : env.attachments)
        total += 4 + key.size() + 4 + (blob ? blob->size() : 0);

    std::vector<uint8_t> buf;
    buf.reserve(total);

    auto append_u32 = [&](uint32_t v) {
        const auto* p = reinterpret_cast<const uint8_t*>(&v);
        buf.insert(buf.end(), p, p + 4);
    };
    auto append_bytes = [&](const uint8_t* data, size_t len) {
        if (len == 0)
            return;
        buf.insert(buf.end(), data, data + len);
    };

    // Row.
    append_u32(static_cast<uint32_t>(env.row.size()));
    append_bytes(env.row.data(), env.row.size());

    // Attachments.
    append_u32(static_cast<uint32_t>(env.attachments.size()));
    for (const auto& [key, blob] : env.attachments) {
        append_u32(static_cast<uint32_t>(key.size()));
        append_bytes(reinterpret_cast<const uint8_t*>(key.data()), key.size());
        const uint32_t blob_len = blob ? static_cast<uint32_t>(blob->size()) : 0;
        append_u32(blob_len);
        if (blob_len > 0)
            append_bytes(blob->data(), blob_len);
    }

    return buf;
}

inline Envelope DeserializeEnvelope(const uint8_t* data, size_t size) {
    if (size < 8)
        throw std::invalid_argument("DeserializeEnvelope: buffer too small");

    size_t pos = 0;

    auto read_u32 = [&]() -> uint32_t {
        if (pos + 4 > size)
            throw std::invalid_argument("DeserializeEnvelope: unexpected end of buffer");
        uint32_t v;
        std::memcpy(&v, data + pos, 4);
        pos += 4;
        return v;
    };

    // Row.
    const uint32_t row_len = read_u32();
    if (pos + row_len > size)
        throw std::invalid_argument("DeserializeEnvelope: row data truncated");
    EncodedRow row(data + pos, data + pos + row_len);
    pos += row_len;

    // Attachments.
    const uint32_t attach_count = read_u32();
    Attachments attachments;
    for (uint32_t i = 0; i < attach_count; ++i) {
        const uint32_t key_len = read_u32();
        if (pos + key_len > size)
            throw std::invalid_argument("DeserializeEnvelope: key data truncated");
        std::string key(reinterpret_cast<const char*>(data + pos), key_len);
        pos += key_len;

        const uint32_t blob_len = read_u32();
        if (pos + blob_len > size)
            throw std::invalid_argument("DeserializeEnvelope: blob data truncated");
        auto blob = std::make_shared<const std::vector<uint8_t>>(
            data + pos, data + pos + blob_len);
        pos += blob_len;

        attachments[std::move(key)] = std::move(blob);
    }

    return Envelope{std::move(row), std::move(attachments)};
}

inline Envelope DeserializeEnvelope(const std::vector<uint8_t>& buf) {
    return DeserializeEnvelope(buf.data(), buf.size());
}

}  // namespace fletcher

#endif  // FLETCHER_INCLUDE_CORE_ENVELOPE_HPP_
