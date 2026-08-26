// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fletcher/core/write_buffer.hpp>
#include <fletcher/fastdds_pubsub_provider/fast_dds_pubsub_provider.hpp>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include "internal/data_reader_listener.hpp"
#include "internal/ordered_delivery.hpp"

using namespace fletcher;
using namespace eprosima::fastdds::dds;

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
// Tests — basic provider behaviour
// ---------------------------------------------------------------------------

TEST(FastDDSPubSubProviderTest, ConstructDestruct) {
    EXPECT_NO_THROW({ FastDDSPubSubProvider p(FastDDSProviderOptions{}); });
}

TEST(FastDDSPubSubProviderTest, CreateTopicSucceeds) {
    FastDDSPubSubProvider p(FastDDSProviderOptions{});
    EXPECT_NO_THROW(p.CreateTopic({"create", "ok"}, MakeSchema()));
}

TEST(FastDDSPubSubProviderTest, CreateTopicIsIdempotent) {
    // CreateTopic mirrors the in-process reference provider: declaring an
    // already-existing topic is a no-op, not an error. This lets a publisher
    // attach to a topic a subscriber created first (subscriber-first) and
    // makes repeated declarations harmless.
    FastDDSPubSubProvider p(FastDDSProviderOptions{});
    p.CreateTopic({"create", "dup"}, MakeSchema());
    EXPECT_NO_THROW(p.CreateTopic({"create", "dup"}, MakeSchema()));
}

TEST(FastDDSPubSubProviderTest, CreateTopicRejectsConflictingSchema) {
    // Idempotent re-declaration is fine, but declaring an existing topic with a
    // *different* schema is a genuine conflict and must not be silently dropped.
    FastDDSPubSubProvider p(FastDDSProviderOptions{});
    p.CreateTopic({"create", "conflict"}, MakeSchema());
    EXPECT_THROW(p.CreateTopic({"create", "conflict"}, MakeOtherSchema()), std::runtime_error);
}

TEST(FastDDSPubSubProviderTest, PublishWithoutSubscriberDoesNotThrow) {
    FastDDSPubSubProvider p(FastDDSProviderOptions{});
    p.CreateTopic({"pub", "nosub"}, MakeSchema());
    EXPECT_NO_THROW(p.Publish({"pub", "nosub"}, MakeEncoder(1)));
}

TEST(FastDDSPubSubProviderTest, RoundTripPublishSubscribe) {
    FastDDSPubSubProvider pub_provider(FastDDSProviderOptions{});
    FastDDSPubSubProvider sub_provider(FastDDSProviderOptions{});

    pub_provider.CreateTopic({"roundtrip", "x"}, MakeSchema());

    std::atomic<int32_t> received{-1};
    SubscriptionResult result = sub_provider.Subscribe(
        {"roundtrip", "x"},
        [&](const uint8_t* data, size_t len, const SharedSchema&, const Attachments&) {
            if (len >= 5) received.store(DecodeRow(data));
        });

    ASSERT_TRUE(result.schema.valid());
    SharedSchema sch = result.schema.get();
    ASSERT_TRUE(sch);
    ASSERT_EQ(sch->n_children, 1);
    EXPECT_EQ(std::string(sch->children[0]->name), "x");
    EXPECT_EQ(std::string(sch->children[0]->format), "i");

    pub_provider.Publish({"roundtrip", "x"}, MakeEncoder(42));

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (received.load() == -1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    EXPECT_EQ(received.load(), 42);
}

// ---------------------------------------------------------------------------
// Tests — loans / data-sharing
// ---------------------------------------------------------------------------

// The sample type is bounded and plain, so every endpoint reserves the whole payload bound per
// history slot — which is what the resource limits here keep in check. The default memory policy
// preallocates, so a reader built from these reads through loans; LoanPublishOptions() makes the
// publish side loan too.
static FastDDSProviderOptions BoundedOptions() {
    FastDDSProviderOptions opts;
    opts.default_writer_qos.history().kind = KEEP_LAST_HISTORY_QOS;
    opts.default_writer_qos.history().depth = 10;
    opts.default_writer_qos.resource_limits().max_samples = 10;
    opts.default_writer_qos.resource_limits().allocated_samples = 10;
    opts.default_reader_qos.history().kind = KEEP_LAST_HISTORY_QOS;
    opts.default_reader_qos.history().depth = 10;
    opts.default_reader_qos.resource_limits().max_samples = 10;
    opts.default_reader_qos.resource_limits().allocated_samples = 10;
    return opts;
}

static FastDDSProviderOptions LoanPublishOptions() {
    FastDDSProviderOptions opts = BoundedOptions();
    opts.loan_publish = true;
    return opts;
}

// The bound is a runtime option, and it is taken as given — PayloadBytes() is what a row has to fit
// and what the registered type name carries.
TEST(FastDDSPubSubProviderTest, ThePayloadBoundIsWhatWasAskedFor) {
    FastDDSProviderOptions opts = BoundedOptions();
    opts.max_payload_bytes = kPayloadBytes<128 * 1024>;
    FastDDSPubSubProvider provider(opts);
    EXPECT_EQ(provider.PayloadBytes(), 128u * 1024);
}

// Nothing is rounded, and 4-byte alignment is the whole rule; these values cannot work.
TEST(FastDDSPubSubProviderTest, AnUnusablePayloadBoundIsRefused) {
    FastDDSProviderOptions opts = BoundedOptions();
    opts.max_payload_bytes = 100'001;  // not a multiple of 4, so the sample carries tail padding
    EXPECT_THROW(FastDDSPubSubProvider provider(opts), std::invalid_argument);

    opts.max_payload_bytes = 0;
    EXPECT_THROW(FastDDSPubSubProvider provider(opts), std::invalid_argument);

    // Past where a sample's own size still fits the uint32 Fast DDS reports it in.
    opts.max_payload_bytes = kMaxPayloadBytes + 4;
    EXPECT_THROW(FastDDSPubSubProvider provider(opts), std::invalid_argument);
}

// A bound that is not a power of two is ordinary.
TEST(FastDDSPubSubProviderTest, ABoundThatIsNotAPowerOfTwoIsFine) {
    FastDDSProviderOptions opts = BoundedOptions();
    opts.max_payload_bytes = kPayloadBytes<100'000>;
    FastDDSPubSubProvider provider(opts);
    EXPECT_EQ(provider.PayloadBytes(), 100'000u);
}

// Nothing caps a bound; a constructor allocates no pools, which are per endpoint.
TEST(FastDDSPubSubProviderTest, ALargeBoundIsFine) {
    FastDDSProviderOptions opts = BoundedOptions();
    opts.max_payload_bytes = kPayloadBytes<8 * 1024 * 1024>;
    EXPECT_NO_THROW(FastDDSPubSubProvider provider(opts));

    // Well past where the old compiled set stopped.
    FastDDSProviderOptions large = BoundedOptions();
    large.max_payload_bytes = kPayloadBytes<256 * 1024 * 1024>;
    EXPECT_NO_THROW(FastDDSPubSubProvider provider(large));

    // And with Fast DDS's own default max_samples behind it, which the old check rejected outright.
    FastDDSProviderOptions unbounded_limits = BoundedOptions();
    unbounded_limits.max_payload_bytes = kPayloadBytes<8 * 1024 * 1024>;
    unbounded_limits.default_writer_qos.resource_limits().max_samples = 5000;
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
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (received.load() == -1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return received.load();
}

// loan_publish: the writer encodes into a loaned payload and the reader reads the row out of the
// loan it takes, with no serialise/deserialise pair in between.
TEST(FastDDSPubSubProviderTest, LoanedRoundTrip) {
    FastDDSPubSubProvider pub_provider(LoanPublishOptions());
    FastDDSPubSubProvider sub_provider(BoundedOptions());

    pub_provider.CreateTopic({"loaned", "x"}, MakeSchema());

    std::atomic<int32_t> received{-1};
    SubscriptionResult result = sub_provider.Subscribe(
        {"loaned", "x"},
        [&](const uint8_t* data, size_t len, const SharedSchema&, const Attachments&) {
            if (len >= 5) received.store(DecodeRow(data));
        });
    ASSERT_TRUE(result.schema.get());

    pub_provider.Publish({"loaned", "x"}, MakeEncoder(42));

    EXPECT_EQ(AwaitRow(received), 42);
}

// A non-preallocating reader gets nodes sized to what arrived, so it reads through copies.
TEST(FastDDSPubSubProviderTest, DynamicMemoryReaderRoundTripsThroughCopies) {
    FastDDSProviderOptions sub_opts = BoundedOptions();
    sub_opts.default_reader_qos.endpoint().history_memory_policy =
        eprosima::fastdds::rtps::DYNAMIC_RESERVE_MEMORY_MODE;

    FastDDSPubSubProvider pub_provider(BoundedOptions());
    FastDDSPubSubProvider sub_provider(std::move(sub_opts));

    pub_provider.CreateTopic({"dynamic", "reader"}, MakeSchema());

    std::atomic<int32_t> received{-1};
    SubscriptionResult result = sub_provider.Subscribe(
        {"dynamic", "reader"},
        [&](const uint8_t* data, size_t len, const SharedSchema&, const Attachments&) {
            if (len >= 5) received.store(DecodeRow(data));
        });
    ASSERT_TRUE(result.schema.get());

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
    FastDDSPubSubProvider pub_provider(BoundedOptions());
    FastDDSPubSubProvider sub_provider(BoundedOptions());

    pub_provider.CreateTopic({"datasharing", "x"}, MakeSchema());

    std::atomic<int32_t> received{-1};
    SubscriptionResult result = sub_provider.Subscribe(
        {"datasharing", "x"},
        [&](const uint8_t* data, size_t len, const SharedSchema&, const Attachments&) {
            if (len >= 5) received.store(DecodeRow(data));
        });
    ASSERT_TRUE(result.schema.get());

    pub_provider.Publish({"datasharing", "x"}, MakeEncoder(31));

    EXPECT_EQ(AwaitRow(received), 31);
}

// Same DataSharingKind::ON check as for zero-copy: it passes only while the type
// declares itself bounded, which is all data-sharing needs. Being plain (loans)
// is a separate claim this mode does not make.
TEST(FastDDSPubSubProviderTest, DataSharingTypeIsAcceptedForDataSharing) {
    FastDDSProviderOptions pub_opts = BoundedOptions();
    pub_opts.default_writer_qos.data_sharing().on("");
    FastDDSProviderOptions sub_opts = BoundedOptions();
    sub_opts.default_reader_qos.data_sharing().on("");

    FastDDSPubSubProvider pub_provider(std::move(pub_opts));
    FastDDSPubSubProvider sub_provider(std::move(sub_opts));

    pub_provider.CreateTopic({"datasharing", "on"}, MakeSchema());

    std::atomic<int32_t> received{-1};
    SubscriptionResult result = sub_provider.Subscribe(
        {"datasharing", "on"},
        [&](const uint8_t* data, size_t len, const SharedSchema&, const Attachments&) {
            if (len >= 5) received.store(DecodeRow(data));
        });
    ASSERT_TRUE(result.schema.get());

    pub_provider.Publish({"datasharing", "on"}, MakeEncoder(29));

    EXPECT_EQ(AwaitRow(received), 29);
}

// The mirror of the zero-copy oversize test, and what separates the two modes:
// with no loan to encode into, the overflow happens inside serialize(), which
// reports it to Fast DDS instead of throwing out of Publish. The sample is
// dropped either way.
TEST(FastDDSPubSubProviderTest, DataSharingOversizedRowDoesNotThrow) {
    FastDDSPubSubProvider pub_provider(BoundedOptions());
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
    FastDDSProviderOptions pub_opts = BoundedOptions();
    pub_opts.default_writer_qos.data_sharing().on("");
    FastDDSProviderOptions sub_opts = BoundedOptions();
    sub_opts.default_reader_qos.data_sharing().on("");

    FastDDSPubSubProvider pub_provider(std::move(pub_opts));
    FastDDSPubSubProvider sub_provider(std::move(sub_opts));

    pub_provider.CreateTopic({"bounded", "datasharing"}, MakeSchema());

    std::atomic<int32_t> received{-1};
    SubscriptionResult result = sub_provider.Subscribe(
        {"bounded", "datasharing"},
        [&](const uint8_t* data, size_t len, const SharedSchema&, const Attachments&) {
            if (len >= 5) received.store(DecodeRow(data));
        });
    ASSERT_TRUE(result.schema.get());

    pub_provider.Publish({"bounded", "datasharing"}, MakeEncoder(23));

    EXPECT_EQ(AwaitRow(received), 23);
}

// A row that overruns the bound throws out of Publish, which only the loaned
// path does (the serialising path swallows the overflow inside serialize()) —
// so this is where the writer proves it loaned. Repeating it past the size of
// the loan pool also proves the failed attempt returned its loan: once loans
// leak, loan_sample starts failing and Publish stops throwing.
TEST(FastDDSPubSubProviderTest, LoanedOversizedRowThrowsWithoutLeakingLoans) {
    FastDDSPubSubProvider pub_provider(LoanPublishOptions());
    pub_provider.CreateTopic({"loaned", "oversized"}, MakeSchema());

    auto oversized = [bound = pub_provider.PayloadBytes()](WriteBuffer& buf) {
        std::vector<uint8_t> blob(bound + 16, 0x5A);
        buf.Append(blob.data(), blob.size());
    };
    for (int i = 0; i < 15; ++i) {
        EXPECT_THROW(pub_provider.Publish({"loaned", "oversized"}, oversized), std::overflow_error)
            << "attempt " << i;
    }

    EXPECT_NO_THROW(pub_provider.Publish({"loaned", "oversized"}, MakeEncoder(3)));
}

// A throwing callback runs on a Fast DDS listener thread, where an escaping exception terminates
// the process. On the loaned path it must also not take the loan with it: the reader has only
// max_samples + extra_samples loans, so a handful of leaks starves delivery for good.
TEST(FastDDSPubSubProviderTest, LoanedThrowingCallbackNeitherEscapesNorLeaksLoans) {
    FastDDSPubSubProvider pub_provider(LoanPublishOptions());
    FastDDSPubSubProvider sub_provider(BoundedOptions());

    pub_provider.CreateTopic({"loaned", "throwing"}, MakeSchema());

    std::atomic<int> deliveries{0};
    std::atomic<int32_t> received{-1};
    SubscriptionResult result = sub_provider.Subscribe(
        {"loaned", "throwing"},
        [&](const uint8_t* data, size_t len, const SharedSchema&, const Attachments&) {
            // More throws than the loan pool holds, so a leak cannot be masked by spare slots.
            if (deliveries.fetch_add(1) < 15) throw std::runtime_error("callback failure");
            if (len >= 5) received.store(DecodeRow(data));
        });
    ASSERT_TRUE(result.schema.get());

    for (int32_t i = 0; i < 40; ++i) {
        pub_provider.Publish({"loaned", "throwing"}, MakeEncoder(i));
    }

    EXPECT_NE(AwaitRow(received), -1) << "delivery stopped after the throwing callbacks";
    EXPECT_GT(deliveries.load(), 15);
}

// Same guarantee with a publisher that does not loan.
TEST(FastDDSPubSubProviderTest, CopyingThrowingCallbackDoesNotEscape) {
    FastDDSPubSubProvider pub_provider(BoundedOptions());
    FastDDSPubSubProvider sub_provider(BoundedOptions());

    pub_provider.CreateTopic({"datasharing", "throwing"}, MakeSchema());

    std::atomic<int> deliveries{0};
    std::atomic<int32_t> received{-1};
    SubscriptionResult result = sub_provider.Subscribe(
        {"datasharing", "throwing"},
        [&](const uint8_t* data, size_t len, const SharedSchema&, const Attachments&) {
            if (deliveries.fetch_add(1) < 5) throw std::runtime_error("callback failure");
            if (len >= 5) received.store(DecodeRow(data));
        });
    ASSERT_TRUE(result.schema.get());

    for (int32_t i = 0; i < 20; ++i) {
        pub_provider.Publish({"datasharing", "throwing"}, MakeEncoder(i));
    }

    EXPECT_NE(AwaitRow(received), -1) << "delivery stopped after the throwing callbacks";
    EXPECT_GT(deliveries.load(), 5);
}

// Attachments ride the same envelope on the loaned path.
TEST(FastDDSPubSubProviderTest, LoanedDeliversAttachments) {
    FastDDSPubSubProvider pub_provider(LoanPublishOptions());
    FastDDSPubSubProvider sub_provider(BoundedOptions());

    pub_provider.CreateTopic({"loaned", "attachments"}, MakeSchema());

    std::atomic<int32_t> received{-1};
    std::vector<uint8_t> blob_seen;
    SubscriptionResult result = sub_provider.Subscribe(
        {"loaned", "attachments"},
        [&](const uint8_t* data, size_t len, const SharedSchema&, const Attachments& att) {
            auto it = att.find("sidecar");
            if (it != att.end() && it->second) blob_seen = *it->second;
            if (len >= 5) received.store(DecodeRow(data));
        });
    ASSERT_TRUE(result.schema.get());

    Attachments att;
    att["sidecar"] = std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{1, 2, 3});
    pub_provider.Publish({"loaned", "attachments"}, MakeEncoder(7), att);

    EXPECT_EQ(AwaitRow(received), 7);
    EXPECT_EQ(blob_seen, (std::vector<uint8_t>{1, 2, 3}));
}

// Loans are never negotiated: each side's own type gates its own, so all pairings interoperate.

// ---------------------------------------------------------------------------
// Tests — QoS configuration via FastDDSProviderOptions
// ---------------------------------------------------------------------------

// A KEEP_LAST(N) writer QoS keeps things working but lets us swap out the
// default KEEP_ALL profile and still verify message delivery — proving the
// configured QoS is the one actually applied to the DataWriter.
TEST(FastDDSPubSubProviderTest, CustomDefaultWriterQos) {
    FastDDSProviderOptions opts;
    opts.default_writer_qos.history().kind = KEEP_LAST_HISTORY_QOS;
    opts.default_writer_qos.history().depth = 10;
    FastDDSPubSubProvider pub_provider(std::move(opts));

    FastDDSPubSubProvider sub_provider(FastDDSProviderOptions{});

    pub_provider.CreateTopic({"customdefault", "writer"}, MakeSchema());

    std::atomic<int32_t> received{-1};
    sub_provider.Subscribe(
        {"customdefault", "writer"},
        [&](const uint8_t* data, size_t len, const SharedSchema&, const Attachments&) {
            if (len >= 5) received.store(DecodeRow(data));
        });

    pub_provider.Publish({"customdefault", "writer"}, MakeEncoder(7));

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (received.load() == -1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    EXPECT_EQ(received.load(), 7);
}

TEST(FastDDSPubSubProviderTest, CustomDefaultReaderQos) {
    FastDDSPubSubProvider pub_provider(FastDDSProviderOptions{});

    FastDDSProviderOptions opts;
    opts.default_reader_qos.history().kind = KEEP_LAST_HISTORY_QOS;
    opts.default_reader_qos.history().depth = 10;
    FastDDSPubSubProvider sub_provider(std::move(opts));

    pub_provider.CreateTopic({"customdefault", "reader"}, MakeSchema());

    std::atomic<int32_t> received{-1};
    sub_provider.Subscribe(
        {"customdefault", "reader"},
        [&](const uint8_t* data, size_t len, const SharedSchema&, const Attachments&) {
            if (len >= 5) received.store(DecodeRow(data));
        });

    pub_provider.Publish({"customdefault", "reader"}, MakeEncoder(11));

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (received.load() == -1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    EXPECT_EQ(received.load(), 11);
}

// Per-topic override should affect only the specified topic; an untouched
// topic on the same provider must still use the instance default.
TEST(FastDDSPubSubProviderTest, PerTopicWriterQosOverridesDefault) {
    FastDDSProviderOptions opts;
    DataWriterQos override_qos = opts.default_writer_qos;
    override_qos.history().kind = KEEP_LAST_HISTORY_QOS;
    override_qos.history().depth = 5;
    opts.topic_writer_qos["pertopic/override"] = std::move(override_qos);

    FastDDSPubSubProvider pub_provider(std::move(opts));
    FastDDSPubSubProvider sub_provider(FastDDSProviderOptions{});

    pub_provider.CreateTopic({"pertopic", "override"}, MakeSchema());
    pub_provider.CreateTopic({"pertopic", "default"}, MakeSchema());

    std::atomic<int32_t> received_override{-1};
    std::atomic<int32_t> received_default{-1};
    sub_provider.Subscribe({"pertopic", "override"}, [&](const uint8_t* data, size_t len,
                                                         const SharedSchema&, const Attachments&) {
        if (len >= 5) received_override.store(DecodeRow(data));
    });
    sub_provider.Subscribe({"pertopic", "default"}, [&](const uint8_t* data, size_t len,
                                                        const SharedSchema&, const Attachments&) {
        if (len >= 5) received_default.store(DecodeRow(data));
    });

    pub_provider.Publish({"pertopic", "override"}, MakeEncoder(101));
    pub_provider.Publish({"pertopic", "default"}, MakeEncoder(202));

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while ((received_override.load() == -1 || received_default.load() == -1) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    EXPECT_EQ(received_override.load(), 101);
    EXPECT_EQ(received_default.load(), 202);
}

TEST(FastDDSPubSubProviderTest, PerTopicReaderQosOverridesDefault) {
    FastDDSPubSubProvider pub_provider(FastDDSProviderOptions{});

    FastDDSProviderOptions sub_opts;
    DataReaderQos override_qos = sub_opts.default_reader_qos;
    override_qos.history().kind = KEEP_LAST_HISTORY_QOS;
    override_qos.history().depth = 5;
    sub_opts.topic_reader_qos["pertopic/readeroverride"] = std::move(override_qos);

    FastDDSPubSubProvider sub_provider(std::move(sub_opts));

    pub_provider.CreateTopic({"pertopic", "readeroverride"}, MakeSchema());

    std::atomic<int32_t> received{-1};
    sub_provider.Subscribe(
        {"pertopic", "readeroverride"},
        [&](const uint8_t* data, size_t len, const SharedSchema&, const Attachments&) {
            if (len >= 5) received.store(DecodeRow(data));
        });

    pub_provider.Publish({"pertopic", "readeroverride"}, MakeEncoder(303));

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (received.load() == -1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    EXPECT_EQ(received.load(), 303);
}

// Mirrors Autonomy's QoS profile: TRANSIENT_LOCAL + RELIABLE + KEEP_LAST(10)
// + explicit resource limits on both writer and reader. Sets it via Options
// to prove the typed Options path covers a realistic production profile.
TEST(FastDDSPubSubProviderTest, AutonomyStyleProfileViaOptions) {
    FastDDSProviderOptions pub_opts;
    pub_opts.default_writer_qos.reliability().kind = RELIABLE_RELIABILITY_QOS;
    pub_opts.default_writer_qos.durability().kind = TRANSIENT_LOCAL_DURABILITY_QOS;
    pub_opts.default_writer_qos.history().kind = KEEP_LAST_HISTORY_QOS;
    pub_opts.default_writer_qos.history().depth = 10;
    pub_opts.default_writer_qos.resource_limits().max_samples = 100;
    pub_opts.default_writer_qos.resource_limits().max_samples_per_instance = 100;

    FastDDSProviderOptions sub_opts;
    sub_opts.default_reader_qos.reliability().kind = RELIABLE_RELIABILITY_QOS;
    sub_opts.default_reader_qos.durability().kind = TRANSIENT_LOCAL_DURABILITY_QOS;
    sub_opts.default_reader_qos.history().kind = KEEP_LAST_HISTORY_QOS;
    sub_opts.default_reader_qos.history().depth = 10;
    sub_opts.default_reader_qos.resource_limits().max_samples = 100;
    sub_opts.default_reader_qos.resource_limits().max_samples_per_instance = 100;

    FastDDSPubSubProvider pub_provider(std::move(pub_opts));
    FastDDSPubSubProvider sub_provider(std::move(sub_opts));

    pub_provider.CreateTopic({"autonomy", "profile"}, MakeSchema());

    std::atomic<int32_t> received{-1};
    sub_provider.Subscribe({"autonomy", "profile"}, [&](const uint8_t* data, size_t len,
                                                        const SharedSchema&, const Attachments&) {
        if (len >= 5) received.store(DecodeRow(data));
    });

    pub_provider.Publish({"autonomy", "profile"}, MakeEncoder(2026));

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (received.load() == -1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    EXPECT_EQ(received.load(), 2026);
}

// ---------------------------------------------------------------------------
// Subscriber-first: Subscribe before any publisher/topic exists must not block
// or throw, and once a publisher appears the schema future resolves and the
// first callback fires with a non-null schema (data is held until the schema
// arrives — the callback is never invoked with a null schema).
// ---------------------------------------------------------------------------
TEST(FastDDSPubSubProviderTest, SubscribeBeforePublishDeliversWithSchema) {
    FastDDSPubSubProvider sub_provider(FastDDSProviderOptions{});
    FastDDSPubSubProvider pub_provider(FastDDSProviderOptions{});

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

    // No publisher has announced the schema yet, so the future is unresolved.
    EXPECT_TRUE(result.schema.valid());
    EXPECT_EQ(result.schema.wait_for(std::chrono::milliseconds(0)), std::future_status::timeout);

    // A publisher appears and publishes.
    pub_provider.CreateTopic({"subfirst", "x"}, MakeSchema());
    pub_provider.Publish({"subfirst", "x"}, MakeEncoder(99));

    // The schema future now resolves with a non-null schema.
    ASSERT_EQ(result.schema.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    SharedSchema fut_schema = result.schema.get();
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
// Subscribe-first burst: a real round-trip where the subscriber joins before
// the publisher, so the first samples are buffered until the schema arrives
// and then flushed. Functional smoke test that the whole burst is delivered,
// in order. (A deterministic proof of the handoff-ordering invariant — that a
// live sample arriving mid-flush cannot overtake the backlog — is the
// OrderedDelivery unit test below; that race is timing-dependent over real
// DDS, so it is verified directly on the mechanism instead.)
// ---------------------------------------------------------------------------
TEST(FastDDSPubSubProviderTest, SubscribeFirstBurstDeliveredInOrder) {
    constexpr int32_t kCount = 1000;

    FastDDSPubSubProvider sub_provider(FastDDSProviderOptions{});
    FastDDSPubSubProvider pub_provider(FastDDSProviderOptions{});

    std::mutex mu;
    std::condition_variable cv;
    std::vector<int32_t> received;

    sub_provider.Subscribe({"ordering", "burst"}, [&](const uint8_t* data, size_t len,
                                                      const SharedSchema&, const Attachments&) {
        if (len < 5) return;
        std::lock_guard<std::mutex> lk(mu);
        received.push_back(DecodeRow(data));
        cv.notify_all();
    });

    pub_provider.CreateTopic({"ordering", "burst"}, MakeSchema());
    for (int32_t i = 0; i < kCount; ++i) {
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
// OrderedDelivery — deterministic proof of the schema-handoff invariant.
//
// The bug: the buggy listener had two delivery paths — a live sample (data
// thread) was delivered directly, bypassing the backlog being flushed by the
// schema thread — so the two ran concurrently and a live sample could
// overtake the backlog. OrderedDelivery removes the second path: every sample
// goes through one FIFO drained by a single drainer.
//
// The single-drainer guard is what serialises delivery. Its observable,
// thread-free signature: a sample offered *while a drain is in progress* must
// NOT be delivered inline (nested inside the current callback) — on the real
// two-thread path an inline/concurrent delivery is exactly the overtaking
// race. We trigger it deterministically by re-entering Offer() from inside the
// callback. With the guard the re-offered sample is queued and delivered after
// the current callback returns (no nesting); remove the guard and Offer drains
// reentrantly, invoking a callback nested inside another — which this test
// catches. Order is asserted too: the late sample lands strictly last.
// ---------------------------------------------------------------------------
TEST(OrderedDeliveryTest, MidFlushOfferIsNotDeliveredInline) {
    std::vector<int32_t> order;
    int active = 0;       // callbacks currently on the stack
    bool nested = false;  // a callback was entered while another was active
    fletcher::internal::OrderedDelivery* self = nullptr;
    bool injected = false;

    fletcher::internal::OrderedDelivery delivery(
        [&](const uint8_t* data, size_t len, const SharedSchema&, const Attachments&) {
            ASSERT_GE(len, 5u);
            if (active > 0) {
                nested = true;
            }
            ++active;
            order.push_back(DecodeRow(data));
            // While the backlog [0,1,2] is draining, a fresh live sample
            // arrives. Re-entering Offer here is the deterministic stand-in for
            // the data-reader thread delivering during the flush.
            if (!injected) {
                injected = true;
                std::vector<uint8_t> row(5);
                row[0] = 0x00;
                int32_t v = 99;
                std::memcpy(row.data() + 1, &v, sizeof(v));
                self->Offer(std::move(row), {});
            }
            --active;
        });
    self = &delivery;

    auto row_bytes = [](int32_t v) {
        std::vector<uint8_t> row(5);
        row[0] = 0x00;
        std::memcpy(row.data() + 1, &v, sizeof(v));
        return row;
    };

    // Three samples arrive before the schema is known — buffered, not delivered.
    delivery.Offer(row_bytes(0), {});
    delivery.Offer(row_bytes(1), {});
    delivery.Offer(row_bytes(2), {});
    EXPECT_TRUE(order.empty()) << "samples must be held until the schema is set";

    // Schema resolves: the backlog drains. The sample offered mid-flush must be
    // delivered after the current callback returns (not nested), and land last.
    delivery.SetSchema(MakeSharedSchema(MakeSchema()));

    EXPECT_FALSE(nested)
        << "a sample offered mid-flush was delivered inline — on the real two-thread "
           "path that is the live-sample-overtakes-backlog race";
    EXPECT_EQ(order, (std::vector<int32_t>{0, 1, 2, 99}));
}

// A sample offered before the schema is known must not reach the callback
// until SetSchema arrives (no null-schema delivery).
TEST(OrderedDeliveryTest, HoldsSamplesUntilSchemaIsSet) {
    std::vector<int32_t> order;
    fletcher::internal::OrderedDelivery delivery(
        [&](const uint8_t* data, size_t len, const SharedSchema& schema, const Attachments&) {
            ASSERT_GE(len, 5u);
            EXPECT_TRUE(schema) << "callback invoked with a null schema";
            order.push_back(DecodeRow(data));
        });

    std::vector<uint8_t> row(5);
    row[0] = 0x00;
    int32_t v = 7;
    std::memcpy(row.data() + 1, &v, sizeof(v));
    delivery.Offer(row, {});
    EXPECT_TRUE(order.empty());

    delivery.SetSchema(MakeSharedSchema(MakeSchema()));
    ASSERT_EQ(order.size(), 1u);
    EXPECT_EQ(order[0], 7);
}

// A null schema must never release buffered samples — that would drain them
// with a null schema and break schema-before-data. Buffering continues until a
// real schema arrives.
TEST(OrderedDeliveryTest, NullSchemaDoesNotReleaseBufferedSamples) {
    std::vector<int32_t> order;
    fletcher::internal::OrderedDelivery delivery(
        [&](const uint8_t* data, size_t len, const SharedSchema& schema, const Attachments&) {
            ASSERT_GE(len, 5u);
            EXPECT_TRUE(schema) << "callback invoked with a null schema";
            order.push_back(DecodeRow(data));
        });

    std::vector<uint8_t> row(5);
    row[0] = 0x00;
    int32_t v = 7;
    std::memcpy(row.data() + 1, &v, sizeof(v));
    delivery.Offer(row, {});

    delivery.SetSchema(nullptr);  // must not flip schema_ready_ or drain
    EXPECT_TRUE(order.empty());

    delivery.SetSchema(MakeSharedSchema(MakeSchema()));
    ASSERT_EQ(order.size(), 1u);
    EXPECT_EQ(order[0], 7);
}

// OfferView only borrows its bytes. A view buffered before the schema arrives
// must therefore be copied, not remembered — the loan it points into is
// returned as soon as OfferView comes back.
TEST(OrderedDeliveryTest, OfferViewCopiesWhatItCannotDeliverYet) {
    std::vector<int32_t> order;
    fletcher::internal::OrderedDelivery delivery(
        [&](const uint8_t* data, size_t len, const SharedSchema&, const Attachments&) {
            ASSERT_GE(len, 5u);
            order.push_back(DecodeRow(data));
        });

    std::vector<uint8_t> row(5);
    row[0] = 0x00;
    int32_t v = 7;
    std::memcpy(row.data() + 1, &v, sizeof(v));

    delivery.OfferView(row.data(), row.size(), {});
    EXPECT_TRUE(order.empty());

    row.assign(row.size(), 0xAA);  // the "loan" is gone

    delivery.SetSchema(MakeSharedSchema(MakeSchema()));
    ASSERT_EQ(order.size(), 1u);
    EXPECT_EQ(order[0], 7);
}

// With the schema in hand and nothing queued, OfferView hands the view straight
// to the callback.
TEST(OrderedDeliveryTest, OfferViewDeliversInlineOnceSchemaIsKnown) {
    const uint8_t* seen = nullptr;
    fletcher::internal::OrderedDelivery delivery(
        [&](const uint8_t* data, size_t, const SharedSchema&, const Attachments&) { seen = data; },
        MakeSharedSchema(MakeSchema()));

    std::vector<uint8_t> row(5);
    delivery.OfferView(row.data(), row.size(), {});
    EXPECT_EQ(seen, row.data()) << "the view was copied instead of delivered in place";
}

// A subscriber that starts before its publisher buffers everything until the schema arrives. If no
// publisher ever appears that is unbounded growth on a reachable path, so the backlog is capped and
// the oldest go — which is what KEEP_LAST would have done to the same samples.
TEST(OrderedDeliveryTest, BacklogIsCappedAndDropsOldest) {
    std::vector<int32_t> order;
    fletcher::internal::OrderedDelivery delivery(
        [&](const uint8_t* data, size_t len, const SharedSchema&, const Attachments&) {
            ASSERT_GE(len, 5u);
            order.push_back(DecodeRow(data));
        },
        nullptr, /*max_queued=*/3);

    for (int32_t i = 0; i < 10; ++i) {
        std::vector<uint8_t> row(5);
        row[0] = 0x00;
        std::memcpy(row.data() + 1, &i, sizeof(i));
        delivery.Offer(row, {});
    }
    EXPECT_TRUE(order.empty()) << "nothing may be delivered before the schema is known";

    delivery.SetSchema(MakeSharedSchema(MakeSchema()));
    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order, (std::vector<int32_t>{7, 8, 9})) << "the newest samples should survive";
}

// ---------------------------------------------------------------------------
// The latched (steady) path
//
// Once the schema is known and the backlog is gone, OrderedDelivery has nothing left to order: the
// schema listener has fired for the last time, and Fast DDS serialises every on_data_available for
// one reader under that reader's own mutex. It latches into a path that skips the mutex, the queue
// and the schema copy. These tests pin the contract that must survive that: every sample, in order,
// never with a null schema, loaned bytes still lent, and re-entry still queued rather than nested.
// ---------------------------------------------------------------------------

namespace {
std::vector<uint8_t> DeliveryRow(int32_t value) {
    std::vector<uint8_t> row(5);
    row[0] = 0x00;
    std::memcpy(row.data() + 1, &value, sizeof(value));
    return row;
}
}  // namespace

TEST(OrderedDeliveryTest, SteadyStateDeliversEverySampleInOrderWithASchema) {
    std::vector<int32_t> order;
    size_t with_schema = 0;
    fletcher::internal::OrderedDelivery delivery(
        [&](const uint8_t* data, size_t len, const SharedSchema& schema, const Attachments&) {
            ASSERT_GE(len, 5u);
            if (schema) ++with_schema;
            order.push_back(DecodeRow(data));
        },
        nullptr, /*max_queued=*/16);

    // Reach the steady state by draining a backlog rather than starting in it.
    for (int32_t i = 0; i < 3; ++i) delivery.Offer(DeliveryRow(i), {});
    EXPECT_TRUE(order.empty());
    delivery.SetSchema(MakeSharedSchema(MakeSchema()));
    ASSERT_EQ(order.size(), 3u);

    // Everything from here takes the latched path, alternating both entry points.
    for (int32_t i = 3; i < 13; ++i) {
        if (i % 2 == 0) {
            delivery.Offer(DeliveryRow(i), {});
        } else {
            const std::vector<uint8_t> row = DeliveryRow(i);
            delivery.OfferView(row.data(), row.size(), {});
        }
    }

    ASSERT_EQ(order.size(), 13u);
    EXPECT_EQ(with_schema, 13u) << "the callback must never see a null schema";
    for (int32_t i = 0; i < 13; ++i) {
        EXPECT_EQ(order[static_cast<size_t>(i)], i) << "out of order at index " << i;
    }
}

TEST(OrderedDeliveryTest, SteadyStateOfferViewStillLendsTheBytes) {
    const uint8_t* seen = nullptr;
    fletcher::internal::OrderedDelivery delivery(
        [&](const uint8_t* data, size_t, const SharedSchema&, const Attachments&) { seen = data; },
        MakeSharedSchema(MakeSchema()));

    const std::vector<uint8_t> first(5);
    delivery.OfferView(first.data(), first.size(), {});
    ASSERT_EQ(seen, first.data());

    // The first delivery latches; this one takes the lock-free path.
    const std::vector<uint8_t> second(5);
    delivery.OfferView(second.data(), second.size(), {});
    EXPECT_EQ(seen, second.data()) << "the latched path copied the view instead of lending it";
}

// The no-nesting guarantee of MidFlushOfferIsNotDeliveredInline, restated for the latched path: a
// sample offered from inside the callback lands after it, not on top of it. Nesting here would also
// let a re-offering callback recurse until the stack runs out.
TEST(OrderedDeliveryTest, SteadyStateReentrantOfferIsQueuedNotNested) {
    std::vector<int32_t> order;
    int active = 0;
    bool nested = false;
    fletcher::internal::OrderedDelivery* self = nullptr;
    bool injected = false;

    fletcher::internal::OrderedDelivery delivery(
        [&](const uint8_t* data, size_t, const SharedSchema&, const Attachments&) {
            if (active > 0) nested = true;
            ++active;
            order.push_back(DecodeRow(data));
            if (!injected) {
                injected = true;
                self->Offer(DeliveryRow(99), {});
            }
            --active;
        },
        MakeSharedSchema(MakeSchema()));
    self = &delivery;

    delivery.Offer(DeliveryRow(1), {});  // latches the steady state
    delivery.Offer(DeliveryRow(2), {});  // re-enters from inside the callback

    EXPECT_FALSE(nested) << "the latched path delivered a re-offered sample inline";
    EXPECT_EQ(order, (std::vector<int32_t>{1, 99, 2}));
}

// A throwing callback must not leave the latched path wedged: the next sample still gets through.
TEST(OrderedDeliveryTest, SteadyStateSurvivesAThrowingCallback) {
    std::vector<int32_t> order;
    bool thrown = false;
    fletcher::internal::OrderedDelivery delivery(
        [&](const uint8_t* data, size_t, const SharedSchema&, const Attachments&) {
            order.push_back(DecodeRow(data));
            if (!thrown) {
                thrown = true;
                throw std::runtime_error("callback");
            }
        },
        MakeSharedSchema(MakeSchema()));

    EXPECT_THROW(delivery.Offer(DeliveryRow(1), {}), std::runtime_error);
    delivery.Offer(DeliveryRow(2), {});
    EXPECT_EQ(order, (std::vector<int32_t>{1, 2})) << "delivery wedged after the callback threw";
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
    FastDDSPubSubProvider pub_provider(FastDDSProviderOptions{});
    FastDDSPubSubProvider sub_provider(FastDDSProviderOptions{});
    pub_provider.CreateTopic({"resub", "x"}, MakeSchema());

    std::atomic<int32_t> first{-1};
    sub_provider.Subscribe({"resub", "x"}, [&](const uint8_t* data, size_t len, const SharedSchema&,
                                               const Attachments&) {
        if (len >= 5) first.store(DecodeRow(data));
    });
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
        });
    ASSERT_TRUE(again.schema.valid());
    ASSERT_TRUE(again.schema.get());

    pub_provider.Publish({"resub", "x"}, MakeEncoder(2));
    EXPECT_EQ(AwaitRow(second), 2);
}

// Unsubscribing a topic that was never subscribed is a no-op, and must not disturb a live
// subscription on a different topic.
TEST(FastDDSPubSubProviderTest, UnsubscribeUnknownTopicIsHarmless) {
    FastDDSPubSubProvider pub_provider(FastDDSProviderOptions{});
    FastDDSPubSubProvider sub_provider(FastDDSProviderOptions{});
    pub_provider.CreateTopic({"unsub", "live"}, MakeSchema());

    std::atomic<int32_t> received{-1};
    sub_provider.Subscribe({"unsub", "live"}, [&](const uint8_t* data, size_t len,
                                                  const SharedSchema&, const Attachments&) {
        if (len >= 5) received.store(DecodeRow(data));
    });

    EXPECT_NO_THROW(sub_provider.Unsubscribe({"unsub", "never"}));

    pub_provider.Publish({"unsub", "live"}, MakeEncoder(7));
    EXPECT_EQ(AwaitRow(received), 7);
}

// The write is checked, so an oversized schema surfaces at declaration time.
TEST(FastDDSPubSubProviderTest, ASchemaTooLargeForItsChannelIsReported) {
    FastDDSProviderOptions opts;
    opts.max_schema_bytes = 8;  // smaller than any real Arrow IPC schema
    FastDDSPubSubProvider p(opts);
    EXPECT_THROW(p.CreateTopic({"schema", "toobig"}, MakeSchema()), std::runtime_error);
}
