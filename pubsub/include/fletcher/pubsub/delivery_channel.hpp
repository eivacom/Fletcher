// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The sealed handle a provider reaches a subscriber's callback through.
//
// One answer, in one place, to spec §5.3's question — *what happens when a
// delivery callback throws?* — and one place where a delivery frame is marked, so
// the door check on `Unsubscribe` has something to ask about.
//
// **Why a type and not a rule.** Three providers had three answers: the loopback
// let the exception unwind into the PUBLISHER's `TranslateSeamFailure`, where
// `std::overflow_error` became `kPayloadTooLarge` — a subscriber's bug charged to
// an unrelated publisher, and on XRCE to a publisher's `CreateTopic`; Fast DDS
// rethrew through `OrderedDelivery` into a listener thread; XRCE would have
// unwound across the session pump's C frames, which is UB on MSVC and process
// termination in practice. Owner ruling 2026-09-05: **contain and report at the
// failure site; the publisher learns nothing.** `Deliver` is `noexcept`, so that
// is a property of the type rather than a comment a provider can forget.
//
// **Copyable, on purpose.** Three dispatch sites copy the callback into a local
// before invoking user code, because a callback that re-enters `Unsubscribe`
// would otherwise destroy the `std::function` currently executing — the HARD-4 /
// issue-#62 use-after-free (`in_process_provider.cpp`,
// `xrce_dds_pubsub_provider.cpp` twice). Those copies survive as copies of the
// CHANNEL. A move-only handle would have forced each site to invent something
// else.
//
// There is deliberately **no accessor for the wrapped callback**: a provider that
// could unwrap one could dispatch around the containment.
#ifndef FLETCHER_INCLUDE_PUBSUB_DELIVERY_CHANNEL_HPP_
#define FLETCHER_INCLUDE_PUBSUB_DELIVERY_CHANNEL_HPP_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

#include "fletcher/pubsub/provider.hpp"

namespace fletcher {

class DeliveryChannel {
   public:
    /// An empty channel. Delivers nothing; `bool(channel)` is false. This is what
    /// a topic slot with no live subscription holds. Its token is null, which is
    /// why the constructor below rejects a null one: a channel built with one
    /// would push a frame indistinguishable from this.
    DeliveryChannel() = default;

    /// The ordinary constructor: `provider` identifies the PROVIDER INSTANCE
    /// dispatching. It is compared by address and never dereferenced (see
    /// `core/…/internal/delivery_frame.hpp`); passing the provider's own `this`
    /// is what lets its doors recognise a re-entrant call.
    ///
    /// **The parameter is a `const PubSubProvider*` so that the identity cannot
    /// be got wrong.** Every door asks with a BASE pointer — the caller tier
    /// with `impl_->provider.get()` — while a provider naturally has a DERIVED
    /// `this`, and `static_cast<const void*>` of the two is only *incidentally*
    /// the same address: true for single, non-virtual, sole-base inheritance and
    /// unspecified in general. The implicit derived→base conversion at this call
    /// performs the adjustment, so a provider writing `DeliveryChannel(this, …)`
    /// is exact rather than lucky, and cannot opt out by forgetting a cast.
    DeliveryChannel(const PubSubProvider* provider, PubSubProvider::SubscribeCallback callback);

    /// Tag for the constructor below. Named, so that using it is a deliberate
    /// statement rather than an overload that happens to match.
    struct RawToken {
        explicit RawToken() = default;
    };

    /// The exception, for a dispatch site that has **no `PubSubProvider` object
    /// to name**: the XRCE test hook builds a channel over a bare `Impl` and its
    /// own re-entrancy check asks with that same `Impl*`, and the Fast DDS unit
    /// tests and benchmark build one over a file-local static because nothing in
    /// them asks about the token at all. Self-consistent in each case, and
    /// outside the base/derived question entirely.
    ///
    /// Not for providers. A provider that reaches for this is choosing an
    /// identity its own doors may not recognise.
    DeliveryChannel(RawToken, const void* provider_token,
                    PubSubProvider::SubscribeCallback callback);

    /// True when there is a callback to deliver to.
    explicit operator bool() const noexcept { return static_cast<bool>(callback_); }

    /// Invoke the callback inside a delivery frame for this provider instance,
    /// absorbing anything it throws.
    ///
    /// `noexcept` is load-bearing rather than decorative: the XRCE dispatch runs
    /// inside `uxr_run_session_time`'s C frames, where an unwind is undefined
    /// behaviour, and the Fast DDS dispatch runs on a listener thread that holds
    /// the RTPS reader mutex across the call.
    void Deliver(const uint8_t* data, size_t len, const SharedSchema& schema,
                 const Attachments& attachments) const noexcept;

    /// How many callback invocations THIS channel has absorbed an exception from.
    /// Shared across copies, so the count a dispatch site's local reports is the
    /// count the channel it was copied from reports.
    ///
    /// It exists because `pubsub/src` has no logging facility at all, so deleting
    /// the per-provider catch blocks would otherwise leave a swallowed exception
    /// with no observable at all. A number a test can read is stronger than a log
    /// line nothing asserts on.
    [[nodiscard]] uint64_t AbsorbedCount() const noexcept;

    /// Every absorption in this process, across every channel.
    ///
    /// The conformance clause that asserts "absorbed, not merely unobserved" sees
    /// only a `ProviderSubject` — it cannot reach the channel a provider built
    /// inside itself — so the per-channel count above is unobservable from there.
    /// This is: the clause brackets its publish and asserts the total rose by
    /// exactly one.
    [[nodiscard]] static uint64_t AbsorbedTotal() noexcept;

   private:
    PubSubProvider::SubscribeCallback callback_;
    const void* provider_token_ = nullptr;
    std::shared_ptr<std::atomic<uint64_t>> absorbed_;
};

}  // namespace fletcher

#endif  // FLETCHER_INCLUDE_PUBSUB_DELIVERY_CHANNEL_HPP_
