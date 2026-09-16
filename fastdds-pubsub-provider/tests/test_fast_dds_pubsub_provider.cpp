// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fastdds/dds/core/condition/StatusCondition.hpp>
#include <fastdds/dds/core/condition/WaitSet.hpp>
#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/domain/DomainParticipantListener.hpp>
#include <fastdds/dds/log/Log.hpp>
#include <fastdds/dds/publisher/DataWriter.hpp>
#include <fastdds/dds/publisher/Publisher.hpp>
#include <fastdds/dds/subscriber/Subscriber.hpp>
#include <fastdds/dds/topic/Topic.hpp>
#include <fastdds/dds/topic/TypeSupport.hpp>
#include <fletcher/core/write_buffer.hpp>
#include <fletcher/fastdds_pubsub_provider/fast_dds_pubsub_provider.hpp>
#include <fletcher/pubsub/delivery_channel.hpp>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "internal/data_reader_listener.hpp"
#include "internal/fletcher_sample_pub_sub_type.hpp"
#include "internal/profile_document.hpp"
#include "internal/qos_defaults.hpp"
#include "internal/sample_writer.hpp"
#include "internal/transport_data.hpp"

using namespace fletcher;
using namespace eprosima::fastdds::dds;

namespace {

// This file's one poll-free wait: a producer (a subscriber callback, a Log consumer) calls
// NotifyWaiters() right after changing whatever a WaitUntil() predicate reads; the waiter blocks
// on a condition_variable instead of sleeping and re-checking. notify_all() is issued under
// g_wait_mutex, so a notify that lands between a waiter's predicate check and it actually
// starting to wait is never lost (serialized through the same mutex) rather than merely
// eventually-consistent.
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

// The one waiting mechanism (spec §3.4). Deliberately spelled out rather than
// hidden: a test that wants the schema states its budget and reads a TYPED
// outcome, exactly as a C#/Rust caller would.
SharedSchema AwaitSchema(const SubscriptionResult& result, std::chrono::milliseconds budget) {
    SharedSchema schema;
    EXPECT_EQ(result.schema.Wait(budget, &schema), PubSubStatus::kOk)
        << "schema arrival: " << result.schema.Message();
    return schema;
}

// The data channel is VOLATILE now (qos_defaults.cpp, owner decision 2026-09-15): AwaitSchema
// proves the SCHEMA reader matched (a separate channel, still TRANSIENT_LOCAL) -- not the DATA
// reader. The data reader is enabled once the schema resolves, but matching it against the
// publisher's data writer is a further, asynchronous step, and a row published before it
// completes is dropped rather than replayed. Attach one of these to the PUBLISHER (its OnMatched
// fires once its data writer sees the subscriber's data reader) and wait before the first Publish
// that expects delivery.
class DataWriterMatchListener : public FastDDSStatusListener {
   public:
    bool AwaitMatch(std::chrono::milliseconds budget = std::chrono::seconds(10)) {
        std::unique_lock<std::mutex> lk(m_);
        return cv_.wait_for(lk, budget, [&] { return positive_matches_ >= 1; });
    }
    int32_t PositiveMatches() {
        std::lock_guard<std::mutex> lk(m_);
        return positive_matches_;
    }
    // For a REMATCH (e.g. after Unsubscribe/Subscribe on a fresh reader), told apart from the
    // still-live earlier match by requiring the count to move past `since`.
    bool AwaitMatchAfter(int32_t since,
                         std::chrono::milliseconds budget = std::chrono::seconds(10)) {
        std::unique_lock<std::mutex> lk(m_);
        return cv_.wait_for(lk, budget, [&] { return positive_matches_ > since; });
    }

   private:
    void OnMatched(Endpoint endpoint, int32_t /*current_count*/, int32_t change) noexcept override {
        if (change <= 0 || !endpoint.is_writer || endpoint.is_schema_channel) return;
        std::lock_guard<std::mutex> lk(m_);
        ++positive_matches_;
        cv_.notify_all();
    }
    std::mutex m_;
    std::condition_variable cv_;
    int32_t positive_matches_ = 0;
};

// A dispatch site is a DeliveryChannel now, not a bare std::function, so these
// unit tests build one over a lambda. The token is this file's own static: a
// delivery frame is pushed and popped around each invocation exactly as a
// provider's would be, and nothing in these tests asks about it.
inline fletcher::DeliveryChannel TestChannel(fletcher::PubSubProvider::SubscribeCallback cb) {
    static const int kTestProviderToken = 0;
    return fletcher::DeliveryChannel(fletcher::DeliveryChannel::RawToken{}, &kTestProviderToken,
                                     std::move(cb));
}

}  // namespace

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static OwnedSchema MakeSchema() {
    OwnedSchema s;
    ArrowSchemaInit(s.get());
    ArrowSchemaSetTypeStruct(s.get(), 1);
    ArrowSchemaSetName(s->children[0], "x");
    ArrowSchemaSetType(s->children[0], NANOARROW_TYPE_INT32);
    return s;
}

// A schema that differs from MakeSchema() (different field name + type) so a
// re-declaration with it is a genuine conflict.
static OwnedSchema MakeOtherSchema() {
    OwnedSchema s;
    ArrowSchemaInit(s.get());
    ArrowSchemaSetTypeStruct(s.get(), 1);
    ArrowSchemaSetName(s->children[0], "y");
    ArrowSchemaSetType(s->children[0], NANOARROW_TYPE_DOUBLE);
    return s;
}

static PubSubProvider::RowEncoder MakeEncoder(int32_t x) {
    return [x](WriteBuffer& buf) {
        buf.AppendByte(0x00);
        buf.AppendFixed<int32_t>(x);
    };
}

static int32_t DecodeRow(const uint8_t* data) {
    int32_t v;
    std::memcpy(&v, data + 1, sizeof(v));
    return v;
}

// ---------------------------------------------------------------------------
// #60 — serialize() must surface a swallowed encoder exception as a diagnostic
// the caller can act on, while still returning false and NOT propagating out of
// the DDS callback (H-INV-3). Exercised directly on the internal sample type via
// a throwing encoder — no DDS participant required.
//
// Re-anchored from FletcherTopicType (retired with the data-sharing rewrite) onto
// FletcherSamplePubSubType. The diagnostic now rides on the per-publish
// PublishData rather than a sink on the shared type instance — see below.
// ---------------------------------------------------------------------------
TEST(FletcherSamplePubSubTypeTest, SerializeCapturesEncoderExceptionDiagnostic) {
    fletcher::internal::FletcherSamplePubSubType type(128);

    Attachments attachments;
    const PubSubProvider::RowEncoder bad = [](WriteBuffer&) {
        throw std::runtime_error("encoder boom");
    };
    fletcher::internal::PublishData data;
    data.attachments = &attachments;
    data.encoder = &bad;

    // Use the reserving constructor so the SerializedPayload_t owns its
    // (calloc-backed) buffer and frees it correctly on destruction. Aliasing a
    // std::vector into payload.data would make ~SerializedPayload_t free()
    // memory it does not own.
    eprosima::fastdds::rtps::SerializedPayload_t payload(128);
    payload.length = 123;

    // No exception may escape serialize (H-INV-3): the encoder throw is caught.
    bool result = true;
    EXPECT_NO_THROW({
        result = type.serialize(&data, payload, eprosima::fastdds::dds::DataRepresentationId_t{});
    });
    EXPECT_FALSE(result);
    EXPECT_EQ(payload.length, 0u);
    EXPECT_THAT(data.serialize_error, testing::HasSubstr("encoder boom"));
}

// #60 hardening, restated for the per-publish diagnostic. One
// FletcherSamplePubSubType instance is shared by every data topic and every
// thread, so a diagnostic stored ON THE TYPE would need clearing before each
// write() or a stale error from an earlier failed publish on another topic would
// be misattributed to this one — and under the provider's SHARED publish lock,
// concurrent publishes would race on it outright.
//
// Carrying it on the per-call PublishData removes the failure mode rather than
// guarding it: each publish reads only its own. This replaces HARD's
// ClearSerializeErrorResetsStaleDiagnostic, whose premise (a shared sink needing
// a pre-write reset) no longer exists.
TEST(FletcherSamplePubSubTypeTest, SerializeDiagnosticDoesNotLeakBetweenPublishes) {
    fletcher::internal::FletcherSamplePubSubType type(128);  // shared by every topic
    Attachments attachments;

    // A prior failed publish records its reason.
    const PubSubProvider::RowEncoder bad = [](WriteBuffer&) {
        throw std::runtime_error("stale boom");
    };
    fletcher::internal::PublishData failed;
    failed.attachments = &attachments;
    failed.encoder = &bad;
    eprosima::fastdds::rtps::SerializedPayload_t p1(128);
    type.serialize(&failed, p1, eprosima::fastdds::dds::DataRepresentationId_t{});
    ASSERT_THAT(failed.serialize_error, testing::HasSubstr("stale boom"));

    // The next publish through the SAME type instance carries its own, and a
    // success leaves it empty — so Publish cannot misattribute the earlier failure.
    const PubSubProvider::RowEncoder good = MakeEncoder(7);
    fletcher::internal::PublishData ok;
    ok.attachments = &attachments;
    ok.encoder = &good;
    eprosima::fastdds::rtps::SerializedPayload_t p2(128);
    EXPECT_TRUE(type.serialize(&ok, p2, eprosima::fastdds::dds::DataRepresentationId_t{}));
    EXPECT_TRUE(ok.serialize_error.empty());
}

// ---------------------------------------------------------------------------
// Tests — basic provider behaviour
// ---------------------------------------------------------------------------

TEST(FastDDSPubSubProviderTest, ConstructDestruct) {
    EXPECT_NO_THROW({ FastDDSPubSubProvider p(ProviderConfig{}); });
}

// #63 (HARD-4) — Destruction is a documented quiescence contract, not a
// synchronization boundary: callers must ensure no public API call is in flight
// and no re-entrant provider callback is pending when the provider is destroyed
// (see ~FastDDSPubSubProvider). This exercises the SUPPORTED teardown — quiescent
// use, then destruction with no concurrent activity — and documents that
// contract. It is intentionally NOT a race test: a destruction-during-use race
// would be flaky and would not make that usage supported (HARD-4 design).
TEST(FastDDSPubSubProviderTest, DestructAfterQuiescentUseDocumentsContract) {
    EXPECT_NO_THROW({
        FastDDSPubSubProvider provider(ProviderConfig{});
        provider.CreateTopic({"quiescent", "teardown"}, MakeSchema());

        std::atomic<int32_t> received{-1};
        static_cast<void>(
            provider.Subscribe({"quiescent", "teardown"},
                               [&](const uint8_t* data, size_t len, SharedSchema, Attachments) {
                                   if (len >= 5) received.store(DecodeRow(data));
                               }));
        provider.Publish({"quiescent", "teardown"}, MakeEncoder(1));

        // Reach a quiescent point before teardown: Unsubscribe deletes the
        // readers outside the provider lock, draining any in-flight listener
        // callback, so no callback can be running when `provider` is destroyed
        // at end of scope with no concurrent API calls.
        provider.Unsubscribe({"quiescent", "teardown"});
    });
}

TEST(FastDDSPubSubProviderTest, CreateTopicSucceeds) {
    FastDDSPubSubProvider p(ProviderConfig{});
    EXPECT_NO_THROW(p.CreateTopic({"create", "ok"}, MakeSchema()));
}

TEST(FastDDSPubSubProviderTest, CreateTopicIsIdempotent) {
    // CreateTopic mirrors the in-process reference provider: declaring an
    // already-existing topic is a no-op, not an error. This lets a publisher
    // attach to a topic a subscriber created first (subscriber-first) and
    // makes repeated declarations harmless.
    FastDDSPubSubProvider p(ProviderConfig{});
    p.CreateTopic({"create", "dup"}, MakeSchema());
    EXPECT_NO_THROW(p.CreateTopic({"create", "dup"}, MakeSchema()));
}

TEST(FastDDSPubSubProviderTest, CreateTopicRejectsConflictingSchema) {
    // Idempotent re-declaration is fine, but declaring an existing topic with a
    // *different* schema is a genuine conflict and must not be silently dropped.
    FastDDSPubSubProvider p(ProviderConfig{});
    p.CreateTopic({"create", "conflict"}, MakeSchema());
    EXPECT_THROW(p.CreateTopic({"create", "conflict"}, MakeOtherSchema()), std::runtime_error);
}

TEST(FastDDSPubSubProviderTest, PublishWithoutSubscriberDoesNotThrow) {
    FastDDSPubSubProvider p(ProviderConfig{});
    p.CreateTopic({"pub", "nosub"}, MakeSchema());
    EXPECT_NO_THROW(p.Publish({"pub", "nosub"}, MakeEncoder(1)));
}

// #60 (production half): a failing row encoder makes serialize() fail (captured in
// LastSerializeError); Publish must SURFACE it as a throw carrying the diagnostic,
// not drop the row silently. Paired with PublishWithoutSubscriberDoesNotThrow above,
// which proves a benign no-reader write()==false (no serialize error) does NOT throw
// — so the signal is the serialize diagnostic, not the write() return.
TEST(FastDDSPubSubProviderTest, PublishThrowsWhenEncoderFails) {
    FastDDSPubSubProvider p(ProviderConfig{});
    p.CreateTopic({"pub", "encfail"}, MakeSchema());
    PubSubProvider::RowEncoder bad = [](WriteBuffer&) { throw std::runtime_error("encoder boom"); };
    try {
        p.Publish({"pub", "encfail"}, bad);
        FAIL() << "Publish must throw when the row encoder fails to serialize";
    } catch (const std::runtime_error& e) {
        EXPECT_THAT(e.what(), testing::HasSubstr("failed to publish"));
        EXPECT_THAT(e.what(), testing::HasSubstr("encoder boom"));
    }
}

TEST(FastDDSPubSubProviderTest, RoundTripPublishSubscribe) {
    DataWriterMatchListener pub_listener;
    FastDDSPubSubProvider pub_provider(ProviderConfig{}, &pub_listener);
    FastDDSPubSubProvider sub_provider(ProviderConfig{});

    pub_provider.CreateTopic({"roundtrip", "x"}, MakeSchema());

    std::atomic<int32_t> received{-1};
    SubscriptionResult result = sub_provider.Subscribe(
        {"roundtrip", "x"},
        [&](const uint8_t* data, size_t len, const SharedSchema&, const Attachments&) {
            if (len >= 5) received.store(DecodeRow(data));
            NotifyWaiters();
        });

    SharedSchema sch = AwaitSchema(result, std::chrono::seconds(5));
    ASSERT_TRUE(sch);
    ASSERT_EQ(sch->n_children, 1);
    EXPECT_EQ(std::string(sch->children[0]->name), "x");
    EXPECT_EQ(std::string(sch->children[0]->format), "i");

    ASSERT_TRUE(pub_listener.AwaitMatch())
        << "the data writer never matched the subscriber's data reader";
    pub_provider.Publish({"roundtrip", "x"}, MakeEncoder(42));

    WaitUntil([&] { return received.load() != -1; }, std::chrono::seconds(5));

    EXPECT_EQ(received.load(), 42);
}

// ---------------------------------------------------------------------------
// Tests — loans / data-sharing
// ---------------------------------------------------------------------------

// The sample type is bounded and plain, so every endpoint reserves the whole payload bound per
// history slot — which is what the resource limits here keep in check. The default memory policy
// preallocates, so a reader built from these reads through loans; the publish side never loans any
// more (owner decision 2026-09-15) -- see ALoanableSampleWriterWritesThroughARealWriter below for
// LoanableSampleWriter's own direct coverage.
//
// PDA-DEC-6 — these used to be typed `FastDDSProviderOptions` fields; they are now lines in the
// provider's own Fast DDS XML profiles document. Note the durability / reliability /
// data-sharing lines are restated in full: **a supplied profile is that endpoint's WHOLE QoS**
// (owner ruling 2026-09-02), so Fletcher's built-in profile is NOT underneath these, and a
// profile that mentioned only `historyQos` would silently take Fast DDS's defaults — a
// BEST_EFFORT, data-sharing-AUTO reader, which is not what these tests are about.
namespace {

// VOLATILE mirrors Fletcher's built-in data profile (owner decision 2026-09-15,
// qos_defaults.cpp): none of the tests below need replay, so there is no reason to diverge.
constexpr const char* kFletcherWriterQos = R"(
        <durability><kind>VOLATILE</kind></durability>
        <reliability><kind>RELIABLE</kind></reliability>)";

constexpr const char* kFletcherReaderQos = R"(
        <durability><kind>VOLATILE</kind></durability>
        <reliability><kind>RELIABLE</kind></reliability>
        <data_sharing><kind>OFF</kind></data_sharing>)";

// Ten slots rather than Fletcher's published hundred: small enough that the loaned tests below
// can exhaust the pool deliberately.
constexpr const char* kTenSlots = R"(
        <historyQos><kind>KEEP_LAST</kind><depth>10</depth></historyQos>
        <resourceLimitsQos>
          <max_samples>10</max_samples>
          <max_instances>1</max_instances>
          <max_samples_per_instance>10</max_samples_per_instance>
          <allocated_samples>10</allocated_samples>
        </resourceLimitsQos>)";

struct DocumentParts {
    std::string writer_qos = kFletcherWriterQos;
    std::string reader_qos = kFletcherReaderQos;
    std::string writer_topic = kTenSlots;
    // Children of <data_reader> that are not <qos>/<topic> — <historyMemoryPolicy>, say.
    std::string reader_tail;
    // Goes inside the mandatory <participant profile_name="fletcher_participant"> anchor.
    std::string anchor_body;
};

// Both endpoint profiles carry `is_default_profile="true"`: `fletcher_writer` / `fletcher_reader`
// are not special names any more (design rule 2, internal/profile_document.hpp) -- none of the
// topics these tests use are literally named that, so without the marking the document below
// would configure nothing and every test through it would silently run on Fast DDS's own default.
std::string BoundedDocument(const DocumentParts& parts = {}) {
    return std::string(R"(<?xml version="1.0" encoding="UTF-8"?>
<dds xmlns="http://www.eprosima.com/XMLSchemas/fastRTPS_Profiles">
  <profiles>
    <participant profile_name="fletcher_participant">)") +
           parts.anchor_body + R"(</participant>
    <data_writer profile_name="fletcher_writer" is_default_profile="true">
      <qos>)" +
           parts.writer_qos +
           R"(
      </qos>
      <topic>)" +
           parts.writer_topic +
           R"(
      </topic>
    </data_writer>
    <data_reader profile_name="fletcher_reader" is_default_profile="true">
      <qos>)" +
           parts.reader_qos +
           R"(
      </qos>
      <topic>)" +
           kTenSlots +
           R"(
      </topic>)" +
           parts.reader_tail +
           R"(
    </data_reader>
  </profiles>
</dds>)";
}

}  // namespace

static ProviderConfig BoundedConfig(const DocumentParts& parts = {}) {
    ProviderConfig config;
    config.document = BoundedDocument(parts);
    return config;
}

// The bound is part of the typed core (spec §4.1) and is taken as given — PayloadBytes() is what
// a row has to fit and what the registered type name carries.
TEST(FastDDSPubSubProviderTest, ThePayloadBoundIsWhatWasAskedFor) {
    ProviderConfig config = BoundedConfig();
    config.max_payload_bytes = kPayloadBytes<128 * 1024>;
    FastDDSPubSubProvider provider(config);
    EXPECT_EQ(provider.PayloadBytes(), 128u * 1024);
}

// Nothing is rounded, and 4-byte alignment is the whole rule; these values cannot work.
//
// Re-anchored by PDA-DEC-6: the refusal is the SAME refusal, but it now crosses as the seam's one
// error type carrying a stable number (`kInvalidArgument`) rather than as a bare
// `std::invalid_argument`, which through a registry factory would reach the caller as `kInternal`
// and tell an operator nothing (spec §5.1). The `max_payload_bytes = 0` row is GONE from this
// test on purpose: 0 now means *unset* and resolves to 65536, which
// `FastDdsConfig.AnUnsetPayloadBoundResolvesToSixtyFourKiB` pins.
TEST(FastDDSPubSubProviderTest, AnUnusablePayloadBoundIsRefused) {
    ProviderConfig config = BoundedConfig();
    config.max_payload_bytes = 100'001;  // not a multiple of 4, so the sample carries tail padding
    EXPECT_THROW(FastDDSPubSubProvider provider(config), PubSubError);

    // Past where a sample's own size still fits the uint32 Fast DDS reports it in.
    config.max_payload_bytes = kMaxPayloadBytes + 4;
    EXPECT_THROW(FastDDSPubSubProvider provider(config), PubSubError);
}

// A bound that is not a power of two is ordinary.
TEST(FastDDSPubSubProviderTest, ABoundThatIsNotAPowerOfTwoIsFine) {
    ProviderConfig config = BoundedConfig();
    config.max_payload_bytes = kPayloadBytes<100'000>;
    FastDDSPubSubProvider provider(config);
    EXPECT_EQ(provider.PayloadBytes(), 100'000u);
}

// Nothing caps a bound; a constructor allocates no pools, which are per endpoint.
TEST(FastDDSPubSubProviderTest, ALargeBoundIsFine) {
    ProviderConfig config = BoundedConfig();
    config.max_payload_bytes = kPayloadBytes<8 * 1024 * 1024>;
    EXPECT_NO_THROW(FastDDSPubSubProvider provider(config));

    // Well past where the old compiled set stopped.
    ProviderConfig large = BoundedConfig();
    large.max_payload_bytes = kPayloadBytes<256 * 1024 * 1024>;
    EXPECT_NO_THROW(FastDDSPubSubProvider provider(large));
}

// And with Fast DDS's own default max_samples behind it, which the old check rejected outright —
// now a line in the document rather than a field. Own TEST/process: this document's
// "fletcher_writer" profile (max_samples 5000) differs from BoundedConfig()'s default one above
// (max_samples 10, kTenSlots), and Fast DDS profile names are process-wide.
TEST(FastDDSPubSubProviderTest, ALargeBoundWithFastDdsDefaultMaxSamplesIsFine) {
    DocumentParts unbounded;
    unbounded.writer_topic = R"(
        <historyQos><kind>KEEP_LAST</kind><depth>10</depth></historyQos>
        <resourceLimitsQos><max_samples>5000</max_samples></resourceLimitsQos>)";
    ProviderConfig unbounded_limits = BoundedConfig(unbounded);
    unbounded_limits.max_payload_bytes = kPayloadBytes<8 * 1024 * 1024>;
    EXPECT_NO_THROW(FastDDSPubSubProvider provider(unbounded_limits));
}

// Every provider must spell this identically, and it is the only thing keeping bounds apart.
TEST(FastDDSPubSubProviderTest, TheTypeNameCarriesTheBoundForEveryProvider) {
    EXPECT_EQ("fletcher_65536", FletcherTypeName(64 * 1024));
    EXPECT_EQ("fletcher_100000", FletcherTypeName(100'000));
    EXPECT_EQ("fletcher_4", FletcherTypeName(kMinPayloadBytes));
    EXPECT_NE(FletcherTypeName(64 * 1024), FletcherTypeName(8 * 1024 * 1024));
}

static int32_t AwaitRow(const std::atomic<int32_t>& received) {
    WaitUntil([&] { return received.load() != -1; }, std::chrono::seconds(5));
    return received.load();
}

// A non-preallocating reader reads through copies -- true of every reader since item C
// (CopyingDataReaderListener is the default flow), but this one could not have taken the loaned
// path anyway: CanLoanSamplesFollowsTheMemoryPolicy above is the predicate
// LoanedDataReaderListener's own direct test relies on, and DYNAMIC fails it. The document line
// that sets the memory policy is asserted directly rather than inferred from "a row arrived": a
// `<historyMemoryPolicy>` value Fast DDS did not understand would leave the reader PREALLOCATED and
// this test would pass while silently no longer exercising DYNAMIC at all.
TEST(FastDDSPubSubProviderTest, DynamicMemoryReaderRoundTripsThroughCopies) {
    DocumentParts dynamic_reader;
    dynamic_reader.reader_tail = R"(
      <historyMemoryPolicy>DYNAMIC</historyMemoryPolicy>)";
    const ProviderConfig sub_config = BoundedConfig(dynamic_reader);
    {
        // The load happens BEFORE the Subscriber below is created: its default reader QoS is
        // seeded from the registry's is_default_profile reader profile at construction time, and
        // "any/topic" below matches no profile by name, so it resolves to that seeded default.
        {
            std::lock_guard<std::mutex> lock(internal::profile_registry_mutex);
            internal::LoadDocumentOnce(sub_config.document);
        }
        DomainParticipant* probe = DomainParticipantFactory::get_instance()->create_participant(
            0, PARTICIPANT_QOS_DEFAULT);
        ASSERT_NE(probe, nullptr);
        Subscriber* subscriber = probe->create_subscriber(SUBSCRIBER_QOS_DEFAULT);
        ASSERT_NE(subscriber, nullptr);
        const DataReaderQos resolved = internal::ResolveDataReaderQos(*subscriber, "any/topic");
        EXPECT_EQ(resolved.endpoint().history_memory_policy,
                  eprosima::fastdds::rtps::DYNAMIC_RESERVE_MEMORY_MODE)
            << "the document's <historyMemoryPolicy> did not reach the reader QoS";
        EXPECT_FALSE(internal::CanLoanSamples(resolved));
        probe->delete_subscriber(subscriber);
        DomainParticipantFactory::get_instance()->delete_participant(probe);
    }

    // Both providers share this document (Fast DDS profile names are process-wide): dynamic_reader
    // only touches the reader profile, so pub_provider's writer role resolves identically to
    // BoundedConfig()'s default.
    DataWriterMatchListener pub_listener;
    FastDDSPubSubProvider pub_provider(BoundedConfig(dynamic_reader), &pub_listener);
    FastDDSPubSubProvider sub_provider(sub_config);

    pub_provider.CreateTopic({"dynamic", "reader"}, MakeSchema());

    std::atomic<int32_t> received{-1};
    SubscriptionResult result = sub_provider.Subscribe(
        {"dynamic", "reader"},
        [&](const uint8_t* data, size_t len, const SharedSchema&, const Attachments&) {
            if (len >= 5) received.store(DecodeRow(data));
            NotifyWaiters();
        });
    ASSERT_TRUE(AwaitSchema(result, std::chrono::seconds(5)));

    ASSERT_TRUE(pub_listener.AwaitMatch())
        << "the data writer never matched the subscriber's data reader";
    pub_provider.Publish({"dynamic", "reader"}, MakeEncoder(57));

    EXPECT_EQ(AwaitRow(received), 57);
}

// And the predicate itself, since it is what decides which flow a reader gets.
TEST(FastDDSPubSubProviderTest, CanLoanSamplesFollowsTheMemoryPolicy) {
    DataReaderQos qos;
    EXPECT_TRUE(internal::CanLoanSamples(qos)) << "the default policy preallocates";

    qos.endpoint().history_memory_policy = eprosima::fastdds::rtps::PREALLOCATED_MEMORY_MODE;
    EXPECT_TRUE(internal::CanLoanSamples(qos));

    qos.endpoint().history_memory_policy = eprosima::fastdds::rtps::DYNAMIC_RESERVE_MEMORY_MODE;
    EXPECT_FALSE(internal::CanLoanSamples(qos));

    qos.endpoint().history_memory_policy = eprosima::fastdds::rtps::DYNAMIC_REUSABLE_MEMORY_MODE;
    EXPECT_FALSE(internal::CanLoanSamples(qos));
}

// The publish side not loaning: Fast DDS may still use shared memory, but the writer serialises
// into the payload rather than handing its buffer to the encoder. The reader loans either way, so
// this is also the copying-publisher-to-loaning-subscriber pairing — a serialised payload is only
// as long as the row needs, while the loan spans a whole slot, and it is the sample's own length
// rather than the payload length that bounds the read.
TEST(FastDDSPubSubProviderTest, DataSharingRoundTrip) {
    DataWriterMatchListener pub_listener;
    FastDDSPubSubProvider pub_provider(BoundedConfig(), &pub_listener);
    FastDDSPubSubProvider sub_provider(BoundedConfig());

    pub_provider.CreateTopic({"datasharing", "x"}, MakeSchema());

    std::atomic<int32_t> received{-1};
    SubscriptionResult result = sub_provider.Subscribe(
        {"datasharing", "x"},
        [&](const uint8_t* data, size_t len, const SharedSchema&, const Attachments&) {
            if (len >= 5) received.store(DecodeRow(data));
            NotifyWaiters();
        });
    ASSERT_TRUE(AwaitSchema(result, std::chrono::seconds(5)));

    ASSERT_TRUE(pub_listener.AwaitMatch())
        << "the data writer never matched the subscriber's data reader";
    pub_provider.Publish({"datasharing", "x"}, MakeEncoder(31));

    EXPECT_EQ(AwaitRow(received), 31);
}

// Same DataSharingKind::ON check as for zero-copy: it passes only while the type
// declares itself bounded, which is all data-sharing needs. Being plain (loans)
// is a separate claim this mode does not make.
TEST(FastDDSPubSubProviderTest, DataSharingTypeIsAcceptedForDataSharing) {
    DocumentParts sharing;
    sharing.writer_qos = std::string(kFletcherWriterQos) + R"(
        <data_sharing><kind>ON</kind></data_sharing>)";
    // VOLATILE, matching the writer above: a TRANSIENT_LOCAL request against a VOLATILE offer is
    // an incompatible-QoS mismatch, and this test is not about that.
    sharing.reader_qos = R"(
        <durability><kind>VOLATILE</kind></durability>
        <reliability><kind>RELIABLE</kind></reliability>
        <data_sharing><kind>ON</kind></data_sharing>)";

    DataWriterMatchListener pub_listener;
    FastDDSPubSubProvider pub_provider(BoundedConfig(sharing), &pub_listener);
    FastDDSPubSubProvider sub_provider(BoundedConfig(sharing));

    pub_provider.CreateTopic({"datasharing", "on"}, MakeSchema());

    std::atomic<int32_t> received{-1};
    SubscriptionResult result = sub_provider.Subscribe(
        {"datasharing", "on"},
        [&](const uint8_t* data, size_t len, const SharedSchema&, const Attachments&) {
            if (len >= 5) received.store(DecodeRow(data));
            NotifyWaiters();
        });
    ASSERT_TRUE(AwaitSchema(result, std::chrono::seconds(5)));

    ASSERT_TRUE(pub_listener.AwaitMatch())
        << "the data writer never matched the subscriber's data reader";
    pub_provider.Publish({"datasharing", "on"}, MakeEncoder(29));

    EXPECT_EQ(AwaitRow(received), 29);
}

// The mirror of the zero-copy oversize test, and what separates the two modes:
// with no loan to encode into, the overflow happens inside serialize(), which
// reports it to Fast DDS instead of throwing out of Publish. The sample is
// dropped either way.
TEST(FastDDSPubSubProviderTest, DataSharingOversizedRowDoesNotThrow) {
    FastDDSPubSubProvider pub_provider(BoundedConfig());
    pub_provider.CreateTopic({"datasharing", "oversized"}, MakeSchema());

    auto oversized = [bound = pub_provider.PayloadBytes()](WriteBuffer& buf) {
        std::vector<uint8_t> blob(bound + 16, 0x5A);
        buf.Append(blob.data(), blob.size());
    };
    EXPECT_NO_THROW(pub_provider.Publish({"datasharing", "oversized"}, oversized));
    EXPECT_NO_THROW(pub_provider.Publish({"datasharing", "oversized"}, MakeEncoder(3)));
}

// DataSharingKind::ON makes Fast DDS reject an endpoint whose type is not
// bounded, instead of quietly falling back to the transport — so this only
// passes while the type really declares itself bounded.
TEST(FastDDSPubSubProviderTest, BoundedTypeIsAcceptedForForcedDataSharing) {
    DocumentParts sharing;
    sharing.writer_qos = std::string(kFletcherWriterQos) + R"(
        <data_sharing><kind>ON</kind></data_sharing>)";
    // VOLATILE, matching the writer above: a TRANSIENT_LOCAL request against a VOLATILE offer is
    // an incompatible-QoS mismatch, and this test is not about that.
    sharing.reader_qos = R"(
        <durability><kind>VOLATILE</kind></durability>
        <reliability><kind>RELIABLE</kind></reliability>
        <data_sharing><kind>ON</kind></data_sharing>)";

    DataWriterMatchListener pub_listener;
    FastDDSPubSubProvider pub_provider(BoundedConfig(sharing), &pub_listener);
    FastDDSPubSubProvider sub_provider(BoundedConfig(sharing));

    pub_provider.CreateTopic({"bounded", "datasharing"}, MakeSchema());

    std::atomic<int32_t> received{-1};
    SubscriptionResult result = sub_provider.Subscribe(
        {"bounded", "datasharing"},
        [&](const uint8_t* data, size_t len, const SharedSchema&, const Attachments&) {
            if (len >= 5) received.store(DecodeRow(data));
            NotifyWaiters();
        });
    ASSERT_TRUE(AwaitSchema(result, std::chrono::seconds(5)));

    ASSERT_TRUE(pub_listener.AwaitMatch())
        << "the data writer never matched the subscriber's data reader";
    pub_provider.Publish({"bounded", "datasharing"}, MakeEncoder(23));

    EXPECT_EQ(AwaitRow(received), 23);
}

// T6: a row large enough that Fast DDS must FRAGMENT it at the RTPS level still crosses intact.
// `bound` is the payload bound both endpoints resolve to here (65536, BoundedConfig()'s default —
// see FastDdsConfig.AnUnsetPayloadBoundResolvesToSixtyFourKiB in test_profile_document.cpp).
// `- 8` is EncodeEnvelopeBody's own zero-attachment body framing — a 4-byte ROW_LEN placeholder
// plus a 4-byte ATTACH_COUNT of zero (envelope_codec.hpp; see
// FletcherSamplePubSubTypeTest.ARowOfExactlyTheAvailableCapacitySerializesAndRoundTrips in
// test_fletcher_sample_pub_sub_type.cpp) — which keeps the row inside FletcherSamplePubSubType's
// own serialize() capacity. The total wire length this produces — row size + 16, that same 8 plus
// the 4-byte CDR encapsulation header and the 4-byte sample length prefix — then lands at exactly
// `bound - 8` = 65528 bytes: past `eprosima::fastdds::rtps::s_maximumMessageSize` (65500,
// TransportInterface.hpp), the default UDPv4Transport ceiling for one RTPS message, so this
// sample cannot go out as a single DATA submessage and Fast DDS must fragment it (DATA_FRAG).
// Synchronous writers fragment fine on 3.4.0 — the upstream "async required" note is stale here.
TEST(FastDDSPubSubProviderTest, AFragmentingRowCrossesIntact) {
    DataWriterMatchListener pub_listener;
    FastDDSPubSubProvider pub_provider(BoundedConfig(), &pub_listener);
    FastDDSPubSubProvider sub_provider(BoundedConfig());
    pub_provider.CreateTopic({"fragmenting", "x"}, MakeSchema());

    const size_t row_size = static_cast<size_t>(pub_provider.PayloadBytes()) - 8 - 16;
    std::vector<uint8_t> expected(row_size);
    for (size_t i = 0; i < row_size; ++i) expected[i] = static_cast<uint8_t>(i);

    std::mutex mtx;
    std::vector<uint8_t> received_row;
    bool received = false;
    SubscriptionResult result = sub_provider.Subscribe(
        {"fragmenting", "x"},
        [&](const uint8_t* data, size_t len, const SharedSchema&, const Attachments&) {
            {
                std::lock_guard<std::mutex> lock(mtx);
                received_row.assign(data, data + len);
                received = true;
            }
            NotifyWaiters();
        });
    ASSERT_TRUE(AwaitSchema(result, std::chrono::seconds(5)));

    ASSERT_TRUE(pub_listener.AwaitMatch())
        << "the data writer never matched the subscriber's data reader";
    pub_provider.Publish({"fragmenting", "x"},
                         [&](WriteBuffer& buf) { buf.Append(expected.data(), expected.size()); });

    WaitUntil(
        [&] {
            std::lock_guard<std::mutex> lock(mtx);
            return received;
        },
        std::chrono::seconds(10));

    std::lock_guard<std::mutex> lock(mtx);
    ASSERT_TRUE(received) << "the fragmenting row never arrived";
    EXPECT_EQ(received_row, expected);
}

// Same guarantee with a publisher that does not loan.
TEST(FastDDSPubSubProviderTest, CopyingThrowingCallbackDoesNotEscape) {
    DataWriterMatchListener pub_listener;
    FastDDSPubSubProvider pub_provider(BoundedConfig(), &pub_listener);
    FastDDSPubSubProvider sub_provider(BoundedConfig());

    pub_provider.CreateTopic({"datasharing", "throwing"}, MakeSchema());

    std::atomic<int> deliveries{0};
    std::atomic<int32_t> received{-1};
    SubscriptionResult result = sub_provider.Subscribe(
        {"datasharing", "throwing"},
        [&](const uint8_t* data, size_t len, const SharedSchema&, const Attachments&) {
            if (deliveries.fetch_add(1) < 5) throw std::runtime_error("callback failure");
            if (len >= 5) received.store(DecodeRow(data));
            NotifyWaiters();
        });
    ASSERT_TRUE(AwaitSchema(result, std::chrono::seconds(5)));

    ASSERT_TRUE(pub_listener.AwaitMatch())
        << "the data writer never matched the subscriber's data reader";
    for (int32_t i = 0; i < 20; ++i) {
        pub_provider.Publish({"datasharing", "throwing"}, MakeEncoder(i));
    }

    EXPECT_NE(AwaitRow(received), -1) << "delivery stopped after the throwing callbacks";
    EXPECT_GT(deliveries.load(), 5);
}

// LoanableSampleWriter stays in the tree, unit-tested, even though `Publish` always writes
// through the regular `WriteSample` now (owner decision 2026-09-15). Driven directly against a
// real, hand-built DataWriter on the SAME topic a normal provider subscribes to: the simplest way
// left to prove it still loans, fills the sample -- attachments included -- and that a row too
// large for the loan throws without leaking it back, repeated past the writer's own resource
// limits (Fletcher's baked-in 100) so a leak cannot hide behind spare slots.
TEST(FastDDSPubSubProviderTest, ALoanableSampleWriterWritesThroughARealWriter) {
    FastDDSPubSubProvider pub_provider(ProviderConfig{});
    pub_provider.CreateTopic({"loanablewriter", "direct"}, MakeSchema());
    FastDDSPubSubProvider sub_provider(ProviderConfig{});

    std::atomic<int32_t> received{-1};
    std::vector<uint8_t> blob_seen;
    SubscriptionResult result = sub_provider.Subscribe(
        {"loanablewriter", "direct"},
        [&](const uint8_t* data, size_t len, const SharedSchema&, const Attachments& att) {
            const Blob* sidecar = att.Find("sidecar");
            if (sidecar != nullptr) {
                blob_seen.assign(sidecar->data(), sidecar->data() + sidecar->size());
            }
            if (len >= 5) received.store(DecodeRow(data));
            NotifyWaiters();
        });
    ASSERT_TRUE(AwaitSchema(result, std::chrono::seconds(5)));

    // A hand-built writer on the same topic, NOT the provider's own -- Publish never loans any
    // more, so this is the only way left to drive LoanableSampleWriter against a real DataWriter.
    // Same process as `pub_provider`/`sub_provider`, but `participant` below is a brand new
    // participant the subscriber's participant has not discovered yet: PDP still runs over the
    // transport even in one process, so the match with the already enabled data reader above is
    // NOT guaranteed to complete inside create_datawriter -- the write loop below covers the gap.
    DomainParticipant* participant =
        DomainParticipantFactory::get_instance()->create_participant(0, PARTICIPANT_QOS_DEFAULT);
    ASSERT_NE(participant, nullptr);
    TypeSupport data_type_support;
    data_type_support.reset(new internal::FletcherSamplePubSubType(pub_provider.PayloadBytes()));
    ASSERT_EQ(data_type_support.register_type(participant), RETCODE_OK);
    Publisher* publisher = participant->create_publisher(PUBLISHER_QOS_DEFAULT);
    ASSERT_NE(publisher, nullptr);
    Topic* topic = participant->create_topic("loanablewriter/direct",
                                             data_type_support.get_type_name(), TOPIC_QOS_DEFAULT);
    ASSERT_NE(topic, nullptr);
    // The registry already carries Fletcher's baked-in document (`pub_provider` above loaded it),
    // so this Publisher's own default writer QoS is already seeded from it -- the same profile
    // `pub_provider`'s own (unused here) writer would have gotten.
    DataWriter* writer =
        publisher->create_datawriter(topic, publisher->get_default_datawriter_qos());
    ASSERT_NE(writer, nullptr);

    internal::LoanableSampleWriter loanable(pub_provider.PayloadBytes());

    Attachments att;
    att.Set("sidecar", Blob{std::vector<uint8_t>{1, 2, 3}});

    // The hand-built writer lives on a participant the subscriber's participant has yet to
    // discover: PDP runs over the transport even in one process, so the first write can leave
    // before the match and a VOLATILE row sent then is gone. Republish the same row until it
    // lands, bounded — the sibling loaned-reader test does the same.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (received.load() == -1 && std::chrono::steady_clock::now() < deadline) {
        loanable.Write(writer, MakeEncoder(7), att);
        WaitUntil([&] { return received.load() != -1; }, std::chrono::milliseconds(200));
    }
    EXPECT_EQ(received.load(), 7);
    EXPECT_EQ(blob_seen, (std::vector<uint8_t>{1, 2, 3}));

    // A row past the bound throws (only the loaned path does -- the serialising path swallows the
    // overflow inside serialize()), and repeating it past the writer's pool proves the failed
    // attempts returned their loans: once a loan leaks, loan_sample itself starts failing.
    const auto oversized = [bound = pub_provider.PayloadBytes()](WriteBuffer& buf) {
        std::vector<uint8_t> blob(bound + 16, 0x5A);
        buf.Append(blob.data(), blob.size());
    };
    for (int i = 0; i < 105; ++i) {
        EXPECT_THROW(loanable.Write(writer, oversized, Attachments{}), std::overflow_error)
            << "attempt " << i;
    }
    EXPECT_NO_THROW(loanable.Write(writer, MakeEncoder(3), Attachments{}));

    publisher->delete_datawriter(writer);
    participant->delete_topic(topic);
    participant->delete_publisher(publisher);
    DomainParticipantFactory::get_instance()->delete_participant(participant);
}

// LoanedDataReaderListener stays in the tree, unit-tested, even though Subscribe does not install
// it any more (owner ruling 2026-09-14). This drives it directly against a real, hand-built reader
// on the SAME topic a normal provider publishes to: the simplest way left to prove it still takes
// a loan, decodes it, and delivers. Fletcher's baked-in reader profile preallocates by default
// (CanLoanSamplesFollowsTheMemoryPolicy above), so no extra document is needed to admit the loaned
// path here -- `pub_provider`'s own construction already loaded it into the registry.
TEST(FastDDSPubSubProviderTest, ALoanedDataReaderListenerDrainsARealReaderDirectly) {
    DataWriterMatchListener pub_listener;
    FastDDSPubSubProvider pub_provider(ProviderConfig{}, &pub_listener);
    pub_provider.CreateTopic({"loaned", "direct"}, MakeSchema());

    DomainParticipant* participant =
        DomainParticipantFactory::get_instance()->create_participant(0, PARTICIPANT_QOS_DEFAULT);
    ASSERT_NE(participant, nullptr);
    TypeSupport data_type_support;
    data_type_support.reset(new internal::FletcherSamplePubSubType(pub_provider.PayloadBytes()));
    ASSERT_EQ(data_type_support.register_type(participant), RETCODE_OK);
    Subscriber* subscriber = participant->create_subscriber(SUBSCRIBER_QOS_DEFAULT);
    ASSERT_NE(subscriber, nullptr);
    Topic* topic = participant->create_topic("loaned/direct", data_type_support.get_type_name(),
                                             TOPIC_QOS_DEFAULT);
    ASSERT_NE(topic, nullptr);

    const DataReaderQos rqos = internal::ResolveDataReaderQos(*subscriber, "loaned/direct");
    ASSERT_TRUE(internal::CanLoanSamples(rqos));
    DataReader* reader = subscriber->create_datareader(topic, rqos);
    ASSERT_NE(reader, nullptr);

    std::atomic<int32_t> received{-1};
    // Through the base class pointer: Drain is a private override in LoanedDataReaderListener (it
    // overrides DataReaderListenerBase's public virtual under a `private:` section, matching how
    // on_data_available calls it), and access is checked against the static type used to call it.
    // Not installed on `reader` -- driven by calling Drain directly, in a polling loop, same as
    // this test always has.
    const std::unique_ptr<internal::DataReaderListenerBase> listener =
        std::make_unique<internal::LoanedDataReaderListener>(
            /*status_listener=*/nullptr, pub_provider.PayloadBytes(),
            TestChannel(
                [&](const uint8_t* data, size_t len, const SharedSchema&, const Attachments&) {
                    if (len >= 5) received.store(DecodeRow(data));
                }));
    listener->SetSchema(MakeSharedSchema(MakeSchema()));

    // This reader is enabled immediately (a raw Fast DDS entity, not Fletcher's own
    // Subscribe/schema gate), but the data channel is VOLATILE now (qos_defaults.cpp): a row
    // published before the writer matches it is dropped. AwaitMatch only observes the WRITER's
    // own side of that match, and RTPS matching is not perfectly simultaneous on both ends -- a
    // reader whose OWN match is still settling can still miss a sample sent the instant the
    // writer's side completed. Republishing the same row until it lands, bounded by an overall
    // deadline, covers that gap without guessing at its size.
    ASSERT_TRUE(pub_listener.AwaitMatch()) << "the data writer never matched the hand-built reader";

    // No listener is installed on `reader` (Drain is called by hand below), so this test has to
    // notice new data itself: a WaitSet on the reader's own status condition, limited to
    // data_available, wakes as soon as Fast DDS accepts a sample instead of on a fixed interval.
    StatusCondition& status_condition = reader->get_statuscondition();
    status_condition.set_enabled_statuses(StatusMask::data_available());
    WaitSet wait_set;
    wait_set.attach_condition(status_condition);
    ConditionSeq active;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (received.load() == -1 && std::chrono::steady_clock::now() < deadline) {
        pub_provider.Publish({"loaned", "direct"}, MakeEncoder(99));
        if (wait_set.wait(active, Duration_t(1, 0)) == RETCODE_OK) {
            listener->Drain(reader);
        }
    }
    EXPECT_EQ(received.load(), 99);

    subscriber->delete_datareader(reader);
    participant->delete_topic(topic);
    participant->delete_subscriber(subscriber);
    DomainParticipantFactory::get_instance()->delete_participant(participant);
}

// Loans are never negotiated: each side's own type gates its own, so all pairings interoperate.

// ---------------------------------------------------------------------------
// Tests — QoS configuration
// ---------------------------------------------------------------------------
//
// PDA-DEC-6 retired five tests from here — `CustomDefaultWriterQos`,
// `CustomDefaultReaderQos`, `PerTopicWriterQosOverridesDefault`,
// `PerTopicReaderQosOverridesDefault` and `AutonomyStyleProfileViaOptions` — with the typed
// `FastDDSProviderOptions` struct they configured. Each set a QoS value and then asserted only
// that a row arrived, which is true of almost any QoS; none could tell a provider that applied
// the setting from one that ignored it. Their replacements in `test_profile_document.cpp` read
// back what the endpoint ANNOUNCED on the network, setting for setting, and are strictly
// stronger:
//
//   CustomDefault{Writer,Reader}Qos        -> FastDdsConfig.ProfileDocumentConfiguresQos
//                                             FastDdsConfig.ReaderProfileConfiguresTheReader
//   PerTopic{Writer,Reader}QosOverridesDefault
//                                          -> FastDdsConfig.PerTopicProfileOverridesTheDefault
//   AutonomyStyleProfileViaOptions         -> FastDdsConfig.DefaultProfileTranscriptionIsExact
//                                             (whole-struct, so it covers the history and
//                                             resource_limits that discovery cannot carry)
// ---------------------------------------------------------------------------
// Subscriber-first: Subscribe before any publisher/topic exists must not block or throw. The
// data reader it creates stays DISABLED until the topic's schema is known, so nothing reaches
// the callback before then; once a publisher appears and announces, the schema thread hands the
// schema to the listener and enables the reader, and the first callback fires with a non-null
// schema (never invoked with a null one).
// ---------------------------------------------------------------------------
TEST(FastDDSPubSubProviderTest, SubscribeBeforePublishDeliversWithSchema) {
    DataWriterMatchListener pub_listener;
    FastDDSPubSubProvider sub_provider(ProviderConfig{});
    FastDDSPubSubProvider pub_provider(ProviderConfig{}, &pub_listener);

    std::mutex mu;
    std::condition_variable cv;
    std::atomic<int32_t> received{-1};
    SharedSchema rx_schema;

    // Subscribe with no publisher yet — must return immediately (no block, no throw).
    SubscriptionResult result = sub_provider.Subscribe(
        {"subfirst", "x"},
        [&](const uint8_t* data, size_t len, const SharedSchema& schema, const Attachments&) {
            std::lock_guard<std::mutex> lk(mu);
            rx_schema = schema;
            if (len >= 5) received.store(DecodeRow(data));
            cv.notify_all();
        });

    // No publisher has announced the schema yet, so the arrival is PENDING —
    // which is a distinct answer from "this transport carries no schemas" and
    // from "this subscription is over".
    SharedSchema polled;
    EXPECT_EQ(result.schema.Wait(std::chrono::milliseconds(0), &polled), PubSubStatus::kPending);
    EXPECT_EQ(polled, nullptr) << "*out is untouched unless the answer is kOk";

    // A publisher appears and publishes.
    pub_provider.CreateTopic({"subfirst", "x"}, MakeSchema());
    ASSERT_TRUE(pub_listener.AwaitMatch())
        << "the data writer never matched the subscriber's data reader";
    pub_provider.Publish({"subfirst", "x"}, MakeEncoder(99));

    // The arrival now answers kOk with a non-null schema.
    SharedSchema fut_schema = AwaitSchema(result, std::chrono::seconds(5));
    ASSERT_TRUE(fut_schema);
    EXPECT_EQ(fut_schema->n_children, 1);

    // The first callback fired with the row and a non-null schema.
    {
        std::unique_lock<std::mutex> lk(mu);
        ASSERT_TRUE(
            cv.wait_for(lk, std::chrono::seconds(5), [&] { return received.load() != -1; }));
    }
    EXPECT_EQ(received.load(), 99);
    {
        std::lock_guard<std::mutex> lk(mu);
        ASSERT_TRUE(rx_schema);
        EXPECT_EQ(rx_schema->n_children, 1);
    }

    sub_provider.Unsubscribe({"subfirst", "x"});
}

// ---------------------------------------------------------------------------
// Subscribe-first burst: the subscriber joins before the publisher. The data reader stays
// disabled until the schema arrives, and matching cannot fire before that -- but the data channel
// is VOLATILE now (qos_defaults.cpp, owner decision 2026-09-15), so a row published before the
// writer actually matches the reader is dropped, not replayed. Waiting for the publisher's own
// OnMatched on the data topic (never fires before the reader is enabled) before starting the burst
// proves both the schema arrived and the match happened. Functional smoke test: 1000 rows,
// delivered in order.
//
// Paced by real delivery progress, not a fixed delay -- MEASURED: the writer profile here is
// KEEP_ALL with a 100-sample resourceLimitsQos.max_samples and Fast DDS's default
// max_blocking_time (100 ms; qos_defaults.cpp no longer sets one), so an unpaced 1000-sample burst
// that outruns the reader drops samples once it blocks past that. Keeping the gap between
// published and received counts under the resource limit, using the same cv `received` already
// notifies, sidesteps it without guessing at a delay.
TEST(FastDDSPubSubProviderTest, SubscribeFirstBurstDeliveredInOrder) {
    constexpr int32_t kCount = 1000;
    constexpr size_t kWindow = 50;  // comfortably under the 100-sample resourceLimitsQos pool

    // Declared before the providers: calls arrive until the last of them is destroyed.
    DataWriterMatchListener pub_listener;
    FastDDSPubSubProvider sub_provider(ProviderConfig{});
    FastDDSPubSubProvider pub_provider(ProviderConfig{}, &pub_listener);

    std::mutex mu;
    std::condition_variable cv;
    std::vector<int32_t> received;

    (void)sub_provider.Subscribe(
        {"ordering", "burst"},
        [&](const uint8_t* data, size_t len, const SharedSchema&, const Attachments&) {
            if (len < 5) return;
            std::lock_guard<std::mutex> lk(mu);
            received.push_back(DecodeRow(data));
            cv.notify_all();
        });

    pub_provider.CreateTopic({"ordering", "burst"}, MakeSchema());

    ASSERT_TRUE(pub_listener.AwaitMatch(std::chrono::seconds(15)))
        << "the data writer never matched the subscriber's data reader";

    for (int32_t i = 0; i < kCount; ++i) {
        if (static_cast<size_t>(i) >= kWindow) {
            std::unique_lock<std::mutex> lk(mu);
            ASSERT_TRUE(cv.wait_for(
                lk, std::chrono::seconds(15),
                [&] { return received.size() >= static_cast<size_t>(i) - kWindow + 1; }))
                << "delivery stalled at " << received.size() << " of " << kCount << " samples";
        }
        pub_provider.Publish({"ordering", "burst"}, MakeEncoder(i));
    }

    {
        std::unique_lock<std::mutex> lk(mu);
        ASSERT_TRUE(cv.wait_for(lk, std::chrono::seconds(15),
                                [&] { return received.size() == static_cast<size_t>(kCount); }))
            << "received " << received.size() << " of " << kCount << " samples";
    }
    sub_provider.Unsubscribe({"ordering", "burst"});

    std::lock_guard<std::mutex> lk(mu);
    ASSERT_EQ(received.size(), static_cast<size_t>(kCount));
    for (int32_t i = 0; i < kCount; ++i) {
        ASSERT_EQ(received[static_cast<size_t>(i)], i) << "out-of-order delivery at index " << i;
    }
}

// ---------------------------------------------------------------------------
// A data reader is created DISABLED and stays that way until its topic's schema is known: no
// RTPS reader exists for it at all (Entity::enable()), so Fast DDS announces nothing about it on
// the network in the meantime. The schema reader gets no such treatment -- it is enabled
// immediately (EnsureSchemaChannel) -- so a bystander sees the schema side of a subscriber-first
// Subscribe appear right away and the data side only once a publisher's CreateTopic has supplied
// the schema. Modeled on DiscoveryObserver (test_profile_document.cpp), which is file-local there.
// ---------------------------------------------------------------------------
namespace {

class ReaderDiscoveryObserver : public DomainParticipantListener {
   public:
    explicit ReaderDiscoveryObserver(uint32_t domain) {
        DomainParticipantQos qos;
        qos.name("FletcherReaderDiscoveryObserver");
        participant_ = DomainParticipantFactory::get_instance()->create_participant(
            domain, qos, this, StatusMask::none());
    }

    ~ReaderDiscoveryObserver() override {
        if (participant_)
            DomainParticipantFactory::get_instance()->delete_participant(participant_);
    }

    ReaderDiscoveryObserver(const ReaderDiscoveryObserver&) = delete;
    ReaderDiscoveryObserver& operator=(const ReaderDiscoveryObserver&) = delete;

    void on_data_reader_discovery(DomainParticipant*,
                                  eprosima::fastdds::rtps::ReaderDiscoveryStatus reason,
                                  const eprosima::fastdds::rtps::SubscriptionBuiltinTopicData& info,
                                  bool& should_be_ignored) override {
        should_be_ignored = false;
        if (reason != eprosima::fastdds::rtps::ReaderDiscoveryStatus::DISCOVERED_READER) return;
        std::lock_guard<std::mutex> lock(mu_);
        seen_.insert(info.topic_name.to_string());
        cv_.notify_all();
    }

    bool Sees(const std::string& topic) {
        std::lock_guard<std::mutex> lock(mu_);
        return seen_.count(topic) != 0;
    }

    bool AwaitReader(const std::string& topic, std::chrono::milliseconds budget) {
        std::unique_lock<std::mutex> lock(mu_);
        return cv_.wait_for(lock, budget, [&] { return seen_.count(topic) != 0; });
    }

   private:
    DomainParticipant* participant_ = nullptr;
    std::mutex mu_;
    std::condition_variable cv_;
    std::set<std::string> seen_;
};

}  // namespace

TEST(FastDDSPubSubProviderTest, ADataReaderIsNotEnabledBeforeTheSchemaArrives) {
    ReaderDiscoveryObserver observer(0);

    FastDDSPubSubProvider sub_provider(ProviderConfig{});

    std::atomic<int32_t> received{-1};
    SubscriptionResult result = sub_provider.Subscribe(
        {"notenabled", "x"},
        [&](const uint8_t* data, size_t len, const SharedSchema&, const Attachments&) {
            if (len >= 5) received.store(DecodeRow(data));
            NotifyWaiters();
        });

    ASSERT_TRUE(observer.AwaitReader("notenabled/x/__schema", std::chrono::seconds(5)))
        << "the schema reader, which is enabled immediately, was never discovered";

    // Discovery fence, not a settle: a second Subscribe on a fresh topic from the same provider
    // creates another schema reader immediately (same as the first), strictly after "notenabled/x"
    // in program order. sub_provider's own endpoint creation and this observer's builtin discovery
    // reader both process in that order, so once the fence's schema reader is seen, any premature
    // "notenabled/x" data reader discovery would already be visible too.
    (void)sub_provider.Subscribe(
        {"notenabled", "fence"},
        [](const uint8_t*, size_t, const SharedSchema&, const Attachments&) {});
    ASSERT_TRUE(observer.AwaitReader("notenabled/fence/__schema", std::chrono::seconds(5)))
        << "the fence subscription's own schema reader was never discovered";
    EXPECT_FALSE(observer.Sees("notenabled/x"))
        << "the data reader was discovered before its topic's schema arrived";
    sub_provider.Unsubscribe({"notenabled", "fence"});

    DataWriterMatchListener pub_listener;
    FastDDSPubSubProvider pub_provider(ProviderConfig{}, &pub_listener);
    pub_provider.CreateTopic({"notenabled", "x"}, MakeSchema());

    ASSERT_TRUE(observer.AwaitReader("notenabled/x", std::chrono::seconds(5)))
        << "the data reader never appeared once its schema was known";
    ASSERT_TRUE(AwaitSchema(result, std::chrono::seconds(5)));

    ASSERT_TRUE(pub_listener.AwaitMatch())
        << "the data writer never matched the subscriber's data reader";
    pub_provider.Publish({"notenabled", "x"}, MakeEncoder(11));
    EXPECT_EQ(AwaitRow(received), 11);

    sub_provider.Unsubscribe({"notenabled", "x"});
}

// ---------------------------------------------------------------------------
// CreateTopic then Subscribe, on the SAME provider: EnsureSchemaChannel resolves the schema on
// the spot from `ts.schema` and opens no `__schema` reader at all for this topic, so a bystander
// never discovers one from this provider here. (Subscribe's enable()-failure unwind used to
// dereference a null `ts.schema_reader` on exactly this path -- fixed to guard it the way its
// sibling unwinds do. Forcing that enable() failure cheaply is not practical here, so this pins
// the happy path: delivery still works, and no schema reader was created.)
// ---------------------------------------------------------------------------
TEST(FastDDSPubSubProviderTest, CreateTopicThenSubscribeOnOneProviderDeliversWithoutASchemaReader) {
    ReaderDiscoveryObserver observer(0);

    FastDDSPubSubProvider provider(ProviderConfig{});
    provider.CreateTopic({"selfschema", "x"}, MakeSchema());

    std::atomic<int32_t> received{-1};
    SubscriptionResult result = provider.Subscribe(
        {"selfschema", "x"},
        [&](const uint8_t* data, size_t len, const SharedSchema&, const Attachments&) {
            if (len >= 5) received.store(DecodeRow(data));
            NotifyWaiters();
        });

    ASSERT_TRUE(AwaitSchema(result, std::chrono::seconds(5)));
    ASSERT_TRUE(observer.AwaitReader("selfschema/x", std::chrono::seconds(5)))
        << "the data reader never appeared";

    provider.Publish({"selfschema", "x"}, MakeEncoder(7));
    EXPECT_EQ(AwaitRow(received), 7);

    EXPECT_FALSE(observer.Sees("selfschema/x/__schema"))
        << "a schema reader was created even though this provider already knew the schema locally";

    provider.Unsubscribe({"selfschema", "x"});
}

// EachTopicHasItsOwnDeliveryThread's premise died with this round's reader-side redesign (see the
// file-header table, fast_dds_pubsub_provider.cpp): data delivery is a Fast DDS listener again, so
// a live sample's on_data_available runs wherever Fast DDS calls it -- intraprocess, that is the
// PUBLISHING thread (DataReaderListenerBase, internal/data_reader_listener.hpp) -- not on a thread
// this provider owns. "Neither the subscribing nor the publishing thread" is therefore no longer
// true of a data callback and would be pinning the wrong thing.
//
// What IS still true, and worth pinning instead: schema-before-data ordering
// (ADataReaderIsNotEnabledBeforeTheSchemaArrives, above) still holds, and schema RESOLUTION still
// runs on a thread this provider owns -- its one schema thread (Impl::SchemaLoop) -- never inline
// on whichever thread called Subscribe or CreateTopic. Observed the same way
// OnMatchedFiresForASchemaReadersOwnCondition (below) observes that thread exists at all: a status
// callback for the schema reader's own subscription_matched, forwarded from inside HandleSchema
// (DispatchReaderStatuses, fast_dds_pubsub_provider.cpp) on the schema thread.
TEST(FastDDSPubSubProviderTest,
     DataCallbacksRunOnAFastDdsThreadAndSchemaResolvesOnTheProviderThread) {
    struct SchemaThreadListener : FastDDSStatusListener {
        std::mutex m;
        std::condition_variable cv;
        bool got_schema_match = false;
        std::thread::id schema_thread_id;

        void OnMatched(Endpoint endpoint, int32_t /*current_count*/,
                       int32_t change) noexcept override {
            if (change <= 0 || endpoint.is_writer || !endpoint.is_schema_channel) return;
            std::lock_guard<std::mutex> lk(m);
            schema_thread_id = std::this_thread::get_id();
            got_schema_match = true;
            cv.notify_all();
        }
    };

    SchemaThreadListener listener;
    DataWriterMatchListener pub_listener;
    FastDDSPubSubProvider pub_provider(ProviderConfig{}, &pub_listener);
    FastDDSPubSubProvider sub_provider(ProviderConfig{}, &listener);

    std::mutex data_m;
    std::condition_variable data_cv;
    std::thread::id data_thread_id;
    bool got_data = false;

    pub_provider.CreateTopic({"schemathread", "x"}, MakeSchema());
    SubscriptionResult result = sub_provider.Subscribe(
        {"schemathread", "x"},
        [&](const uint8_t*, size_t, const SharedSchema&, const Attachments&) {
            std::lock_guard<std::mutex> lk(data_m);
            data_thread_id = std::this_thread::get_id();
            got_data = true;
            data_cv.notify_all();
        });
    ASSERT_TRUE(AwaitSchema(result, std::chrono::seconds(5)));

    std::unique_lock<std::mutex> lk(listener.m);
    ASSERT_TRUE(listener.cv.wait_for(lk, std::chrono::seconds(5), [&] {
        return listener.got_schema_match;
    })) << "the schema reader's own subscription_matched never reached OnMatched";
    // This test's own thread both Subscribed and (through pub_provider) declared the schema, so
    // checking against it covers both "not the subscribing thread" and "not the publishing
    // thread" at once -- the schema thread is neither.
    EXPECT_NE(listener.schema_thread_id, std::this_thread::get_id());
    lk.unlock();

    ASSERT_TRUE(pub_listener.AwaitMatch())
        << "the data writer never matched the subscriber's data reader";
    pub_provider.Publish({"schemathread", "x"}, MakeEncoder(1));
    {
        std::unique_lock<std::mutex> data_lk(data_m);
        ASSERT_TRUE(data_cv.wait_for(data_lk, std::chrono::seconds(5), [&] { return got_data; }))
            << "the data callback never fired";
    }
    // The data callback runs wherever Fast DDS calls on_data_available -- intraprocess, the
    // PUBLISHING thread -- never on this provider's own schema thread.
    EXPECT_NE(data_thread_id, listener.schema_thread_id);

    sub_provider.Unsubscribe({"schemathread", "x"});
}

// ---------------------------------------------------------------------------
// Unsubscribe detaches the listeners it owns, not whatever is there afterwards
//
// Unsubscribe deletes the readers outside the provider lock, because their callbacks take locks of
// their own. It used to then re-find the topic and reset its listeners — but a Subscribe racing
// that window sees `reader == nullptr`, installs a *new* reader and listener, and the second lookup
// destroys that live listener underneath it. The listeners are now moved out alongside the readers
// under the first lock, so a resubscribe cycle can never lose the new one.
// ---------------------------------------------------------------------------
TEST(FastDDSPubSubProviderTest, ResubscribeAfterUnsubscribeKeepsDelivering) {
    // `PositiveMatches()` tells the rematch after Unsubscribe apart from the still-live first one.
    DataWriterMatchListener pub_listener;
    FastDDSPubSubProvider pub_provider(ProviderConfig{}, &pub_listener);
    FastDDSPubSubProvider sub_provider(ProviderConfig{});
    pub_provider.CreateTopic({"resub", "x"}, MakeSchema());

    std::atomic<int32_t> first{-1};
    const SubscriptionResult fence = sub_provider.Subscribe(
        {"resub", "x"},
        [&](const uint8_t* data, size_t len, const SharedSchema&, const Attachments&) {
            if (len >= 5) first.store(DecodeRow(data));
            NotifyWaiters();
        });
    static_cast<void>(fence);  // unused on purpose: this is the discovery fence, not a schema wait

    ASSERT_TRUE(pub_listener.AwaitMatch())
        << "the data writer never matched the subscriber's data reader";
    const int32_t matches_before_publish = pub_listener.PositiveMatches();
    pub_provider.Publish({"resub", "x"}, MakeEncoder(1));
    ASSERT_EQ(AwaitRow(first), 1);

    sub_provider.Unsubscribe({"resub", "x"});

    // The second subscription must be fully functional: its listener has to survive the teardown of
    // the first, and the schema future has to resolve again.
    std::atomic<int32_t> second{-1};
    SubscriptionResult again = sub_provider.Subscribe(
        {"resub", "x"},
        [&](const uint8_t* data, size_t len, const SharedSchema&, const Attachments&) {
            if (len >= 5) second.store(DecodeRow(data));
            NotifyWaiters();
        });
    ASSERT_TRUE(AwaitSchema(again, std::chrono::seconds(5)));

    ASSERT_TRUE(pub_listener.AwaitMatchAfter(matches_before_publish))
        << "the data writer never rematched the new reader after resubscribe";
    pub_provider.Publish({"resub", "x"}, MakeEncoder(2));
    ASSERT_EQ(AwaitRow(second), 2);
}

// Unsubscribing a topic that was never subscribed is a no-op, and must not disturb a live
// subscription on a different topic.
TEST(FastDDSPubSubProviderTest, UnsubscribeUnknownTopicIsHarmless) {
    DataWriterMatchListener pub_listener;
    FastDDSPubSubProvider pub_provider(ProviderConfig{}, &pub_listener);
    FastDDSPubSubProvider sub_provider(ProviderConfig{});
    pub_provider.CreateTopic({"unsub", "live"}, MakeSchema());

    std::atomic<int32_t> received{-1};
    const SubscriptionResult fence = sub_provider.Subscribe(
        {"unsub", "live"},
        [&](const uint8_t* data, size_t len, const SharedSchema&, const Attachments&) {
            if (len >= 5) received.store(DecodeRow(data));
            NotifyWaiters();
        });
    static_cast<void>(fence);  // unused on purpose: this is the discovery fence, not a schema wait

    EXPECT_NO_THROW(sub_provider.Unsubscribe({"unsub", "never"}));

    ASSERT_TRUE(pub_listener.AwaitMatch())
        << "the data writer never matched the subscriber's data reader";
    pub_provider.Publish({"unsub", "live"}, MakeEncoder(7));
    EXPECT_EQ(AwaitRow(received), 7);
}

// The schema channel's bound was `FastDDSProviderOptions::max_schema_bytes`, then PDA-DEC-6 moved
// it into the document as the `fletcher.max_schema_bytes` vendor property; owner decision
// 2026-09-15 fixed it at `kSchemaPayloadBytes` (pubsub/payload_bound.hpp) -- no document property
// any more. The tests for it live in `test_profile_document.cpp`:
//
//   ASchemaLargerThanTheSchemaBoundIsRefused ->
//   FastDdsConfig.ASchemaLargerThanTheSchemaBoundIsRefused AFailedSchemaAnnouncementCanBeRetried ->
//   FastDdsConfig.AFailedSchemaAnnouncementCanBeRetried

// ---------------------------------------------------------------------------
// Tests — FastDDSStatusListener
//
// The statuses are the application's now: a provider built from a `ProviderConfig` alone observes
// nothing, and a provider handed a listener reports through Fletcher's own vocabulary rather than
// eProsima's. Both tests below pin what a consumer actually rebuilds from these callbacks — a
// matched count, and the one diagnostic a payload-bound mismatch has.
// ---------------------------------------------------------------------------

// One listener, both providers: the same object hears the publisher's side (`is_writer == true`)
// and the subscriber's (`false`) for the one topic, which is what makes `Endpoint::is_writer` the
// thing that tells them apart rather than the callback's name.
TEST(FastDDSPubSubProviderTest, StatusListenerReportsMatchingBothWays) {
    struct CountingListener : FastDDSStatusListener {
        struct Match {
            std::string topic;
            bool is_writer;
            int32_t current_count;
        };

        std::mutex m;
        std::condition_variable cv;
        std::vector<Match> matches;

        void OnMatched(Endpoint endpoint, int32_t current_count, int32_t change) noexcept override {
            // Only the gained half is asserted on below. The companion __schema reader now reports
            // matches too (item A: every reader's StatusCondition is left at its default enabled
            // mask, so subscription_matched is no longer schema-reader-exempt) -- excluded here by
            // `is_schema_channel` so this stays a pin on the DATA channel specifically, not on
            // whichever of the two happens to match first.
            if (change <= 0 || endpoint.is_schema_channel) return;
            std::lock_guard<std::mutex> lk(m);
            matches.push_back({std::string(endpoint.topic), endpoint.is_writer, current_count});
            cv.notify_all();
        }

        // Called with `m` held by the waiter.
        bool Saw(const std::string& topic, bool is_writer) const {
            for (const auto& match : matches) {
                if (match.topic == topic && match.is_writer == is_writer &&
                    match.current_count == 1) {
                    return true;
                }
            }
            return false;
        }
    };

    // Declared before the providers: calls arrive until the last of them is destroyed.
    CountingListener listener;
    FastDDSPubSubProvider pub_provider(ProviderConfig{}, &listener);
    FastDDSPubSubProvider sub_provider(ProviderConfig{}, &listener);

    pub_provider.CreateTopic({"status", "x"}, MakeSchema());
    SubscriptionResult result = sub_provider.Subscribe(
        {"status", "x"}, [](const uint8_t*, size_t, const SharedSchema&, const Attachments&) {});
    ASSERT_TRUE(AwaitSchema(result, std::chrono::seconds(5)));

    pub_provider.Publish({"status", "x"}, MakeEncoder(1));

    std::unique_lock<std::mutex> lk(listener.m);
    EXPECT_TRUE(listener.cv.wait_for(lk, std::chrono::seconds(5), [&] {
        return listener.Saw("status/x", true) && listener.Saw("status/x", false);
    })) << "matching was not reported on both sides";
}

// A subscriber whose payload bound differs from its publisher's never matches — the bound is part
// of the registered type name, so endpoint matching refuses the pair — but discovery still sees the
// remote writer, and `type_name` is where the other bound is legible. That gap is the diagnostic
// this listener exists for: for the DATA channel specifically, `OnMatched` and `OnIncompatibleQos`
// both stay silent (the companion __schema channel matches regardless -- see below).
TEST(FastDDSPubSubProviderTest, DiscoverySeesAWriterOnAnotherBound) {
    struct DiscoveryListener : FastDDSStatusListener {
        struct Seen {
            std::string topic;
            std::string type_name;
            bool alive;
        };

        std::mutex m;
        std::condition_variable cv;
        std::vector<Seen> writers;
        bool matched = false;
        bool incompatible = false;

        void OnWriterDiscovered(std::string_view topic, std::string_view type_name,
                                bool alive) noexcept override {
            std::lock_guard<std::mutex> lk(m);
            writers.push_back({std::string(topic), std::string(type_name), alive});
            cv.notify_all();
        }

        void OnMatched(Endpoint endpoint, int32_t, int32_t) noexcept override {
            // The companion __schema reader DOES match (its type carries no payload bound -- see
            // the comment below, above AwaitSchema): excluded here so this stays a pin on the DATA
            // channel specifically (item A: every reader's StatusCondition, the schema reader's
            // included, now reports subscription_matched, not only the data reader's).
            if (endpoint.topic != "bound/x" || endpoint.is_schema_channel) return;
            std::lock_guard<std::mutex> lk(m);
            matched = true;
        }

        void OnIncompatibleQos(Endpoint endpoint, uint32_t, uint32_t) noexcept override {
            if (endpoint.topic != "bound/x") return;
            std::lock_guard<std::mutex> lk(m);
            incompatible = true;
        }

        // Called with `m` held by the waiter.
        bool SawWriter(const std::string& topic, const std::string& type_name) const {
            for (const auto& writer : writers) {
                if (writer.topic == topic && writer.type_name == type_name && writer.alive) {
                    return true;
                }
            }
            return false;
        }
    };

    DiscoveryListener listener;
    FastDDSPubSubProvider sub_provider(ProviderConfig{}, &listener);

    ProviderConfig pub_config;
    pub_config.max_payload_bytes = kPayloadBytes<8192>;
    FastDDSPubSubProvider pub_provider(pub_config);

    SubscriptionResult result = sub_provider.Subscribe(
        {"bound", "x"}, [](const uint8_t*, size_t, const SharedSchema&, const Attachments&) {});
    pub_provider.CreateTopic({"bound", "x"}, MakeSchema());
    pub_provider.Publish({"bound", "x"}, MakeEncoder(1));

    {
        std::unique_lock<std::mutex> lk(listener.m);
        ASSERT_TRUE(listener.cv.wait_for(lk, std::chrono::seconds(5), [&] {
            return listener.SawWriter("bound/x", FletcherTypeName(8192));
        })) << "the writer on the other bound was never discovered";
    }

    // The companion `/__schema` channel DOES pair up — its type name carries no payload bound — so
    // the schema arrives even though no row ever will. Worth pinning: it is why a resolved schema
    // is not evidence that the data endpoints matched.
    ASSERT_TRUE(AwaitSchema(result, std::chrono::seconds(5)));

    // Discovery fires from PDP::addWriterProxyData, BEFORE EDP is asked whether the pair matches,
    // so neither wait above is a point after which "did not match" can be read off.
    //
    // Discovery fence, not a settle: a second writer, created on the SAME participant strictly
    // after "bound/x"'s, whose own SEDP proxy data this listener's single discovery thread
    // processes strictly after "bound/x"'s -- so once its OnWriterDiscovered is seen, the EDP
    // match check for "bound/x" (which runs inline while processing ITS proxy data) has already
    // run too.
    pub_provider.CreateTopic({"bound", "fence"}, MakeSchema());
    pub_provider.Publish({"bound", "fence"}, MakeEncoder(1));
    {
        std::unique_lock<std::mutex> lk(listener.m);
        ASSERT_TRUE(listener.cv.wait_for(lk, std::chrono::seconds(5), [&] {
            return listener.SawWriter("bound/fence", FletcherTypeName(8192));
        })) << "the fence writer was never discovered";
    }
    {
        std::lock_guard<std::mutex> lk(listener.m);
        EXPECT_FALSE(listener.matched) << "endpoints on different bounds matched";
        EXPECT_FALSE(listener.incompatible)
            << "a bound mismatch surfaced as incompatible QoS; discovery is no longer the only "
               "diagnostic and this test's premise is stale";
    }
}

// ---------------------------------------------------------------------------
// Tests — FastDDSLoggingStatusListener / FastDDSStatusListener (cycle 2, T7 + P22)
//
// Deadline, liveliness, sample-lost and sample-rejected coverage THROUGH A LIVE PROVIDER (forcing
// the real DDS conditions that drive them) is NOT attempted here — deferred to a later cycle,
// per the brief, and listed as untested in this cycle's report. P22 below exercises every one of
// FastDDSLoggingStatusListener's eight bodies directly instead, which needs no DDS condition to be
// forced at all; the FastDDSStatusListener subclass tests that follow it drive the ones that ARE
// reachable through a real provider pair (OnIncompatibleQos, OnReaderDiscovered,
// OnParticipantDiscovered, and -- since item A (2026-09-14) -- OnMatched for a __schema reader
// itself: is_schema_channel == true reaching OnMatched used to be unreachable, because the schema
// reader's own status mask (the now-deleted internal::SchemaReaderStatusMask()) carried no
// subscription_matched bit; the schema reader's StatusCondition is left at its default enabled
// mask now, and this provider's one schema thread forwards whatever changed on it
// (DispatchReaderStatuses, fast_dds_pubsub_provider.cpp) -- see
// OnMatchedFiresForASchemaReadersOwnCondition below.
// ---------------------------------------------------------------------------

namespace {

// The Cycle 1 conflict test's pattern (ConflictingCrossProviderSchemaIsLoggedNotSwallowed):
// RegisterConsumer only ADDS a consumer, never Log::ClearConsumers, which would drop the default
// stdout consumer along with it.
class CapturingLogConsumer : public eprosima::fastdds::dds::LogConsumer {
   public:
    void Consume(const eprosima::fastdds::dds::Log::Entry& entry) override {
        {
            std::lock_guard<std::mutex> lk(m);
            messages.push_back(entry.message);
        }
        NotifyWaiters();
    }

    bool SawContaining(const std::string& needle) {
        std::lock_guard<std::mutex> lk(m);
        for (const auto& msg : messages) {
            if (msg.find(needle) != std::string::npos) return true;
        }
        return false;
    }

    std::mutex m;
    std::vector<std::string> messages;
};

}  // namespace

// P22: FastDDSLoggingStatusListener driven directly — no DDS participant at all, since every one
// of its eight bodies (status_listener.cpp) only formats `endpoint` and its arguments into a log
// line. Each call below names a topic string found nowhere else in this process, so a hit against
// it can only be the call that named it, never another test's own transport traffic running
// concurrently.
//
// Verbosity is raised on purpose: probing this test empirically (not assumed from Log.hpp's
// comments) showed this process's DEFAULT runtime verbosity admits ERROR only — EPROSIMA_LOG_ERROR
// is unconditional, but EPROSIMA_LOG_WARNING is gated at runtime by
// `Log::GetVerbosity() >= Log::Kind::Warning`, and that gate is closed by default here. Without
// raising it, every WARNING-level body below is silently dropped before it ever reaches a
// consumer, which is what the first run of this test found the hard way. Each test runs in its own
// process (ctest invokes one `--gtest_filter` per test), so this has no other test to leak into.
// EPROSIMA_LOG_INFO needs FASTDDS_ENFORCE_LOG_INFO besides, which this build does not define, so it
// compiles to nothing regardless of verbosity — no call here is chosen to need it.
//
// The two is_schema_channel branches of OnSampleLost and OnSampleRejected are called ALONGSIDE
// their data-channel siblings (the "plus both is_schema_channel branches" half of this item), but
// their fixed messages (status_listener.cpp) do not carry `endpoint.topic` at all — a schema
// rejection or loss is reported once per channel, not per topic — so those two are asserted on
// their own literal, distinctive text instead of a topic string.
TEST(FastDDSLoggingStatusListenerTest, EveryCallbackLogsIncludingBothSchemaChannelBranches) {
    eprosima::fastdds::dds::Log::SetVerbosity(eprosima::fastdds::dds::Log::Kind::Warning);

    auto* consumer = new CapturingLogConsumer();
    eprosima::fastdds::dds::Log::RegisterConsumer(
        std::unique_ptr<eprosima::fastdds::dds::LogConsumer>(consumer));

    FastDDSLoggingStatusListener listener;
    using Endpoint = FastDDSStatusListener::Endpoint;

    listener.OnMatched(Endpoint{"logprobe/matched", false, true}, 3, -1);
    listener.OnIncompatibleQos(Endpoint{"logprobe/incompatible", false, false}, 7, 2);
    listener.OnDeadlineMissed(Endpoint{"logprobe/deadline", false, true}, 4);
    listener.OnLivelinessChanged(Endpoint{"logprobe/liveliness", false, false}, 1, 2);
    listener.OnLivelinessLost(Endpoint{"logprobe/livelost", false, true}, 5);
    listener.OnSampleLost(Endpoint{"logprobe/samplelost_data", false, false}, 6);
    listener.OnSampleLost(Endpoint{"logprobe/samplelost_schema", true, false}, 6);
    listener.OnSampleRejected(Endpoint{"logprobe/samplerejected_data", false, false}, 1, 8);
    listener.OnSampleRejected(Endpoint{"logprobe/samplerejected_schema", true, false}, 1, 8);
    listener.OnUnacknowledgedSampleRemoved(Endpoint{"logprobe/unacked", false, true});

    // Every OnXxx call above ran synchronously on this thread, so QueueLog already queued its log
    // entry before Flush() is reached below; Flush() blocks until the logging thread has consumed
    // everything queued as of this call (Log::Flush(), Log.cpp), so the wait below is a safety net
    // for Consume() dispatch, not for anything still outstanding.
    eprosima::fastdds::dds::Log::Flush();
    WaitUntil([&] { return consumer->SawContaining("logprobe/unacked"); }, std::chrono::seconds(2));
    eprosima::fastdds::dds::Log::Flush();

    EXPECT_TRUE(consumer->SawContaining("logprobe/matched")) << "OnMatched did not log";
    EXPECT_TRUE(consumer->SawContaining("logprobe/incompatible"))
        << "OnIncompatibleQos did not log";
    EXPECT_TRUE(consumer->SawContaining("logprobe/deadline")) << "OnDeadlineMissed did not log";
    EXPECT_TRUE(consumer->SawContaining("logprobe/liveliness"))
        << "OnLivelinessChanged did not log";
    EXPECT_TRUE(consumer->SawContaining("logprobe/livelost")) << "OnLivelinessLost did not log";
    EXPECT_TRUE(consumer->SawContaining("logprobe/samplelost_data"))
        << "OnSampleLost (data channel) did not log";
    EXPECT_TRUE(consumer->SawContaining("a schema sample was lost"))
        << "OnSampleLost (schema channel) did not log";
    EXPECT_TRUE(consumer->SawContaining("logprobe/samplerejected_data"))
        << "OnSampleRejected (data channel) did not log";
    EXPECT_TRUE(consumer->SawContaining("a schema sample was rejected"))
        << "OnSampleRejected (schema channel) did not log";
    EXPECT_TRUE(consumer->SawContaining("logprobe/unacked"))
        << "OnUnacknowledgedSampleRemoved did not log";
}

// A test-local FastDDSStatusListener subclass, driven through a real provider pair: a RELIABLE
// reader against a BEST_EFFORT writer is a REQUESTED_INCOMPATIBLE_QOS on the reliability policy,
// so the reader's side of OnIncompatibleQos must fire. The __schema channel keeps its own fixed
// QoS regardless of what the data channel's document says (this file's "A supplied profile is
// that endpoint's WHOLE quality-of-service" note, above BoundedDocument), so the schema still
// resolves even though the data endpoints never match.
TEST(FastDDSStatusListenerTest, OnIncompatibleQosFiresForAReliableReaderAgainstABestEffortWriter) {
    DocumentParts best_effort_writer;
    best_effort_writer.writer_qos = R"(
        <durability><kind>TRANSIENT_LOCAL</kind></durability>
        <reliability><kind>BEST_EFFORT</kind></reliability>)";

    struct IncompatibleListener : FastDDSStatusListener {
        std::mutex m;
        std::condition_variable cv;
        bool reader_incompatible = false;

        void OnIncompatibleQos(Endpoint endpoint, uint32_t /*policy_id*/,
                               uint32_t /*total_count*/) noexcept override {
            if (endpoint.is_writer || endpoint.topic != "incompatible/x") return;
            std::lock_guard<std::mutex> lk(m);
            reader_incompatible = true;
            cv.notify_all();
        }
    };

    IncompatibleListener listener;
    // Both providers share the document (Fast DDS profile names are process-wide): writer_qos
    // only touches the writer role, so sub_provider's reader still resolves to the default
    // RELIABLE reader profile, which is what makes the two incompatible.
    FastDDSPubSubProvider pub_provider(BoundedConfig(best_effort_writer));
    FastDDSPubSubProvider sub_provider(BoundedConfig(best_effort_writer), &listener);

    pub_provider.CreateTopic({"incompatible", "x"}, MakeSchema());
    SubscriptionResult result = sub_provider.Subscribe(
        {"incompatible", "x"},
        [](const uint8_t*, size_t, const SharedSchema&, const Attachments&) {});
    ASSERT_TRUE(AwaitSchema(result, std::chrono::seconds(5)));

    pub_provider.Publish({"incompatible", "x"}, MakeEncoder(1));

    std::unique_lock<std::mutex> lk(listener.m);
    EXPECT_TRUE(listener.cv.wait_for(lk, std::chrono::seconds(5), [&] {
        return listener.reader_incompatible;
    })) << "OnIncompatibleQos was never reported for the RELIABLE reader";
}

// The mirror of DiscoverySeesAWriterOnAnotherBound above: this time the LISTENER sits on the
// publisher side, and it is the SUBSCRIBER whose bound differs, so discovery must report the
// remote READER rather than the remote writer.
TEST(FastDDSStatusListenerTest, OnReaderDiscoveredMirrorsOnWriterDiscovered) {
    struct DiscoveryListener : FastDDSStatusListener {
        std::mutex m;
        std::condition_variable cv;
        std::vector<std::pair<std::string, std::string>> readers;

        void OnReaderDiscovered(std::string_view topic, std::string_view type_name,
                                bool alive) noexcept override {
            if (!alive) return;
            std::lock_guard<std::mutex> lk(m);
            readers.emplace_back(std::string(topic), std::string(type_name));
            cv.notify_all();
        }

        bool SawReader(const std::string& topic, const std::string& type_name) const {
            for (const auto& r : readers) {
                if (r.first == topic && r.second == type_name) return true;
            }
            return false;
        }
    };

    DiscoveryListener listener;
    FastDDSPubSubProvider pub_provider(ProviderConfig{}, &listener);

    ProviderConfig sub_config;
    sub_config.max_payload_bytes = kPayloadBytes<8192>;
    FastDDSPubSubProvider sub_provider(sub_config);

    pub_provider.CreateTopic({"readerbound", "x"}, MakeSchema());
    SubscriptionResult result = sub_provider.Subscribe(
        {"readerbound", "x"},
        [](const uint8_t*, size_t, const SharedSchema&, const Attachments&) {});
    pub_provider.Publish({"readerbound", "x"}, MakeEncoder(1));

    {
        std::unique_lock<std::mutex> lk(listener.m);
        ASSERT_TRUE(listener.cv.wait_for(lk, std::chrono::seconds(5), [&] {
            return listener.SawReader("readerbound/x", FletcherTypeName(8192));
        })) << "the reader on the other bound was never discovered";
    }

    // The companion channel pairs up regardless (same reasoning as
    // DiscoverySeesAWriterOnAnotherBound).
    ASSERT_TRUE(AwaitSchema(result, std::chrono::seconds(5)));
}

// OnParticipantDiscovered fires for a remote participant joining the same domain, independent of
// any topic or endpoint — the widest of the three discovery callbacks.
TEST(FastDDSStatusListenerTest, OnParticipantDiscoveredFiresForARemoteParticipant) {
    struct ParticipantListener : FastDDSStatusListener {
        std::mutex m;
        std::condition_variable cv;
        std::vector<std::string> names;

        void OnParticipantDiscovered(std::string_view name, bool alive) noexcept override {
            if (!alive) return;
            std::lock_guard<std::mutex> lk(m);
            names.emplace_back(name);
            cv.notify_all();
        }
    };

    ParticipantListener listener;
    FastDDSPubSubProvider provider(ProviderConfig{}, &listener);

    DomainParticipant* other =
        DomainParticipantFactory::get_instance()->create_participant(0, PARTICIPANT_QOS_DEFAULT);
    ASSERT_NE(other, nullptr);

    {
        std::unique_lock<std::mutex> lk(listener.m);
        EXPECT_TRUE(listener.cv.wait_for(lk, std::chrono::seconds(5), [&] {
            return !listener.names.empty();
        })) << "no remote participant was ever discovered";
    }

    DomainParticipantFactory::get_instance()->delete_participant(other);
}

// item A (2026-09-14): the schema reader's own subscription_matched now reaches OnMatched with
// is_schema_channel == true -- previously unreachable (see the section comment above). Subscribe
// alone is enough to create and enable the schema reader (EnsureSchemaChannel); no data need flow.
TEST(FastDDSStatusListenerTest, OnMatchedFiresForASchemaReadersOwnCondition) {
    struct SchemaMatchListener : FastDDSStatusListener {
        std::mutex m;
        std::condition_variable cv;
        bool schema_reader_matched = false;

        void OnMatched(Endpoint endpoint, int32_t /*current_count*/,
                       int32_t change) noexcept override {
            if (change <= 0 || endpoint.is_writer || !endpoint.is_schema_channel) return;
            std::lock_guard<std::mutex> lk(m);
            schema_reader_matched = true;
            cv.notify_all();
        }
    };

    SchemaMatchListener listener;
    FastDDSPubSubProvider pub_provider(ProviderConfig{});
    FastDDSPubSubProvider sub_provider(ProviderConfig{}, &listener);

    pub_provider.CreateTopic({"schemamatch", "x"}, MakeSchema());
    SubscriptionResult result = sub_provider.Subscribe(
        {"schemamatch", "x"},
        [](const uint8_t*, size_t, const SharedSchema&, const Attachments&) {});
    ASSERT_TRUE(AwaitSchema(result, std::chrono::seconds(5)));

    std::unique_lock<std::mutex> lk(listener.m);
    EXPECT_TRUE(listener.cv.wait_for(lk, std::chrono::seconds(5), [&] {
        return listener.schema_reader_matched;
    })) << "the schema reader's own subscription_matched never reached OnMatched";
}

// ---------------------------------------------------------------------------
// Tests — SubscribeSchema / UnsubscribeSchema: the schema without the data.
//
// The watch IS the companion `__schema` channel with no data reader beside it, so these read the
// same `SchemaArrival` a `Subscribe` hands back — `Wait(0ms)` for a poll, a budget for a wait, and
// the TYPED outcome (kOk / kPending / kSubscriptionEnded) as the assertion.
// ---------------------------------------------------------------------------

// The one waiting mechanism again, for an arrival that arrives on its own rather than inside a
// SubscriptionResult.
static SharedSchema AwaitWatch(const SchemaArrival& arrival, std::chrono::milliseconds budget) {
    SharedSchema schema;
    EXPECT_EQ(arrival.Wait(budget, &schema), PubSubStatus::kOk)
        << "schema arrival: " << arrival.Message();
    return schema;
}

// The proof the feature exists for: a watch pairs with the publisher's `__schema` writer and never
// with its data writer. The positive half is the schema arriving at all — nothing else can deliver
// it — and the negative half is the publisher's own OnMatched staying silent for the data topic:
// `WriterMatchListener` below filters both `is_writer` and `is_schema_channel` itself, since (item
// A) every reader's StatusCondition -- the schema reader's included -- now reports every status it
// has, not only the ones an old, narrower mask allowed through.
TEST(FastDDSPubSubProviderTest, SchemaWatchResolvesWithoutADataReader) {
    struct WriterMatchListener : FastDDSStatusListener {
        std::mutex m;
        std::vector<std::string> data_writer_matches;

        void OnMatched(Endpoint endpoint, int32_t, int32_t change) noexcept override {
            if (change <= 0 || !endpoint.is_writer || endpoint.is_schema_channel) return;
            {
                std::lock_guard<std::mutex> lk(m);
                data_writer_matches.emplace_back(endpoint.topic);
            }
            NotifyWaiters();
        }
    };

    // Declared before the providers: calls arrive until the last of them is destroyed.
    WriterMatchListener pub_listener;
    FastDDSPubSubProvider pub(ProviderConfig{}, &pub_listener);
    FastDDSPubSubProvider sub(ProviderConfig{});

    pub.CreateTopic({"schemaonly", "x"}, MakeSchema());
    pub.Publish({"schemaonly", "x"}, MakeEncoder(1));  // creates the data writer

    SchemaArrival watch = sub.SubscribeSchema({"schemaonly", "x"});
    SharedSchema schema = AwaitWatch(watch, std::chrono::seconds(5));
    ASSERT_TRUE(schema);
    ASSERT_EQ(schema->n_children, 1);
    EXPECT_STREQ(schema->children[0]->name, "x");

    // Discovery fence, not a settle: a REAL data reader/writer pair on a fresh topic through the
    // same pub/pub_listener. Its OnMatched rides the same per-participant status-dispatch path as
    // any match for "schemaonly/x" would have, strictly after it (created here, after the schema
    // above already resolved) -- so seeing it proves the earlier negative already held.
    SubscriptionResult fence_result =
        sub.Subscribe({"schemaonly", "fence"},
                      [](const uint8_t*, size_t, const SharedSchema&, const Attachments&) {});
    pub.CreateTopic({"schemaonly", "fence"}, MakeSchema());
    pub.Publish({"schemaonly", "fence"}, MakeEncoder(1));
    ASSERT_TRUE(AwaitSchema(fence_result, std::chrono::seconds(5)));
    ASSERT_TRUE(WaitUntil(
        [&] {
            std::lock_guard<std::mutex> lk(pub_listener.m);
            for (const auto& topic : pub_listener.data_writer_matches) {
                if (topic == "schemaonly/fence") return true;
            }
            return false;
        },
        std::chrono::seconds(5)))
        << "the fence data writer never matched";

    {
        std::lock_guard<std::mutex> lk(pub_listener.m);
        EXPECT_THAT(pub_listener.data_writer_matches,
                    testing::Not(testing::Contains("schemaonly/x")))
            << "a schema-only watch created a data reader";
    }

    sub.Unsubscribe({"schemaonly", "fence"});
    sub.UnsubscribeSchema({"schemaonly", "x"});
}

TEST(FastDDSPubSubProviderTest, SubscribeAfterSchemaWatchReusesTheSchema) {
    FastDDSPubSubProvider pub(ProviderConfig{});
    FastDDSPubSubProvider sub(ProviderConfig{});
    pub.CreateTopic({"watchthen", "sub"}, MakeSchema());

    SchemaArrival watch = sub.SubscribeSchema({"watchthen", "sub"});
    SharedSchema s = AwaitWatch(watch, std::chrono::seconds(5));
    ASSERT_TRUE(s);
    // Idempotent: a second watch is the same channel, so the same schema object.
    EXPECT_EQ(AwaitWatch(sub.SubscribeSchema({"watchthen", "sub"}), std::chrono::seconds(0)).get(),
              s.get());

    std::mutex mu;
    std::condition_variable cv;
    std::atomic<int32_t> received{-1};
    int deliveries = 0;
    SharedSchema rx_schema;
    SubscriptionResult result = sub.Subscribe(
        {"watchthen", "sub"},
        [&](const uint8_t* data, size_t len, const SharedSchema& schema, const Attachments&) {
            std::lock_guard<std::mutex> lk(mu);
            ++deliveries;
            rx_schema = schema;
            if (len >= 5) received.store(DecodeRow(data));
            cv.notify_all();
        });
    // Same channel, same schema object — no second fetch, and the arrival is already resolved
    // when Subscribe returns rather than pending.
    EXPECT_EQ(AwaitSchema(result, std::chrono::milliseconds(0)).get(), s.get());

    pub.Publish({"watchthen", "sub"}, MakeEncoder(7));
    {
        std::unique_lock<std::mutex> lk(mu);
        ASSERT_TRUE(
            cv.wait_for(lk, std::chrono::seconds(5), [&] { return received.load() != -1; }));
        EXPECT_EQ(received.load(), 7);
        EXPECT_EQ(deliveries, 1);
        // The data listener started with the watched schema: no handoff, no copy.
        EXPECT_EQ(rx_schema.get(), s.get());
    }

    sub.Unsubscribe({"watchthen", "sub"});
    sub.UnsubscribeSchema({"watchthen", "sub"});
}

TEST(FastDDSPubSubProviderTest, SchemaWatchBeforeAnyPublisherResolvesWhenOneAppears) {
    FastDDSPubSubProvider sub(ProviderConfig{});

    SchemaArrival watch = sub.SubscribeSchema({"watchfirst", "x"});
    SharedSchema polled;
    EXPECT_EQ(watch.Wait(std::chrono::milliseconds(0), &polled), PubSubStatus::kPending);
    EXPECT_EQ(polled, nullptr) << "*out is untouched unless the answer is kOk";

    FastDDSPubSubProvider pub(ProviderConfig{});
    pub.CreateTopic({"watchfirst", "x"}, MakeSchema());

    SharedSchema schema = AwaitWatch(watch, std::chrono::seconds(5));
    ASSERT_TRUE(schema);
    EXPECT_EQ(schema->n_children, 1);

    sub.UnsubscribeSchema({"watchfirst", "x"});
}

TEST(FastDDSPubSubProviderTest, UnsubscribeSchemaBreaksAPendingWatch) {
    FastDDSPubSubProvider sub(ProviderConfig{});

    SchemaArrival watch = sub.SubscribeSchema({"watch", "broken"});
    sub.UnsubscribeSchema({"watch", "broken"});

    // kSubscriptionEnded, not a failure: the watch was released, which is the caller's own doing
    // and nothing a retry would cure.
    SharedSchema polled;
    EXPECT_EQ(watch.Wait(std::chrono::seconds(5), &polled), PubSubStatus::kSubscriptionEnded);
    EXPECT_EQ(polled, nullptr);

    // A topic this provider has never seen: nothing to release, nothing thrown.
    EXPECT_NO_THROW(sub.UnsubscribeSchema({"never", "seen"}));
}

TEST(FastDDSPubSubProviderTest, UnsubscribeSchemaLeavesALiveDataSubscriptionAlone) {
    FastDDSPubSubProvider sub(ProviderConfig{});

    std::mutex mu;
    std::condition_variable cv;
    std::atomic<int32_t> received{-1};
    SharedSchema rx_schema;
    SubscriptionResult result = sub.Subscribe(
        {"keep", "data"},
        [&](const uint8_t* data, size_t len, const SharedSchema& schema, const Attachments&) {
            std::lock_guard<std::mutex> lk(mu);
            rx_schema = schema;
            if (len >= 5) received.store(DecodeRow(data));
            cv.notify_all();
        });

    // A watch on the same topic shares the data subscription's channel. Releasing it must not
    // take that channel down: the data side is live and its arrival stays pending.
    SchemaArrival watch = sub.SubscribeSchema({"keep", "data"});
    sub.UnsubscribeSchema({"keep", "data"});
    SharedSchema early;
    EXPECT_EQ(watch.Wait(std::chrono::milliseconds(0), &early), PubSubStatus::kPending)
        << "releasing the watch ended the arrival a live data subscription still owns";

    DataWriterMatchListener pub_listener;
    FastDDSPubSubProvider pub(ProviderConfig{}, &pub_listener);
    pub.CreateTopic({"keep", "data"}, MakeSchema());
    ASSERT_TRUE(pub_listener.AwaitMatch())
        << "the data writer never matched the subscriber's data reader";
    pub.Publish({"keep", "data"}, MakeEncoder(5));

    ASSERT_TRUE(AwaitSchema(result, std::chrono::seconds(5)));
    // The same arrival, seen through the released watch's copy, resolved with the data side.
    SharedSchema via_watch;
    EXPECT_EQ(watch.Wait(std::chrono::milliseconds(0), &via_watch), PubSubStatus::kOk);
    EXPECT_TRUE(via_watch);
    {
        std::unique_lock<std::mutex> lk(mu);
        ASSERT_TRUE(
            cv.wait_for(lk, std::chrono::seconds(5), [&] { return received.load() != -1; }));
        EXPECT_EQ(received.load(), 5);
        ASSERT_TRUE(rx_schema);
    }

    sub.Unsubscribe({"keep", "data"});
}

// A catalog's watch has to survive the Unsubscribe of a data subscription it knows nothing about —
// here with the schema still pending, so the kept channel gets a fresh `__schema` reader.
TEST(FastDDSPubSubProviderTest, UnsubscribeKeepsAPendingSchemaWatch) {
    FastDDSPubSubProvider sub(ProviderConfig{});

    SchemaArrival watch = sub.SubscribeSchema({"keepwatch", "pending"});
    SharedSchema polled;
    ASSERT_EQ(watch.Wait(std::chrono::milliseconds(0), &polled), PubSubStatus::kPending);

    static_cast<void>(
        sub.Subscribe({"keepwatch", "pending"},
                      [](const uint8_t*, size_t, const SharedSchema&, const Attachments&) {}));
    sub.Unsubscribe({"keepwatch", "pending"});

    // Not ended by that Unsubscribe: the publisher appears only now.
    ASSERT_EQ(watch.Wait(std::chrono::milliseconds(0), &polled), PubSubStatus::kPending);

    FastDDSPubSubProvider pub(ProviderConfig{});
    pub.CreateTopic({"keepwatch", "pending"}, MakeSchema());

    SharedSchema schema = AwaitWatch(watch, std::chrono::seconds(5));
    ASSERT_TRUE(schema);
    EXPECT_EQ(schema->n_children, 1);

    sub.UnsubscribeSchema({"keepwatch", "pending"});
}

TEST(FastDDSPubSubProviderTest, UnsubscribeKeepsAResolvedSchemaWatch) {
    FastDDSPubSubProvider pub(ProviderConfig{});
    FastDDSPubSubProvider sub(ProviderConfig{});
    pub.CreateTopic({"keepwatch", "resolved"}, MakeSchema());

    SchemaArrival watch = sub.SubscribeSchema({"keepwatch", "resolved"});
    SharedSchema schema = AwaitWatch(watch, std::chrono::seconds(5));
    ASSERT_TRUE(schema);

    static_cast<void>(
        sub.Subscribe({"keepwatch", "resolved"},
                      [](const uint8_t*, size_t, const SharedSchema&, const Attachments&) {}));
    sub.Unsubscribe({"keepwatch", "resolved"});

    // The channel stayed with the watch, so the schema is still there — the same object, with no
    // re-fetch and no new reader (a resolved channel ignores a second Resolve). That call is its
    // own watch under the count (P1), not a free peek, so it is balanced by its own release right
    // after — otherwise the ONE UnsubscribeSchema below would not be the last one out.
    EXPECT_EQ(
        AwaitWatch(sub.SubscribeSchema({"keepwatch", "resolved"}), std::chrono::seconds(0)).get(),
        schema.get());
    sub.UnsubscribeSchema({"keepwatch", "resolved"});

    // UnsubscribeSchema is what ends it: the next watch is a new channel with its own schema.
    sub.UnsubscribeSchema({"keepwatch", "resolved"});
    SharedSchema again =
        AwaitWatch(sub.SubscribeSchema({"keepwatch", "resolved"}), std::chrono::seconds(5));
    ASSERT_TRUE(again);
    EXPECT_NE(again.get(), schema.get());

    sub.UnsubscribeSchema({"keepwatch", "resolved"});
}

// A watch on a topic THIS provider published resolves from its own schema with no __schema reader
// behind it. Releasing it must not touch a reader that does not exist, and a second watch must
// resolve the same way.
TEST(FastDDSPubSubProviderTest, SubscribeSchemaOnASelfPublishedTopicReleasesCleanly) {
    FastDDSPubSubProvider provider(ProviderConfig{});
    provider.CreateTopic({"selfwatch", "x"}, MakeSchema());

    SchemaArrival first = provider.SubscribeSchema({"selfwatch", "x"});
    SharedSchema schema;
    ASSERT_EQ(first.Wait(std::chrono::milliseconds(0), &schema), PubSubStatus::kOk);
    ASSERT_TRUE(schema);
    EXPECT_EQ(schema->n_children, 1);
    EXPECT_NO_THROW(provider.UnsubscribeSchema({"selfwatch", "x"}));

    SchemaArrival second = provider.SubscribeSchema({"selfwatch", "x"});
    SharedSchema again;
    ASSERT_EQ(second.Wait(std::chrono::milliseconds(0), &again), PubSubStatus::kOk);
    ASSERT_TRUE(again);
    EXPECT_NO_THROW(provider.UnsubscribeSchema({"selfwatch", "x"}));
}

// The watch count: two watchers on the same topic share one channel, and releasing one of them
// must not end the arrival the other is still holding — that is what a bool `schema_watch` flag
// could not tell apart from a single watcher's own release.
TEST(FastDDSPubSubProviderTest, TwoWatchersOneRelease) {
    FastDDSPubSubProvider sub(ProviderConfig{});

    SchemaArrival first = sub.SubscribeSchema({"twowatchers", "release"});
    SchemaArrival second = sub.SubscribeSchema({"twowatchers", "release"});

    SharedSchema polled;
    EXPECT_EQ(first.Wait(std::chrono::milliseconds(0), &polled), PubSubStatus::kPending);
    EXPECT_EQ(second.Wait(std::chrono::milliseconds(0), &polled), PubSubStatus::kPending);

    sub.UnsubscribeSchema({"twowatchers", "release"});
    EXPECT_EQ(first.Wait(std::chrono::milliseconds(0), &polled), PubSubStatus::kPending)
        << "one release must not end a watch a second caller is still holding";

    sub.UnsubscribeSchema({"twowatchers", "release"});
    EXPECT_EQ(first.Wait(std::chrono::milliseconds(0), &polled), PubSubStatus::kSubscriptionEnded);
}

// P2 — the channel never leaves its topic slot: a Subscribe racing an Unsubscribe that keeps a
// watch must never find `schema_reader` and `received_schema` both null and open a second one
// through EnsureSchemaChannel, replacing the watch's own arrival with a fresh
// `SchemaArrival::Create()` pair -- the watch's own arrival must survive every one of these cycles
// undisturbed.
TEST(FastDDSPubSubProviderTest, SubscribeRacingUnsubscribeKeepsTheWatch) {
    FastDDSPubSubProvider sub(ProviderConfig{});
    const std::vector<std::string> t = {"racing", "watch"};
    auto noop = [](const uint8_t*, size_t, const SharedSchema&, const Attachments&) {};

    SchemaArrival watch = sub.SubscribeSchema(t);

    for (int i = 0; i < 20; ++i) {
        static_cast<void>(sub.Subscribe(t, noop));

        // -1: no exception; otherwise the PubSubStatus thread B's own attempt saw.
        std::atomic<int32_t> b_status{-1};
        bool a_joined_by_b = false;
        std::thread a([&] { sub.Unsubscribe(t); });
        std::thread b([&] {
            try {
                static_cast<void>(sub.Subscribe(t, noop));
            } catch (const PubSubError& e) {
                // Lost the race: A had not cleared `ts.reader` yet. Wait for A to finish — the
                // retry is then guaranteed to find the topic unsubscribed — and try once more.
                b_status.store(static_cast<int32_t>(e.status()));
                a.join();
                a_joined_by_b = true;
                static_cast<void>(sub.Subscribe(t, noop));
            }
        });
        b.join();
        if (!a_joined_by_b) a.join();

        if (b_status.load() != -1) {
            EXPECT_EQ(b_status.load(), static_cast<int32_t>(PubSubStatus::kInvalidArgument))
                << "iteration " << i;
        }

        sub.Unsubscribe(t);
    }

    SharedSchema polled;
    EXPECT_EQ(watch.Wait(std::chrono::milliseconds(0), &polled), PubSubStatus::kPending)
        << "twenty racing Subscribe/Unsubscribe cycles must not have ended the watch";

    FastDDSPubSubProvider pub(ProviderConfig{});
    pub.CreateTopic(t, MakeSchema());

    SharedSchema schema;
    EXPECT_EQ(watch.Wait(std::chrono::seconds(5), &schema), PubSubStatus::kOk);
    ASSERT_TRUE(schema);

    sub.UnsubscribeSchema(t);
}

// The door, on the two new methods: refused by name from inside a delivery on this instance, like
// the four data-path methods. Not decoration — SubscribeSchema can reach create_datareader and
// UnsubscribeSchema can reach delete_datareader, and both hang if let through on a Fast DDS
// listener thread.
TEST(FastDDSPubSubProviderTest, SchemaWatchIsRefusedFromInsideADelivery) {
    DataWriterMatchListener pub_listener;
    FastDDSPubSubProvider pub(ProviderConfig{}, &pub_listener);
    FastDDSPubSubProvider sub(ProviderConfig{});
    pub.CreateTopic({"reentrant", "watch"}, MakeSchema());

    std::mutex mu;
    std::condition_variable cv;
    std::atomic<int32_t> subscribe_refusal{-1};
    std::atomic<int32_t> unsubscribe_refusal{-1};

    SubscriptionResult result =
        sub.Subscribe({"reentrant", "watch"},
                      [&](const uint8_t*, size_t, const SharedSchema&, const Attachments&) {
                          try {
                              static_cast<void>(sub.SubscribeSchema({"reentrant", "watch"}));
                          } catch (const PubSubError& e) {
                              subscribe_refusal.store(static_cast<int32_t>(e.status()));
                          }
                          try {
                              sub.UnsubscribeSchema({"reentrant", "watch"});
                          } catch (const PubSubError& e) {
                              unsubscribe_refusal.store(static_cast<int32_t>(e.status()));
                          }
                          std::lock_guard<std::mutex> lk(mu);
                          cv.notify_all();
                      });
    ASSERT_TRUE(AwaitSchema(result, std::chrono::seconds(5)));

    ASSERT_TRUE(pub_listener.AwaitMatch())
        << "the data writer never matched the subscriber's data reader";
    pub.Publish({"reentrant", "watch"}, MakeEncoder(1));
    {
        std::unique_lock<std::mutex> lk(mu);
        ASSERT_TRUE(cv.wait_for(lk, std::chrono::seconds(5), [&] {
            return subscribe_refusal.load() != -1 && unsubscribe_refusal.load() != -1;
        })) << "the delivery never ran, or neither call was answered";
    }
    EXPECT_EQ(subscribe_refusal.load(), static_cast<int32_t>(PubSubStatus::kReentrantCall));
    EXPECT_EQ(unsubscribe_refusal.load(), static_cast<int32_t>(PubSubStatus::kReentrantCall));

    sub.Unsubscribe({"reentrant", "watch"});
}

// P5 — an undecodable __schema sample fails the arrival outright rather than leaving it pending
// forever: a raw Fast DDS writer, entirely outside this provider, announces garbage bytes on the
// companion channel, and a watch on that topic must come back with a diagnostic status instead of
// hanging.
TEST(FastDDSPubSubProviderTest, UndecodableSchemaSampleFailsTheArrival) {
    const std::vector<std::string> t = {"undecodable", "schema"};
    const std::string joined = "undecodable/schema";

    DomainParticipant* raw_participant =
        DomainParticipantFactory::get_instance()->create_participant(0, PARTICIPANT_QOS_DEFAULT);
    ASSERT_NE(raw_participant, nullptr);

    TypeSupport raw_type_support(new internal::SchemaBytesPubSubType(65536));
    ASSERT_EQ(raw_type_support.register_type(raw_participant), RETCODE_OK);

    Topic* schema_topic = raw_participant->create_topic(
        joined + "/__schema", raw_type_support.get_type_name(), TOPIC_QOS_DEFAULT);
    ASSERT_NE(schema_topic, nullptr);

    Publisher* raw_publisher = raw_participant->create_publisher(PUBLISHER_QOS_DEFAULT);
    ASSERT_NE(raw_publisher, nullptr);
    DataWriter* raw_writer =
        raw_publisher->create_datawriter(schema_topic, internal::MakeSchemaChannelWriterQos());
    ASSERT_NE(raw_writer, nullptr);

    // The row rides the ordinary envelope -- same as any other schema announcement -- but its
    // bytes are not a valid Arrow IPC stream, which is what must fail the arrival.
    const std::vector<uint8_t> garbage_row = {0xde, 0xad, 0xbe, 0xef, 1, 2, 3};
    const PubSubProvider::RowEncoder encoder = [&garbage_row](WriteBuffer& b) {
        b.Append(garbage_row.data(), garbage_row.size());
    };
    const Attachments none;
    internal::PublishData transport;
    transport.encoder = &encoder;
    transport.attachments = &none;
    ASSERT_EQ(raw_writer->write(&transport), RETCODE_OK);

    FastDDSPubSubProvider sub(ProviderConfig{});
    SchemaArrival watch = sub.SubscribeSchema(t);

    SharedSchema schema;
    const PubSubStatus status = watch.Wait(std::chrono::seconds(5), &schema);
    EXPECT_NE(status, PubSubStatus::kOk);
    EXPECT_NE(status, PubSubStatus::kPending);
    EXPECT_NE(status, PubSubStatus::kSubscriptionEnded);
    EXPECT_EQ(status, PubSubStatus::kInternal);
    EXPECT_FALSE(watch.Message().empty());

    sub.UnsubscribeSchema(t);
    raw_publisher->delete_datawriter(raw_writer);
    raw_participant->delete_publisher(raw_publisher);
    raw_participant->delete_topic(schema_topic);
    DomainParticipantFactory::get_instance()->delete_participant(raw_participant);
}

// P6 — a conflicting later announcement from a second publisher is logged rather than silently
// swallowed: this is the cross-process half of the seam's conflict refusal
// (CreateTopicRejectsConflictingSchema above is the local, same-instance half, which throws
// instead — there is no local caller here to throw at).
namespace {

class SchemaConflictLogConsumer : public eprosima::fastdds::dds::LogConsumer {
   public:
    void Consume(const eprosima::fastdds::dds::Log::Entry& entry) override {
        if (entry.message.find("different schema") == std::string::npos) return;
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

TEST(FastDDSPubSubProviderTest, ConflictingCrossProviderSchemaIsLoggedNotSwallowed) {
    // Not Log::ClearConsumers: that would drop the default stdout consumer along with it.
    // RegisterConsumer only adds one, so the test binary's stderr gets one harmless extra line.
    auto* consumer = new SchemaConflictLogConsumer();
    eprosima::fastdds::dds::Log::RegisterConsumer(
        std::unique_ptr<eprosima::fastdds::dds::LogConsumer>(consumer));

    FastDDSPubSubProvider a(ProviderConfig{});
    FastDDSPubSubProvider c(ProviderConfig{});
    const std::vector<std::string> t = {"schemaconflict", "x"};
    a.CreateTopic(t, MakeSchema());

    SchemaArrival watch = c.SubscribeSchema(t);
    SharedSchema schema = AwaitWatch(watch, std::chrono::seconds(5));
    ASSERT_TRUE(schema);

    // B declares only NOW, strictly after C already resolved from A: both orders are otherwise
    // possible (a fan-in from A and B racing each other), and only serialising B after C's
    // resolution makes B's announcement unambiguously the LATER, conflicting one.
    FastDDSPubSubProvider b(ProviderConfig{});
    b.CreateTopic(t, MakeOtherSchema());

    // Not a poll: the "different schema" entry is queued by a background DDS thread once it
    // processes B's conflicting announcement, and Log's own QueueLog (Log.cpp) drains the queue as
    // soon as that happens -- no repeated Flush() from this thread is needed to trigger it, only
    // Consume()'s NotifyWaiters() call below to wake this wait.
    eprosima::fastdds::dds::Log::Flush();
    WaitUntil(
        [&] {
            std::lock_guard<std::mutex> lk(consumer->m);
            return consumer->count >= 1;
        },
        std::chrono::seconds(5));

    eprosima::fastdds::dds::Log::Flush();
    std::lock_guard<std::mutex> lk(consumer->m);
    EXPECT_GE(consumer->count, 1)
        << "a conflicting cross-process schema announcement was not logged";

    c.UnsubscribeSchema(t);
}
