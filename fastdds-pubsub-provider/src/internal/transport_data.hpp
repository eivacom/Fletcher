// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The sample types the provider hands to Fast DDS: one per direction for the data channel, and the
// same PublishData/ReceivedData pair for the companion __schema channel (it rides the same plain
// sample layout as a row -- the IPC bytes -- plus one attachment carrying the publisher's payload
// bound — see fletcher_sample_pub_sub_type.hpp).
#ifndef FLETCHER_FASTDDS_PUBSUB_PROVIDER_INTERNAL_TRANSPORT_DATA_HPP_
#define FLETCHER_FASTDDS_PUBSUB_PROVIDER_INTERNAL_TRANSPORT_DATA_HPP_

#include <cstdint>
#include <fletcher/core/types.hpp>
#include <fletcher/pubsub/provider.hpp>
#include <memory>
#include <string>
#include <vector>

namespace fletcher {
namespace internal {

// What serialize() reads — the encoder writes row bytes directly into the DDS payload buffer via
// FixedWriteBuffer. Both members point at the caller's: serialize() runs synchronously inside
// DataWriter::write, so neither outlives the Publish call that set it.
struct PublishData {
    const PubSubProvider::RowEncoder* encoder = nullptr;
    const Attachments* attachments = nullptr;

    // Why serialize() failed, so Publish can throw a diagnostic instead of the caller seeing
    // only a return code that cannot distinguish the cause. serialize() must not rethrow, so it
    // records here and Publish reads it after write() returns.
    //
    // Deliberately per-publish rather than a sink on the shared type instance: Publish holds the
    // provider mutex SHARED, so concurrent publishes to different topics run at once and would race
    // on shared state. This struct is one Publish call's own, so there is nothing to race.
    // `mutable` because serialize() receives it as `const void* const`.
    mutable std::string serialize_error;

    // Exception-safe by contract: called from serialize()'s catch handlers, where nothing may
    // throw. Takes const char* (std::exception::what() is noexcept, so no allocation at the
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
// Separate from PublishData rather than one struct carrying both directions: the split is kept
// because a bundled struct made every serialised publish build an `Attachments` it never reads;
// the measured cost is recorded in the README.
struct ReceivedData {
    std::vector<uint8_t> decoded_row;
    Attachments decoded_attachments;

    // Owns the bytes `decoded_attachments` alias: one copy of the sample body, taken ONLY when the
    // sample carries attachments. Fast DDS may recycle the payload the moment deserialize()
    // returns, so the blobs cannot simply point at it — but they can point into this, which lives
    // as long as any of them does.
    //
    // Null for an attachment-free sample, which is the hot path: it pays nothing for this.
    std::shared_ptr<const std::vector<uint8_t>> body;
};

}  // namespace internal
}  // namespace fletcher

#endif  // FLETCHER_FASTDDS_PUBSUB_PROVIDER_INTERNAL_TRANSPORT_DATA_HPP_
