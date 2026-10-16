// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The copy-accounting oracle's LEDGER and its one scoring function: decides, by
// ADDRESS PROVENANCE, whether the payload bytes a subscriber sees are the very
// bytes the publisher wrote - for rows and for attachments. Contract:
// docs/pubsub-interface-spec.md §8.1.
//
// Moved here from `integration-tests/pubsub-conformance` by D-BIND-62, verbatim
// in substance, so the C++ harness and the binding shim's probe variant score
// with ONE Judge(). The argument - what counts as a copy, why provenance and
// not counting, the premises P2 (synchronous delivery) and P5 (encode-window
// liveness) a new subject must satisfy, and what green does NOT prove - is
// written once, in pubsub-conformance's README.md, "The `CopyAccounting` suite".

#ifndef FLETCHER_COPY_PROBE_LEDGER_HPP_
#define FLETCHER_COPY_PROBE_LEDGER_HPP_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace fletcher {
namespace copy_probe {

/// Addresses are sampled into integers WHILE THE STORAGE IS LIVE and compared
/// afterwards: using the pointer values themselves is implementation-defined
/// once the storage dies ([basic.stc.general]/4), and the verdict is read after
/// the round trip returns. 0 means "no address".
using Address = uintptr_t;

/// Sample an address while its storage is still live (see `Address`).
inline Address At(const void* p) { return reinterpret_cast<Address>(p); }

/// One attachment's provenance: where its bytes were published and delivered.
/// `delivered_data == 0` means the delivery carried nothing under this key,
/// which Judge() scores as a copy.
///
/// NOTE on `content_ok`: it compares the delivered bytes against the PUBLISHED
/// ADDRESS, so when provenance holds it is comparing a buffer with itself and
/// says nothing. That is deliberate and sufficient here - its job is to catch a
/// COPY that garbled the bytes, which by definition sits at a second address -
/// but it is not a liveness check. The liveness claim is `retained_content_ok`,
/// which compares against harness-owned storage for exactly this reason.
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
    /// PRODUCER side - where the row's bytes were written BY THE CLIENT, sampled
    /// at production time. This is the half §8.1 used to begin after: the
    /// measured interval started at "the window base after the encoder's last
    /// append", so a client that composed its row elsewhere and handed it over
    /// was invisible. `produced_at == 0` means the sampler never ran and NO
    /// verdict may be read - an unsampled leg must fail as itself rather than
    /// default into `encode_copies == 1`.
    ///
    /// On a SOURCE-TRACED leg (D-BIND-61, `ScoreProducedFromSource`) there is no
    /// client-side sampler to run: the producer is a codec the harness cannot
    /// see into. There `produced_at` is where the window's payload bytes CAME
    /// FROM, and `produced_in_window` means they came straight from the caller's
    /// own buffer, with no intermediate.
    Address produced_at = 0;
    size_t produced_len = 0;
    /// Sampled INSIDE the producer: was `produced_at` exactly the subject
    /// buffer's own write cursor, `Data() + Position()`? Only the producer can
    /// answer that, and only while it is running.
    bool produced_in_window = false;

    /// Encode side: the window base after the encoder's LAST append, and the
    /// position then. Now an INTERIOR point of the measured interval, not its
    /// start - `row_copies` is preceded by `encode_copies`, not replaced by it.
    Address encode_base = 0;
    size_t encode_len = 0;
    /// Appends across which the base changed while `Position() > 0` - a refill
    /// that relocated already-written bytes - and how many bytes moved.
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
    /// freed and handed back at the same address with its bytes intact - see
    /// "Not airtight" in the harness README.
    bool window_intact = false;
    /// What the DELIVERY carried - the published-side count comes from the
    /// input map and so cannot catch a dropped entry.
    size_t delivered_attachments = 0;

    /// INPUT. When non-empty, the capture keeps its own copy of the attachment
    /// delivered under this key - §3.2 clause 1's "a callee that keeps it takes
    /// its own reference" - and the two fields below are read AFTER the callback
    /// has returned. Empty means the leg is not run.
    std::string retain_key;
    /// INPUT. What the retained bytes must still read back as, held by the
    /// HARNESS in its own storage.
    ///
    /// It has to be an independent copy, and that is not a detail: the retained
    /// blob's address IS the published address when provenance holds, so
    /// comparing the two would be `memcmp(p, p, n)` - true by construction, for a
    /// live owner and a dead one alike. Measured, not reasoned: a mutation that
    /// gave the blob an owner unrelated to the arena left this leg green until
    /// the comparand moved off the arena.
    std::vector<uint8_t> retain_expected;
    /// Where the retained bytes live once the delivery call is over, and whether
    /// they still read back byte for byte. A blob whose bytes die with the
    /// callback cannot satisfy both: either the owner is real, or it is not.
    ///
    /// Read only AFTER the subject has been destroyed (`subject_released`), so
    /// the `Blob`'s own owner is the only thing that can still be holding those
    /// bytes. Read while the provider was alive, `retained_content_ok` would
    /// pass for a span with no owner at all - the exact case it claims to
    /// distinguish, and a vacuous guard.
    Address retained_data = 0;
    bool retained_content_ok = false;
    /// The subject really was destroyed before the two fields above were read -
    /// a `weak_ptr` to it had expired. Asserted by the tests, so that a keep-alive
    /// added later cannot quietly make the ownership claim vacuous again.
    bool subject_released = false;

    std::vector<AttachmentTrace> attachments;
};

/// What Judge() decided. No subject-keyed expectation lives here, deliberately:
/// every registered subject faces the same numbers, so a provider cannot
/// declare its way to green.
struct CopyVerdict {
    /// The CLIENT's half of the send path: 0 iff the producer wrote the row
    /// straight into the delivered window, 1 if it composed elsewhere and the
    /// bytes were copied in. `row_copies` cannot see this - a staged row is
    /// copied into the window BEFORE the window base is sampled, so the
    /// provider half is a clean zero either way. That blindness is the defect
    /// PDA-DEC-A1 removes, and `StagingProducerIsCaught` is it, pinned.
    ///
    /// EMPTY on a leg whose producer was never sampled. It is an optional rather
    /// than a number precisely so that an unsampled leg cannot default into
    /// "the client copied the row": reading it there throws
    /// `std::bad_optional_access` and the leg fails as itself, which is the rule
    /// `CopyLedger::produced_at` states and nothing but convention used to
    /// enforce.
    std::optional<size_t> encode_copies;
    size_t row_copies = 0;
    size_t attachment_copies = 0;
    size_t refill_moves = 0;
    size_t refill_bytes = 0;
};

/// The whole decision, as a pure function of the ledger - testable without a
/// provider, and exactly ONE scoring path for every subject and control.
/// `row_copies` is 0 iff the delivered span is EXACTLY the encode window; not
/// containment, which for a shorter range admits an identity-preserving in-place
/// `memmove` to the window base with `memcmp` passing by construction.
/// `encode_copies` is 0 iff the producer wrote into that window itself, and is
/// EMPTY when no producer was sampled. The two are consecutive halves of one
/// path, not two views of the same half.
CopyVerdict Judge(const CopyLedger& ledger);

}  // namespace copy_probe
}  // namespace fletcher

#endif  // FLETCHER_COPY_PROBE_LEDGER_HPP_
