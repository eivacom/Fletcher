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
#include <fletcher/pubsub_arrow/schema_import.hpp>
#include <functional>
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
        // The topic's schema arrival, in the seam's own vocabulary (see
        // SchemaArrival): non-blocking, one waiting mechanism for C++ and for a
        // C#/Rust caller alike.
        //
        // It used to be an Arrow-typed future. It is not any more, deliberately:
        // a `shared_future<shared_ptr<arrow::Schema>>` is the least
        // C-expressible thing at the seam, and keeping an Arrow-typed one here
        // would have been a second waiting mechanism beside the seam's. Callers
        // that want an `arrow::Schema` convert with `fletcher::ImportArrowSchema`
        // — the one safe conversion, now public precisely so that this change
        // does not hand anyone the unsafe one.
        SchemaArrival schema;
    };

    /// Subscribe with ArrowRow delivery.
    using SubscribeCallback = std::function<void(ArrowRow row, const Attachments& attachments)>;

    /// `options` is optional; a default-constructed value means the provider's defaults, forwarded
    /// verbatim to `Subscriber::Subscribe`'s options form — the options apply to the first
    /// provider-level subscription of the topic, checked field-wise there: a later call may repeat
    /// or omit a field already stored, never change one, and a non-empty field against an EMPTY
    /// stored one is a conflict too, thrown as `PubSubError(kInvalidArgument)`.
    [[nodiscard]] SubscribeResult Subscribe(const std::vector<std::string>& segments,
                                            SubscribeCallback callback,
                                            const TopicOptions& options = {});

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
    /// Rows are decoded straight into Arrow builders (`BatchDecoder`); a
    /// window that would overflow a 32-bit Arrow offset (utf8/binary/list
    /// columns beyond 2 GiB) is flushed early with reason kRowLimit.
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
    ///
    /// `batch` is null only when `BatchDecoder`'s constructor rejected the
    /// topic's schema; every row is then counted in `rows_dropped`, with a
    /// null batch, for the entire life of this subscription — there is no
    /// later recovery once the schema is known. Otherwise `batch` is never
    /// null, and may have zero rows when a window contained only dropped
    /// rows.
    ///
    /// What that costs depends on WHY `BatchDecoder` rejected the schema.
    /// Null, extension, decimal32/64, run-end-encoded and list-view types are
    /// not decodable through EITHER `SubscriberArrow::Subscribe` overload —
    /// `Codec::DecodeRow` (the per-row one) throws on the same schema, so
    /// there is no fallback to switch to. A dictionary nested below the top
    /// level, an ordered dictionary, and a dictionary whose value type is
    /// nested or `float16` are different: `Codec::DecodeRow` decodes all
    /// three fine, so a caller who needs one of those three shapes and wants
    /// data at all uses the per-row `Subscribe` overload for that topic
    /// instead of this one.
    using RecordBatchCallback =
        std::function<void(std::shared_ptr<arrow::RecordBatch> batch,
                           std::vector<Attachments> attachments, BatchStatus status)>;

    /// `topic_options` is optional; a default-constructed value means the provider's defaults,
    /// forwarded to `Subscriber::Subscribe`'s options form — checked field-wise there: a later
    /// call may repeat or omit a field already stored, never change one, and a non-empty field
    /// against an EMPTY stored one is a conflict too, thrown as `PubSubError(kInvalidArgument)`.
    [[nodiscard]] SubscribeResult Subscribe(const std::vector<std::string>& segments,
                                            RecordBatchCallback callback, BatchOptions options,
                                            const TopicOptions& topic_options = {});

    /// Convenience overload using the default BatchOptions (8000 rows, 1 min).
    /// (BatchOptions cannot be a defaulted argument above: a nested aggregate's member
    /// initializers aren't usable in a default arg of the same class. `TopicOptions` carries no
    /// such restriction — it is not nested in this class — which is why it defaults above.)
    [[nodiscard]] SubscribeResult Subscribe(const std::vector<std::string>& segments,
                                            RecordBatchCallback callback) {
        return Subscribe(segments, std::move(callback), BatchOptions{});
    }

    void Unsubscribe(uint64_t subscription_id);

    /// The topic's schema without its data — Subscriber::SubscribeSchema, forwarded as is. Poll or
    /// wait on the arrival; convert with fletcher::ImportArrowSchema when it reports kOk. Released
    /// by UnsubscribeSchema.
    [[nodiscard]] SchemaArrival SubscribeSchema(const std::vector<std::string>& segments);
    /// Subscriber::UnsubscribeSchema, forwarded as is (watches are counted per topic there).
    void UnsubscribeSchema(const std::vector<std::string>& segments);

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
};

}  // namespace fletcher

#endif  // FLETCHER_INCLUDE_SUBSCRIBER_ARROW_HPP_
