// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include "fletcher/pubsub/schema_arrival.hpp"

#include <condition_variable>
#include <mutex>
#include <utility>

namespace fletcher {
namespace internal {

struct SchemaArrivalState {
    std::mutex mu;
    std::condition_variable cv;
    bool settled = false;
    PubSubStatus status = PubSubStatus::kPending;
    SharedSchema schema;
    std::string message;

    void Settle(PubSubStatus outcome, SharedSchema value, std::string text) {
        {
            std::lock_guard<std::mutex> lock(mu);
            if (settled) return;
            status = outcome;
            schema = std::move(value);
            message = std::move(text);
            settled = true;
        }
        cv.notify_all();
    }
};

}  // namespace internal

SchemaArrival SchemaArrival::Ready(SharedSchema schema) {
    auto state = std::make_shared<internal::SchemaArrivalState>();
    // Null is legitimate here and ONLY here: it is the schema-less transport's
    // answer (§7 clause 1).
    state->status = PubSubStatus::kOk;
    state->schema = std::move(schema);
    state->settled = true;
    return SchemaArrival(std::move(state));
}

std::pair<SchemaArrival, SchemaResolver> SchemaArrival::Create() {
    auto state = std::make_shared<internal::SchemaArrivalState>();
    // Two named steps, deliberately. Written as
    // `return {SchemaArrival(state), SchemaResolver(std::move(state))}` MSVC
    // evaluates the initializer-clauses RIGHT TO LEFT, so the resolver moved the
    // state out before the arrival ever saw it — and the caller got an arrival
    // with no shared state, which answers kSubscriptionEnded on its very first
    // Wait. Silent, and it looked exactly like a subscription torn down early.
    SchemaArrival arrival(state);
    SchemaResolver resolver(std::move(state));
    return {std::move(arrival), std::move(resolver)};
}

PubSubStatus SchemaArrival::Wait(std::chrono::milliseconds timeout, SharedSchema* out) const {
    if (out == nullptr) {
        throw PubSubError(PubSubStatus::kInvalidArgument,
                          "SchemaArrival::Wait: the out parameter must not be null");
    }
    // Refused rather than treated as a poll, and rather than as "forever":
    // either reading could otherwise be invented independently by the two ABI
    // rounds (spec §5.2).
    if (timeout < std::chrono::milliseconds::zero()) {
        throw PubSubError(PubSubStatus::kInvalidArgument,
                          "SchemaArrival::Wait: a negative timeout is not a wait");
    }
    // No shared state: nothing can ever resolve it, which is exactly what
    // kSubscriptionEnded says.
    if (!state_) return PubSubStatus::kSubscriptionEnded;

    std::unique_lock<std::mutex> lock(state_->mu);
    if (!state_->settled) {
        if (timeout == std::chrono::milliseconds::zero()) {
            return PubSubStatus::kPending;
        }
        if (timeout == std::chrono::milliseconds::max()) {
            // The unbounded form. NOT wait_for(duration::max()): that overflows
            // the implementation's internal time_point on common standard
            // libraries and can return immediately.
            state_->cv.wait(lock, [this] { return state_->settled; });
        } else {
            state_->cv.wait_for(lock, timeout, [this] { return state_->settled; });
        }
        if (!state_->settled) return PubSubStatus::kPending;
    }

    // `*out` is written on kOk and ONLY on kOk — including kOk + null, which is
    // written null so a caller can tell "answered, no schemas here" from "not
    // answered" without inspecting anything else.
    if (state_->status == PubSubStatus::kOk) {
        *out = state_->schema;
    }
    return state_->status;
}

std::string SchemaArrival::Message() const {
    if (!state_) return {};
    std::lock_guard<std::mutex> lock(state_->mu);
    return state_->message;
}

SchemaResolver::~SchemaResolver() {
    // The third terminal outcome. Nothing resolved this arrival and nothing now
    // can, so say so — a waiter wakes with kSubscriptionEnded instead of
    // blocking forever, and it is distinguishable from kOk + null.
    if (state_) {
        state_->Settle(PubSubStatus::kSubscriptionEnded, nullptr,
                       "the subscription ended before a schema arrived");
    }
}

void SchemaResolver::Resolve(SharedSchema schema) && {
    std::shared_ptr<internal::SchemaArrivalState> state = std::move(state_);
    if (!state) {
        throw PubSubError(PubSubStatus::kInvalidArgument,
                          "SchemaResolver::Resolve: the token has already been consumed");
    }
    if (!schema) {
        // kOk + null is the schema-less transport's answer and nothing else. A
        // carrying provider that resolved with null would re-open the exact
        // conflation the typed outcome exists to close, so it is refused here —
        // SchemaArrival::Ready(nullptr) stays the only producer.
        state->Settle(PubSubStatus::kInternal, nullptr,
                      "SchemaResolver::Resolve was given a null schema");
        throw PubSubError(PubSubStatus::kInvalidArgument,
                          "SchemaResolver::Resolve: a null schema is not an arrival; kOk with a "
                          "null schema is reserved for transports that carry no schemas at all");
    }
    state->Settle(PubSubStatus::kOk, std::move(schema), {});
}

void SchemaResolver::Fail(PubSubStatus status, std::string message) && {
    std::shared_ptr<internal::SchemaArrivalState> state = std::move(state_);
    if (!state) {
        throw PubSubError(PubSubStatus::kInvalidArgument,
                          "SchemaResolver::Fail: the token has already been consumed");
    }
    // Reuses PubSubError's own refusal, so "which statuses are failures" is
    // decided in exactly one place.
    const PubSubStatus sanitized = PubSubError(status, {}).status();
    state->Settle(sanitized, nullptr, std::move(message));
}

}  // namespace fletcher
