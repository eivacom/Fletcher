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
#include <fletcher/arrow_bridge/batch_decoder.hpp>
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
    RecordBatchBatcher(RecordBatchCallback cb, int64_t max_rows, std::chrono::milliseconds timeout)
        : cb_(std::move(cb)), max_rows_(max_rows < 1 ? 1 : max_rows), timeout_(timeout) {}

    ~RecordBatchBatcher() { Stop(); }

    RecordBatchBatcher(const RecordBatchBatcher&) = delete;
    RecordBatchBatcher& operator=(const RecordBatchBatcher&) = delete;

    // Starts the timer thread, given a shared_ptr to this same batcher so the thread is a
    // co-owner, not a borrower. Flush() runs the user callback with mu_ released, and that
    // callback may call SubscriberArrow::Unsubscribe, which drops the SubscriberArrow's own
    // shared_ptr to this batcher; without a second owner that would destroy *this* while the
    // timer thread is still inside Flush()/TimerLoop(). The lambda's `self` capture is destroyed
    // only after TimerLoop() returns, i.e. after mu_ is released, so a detach (see Stop()) leaves
    // the final destructor running on the timer thread with mu_ free and `timer_` moved-from;
    // ~RecordBatchBatcher's call to Stop() is then idempotent (already stopped_, empty timer_)
    // and neither joins nor detaches again. Must be called once, right after construction, before
    // the batcher can be reached by any delivery.
    void Start(std::shared_ptr<RecordBatchBatcher> self) {
        timer_ = std::thread([self = std::move(self)] { self->TimerLoop(); });
    }

    // Provides the schema once decided (called at most once per subscription). Until a non-null
    // schema is set, and for a null one (the delivered schema could not be imported) or one the
    // decoder cannot build (see BatchDecoder), decoder_ stays null: every row is then counted
    // dropped and reported with a null batch, so the loss is visible rather than silent.
    void SetSchema(std::shared_ptr<arrow::Schema> schema) {
        std::lock_guard<std::mutex> lk(mu_);
        if (schema) {
            try {
                decoder_ = std::make_unique<BatchDecoder>(schema);
                decoder_->Reserve(max_rows_);
            } catch (...) {
                decoder_.reset();
            }
        }
    }

    // Decodes one wire row straight into the pending batch. Runs on the
    // provider's delivery thread (a DDS listener), so nothing may escape:
    // every failure is folded into the drop count.
    void AddRow(const uint8_t* data, size_t len, const Attachments& att) {
        std::unique_lock<std::mutex> lk(mu_);
        if (stopped_) return;
        if (!decoder_) {
            ++dropped_;
            ArmTimer();
            return;
        }
        try {
            decoder_->Append(data, len);
            atts_.push_back(att);
        } catch (const BatchCapacityExceeded&) {
            // A well-formed row that does not fit a 32-bit Arrow offset any
            // more: close this batch and start the next one with it.
            Flush(lk, BatchStatus::Reason::kRowLimit);
            // Flush() ran the callback with mu_ released, so it may have called
            // Unsubscribe -> Stop() in the meantime; don't touch the decoder if so.
            if (stopped_) return;
            try {
                decoder_->Append(data, len);
                atts_.push_back(att);
            } catch (const BatchCapacityExceeded&) {
                ++dropped_;  // the row alone exceeds the budget; nothing was appended
            } catch (const std::invalid_argument&) {
                ++dropped_;  // malformed after all (Skip stopped early the first time)
            } catch (...) {
                DiscardPending(1);
            }
        } catch (const std::invalid_argument&) {
            // Malformed row: nothing was appended; its attachment goes with
            // it (the metadata naming the blob lived in the row).
            ++dropped_;
        } catch (...) {
            DiscardPending(1);
        }
        ArmTimer();
        if (decoder_->num_rows() >= max_rows_) {
            Flush(lk, BatchStatus::Reason::kRowLimit);
        }
    }

    // Stops the timer thread and delivers any pending rows/drops (reason
    // kClosing). Idempotent.
    void Stop() {
        std::thread t;
        {
            std::unique_lock<std::mutex> lk(mu_);
            if (!stopped_) {
                stopped_ = true;
                Flush(lk, BatchStatus::Reason::kClosing);
                cv_.notify_all();
            }
            t = std::move(timer_);
        }
        // Don't join ourselves if a callback on the timer thread called Stop().
        if (t.joinable()) {
            if (t.get_id() == std::this_thread::get_id()) {
                t.detach();
            } else {
                t.join();
            }
        }
    }

   private:
    bool HasPending() const { return (decoder_ && decoder_->num_rows() > 0) || dropped_ > 0; }

    // Internal failure (allocation) inside Append() or Finish(): the pending window is undefined,
    // so count it dropped whole (plus `extra` for a row that was being appended), drop its
    // attachments, and reset the decoder with a Finish() whose result is discarded. That Finish()
    // is wrapped too: under the memory pressure that just failed, it can fail as well, and nothing
    // may escape the delivery, timer or teardown thread.
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

    void TimerLoop() {
        std::unique_lock<std::mutex> lk(mu_);
        while (!stopped_) {
            if (!has_deadline_) {
                cv_.wait(lk, [this] { return stopped_ || has_deadline_; });
                continue;
            }
            // Wake on stop, on the deadline being cleared (flushed by count), or
            // when the deadline is reached.
            if (cv_.wait_until(lk, deadline_, [this] { return stopped_ || !has_deadline_; })) {
                continue;
            }
            // Every reached deadline flushes: with no decoder the window closes as a null batch
            // reporting its drops, and an idle window finds nothing pending (L5, L6).
            Flush(lk, BatchStatus::Reason::kTimeout);
        }
    }

    // Hands the pending batch to the callback. A window with only dropped
    // rows still delivers a zero-row batch (or a null one when there is no
    // decoder: no usable schema has arrived, or the schema was rejected) so
    // the loss is reported. Caller holds `lk`; the callback runs with it released.
    void Flush(std::unique_lock<std::mutex>& lk, BatchStatus::Reason reason) {
        has_deadline_ = false;
        last_flush_ = std::chrono::steady_clock::now();
        if (!HasPending()) return;  // truly idle — nothing to deliver

        // Finish() only hands the builders' buffers over — O(1) per column —
        // so it stays under the lock and the delivery thread never sees a
        // half-built batch.
        std::shared_ptr<arrow::RecordBatch> batch;
        if (decoder_) {
            try {
                batch = decoder_->Finish();
            } catch (...) {
                // Finish() itself failed (allocation, a dictionary re-fold that
                // does not fit its index type): no batch row is left to align an
                // attachment with, so the window is dropped whole.
                DiscardPending(0);
            }
            // A failed pre-size is not fatal: the next Append() just grows the
            // builders as it goes.
            try {
                decoder_->Reserve(max_rows_);
            } catch (...) {
            }
        }
        std::vector<Attachments> atts = std::move(atts_);
        atts_.clear();
        int64_t dropped = dropped_;
        dropped_ = 0;

        lk.unlock();
        // Isolate the user callback: a throw here would otherwise escape the
        // timer thread (→ std::terminate) or propagate out of ~SubscriberArrow
        // via Stop()'s kClosing flush. Internal threads must not depend on
        // user code being noexcept.
        try {
            cb_(std::move(batch), std::move(atts), BatchStatus{reason, dropped});
        } catch (...) {
        }
        lk.lock();
    }

    RecordBatchCallback cb_;
    int64_t max_rows_;
    std::chrono::milliseconds timeout_;

    std::mutex mu_;
    std::condition_variable cv_;
    std::unique_ptr<BatchDecoder> decoder_;
    std::vector<Attachments> atts_;
    int64_t dropped_ = 0;
    std::chrono::steady_clock::time_point deadline_;
    std::chrono::steady_clock::time_point last_flush_{std::chrono::steady_clock::now()};
    static constexpr std::chrono::milliseconds kMinWindow{1};
    bool has_deadline_ = false;
    bool stopped_ = false;
    std::thread timer_;
};

// -----------------------------------------------------------------------
// Construction
// -----------------------------------------------------------------------

SubscriberArrow::SubscriberArrow(std::shared_ptr<PubSubProvider> provider)
    : subscriber_(std::make_unique<Subscriber>(std::move(provider))) {}

SubscriberArrow::~SubscriberArrow() {
    std::vector<std::shared_ptr<RecordBatchBatcher>> to_stop;
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto& entry : batchers_) to_stop.push_back(entry.second);
        batchers_.clear();
    }
    for (auto& b : to_stop) b->Stop();  // join timer threads + deliver closing flush

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
                                                        options.timeout);
    batcher->Start(batcher);  // timer thread co-owns; see Start()'s comment
    auto schema_set = std::make_shared<std::once_flag>();
    // Set once the schema has been decided for this subscription (usable or not): afterwards the
    // per-row path never asks again, so it takes no process-wide lock and hashes no topic key.
    auto schema_decided = std::make_shared<std::atomic<bool>>(false);

    Subscriber::SubscribeResult result = subscriber_->Subscribe(
        segments,
        [this, key, batcher, schema_set, schema_decided](
            uint64_t /*sub_id*/, const uint8_t* data, size_t len, const SharedSchema& schema,
            const Attachments& att) {
            // Lazy-init the schema from the per-message one: in subscriber-first mode (no prior
            // CreateTopic) nothing is registered yet and the provider can deliver before Subscribe
            // returns. A row that brings no schema leaves the question open for the next row.
            if (!schema_decided->load(std::memory_order_acquire)) {
                TopicCodec found = AcquireCodec(key, schema);
                // Decided when the schema is known, or its import threw (permanent, L8). A schema
                // that is merely absent (null, or nothing to import) decides nothing.
                if (found.arrow_schema || found.import_failed) {
                    std::call_once(*schema_set,
                                   [&] { batcher->SetSchema(std::move(found.arrow_schema)); });
                    schema_decided->store(true, std::memory_order_release);
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
        batchers_[result.subscription_id] = std::move(batcher);
        sub_topic_[result.subscription_id] = key;
    }

    // Subscribe is non-blocking (mirrors the ArrowRow overload) and never waits for a schema
    // (L13). The batcher is handed the schema by the first row that brings one, above. Rows that
    // arrive before that are not decoded: they are counted dropped and reported with a null batch
    // at the window's deadline or at close, so subscriber-first works, and its early windows are
    // told as losses rather than held back.
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
    std::shared_ptr<RecordBatchBatcher> batcher;
    {
        std::lock_guard lock(mu_);
        auto it = batchers_.find(subscription_id);
        if (it != batchers_.end()) {
            batcher = std::move(it->second);
            batchers_.erase(it);
        }
    }
    // Flush the partial batch (reason kClosing) and join the timer before the
    // Subscriber stops delivering. No-op for non-batched (ArrowRow) subscriptions.
    if (batcher) batcher->Stop();
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
