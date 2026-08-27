// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// HARD-6 forcing translation unit (negative-compile) for the CONCRETE provider.
//
// [[nodiscard]] is NOT inherited by an override, and the diagnostic keys off the
// STATIC type at the call site. Annotating only PubSubProvider::Subscribe (the
// base virtual) therefore fires nowhere an application actually calls: real code
// holds a concrete FastDDSPubSubProvider. This TU discards the return of the
// CONCRETE override so the annotation cannot be dropped again without a red test.
//
// Mechanics mirror pubsub-arrow/tests/discard_probe.cpp: EXCLUDE_FROM_ALL, built
// only by its CTest entry with C4834 promoted to an error (MSVC /we4834) or
// -Werror=unused-result (gcc/clang). With the annotation present the discard
// emits the diagnostic and the test PASSES; with it removed the TU compiles
// clean, no diagnostic appears, and the test FAILS (red-first polarity).
//
// NOTE: this file and its object-library target deliberately avoid the substring
// "nodiscard" — MSVC/cl echoes the source filename and the MSBuild target name
// into the output that PASS_REGULAR_EXPRESSION scans, so a "nodiscard" in a name
// would self-match and mask the red state. Only a genuine diagnostic may supply
// the match.
//
// This file must never be added to a normal build target.

#include <fletcher/fastdds_pubsub_provider/fast_dds_pubsub_provider.hpp>

void DiscardConcreteFastDDSProvider(fletcher::FastDDSPubSubProvider& provider) {
    provider.Subscribe({"probe"}, {});
}
