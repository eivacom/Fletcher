// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// PDA-DEC-6 — the provider is configured by a Fast DDS XML profiles document, and nothing else.
//
// ── Why these tests are shaped the way they are ─────────────────────────────────────────────
// A supplied profile is that endpoint's WHOLE quality-of-service (owner ruling 2026-09-02), so
// **silence is load-bearing**: what a document FAILS to say decides an endpoint's QoS outright.
// Every test below is judged by one question — would it go red if the document never reached
// Fast DDS, or if a policy silently fell back? Six silences are guarded here:
//
//   1. an empty document                     -> Fletcher's baked-in one, loaded like any other
//                                               (forcing row 1;
//                                               internal::FletcherDefaultProfilesDocument())
//   2. an anchor-only document               -> Fast DDS's own default   (forcing row 2)
//   3. a whole profile absent for one role   -> Fast DDS's own default   (SchemaChannel..., C2-2)
//   4. a whole document that will not parse  -> refused, never defaulted  (Malformed...)
//   5. the POLICIES a supplied profile omits -> Fast DDS's default, NOT Fletcher's
//                                               (MinimalProfileTakesFastDdsDefaultsNotFletchers)
//   6. an EMPTY document                     -> the SAME writer/reader QoS a document holding
//                                               FletcherDefaultProfilesDocument()'s own text would
//                                               (AnEmptyDocumentIsFletchersBuiltInEverywhere); a
//                                               document with no reader profile falls to Fast
//                                               DDS's own default instead, not Fletcher's
//                                               (WriterOnlyDocumentLeavesTheReaderOnFastDdsDefault)
//
// ── Two shapes of assertion, and why both are needed ───────────────────────────────────────
// Where the claim is that a QoS reached a live ENDPOINT, the value is read out of DDS discovery
// from a bare observer participant, with a **per-row latch and a hard timeout**: nothing is
// compared until that row's discovery callback has fired, because a row expecting a value that
// happens to equal a default-constructed policy would otherwise prove nothing while staying
// green. Where the claim is about RESOLUTION, the QoS structs are compared WHOLE and in-process
// — no discovery, every policy — because `history` and `resource_limits` are `optional` in
// `PublicationBuiltinTopicData` and are simply not observable on the network.
//
// ── One TEST, at most one distinct document ─────────────────────────────────────────────────
// Fast DDS's profile registry is process-wide and keeps a profile for the life of the process
// (internal/profile_document.hpp): a second document naming a profile another document in this
// process already registered is refused, even if it is refused only after the first has already
// won. An EMPTY document is no exception any more (owner decision 2026-09-15): it resolves to
// Fletcher's baked-in one and is loaded like any other, so it collides with a different document
// exactly the way two different non-empty ones do. So within ONE `TEST()` every provider must be
// built from the SAME document bytes, and a TEST may load at most one distinct document — a TEST
// whose second, different document is MEANT to be refused is the exception. `gtest_discover_tests(
// ... DISCOVERY_MODE PRE_TEST)` in `tests/CMakeLists.txt` already runs one `TEST()` per process
// under CTest, which is what makes this workable; running this binary bare on the whole suite
// collides by design (`FastDdsConfig.*` run together would fight over the same profile names).

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/domain/DomainParticipantListener.hpp>
#include <fastdds/dds/log/Log.hpp>
#include <fastdds/dds/publisher/Publisher.hpp>
#include <fastdds/dds/subscriber/Subscriber.hpp>
#include <fletcher/core/write_buffer.hpp>
#include <fletcher/fastdds_pubsub_provider/fast_dds_pubsub_provider.hpp>
#include <fletcher/pubsub/schema_ipc.hpp>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "internal/profile_document.hpp"
#include "internal/qos_defaults.hpp"

using namespace fletcher;
using namespace eprosima::fastdds::dds;

namespace {

// ---------------------------------------------------------------------------
// Documents
// ---------------------------------------------------------------------------

// The provider README, by absolute path from `target_compile_definitions` (the tree's convention
// for a test that reads a source-controlled artefact — see protoc-coverage's
// PARITY_GOLDEN_DIR_PATH). `DefaultProfileTranscriptionIsExact` reads the published starting-point
// block out of it instead of holding a copy, so the README's drift claim is enforced rather than
// asserted by hand.
#ifndef FLETCHER_FASTDDS_README_PATH
#error "FLETCHER_FASTDDS_README_PATH is not defined; see tests/CMakeLists.txt"
#endif
constexpr const char* kReadmePath = FLETCHER_FASTDDS_README_PATH;

std::string ReadWholeFile(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

// The first ```xml fenced block after `marker`. Empty if either is missing — the caller asserts,
// so a renamed heading is a red test and never a silently skipped one.
std::string ExtractFencedXmlAfter(const std::string& text, const std::string& marker) {
    const size_t at = text.find(marker);
    if (at == std::string::npos) return {};
    const size_t fence = text.find("```xml", at);
    if (fence == std::string::npos) return {};
    const size_t body = text.find('\n', fence);
    if (body == std::string::npos) return {};
    const size_t close = text.find("\n```", body);
    if (close == std::string::npos) return {};
    return text.substr(body + 1, close - body - 1);
}

// Every non-empty document must carry this anchor, even empty of policies: Fast DDS accepts a
// document with no <profiles> element as a silent no-op (XMLProfileManager::loadXMLString),
// registering nothing and erroring at neither load nor lookup, so one mandatory profile is the
// only self-identification a document can carry.
constexpr const char* kAnchorOnly =
    R"(<?xml version="1.0" encoding="UTF-8"?>
<dds xmlns="http://www.eprosima.com/XMLSchemas/fastRTPS_Profiles">
  <profiles>
    <participant profile_name="fletcher_participant"/>
  </profiles>
</dds>)";

// A document with the anchor plus whatever `body` adds. `body` goes inside <profiles>.
std::string Document(const std::string& body, const std::string& anchor_body = "") {
    return R"(<?xml version="1.0" encoding="UTF-8"?>
<dds xmlns="http://www.eprosima.com/XMLSchemas/fastRTPS_Profiles">
  <profiles>
    <participant profile_name="fletcher_participant">)" +
           anchor_body + R"(</participant>
)" + body + R"(
  </profiles>
</dds>)";
}

// `is_default` marks the profile `is_default_profile="true"`: Fast DDS seeds the Publisher's /
// Subscriber's own default QoS from it when each is created (rule 2,
// internal/profile_document.hpp). `fletcher_writer` / `fletcher_reader` are not special names any
// more -- a document that wants a role's default must say so explicitly, same as the operator
// would.
std::string WriterProfile(const std::string& profile_name, const std::string& qos_body,
                          const std::string& topic_body = "", bool is_default = false) {
    return R"(    <data_writer profile_name=")" + profile_name + R"(")" +
           (is_default ? R"( is_default_profile="true")" : std::string()) + R"(>
      <qos>)" +
           qos_body +
           R"(</qos>
      <topic>)" +
           topic_body +
           R"(</topic>
    </data_writer>)";
}

std::string ReaderProfile(const std::string& profile_name, const std::string& qos_body,
                          const std::string& topic_body = "", bool is_default = false) {
    return R"(    <data_reader profile_name=")" + profile_name + R"(")" +
           (is_default ? R"( is_default_profile="true")" : std::string()) + R"(>
      <qos>)" +
           qos_body +
           R"(</qos>
      <topic>)" +
           topic_body +
           R"(</topic>
    </data_reader>)";
}

// Ten slots of the payload bound rather than Fletcher's twenty-five: a bounded plain type reserves
// the whole bound per history slot per endpoint, and the loaned tests below want a pool small
// enough to exhaust deliberately.
constexpr const char* kTenSlots = R"(
        <historyQos><kind>KEEP_LAST</kind><depth>10</depth></historyQos>
        <resourceLimitsQos>
          <max_samples>10</max_samples>
          <max_instances>1</max_instances>
          <max_samples_per_instance>10</max_samples_per_instance>
          <allocated_samples>10</allocated_samples>
        </resourceLimitsQos>)";

// ---------------------------------------------------------------------------
// Schema / row helpers (same shape as the main provider TU)
// ---------------------------------------------------------------------------

OwnedSchema MakeSchema() {
    OwnedSchema s;
    ArrowSchemaInit(s.get());
    ArrowSchemaSetTypeStruct(s.get(), 1);
    ArrowSchemaSetName(s->children[0], "x");
    ArrowSchemaSetType(s->children[0], NANOARROW_TYPE_INT32);
    return s;
}

PubSubProvider::RowEncoder MakeEncoder(int32_t x) {
    return [x](WriteBuffer& buf) {
        buf.AppendByte(0x00);
        buf.AppendFixed<int32_t>(x);
    };
}

int32_t DecodeRow(const uint8_t* data) {
    int32_t v;
    std::memcpy(&v, data + 1, sizeof(v));
    return v;
}

// Many columns with long names: comfortably past `kSchemaPayloadBytes - 37` once IPC-serialized,
// for the two tests that need a schema the fixed `__schema` bound refuses.
OwnedSchema MakeOversizedSchema() {
    constexpr int kColumns = 2000;
    OwnedSchema s;
    ArrowSchemaInit(s.get());
    ArrowSchemaSetTypeStruct(s.get(), kColumns);
    for (int i = 0; i < kColumns; ++i) {
        ArrowSchemaSetName(s->children[i],
                           ("column_with_a_reasonably_long_name_" + std::to_string(i)).c_str());
        ArrowSchemaSetType(s->children[i], NANOARROW_TYPE_INT32);
    }
    return s;
}

// ---------------------------------------------------------------------------
// The discovery observer: what an endpoint ANNOUNCED, read off the network
// ---------------------------------------------------------------------------

// What discovery data actually carries for an endpoint. `history` and `resource_limits` are
// deliberately absent: they are `fastcdr::optional` in the builtin topic data and are not
// propagated, which is why the resolution claims are asserted in-process instead.
struct Announced {
    DurabilityQosPolicyKind durability{};
    ReliabilityQosPolicyKind reliability{};
    DataSharingKind data_sharing{};
};

// A participant that creates no endpoint of its own and only listens. Because it is on the same
// domain as the provider under test, it sees every writer and reader that provider creates.
class DiscoveryObserver : public DomainParticipantListener {
   public:
    explicit DiscoveryObserver(uint32_t domain) {
        DomainParticipantQos qos;
        qos.name("FletcherTestObserver");
        participant_ = DomainParticipantFactory::get_instance()->create_participant(
            domain, qos, this, StatusMask::none());
    }

    ~DiscoveryObserver() override {
        if (participant_) {
            DomainParticipantFactory::get_instance()->delete_participant(participant_);
        }
    }

    DiscoveryObserver(const DiscoveryObserver&) = delete;
    DiscoveryObserver& operator=(const DiscoveryObserver&) = delete;

    bool ok() const { return participant_ != nullptr; }

    void on_data_writer_discovery(DomainParticipant*,
                                  eprosima::fastdds::rtps::WriterDiscoveryStatus reason,
                                  const eprosima::fastdds::rtps::PublicationBuiltinTopicData& info,
                                  bool& should_be_ignored) override {
        should_be_ignored = false;
        if (reason != eprosima::fastdds::rtps::WriterDiscoveryStatus::DISCOVERED_WRITER) return;
        std::lock_guard<std::mutex> lock(mu_);
        writers_[info.topic_name.to_string()] =
            Announced{info.durability.kind, info.reliability.kind, info.data_sharing.kind()};
        cv_.notify_all();
    }

    void on_data_reader_discovery(DomainParticipant*,
                                  eprosima::fastdds::rtps::ReaderDiscoveryStatus reason,
                                  const eprosima::fastdds::rtps::SubscriptionBuiltinTopicData& info,
                                  bool& should_be_ignored) override {
        should_be_ignored = false;
        if (reason != eprosima::fastdds::rtps::ReaderDiscoveryStatus::DISCOVERED_READER) return;
        std::lock_guard<std::mutex> lock(mu_);
        readers_[info.topic_name.to_string()] =
            Announced{info.durability.kind, info.reliability.kind, info.data_sharing.kind()};
        cv_.notify_all();
    }

    // A propagated ("<propagate>true</propagate>") participant property, keyed by name: the only
    // way left to see that the anchor's properties reach the created participant without reading
    // product internals -- Fast DDS only puts a property on the wire (PDP::
    // set_external_participant_properties_) when it is marked propagate.
    void on_participant_discovery(DomainParticipant*,
                                  eprosima::fastdds::rtps::ParticipantDiscoveryStatus reason,
                                  const eprosima::fastdds::rtps::ParticipantBuiltinTopicData& info,
                                  bool& should_be_ignored) override {
        should_be_ignored = false;
        if (reason != eprosima::fastdds::rtps::ParticipantDiscoveryStatus::DISCOVERED_PARTICIPANT)
            return;
        std::lock_guard<std::mutex> lock(mu_);
        for (const auto& property : info.properties)
            properties_[property.first()] = property.second();
        cv_.notify_all();
    }

    // THE LATCH. Nothing is compared until the row's callback has fired; a timeout is a hard
    // failure, never a silently-default-initialised comparison (DEBT-2).
    bool AwaitWriter(const std::string& topic, Announced* out,
                     std::chrono::milliseconds budget = std::chrono::seconds(20)) {
        return Await(writers_, topic, out, budget);
    }

    bool AwaitReader(const std::string& topic, Announced* out,
                     std::chrono::milliseconds budget = std::chrono::seconds(20)) {
        return Await(readers_, topic, out, budget);
    }

    bool AwaitParticipantProperty(const std::string& name, std::string* value,
                                  std::chrono::milliseconds budget = std::chrono::seconds(20)) {
        std::unique_lock<std::mutex> lock(mu_);
        const bool found = cv_.wait_for(lock, budget, [&] { return properties_.count(name) != 0; });
        if (found) *value = properties_.at(name);
        return found;
    }

   private:
    bool Await(const std::map<std::string, Announced>& table, const std::string& topic,
               Announced* out, std::chrono::milliseconds budget) {
        std::unique_lock<std::mutex> lock(mu_);
        const bool found = cv_.wait_for(lock, budget, [&] { return table.count(topic) != 0; });
        if (found) *out = table.at(topic);
        return found;
    }

    DomainParticipant* participant_ = nullptr;
    std::mutex mu_;
    std::condition_variable cv_;
    std::map<std::string, Announced> writers_;
    std::map<std::string, Announced> readers_;
    std::map<std::string, std::string> properties_;
};

// A bare participant + publisher + subscriber, so the in-process resolution tests can call the
// production ladders (`internal::Resolve{Writer,Reader}Qos`) without a provider, a topic or any
// discovery at all. `get_*_qos_from_profile` -- what those ladders call -- lives on Publisher /
// Subscriber, hence the scaffolding. `DefaultProfileTranscriptionIsExact` also borrows it, for its
// own deliberate `get_*_qos_from_xml` string call, which bypasses the registry entirely.
class XmlProbe {
   public:
    explicit XmlProbe(uint32_t domain) {
        participant_ = DomainParticipantFactory::get_instance()->create_participant(
            domain, PARTICIPANT_QOS_DEFAULT);
        if (!participant_) return;
        publisher_ = participant_->create_publisher(PUBLISHER_QOS_DEFAULT);
        subscriber_ = participant_->create_subscriber(SUBSCRIBER_QOS_DEFAULT);
    }

    ~XmlProbe() {
        if (!participant_) return;
        if (publisher_) participant_->delete_publisher(publisher_);
        if (subscriber_) participant_->delete_subscriber(subscriber_);
        DomainParticipantFactory::get_instance()->delete_participant(participant_);
    }

    XmlProbe(const XmlProbe&) = delete;
    XmlProbe& operator=(const XmlProbe&) = delete;

    bool ok() const { return publisher_ != nullptr && subscriber_ != nullptr; }
    // Non-const despite `const`: AnEmptyDocumentIsFletchersBuiltInEverywhere needs to call
    // set_default_datawriter_qos / set_default_datareader_qos, which mutate the Publisher /
    // Subscriber, not this probe.
    Publisher& publisher() const { return *publisher_; }
    Subscriber& subscriber() const { return *subscriber_; }

   private:
    DomainParticipant* participant_ = nullptr;
    Publisher* publisher_ = nullptr;
    Subscriber* subscriber_ = nullptr;
};

// Every test gets its own DDS domain: the observer sees everything on its domain, and the other
// TUs in this binary run on domain 0.
constexpr uint32_t kDomainForcing = 91;
constexpr uint32_t kDomainPerTopic = 92;
constexpr uint32_t kDomainReader = 93;
constexpr uint32_t kDomainReaderSilence = 94;
constexpr uint32_t kDomainSchemaChannel = 95;
constexpr uint32_t kDomainTwoInstances = 96;
constexpr uint32_t kDomainProbe = 97;
constexpr uint32_t kDomainAnchorProperties = 98;
constexpr uint32_t kDomainMissingDefaultWarning = 99;
constexpr uint32_t kDomainBuiltInNoWarning = 100;
constexpr uint32_t kDomainSingleQuotedDefault = 101;

}  // namespace

// ===========================================================================
// THE FORCING TEST(S)
//
// The setting an endpoint ANNOUNCES on the network is the document's — not merely that the
// document loaded. Originally one TEST looping a four-row table; now four TESTs, one per row,
// because Fast DDS's profile registry is process-wide. Rows 3 and 4 each define a DIFFERENT
// "default_writer" profile, which collides if both are loaded in one process (see the
// one-document-per-TEST note at the top of this file). Row 1 (the empty document) moved out to
// its own TEST too, whole-struct rather than through discovery: an empty document now loads
// `FletcherDefaultProfilesDocument()`'s own bytes into the SAME registry every other document
// uses (owner decision 2026-09-15 -- AnEmptyDocumentIsFletchersBuiltInEverywhere below pins the
// resolved values, AnEmptyDocumentIsTheDefaultDocumentAndCollidesWithAnotherOne pins that it now
// collides like any other document), so its own process here keeps row 1 from colliding with rows
// 2-4. Rows 3 and 4 still differ from Fast DDS's default AND from each other, so a provider that
// hard-codes any single value reddens at least one of the four TESTs; row 2 (here) pins that an
// anchor-only document still reaches Fast DDS's registry and gets a live writer, not that its QoS
// differs from Fletcher's baked-in one: durability and reliability happen to coincide with both.
// `AnAnchorOnlyDocumentResolvesToFastDdsDefaults` is the in-process, whole-struct test that
// actually tells them apart, on history and resource_limits, which are not on the wire.
// ===========================================================================

// Row 1, split into its own TEST/process (see the note above), whole-struct rather than through
// discovery: `history` and `resource_limits` are not on the wire, and they are exactly where
// Fletcher's baked-in profile and Fast DDS's raw default disagree. An empty document IS
// `FletcherDefaultProfilesDocument()`'s bytes, loaded like any other (owner decision 2026-09-15;
// fast_dds_pubsub_provider.cpp resolves the empty case to this exact string before it ever reaches
// the registry), so this loads it directly rather than through a provider.
TEST(FastDdsConfig, AnEmptyDocumentIsFletchersBuiltInEverywhere) {
    const std::string document(internal::FletcherDefaultProfilesDocument());
    {
        std::lock_guard<std::mutex> lock(internal::profile_registry_mutex);
        internal::LoadDocumentOnce(document);
    }

    // Created AFTER the load: the Publisher's / Subscriber's default QoS is seeded from the
    // registry's is_default_profile profiles at construction time.
    XmlProbe probe(kDomainProbe);
    ASSERT_TRUE(probe.ok());

    // The independent answer: parsed straight from the string, bypassing the registry entirely.
    DataWriterQos expected_writer;
    ASSERT_EQ(probe.publisher().get_default_datawriter_qos_from_xml(document, expected_writer),
              RETCODE_OK);
    DataReaderQos expected_reader;
    ASSERT_EQ(probe.subscriber().get_default_datareader_qos_from_xml(document, expected_reader),
              RETCODE_OK);

    const DataWriterQos writer = internal::ResolveDataWriterQos(probe.publisher(), "forcing/empty");
    const DataReaderQos reader =
        internal::ResolveDataReaderQos(probe.subscriber(), "forcing/empty");
    EXPECT_TRUE(writer == expected_writer);
    EXPECT_TRUE(reader == expected_reader);
}

// Rule 1's replacement (owner decision 2026-09-15): an EMPTY document is Fletcher's baked-in one,
// loaded like any other document -- so it is no longer immune to the process-wide registry
// collision every other document is subject to. Provider A registers a document with a DIFFERENT
// "default_writer" profile; provider B, built with an EMPTY document, then tries to load
// Fletcher's baked-in one under the SAME profile name and is refused, the same way
// `ASecondDifferentDocumentInOneProcessIsRefused`'s pair is.
TEST(FastDdsConfig, AnEmptyDocumentIsTheDefaultDocumentAndCollidesWithAnotherOne) {
    const std::string document_a = Document(
        WriterProfile("default_writer", "<durability><kind>VOLATILE</kind></durability>", kTenSlots,
                      /*is_default=*/true));

    ProviderConfig config_a;
    config_a.domain_id = kDomainTwoInstances;
    config_a.document = document_a;
    ProviderConfig config_b;
    config_b.domain_id = kDomainTwoInstances;
    config_b.document = "";

    FastDDSPubSubProvider a(config_a);
    try {
        FastDDSPubSubProvider b(config_b);
        ADD_FAILURE() << "an empty document was accepted after a different one was already loaded "
                         "in this process";
    } catch (const PubSubError& e) {
        EXPECT_EQ(e.status(), PubSubStatus::kInvalidArgument) << e.what();
        EXPECT_NE(std::string(e.what()).find("process"), std::string::npos) << e.what();
    }
}

// Row 2: the most common document there is — an anchor and nothing else, which is the shape of
// every document that exists only to carry a `fletcher.*` property. It registers no writer
// profile, so Fast DDS's own default decides here, not Fletcher's built-in.
TEST(FastDdsConfig, ProfileDocumentConfiguresQos) {
    DiscoveryObserver observer(kDomainForcing);
    ASSERT_TRUE(observer.ok());

    ProviderConfig config;
    config.domain_id = kDomainForcing;
    config.document = kAnchorOnly;

    FastDDSPubSubProvider provider(config);
    provider.CreateTopic({"forcing", "anchor"}, MakeSchema());
    // CreateTopic already creates the DataWriter with the resolved QoS; publish anyway so the
    // sample write path is exercised too, same as every other row here.
    provider.Publish({"forcing", "anchor"}, MakeEncoder(1));

    Announced announced;
    ASSERT_TRUE(observer.AwaitWriter("forcing/anchor", &announced))
        << "no writer discovery for forcing/anchor — nothing was compared";
    EXPECT_EQ(announced.durability, TRANSIENT_LOCAL_DURABILITY_QOS);
    EXPECT_EQ(announced.reliability, RELIABLE_RELIABILITY_QOS);
}

// Row 3, split into its own TEST/process: this document's "default_writer" profile differs from
// the reliability row's below, and Fast DDS profile names are process-wide.
TEST(FastDdsConfig, AFletcherWriterProfileDecidesDurability) {
    const std::string document = Document(
        WriterProfile("default_writer", "<durability><kind>VOLATILE</kind></durability>", kTenSlots,
                      /*is_default=*/true));

    DiscoveryObserver observer(kDomainForcing);
    ASSERT_TRUE(observer.ok());

    ProviderConfig config;
    config.domain_id = kDomainForcing;
    config.document = document;
    FastDDSPubSubProvider provider(config);

    provider.CreateTopic({"forcing", "volatile"}, MakeSchema());
    provider.Publish({"forcing", "volatile"}, MakeEncoder(1));

    Announced announced;
    ASSERT_TRUE(observer.AwaitWriter("forcing/volatile", &announced))
        << "no writer discovery for forcing/volatile — nothing was compared";
    EXPECT_EQ(announced.durability, VOLATILE_DURABILITY_QOS);
    EXPECT_EQ(announced.reliability, RELIABLE_RELIABILITY_QOS);
}

// Row 4, split into its own TEST/process. Expected durability here is TRANSIENT_LOCAL and that is
// NOT Fletcher's built-in leaking through — it is MEASURED to be Fast DDS's own default on the
// XML path, which for a writer's `durability` is TRANSIENT_LOCAL rather than `DataWriterQos()`'s
// VOLATILE (the XML parser fills an RTPS-level `WriterQos`, whose durability default differs from
// the DDS-level one). `MinimalProfileTakesFastDdsDefaultsNotFletchers` pins that fact directly and
// distinguishes the two answers to the merge question on `history` and `resource_limits`, where
// Fletcher's and Fast DDS's DO differ. What this row exists to prove is reliability, and
// BEST_EFFORT differs from every other row.
TEST(FastDdsConfig, AFletcherWriterProfileDecidesReliability) {
    const std::string document = Document(WriterProfile(
        "default_writer", "<reliability><kind>BEST_EFFORT</kind></reliability>", kTenSlots,
        /*is_default=*/true));

    DiscoveryObserver observer(kDomainForcing);
    ASSERT_TRUE(observer.ok());

    ProviderConfig config;
    config.domain_id = kDomainForcing;
    config.document = document;
    FastDDSPubSubProvider provider(config);

    provider.CreateTopic({"forcing", "besteffort"}, MakeSchema());
    provider.Publish({"forcing", "besteffort"}, MakeEncoder(1));

    Announced announced;
    ASSERT_TRUE(observer.AwaitWriter("forcing/besteffort", &announced))
        << "no writer discovery for forcing/besteffort — nothing was compared";
    EXPECT_EQ(announced.durability, TRANSIENT_LOCAL_DURABILITY_QOS);
    EXPECT_EQ(announced.reliability, BEST_EFFORT_RELIABILITY_QOS);
}

// A profile named after the topic wins over `fletcher_writer`. Two topics on ONE instance, only
// one of them named by a profile: the discovered durability must differ between them, which is
// what a provider that either dropped the topic-name lookup or applied the topic profile
// everywhere cannot produce.
TEST(FastDdsConfig, PerTopicProfileOverridesTheDefault) {
    const std::string document =
        Document(WriterProfile("default_writer", "<durability><kind>VOLATILE</kind></durability>",
                               kTenSlots, /*is_default=*/true) +
                 "\n" +
                 WriterProfile("pertopic/special",
                               "<durability><kind>TRANSIENT_LOCAL</kind></durability>", kTenSlots));

    DiscoveryObserver observer(kDomainPerTopic);
    ASSERT_TRUE(observer.ok());

    ProviderConfig config;
    config.domain_id = kDomainPerTopic;
    config.document = document;
    FastDDSPubSubProvider provider(config);

    provider.CreateTopic({"pertopic", "special"}, MakeSchema());
    provider.CreateTopic({"pertopic", "ordinary"}, MakeSchema());
    provider.Publish({"pertopic", "special"}, MakeEncoder(1));
    provider.Publish({"pertopic", "ordinary"}, MakeEncoder(1));

    Announced special;
    Announced ordinary;
    ASSERT_TRUE(observer.AwaitWriter("pertopic/special", &special));
    ASSERT_TRUE(observer.AwaitWriter("pertopic/ordinary", &ordinary));
    EXPECT_EQ(special.durability, TRANSIENT_LOCAL_DURABILITY_QOS);
    EXPECT_EQ(ordinary.durability, VOLATILE_DURABILITY_QOS);
}

// The live control for "the document is applied to the WRITER only": a provider that applied a
// resolved writer profile to its readers as well reddens the forcing test, and one that applied
// the reader profile to writers reddens this. Neither test alone is sufficient.
TEST(FastDdsConfig, ReaderProfileConfiguresTheReader) {
    const std::string document = Document(
        ReaderProfile("default_reader", "<durability><kind>VOLATILE</kind></durability>", kTenSlots,
                      /*is_default=*/true));

    DiscoveryObserver observer(kDomainReader);
    ASSERT_TRUE(observer.ok());

    ProviderConfig config;
    config.domain_id = kDomainReader;
    config.document = document;
    FastDDSPubSubProvider provider(config);

    // A data reader is created disabled and enabled only once its topic's schema is known
    // (reader-side redesign): declaring the topic locally first resolves that synchronously, on
    // this thread, inside Subscribe below -- which is what makes the reader observable on
    // discovery at all.
    provider.CreateTopic({"readercfg", "topic"}, MakeSchema());

    SubscriptionResult result =
        provider.Subscribe({"readercfg", "topic"},
                           [](const uint8_t*, size_t, const SharedSchema&, const Attachments&) {});
    (void)result;

    Announced announced;
    ASSERT_TRUE(observer.AwaitReader("readercfg/topic", &announced));
    EXPECT_EQ(announced.durability, VOLATILE_DURABILITY_QOS);
}

// C2-2's RENAME — the built-in floor under a non-empty document is gone (design rule 2, owner
// ruling 2026-09-14): `fletcher_writer` / `fletcher_reader` are not special names any more, so a
// document that configures only the default WRITER profile leaves the reader on FAST DDS's OWN
// default now, not Fletcher's — an operator who supplies a document must say what the reader gets,
// same as the writer. (C2-2's original note about `data_sharing().off()` protecting the receive
// side no longer applies at all, empty document or not: item D, owner decision 2026-09-14, made
// Fletcher's own default AUTOMATIC too, same as Fast DDS's own — see qos_defaults.cpp.)
TEST(FastDdsConfig, WriterOnlyDocumentLeavesTheReaderOnFastDdsDefault) {
    const std::string document = Document(
        WriterProfile("default_writer", "<durability><kind>VOLATILE</kind></durability>", kTenSlots,
                      /*is_default=*/true));

    DiscoveryObserver observer(kDomainReaderSilence);
    ASSERT_TRUE(observer.ok());

    ProviderConfig config;
    config.domain_id = kDomainReaderSilence;
    config.document = document;
    FastDDSPubSubProvider provider(config);

    // See ReaderProfileConfiguresTheReader above: a data reader stays disabled -- and off
    // discovery -- until its topic's schema is known, so declare it locally first.
    provider.CreateTopic({"readersilence", "topic"}, MakeSchema());

    SubscriptionResult result =
        provider.Subscribe({"readersilence", "topic"},
                           [](const uint8_t*, size_t, const SharedSchema&, const Attachments&) {});
    (void)result;

    Announced announced;
    ASSERT_TRUE(observer.AwaitReader("readersilence/topic", &announced));
    // MEASURED (QosPolicies.hpp: "By default the value for DataReaders: VOLATILE_DURABILITY_QOS,
    // for DataWriters TRANSIENT_LOCAL_DURABILITY_QOS") -- the two default asymmetrically, so this
    // is genuinely VOLATILE and not a fallback that slipped: `AFletcherWriterProfileDecidesXxx`'s
    // TRANSIENT_LOCAL is the WRITER's XML-parsed default, which never applies here at all.
    EXPECT_EQ(announced.durability, VOLATILE_DURABILITY_QOS)
        << "the reader's durability moved off Fast DDS's own default";

    // Whole-struct, in-process, for the policies discovery cannot carry (history,
    // resource_limits): the reader's default is Fast DDS's OWN default end to end, not merely on
    // data_sharing -- `document` is already loaded (by `provider` above), and this registry
    // lookup still misses, because the document registers no default reader profile.
    XmlProbe probe(kDomainProbe);
    ASSERT_TRUE(probe.ok());
    const DataReaderQos reader_default =
        internal::ResolveDataReaderQos(probe.subscriber(), "readersilence/topic");
    EXPECT_TRUE(reader_default == DATAREADER_QOS_DEFAULT)
        << "the reader's default is no longer Fletcher's built-in -- fletcher_writer / "
           "fletcher_reader are not special names any more";
}

// The internal `__schema` channel consults NO profile name, ever. It is also this file's negative
// control for "the document reached Fast DDS at all": the same document that turns the data
// writer VOLATILE must leave the schema writer TRANSIENT_LOCAL, so a provider that applied one
// resolved profile to every writer it creates reddens here while greening the forcing test.
TEST(FastDdsConfig, SchemaChannelIgnoresTheDocument) {
    const std::string document = Document(
        WriterProfile("default_writer", "<durability><kind>VOLATILE</kind></durability>", kTenSlots,
                      /*is_default=*/true));

    DiscoveryObserver observer(kDomainSchemaChannel);
    ASSERT_TRUE(observer.ok());

    ProviderConfig config;
    config.domain_id = kDomainSchemaChannel;
    config.document = document;
    FastDDSPubSubProvider provider(config);

    provider.CreateTopic({"schemachannel", "topic"}, MakeSchema());
    provider.Publish({"schemachannel", "topic"}, MakeEncoder(1));

    Announced data_writer;
    Announced schema_writer;
    ASSERT_TRUE(observer.AwaitWriter("schemachannel/topic", &data_writer));
    ASSERT_TRUE(observer.AwaitWriter("schemachannel/topic/__schema", &schema_writer));
    EXPECT_EQ(data_writer.durability, VOLATILE_DURABILITY_QOS)
        << "the document did not reach Fast DDS at all, so this test proves nothing";
    EXPECT_EQ(schema_writer.durability, TRANSIENT_LOCAL_DURABILITY_QOS);
}

// Fast DDS's profile registry is process-wide (internal/profile_document.hpp): two providers in
// one process, the SAME byte-identical document, both resolve the SAME registered profile — not
// two independent copies. This is the positive half of the process-wide-registry consequence.
TEST(FastDdsConfig, TwoInstancesShareOneDocument) {
    const std::string document_a = Document(
        WriterProfile("default_writer", "<durability><kind>VOLATILE</kind></durability>", kTenSlots,
                      /*is_default=*/true));

    DiscoveryObserver observer(kDomainTwoInstances);
    ASSERT_TRUE(observer.ok());

    ProviderConfig config_a;
    config_a.domain_id = kDomainTwoInstances;
    config_a.document = document_a;
    ProviderConfig config_b = config_a;  // byte-identical: a no-op the second time it loads

    FastDDSPubSubProvider a(config_a);
    FastDDSPubSubProvider b(config_b);

    a.CreateTopic({"twoinstances", "a"}, MakeSchema());
    b.CreateTopic({"twoinstances", "b"}, MakeSchema());
    a.Publish({"twoinstances", "a"}, MakeEncoder(1));
    b.Publish({"twoinstances", "b"}, MakeEncoder(1));

    Announced from_a;
    Announced from_b;
    ASSERT_TRUE(observer.AwaitWriter("twoinstances/a", &from_a));
    ASSERT_TRUE(observer.AwaitWriter("twoinstances/b", &from_b));
    EXPECT_EQ(from_a.durability, VOLATILE_DURABILITY_QOS);
    EXPECT_EQ(from_b.durability, VOLATILE_DURABILITY_QOS);
}

// The negative half: a SECOND, DIFFERENT document in the same process collides on the profile
// name Fast DDS's registry already holds, and is refused rather than silently winning or losing.
TEST(FastDdsConfig, ASecondDifferentDocumentInOneProcessIsRefused) {
    const std::string document_a = Document(WriterProfile(
        "fletcher_writer", "<durability><kind>VOLATILE</kind></durability>", kTenSlots));
    // document_b also registers a NEW profile name ("twoinstances/b") beside the colliding
    // "fletcher_writer": XMLProfileManager::extractProfiles turns that collision into XML_NOK and
    // keeps going, so Fast DDS ALONE would accept this document -- only Fletcher's own check below
    // refuses it, which is what this test actually pins.
    const std::string document_b =
        Document(WriterProfile("fletcher_writer",
                               "<durability><kind>TRANSIENT_LOCAL</kind></durability>", kTenSlots) +
                 "\n" +
                 WriterProfile("twoinstances/b", "<durability><kind>VOLATILE</kind></durability>",
                               kTenSlots));

    ProviderConfig config_a;
    config_a.domain_id = kDomainTwoInstances;
    config_a.document = document_a;
    ProviderConfig config_b = config_a;
    config_b.document = document_b;

    FastDDSPubSubProvider a(config_a);
    try {
        FastDDSPubSubProvider b(config_b);
        ADD_FAILURE() << "a second, different document in one process was accepted";
    } catch (const PubSubError& e) {
        EXPECT_EQ(e.status(), PubSubStatus::kInvalidArgument) << e.what();
        EXPECT_NE(std::string(e.what()).find("process"), std::string::npos) << e.what();
    }
}

// The anchor stays mandatory because `loadXMLString` accepts a `<dds>` with no `<profiles>`
// element as a silent no-op: nothing registers, nothing errors, and without this test a document
// shaped like this one would resolve to "no such profile" and run on the built-in defaults.
TEST(FastDdsConfig, ADocumentWithoutAProfilesElementIsRefused) {
    ProviderConfig config;
    config.domain_id = kDomainProbe;
    config.document =
        R"(<?xml version="1.0" encoding="UTF-8"?><dds xmlns="http://www.eprosima.com/XMLSchemas/fastRTPS_Profiles"><participant profile_name="fletcher_participant"/></dds>)";
    try {
        FastDDSPubSubProvider provider(config);
        ADD_FAILURE() << "a document with no <profiles> element was accepted";
    } catch (const PubSubError& e) {
        EXPECT_EQ(e.status(), PubSubStatus::kInvalidArgument) << e.what();
        EXPECT_NE(std::string(e.what()).find("fletcher_participant"), std::string::npos)
            << e.what();
    }
}

// ===========================================================================
// In-process, whole-struct: the claims discovery cannot carry
// ===========================================================================

// The README publishes Fletcher's own profile as the operator's copy-paste starting point, and
// this is what keeps it true setting-for-setting. WHOLE-STRUCT equality, in process: it covers
// all six policies including `history` (depth 25 — invisible to discovery, and what bounds the
// RELIABLE in-flight window and the reader's `OnSampleLost` threshold) and `resource_limits`
// (max_samples 25 — 5000 would overflow the data-sharing segment's 32-bit size and drop the
// endpoint back to the transport), neither of which is observable in discovery data.
// `DataWriterQos::operator==` and `DataReaderQos::operator==` each compare 22 of 22 members, and
// `RTPSEndpointQos::operator==` carries `history_memory_policy`, so a block that silently loses
// the zero-copy read path reddens here too.
//
// If some policy provably cannot be transcribed into XML, this assert says so and the README
// names it as a known non-transcribable difference — that is the honest outcome, not a weaker
// assert.
TEST(FastDdsConfig, DefaultProfileTranscriptionIsExact) {
    XmlProbe probe(kDomainProbe);
    ASSERT_TRUE(probe.ok());

    // The block is read OFF DISK, out of the README itself, and compared BYTE FOR BYTE against
    // `internal::FletcherDefaultProfilesDocument()` — the ONE other copy of this text in the
    // repository (qos_defaults.cpp) — so editing either the README block or that string alone
    // reddens this test.
    const std::string readme = ReadWholeFile(kReadmePath);
    ASSERT_FALSE(readme.empty())
        << "could not read the provider README at " << kReadmePath
        << " - this test compares the README's published starting point against the code, so it "
           "cannot run without it (the path arrives via target_compile_definitions, and README.md "
           "is exported by conanfile.py so the cache build can read it too)";

    const std::string published =
        ExtractFencedXmlAfter(readme, "#### The published starting point");
    ASSERT_FALSE(published.empty())
        << "found no ```xml fenced block under '#### The published starting point' in "
        << kReadmePath << " - if that section was renamed, this test's marker moves with it";

    // Trim only TRAILING whitespace: the fence extraction and the C++ raw string literal can
    // differ on a trailing newline with no meaningful drift either side of it.
    auto trim_trailing = [](std::string s) {
        while (!s.empty() &&
               (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) {
            s.pop_back();
        }
        return s;
    };
    EXPECT_EQ(trim_trailing(published), trim_trailing(internal::FletcherDefaultProfilesDocument()))
        << "the README's published starting point no longer matches "
           "FletcherDefaultProfilesDocument() byte for byte";

    // Whole-struct parse compare, in process: cheap, and it is what actually proves Fast DDS can
    // read the published block as a QoS at all -- the DEFAULT lookup, not the by-name one, fills
    // `qos` from whichever profile carries `is_default_profile="true"`, so a block that lost that
    // attribute fails this lookup outright.
    DataWriterQos writer;
    ASSERT_EQ(probe.publisher().get_default_datawriter_qos_from_xml(published, writer), RETCODE_OK)
        << "the README's published starting point no longer defines an is_default_profile=\"true\" "
           "data_writer profile Fast DDS can parse";
    DataWriterQos expected_writer;
    ASSERT_EQ(probe.publisher().get_default_datawriter_qos_from_xml(
                  internal::FletcherDefaultProfilesDocument(), expected_writer),
              RETCODE_OK);
    EXPECT_TRUE(writer == expected_writer);

    DataReaderQos reader;
    ASSERT_EQ(probe.subscriber().get_default_datareader_qos_from_xml(published, reader), RETCODE_OK)
        << "the README's published starting point no longer defines an is_default_profile=\"true\" "
           "data_reader profile Fast DDS can parse";
    DataReaderQos expected_reader;
    ASSERT_EQ(probe.subscriber().get_default_datareader_qos_from_xml(
                  internal::FletcherDefaultProfilesDocument(), expected_reader),
              RETCODE_OK);
    EXPECT_TRUE(reader == expected_reader);
}

// The public header's copy of the built-in document is not a second copy: it forwards straight to
// the one string this whole file already pins (`FletcherDefaultProfilesDocument()`,
// `DefaultProfileTranscriptionIsExact` above).
TEST(FastDdsConfig, DefaultProfilesDocumentIsTheBuiltInOne) {
    const std::string from_header(FastDDSPubSubProvider::DefaultProfilesDocument());
    EXPECT_EQ(from_header, internal::FletcherDefaultProfilesDocument());

    // Both is_default_profile="true" attributes survive on the public copy -- the whole point of
    // exposing this string is that a caller can find them in it and add named profiles alongside.
    const size_t first = from_header.find("is_default_profile=\"true\"");
    ASSERT_NE(first, std::string::npos);
    EXPECT_NE(from_header.find("is_default_profile=\"true\"", first + 1), std::string::npos);
}

// K.1's four named pairs resolve by name on BOTH sides, read straight out of the document text
// (`get_data{writer,reader}_qos_from_xml(document, qos, name)`) the same way
// DefaultProfileTranscriptionIsExact reads the default pair, rather than through the registry or a
// provider: this is a claim about what the document SAYS, independent of resolution order.
TEST(FastDdsConfig, BuiltInNamedProfilesResolveOnBothSides) {
    XmlProbe probe(kDomainProbe);
    ASSERT_TRUE(probe.ok());

    const std::string document(internal::FletcherDefaultProfilesDocument());

    struct Row {
        const char* name;
        ReliabilityQosPolicyKind reliability;
        DurabilityQosPolicyKind durability;
        HistoryQosPolicyKind history_kind;
        int32_t depth;  // only meaningful for KEEP_LAST
        int32_t max_samples;
        int32_t max_instances;
        int32_t max_samples_per_instance;
        int32_t allocated_samples;
    };

    const std::vector<Row> rows = {
        {"fire_and_forget", BEST_EFFORT_RELIABILITY_QOS, VOLATILE_DURABILITY_QOS,
         KEEP_LAST_HISTORY_QOS, 1, 1, 1, 1, 1},
        {"store_latest", RELIABLE_RELIABILITY_QOS, TRANSIENT_LOCAL_DURABILITY_QOS,
         KEEP_LAST_HISTORY_QOS, 1, 1, 1, 1, 1},
        {"store_history", RELIABLE_RELIABILITY_QOS, TRANSIENT_LOCAL_DURABILITY_QOS,
         KEEP_LAST_HISTORY_QOS, 25, 25, 1, 25, 25},
        {"lossless", RELIABLE_RELIABILITY_QOS, VOLATILE_DURABILITY_QOS, KEEP_ALL_HISTORY_QOS, 0, 25,
         1, 25, 25},
    };

    for (const Row& row : rows) {
        SCOPED_TRACE(row.name);

        DataWriterQos writer;
        ASSERT_EQ(probe.publisher().get_datawriter_qos_from_xml(document, writer, row.name),
                  RETCODE_OK)
            << "no <data_writer> profile named '" << row.name << "' in the built-in document";
        EXPECT_EQ(writer.reliability().kind, row.reliability);
        EXPECT_EQ(writer.durability().kind, row.durability);
        EXPECT_EQ(writer.history().kind, row.history_kind);
        if (row.history_kind == KEEP_LAST_HISTORY_QOS) EXPECT_EQ(writer.history().depth, row.depth);
        EXPECT_EQ(writer.resource_limits().max_samples, row.max_samples);
        EXPECT_EQ(writer.resource_limits().max_instances, row.max_instances);
        EXPECT_EQ(writer.resource_limits().max_samples_per_instance, row.max_samples_per_instance);
        EXPECT_EQ(writer.resource_limits().allocated_samples, row.allocated_samples);

        DataReaderQos reader;
        ASSERT_EQ(probe.subscriber().get_datareader_qos_from_xml(document, reader, row.name),
                  RETCODE_OK)
            << "no <data_reader> profile named '" << row.name << "' in the built-in document";
        EXPECT_EQ(reader.reliability().kind, row.reliability);
        EXPECT_EQ(reader.durability().kind, row.durability);
        EXPECT_EQ(reader.history().kind, row.history_kind);
        if (row.history_kind == KEEP_LAST_HISTORY_QOS) EXPECT_EQ(reader.history().depth, row.depth);
        EXPECT_EQ(reader.resource_limits().max_samples, row.max_samples);
        EXPECT_EQ(reader.resource_limits().max_instances, row.max_instances);
        EXPECT_EQ(reader.resource_limits().max_samples_per_instance, row.max_samples_per_instance);
        EXPECT_EQ(reader.resource_limits().allocated_samples, row.allocated_samples);

        if (std::string(row.name) == "lossless") {
            EXPECT_EQ(writer.reliability().max_blocking_time, c_TimeInfinite)
                << "the lossless writer's max_blocking_time is not infinite";
            EXPECT_EQ(writer.reliable_writer_qos().times.heartbeat_period, Duration_t(0, 20000000))
                << "the lossless writer's heartbeat period is not 20 ms";
        } else {
            EXPECT_EQ(writer.reliable_writer_qos().times.heartbeat_period,
                      DATAWRITER_QOS_DEFAULT.reliable_writer_qos().times.heartbeat_period)
                << row.name << "'s writer heartbeat moved off Fast DDS's own default";
        }
    }
}

// DEBT-1's RENAME (owner ruling 2026-09-14) — an anchor-only, non-empty document used to resolve
// to Fletcher's built-in QoS; the built-in floor under a non-empty document is gone now
// (design rule 2), so it resolves to FAST DDS's OWN default instead, because this document
// registers no `is_default_profile="true"` profile for either role. In-process, WHOLE-STRUCT, on
// the production ladder: `history` and `resource_limits` are not on the wire, and they are
// exactly where Fletcher's built-in and Fast DDS's own default disagree, so durability and
// reliability alone could not tell a leftover Fletcher-fallback from the correct answer.
TEST(FastDdsConfig, AnAnchorOnlyDocumentResolvesToFastDdsDefaults) {
    const std::string document(kAnchorOnly);
    {
        std::lock_guard<std::mutex> lock(internal::profile_registry_mutex);
        internal::LoadDocumentOnce(document);
    }

    // Created AFTER the load: the Publisher's / Subscriber's default QoS is seeded from the
    // registry at construction, and this anchor-only document registers no default profile for
    // either role.
    XmlProbe probe(kDomainProbe);
    ASSERT_TRUE(probe.ok());

    const DataWriterQos writer =
        internal::ResolveDataWriterQos(probe.publisher(), "anchoronly/topic");
    EXPECT_TRUE(writer == DataWriterQos())
        << "a document that names no writer profile did not fall back to Fast DDS's own "
           "default -- fletcher_writer / fletcher_reader are not special names any more, so "
           "nothing here should still resolve to Fletcher's baked-in profile";

    const DataReaderQos reader =
        internal::ResolveDataReaderQos(probe.subscriber(), "anchoronly/topic");
    EXPECT_TRUE(reader == DataReaderQos())
        << "a document that names no reader profile did not fall back to Fast DDS's own "
           "default";

    // Not vacuous: Fletcher's baked-in profile and Fast DDS's default actually differ, or a
    // leftover Fletcher-fallback would pass this test by accident. They differ on history and on
    // resource_limits, and on nothing discovery can carry. Parsed straight from the constant,
    // bypassing the registry entirely, so this is independent of the resolution ladder above.
    DataWriterQos baked_in_writer;
    ASSERT_EQ(probe.publisher().get_default_datawriter_qos_from_xml(
                  internal::FletcherDefaultProfilesDocument(), baked_in_writer),
              RETCODE_OK);
    DataReaderQos baked_in_reader;
    ASSERT_EQ(probe.subscriber().get_default_datareader_qos_from_xml(
                  internal::FletcherDefaultProfilesDocument(), baked_in_reader),
              RETCODE_OK);
    ASSERT_FALSE(baked_in_writer == DataWriterQos())
        << "Fletcher's baked-in writer profile is now identical to Fast DDS's default, so this "
           "test can no longer tell them apart";
    ASSERT_FALSE(baked_in_reader == DataReaderQos());
}

// C2-1 — THE FIFTH SILENCE: the only thing here that can tell the owner's answer from the one
// they rejected, for as long as the substrate can tell them apart at all (see the CORRECTED note
// below — against fast-dds/3.4.0 that is a claim about the substrate, not about this code).
//
// Owner ruling 2026-09-02: "a supplied profile is that endpoint's complete quality-of-service;
// anything unmentioned takes the DDS default." Every other test here supplies a profile and then
// asserts only the policy that profile SET, so a build implementing the rejected answer — merge
// semantics, Fletcher's defaults staying underneath — passes all of them. This is the assert
// that does not.
//
// A minimal profile that mentions ONLY durability must resolve to Fast DDS's raw `history().depth`
// (1), not Fletcher's built-in (25), and to Fast DDS's `max_samples` (5000), not Fletcher's 25.
// It calls the production ladder, so it holds the shape rather than a re-implementation of it.
//
// CORRECTED (review 4b, measured): this comment used to claim that seeding
// `internal::ResolveDataWriterQos`'s output with `MakeFletcherDefaultDataWriterQos()` reddens this
// test and nothing else. It reddens NOTHING — the whole suite stays green under that mutation,
// because fast-dds/3.4.0 OVERWRITES the output parameter rather than overlaying onto it.
// `src/internal/profile_document.hpp` (the MEASURED note) always said so and is the correct
// record. So what this test asserts is that the SUBSTRATE does not merge; the fresh-QoS-per-call
// form is mandated structurally there and cannot be asserted from here (review 4a F3, accepted
// debt). What this test WOULD catch is a future Fast DDS that starts overlaying while the seeding
// was reintroduced — and, today, any implementation that puts a Fletcher floor under a supplied
// profile by hand.
TEST(FastDdsConfig, MinimalProfileTakesFastDdsDefaultsNotFletchers) {
    XmlProbe probe(kDomainProbe);
    ASSERT_TRUE(probe.ok());

    const std::string document = Document(
        WriterProfile("fletcher_writer", "<durability><kind>VOLATILE</kind></durability>"));

    // Sanity: Fast DDS's raw default history kind is KEEP_LAST, same as Fletcher's built-in
    // (README "published starting point") -- depth is where they differ, 1 against 25, so the
    // depth and max_samples checks below are what makes this test non-vacuous.
    ASSERT_EQ(DataWriterQos().history().kind, KEEP_LAST_HISTORY_QOS);

    {
        std::lock_guard<std::mutex> lock(internal::profile_registry_mutex);
        internal::LoadDocumentOnce(document);
    }
    const DataWriterQos resolved =
        internal::ResolveDataWriterQos(probe.publisher(), "fletcher_writer");

    EXPECT_EQ(resolved.durability().kind, VOLATILE_DURABILITY_QOS) << "the profile was not applied";
    EXPECT_EQ(resolved.history().kind, KEEP_LAST_HISTORY_QOS)
        << "history kind no longer distinguishes Fletcher's built-in from Fast DDS's raw default "
           "-- both are KEEP_LAST now; depth and max_samples below do that job";
    EXPECT_EQ(resolved.history().depth, DataWriterQos().history().depth)
        << "a policy the profile omitted fell back to Fletcher's depth (25) — that is the merge "
           "semantics owner ruling 2026-09-02 rejected";
    EXPECT_EQ(resolved.resource_limits().max_samples, DataWriterQos().resource_limits().max_samples)
        << "resource_limits came from Fletcher's built-in rather than from Fast DDS's default";
}

// MEASURED, not assumed: "Fast DDS's default" means the default of the path the document
// actually travels, and the XML parser fills an RTPS-level `WriterQos` whose `durability`
// default is TRANSIENT_LOCAL — NOT `DataWriterQos()`'s VOLATILE. So a writer profile that omits
// durability announces TRANSIENT_LOCAL, which happens to coincide with Fletcher's built-in and
// therefore cannot distinguish the two answers to the merge question. That is exactly why C2-1's
// assert (above) is anchored on `history` and `resource_limits`, where the two genuinely differ.
// Pinned here so a Fast DDS upgrade that changed it is visible. Own TEST/process: this document's
// "fletcher_writer" profile differs from the one above, and Fast DDS profile names are
// process-wide.
TEST(FastDdsConfig, AWriterProfileSilentOnDurabilityGetsFastDdsTransientLocal) {
    XmlProbe probe(kDomainProbe);
    ASSERT_TRUE(probe.ok());

    const std::string silent_document = Document(
        WriterProfile("fletcher_writer", "<reliability><kind>BEST_EFFORT</kind></reliability>"));
    {
        std::lock_guard<std::mutex> lock(internal::profile_registry_mutex);
        internal::LoadDocumentOnce(silent_document);
    }
    const DataWriterQos silent_on_durability =
        internal::ResolveDataWriterQos(probe.publisher(), "fletcher_writer");
    EXPECT_EQ(silent_on_durability.durability().kind, TRANSIENT_LOCAL_DURABILITY_QOS)
        << "the XML path's writer durability default moved; "
           "AFletcherWriterProfileDecidesReliability's expectation and this comment both describe "
           "it and must move with it";
}

// The same rule on the reader. Was the data-sharing line (Fletcher's built-in OFF, Fast DDS's
// default AUTO); since item D (owner decision 2026-09-14) both are AUTOMATIC, so that line no
// longer tells "inherited Fletcher's default" apart from "fell to Fast DDS's own" -- reliability
// does the same job now (Fletcher's built-in RELIABLE, Fast DDS's own default BEST_EFFORT,
// QosPolicies.hpp); history no longer tells them apart either -- Fletcher's built-in reader is
// KEEP_LAST now too (depth 25 against Fast DDS's raw depth 1), a difference this test does not
// itself check. A supplied reader profile owns the decision either way (handled residue H2 — a
// Fletcher floor would mean the document does not really configure QoS, and the PDA-ABI-7 defect
// hunt needs it on). Own TEST/process: a distinct document from the two writer-side TESTs above.
TEST(FastDdsConfig, AMinimalReaderProfileTakesFastDdsDefaultsNotFletchers) {
    XmlProbe probe(kDomainProbe);
    ASSERT_TRUE(probe.ok());

    const std::string reader_document = Document(
        ReaderProfile("fletcher_reader", "<durability><kind>VOLATILE</kind></durability>"));
    {
        std::lock_guard<std::mutex> lock(internal::profile_registry_mutex);
        internal::LoadDocumentOnce(reader_document);
    }
    const DataReaderQos reader_resolved =
        internal::ResolveDataReaderQos(probe.subscriber(), "fletcher_reader");
    // Sanity: Fast DDS's raw default is BEST_EFFORT (QosPolicies.hpp), not Fletcher's baked-in
    // RELIABLE (README "published starting point"), so the check below is not vacuous.
    ASSERT_EQ(DataReaderQos().reliability().kind, BEST_EFFORT_RELIABILITY_QOS);
    EXPECT_EQ(reader_resolved.history().kind, KEEP_LAST_HISTORY_QOS);
    EXPECT_EQ(reader_resolved.history().depth, DataReaderQos().history().depth)
        << "a policy the profile omitted fell back to Fletcher's depth (25) instead of Fast "
           "DDS's raw default (1) -- history kind alone no longer distinguishes Fletcher's "
           "built-in reader from Fast DDS's own, both are KEEP_LAST now, so this test "
           "discriminates on depth too, the same way its writer-side twin "
           "(MinimalProfileTakesFastDdsDefaultsNotFletchers) does";
    EXPECT_EQ(reader_resolved.reliability().kind, DataReaderQos().reliability().kind)
        << "a supplied reader profile is not the whole QoS: Fletcher's RELIABLE default leaked "
           "underneath it";
}

// ConsumeFletcherProperties/FletcherProperties are gone (owner decision 2026-09-15): the anchor's
// `<propertiesPolicy>` now reaches `create_participant` untouched, nothing left to strip. This is
// the positive half a stripped implementation could never have shown -- a foreign, PROPAGATED
// property in the anchor is visible on the participant this provider actually created, read back
// off the wire through a `DiscoveryObserver` (`participant->get_qos()` is not reachable from a
// test without product code: `Impl::participant` is private).
TEST(FastDdsConfig, AnchorPropertiesReachTheParticipant) {
    const std::string document = Document("", R"(
      <rtps>
        <propertiesPolicy>
          <properties>
            <property>
              <name>fletcherish.but.not.ours</name>
              <value>kept</value>
              <propagate>true</propagate>
            </property>
          </properties>
        </propertiesPolicy>
      </rtps>)");

    DiscoveryObserver observer(kDomainAnchorProperties);
    ASSERT_TRUE(observer.ok());

    ProviderConfig config;
    config.domain_id = kDomainAnchorProperties;
    config.document = document;
    FastDDSPubSubProvider provider(config);

    std::string value;
    ASSERT_TRUE(observer.AwaitParticipantProperty("fletcherish.but.not.ours", &value))
        << "no participant discovery carried the anchor's propagated property -- nothing was "
           "compared";
    EXPECT_EQ(value, "kept");
}

namespace {

std::string EndpointProperty(const std::string& name, const std::string& value) {
    return R"(
      <propertiesPolicy>
        <properties>
          <property><name>)" +
           name + R"(</name><value>)" + value + R"(</value></property>
        </properties>
      </propertiesPolicy>)";
}

}  // namespace

// Nothing here is about `fletcher.*` any more (that concept is gone, owner decision 2026-09-15) --
// this pins a plain Fast DDS XML grammar fact instead: `<propertiesPolicy>` belongs directly under
// `<data_writer>`, as a sibling of `<qos>`, not nested inside it. A profile shaped like this fails
// to parse, so the WHOLE document fails to load.
TEST(FastDdsConfig, APropertiesPolicyInsideAnEndpointQosIsRefusedAsMalformed) {
    ProviderConfig config;
    config.domain_id = kDomainProbe;
    config.document = Document(
        WriterProfile("fletcher_writer", EndpointProperty("example.vendor.property", "true")));
    try {
        FastDDSPubSubProvider provider(config);
        ADD_FAILURE() << "a <propertiesPolicy> inside <qos> was accepted; the writer profile is "
                         "unparseable, so a document shaped like this would silently register "
                         "nothing for it";
    } catch (const PubSubError& e) {
        EXPECT_EQ(e.status(), PubSubStatus::kInvalidArgument) << e.what();
        EXPECT_NE(std::string(e.what()).find("not a Fast DDS XML profiles document"),
                  std::string::npos)
            << e.what();
    }
}

// 4a F4 — the READER's per-topic lookup, which nothing asserted:
// `PerTopicProfileOverridesTheDefault` is writer-only and `ReaderProfileConfiguresTheReader`
// supplies only `default_reader`, so deleting the reader ladder's topic-name lookup reddened
// nothing. In-process on the production ladder, because the interesting half is the NEGATIVE one:
// the same document must NOT resolve that profile for a different topic.
TEST(FastDdsConfig, ReaderPerTopicProfileOverridesTheDefault) {
    const std::string document =
        Document(ReaderProfile("readertopic/special",
                               "<durability><kind>VOLATILE</kind></durability>", kTenSlots) +
                 "\n" +
                 ReaderProfile("default_reader",
                               "<durability><kind>TRANSIENT_LOCAL</kind></durability>", kTenSlots,
                               /*is_default=*/true));
    {
        std::lock_guard<std::mutex> lock(internal::profile_registry_mutex);
        internal::LoadDocumentOnce(document);
    }

    // Created AFTER the load: the Subscriber's default reader QoS is seeded from the registry's
    // is_default_profile reader profile at construction, which is what "ordinary" (no per-topic
    // match) below resolves to.
    XmlProbe probe(kDomainProbe);
    ASSERT_TRUE(probe.ok());

    const DataReaderQos special =
        internal::ResolveDataReaderQos(probe.subscriber(), "readertopic/special");
    EXPECT_EQ(special.durability().kind, VOLATILE_DURABILITY_QOS)
        << "the reader profile named after the topic did not win";

    const DataReaderQos ordinary =
        internal::ResolveDataReaderQos(probe.subscriber(), "readertopic/ordinary");
    EXPECT_EQ(ordinary.durability().kind, TRANSIENT_LOCAL_DURABILITY_QOS)
        << "a per-topic reader profile resolved for a topic it is not named after";
}

// ===========================================================================
// Refusals — rung 2, all in the constructor, all kInvalidArgument
// ===========================================================================

// The guard that stops a provider which never reads the document from greening this whole file.
// Each row asserts the STATUS and the quoted text, so a refusal for the wrong reason is not a
// pass.
//
// One document per TEST, so this table keeps only the rows refused at PARSE/LOAD time, before
// anything is registered (truncated XML, the XRCE paste, and P6's syntactically broken
// fletcher_writer — the anchor is well-formed but the whole document still fails to parse, so
// nothing from it reaches the registry either), plus, LAST, the no-anchor row — the only one that
// registers a profile ("fletcher_writer"), so nothing after it could collide. The typo'd-property
// and unparseable-value rows, and the <domainId> row, moved to their own TESTs below: each of
// those documents DOES register the anchor, so two of them together in one process would collide.
//
// P6 — "a document containing any malformed profile fails the WHOLE registry load" — is measured
// by the third row: a valid anchor plus a syntactically broken `fletcher_writer`. That row must
// PASS against the correct implementation. If it FAILS, the document was accepted, P6 is false,
// and the `fletcher_participant` anchor is not sufficient to catch partial malformation — STOP AND
// ASK, and do not delete or weaken the row.
TEST(FastDdsConfig, MalformedProfileDocumentIsRefused) {
    struct Row {
        const char* what;
        std::string document;
        uint32_t domain_id;
        const char* quoted;
    };

    const std::vector<Row> rows = {
        {"truncated XML", R"(<dds><profiles><participant profile_name="fletcher_participant">)", 0,
         "not a Fast DDS XML profiles document"},
        {"an XRCE key=value document pasted into the wrong field", "schema_carriage=carried", 0,
         "not a Fast DDS XML profiles document"},
        // P6: the anchor itself is well-formed; only fletcher_writer is broken.
        {"a valid anchor plus a syntactically broken fletcher_writer (P6)",
         Document(WriterProfile("fletcher_writer", "<durabilty><kind>VOLATILE</durabilty></kind>")),
         0, "not a Fast DDS XML profiles document"},
        // LAST: the only row that registers anything (just "fletcher_writer" — no anchor).
        {"a profiles document with no fletcher_participant anchor",
         R"(<?xml version="1.0" encoding="UTF-8"?>
<dds xmlns="http://www.eprosima.com/XMLSchemas/fastRTPS_Profiles">
  <profiles>
    <data_writer profile_name="fletcher_writer">
      <qos><durability><kind>VOLATILE</kind></durability></qos>
    </data_writer>
  </profiles>
</dds>)",
         0, "fletcher_participant"},
    };

    for (const Row& row : rows) {
        SCOPED_TRACE(row.what);
        ProviderConfig config;
        config.domain_id = row.domain_id;
        config.document = row.document;
        try {
            FastDDSPubSubProvider provider(config);
            ADD_FAILURE() << "the document was accepted";
        } catch (const PubSubError& e) {
            EXPECT_EQ(e.status(), PubSubStatus::kInvalidArgument);
            EXPECT_NE(std::string(e.what()).find(row.quoted), std::string::npos)
                << "refused, but not for this reason: " << e.what();
        }
    }
}

// The domain refusal quotes BOTH numbers, because an operator meeting this rule for the first
// time needs to know which one the deployment is on.
TEST(FastDdsConfig, TheDomainRefusalQuotesBothNumbers) {
    ProviderConfig config;
    config.domain_id = 7;
    config.document = Document("", "<domainId>3</domainId>");
    try {
        FastDDSPubSubProvider provider(config);
        ADD_FAILURE() << "the disagreement was accepted";
    } catch (const PubSubError& e) {
        const std::string message = e.what();
        EXPECT_NE(message.find("<domainId> is 3"), std::string::npos) << message;
        EXPECT_NE(message.find("domain_id is 7"), std::string::npos) << message;
    }
}

// An explicit <domainId>0</domainId> cannot be told from absent, so it is accepted as absent —
// the accepted residue (re-review §B2). Refusing it would need a fact the XML API never reports,
// and refusing whenever the numbers differ would force every document to restate the domain and
// make documents non-portable across domains.
TEST(FastDdsConfig, AnExplicitZeroDomainIdIsReadAsAbsent) {
    ProviderConfig config;
    config.domain_id = kDomainProbe;
    config.document = Document("", "<domainId>0</domainId>");
    EXPECT_NO_THROW({ FastDDSPubSubProvider provider(config); });
}

// max_payload_bytes: 0 means unset and resolves to 65536 — bit-for-bit what the retired options
// struct defaulted to. This is the number this provider's own publishers register in the type
// name; a subscriber follows whatever a publisher announces instead.
TEST(FastDdsConfig, AnUnsetPayloadBoundResolvesToSixtyFourKiB) {
    FastDDSPubSubProvider provider(ProviderConfig{});
    EXPECT_EQ(provider.PayloadBytes(), 64u * 1024);
}

// An unusable bound is refused before the participant exists, and as kInvalidArgument: reached
// through a registry factory, a std::invalid_argument would arrive at the caller as kInternal,
// which tells an operator nothing (spec §5.1).
TEST(FastDdsConfig, AnUnusablePayloadBoundIsRefusedAsInvalidArgument) {
    ProviderConfig config;
    config.max_payload_bytes = 4095;  // not a multiple of 4
    try {
        FastDDSPubSubProvider provider(config);
        ADD_FAILURE() << "an unusable bound was accepted";
    } catch (const PubSubError& e) {
        EXPECT_EQ(e.status(), PubSubStatus::kInvalidArgument);
        EXPECT_NE(std::string(e.what()).find("4095"), std::string::npos) << e.what();
    }
}

// ===========================================================================
// The fixed schema bound (owner decision 2026-09-15: no more document properties)
// ===========================================================================

// The bound is fixed at `kSchemaPayloadBytes` now, not a document property: a real Arrow IPC
// schema too large for it is refused at CreateTopic. `SerializeSchemaIpc`'s own size is checked
// first, not assumed, so this test cannot pass on a schema that happens to still fit.
TEST(FastDdsConfig, ASchemaLargerThanTheSchemaBoundIsRefused) {
    const std::vector<uint8_t> ipc = SerializeSchemaIpc(MakeOversizedSchema().get());
    ASSERT_GT(ipc.size(), kSchemaPayloadBytes - 37)
        << "MakeOversizedSchema() no longer exceeds the schema bound; grow it";

    FastDDSPubSubProvider provider(ProviderConfig{});
    try {
        provider.CreateTopic({"schemabound", "toolarge"}, MakeOversizedSchema());
        ADD_FAILURE() << "an oversized schema was accepted";
    } catch (const PubSubError& e) {
        EXPECT_EQ(e.status(), PubSubStatus::kTransportFailure) << e.what();
        EXPECT_NE(std::string(e.what()).find(std::to_string(ipc.size())), std::string::npos)
            << e.what();
        EXPECT_NE(std::string(e.what()).find(std::to_string(kSchemaPayloadBytes)),
                  std::string::npos)
            << e.what();
    }
}

// A throw invites a retry, so a failed schema announcement has to leave nothing behind for that
// retry to short-circuit on. Same subject as the test above, retried.
TEST(FastDdsConfig, AFailedSchemaAnnouncementCanBeRetried) {
    FastDDSPubSubProvider provider(ProviderConfig{});

    auto announce = [&provider] {
        try {
            provider.CreateTopic({"schemabound", "retry"}, MakeOversizedSchema());
        } catch (const PubSubError& e) {
            return std::string(e.what());
        }
        return std::string("returned without announcing");
    };

    EXPECT_NE(announce().find("failed to announce the schema"), std::string::npos);
    EXPECT_NE(announce().find("failed to announce the schema"), std::string::npos);
}

// ===========================================================================
// The Publish contract: CreateTopic first, always
// ===========================================================================

// The DataWriter is created in CreateTopic now, not lazily on first Publish (design item 3), so a
// topic this provider only Subscribed to -- never CreateTopic'd -- has no writer to publish
// through. Audited against both test files: every existing Publish call is already preceded by a
// CreateTopic call on the same provider and topic, so this is the one new TEST that pins the
// contract rather than a fix to an existing one.
TEST(FastDdsConfig, PublishOnASubscribedTopicWithoutCreateTopicIsRefused) {
    FastDDSPubSubProvider provider(ProviderConfig{});
    static_cast<void>(
        provider.Subscribe({"subscribedonly", "topic"},
                           [](const uint8_t*, size_t, const SharedSchema&, const Attachments&) {}));
    try {
        provider.Publish({"subscribedonly", "topic"}, MakeEncoder(1));
        ADD_FAILURE() << "Publish on a topic only Subscribed to, never CreateTopic'd, was accepted";
    } catch (const PubSubError& e) {
        EXPECT_EQ(e.status(), PubSubStatus::kTopicNotDeclared) << e.what();
    }
}

// ===========================================================================
// The construction-time warning when the document defines no default profiles
// ===========================================================================

namespace {

// This file's one poll-free wait -- same pattern as test_fast_dds_pubsub_provider.cpp's own
// WaitUntil/NotifyWaiters (a separate translation unit, so its anonymous-namespace copy is not
// reachable here): a producer (the log consumer below) calls NotifyWaiters() right after changing
// whatever a WaitUntil() predicate reads, so the waiter blocks on a condition_variable instead of
// polling.
std::mutex g_wait_mutex;
std::condition_variable g_wait_cv;

void NotifyWaiters() {
    std::lock_guard<std::mutex> lock(g_wait_mutex);
    g_wait_cv.notify_all();
}

template <class Pred>
bool WaitUntil(Pred pred, std::chrono::milliseconds budget) {
    std::unique_lock<std::mutex> lock(g_wait_mutex);
    return g_wait_cv.wait_for(lock, budget, std::move(pred));
}

// Counts messages containing `is_default_profile`, the substring the constructor's warning names
// (fast_dds_pubsub_provider.cpp). Not eprosima::fastdds::dds::Log::ClearConsumers: that would drop
// the default stdout consumer along with it -- RegisterConsumer only adds one, the same pattern as
// SchemaConflictLogConsumer in test_fast_dds_pubsub_provider.cpp.
class DefaultProfileWarningLogConsumer : public eprosima::fastdds::dds::LogConsumer {
   public:
    void Consume(const eprosima::fastdds::dds::Log::Entry& entry) override {
        if (entry.message.find("is_default_profile") == std::string::npos) return;
        {
            std::lock_guard<std::mutex> lk(m);
            ++count;
        }
        NotifyWaiters();
    }

    std::mutex m;
    int count = 0;
};

}  // namespace

// An anchor-only document defines neither role's default profile, so construction logs the
// warning once. Verbosity is raised on purpose: probed the same way
// EveryCallbackLogsIncludingBothSchemaChannelBranches (test_fast_dds_pubsub_provider.cpp) already
// documents it -- this process's default runtime verbosity admits ERROR only, and
// EPROSIMA_LOG_WARNING is gated at runtime by `Log::GetVerbosity() >= Log::Kind::Warning`, closed
// by default. Every TEST() here runs in its own process (gtest_discover_tests DISCOVERY_MODE
// PRE_TEST, see the top of this file), so raising it has nothing else to leak into.
TEST(FastDdsConfig, AnchorOnlyDocumentWarnsAboutMissingDefaultProfiles) {
    eprosima::fastdds::dds::Log::SetVerbosity(eprosima::fastdds::dds::Log::Kind::Warning);

    auto* consumer = new DefaultProfileWarningLogConsumer();
    eprosima::fastdds::dds::Log::RegisterConsumer(
        std::unique_ptr<eprosima::fastdds::dds::LogConsumer>(consumer));

    ProviderConfig config;
    config.domain_id = kDomainMissingDefaultWarning;
    config.document = kAnchorOnly;
    FastDDSPubSubProvider provider(config);

    // The constructor's EPROSIMA_LOG_WARNING call ran synchronously on this thread, so QueueLog
    // already queued it before Flush() is reached below; Flush() blocks until the logging thread
    // has consumed everything queued as of this call (Log::Flush(), Log.cpp), so the WaitUntil
    // below is a safety net for Consume() dispatch, not for anything still outstanding.
    eprosima::fastdds::dds::Log::Flush();
    WaitUntil(
        [&] {
            std::lock_guard<std::mutex> lk(consumer->m);
            return consumer->count >= 1;
        },
        std::chrono::seconds(2));
    eprosima::fastdds::dds::Log::Flush();

    std::lock_guard<std::mutex> lk(consumer->m);
    EXPECT_EQ(consumer->count, 1)
        << "an anchor-only document (no is_default_profile data_writer/data_reader) did not log "
           "the construction-time warning exactly once";
}

// The built-in document defines both is_default_profile profiles, so construction logs nothing: a
// caller that never supplies a document sees no warning about its own default document.
TEST(FastDdsConfig, BuiltInDocumentLogsNoDefaultProfileWarning) {
    eprosima::fastdds::dds::Log::SetVerbosity(eprosima::fastdds::dds::Log::Kind::Warning);

    auto* consumer = new DefaultProfileWarningLogConsumer();
    eprosima::fastdds::dds::Log::RegisterConsumer(
        std::unique_ptr<eprosima::fastdds::dds::LogConsumer>(consumer));

    ProviderConfig config;
    config.domain_id = kDomainBuiltInNoWarning;
    FastDDSPubSubProvider provider(config);

    eprosima::fastdds::dds::Log::Flush();
    std::lock_guard<std::mutex> lk(consumer->m);
    EXPECT_EQ(consumer->count, 0)
        << "the built-in document (both is_default_profile data_writer and data_reader) logged "
           "the missing-default-profile warning";
}

// XML admits either quote around an attribute value, and so does the parser Fast DDS reads the
// document with, so a document that spells the attribute is_default_profile='true' DOES define its
// defaults and must not be warned about.
TEST(FastDdsConfig, SingleQuotedDefaultProfileAttributeLogsNoWarning) {
    eprosima::fastdds::dds::Log::SetVerbosity(eprosima::fastdds::dds::Log::Kind::Warning);

    auto* consumer = new DefaultProfileWarningLogConsumer();
    eprosima::fastdds::dds::Log::RegisterConsumer(
        std::unique_ptr<eprosima::fastdds::dds::LogConsumer>(consumer));

    ProviderConfig config;
    config.domain_id = kDomainSingleQuotedDefault;
    config.document = Document(
        R"(    <data_writer profile_name="default_writer" is_default_profile='true'>
      <qos><reliability><kind>RELIABLE</kind></reliability></qos>
    </data_writer>
    <data_reader profile_name="default_reader" is_default_profile='true'>
      <qos><reliability><kind>RELIABLE</kind></reliability></qos>
    </data_reader>)");
    FastDDSPubSubProvider provider(config);

    eprosima::fastdds::dds::Log::Flush();
    std::lock_guard<std::mutex> lk(consumer->m);
    EXPECT_EQ(consumer->count, 0)
        << "a document whose is_default_profile attributes are single-quoted was reported as "
           "defining no default profiles";
}
