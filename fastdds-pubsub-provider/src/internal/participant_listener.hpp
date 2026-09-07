// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// Discovery of remote entities, forwarded to the application's FastDDSStatusListener.
//
// Installed with StatusMask::none(): a participant listener holding the data_on_readers bit would
// be handed every reader's data instead of the reader's own listener
// (DataReaderImpl::set_read_communication_status asks the subscriber/participant chain for
// data_on_readers first), and the three discovery callbacks below are not mask-gated at all
// (DomainParticipantImpl::MyRTPSParticipantListener fires them whenever a listener is set), so
// none() costs nothing here.
#ifndef FLETCHER_FASTDDS_PUBSUB_PROVIDER_INTERNAL_PARTICIPANT_LISTENER_HPP_
#define FLETCHER_FASTDDS_PUBSUB_PROVIDER_INTERNAL_PARTICIPANT_LISTENER_HPP_

#include <fastdds/dds/domain/DomainParticipantListener.hpp>
#include <string_view>

#include "fletcher/fastdds_pubsub_provider/fast_dds_pubsub_provider.hpp"

namespace fletcher {
namespace internal {

class ParticipantListener : public eprosima::fastdds::dds::DomainParticipantListener {
   public:
    explicit ParticipantListener(FastDDSStatusListener* status_listener)
        : status_listener_(status_listener) {}

    // Discovery names and type names are `fastcdr::string_255`, not std::string: the view is built
    // over the fixed buffer the info struct owns for the duration of this call. A view over
    // `.to_string()` would dangle the moment the temporary died.
    void on_participant_discovery(eprosima::fastdds::dds::DomainParticipant* /*participant*/,
                                  eprosima::fastdds::rtps::ParticipantDiscoveryStatus reason,
                                  const eprosima::fastdds::dds::ParticipantBuiltinTopicData& info,
                                  bool& /*should_be_ignored*/) override {
        if (!status_listener_) return;
        using eprosima::fastdds::rtps::ParticipantDiscoveryStatus;
        const bool alive = reason != ParticipantDiscoveryStatus::REMOVED_PARTICIPANT &&
                           reason != ParticipantDiscoveryStatus::DROPPED_PARTICIPANT;
        status_listener_->OnParticipantDiscovered(
            std::string_view(info.participant_name.c_str(), info.participant_name.size()), alive);
    }

    void on_data_reader_discovery(eprosima::fastdds::dds::DomainParticipant* /*participant*/,
                                  eprosima::fastdds::rtps::ReaderDiscoveryStatus reason,
                                  const eprosima::fastdds::dds::SubscriptionBuiltinTopicData& info,
                                  bool& /*should_be_ignored*/) override {
        if (!status_listener_ || IsCompanionChannel(info.topic_name)) return;
        status_listener_->OnReaderDiscovered(
            std::string_view(info.topic_name.c_str(), info.topic_name.size()),
            std::string_view(info.type_name.c_str(), info.type_name.size()),
            reason != eprosima::fastdds::rtps::ReaderDiscoveryStatus::REMOVED_READER);
    }

    void on_data_writer_discovery(eprosima::fastdds::dds::DomainParticipant* /*participant*/,
                                  eprosima::fastdds::rtps::WriterDiscoveryStatus reason,
                                  const eprosima::fastdds::dds::PublicationBuiltinTopicData& info,
                                  bool& /*should_be_ignored*/) override {
        if (!status_listener_ || IsCompanionChannel(info.topic_name)) return;
        status_listener_->OnWriterDiscovered(
            std::string_view(info.topic_name.c_str(), info.topic_name.size()),
            std::string_view(info.type_name.c_str(), info.type_name.size()),
            reason != eprosima::fastdds::rtps::WriterDiscoveryStatus::REMOVED_WRITER);
    }

   private:
    // Fletcher's own `<topic>/__schema` endpoints are not reported: a catalog built from discovery
    // would otherwise hold a name `Subscribe` refuses (a `__`-prefixed segment), and the schema
    // channel's type name carries no payload bound, so there is nothing to diagnose from it either.
    static bool IsCompanionChannel(const eprosima::fastcdr::string_255& topic_name) {
        const std::string_view name(topic_name.c_str(), topic_name.size());
        return name.ends_with("/__schema");
    }

    FastDDSStatusListener* status_listener_;
};

}  // namespace internal
}  // namespace fletcher

#endif  // FLETCHER_FASTDDS_PUBSUB_PROVIDER_INTERNAL_PARTICIPANT_LISTENER_HPP_
