// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include "fletcher/conformance/fixtures.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <unordered_map>

#ifdef _WIN32
#include <process.h>
#define FLETCHER_GETPID _getpid
#else
#include <unistd.h>
#define FLETCHER_GETPID getpid
#endif

namespace fletcher {
namespace conformance {

// ── Traits ──────────────────────────────────────────────────────────
Retention RetentionForProvider(const std::string& provider) {
    // The one table. A provider appears here once, so its in-process and
    // cross-process subjects cannot disagree — clause 6's all-or-nothing
    // assertion has no per-subject escape hatch.
    static const std::unordered_map<std::string, Retention> kTable = {
        // The loopback holds no history at all: a publish with no subscriber
        // registered is dropped on the floor.
        {"inprocess", Retention::kDropsPreSubscribe},
        // Shipped defaults are RELIABLE + KEEP_ALL + TRANSIENT_LOCAL, so a late
        // joiner is replayed the retained backlog.
        {"fastdds", Retention::kRetainsPreSubscribe},
        // Same QoS triple, expressed through the Agent.
        {"xrce", Retention::kRetainsPreSubscribe},
    };
    auto it = kTable.find(provider);
    if (it == kTable.end()) {
        throw std::runtime_error(
            "conformance: no retention recorded for provider '" + provider +
            "' — add it to RetentionForProvider so both of its subjects share one answer");
    }
    return it->second;
}

ProviderTraits MakeTraits(std::string provider, SchemaMode schema_mode) {
    Retention retention = RetentionForProvider(provider);
    return ProviderTraits{std::move(provider), schema_mode, retention};
}

// ── Schemas ─────────────────────────────────────────────────────────
OwnedSchema MakeConformanceSchema(SchemaId id) {
    OwnedSchema s;
    if (id == SchemaId::kNone) {
        return s;
    }
    ArrowSchemaInit(s.get());
    const int children = (id == SchemaId::kB) ? 2 : 1;
    if (ArrowSchemaSetTypeStruct(s.get(), children) != NANOARROW_OK) {
        throw std::runtime_error("conformance: ArrowSchemaSetTypeStruct failed");
    }
    ArrowSchemaSetName(s->children[0], "seq");
    ArrowSchemaSetType(s->children[0], NANOARROW_TYPE_INT32);
    if (id == SchemaId::kB) {
        ArrowSchemaSetName(s->children[1], "extra");
        ArrowSchemaSetType(s->children[1], NANOARROW_TYPE_DOUBLE);
    }
    return s;
}

// ── The opaque row ──────────────────────────────────────────────────
std::optional<uint32_t> DecodeRow(const uint8_t* data, size_t len) {
    if (data == nullptr || len != kRowBytes) {
        return std::nullopt;
    }
    uint32_t magic = 0;
    uint32_t seq = 0;
    std::memcpy(&magic, data, sizeof(magic));
    std::memcpy(&seq, data + sizeof(magic), sizeof(seq));
    if (magic != kRowMagic) {
        return std::nullopt;
    }
    return seq;
}

// ── Collector ───────────────────────────────────────────────────────
SubscribeCallback Collector::Callback() {
    return [this](const uint8_t* data, size_t len, const SharedSchema& schema, const Attachments&) {
        Record(data, len, schema != nullptr);
    };
}

void Collector::Record(const uint8_t* data, size_t len, bool had_schema) {
    const size_t now = in_flight_.fetch_add(1) + 1;
    for (size_t seen = max_in_flight_.load(); now > seen;) {
        if (max_in_flight_.compare_exchange_weak(seen, now)) {
            break;
        }
    }

    const int64_t hold_us = hold_us_.load();
    if (hold_us > 0) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(hold_us);
        while (std::chrono::steady_clock::now() < until) {
            std::this_thread::yield();
        }
    }

    std::optional<uint32_t> seq = DecodeRow(data, len);
    if (!seq.has_value()) {
        foreign_.fetch_add(1);
    } else {
        {
            std::lock_guard lock(mu_);
            deliveries_.push_back(Delivery{*seq, had_schema});
        }
        cv_.notify_all();
    }
    in_flight_.fetch_sub(1);
}

std::vector<Collector::Delivery> Collector::Snapshot() const {
    std::lock_guard lock(mu_);
    return deliveries_;
}

size_t Collector::Count() const {
    std::lock_guard lock(mu_);
    return deliveries_.size();
}

std::vector<uint32_t> Collector::Seqs() const {
    std::lock_guard lock(mu_);
    std::vector<uint32_t> out;
    out.reserve(deliveries_.size());
    for (const Delivery& d : deliveries_) {
        out.push_back(d.seq);
    }
    return out;
}

bool Collector::WaitForCount(size_t n, std::chrono::steady_clock::time_point deadline) const {
    std::unique_lock lock(mu_);
    return cv_.wait_until(lock, deadline, [&] { return deliveries_.size() >= n; });
}

bool Collector::WaitForSeq(uint32_t seq, std::chrono::steady_clock::time_point deadline) const {
    std::unique_lock lock(mu_);
    return cv_.wait_until(lock, deadline, [&] {
        for (const Delivery& d : deliveries_) {
            if (d.seq == seq) {
                return true;
            }
        }
        return false;
    });
}

// ── Fresh topics ───────────────────────────────────────────
Topic FreshTopic(const std::string& clause) {
    static std::atomic<uint32_t> counter{0};
    char tail[64];
    std::snprintf(tail, sizeof(tail), "%d_%u", static_cast<int>(FLETCHER_GETPID()),
                  counter.fetch_add(1));
    return Topic{"conf", clause, std::string(tail)};
}

}  // namespace conformance
}  // namespace fletcher
