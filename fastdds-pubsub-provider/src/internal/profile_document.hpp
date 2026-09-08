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

/// Could `document` possibly define a profile called `profile_name`?
///
/// Why this exists: `get_*_qos_from_xml` logs a miss at **ERROR** level
/// (`[XMLPARSER Error] Publisher profile not found`), and the ladders below use a miss as
/// ordinary control flow. Left alone, the most common document there is — an anchor plus one
/// `fletcher.*` property — printed four `[XMLPARSER Error]` lines per topic on a **correct**
/// configuration, which both alarms an operator reading a clean start-up and buries the one line
/// that IS an error (review 4b S3). So a lookup that provably cannot succeed is not made at all.
///
/// This is a proof, not a heuristic, and it can only ever err towards asking Fast DDS anyway: a
/// profile named `N` is written `profile_name="N"`, so `N` occurs literally in the document -
/// UNLESS the name reached the parser through one of the **two** channels by which an attribute
/// value can differ from the text between its quotes (review 4c F9): an entity or character
/// reference, which needs an `&` **in the document**; or whitespace normalisation — a literal
/// tab/CR/LF inside the quotes is yielded as a space, a CR/CRLF line end as an LF — which can
/// only make the yielded name differ from the document text at a position where the **yielded
/// name** holds whitespace. So a document with no `&`, asked for a name with no whitespace,
/// cannot be naming a profile that is not a literal substring of it; either precondition sends
/// the lookup to Fast DDS unconditionally, log line and all. The second is tested on the
/// requested name and NOT on the document because an ordinary document is full of LFs between
/// elements, where they are not attribute values and normalise nothing — scanning the document
/// for them would skip no lookup at all and hand back the whole `[XMLPARSER Error]` flood this
/// exists to remove. Nothing here decides a QoS — being wrong towards the lookup costs a log
/// line, never a policy. MEASURED on fast-dds/3.4.0: asking for `a b/topic` against a document
/// holding `profile_name="a<LF>b/topic"` misses, so that parser does not apply attribute-value
/// normalisation today; the second hatch is completeness for a parser that does (and for line-end
/// normalisation), and it costs one lookup only for a name that contains whitespace.
inline bool DocumentMayName(const std::string& document, const std::string& profile_name) {
    // Both hatches are load-bearing: dropping either skips a lookup that CAN succeed, which
    // silently changes a QoS. `FastDdsConfig.ALookupThatCannotSucceedIsNotAttempted` reddens on
    // the removal of either one.
    if (document.find('&') != std::string::npos) return true;  // cannot reason — ask Fast DDS
    if (profile_name.find_first_of(" \t\r\n") != std::string::npos) return true;
    return document.find(profile_name) != std::string::npos;
}

/// Could `document` define ANY data-writer profile? One rung coarser than `DocumentMayName`, and
/// it needs no `&` caveat: XML **element** names can never carry a reference, so a writer profile
/// is a literal `<data_writer>` (or `<publisher>`, the pre-3.x spelling) and one of those two
/// words occurs in the text. This is what keeps a document mentioning a topic name only inside a
/// `<topic><name>` from probing for a writer profile named after that topic.
inline bool DocumentMayDefineWriterProfile(const std::string& document) {
    return document.find("writer") != std::string::npos ||
           document.find("publisher") != std::string::npos;
}

/// The reader counterpart of `DocumentMayDefineWriterProfile`.
inline bool DocumentMayDefineReaderProfile(const std::string& document) {
    return document.find("reader") != std::string::npos ||
           document.find("subscriber") != std::string::npos;
}

/// Refuse a `fletcher.*` property found on a resolved WRITER or READER profile.
///
/// The two Fletcher settings are provider-wide and are read from the participant anchor and
/// nowhere else. A correctly spelled `fletcher.loan_publish` inside a `<data_writer>` profile is
/// not a setting in a slightly odd place — it is silently inert, which is exactly the failure a
/// *misspelled* name is already refused for (review 4a F5). MEASURED on fast-dds/3.4.0: a
/// `<propertiesPolicy>` that is a direct child of `<data_writer>` / `<data_reader>` arrives in
/// `Data{Writer,Reader}Qos::properties()`, so it is observable and therefore refused; the other
/// placement, inside `<qos>`, is rejected by Fast DDS's own parser and so is already refused as a
/// malformed document. Neither placement is inert any more.
inline void RefuseFletcherPropertiesOutsideTheAnchor(
    const eprosima::fastdds::dds::PropertyPolicyQos& properties, const char* element,
    const std::string& profile_name) {
    for (const auto& property : properties.properties()) {
        if (property.name().rfind("fletcher.", 0) != 0) continue;
        throw PubSubError(
            PubSubStatus::kInvalidArgument,
            "FastDDS: property '" + property.name() + "' appears in the <" + element +
                "> profile '" + profile_name +
                "', where the provider never reads it, so it would have no effect at all. Both "
                "'fletcher.' settings are provider-wide and are read ONLY from the anchor's "
                "<rtps><propertiesPolicy> in <participant profile_name=\"fletcher_participant\">");
    }
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
///
/// The `DocumentMay*` guards ahead of each lookup change no answer: they skip only a lookup that
/// cannot succeed, whose one effect would be an `[XMLPARSER Error]` line on a correct
/// configuration. Read the proofs above them before touching them.
inline eprosima::fastdds::dds::DataWriterQos ResolveWriterQos(
    const eprosima::fastdds::dds::Publisher& publisher, const std::string& document,
    const std::string& topic_name) {
    using eprosima::fastdds::dds::DataWriterQos;
    using eprosima::fastdds::dds::RETCODE_OK;

    if (!document.empty() && DocumentMayDefineWriterProfile(document)) {
        if (DocumentMayName(document, topic_name)) {
            DataWriterQos per_topic;
            if (publisher.get_datawriter_qos_from_xml(document, per_topic, topic_name) ==
                RETCODE_OK) {
                RefuseFletcherPropertiesOutsideTheAnchor(per_topic.properties(), "data_writer",
                                                         topic_name);
                return per_topic;
            }
        }
        if (DocumentMayName(document, kWriterProfile)) {
            DataWriterQos role;
            if (publisher.get_datawriter_qos_from_xml(document, role, kWriterProfile) ==
                RETCODE_OK) {
                RefuseFletcherPropertiesOutsideTheAnchor(role.properties(), "data_writer",
                                                         kWriterProfile);
                return role;
            }
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

    if (!document.empty() && DocumentMayDefineReaderProfile(document)) {
        if (DocumentMayName(document, topic_name)) {
            DataReaderQos per_topic;
            if (subscriber.get_datareader_qos_from_xml(document, per_topic, topic_name) ==
                RETCODE_OK) {
                RefuseFletcherPropertiesOutsideTheAnchor(per_topic.properties(), "data_reader",
                                                         topic_name);
                return per_topic;
            }
        }
        if (DocumentMayName(document, kReaderProfile)) {
            DataReaderQos role;
            if (subscriber.get_datareader_qos_from_xml(document, role, kReaderProfile) ==
                RETCODE_OK) {
                RefuseFletcherPropertiesOutsideTheAnchor(role.properties(), "data_reader",
                                                         kReaderProfile);
                return role;
            }
        }
    }
    return MakeFletcherDefaultReaderQos();
}

/// Refuse a misplaced `fletcher.*` property in either ROLE profile, at construction.
///
/// A per-topic profile can only be checked when its topic is created — a document cannot be
/// enumerated, only asked about a name — and `Resolve{Writer,Reader}Qos` do that. The two role
/// profiles have names the provider knows up front, so the common case is refused before the
/// participant exists (spec §5.1 rung 2), like every other document refusal in this item.
inline void RefuseMisplacedFletcherPropertiesInRoleProfiles(
    const eprosima::fastdds::dds::Publisher& publisher,
    const eprosima::fastdds::dds::Subscriber& subscriber, const std::string& document) {
    using eprosima::fastdds::dds::DataReaderQos;
    using eprosima::fastdds::dds::DataWriterQos;
    using eprosima::fastdds::dds::RETCODE_OK;

    if (document.empty()) return;

    if (DocumentMayDefineWriterProfile(document) && DocumentMayName(document, kWriterProfile)) {
        DataWriterQos writer;
        if (publisher.get_datawriter_qos_from_xml(document, writer, kWriterProfile) == RETCODE_OK) {
            RefuseFletcherPropertiesOutsideTheAnchor(writer.properties(), "data_writer",
                                                     kWriterProfile);
        }
    }
    if (DocumentMayDefineReaderProfile(document) && DocumentMayName(document, kReaderProfile)) {
        DataReaderQos reader;
        if (subscriber.get_datareader_qos_from_xml(document, reader, kReaderProfile) ==
            RETCODE_OK) {
            RefuseFletcherPropertiesOutsideTheAnchor(reader.properties(), "data_reader",
                                                     kReaderProfile);
        }
    }
}

}  // namespace internal
}  // namespace fletcher

#endif  // FLETCHER_FAST_DDS_SRC_INTERNAL_PROFILE_DOCUMENT_HPP_
