// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The SERVER build's backend of `fl_subscriber_subscribe_batches` and its close (D9S-4, D9S-5a,
// D9S-6; d9 L12, L14, L15, L18).
//
// There is ONE batch assembler and it is domain 7's `SubscriberArrow` (ruling 2026-11-12). This
// file only hands over what it closes: it forwards the options as given, and each window it
// delivers reaches the binding's callback as one `fl_batch`. It keeps no window, counts no rows,
// owns no thread and reads no clock (BC-9; `ShimBatchesNothing.SourcesHoldNoThreadClockOrTimer`
// scans this directory for exactly that). Order and thread are domain 7's (d7 L12).
//
// Compiled into the shipped shim of a `variant=server` package only (c-abi/CMakeLists.txt); the
// edge build and the probe shim get `batches_edge.cpp` instead, and never see Arrow.
#include <arrow/c/bridge.h>
#include <arrow/record_batch.h>

#include <chrono>
#include <cstdlib>
#include <deque>
#include <fletcher/core/status.hpp>
#include <fletcher/pubsub_arrow/subscriber_arrow.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "containment.hpp"
#include "handles.hpp"

#ifndef FLETCHER_C_ABI_VARIANT
#error "FLETCHER_C_ABI_VARIANT is not defined: see c-abi/CMakeLists.txt"
#endif
// The build is asserted, never decided here: a backend compiled into the wrong artefact does not
// compile (BC-10's single site is `fl_binding_variant`, which the entry point asks first).
static_assert(FLETCHER_C_ABI_VARIANT == FL_VARIANT_SERVER,
              "batches_server.cpp belongs to the server build only");

// D-D9S-4: the hand-over's fault point. Set only on the test-only `fletcher-c-abi-handover-fault`
// shim (tests/CMakeLists.txt), whose `tests/handover_fault.cpp` defines it; never on a shipped one.
#ifdef FLETCHER_C_ABI_HANDOVER_FAULT
namespace fletcher::abi::internal {
void HandoverFaultPoint();
}
#define FLETCHER_HANDOVER_FAULT_POINT() ::fletcher::abi::internal::HandoverFaultPoint()
#else
#define FLETCHER_HANDOVER_FAULT_POINT() ((void)0)
#endif

namespace {

using fletcher::PubSubError;
using fletcher::PubSubStatus;
using fletcher::SubscriberArrow;

/// The subscriber's batch host: domain 7's batched subscribe over the subscriber's provider.
struct BatchHost {
    explicit BatchHost(std::shared_ptr<fletcher::PubSubProvider> provider)
        : arrow(std::move(provider)) {}
    SubscriberArrow arrow;
};

/// Releases, on every exit, whatever the callback did not move away (d9 L14, BC-13's one batch
/// site). The `release` pointer is the only state: a binding that moved a struct set it to NULL,
/// and anything still non-NULL is ours to release, exactly once.
struct ReleaseUnmoved {
    ArrowArray* array;
    ArrowSchema* schema;
    ~ReleaseUnmoved() {
        if (array->release != nullptr) array->release(array);
        if (schema->release != nullptr) schema->release(schema);
    }
};

/// domain 7's reason as `fl_batch.reason`. A `switch` over every enumerator with NO `default`,
/// and the source is compiled with the switch warning as an error (CMake): a fourth reason
/// (d7 Q4) fails the build here and can never cross as 0 (design debt DEBT-2). `noexcept`, so the
/// hand-over below can fix the reason before it tries anything (d9 L18): the tail is unreachable,
/// and reaching it anyway aborts rather than throw into domain 7 (d9 L1).
int32_t ReasonOf(SubscriberArrow::BatchStatus::Reason reason) noexcept {
    switch (reason) {
        case SubscriberArrow::BatchStatus::Reason::kRowLimit:
            return FL_BATCH_ROW_LIMIT;
        case SubscriberArrow::BatchStatus::Reason::kTimeout:
            return FL_BATCH_TIMEOUT;
        case SubscriberArrow::BatchStatus::Reason::kClosing:
            return FL_BATCH_CLOSING;
    }
    std::abort();
}

}  // namespace

namespace fletcher::abi::internal {

BatchedSubscription SubscribeBatches(fl_subscriber& subscriber, std::vector<std::string> segments,
                                     int64_t max_rows, int64_t timeout_ms,
                                     const fletcher::TopicOptions& options, fl_batch_fn on_batch,
                                     void* ctx) {
    // The host is created on the first batched subscribe, under the lock, and the lock covers
    // only that: never the `Subscribe` call below, never a callback (d9 L11).
    BatchHost* host = nullptr;
    {
        std::lock_guard<std::mutex> lock(subscriber.batches_mu);
        if (!subscriber.batches) {
            subscriber.batches = std::make_shared<BatchHost>(subscriber.provider);
        }
        host = static_cast<BatchHost*>(subscriber.batches.get());
    }

    // The thunk captures only `(on_batch, ctx)`, never the `fl_subscriber`: a destroy made inside
    // a callback leaves the remaining deliveries nothing to dangle on.
    auto thunk = [on_batch, ctx](std::shared_ptr<arrow::RecordBatch> batch,
                                 std::vector<fletcher::Attachments> attachments,
                                 SubscriberArrow::BatchStatus status) {
        // NEVER THROWS INTO DOMAIN 7 (D-D9S-4), and every window reaches `on_batch` exactly once
        // (d9 L18, BC-12). Nothing before the hand-over's `try` can throw: `noexcept` calls, empty
        // optionals and trivial aggregates only, so the reason and the window's rows are fixed
        // before anything is attempted.
        const int32_t reason = ReasonOf(status.reason);
        const int64_t offered = batch != nullptr ? batch->num_rows() : 0;
        std::optional<std::deque<fl_attachments>> sets;
        std::optional<std::vector<const fl_attachments*>> set_ptrs;
        ArrowArray array = {};
        ArrowSchema schema = {};
        const ReleaseUnmoved release_unmoved{&array, &schema};  // BC-13's one release, every exit
        fl_error failure = {};
        fl_batch out = {};
        try {
            // THE HAND-OVER: everything or nothing. One set per entry of domain 7's row-aligned
            // list, in its order (d7 L3), each taking its entry's blobs (no byte is copied). A
            // deque never moves its elements, and an `fl_attachments` can be neither copied nor
            // moved. No count or alignment check of the shim's own (d9 L15).
            sets.emplace();
            set_ptrs.emplace();
            for (fletcher::Attachments& entry : attachments) {
                sets->emplace_back(std::move(entry));
                set_ptrs->push_back(&sets->back());
            }
            out.reason = reason;
            out.rows_dropped = status.rows_dropped;
            out.attachments = set_ptrs->empty() ? nullptr : set_ptrs->data();
            if (batch != nullptr) {
                const arrow::Status exported = arrow::ExportRecordBatch(*batch, &array, &schema);
                if (!exported.ok()) {
                    throw PubSubError(PubSubStatus::kInternal,
                                      "fl_subscriber_subscribe_batches: " + exported.ToString());
                }
                out.num_rows = offered;
                out.array = &array;
                out.schema = &schema;
            }
            FLETCHER_HANDOVER_FAULT_POINT();  // empty outside the test shim
        } catch (...) {
            // d9 L18, BC-12: the one site. The window still arrives, as a FRESH no-batch: domain
            // 7's reason (CLOSING included, ruling 2026-11-14), its rows counted lost, and the
            // failure with it (d9 §1, ruling :504), borrowed for the call. Whatever the attempt
            // built is released after the call, like an unmoved batch. `Capture` is `noexcept`.
            fletcher::abi::Capture(&failure, FL_ORIGIN_HANDOVER);
            out = fl_batch{reason,  status.rows_dropped + offered, 0, nullptr, nullptr, nullptr,
                           &failure};
        }
        // The callback must not throw (binding.h); one that does anyway has already been handed
        // its window, so nothing is lost, and the throw never reaches domain 7 (d9 L18).
        try {
            on_batch(ctx, &out);
        } catch (...) {
        }
        fl_error_dispose(&failure);  // the shim's, after the call
    };

    auto result = host->arrow.Subscribe(
        segments, std::move(thunk),
        SubscriberArrow::BatchOptions{max_rows, std::chrono::milliseconds(timeout_ms)}, options);
    return {result.subscription_id, std::move(result.schema)};
}

void UnsubscribeBatches(fl_subscriber& subscriber, uint64_t id) {
    // The lock covers reading the host pointer only, never `Unsubscribe` or a callback (d9 L11).
    BatchHost* host = nullptr;
    {
        std::lock_guard<std::mutex> lock(subscriber.batches_mu);
        host = static_cast<BatchHost*>(subscriber.batches.get());
    }
    if (host == nullptr) return;  // nothing was ever subscribed for batches here
    // Domain 7 answers everything: when the closing batch comes, whether to wait (d7 L9), and
    // that an id no longer live is a no-op. The shim adds no wait, flag or check (d9 L15).
    host->arrow.Unsubscribe(id);
}

}  // namespace fletcher::abi::internal
