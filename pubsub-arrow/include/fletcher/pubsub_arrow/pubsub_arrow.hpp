// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#ifndef FLETCHER_INCLUDE_PUBSUB_ARROW_HPP_
#define FLETCHER_INCLUDE_PUBSUB_ARROW_HPP_

#include <arrow/type_fwd.h>

#include <chrono>
#include <cstdint>
#include <fletcher/arrow_bridge/codec.hpp>
#include <fletcher/pubsub/driver.hpp>
#include <fletcher/pubsub/pubsub.hpp>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fletcher {

/// Arrow-friendly wrapper around the nanoarrow-based PubSub Driver.
///
/// PubSubArrow bridges the gap between the nanoarrow pub/sub core (which
/// deals with raw bytes and ArrowSchema C structs) and server-side code
/// that works with Arrow C++ types (arrow::Schema, ArrowRow).
///
/// Edge-deployed code uses the Driver directly; server-side code uses
/// PubSubArrow for convenience.
class PubSubArrow {
   public:
    explicit PubSubArrow(std::shared_ptr<PubSub> provider);

    /// Stops and flushes any batched subscriptions (see the RecordBatch
    /// Subscribe overload) before teardown.
    ~PubSubArrow();

    /// Create a topic with an Arrow C++ schema.
    /// Passing nullptr is allowed (topic created without schema).
    void CreateTopic(const std::vector<std::string>& segments,
                     std::shared_ptr<arrow::Schema> schema, std::any config = {});

    /// Publish an ArrowRow (encoded via Codec).
    void Publish(const std::vector<std::string>& segments, const ArrowRow& row,
                 const Attachments& attachments = {});

    /// Publish using a direct encoder (passthrough to Driver).
    void PublishDirect(const std::vector<std::string>& segments, PubSub::RowEncoder encoder,
                       const Attachments& attachments = {});

    struct SubscribeResult {
        uint64_t subscription_id;
        std::shared_ptr<arrow::Schema> schema;
    };

    /// Subscribe with ArrowRow delivery.
    using SubscribeCallback = std::function<void(ArrowRow row, Attachments attachments)>;
    SubscribeResult Subscribe(const std::vector<std::string>& segments, SubscribeCallback callback,
                              std::any config = {});

    /// Tuning for the batched (RecordBatch) Subscribe overload.
    struct BatchOptions {
        int64_t max_rows = 8000;                                      // flush at this many rows
        std::chrono::milliseconds timeout = std::chrono::minutes(1);  // ...or after this long
    };

    /// Describes why a batch was delivered and whether any rows were lost.
    struct BatchStatus {
        enum class Reason {
            kRowLimit,  // flushed because max_rows was reached
            kTimeout,   // flushed because the timeout elapsed
            kClosing,   // flushed because the subscription is being torn down
        };
        Reason reason;
        int64_t rows_dropped;  // rows lost since the previous flush; 0 == all good
    };

    /// Subscribe with batched RecordBatch delivery (Arrow tier only).
    ///
    /// Decoded rows are accumulated and flushed to `callback` when
    /// `options.max_rows` is reached or `options.timeout` elapses since the
    /// first row/drop of the current batch — whichever comes first. A partial
    /// batch is flushed on Unsubscribe with reason kClosing.
    ///
    /// `attachments[i]` belongs to batch row `i` (parallel, in batch order).
    /// A row that fails to decode is counted in `status.rows_dropped` and
    /// contributes neither a row nor an attachment — the metadata identifying
    /// its attachment lived in that row. If a window contains only dropped
    /// rows, a zero-row batch is still delivered so the loss is reported.
    using RecordBatchCallback =
        std::function<void(std::shared_ptr<arrow::RecordBatch> batch,
                           std::vector<Attachments> attachments, BatchStatus status)>;
    SubscribeResult Subscribe(const std::vector<std::string>& segments,
                              RecordBatchCallback callback, BatchOptions options,
                              std::any config = {});

    /// Convenience overload using the default BatchOptions (8000 rows, 1 min).
    /// (BatchOptions cannot be a defaulted argument above: a nested aggregate's
    /// member initializers aren't usable in a default arg of the same class.)
    SubscribeResult Subscribe(const std::vector<std::string>& segments,
                              RecordBatchCallback callback) {
        return Subscribe(segments, std::move(callback), BatchOptions{});
    }

    void Unsubscribe(uint64_t subscription_id);

    std::vector<std::string> ListTopics() const;
    bool HasTopic(const std::vector<std::string>& segments) const;

   private:
    std::unique_ptr<Driver> driver_;

    mutable std::mutex mu_;
    struct TopicCodec {
        std::shared_ptr<arrow::Schema> arrow_schema;
        std::unique_ptr<Codec> codec;
    };
    std::unordered_map<std::string, TopicCodec> codecs_;

    // Find the codec for `key`, building it lazily from the per-message
    // SharedSchema when missing (subscriber-only path: no prior CreateTopic,
    // and the provider can deliver before driver_->Subscribe returns).
    // Returns nullptr if no schema is available to build one. Acquires mu_.
    Codec* AcquireCodec(const std::string& key, const SharedSchema& schema);

    // Accumulates decoded rows into RecordBatches for the batched Subscribe
    // overload. Defined in pubsub_arrow.cpp; one per batched subscription,
    // keyed by subscription id so Unsubscribe can stop and flush it.
    class RecordBatchBatcher;
    std::unordered_map<uint64_t, std::shared_ptr<RecordBatchBatcher>> batchers_;

    static std::string JoinSegments(const std::vector<std::string>& segs);
};

}  // namespace fletcher

#endif  // FLETCHER_INCLUDE_PUBSUB_ARROW_HPP_
