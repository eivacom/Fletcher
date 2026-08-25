// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// Orders subscriber delivery across the schema handoff.
//
// A subscriber that joins before the publisher buffers a backlog of samples
// until the topic schema arrives, then flushes it. The flush runs on the
// schema-listener thread while fresh live samples are still being delivered
// on the data-reader thread. Delivering from both threads concurrently let a
// live sample overtake the backlog being flushed — breaking the per-writer
// order that a single writer + RELIABLE QoS otherwise guarantees.
//
// OrderedDelivery removes that race: every sample (backlog or live) is held in
// one FIFO and delivered to the callback by a single drainer. A sample offered
// while a drain is already in progress is appended behind the in-flight
// backlog rather than delivered inline, so it can never overtake earlier
// samples. The callback runs with the lock released (it may re-enter Offer);
// the drain flag is cleared even if the callback throws, so delivery cannot
// wedge.
//
// **That race exists only during startup, and this class charges for it only during startup.** The
// schema listener fires once and is then finished, and Fast DDS serialises every on_data_available
// for one reader under the RTPS reader's own mutex — so once the schema is known and the backlog is
// gone there is a single caller left and nothing to order. `steady_` latches at that point and both
// Offer paths hand the sample straight to the callback: no mutex, no queue, no schema copy. See
// steady_ at the bottom for why that is sound.
#ifndef FLETCHER_FASTDDS_PUBSUB_PROVIDER_INTERNAL_ORDERED_DELIVERY_HPP_
#define FLETCHER_FASTDDS_PUBSUB_PROVIDER_INTERNAL_ORDERED_DELIVERY_HPP_

#include <atomic>
#include <cstdint>
#include <deque>
#include <fastdds/dds/log/Log.hpp>
#include <fletcher/core/types.hpp>
#include <fletcher/pubsub/owned_schema.hpp>
#include <fletcher/pubsub/provider.hpp>
#include <mutex>
#include <utility>
#include <vector>

namespace fletcher {
namespace internal {

class OrderedDelivery {
   public:
    // `max_queued` bounds the pre-schema backlog. A subscriber that starts before its publisher
    // buffers everything that arrives until the schema does, and if no publisher ever appears that
    // is unbounded growth on a reachable path. Dropping the oldest is what KEEP_LAST would have
    // done to the same samples had the reader been able to decode them yet.
    explicit OrderedDelivery(PubSubProvider::SubscribeCallback callback,
                             SharedSchema schema = nullptr, size_t max_queued = 0)
        : callback_(std::move(callback)),
          schema_(std::move(schema)),
          schema_ready_(schema_ != nullptr),
          max_queued_(max_queued) {}

    // Enqueue a sample. Delivered in order once the schema is known; held
    // until then (so the callback is never invoked with a null schema).
    void Offer(const std::vector<uint8_t>& row, const Attachments& attachments) {
        if (steady_.load(std::memory_order_acquire) &&
            DeliverSteady(row.data(), row.size(), attachments)) {
            return;
        }
        std::unique_lock<std::mutex> lk(mu_);
        queue_.push_back({row, attachments});
        TrimLocked();
        if (schema_ready_) {
            DrainLocked(lk);
        }
    }

    // Offer a sample the caller only lends for the duration of this call (a
    // loaned DDS payload). Delivered inline — no copy — when the schema is
    // known and nothing is queued ahead of it; otherwise the bytes are copied,
    // because the view dies when this call returns.
    void OfferView(const uint8_t* row, size_t len, const Attachments& attachments) {
        if (steady_.load(std::memory_order_acquire) && DeliverSteady(row, len, attachments)) {
            return;
        }
        std::unique_lock<std::mutex> lk(mu_);
        if (!schema_ready_ || draining_ || !queue_.empty()) {
            queue_.push_back({std::vector<uint8_t>(row, row + len), attachments});
            TrimLocked();
            if (schema_ready_) {
                DrainLocked(lk);
            }
            return;
        }

        // Claim the drain slot so a sample offered from inside the callback is
        // queued behind this one instead of overtaking it.
        draining_ = true;
        SharedSchema schema = schema_;
        lk.unlock();
        try {
            callback_(row, len, schema, attachments);
        } catch (...) {
            lk.lock();
            draining_ = false;
            throw;
        }
        lk.lock();
        draining_ = false;
        DrainLocked(lk);
    }

    // Supply the schema once known, then drain everything buffered so far.
    void SetSchema(SharedSchema schema) {
        // A null schema never satisfies the schema-before-data contract: keep
        // buffering until a real schema arrives rather than draining with null.
        if (!schema) {
            return;
        }
        std::unique_lock<std::mutex> lk(mu_);
        if (schema_ready_) {
            return;
        }
        schema_ = std::move(schema);
        schema_ready_ = true;
        DrainLocked(lk);
    }

   private:
    struct PendingSample {
        std::vector<uint8_t> row;
        Attachments att;
    };

    // Drops the oldest samples once the backlog exceeds its bound. Warns once, because the drop
    // that matters is the first one — after that it is a steady state, not an event.
    void TrimLocked() {
        if (max_queued_ == 0 || queue_.size() <= max_queued_) return;
        while (queue_.size() > max_queued_) {
            queue_.pop_front();
        }
        if (!warned_) {
            warned_ = true;
            EPROSIMA_LOG_WARNING(FLETCHER_DELIVERY, "pre-schema backlog exceeded "
                                                        << max_queued_
                                                        << " samples; dropping the oldest");
        }
    }

    // The latched path: deliver without touching the mutex or the queue. Single-threaded by
    // construction (see steady_), so draining_ and queue_ are read here unlocked — no other thread
    // is left to race with.
    //
    // Returns false when re-entered from inside the callback, so the caller queues instead. That
    // keeps the guarantee MidFlushOfferIsNotDeliveredInline pins down: a sample offered while a
    // delivery is in progress lands *after* it, never nested inside it. Nesting would also let a
    // callback that re-offers recurse until the stack runs out, where the queue makes it iterative.
    bool DeliverSteady(const uint8_t* row, size_t len, const Attachments& attachments) {
        if (draining_) {
            return false;
        }
        draining_ = true;
        try {
            callback_(row, len, schema_, attachments);
        } catch (...) {
            draining_ = false;
            throw;
        }
        draining_ = false;
        // Anything the callback re-offered is waiting; drain it the ordinary way.
        if (!queue_.empty()) {
            std::unique_lock<std::mutex> lk(mu_);
            DrainLocked(lk);
        }
        return true;
    }

    // The handoff is over — schema known, nothing buffered — so every later sample can go straight
    // to the callback. Runs with the lock held, which is what makes publishing steady_ safe.
    void MarkSteadyLocked() {
        if (schema_ready_ && queue_.empty() && !draining_) {
            steady_.store(true, std::memory_order_release);
        }
    }

    // Delivers queued samples in FIFO order. At most one thread drains at a
    // time: a second caller that enqueues mid-drain returns immediately and
    // leaves its sample for the active drainer, preserving order.
    void DrainLocked(std::unique_lock<std::mutex>& lk) {
        // An empty queue leaves before anything is paid for. OfferView's fast path calls this after
        // every inline delivery precisely to find it empty, so hoisting the schema copy above the
        // loop without this measured +7 ns on every loaned sample — the copy moved from once per
        // queued sample to once per call.
        if (draining_) {
            return;
        }
        if (queue_.empty()) {
            MarkSteadyLocked();
            return;
        }
        draining_ = true;
        // Fixed for the whole drain: SetSchema assigns schema_ once, before schema_ready_, and
        // nothing drains before that.
        const SharedSchema schema = schema_;
        while (!queue_.empty()) {
            PendingSample sample = std::move(queue_.front());
            queue_.pop_front();
            lk.unlock();
            try {
                callback_(sample.row.data(), sample.row.size(), schema, sample.att);
            } catch (...) {
                lk.lock();
                draining_ = false;
                throw;
            }
            lk.lock();
        }
        draining_ = false;
        MarkSteadyLocked();
    }

    PubSubProvider::SubscribeCallback callback_;
    std::mutex mu_;
    SharedSchema schema_;
    bool schema_ready_ = false;
    bool draining_ = false;
    std::deque<PendingSample> queue_;
    size_t max_queued_ = 0;
    bool warned_ = false;

    // Latched once the handoff is over, never cleared, and the reason everything above stops
    // costing anything in the steady state.
    //
    // Two threads can be in here, and only during startup: the data reader's listener (Offer /
    // OfferView) and the __schema reader's listener (SetSchema). Fast DDS invokes on_data_available
    // holding the RTPS reader's own mutex — StatefulReader::process_data_msg takes it and holds
    // it through change_received, NotifyChanges and the listener call, and the data-sharing
    // thread reaches the same place through the same function — so all calls for *one* reader
    // are serialised. The schema listener fires once (SchemaListener::fired_) and never returns.
    // So after the backlog drains there is a single caller left, and the queue and mutex have no
    // work to do: the sample goes straight to the callback.
    //
    // Publishing is safe because MarkSteadyLocked runs under mu_ and only when the queue is
    // observably empty, and reading is safe because schema_ is written once before it is set.
    std::atomic<bool> steady_{false};
};

}  // namespace internal
}  // namespace fletcher

#endif  // FLETCHER_FASTDDS_PUBSUB_PROVIDER_INTERNAL_ORDERED_DELIVERY_HPP_
