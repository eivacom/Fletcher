// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// End-to-end publish-to-callback latency and throughput, intraprocess, through two real providers
// (publisher + subscriber, one process, domain 43), against Fletcher's built-in QoS throughout.
//
// Latency: one sample in flight at a time. The publisher stamps steady_clock::now() (nanoseconds
// since an unspecified epoch) into the row's first 8 bytes; the subscriber callback reads it back
// and computes now() minus the stamp -- the delivery latency, not a round trip. Publishes are paced
// ~100 us apart with a busy-wait so every sample finds the receiver idle: the wake-up cost is what
// this arm is for.
//
// Throughput: the same two providers, one row size, publish 200 000 samples back to back with no
// pacing; wall time runs until the callback has counted all of them.
//
// --arm latency|throughput|all, --bytes 198|60000|all run a single arm instead of the full sweep.
//
// Every arm runs on its own thread under a 30 s watchdog (RunGuarded): if the thread has not
// finished by then -- most likely pub.Publish() itself blocked forever in write(), which no
// in-loop deadline can catch because the loop never returns to check one -- this prints
// `STALL arm=... bytes=... arrived=x/y` and calls std::_Exit(1) immediately instead of returning
// normally. A normal return would destroy the two FastDDSPubSubProvider instances on the wedged
// thread's stack, and that teardown can itself wait on the same stuck internals; _Exit skips it on
// purpose. The two in-loop "LOST" waits below stay bounded well under 30 s so a partial loss (some
// samples arrived, none are still coming) is reported as LOST rather than mistaken for a STALL.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fletcher/core/write_buffer.hpp>
#include <fletcher/fastdds_pubsub_provider/fast_dds_pubsub_provider.hpp>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace fletcher;

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kLatencyWarmup = 2000;
constexpr int kLatencyIters = 20000;
constexpr int kThroughputIters = 200000;
constexpr auto kPace = std::chrono::microseconds(100);
constexpr size_t kRowSizes[] = {198, 60000};

OwnedSchema MakeSchema() {
    OwnedSchema s;
    ArrowSchemaInit(s.get());
    ArrowSchemaSetTypeStruct(s.get(), 1);
    ArrowSchemaSetName(s->children[0], "x");
    ArrowSchemaSetType(s->children[0], NANOARROW_TYPE_INT32);
    return s;
}

int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
        .count();
}

// Yields every spin: under the single-core pin this harness runs on, a pure spin never lets the
// provider's own delivery thread run in the gap, so the receiver never actually reaches idle --
// the opposite of what the pacing is for. yield() keeps this a busy-wait (polls the clock, no
// sleep-granularity rounding) while still handing off the core when something else is ready.
void BusyWait(Clock::duration d) {
    const auto deadline = Clock::now() + d;
    while (Clock::now() < deadline) {
        std::this_thread::yield();
    }
}

double Percentile(std::vector<double>& v, double p) {
    std::sort(v.begin(), v.end());
    return v[static_cast<size_t>(p * (v.size() - 1))];
}

ProviderConfig Config(const std::string& document) {
    ProviderConfig config;
    config.domain_id = 43;
    config.document = document;
    return config;
}

// Runs `body` on its own thread and gives it 30 s. `progress` is the arm's own received-count
// atomic, read here only to print it -- never written here. On timeout this is almost certainly
// pub.Publish() itself blocked in write() (the scenario under investigation: infinite
// max_blocking_time turning a full reader history into a permanent block instead of a drop), which
// no loop-internal deadline can catch because the loop never returns to check one.
void RunGuarded(const char* arm, size_t bytes, int target, std::atomic<int>& progress,
                const std::function<void()>& body) {
    std::mutex done_mutex;
    std::condition_variable done_cv;
    bool done = false;
    std::thread worker([&body, &done_mutex, &done_cv, &done] {
        body();
        {
            std::lock_guard<std::mutex> lock(done_mutex);
            done = true;
        }
        done_cv.notify_all();
    });
    worker.detach();
    std::unique_lock<std::mutex> lock(done_mutex);
    if (done_cv.wait_for(lock, std::chrono::seconds(30), [&] { return done; })) return;
    lock.unlock();
    std::printf("STALL arm=%s bytes=%zu arrived=%d/%d\n", arm, bytes,
                progress.load(std::memory_order_acquire), target);
    std::fflush(stdout);
    // A normal return would destroy pub/sub on the wedged worker thread's stack; that teardown
    // can wait on the same stuck internals. Skip it on purpose.
    std::_Exit(1);
}

void RunLatencyBody(size_t row_bytes, const std::string& document, std::atomic<int>& received) {
    FastDDSPubSubProvider pub(Config(document));
    FastDDSPubSubProvider sub(Config(document));
    const std::vector<std::string> topic{"e2e", "latency", std::to_string(row_bytes)};
    pub.CreateTopic(topic, MakeSchema());

    std::vector<double> latency_us(kLatencyIters);
    auto result = sub.Subscribe(
        topic, [&](const uint8_t* data, size_t len, const SharedSchema&, const Attachments&) {
            const int64_t now = NowNs();
            int64_t stamp = 0;
            if (len >= sizeof(stamp)) std::memcpy(&stamp, data, sizeof(stamp));
            const int idx = received.fetch_add(1, std::memory_order_acq_rel);
            if (idx >= kLatencyWarmup) {
                latency_us[static_cast<size_t>(idx - kLatencyWarmup)] =
                    static_cast<double>(now - stamp) / 1000.0;
            }
        });
    SharedSchema topic_schema;
    if (result.schema.Wait(std::chrono::seconds(5), &topic_schema) != PubSubStatus::kOk ||
        !topic_schema) {
        std::printf("arm=latency bytes=%zu SCHEMA HANDOFF FAILED\n", row_bytes);
        return;
    }

    std::vector<uint8_t> row(std::max(row_bytes, sizeof(int64_t)), 0xAB);
    const auto encoder = [&row](WriteBuffer& buf) { buf.Append(row.data(), row.size()); };
    constexpr int kTotal = kLatencyWarmup + kLatencyIters;
    for (int i = 0; i < kTotal; ++i) {
        const int64_t stamp = NowNs();
        std::memcpy(row.data(), &stamp, sizeof(stamp));
        pub.Publish(topic, encoder);
        BusyWait(kPace);
    }
    const auto deadline = Clock::now() + std::chrono::seconds(20);
    while (received.load(std::memory_order_acquire) < kTotal && Clock::now() < deadline) {
        std::this_thread::yield();
    }
    if (received.load(std::memory_order_acquire) < kTotal) {
        std::printf("arm=latency bytes=%zu LOST (%d/%d arrived)\n", row_bytes,
                    received.load(std::memory_order_acquire), kTotal);
        return;
    }
    std::printf("arm=latency bytes=%zu p50_us=%.2f p99_us=%.2f max_us=%.2f\n", row_bytes,
                Percentile(latency_us, 0.50), Percentile(latency_us, 0.99),
                Percentile(latency_us, 1.0));
}

void RunLatency(size_t row_bytes, const std::string& document) {
    std::atomic<int> received{0};
    constexpr int kTotal = kLatencyWarmup + kLatencyIters;
    RunGuarded("latency", row_bytes, kTotal, received,
               [&] { RunLatencyBody(row_bytes, document, received); });
}

void RunThroughputBody(size_t row_bytes, const std::string& document, std::atomic<int>& received) {
    FastDDSPubSubProvider pub(Config(document));
    FastDDSPubSubProvider sub(Config(document));
    const std::vector<std::string> topic{"e2e", "throughput", std::to_string(row_bytes)};
    pub.CreateTopic(topic, MakeSchema());

    auto result =
        sub.Subscribe(topic, [&](const uint8_t*, size_t, const SharedSchema&, const Attachments&) {
            received.fetch_add(1, std::memory_order_acq_rel);
        });
    SharedSchema topic_schema;
    if (result.schema.Wait(std::chrono::seconds(5), &topic_schema) != PubSubStatus::kOk ||
        !topic_schema) {
        std::printf("arm=throughput bytes=%zu SCHEMA HANDOFF FAILED\n", row_bytes);
        return;
    }

    const std::vector<uint8_t> row(row_bytes, 0xAB);
    const auto encoder = [&row](WriteBuffer& buf) { buf.Append(row.data(), row.size()); };
    const auto start = Clock::now();
    // A flat-out loop with no yield starves the delivery thread outright under the single-core pin
    // this harness runs on: the writer never gives up the core, so the reliable writer's history
    // fills and evicts un-acked samples before the reader can drain it -- real loss, not
    // measurement noise (confirmed: it does not happen unpinned). One yield per publish is the
    // minimum that lets the pinned core alternate between the two, so "counted them all" is
    // actually reachable.
    for (int i = 0; i < kThroughputIters; ++i) {
        pub.Publish(topic, encoder);
        std::this_thread::yield();
    }
    const auto deadline = Clock::now() + std::chrono::seconds(30);
    while (received.load(std::memory_order_acquire) < kThroughputIters && Clock::now() < deadline) {
        std::this_thread::yield();
    }
    const auto end = Clock::now();
    if (received.load(std::memory_order_acquire) < kThroughputIters) {
        std::printf("arm=throughput bytes=%zu LOST (%d/%d arrived)\n", row_bytes,
                    received.load(std::memory_order_acquire), kThroughputIters);
        return;
    }
    const double seconds = std::chrono::duration<double>(end - start).count();
    const double samples_per_s = kThroughputIters / seconds;
    // Decimal MB (1e6 bytes), the usual convention for a throughput number.
    const double mb_per_s = (static_cast<double>(kThroughputIters) * row_bytes) / 1e6 / seconds;
    std::printf("arm=throughput bytes=%zu samples_per_s=%.0f mb_per_s=%.2f\n", row_bytes,
                samples_per_s, mb_per_s);
}

void RunThroughput(size_t row_bytes, const std::string& document) {
    std::atomic<int> received{0};
    RunGuarded("throughput", row_bytes, kThroughputIters, received,
               [&] { RunThroughputBody(row_bytes, document, received); });
}

struct Args {
    bool run_latency = true;
    bool run_throughput = true;
    bool run_198 = true;
    bool run_60000 = true;
};

void PrintUsage(const char* prog) {
    std::printf("usage: %s [--arm latency|throughput|all] [--bytes 198|60000|all]\n", prog);
    std::printf("defaults: --arm all --bytes all\n");
}

bool ParseArgs(int argc, char** argv, Args& out) {
    for (int i = 1; i < argc; ++i) {
        const std::string flag = argv[i];
        if (i + 1 >= argc) return false;
        const std::string value = argv[++i];
        if (flag == "--arm") {
            if (value == "latency") {
                out.run_latency = true;
                out.run_throughput = false;
            } else if (value == "throughput") {
                out.run_latency = false;
                out.run_throughput = true;
            } else if (value == "all") {
                out.run_latency = true;
                out.run_throughput = true;
            } else {
                return false;
            }
        } else if (flag == "--bytes") {
            if (value == "198") {
                out.run_198 = true;
                out.run_60000 = false;
            } else if (value == "60000") {
                out.run_198 = false;
                out.run_60000 = true;
            } else if (value == "all") {
                out.run_198 = true;
                out.run_60000 = true;
            } else {
                return false;
            }
        } else {
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Args args;
    if (!ParseArgs(argc, argv, args)) {
        PrintUsage(argv[0]);
        return 2;
    }
    std::setbuf(stdout, nullptr);
    const std::string document;  // empty -- Fletcher's built-in QoS
    for (size_t bytes : kRowSizes) {
        if (bytes == 198 && !args.run_198) continue;
        if (bytes == 60000 && !args.run_60000) continue;
        if (args.run_latency) RunLatency(bytes, document);
        if (args.run_throughput) RunThroughput(bytes, document);
    }
    return 0;
}
