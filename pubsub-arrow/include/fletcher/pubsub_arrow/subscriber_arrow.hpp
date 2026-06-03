// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#ifndef FLETCHER_INCLUDE_SUBSCRIBER_ARROW_HPP_
#define FLETCHER_INCLUDE_SUBSCRIBER_ARROW_HPP_

#include <arrow/type_fwd.h>

#include <chrono>
#include <cstdint>
#include <fletcher/arrow_bridge/codec.hpp>
#include <fletcher/pubsub/provider.hpp>
#include <fletcher/pubsub/subscriber.hpp>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fletcher {

/// Arrow-friendly wrapper around Subscriber. Decodes incoming row
/// bytes into ArrowRow via Codec before delivering to the caller, or
/// accumulates them into RecordBatches via the batched Subscribe
/// overload.
///
/// Server-side code that works with Arrow C++ types should use
/// SubscriberArrow; edge-deployed code that already speaks the raw
/// row-bytes interface uses Subscriber directly.
class SubscriberArrow {
   public:
    explicit SubscriberArrow(std::shared_ptr<PubSubProvider> provider);

    /// Stops and flushes any batched subscriptions (see the RecordBatch
    /// Subscribe overload) before teardown.
    ~SubscriberArrow();

    SubscriberArrow(const SubscriberArrow&) = delete;
    SubscriberArrow& operator=(const SubscriberArrow&) = delete;

    struct SubscribeResult {
        uint64_t subscription_id;
        // Future for the topic's Arrow schema (see SubscriptionResult):
        // non-blocking, resolves with a non-null schema once known.
        std::shared_future<std::shared_ptr<arrow::Schema>> schema;
    };

    /// Subscribe with ArrowRow delivery.
    using SubscribeCallback = std::function<void(ArrowRow row, Attachments attachments)>;
    SubscribeResult Subscribe(const std::vector<std::string>& segments, SubscribeCallback callback);

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
                              RecordBatchCallback callback, BatchOptions options);

    /// Convenience overload using the default BatchOptions (8000 rows, 1 min).
    /// (BatchOptions cannot be a defaulted argument above: a nested aggregate's
    /// member initializers aren't usable in a default arg of the same class.)
    SubscribeResult Subscribe(const std::vector<std::string>& segments,
                              RecordBatchCallback callback) {
        return Subscribe(segments, std::move(callback), BatchOptions{});
    }

    void Unsubscribe(uint64_t subscription_id);

   private:
    class RecordBatchBatcher;  // defined in subscriber_arrow.cpp

    std::unique_ptr<Subscriber> subscriber_;

    mutable std::mutex mu_;
    struct TopicCodec {
        std::shared_ptr<arrow::Schema> arrow_schema;
        std::unique_ptr<Codec> codec;
    };
    std::unordered_map<std::string, TopicCodec> codecs_;
    // Maps subscription_id -> topic key so Unsubscribe can free codec
    // entries once the last subscription for a topic is gone.
    std::unordered_map<uint64_t, std::string> sub_topic_;
    // Live batched subscriptions (sub_id -> batcher).
    std::unordered_map<uint64_t, std::shared_ptr<RecordBatchBatcher>> batchers_;

    // Look up the codec for `key`, lazily creating one from the
    // delivered schema if none is registered yet. Used by the batched
    // path so a subscriber-only process can still decode rows that
    // arrive before subscriber_->Subscribe returns.
    Codec* AcquireCodec(const std::string& key, const SharedSchema& schema);

    static std::string JoinSegments(const std::vector<std::string>& segs);
};

}  // namespace fletcher

#endif  // FLETCHER_INCLUDE_SUBSCRIBER_ARROW_HPP_
