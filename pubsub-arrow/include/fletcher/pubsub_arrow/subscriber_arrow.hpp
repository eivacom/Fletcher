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

    /// Closes every batched subscription as an `Unsubscribe` from another thread does: it returns
    /// once each one has delivered its last batch. Destroyed from inside one of its own batch
    /// callbacks, it does not wait for that callback's subscription, whose remaining batches arrive
    /// after the callback returns, so the callback must outlive them; it still waits for every
    /// other batched subscription. Destroying it from a thread delivering rows on its transport
    /// stays forbidden (see `Subscriber`).
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
        /// Flush at this many rows. At least 1; below 1 is refused (never clamped).
        int64_t max_rows = 8000;
        /// ...or after this long. 0 or more; negative is refused. A zero timeout delivers no
        /// sooner than 1 ms after the window's first row or drop. `kNoTimeout` means no timeout:
        /// a window then closes only when full, on the offset budget, or at Unsubscribe or
        /// destruction, never on a clock. Any timeout too long for the clock to reach means the
        /// same; a long timeout the clock can reach gives a deadline at its true time.
        std::chrono::milliseconds timeout = std::chrono::minutes(1);
        /// No timeout: windows close only at max_rows, on the offset budget, or at closing;
        /// never on a clock.
        static constexpr std::chrono::milliseconds kNoTimeout = std::chrono::milliseconds::max();
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
    /// Decoded rows are accumulated and flushed to `callback` when `options.max_rows` rows have
    /// been decoded, or at the window's deadline — `options.timeout` after the previous flush, but
    /// never sooner than 1 ms after the window's first row or drop — whichever comes first. This
    /// holds whether or not a usable schema has arrived: a window of lost rows is reported at its
    /// deadline too. A partial batch is flushed on Unsubscribe with reason kClosing.
    ///
    /// With `BatchOptions::kNoTimeout` there is no deadline: a window closes only when full, on the
    /// offset budget, or at Unsubscribe or destruction. A window of only lost rows (no usable
    /// schema, an unusable schema, malformed rows) never counts toward `max_rows`, so it is then
    /// reported only in the kClosing batch.
    ///
    /// Throws `PubSubError(kInvalidArgument)` when `options.max_rows < 1` or `options.timeout` is
    /// negative, before anything is subscribed: no thread is started, the callback is released,
    /// and nothing is ever delivered. Any other timeout is used as given.
    ///
    /// `callback` runs on a thread the subscription owns, one batch at a time, in window order;
    /// never inside `Publish` or on the transport's delivery thread, so publishing never waits on
    /// it. A callback slower than the data lets finished batches queue up in memory.
    ///
    /// `attachments[i]` belongs to batch row `i` (parallel, in batch order).
    /// A row that fails to decode is counted in `status.rows_dropped` and
    /// contributes neither a row nor an attachment — the metadata identifying
    /// its attachment lived in that row. If a window contains only dropped
    /// rows, a zero-row (or null) batch is still delivered so the loss is
    /// reported.
    ///
    /// `batch` is null when no batch could be built for this window: no usable
    /// schema has arrived yet, the schema was rejected (permanent), or
    /// finishing the batch failed; `rows_dropped` counts the window's rows in
    /// every case. A rejected schema stays rejected for the entire life of this
    /// subscription — there is no later recovery once the schema is known, and
    /// no partial decoding. A non-null `batch` may have zero rows when a window
    /// contained only dropped rows.
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

    /// For a batched subscription, every finished batch and then the partial one (reason
    /// kClosing) is delivered before this returns; nothing is delivered after. Two calls return
    /// at once instead: one from inside this subscription's own batch callback, and one from a
    /// thread that is delivering rows on the subscription's own transport (e.g. inside a per-row
    /// callback). The remaining batches, then kClosing, still arrive in order after that, so what
    /// those callbacks use may not be freed until the kClosing batch has arrived.
    ///
    /// Any other close made inside a batch callback waits, so two can wait on each other forever:
    /// two subscriptions whose callbacks each `Unsubscribe` the other, or one callback destroying
    /// this `SubscriberArrow` while another subscription's callback unsubscribes the first.
    void Unsubscribe(uint64_t subscription_id);

    /// The topic's schema without its data — Subscriber::SubscribeSchema, forwarded as is. Poll or
    /// wait on the arrival; convert with fletcher::ImportArrowSchema when it reports kOk. Released
    /// by UnsubscribeSchema.
    [[nodiscard]] SchemaArrival SubscribeSchema(const std::vector<std::string>& segments);
    /// Subscriber::UnsubscribeSchema, forwarded as is (watches are counted per topic there).
    void UnsubscribeSchema(const std::vector<std::string>& segments);

   private:
    class RecordBatchBatcher;  // defined in subscriber_arrow.cpp

    // The provider's identity, compared by address and never dereferenced: is the thread calling
    // Unsubscribe delivering rows on this transport? Declared before subscriber_, which takes the
    // provider over.
    const void* provider_token_;
    std::unique_ptr<Subscriber> subscriber_;

    mutable std::mutex mu_;
    struct TopicCodec {
        std::shared_ptr<arrow::Schema> arrow_schema;
        std::shared_ptr<Codec> codec;
        bool import_failed = false;  // only from AcquireCodec: the delivered schema's import threw
    };
    std::unordered_map<std::string, TopicCodec> codecs_;
    // Maps subscription_id -> topic key so Unsubscribe can free codec
    // entries once the last subscription for a topic is gone.
    std::unordered_map<uint64_t, std::string> sub_topic_;
    // Batched subscriptions whose deliveries have not ended (sub_id -> batcher).
    std::unordered_map<uint64_t, std::shared_ptr<RecordBatchBatcher>> batchers_;

    // The codec entry for `key`, lazily created from the delivered schema if none is registered
    // yet, returned BY VALUE so the caller holds what it found whatever happens to `codecs_`
    // afterwards (a last Unsubscribe on the topic erases the entry). Empty when no entry exists and
    // the row brings no importable schema; `import_failed` when importing the row's schema threw.
    // Lets a subscriber-only process decode rows that arrive before subscriber_->Subscribe returns.
    TopicCodec AcquireCodec(const std::string& key, const SharedSchema& schema);
};

}  // namespace fletcher

#endif  // FLETCHER_INCLUDE_SUBSCRIBER_ARROW_HPP_
