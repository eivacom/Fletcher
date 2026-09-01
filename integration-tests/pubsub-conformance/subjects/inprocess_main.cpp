// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// Subject registration: the in-process loopback, exercised as the schema-LESS
// transport §7 clause 1's last sentence names it as. A schema-CARRYING loopback
// subject joins this suite when PDA-DEC-3 lands (one INSTANTIATE line, one trait
// row, no new clause) — see the README.
//
// This binary links no clause-2 TU, because the loopback is kAbsent, and it
// links no provider but pubsub's own.

#include <gtest/gtest.h>

#include <fletcher/pubsub/in_process_provider.hpp>
#include <memory>

#include "fletcher/conformance/suite.hpp"

namespace fletcher {
namespace conformance {

INSTANTIATE_TEST_SUITE_P(InProcessLocal, ProviderConformance,
                         ::testing::Values(MakeLocalSubjectFactory(
                             "InProcessLocal", "inprocess", SchemaMode::kAbsent, [] {
                                 return std::shared_ptr<PubSubProvider>(
                                     std::make_shared<InProcessPubSubProvider>());
                             })));

}  // namespace conformance
}  // namespace fletcher
