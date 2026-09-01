// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#ifndef FLETCHER_INCLUDE_CORE_ENVELOPE_HPP_
#define FLETCHER_INCLUDE_CORE_ENVELOPE_HPP_

#include <bit>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "fletcher/core/types.hpp"

namespace fletcher {

static_assert(std::endian::native == std::endian::little,
              "Envelope wire format assumes little-endian host");

// An encoded row bundled with optional attachments.
struct Envelope {
    EncodedRow row;
    Attachments attachments;
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

[[nodiscard]] inline std::vector<uint8_t> SerializeEnvelope(const Envelope& env) {
    // Pre-compute total size.
    size_t total = 4 + env.row.size() + 4;
    for (const auto& [key, blob] : env.attachments) total += 4 + key.size() + 4 + blob.size();

    std::vector<uint8_t> buf;
    buf.reserve(total);

    auto append_u32 = [&](uint32_t v) {
        const auto* p = reinterpret_cast<const uint8_t*>(&v);
        buf.insert(buf.end(), p, p + 4);
    };
    auto append_bytes = [&](const uint8_t* data, size_t len) {
        if (len == 0) return;
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
        const auto blob_len = static_cast<uint32_t>(blob.size());
        append_u32(blob_len);
        if (blob_len > 0) append_bytes(blob.data(), blob_len);
    }

    return buf;
}

namespace internal {

// How many attachments the buffer claims, without parsing them. 0 for a buffer that carries none
// or is too short to say.
//
// INTERNAL, and deliberately so: it exists only so a parse can decide whether it needs a shared
// OWNER for these bytes before it pays for one. A caller never has to reason about that — the
// two-argument DeserializeEnvelope below does it for them, and the three-argument form is for the
// receive path that already holds its bytes in shared storage and must not pay a second copy.
//
// Shared with the Fast DDS envelope codec rather than written twice: it is the same 14-line parse
// of the same wire header, and two copies of it are two chances to disagree about the layout.
[[nodiscard]] inline uint32_t EnvelopeAttachmentCount(const uint8_t* data, size_t size) {
    if (size < 4) return 0;
    uint32_t row_len = 0;
    std::memcpy(&row_len, data, 4);
    if (row_len > size - 4) return 0;
    const size_t pos = 4 + static_cast<size_t>(row_len);
    if (size - pos < 4) return 0;
    uint32_t count = 0;
    std::memcpy(&count, data + pos, 4);
    return count;
}

}  // namespace internal

// Parse `[data, data+size)` into an Envelope whose attachment blobs ALIAS that
// buffer — `owner` is what keeps it alive, and every Blob produced here takes
// its own reference to it (spec §3.2). No attachment byte is copied.
//
// The caller must therefore hand over a shared owner for the bytes it is
// parsing: a receive path whose payload lives in a local `std::vector` makes
// that vector a `shared_ptr` first. `Envelope::row` is still a copy — the
// residue §8/§11 assign to the zero-copy-receive stage.
//
// `owner` may be null ONLY for a buffer carrying no attachments. A buffer that claims a non-empty
// attachment with no owner to hand it is refused — with PubSubError(kInvalidArgument), from Blob's
// own constructor, rather than by producing a blob nothing keeps alive. **Prefer the two-argument
// overload below**, which cannot be called wrongly; this form is for a caller that already holds
// the bytes in shared storage and would otherwise pay a second copy for them.
//
// Two exception types cross this function, and they are different failures: a malformed or
// truncated buffer is std::invalid_argument (a wire problem), a non-empty attachment with no owner
// is PubSubError(kInvalidArgument) (a caller problem).
[[nodiscard]] inline Envelope DeserializeEnvelope(std::shared_ptr<const void> owner,
                                                  const uint8_t* data, size_t size) {
    if (size < 8) throw std::invalid_argument("DeserializeEnvelope: buffer too small");

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
        // Where the bytes lie, not a copy of them: the caller's `owner` keeps
        // them alive for as long as this Blob (or any copy of it) exists. An
        // empty attachment carries no pointer at all, per §3.2 clause 5.
        Blob blob = blob_len > 0 ? Blob(owner, data + pos, blob_len) : Blob();
        pos += blob_len;

        attachments[std::move(key)] = std::move(blob);
    }

    return Envelope{std::move(row), std::move(attachments)};
}

// Parse a buffer the caller does NOT hold in shared storage — a transport read buffer, a WebSocket
// frame — and hand back attachments that stay valid after it returns.
//
// It takes ONE shared copy of the buffer, and **only when the envelope carries attachments**; an
// attachment-free envelope is parsed in place and copies nothing beyond `Envelope::row`, which was
// always an owning copy. That is byte-for-byte what every caller of the three-argument form used
// to hand-roll, minus the chance of getting the owner rule wrong: there is no way to call this and
// produce a blob nothing keeps alive.
[[nodiscard]] inline Envelope DeserializeEnvelope(const uint8_t* data, size_t size) {
    if (internal::EnvelopeAttachmentCount(data, size) == 0) {
        return DeserializeEnvelope(nullptr, data, size);
    }
    auto owner = std::make_shared<const std::vector<uint8_t>>(data, data + size);
    return DeserializeEnvelope(owner, owner->data(), size);
}

// Convenience form for a buffer Fletcher already holds in shared storage: the
// vector is its own owner.
[[nodiscard]] inline Envelope DeserializeEnvelope(
    const std::shared_ptr<const std::vector<uint8_t>>& buf) {
    if (!buf) throw std::invalid_argument("DeserializeEnvelope: null buffer");
    return DeserializeEnvelope(buf, buf->data(), buf->size());
}

}  // namespace fletcher

#endif  // FLETCHER_INCLUDE_CORE_ENVELOPE_HPP_
