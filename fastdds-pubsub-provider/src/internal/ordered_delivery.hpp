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
#ifndef FLETCHER_FASTDDS_PUBSUB_PROVIDER_INTERNAL_ORDERED_DELIVERY_HPP_
#define FLETCHER_FASTDDS_PUBSUB_PROVIDER_INTERNAL_ORDERED_DELIVERY_HPP_

#include <cstdint>
#include <deque>
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
    explicit OrderedDelivery(PubSubProvider::SubscribeCallback callback,
                             SharedSchema schema = nullptr)
        : callback_(std::move(callback)),
          schema_(std::move(schema)),
          schema_ready_(schema_ != nullptr) {}

    // Enqueue a sample. Delivered in order once the schema is known; held
    // until then (so the callback is never invoked with a null schema).
    void Offer(std::vector<uint8_t> row, Attachments attachments) {
        std::unique_lock<std::mutex> lk(mu_);
        queue_.push_back({std::move(row), std::move(attachments)});
        if (schema_ready_) {
            DrainLocked(lk);
        }
    }

    // Supply the schema once known, then drain everything buffered so far.
    void SetSchema(SharedSchema schema) {
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

    // Delivers queued samples in FIFO order. At most one thread drains at a
    // time: a second caller that enqueues mid-drain returns immediately and
    // leaves its sample for the active drainer, preserving order.
    void DrainLocked(std::unique_lock<std::mutex>& lk) {
        if (draining_) {
            return;
        }
        draining_ = true;
        while (!queue_.empty()) {
            PendingSample sample = std::move(queue_.front());
            queue_.pop_front();
            SharedSchema schema = schema_;
            lk.unlock();
            try {
                callback_(sample.row.data(), sample.row.size(), schema, std::move(sample.att));
            } catch (...) {
                lk.lock();
                draining_ = false;
                throw;
            }
            lk.lock();
        }
        draining_ = false;
    }

    PubSubProvider::SubscribeCallback callback_;
    std::mutex mu_;
    SharedSchema schema_;
    bool schema_ready_ = false;
    bool draining_ = false;
    std::deque<PendingSample> queue_;
};

}  // namespace internal
}  // namespace fletcher

#endif  // FLETCHER_FASTDDS_PUBSUB_PROVIDER_INTERNAL_ORDERED_DELIVERY_HPP_
