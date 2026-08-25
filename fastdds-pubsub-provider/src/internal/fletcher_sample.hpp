// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The plain sample that Fast DDS lends at both ends.
//
// Zero-copy in Fast DDS is defined over *plain* types: ones whose in-memory layout already is their
// CDR representation, so nothing has to be serialised on the way out or deserialised on the way in
// (Fast DDS 3.4 "Zero-Copy communication"; the maintained example is
// examples/cpp/delivery_mechanisms, whose IDL is a FINAL struct of primitives and fixed arrays).
// This struct is that type, written by hand rather than generated because Fletcher has no IDL step —
// the static_asserts below stand in for what fastddsgen would have guaranteed.
//
// It is what `loan_sample` hands back on the publish side and what the reader's LoanableSequence
// yields on the subscribe side, so both ends address the same fields instead of reaching into raw
// bytes through a punned element type.
#ifndef FLETCHER_FASTDDS_PUBSUB_PROVIDER_INTERNAL_FLETCHER_SAMPLE_HPP_
#define FLETCHER_FASTDDS_PUBSUB_PROVIDER_INTERNAL_FLETCHER_SAMPLE_HPP_

#include <bit>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace fletcher {
namespace internal {

// The wire sample: `length` bytes of `body` are in use, the rest is unwritten padding.
//
// A fixed-size array is what makes the type plain, so the bound has to be a template parameter
// rather than a constructor argument — a plain type's size has to be known to the compiler, and
// every endpoint of a topic has to agree on it. The provider instantiates one of these per bound
// PayloadBound admits and chooses between them at construction (internal/payload_binding.hpp), and
// FletcherSamplePubSubType puts N in the registered type name so that endpoints on different bounds
// fail to match instead of losing samples quietly.
template <uint32_t N>
struct FletcherSample {
    static_assert(N % 4 == 0, "a payload bound must be a multiple of 4 to avoid tail padding");

    uint32_t length;
    uint8_t body[N];
};

// What "plain" means, spelled out, and the answer FletcherSamplePubSubType<N>::is_plain reports.
// Fast DDS skips serialisation entirely for a loaned sample once told the type is plain, so these
// are the conditions that make that safe, and a build-time failure is the right outcome if a bound
// breaks one:
//   - trivially copyable, so the bytes are the whole value;
//   - no padding anywhere: `length` is 4-aligned and `body` follows immediately, which holds while
//     N is a multiple of 4;
//   - little-endian, because CDR_LE is what the encapsulation header advertises.
//
// They live in a function rather than at namespace scope because FletcherSample<N> is incomplete
// inside its own definition; every bound the provider instantiates drags them in through the type
// support.
template <uint32_t N>
constexpr bool FletcherSampleIsPlain() {
    using Sample = FletcherSample<N>;
    static_assert(std::is_trivially_copyable_v<Sample>);
    static_assert(std::is_standard_layout_v<Sample>);
    static_assert(sizeof(Sample) == sizeof(uint32_t) + N);
    static_assert(offsetof(Sample, body) == sizeof(uint32_t));
    static_assert(std::endian::native == std::endian::little);

    // The generator's criterion: the type's size is exactly the last member's offset plus that
    // member's size, i.e. no padding anywhere. Reported rather than asserted so that a struct which
    // grew padding would degrade to the copying flows instead of lying to Fast DDS — the asserts
    // above still fail the build first, deliberately, because for this type the loan path is the
    // point.
    return sizeof(Sample) == offsetof(Sample, body) + sizeof(Sample::body);
}

}  // namespace internal
}  // namespace fletcher

#endif  // FLETCHER_FASTDDS_PUBSUB_PROVIDER_INTERNAL_FLETCHER_SAMPLE_HPP_
