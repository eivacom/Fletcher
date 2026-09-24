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
        // DISCOVERED and CHANGED_QOS are "here"; REMOVED, DROPPED and IGNORED are not — an
        // `!= REMOVED && != DROPPED` used to stand in for that and silently counted IGNORED as
        // alive too. (Authentication is a distinct, `HAVE_SECURITY`-gated callback —
        // `onParticipantAuthentication`, not a value of this enum at all — and this build has
        // security off, so it never fires and is not overridden here.)
        const bool alive = reason == ParticipantDiscoveryStatus::DISCOVERED_PARTICIPANT ||
                           reason == ParticipantDiscoveryStatus::CHANGED_QOS_PARTICIPANT;
        // CHANGED_QOS_PARTICIPANT reports the SAME participant, with `alive` freshly true again —
        // not a second join. A census that just counts `true` calls double-counts it; key by
        // `participant_name` (the only identity this callback carries) and let a repeat overwrite
        // an existing entry instead of adding to it.
        status_listener_->OnParticipantDiscovered(
            std::string_view(info.participant_name.c_str(), info.participant_name.size()), alive);
    }

    void on_data_reader_discovery(eprosima::fastdds::dds::DomainParticipant* /*participant*/,
                                  eprosima::fastdds::rtps::ReaderDiscoveryStatus reason,
                                  const eprosima::fastdds::dds::SubscriptionBuiltinTopicData& info,
                                  bool& /*should_be_ignored*/) override {
        if (!status_listener_ || IsCompanionChannel(info)) return;
        using eprosima::fastdds::rtps::ReaderDiscoveryStatus;
        // DISCOVERED and CHANGED_QOS is "here"; REMOVED and IGNORED are not.
        const bool alive = reason == ReaderDiscoveryStatus::DISCOVERED_READER ||
                           reason == ReaderDiscoveryStatus::CHANGED_QOS_READER;
        // Same idiom as participant discovery above: CHANGED_QOS_READER resends `alive == true` for
        // a reader already reported alive, not a new one. A census keyed on a bare counter
        // double-counts it — key by `(topic, type_name)`, the identity this callback carries (and
        // the same is true of `on_data_writer_discovery` below).
        status_listener_->OnReaderDiscovered(
            std::string_view(info.topic_name.c_str(), info.topic_name.size()),
            std::string_view(info.type_name.c_str(), info.type_name.size()), alive);
    }

    void on_data_writer_discovery(eprosima::fastdds::dds::DomainParticipant* /*participant*/,
                                  eprosima::fastdds::rtps::WriterDiscoveryStatus reason,
                                  const eprosima::fastdds::dds::PublicationBuiltinTopicData& info,
                                  bool& /*should_be_ignored*/) override {
        if (!status_listener_ || IsCompanionChannel(info)) return;
        using eprosima::fastdds::rtps::WriterDiscoveryStatus;
        // DISCOVERED and CHANGED_QOS is "here"; REMOVED and IGNORED are not.
        const bool alive = reason == WriterDiscoveryStatus::DISCOVERED_WRITER ||
                           reason == WriterDiscoveryStatus::CHANGED_QOS_WRITER;
        status_listener_->OnWriterDiscovered(
            std::string_view(info.topic_name.c_str(), info.topic_name.size()),
            std::string_view(info.type_name.c_str(), info.type_name.size()), alive);
    }

   private:
    // Fletcher's own `<topic>/__schema` endpoints are not reported: a catalog built from discovery
    // would otherwise hold a name `Subscribe` refuses (a `__`-prefixed segment), and the schema
    // channel's type name carries no payload bound, so there is nothing to diagnose from it either.
    // The topic name suffix alone is not enough — a foreign DDS topic may legitimately end in
    // "/__schema" on its own — so the type name is checked too: only the pair identifies Fletcher's
    // own companion channel.
    template <typename BuiltinTopicData>
    static bool IsCompanionChannel(const BuiltinTopicData& info) {
        const std::string_view name(info.topic_name.c_str(), info.topic_name.size());
        const std::string_view type(info.type_name.c_str(), info.type_name.size());
        return name.ends_with("/__schema") && type == kSchemaTypeName;
    }

    FastDDSStatusListener* status_listener_;
};

}  // namespace internal
}  // namespace fletcher

#endif  // FLETCHER_FASTDDS_PUBSUB_PROVIDER_INTERNAL_PARTICIPANT_LISTENER_HPP_
