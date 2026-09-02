// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// Arrow-tier serialization benchmark suite: what fletcher::Codec, the positional wire format it
// implements, and the SubscriberArrow batched-decode path cost, across seven row shapes (see
// fixtures.hpp). BASELINE run against the packages in the Conan cache pinned by conan.lock — see
// README.md.
//
// Arms, per shape (all seven unless noted):
//   BM_Encode_Codec_S              Codec::EncodeRow of a prebuilt ArrowRow.
//   BM_Encode_Positional_S         the hand-written PositionalWriter twin into a FixedWriteBuffer —
//                                  the generated-code floor.
//   BM_Decode_Row_S                Codec::DecodeRow of prebuilt bytes.
//   BM_Decode_Positional_S         the hand-written PositionalReader twin — the floor.
//   BM_Memcpy_S                    the wire bytes moved once, nothing else.
//   BM_Decode_Batch_ScalarPath_S/N the CURRENT batched decode (SubscriberArrow's BuildBatch,
//                                  reproduced verbatim), N pre-decoded rows in, one RecordBatch
//                                  out.
//   BM_Decode_Batch_S/N            the new batched decode (fletcher::BatchDecoder), N pre-encoded
//                                  wire rows in, one RecordBatch out — Reserve/Append x N/Finish,
//                                  the same cycle SubscriberArrow::RecordBatchBatcher::Flush runs.
//   BM_Encode_Codec_S_IntoFixed    Codec::EncodeRow(row, FixedWriteBuffer) into a preallocated
//                                  buffer — the provider-buffer path BM_Encode_Codec_S's vector
//                                  overload does not exercise.
//   BM_ArrowIpc_Write_S/N          arrow::ipc stream writer over one N-row RecordBatch.
//   BM_ArrowIpc_Read_S/N           arrow::ipc stream reader over the bytes BM_ArrowIpc_Write_S/N
//                                  produces.
//   BM_EndToEnd_Batched_S/N        PublisherArrow.Publish, N rows through a loopback provider, into
//                                  a batched SubscriberArrow (BatchOptions{N, 1 min}) — time per N
//                                  rows.
//
// Plus, standalone: BM_Run_PerElement_2667f vs BM_Run_Memcpy_2667f — filling an arrow::FloatBuilder
// from 2667 unaligned floats one at a time against one bulk memcpy, the same comparison
// Codec::DecodeListElements makes for a primitive list run (codec.cpp:224-240).
//
// A validation pass runs first and fails the process (non-zero exit, no benchmarks run) if any
// shape's Codec bytes disagree with its positional twin, a decoded value disagrees with the row
// that produced it, or fletcher::BatchDecoder's output (Finish()'d over the same 3 rows) disagrees
// with Codec::DecodeRow cell-for-cell, or fails RecordBatch::ValidateFull().
#include <arrow/api.h>
#include <arrow/c/bridge.h>
#include <arrow/io/api.h>
#include <arrow/ipc/reader.h>
#include <arrow/ipc/writer.h>
#include <benchmark/benchmark.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fletcher/arrow_bridge/batch_decoder.hpp>
#include <fletcher/arrow_bridge/codec.hpp>
#include <fletcher/core/positional_io.hpp>
#include <fletcher/core/write_buffer.hpp>
#include <fletcher/pubsub/owned_schema.hpp>
#include <fletcher/pubsub/provider.hpp>
#include <fletcher/pubsub_arrow/publisher_arrow.hpp>
#include <fletcher/pubsub_arrow/subscriber_arrow.hpp>
#include <memory>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

#include "fixtures.hpp"

// ---------------------------------------------------------------------------
// Allocation counting — a thread-local counter bumped by every operator new, sampled around one
// sample call outside the timed loop (per-iteration counting would itself allocate via the
// benchmark harness's own bookkeeping). Default ON for this target (see CMakeLists.txt); with the
// macro undefined, this whole section — and the allocs_per_row counters below — compiles out.
// ---------------------------------------------------------------------------

#ifdef FLETCHER_BENCH_COUNT_ALLOCS
namespace fletcher_bench {
thread_local uint64_t g_alloc_count = 0;
}  // namespace fletcher_bench

void* operator new(std::size_t sz) {
    ++fletcher_bench::g_alloc_count;
    if (sz == 0) sz = 1;
    void* p = std::malloc(sz);
    if (!p) throw std::bad_alloc();
    return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
#endif  // FLETCHER_BENCH_COUNT_ALLOCS

namespace {

using fletcher::ArrowRow;

// ---------------------------------------------------------------------------
// Loopback provider — same shape as MockProvider in pubsub-arrow/tests/test_pubsub_arrow.cpp:25-86.
// Encodes into a VectorWriteBuffer and delivers synchronously to the subscriber callback with the
// schema, on the calling thread — no DDS, no network, nothing measured but the Arrow-tier code
// above it.
// ---------------------------------------------------------------------------

class LoopbackProvider : public fletcher::PubSubProvider {
   public:
    void CreateTopic(const std::vector<std::string>& segments,
                     fletcher::OwnedSchema schema) override {
        std::string key = Join(segments);
        if (schema) schemas_[key] = fletcher::OwnedSchema::DeepCopy(schema.get());
    }

    void Publish(const std::vector<std::string>& segments, const RowEncoder& encoder,
                 const fletcher::Attachments& attachments) override {
        std::string key = Join(segments);
        fletcher::VectorWriteBuffer wb;
        encoder(wb);
        const std::vector<uint8_t> buf = wb.Finish();

        auto it = callbacks_.find(key);
        if (it != callbacks_.end()) {
            fletcher::SharedSchema sp;
            auto sit = schemas_.find(key);
            if (sit != schemas_.end()) {
                sp = fletcher::MakeSharedSchema(fletcher::OwnedSchema::DeepCopy(sit->second.get()));
            }
            it->second(buf.data(), buf.size(), sp, attachments);
        }
    }

    fletcher::SubscriptionResult Subscribe(const std::vector<std::string>& segments,
                                           SubscribeCallback callback) override {
        std::string key = Join(segments);
        callbacks_[key] = std::move(callback);
        fletcher::SharedSchema schema;
        auto it = schemas_.find(key);
        if (it != schemas_.end()) {
            schema = fletcher::MakeSharedSchema(fletcher::OwnedSchema::DeepCopy(it->second.get()));
        }
        return {fletcher::MakeReadySchemaFuture(std::move(schema))};
    }

    void Unsubscribe(const std::vector<std::string>& segments) override {
        callbacks_.erase(Join(segments));
    }

   private:
    std::unordered_map<std::string, SubscribeCallback> callbacks_;
    std::unordered_map<std::string, fletcher::OwnedSchema> schemas_;

    static std::string Join(const std::vector<std::string>& segs) {
        std::string out;
        for (size_t i = 0; i < segs.size(); ++i) {
            if (i > 0) out += '/';
            out += segs[i];
        }
        return out;
    }
};

// ---------------------------------------------------------------------------
// Per-shape validation — Codec bytes must equal the positional twin's bytes, and decode must
// round-trip every column value. Runs before any benchmark; a failure here is a finding (a real
// codec/twin disagreement), not something to paper over — see the module comment in fixtures.hpp.
// ---------------------------------------------------------------------------

template <typename RowValuesT, std::shared_ptr<arrow::Schema> (*SchemaFn)(),
          ArrowRow (*MakeRowFn)(int64_t), RowValuesT (*MakeRowValuesFn)(int64_t),
          void (*WritePositionalFn)(const RowValuesT&, fletcher::WriteBuffer&),
          RowValuesT (*ReadPositionalFn)(const uint8_t*, size_t)>
bool ValidateShape(const char* name) {
    auto schema = SchemaFn();
    fletcher::Codec codec(schema);

    for (int64_t i = 0; i < 3; ++i) {
        ArrowRow row = MakeRowFn(i);
        fletcher::EncodedRow codec_bytes = codec.EncodeRow(row);

        fletcher::VectorWriteBuffer wb;
        WritePositionalFn(MakeRowValuesFn(i), wb);
        std::vector<uint8_t> twin_bytes = wb.Finish();

        if (codec_bytes != twin_bytes) {
            size_t n =
                codec_bytes.size() < twin_bytes.size() ? codec_bytes.size() : twin_bytes.size();
            size_t first_diff = n;
            for (size_t k = 0; k < n; ++k) {
                if (codec_bytes[k] != twin_bytes[k]) {
                    first_diff = k;
                    break;
                }
            }
            std::fprintf(
                stderr,
                "%s: row %lld: Codec bytes (%zu B) != positional twin bytes (%zu B), first "
                "differing offset %zu\n",
                name, static_cast<long long>(i), codec_bytes.size(), twin_bytes.size(), first_diff);
            return false;
        }

        // The twin's own read side must also parse without throwing (exercised for real by
        // BM_Decode_Positional_S below); a throw here would mean the write/read twins disagree with
        // each other even though both agree with Codec on the bytes.
        (void)ReadPositionalFn(codec_bytes.data(), codec_bytes.size());

        ArrowRow decoded = codec.DecodeRow(codec_bytes);
        if (decoded.size() != row.size()) {
            std::fprintf(stderr, "%s: row %lld: decoded field count %zu != %zu\n", name,
                         static_cast<long long>(i), decoded.size(), row.size());
            return false;
        }
        for (size_t c = 0; c < row.size(); ++c) {
            if (!decoded[c]->Equals(*row[c])) {
                std::fprintf(stderr, "%s: row %lld: column %zu differs after decode (%s vs %s)\n",
                             name, static_cast<long long>(i), c, decoded[c]->ToString().c_str(),
                             row[c]->ToString().c_str());
                return false;
            }
        }
    }

    // fletcher::BatchDecoder must agree with Codec::DecodeRow cell-for-cell over the same 3 rows in
    // one batch (the differential oracle test_batch_decoder.cpp leans on), and the batch it
    // produces must pass ValidateFull(). For the dictionary column (Generic's "dict" field),
    // DecodeRow yields a plain value scalar (see codec.hpp) while BatchDecoder yields a real
    // dictionary column, so GetEncodedValue() resolves the index before comparing.
    {
        fletcher::BatchDecoder decoder(schema);
        std::vector<fletcher::EncodedRow> all_bytes;
        all_bytes.reserve(3);
        for (int64_t i = 0; i < 3; ++i) {
            fletcher::EncodedRow bytes = codec.EncodeRow(MakeRowFn(i));
            decoder.Append(bytes.data(), bytes.size());
            all_bytes.push_back(std::move(bytes));
        }
        std::shared_ptr<arrow::RecordBatch> batch = decoder.Finish();
        arrow::Status vs = batch->ValidateFull();
        if (!vs.ok()) {
            std::fprintf(stderr, "%s: BatchDecoder output failed ValidateFull: %s\n", name,
                         vs.ToString().c_str());
            return false;
        }
        for (int64_t r = 0; r < 3; ++r) {
            ArrowRow oracle_row = codec.DecodeRow(all_bytes[static_cast<size_t>(r)]);
            for (int c = 0; c < batch->num_columns(); ++c) {
                std::shared_ptr<arrow::Scalar> got = batch->column(c)->GetScalar(r).ValueOrDie();
                if (got->type->id() == arrow::Type::DICTIONARY) {
                    got = static_cast<const arrow::DictionaryScalar&>(*got)
                              .GetEncodedValue()
                              .ValueOrDie();
                }
                if (!got->Equals(*oracle_row[static_cast<size_t>(c)])) {
                    std::fprintf(stderr,
                                 "%s: BatchDecoder mismatch: row %lld col %d: got %s, want %s\n",
                                 name, static_cast<long long>(r), c, got->ToString().c_str(),
                                 oracle_row[static_cast<size_t>(c)]->ToString().c_str());
                    return false;
                }
            }
        }
    }

    std::printf(
        "%-12s validated: 3 rows, Codec bytes == positional twin, decode round-trips, "
        "BatchDecoder agrees + ValidateFull (row %zu B)\n",
        name, codec.EncodeRow(MakeRowFn(0)).size());
    return true;
}

// ---------------------------------------------------------------------------
// Per-shape benchmark registration.
// ---------------------------------------------------------------------------

template <typename RowValuesT, std::shared_ptr<arrow::Schema> (*SchemaFn)(),
          ArrowRow (*MakeRowFn)(int64_t), RowValuesT (*MakeRowValuesFn)(int64_t),
          void (*WritePositionalFn)(const RowValuesT&, fletcher::WriteBuffer&),
          RowValuesT (*ReadPositionalFn)(const uint8_t*, size_t)>
void RegisterShape(const std::string& name) {
    auto schema = SchemaFn();
    auto codec = std::make_shared<fletcher::Codec>(schema);
    const size_t row_bytes = codec->EncodeRow(MakeRowFn(0)).size();

    // BM_Encode_Codec_<name> — Codec::EncodeRow of a prebuilt ArrowRow.
    benchmark::RegisterBenchmark(("BM_Encode_Codec_" + name).c_str(), [=](benchmark::State& state) {
        ArrowRow row = MakeRowFn(0);
        for (auto _ : state) {
            fletcher::EncodedRow bytes = codec->EncodeRow(row);
            benchmark::DoNotOptimize(bytes.data());
        }
#ifdef FLETCHER_BENCH_COUNT_ALLOCS
        {
            uint64_t before = fletcher_bench::g_alloc_count;
            fletcher::EncodedRow one = codec->EncodeRow(row);
            benchmark::DoNotOptimize(one.data());
            state.counters["allocs_per_row"] =
                static_cast<double>(fletcher_bench::g_alloc_count - before);
        }
#endif
        state.counters["bytes"] = static_cast<double>(row_bytes);
        state.SetItemsProcessed(state.iterations());
    });

    // BM_Encode_Codec_<name>_IntoFixed — Codec::EncodeRow(row, WriteBuffer&) into a preallocated
    // FixedWriteBuffer: the provider-buffer path (what a RowEncoder callback actually calls), as
    // opposed to BM_Encode_Codec_<name>'s fresh-vector overload above.
    benchmark::RegisterBenchmark(
        ("BM_Encode_Codec_" + name + "_IntoFixed").c_str(), [=](benchmark::State& state) {
            ArrowRow row = MakeRowFn(0);
            std::vector<uint8_t> scratch(row_bytes + 64);
            for (auto _ : state) {
                fletcher::FixedWriteBuffer buf(scratch.data(), scratch.size());
                codec->EncodeRow(row, buf);
                benchmark::DoNotOptimize(scratch.data());
            }
#ifdef FLETCHER_BENCH_COUNT_ALLOCS
            {
                uint64_t before = fletcher_bench::g_alloc_count;
                fletcher::FixedWriteBuffer buf(scratch.data(), scratch.size());
                codec->EncodeRow(row, buf);
                benchmark::DoNotOptimize(scratch.data());
                state.counters["allocs_per_row"] =
                    static_cast<double>(fletcher_bench::g_alloc_count - before);
            }
#endif
            state.counters["bytes"] = static_cast<double>(row_bytes);
            state.SetItemsProcessed(state.iterations());
        });

    // BM_Encode_Positional_<name> — the hand-written twin into a FixedWriteBuffer: the
    // generated-code floor.
    benchmark::RegisterBenchmark(
        ("BM_Encode_Positional_" + name).c_str(), [=](benchmark::State& state) {
            RowValuesT values = MakeRowValuesFn(0);
            std::vector<uint8_t> scratch(row_bytes + 64);
            for (auto _ : state) {
                fletcher::FixedWriteBuffer buf(scratch.data(), scratch.size());
                WritePositionalFn(values, buf);
                benchmark::DoNotOptimize(scratch.data());
            }
            state.counters["bytes"] = static_cast<double>(row_bytes);
            state.SetItemsProcessed(state.iterations());
        });

    // BM_Decode_Row_<name> — Codec::DecodeRow of prebuilt bytes.
    benchmark::RegisterBenchmark(("BM_Decode_Row_" + name).c_str(), [=](benchmark::State& state) {
        fletcher::EncodedRow bytes = codec->EncodeRow(MakeRowFn(0));
        for (auto _ : state) {
            ArrowRow row = codec->DecodeRow(bytes.data(), bytes.size());
            benchmark::DoNotOptimize(row.data());
        }
#ifdef FLETCHER_BENCH_COUNT_ALLOCS
        {
            uint64_t before = fletcher_bench::g_alloc_count;
            ArrowRow one = codec->DecodeRow(bytes.data(), bytes.size());
            benchmark::DoNotOptimize(one.data());
            state.counters["allocs_per_row"] =
                static_cast<double>(fletcher_bench::g_alloc_count - before);
        }
#endif
        state.counters["bytes"] = static_cast<double>(row_bytes);
        state.SetItemsProcessed(state.iterations());
    });

    // BM_Decode_Positional_<name> — the hand-written twin: the floor. ClobberMemory per iteration
    // (bench_pub_sub_type.cpp:334): without it a decode of a buffer that never changes is free to
    // be hoisted out of the loop.
    benchmark::RegisterBenchmark(
        ("BM_Decode_Positional_" + name).c_str(), [=](benchmark::State& state) {
            fletcher::EncodedRow bytes = codec->EncodeRow(MakeRowFn(0));
            for (auto _ : state) {
                benchmark::ClobberMemory();
                RowValuesT values = ReadPositionalFn(bytes.data(), bytes.size());
                benchmark::DoNotOptimize(&values);
            }
            state.counters["bytes"] = static_cast<double>(row_bytes);
            state.SetItemsProcessed(state.iterations());
        });

    // BM_Memcpy_<name> — the wire bytes moved once, nothing else.
    benchmark::RegisterBenchmark(("BM_Memcpy_" + name).c_str(), [=](benchmark::State& state) {
        fletcher::EncodedRow bytes = codec->EncodeRow(MakeRowFn(0));
        std::vector<uint8_t> dest(bytes.size());
        for (auto _ : state) {
            std::memcpy(dest.data(), bytes.data(), bytes.size());
            benchmark::DoNotOptimize(dest.data());
        }
        state.counters["bytes"] = static_cast<double>(row_bytes);
        state.SetItemsProcessed(state.iterations());
    });

    // BM_Decode_Batch_ScalarPath_<name>/N — the CURRENT batched decode
    // (subscriber_arrow.cpp:177-205 BuildBatch), reproduced verbatim over N pre-decoded rows. N in
    // {1, 1000, 8000}.
    benchmark::RegisterBenchmark(
        ("BM_Decode_Batch_ScalarPath_" + name).c_str(),
        [=](benchmark::State& state) {
            const int64_t n = state.range(0);
            std::vector<ArrowRow> rows;
            rows.reserve(static_cast<size_t>(n));
            for (int64_t i = 0; i < n; ++i) rows.push_back(MakeRowFn(i));

            for (auto _ : state) {
                auto batch = fletcher::benchmarks::BuildBatchScalarPath(schema, rows);
                benchmark::DoNotOptimize(batch.get());
            }
#ifdef FLETCHER_BENCH_COUNT_ALLOCS
            if (n == 1000) {
                uint64_t before = fletcher_bench::g_alloc_count;
                auto one = fletcher::benchmarks::BuildBatchScalarPath(schema, rows);
                benchmark::DoNotOptimize(one.get());
                state.counters["allocs_per_row"] =
                    static_cast<double>(fletcher_bench::g_alloc_count - before) /
                    static_cast<double>(n);
            }
#endif
            state.SetItemsProcessed(state.iterations() * n);
        })
        ->Arg(1)
        ->Arg(1000)
        ->Arg(8000);

    // BM_Decode_Batch_<name>/N — the NEW batched decode (fletcher::BatchDecoder), N pre-encoded
    // wire rows in, one RecordBatch out. One decoder built outside the timed loop; each iteration
    // repeats the Reserve/Append x N/Finish cycle SubscriberArrow::RecordBatchBatcher::Flush runs
    // per batch window (Reserve(max_rows_) right after Finish(), ready for the next window — see
    // subscriber_arrow.cpp). N in {1, 1000, 8000}.
    benchmark::RegisterBenchmark(
        ("BM_Decode_Batch_" + name).c_str(),
        [=](benchmark::State& state) {
            const int64_t n = state.range(0);
            std::vector<fletcher::EncodedRow> rows_bytes;
            rows_bytes.reserve(static_cast<size_t>(n));
            for (int64_t i = 0; i < n; ++i) rows_bytes.push_back(codec->EncodeRow(MakeRowFn(i)));

            fletcher::BatchDecoder decoder(schema);
            for (auto _ : state) {
                decoder.Reserve(n);
                for (int64_t i = 0; i < n; ++i) {
                    const auto& bytes = rows_bytes[static_cast<size_t>(i)];
                    decoder.Append(bytes.data(), bytes.size());
                }
                auto batch = decoder.Finish();
                benchmark::DoNotOptimize(batch.get());
            }
#ifdef FLETCHER_BENCH_COUNT_ALLOCS
            if (n == 1000) {
                uint64_t before = fletcher_bench::g_alloc_count;
                decoder.Reserve(n);
                for (int64_t i = 0; i < n; ++i) {
                    const auto& bytes = rows_bytes[static_cast<size_t>(i)];
                    decoder.Append(bytes.data(), bytes.size());
                }
                auto one = decoder.Finish();
                benchmark::DoNotOptimize(one.get());
                state.counters["allocs_per_row"] =
                    static_cast<double>(fletcher_bench::g_alloc_count - before) /
                    static_cast<double>(n);
            }
#endif
            state.counters["bytes"] = static_cast<double>(row_bytes);
            state.SetItemsProcessed(state.iterations() * n);
        })
        ->Arg(1)
        ->Arg(1000)
        ->Arg(8000);

    // BM_ArrowIpc_Write_<name>/N — stream writer over one N-row RecordBatch. N in {1000, 8000}.
    benchmark::RegisterBenchmark(
        ("BM_ArrowIpc_Write_" + name).c_str(),
        [=](benchmark::State& state) {
            const int64_t n = state.range(0);
            std::vector<ArrowRow> rows;
            rows.reserve(static_cast<size_t>(n));
            for (int64_t i = 0; i < n; ++i) rows.push_back(MakeRowFn(i));
            auto batch = fletcher::benchmarks::BuildBatchScalarPath(schema, rows);

            for (auto _ : state) {
                auto out = arrow::io::BufferOutputStream::Create().ValueOrDie();
                auto writer = arrow::ipc::MakeStreamWriter(out, schema).ValueOrDie();
                if (!writer->WriteRecordBatch(*batch).ok()) {
                    state.SkipWithError("WriteRecordBatch failed");
                    break;
                }
                if (!writer->Close().ok()) {
                    state.SkipWithError("writer Close failed");
                    break;
                }
                auto buf = out->Finish().ValueOrDie();
                benchmark::DoNotOptimize(buf->data());
            }
            state.SetItemsProcessed(state.iterations() * n);
        })
        ->Arg(1000)
        ->Arg(8000);

    // BM_ArrowIpc_Read_<name>/N — stream reader over the bytes the write arm produces.
    benchmark::RegisterBenchmark(
        ("BM_ArrowIpc_Read_" + name).c_str(),
        [=](benchmark::State& state) {
            const int64_t n = state.range(0);
            std::vector<ArrowRow> rows;
            rows.reserve(static_cast<size_t>(n));
            for (int64_t i = 0; i < n; ++i) rows.push_back(MakeRowFn(i));
            auto batch = fletcher::benchmarks::BuildBatchScalarPath(schema, rows);

            auto out = arrow::io::BufferOutputStream::Create().ValueOrDie();
            auto writer = arrow::ipc::MakeStreamWriter(out, schema).ValueOrDie();
            if (!writer->WriteRecordBatch(*batch).ok() || !writer->Close().ok()) {
                std::fprintf(stderr, "%s: IPC read fixture serialize failed\n", name.c_str());
                std::abort();
            }
            auto serialized = out->Finish().ValueOrDie();

            for (auto _ : state) {
                auto buf_reader = std::make_shared<arrow::io::BufferReader>(serialized);
                auto reader = arrow::ipc::RecordBatchStreamReader::Open(buf_reader).ValueOrDie();
                std::shared_ptr<arrow::RecordBatch> rb;
                if (!reader->ReadNext(&rb).ok()) {
                    state.SkipWithError("ReadNext failed");
                    break;
                }
                benchmark::DoNotOptimize(rb.get());
            }
            state.SetItemsProcessed(state.iterations() * n);
        })
        ->Arg(1000)
        ->Arg(8000);

    // BM_EndToEnd_Batched_<name>/N — PublisherArrow.Publish, N rows through a loopback provider,
    // into a batched SubscriberArrow (BatchOptions{N, 1 min}); the callback increments a counter.
    // Time per N rows.
    benchmark::RegisterBenchmark(("BM_EndToEnd_Batched_" + name).c_str(),
                                 [=, schema_capture = schema](benchmark::State& state) {
                                     const int64_t n = state.range(0);
                                     auto provider = std::make_shared<LoopbackProvider>();
                                     fletcher::PublisherArrow pub(provider);
                                     fletcher::SubscriberArrow sub(provider);
                                     const std::vector<std::string> segments = {"bench", name};
                                     pub.CreateTopic(segments, schema_capture);

                                     std::atomic<int64_t> received{0};
                                     fletcher::SubscriberArrow::BatchOptions options;
                                     options.max_rows = n;
                                     options.timeout = std::chrono::minutes(1);
                                     sub.Subscribe(
                                         segments,
                                         [&received](std::shared_ptr<arrow::RecordBatch> batch,
                                                     std::vector<fletcher::Attachments>,
                                                     fletcher::SubscriberArrow::BatchStatus) {
                                             received += batch ? batch->num_rows() : 0;
                                         },
                                         options);

                                     for (auto _ : state) {
                                         for (int64_t i = 0; i < n; ++i)
                                             pub.Publish(segments, MakeRowFn(i));
                                     }
                                     benchmark::DoNotOptimize(received.load());
                                     state.SetItemsProcessed(state.iterations() * n);
                                 })
        ->Arg(1000)
        ->Arg(8000);
}

// ---------------------------------------------------------------------------
// Extra: BM_Run_PerElement_2667f vs BM_Run_Memcpy_2667f — the same comparison
// Codec::DecodeListElements makes for a primitive list run (codec.cpp AppendPrimitiveRun),
// isolated: filling an arrow::FloatBuilder from 2667 floats that sit at an unaligned offset in
// their backing buffer, one at a time against one bulk memcpy.
// ---------------------------------------------------------------------------

void RegisterExtraArms() {
    constexpr int64_t kN = 2667;
    // A static buffer, floats starting 1 byte in so the source is deliberately unaligned.
    static std::vector<uint8_t> storage(static_cast<size_t>(kN) * sizeof(float) + 1);
    for (int64_t i = 0; i < kN; ++i) {
        float f = static_cast<float>(i) * 0.001f;
        std::memcpy(storage.data() + 1 + static_cast<size_t>(i) * sizeof(float), &f, sizeof(float));
    }
    const uint8_t* p = storage.data() + 1;

    benchmark::RegisterBenchmark("BM_Run_PerElement_2667f", [p](benchmark::State& state) {
        for (auto _ : state) {
            arrow::FloatBuilder builder;
            if (!builder.Reserve(kN).ok()) {
                state.SkipWithError("Reserve failed");
                break;
            }
            for (int64_t i = 0; i < kN; ++i) {
                float v;
                std::memcpy(&v, p + sizeof(float) * static_cast<size_t>(i), sizeof(float));
                builder.UnsafeAppend(v);
            }
            auto arr = builder.Finish().ValueOrDie();
            benchmark::DoNotOptimize(arr.get());
        }
        state.SetItemsProcessed(state.iterations() * kN);
    });

    benchmark::RegisterBenchmark("BM_Run_Memcpy_2667f", [p](benchmark::State& state) {
        for (auto _ : state) {
            arrow::FloatBuilder builder;
            if (!builder.Reserve(kN).ok()) {
                state.SkipWithError("Reserve failed");
                break;
            }
            std::memcpy(builder.GetMutableValue(builder.length()), p,
                        static_cast<size_t>(kN) * sizeof(float));
            builder.UnsafeAdvance(kN);
            auto arr = builder.Finish().ValueOrDie();
            benchmark::DoNotOptimize(arr.get());
        }
        state.SetItemsProcessed(state.iterations() * kN);
    });
}

// ---------------------------------------------------------------------------
// Validation + registration driver
// ---------------------------------------------------------------------------

bool RunValidation() {
    namespace b = fletcher::benchmarks;
    bool ok = true;
    ok = ValidateShape<b::scalars10::RowValues, b::scalars10::Schema, b::scalars10::MakeRow,
                       b::scalars10::MakeRowValues, b::scalars10::WritePositional,
                       b::scalars10::ReadPositional>("Scalars10") &&
         ok;
    ok = ValidateShape<b::nullable10::RowValues, b::nullable10::Schema, b::nullable10::MakeRow,
                       b::nullable10::MakeRowValues, b::nullable10::WritePositional,
                       b::nullable10::ReadPositional>("Nullable10") &&
         ok;
    ok =
        ValidateShape<b::cloud::RowValues, b::cloud::Schema, b::cloud::MakeRow,
                      b::cloud::MakeRowValues, b::cloud::WritePositional, b::cloud::ReadPositional>(
            "Cloud") &&
        ok;
    ok =
        ValidateShape<b::pose::RowValues, b::pose::Schema, b::pose::MakeRow, b::pose::MakeRowValues,
                      b::pose::WritePositional, b::pose::ReadPositional>("Pose") &&
        ok;
    ok = ValidateShape<b::points::RowValues, b::points::Schema, b::points::MakeRow,
                       b::points::MakeRowValues, b::points::WritePositional,
                       b::points::ReadPositional>("Points") &&
         ok;
    ok = ValidateShape<b::nested::RowValues, b::nested::Schema, b::nested::MakeRow,
                       b::nested::MakeRowValues, b::nested::WritePositional,
                       b::nested::ReadPositional>("Nested") &&
         ok;
    ok = ValidateShape<b::generic::RowValues, b::generic::Schema, b::generic::MakeRow,
                       b::generic::MakeRowValues, b::generic::WritePositional,
                       b::generic::ReadPositional>("Generic") &&
         ok;
    return ok;
}

void RegisterAllShapes() {
    namespace b = fletcher::benchmarks;
    RegisterShape<b::scalars10::RowValues, b::scalars10::Schema, b::scalars10::MakeRow,
                  b::scalars10::MakeRowValues, b::scalars10::WritePositional,
                  b::scalars10::ReadPositional>("Scalars10");
    RegisterShape<b::nullable10::RowValues, b::nullable10::Schema, b::nullable10::MakeRow,
                  b::nullable10::MakeRowValues, b::nullable10::WritePositional,
                  b::nullable10::ReadPositional>("Nullable10");
    RegisterShape<b::cloud::RowValues, b::cloud::Schema, b::cloud::MakeRow, b::cloud::MakeRowValues,
                  b::cloud::WritePositional, b::cloud::ReadPositional>("Cloud");
    RegisterShape<b::pose::RowValues, b::pose::Schema, b::pose::MakeRow, b::pose::MakeRowValues,
                  b::pose::WritePositional, b::pose::ReadPositional>("Pose");
    RegisterShape<b::points::RowValues, b::points::Schema, b::points::MakeRow,
                  b::points::MakeRowValues, b::points::WritePositional, b::points::ReadPositional>(
        "Points");
    RegisterShape<b::nested::RowValues, b::nested::Schema, b::nested::MakeRow,
                  b::nested::MakeRowValues, b::nested::WritePositional, b::nested::ReadPositional>(
        "Nested");
    RegisterShape<b::generic::RowValues, b::generic::Schema, b::generic::MakeRow,
                  b::generic::MakeRowValues, b::generic::WritePositional,
                  b::generic::ReadPositional>("Generic");
}

}  // namespace

int main(int argc, char** argv) {
    if (!RunValidation()) return 1;
    RegisterAllShapes();
    RegisterExtraArms();

    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}
