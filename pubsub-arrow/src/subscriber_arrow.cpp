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
#include <fletcher/pubsub/owned_schema.hpp>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace fletcher {

namespace {

std::shared_ptr<arrow::Schema> ImportFromNano(const ArrowSchema* schema) {
    if (!schema || !schema->release) {
        return nullptr;
    }
    OwnedSchema copy = OwnedSchema::DeepCopy(schema);
    auto result = arrow::ImportSchema(copy.get());
    if (!result.ok()) {
        throw std::runtime_error("SubscriberArrow: ImportSchema: " + result.status().ToString());
    }
    return *result;
}

}  // anonymous namespace

// -----------------------------------------------------------------------
// RecordBatchBatcher — accumulates decoded rows into RecordBatches and
// flushes on row-count, timeout, or close (see the batched Subscribe).
// -----------------------------------------------------------------------

class SubscriberArrow::RecordBatchBatcher {
   public:
    RecordBatchBatcher(RecordBatchCallback cb, int64_t max_rows, std::chrono::milliseconds timeout)
        : cb_(std::move(cb)), max_rows_(max_rows < 1 ? 1 : max_rows), timeout_(timeout) {
        timer_ = std::thread([this] { TimerLoop(); });
    }

    ~RecordBatchBatcher() { Stop(); }

    RecordBatchBatcher(const RecordBatchBatcher&) = delete;
    RecordBatchBatcher& operator=(const RecordBatchBatcher&) = delete;

    // Provides the schema once known (from the subscription result). Until a
    // non-null schema is set the batcher buffers but cannot build batches.
    // A schema the decoder cannot build (see BatchDecoder) leaves decoder_
    // null — every row is then counted dropped and the periodic report
    // carries a null batch, so the loss is visible rather than silent.
    void SetSchema(std::shared_ptr<arrow::Schema> schema) {
        std::unique_lock<std::mutex> lk(mu_);
        schema_ = std::move(schema);
        if (schema_) {
            try {
                decoder_ = std::make_unique<BatchDecoder>(schema_);
                decoder_->Reserve(max_rows_);
            } catch (const std::invalid_argument&) {
                decoder_.reset();
            }
        }
        ready_ = (schema_ != nullptr);
        if (ready_ && decoder_ && decoder_->num_rows() >= max_rows_) {
            Flush(lk, BatchStatus::Reason::kRowLimit);
        }
        cv_.notify_all();
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
            try {
                decoder_->Append(data, len);
                atts_.push_back(att);
            } catch (...) {
                ++dropped_;
            }
        } catch (const std::invalid_argument&) {
            // Malformed row: nothing was appended; its attachment goes with
            // it (the metadata naming the blob lived in the row).
            ++dropped_;
        } catch (...) {
            // Internal failure (allocation): the pending window is undefined
            // — discard it whole. The reset Finish() is itself wrapped:
            // under the same memory pressure that just failed Append(), it
            // can fail too, and nothing may escape AddRow.
            dropped_ += decoder_->num_rows() + 1;
            atts_.clear();
            try {
                (void)decoder_->Finish();
            } catch (...) {
            }
        }
        ArmTimer();
        if (ready_ && decoder_->num_rows() >= max_rows_) {
            Flush(lk, BatchStatus::Reason::kRowLimit);
        }
    }

    // A row that failed to decode: counted as lost. Its attachment is dropped
    // with it, since the metadata identifying the attachment was in that row.
    void NoteDropped() {
        std::unique_lock<std::mutex> lk(mu_);
        if (stopped_) return;
        ++dropped_;
        ArmTimer();
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
            if (ready_) {
                Flush(lk, BatchStatus::Reason::kTimeout);
            } else {
                // Deadline reached but schema not ready yet — wait for it.
                cv_.wait(lk, [this] { return stopped_ || ready_ || !has_deadline_; });
            }
        }
    }

    // Hands the pending batch to the callback. Per design, a window with only
    // dropped rows still delivers a zero-row (or null, if the schema can't be
    // decoded) batch so the loss is reported. Caller holds `lk`; the callback
    // runs with it released.
    void Flush(std::unique_lock<std::mutex>& lk, BatchStatus::Reason reason) {
        has_deadline_ = false;
        last_flush_ = std::chrono::steady_clock::now();
        if (!ready_) return;        // schema not set — cannot build
        if (!HasPending()) return;  // truly idle — nothing to deliver

        // Finish() only hands the builders' buffers over — O(1) per column —
        // so it stays under the lock and the delivery thread never sees a
        // half-built batch.
        std::shared_ptr<arrow::RecordBatch> batch;
        std::vector<Attachments> atts = std::move(atts_);
        atts_.clear();
        if (decoder_) {
            try {
                batch = decoder_->Finish();
            } catch (const std::runtime_error&) {
                // Finish() itself failed (allocation): the decoder's row
                // count is now undefined. Count every pending row dropped —
                // its attachment goes with it, since there's no batch row
                // left to align it with — and reset the decoder with a
                // second Finish(), discarding the result.
                dropped_ += decoder_->num_rows();
                atts.clear();
                try {
                    (void)decoder_->Finish();
                } catch (...) {
                }
            }
            decoder_->Reserve(max_rows_);
        }
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
    std::shared_ptr<arrow::Schema> schema_;
    std::unique_ptr<BatchDecoder> decoder_;
    std::vector<Attachments> atts_;
    int64_t dropped_ = 0;
    std::chrono::steady_clock::time_point deadline_;
    std::chrono::steady_clock::time_point last_flush_{std::chrono::steady_clock::now()};
    static constexpr std::chrono::milliseconds kMinWindow{1};
    bool has_deadline_ = false;
    bool ready_ = false;
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
    const std::vector<std::string>& segments, SubscribeCallback callback) {
    std::string key = JoinSegments(segments);

    Subscriber::SubscribeResult result = subscriber_->Subscribe(
        segments,
        [this, key, cb = std::move(callback)](uint64_t /*sub_id*/, const uint8_t* data, size_t len,
                                              const SharedSchema& schema, const Attachments& att) {
            // Lazy codec acquisition from the per-message schema: in
            // subscriber-first mode the codec is not registered at Subscribe
            // time (no prior CreateTopic), and the provider may deliver before
            // Subscribe returns. AcquireCodec builds + caches it on first use.
            Codec* codec = AcquireCodec(key, schema);
            if (!codec) {
                return;
            }
            ArrowRow row = codec->DecodeRow(data, len);
            cb(std::move(row), att);
        });

    // Track sub_id -> topic_key so Unsubscribe can release the codec
    // entry when the last subscription for a topic is removed.
    {
        std::lock_guard lock(mu_);
        sub_topic_[result.subscription_id] = key;
    }

    // Subscribe is non-blocking. The SharedSchema -> arrow::Schema conversion is
    // deferred until the caller reads the future: the deferred task runs on the
    // caller's thread at .get() time and blocks only then (if at all). The
    // provider resolves the underlying schema future when the companion
    // /__schema sample arrives — independent of any data — so this is correct
    // for subscriber-first (no publisher yet) without ever blocking Subscribe.
    std::shared_future<std::shared_ptr<arrow::Schema>> schema_future =
        std::async(std::launch::deferred, [pf = result.schema]() -> std::shared_ptr<arrow::Schema> {
            SharedSchema nano = pf.get();
            return nano ? ImportFromNano(nano.get()) : nullptr;
        }).share();

    return {result.subscription_id, std::move(schema_future)};
}

// -----------------------------------------------------------------------
// Subscribe — batched RecordBatch delivery
// -----------------------------------------------------------------------

SubscriberArrow::SubscribeResult SubscriberArrow::Subscribe(
    const std::vector<std::string>& segments, RecordBatchCallback callback, BatchOptions options) {
    std::string key = JoinSegments(segments);

    auto batcher = std::make_shared<RecordBatchBatcher>(std::move(callback), options.max_rows,
                                                        options.timeout);
    auto schema_set = std::make_shared<std::once_flag>();
    // The codec, once resolved, for this subscription's samples. Codecs live in `codecs_` for the
    // SubscriberArrow's lifetime, so the pointer stays valid; without this every sample of every
    // topic took the process-wide `mu_` and hashed the topic key.
    auto cached_codec = std::make_shared<std::atomic<Codec*>>(nullptr);

    Subscriber::SubscribeResult result =
        subscriber_->Subscribe(segments, [this, key, batcher, schema_set, cached_codec](
                                             uint64_t /*sub_id*/, const uint8_t* data, size_t len,
                                             const SharedSchema& schema, const Attachments& att) {
            // Lazy-init the codec from the per-message schema: in
            // subscriber-first mode (no prior CreateTopic) the codec isn't
            // registered yet and the provider can deliver before Subscribe
            // returns. If no codec can be built, count the message as dropped
            // so the loss is reported.
            Codec* codec = cached_codec->load(std::memory_order_acquire);
            if (!codec) {
                codec = AcquireCodec(key, schema);
                if (codec) cached_codec->store(codec, std::memory_order_release);
            }
            if (!codec) {
                batcher->NoteDropped();
                return;
            }
            // Hand the batcher the schema on the first decodable sample so it
            // can build batches (idempotent; lets the batched path work
            // subscriber-first without blocking Subscribe on the schema).
            std::call_once(*schema_set, [&] {
                std::shared_ptr<arrow::Schema> arrow_schema;
                {
                    std::lock_guard lock(mu_);
                    auto it = codecs_.find(key);
                    if (it != codecs_.end()) arrow_schema = it->second.arrow_schema;
                }
                batcher->SetSchema(std::move(arrow_schema));
            });
            batcher->AddRow(data, len, att);
        });

    {
        std::lock_guard lock(mu_);
        batchers_[result.subscription_id] = std::move(batcher);
        sub_topic_[result.subscription_id] = key;
    }

    // Subscribe is non-blocking (mirrors the ArrowRow overload). The
    // SharedSchema -> arrow::Schema conversion is deferred to .get() time on the
    // caller's thread, and the batcher receives the schema lazily from the first
    // decodable sample above — so this works for subscriber-first without
    // blocking and without dropping the first window for want of a schema.
    std::shared_future<std::shared_ptr<arrow::Schema>> schema_future =
        std::async(std::launch::deferred, [pf = result.schema]() -> std::shared_ptr<arrow::Schema> {
            SharedSchema nano = pf.get();
            return nano ? ImportFromNano(nano.get()) : nullptr;
        }).share();
    return {result.subscription_id, std::move(schema_future)};
}

Codec* SubscriberArrow::AcquireCodec(const std::string& key, const SharedSchema& schema) {
    std::lock_guard lock(mu_);
    auto it = codecs_.find(key);
    if (it != codecs_.end()) return it->second.codec.get();
    if (!schema) return nullptr;
    std::shared_ptr<arrow::Schema> arrow_schema;
    try {
        arrow_schema = ImportFromNano(schema.get());
    } catch (...) {
        return nullptr;
    }
    if (!arrow_schema) return nullptr;
    auto codec = std::make_unique<Codec>(arrow_schema);
    Codec* codec_ptr = codec.get();
    codecs_.emplace(key, TopicCodec{arrow_schema, std::move(codec)});
    return codec_ptr;
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

std::string SubscriberArrow::JoinSegments(const std::vector<std::string>& segs) {
    if (segs.empty()) {
        return {};
    }
    std::string out = segs[0];
    for (size_t i = 1; i < segs.size(); ++i) {
        out += '/';
        out += segs[i];
    }
    return out;
}

}  // namespace fletcher
