// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The copy-accounting oracle: decides, by ADDRESS PROVENANCE, whether the
// payload bytes a subscriber sees are the very bytes the publisher wrote — for
// rows and for attachments. Contract: docs/pubsub-interface-spec.md §8.1.
//
// The argument — what counts as a copy, why provenance and not counting, the
// premises P2 (synchronous delivery) and P5 (encode-window liveness) a new
// subject must satisfy, and what green does NOT prove — is written once, in
// README.md, "The `CopyAccounting` suite". Declarations here cite it.

#ifndef FLETCHER_CONFORMANCE_COPY_ACCOUNTING_HPP_
#define FLETCHER_CONFORMANCE_COPY_ACCOUNTING_HPP_

#include <cstddef>
#include <cstdint>
#include <fletcher/core/types.hpp>
#include <fletcher/core/write_buffer.hpp>
#include <fletcher/pubsub/provider.hpp>
#include <functional>
#include <memory>
#include <ostream>
#include <string>
#include <type_traits>
#include <vector>

#include "fletcher/conformance/subject.hpp"

namespace fletcher {
namespace conformance {

/// Addresses are sampled into integers WHILE THE STORAGE IS LIVE and compared
/// afterwards: using the pointer values themselves is implementation-defined
/// once the storage dies ([basic.stc.general]/4), and the verdict is read after
/// the round trip returns. 0 means "no address".
using Address = uintptr_t;

// ── The ledger ──────────────────────────────────────────────────────

/// One attachment's provenance: where its bytes were published and delivered.
/// `delivered_data == 0` means the delivery carried nothing under this key,
/// which Judge() scores as a copy.
struct AttachmentTrace {
    std::string key;
    Address published_data = 0;
    size_t published_len = 0;
    Address delivered_data = 0;
    size_t delivered_len = 0;
    /// memcmp against the published bytes, read BEFORE the verdict so
    /// "garbled" and "at a second address" are different failures.
    bool content_ok = false;
};

/// Everything one publish→delivery round trip observed. Written on the
/// publishing thread only (P2), so no lock.
struct CopyLedger {
    /// Encode side: the window base after the encoder's LAST append, and the
    /// position then.
    Address encode_base = 0;
    size_t encode_len = 0;
    /// Appends across which the base changed while `Position() > 0` — a refill
    /// that relocated already-written bytes — and how many bytes moved.
    /// Reported, never failed (2026-09-01 ruling).
    size_t refill_moves = 0;
    size_t refill_bytes = 0;

    /// Delivery side, captured inside the subscriber callback. `deliveries` is
    /// asserted `== 1` before any verdict is read: zero deliveries must never
    /// read as "no copies".
    size_t deliveries = 0;
    Address delivered_data = 0;
    size_t delivered_len = 0;
    bool row_content_ok = false;
    /// P5 enforced rather than merely documented: the encode window still held
    /// the row, byte for byte, when the callback ran. This catches a subject that
    /// clobbers or recycles the window before delivery, and relabels the failure
    /// as a P5 violation instead of a copy count. It does NOT catch a window
    /// freed and handed back at the same address with its bytes intact — see
    /// "Not airtight" in the harness README.
    bool window_intact = false;
    /// What the DELIVERY carried — the published-side count comes from the
    /// input map and so cannot catch a dropped entry.
    size_t delivered_attachments = 0;

    /// INPUT. When non-empty, the capture keeps its own copy of the attachment
    /// delivered under this key — §3.2 clause 1's "a callee that keeps it takes
    /// its own reference" — and the two fields below are read AFTER the callback
    /// has returned. Empty means the leg is not run.
    std::string retain_key;
    /// Where the retained bytes live once the delivery call is over, and whether
    /// they still read back byte for byte. A blob whose bytes die with the
    /// callback cannot satisfy both: either the owner is real, or it is not.
    Address retained_data = 0;
    bool retained_content_ok = false;

    std::vector<AttachmentTrace> attachments;
};

/// What Judge() decided. No subject-keyed expectation lives here, deliberately:
/// every registered subject faces the same numbers, so a provider cannot
/// declare its way to green.
struct CopyVerdict {
    size_t row_copies = 0;
    size_t attachment_copies = 0;
    size_t refill_moves = 0;
    size_t refill_bytes = 0;
};

/// The whole decision, as a pure function of the ledger — testable without a
/// provider, and exactly ONE scoring path for every subject and control.
/// `row_copies` is 0 iff the delivered span is EXACTLY the encode window; not
/// containment, which for a shorter range admits an identity-preserving in-place
/// `memmove` to the window base with `memcmp` passing by construction.
CopyVerdict Judge(const CopyLedger& ledger);

// ── Subjects ────────────────────────────────────────────────────────

/// One provider, exercised one way, ALWAYS in this process and synchronously
/// (P2). No peer verb and an unsynchronised ledger, so a cross-process or
/// off-thread subject cannot be built at all.
class CopyRunner {
   public:
    virtual ~CopyRunner() = default;

    virtual void Subscribe(const Topic& topic, PubSubProvider::SubscribeCallback cb) = 0;
    virtual void Publish(const Topic& topic, const PubSubProvider::RowEncoder& encoder,
                         const Attachments& attachments) = 0;
    virtual void Unsubscribe(const Topic& topic) = 0;
};

/// A gtest test PARAMETER, so it carries a label: printing a bare std::function
/// dumps uninitialised stack, and the label is what keeps ctest names stable.
struct CopySubject {
    std::string label;
    std::function<std::unique_ptr<CopyRunner>()> make;

    std::unique_ptr<CopyRunner> operator()() const { return make(); }
};

inline void PrintTo(const CopySubject& subject, std::ostream* os) { *os << subject.label; }

/// The subjects the forcing test runs over (README has the table). Compile-time,
/// so a subject that stops being registered shows up as a missing `ctest -N`
/// entry rather than as a silent skip.
const std::vector<CopySubject>& CopyAccountingSubjects();

/// The live negative control: the probe plus a deliberate staging copy of the
/// row and a deep copy of every blob, scored by the SAME Judge() through the
/// SAME capture. Deliberately NOT in the list above.
CopySubject StagingControlSubject();

/// The refill control: a probe whose encode window is a HARNESS-OWNED growable
/// buffer that relocates on every refill, so the counter's liveness is never
/// pinned to a provider's allocation strategy.
CopySubject GrowableControlSubject();

// ── Running one round trip ──────────────────────────────────────────

/// A deterministic pattern, so a garbled delivery is distinguishable from a
/// copied one.
std::vector<uint8_t> CopyPayload(size_t len);

/// Row sizes: 64 B is the ordinary case; 4 KiB forces a growable window to
/// relocate already-written bytes, which is how row identity most plausibly
/// breaks. `kAppendChunk` is pinned because a single `Append(payload, 4096)`
/// takes `VectorWriteBuffer`'s bulk path and relocates NOTHING.
inline constexpr size_t kSmallRowBytes = 64;
inline constexpr size_t kLargeRowBytes = 4096;
inline constexpr size_t kAppendChunk = 64;

/// Two attachments, not one, so `attachment_copies == 0` cannot be satisfied
/// vacuously by an empty map.
inline constexpr size_t kAttachmentBytes = 1024;
inline constexpr size_t kAttachmentCount = 2;

/// The outcome of one round trip: a ledger, or the reason there is none. A
/// provider that throws is reported by name, never as a silent `row_copies == 0`.
struct RoundTrip {
    CopyLedger ledger;
    /// Empty on success; "<type>: <what>" if the subject threw.
    std::string error;
};

/// Publish one row of `row_bytes` (in `kAppendChunk`-sized appends) plus
/// `attachments` on `topic`, and capture the ledger. The published attachment
/// addresses are taken from the caller's own Blobs, never re-derived.
RoundTrip RunRoundTrip(CopyRunner& runner, const Topic& topic, size_t row_bytes,
                       const Attachments& attachments);

/// Build `kAttachmentCount` attachments of `kAttachmentBytes` each.
Attachments MakeCopyAttachments();

/// Leg 3 — the copy §3.2 forces on a provider holding payload bytes in memory
/// IT owns (a stand-in for a transport-loaned sample).
///
/// The PROVIDER, not this harness, must produce the `Blob`: it injects the
/// loaned entry into the delivered `Attachments` inside `Publish`, the only
/// place the §3.2 limitation can bite. A caller-owned blob rides the same
/// publish and must cross untouched, so the pinned total of 1 is three-valued
/// and provider-dependent in both directions — 0 says the seam gained the
/// ability to carry borrowed memory, 2 says a provider copied bytes it was
/// handed by shared_ptr. `copying_provider` runs the identical leg against the
/// deep-copying probe, which is how the test proves, standing rather than by
/// inspection, that the number moves with provider behaviour.
RoundTrip RunBorrowedAttachmentRoundTrip(const Topic& topic, bool copying_provider = false);

/// The static half of leg 3, and the reason the runtime pin is not the whole
/// story.
///
/// The tripwire this REPLACES said: `Blob` points at a `vector`, and a `vector`
/// owns its bytes, so borrowed memory cost a copy no provider could escape — and
/// it named the residual it feared, "a PDA-DEC-3 that leaves `Blob` untouched and
/// adds a PARALLEL borrowed-blob type trips neither this nor the runtime pin".
///
/// PDA-DEC-3 did the opposite: `Blob` ITSELF became an owner plus a span, with no
/// conversion from the retired alias, so the change was source-breaking and no
/// parallel type exists. These assertions are the inverse of the old one — not a
/// relaxation of it, which its own text forbids. They fail the BUILD if anyone
/// puts the old shape back, or bolts a quiet conversion onto the new one.
static_assert(!std::is_same_v<Blob, std::shared_ptr<const std::vector<uint8_t>>>,
              "Blob is back to being a shared_ptr to a vector, so the seam can no longer carry "
              "memory it does not own and CopyAccounting.BorrowedAttachmentCostsNoCopies is "
              "measuring nothing. Do not relax this assertion to restore the build.");
static_assert(std::is_constructible_v<Blob, std::shared_ptr<const void>, const uint8_t*, size_t>,
              "Blob lost its owner-plus-span constructor — the one thing that lets a transport "
              "hand over its own bytes where they lie (spec 3.2).");
static_assert(!std::is_convertible_v<std::shared_ptr<const std::vector<uint8_t>>, Blob>,
              "Blob gained a conversion from the retired alias. That is the coexistence window "
              "this change exists not to have: every old call site would compile again and keep "
              "its copy, unnoticed.");

}  // namespace conformance
}  // namespace fletcher

#endif  // FLETCHER_CONFORMANCE_COPY_ACCOUNTING_HPP_
