// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The provider's configuration document: a Fast DDS XML profiles document, as text. Never empty
// by the time it reaches here: an empty ProviderConfig::document resolves to Fletcher's baked-in
// one (qos_defaults.cpp) first, so there is one path, always -- load ONCE per process into Fast
// DDS's OWN profile registry (DomainParticipantFactory::load_XML_profiles_string), the way
// eProsima's docs and rmw_fastrtps use it; the registry then decides every endpoint, by profile
// name, silent on miss. `XMLProfileManager`'s maps are unsynchronised statics, so every registry
// call below holds `profile_registry_mutex`. Profile names are process-wide, so every provider in
// a process shares one byte-identical document -- an empty one and a non-empty one collide on that
// rule too. The `fletcher_participant` anchor stays mandatory: `loadXMLString` accepts a `<dds>`
// with no `<profiles>` as a silent no-op.

#ifndef FLETCHER_FAST_DDS_SRC_INTERNAL_PROFILE_DOCUMENT_HPP_
#define FLETCHER_FAST_DDS_SRC_INTERNAL_PROFILE_DOCUMENT_HPP_

#include <cstdint>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/domain/qos/DomainParticipantExtendedQos.hpp>
#include <fastdds/dds/domain/qos/DomainParticipantQos.hpp>
#include <fastdds/dds/publisher/Publisher.hpp>
#include <fastdds/dds/publisher/qos/DataWriterQos.hpp>
#include <fastdds/dds/subscriber/Subscriber.hpp>
#include <fastdds/dds/subscriber/qos/DataReaderQos.hpp>
#include <fletcher/core/status.hpp>
#include <mutex>
#include <string>

namespace fletcher {
namespace internal {

/// `XMLProfileManager`'s maps are unsynchronised statics; every call below into Fast DDS's
/// registry -- loading a document, or looking a profile up -- holds this.
inline std::mutex profile_registry_mutex;

/// Load `document` into Fast DDS's process-wide profile registry, once. Caller holds
/// `profile_registry_mutex`. The refusal is FLETCHER'S: `XMLProfileManager::extractProfiles`
/// turns a colliding profile name into `XML_NOK` and keeps going, so `load_XML_profiles_string`
/// returns `RETCODE_OK` for a second, different document that registers even one new name --
/// this function tracks the process's one document itself. A document that loads here and is
/// later refused by Fletcher's own checks (bad anchor, domain) already IS it. Fletcher's own
/// baked-in document (qos_defaults.cpp) is a document like any other here: two providers in one
/// process must carry the same bytes, so an empty `ProviderConfig::document` and a non-empty one
/// COLLIDE, and whichever loads second is refused -- an accepted consequence of there being
/// exactly one path.
inline void LoadDocumentOnce(const std::string& document) {
    // The process's document; empty until a load succeeds. Never empty on entry: the constructor
    // substitutes the built-in document for an empty one.
    static std::string loaded;
    if (!loaded.empty()) {
        if (loaded == document) return;
        throw PubSubError(
            PubSubStatus::kInvalidArgument,
            "FastDDS: a different provider document is already loaded in this process. Fast DDS "
            "profile names are process-wide and cannot be unloaded, so every "
            "FastDDSPubSubProvider in one process must be built from the same (byte-identical) "
            "document");
    }
    if (eprosima::fastdds::dds::DomainParticipantFactory::get_instance()->load_XML_profiles_string(
            document.data(), document.size()) != eprosima::fastdds::dds::RETCODE_OK) {
        throw PubSubError(PubSubStatus::kInvalidArgument,
                          "FastDDS: the provider document is not a Fast DDS XML profiles document "
                          "Fast DDS can parse (DomainParticipantFactory::load_XML_profiles_string "
                          "refused it)");
    }
    loaded = document;
}

/// Resolve the participant QoS and the anchor in one pass. `document` is never empty here (an
/// empty `ProviderConfig::document` already became Fletcher's baked-in one by the time
/// fast_dds_pubsub_provider.cpp calls this), so there is exactly one path: load it, require the
/// anchor, slice its QoS out.
///
/// The **extended** lookup is used so `<domainId>` cannot be dropped in silence: it returns the
/// QoS we already wanted *plus* the domain, so it replaces the plain lookup rather than adding
/// one. The deployment's domain always wins, but a disagreement is refused
/// quoting both numbers rather than landing on domain 0 with no error. An explicit
/// `<domainId>0</domainId>` cannot be told from absent — `domainId_` defaults to 0 — and is
/// accepted as absent.
inline void ResolveParticipantQos(const std::string& document, uint32_t domain_id,
                                  eprosima::fastdds::dds::DomainParticipantQos& qos) {
    using eprosima::fastdds::dds::DomainParticipantExtendedQos;
    using eprosima::fastdds::dds::DomainParticipantFactory;
    using eprosima::fastdds::dds::RETCODE_OK;

    auto* factory = DomainParticipantFactory::get_instance();
    std::lock_guard<std::mutex> lock(profile_registry_mutex);
    factory->load_profiles();  // idempotent; the same call create_participant makes later
    LoadDocumentOnce(document);
    DomainParticipantExtendedQos extended;
    if (factory->get_participant_extended_qos_from_profile("fletcher_participant", extended) !=
        RETCODE_OK) {
        throw PubSubError(
            PubSubStatus::kInvalidArgument,
            "FastDDS: the provider document does not define "
            "<participant profile_name=\"fletcher_participant\">. Every document must "
            "define that anchor: Fast DDS accepts a document with no <profiles> element as a "
            "silent no-op (XMLProfileManager::loadXMLString), so without it a document that "
            "registered nothing would run on Fast DDS's own defaults unnoticed");
    }
    if (extended.domainId() != 0 && extended.domainId() != domain_id) {
        throw PubSubError(
            PubSubStatus::kInvalidArgument,
            "FastDDS: the deployment's domain always wins, and profile 'fletcher_participant' "
            "disagrees with it: <domainId> is " +
                std::to_string(extended.domainId()) + " but ProviderConfig::domain_id is " +
                std::to_string(domain_id) +
                ". An anchor's <domainId> must either match the deployment's domain or be "
                "absent");
    }
    qos = static_cast<const eprosima::fastdds::dds::DomainParticipantQos&>(extended);
}

/// The per-topic profile if the registry has one, else the Publisher's own default QoS -- which
/// Fast DDS itself seeded from the document's `is_default_profile="true"` `<data_writer>` profile
/// if it had one, else Fast DDS's own default. `fletcher_writer` is not a special name.
///
/// `profile`, when non-empty, is a `TopicOptions::profile` naming a `<data_writer>` profile
/// directly -- `topic_name` plays no part then, and a name the document does not define throws
/// `PubSubError(kInvalidArgument)` naming it, rather than falling back the way a topic-name miss
/// does. Empty (the default) is today's lookup: `topic_name`'s own profile if the registry has
/// one, else the Publisher's default.
inline eprosima::fastdds::dds::DataWriterQos ResolveDataWriterQos(
    const eprosima::fastdds::dds::Publisher& publisher, const std::string& topic_name,
    const std::string& profile = "") {
    using eprosima::fastdds::dds::DataWriterQos;
    using eprosima::fastdds::dds::RETCODE_OK;

    std::lock_guard<std::mutex> lock(profile_registry_mutex);
    DataWriterQos qos;  // fresh out-param -- a resolved profile is that endpoint's WHOLE QoS,
                        // never merged with anything
    if (!profile.empty()) {
        if (publisher.get_datawriter_qos_from_profile(profile, qos) == RETCODE_OK) return qos;
        throw PubSubError(
            PubSubStatus::kInvalidArgument,
            "FastDDS: the document defines no <data_writer> profile named '" + profile + "'");
    }
    if (publisher.get_datawriter_qos_from_profile(topic_name, qos) == RETCODE_OK) return qos;
    return publisher.get_default_datawriter_qos();
}

/// The reader mirror of `ResolveDataWriterQos`, `profile` included.
inline eprosima::fastdds::dds::DataReaderQos ResolveDataReaderQos(
    const eprosima::fastdds::dds::Subscriber& subscriber, const std::string& topic_name,
    const std::string& profile = "") {
    using eprosima::fastdds::dds::DataReaderQos;
    using eprosima::fastdds::dds::RETCODE_OK;

    std::lock_guard<std::mutex> lock(profile_registry_mutex);
    DataReaderQos qos;
    if (!profile.empty()) {
        if (subscriber.get_datareader_qos_from_profile(profile, qos) == RETCODE_OK) return qos;
        throw PubSubError(
            PubSubStatus::kInvalidArgument,
            "FastDDS: the document defines no <data_reader> profile named '" + profile + "'");
    }
    if (subscriber.get_datareader_qos_from_profile(topic_name, qos) == RETCODE_OK) return qos;
    return subscriber.get_default_datareader_qos();
}

}  // namespace internal
}  // namespace fletcher

#endif  // FLETCHER_FAST_DDS_SRC_INTERNAL_PROFILE_DOCUMENT_HPP_
