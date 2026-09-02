// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The `Registry` suite: uniform provider selection (spec §4, §4.1, §4.2).
//
// A FOURTH suite in this harness, for the same reason the copy oracle is a
// second one and the seam vocabulary a third: what it asserts — that a *string
// read at run time* decides which provider a caller gets, and that the caller
// never learns which — is not reachable from a provider-parameterised clause,
// because every clause is handed a subject that has already chosen.
//
// Its binary links `fletcher-pubsub` and NO transport SDK. That link line is the
// machine check for the design's forbidden case 5: no DDS or XRCE vocabulary can
// enter from here, whatever the registry's implementation does.
//
// Including provider_registry.hpp is also the machine check that `Create`'s
// whole signature is frozen — the static_assert lives in the header, so this
// binary cannot build against a widened one.
//
// Vacuity rule for this file, from the design's forcing-test table: no test may
// pass on "non-null" or on "it threw". Every test below asserts either a
// delivered row under the tag the selector maps to, or a specific PubSubStatus.

#include <gtest/gtest.h>

#include <cstdint>
#include <fletcher/core/status.hpp>
#include <fletcher/core/types.hpp>
#include <fletcher/core/write_buffer.hpp>
#include <fletcher/pubsub/provider.hpp>
#include <fletcher/pubsub/provider_registry.hpp>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace fletcher {
namespace conformance {
namespace {

// ── The journal, and the probes that write into it ──────────────────
//
// A probe records the rows published through it under ITS OWN tag. That tag is
// how a test tells which factory the registry actually reached — a non-null
// check cannot, and neither can comparing providers by address.

struct PublishedRow {
    std::string tag;
    std::vector<uint8_t> bytes;
};

struct Journal {
    std::vector<PublishedRow> rows;
    // What the stand-in resolver was handed. Empty until it is reached.
    std::string resolved_path;
    int resolver_calls = 0;
    // The configuration the factory saw, so §4.1 routing is observable.
    ProviderConfig last_config;
    int live_probes = 0;
};

class ProbeProvider : public PubSubProvider {
   public:
    ProbeProvider(std::string tag, std::shared_ptr<Journal> journal)
        : tag_(std::move(tag)), journal_(std::move(journal)) {
        ++journal_->live_probes;
    }
    ~ProbeProvider() override { --journal_->live_probes; }

    void CreateTopic(const std::vector<std::string>&, OwnedSchema) override {}

    void Publish(const std::vector<std::string>&, const RowEncoder& encoder,
                 const Attachments&) override {
        VectorWriteBuffer buffer;
        encoder(buffer);
        journal_->rows.push_back(PublishedRow{tag_, buffer.Finish()});
    }

    [[nodiscard]] SubscriptionResult Subscribe(const std::vector<std::string>&,
                                               SubscribeCallback) override {
        return SubscriptionResult{SchemaArrival::Ready(nullptr)};
    }

    void Unsubscribe(const std::vector<std::string>&) override {}

   private:
    std::string tag_;
    std::shared_ptr<Journal> journal_;
};

ProviderRegistry::Factory ProbeFactory(std::string tag, std::shared_ptr<Journal> journal) {
    return [tag = std::move(tag), journal](const ProviderConfig& config) {
        journal->last_config = config;
        return std::make_shared<ProbeProvider>(tag, journal);
    };
}

// ── The caller ──────────────────────────────────────────────────────
//
// THE point of the item, in three lines: everything a caller does with a
// provider setting. It names no concrete provider type, holds exactly one
// `Create`, and is byte-for-byte the same call for a built-in name and for a
// driver path — only the string differs. If PDA-ABI ever had to widen the
// registry to admit a path, THIS function would have to change, and that is the
// one failure this item cannot survive.
std::shared_ptr<PubSubProvider> MakeProvider(const ProviderRegistry& registry,
                                             const std::string& provider_setting,
                                             const ProviderConfig& config) {
    return registry.Create(ProviderSelector::Parse(provider_setting), config);
}

// A row with a caller-chosen marker byte. The expected bytes are written out as
// a literal at each assertion rather than produced by running this encoder a
// second time: a guard that compares a buffer with itself asserts nothing.
void PublishRow(PubSubProvider& provider, uint8_t marker) {
    provider.Publish({"registry", "probe"}, [marker](WriteBuffer& buffer) {
        buffer.AppendByte(marker);
        buffer.AppendByte('R');
        buffer.AppendByte('O');
        buffer.AppendByte('W');
    });
}

std::vector<uint8_t> Row(uint8_t marker) { return {marker, 'R', 'O', 'W'}; }

// The status of a call that must fail, or a gtest failure naming what happened
// instead. Returning the status rather than wrapping the assertion keeps each
// test's expectation on ITS OWN line, so a refusal with the wrong cause reads as
// a value mismatch rather than as "something threw".
template <typename Fn>
PubSubStatus RefusalOf(Fn&& fn, const char* what) {
    try {
        std::forward<Fn>(fn)();
    } catch (const PubSubError& error) {
        return error.status();
    } catch (const std::exception& error) {
        ADD_FAILURE() << what << " threw something that is not a PubSubError: " << error.what();
        return PubSubStatus::kOk;
    }
    ADD_FAILURE() << what << " did not fail at all";
    return PubSubStatus::kOk;
}

template <typename Fn>
std::string MessageOf(Fn&& fn) {
    try {
        std::forward<Fn>(fn)();
    } catch (const std::exception& error) {
        return error.what();
    }
    return {};
}

bool Mentions(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

// ── THE FORCING TEST ────────────────────────────────────────────────
//
// A name read at run time picks one of two providers, and the caller never
// learns which it got. Both directions are asserted — `alpha` reaches alpha AND
// `beta` reaches beta — because a test that only checks one direction stays
// green when `Create` ignores the selector and returns the first factory it has
// (mutation M2). Nothing here is greenable by a non-null check: the claim is
// that a row published through the returned provider surfaces under the tag the
// NAME maps to.
TEST(Registry, SelectsByNameWithoutCallerKnowingTheProvider) {
    auto journal = std::make_shared<Journal>();

    ProviderRegistry registry;
    registry.Register("alpha", ProbeFactory("alpha", journal));
    registry.Register("beta", ProbeFactory("beta", journal));

    // The two selections differ ONLY in the string an operator wrote.
    const ProviderConfig config;
    std::shared_ptr<PubSubProvider> first = MakeProvider(registry, "alpha", config);
    ASSERT_NE(first, nullptr) << "the registry produced nothing for a registered name";
    PublishRow(*first, 0xA1);

    std::shared_ptr<PubSubProvider> second = MakeProvider(registry, "beta", config);
    ASSERT_NE(second, nullptr) << "the registry produced nothing for a registered name";
    PublishRow(*second, 0xB2);

    ASSERT_EQ(journal->rows.size(), static_cast<size_t>(2))
        << "the rows never reached a probe, so nothing below is evidence of anything";

    EXPECT_EQ(journal->rows[0].tag, "alpha")
        << "the setting said `alpha` and the row was delivered by the provider tagged "
        << journal->rows[0].tag << ": the name did not decide which provider the caller got";
    EXPECT_EQ(journal->rows[0].bytes, Row(0xA1))
        << "the alpha probe recorded bytes the caller never published";

    EXPECT_EQ(journal->rows[1].tag, "beta")
        << "the setting said `beta` and the row was delivered by the provider tagged "
        << journal->rows[1].tag << ": one name is answered for every selector";
    EXPECT_EQ(journal->rows[1].bytes, Row(0xB2))
        << "the beta probe recorded bytes the caller never published";
}

// ── The round's central promise, made executable ────────────────────
//
// "PDA-ABI adds a resolver, not a second API." A stand-in resolver stands where
// a real loader will stand, and a path selector reaches it through the IDENTICAL
// `MakeProvider` call, with the identical config — only the string differs. If
// this claim were false, it would be false here: the helper would need a second
// entry point, an overload or a flag.
TEST(Registry, PathSelectorResolvesThroughTheSameCall) {
    auto journal = std::make_shared<Journal>();

    ProviderRegistry registry;
    registry.Register("alpha", ProbeFactory("alpha", journal));
    registry.SetPathResolver([journal](const std::string& path, const ProviderConfig& config) {
        journal->resolved_path = path;
        journal->last_config = config;
        ++journal->resolver_calls;
        return std::make_shared<ProbeProvider>("loaded", journal);
    });

    const ProviderConfig config;
    const std::string kPath = "/opt/fletcher/libstandin_driver.so";

    std::shared_ptr<PubSubProvider> built_in = MakeProvider(registry, "alpha", config);
    ASSERT_NE(built_in, nullptr);
    PublishRow(*built_in, 0xA1);

    std::shared_ptr<PubSubProvider> loaded = MakeProvider(registry, kPath, config);
    ASSERT_NE(loaded, nullptr) << "the path selector produced nothing";
    PublishRow(*loaded, 0xC3);

    EXPECT_EQ(journal->resolver_calls, 1)
        << "the resolver seat was reached " << journal->resolver_calls
        << " times for one path selector";
    EXPECT_EQ(journal->resolved_path, kPath)
        << "the resolver was handed `" << journal->resolved_path
        << "`, not the path the caller configured — a loader would open the wrong library";

    ASSERT_EQ(journal->rows.size(), static_cast<size_t>(2))
        << "one of the two selections delivered nothing";
    EXPECT_EQ(journal->rows[0].tag, "alpha") << "the name half of the same call regressed";
    EXPECT_EQ(journal->rows[0].bytes, Row(0xA1));
    EXPECT_EQ(journal->rows[1].tag, "loaded")
        << "a path selector was answered by the provider tagged " << journal->rows[1].tag
        << " rather than by the resolver's product";
    EXPECT_EQ(journal->rows[1].bytes, Row(0xC3));
}

// ── The live negative control for the test above ────────────────────
//
// It fails in exactly the state that would let the previous test pass for the
// wrong reason: the path branch wired to something, or defaulting to a provider
// instead of refusing. And it asserts the STATUS, not that something threw —
// `kNotSupported` ("this build cannot load drivers") is a different operator
// action from an unknown name's `kInvalidArgument` ("no such protocol here"),
// and the 2026-09-02 ruling turns on that distinction.
TEST(Registry, PathSelectorWithoutResolverIsRefusedAsUnsupported) {
    auto journal = std::make_shared<Journal>();

    ProviderRegistry registry;
    registry.Register("alpha", ProbeFactory("alpha", journal));

    const ProviderConfig config;
    EXPECT_EQ(RefusalOf([&] { return MakeProvider(registry, "/opt/fletcher/libzenoh.so", config); },
                        "a path selector with no resolver installed"),
              PubSubStatus::kNotSupported);

    EXPECT_EQ(journal->live_probes, 0)
        << "the refused path selector built a provider anyway, so the seat is wired to something";

    // Distinctness is the ruling's whole point, so it is asserted, not implied.
    EXPECT_EQ(RefusalOf([&] { return MakeProvider(registry, "gamma", config); },
                        "an unknown built-in name"),
              PubSubStatus::kInvalidArgument);

    // DEBT-8: trailing whitespace or a CRLF out of a config file is the
    // realistic misclassification, and "this build cannot load drivers" alone
    // reads as an infrastructure fault rather than as a typo.
    const std::string message =
        MessageOf([&] { return MakeProvider(registry, "fastdds\r\n", config); });
    EXPECT_TRUE(Mentions(message, "path"))
        << "the refusal does not say the selector was classified as a path: " << message;
    EXPECT_TRUE(Mentions(message, "offset 7"))
        << "the refusal does not locate the character that made it a path: " << message;
}

// ── The rest of the door ────────────────────────────────────────────

TEST(Registry, UnknownNameIsRefusedWithTheAvailableNames) {
    auto journal = std::make_shared<Journal>();
    ProviderRegistry registry;
    registry.Register("alpha", ProbeFactory("alpha", journal));
    registry.Register("beta", ProbeFactory("beta", journal));

    const ProviderConfig config;
    EXPECT_EQ(RefusalOf([&] { return MakeProvider(registry, "gamma", config); }, "an unknown name"),
              PubSubStatus::kInvalidArgument);

    const std::string message = MessageOf([&] { return MakeProvider(registry, "gamma", config); });
    EXPECT_TRUE(Mentions(message, "alpha") && Mentions(message, "beta"))
        << "the refusal does not tell the operator what IS available: " << message;
}

TEST(Registry, DuplicateRegistrationIsRefused) {
    auto journal = std::make_shared<Journal>();
    ProviderRegistry registry;
    registry.Register("alpha", ProbeFactory("alpha", journal));

    EXPECT_EQ(RefusalOf([&] { registry.Register("alpha", ProbeFactory("impostor", journal)); },
                        "registering one name twice"),
              PubSubStatus::kInvalidArgument);

    // No last-wins: the first registration is still what the name means.
    std::shared_ptr<PubSubProvider> provider = MakeProvider(registry, "alpha", ProviderConfig{});
    ASSERT_NE(provider, nullptr);
    PublishRow(*provider, 0xA1);
    ASSERT_EQ(journal->rows.size(), static_cast<size_t>(1));
    EXPECT_EQ(journal->rows[0].tag, "alpha")
        << "a refused duplicate silently replaced what `alpha` means";
}

TEST(Registry, EachCreateReturnsAnIndependentInstance) {
    auto journal = std::make_shared<Journal>();
    ProviderRegistry registry;
    registry.Register("alpha", ProbeFactory("alpha", journal));

    const ProviderConfig config;
    std::shared_ptr<PubSubProvider> first = MakeProvider(registry, "alpha", config);
    std::shared_ptr<PubSubProvider> second = MakeProvider(registry, "alpha", config);

    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_NE(first.get(), second.get())
        << "two selections share one transport: the registry memoized";
    EXPECT_EQ(journal->live_probes, 2) << "the second selection did not construct anything";
}

TEST(Registry, ProvidersOutliveTheRegistryThatMadeThem) {
    auto journal = std::make_shared<Journal>();
    std::shared_ptr<PubSubProvider> provider;
    {
        ProviderRegistry registry;
        registry.Register("alpha", ProbeFactory("alpha", journal));
        provider = MakeProvider(registry, "alpha", ProviderConfig{});
    }
    ASSERT_NE(provider, nullptr);
    PublishRow(*provider, 0xA1);
    ASSERT_EQ(journal->rows.size(), static_cast<size_t>(1))
        << "the provider stopped working when the registry that made it was destroyed";
    EXPECT_EQ(journal->rows[0].tag, "alpha");
}

TEST(Registry, SelectorShapeDecidesAndIsRefusedWhenItCannotMeanAnything) {
    ProviderRegistry registry;
    const ProviderConfig config;

    EXPECT_EQ(RefusalOf([&] { return ProviderSelector::Parse(""); }, "an empty selector"),
              PubSubStatus::kInvalidArgument);

    // DEBT-5: a length-carrying binding can hand the seam an embedded NUL. It
    // classifies as a path and would reach a future `dlopen(path.c_str())`
    // TRUNCATED — loading a different library with no signal. Refused once, here,
    // rather than by every resolver remembering to check.
    std::string with_nul("fastdds");
    with_nul.push_back('\0');
    with_nul += "/../evil.so";
    EXPECT_EQ(RefusalOf([&] { return ProviderSelector::Parse(with_nul); },
                        "a selector with an embedded NUL"),
              PubSubStatus::kInvalidArgument);

    // The rule never consults the registry, so a string means the same thing in
    // a build where nothing is registered as in one where everything is.
    EXPECT_EQ(RefusalOf([&] { return MakeProvider(registry, "fastdds", config); },
                        "a name in an empty registry"),
              PubSubStatus::kInvalidArgument);
    EXPECT_EQ(RefusalOf([&] { return MakeProvider(registry, "./fastdds", config); },
                        "a path in an empty registry"),
              PubSubStatus::kNotSupported);
}

TEST(Registry, RegistrationAndSelectionShareOneVocabulary) {
    auto journal = std::make_shared<Journal>();
    ProviderRegistry registry;

    // A name `Parse` would call a path cannot be registered, so a registered
    // name is always selectable and the two vocabularies cannot drift.
    EXPECT_EQ(RefusalOf([&] { registry.Register("fastdds.v2", ProbeFactory("x", journal)); },
                        "registering a name Parse would classify as a path"),
              PubSubStatus::kInvalidArgument);
    EXPECT_EQ(RefusalOf([&] { registry.Register("", ProbeFactory("x", journal)); },
                        "registering an empty name"),
              PubSubStatus::kInvalidArgument);
    EXPECT_EQ(RefusalOf([&] { registry.Register("alpha", ProviderRegistry::Factory{}); },
                        "registering an empty factory"),
              PubSubStatus::kInvalidArgument);
    EXPECT_EQ(RefusalOf([&] { registry.SetPathResolver(ProviderRegistry::PathResolver{}); },
                        "installing an empty resolver"),
              PubSubStatus::kInvalidArgument);

    // Every spelling of a shared library on every target carries a dot or a
    // separator, so it is a path; a plain word is a name.
    registry.Register("in-process", ProbeFactory("in-process", journal));
    registry.Register("xrce_dds", ProbeFactory("xrce_dds", journal));
    std::shared_ptr<PubSubProvider> provider =
        MakeProvider(registry, "in-process", ProviderConfig{});
    ASSERT_NE(provider, nullptr);
    PublishRow(*provider, 0xD4);
    ASSERT_EQ(journal->rows.size(), static_cast<size_t>(1));
    EXPECT_EQ(journal->rows[0].tag, "in-process");
}

// DEBT-4: swapping which loader EVERY path means is the same hazard as swapping
// what one name means, and it is reachable — a host and a bootstrap library both
// installing one, or a real loader overwriting a test double.
TEST(Registry, SecondPathResolverIsRefusedAndTheFirstStillStands) {
    auto journal = std::make_shared<Journal>();
    ProviderRegistry registry;
    registry.SetPathResolver([journal](const std::string& path, const ProviderConfig&) {
        journal->resolved_path = path;
        return std::make_shared<ProbeProvider>("first", journal);
    });

    EXPECT_EQ(
        RefusalOf(
            [&] {
                registry.SetPathResolver([journal](const std::string&, const ProviderConfig&) {
                    return std::make_shared<ProbeProvider>("second", journal);
                });
            },
            "installing a second path resolver"),
        PubSubStatus::kInvalidArgument);

    std::shared_ptr<PubSubProvider> provider =
        MakeProvider(registry, "./driver.so", ProviderConfig{});
    ASSERT_NE(provider, nullptr);
    PublishRow(*provider, 0xE5);
    ASSERT_EQ(journal->rows.size(), static_cast<size_t>(1));
    EXPECT_EQ(journal->rows[0].tag, "first")
        << "a refused second resolver silently replaced the first";
}

TEST(Registry, ConfigurationReachesTheProviderAndIsNeverRead) {
    auto journal = std::make_shared<Journal>();
    ProviderRegistry registry;
    registry.Register("alpha", ProbeFactory("alpha", journal));

    ProviderConfig config;
    config.max_payload_bytes = 65000;
    config.domain_id = 151;
    // Bytes no parser in this tree could accept, INCLUDING an embedded NUL: the
    // seam transports the document, it does not read or validate it (§4.2,
    // decision 8). A Fletcher that had grown a parser would reject this.
    config.document = "<qos>\xff\xfe not utf-8, not xml, not json {[</qos>";
    config.document.push_back('\0');
    config.document += "tail";
    const size_t document_bytes = config.document.size();

    std::shared_ptr<PubSubProvider> provider = MakeProvider(registry, "alpha", config);
    ASSERT_NE(provider, nullptr);
    EXPECT_EQ(journal->last_config.max_payload_bytes, 65000u);
    EXPECT_EQ(journal->last_config.domain_id, 151u);
    EXPECT_EQ(journal->last_config.document, config.document)
        << "the document did not reach the provider byte-for-byte";
    EXPECT_EQ(journal->last_config.document.size(), document_bytes)
        << "the document was truncated — the length is authoritative, the bytes may contain NUL";
}

TEST(Registry, AFactoryThatFailsIsReportedAsATypedSeamFailure) {
    ProviderRegistry registry;
    registry.Register("null-returner",
                      [](const ProviderConfig&) { return std::shared_ptr<PubSubProvider>{}; });
    registry.Register("thrower", [](const ProviderConfig&) -> std::shared_ptr<PubSubProvider> {
        throw std::runtime_error("the transport would not start");
    });
    registry.Register("typed-thrower",
                      [](const ProviderConfig&) -> std::shared_ptr<PubSubProvider> {
                          throw PubSubError(PubSubStatus::kTransportFailure, "no endpoint");
                      });
    registry.Register("overflower", [](const ProviderConfig&) -> std::shared_ptr<PubSubProvider> {
        throw std::overflow_error("payload bound");
    });

    const ProviderConfig config;
    EXPECT_EQ(RefusalOf([&] { return MakeProvider(registry, "null-returner", config); },
                        "a factory returning null"),
              PubSubStatus::kInternal);
    EXPECT_EQ(RefusalOf([&] { return MakeProvider(registry, "thrower", config); },
                        "a factory throwing an untyped exception"),
              PubSubStatus::kInternal);
    EXPECT_TRUE(Mentions(MessageOf([&] { return MakeProvider(registry, "thrower", config); }),
                         "the transport would not start"))
        << "the original message was lost on the way out of the seam";
    EXPECT_EQ(RefusalOf([&] { return MakeProvider(registry, "typed-thrower", config); },
                        "a factory throwing a PubSubError"),
              PubSubStatus::kTransportFailure);
    EXPECT_EQ(RefusalOf([&] { return MakeProvider(registry, "overflower", config); },
                        "a factory throwing std::overflow_error"),
              PubSubStatus::kPayloadTooLarge);
}

}  // namespace conformance
}  // namespace fletcher
