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

// Observable POD result of the re-entrant-Unsubscribe scenario.
//
// `delivery_count` is the number of buffered envelopes the schema-flush path
// delivered; it must be 2, from local copies, even though the first callback
// reset the live TopicState underneath the flush.
//
// `refusal_status` is the PubSubStatus the callback's own cancellation attempt
// was refused with, as a number: `kReentrantCall` (10) after PDA-DEC-AG1, and 0
// if nothing was thrown at all — which is what this provider used to do, since
// its recursive mutex let a re-entrant cancel straight through.
struct ReentrantUnsubscribeResult {
    int delivery_count = 0;
    int32_t refusal_status = 0;
};

// Builds a topic state with two buffered pending envelopes and a callback that
// re-enters Unsubscribe on its own topic (an in-place TopicState reset), then
// drives the real Impl::OnTopic() schema-flush path with a synthesized schema
// sample. See the body in xrce_dds_pubsub_provider.cpp.
ReentrantUnsubscribeResult RunReentrantUnsubscribeSchemaFlushScenario();

}  // namespace fletcher::xrce::test
