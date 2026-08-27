// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// Test-only seam for the XRCE provider (issue #62 residual, HARD-4).
//
// This header is NOT installed and is compiled only when FLETCHER_BUILD_TESTS
// is defined. It declares a free hook whose signature uses only complete
// standard types; it must never name, dereference, store, or require the
// private pimpl `XrceDDSPubSubProvider::Impl`. The hook BODY lives in
// xrce_dds_pubsub_provider.cpp, where `Impl`, `TopicState`, and the real
// `Impl::OnTopic()` are complete, so the hook can build a real internal
// scenario and drive the real dispatch path without reimplementing it.
#pragma once

#ifndef FLETCHER_BUILD_TESTS
#error "xrce_test_hook.hpp is test-only; build with FLETCHER_BUILD_TESTS"
#endif

#include <cstddef>
#include <cstdint>

namespace fletcher::xrce::test {

// Observable POD result of the re-entrant-Unsubscribe scenario. `delivery_count`
// is the number of buffered envelopes the schema-flush path delivered before
// returning. Pre-fix the scenario throws std::bad_function_call (so the count is
// < 2 and never returned); post-fix it returns with delivery_count == 2.
struct ReentrantUnsubscribeResult {
    int delivery_count = 0;
};

// Builds a topic state with two buffered pending envelopes and a callback that
// re-enters Unsubscribe on its own topic (an in-place TopicState reset), then
// drives the real Impl::OnTopic() schema-flush path with a synthesized schema
// sample. See the body in xrce_dds_pubsub_provider.cpp.
ReentrantUnsubscribeResult RunReentrantUnsubscribeSchemaFlushScenario();

}  // namespace fletcher::xrce::test
