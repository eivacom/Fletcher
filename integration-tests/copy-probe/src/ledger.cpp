// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The one pure Judge(), moved here unchanged from pubsub-conformance's
// `copy_accounting.cpp` by D-BIND-62.

#include "fletcher/copy_probe/ledger.hpp"

namespace fletcher {
namespace copy_probe {

CopyVerdict Judge(const CopyLedger& ledger) {
    CopyVerdict verdict;

    // The CLIENT's half, which §8.1 used to begin after. A staged row is copied
    // into the window before the window base is sampled, so `row_copies` below
    // is a clean zero for it either way - this is the only field that can tell
    // the two producers apart.
    //
    // Left EMPTY when the producer sampler never ran, which is the rule the
    // ledger states: an unsampled leg must fail as itself rather than default
    // into "the client copied the row". Every pre-existing leg is unsampled, so
    // this is the difference between six legs carrying a manufactured verdict
    // and six legs carrying none.
    if (ledger.produced_at != 0) {
        verdict.encode_copies = ledger.produced_in_window ? 0u : 1u;
    }

    // Strict equality, not containment - see the header for the in-place
    // memmove that containment would score as zero.
    const bool row_is_the_encode_window = ledger.delivered_data != 0 &&
                                          ledger.delivered_data == ledger.encode_base &&
                                          ledger.delivered_len == ledger.encode_len;
    verdict.row_copies = row_is_the_encode_window ? 0 : 1;

    for (const AttachmentTrace& trace : ledger.attachments) {
        const bool same_bytes = trace.delivered_data != 0 &&
                                trace.delivered_data == trace.published_data &&
                                trace.delivered_len == trace.published_len;
        if (!same_bytes) ++verdict.attachment_copies;
    }

    verdict.refill_moves = ledger.refill_moves;
    verdict.refill_bytes = ledger.refill_bytes;
    return verdict;
}

}  // namespace copy_probe
}  // namespace fletcher
