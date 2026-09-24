// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// Helpers shared by more than one TU in this suite. Every TEST() here runs in its own process
// (gtest_discover_tests, tests/CMakeLists.txt), so a symbol with external linkage shared across
// translation units carries no state between tests.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <fletcher/core/write_buffer.hpp>
#include <fletcher/fastdds_pubsub_provider/fast_dds_pubsub_provider.hpp>
#include <fletcher/pubsub/schema_ipc.hpp>
#include <mutex>
#include <utility>

inline fletcher::OwnedSchema MakeSchema() {
    fletcher::OwnedSchema s;
    ArrowSchemaInit(s.get());
    ArrowSchemaSetTypeStruct(s.get(), 1);
    ArrowSchemaSetName(s->children[0], "x");
    ArrowSchemaSetType(s->children[0], NANOARROW_TYPE_INT32);
    return s;
}

inline fletcher::PubSubProvider::RowEncoder MakeEncoder(int32_t x) {
    return [x](fletcher::WriteBuffer& buf) {
        buf.AppendByte(0x00);
        buf.AppendFixed<int32_t>(x);
    };
}

inline int32_t DecodeRow(const uint8_t* data) {
    int32_t v;
    std::memcpy(&v, data + 1, sizeof(v));
    return v;
}

// Ten slots of the payload bound rather than Fletcher's published twenty-five: a bounded plain
// type reserves the whole bound per history slot per endpoint, and the loaned tests want a pool
// small enough to exhaust deliberately.
inline constexpr const char* kTenSlots = R"(
        <historyQos><kind>KEEP_LAST</kind><depth>10</depth></historyQos>
        <resourceLimitsQos>
          <max_samples>10</max_samples>
          <max_instances>1</max_instances>
          <max_samples_per_instance>10</max_samples_per_instance>
          <allocated_samples>10</allocated_samples>
        </resourceLimitsQos>)";

// This suite's one poll-free wait: a producer (a subscriber callback, a Log consumer) calls
// NotifyWaiters() right after changing whatever a WaitUntil() predicate reads; the waiter blocks
// on a condition_variable instead of sleeping and re-checking. notify_all() is issued under
// g_wait_mutex, so a notify that lands between a waiter's predicate check and it actually starting
// to wait is never lost (serialized through the same mutex) rather than merely
// eventually-consistent.
inline std::mutex g_wait_mutex;
inline std::condition_variable g_wait_cv;

inline void NotifyWaiters() {
    std::lock_guard<std::mutex> lock(g_wait_mutex);
    g_wait_cv.notify_all();
}

template <class Pred>
bool WaitUntil(Pred pred, std::chrono::milliseconds budget) {
    std::unique_lock<std::mutex> lock(g_wait_mutex);
    return g_wait_cv.wait_for(lock, budget, std::move(pred));
}
