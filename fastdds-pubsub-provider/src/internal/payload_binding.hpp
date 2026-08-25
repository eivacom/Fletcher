// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// Everything that has to be built knowing the payload bound, behind one interface.
//
// The bound is a template parameter — a plain type's size has to be known to the compiler — but it
// reaches the provider as a runtime option, so something has to cross from one to the other. This
// is that seam, and deliberately the only one: the registered type, the loaned publish flow and the
// loaned read flow all come from the same object, so they cannot end up disagreeing about N. A
// disagreement would compile, and only the reads would be wrong.
#ifndef FLETCHER_FASTDDS_PUBSUB_PROVIDER_INTERNAL_PAYLOAD_BINDING_HPP_
#define FLETCHER_FASTDDS_PUBSUB_PROVIDER_INTERNAL_PAYLOAD_BINDING_HPP_

#include <cstdint>
#include <fastdds/dds/topic/TypeSupport.hpp>
#include <fletcher/pubsub/provider.hpp>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "data_reader_listener.hpp"
#include "fletcher/fastdds_pubsub_provider/fast_dds_pubsub_provider.hpp"
#include "fletcher_sample_pub_sub_type.hpp"
#include "sample_writer.hpp"

namespace fletcher {
namespace internal {

class PayloadBinding {
   public:
    virtual ~PayloadBinding() = default;

    virtual uint32_t Bytes() const = 0;
    virtual eprosima::fastdds::dds::TypeSupport MakeTypeSupport() const = 0;
    virtual std::unique_ptr<SampleWriterBase> MakeLoanedWriter() const = 0;
    virtual std::unique_ptr<DataReaderListenerBase> MakeLoanedListener(
        PubSubProvider::SubscribeCallback callback, SharedSchema schema,
        size_t max_queued) const = 0;
};

template <uint32_t N>
    requires PayloadBound<N>
class PayloadBindingFor final : public PayloadBinding {
   public:
    uint32_t Bytes() const override { return N; }

    eprosima::fastdds::dds::TypeSupport MakeTypeSupport() const override {
        return eprosima::fastdds::dds::TypeSupport(new FletcherSamplePubSubType<N>());
    }

    std::unique_ptr<SampleWriterBase> MakeLoanedWriter() const override {
        return std::make_unique<LoanableSampleWriter<N>>();
    }

    std::unique_ptr<DataReaderListenerBase> MakeLoanedListener(
        PubSubProvider::SubscribeCallback callback, SharedSchema schema,
        size_t max_queued) const override {
        return std::make_unique<LoanableDataReaderListener<N>>(std::move(callback),
                                                               std::move(schema), max_queued);
    }
};

// Instantiates one binding per bound the rule admits and returns the one matching `bytes`.
// Recursion rather than a written-out list, so the set of compiled bounds lives only in the header
// that states the rule — a loop cannot do this, because `N` has to be a constant expression.
//
// The comparisons are incidental: the walk is visiting each N anyway to instantiate it. Validation
// is not its job, which is why the last step returns unconditionally — every smaller bound has been
// ruled out and IsPayloadBound admitted `bytes`, so it can only be this one.
//
// Precondition: IsPayloadBound(bytes). MakePayloadBinding is the entry point that enforces it.
template <uint32_t N = kMinPayloadBytes>
std::unique_ptr<PayloadBinding> SelectPayloadBinding(uint32_t bytes) {
    if constexpr (N == kMaxPayloadBytes) {
        return std::make_unique<PayloadBindingFor<N>>();
    } else {
        return bytes == N ? std::make_unique<PayloadBindingFor<N>>()
                          : SelectPayloadBinding<N * 2>(bytes);
    }
}

// Nothing is rounded: a bound this provider has no plain type for is a mistake, and one that
// reached run time at all only because it was not written as kPayloadBytes<N> — where the same
// rule, the same expression, is a compile error.
inline std::unique_ptr<PayloadBinding> MakePayloadBinding(uint32_t bytes) {
    if (!IsPayloadBound(bytes)) {
        throw std::invalid_argument(
            "FastDDS: max_payload_bytes " + std::to_string(bytes) +
            " is not a bound this provider has a plain type for; it must be a power of two from " +
            std::to_string(kMinPayloadBytes) + " to " + std::to_string(kMaxPayloadBytes));
    }
    return SelectPayloadBinding(bytes);
}

}  // namespace internal
}  // namespace fletcher

#endif  // FLETCHER_FASTDDS_PUBSUB_PROVIDER_INTERNAL_PAYLOAD_BINDING_HPP_
