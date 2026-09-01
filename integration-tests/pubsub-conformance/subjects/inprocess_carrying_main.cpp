// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The sixth subject: the SAME in-process loopback, constructed schema-CARRYING.
// Every delivery carries a non-null schema, and a subscription's schema arrival
// stays pending until a publisher declares the topic rather than answering
// kOk+null. The mode is fixed at construction, so §7 clause 1's "never mix the
// two" is a property of the object.
//
// Why its own binary rather than a second INSTANTIATE line beside InProcessLocal:
// clause 2's axis gate IS the link line. `INSTANTIATE_TEST_SUITE_P` registers
// EVERY `ProviderConformance` TEST_P in the binary against its subject, so two
// subjects of different schema modes in one binary would run clause 2 against the
// schema-less one — present and failing where the harness's design says it should
// be absent from the ctest list entirely. One binary per schema mode keeps
// "absent, not skipped" true.
//
// Retention is not this file's to choose: it comes from the provider table keyed
// "inprocess", so both loopback subjects necessarily agree that it drops
// pre-subscribe rows.

#include <gtest/gtest.h>

#include <fletcher/pubsub/in_process_provider.hpp>
#include <memory>

#include "fletcher/conformance/suite.hpp"

namespace fletcher {
namespace conformance {

INSTANTIATE_TEST_SUITE_P(InProcessCarrying, ProviderConformance,
                         ::testing::Values(MakeLocalSubjectFactory(
                             "InProcessCarrying", "inprocess", SchemaMode::kCarried, [] {
                                 return std::shared_ptr<PubSubProvider>(
                                     std::make_shared<InProcessPubSubProvider>(
                                         InProcessPubSubProvider::SchemaCarriage::kCarried));
                             })));

}  // namespace conformance
}  // namespace fletcher
