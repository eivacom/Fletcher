// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The copy-accounting oracle: decides, by ADDRESS PROVENANCE, whether the
// payload bytes a subscriber sees are the very bytes the publisher wrote — for
// rows and for attachments. Oracle: docs/pubsub-interface-spec.md §8, §8.1,
// §3.1, §3.2.
//
// What counts as a copy, stated before anything is measured:
//
//  - **Payload bytes** are the bytes a RowEncoder writes, and the bytes of each
//    attachment Blob.
//  - **A copy** is those bytes coming to exist at a second address, or moving to
//    a different address, BY THE PROVIDER (and by the thin Publisher/Subscriber
//    layer, where a subject routes through it), between the encoder's first
//    write and the subscriber callback's return.
//  - **Not a copy:** the encode itself; anything a transport does with the bytes
//    after they leave the seam (§8 scopes the property to the seam); and a
//    window refill inside a provider's growable buffer, which §3.1 clause 1
//    sanctions explicitly ("must not move ... except inside a refill, which must
//    preserve them verbatim") and the owner's 2026-09-01 ruling permits with its
//    cost PUBLISHED AS A NUMBER. Every other byte movement is a violation.
//
// Why provenance and not counting: allocation counting is blind to copies into a
// pooled buffer and, on Windows, to allocations inside a provider DLL carrying
// its own CRT — blind exactly where a loaded driver will live. Full argument in
// README.md and plans/PDA-DEC-2-copy-accounting-oracle.md.
//
// Premises this instrument rests on — a subject that breaks one must not be
// registered:
//
//  - **P2** every registered subject delivers SYNCHRONOUSLY on the publishing
//    thread, so the ledger needs no lock and has none.
//  - **P5 (liveness)** the bytes `encode_base` names remain allocated and
//    unfreed until the subscriber callback returns. That is what makes address
//    provenance immune to allocator reuse: a live allocation cannot be handed
//    out twice, so a same-address copy would require free-then-allocate — and a
//    freed encode window delivered to a callback is a use-after-free, a worse
//    bug than a copy. A subject that frees, recycles or pools the encode window
//    before delivery makes provenance unsound and MUST NOT be registered.
//    STOP-AND-ASK before registering one.

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
#include <vector>

#include "fletcher/conformance/subject.hpp"

namespace fletcher {
namespace conformance {

// ── The ledger ──────────────────────────────────────────────────────

/// One attachment's provenance: where its bytes were when published, and where
/// they were when delivered.
struct AttachmentTrace {
    std::string key;
    const uint8_t* published_data = nullptr;
    size_t published_len = 0;
    /// Null when the delivery carried no attachment under this key at all —
    /// which Judge() scores as a copy, because absent bytes are certainly not
    /// the same bytes.
    const uint8_t* delivered_data = nullptr;
    size_t delivered_len = 0;
    /// memcmp of the delivered bytes against the published ones. Read BEFORE
    /// the verdict, so "arrived garbled" and "arrived at a second address" are
    /// different failures.
    bool content_ok = false;
};

/// Everything one publish→delivery round trip observed. Written on the
/// publishing thread only (P2), so no lock.
struct CopyLedger {
    // -- encode side, sampled by the accounting encoder --
    /// Window base after the encoder's LAST append, and the position then.
    const uint8_t* encode_base = nullptr;
    size_t encode_len = 0;
    /// Appends across which the window base changed while `Position() > 0`,
    /// i.e. a refill that relocated already-written bytes, and how many bytes
    /// those relocations moved. Reported, never failed (2026-09-01 ruling).
    size_t refill_moves = 0;
    size_t refill_bytes = 0;

    // -- delivery side, captured inside the subscriber callback --
    /// Asserted `== 1` before any verdict is read: zero deliveries must never
    /// read as "no copies" (rung-2 item 8).
    size_t deliveries = 0;
    const uint8_t* delivered_data = nullptr;
    size_t delivered_len = 0;
    bool row_content_ok = false;

    std::vector<AttachmentTrace> attachments;
};

/// What Judge() decided. No subject-keyed expectation lives here, deliberately:
/// every registered subject faces the same numbers, so a provider cannot
/// declare its way to green (locked decision 11's forbidden pinned divergence
/// wearing a trait).
struct CopyVerdict {
    size_t row_copies = 0;
    size_t attachment_copies = 0;
    size_t refill_moves = 0;
    size_t refill_bytes = 0;
};

/// The whole decision, as a pure function of the ledger — so its arithmetic is
/// testable without a provider, and so there is exactly ONE scoring path for
/// every subject and for the negative control.
///
/// `row_copies` is 0 iff the delivered span is EXACTLY the encode window:
/// `delivered_data == encode_base && delivered_len == encode_len`. Not
/// containment. Containment degenerates to identity only when the lengths
/// match, and for a shorter delivered range it admits an identity-preserving
/// in-place `memmove` to the window base — every payload byte moved, the
/// delivered address unchanged, and `memcmp` passing by construction. That is
/// the one false-pass shape that needs no undefined behaviour. A subject that
/// legitimately delivers a sub-range of its encode window must say why in the
/// subject table before this rule is relaxed for it.
CopyVerdict Judge(const CopyLedger& ledger);

// ── Subjects ────────────────────────────────────────────────────────

/// One provider, exercised one way, ALWAYS in this process and always
/// synchronously (P2). There is no peer verb and the ledger is deliberately
/// unsynchronised, so a cross-process or off-thread subject cannot be built at
/// all — provenance is an address, and an address means nothing across an
/// address space.
class CopyRunner {
   public:
    virtual ~CopyRunner() = default;

    virtual void Subscribe(const Topic& topic, PubSubProvider::SubscribeCallback cb) = 0;
    virtual void Publish(const Topic& topic, const PubSubProvider::RowEncoder& encoder,
                         const Attachments& attachments) = 0;
    virtual void Unsubscribe(const Topic& topic) = 0;
};

/// A gtest test PARAMETER, so it carries a label: gtest prints the parameter
/// into the test's name, and printing a bare std::function falls back to a raw
/// byte dump of uninitialised stack. The label keeps ctest names stable.
struct CopySubject {
    std::string label;
    std::function<std::unique_ptr<CopyRunner>()> make;

    std::unique_ptr<CopyRunner> operator()() const { return make(); }
};

inline void PrintTo(const CopySubject& subject, std::ostream* os) { *os << subject.label; }

/// The subjects the forcing test runs over. Compile-time, so a subject that
/// stops being registered shows up as a missing entry in `ctest -N` rather than
/// as a silent skip (there is no GTEST_SKIP anywhere in this suite).
///
///  - `SeamProbe`           — a fixed-arena provider in this harness: positive
///                            control, proves the seam PERMITS zero-copy.
///  - `InProcessLoopback`   — the real `InProcessPubSubProvider`, called
///                            directly at the seam.
///  - `InProcessViaPubSub`  — the same provider reached through `Publisher` and
///                            `Subscriber`, so the layers ABOVE the seam are
///                            inside a measured path too. §8 words the
///                            attachment claim "publisher → provider →
///                            subscriber"; without this subject nothing above
///                            the seam is measured and a `std::vector`
///                            materialised in `Subscriber`'s fan-out "for
///                            safety" would be silent.
const std::vector<CopySubject>& CopyAccountingSubjects();

/// The live negative control: `SeamProbe`'s arena, plus a deliberate staging
/// copy of the row and a deep copy of every blob. Scored by the SAME Judge(),
/// through the SAME capture, and NOT in the list above — so rung-1 item 2
/// ("every registered subject faces the same numbers") stays true while the
/// control still exercises the whole instrument. It is the thing that fails if
/// the instrument is stubbed, always-zero, or deleted.
CopySubject StagingControlSubject();

// ── Running one round trip ──────────────────────────────────────────

/// The payload the oracle publishes: a deterministic pattern, so a garbled
/// delivery is distinguishable from a copied one.
std::vector<uint8_t> CopyPayload(size_t len);

/// Row sizes the forcing test exercises.
///
/// 64 B is the ordinary case. 4 KiB exists to force a GROWABLE provider buffer
/// to relocate its already-written bytes — which is the way row identity most
/// plausibly breaks — and that only happens under the append pattern below.
inline constexpr size_t kSmallRowBytes = 64;
inline constexpr size_t kLargeRowBytes = 4096;

/// Append granularity, pinned rather than left to chance.
///
/// `VectorWriteBuffer` has a `kChunk` of 256: a single `Append(payload, 4096)`
/// takes its bulk path (one `reserve`, one `insert` into an empty buffer),
/// relocates NOTHING, and reports `refill_bytes == 0` — leaving the 4 KiB leg
/// indistinguishable from the 64 B one and the refill counter unevidenced.
/// Sub-`kChunk` appends go through `Refill`→`Reserve` and reallocate at 512,
/// 1536, 3584 …, relocating 512/1536/… bytes. So the oracle appends in 64-byte
/// pieces and `refill_bytes > 0` is the EXPECTED observation on the 4 KiB leg
/// for any growable buffer.
inline constexpr size_t kAppendChunk = 64;

/// Attachment leg: two entries, 1 KiB each. Two, not one, so
/// `attachment_copies == 0` cannot be satisfied vacuously by an empty map, and
/// so the control has a number (2) that a one-attachment run could not produce.
inline constexpr size_t kAttachmentBytes = 1024;
inline constexpr size_t kAttachmentCount = 2;

/// The outcome of one round trip: a ledger, or the reason there is none.
///
/// A provider that throws during publish is reported by name and never as a
/// silent `row_copies == 0` (rung-2 item 9).
struct RoundTrip {
    CopyLedger ledger;
    /// Empty on success; "<type>: <what>" if the subject threw.
    std::string error;
};

/// Publish one row of `row_bytes` (in `kAppendChunk`-sized appends) plus
/// `attachments` on `topic`, and capture the ledger.
///
/// The published attachment bytes are recorded from the caller's own Blobs, so
/// leg 2 compares the delivered `data()` against the published `data()` and not
/// against a re-derivation.
RoundTrip RunRoundTrip(CopyRunner& runner, const Topic& topic, size_t row_bytes,
                       const Attachments& attachments);

/// Build `kAttachmentCount` attachments of `kAttachmentBytes` each.
Attachments MakeCopyAttachments();

/// Leg 3, borrowed receive memory: a `SeamProbe` that already holds payload
/// bytes in its own arena — a stand-in for a transport-loaned sample — and must
/// publish them as an `Attachments` entry.
///
/// `Blob = shared_ptr<const vector<uint8_t>>` cannot alias foreign memory
/// (§3.2), so the provider is FORCED to copy into a vector. This measures that
/// forced copy at exactly one. PDA-DEC-3 removes the limitation; when it does,
/// the test built on this goes red on purpose.
RoundTrip RunBorrowedAttachmentRoundTrip(const Topic& topic);

}  // namespace conformance
}  // namespace fletcher

#endif  // FLETCHER_CONFORMANCE_COPY_ACCOUNTING_HPP_
