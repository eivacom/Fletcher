// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The writer side's statuses, forwarded to the application's FastDDSStatusListener, plus the
// status mask for the writer end. One instance per provider, shared by every DataWriter it
// creates -- it carries no per-topic state, and the topic name is on the endpoint itself.
//
// The reader side splits in two: a data reader has a real Fast DDS listener
// (DataReaderListenerBase, internal/data_reader_listener.hpp), forwarding its own statuses
// from inside on_subscription_matched et al. A schema reader has none -- `listener =
// nullptr, StatusMask::all()` (ddsbus's WaitsetDataReader pattern), StatusCondition left at its
// default enabled mask -- and this provider's one schema thread wakes on that condition, reads
// `get_status_changes()` itself and forwards each one there (Impl::DispatchReaderStatuses,
// fast_dds_pubsub_provider.cpp). Writers have no thread of their own either way, so
// DataWriterListener below is unchanged.
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

    // The mirror of the reader side's requested_incompatible_qos (DataReaderListenerBase,
    // internal/data_reader_listener.hpp), and the reason both exist: a QoS mismatch means the
    // endpoints never match, which otherwise shows up only as a subscriber that never receives
    // anything. Publish keeps succeeding — there is simply nobody to deliver to.
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
    // Not in the writer's status mask (CreateTopic): this is a Fast DDS extension with no
    // StatusMask bit, dispatched whenever a listener is set at all (DataWriterImpl.cpp).
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
