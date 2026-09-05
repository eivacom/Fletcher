// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// Subject registration: the in-process loopback, exercised BOTH ways — as the
// schema-LESS transport §7 clause 1's last sentence names it as, and as a
// schema-CARRYING one. One provider, two usages, the same clauses; the mode is
// fixed at construction, so "never mix the two" is a property of the object
// rather than of a code path a clause has to police.
//
// The schema-CARRYING half lives in its own binary (inprocess_carrying_main.cpp)
// and that file says why: clause 2's axis gate is the link line, and a second
// subject here would drag clause 2 onto this schema-less one.
//
// This binary links no clause-2 TU, because the loopback is kAbsent here, and it
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
