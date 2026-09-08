// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The companion __schema channel: a per-subscription promise, and the listener that resolves it.
#ifndef FLETCHER_FASTDDS_PUBSUB_PROVIDER_INTERNAL_SCHEMA_CHANNEL_HPP_
#define FLETCHER_FASTDDS_PUBSUB_PROVIDER_INTERNAL_SCHEMA_CHANNEL_HPP_

#include <atomic>
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
struct SchemaChannel {
    SchemaChannel() {
        auto pair = SchemaArrival::Create();
        arrival = std::move(pair.first);
        resolver.emplace(std::move(pair.second));
    }

    std::mutex m;
    SchemaArrival arrival;
    std::optional<SchemaResolver> resolver;

    void Resolve(SharedSchema schema) {
        std::optional<SchemaResolver> token;
        {
            std::lock_guard<std::mutex> lk(m);
            if (!resolver.has_value()) return;
            token = std::move(resolver);
            resolver.reset();
        }
        std::move(*token).Resolve(std::move(schema));
    }

    // Unsubscribed before the schema arrived. Dropping the token unresolved IS
    // the outcome — kSubscriptionEnded — which is what the broken promise used
    // to say by throwing out of get(), only now it is a value a binding can read
    // and cannot confuse with "this transport carries no schemas".
    void Break() {
        std::optional<SchemaResolver> token;
        {
            std::lock_guard<std::mutex> lk(m);
            token = std::move(resolver);
            resolver.reset();
        }
    }
};

// DataReaderListener for the companion __schema topic. Fires once when the
// retained schema sample arrives and forwards the deserialised schema to the
// callback installed by Subscribe (which resolves the subscription's schema
// future and flushes buffered data samples).
class SchemaListener : public eprosima::fastdds::dds::DataReaderListener {
   public:
    explicit SchemaListener(std::function<void(SharedSchema)> on_schema)
        : on_schema_(std::move(on_schema)) {}

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

    // Unreported, either leaves subscribers waiting on a schema future that never resolves.
    void on_sample_rejected(eprosima::fastdds::dds::DataReader* /*reader*/,
                            const eprosima::fastdds::dds::SampleRejectedStatus& status) override {
        EPROSIMA_LOG_ERROR(FLETCHER_SCHEMA,
                           "a schema sample was rejected (reason "
                               << static_cast<int>(status.last_reason) << ", " << status.total_count
                               << " total); if it was too large for the channel, raise "
                                  "max_schema_bytes on this endpoint");
    }

    void on_sample_lost(eprosima::fastdds::dds::DataReader* /*reader*/,
                        const eprosima::fastdds::dds::SampleLostStatus& status) override {
        EPROSIMA_LOG_WARNING(FLETCHER_SCHEMA, "a schema sample was lost ("
                                                  << status.total_count
                                                  << " total); the schema future stays unresolved "
                                                     "until the writer's retained sample arrives");
    }

   private:
    std::function<void(SharedSchema)> on_schema_;
    std::atomic<bool> fired_{false};
};

}  // namespace internal
}  // namespace fletcher

#endif  // FLETCHER_FASTDDS_PUBSUB_PROVIDER_INTERNAL_SCHEMA_CHANNEL_HPP_
