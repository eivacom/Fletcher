// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The companion __schema channel: a per-subscription promise, and the listener that resolves it.
#ifndef FLETCHER_FASTDDS_PUBSUB_PROVIDER_INTERNAL_SCHEMA_CHANNEL_HPP_
#define FLETCHER_FASTDDS_PUBSUB_PROVIDER_INTERNAL_SCHEMA_CHANNEL_HPP_

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fastdds/dds/log/Log.hpp>
#include <fastdds/dds/subscriber/DataReader.hpp>
#include <fastdds/dds/subscriber/DataReaderListener.hpp>
#include <fastdds/dds/subscriber/SampleInfo.hpp>
#include <fletcher/core/status.hpp>
#include <fletcher/pubsub/owned_schema.hpp>
#include <fletcher/pubsub/schema_arrival.hpp>
#include <fletcher/pubsub/schema_ipc.hpp>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include "fletcher/fastdds_pubsub_provider/fast_dds_pubsub_provider.hpp"
#include "status_endpoint.hpp"
#include "transport_data.hpp"

namespace fletcher {
namespace internal {

// Per-subscription schema handoff. The arrival is resolved by the SchemaListener
// (on a FastDDS thread) when the companion __schema sample arrives; the caller
// gets the SchemaArrival. Guarded by its OWN mutex — NEVER the provider mutex —
// so this FastDDS-thread callback can never contend with the provider lock the
// application thread holds while inside a FastDDS API (which would invert with
// FastDDS' internal subscriber mutex and deadlock).
//
// The resolver is single-use by construction, so "resolved twice" is
// unrepresentable rather than guarded by a `resolved` flag; this mutex now only
// serialises the two threads that might reach for the one token.
//
// The channel also carries a one-shot continuation, installed by Subscribe when the schema is
// still pending, that hands the schema to the data listener. SubscribeSchema opens a channel with
// no continuation; a Subscribe that follows adds one, and an Unsubscribe that keeps the channel
// for a watch Withdraws it — the listener it points at is about to be destroyed.
struct SchemaChannel {
    SchemaChannel() {
        auto pair = SchemaArrival::Create();
        arrival = std::move(pair.first);
        resolver.emplace(std::move(pair.second));
    }

    std::mutex m;
    SchemaArrival arrival;
    std::optional<SchemaResolver> resolver;
    // At most one: the data listener's SetSchema, when a Subscribe found the schema still pending.
    std::function<void(SharedSchema)> on_resolved;

    void Resolve(SharedSchema schema) {
        std::function<void(SharedSchema)> fn;
        {
            std::lock_guard<std::mutex> lk(m);
            if (!resolver.has_value()) return;
            std::optional<SchemaResolver> token = std::move(resolver);
            resolver.reset();
            fn = std::move(on_resolved);
            on_resolved = nullptr;
            // Settled UNDER `m`, unlike the continuation below: OnResolved decides "already
            // resolved?" by polling `arrival`, so a transition that landed after this lock was
            // released would let it install a continuation onto a channel whose only chance to
            // run one has just gone by. Nothing caller-written runs in here — SchemaResolver
            // only wakes waiters — so this holds the lock across a notify, not across user code.
            std::move(*token).Resolve(schema);
        }
        // Outside the lock: it flushes the backlog through user code.
        if (fn) fn(std::move(schema));
    }

    // Unsubscribed before the schema arrived. Dropping the token unresolved IS
    // the outcome — kSubscriptionEnded — which is what the broken promise used
    // to say by throwing out of get(), only now it is a value a binding can read
    // and cannot confuse with "this transport carries no schemas". Under `m` for the reason
    // Resolve gives.
    void Break() {
        std::lock_guard<std::mutex> lk(m);
        resolver.reset();
    }

    // The transport could not open the channel at all. Distinct from Break: a waiter must not read
    // a fault as the normal end of a subscription.
    void Fail(PubSubStatus status, std::string message) {
        std::lock_guard<std::mutex> lk(m);
        if (!resolver.has_value()) return;
        std::optional<SchemaResolver> token = std::move(resolver);
        resolver.reset();
        std::move(*token).Fail(status, std::move(message));
    }

    // Runs `fn` with the schema exactly once: inline if it has already arrived, otherwise from
    // Resolve. A channel that has already ended drops `fn` rather than running it with null —
    // unreachable in practice, since Break only follows a channel being moved out of its topic
    // state, where no Subscribe can find it any more.
    void OnResolved(std::function<void(SharedSchema)> fn) {
        SharedSchema schema;
        {
            std::lock_guard<std::mutex> lk(m);
            // The existing arrival IS the record of what Resolve delivered; a second
            // `SharedSchema` member here would be a second copy of that fact to keep in step.
            if (arrival.Wait(std::chrono::milliseconds(0), &schema) == PubSubStatus::kPending) {
                on_resolved = std::move(fn);
                return;
            }
        }
        if (schema) fn(std::move(schema));
    }

    // Drops a continuation that has not run. Safe only once nothing can call Resolve any more —
    // the schema reader whose listener would have is deleted — because a Resolve already past the
    // lock has taken the continuation out and is running it.
    void Withdraw() {
        std::lock_guard<std::mutex> lk(m);
        on_resolved = nullptr;
    }
};

// DataReaderListener for the companion __schema topic. Fires once when the
// retained schema sample arrives and resolves the topic's SchemaChannel with the
// deserialised schema; the channel then runs the continuation a Subscribe
// installed, which flushes the buffered data samples.
class SchemaListener : public eprosima::fastdds::dds::DataReaderListener {
   public:
    SchemaListener(std::function<void(SharedSchema)> on_schema,
                   FastDDSStatusListener* status_listener)
        : on_schema_(std::move(on_schema)), status_listener_(status_listener) {}

    void on_data_available(eprosima::fastdds::dds::DataReader* reader) override {
        RawBytes raw;
        eprosima::fastdds::dds::SampleInfo info;
        while (reader->take_next_sample(&raw, &info) == eprosima::fastdds::dds::RETCODE_OK) {
            if (!info.valid_data) continue;
            if (fired_.load()) continue;
            // Deserialize before claiming `fired_`. A malformed (or partially
            // received) schema sample must not throw out of this Fast DDS
            // listener thread (which could terminate the process), nor mark the
            // listener fired — that would leave the schema future unresolved
            // forever. On failure, wait for a subsequent valid sample.
            OwnedSchema owned;
            try {
                owned = DeserializeSchemaIpc(raw.data.data(), raw.data.size());
            } catch (const std::exception& e) {
                // If no later sample decodes, every subscriber waits on the future forever.
                EPROSIMA_LOG_ERROR(FLETCHER_SCHEMA,
                                   "ignoring a schema sample that will not decode ("
                                       << raw.data.size() << " bytes): " << e.what());
                continue;
            } catch (...) {
                EPROSIMA_LOG_ERROR(
                    FLETCHER_SCHEMA,
                    "ignoring a schema sample that will not decode: non-std exception");
                continue;
            }
            bool expected = false;
            if (fired_.compare_exchange_strong(expected, true)) {
                // Resolving the schema flushes the buffered backlog through the user callback, so
                // user code throws on this thread too. Same reason as the catch above.
                try {
                    on_schema_(MakeSharedSchema(std::move(owned)));
                } catch (const std::exception& e) {
                    EPROSIMA_LOG_ERROR(
                        FLETCHER_SCHEMA,
                        "subscribe callback threw during schema handoff: " << e.what());
                } catch (...) {
                    EPROSIMA_LOG_ERROR(
                        FLETCHER_SCHEMA,
                        "subscribe callback threw a non-std exception during schema handoff");
                }
            }
        }
    }

    // Unobserved, either leaves subscribers waiting on a schema arrival that never resolves. The
    // Endpoint these forward carries `is_schema_channel == true`, which is how a listener tells
    // this from a rejected or lost row.
    void on_sample_rejected(eprosima::fastdds::dds::DataReader* reader,
                            const eprosima::fastdds::dds::SampleRejectedStatus& status) override {
        if (status_listener_)
            status_listener_->OnSampleRejected(ReaderEndpoint(reader),
                                               static_cast<int32_t>(status.last_reason),
                                               status.total_count);
    }

    void on_sample_lost(eprosima::fastdds::dds::DataReader* reader,
                        const eprosima::fastdds::dds::SampleLostStatus& status) override {
        if (status_listener_)
            status_listener_->OnSampleLost(ReaderEndpoint(reader),
                                           static_cast<uint32_t>(status.total_count));
    }

   private:
    std::function<void(SharedSchema)> on_schema_;
    FastDDSStatusListener* status_listener_;
    std::atomic<bool> fired_{false};
};

}  // namespace internal
}  // namespace fletcher

#endif  // FLETCHER_FASTDDS_PUBSUB_PROVIDER_INTERNAL_SCHEMA_CHANNEL_HPP_
