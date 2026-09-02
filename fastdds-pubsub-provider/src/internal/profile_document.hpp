// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The provider's configuration document: a **Fast DDS XML profiles document, as text** (owner
// ruling 2026-09-02). Fast DDS parses it — Fletcher gains no parser, and this provider gains no
// dependency (locked decision 8, spec §4.2). Everything here is a call into
// `get_*_qos_from_xml`, which parses a *string* and returns a QoS; `load_XML_profiles_string` is
// deliberately NOT used, because it writes into the singleton `XMLProfileManager`, where two
// provider instances with different documents under one profile name collide — the global state
// spec §4 clause 3 forbids. `FastDdsConfig.TwoInstancesResolveTheirOwnDocuments` measures that.
//
// ── The one rule that governs this file ─────────────────────────────────────────────────────
// **A resolved profile is that endpoint's WHOLE quality-of-service.** There is no merge and no
// floor: anything a supplied profile leaves out takes *Fast DDS's* default, not Fletcher's. So
// every `get_*_from_xml` call below is made into a **freshly default-constructed QoS**, and
// Fletcher's built-in is returned only on the not-found branch — never handed in as the call's
// input. That form is correct whether the substrate overwrites its output parameter or overlays
// onto it, so it needs no premise; seeding the call with the built-in would silently implement
// merge semantics under the second behaviour.
//
// MEASURED (PDA-DEC-6 implementation, fast-dds/3.4.0): seeding the call with
// `MakeFletcherDefault*Qos()` leaves the whole suite GREEN, because this version *overwrites*
// its output parameter. So the form below is NOT redundant and NOT an optimisation to remove —
// it is the only thing standing between this provider and merge semantics if a later Fast DDS
// overlays instead, and no test could tell you it had happened. Keep it.
//
// What IS asserted, and how each is falsified:
//   * `MinimalProfileTakesFastDdsDefaultsNotFletchers` — a supplied profile is the whole QoS;
//     goes red the moment the substrate starts overlaying and the seeding above is reintroduced.
//   * `AnAnchorOnlyDocumentResolvesToFletchersBuiltIn` — the not-found branch returns Fletcher's
//     built-in, whole-struct. This is the ONLY guard that catches returning Fast DDS's default
//     there: Fast DDS's writer defaults are durability TRANSIENT_LOCAL + reliability RELIABLE,
//     bit-identical to Fletcher's on both policies DDS discovery can carry.

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
#include <string>
#include <utility>
#include <vector>

#include "qos_defaults.hpp"

namespace fletcher {
namespace internal {

/// The one profile a non-empty document MUST define. `get_*_from_xml` returns `BAD_PARAMETER`
/// for both "malformed" and "no such profile", so without one mandatory anchor a broken
/// document — or an XRCE `key=value` document pasted into the wrong field — would resolve to
/// "no profiles found" and run happily on the defaults.
inline constexpr const char* kParticipantProfile = "fletcher_participant";
inline constexpr const char* kWriterProfile = "fletcher_writer";
inline constexpr const char* kReaderProfile = "fletcher_reader";

/// The two settings a DDS QoS profile has no way to express, because they are Fletcher's rather
/// than DDS's. They ride as vendor properties in the anchor's `<rtps><propertiesPolicy>` — native
/// Fast DDS XML, so still no second document format and still no second reader.
struct FletcherProperties {
    /// `fletcher.loan_publish` — which publish path. Provider-wide, which is what a participant
    /// profile is for.
    bool loan_publish = false;
    /// `fletcher.max_schema_bytes` — the `RawBytesPubSubType` bound on the internal schema channel.
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

/// Resolve the participant QoS, the anchor, and the `fletcher.*` properties in one pass.
///
/// An empty document is Fletcher's own participant: the Fast DDS default plus the
/// `FletcherParticipant` name. A **non-empty** document must define
/// `<participant profile_name="fletcher_participant">` — failing that lookup is
/// `kInvalidArgument`, in the constructor, so a misconfigured instance never exists. Note the
/// anchor may be empty of policies, and then it drops the `FletcherParticipant` name; that is
/// diagnostic-only and nothing in the tree keys on it.
///
/// The **extended** call is used so `<domainId>` cannot be dropped in silence: it returns the QoS
/// we already wanted *plus* the domain, so it replaces the plain call rather than adding one. The
/// deployment's domain always wins (ruling 2026-09-02), but a disagreement is refused quoting both
/// numbers rather than landing on domain 0 with no error. An explicit `<domainId>0</domainId>`
/// cannot be told from absent — `domainId_` defaults to 0 — and is accepted as absent.
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

    DomainParticipantExtendedQos extended;  // fresh; see the file comment
    if (DomainParticipantFactory::get_instance()->get_participant_extended_qos_from_xml(
            document, extended, kParticipantProfile) != RETCODE_OK) {
        throw PubSubError(
            PubSubStatus::kInvalidArgument,
            "FastDDS: the provider document is not a Fast DDS XML profiles document Fast DDS can "
            "parse, or it does not define <participant profile_name=\"fletcher_participant\">. "
            "Every non-empty document must define that anchor, even if it carries no policies: "
            "Fast DDS reports 'malformed' and 'no such profile' with the same code, so without it "
            "a broken document would silently run on the built-in defaults");
    }

    if (extended.domainId() != 0 && extended.domainId() != domain_id) {
        throw PubSubError(
            PubSubStatus::kInvalidArgument,
            "FastDDS: the deployment's domain always wins, and profile 'fletcher_participant' "
            "disagrees with it: <domainId> is " +
                std::to_string(extended.domainId()) + " but ProviderConfig::domain_id is " +
                std::to_string(domain_id) +
                ". An anchor's <domainId> must either match the deployment's domain or be absent");
    }

    properties = ConsumeFletcherProperties(extended.properties());
    qos = static_cast<const eprosima::fastdds::dds::DomainParticipantQos&>(extended);
}

/// Writer QoS for `topic_name`: the profile named after the topic, then `fletcher_writer`, then
/// Fletcher's built-in. Each lookup starts from a freshly default-constructed QoS — see the file
/// comment; this is the form C2-1 mandates and it is not an optimisation to remove.
inline eprosima::fastdds::dds::DataWriterQos ResolveWriterQos(
    const eprosima::fastdds::dds::Publisher& publisher, const std::string& document,
    const std::string& topic_name) {
    using eprosima::fastdds::dds::DataWriterQos;
    using eprosima::fastdds::dds::RETCODE_OK;

    if (!document.empty()) {
        DataWriterQos per_topic;
        if (publisher.get_datawriter_qos_from_xml(document, per_topic, topic_name) == RETCODE_OK) {
            return per_topic;
        }
        DataWriterQos role;
        if (publisher.get_datawriter_qos_from_xml(document, role, kWriterProfile) == RETCODE_OK) {
            return role;
        }
    }
    return MakeFletcherDefaultWriterQos();
}

/// Reader QoS for `topic_name`: the profile named after the topic, then `fletcher_reader`, then
/// Fletcher's built-in — whose `data_sharing().off()` is what holds back the measured
/// receive-side row-loss defect, so the fallback is load-bearing and not a formality.
inline eprosima::fastdds::dds::DataReaderQos ResolveReaderQos(
    const eprosima::fastdds::dds::Subscriber& subscriber, const std::string& document,
    const std::string& topic_name) {
    using eprosima::fastdds::dds::DataReaderQos;
    using eprosima::fastdds::dds::RETCODE_OK;

    if (!document.empty()) {
        DataReaderQos per_topic;
        if (subscriber.get_datareader_qos_from_xml(document, per_topic, topic_name) == RETCODE_OK) {
            return per_topic;
        }
        DataReaderQos role;
        if (subscriber.get_datareader_qos_from_xml(document, role, kReaderProfile) == RETCODE_OK) {
            return role;
        }
    }
    return MakeFletcherDefaultReaderQos();
}

}  // namespace internal
}  // namespace fletcher

#endif  // FLETCHER_FAST_DDS_SRC_INTERNAL_PROFILE_DOCUMENT_HPP_
