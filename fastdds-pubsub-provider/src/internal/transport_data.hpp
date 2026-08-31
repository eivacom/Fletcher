// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The sample types the provider hands to Fast DDS: RawBytes for the companion schema channel, and
// one per direction for the data channel.
#ifndef FLETCHER_FASTDDS_PUBSUB_PROVIDER_INTERNAL_TRANSPORT_DATA_HPP_
#define FLETCHER_FASTDDS_PUBSUB_PROVIDER_INTERNAL_TRANSPORT_DATA_HPP_

#include <cstdint>
#include <fletcher/core/types.hpp>
#include <fletcher/pubsub/provider.hpp>
#include <string>
#include <vector>

namespace fletcher {
namespace internal {

struct RawBytes {
    std::vector<uint8_t> data;
};

// What serialize() reads — the encoder writes row bytes directly into the DDS payload buffer via
// FixedWriteBuffer. Both members point at the caller's: serialize() runs synchronously inside
// DataWriter::write, so neither outlives the Publish call that set it.
struct PublishData {
    const PubSubProvider::RowEncoder* encoder = nullptr;
    const Attachments* attachments = nullptr;

    // #60: why serialize() failed, so Publish can throw a diagnostic instead of the caller seeing
    // only a return code that cannot distinguish the cause (H-INV-2). serialize() must not rethrow
    // (H-INV-3), so it records here and Publish reads it after write() returns.
    //
    // Deliberately per-publish rather than a sink on the shared type instance: Publish holds the
    // provider mutex SHARED, so concurrent publishes to different topics run at once and would race
    // on shared state. This struct is one Publish call's own, so there is nothing to race.
    // `mutable` because serialize() receives it as `const void* const`.
    mutable std::string serialize_error;

    // Exception-safe by contract: called from serialize()'s catch handlers, where nothing may throw
    // (H-INV-3). Takes const char* (std::exception::what() is noexcept, so no allocation at the
    // call site) and swallows a bad_alloc from the assignment itself.
    void RecordSerializeError(const char* what) const noexcept {
        try {
            serialize_error = what;
        } catch (...) {  // NOLINT(bugprone-empty-catch) — a lost diagnostic beats a throw here.
        }
    }
};

// What deserialize() fills, decoded in place and moved on by the listener.
//
// Separate from PublishData rather than one struct carrying both directions: Attachments is an
// unordered_map, and MSVC allocates a sentinel node in its default constructor, so a bundled struct
// made every serialised publish allocate and free a node for a member that path never reads. See
// README "Measured decisions".
struct ReceivedData {
    std::vector<uint8_t> decoded_row;
    Attachments decoded_attachments;
};

}  // namespace internal
}  // namespace fletcher

#endif  // FLETCHER_FASTDDS_PUBSUB_PROVIDER_INTERNAL_TRANSPORT_DATA_HPP_
