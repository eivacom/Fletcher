// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The seam's failure vocabulary: ONE error type carrying ONE stable numbered
// cause (owner ruling 2026-09-01, spec §5.1).
//
// Why a number and not a type hierarchy: exceptions cannot cross a C boundary,
// and two independent language bindings that each map an assortment of standard
// exception types onto their own statuses will drift. A single number, fixed
// forever, is the thing both boundaries can carry unchanged.
#ifndef FLETCHER_INCLUDE_CORE_STATUS_HPP_
#define FLETCHER_INCLUDE_CORE_STATUS_HPP_

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

namespace fletcher {

/// The cause of a seam outcome.
///
/// **Fixed integers, APPENDED ONLY.** A value is never renumbered, never
/// reordered and never reused — a boundary that has shipped one of these
/// numbers to an application cannot take it back. The static_asserts below are
/// the machine check: a reorder fails the build rather than silently
/// re-labelling every error an already-deployed binding has ever seen.
///
/// C form (conceptual — see the ownership note on Blob in types.hpp): a signed
/// 32-bit integer with these values. Nothing about the *spelling* is normative;
/// the numbers are.
enum class PubSubStatus : int32_t {
    /// Success. Present because both C boundaries need a success value in the
    /// same enum (§5.2); PubSubError refuses it, so a boundary cannot translate
    /// a thrown failure into a success.
    kOk = 0,
    /// The caller passed something the seam refuses to interpret: an empty
    /// topic-segment list, a blob with bytes and no owner, a negative timeout.
    kInvalidArgument = 1,
    /// A topic was re-declared with a provably different schema (§7 clause 3).
    kSchemaConflict = 2,
    /// The topic has not been declared on this instance.
    kTopicNotDeclared = 3,
    /// The encoded sample does not fit the transport's payload bound.
    kPayloadTooLarge = 4,
    /// The transport refused or failed: an endpoint that would not be created,
    /// a write that did not go out, a session that is gone.
    kTransportFailure = 5,
    /// This provider does not implement the requested behaviour.
    kNotSupported = 6,
    /// The total catch-all. Anything with no better home arrives here carrying
    /// the original message — a taxonomy that lets std::bad_alloc through
    /// untyped is not a taxonomy.
    kInternal = 7,
    /// §2 OUTCOME, never thrown: the answer is not available yet, within the
    /// timeout that was asked for.
    kPending = 8,
    /// §2 OUTCOME, never thrown: the answer will never arrive, because the
    /// subscription that would have produced it is gone. Distinct from
    /// kOk + null, which means "this transport carries no schemas at all" — the
    /// two demand opposite handling at a subscriber (§7 clause 1).
    kSubscriptionEnded = 9,
};

// The numbering, pinned one value at a time. Not a single assert on the last
// value: that would let two values swap places and still pass.
static_assert(static_cast<int32_t>(PubSubStatus::kOk) == 0, "PubSubStatus values are frozen");
static_assert(static_cast<int32_t>(PubSubStatus::kInvalidArgument) == 1,
              "PubSubStatus values are frozen");
static_assert(static_cast<int32_t>(PubSubStatus::kSchemaConflict) == 2,
              "PubSubStatus values are frozen");
static_assert(static_cast<int32_t>(PubSubStatus::kTopicNotDeclared) == 3,
              "PubSubStatus values are frozen");
static_assert(static_cast<int32_t>(PubSubStatus::kPayloadTooLarge) == 4,
              "PubSubStatus values are frozen");
static_assert(static_cast<int32_t>(PubSubStatus::kTransportFailure) == 5,
              "PubSubStatus values are frozen");
static_assert(static_cast<int32_t>(PubSubStatus::kNotSupported) == 6,
              "PubSubStatus values are frozen");
static_assert(static_cast<int32_t>(PubSubStatus::kInternal) == 7, "PubSubStatus values are frozen");
static_assert(static_cast<int32_t>(PubSubStatus::kPending) == 8, "PubSubStatus values are frozen");
static_assert(static_cast<int32_t>(PubSubStatus::kSubscriptionEnded) == 9,
              "PubSubStatus values are frozen");

/// A stable, human-readable name for `status`. For messages and logs; the
/// NUMBER is the contract, this is not.
[[nodiscard]] inline const char* PubSubStatusName(PubSubStatus status) noexcept {
    switch (status) {
        case PubSubStatus::kOk:
            return "ok";
        case PubSubStatus::kInvalidArgument:
            return "invalid_argument";
        case PubSubStatus::kSchemaConflict:
            return "schema_conflict";
        case PubSubStatus::kTopicNotDeclared:
            return "topic_not_declared";
        case PubSubStatus::kPayloadTooLarge:
            return "payload_too_large";
        case PubSubStatus::kTransportFailure:
            return "transport_failure";
        case PubSubStatus::kNotSupported:
            return "not_supported";
        case PubSubStatus::kInternal:
            return "internal";
        case PubSubStatus::kPending:
            return "pending";
        case PubSubStatus::kSubscriptionEnded:
            return "subscription_ended";
    }
    return "internal";
}

/// The ONE exception type the seam throws.
///
/// Derives from std::runtime_error so every existing catch(const
/// std::exception&) site keeps working unchanged, and the message is what it
/// always was — the ruling moved branching on error *type* into the code, it did
/// not rewrite the diagnostics.
///
/// **Refuses kOk, kPending and kSubscriptionEnded** at construction. The first
/// would let a C boundary report a failed call as a success; the other two are
/// §2 outcomes of a wait, not failures, and the enum says so. A refused status
/// is coerced to kInternal rather than throwing from inside a throw expression,
/// where a second exception in flight would be worse than a mislabelled one.
class PubSubError : public std::runtime_error {
   public:
    PubSubError(PubSubStatus status, std::string what)
        : std::runtime_error(std::move(what)), status_(Sanitize(status)) {}

    [[nodiscard]] PubSubStatus status() const noexcept { return status_; }

   private:
    static PubSubStatus Sanitize(PubSubStatus status) noexcept {
        switch (status) {
            case PubSubStatus::kOk:
            case PubSubStatus::kPending:
            case PubSubStatus::kSubscriptionEnded:
                return PubSubStatus::kInternal;
            default:
                return status;
        }
    }

    PubSubStatus status_;
};

/// Run `fn` at a seam entry point, so the only exception that can leave is a
/// PubSubError.
///
/// Spec §5.1 asks each C boundary to translate; that is only possible if what
/// reaches the boundary is already typed. A std::bad_alloc, or a transport SDK's
/// own exception type, escaping a provider would arrive at a boundary as
/// something it has no number for — so it is caught here and becomes kInternal
/// carrying the original what().
template <typename Fn>
decltype(auto) TranslateSeamFailure(Fn&& fn) {
    try {
        return fn();
    } catch (const PubSubError&) {
        throw;  // already typed — do not re-wrap and lose the cause
    } catch (const std::overflow_error& e) {
        // The one std::-type mapping worth making by hand. At this seam an
        // overflow_error is always FixedWriteBuffer refusing a row that does not
        // fit the transport's payload bound (write_buffer.hpp is its only
        // thrower), and that cause has a number of its own — a caller can raise
        // the bound or split the row, which is nothing like kInternal.
        throw PubSubError(PubSubStatus::kPayloadTooLarge, e.what());
    } catch (const std::exception& e) {
        throw PubSubError(PubSubStatus::kInternal, e.what());
    } catch (...) {
        throw PubSubError(PubSubStatus::kInternal, "unknown exception at the pub/sub seam");
    }
}

}  // namespace fletcher

#endif  // FLETCHER_INCLUDE_CORE_STATUS_HPP_
