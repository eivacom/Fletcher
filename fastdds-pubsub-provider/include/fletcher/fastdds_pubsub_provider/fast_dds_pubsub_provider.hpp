// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#ifndef FLETCHER_INCLUDE_FAST_DDS_PUBSUB_PROVIDER_HPP_
#define FLETCHER_INCLUDE_FAST_DDS_PUBSUB_PROVIDER_HPP_

#include <cstdint>
#include <fastdds/dds/publisher/qos/DataWriterQos.hpp>
#include <fastdds/dds/subscriber/qos/DataReaderQos.hpp>
#include <fletcher/pubsub/payload_bound.hpp>
#include <fletcher/pubsub/provider.hpp>
#include <memory>
#include <string>
#include <unordered_map>

#include "fletcher/fastdds_pubsub_provider/internal/qos_defaults.hpp"

namespace fletcher {

/// Typed configuration for FastDDSPubSubProvider. All QoS settings
/// are specified up-front; the provider is immutable with respect to
/// configuration after construction.
///
/// Per-topic QoS overrides are keyed by the joined topic string
/// ("a/b/c"). The provider looks up overrides first, then falls back
/// to the instance defaults. The default values for default_writer_qos
/// and default_reader_qos encode Fletcher's profile — see the
/// fastdds-pubsub-provider README for what that profile contains.
struct FastDDSProviderOptions {
    /// DDS domain ID.
    uint32_t domain_id = 0;

    /// @brief Ceiling on the row payload of one sample: any positive multiple of 4.
    ///
    /// Checked by `IsPayloadBound`; the constructor throws otherwise, and nothing is rounded.
    /// Write it as `kPayloadBytes<N>` to be told at compile time instead.
    ///
    /// The bound is part of the registered DDS type name, so endpoints on different bounds fail
    /// to discover each other rather than dropping samples.
    ///
    /// @warning The expensive number in this header. A bounded type preallocates the whole bound
    /// per history slot per endpoint, and a data-sharing writer reserves
    /// `(max_samples + extra_samples) * (bound + 8)` bytes of shared segment up front. Nothing
    /// caps that product; if it exceeds a 32-bit segment size Fast DDS uses the transport
    /// instead. Bring `max_samples` down as the bound goes up.
    uint32_t max_payload_bytes = kPayloadBytes<64 * 1024>;

    /// @brief Maximum serialised Arrow IPC schema size, bounding the companion __schema channel.
    ///
    /// Any value: unlike `max_payload_bytes` this channel's type is not plain, so it need not
    /// avoid padding. It reports itself bounded, which data-sharing asks for and which costs one
    /// preallocated slot per endpoint here, since the channel is `KEEP_LAST(1)` and `CreateTopic`
    /// announces a schema once per topic.
    ///
    /// @warning A PREALLOCATED pool cannot grow, so this matters at both ends: a schema larger
    /// than a *receiving* endpoint's value is rejected rather than delivered, and that
    /// subscriber's schema future never resolves. The reader reports `on_sample_rejected`.
    uint32_t max_schema_bytes = 64 * 1024;

    /// Publish out of a buffer the transport owns instead of encoding through one it then copies.
    /// With data-sharing that buffer *is* the segment the subscriber reads from, so a same-machine
    /// sample is never copied at all, and no `serialize()` runs on the publish path.
    ///
    /// A deployment switch, not a topic property: it says "my subscribers are on this box", and it
    /// applies to every topic this provider publishes. Two things change when it is on:
    ///   - Every sample crosses the wire at the full sample size. Fast DDS stamps a loaned payload
    ///     `length = max_serialized_type_size` and nothing recomputes it, so a 214-byte row costs
    ///     a whole `PayloadBytes()` to any reader off this host. A serialised sample is the same
    ///     layout truncated after the bytes in use, so off, small rows stay small.
    ///   - A row past `PayloadBytes()` throws out of Publish, where an unloaned writer fails the
    ///     serialisation internally and drops the sample.
    ///   - **The unused tail of every sample goes on the wire.** The loan is taken with
    ///     `NO_LOAN_INITIALIZATION` and only the bytes in use are written, so the rest of the
    ///     `PayloadBytes()` a remote reader receives is a **previous sample of this topic** —
    ///     zeros the first time a slot is used, since Fast DDS's payload nodes come from `calloc`
    ///     (`TopicPayloadPool`) and a data-sharing segment from the OS. Never unrelated process
    ///     memory. Not zeroed per sample deliberately: a `memset` of the whole bound costs more
    ///     than the copy this option exists to avoid. With data-sharing (subscribers on this box,
    ///     which is what the option is for) nothing crosses a wire at all. Leave it off if
    ///     subscribers are remote and one topic's samples must not leak into each other.
    ///
    /// Subscribers need no matching setting. Loans are not negotiated: the reader's own type gates
    /// them (Fast DDS 3.4 `DataReaderImpl.cpp`), the writer's own gates `loan_sample`
    /// (`DataWriterImpl.cpp`), and both publish paths write the same two fields in the same places.
    /// Fletcher subscribers therefore always read out of the transport's buffer, whichever way the
    /// publisher wrote it.
    bool loan_publish = false;

    /// Default DataWriter QoS applied to any topic without a per-topic
    /// override.
    eprosima::fastdds::dds::DataWriterQos default_writer_qos =
        internal::MakeFletcherDefaultWriterQos();

    /// Default DataReader QoS applied to any topic without a per-topic
    /// override.
    eprosima::fastdds::dds::DataReaderQos default_reader_qos =
        internal::MakeFletcherDefaultReaderQos();

    /// Per-topic DataWriter QoS overrides. Key: joined topic string.
    std::unordered_map<std::string, eprosima::fastdds::dds::DataWriterQos> topic_writer_qos;

    /// Per-topic DataReader QoS overrides. Key: joined topic string.
    std::unordered_map<std::string, eprosima::fastdds::dds::DataReaderQos> topic_reader_qos;
};

/// PubSubProvider transport backed by eProsima Fast DDS.
///
/// QoS configuration is supplied entirely via FastDDSProviderOptions
/// at construction time. There are no runtime setters: this avoids
/// timing bugs (e.g. setting QoS after the DataWriter has already
/// been created) and keeps the provider's internal state immutable
/// after construction.
///
/// The companion schema channel (__schema topic) always uses RELIABLE +
/// KEEP_LAST(depth=1) + TRANSIENT_LOCAL and is not configurable —
/// Fletcher-internal implementation detail.
class FastDDSPubSubProvider : public PubSubProvider {
   public:
    explicit FastDDSPubSubProvider(FastDDSProviderOptions options);

    /// Destruction precondition: the caller must ensure the provider is
    /// quiescent — no thread executing or about to enter a public API on this
    /// instance, and no provider callback still in flight that can re-enter it.
    /// The destructor tears down DDS entities and invalidates all internal
    /// state; it is not a synchronization boundary for concurrent use.
    ~FastDDSPubSubProvider() override;

    FastDDSPubSubProvider(const FastDDSPubSubProvider&) = delete;
    FastDDSPubSubProvider& operator=(const FastDDSPubSubProvider&) = delete;

    void CreateTopic(const std::vector<std::string>& topic_segments, OwnedSchema schema) override;

    void Publish(const std::vector<std::string>& topic_segments, const RowEncoder& encoder,
                 const Attachments& attachments = {}) override;

    // [[nodiscard]] is NOT inherited from the PubSubProvider base declaration and
    // the diagnostic keys off the STATIC type at the call site, so the annotation
    // must be repeated on every concrete override or it never fires where
    // applications actually call (#56).
    [[nodiscard]] SubscriptionResult Subscribe(const std::vector<std::string>& topic_segments,
                                               SubscribeCallback callback) override;

    void Unsubscribe(const std::vector<std::string>& topic_segments) override;

    /// The payload bound in force — `FastDDSProviderOptions::max_payload_bytes` exactly as given,
    /// since an unsupported one never gets past the constructor. It is the number in the registered
    /// type name, and the size a row has to fit.
    uint32_t PayloadBytes() const;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace fletcher

#endif  // FLETCHER_INCLUDE_FAST_DDS_PUBSUB_PROVIDER_HPP_
