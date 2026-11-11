// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include "fletcher/pubsub_arrow/subscriber_arrow.hpp"

#include <arrow/api.h>
#include <arrow/c/bridge.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fletcher/arrow_bridge/batch_decoder.hpp>
#include <fletcher/core/internal/delivery_frame.hpp>
#include <fletcher/pubsub/internal/segments.hpp>
#include <fletcher/pubsub/owned_schema.hpp>
#include <fletcher/pubsub_arrow/schema_import.hpp>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace fletcher {

// -----------------------------------------------------------------------
// RecordBatchBatcher — accumulates decoded rows into RecordBatches and
// flushes on row-count, timeout, or close (see the batched Subscribe).
// -----------------------------------------------------------------------

class SubscriberArrow::RecordBatchBatcher {
   public:
    RecordBatchBatcher(RecordBatchCallback cb, int64_t max_rows, std::chrono::milliseconds timeout,
                       const void* provider)
        : cb_(std::move(cb)),
          max_rows_(max_rows < 1 ? 1 : max_rows),
          timeout_(timeout),
          provider_(provider) {}

    ~RecordBatchBatcher() { static_cast<void>(Stop()); }

    RecordBatchBatcher(const RecordBatchBatcher&) = delete;
    RecordBatchBatcher& operator=(const RecordBatchBatcher&) = delete;

    // Starts the batch thread, given a shared_ptr to this same batcher so the thread is a
    // co-owner, not a borrower: a callback may drop every other owner (Unsubscribe, or destroying
    // the SubscriberArrow), and the batcher must outlive the thread running inside it (L11). The
    // capture is released only once Loop() has returned with mu_ free; if it is the last owner,
    // ~RecordBatchBatcher then runs on the batch thread and its Stop() returns at once. Called
    // once, right after construction, before any delivery can reach the batcher.
    void Start(std::shared_ptr<RecordBatchBatcher> self) {
        ended_ = false;
        try {
            thread_ = std::thread([self = std::move(self)] { self->Loop(); });
        } catch (...) {
            ended_ = true;
            throw;
        }
    }

    // Decides the schema, once (L8): the first decision wins and any later call is refused.
    // Until a usable schema is set, and for a null one (the delivered schema could not be
    // imported) or one the decoder cannot build (see BatchDecoder), decoder_ stays null: every row
    // is then counted dropped and reported with a null batch, so the loss is visible.
    void SetSchema(std::shared_ptr<arrow::Schema> schema) {
        std::lock_guard<std::mutex> lk(mu_);
        if (decided_ || stopped_) return;  // closed: no decoder is built again
        decided_ = true;
        if (schema) {
            try {
                decoder_ = std::make_unique<BatchDecoder>(schema);
                decoder_->Reserve(max_rows_);
            } catch (...) {
                decoder_.reset();
            }
        }
    }

    bool SchemaDecided() const { return decided_.load(); }

    // The loop has ended: every delivery this batcher owed has been made.
    bool Ended() const { return ended_.load(); }

    // The one entry for every transport row. Runs on the provider's delivery thread, so it never
    // calls or waits on the callback (D10), and nothing may escape: every failure is folded into
    // the drop count. Closing a window only queues its batch.
    void AddRow(const uint8_t* data, size_t len, const Attachments& att) {
        std::lock_guard<std::mutex> lk(mu_);
        if (stopped_) return;
        // A row never joins a window whose deadline has passed, even while the batch thread is
        // busy in a callback (L4).
        if (has_deadline_ && std::chrono::steady_clock::now() >= deadline_) {
            Close(BatchStatus::Reason::kTimeout);
        }
        if (!decoder_) {
            ++dropped_;
            ArmTimer();
            return;
        }
        if (!TryAppend(data, len, att)) {
            // A well-formed row that does not fit a 32-bit Arrow offset any more: close this
            // window and start the next one with it. mu_ is held throughout, so nothing can close
            // the subscription in between.
            Close(BatchStatus::Reason::kRowLimit);
            if (!TryAppend(data, len, att)) ++dropped_;  // the row alone exceeds the budget
        }
        ArmTimer();
        if (decoder_->num_rows() >= max_rows_) Close(BatchStatus::Reason::kRowLimit);
    }

    // Closes the subscription: the pending window becomes the kClosing batch, behind any batch
    // already queued, and later rows are ignored (L9). Idempotent. Returns true once every
    // delivery has been made. Returns false at once, delivering nothing, when called from inside
    // this subscription's own callback, or from a thread delivering rows on its transport (owner
    // rulings 2026-11-11); the batch thread then delivers the rest and ends on its own.
    [[nodiscard]] bool Stop() {
        std::thread t;
        {
            std::unique_lock<std::mutex> lk(mu_);
            if (!stopped_) {
                stopped_ = true;
                Close(BatchStatus::Reason::kClosing);
                cv_.notify_all();
            }
            if (std::this_thread::get_id() == loop_id_ || internal::InsideDeliveryOn(provider_)) {
                t = std::move(thread_);
                lk.unlock();
                if (t.joinable()) t.detach();
                return false;
            }
            cv_.wait(lk, [this] { return ended_.load(); });
            t = std::move(thread_);
        }
        if (t.joinable()) t.join();
        return true;
    }

   private:
    struct Delivery {
        std::shared_ptr<arrow::RecordBatch> batch;
        std::vector<Attachments> atts;
        BatchStatus status;
    };

    bool HasPending() const { return (decoder_ && decoder_->num_rows() > 0) || dropped_ > 0; }

    // Appends one row, or counts it lost. False only when the row does not fit the window's
    // offset budget; then nothing was appended or counted.
    bool TryAppend(const uint8_t* data, size_t len, const Attachments& att) {
        try {
            decoder_->Append(data, len);
            atts_.push_back(att);
        } catch (const BatchCapacityExceeded&) {
            return false;
        } catch (const std::invalid_argument&) {
            // Malformed row: nothing was appended; its attachment goes with it (the metadata
            // naming the blob lived in the row).
            ++dropped_;
        } catch (...) {
            DiscardPending(1);
        }
        return true;
    }

    // Internal failure (allocation) inside Append() or Finish(): the pending window is undefined,
    // so count it dropped whole (plus `extra` for a row that was being appended), drop its
    // attachments, and reset the decoder with a Finish() whose result is discarded. That Finish()
    // is wrapped too: under the memory pressure that just failed, it can fail as well, and nothing
    // may escape the delivery, batch or closing thread.
    void DiscardPending(int64_t extra) {
        dropped_ += decoder_->num_rows() + extra;
        atts_.clear();
        try {
            (void)decoder_->Finish();
        } catch (...) {
        }
    }

    // Arms the timeout deadline on the first event (row or drop) of a window. The deadline is
    // anchored to the previous flush, not to the event: a stream that keeps delivering then flushes
    // every `timeout_` (measured 65 ms per window at 30 Hz with a 33 ms timeout before — the window
    // plus the stream's own inter-arrival gap), and a stream that has been idle longer than a
    // window is delivered right away instead of waiting another one.
    void ArmTimer() {
        if (!has_deadline_ && HasPending()) {
            const auto now = std::chrono::steady_clock::now();
            deadline_ = std::max(now + kMinWindow, last_flush_ + timeout_);
            has_deadline_ = true;
            cv_.notify_all();
        }
    }

    // The one window closer (row limit, deadline, budget, closing). Caller holds mu_, which is
    // never released here. A window with only dropped rows still yields a zero-row batch (or a
    // null one when there is no decoder: no usable schema has arrived, or the schema was
    // rejected), so the loss is reported; an idle window yields nothing (L6). The batch is queued
    // for the batch thread, in window order.
    void Close(BatchStatus::Reason reason) {
        has_deadline_ = false;
        last_flush_ = std::chrono::steady_clock::now();
        if (!HasPending()) return;

        // Finish() only hands the builders' buffers over — O(1) per column.
        std::shared_ptr<arrow::RecordBatch> batch;
        if (decoder_) {
            try {
                batch = decoder_->Finish();
            } catch (...) {
                // Finish() itself failed (allocation, a dictionary re-fold that does not fit its
                // index type): no batch row is left to align an attachment with, so the window is
                // dropped whole.
                DiscardPending(0);
            }
            // A failed pre-size is not fatal: the next Append() just grows the builders. No window
            // follows the closing one, so it is not pre-sized.
            if (reason != BatchStatus::Reason::kClosing) {
                try {
                    decoder_->Reserve(max_rows_);
                } catch (...) {
                }
            }
        }
        std::vector<Attachments> atts = std::move(atts_);
        atts_.clear();
        queue_.push_back({std::move(batch), std::move(atts), BatchStatus{reason, dropped_}});
        dropped_ = 0;
        cv_.notify_all();
    }

    // The batch thread: the only place the callback is called (L12, D10), one batch at a time, in
    // window order, with mu_ released. It also closes a window whose deadline it reaches. It ends
    // only once closed and with nothing left to deliver, so kClosing is its last delivery (L9).
    void Loop() {
        std::unique_lock<std::mutex> lk(mu_);
        loop_id_ = std::this_thread::get_id();
        for (;;) {
            if (!queue_.empty()) {
                Delivery d = std::move(queue_.front());
                queue_.pop_front();
                lk.unlock();
                // Isolate the user callback: a throw would otherwise escape this thread (L10).
                try {
                    cb_(std::move(d.batch), std::move(d.atts), d.status);
                } catch (...) {
                }
                lk.lock();
            } else if (stopped_) {
                break;
            } else if (!has_deadline_) {
                cv_.wait(lk);
            } else if (std::chrono::steady_clock::now() >= deadline_) {
                Close(BatchStatus::Reason::kTimeout);
            } else {
                cv_.wait_until(lk, deadline_);
            }
        }
        // Nothing calls the callback or decodes again. Release both here, before the loop reports
        // its end, so the user's callback (and what it captured) is gone by the time any waiting
        // closing call returns, rather than whenever a detached thread drops the last owner.
        RecordBatchCallback cb = std::move(cb_);
        cb_ = nullptr;
        std::unique_ptr<BatchDecoder> decoder = std::move(decoder_);
        lk.unlock();
        cb = nullptr;
        decoder.reset();
        lk.lock();
        ended_ = true;
        lk.unlock();
        cv_.notify_all();
    }

    RecordBatchCallback cb_;
    int64_t max_rows_;
    std::chrono::milliseconds timeout_;
    const void* provider_;  // identity token only, never dereferenced

    // Every notify on cv_ is notify_all: the loop and any number of Stop() callers wait on it.
    std::mutex mu_;
    std::condition_variable cv_;
    std::unique_ptr<BatchDecoder> decoder_;
    std::vector<Attachments> atts_;
    int64_t dropped_ = 0;
    std::deque<Delivery> queue_;
    std::chrono::steady_clock::time_point deadline_;
    std::chrono::steady_clock::time_point last_flush_{std::chrono::steady_clock::now()};
    static constexpr std::chrono::milliseconds kMinWindow{1};
    bool has_deadline_ = false;
    bool stopped_ = false;
    std::atomic<bool> decided_{false};
    std::atomic<bool> ended_{true};  // true until Start(), so Stop() never waits on no thread
    std::thread::id loop_id_;
    std::thread thread_;
};

// -----------------------------------------------------------------------
// Construction
// -----------------------------------------------------------------------

SubscriberArrow::SubscriberArrow(std::shared_ptr<PubSubProvider> provider)
    : provider_token_(static_cast<const PubSubProvider*>(provider.get())),
      subscriber_(std::make_unique<Subscriber>(std::move(provider))) {}

SubscriberArrow::~SubscriberArrow() {
    // A snapshot, not a clear: a callback that calls Unsubscribe meanwhile still finds its entry.
    // Each Stop() waits for that subscription's last delivery, except where this destructor runs
    // inside that subscription's own callback: it delivers the rest once the callback returns (L9).
    std::vector<std::shared_ptr<RecordBatchBatcher>> to_stop;
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto& entry : batchers_) to_stop.push_back(entry.second);
    }
    for (auto& b : to_stop) static_cast<void>(b->Stop());

    // Tear down the Subscriber here, while mu_/codecs_ are still alive.
    // Member destruction order would otherwise destroy subscriber_ last
    // (it's the first member), so subscriber lambdas unwound by
    // ~Subscriber could touch mu_ and codecs_ after they're gone.
    subscriber_.reset();
}

// -----------------------------------------------------------------------
// Subscribe — ArrowRow per-row delivery
// -----------------------------------------------------------------------

SubscriberArrow::SubscribeResult SubscriberArrow::Subscribe(
    const std::vector<std::string>& segments, SubscribeCallback callback,
    const TopicOptions& options) {
    std::string key = internal::JoinSegments(segments);

    Subscriber::SubscribeResult result = subscriber_->Subscribe(
        segments,
        [this, key, cb = std::move(callback)](uint64_t /*sub_id*/, const uint8_t* data, size_t len,
                                              const SharedSchema& schema, const Attachments& att) {
            // Lazy codec acquisition from the per-message schema: in
            // subscriber-first mode the codec is not registered at Subscribe
            // time (no prior CreateTopic), and the provider may deliver before
            // Subscribe returns. AcquireCodec builds + caches it on first use.
            TopicCodec found = AcquireCodec(key, schema);
            if (!found.codec) {
                return;
            }
            ArrowRow row = found.codec->DecodeRow(data, len);
            cb(std::move(row), att);
        },
        options);

    // Track sub_id -> topic_key so Unsubscribe can release the codec
    // entry when the last subscription for a topic is removed.
    {
        std::lock_guard lock(mu_);
        sub_topic_[result.subscription_id] = key;
    }

    // Subscribe is non-blocking, and the arrival is handed straight back: no
    // deferred task wraps it any more, because the seam's own SchemaArrival is
    // already the non-blocking, deadline-bearing wait. A caller that wants an
    // arrow::Schema calls fletcher::ImportArrowSchema on what the wait yields —
    // one mechanism, the same one a C#/Rust caller uses.
    return {result.subscription_id, result.schema};
}

// -----------------------------------------------------------------------
// Subscribe — batched RecordBatch delivery
// -----------------------------------------------------------------------

SubscriberArrow::SubscribeResult SubscriberArrow::Subscribe(
    const std::vector<std::string>& segments, RecordBatchCallback callback, BatchOptions options,
    const TopicOptions& topic_options) {
    std::string key = internal::JoinSegments(segments);

    auto batcher = std::make_shared<RecordBatchBatcher>(std::move(callback), options.max_rows,
                                                        options.timeout, provider_token_);
    batcher->Start(batcher);  // the batch thread co-owns; see Start()'s comment

    // If subscribing throws, the batch thread is already running: close it, so that it ends and
    // releases the callback, then let the exception go.
    struct StopOnThrow {
        RecordBatchBatcher* batcher;
        ~StopOnThrow() {
            if (batcher) static_cast<void>(batcher->Stop());
        }
    } stop_on_throw{batcher.get()};
    Subscriber::SubscribeResult result = subscriber_->Subscribe(
        segments,
        [this, key, batcher](uint64_t /*sub_id*/, const uint8_t* data, size_t len,
                             const SharedSchema& schema, const Attachments& att) {
            // Lazy-init the schema from the per-message one: in subscriber-first mode (no prior
            // CreateTopic) nothing is registered yet and the provider can deliver before Subscribe
            // returns. A row that brings no schema leaves the question open for the next row. Once
            // decided, the per-row path never asks again, so it takes no process-wide lock.
            if (!batcher->SchemaDecided()) {
                TopicCodec found = AcquireCodec(key, schema);
                // Decided when the schema is known, or its import threw (permanent, L8). A schema
                // that is merely absent (null, or nothing to import) decides nothing.
                if (found.arrow_schema || found.import_failed) {
                    batcher->SetSchema(std::move(found.arrow_schema));
                }
            }
            // The one entry for every transport row. Without a decoder (no usable schema yet, or
            // one that was rejected) AddRow counts the row dropped, so the loss is reported at the
            // window's deadline or at close.
            batcher->AddRow(data, len, att);
        },
        topic_options);

    {
        std::lock_guard lock(mu_);
        batchers_[result.subscription_id] = batcher;
        sub_topic_[result.subscription_id] = key;
    }
    stop_on_throw.batcher = nullptr;  // registered: the batcher is closed by the usual paths

    // Subscribe is non-blocking (mirrors the ArrowRow overload) and never waits for a schema
    // (L13). The batcher is handed the schema by the first row that brings one, above. Rows that
    // arrive before that are not decoded: they are counted dropped and reported in their window's
    // rows_dropped, as a null batch when that window closes before a usable schema arrives (at its
    // deadline or at close), so subscriber-first works, and its early windows are told as losses
    // rather than held back.
    return {result.subscription_id, result.schema};
}

SubscriberArrow::TopicCodec SubscriberArrow::AcquireCodec(const std::string& key,
                                                           const SharedSchema& schema) {
    std::lock_guard lock(mu_);
    auto it = codecs_.find(key);
    if (it != codecs_.end()) return it->second;  // copies both owners; nothing is looked up again
    if (!schema) return {};
    std::shared_ptr<arrow::Schema> arrow_schema;
    try {
        arrow_schema = ImportArrowSchema(schema);
    } catch (...) {
        return {nullptr, nullptr, true};
    }
    if (!arrow_schema) return {};  // nothing to import (a released handle): not a decision
    TopicCodec entry{arrow_schema, std::make_shared<Codec>(arrow_schema), false};
    codecs_.emplace(key, entry);
    return entry;
}

void SubscriberArrow::Unsubscribe(uint64_t subscription_id) {
    // A batcher leaves batchers_ only once its deliveries have ended, so a racing Unsubscribe of
    // the same id, or the destructor, always finds it and waits for the same drain (L9).
    std::shared_ptr<RecordBatchBatcher> batcher;
    std::vector<std::shared_ptr<RecordBatchBatcher>> ended;
    {
        std::lock_guard lock(mu_);
        for (auto it = batchers_.begin(); it != batchers_.end();) {
            if (it->first == subscription_id) batcher = it->second;
            if (it->first != subscription_id && it->second->Ended()) {
                ended.push_back(std::move(it->second));
                it = batchers_.erase(it);
            } else {
                ++it;
            }
        }
    }
    ended.clear();  // released outside mu_
    // Every finished window, then the kClosing batch, is delivered before the Subscriber stops
    // delivering rows. No-op for non-batched (ArrowRow) subscriptions.
    if (batcher && batcher->Stop()) {
        std::lock_guard lock(mu_);
        batchers_.erase(subscription_id);
    }
    batcher.reset();
    subscriber_->Unsubscribe(subscription_id);

    // Clean up the per-subscription topic-key bookkeeping and free the codec
    // entry if this was the last subscription for that topic.
    std::lock_guard lock(mu_);
    auto it = sub_topic_.find(subscription_id);
    if (it == sub_topic_.end()) {
        return;
    }
    std::string key = std::move(it->second);
    sub_topic_.erase(it);

    bool any_remaining = false;
    for (const auto& [_, topic] : sub_topic_) {
        if (topic == key) {
            any_remaining = true;
            break;
        }
    }
    if (!any_remaining) {
        codecs_.erase(key);
    }
}

SchemaArrival SubscriberArrow::SubscribeSchema(const std::vector<std::string>& segments) {
    return subscriber_->SubscribeSchema(segments);
}

void SubscriberArrow::UnsubscribeSchema(const std::vector<std::string>& segments) {
    subscriber_->UnsubscribeSchema(segments);
}

}  // namespace fletcher
