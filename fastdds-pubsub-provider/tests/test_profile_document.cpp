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
//   1. an empty document                     -> Fletcher's built-in       (forcing row 1)
//   2. an anchor-only, non-empty document    -> Fast DDS's own default   (forcing row 2)
//   3. a whole profile absent for one role   -> that role's built-in      (SchemaChannel..., C2-2)
//   4. a whole document that will not parse  -> refused, never defaulted  (Malformed...)
//   5. the POLICIES a supplied profile omits -> Fast DDS's default, NOT Fletcher's
//                                               (MinimalProfileTakesFastDdsDefaultsNotFletchers)
//   6. an EMPTY document                     -> MakeFletcherDefaultReaderQos() on the reader too
//                                               (AnEmptyDocumentIsFletchersBuiltInEverywhere); a
//                                               non-empty one with no reader profile falls to Fast
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
// ── One TEST, at most one distinct non-empty document ──────────────────────────────────────
// Fast DDS's profile registry is process-wide and keeps a profile for the life of the process
// (internal/profile_document.hpp): a second document naming a profile another document in this
// process already registered is refused, even if it is refused only after the first has already
// won. So within ONE `TEST()` every provider built from a non-empty document must use the SAME
// bytes, and a TEST may load at most one distinct non-empty document — a TEST whose second,
// different document is MEANT to be refused is the exception. `gtest_discover_tests(...
// DISCOVERY_MODE PRE_TEST)` in `tests/CMakeLists.txt` already runs one `TEST()` per process under
// CTest, which is what makes this workable; running this binary bare on the whole suite collides
// by design (`FastDdsConfig.*` run together would fight over the same profile names).

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/domain/DomainParticipantListener.hpp>
#include <fastdds/dds/publisher/Publisher.hpp>
#include <fastdds/dds/subscriber/Subscriber.hpp>
#include <fletcher/core/write_buffer.hpp>
#include <fletcher/fastdds_pubsub_provider/fast_dds_pubsub_provider.hpp>
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

// One `fletcher.*` (or foreign) vendor property inside the anchor's <rtps><propertiesPolicy>.
std::string AnchorProperty(const std::string& name, const std::string& value) {
    return R"(
      <rtps>
        <propertiesPolicy>
          <properties>
            <property><name>)" +
           name + R"(</name><value>)" + value + R"(</value></property>
          </properties>
        </propertiesPolicy>
      </rtps>)";
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

// Ten slots of the payload bound rather than Fletcher's hundred: a bounded plain type reserves
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

int32_t AwaitRow(const std::atomic<int32_t>& cell,
                 std::chrono::milliseconds budget = std::chrono::seconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (cell.load() < 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return cell.load();
}

// A row 16 bytes past `bound`, for the two loan-publish tests: the loaned path throws out of
// Publish, the serialising path swallows the overflow inside serialize().
PubSubProvider::RowEncoder OversizedRow(uint32_t bound) {
    return [bound](WriteBuffer& buf) {
        std::vector<uint8_t> blob(bound + 16, 0x5A);
        buf.Append(blob.data(), blob.size());
    };
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
constexpr uint32_t kDomainLoan = 98;
constexpr uint32_t kDomainSchemaBound = 99;

}  // namespace

// ===========================================================================
// THE FORCING TEST(S)
//
// The setting an endpoint ANNOUNCES on the network is the document's — not merely that the
// document loaded. Originally one TEST looping a four-row table; now four TESTs, one per row,
// because Fast DDS's profile registry is process-wide. Rows 3 and 4 each define a DIFFERENT
// "default_writer" profile, which collides if both are loaded in one process (see the
// one-document-per-TEST note at the top of this file). Row 1 (the empty document) moved out to
// its own TEST too, now for a different reason than it used to: an empty document ignores the
// registry outright, unconditionally (design rule 1 -- AnEmptyDocumentIgnoresTheRegistry pins
// that directly against a registry that is NOT empty), so its own process here is only about
// keeping row 1's document from colliding with rows 2-4's, not about an empty-registry
// precondition. Rows 3 and 4 still differ from Fast DDS's default AND from each other, so a
// provider that hard-codes any single value reddens at least one of the four TESTs; row 2 (here)
// pins that an anchor-only document — the shape every `fletcher.*`-property document has — still
// reaches Fast DDS's registry and gets a live writer, not that its QoS differs from Fletcher's
// built-in: durability and reliability happen to coincide with both.
// `AnAnchorOnlyDocumentResolvesToFastDdsDefaults` is the in-process, whole-struct test that
// actually tells them apart, on history and resource_limits, which are not on the wire.
// ===========================================================================

// Row 1, split into its own TEST/process (see the note above), whole-struct rather than through
// discovery: `history` and `resource_limits` are not on the wire, and they are exactly where
// Fletcher's built-in and Fast DDS's own default disagree. The seeding mirrors what the
// constructor does for an empty document (fast_dds_pubsub_provider.cpp): Publisher::
// set_default_datawriter_qos / Subscriber::set_default_datareader_qos, never a registry lookup.
TEST(FastDdsConfig, AnEmptyDocumentIsFletchersBuiltInEverywhere) {
    XmlProbe probe(kDomainProbe);
    ASSERT_TRUE(probe.ok());

    ASSERT_EQ(
        probe.publisher().set_default_datawriter_qos(internal::MakeFletcherDefaultWriterQos()),
        RETCODE_OK);
    ASSERT_EQ(
        probe.subscriber().set_default_datareader_qos(internal::MakeFletcherDefaultReaderQos()),
        RETCODE_OK);

    const DataWriterQos writer =
        internal::ResolveWriterQos(probe.publisher(), "forcing/empty", /*registry=*/false);
    const DataReaderQos reader =
        internal::ResolveReaderQos(probe.subscriber(), "forcing/empty", /*registry=*/false);
    EXPECT_TRUE(writer == internal::MakeFletcherDefaultWriterQos());
    EXPECT_TRUE(reader == internal::MakeFletcherDefaultReaderQos());
}

// Rule 1 in the flesh: an EMPTY document ignores the registry entirely, even when another
// provider in the SAME process has already loaded a non-empty one -- `fletcher_writer` /
// `fletcher_reader` are not special names any more, so there is no name left for an empty
// document to accidentally collide with; if it consulted the registry it would pick up A's
// default writer profile instead of Fletcher's built-in. Durability tells the two apart: VOLATILE
// (A's document) vs TRANSIENT_LOCAL (Fletcher's built-in, B's empty document).
TEST(FastDdsConfig, AnEmptyDocumentIgnoresTheRegistry) {
    const std::string document_a = Document(
        WriterProfile("default_writer", "<durability><kind>VOLATILE</kind></durability>", kTenSlots,
                      /*is_default=*/true));

    DiscoveryObserver observer(kDomainTwoInstances);
    ASSERT_TRUE(observer.ok());

    ProviderConfig config_a;
    config_a.domain_id = kDomainTwoInstances;
    config_a.document = document_a;
    ProviderConfig config_b;
    config_b.domain_id = kDomainTwoInstances;
    config_b.document = "";

    FastDDSPubSubProvider a(config_a);
    FastDDSPubSubProvider b(config_b);

    a.CreateTopic({"ignoresregistry", "a"}, MakeSchema());
    b.CreateTopic({"ignoresregistry", "b"}, MakeSchema());
    a.Publish({"ignoresregistry", "a"}, MakeEncoder(1));
    b.Publish({"ignoresregistry", "b"}, MakeEncoder(1));

    Announced from_a;
    Announced from_b;
    ASSERT_TRUE(observer.AwaitWriter("ignoresregistry/a", &from_a));
    ASSERT_TRUE(observer.AwaitWriter("ignoresregistry/b", &from_b));
    EXPECT_EQ(from_a.durability, VOLATILE_DURABILITY_QOS);
    EXPECT_EQ(from_b.durability, TRANSIENT_LOCAL_DURABILITY_QOS)
        << "the empty document consulted the registry: it picked up provider a's default writer "
           "profile instead of Fletcher's built-in";
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
        internal::ResolveReaderQos(probe.subscriber(), "readersilence/topic", /*registry=*/true);
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
// all six policies including `history` (KEEP_ALL — what stops a RELIABLE writer overwriting
// unacked samples, i.e. silent row loss) and `resource_limits` (max_samples 100 — 5000 would
// overflow the data-sharing segment's 32-bit size and drop the endpoint back to the transport),
// neither of which is observable in discovery data. `DataWriterQos::operator==` and
// `DataReaderQos::operator==` each compare 22 of 22 members, and `RTPSEndpointQos::operator==`
// carries `history_memory_policy`, so a block that silently loses the zero-copy read path
// reddens here too.
//
// If some policy provably cannot be transcribed into XML, this assert says so and the README
// names it as a known non-transcribable difference — that is the honest outcome, not a weaker
// assert.
TEST(FastDdsConfig, DefaultProfileTranscriptionIsExact) {
    XmlProbe probe(kDomainProbe);
    ASSERT_TRUE(probe.ok());

    // The block is read OFF DISK, out of the README itself. It used to be a copy of that block
    // held here, which made the README's claim — "this block cannot drift from the code without a
    // test going red" — false in one direction: editing the README alone reddened nothing (review
    // 4a F2 / 4b RECORD, found independently). There is now exactly one copy of the XML in the
    // repository, and it is the published one, so editing EITHER the README block or
    // `MakeFletcherDefault*Qos()` alone reddens this test.
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

    // A truncated or mis-anchored extraction must fail loudly rather than quietly compare a
    // fragment: the two lookups below are satisfied by any document that happens to define the
    // two profile names, so the block's own shape is asserted first.
    ASSERT_NE(published.find("<?xml"), std::string::npos) << "extracted block: " << published;
    ASSERT_NE(published.find("profile_name=\"fletcher_participant\""), std::string::npos)
        << "the published starting point no longer carries the mandatory anchor";

    // The DEFAULT lookup, not the by-name one: it fills `qos` from whichever profile carries
    // `is_default_profile="true"`, so a block that lost that attribute fails this lookup outright
    // instead of still resolving by the literal name "default_writer" / "default_reader" — which
    // is what keeps this test honest about the one thing the published block claims to prove.
    DataWriterQos writer;
    ASSERT_EQ(probe.publisher().get_default_datawriter_qos_from_xml(published, writer), RETCODE_OK)
        << "the README's published starting point no longer defines an is_default_profile=\"true\" "
           "data_writer profile Fast DDS can parse";
    EXPECT_TRUE(writer == internal::MakeFletcherDefaultWriterQos())
        << "the README's published starting point no longer transcribes "
           "MakeFletcherDefaultWriterQos() exactly";

    DataReaderQos reader;
    ASSERT_EQ(probe.subscriber().get_default_datareader_qos_from_xml(published, reader), RETCODE_OK)
        << "the README's published starting point no longer defines an is_default_profile=\"true\" "
           "data_reader profile Fast DDS can parse";
    EXPECT_TRUE(reader == internal::MakeFletcherDefaultReaderQos())
        << "the README's published starting point no longer transcribes "
           "MakeFletcherDefaultReaderQos() exactly";
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
        internal::ResolveWriterQos(probe.publisher(), "anchoronly/topic", /*registry=*/true);
    EXPECT_TRUE(writer == DataWriterQos())
        << "a non-empty document that names no writer profile did not fall back to Fast DDS's own "
           "default -- fletcher_writer / fletcher_reader are not special names any more, so "
           "nothing here should still resolve to Fletcher's built-in";

    const DataReaderQos reader =
        internal::ResolveReaderQos(probe.subscriber(), "anchoronly/topic", /*registry=*/true);
    EXPECT_TRUE(reader == DataReaderQos())
        << "a non-empty document that names no reader profile did not fall back to Fast DDS's own "
           "default";

    // Not vacuous: Fletcher's built-in and Fast DDS's default actually differ, or a leftover
    // Fletcher-fallback would pass this test by accident. They differ on history and on
    // resource_limits, and on nothing discovery can carry.
    ASSERT_FALSE(internal::MakeFletcherDefaultWriterQos() == DataWriterQos())
        << "Fletcher's built-in writer profile is now identical to Fast DDS's default, so this "
           "test can no longer tell them apart";
    ASSERT_FALSE(internal::MakeFletcherDefaultReaderQos() == DataReaderQos());
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
// A minimal profile that mentions ONLY durability must resolve to Fast DDS's `KEEP_LAST(1)`
// history, not Fletcher's `KEEP_ALL`, and to Fast DDS's `max_samples` (5000), not Fletcher's 100.
// It calls the production ladder, so it holds the shape rather than a re-implementation of it.
//
// CORRECTED (review 4b, measured): this comment used to claim that seeding
// `internal::ResolveWriterQos`'s output with `MakeFletcherDefaultWriterQos()` reddens this test
// and nothing else. It reddens NOTHING — the whole suite stays green under that mutation, because
// fast-dds/3.4.0 OVERWRITES the output parameter rather than overlaying onto it.
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

    // Sanity: these two DIFFER, so the assertions below are not vacuous.
    ASSERT_EQ(internal::MakeFletcherDefaultWriterQos().history().kind, KEEP_ALL_HISTORY_QOS);
    ASSERT_EQ(DataWriterQos().history().kind, KEEP_LAST_HISTORY_QOS);

    {
        std::lock_guard<std::mutex> lock(internal::profile_registry_mutex);
        internal::LoadDocumentOnce(document);
    }
    const DataWriterQos resolved =
        internal::ResolveWriterQos(probe.publisher(), "fletcher_writer", /*registry=*/true);

    EXPECT_EQ(resolved.durability().kind, VOLATILE_DURABILITY_QOS) << "the profile was not applied";
    EXPECT_EQ(resolved.history().kind, KEEP_LAST_HISTORY_QOS)
        << "a policy the profile omitted fell back to Fletcher's KEEP_ALL — that is the merge "
           "semantics owner ruling 2026-09-02 rejected";
    EXPECT_EQ(resolved.history().depth, DataWriterQos().history().depth);
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
        internal::ResolveWriterQos(probe.publisher(), "fletcher_writer", /*registry=*/true);
    EXPECT_EQ(silent_on_durability.durability().kind, TRANSIENT_LOCAL_DURABILITY_QOS)
        << "the XML path's writer durability default moved; "
           "AFletcherWriterProfileDecidesReliability's expectation and this comment both describe "
           "it and must move with it";
}

// The same rule on the reader. Was the data-sharing line (Fletcher's built-in OFF, Fast DDS's
// default AUTO); since item D (owner decision 2026-09-14) both are AUTOMATIC, so that line no
// longer tells "inherited Fletcher's default" apart from "fell to Fast DDS's own" -- reliability
// does the same job now (Fletcher's built-in RELIABLE, Fast DDS's own default BEST_EFFORT,
// QosPolicies.hpp), alongside history (KEEP_ALL vs KEEP_LAST), which this test already pinned. A
// supplied reader profile owns the decision either way (handled residue H2 — a Fletcher floor
// would mean the document does not really configure QoS, and the PDA-ABI-7 defect hunt needs it
// on). Own TEST/process: a distinct document from the two writer-side TESTs above.
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
        internal::ResolveReaderQos(probe.subscriber(), "fletcher_reader", /*registry=*/true);
    ASSERT_EQ(internal::MakeFletcherDefaultReaderQos().reliability().kind,
              RELIABLE_RELIABILITY_QOS);
    EXPECT_EQ(reader_resolved.history().kind, KEEP_LAST_HISTORY_QOS);
    EXPECT_EQ(reader_resolved.reliability().kind, DataReaderQos().reliability().kind)
        << "a supplied reader profile is not the whole QoS: Fletcher's RELIABLE default leaked "
           "underneath it";
}

// C2-5 — the strip is exact. The two `fletcher.*` properties this provider consumes never reach
// `create_participant`, so a `<propagate>true</propagate>` cannot put a Fletcher key into DDS
// discovery data; every OTHER property survives untouched, because an over-reaching strip would
// silently drop `dds.sec.*` and the participant would come up UNSECURED with no error.
TEST(FastDdsConfig, ForeignPropertiesSurviveTheStrip) {
    PropertyPolicyQos properties;
    properties.properties().emplace_back("dds.sec.auth.plugin", "builtin.PKI-DH");
    properties.properties().emplace_back("fletcher.loan_publish", "true");
    properties.properties().emplace_back("fletcher.max_schema_bytes", "131072");
    properties.properties().emplace_back("fletcherish.but.not.ours", "kept");

    const internal::FletcherProperties consumed = internal::ConsumeFletcherProperties(properties);
    EXPECT_TRUE(consumed.loan_publish);
    EXPECT_EQ(consumed.max_schema_bytes, 131072u);

    std::vector<std::string> names;
    for (const auto& property : properties.properties()) names.push_back(property.name());
    EXPECT_EQ(names, (std::vector<std::string>{"dds.sec.auth.plugin", "fletcherish.but.not.ours"}));
}

// Pins the fix for the Fast DDS 3.4.0 teardown hang: `~StatefulReader` clears `is_alive_` before
// `DataSharingListener::stop()`, and `DataSharingListener::process_new_data` spins on a refused
// payload without checking `is_running_`. Reproduced by
// `ConflictingCrossProviderSchemaIsLoggedNotSwallowed` at roughly 1 in 2 isolated runs before the
// fix. No DDS entities needed: the QoS is data, not discovery.
TEST(FastDdsConfig, TheSchemaChannelDeclinesDataSharing) {
    EXPECT_EQ(internal::MakeSchemaChannelWriterQos().data_sharing().kind(),
              eprosima::fastdds::dds::OFF);
    EXPECT_EQ(internal::MakeSchemaChannelReaderQos().data_sharing().kind(),
              eprosima::fastdds::dds::OFF);
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

// Design rule 2: a `fletcher.*` property outside the anchor is simply not read any more — no
// guard, because there is nothing left for it to defeat (the built-in floor a misplaced-but-inert
// property used to bypass is gone). The one placement that is STILL refused is this one, and for
// an unrelated reason: `<propertiesPolicy>` inside a `<data_writer>`'s `<qos>` is rejected by Fast
// DDS's own parser, so the whole document fails to load — a parse failure, not a semantic refusal.
TEST(FastDdsConfig, AFletcherPropertyInsideAnEndpointQosIsRefusedAsMalformed) {
    ProviderConfig config;
    config.domain_id = kDomainProbe;
    config.document = Document(
        WriterProfile("fletcher_writer", EndpointProperty("fletcher.loan_publish", "true")));
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
        internal::ResolveReaderQos(probe.subscriber(), "readertopic/special", /*registry=*/true);
    EXPECT_EQ(special.durability().kind, VOLATILE_DURABILITY_QOS)
        << "the reader profile named after the topic did not win";

    const DataReaderQos ordinary =
        internal::ResolveReaderQos(probe.subscriber(), "readertopic/ordinary", /*registry=*/true);
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

// Moved out of the table above: this document registers the anchor (unlike the parse-failure
// rows), so it needs its own process.
TEST(FastDdsConfig, ATypodFletcherPropertyIsRefused) {
    ProviderConfig config;
    config.document = Document("", AnchorProperty("fletcher.loanpublish", "true"));
    try {
        FastDDSPubSubProvider provider(config);
        ADD_FAILURE() << "the document was accepted";
    } catch (const PubSubError& e) {
        EXPECT_EQ(e.status(), PubSubStatus::kInvalidArgument);
        EXPECT_NE(std::string(e.what()).find("fletcher.loanpublish"), std::string::npos)
            << "refused, but not for this reason: " << e.what();
    }
}

TEST(FastDdsConfig, AnUnparseableLoanPublishValueIsRefused) {
    ProviderConfig config;
    config.document = Document("", AnchorProperty("fletcher.loan_publish", "yes"));
    try {
        FastDDSPubSubProvider provider(config);
        ADD_FAILURE() << "the document was accepted";
    } catch (const PubSubError& e) {
        EXPECT_EQ(e.status(), PubSubStatus::kInvalidArgument);
        EXPECT_NE(std::string(e.what()).find("yes"), std::string::npos)
            << "refused, but not for this reason: " << e.what();
    }
}

TEST(FastDdsConfig, AnUnparseableSchemaBoundValueIsRefused) {
    ProviderConfig config;
    config.document = Document("", AnchorProperty("fletcher.max_schema_bytes", "lots"));
    try {
        FastDDSPubSubProvider provider(config);
        ADD_FAILURE() << "the document was accepted";
    } catch (const PubSubError& e) {
        EXPECT_EQ(e.status(), PubSubStatus::kInvalidArgument);
        EXPECT_NE(std::string(e.what()).find("lots"), std::string::npos)
            << "refused, but not for this reason: " << e.what();
    }
}

// The schema channel's TopicDataType is FletcherSamplePubSubType too, under a different registered
// name, so its bound has to satisfy the same rule max_payload_bytes does: a value that parses fine
// as an integer but is not a multiple of 4 is refused just the same.
TEST(FastDdsConfig, AnUnusableSchemaBoundIsRefused) {
    ProviderConfig config;
    config.document = Document("", AnchorProperty("fletcher.max_schema_bytes", "10"));
    try {
        FastDDSPubSubProvider provider(config);
        ADD_FAILURE() << "the document was accepted";
    } catch (const PubSubError& e) {
        EXPECT_EQ(e.status(), PubSubStatus::kInvalidArgument);
        EXPECT_NE(std::string(e.what()).find("10"), std::string::npos)
            << "refused, but not for this reason: " << e.what();
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
// struct defaulted to, and it has to be, because the bound is part of the registered DDS type
// name and a different number silently stops endpoints discovering each other.
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
// The two settings a QoS profile cannot express
// ===========================================================================

// `fletcher.loan_publish=true` selects the loaned publish path: a row past the bound THROWS out
// of Publish. Own TEST/process: this document and the "absent" one below both define
// "fletcher_writer" with different anchor properties, so they cannot share a process (Fast DDS
// profile names are process-wide).
TEST(FastDdsConfig, LoanPublishTrueUsesTheLoanedPublishPath) {
    const std::string writer = WriterProfile(
        "fletcher_writer", "<durability><kind>TRANSIENT_LOCAL</kind></durability>", kTenSlots);
    ProviderConfig loaned;
    loaned.domain_id = kDomainLoan;
    loaned.document = Document(writer, AnchorProperty("fletcher.loan_publish", "true"));

    FastDDSPubSubProvider provider(loaned);
    provider.CreateTopic({"loancfg", "on"}, MakeSchema());
    try {
        provider.Publish({"loancfg", "on"}, OversizedRow(provider.PayloadBytes()));
        ADD_FAILURE() << "fletcher.loan_publish=true did not take: an oversized row was "
                         "accepted, which only the serialising path does";
    } catch (const PubSubError& e) {
        EXPECT_EQ(e.status(), PubSubStatus::kPayloadTooLarge);
    }
}

// The absence of `fletcher.loan_publish` selects the serialising path: the same oversized row is
// accepted (the overflow happens inside serialize(), reported to Fast DDS, the sample dropped).
TEST(FastDdsConfig, LoanPublishAbsentUsesTheSerialisingPublishPath) {
    const std::string writer = WriterProfile(
        "fletcher_writer", "<durability><kind>TRANSIENT_LOCAL</kind></durability>", kTenSlots);
    ProviderConfig serialising;
    serialising.domain_id = kDomainLoan;
    serialising.document = Document(writer);

    FastDDSPubSubProvider provider(serialising);
    provider.CreateTopic({"loancfg", "off"}, MakeSchema());
    EXPECT_NO_THROW(provider.Publish({"loancfg", "off"}, OversizedRow(provider.PayloadBytes())))
        << "the publish path loaned although no fletcher.loan_publish property was given";
}

// `fletcher.max_schema_bytes` bounds the internal schema channel: a real Arrow IPC schema is
// rejected on the channel when the property sets the bound below it. Own TEST/process: this
// document and the anchor-only one below both define "fletcher_participant" with different
// content (one carries the property, the other does not), so they cannot share a process.
TEST(FastDdsConfig, ASchemaBoundBelowTheSchemaIsRefused) {
    ProviderConfig config;
    config.domain_id = kDomainSchemaBound;
    config.document = Document("", AnchorProperty("fletcher.max_schema_bytes", "16"));
    FastDDSPubSubProvider provider(config);
    EXPECT_THROW(provider.CreateTopic({"schemabound", "toobig"}, MakeSchema()), PubSubError);
}

// With the property absent, the same schema delivers end to end.
TEST(FastDdsConfig, ASchemaBoundAboveTheSchemaDelivers) {
    ProviderConfig config;
    config.domain_id = kDomainSchemaBound;
    config.document = std::string(kAnchorOnly);
    FastDDSPubSubProvider pub(config);
    FastDDSPubSubProvider sub(config);
    pub.CreateTopic({"schemabound", "fits"}, MakeSchema());

    std::atomic<int32_t> received{-1};
    SubscriptionResult result = sub.Subscribe(
        {"schemabound", "fits"},
        [&](const uint8_t* data, size_t len, const SharedSchema&, const Attachments&) {
            if (len >= 5) received.store(DecodeRow(data));
        });
    SharedSchema schema;
    ASSERT_EQ(result.schema.Wait(std::chrono::seconds(10), &schema), PubSubStatus::kOk)
        << result.schema.Message();

    // The schema resolving says only that the SCHEMA is known, not that `sub`'s data reader has
    // finished matching `pub`'s writer yet -- the two are separate participants, so matching runs
    // through ordinary asynchronous discovery, not the synchronous intraprocess fast-path.
    // `kAnchorOnly` defines no endpoint profiles, so both sides sit on Fast DDS's own defaults,
    // which are asymmetric (QosPolicies.hpp): the writer's is TRANSIENT_LOCAL, but the reader's is
    // VOLATILE -- and a VOLATILE reader that finishes matching AFTER a row was sent never sees it
    // retroactively. Retry the publish rather than assume one call wins that race.
    for (int attempt = 0; attempt < 20 && received.load() < 0; ++attempt) {
        pub.Publish({"schemabound", "fits"}, MakeEncoder(11));
        AwaitRow(received, std::chrono::milliseconds(250));
    }
    EXPECT_EQ(received.load(), 11);
}

// A throw invites a retry, so a failed schema announcement has to leave nothing behind for that
// retry to short-circuit on. Re-anchored from the retired `max_schema_bytes = 8` option onto the
// document property that replaced it — same subject, same failure, new configuration route.
TEST(FastDdsConfig, AFailedSchemaAnnouncementCanBeRetried) {
    ProviderConfig config;
    config.domain_id = kDomainSchemaBound;
    config.document = Document("", AnchorProperty("fletcher.max_schema_bytes", "16"));
    FastDDSPubSubProvider provider(config);

    auto announce = [&provider] {
        try {
            provider.CreateTopic({"schemabound", "retry"}, MakeSchema());
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
