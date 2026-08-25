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
