// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// BIND-4d-v, the native half: the per-row publish benchmark (D-BIND-37, shape D-BIND-48).
//
// ── What the arms are, and what each gap means ─────────────────────────────
//   C1  generated `TelemetryFeed_TelemetryStreamPublisher::Publish(row)`
//       THE BASELINE the acceptance bullet names. Encodes straight into the provider's window.
//   C2  `fl_publisher_publish_row` over a pre-bound batch
//       The shim with no managed code: `ToSegments`, the nanoarrow codec, the containment site.
//       This is the native half of the managed M1.
//   C3  `fl_rows_bind` → `fl_publisher_publish_row` → `fl_rows_unbind` on a one-row array
//       The native half of the managed M2 - a lone row paying for a bind. The array is built once,
//       outside the loop, because building it is the MANAGED half's cost, not this one's.
//   C4  `fl_publisher_publish_rows` over the whole batch, reported per row
//       The native half of the managed M3 - the batch mitigation B-2 names.
//
// C1 → C2 is what the binding's native layer costs over generated C++; C2 → C3 is what a per-row
// bind costs; C2 → C4 is what hoisting `ToSegments` and the crossing out of the loop buys back.
//
// ── Two things that make C1 and C2 comparable, and one that does not ───────
// ONE row, ONE schema, ONE topic: C2 opens its codec on the generated `TelemetrySchema()` itself,
// and the validation pass below refuses to benchmark at all unless C1 and C2 put byte-identical
// payloads on the wire. Both arms publish into an `inprocess` provider with no subscriber - the
// provider's own per-publish buffer and key allocation are paid on EVERY arm and cancel in the
// comparison. What does NOT cancel is the topic: the generated publisher's `TopicSegments()` is a
// function-local static, while the shim rebuilds the segment vector from an `fl_topic` on every
// `fl_publisher_publish_row`. That asymmetry is B-2, and it is what this benchmark exists to price.
//
// ── Allocation counts ──────────────────────────────────────────────────────
// `allocs_per_row` is reported only when `alloc_count.c` is LD_PRELOADed (Linux). On Windows the
// counter is absent rather than zero, because an executable cannot see allocations made inside the
// dynamically-linked shim (D-BIND-38) and a zero would read as a measurement. The count is sampled
// over `kAllocSamples` calls OUTSIDE the timed loop - Google Benchmark's own bookkeeping allocates.

#include <benchmark/benchmark.h>
#include <fletcher/abi/binding.h>
#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fletcher/pubsub/in_process_provider.hpp>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#ifndef _WIN32
#include <dlfcn.h>
#endif

#include "pubsub.fletcher.pb.h"

namespace {

namespace gen = fletcher_gen::integration::pubsub;
using GeneratedPublisher = gen::TelemetryFeed_TelemetryStreamPublisher;

constexpr int64_t kBatchRows = 1024;
constexpr int kAllocSamples = 1000;

// The row every arm publishes, C++ and C#. `dotnet/benchmarks` builds the same values; the
// validation pass pins the wire bytes they must produce.
constexpr int32_t kDeviceId = 42;
constexpr double kValue = 3.14;
constexpr int64_t kTimestamp = 1700000000000;
constexpr std::string_view kMetricName = "cpu.temperature";

// The canonical row's wire bytes, as hex. The C# harness asserts the SAME constant, which is what
// makes its numbers comparable with these: same schema, same values, same bytes. Pinned from the
// first validated run (C1 == C2); a run that disagrees stops before timing anything. Empty would
// mean "not pinned" and only print.
constexpr std::string_view kCanonicalRowHex =
    "002a0000001f85eb51b81e09400068e5cf8b0100000f0000006370752e74656d7065726174757265";

[[noreturn]] void Fail(const std::string& what) {
    std::fprintf(stderr, "bench_publish: %s\n", what.c_str());
    std::exit(2);
}

gen::Telemetry CanonicalRow() {
    gen::Telemetry row;
    row.set_device_id(kDeviceId)
        .set_value(kValue)
        .set_timestamp(kTimestamp)
        .set_metric_name(std::string(kMetricName));
    return row;
}

std::string Hex(const std::vector<uint8_t>& bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (uint8_t b : bytes) {
        out += kDigits[b >> 4];
        out += kDigits[b & 0xF];
    }
    return out;
}

// ── Allocation counter ─────────────────────────────────────────────────────

using CountFn = uint64_t (*)();

CountFn AllocCounter() {
#ifdef _WIN32
    return nullptr;
#else
    static const CountFn fn = reinterpret_cast<CountFn>(dlsym(RTLD_DEFAULT, "fbench_alloc_count"));
    return fn;
#endif
}

template <typename Call>
void RecordAllocs(benchmark::State& state, Call&& call, int64_t rows_per_call) {
    const CountFn count = AllocCounter();
    if (count == nullptr) return;
    call();  // warm: first-touch growth is not the steady state being priced
    const uint64_t before = count();
    for (int i = 0; i < kAllocSamples; ++i) call();
    const uint64_t after = count();
    state.counters["allocs_per_row"] =
        static_cast<double>(after - before) / (static_cast<double>(kAllocSamples) * rows_per_call);
}

void RecordRate(benchmark::State& state, int64_t rows_per_iteration) {
    const auto rows = static_cast<int64_t>(state.iterations()) * rows_per_iteration;
    state.SetItemsProcessed(rows);
    // Seconds per row, formatted by the library ("85.2n"), so every arm reads in one unit
    // whatever its batch size.
    state.counters["per_row"] = benchmark::Counter(
        static_cast<double>(rows), benchmark::Counter::kIsRate | benchmark::Counter::kInvert);
}

// ── Shim scaffolding ───────────────────────────────────────────────────────

struct ShimError : fl_error {
    ShimError() : fl_error{} {}
    ~ShimError() { fl_error_dispose(this); }
    ShimError(const ShimError&) = delete;
    ShimError& operator=(const ShimError&) = delete;
};

void Check(fl_status status, const ShimError& err, const char* what) {
    if (status == FL_OK) return;
    Fail(std::string(what) +
         " failed: " + std::string(reinterpret_cast<const char*>(err.message), err.message_len));
}

fl_str Str(std::string_view s) { return {reinterpret_cast<const uint8_t*>(s.data()), s.size()}; }

/// The generated topic, as an `fl_topic`. Built once: the per-publish conversion under test is the
/// one INSIDE the shim, not this one.
struct ShimTopic {
    std::vector<fl_str> segments;
    fl_topic topic{};

    ShimTopic() {
        for (const std::string& s : GeneratedPublisher::TopicSegments()) segments.push_back(Str(s));
        topic = {segments.data(), segments.size()};
    }
};

struct OwnedArray {
    ArrowArray array{};
    OwnedArray() = default;
    ~OwnedArray() {
        if (array.release != nullptr) array.release(&array);
    }
    OwnedArray(const OwnedArray&) = delete;
    OwnedArray& operator=(const OwnedArray&) = delete;
};

void RequireNa(ArrowErrorCode code, const char* what) {
    if (code != NANOARROW_OK) Fail(std::string("nanoarrow: ") + what);
}

/// `rows` copies of the canonical row, as a struct array on the generated schema.
void BuildTelemetry(OwnedArray& out, const ArrowSchema* schema, int64_t rows) {
    ArrowError error{};
    RequireNa(ArrowArrayInitFromSchema(&out.array, schema, &error), "init from schema");
    RequireNa(ArrowArrayStartAppending(&out.array), "start appending");
    if (out.array.n_children != 4) Fail("the generated Telemetry schema no longer has 4 fields");
    for (int64_t r = 0; r < rows; ++r) {
        RequireNa(ArrowArrayAppendInt(out.array.children[0], kDeviceId), "device_id");
        RequireNa(ArrowArrayAppendDouble(out.array.children[1], kValue), "value");
        RequireNa(ArrowArrayAppendInt(out.array.children[2], kTimestamp), "timestamp");
        RequireNa(ArrowArrayAppendString(out.array.children[3],
                                         ArrowStringView{kMetricName.data(),
                                                         static_cast<int64_t>(kMetricName.size())}),
                  "metric_name");
        RequireNa(ArrowArrayFinishElement(&out.array), "finish element");
    }
    RequireNa(ArrowArrayFinishBuildingDefault(&out.array, &error), "finish building");
}

/// Provider, publisher, declared topic and open codec through the shim, plus an array of
/// `rows` canonical rows. `bound()` is the array bound once; C3 binds its own per call.
class Shim {
   public:
    explicit Shim(int64_t rows) : schema_(gen::TelemetrySchema()) {
        ShimError err;
        const fl_provider_config config{0, 0, {nullptr, 0}};
        Check(fl_provider_create(Str("inprocess"), &config, &provider_, &err), err,
              "fl_provider_create");
        Check(fl_publisher_create(provider_, &publisher_, &err), err, "fl_publisher_create");
        Check(fl_publisher_create_topic(publisher_, topic_.topic, schema_.get(), &err), err,
              "fl_publisher_create_topic");
        Check(fl_codec_open(schema_.get(), &codec_, &err), err, "fl_codec_open");
        BuildTelemetry(array_, schema_.get(), rows);
        Check(fl_rows_bind(codec_, &array_.array, &bound_, &err), err, "fl_rows_bind");
    }

    ~Shim() {
        fl_rows_unbind(bound_);
        fl_codec_close(codec_);
        fl_publisher_destroy(publisher_);
        fl_provider_destroy(provider_);
    }

    Shim(const Shim&) = delete;
    Shim& operator=(const Shim&) = delete;

    fl_provider* provider() const { return provider_; }
    fl_publisher* publisher() const { return publisher_; }
    fl_topic topic() const { return topic_.topic; }
    const fl_codec* codec() const { return codec_; }
    const ArrowArray* array() const { return &array_.array; }
    const fl_rows* bound() const { return bound_; }

   private:
    fletcher::OwnedSchema schema_;
    ShimTopic topic_;
    OwnedArray array_;
    fl_provider* provider_ = nullptr;
    fl_publisher* publisher_ = nullptr;
    fl_codec* codec_ = nullptr;
    fl_rows* bound_ = nullptr;
};

// ── Validation: refuse to time arms that do not put the same bytes on the wire ──

std::vector<uint8_t> CaptureGenerated() {
    auto provider = std::make_shared<fletcher::InProcessPubSubProvider>();
    GeneratedPublisher publisher(provider);
    std::vector<uint8_t> captured;
    // The arrival is not waited on: `inprocess` delivers synchronously inside Publish.
    [[maybe_unused]] const fletcher::SubscriptionResult subscription = provider->Subscribe(
        GeneratedPublisher::TopicSegments(),
        [&](const uint8_t* data, size_t len, const fletcher::SharedSchema&,
            const fletcher::Attachments&) { captured.assign(data, data + len); });
    publisher.Publish(CanonicalRow());
    provider->Unsubscribe(GeneratedPublisher::TopicSegments());
    return captured;
}

void OnShimDelivery(void* ctx, uint64_t, const uint8_t* data, size_t len, const fl_schema*,
                    const fl_attachments*) {
    static_cast<std::vector<uint8_t>*>(ctx)->assign(data, data + len);
}

std::vector<uint8_t> CaptureShim() {
    Shim shim(1);
    std::vector<uint8_t> captured;
    ShimError err;
    fl_subscriber* subscriber = nullptr;
    Check(fl_subscriber_create(shim.provider(), &subscriber, &err), err, "fl_subscriber_create");
    uint64_t id = 0;
    fl_schema_arrival* arrival = nullptr;
    Check(fl_subscriber_subscribe(subscriber, shim.topic(), &OnShimDelivery, &captured, &id,
                                  &arrival, &err),
          err, "fl_subscriber_subscribe");
    fl_schema_arrival_dispose(arrival);
    Check(fl_publisher_publish_row(shim.publisher(), shim.topic(), shim.bound(), 0, nullptr, &err),
          err, "fl_publisher_publish_row");
    Check(fl_subscriber_unsubscribe(subscriber, id, &err), err, "fl_subscriber_unsubscribe");
    fl_subscriber_destroy(subscriber);
    return captured;
}

void Validate() {
    const std::vector<uint8_t> generated = CaptureGenerated();
    const std::vector<uint8_t> shim = CaptureShim();
    if (generated.empty()) Fail("validation: the generated publisher delivered nothing");
    if (generated != shim) {
        Fail("validation: C1 and C2 put different bytes on the wire\n  C1 " + Hex(generated) +
             "\n  C2 " + Hex(shim));
    }
    const std::string hex = Hex(generated);
    if (!kCanonicalRowHex.empty() && hex != kCanonicalRowHex) {
        Fail("validation: the canonical row no longer encodes to kCanonicalRowHex\n  got      " +
             hex + "\n  expected " + std::string(kCanonicalRowHex));
    }
    std::printf("validation: C1 == C2, %zu bytes: %s%s\n", generated.size(), hex.c_str(),
                kCanonicalRowHex.empty() ? "  (kCanonicalRowHex not pinned)" : "");
    std::printf("allocation counter: %s\n", AllocCounter() != nullptr
                                                ? "present (allocs_per_row reported)"
                                                : "absent (timings only; see alloc_count.c)");
}

// ── The arms ───────────────────────────────────────────────────────────────

void BM_C1_GeneratedPublish(benchmark::State& state) {
    auto provider = std::make_shared<fletcher::InProcessPubSubProvider>();
    GeneratedPublisher publisher(provider);
    const gen::Telemetry row = CanonicalRow();

    RecordAllocs(state, [&] { publisher.Publish(row); }, 1);
    for (auto _ : state) publisher.Publish(row);
    RecordRate(state, 1);
}

void BM_C2_ShimPublishRow(benchmark::State& state) {
    Shim shim(kBatchRows);
    ShimError err;
    int64_t i = 0;
    auto publish = [&] {
        Check(fl_publisher_publish_row(shim.publisher(), shim.topic(), shim.bound(), i, nullptr,
                                       &err),
              err, "fl_publisher_publish_row");
        i = (i + 1 == kBatchRows) ? 0 : i + 1;
    };

    RecordAllocs(state, publish, 1);
    for (auto _ : state) publish();
    RecordRate(state, 1);
}

void BM_C3_ShimBindPublishUnbind(benchmark::State& state) {
    Shim shim(1);
    ShimError err;
    auto publish = [&] {
        fl_rows* rows = nullptr;
        Check(fl_rows_bind(shim.codec(), shim.array(), &rows, &err), err, "fl_rows_bind");
        Check(fl_publisher_publish_row(shim.publisher(), shim.topic(), rows, 0, nullptr, &err), err,
              "fl_publisher_publish_row");
        fl_rows_unbind(rows);
    };

    RecordAllocs(state, publish, 1);
    for (auto _ : state) publish();
    RecordRate(state, 1);
}

void BM_C4_ShimPublishRows(benchmark::State& state) {
    Shim shim(kBatchRows);
    ShimError err;
    auto publish = [&] {
        Check(fl_publisher_publish_rows(shim.publisher(), shim.topic(), shim.bound(), 0, kBatchRows,
                                        nullptr, &err),
              err, "fl_publisher_publish_rows");
    };

    RecordAllocs(state, publish, kBatchRows);
    for (auto _ : state) publish();
    RecordRate(state, kBatchRows);
}

// WALL time, not CPU time. Every arm is single-threaded and never blocks, so the two agree in
// principle - but Windows' per-thread CPU clock ticks coarsely enough that a rate derived from it
// quantises: the first recorded run read C1's `per_row` as exactly 188.918 ns in both passes while
// its wall time moved. `per_row` is derived from whatever the arm is timed on, so this is also
// what makes it comparable with BenchmarkDotNet's Mean, which is wall time.
BENCHMARK(BM_C1_GeneratedPublish)->UseRealTime();
BENCHMARK(BM_C2_ShimPublishRow)->UseRealTime();
BENCHMARK(BM_C3_ShimBindPublishUnbind)->UseRealTime();
BENCHMARK(BM_C4_ShimPublishRows)->UseRealTime();

}  // namespace

int main(int argc, char** argv) {
    Validate();
    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}
