// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The payload bound and the DDS type name it produces.
//
// Lives in pubsub because every provider that puts Fletcher rows on a DDS topic has to agree on
// both: the bound rides in the registered type name, and DDS matches endpoints by that name.
//
// A bound is bytes of row payload in one sample — the encoded row plus its attachments — not
// counting the framing below.
#ifndef FLETCHER_INCLUDE_PUBSUB_PAYLOAD_BOUND_HPP_
#define FLETCHER_INCLUDE_PUBSUB_PAYLOAD_BOUND_HPP_

#include <cstdint>
#include <string>

namespace fletcher {

/// The sample's own 4-byte length plus the 4-byte CDR encapsulation header. A bound plus these is
/// the size Fast DDS is told the type has, which it reports in a uint32.
inline constexpr uint32_t kPayloadFramingBytes = 8;

/// The floor is the smallest envelope that can exist; the ceiling is where a sample's size stops
/// fitting that uint32, rounded down to the alignment the rule requires anyway. Neither bounds what
/// a deployment can afford: a bound costs whatever the caller's resource limits multiply it by, and
/// nothing here caps that product.
inline constexpr uint32_t kMinPayloadBytes = 4;
inline constexpr uint32_t kMaxPayloadBytes = (UINT32_MAX - kPayloadFramingBytes) & ~uint32_t{3};

/// Whether `bytes` can bound a payload.
///
/// Fast DDS delivers zero-copy only for a *plain* type, one whose in-memory layout already is its
/// CDR representation, which requires it to carry no padding. A sample is a 4-byte length followed
/// by `bytes` of body, so it is padding-free exactly while its total is 4-aligned — exactly while
/// `bytes` is a multiple of 4. Nothing else about the number matters to the transport.
constexpr bool IsPayloadBound(uint32_t bytes) {
    return bytes >= kMinPayloadBytes && bytes <= kMaxPayloadBytes && bytes % 4 == 0;
}

/// The same rule for a bound the compiler knows, so both are rejected by one expression.
template <uint32_t N>
concept PayloadBound = IsPayloadBound(N);

// A subtraction and a mask are easy to get wrong by four, and a bound past this ceiling wraps the
// uint32 its sample size is reported in.
static_assert(uint64_t{kMaxPayloadBytes} + kPayloadFramingBytes <= UINT32_MAX);
static_assert(IsPayloadBound(kMinPayloadBytes) && IsPayloadBound(kMaxPayloadBytes));

/// Spell a bound this way and it is checked where it is written:
///
///     options.max_payload_bytes = fletcher::kPayloadBytes<128 * 1024>;  // fine
///     options.max_payload_bytes = fletcher::kPayloadBytes<100'001>;     // does not compile
///
/// A bound that only exists at run time is rejected by the provider constructor instead. Nothing is
/// ever rounded.
template <uint32_t N>
    requires PayloadBound<N>
inline constexpr uint32_t kPayloadBytes = N;

/// The registered DDS type name for a bound, and the only thing keeping two bounds apart: DDS
/// matches by type name, so endpoints on different bounds fail to discover each other rather than
/// exchanging samples one of them cannot hold. Every provider must spell it through here.
inline std::string FletcherTypeName(uint32_t payload_bytes) {
    return "fletcher_" + std::to_string(payload_bytes);
}

/// The registered DDS type name of the companion `__schema` channel, which is bound-independent.
inline constexpr const char* kSchemaTypeName = "SchemaBytes";

}  // namespace fletcher

#endif  // FLETCHER_INCLUDE_PUBSUB_PAYLOAD_BOUND_HPP_
