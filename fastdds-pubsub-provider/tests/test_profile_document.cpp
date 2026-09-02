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
//   2. an anchor-only, non-empty document    -> Fletcher's built-in       (forcing row 2)
//   3. a whole profile absent for one role   -> that role's built-in      (SchemaChannel..., C2-2)
//   4. a whole document that will not parse  -> refused, never defaulted  (Malformed...)
//   5. the POLICIES a supplied profile omits -> Fast DDS's default, NOT Fletcher's
//                                               (MinimalProfileTakesFastDdsDefaultsNotFletchers)
//   6. a document that says nothing about the READER
//                                            -> MakeFletcherDefaultReaderQos(), whose
//                                               data_sharing().off() holds back the measured
//                                               receive-side row-loss defect
//                                               (WriterOnlyDocumentLeavesTheReaderOn...)
//
// ── Two shapes of assertion, and why both are needed ───────────────────────────────────────
// Where the claim is that a QoS reached a live ENDPOINT, the value is read out of DDS discovery
// from a bare observer participant, with a **per-row latch and a hard timeout**: nothing is
// compared until that row's discovery callback has fired, because a row expecting a value that
// happens to equal a default-constructed policy would otherwise prove nothing while staying
// green. Where the claim is about RESOLUTION, the QoS structs are compared WHOLE and in-process
// — no discovery, every policy — because `history` and `resource_limits` are `optional` in
// `PublicationBuiltinTopicData` and are simply not observable on the network.

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

// Every non-empty document must carry this anchor, even empty of policies: `get_*_from_xml`
// reports "malformed" and "no such profile" with the same code, so one mandatory profile is the
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

std::string WriterProfile(const std::string& profile_name, const std::string& qos_body,
                          const std::string& topic_body = "") {
    return R"(    <data_writer profile_name=")" + profile_name + R"(">
      <qos>)" +
           qos_body +
           R"(</qos>
      <topic>)" +
           topic_body +
           R"(</topic>
    </data_writer>)";
}

std::string ReaderProfile(const std::string& profile_name, const std::string& qos_body,
                          const std::string& topic_body = "") {
    return R"(    <data_reader profile_name=")" + profile_name + R"(">
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
// discovery at all. `get_*_qos_from_xml` lives on Publisher / Subscriber, hence the scaffolding.
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
    const Publisher& publisher() const { return *publisher_; }
    const Subscriber& subscriber() const { return *subscriber_; }

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
// THE FORCING TEST
//
// The setting an endpoint ANNOUNCES on the network is the document's — not merely that the
// document loaded. One TEST rather than a TEST_P so the ctest name is exact, looping a
// four-row table. Rows 3 and 4 differ from Fast DDS's default AND from each other, so a
// provider that hard-codes any single value reddens at least one row; rows 1 and 2 are the
// omission guards, and row 2 is the only row that catches a provider which reads the document
// but drops Fletcher's built-in whenever the document is non-empty — the shape every
// `fletcher.*`-property document has.
// ===========================================================================
TEST(FastDdsConfig, ProfileDocumentConfiguresQos) {
    struct Row {
        const char* what;
        std::string document;
        const char* topic_leaf;
        DurabilityQosPolicyKind durability;
        ReliabilityQosPolicyKind reliability;
    };

    const std::vector<Row> rows = {
        {"an empty document is Fletcher's built-in profile", "", "empty",
         TRANSIENT_LOCAL_DURABILITY_QOS, RELIABLE_RELIABILITY_QOS},
        // The most common document there is: an anchor and nothing else, which is the shape of
        // every document that exists only to carry a `fletcher.*` property.
        {"an anchor-only document is STILL Fletcher's built-in, not Fast DDS's", kAnchorOnly,
         "anchor", TRANSIENT_LOCAL_DURABILITY_QOS, RELIABLE_RELIABILITY_QOS},
        {"a fletcher_writer profile decides durability",
         Document(WriterProfile("fletcher_writer", "<durability><kind>VOLATILE</kind></durability>",
                                kTenSlots)),
         "volatile", VOLATILE_DURABILITY_QOS, RELIABLE_RELIABILITY_QOS},
        // Expected durability here is TRANSIENT_LOCAL and that is NOT Fletcher's built-in
        // leaking through — it is MEASURED to be Fast DDS's own default on the XML path, which
        // for a writer's `durability` is TRANSIENT_LOCAL rather than `DataWriterQos()`'s
        // VOLATILE (the XML parser fills an RTPS-level `WriterQos`, whose durability default
        // differs from the DDS-level one). `MinimalProfileTakesFastDdsDefaultsNotFletchers`
        // pins that fact directly and distinguishes the two answers to the merge question on
        // `history` and `resource_limits`, where Fletcher's and Fast DDS's DO differ. What this
        // row exists to prove is reliability, and BEST_EFFORT differs from every other row.
        {"a fletcher_writer profile decides reliability",
         Document(WriterProfile("fletcher_writer",
                                "<reliability><kind>BEST_EFFORT</kind></reliability>", kTenSlots)),
         "besteffort", TRANSIENT_LOCAL_DURABILITY_QOS, BEST_EFFORT_RELIABILITY_QOS},
    };

    DiscoveryObserver observer(kDomainForcing);
    ASSERT_TRUE(observer.ok());

    for (const Row& row : rows) {
        SCOPED_TRACE(row.what);
        ProviderConfig config;
        config.domain_id = kDomainForcing;
        config.document = row.document;

        FastDDSPubSubProvider provider(config);
        const std::string topic = std::string("forcing/") + row.topic_leaf;
        provider.CreateTopic({"forcing", row.topic_leaf}, MakeSchema());
        // The DataWriter is created lazily on first publish, so the row has to publish before
        // there is anything for the observer to discover.
        provider.Publish({"forcing", row.topic_leaf}, MakeEncoder(1));

        Announced announced;
        ASSERT_TRUE(observer.AwaitWriter(topic, &announced))
            << "no writer discovery for " << topic << " — nothing was compared";
        EXPECT_EQ(announced.durability, row.durability);
        EXPECT_EQ(announced.reliability, row.reliability);
    }
}

// A profile named after the topic wins over `fletcher_writer`. Two topics on ONE instance, only
// one of them named by a profile: the discovered durability must differ between them, which is
// what a provider that either dropped the topic-name lookup or applied the topic profile
// everywhere cannot produce.
TEST(FastDdsConfig, PerTopicProfileOverridesTheDefault) {
    const std::string document =
        Document(WriterProfile("fletcher_writer", "<durability><kind>VOLATILE</kind></durability>",
                               kTenSlots) +
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
    const std::string document = Document(ReaderProfile(
        "fletcher_reader", "<durability><kind>VOLATILE</kind></durability>", kTenSlots));

    DiscoveryObserver observer(kDomainReader);
    ASSERT_TRUE(observer.ok());

    ProviderConfig config;
    config.domain_id = kDomainReader;
    config.document = document;
    FastDDSPubSubProvider provider(config);

    SubscriptionResult result =
        provider.Subscribe({"readercfg", "topic"},
                           [](const uint8_t*, size_t, const SharedSchema&, const Attachments&) {});
    (void)result;

    Announced announced;
    ASSERT_TRUE(observer.AwaitReader("readercfg/topic", &announced));
    EXPECT_EQ(announced.durability, VOLATILE_DURABILITY_QOS);
}

// C2-2 — THE READER'S SILENCE, and it touches a live defect.
//
// A document that says something about the writer and NOTHING about the reader must leave the
// reader on `MakeFletcherDefaultReaderQos()`, whose `data_sharing().off()` is the single line
// holding back the measured receive-side row-loss defect: with data-sharing on both ends a
// reader that joins after publication intermittently receives only a subset of the
// TRANSIENT_LOCAL backlog, with no error anywhere (src/qos_defaults.cpp). If the reader's
// fallback slipped to `DATAREADER_QOS_DEFAULT`, data-sharing comes back on (its default kind is
// AUTO) and the signature is silent row loss. `gateway-fastdds-ts` cannot cover this — it runs
// with an empty document, so it never exercises the non-empty-document reader path at all.
TEST(FastDdsConfig, WriterOnlyDocumentLeavesTheReaderOnFletchersDefault) {
    const std::string document = Document(WriterProfile(
        "fletcher_writer", "<durability><kind>VOLATILE</kind></durability>", kTenSlots));

    DiscoveryObserver observer(kDomainReaderSilence);
    ASSERT_TRUE(observer.ok());

    ProviderConfig config;
    config.domain_id = kDomainReaderSilence;
    config.document = document;
    FastDDSPubSubProvider provider(config);

    SubscriptionResult result =
        provider.Subscribe({"readersilence", "topic"},
                           [](const uint8_t*, size_t, const SharedSchema&, const Attachments&) {});
    (void)result;

    Announced announced;
    ASSERT_TRUE(observer.AwaitReader("readersilence/topic", &announced));
    // The one that matters: OFF, not AUTO.
    EXPECT_EQ(announced.data_sharing, eprosima::fastdds::dds::OFF)
        << "the reader fell off MakeFletcherDefaultReaderQos(): receive-side data-sharing is back "
           "on, which is the known intermittent row-loss defect";
    EXPECT_EQ(announced.durability, TRANSIENT_LOCAL_DURABILITY_QOS);
    EXPECT_EQ(announced.reliability, RELIABLE_RELIABILITY_QOS);
}

// The internal `__schema` channel consults NO profile name, ever. It is also this file's negative
// control for "the document reached Fast DDS at all": the same document that turns the data
// writer VOLATILE must leave the schema writer TRANSIENT_LOCAL, so a provider that applied one
// resolved profile to every writer it creates reddens here while greening the forcing test.
TEST(FastDdsConfig, SchemaChannelIgnoresTheDocument) {
    const std::string document = Document(WriterProfile(
        "fletcher_writer", "<durability><kind>VOLATILE</kind></durability>", kTenSlots));

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

// P1's global-state half, measured rather than assumed. `get_*_qos_from_xml` is documented not to
// register anything process-wide, but the Conan package is binary-only so no header can prove it.
// Two providers alive at once, the SAME profile names, DIFFERENT values: under process-global
// registration either the second load collides (construction fails) or the first wins (instance
// B's writer announces A's durability). Both failure modes land here.
TEST(FastDdsConfig, TwoInstancesResolveTheirOwnDocuments) {
    const std::string document_a = Document(WriterProfile(
        "fletcher_writer", "<durability><kind>VOLATILE</kind></durability>", kTenSlots));
    const std::string document_b = Document(WriterProfile(
        "fletcher_writer", "<durability><kind>TRANSIENT_LOCAL</kind></durability>", kTenSlots));

    DiscoveryObserver observer(kDomainTwoInstances);
    ASSERT_TRUE(observer.ok());

    ProviderConfig config_a;
    config_a.domain_id = kDomainTwoInstances;
    config_a.document = document_a;
    ProviderConfig config_b = config_a;
    config_b.document = document_b;

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
    EXPECT_EQ(from_b.durability, TRANSIENT_LOCAL_DURABILITY_QOS);
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

    DataWriterQos writer;
    ASSERT_EQ(probe.publisher().get_datawriter_qos_from_xml(published, writer, "fletcher_writer"),
              RETCODE_OK)
        << "the README's published starting point no longer defines a 'fletcher_writer' profile "
           "Fast DDS can parse";
    EXPECT_TRUE(writer == internal::MakeFletcherDefaultWriterQos())
        << "the README's published starting point no longer transcribes "
           "MakeFletcherDefaultWriterQos() exactly";

    DataReaderQos reader;
    ASSERT_EQ(probe.subscriber().get_datareader_qos_from_xml(published, reader, "fletcher_reader"),
              RETCODE_OK)
        << "the README's published starting point no longer defines a 'fletcher_reader' profile "
           "Fast DDS can parse";
    EXPECT_TRUE(reader == internal::MakeFletcherDefaultReaderQos())
        << "the README's published starting point no longer transcribes "
           "MakeFletcherDefaultReaderQos() exactly";
}

// DEBT-1's SILENCE, and the guard that actually catches M12.
//
// DEBT-1 asked for one forcing-table row: an anchor-only, non-empty document must resolve to
// **Fletcher's** built-in QoS, not Fast DDS's, because that is the shape of every document whose
// only purpose is to carry a `fletcher.*` property, and a provider that fell back to
// `DATAWRITER_QOS_DEFAULT` whenever the document was non-empty would lose rows in production.
// That row is in the forcing test and it stays.
//
// It is not sufficient, and this is MEASURED rather than argued. Fast DDS's own writer defaults
// are durability TRANSIENT_LOCAL and reliability RELIABLE — bit-identical to Fletcher's built-in
// on **both** policies DDS discovery carries (`DataWriterQos()`'s durability is TRANSIENT_LOCAL,
// not the DDS spec's VOLATILE, and the XML path agrees). So the M12 mutation — return Fast DDS's
// default instead of Fletcher's built-in on the not-found branch — was verified to leave the
// WHOLE SUITE green with only the designed row in place (measured when the suite was 80 ctest
// entries / 79 gtest cases; review 4b then measured M12 against the suite AS LANDED, where it
// reddens four tests, this one among them). The two policies that DO differ are `history`
// (Fletcher KEEP_ALL vs Fast DDS KEEP_LAST(1) — the one that stops a RELIABLE writer overwriting
// unacked samples, i.e. silent row loss) and `resource_limits` (100 vs 5000 — 5000 overflows the
// data-sharing segment's 32-bit size and drops the endpoint back to the transport), and both are
// `fastcdr::optional` in the builtin topic data and are simply not on the wire.
//
// Hence: in-process, WHOLE-STRUCT, on the production ladder, for both roles. This is the same
// conclusion BLOCKER B3 reached about the transcription guard, applied to the other direction.
TEST(FastDdsConfig, AnAnchorOnlyDocumentResolvesToFletchersBuiltIn) {
    XmlProbe probe(kDomainProbe);
    ASSERT_TRUE(probe.ok());

    const std::string document(kAnchorOnly);

    const DataWriterQos writer =
        internal::ResolveWriterQos(probe.publisher(), document, "anchoronly/topic");
    EXPECT_TRUE(writer == internal::MakeFletcherDefaultWriterQos())
        << "a non-empty document that names no writer profile did not fall back to Fletcher's "
           "built-in writer profile — the not-found branch is returning something else, and "
           "durability and reliability cannot see the difference because Fast DDS's writer "
           "defaults happen to agree with Fletcher's on exactly those two";

    const DataReaderQos reader =
        internal::ResolveReaderQos(probe.subscriber(), document, "anchoronly/topic");
    EXPECT_TRUE(reader == internal::MakeFletcherDefaultReaderQos())
        << "a non-empty document that names no reader profile did not fall back to "
           "MakeFletcherDefaultReaderQos() — whose data_sharing().off() is what holds back the "
           "measured receive-side row-loss defect";

    // Not vacuous: Fletcher's built-in and Fast DDS's default must actually differ, or the two
    // asserts above are satisfied by any implementation. They differ on history and on
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

    const DataWriterQos resolved =
        internal::ResolveWriterQos(probe.publisher(), document, "minimal/topic");

    EXPECT_EQ(resolved.durability().kind, VOLATILE_DURABILITY_QOS) << "the profile was not applied";
    EXPECT_EQ(resolved.history().kind, KEEP_LAST_HISTORY_QOS)
        << "a policy the profile omitted fell back to Fletcher's KEEP_ALL — that is the merge "
           "semantics owner ruling 2026-09-02 rejected";
    EXPECT_EQ(resolved.history().depth, DataWriterQos().history().depth);
    EXPECT_EQ(resolved.resource_limits().max_samples, DataWriterQos().resource_limits().max_samples)
        << "resource_limits came from Fletcher's built-in rather than from Fast DDS's default";

    // MEASURED, not assumed: "Fast DDS's default" means the default of the path the document
    // actually travels, and the XML parser fills an RTPS-level `WriterQos` whose `durability`
    // default is TRANSIENT_LOCAL — NOT `DataWriterQos()`'s VOLATILE. So a writer profile that
    // omits durability announces TRANSIENT_LOCAL, which happens to coincide with Fletcher's
    // built-in and therefore cannot distinguish the two answers to the merge question. That is
    // exactly why C2-1's assert is anchored on `history` and `resource_limits` above, where the
    // two genuinely differ. Pinned here so a Fast DDS upgrade that changed it is visible.
    const std::string silent_document = Document(
        WriterProfile("fletcher_writer", "<reliability><kind>BEST_EFFORT</kind></reliability>"));
    const DataWriterQos silent_on_durability =
        internal::ResolveWriterQos(probe.publisher(), silent_document, "minimal/topic");
    EXPECT_EQ(silent_on_durability.durability().kind, TRANSIENT_LOCAL_DURABILITY_QOS)
        << "the XML path's writer durability default moved; the forcing test's row-4 expectation "
           "and this comment both describe it and must move with it";

    // The same rule on the reader, and here it is the data-sharing line that moves: Fletcher's
    // built-in turns receive-side data-sharing OFF, Fast DDS's default is AUTO. A supplied reader
    // profile owns that decision (handled residue H2 — a Fletcher floor would mean the document
    // does not really configure QoS, and the PDA-ABI-7 defect hunt needs it on).
    const std::string reader_document = Document(
        ReaderProfile("fletcher_reader", "<durability><kind>VOLATILE</kind></durability>"));
    const DataReaderQos reader_resolved =
        internal::ResolveReaderQos(probe.subscriber(), reader_document, "minimal/topic");
    ASSERT_EQ(internal::MakeFletcherDefaultReaderQos().data_sharing().kind(),
              eprosima::fastdds::dds::OFF);
    EXPECT_EQ(reader_resolved.history().kind, KEEP_LAST_HISTORY_QOS);
    EXPECT_EQ(reader_resolved.data_sharing().kind(), DataReaderQos().data_sharing().kind())
        << "a supplied reader profile is not the whole QoS: Fletcher's data_sharing().off() "
           "leaked underneath it";
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

// 4a F5 — a correctly spelled `fletcher.*` property in the WRONG PROFILE is refused, not inert.
//
// The two settings are provider-wide and are read from the anchor and nowhere else, so the same
// key inside a `<data_writer>` or `<data_reader>` profile does nothing at all. That is the failure
// a MISSPELLED name is already refused for ("a typo'd `fletcher.loanpublish` must not be inert"),
// and it was reachable: an operator writing a per-topic writer profile is the likeliest person to
// put a Fletcher key in it. Unlike the inert-per-topic-PROFILE-NAME limit, this one is NOT a
// substrate limit — `Data{Writer,Reader}Qos::properties()` carries it — so it is refused.
//
// The other placement, `<propertiesPolicy>` inside `<qos>`, is rejected by Fast DDS's own parser,
// which makes the whole document unparseable and takes the existing malformed-document refusal.
// The last row here pins that, so "refused" covers both placements someone might try.
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

// A `<data_writer>`/`<data_reader>` profile carrying `body` directly (NOT inside <qos>).
std::string EndpointProfileWithBody(const std::string& element, const std::string& profile_name,
                                    const std::string& body) {
    return "    <" + element + " profile_name=\"" + profile_name + "\">" + body + "\n    </" +
           element + ">";
}

}  // namespace

TEST(FastDdsConfig, AFletcherPropertyInAnEndpointProfileIsRefused) {
    // The ROLE profiles are refused at CONSTRUCTION, before any endpoint exists.
    for (const char* element : {"data_writer", "data_reader"}) {
        const std::string profile =
            std::string("fletcher_") +
            (std::strcmp(element, "data_writer") == 0 ? "writer" : "reader");
        ProviderConfig config;
        config.domain_id = kDomainProbe;
        config.document = Document(EndpointProfileWithBody(
            element, profile, EndpointProperty("fletcher.loan_publish", "true")));
        try {
            FastDDSPubSubProvider provider(config);
            ADD_FAILURE() << "a fletcher.* property inside the " << element
                          << " profile was accepted, so it is silently inert";
        } catch (const PubSubError& e) {
            const std::string message = e.what();
            EXPECT_EQ(e.status(), PubSubStatus::kInvalidArgument) << message;
            EXPECT_NE(message.find("fletcher.loan_publish"), std::string::npos) << message;
            EXPECT_NE(message.find(element), std::string::npos) << message;
            EXPECT_NE(message.find(profile), std::string::npos) << message;
        }
    }

    // A PER-TOPIC profile can only be checked when its topic's endpoint is resolved — the document
    // cannot be enumerated, only asked about a name — which for a writer is the first publish
    // (the DataWriter is created lazily). It is refused there, so no DataWriter is ever created
    // from a profile carrying a setting nobody reads.
    ProviderConfig per_topic;
    per_topic.domain_id = kDomainProbe;
    per_topic.document = Document(EndpointProfileWithBody(
        "data_writer", "misplaced/topic", EndpointProperty("fletcher.max_schema_bytes", "4096")));
    FastDDSPubSubProvider provider(per_topic);
    provider.CreateTopic({"misplaced", "topic"}, MakeSchema());
    try {
        provider.Publish({"misplaced", "topic"}, MakeEncoder(1));
        ADD_FAILURE() << "a fletcher.* property in a per-topic writer profile was accepted";
    } catch (const PubSubError& e) {
        const std::string message = e.what();
        EXPECT_EQ(e.status(), PubSubStatus::kInvalidArgument) << message;
        EXPECT_NE(message.find("fletcher.max_schema_bytes"), std::string::npos) << message;
        EXPECT_NE(message.find("misplaced/topic"), std::string::npos) << message;
    }
}

// The other placement someone might try, `<propertiesPolicy>` inside a `<data_writer>`'s `<qos>`,
// is rejected by Fast DDS's own parser — and P6 holds for it: the failure propagates to the
// anchor lookup, so the whole document is refused at construction. Between this row and the three
// above, a `fletcher.*` property has nowhere left to sit inertly.
TEST(FastDdsConfig, AFletcherPropertyInsideAnEndpointQosIsRefusedAsMalformed) {
    ProviderConfig config;
    config.domain_id = kDomainProbe;
    config.document = Document(
        WriterProfile("fletcher_writer", EndpointProperty("fletcher.loan_publish", "true")));
    try {
        FastDDSPubSubProvider provider(config);
        ADD_FAILURE() << "a <propertiesPolicy> inside <qos> was accepted; the writer profile is "
                         "unparseable, so this document would run on Fletcher's built-in with "
                         "nothing said";
    } catch (const PubSubError& e) {
        EXPECT_EQ(e.status(), PubSubStatus::kInvalidArgument) << e.what();
        EXPECT_NE(std::string(e.what()).find("not a Fast DDS XML profiles document"),
                  std::string::npos)
            << e.what();
    }
}

// The strip must not over-reach on an endpoint profile either: a FOREIGN property there is the
// operator's business (Fast DDS has its own endpoint properties) and must be left alone. Without
// this row, "refuse anything with a property" would pass the test above.
TEST(FastDdsConfig, AForeignPropertyInAnEndpointProfileIsAccepted) {
    XmlProbe probe(kDomainProbe);
    ASSERT_TRUE(probe.ok());

    const std::string document = Document(EndpointProfileWithBody(
        "data_writer", "fletcher_writer", EndpointProperty("fletcherish.but.not.ours", "kept")));

    DataWriterQos resolved;
    ASSERT_NO_THROW(
        { resolved = internal::ResolveWriterQos(probe.publisher(), document, "foreign/topic"); });
    ASSERT_EQ(resolved.properties().properties().size(), 1u);
    EXPECT_EQ(resolved.properties().properties()[0].name(), "fletcherish.but.not.ours");
}

// 4a F4 — the READER's per-topic lookup, which nothing asserted:
// `PerTopicProfileOverridesTheDefault` is writer-only and `ReaderProfileConfiguresTheReader`
// supplies only `fletcher_reader`, so deleting the reader ladder's topic-name lookup reddened
// nothing. In-process on the production ladder, because the interesting half is the NEGATIVE one:
// the same document must NOT resolve that profile for a different topic.
TEST(FastDdsConfig, ReaderPerTopicProfileOverridesTheDefault) {
    XmlProbe probe(kDomainProbe);
    ASSERT_TRUE(probe.ok());

    const std::string document =
        Document(ReaderProfile("readertopic/special",
                               "<durability><kind>VOLATILE</kind></durability>", kTenSlots) +
                 "\n" +
                 ReaderProfile("fletcher_reader",
                               "<durability><kind>TRANSIENT_LOCAL</kind></durability>", kTenSlots));

    const DataReaderQos special =
        internal::ResolveReaderQos(probe.subscriber(), document, "readertopic/special");
    EXPECT_EQ(special.durability().kind, VOLATILE_DURABILITY_QOS)
        << "the reader profile named after the topic did not win";

    const DataReaderQos ordinary =
        internal::ResolveReaderQos(probe.subscriber(), document, "readertopic/ordinary");
    EXPECT_EQ(ordinary.durability().kind, TRANSIENT_LOCAL_DURABILITY_QOS)
        << "a per-topic reader profile resolved for a topic it is not named after";
}

// 4b S3 — the ladder no longer reports an expected miss as an ERROR.
//
// `get_*_qos_from_xml` logs a miss at ERROR level, and the ladder used a miss as ordinary control
// flow: the whole suite emitted 43 `[XMLPARSER Error] ... profile not found` lines, 4 of them per
// topic on the most common document there is (an anchor plus a `fletcher.*` property). It now
// emits none, because a lookup that provably cannot succeed is not made. What is asserted here is
// the PROOF the skip rests on, since a wrong skip would silently change a QoS:
//
//   * a profile named `N` occurs literally as `profile_name="N"` — UNLESS the name reached the
//     parser through one of the TWO channels that let an attribute value differ from the text
//     between its quotes: an entity or character reference, which needs an `&` in the document,
//     or whitespace normalisation, which needs whitespace in the requested name (review 4c F9);
//   * so a document with no `&`, asked for a name with no whitespace, cannot name a profile that
//     is not a substring of it;
//   * and either precondition takes the lookup, unreasoned.
//
// The last two rows are the ones that matter: each channel's precondition sends the lookup to
// Fast DDS rather than skipping it. Deleting either hatch reddens one of them.
TEST(FastDdsConfig, ALookupThatCannotSucceedIsNotAttempted) {
    XmlProbe probe(kDomainProbe);
    ASSERT_TRUE(probe.ok());

    const std::string anchor_only(kAnchorOnly);
    EXPECT_FALSE(internal::DocumentMayName(anchor_only, internal::kWriterProfile));
    EXPECT_FALSE(internal::DocumentMayDefineWriterProfile(anchor_only));
    EXPECT_FALSE(internal::DocumentMayDefineReaderProfile(anchor_only));

    const std::string writer_only = Document(
        WriterProfile("fletcher_writer", "<durability><kind>VOLATILE</kind></durability>"));
    EXPECT_TRUE(internal::DocumentMayDefineWriterProfile(writer_only));
    EXPECT_TRUE(internal::DocumentMayName(writer_only, internal::kWriterProfile));
    EXPECT_FALSE(internal::DocumentMayDefineReaderProfile(writer_only))
        << "a writer-only document was probed for reader profiles";
    EXPECT_FALSE(internal::DocumentMayName(writer_only, "some/other/topic"));

    // The escape hatch, and the reason the rule is a proof rather than a guess: a topic name
    // holding a character XML must escape does NOT occur literally, so the substring test says
    // "no" — and the `&` test overrides it, the lookup happens, and the profile still wins.
    const std::string escaped = Document(WriterProfile(
        "a&amp;b/topic", "<durability><kind>VOLATILE</kind></durability>", kTenSlots));
    EXPECT_FALSE(escaped.find("a&b/topic") != std::string::npos)
        << "this row is vacuous unless the name really is absent as a literal";
    EXPECT_TRUE(internal::DocumentMayName(escaped, "a&b/topic"));
    const DataWriterQos resolved =
        internal::ResolveWriterQos(probe.publisher(), escaped, "a&b/topic");
    EXPECT_EQ(resolved.durability().kind, VOLATILE_DURABILITY_QOS)
        << "a per-topic profile whose name is XML-escaped in the document stopped resolving - the "
           "lookup skip is no longer sound";

    // The second channel (review 4c F9): XML's attribute-value rules can yield a literal tab/CR/LF
    // inside the quotes as a space, and a CR/CRLF line end as an LF, so a profile the operator
    // wrote as `profile_name="a<LF>b/topic"` can be NAMED `a b/topic` — which is not a literal
    // substring of the document. The substring test alone would then skip a lookup that CAN
    // succeed, silently changing a QoS. The precondition is whitespace in the REQUESTED name, not
    // in the document: an ordinary document is full of LFs between elements, where they are not
    // attribute values and normalise nothing, so scanning the document would skip no lookup at
    // all. Drop the `find_first_of(" \t\r\n")` hatch and this row goes red.
    //
    // MEASURED on fast-dds/3.4.0: resolving `a b/topic` against this document MISSES, so this
    // parser does not apply attribute-value normalisation today — the channel is latent, not
    // live. That is not asserted here: pinning a parser behaviour Fletcher must not depend on
    // either way is exactly what the hatch exists to avoid.
    const std::string normalised = Document(
        WriterProfile("a\nb/topic", "<durability><kind>VOLATILE</kind></durability>", kTenSlots));
    EXPECT_EQ(normalised.find("a b/topic"), std::string::npos)
        << "this row is vacuous unless the name really is absent as a literal";
    EXPECT_TRUE(internal::DocumentMayName(normalised, "a b/topic"))
        << "a name XML could have normalised out of the document's whitespace was not looked up";
}

// ===========================================================================
// Refusals — rung 2, all in the constructor, all kInvalidArgument
// ===========================================================================

// The guard that stops a provider which never reads the document from greening this whole file.
// Each row asserts the STATUS and the quoted text, so a refusal for the wrong reason is not a
// pass.
//
// P6 — "a document containing any malformed profile fails EVERY `get_*_from_xml` call on it" — is
// measured by the fourth row: a valid anchor plus a syntactically broken `fletcher_writer`. That
// row must PASS against the correct implementation. If it FAILS, the document was accepted, P6 is
// false, and the `fletcher_participant` anchor is not sufficient to catch partial malformation —
// STOP AND ASK, and do not delete or weaken the row.
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
        // P6: the anchor itself is well-formed; only fletcher_writer is broken.
        {"a valid anchor plus a syntactically broken fletcher_writer (P6)",
         Document(WriterProfile("fletcher_writer", "<durabilty><kind>VOLATILE</durabilty></kind>")),
         0, "not a Fast DDS XML profiles document"},
        {"a typo'd fletcher property is not inert",
         Document("", AnchorProperty("fletcher.loanpublish", "true")), 0, "fletcher.loanpublish"},
        {"an unparseable fletcher.loan_publish value",
         Document("", AnchorProperty("fletcher.loan_publish", "yes")), 0, "yes"},
        {"an unparseable fletcher.max_schema_bytes value",
         Document("", AnchorProperty("fletcher.max_schema_bytes", "lots")), 0, "lots"},
        {"an anchor <domainId> disagreeing with the deployment's domain",
         Document("", "<domainId>3</domainId>"), 7, "3"},
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

// `fletcher.loan_publish` decides which publish path runs, and the two paths differ observably:
// with a loan, a row past the bound THROWS out of Publish; without one, the overflow happens
// inside serialize(), which reports it to Fast DDS and drops the sample. Hard-code either path
// and one of the two rows goes red.
TEST(FastDdsConfig, LoanPublishComesFromTheDocument) {
    const std::string writer = WriterProfile(
        "fletcher_writer", "<durability><kind>TRANSIENT_LOCAL</kind></durability>", kTenSlots);

    ProviderConfig loaned;
    loaned.domain_id = kDomainLoan;
    loaned.document = Document(writer, AnchorProperty("fletcher.loan_publish", "true"));

    ProviderConfig serialising;
    serialising.domain_id = kDomainLoan;
    serialising.document = Document(writer);

    auto oversized = [](uint32_t bound) {
        return [bound](WriteBuffer& buf) {
            std::vector<uint8_t> blob(bound + 16, 0x5A);
            buf.Append(blob.data(), blob.size());
        };
    };

    {
        FastDDSPubSubProvider provider(loaned);
        provider.CreateTopic({"loancfg", "on"}, MakeSchema());
        try {
            provider.Publish({"loancfg", "on"}, oversized(provider.PayloadBytes()));
            ADD_FAILURE() << "fletcher.loan_publish=true did not take: an oversized row was "
                             "accepted, which only the serialising path does";
        } catch (const PubSubError& e) {
            EXPECT_EQ(e.status(), PubSubStatus::kPayloadTooLarge);
        }
    }
    {
        FastDDSPubSubProvider provider(serialising);
        provider.CreateTopic({"loancfg", "off"}, MakeSchema());
        EXPECT_NO_THROW(provider.Publish({"loancfg", "off"}, oversized(provider.PayloadBytes())))
            << "the publish path loaned although no fletcher.loan_publish property was given";
    }
}

// `fletcher.max_schema_bytes` bounds the internal schema channel. With the property set below any
// real Arrow IPC schema the announcement is rejected on the channel; with it absent the same
// schema is delivered. Ignore the property and the first row goes red.
TEST(FastDdsConfig, SchemaBoundComesFromTheDocument) {
    {
        ProviderConfig config;
        config.domain_id = kDomainSchemaBound;
        config.document = Document("", AnchorProperty("fletcher.max_schema_bytes", "8"));
        FastDDSPubSubProvider provider(config);
        EXPECT_THROW(provider.CreateTopic({"schemabound", "toobig"}, MakeSchema()), PubSubError);
    }
    {
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
        pub.Publish({"schemabound", "fits"}, MakeEncoder(11));
        EXPECT_EQ(AwaitRow(received), 11);
    }
}

// A throw invites a retry, so a failed schema announcement has to leave nothing behind for that
// retry to short-circuit on. Re-anchored from the retired `max_schema_bytes = 8` option onto the
// document property that replaced it — same subject, same failure, new configuration route.
TEST(FastDdsConfig, AFailedSchemaAnnouncementCanBeRetried) {
    ProviderConfig config;
    config.domain_id = kDomainSchemaBound;
    config.document = Document("", AnchorProperty("fletcher.max_schema_bytes", "8"));
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
