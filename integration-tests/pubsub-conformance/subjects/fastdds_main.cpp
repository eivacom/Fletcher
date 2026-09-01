// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// Subject registration: Fast DDS, in-process AND across a process boundary.
// The cross-process one is the whole reason this harness exists — Fast DDS
// serves same-process endpoints over intra-process delivery, so the local
// subject cannot see the transport at all (spec §7.2).
//
// Fixed, distinct domains so the harness cannot collide with
// integration-tests/fastdds-xrce-interop (domain 145) or with itself.

#include <gtest/gtest.h>

#include <cstdint>
#include <fletcher/fastdds_pubsub_provider/fast_dds_pubsub_provider.hpp>
#include <memory>
#include <string>

#include "fletcher/conformance/suite.hpp"

namespace fletcher {
namespace conformance {
namespace {

constexpr uint32_t kLocalDomain = 151;
constexpr uint32_t kPeerDomain = 152;

std::shared_ptr<PubSubProvider> MakeFastDds(uint32_t domain_id) {
    FastDDSProviderOptions options;
    options.domain_id = domain_id;
    return std::make_shared<FastDDSPubSubProvider>(std::move(options));
}

}  // namespace

INSTANTIATE_TEST_SUITE_P(
    FastDdsLocal, ProviderConformance,
    ::testing::Values(MakeLocalSubjectFactory("FastDdsLocal", "fastdds", SchemaMode::kCarried,
                                              [] { return MakeFastDds(kLocalDomain); })));

INSTANTIATE_TEST_SUITE_P(FastDdsCrossProcess, ProviderConformance,
                         ::testing::Values(MakePeerSubjectFactory(
                             "FastDdsCrossProcess", "fastdds", SchemaMode::kCarried,
                             [] { return MakeFastDds(kPeerDomain); }, CONFORMANCE_FASTDDS_PEER,
                             {"--domain-id", std::to_string(kPeerDomain)})));

}  // namespace conformance
}  // namespace fletcher
