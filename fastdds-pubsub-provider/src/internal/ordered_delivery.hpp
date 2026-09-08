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
// samples. The callback runs with the lock released (it may re-enter Offer).
//
// It no longer clears the drain flag on an exception path from the CALLBACK,
// because there is no such path left: every dispatch here goes through
// DeliveryChannel::Deliver, which is `noexcept` and absorbs what a handler throws
// at the site it threw (spec 5.3, owner ruling 2026-09-05). The three
// `catch (...) { draining_ = false; throw; }` pairs that used to guard against
// wedging are gone with the throws they guarded against; rethrowing into a Fast
// DDS listener thread that holds the RTPS reader mutex was never a recovery
// anyway.
//
// One narrower path remains, unchanged by that and never covered by those
// catches either: the `lk.lock()` taken AFTER a delivery can itself throw
// std::system_error under resource exhaustion, and `draining_` then stays true
// and this reader stops draining. Pre-existing, not introduced here, and left
// alone deliberately -- a process that cannot lock a mutex has a larger problem
// than one wedged reader, and the honest statement is that the flag is exception-
// safe against the callback, not against the machine.
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
#include <fletcher/pubsub/delivery_channel.hpp>
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
    explicit OrderedDelivery(DeliveryChannel channel, SharedSchema schema = nullptr,
                             size_t max_queued = 0)
        : channel_(std::move(channel)),
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
        NoteQueuedLocked();
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
            NoteQueuedLocked();
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
        channel_.Deliver(row, len, schema, attachments);
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

    // Call under mu_ after any mutation that can change whether the queue is empty. seq_cst for
    // the reason spelled out in DeliverSteady: this store and the `draining_` read that follows it
    // in DrainLocked are one half of a StoreLoad pair.
    void NoteQueuedLocked() { queued_.store(!queue_.empty(), std::memory_order_seq_cst); }

    // Trims to max_queued_, which is at least 1, so emptiness cannot change.
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

    // False when re-entered from the callback, so the caller queues and delivery stays iterative.
    bool DeliverSteady(const uint8_t* row, size_t len, const Attachments& attachments) {
        bool expected = false;
        if (!draining_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return false;
        }
        channel_.Deliver(row, len, schema_, attachments);
        draining_.store(false, std::memory_order_seq_cst);
        // Anything queued while the callback ran is this thread's to drain, because the thread that
        // queued it saw `draining_` set and bailed. Both operations here are seq_cst, as are their
        // opposite numbers under mu_ (NoteQueuedLocked's store, DrainLocked's read of draining_):
        // storing one flag and then loading the other is a StoreLoad pair, which release/acquire
        // does NOT order. Without a single total order both threads can miss each other and strand
        // the sample until the next Offer — which would then deliver it out of order, the one thing
        // this class exists to prevent. Only one thread offers here in practice (see steady_), so
        // this is belt and braces; it costs one fence per delivered sample.
        if (queued_.load(std::memory_order_seq_cst)) {
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
        // The schema copy sits below the empty check: hoisting it cost +7 ns per sample.
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
            NoteQueuedLocked();
            lk.unlock();
            channel_.Deliver(sample.row.data(), sample.row.size(), schema, sample.att);
            lk.lock();
        }
        draining_ = false;
        MarkSteadyLocked();
    }

    // The one dispatch mechanism, shared with the other two providers: a handler
    // failure is absorbed here rather than unwound into a listener thread, and the
    // delivery frame it pushes is what the provider's Unsubscribe door asks about.
    // Held as a MEMBER and delivered through directly, where the loopback and
    // XRCE copy the channel to a local first (HARD-4 / issue #62: a callback that
    // re-entered Unsubscribe would destroy the std::function being invoked).
    // Safe here only because the door now refuses that re-entrant Unsubscribe
    // before it can reach `delete_datareader` and destroy this object. If §6
    // clause 6 is ever relaxed -- AG1-DEBT-19 asks PDA-ABI to consider exactly
    // that -- these three dispatch sites need the copy-to-local the other two
    // providers already have.
    DeliveryChannel channel_;
    std::mutex mu_;
    SharedSchema schema_;
    bool schema_ready_ = false;
    // Atomic because DeliverSteady tests and sets it without mu_.
    std::atomic<bool> draining_{false};
    std::deque<PendingSample> queue_;
    // Queue emptiness, published for DeliverSteady, which does not take mu_.
    std::atomic<bool> queued_{false};
    size_t max_queued_ = 0;
    bool warned_ = false;

    // Latched after the handoff; its release/acquire pair carries schema_ to the latched path.
    //
    // Once latched there is exactly one thread offering: the schema listener fires once and is then
    // finished, and Fast DDS serialises every on_data_available for one reader under that reader's
    // own RTPS mutex — StatefulReader::process_data_msg takes it and still holds it through
    // change_received, NotifyChanges and the listener call, and the data-sharing thread reaches the
    // same place through the same function (verified against Fast DDS 3.4.0). That is an upstream
    // implementation detail rather than an API guarantee, which is why the two flags above do not
    // lean on it.
    std::atomic<bool> steady_{false};
};

}  // namespace internal
}  // namespace fletcher

#endif  // FLETCHER_FASTDDS_PUBSUB_PROVIDER_INTERNAL_ORDERED_DELIVERY_HPP_
