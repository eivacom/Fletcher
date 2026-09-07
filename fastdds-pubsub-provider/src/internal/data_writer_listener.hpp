// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// Publish-side statuses, forwarded to the application's FastDDSStatusListener. One instance per
// provider, shared by every DataWriter it creates — none of these callbacks carries per-topic
// state, and the topic name is on the writer.
//
// The reader's half of this lives on DataReaderListenerBase, which needs per-topic state anyway.
#ifndef FLETCHER_FASTDDS_PUBSUB_PROVIDER_INTERNAL_DATA_WRITER_LISTENER_HPP_
#define FLETCHER_FASTDDS_PUBSUB_PROVIDER_INTERNAL_DATA_WRITER_LISTENER_HPP_

#include <cstdint>
#include <fastdds/dds/core/status/StatusMask.hpp>
#include <fastdds/dds/publisher/DataWriter.hpp>
#include <fastdds/dds/publisher/DataWriterListener.hpp>

#include "fletcher/fastdds_pubsub_provider/fast_dds_pubsub_provider.hpp"
#include "status_endpoint.hpp"

namespace fletcher {
namespace internal {

// Endpoints are created with the statuses their listener actually implements, rather than the
// default StatusMask::all(): Fast DDS then only dispatches those, and the mask says in one place
// which callbacks below are live. `<<` is how StatusMask composes — plain `|` decays to the
// std::bitset it derives from.
inline eprosima::fastdds::dds::StatusMask WriterStatusMask() {
    return eprosima::fastdds::dds::StatusMask::publication_matched()
           << eprosima::fastdds::dds::StatusMask::offered_deadline_missed()
           << eprosima::fastdds::dds::StatusMask::offered_incompatible_qos()
           << eprosima::fastdds::dds::StatusMask::liveliness_lost();
}

inline eprosima::fastdds::dds::StatusMask ReaderStatusMask() {
    return eprosima::fastdds::dds::StatusMask::data_available()
           << eprosima::fastdds::dds::StatusMask::subscription_matched()
           << eprosima::fastdds::dds::StatusMask::requested_deadline_missed()
           << eprosima::fastdds::dds::StatusMask::liveliness_changed()
           << eprosima::fastdds::dds::StatusMask::requested_incompatible_qos()
           << eprosima::fastdds::dds::StatusMask::sample_lost()
           << eprosima::fastdds::dds::StatusMask::sample_rejected();
}

// sample_rejected matters: the pool is PREALLOCATED and cannot grow for an oversized schema.
inline eprosima::fastdds::dds::StatusMask SchemaReaderStatusMask() {
    return eprosima::fastdds::dds::StatusMask::data_available()
           << eprosima::fastdds::dds::StatusMask::sample_rejected()
           << eprosima::fastdds::dds::StatusMask::sample_lost();
}

// Every callback DataWriterListener declares is overridden, in the order it declares them, so that
// nothing a DataWriter can report is left on the default no-op. Each one translates the DDS status
// into Fletcher's own vocabulary and hands it on; a null listener observes nothing, which is what
// a provider built from a `ProviderConfig` alone gets.
class DataWriterListener : public eprosima::fastdds::dds::DataWriterListener {
   public:
    explicit DataWriterListener(FastDDSStatusListener* status_listener)
        : status_listener_(status_listener) {}

    // Discovery, both directions. Losing a reader is the half worth acting on: a writer with none
    // left keeps accepting publishes and delivers them nowhere. `change` carries the sign.
    void on_publication_matched(
        eprosima::fastdds::dds::DataWriter* writer,
        const eprosima::fastdds::dds::PublicationMatchedStatus& info) override {
        if (status_listener_)
            status_listener_->OnMatched(WriterEndpoint(writer), info.current_count,
                                        info.current_count_change);
    }

    // Fletcher sets no DEADLINE, so this only fires on a writer an operator gave one to through
    // the provider document. Implemented so that a configured policy cannot fail silently.
    void on_offered_deadline_missed(
        eprosima::fastdds::dds::DataWriter* writer,
        const eprosima::fastdds::dds::OfferedDeadlineMissedStatus& status) override {
        if (status_listener_)
            status_listener_->OnDeadlineMissed(WriterEndpoint(writer), status.total_count);
    }

    // The mirror of DataReaderListenerBase::on_requested_incompatible_qos, and the reason both
    // exist: a QoS mismatch means the endpoints never match, which otherwise shows up only as a
    // subscriber that never receives anything. Publish keeps succeeding — there is simply nobody to
    // deliver to.
    void on_offered_incompatible_qos(
        eprosima::fastdds::dds::DataWriter* writer,
        const eprosima::fastdds::dds::OfferedIncompatibleQosStatus& status) override {
        if (status_listener_)
            status_listener_->OnIncompatibleQos(WriterEndpoint(writer),
                                                static_cast<uint32_t>(status.last_policy_id),
                                                status.total_count);
    }

    // Readers have marked this writer NOT_ALIVE and stop expecting its samples. Fletcher leaves
    // LIVELINESS at its AUTOMATIC default with an infinite lease (`QosPolicies.hpp`), where
    // it cannot fire, so this too reports a policy an operator configured.
    void on_liveliness_lost(eprosima::fastdds::dds::DataWriter* writer,
                            const eprosima::fastdds::dds::LivelinessLostStatus& status) override {
        if (status_listener_)
            status_listener_->OnLivelinessLost(WriterEndpoint(writer),
                                               static_cast<uint32_t>(status.total_count));
    }

    // KEEP_ALL + RELIABLE means the writer blocks rather than drops, so an unacknowledged sample
    // being removed is history overflowing under max_blocking_time — data loss, not backpressure.
    // Not in WriterStatusMask(): this is a Fast DDS extension with no StatusMask bit, dispatched
    // whenever a listener is set at all (DataWriterImpl.cpp).
    void on_unacknowledged_sample_removed(
        eprosima::fastdds::dds::DataWriter* writer,
        const eprosima::fastdds::dds::InstanceHandle_t& /*instance*/) override {
        if (status_listener_)
            status_listener_->OnUnacknowledgedSampleRemoved(WriterEndpoint(writer));
    }

   private:
    FastDDSStatusListener* status_listener_;
};

}  // namespace internal
}  // namespace fletcher

#endif  // FLETCHER_FASTDDS_PUBSUB_PROVIDER_INTERNAL_DATA_WRITER_LISTENER_HPP_
