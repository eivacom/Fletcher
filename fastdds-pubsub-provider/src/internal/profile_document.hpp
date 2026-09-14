// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The provider's configuration document: a Fast DDS XML profiles document, as text (owner ruling
// 2026-09-02). EMPTY -- Fletcher's built-in everywhere; the registry is never consulted (rule 1).
// NON-EMPTY -- loaded ONCE per process into Fast DDS's OWN profile registry
// (DomainParticipantFactory::load_XML_profiles_string), the way eProsima's docs, its examples and
// rmw_fastrtps use it, and the registry decides every endpoint: the profile named after its topic,
// else the Publisher's/Subscriber's own default QoS, which Fast DDS itself seeds from the
// document's `is_default_profile="true"` profile if one exists (rule 2). Lookups are silent on
// miss. `XMLProfileManager`'s maps are unsynchronised statics, so every call into the registry
// below holds `profile_registry_mutex`. Profile names are process-wide, so every provider in one
// process must share one byte-identical document; the `fletcher_participant` anchor stays
// mandatory for a non-empty one because `loadXMLString` accepts a `<dds>` with no `<profiles>` as
// a silent no-op.

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
#include <fletcher/pubsub/payload_bound.hpp>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "qos_defaults.hpp"

namespace fletcher {
namespace internal {

/// The one profile a non-empty document MUST define. A miss on the registry lookup is silent
/// (`RETCODE_BAD_PARAMETER`, no log line either way), so without one mandatory anchor a document
/// that registered nothing at all -- or one that failed to parse -- would resolve to "no such
/// profile" and run happily on the built-in defaults.
inline constexpr const char* kParticipantProfile = "fletcher_participant";

/// The two settings a DDS QoS profile has no way to express, because they are Fletcher's rather
/// than DDS's. They ride as vendor properties in the anchor's `<rtps><propertiesPolicy>` — native
/// Fast DDS XML, so still no second document format and still no second reader.
struct FletcherProperties {
    /// `fletcher.loan_publish` — which publish path. Provider-wide, which is what a participant
    /// profile is for.
    bool loan_publish = false;
    /// `fletcher.max_schema_bytes` — the `SchemaBytesPubSubType` bound on the internal schema
    /// channel.
    uint32_t max_schema_bytes = 64 * 1024;
};

/// Consume the two `fletcher.*` properties and **remove them** from `properties`.
///
/// Consume what you own: the two this provider reads never reach `create_participant`, so a
/// `<propagate>true</propagate>` on one cannot put a Fletcher key into DDS discovery data. Every
/// OTHER property is left exactly as it arrived — security plugins (`dds.sec.*`) need that, and
/// `FastDdsConfig.ForeignPropertiesSurviveTheStrip` is the assert.
///
/// A `fletcher.`-prefixed property that is not one of the two, or whose value does not parse, is
/// refused `kInvalidArgument` quoting it: a typo'd `fletcher.loanpublish` must not be inert.
inline FletcherProperties ConsumeFletcherProperties(
    eprosima::fastdds::dds::PropertyPolicyQos& properties) {
    using eprosima::fastdds::rtps::Property;
    using eprosima::fastdds::rtps::PropertySeq;

    FletcherProperties out;
    PropertySeq kept;
    kept.reserve(properties.properties().size());

    for (Property& property : properties.properties()) {
        const std::string& name = property.name();
        if (name.rfind("fletcher.", 0) != 0) {
            kept.push_back(std::move(property));
            continue;
        }
        const std::string& value = property.value();
        if (name == "fletcher.loan_publish") {
            if (value == "true") {
                out.loan_publish = true;
            } else if (value == "false") {
                out.loan_publish = false;
            } else {
                throw PubSubError(PubSubStatus::kInvalidArgument,
                                  "FastDDS: property 'fletcher.loan_publish' must be 'true' or "
                                  "'false', not '" +
                                      value + "'");
            }
        } else if (name == "fletcher.max_schema_bytes") {
            uint64_t parsed = 0;
            bool ok = !value.empty() && value.size() <= 10;
            for (char c : value) {
                if (c < '0' || c > '9') {
                    ok = false;
                    break;
                }
                parsed = parsed * 10 + static_cast<uint64_t>(c - '0');
            }
            if (!ok || parsed == 0 || parsed > 0xFFFFFFFFull) {
                throw PubSubError(
                    PubSubStatus::kInvalidArgument,
                    "FastDDS: property 'fletcher.max_schema_bytes' must be a positive integer "
                    "that fits in 32 bits, not '" +
                        value + "'");
            }
            // The schema channel's TopicDataType is FletcherSamplePubSubType too (under the name
            // `SchemaBytes`), so its bound has to obey the same rule as `max_payload_bytes`.
            if (!IsPayloadBound(static_cast<uint32_t>(parsed))) {
                throw PubSubError(
                    PubSubStatus::kInvalidArgument,
                    "FastDDS: property 'fletcher.max_schema_bytes' " + value +
                        " cannot bound the schema channel's sample; it must be a multiple of 4 "
                        "between " +
                        std::to_string(kMinPayloadBytes) + " and " +
                        std::to_string(kMaxPayloadBytes));
            }
            out.max_schema_bytes = static_cast<uint32_t>(parsed);
        } else {
            throw PubSubError(PubSubStatus::kInvalidArgument,
                              "FastDDS: unknown 'fletcher.' property in the profiles document: '" +
                                  name +
                                  "' (the only two are 'fletcher.loan_publish' and "
                                  "'fletcher.max_schema_bytes')");
        }
    }

    properties.properties() = std::move(kept);
    return out;
}

/// `XMLProfileManager`'s maps are unsynchronised statics; every call below into Fast DDS's
/// registry -- loading a document, or looking a profile up -- holds this.
inline std::mutex profile_registry_mutex;

/// Load `document` into Fast DDS's process-wide profile registry, once. Caller holds
/// `profile_registry_mutex`. The refusal is FLETCHER'S: `XMLProfileManager::extractProfiles`
/// turns a colliding profile name into `XML_NOK` and keeps going, so `load_XML_profiles_string`
/// returns `RETCODE_OK` for a second, different document that registers even one new name --
/// this function tracks the process's one document itself. A document that loads here and is
/// later refused by Fletcher's own checks (bad anchor, `fletcher.*` value, domain) already IS it.
inline void LoadDocumentOnce(const std::string& document) {
    // The process's document; empty until a load succeeds. Never empty on entry: callers only
    // reach here when `!document.empty()`.
    static std::string loaded;
    if (!loaded.empty()) {
        if (loaded == document) return;
        throw PubSubError(
            PubSubStatus::kInvalidArgument,
            "FastDDS: a different provider document is already loaded in this process. Fast DDS "
            "profile names are process-wide and cannot be unloaded, so every "
            "FastDDSPubSubProvider in one process must be built from the same (byte-identical) "
            "document, or an empty one");
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

/// Resolve the participant QoS, the anchor, and the `fletcher.*` properties in one pass.
///
/// An empty document is Fletcher's own participant: the Fast DDS default plus the
/// `FletcherParticipant` name, and the registry is not consulted at all (rule 1) -- not even to
/// look the anchor up. A **non-empty** document must define
/// `<participant profile_name="fletcher_participant">` — failing that lookup is
/// `kInvalidArgument`, in the constructor, so a misconfigured instance never exists.
///
/// The **extended** lookup is used so `<domainId>` cannot be dropped in silence: it returns the
/// QoS we already wanted *plus* the domain, so it replaces the plain lookup rather than adding
/// one. The deployment's domain always wins (ruling 2026-09-02), but a disagreement is refused
/// quoting both numbers rather than landing on domain 0 with no error. An explicit
/// `<domainId>0</domainId>` cannot be told from absent — `domainId_` defaults to 0 — and is
/// accepted as absent.
inline void ResolveParticipantQos(const std::string& document, uint32_t domain_id,
                                  eprosima::fastdds::dds::DomainParticipantQos& qos,
                                  FletcherProperties& properties) {
    using eprosima::fastdds::dds::DomainParticipantExtendedQos;
    using eprosima::fastdds::dds::DomainParticipantFactory;
    using eprosima::fastdds::dds::PARTICIPANT_QOS_DEFAULT;
    using eprosima::fastdds::dds::RETCODE_OK;

    if (document.empty()) {
        qos = PARTICIPANT_QOS_DEFAULT;
        qos.name("FletcherParticipant");
        properties = FletcherProperties{};
        return;
    }

    auto* factory = DomainParticipantFactory::get_instance();
    std::lock_guard<std::mutex> lock(profile_registry_mutex);
    factory->load_profiles();  // idempotent; the same call create_participant makes later
    LoadDocumentOnce(document);

    DomainParticipantExtendedQos extended;
    if (factory->get_participant_extended_qos_from_profile(kParticipantProfile, extended) !=
        RETCODE_OK) {
        throw PubSubError(
            PubSubStatus::kInvalidArgument,
            "FastDDS: the provider document does not define "
            "<participant profile_name=\"fletcher_participant\">. Every non-empty document must "
            "define that anchor: Fast DDS accepts a document with no <profiles> element as a "
            "silent no-op (XMLProfileManager::loadXMLString), so without it a document that "
            "registered nothing would run on the built-in defaults unnoticed");
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
    properties = ConsumeFletcherProperties(extended.properties());
    qos = static_cast<const eprosima::fastdds::dds::DomainParticipantQos&>(extended);
}

/// The per-topic profile if the registry has one, else the Publisher's own default QoS.
///
/// `registry` false (empty document, rule 1) skips the registry outright -- not even for another
/// provider's document already loaded in this process -- and returns the Publisher's default QoS
/// directly: the caller has already set it to Fletcher's built-in (fast_dds_pubsub_provider.cpp).
/// `registry` true (non-empty document, rule 2) tries the profile named after `topic_name` first;
/// on a miss it falls to the Publisher's own default QoS, which Fast DDS itself seeded from the
/// document's `is_default_profile="true"` `<data_writer>` profile if the document had one, else
/// Fast DDS's own default. `fletcher_writer` is not a special name any more.
inline eprosima::fastdds::dds::DataWriterQos ResolveWriterQos(
    const eprosima::fastdds::dds::Publisher& publisher, const std::string& topic_name,
    bool registry) {
    using eprosima::fastdds::dds::DataWriterQos;
    using eprosima::fastdds::dds::RETCODE_OK;

    if (registry) {
        std::lock_guard<std::mutex> lock(profile_registry_mutex);
        DataWriterQos qos;  // fresh out-param -- a resolved profile is that endpoint's WHOLE QoS
                            // (owner ruling 2026-09-02), never merged with anything
        if (publisher.get_datawriter_qos_from_profile(topic_name, qos) == RETCODE_OK) return qos;
    }
    return publisher.get_default_datawriter_qos();
}

/// The reader mirror of `ResolveWriterQos`.
inline eprosima::fastdds::dds::DataReaderQos ResolveReaderQos(
    const eprosima::fastdds::dds::Subscriber& subscriber, const std::string& topic_name,
    bool registry) {
    using eprosima::fastdds::dds::DataReaderQos;
    using eprosima::fastdds::dds::RETCODE_OK;

    if (registry) {
        std::lock_guard<std::mutex> lock(profile_registry_mutex);
        DataReaderQos qos;
        if (subscriber.get_datareader_qos_from_profile(topic_name, qos) == RETCODE_OK) return qos;
    }
    return subscriber.get_default_datareader_qos();
}

}  // namespace internal
}  // namespace fletcher

#endif  // FLETCHER_FAST_DDS_SRC_INTERNAL_PROFILE_DOCUMENT_HPP_
