// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// Clause 2 of the suite, and the only axis-gated one: it asserts a property of
// schema-CARRYING transports, and §7 clause 1 explicitly sanctions a transport
// that carries no schemas at all (the mirror — null throughout, never mixed —
// is clause 3, which runs everywhere).
//
// The gate is the LINK LINE: this TU is compiled into the carrying subjects'
// binaries only. So on a schema-less subject the clause is absent from the ctest
// list rather than present and skipped, and `GTEST_SKIP` appears nowhere in the
// suite. Absence is visible; a skip is not.

#include "fletcher/conformance/suite.hpp"

namespace fletcher {
namespace conformance {

// ── Clause 2 (§7 clause 1) ──────────────────────────────────────────
// "A callback is never invoked with a null schema." Includes the subscriber-first
// case, where the schema can only arrive after the subscription exists.
TEST_P(ProviderConformance, CallbackNeverSeesNullSchema) {
    ASSERT_TRUE(Carried()) << "clause 2 was linked into a schema-less subject's binary; "
                              "the gate is the link line, not a runtime check";
    const Topic topic = Fresh("never_null_schema");
    constexpr uint32_t kRows = 3;

    Collector collector;
    ScopedSubscription sub(Subject(), topic, collector.Callback());

    CONF_MUST_DECLARE(topic, DataSchema());
    for (uint32_t seq = 1; seq <= kRows; ++seq) {
        CONF_MUST_PUBLISH(topic, seq);
    }
    ASSERT_TRUE(collector.WaitForCount(kRows, Deadline()))
        << "only " << collector.Count() << " of " << kRows << " rows arrived";

    for (const Collector::Delivery& d : collector.Snapshot()) {
        EXPECT_TRUE(d.had_schema) << "row " << d.seq << " was delivered with a null schema";
    }
}

}  // namespace conformance
}  // namespace fletcher
