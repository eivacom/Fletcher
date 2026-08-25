// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// What replacing the old DDS type with FletcherSample cost or bought, at nanosecond resolution.
//
// Arms:
//   BM_Serialize / BM_Deserialize    the two TopicDataTypes head to head — legacy (unbounded,
//                                    non-plain, self-framing envelope) against current (bounded and
//                                    plain), plus current with fastcdr placing the encapsulation,
//                                    which is how it was written until that was measured. Row size
//                                    swept.
//   BM_PublishFlow / BM_ReadFlow     what the loaned flow removes from each side, isolated from
//                                    Fast DDS. This is the zero-copy budget: bench_dds_payload can
//                                    only see it at 0.1 us granularity, and bench_e2e not at all.
//   BM_Deliver                       the subscribe-side delivery layer: OrderedDelivery for both
//                                    read flows, and ParseEnvelopeBody with attachments, which the
//                                    ReadFlow arm above never parses. Read each against
//                                    BM_Deliver_CallbackOnly from the same run.
//   BM_ProviderPublishOverhead       what FastDDSPubSubProvider::Publish spends per sample before
//                                    the type is reached, i.e. what the loan saving competes with.
//                                    Superseded by the Monorepo's tools/fletcher_bench/bench_publish,
//                                    which drives the real Publish against a raw DDS control.
//   BM_Memcpy                        the floor: the row bytes moved once, nothing else.
//   BM_BatchRoundTrip                a whole Arrow batch out and back — read a row out of an
//                                    ArrowArray, encode, serialise, deserialise, decode, append to
//                                    a fresh ArrowArray. Row count swept.
//
// The batch arms round-trip through the C data interface (nanoarrow), the Arrow tier Fletcher's
// edge deployments and modules/datamodel use. The Apache Arrow C++ tier (fletcher-arrow-bridge,
// arrow::RecordBatch) sits a layer above and is not measured here.
//
// Results: Monorepo modules/io/docs/serialization-benchmark.md.

#include <benchmark/benchmark.h>
#include <fastcdr/Cdr.h>
#include <fastcdr/FastBuffer.h>
#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fastdds/rtps/common/SerializedPayload.hpp>
#include <functional>
#include <fletcher/core/positional_io.hpp>
#include <fletcher/core/types.hpp>
#include <fletcher/core/write_buffer.hpp>
#include <fletcher/pubsub/internal/segments.hpp>
#include <fletcher/pubsub/owned_schema.hpp>
#include <fletcher/pubsub/provider.hpp>
#include <map>
#include <memory>
#include <shared_mutex>
#include <string>
#include <vector>

#include "envelope_codec.hpp"
#include "fletcher_sample.hpp"
#include "fletcher_sample_pub_sub_type.hpp"
#include "legacy_fletcher_topic_type.hpp"
#include "ordered_delivery.hpp"
#include "transform_batch.hpp"
#include "transport_data.hpp"

namespace {

using eprosima::fastdds::rtps::SerializedPayload_t;

// The payload bound these benchmarks measure at. The type is templated on it — one plain type per
// power of two the provider compiles — and the numbers in the README were taken at 64 KiB.
constexpr uint32_t kBenchPayloadBytes = 64 * 1024;
using FletcherSample = fletcher::internal::FletcherSample<kBenchPayloadBytes>;
using FletcherSamplePubSubType = fletcher::internal::FletcherSamplePubSubType<kBenchPayloadBytes>;

// The row, its Arrow schema, and the batch helpers the round-trip arms use — shared with
// example_arrow_roundtrip.cpp so both put the same thing through the type.
using namespace fletcher::benchmarks;  // NOLINT(build/namespaces)

// The timed arms all measure XCDRv1, the representation a writer gets by default. XCDR2 is
// exercised by ValidatePayloadBytes, which is where the two representation ids have to match
// fastcdr.
constexpr auto kXcdr1 = eprosima::fastdds::dds::DataRepresentationId_t::XCDR_DATA_REPRESENTATION;

// 214 B is the production TransformWithVelocity row; the rest walk up to just under the default
// 64 KiB bound, which is where bench_read_flow found the read-side gain largest.
#define FLETCHER_ROW_SIZES Arg(214)->Arg(4096)->Arg(16384)->Arg(60000)

const fletcher::Attachments kNoAttachments;

fletcher::PubSubProvider::RowEncoder MakeByteEncoder(const std::vector<uint8_t>& row) {
    return [&row](fletcher::WriteBuffer& buf) { buf.Append(row.data(), row.size()); };
}

// ---------------------------------------------------------------------------
// TopicDataType::serialize / deserialize, legacy against current
// ---------------------------------------------------------------------------

template <typename TopicTypeT>
void SerializeArm(benchmark::State& state, TopicTypeT& type) {
    const std::vector<uint8_t> row(static_cast<size_t>(state.range(0)), 0xAB);
    fletcher::internal::PublishData transport;
    const fletcher::PubSubProvider::RowEncoder encoder = MakeByteEncoder(row);
    transport.encoder = &encoder;
    transport.attachments = &kNoAttachments;

    SerializedPayload_t payload(type.max_serialized_type_size);
    for (auto _ : state) {
        if (!type.serialize(&transport, payload, kXcdr1)) {
            state.SkipWithError("serialize failed");
            break;
        }
        benchmark::DoNotOptimize(payload.data);
    }
    state.counters["wire_bytes"] = static_cast<double>(payload.length);
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * row.size());
}

template <typename TopicTypeT>
void DeserializeArm(benchmark::State& state, TopicTypeT& type) {
    const std::vector<uint8_t> row(static_cast<size_t>(state.range(0)), 0xAB);
    fletcher::internal::PublishData source;
    const fletcher::PubSubProvider::RowEncoder encoder = MakeByteEncoder(row);
    source.encoder = &encoder;
    source.attachments = &kNoAttachments;

    SerializedPayload_t payload(type.max_serialized_type_size);
    if (!type.serialize(&source, payload, kXcdr1)) {
        state.SkipWithError("serialize failed");
        return;
    }

    // One ReceivedData across all iterations, because that is what the reader does: the listener
    // owns it and reuses it sample after sample, so decoded_row's capacity is warm from the second
    // sample on. A fresh one per iteration would measure the allocator instead.
    fletcher::internal::ReceivedData sink;
    for (auto _ : state) {
        if (!type.deserialize(payload, &sink)) {
            state.SkipWithError("deserialize failed");
            break;
        }
        benchmark::DoNotOptimize(sink.decoded_row.data());
    }
    state.counters["wire_bytes"] = static_cast<double>(payload.length);
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * row.size());
}

void BM_Serialize_Legacy(benchmark::State& state) {
    fletcher::benchmarks::LegacyFletcherTopicType type(kBenchPayloadBytes);
    SerializeArm(state, type);
}
BENCHMARK(BM_Serialize_Legacy)->FLETCHER_ROW_SIZES;

void BM_Serialize_Current(benchmark::State& state) {
    FletcherSamplePubSubType type;
    SerializeArm(state, type);
}
BENCHMARK(BM_Serialize_Current)->FLETCHER_ROW_SIZES;

void BM_Deserialize_Legacy(benchmark::State& state) {
    fletcher::benchmarks::LegacyFletcherTopicType type(kBenchPayloadBytes);
    DeserializeArm(state, type);
}
BENCHMARK(BM_Deserialize_Legacy)->FLETCHER_ROW_SIZES;

void BM_Deserialize_Current(benchmark::State& state) {
    FletcherSamplePubSubType type;
    DeserializeArm(state, type);
}
BENCHMARK(BM_Deserialize_Current)->FLETCHER_ROW_SIZES;

// What the shipped type would cost if fastcdr placed the eight header bytes, which is how it was
// written until the framing was measured: same type otherwise — is_plain, is_bounded, the type name
// and both loan paths inherited untouched. ValidatePayloadBytes asserts this produces the same
// bytes as the shipped serialize(), which is what makes the shipped one's hand-written
// encapsulation, and its computed padding count, safe to keep.
class FastCdrFramedPubSubType : public FletcherSamplePubSubType {
   public:
    bool serialize(const void* const data, SerializedPayload_t& payload,
                   eprosima::fastdds::dds::DataRepresentationId_t data_representation) override {
        const auto* d = static_cast<const fletcher::internal::PublishData*>(data);
        const bool xcdr1 = data_representation ==
                           eprosima::fastdds::dds::DataRepresentationId_t::XCDR_DATA_REPRESENTATION;

        eprosima::fastcdr::FastBuffer fastbuffer(reinterpret_cast<char*>(payload.data),
                                                 payload.max_size);
        eprosima::fastcdr::Cdr ser(
            fastbuffer, eprosima::fastcdr::Cdr::DEFAULT_ENDIAN,
            xcdr1 ? eprosima::fastcdr::CdrVersion::XCDRv1 : eprosima::fastcdr::CdrVersion::XCDRv2);
        payload.encapsulation =
            ser.endianness() == eprosima::fastcdr::Cdr::BIG_ENDIANNESS ? CDR_BE : CDR_LE;
        ser.set_encoding_flag(xcdr1 ? eprosima::fastcdr::EncodingAlgorithmFlag::PLAIN_CDR
                                    : eprosima::fastcdr::EncodingAlgorithmFlag::PLAIN_CDR2);

        try {
            ser.serialize_encapsulation();

            // Reserve the length and patch it once the body is written — the save-state / serialize
            // / jump sequence fastcdr itself uses for an XCDR2 DHEADER.
            const eprosima::fastcdr::Cdr::state length_state = ser.get_state();
            ser.serialize(static_cast<uint32_t>(0));

            const size_t body_offset = ser.get_serialized_data_length();
            fletcher::FixedWriteBuffer buf(payload.data + body_offset, kBenchPayloadBytes);
            fletcher::internal::EncodeEnvelopeBody(buf, *d->encoder, *d->attachments);
            const auto body_size = static_cast<uint32_t>(buf.Position());

            ser.set_state(length_state);
            ser.serialize(body_size);
            ser.jump(body_size);
            ser.set_dds_cdr_options({0, 0});

            payload.length = static_cast<uint32_t>(ser.get_serialized_data_length());
            return true;
        } catch (...) {
            payload.length = 0;
            return false;
        }
    }
};

void BM_Serialize_CurrentFastCdrFramed(benchmark::State& state) {
    FastCdrFramedPubSubType type;
    SerializeArm(state, type);
}
BENCHMARK(BM_Serialize_CurrentFastCdrFramed)->FLETCHER_ROW_SIZES;

// ---------------------------------------------------------------------------
// The zero-copy budget: what each flow does outside Fast DDS
// ---------------------------------------------------------------------------

// Why the sample struct is split by direction. An empty Attachments is what the publish path used
// to carry and never read, back when one struct held both directions: MSVC's std::unordered_map
// allocates a sentinel node in its default constructor, so every publish allocated and freed one.
// Against BM_PublishFieldsConstruct, which is what PublishData costs now.
void BM_AttachmentsConstruct(benchmark::State& state) {
    for (auto _ : state) {
        fletcher::Attachments attachments;
        benchmark::DoNotOptimize(&attachments);
    }
}
BENCHMARK(BM_AttachmentsConstruct);

// What the publish path carries now: the encoder and a borrowed Attachments pointer.
void BM_PublishFieldsConstruct(benchmark::State& state) {
    const std::vector<uint8_t> row(214, 0xAB);
    const fletcher::PubSubProvider::RowEncoder encoder = MakeByteEncoder(row);
    for (auto _ : state) {
        fletcher::internal::PublishData data;
        data.encoder = &encoder;
        data.attachments = &kNoAttachments;
        benchmark::DoNotOptimize(&data);
    }
}
BENCHMARK(BM_PublishFieldsConstruct);

// SampleWriter::Write — a PublishData per publish, which copies the RowEncoder std::function, then
// serialize() into the transport's payload.
void BM_PublishFlow_Serialised(benchmark::State& state) {
    FletcherSamplePubSubType type;
    const std::vector<uint8_t> row(static_cast<size_t>(state.range(0)), 0xAB);
    const fletcher::PubSubProvider::RowEncoder encoder = MakeByteEncoder(row);
    SerializedPayload_t payload(type.max_serialized_type_size);

    for (auto _ : state) {
        fletcher::internal::PublishData transport;
        transport.encoder = &encoder;
        transport.attachments = &kNoAttachments;
        if (!type.serialize(&transport, payload, kXcdr1)) {
            state.SkipWithError("serialize failed");
            break;
        }
        benchmark::DoNotOptimize(payload.data);
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * row.size());
}
BENCHMARK(BM_PublishFlow_Serialised)->FLETCHER_ROW_SIZES;

// LoanableSampleWriter::Write, minus the loan_sample() call Fast DDS owns: the row goes straight
// into the sample that will be published, and nothing frames it.
void BM_PublishFlow_Loaned(benchmark::State& state) {
    const std::vector<uint8_t> row(static_cast<size_t>(state.range(0)), 0xAB);
    const fletcher::PubSubProvider::RowEncoder encoder = MakeByteEncoder(row);
    auto sample = std::make_unique<FletcherSample>();

    for (auto _ : state) {
        fletcher::FixedWriteBuffer buf(sample->body, kBenchPayloadBytes);
        fletcher::internal::EncodeEnvelopeBody(buf, encoder, kNoAttachments);
        sample->length = static_cast<uint32_t>(buf.Position());
        benchmark::DoNotOptimize(sample->body);
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * row.size());
}
BENCHMARK(BM_PublishFlow_Loaned)->FLETCHER_ROW_SIZES;

// LoanableDataReaderListener's read: the row stays where the writer put it, so the parse hands back
// a pointer. Against BM_Deserialize_Current, which owes a vector copy of the same bytes.
void BM_ReadFlow_Loaned(benchmark::State& state) {
    const std::vector<uint8_t> row(static_cast<size_t>(state.range(0)), 0xAB);
    auto sample = std::make_unique<FletcherSample>();
    {
        fletcher::FixedWriteBuffer buf(sample->body, kBenchPayloadBytes);
        fletcher::internal::EncodeEnvelopeBody(buf, MakeByteEncoder(row), kNoAttachments);
        sample->length = static_cast<uint32_t>(buf.Position());
    }

    fletcher::Attachments attachments;
    for (auto _ : state) {
        // The parse reads a buffer that never changes and returns a pointer into it, so without
        // this the compiler is free to hoist the whole thing out of the loop and the arm measures
        // nothing.
        benchmark::ClobberMemory();
        const uint8_t* decoded = nullptr;
        uint32_t decoded_len = 0;
        if (!fletcher::internal::ParseEnvelopeBody(sample->body, sample->length, decoded,
                                                   decoded_len, attachments)) {
            state.SkipWithError("parse failed");
            break;
        }
        benchmark::DoNotOptimize(decoded);
        benchmark::DoNotOptimize(decoded_len);
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * row.size());
}
BENCHMARK(BM_ReadFlow_Loaned)->FLETCHER_ROW_SIZES;

// ---------------------------------------------------------------------------
// The delivery layer: what OrderedDelivery adds between the reader and the callback
// ---------------------------------------------------------------------------
//
// The subscribe-side counterpart of the monorepo's bench_publish, built the same way: a control arm
// that runs the callback and nothing else, so every arm below is read as its own time minus
// BM_Deliver_CallbackOnly *from the same run*. Absolute numbers here say little — the std::function
// call is most of them.
//
// This cannot be driven through a real DataReader. Delivery arrives on a Fast DDS listener thread
// and `take` consumes, so there is no synchronous loop to hand a benchmark; bench_read_flow in the
// monorepo meters that thread instead, at the ~1.95 us GetThreadTimes tick. Measured here is every
// piece of the path that *is* synchronously callable.

// The sink every delivery arm ends at. Non-trivial enough not to be optimised away, cheap enough not
// to dominate.
fletcher::PubSubProvider::SubscribeCallback MakeSink(size_t& sum) {
    return [&sum](const uint8_t* data, size_t len, const fletcher::SharedSchema&,
                  const fletcher::Attachments&) {
        sum += len ? data[0] : 0;
    };
}

// The floor: the callback alone, with the arguments a delivery hands it.
void BM_Deliver_CallbackOnly(benchmark::State& state) {
    const std::vector<uint8_t> row(static_cast<size_t>(state.range(0)), 0xAB);
    size_t sum = 0;
    const fletcher::PubSubProvider::SubscribeCallback sink = MakeSink(sum);
    const fletcher::SharedSchema schema = fletcher::MakeSharedSchema(TransformSchema());
    for (auto _ : state) {
        sink(row.data(), row.size(), schema, kNoAttachments);
    }
    benchmark::DoNotOptimize(sum);
}
BENCHMARK(BM_Deliver_CallbackOnly)->FLETCHER_ROW_SIZES;

// The loaned read flow's delivery: schema known and nothing queued, so OfferView claims the drain
// slot and hands the borrowed pointer straight on. The row is never copied, which is what should
// keep this flat across row size — hold it against BM_Deliver_Offer below.
void BM_Deliver_OfferView(benchmark::State& state) {
    const std::vector<uint8_t> row(static_cast<size_t>(state.range(0)), 0xAB);
    size_t sum = 0;
    fletcher::internal::OrderedDelivery delivery(MakeSink(sum), fletcher::MakeSharedSchema(TransformSchema()), 10);
    for (auto _ : state) {
        delivery.OfferView(row.data(), row.size(), kNoAttachments);
    }
    benchmark::DoNotOptimize(sum);
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * row.size());
}
BENCHMARK(BM_Deliver_OfferView)->FLETCHER_ROW_SIZES;

// The copying read flow hands Offer the vector its ReceivedData already owns. Both arms are flat
// across row size and within a couple of nanoseconds of the callback itself, because in the steady
// state neither one copies the row: Offer takes it by reference and the latched path passes the
// pointer straight on.
//
// **This is not the copying flow's total cost.** That flow also pays a deserialize per sample, which
// does copy the row into ReceivedData::decoded_row — see BM_Deserialize_Current. What this arm shows
// is only what OrderedDelivery adds on top of it, which is now almost nothing.
void BM_Deliver_Offer(benchmark::State& state) {
    const std::vector<uint8_t> row(static_cast<size_t>(state.range(0)), 0xAB);
    size_t sum = 0;
    fletcher::internal::OrderedDelivery delivery(MakeSink(sum), fletcher::MakeSharedSchema(TransformSchema()), 10);
    for (auto _ : state) {
        delivery.Offer(row, kNoAttachments);
    }
    benchmark::DoNotOptimize(sum);
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * row.size());
}
BENCHMARK(BM_Deliver_Offer)->FLETCHER_ROW_SIZES;

// ParseEnvelopeBody *with* attachments, which BM_ReadFlow_Loaned never exercises — it parses with
// kNoAttachments, so the entire attachment branch was unmeasured. Each one costs two length reads, a
// std::string key, a shared_ptr<vector> blob copied out of the payload, and a map insert.
// state.range(0) is the attachment count; the row stays at the production 214 B.
void BM_Deliver_ParseAttachments(benchmark::State& state) {
    const int count = static_cast<int>(state.range(0));
    const std::vector<uint8_t> row(214, 0xAB);
    const auto blob = std::make_shared<const std::vector<uint8_t>>(64, 0xCD);

    fletcher::Attachments sent;
    for (int i = 0; i < count; ++i) sent["attachment_" + std::to_string(i)] = blob;

    auto sample = std::make_unique<FletcherSample>();
    {
        fletcher::FixedWriteBuffer buf(sample->body, kBenchPayloadBytes);
        fletcher::internal::EncodeEnvelopeBody(buf, MakeByteEncoder(row), sent);
        sample->length = static_cast<uint32_t>(buf.Position());
    }

    // One Attachments across iterations, as the listener has: ParseEnvelopeBody clears and refills
    // it, so its buckets stay warm the way they do in the real read loop.
    fletcher::Attachments decoded_attachments;
    for (auto _ : state) {
        benchmark::ClobberMemory();
        const uint8_t* decoded = nullptr;
        uint32_t decoded_len = 0;
        if (!fletcher::internal::ParseEnvelopeBody(sample->body, sample->length, decoded, decoded_len,
                                                   decoded_attachments)) {
            state.SkipWithError("parse failed");
            break;
        }
        benchmark::DoNotOptimize(decoded);
    }
    state.counters["attachments"] = static_cast<double>(decoded_attachments.size());
}
BENCHMARK(BM_Deliver_ParseAttachments)->Arg(0)->Arg(1)->Arg(4)->Arg(16);

// What FastDDSPubSubProvider::Publish spends per sample before the sample writer is reached: join
// the segments, take the shared lock, find the topic. Independent of row size, and the number to
// hold against the loaned/serialised gap above.
void BM_ProviderPublishOverhead(benchmark::State& state) {
    const std::vector<std::string> segments = {"host", "RovSimulator", "State"};
    std::shared_mutex mutex;
    std::map<std::string, int> topics;
    topics[fletcher::internal::JoinSegments(segments)] = 1;
    topics["host/RovSimulator/Command"] = 2;
    topics["host/Vehicle/Telemetry"] = 3;

    for (auto _ : state) {
        std::string name = fletcher::internal::JoinSegments(segments);
        std::shared_lock lock(mutex);
        auto it = topics.find(name);
        benchmark::DoNotOptimize(it->second);
    }
}
BENCHMARK(BM_ProviderPublishOverhead);

void BM_Memcpy(benchmark::State& state) {
    const std::vector<uint8_t> row(static_cast<size_t>(state.range(0)), 0xAB);
    std::vector<uint8_t> destination(row.size());
    for (auto _ : state) {
        std::memcpy(destination.data(), row.data(), row.size());
        benchmark::DoNotOptimize(destination.data());
    }
    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * row.size());
}
BENCHMARK(BM_Memcpy)->FLETCHER_ROW_SIZES;

// ---------------------------------------------------------------------------
// Arrow batch out and back
// ---------------------------------------------------------------------------

// One batch through the type and back: read row, encode into the DDS payload, serialise,
// deserialise, decode, append to the outgoing batch. Every leg a publisher and a subscriber run per
// sample, minus Fast DDS itself.
template <typename TopicTypeT>
void BatchRoundTripArm(benchmark::State& state, TopicTypeT& type) {
    const int64_t rows = state.range(0);
    const fletcher::OwnedSchema schema = TransformSchema();

    ArrowBatch source(schema.get());
    BuildSourceBatch(source, rows);
    const ArrowBatchView source_view(schema.get(), source.get());

    SerializedPayload_t payload(type.max_serialized_type_size);
    fletcher::internal::ReceivedData sink;

    for (auto _ : state) {
        ArrowBatch out(schema.get());
        for (int64_t i = 0; i < rows; ++i) {
            const TransformRow row = source_view.Row(i);
            fletcher::internal::PublishData transport;
            const fletcher::PubSubProvider::RowEncoder encoder = [&row](fletcher::WriteBuffer& buf) { EncodeRow(row, buf); };
            transport.encoder = &encoder;
            transport.attachments = &kNoAttachments;
            if (!type.serialize(&transport, payload, kXcdr1) || !type.deserialize(payload, &sink)) {
                state.SkipWithError("round trip failed");
                break;
            }
            out.Append(DecodeRow(sink.decoded_row.data(), sink.decoded_row.size()));
        }
        out.Finish();
        benchmark::DoNotOptimize(out.get());
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * rows);
    state.counters["row_bytes"] = static_cast<double>(sink.decoded_row.size());
    state.counters["wire_bytes"] = static_cast<double>(payload.length);
}

void BM_BatchRoundTrip_Legacy(benchmark::State& state) {
    fletcher::benchmarks::LegacyFletcherTopicType type(kBenchPayloadBytes);
    BatchRoundTripArm(state, type);
}
BENCHMARK(BM_BatchRoundTrip_Legacy)->Arg(1000)->Arg(8000);

void BM_BatchRoundTrip_Current(benchmark::State& state) {
    FletcherSamplePubSubType type;
    BatchRoundTripArm(state, type);
}
BENCHMARK(BM_BatchRoundTrip_Current)->Arg(1000)->Arg(8000);

// The same batch over the flows the rewrite unlocked: the row goes into the sample the writer will
// publish, and comes back out of it in place. The legacy type could not do this at either end — it
// was neither bounded nor plain, so Fast DDS never offered it a loan.
void BM_BatchRoundTrip_CurrentLoaned(benchmark::State& state) {
    const int64_t rows = state.range(0);
    const fletcher::OwnedSchema schema = TransformSchema();

    ArrowBatch source(schema.get());
    BuildSourceBatch(source, rows);
    const ArrowBatchView source_view(schema.get(), source.get());

    auto sample = std::make_unique<FletcherSample>();
    fletcher::Attachments attachments;

    for (auto _ : state) {
        ArrowBatch out(schema.get());
        for (int64_t i = 0; i < rows; ++i) {
            const TransformRow row = source_view.Row(i);
            const fletcher::PubSubProvider::RowEncoder encoder =
                [&row](fletcher::WriteBuffer& buf) { EncodeRow(row, buf); };

            fletcher::FixedWriteBuffer buf(sample->body, kBenchPayloadBytes);
            fletcher::internal::EncodeEnvelopeBody(buf, encoder, kNoAttachments);
            sample->length = static_cast<uint32_t>(buf.Position());

            const uint8_t* decoded = nullptr;
            uint32_t decoded_len = 0;
            if (!fletcher::internal::ParseEnvelopeBody(sample->body, sample->length, decoded,
                                                       decoded_len, attachments)) {
                state.SkipWithError("parse failed");
                break;
            }
            out.Append(DecodeRow(decoded, decoded_len));
        }
        out.Finish();
        benchmark::DoNotOptimize(out.get());
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * rows);
}
BENCHMARK(BM_BatchRoundTrip_CurrentLoaned)->Arg(1000)->Arg(8000);

// ---------------------------------------------------------------------------
// Validation — runs before the benchmarks and fails the process
// ---------------------------------------------------------------------------

template <typename TopicTypeT>
bool ValidateBatchRoundTrip(const char* arm, TopicTypeT& type) {
    constexpr int64_t kRows = 1000;
    const fletcher::OwnedSchema schema = TransformSchema();

    ArrowBatch source(schema.get());
    BuildSourceBatch(source, kRows);
    const ArrowBatchView source_view(schema.get(), source.get());

    SerializedPayload_t payload(type.max_serialized_type_size);
    fletcher::internal::ReceivedData sink;
    ArrowBatch out(schema.get());
    for (int64_t i = 0; i < kRows; ++i) {
        const TransformRow row = source_view.Row(i);
        fletcher::internal::PublishData transport;
        const fletcher::PubSubProvider::RowEncoder encoder = [&row](fletcher::WriteBuffer& buf) { EncodeRow(row, buf); };
        transport.encoder = &encoder;
        transport.attachments = &kNoAttachments;
        if (!type.serialize(&transport, payload, kXcdr1)) {
            std::fprintf(stderr, "%s: serialize failed at row %lld\n", arm,
                         static_cast<long long>(i));
            return false;
        }
        if (!type.deserialize(payload, &sink)) {
            std::fprintf(stderr, "%s: deserialize failed at row %lld\n", arm,
                         static_cast<long long>(i));
            return false;
        }
        out.Append(DecodeRow(sink.decoded_row.data(), sink.decoded_row.size()));
    }
    out.Finish();

    const ArrowBatchView out_view(schema.get(), out.get());
    for (int64_t i = 0; i < kRows; ++i) {
        const TransformRow expected = source_view.Row(i);
        const TransformRow actual = out_view.Row(i);
        bool equal = expected.timestamp == actual.timestamp;
        for (int v = 0; v < kPoseValues; ++v) equal = equal && expected.pose[v] == actual.pose[v];
        for (int v = 0; v < kVelocityValues; ++v) {
            equal = equal && expected.velocity[v] == actual.velocity[v];
        }
        if (!equal) {
            std::fprintf(stderr, "%s: row %lld differs after the round trip\n", arm,
                         static_cast<long long>(i));
            return false;
        }
    }
    std::printf("%s: %lld rows round-tripped, row %zu B, wire %u B\n", arm,
                static_cast<long long>(kRows), sink.decoded_row.size(), payload.length);
    return true;
}

// The two types frame differently but must deliver the same row bytes, or the arms above are not
// measuring the same work.
bool ValidateSameRowBytes() {
    const std::vector<uint8_t> row(214, 0xAB);
    fletcher::benchmarks::LegacyFletcherTopicType legacy(kBenchPayloadBytes);
    FletcherSamplePubSubType current;

    fletcher::internal::PublishData transport;
    const fletcher::PubSubProvider::RowEncoder encoder = MakeByteEncoder(row);
    transport.encoder = &encoder;
    transport.attachments = &kNoAttachments;

    SerializedPayload_t legacy_payload(legacy.max_serialized_type_size);
    SerializedPayload_t current_payload(current.max_serialized_type_size);
    fletcher::internal::ReceivedData legacy_sink;
    fletcher::internal::ReceivedData current_sink;
    if (!legacy.serialize(&transport, legacy_payload, kXcdr1) ||
        !legacy.deserialize(legacy_payload, &legacy_sink) ||
        !current.serialize(&transport, current_payload, kXcdr1) ||
        !current.deserialize(current_payload, &current_sink)) {
        std::fputs("row-bytes check: a codec call failed\n", stderr);
        return false;
    }
    if (legacy_sink.decoded_row != current_sink.decoded_row || legacy_sink.decoded_row != row) {
        std::fputs("row-bytes check: the two types disagree on the row\n", stderr);
        return false;
    }
    std::printf("row-bytes check: identical rows, legacy wire %u B, current wire %u B\n",
                legacy_payload.length, current_payload.length);
    return true;
}

// Where the three framings differ on the wire. The shipped type writes its encapsulation by hand,
// so it has to match what fastcdr would have produced byte for byte; legacy is expected to differ
// in exactly one byte, the options field it hard-coded to zero (see fix 6 in
// serialization-benchmark.md).
bool ValidatePayloadBytes() {
    const std::vector<uint8_t> row(214, 0xAB);
    fletcher::internal::PublishData transport;
    const fletcher::PubSubProvider::RowEncoder encoder = MakeByteEncoder(row);
    transport.encoder = &encoder;
    transport.attachments = &kNoAttachments;

    fletcher::benchmarks::LegacyFletcherTopicType legacy(kBenchPayloadBytes);
    FletcherSamplePubSubType current;
    FastCdrFramedPubSubType fastcdr_framed;

    SerializedPayload_t legacy_payload(legacy.max_serialized_type_size);
    SerializedPayload_t current_payload(current.max_serialized_type_size);
    SerializedPayload_t fastcdr_payload(fastcdr_framed.max_serialized_type_size);
    if (!legacy.serialize(&transport, legacy_payload, kXcdr1) ||
        !current.serialize(&transport, current_payload, kXcdr1) ||
        !fastcdr_framed.serialize(&transport, fastcdr_payload, kXcdr1)) {
        std::fputs("payload-bytes check: a serialize call failed\n", stderr);
        return false;
    }

    if (fastcdr_payload.length != current_payload.length ||
        std::memcmp(fastcdr_payload.data, current_payload.data, current_payload.length) != 0) {
        std::fputs(
            "payload-bytes check: the shipped framing does not match fastcdr byte for byte\n",
            stderr);
        return false;
    }

    // Again under XCDR2, where fastcdr writes PLAIN_CDR2 instead of PLAIN_CDR. The shipped type
    // selects that id from the same argument, so it has to match there too — this is the check that
    // keeps the two hand-written representation ids honest.
    {
        const auto kXcdr2 =
            eprosima::fastdds::dds::DataRepresentationId_t::XCDR2_DATA_REPRESENTATION;
        SerializedPayload_t current_xcdr2(current.max_serialized_type_size);
        SerializedPayload_t fastcdr_xcdr2(fastcdr_framed.max_serialized_type_size);
        if (!current.serialize(&transport, current_xcdr2, kXcdr2) ||
            !fastcdr_framed.serialize(&transport, fastcdr_xcdr2, kXcdr2)) {
            std::fputs("payload-bytes check: an XCDR2 serialize call failed\n", stderr);
            return false;
        }
        if (current_xcdr2.length != fastcdr_xcdr2.length ||
            std::memcmp(current_xcdr2.data, fastcdr_xcdr2.data, fastcdr_xcdr2.length) != 0) {
            std::fputs("payload-bytes check: XCDR2 framing does not match fastcdr byte for byte\n",
                       stderr);
            return false;
        }
        if (current_xcdr2.data[1] == current_payload.data[1]) {
            std::fputs("payload-bytes check: XCDR2 reused the XCDR1 representation id\n", stderr);
            return false;
        }
    }

    int differing = 0;
    int first = -1;
    if (legacy_payload.length == current_payload.length) {
        for (uint32_t i = 0; i < current_payload.length; ++i) {
            if (legacy_payload.data[i] != current_payload.data[i]) {
                if (first < 0) first = static_cast<int>(i);
                ++differing;
            }
        }
    } else {
        std::fprintf(stderr, "payload-bytes check: lengths differ, %u vs %u\n",
                     legacy_payload.length, current_payload.length);
        return false;
    }
    std::printf(
        "payload-bytes check: current identical to fastcdr framing; legacy differs in %d byte(s), "
        "first at %d (legacy 0x%02X, current 0x%02X)\n",
        differing, first, first < 0 ? 0 : legacy_payload.data[first],
        first < 0 ? 0 : current_payload.data[first]);
    return true;
}

bool RunValidation() {
    fletcher::benchmarks::LegacyFletcherTopicType legacy(kBenchPayloadBytes);
    FletcherSamplePubSubType current;
    return ValidateSameRowBytes() && ValidatePayloadBytes() &&
           ValidateBatchRoundTrip("legacy", legacy) && ValidateBatchRoundTrip("current", current);
}

}  // namespace

int main(int argc, char** argv) {
    if (!RunValidation()) return 1;
    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}
