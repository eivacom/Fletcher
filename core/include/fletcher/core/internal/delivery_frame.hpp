// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// "Is THIS thread currently inside a delivery on THAT object?" — one thread-local
// stack of identity tokens, shared by every component that dispatches a
// subscriber callback or has to refuse a call issued from inside one.
//
// INTERNAL, not public surface — the `pubsub/…/internal/segments.hpp` precedent:
// an internal header a sibling package may include is still not public API. A
// caller never asks this question; only a dispatch site and a door do.
//
// **One stack, two kinds of token, no interference.** `Subscriber` pushes its own
// identity (owner ruling 2026-09-04 scoped the delivery-depth counter to
// `Subscriber::Impl`, and that scoping is what makes §7's published sentence true
// as written); `DeliveryChannel` pushes the identity of the PROVIDER INSTANCE that
// is dispatching. Both live in the same stack because tokens are compared by
// ADDRESS and NEVER dereferenced, so nothing here has to know — or agree — what a
// token points at. A provider token and a Subscriber token can never collide:
// they are the addresses of distinct live objects.
//
// P1 — ONE FRAME STACK PER BINARY. Every Fletcher component is a STATIC library
// (`pubsub/CMakeLists.txt`; `fletcher-core` is INTERFACE), so this `inline
// thread_local` has exactly one instance per LINKED MODULE, however many
// components include it — one per *process* only because the tree links them all
// into one. **STOP-AND-ASK** if any component becomes a shared library, or if
// PDA-ABI puts the dispatch adapter inside a driver DLL rather than host-side: a
// forked thread-local must not be papered over with a registration handshake.
//
// A language binding (BIND-C#/BIND-Rust) linking `core` — and the pubsub and
// provider components with it — statically into its own module is the FIRST
// CLAUSE's case, not a new one: that module gets its own stack, and the doors go
// on working, because every push and every door for one provider instance then
// executes inside that same module. What the clause refuses is a SPLIT — the
// components in different shared objects, so an instance's `Deliver` marks one
// stack while its `Unsubscribe` door asks another.
#ifndef FLETCHER_INCLUDE_CORE_INTERNAL_DELIVERY_FRAME_HPP_
#define FLETCHER_INCLUDE_CORE_INTERNAL_DELIVERY_FRAME_HPP_

#include <algorithm>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "fletcher/core/status.hpp"

namespace fletcher {
namespace internal {

/// One identity token per delivery frame this thread is inside, innermost last.
inline thread_local std::vector<const void*> g_delivery_stack;

/// Work owed by this thread's delivery frames, run when the LAST one ends.
///
/// Erased to `std::function<void()>` so this header stays free of the types the
/// work touches — A4's per-subscription gate and retirement map live in
/// `pubsub/src/subscriber.cpp` and have no business in `core`. The erasure costs
/// one allocation on a path taken only when a handler cancelled something, which
/// is neither hot nor common.
inline thread_local std::vector<std::function<void()>> g_deferred_at_depth_zero;

/// Is this thread inside a delivery frame identified by `token`?
[[nodiscard]] inline bool InsideDeliveryOn(const void* token) noexcept {
    return std::find(g_delivery_stack.begin(), g_delivery_stack.end(), token) !=
           g_delivery_stack.end();
}

/// Defer `work` until this thread leaves its OUTERMOST delivery frame.
inline void DeferUntilDeliveryDepthZero(std::function<void()> work) {
    g_deferred_at_depth_zero.push_back(std::move(work));
}

/// Marks one delivery frame. Push on construction, pop on destruction.
///
/// **The depth-0 sweep moved when this was lifted, and that is deliberate**
/// (review debt AG1-DEBT-10). A4 fix-cycle-3 runs its deferred drains when
/// `g_delivery_stack` becomes empty. Now that a provider token sits BELOW the
/// `Subscriber` identity in the same stack, the sweep fires when
/// `DeliveryChannel::Deliver`'s frame ends rather than when the fan-out's does.
/// Verified safe in both directions: firing later keeps a retirement PUBLISHED
/// for longer, which is the safe side of A4's promise, and no A4 gate is held at
/// that point. It is a real coupling with the code that needed four fix cycles,
/// so it is stated here rather than left to be rediscovered.
///
/// One consequence of the move worth naming: the sweep now fires while the
/// loopback holds its instance mutex across dispatch, as it did before this item
/// when it fired inside the `Subscriber`'s own frame. Same edge, same position;
/// the move did not add one.
///
/// **The sweep contains its own failures, because nothing outside it can.** A
/// destructor is implicitly `noexcept`, so wrapping the scope in a caller's
/// `try` protects nothing: a throw from the sweep terminates AT THE DESTRUCTOR
/// BOUNDARY, before any `catch` outside it is reachable. That was a real hazard
/// and not a theoretical one — the sweep runs A4's deferred work, which takes a
/// mutex (`std::system_error` on resource exhaustion) and releases from a map.
/// The `try` is therefore inside, and it is PER ITEM: one owed release failing
/// must not strand the rest, exactly as the fan-out will not let one bad
/// subscriber cancel delivery for those after it.
///
/// Nothing is reported from here. This header has no channel to report on, and
/// the work it runs is Fletcher's own bookkeeping rather than user code — the
/// failure a user could cause is absorbed and COUNTED one level up, at
/// `DeliveryChannel::Deliver` and at the `Subscriber` fan-out.
class DeliveryScope {
   public:
    explicit DeliveryScope(const void* token) { g_delivery_stack.push_back(token); }

    ~DeliveryScope() {
        g_delivery_stack.pop_back();
        if (!g_delivery_stack.empty() || g_deferred_at_depth_zero.empty()) return;
        // Swapped out first: a deferred action may itself enter a delivery, and
        // it must not see its own entry still owed.
        std::vector<std::function<void()>> due;
        due.swap(g_deferred_at_depth_zero);
        for (const std::function<void()>& work : due) {
            try {
                work();
            } catch (...) {
                // Per item, so one failed release does not strand the others.
            }
        }
    }

    DeliveryScope(const DeliveryScope&) = delete;
    DeliveryScope& operator=(const DeliveryScope&) = delete;
};

/// The door. Refuse `method` when this thread is already inside a delivery on
/// `token`, and do nothing otherwise.
///
/// **The refused set is EVERY seam method, on every provider** — the four
/// data-path methods `CreateTopic`, `Publish`, `Subscribe`, `Unsubscribe` and
/// the two schema-only ones, `SubscribeSchema` and `UnsubscribeSchema` — on the
/// same instance and the same thread (owner ruling 2026-09-05, "Re-entry is
/// refused on every protocol", which SUPERSEDES the earlier ruling that refused
/// only `Unsubscribe`).
///
/// The earlier ruling rested on the claim that the other three "work today on
/// Fast DDS and XRCE". Probing found that false: a Fast DDS listener callback
/// runs with the RTPS reader mutex held, and Declare+Publish and Subscribe each
/// HANG there independently. The capability was real on XRCE alone, one protocol
/// of three. One uniform rule, enforced loudly, beats two answers to one
/// question — and re-permitting is a registered obligation on PDA-ABI, where the
/// loaned-sample receive path makes deferral affordable.
///
/// **The message spells `kReentrantCall` in text** (review debt AG1-DEBT-11).
/// `~Subscriber` rethrows this status out of a `noexcept` destructor, and the
/// default terminate handler prints `what()` and NOT the numeric status — so
/// without the word in the string, ruling 2026-09-05's "a stated, NAMED error
/// instead of a silent one" would be served by a number nobody sees.
inline void RefuseIfInsideDeliveryOn(const void* token, const char* method) {
    if (!InsideDeliveryOn(token)) return;
    throw PubSubError(PubSubStatus::kReentrantCall,
                      std::string("kReentrantCall: ") + method +
                          " was called from inside a delivery callback on this same provider "
                          "instance, on this same thread, and cannot be served there. No "
                          "PubSubProvider method may be re-entered from a delivery callback: "
                          "copy what you need and act after the callback returns.");
}

}  // namespace internal
}  // namespace fletcher

#endif  // FLETCHER_INCLUDE_CORE_INTERNAL_DELIVERY_FRAME_HPP_
