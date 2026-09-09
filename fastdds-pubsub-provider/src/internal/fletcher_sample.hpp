// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The plain sample Fast DDS lends at both ends: [0,4) uint32 length, then payload_bytes of body.
#ifndef FLETCHER_FASTDDS_PUBSUB_PROVIDER_INTERNAL_FLETCHER_SAMPLE_HPP_
#define FLETCHER_FASTDDS_PUBSUB_PROVIDER_INTERNAL_FLETCHER_SAMPLE_HPP_

#include <bit>
#include <cstdint>
#include <cstring>

namespace fletcher {
namespace internal {

// Where a CDR sequence<octet> carries its length, so an XRCE peer interoperates with either path.
constexpr uint32_t kSampleLengthPrefix = 4;
static_assert(kSampleLengthPrefix % 4 == 0);

constexpr uint32_t SampleSize(uint32_t payload_bytes) {
    return kSampleLengthPrefix + payload_bytes;
}

// Host-order length: it would disagree with the encapsulation header on a big-endian host.
static_assert(std::endian::native == std::endian::little,
              "Fletcher's samples store their length in host order and assume little-endian");

// memcpy, not a punned pointer: a payload-pool address carries no C++ object.
inline uint32_t ReadSampleLength(const uint8_t* sample) {
    uint32_t length = 0;
    std::memcpy(&length, sample, sizeof(length));
    return length;
}

inline void WriteSampleLength(uint8_t* sample, uint32_t length) {
    std::memcpy(sample, &length, sizeof(length));
}

inline const uint8_t* SampleBody(const uint8_t* sample) { return sample + kSampleLengthPrefix; }
inline uint8_t* SampleBody(uint8_t* sample) { return sample + kSampleLengthPrefix; }

}  // namespace internal
}  // namespace fletcher

#endif  // FLETCHER_FASTDDS_PUBSUB_PROVIDER_INTERNAL_FLETCHER_SAMPLE_HPP_
