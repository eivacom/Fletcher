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

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fletcher/core/write_buffer.hpp>
#include <fletcher/fastdds_pubsub_provider/fast_dds_pubsub_provider.hpp>
#include <fletcher/pubsub/provider_registry.hpp>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "fletcher/conformance/suite.hpp"

namespace fletcher {
namespace conformance {
namespace {

constexpr uint32_t kLocalDomain = 151;
constexpr uint32_t kPeerDomain = 152;
constexpr uint32_t kRegistryDomain = 153;

std::shared_ptr<PubSubProvider> MakeFastDds(uint32_t domain_id) {
    return std::make_shared<FastDDSPubSubProvider>(ProviderConfig{0, domain_id, ""});
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

// ── Fast DDS resolves as a built-in NAME (spec §4 clause 4) ─────────
//
// This test lives HERE and not in `conformance_registry` on purpose. That
// binary's link line is deliberately narrow — "it names `fletcher-pubsub` and
// NO transport SDK, so no DDS or XRCE vocabulary resolves from here"
// (CMakeLists.txt) — and linking the Fast DDS SDK into it to register one
// provider would destroy exactly the guard the narrowness IS. This binary
// already links the provider, already holds a RESOURCE_LOCK and already has a
// generous timeout, so the test is free here and destructive there.
//
// What it asserts is the only claim `RegisterFastDDSProvider` makes: the name
// "fastdds" resolves through `ProviderRegistry::Create` — the SAME call a
// driver path will go through in PDA-ABI — and the thing that comes back is a
// working provider reached only through the base-typed handle. Nothing below
// names `FastDDSPubSubProvider`; register it under any other name and this goes
// red as an unknown selector.
TEST(Registry, FastDdsResolvesAsABuiltIn) {
    ProviderRegistry registry;
    RegisterFastDDSProvider(registry);

    // Empty document, unset bound: Fletcher's built-in profile everywhere, and
    // the bound resolves to 65536 — the number in the registered type name.
    ProviderConfig config;
    config.domain_id = kRegistryDomain;

    std::shared_ptr<PubSubProvider> provider =
        registry.Create(ProviderSelector::Parse("fastdds"), config);
    ASSERT_NE(provider, nullptr) << "\"fastdds\" did not resolve to a provider";

    const std::vector<std::string> topic{"registry", "fastdds-probe"};
    provider->CreateTopic(topic, MakeConformanceSchema(SchemaId::kA));

    std::vector<uint8_t> received;
    std::atomic<bool> delivered{false};
    SubscriptionResult result = provider->Subscribe(
        topic, [&](const uint8_t* data, size_t len, const SharedSchema&, const Attachments&) {
            received.assign(data, data + len);
            delivered.store(true);
        });

    // Fast DDS carries the schema on its own channel, so the wait is a real
    // wait rather than the loopback's immediate answer.
    SharedSchema schema;
    ASSERT_EQ(result.schema.Wait(std::chrono::seconds(20), &schema), PubSubStatus::kOk)
        << result.schema.Message();
    ASSERT_NE(schema, nullptr);

    provider->Publish(topic, [](WriteBuffer& buffer) {
        buffer.AppendByte(0x17);
        buffer.AppendByte('R');
        buffer.AppendByte('O');
        buffer.AppendByte('W');
    });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!delivered.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(delivered.load()) << "the row never reached the subscriber";
    // Written out as a literal rather than by running the encoder again: a
    // guard that compares a buffer with itself asserts nothing.
    EXPECT_EQ(received, (std::vector<uint8_t>{0x17, 'R', 'O', 'W'}))
        << "the delivered bytes are not what was published";
}

}  // namespace conformance
}  // namespace fletcher

// Own main rather than gtest_main: SIGPIPE's disposition has to be set before
// any thread exists, and this binary spawns a peer child whose death must reach
// a clause as a typed failure rather than as exit=141.
int main(int argc, char** argv) {
    fletcher::conformance::IgnoreSigPipeOnce();
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
