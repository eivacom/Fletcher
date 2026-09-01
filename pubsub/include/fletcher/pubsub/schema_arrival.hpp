// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// Learning a topic's shape, in a form a C, C# or Rust caller can implement
// without inventing anything (spec §3.4).
//
// It replaces `std::shared_future<SharedSchema>`, which was the least
// C-expressible thing at the seam: two ABI rounds would each have invented their
// own bridge for it, and they would have invented different ones. There is now
// ONE waiting mechanism (owner ruling 2026-09-01) — the C++ caller uses the same
// one a binding does, so the binding path is exercised by the tree's own tests.
#ifndef FLETCHER_INCLUDE_PUBSUB_SCHEMA_ARRIVAL_HPP_
#define FLETCHER_INCLUDE_PUBSUB_SCHEMA_ARRIVAL_HPP_

#include <chrono>
#include <fletcher/core/status.hpp>
#include <memory>
#include <string>
#include <utility>

#include "fletcher/pubsub/owned_schema.hpp"

namespace fletcher {

namespace internal {
struct SchemaArrivalState;
}

class SchemaResolver;

/// A waitable handle for a topic's schema. Copyable; every copy observes the same
/// arrival, and distinct copies may be waited on concurrently from any thread.
/// (That is the thread-safety claim, precisely: it is NOT a licence to assign to
/// one SchemaArrival object from two threads, which races on the handle itself
/// like any other value.)
///
/// ── The outcome is TYPED, never a bare bool ─────────────────────────────────
/// The three facts below are genuinely different and demand different handling
/// at a subscriber, so they are three values rather than one null:
///
///   kOk + non-null       the schema arrived.
///   kOk + null           RESERVED: this transport carries no schemas at all —
///                        the caller brings its own (§7 clause 1). `*out` IS
///                        written, with null: a caller must not be left reading
///                        whatever it passed in. At a C boundary this is the one
///                        kOk on which `out->owner` is **null and must not be
///                        released** — releasing it is the obvious way to
///                        implement "on kOk, release the handle" and it is wrong
///                        here.
///   kPending             not yet, within `timeout`. `*out` is untouched.
///   kSubscriptionEnded   no schema will EVER arrive: the subscription that
///                        would have produced it is gone. `*out` is untouched.
///   anything else        the provider failed to produce one (e.g. a schema
///                        that would not deep-copy). `*out` is untouched.
///
/// Confusing the second with the fourth is the failure §7 exists to prevent: on
/// a schema-carrying transport they demand opposite handling, and the failure
/// mode for guessing wrong is silent wrong-slot decoding, not a crash. So
/// `SchemaArrival::Ready(nullptr)` is the ONLY producer of kOk + null;
/// `SchemaResolver::Resolve` refuses a null schema.
///
/// ── C form ──────────────────────────────────────────────────────────────────
/// `fl_status wait(arrival, int64_t timeout_ms, fl_schema* out)`, where
/// `fl_schema` is §3.2's owner-handle pair `{owner, const ArrowSchema*}` and
/// **not** a bare `ArrowSchema*`. A boundary releases the OWNER HANDLE; it must
/// **never** call the Arrow C Data Interface `release` on a shared schema, which
/// would destroy it under every other holder. On kOk the handle is a NEW
/// reference the caller releases, matching `Blob`'s retain/release idiom.
/// `timeout_ms < 0` is refused with kInvalidArgument; `INT64_MAX` — and any value
/// at or above a deliberately crude ~139-year threshold — is the unbounded form.
///
/// **How a refusal is delivered differs by side, and this is the one place the
/// two must not be read as the same thing.** In C++ a refused argument (negative
/// timeout, null `out`) **throws** `PubSubError`; it is not returned, even though
/// this function returns a `PubSubStatus`, because a returned status here would
/// be indistinguishable from an outcome of the wait. At a C boundary the same
/// refusal is **returned** as `fl_status`, because exceptions cannot cross — the
/// status VALUE is identical either way, only the delivery differs. A binding
/// that reads "refused with kInvalidArgument" as "returns kInvalidArgument" and
/// forgets the catch gets an exception across FFI.
///
/// The SEMANTICS above are pinned; the spelling is illustrative. Each ABI round
/// writes both sides of its own boundary and picks its own names and layout
/// (decision 2) — there is no shared C header and no layout compatibility.
class SchemaArrival {
   public:
    /// An arrival with no state at all. Reports kSubscriptionEnded: nothing will
    /// ever resolve it, and saying so is better than a wait that never ends.
    SchemaArrival() noexcept = default;

    /// Already resolved with `schema`. A null `schema` is the schema-less
    /// transport's answer (kOk + null) and nothing else.
    [[nodiscard]] static SchemaArrival Ready(SharedSchema schema);

    /// A pending arrival and the single-use token that ends it. Exactly one
    /// terminal outcome is possible: `Resolve`, `Fail`, or — if neither runs —
    /// the resolver's destructor, which reports kSubscriptionEnded. That third
    /// outcome is what makes an unbounded `Wait` safe.
    [[nodiscard]] static std::pair<SchemaArrival, SchemaResolver> Create();

    /// Block for at most `timeout` (zero polls, `milliseconds::max()` waits
    /// without a deadline) and report the outcome above.
    ///
    /// A negative `timeout` is refused with kInvalidArgument — it silently
    /// polled otherwise, and "negative means forever" must not be inventable by
    /// one boundary and not the other. `out` must not be null. Both refusals
    /// **throw** `PubSubError` rather than returning; see the class comment for
    /// why, and for what the C form does instead.
    ///
    /// A timeout at or above ~139 years is treated as unbounded, not as an instant
    /// poll. The threshold is crude on purpose and sits far below the point where
    /// `now + timeout` overflows: "a very large number" is how a caller
    /// that cannot name `milliseconds::max()` spells "forever", and answering it
    /// with an immediate kPending would be a silent lie.
    [[nodiscard]] PubSubStatus Wait(std::chrono::milliseconds timeout, SharedSchema* out) const;

    /// The message that came with a failure outcome; empty otherwise. For
    /// diagnostics — the STATUS is the contract.
    [[nodiscard]] std::string Message() const;

   private:
    friend class SchemaResolver;
    explicit SchemaArrival(std::shared_ptr<internal::SchemaArrivalState> state) noexcept
        : state_(std::move(state)) {}

    std::shared_ptr<internal::SchemaArrivalState> state_;
};

/// The write end of a SchemaArrival: move-only, single-use, and its destructor
/// is a terminal outcome in its own right.
///
/// Move-only with `&&`-qualified terminal calls means "resolved twice" and
/// "resolved never" are both unrepresentable: whichever call runs consumes the
/// token, and running neither still ends the arrival.
class SchemaResolver {
   public:
    SchemaResolver() noexcept = default;
    SchemaResolver(SchemaResolver&&) noexcept = default;
    SchemaResolver& operator=(SchemaResolver&&) noexcept = default;
    SchemaResolver(const SchemaResolver&) = delete;
    SchemaResolver& operator=(const SchemaResolver&) = delete;

    /// Unresolved at destruction means kSubscriptionEnded — the legitimate
    /// teardown-before-arrival path, reported rather than refused.
    ~SchemaResolver();

    /// The schema arrived. **Refuses null** with kInvalidArgument: kOk + null is
    /// reserved for a schema-less transport, and a carrying provider producing
    /// it would re-open exactly the conflation this type exists to close.
    ///
    /// The refusal is **terminal**, and a boundary implementing `resolve` must
    /// reproduce all three parts: it throws `PubSubError(kInvalidArgument)` at
    /// the caller, it CONSUMES the token, and it settles the arrival at
    /// `kInternal` with a diagnostic message. So the resolver cannot be retried
    /// with a valid schema, and a concurrent waiter is answered `kInternal`
    /// rather than `kInvalidArgument`. That asymmetry is deliberate: a null
    /// resolve is a bug in the provider, and leaving the arrival open would hang
    /// every waiter on it until teardown instead of reporting the bug.
    void Resolve(SharedSchema schema) &&;

    /// The provider could not produce a schema. `status` must be a failure —
    /// kOk, kPending and kSubscriptionEnded are refused (kSubscriptionEnded is
    /// the destructor's outcome, not a thing a provider announces) and become
    /// kInternal.
    void Fail(PubSubStatus status, std::string message) &&;

    /// False once a terminal call has consumed the token, or for a
    /// default-constructed resolver.
    [[nodiscard]] bool valid() const noexcept { return state_ != nullptr; }

   private:
    friend class SchemaArrival;
    explicit SchemaResolver(std::shared_ptr<internal::SchemaArrivalState> state) noexcept
        : state_(std::move(state)) {}

    std::shared_ptr<internal::SchemaArrivalState> state_;
};

}  // namespace fletcher

#endif  // FLETCHER_INCLUDE_PUBSUB_SCHEMA_ARRIVAL_HPP_
