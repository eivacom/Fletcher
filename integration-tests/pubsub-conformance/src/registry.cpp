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

#include <chrono>
#include <cstdint>
#include <fletcher/core/status.hpp>
#include <fletcher/core/types.hpp>
#include <fletcher/core/write_buffer.hpp>
#include <fletcher/pubsub/in_process_provider.hpp>
#include <fletcher/pubsub/owned_schema.hpp>
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

// Same encoding, an explicit topic — for the loopback tests below, where the
// topic (not just the marker byte) decides which subscription sees the row.
void PublishRowTo(PubSubProvider& provider, const std::vector<std::string>& topic,
                  uint8_t marker) {
    provider.Publish(topic, [marker](WriteBuffer& buffer) {
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

// A minimal real schema — struct<seq:int32> — built with nanoarrow directly
// rather than by linking conformance_support's fixtures: this binary's link
// line (`fletcher-pubsub` and no transport SDK, no conformance_support) is
// itself a machine check (design forbidden case 5), and pulling in the shared
// fixture would weaken it for one convenience function.
OwnedSchema MakeSchema() {
    OwnedSchema s;
    ArrowSchemaInit(s.get());
    if (ArrowSchemaSetTypeStruct(s.get(), 1) != NANOARROW_OK) {
        throw std::runtime_error("MakeSchema: ArrowSchemaSetTypeStruct failed");
    }
    ArrowSchemaSetName(s->children[0], "seq");
    ArrowSchemaSetType(s->children[0], NANOARROW_TYPE_INT32);
    return s;
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

    // Non-default on every field: "the caller's config arrived" has to be
    // distinguishable from "a default-constructed one did". A resolver handed
    // `ProviderConfig{}` would put a loaded driver on domain 0 with no payload
    // bound, silently — a wrong answer rather than a failure.
    ProviderConfig config;
    config.max_payload_bytes = 65000;
    config.domain_id = 151;
    config.document = "<qos/>";
    config.document.push_back('\0');
    config.document += "tail";
    const std::string kPath = "/opt/fletcher/libstandin_driver.so";

    std::shared_ptr<PubSubProvider> built_in = MakeProvider(registry, "alpha", config);
    ASSERT_NE(built_in, nullptr);
    PublishRow(*built_in, 0xA1);

    // So that what is asserted below is what the RESOLVER saw, not what the
    // name branch left behind.
    journal->last_config = ProviderConfig{};

    std::shared_ptr<PubSubProvider> loaded = MakeProvider(registry, kPath, config);
    ASSERT_NE(loaded, nullptr) << "the path selector produced nothing";
    PublishRow(*loaded, 0xC3);

    EXPECT_EQ(journal->resolver_calls, 1)
        << "the resolver seat was reached " << journal->resolver_calls
        << " times for one path selector";
    EXPECT_EQ(journal->resolved_path, kPath)
        << "the resolver was handed `" << journal->resolved_path
        << "`, not the path the caller configured — a loader would open the wrong library";

    // "with the identical config — only the string differs" is the claim this
    // whole test makes; here it is asserted rather than commented.
    EXPECT_EQ(journal->last_config.max_payload_bytes, 65000u)
        << "the resolver was handed a payload bound of " << journal->last_config.max_payload_bytes
        << ", not the caller's";
    EXPECT_EQ(journal->last_config.domain_id, 151u)
        << "the resolver was handed domain " << journal->last_config.domain_id
        << ", not the caller's — a loaded driver would join the wrong domain with no error";
    EXPECT_EQ(journal->last_config.document, config.document)
        << "the document did not reach the resolver byte-for-byte";

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

    // `\xNN` is the diagnostic's OWN escape spelling for a NON-PRINTABLE byte, so
    // without escaping the backslash the encoding is not injective: a path
    // containing `\`, `x`, `f`, `f` renders identically to one holding the single
    // byte 0xFF, in the one message an operator reads when a driver will not
    // load. (0x64 is `d`, printable, so it is emitted raw and never collides —
    // this case pins the ordinary Windows path shape.)
    const std::string windows_path = "C:\\x64\\driver.dll";
    const std::string quoted =
        MessageOf([&] { return MakeProvider(registry, windows_path, config); });
    EXPECT_TRUE(Mentions(quoted, "\"C:\\\\x64\\\\driver.dll\""))
        << "the backslashes in a Windows driver path are not escaped: " << quoted;
    EXPECT_FALSE(Mentions(quoted, windows_path))
        << "the path is reproduced raw, so its backslash sequences are ambiguous "
           "with the diagnostic's own escapes: " << quoted;
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

    // The alphabet is [A-Za-z0-9_-], not [a-z_-]. A build that quietly dropped
    // the digits or the uppercase range would reclassify this name as a PATH:
    // `Register` would refuse it, and a configuration saying `Fast2DDS-v1_x`
    // would be told this build cannot load drivers. Every other entry in this
    // file uses lowercase, `-` and `_` only, so nothing else would notice.
    registry.Register("Fast2DDS-v1_x", ProbeFactory("Fast2DDS-v1_x", journal));
    std::shared_ptr<PubSubProvider> mixed =
        MakeProvider(registry, "Fast2DDS-v1_x", ProviderConfig{});
    ASSERT_NE(mixed, nullptr) << "a name spelling the full alphabet did not select";
    PublishRow(*mixed, 0xF7);
    ASSERT_EQ(journal->rows.size(), static_cast<size_t>(2));
    EXPECT_EQ(journal->rows[1].tag, "Fast2DDS-v1_x");
}

// ── PDA-DEC-5: the loopback becomes a built-in ──────────────────────
//
// THE FORCING TEST. `RegisterInProcessProvider` is the whole of §1's
// registration; before it exists this file does not compile. Not greenable by
// a non-null check: the row must arrive byte-identical THROUGH THE BASE-TYPED
// HANDLE (no downcast to InProcessPubSubProvider anywhere below), and a
// publish to a topic nobody declared must succeed — pinning the default mode
// to as_declared without exposing an accessor for it.
TEST(Registry, InProcessResolvesAsABuiltIn) {
    ProviderRegistry registry;
    RegisterInProcessProvider(registry);

    const ProviderConfig config;  // empty document -> the defaults
    std::shared_ptr<PubSubProvider> provider = MakeProvider(registry, "inprocess", config);
    ASSERT_NE(provider, nullptr) << "\"inprocess\" did not resolve to a provider";

    std::vector<uint8_t> received;
    bool delivered = false;
    SubscriptionResult result = provider->Subscribe(
        {"registry", "probe"},
        [&](const uint8_t* data, size_t len, const SharedSchema&, const Attachments&) {
            received.assign(data, data + len);
            delivered = true;
        });

    // as_declared with nothing ever declared answers immediately with a null
    // schema (kOk + null is the schema-less transport's answer, §7 clause 1) —
    // not kPending, which is what a schema-carrying instance would do instead.
    SharedSchema schema;
    ASSERT_EQ(result.schema.Wait(std::chrono::milliseconds(0), &schema), PubSubStatus::kOk);
    EXPECT_EQ(schema, nullptr);

    PublishRow(*provider, 0x17);
    ASSERT_TRUE(delivered) << "the row never reached the subscriber";
    EXPECT_EQ(received, Row(0x17)) << "the delivered bytes are not what was published";

    // A SECOND, never-declared topic: the as_declared default has no
    // CreateTopic requirement, so this must succeed rather than throw
    // kTopicNotDeclared. Nothing here names InProcessPubSubProvider or its
    // (private, post-PDA-DEC-5) carriage enum — the base-typed handle is the
    // whole test.
    EXPECT_NO_THROW(PublishRowTo(*provider, {"registry", "never-declared"}, 0x18));
}

// ── The live control for the forcing test's default-mode claim ─────
//
// Same helper, a document instead of nothing. If the document were ignored
// this test goes red while the forcing test above stays green — neither alone
// tells the whole story, together they pin that the MODE genuinely comes from
// `config.document`, not from some other default baked into the factory.
TEST(Registry, InProcessCarriageComesFromTheDocument) {
    ProviderRegistry registry;
    RegisterInProcessProvider(registry);

    ProviderConfig config;
    config.document = "schema_carriage=carried";
    std::shared_ptr<PubSubProvider> provider = MakeProvider(registry, "inprocess", config);
    ASSERT_NE(provider, nullptr);

    // Schema-before-data, upheld by refusal: publishing before any
    // CreateTopic is kTopicNotDeclared — the opposite of the forcing test's
    // as_declared assertion above, over the identical registry call.
    EXPECT_EQ(RefusalOf([&] { PublishRowTo(*provider, {"registry", "carried"}, 0x19); },
                        "publish to an undeclared topic on a carrying instance"),
              PubSubStatus::kTopicNotDeclared);

    provider->CreateTopic({"registry", "carried"}, MakeSchema());

    SharedSchema received_schema;
    bool delivered = false;
    SubscriptionResult result = provider->Subscribe(
        {"registry", "carried"},
        [&](const uint8_t*, size_t, const SharedSchema& schema, const Attachments&) {
            received_schema = schema;
            delivered = true;
        });
    SharedSchema arrived;
    ASSERT_EQ(result.schema.Wait(std::chrono::milliseconds(0), &arrived), PubSubStatus::kOk);
    ASSERT_NE(arrived, nullptr) << "a carrying instance answered kOk with a null schema";

    PublishRowTo(*provider, {"registry", "carried"}, 0x1a);
    ASSERT_TRUE(delivered);
    EXPECT_NE(received_schema, nullptr)
        << "a carrying instance delivered a row with no schema attached";
}

// ── Rung-2 case 6: the document reader refuses at the door ──────────
//
// Every input here must fail with kInvalidArgument (not "threw something"),
// quoting the offending entry — P1's converse obligation, and the machine
// check that a `PubSubError` thrown by a factory reaches the caller intact.
// M5 (design table): ignoring an unknown entry instead of refusing it reddens
// this test.
TEST(Registry, InProcessRefusesAnUnrecognisedDocumentEntry) {
    ProviderRegistry registry;
    RegisterInProcessProvider(registry);

    // `offending_entry` is the ONE entry the reader must quote — for the
    // duplicate-key case that is the SECOND occurrence, not the document's
    // first line, so it is passed explicitly rather than derived.
    auto refuse = [&](const std::string& document, const std::string& offending_entry,
                      const char* what) {
        ProviderConfig config;
        config.document = document;
        EXPECT_EQ(RefusalOf([&] { return MakeProvider(registry, "inprocess", config); }, what),
                  PubSubStatus::kInvalidArgument);
        const std::string message =
            MessageOf([&] { return MakeProvider(registry, "inprocess", config); });
        // The offending entry, quoted, must appear in the refusal — a message
        // naming only "bad document" would leave an operator guessing which
        // line of a multi-line document was wrong.
        EXPECT_TRUE(Mentions(message, offending_entry))
            << what << ": refusal does not quote the offending entry: " << message;
    };

    refuse("schema_carriage=Carried", "schema_carriage=Carried", "an unrecognised value");
    refuse("nonsense=1", "nonsense=1", "an unrecognised key");
    refuse("schema_carriage", "schema_carriage", "an entry with no '='");
    // DEBT-3: the fourth rung-2 refusal — a duplicate key — needs its own case,
    // or "duplicate key -> kInvalidArgument" is a rule asserted by nothing.
    refuse("schema_carriage=as_declared\nschema_carriage=carried", "schema_carriage=carried",
          "a duplicate key");
    // An empty value IS an unrecognised value (`"" != "as_declared"` and
    // `"" != "carried"`) — already the door's behaviour, but asserted by
    // nothing before this cycle. Mutation: treating an empty value as a
    // no-op default reddens this.
    refuse("schema_carriage=", "schema_carriage=", "an empty value");
    // "Nothing else is trimmed" (design §2), pinned rather than assumed: a
    // leading space makes the KEY " schema_carriage" (unknown key), and a
    // trailing space makes the VALUE "carried " (unknown value). Mutation: a
    // future trim added to either side would make one of these SUCCEED where
    // it must be refused, reddening whichever row it trims.
    refuse(" schema_carriage=carried", " schema_carriage=carried",
          "leading whitespace is not trimmed");
    refuse("schema_carriage=carried ", "schema_carriage=carried ",
          "trailing whitespace on the value is not trimmed");
}

// ── The tolerance rules, proved by mutation rather than assumed ─────
//
// Design §2 commits to two rules beyond the four refusals above: a trailing
// `\r` is stripped (H2 — a document authored on Windows must mean the same
// thing as one authored on Linux), and a blank entry (a blank line or a
// trailing newline) is skipped. Both were implemented and neither was
// guarded — closed here. `is_carried` turns "did this document construct a
// CARRYING instance" into one boolean: publish-before-declare on a carrying
// instance is refused kTopicNotDeclared (proved by
// `InProcessCarriageComesFromTheDocument` above), and that refusal is
// unreachable from `as_declared`, so it is a faithful witness of the mode
// with no accessor needed.
TEST(Registry, InProcessDocumentToleratesCrlfAndBlankLines) {
    ProviderRegistry registry;
    RegisterInProcessProvider(registry);

    auto is_carried = [&](const std::string& document) {
        ProviderConfig config;
        config.document = document;
        std::shared_ptr<PubSubProvider> provider = MakeProvider(registry, "inprocess", config);
        return RefusalOf(
                   [&] {
                       PublishRowTo(*provider, {"registry", "tolerance-probe"}, 0x1b);
                   },
                   "publish to an undeclared topic") == PubSubStatus::kTopicNotDeclared;
    };

    // Mutation: deleting the `\r`-strip block makes "carried\r" compare
    // unequal to "carried", so THIS CONSTRUCTION THROWS (unknown value)
    // instead of succeeding — `is_carried` never gets called, and the
    // EXPECT_TRUE below fails because MakeProvider's exception escapes it.
    EXPECT_TRUE(is_carried("schema_carriage=carried\r\n"))
        << "a CRLF-terminated entry was not recognised as carried — the \\r strip regressed";

    // Mutation: deleting the empty-entry skip makes the interior blank line
    // fail "entry with no '='" (empty string, no '='), so this construction
    // throws where it must succeed.
    EXPECT_TRUE(is_carried("\nschema_carriage=carried"))
        << "a leading blank line was not skipped — the empty-entry skip regressed";

    // Same mutation, the OTHER shape it guards: a trailing newline produces a
    // trailing empty entry via the same split.
    EXPECT_TRUE(is_carried("schema_carriage=carried\n"))
        << "a trailing newline was not skipped — the empty-entry skip regressed";

    // Both rules at once, including a blank line strictly between two
    // entries — not just at an edge.
    EXPECT_TRUE(is_carried("\r\nschema_carriage=carried\r\n\r\n"))
        << "CRLF and blank-line tolerance did not compose";
}

// The NUL refusal added by this fix cycle: `ProviderConfig::document`'s C
// form sanctions an embedded NUL (`provider_registry.hpp`), so one can
// legitimately reach this reader — and unlike every other refusal above, the
// message here does NOT quote the raw entry (a `std::runtime_error`'s
// `what()` -> `c_str()` stops dead at a NUL, so quoting one would silently
// truncate the diagnostic an operator reads). Mutation: deleting the check
// reddens this test AND turns
// `Registry.AFactoryThatFailsIsReportedAsATypedSeamFailure`-style silent
// truncation into a live bug — a NUL-bearing document would either match an
// unrelated key/value pair by accident (it does not, here — the byte is
// simply part of the compared string) or produce a refusal message that
// stops mid-sentence.
TEST(Registry, InProcessRefusesADocumentContainingANul) {
    ProviderRegistry registry;
    RegisterInProcessProvider(registry);

    std::string document = "schema_carriage=carried";
    document.push_back('\0');
    document += "tail";
    ProviderConfig config;
    config.document = document;

    EXPECT_EQ(RefusalOf([&] { return MakeProvider(registry, "inprocess", config); },
                        "a document containing an embedded NUL"),
              PubSubStatus::kInvalidArgument);
    const std::string message =
        MessageOf([&] { return MakeProvider(registry, "inprocess", config); });
    EXPECT_TRUE(Mentions(message, "NUL"))
        << "the refusal does not say why: " << message;
    EXPECT_TRUE(Mentions(message, "offset 23"))
        << "the refusal does not locate the NUL (offset 23, right after 'carried'): " << message;
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

// The resolver seat's failure path is normative — the header and spec §5.1 both
// say "a factory **or resolver**" — and it is the branch PDA-ABI fills blind,
// against a header it cannot see this file from. Without these entries, deleting
// the `TranslateSeamFailure` around the resolver call, or deleting the
// resolver's null check, leaves every other entry in this file green. One
// registry per shape, because a second `SetPathResolver` is refused.
TEST(Registry, AResolverThatFailsIsReportedAsATypedSeamFailure) {
    const ProviderConfig config;
    const std::string kPath = "./driver.so";

    ProviderRegistry thrower;
    thrower.SetPathResolver(
        [](const std::string&, const ProviderConfig&) -> std::shared_ptr<PubSubProvider> {
            throw std::runtime_error("the driver would not load");
        });
    EXPECT_EQ(RefusalOf([&] { return MakeProvider(thrower, kPath, config); },
                        "a resolver throwing an untyped exception"),
              PubSubStatus::kInternal);
    EXPECT_TRUE(Mentions(MessageOf([&] { return MakeProvider(thrower, kPath, config); }),
                         "the driver would not load"))
        << "the loader's own message was lost on the way out of the seam, which is the one "
           "diagnostic an operator has when a driver refuses to load";

    ProviderRegistry typed_thrower;
    typed_thrower.SetPathResolver(
        [](const std::string&, const ProviderConfig&) -> std::shared_ptr<PubSubProvider> {
            throw PubSubError(PubSubStatus::kTransportFailure, "no endpoint");
        });
    EXPECT_EQ(RefusalOf([&] { return MakeProvider(typed_thrower, kPath, config); },
                        "a resolver throwing a PubSubError"),
              PubSubStatus::kTransportFailure)
        << "a resolver cannot choose its own status, so an unloadable driver cannot fail "
           "distinctly (owner ruling 2026-09-02)";

    ProviderRegistry null_returner;
    null_returner.SetPathResolver([](const std::string&, const ProviderConfig&) {
        return std::shared_ptr<PubSubProvider>{};
    });
    EXPECT_EQ(RefusalOf([&] { return MakeProvider(null_returner, kPath, config); },
                        "a resolver returning null"),
              PubSubStatus::kInternal)
        << "Create handed the caller a null provider instead of refusing";
}

// ── The lifetime rule, enforced rather than asked for ───────────────
//
// DEBT-1's rule obliges a resolver OR a factory to keep a loaded module alive
// for at least as long as the providers it made. This asserts the registry holds
// them to it: the stand-in module is reachable only through the seam — the local
// handle is gone and the registry itself is destroyed — and it must still be
// loaded while a provider made from it is publishing, then be released when the
// last such provider goes. Both routes PDA-ABI can take are covered, because
// `Register("zenoh", factory_that_dlopens)` reaches a loaded driver by name.
namespace {

// Stands where a loaded module stands: the thing whose code the provider is
// still executing. It records its own unload rather than crashing, because a
// real use-after-unload is "not reliably loud".
struct ModuleStandIn {
    explicit ModuleStandIn(bool* unloaded) : unloaded_(unloaded) {}
    ~ModuleStandIn() { *unloaded_ = true; }
    bool* unloaded_;
};

// A provider that reads its module's state AT ITS OWN DESTRUCTION.
//
// Sampling the flag from the test body cannot see the ordering that makes the
// mechanism work: around `provider.reset()` the flag reads false then true
// whichever of the two the anchor releases first. The only observer inside the
// window is the provider's own destructor — which is also the window that
// matters, because that is where a real provider closes endpoints by calling
// into module code. Reversing the anchor's members reddens this and nothing
// else.
class ModuleUserProvider : public ProbeProvider {
   public:
    ModuleUserProvider(std::string tag, std::shared_ptr<Journal> journal, const bool* unloaded,
                       bool* module_was_loaded_at_my_death)
        : ProbeProvider(std::move(tag), std::move(journal)),
          unloaded_(unloaded),
          module_was_loaded_at_my_death_(module_was_loaded_at_my_death) {}

    ~ModuleUserProvider() override { *module_was_loaded_at_my_death_ = !*unloaded_; }

   private:
    const bool* unloaded_;
    bool* module_was_loaded_at_my_death_;
};

}  // namespace

TEST(Registry, AModuleHeldOnlyByTheSeamOutlivesTheProvidersItMade) {
    // (a) reached by PATH, through the resolver seat.
    {
        auto journal = std::make_shared<Journal>();
        bool unloaded = false;
        bool module_was_loaded_at_my_death = false;
        std::shared_ptr<PubSubProvider> provider;
        {
            ProviderRegistry registry;
            auto module = std::make_shared<ModuleStandIn>(&unloaded);
            // The resolver author does the WRONG thing on purpose: the module is
            // captured by the resolver and not tied to the provider's ownership.
            registry.SetPathResolver([module, journal, &unloaded, &module_was_loaded_at_my_death](
                                         const std::string&, const ProviderConfig&) {
                return std::make_shared<ModuleUserProvider>("loaded", journal, &unloaded,
                                                            &module_was_loaded_at_my_death);
            });
            provider = MakeProvider(registry, "./libstandin_driver.so", ProviderConfig{});
            ASSERT_NE(provider, nullptr);
        }  // the registry, and the caller's own handle on the module, are gone

        EXPECT_FALSE(unloaded)
            << "the module was unloaded while a provider made from it was still live: this is "
               "the use-after-unload DEBT-1 describes, and prose did not stop it";
        PublishRow(*provider, 0xF6);
        ASSERT_EQ(journal->rows.size(), static_cast<size_t>(1));
        EXPECT_EQ(journal->rows[0].tag, "loaded");

        provider.reset();
        EXPECT_TRUE(unloaded)
            << "the module outlived the last provider that needed it, so nothing ever unloads it";
        EXPECT_TRUE(module_was_loaded_at_my_death)
            << "the provider's destructor ran after its module was already unloaded: the anchor "
               "releases the seat before the provider, so a provider closing endpoints in its "
               "destructor calls into unmapped module code";
    }

    // (b) reached by NAME, through a factory — the linkage ruling's static half
    // and `Register("zenoh", factory_that_dlopens)`.
    {
        auto journal = std::make_shared<Journal>();
        bool unloaded = false;
        bool module_was_loaded_at_my_death = false;
        std::shared_ptr<PubSubProvider> provider;
        {
            ProviderRegistry registry;
            auto module = std::make_shared<ModuleStandIn>(&unloaded);
            registry.Register("zenoh", [module, journal, &unloaded,
                                        &module_was_loaded_at_my_death](const ProviderConfig&) {
                return std::make_shared<ModuleUserProvider>("zenoh", journal, &unloaded,
                                                            &module_was_loaded_at_my_death);
            });
            provider = MakeProvider(registry, "zenoh", ProviderConfig{});
            ASSERT_NE(provider, nullptr);
        }

        EXPECT_FALSE(unloaded)
            << "a factory holding a module handle is the same use-after-unload by the other "
               "route, and the registry owns `factories_` exactly as it owns the resolver";
        PublishRow(*provider, 0xF8);
        ASSERT_EQ(journal->rows.size(), static_cast<size_t>(1));
        EXPECT_EQ(journal->rows[0].tag, "zenoh");

        provider.reset();
        EXPECT_TRUE(unloaded) << "the factory outlived the last provider it made";
        EXPECT_TRUE(module_was_loaded_at_my_death)
            << "by the name route too: the provider's destructor ran after the factory that "
               "captured its module had already been released";
    }
}

}  // namespace conformance
}  // namespace fletcher
