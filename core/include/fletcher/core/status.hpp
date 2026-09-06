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
    /// The caller re-entered the seam from inside a delivery callback, on the
    /// same provider instance and the same thread, through a door that cannot
    /// serve it there. ALL FOUR PubSubProvider methods refuse, on every provider
    /// (spec §6 clause 6). Distinct from kNotSupported, which says the provider cannot
    /// do this AT ALL rather than "not from in there" — two different operator
    /// problems (owner ruling 2026-09-03).
    kReentrantCall = 10,
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
static_assert(static_cast<int32_t>(PubSubStatus::kReentrantCall) == 10,
              "PubSubStatus values are frozen");

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
///
/// ── The message is part of the error's VALUE (spec §5.1, PDA-DEC-AG2) ────────
///
/// 1. A refusal's message is retrievable **from the error instance that produced
///    it** — never from a global or thread-local `errno`-style slot. There is no
///    such slot to add to: several provider instances live in one process (§4),
///    and a slot could not say which of them refused. The sibling driver spec
///    states the same rule from its own side, so the two boundaries derive one
///    rule rather than two.
/// 2. The message is **bytes plus length** and **contains no zero byte**. That
///    is true by construction, below.
/// 3. A boundary must convey **the number AND the message**. For a §4
///    configuration refusal the message *is* the answer: `kInvalidArgument` is
///    shared with many other refusals, so a boundary forwarding only the number
///    hands an operator a bare "invalid argument" for a mistyped provider name.
///
/// Rule 2 is enforced here because `what()` returns a `const char*`: a message
/// holding a zero byte is truncated there — before it ever reaches a boundary —
/// and everything after the zero is lost with no signal. `Escape` rewrites each
/// zero byte as the four characters `\x00` so the whole message survives.
///
/// **Deliberately non-injective, and not to be "fixed".** A message that already
/// held the literal characters `\x00` renders identically to one that held a
/// zero byte. `Quoted` in `provider_registry.cpp` escapes its backslashes to
/// avoid exactly this collision, and does not apply here: it also escapes every
/// byte >= 0x7f, which would mangle legitimate non-ASCII diagnostic text, and
/// widening this escape would change messages the owner's 2026-09-06 ruling did
/// not authorise changing. Only a zero byte truncates; only a zero byte is
/// escaped.
class PubSubError : public std::runtime_error {
   public:
    PubSubError(PubSubStatus status, std::string what)
        : std::runtime_error(Escape(std::move(what))), status_(Sanitize(status)) {}

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

    // Rule 2, made true rather than asserted. The common case allocates nothing
    // extra: a message with no zero byte is moved through untouched.
    static std::string Escape(std::string what) {
        if (what.find(static_cast<char>(0)) == std::string::npos) return what;
        std::string escaped;
        escaped.reserve(what.size() + 8);
        for (const char c : what) {
            if (c == static_cast<char>(0)) {
                escaped += "\\x00";
            } else {
                escaped += c;
            }
        }
        return escaped;
    }

    PubSubStatus status_;
};

/// Run `fn` at a seam entry point, so the only exception that can leave is a
/// PubSubError.
///
/// PROVIDER-AUTHORING API, not caller API. It is public because a provider is a
/// separate Conan package consuming this header, and spec 5.1's "every seam
/// entry point translates" is the obligation it implements - one implementation
/// of that rule rather than one per package.
///
/// Spec §5.1 asks each C boundary to translate; that is only possible if what
/// reaches the boundary is already typed. A std::bad_alloc, or a transport SDK's
/// own exception type, escaping a provider would arrive at a boundary as
/// something it has no number for — so it is caught here and becomes kInternal
/// carrying the original what().
template <typename Fn>
decltype(auto) TranslateSeamFailure(Fn&& fn) {
    try {
        return std::forward<Fn>(fn)();
    } catch (const PubSubError&) {
        throw;  // already typed — do not re-wrap and lose the cause
    } catch (const std::overflow_error& e) {
        // The one std::-type mapping worth making by hand. At this seam an
        // overflow_error is a row that no window here can hold, and that cause
        // has a number of its own — a caller can raise the bound or split the
        // row, which is nothing like kInternal. write_buffer.hpp is still its
        // only thrower, but PDA-DEC-A1 gave it three causes, not one:
        // FixedWriteBuffer refusing a row past the transport's payload bound
        // (§3.1 clause 4), AppendInPlace refusing a min_bytes no window could
        // ever satisfy, and AppendInPlace refusing a subclass whose refill
        // under-delivered. The last is a subclass defect rather than a bound the
        // caller can raise; it is reported under this number anyway, because a
        // caller cannot act on the difference and the message names it.
        throw PubSubError(PubSubStatus::kPayloadTooLarge, e.what());
    } catch (const std::exception& e) {
        throw PubSubError(PubSubStatus::kInternal, e.what());
    } catch (...) {
        throw PubSubError(PubSubStatus::kInternal, "unknown exception at the pub/sub seam");
    }
}

}  // namespace fletcher

#endif  // FLETCHER_INCLUDE_CORE_STATUS_HPP_
