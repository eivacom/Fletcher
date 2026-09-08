// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// Subject registration: Fast DDS, in-process AND across a process boundary.
// The cross-process one is the whole reason this harness exists — Fast DDS
// serves same-process endpoints over intra-process delivery, so the local
// subject cannot see the transport at all (spec §7.2).
//
// Fixed, distinct domains so the harness cannot collide with
// integration-tests/fastdds-xrce-interop (domain 145) or with itself. Domains
// 151 and 152 are shared with the TypeScript gateway harnesses
// (integration-tests/gateway-end-to-end/test/end-to-end.test.ts:54 and
// protoc-gen.test.ts:55, both overridable by DDS_DOMAIN_ID) — pre-existing, and
// left as it is; 153 and the Registry.TwoInstances* block at 161-167 are this
// binary's alone.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <fletcher/core/write_buffer.hpp>
#include <fletcher/fastdds_pubsub_provider/fast_dds_pubsub_provider.hpp>
#include <fletcher/pubsub/provider_registry.hpp>
#include <memory>
#include <mutex>
#include <stdexcept>
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
/// `TopicNames.AmbiguousSegmentsAreRefused`'s own domain, NOT shared with the registry
/// case above. That case's participant is discovering and matching for its whole run, and
/// this one stands a participant up too (a refusal is checked at the seam door, but the
/// provider is constructed first). This round has already lost a review cycle to two cases
/// sharing a domain, so the census range is used rather than re-argued: PDA-DEC-8 recorded
/// 154-158 unused, and 154 is in fact taken by
/// integration-tests/gateway-end-to-end/test/end-to-end.test.ts:360 — a TypeScript harness
/// the C++-only grep behind that record could not see. 155 is the first genuinely free one.
constexpr uint32_t kTopicNamesDomain = 155;

std::shared_ptr<PubSubProvider> MakeFastDds(uint32_t domain_id) {
    return std::make_shared<FastDDSPubSubProvider>(ProviderConfig{.domain_id = domain_id});
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

// ── §3.5 — a topic's segment list IS its identity, on real Fast DDS ──
//
// The CROSS-PROVIDER half of PDA-DEC-A5. The refusal lives at one door
// (`internal::RequireSegments`), so what this asserts is that Fast DDS still
// routes every entry point through it — the 2026-08-31 divergence ruling
// requires all three providers to agree, and "the loopback agrees with itself"
// is not that evidence.
//
// A plain `TEST` and not a `TEST_P(ProviderConformance, ...)`, deliberately:
// the parameterised suite also runs on `FastDdsCrossProcess`, whose
// `PeerSubject::RejectUnsendableTopic` refuses these shapes ITSELF and returns
// `Reply::HarnessFailure` — so the clause would score the harness's door for
// three shapes and pass vacuously on the fourth. Constructing the provider
// directly, as `Registry.FastDdsResolvesAsABuiltIn` above does, removes the
// question rather than working around it.
//
// Nothing here waits for discovery or for a row: a refused name never reaches
// the transport, which is the whole point of refusing at the door.
TEST(TopicNames, AmbiguousSegmentsAreRefused) {
    std::shared_ptr<PubSubProvider> provider = MakeFastDds(kTopicNamesDomain);

    auto refused = [](auto&& call) {
        try {
            call();
        } catch (const PubSubError& e) {
            return e.status() == PubSubStatus::kInvalidArgument;
        } catch (...) {
            return false;
        }
        return false;
    };

    // Built byte by byte: `std::string("a\0b")` would stop at the zero and
    // quietly become an ordinary one-character segment.
    std::string nul_bearing = "a";
    nul_bearing.push_back('\0');
    nul_bearing += "b";

    const std::vector<std::pair<std::vector<std::string>, std::string>> kRefused = {
        {{nul_bearing}, "a NUL, which Fast DDS carries whole but XRCE truncates"},
        {{"a/b"}, "the separator, which aliases with {a, b}"},
        {{"a", "b/c"}, "the separator in a later segment"},
        {{""}, "an empty segment"},
        {{"a", ""}, "a trailing empty segment"},
        {{"a", "__schema"}, "the reserved `__` namespace this provider derives its companion in"},
        {{"__anything"}, "the reserved `__` namespace"},
    };

    for (const auto& entry : kRefused) {
        const std::vector<std::string>& topic = entry.first;
        const std::string& why = entry.second;

        EXPECT_TRUE(refused([&] {
            provider->CreateTopic(topic, MakeConformanceSchema(SchemaId::kA));
        })) << "Fast DDS CreateTopic accepted "
            << why;
        EXPECT_TRUE(refused([&] {
            provider->Publish(topic, [](WriteBuffer& buffer) { buffer.AppendByte(0x01); });
        })) << "Fast DDS Publish accepted "
            << why;
        EXPECT_TRUE(refused([&] {
            static_cast<void>(provider->Subscribe(
                topic, [](const uint8_t*, size_t, const SharedSchema&, const Attachments&) {}));
        })) << "Fast DDS Subscribe accepted "
            << why;
        EXPECT_TRUE(refused([&] { provider->Unsubscribe(topic); }))
            << "Fast DDS Unsubscribe accepted " << why;
    }

    // The bound: an ordinary name with a dot, a hyphen and a single leading
    // underscore still declares. Without this a provider that refused every
    // topic would be green above.
    EXPECT_NO_THROW(
        provider->CreateTopic({"_vessel.bow", "depth-raw"}, MakeConformanceSchema(SchemaId::kA)));
}

// ── Two instances, one registry, one process (spec §4, third clause) ──
//
// Spec §4's third normative item: "**No global state.** The registry takes and
// returns explicit objects; multiple instances of the same provider with
// different configs must be ordinary."  The four cases below are that clause,
// executable, over the provider most likely to break it — Fast DDS routes every
// participant through a process-wide `DomainParticipantFactory` singleton.
//
// **These cases are GREEN on the tree that first shipped them, and that is
// disclosed rather than dressed up.** The property already holds, so a passing
// run proves nothing on its own; what makes them a guard is the six-row mutation
// table in README.md, each row a minimal edit to product code that turns a NAMED
// CASE here red — by a named assertion, or by the typed refusal that row's README
// entry records, three of which kill the case in `Instance`'s constructor before
// any assertion is reached — observed and recorded. A proof over a property the
// tree already has has no other honest red.
//
// Three things carry the arrangement, and none of them may drift:
//
//  1. **One `kBound`, equal in both instances of every case that asserts or
//     denies a crossing.** The registered DDS type name is `fletcher_<bound>`
//     (`payload_bound.hpp`, locked decision 13), and DDS matches by type name —
//     so unequal bounds are an INDEPENDENT reason two endpoints never meet, on
//     any domain. Give the two instances different bounds here and the isolation
//     case would pass identically with process-wide state present, because the
//     streams could never have met in the first place. The per-instance-bound
//     claim therefore lives in its own pair, on its own two domains
//     (`TwoInstancesKeepTheirOwnPayloadBounds`), which MAKES NO CROSSING CLAIM
//     in either direction. `domain_id` is the only wire-visible difference left:
//     the schema companion type name is the bound-independent constant
//     `SchemaBytes`, topic names are identical by construction, no partitions
//     are set anywhere, and both instances take an EMPTY document, so
//     participant, writer and reader QoS are byte-identical.
//  2. **The standing positive control.** `TwoInstancesOneDomainDoInterfere` runs
//     the same helper, the same topic names and the same `kBound`, differing
//     only in that both instances sit on one domain, and asserts the row DOES
//     cross inside `kSettle`. Whenever the arrangement loses its teeth the
//     control reddens while the isolation case stays green — that is what
//     separates "isolated" from "never had a chance". One `kSettle` serves both,
//     so no edit can widen the control's window and narrow the isolation
//     case's.
//  3. **Journals compared WHOLE**, against the exact vector of markers that
//     instance published: never "contains", never a tolerance, never a leakage
//     threshold. "Mostly isolated" is not a result these cases can report.
//
// The schema shape is deliberately NOT part of the discovery key and cannot be:
// the data type name is `fletcher_<bound>` alone, and a reader performs no
// row-against-schema validation, so giving the two instances different shapes
// can neither keep the streams apart nor suppress a delivery. It is asserted as
// an OUTCOME (each subscriber receives its own shape), not relied on as a
// separator. The control uses one shape in both instances precisely so that the
// §7-clause-3 question it does not own cannot arise.
namespace {

// Seven domains this file owns outright. It shares topic names deliberately, so
// unlike integration-tests/pubsub-arrow-fastdds — four tests on domain 137 with
// the same topic names, which cross-talk under a parallel preset — the domains
// are what keeps the cases apart.
//
// The range is 161-167, and it was checked across EVERY harness in the tree
// rather than the C++ ones only. The census command, run from the repo root:
//
//   grep -rniE 'domain' --include='*.ts' --include='*.js' --include='*.json'
//        --include='*.cpp' --include='*.hpp' --include='*.py' --include='*.txt'
//        --include='*.cmake' --include='*.sh' --include='*.ps1' --include='*.xml'
//        --include='*.md' . | grep -v /build | grep -v node_modules
//
// filtered to the numbers each hit names, which yields 0, 3, 4, 5, 7, 16, 32,
// 43, 91-99, 137, 142, 145, 151-154 — and nothing at all in 161-167.
//
// This block's first draft took 154-160 on a census of the C++ suites alone, and
// 154 is in fact taken: integration-tests/gateway-end-to-end/test/
// end-to-end.test.ts:360 sets `domainId: '154'`, in a TypeScript harness a
// C++-only grep cannot see. That harness runs under `npm test` rather than as a
// ctest entry, so it never serialises against this binary through
// RESOURCE_LOCK — which is why the range was moved rather than argued about.
constexpr uint32_t kIsolationDomainA = 161;
constexpr uint32_t kIsolationDomainB = 162;
constexpr uint32_t kControlDomain = 163;  // BOTH instances, on purpose
constexpr uint32_t kConcurrentDomainA = 164;
constexpr uint32_t kConcurrentDomainB = 165;
constexpr uint32_t kLowBoundDomain = 166;
constexpr uint32_t kHighBoundDomain = 167;

// ONE bound, for every case that asserts or denies a crossing. Making these two
// instances differ here means deleting this constant — see (1) above.
constexpr uint32_t kBound = 65536;

// The bound pair's two bounds, and a row size strictly between them.
constexpr uint32_t kLowBound = 4096;
constexpr uint32_t kHighBound = 65536;
constexpr size_t kBetweenBoundsRowBytes = 8192;
static_assert(kLowBound < kBetweenBoundsRowBytes && kBetweenBoundsRowBytes < kHighBound);

// The window in which the CONTROL measured a real crossing on one domain, and
// therefore the window in which "no row crossed" means something. One number for
// both cases, and the only pressure on it is toward widening — which strengthens
// the isolation claim rather than weakening it.
constexpr std::chrono::milliseconds kSettle{1500};

// Rows published concurrently per instance, in the concurrent case.
constexpr uint8_t kConcurrentRows = 32;

// A row is two marker bytes — the publishing instance's tag and a sequence
// number — plus whatever padding the case wants. Both bytes are journalled, so
// a marker names (instance, publish) uniquely and a foreign row can never be
// mistaken for an own one.
//
// Rendered as a short string rather than kept as the two bytes because these
// cases exist to be READ when they fail: a whole-journal mismatch printed as
// `{ "B:0", "A:0" }` versus `{ "A:0" }` names the interference, and the same
// mismatch printed as two integers does not. A red nobody can interpret is not
// evidence (README, the mutation table).
std::string Marker(char tag, uint8_t seq) {
    return std::string(1, tag) + ":" + std::to_string(static_cast<unsigned>(seq));
}

// What one subscription actually saw.
//
// Appended on a Fast DDS listener thread and read on the main thread, so every
// access is under the mutex and every comparison is made against a snapshot
// taken under it. This is not hygiene: unguarded, a `push_back` racing the read
// is UB, and — the failure that would matter here — a FOREIGN marker arriving
// during the read could be missed, which is a green this arrangement did not
// earn. That is the exact defect class these cases exist to rule out, so a race
// that hides interference would defeat their entire purpose.
class Journal {
   public:
    struct Entry {
        std::string marker;
        // `n_children` of the schema delivered with the row: 1 for the
        // conformance suite's shape A (struct<seq:int32>), 2 for shape B
        // (struct<seq:int32,extra:float64>). -1 when no schema arrived at all,
        // which spec §7 forbids and which is therefore asserted against rather
        // than tolerated.
        int64_t schema_children;
    };

    void Record(const uint8_t* data, size_t len, const SharedSchema& schema) {
        Entry entry{"<row shorter than its marker>", schema == nullptr ? -1 : schema->n_children};
        if (len >= 2) entry.marker = Marker(static_cast<char>(data[0]), data[1]);
        std::lock_guard<std::mutex> lock(mu_);
        entries_.push_back(entry);
    }

    std::vector<Entry> Snapshot() const {
        std::lock_guard<std::mutex> lock(mu_);
        return entries_;
    }

    size_t Count() const {
        std::lock_guard<std::mutex> lock(mu_);
        return entries_.size();
    }

   private:
    mutable std::mutex mu_;
    std::vector<Entry> entries_;
};

std::vector<std::string> Markers(const std::vector<Journal::Entry>& entries) {
    std::vector<std::string> out;
    out.reserve(entries.size());
    for (const Journal::Entry& entry : entries) out.push_back(entry.marker);
    return out;
}

// Every distinct schema shape the subscription was handed, so the assertion can
// name what arrived instead of only that something did.
std::vector<int64_t> Shapes(const std::vector<Journal::Entry>& entries) {
    std::vector<int64_t> out;
    for (const Journal::Entry& entry : entries) {
        if (std::find(out.begin(), out.end(), entry.schema_children) == out.end()) {
            out.push_back(entry.schema_children);
        }
    }
    return out;
}

// One instance: a provider built THROUGH the registry, a topic name shared with
// every other instance, a topic name private to this one, and one journal per
// subscription.
//
// Member order is load-bearing. `provider_` is declared LAST, so it is destroyed
// FIRST — the provider's teardown deletes its DataReaders, and therefore stops
// every listener that can reach a journal, before the journals themselves go
// away (spec §6 clause 5 quiescence).
class Instance {
   public:
    Instance(const ProviderRegistry& registry, uint32_t domain_id, uint32_t payload_bound,
             SchemaId shape, char tag)
        : tag_(tag),
          shared_topic_{"pdadec8", "shared"},
          private_topic_{"pdadec8", std::string("only-") +
                                        static_cast<char>(std::tolower(
                                            static_cast<int>(static_cast<unsigned char>(tag))))} {
        ProviderConfig config;
        config.domain_id = domain_id;
        config.max_payload_bytes = payload_bound;
        // Empty document throughout: FastDdsConfig.TwoInstancesResolveTheirOwnDocuments
        // owns the per-instance-document claim and it is not re-litigated here.
        provider_ = registry.Create(ProviderSelector::Parse("fastdds"), config);
        if (provider_ == nullptr) throw std::runtime_error("\"fastdds\" did not resolve");

        provider_->CreateTopic(shared_topic_, MakeConformanceSchema(shape));
        provider_->CreateTopic(private_topic_, MakeConformanceSchema(shape));
        shared_result_ = provider_->Subscribe(
            shared_topic_,
            [this](const uint8_t* data, size_t len, const SharedSchema& schema,
                   const Attachments&) { shared_journal_.Record(data, len, schema); });
        private_result_ = provider_->Subscribe(
            private_topic_,
            [this](const uint8_t* data, size_t len, const SharedSchema& schema,
                   const Attachments&) { private_journal_.Record(data, len, schema); });
    }

    Instance(const Instance&) = delete;
    Instance& operator=(const Instance&) = delete;

    // Both subscriptions have their schema, so neither publish below can be
    // absent merely because a subscription was not live yet — the control's
    // named failure mode.
    PubSubStatus AwaitSubscriptionsLive() const {
        SharedSchema schema;
        const PubSubStatus shared = shared_result_.schema.Wait(kClauseBudget, &schema);
        if (shared != PubSubStatus::kOk) return shared;
        // A resolution that reports kOk and hands back no schema is what spec §7
        // forbids, so it is mapped onto a status of its own rather than onto the
        // one the wait returned. Written as `shared != kOk || schema == nullptr`
        // this guard returned `shared` — i.e. kOk — on exactly the state it
        // exists to catch, and read as protection while being inert.
        if (schema == nullptr) return PubSubStatus::kInternal;
        const PubSubStatus priv = private_result_.schema.Wait(kClauseBudget, &schema);
        if (priv != PubSubStatus::kOk) return priv;
        if (schema == nullptr) return PubSubStatus::kInternal;
        return PubSubStatus::kOk;
    }

    std::string SchemaWaitMessage() const {
        return shared_result_.schema.Message() + " / " + private_result_.schema.Message();
    }

    void PublishShared(uint8_t seq, size_t row_bytes = 2) {
        Publish(shared_topic_, seq, row_bytes);
    }
    void PublishPrivate(uint8_t seq, size_t row_bytes = 2) {
        Publish(private_topic_, seq, row_bytes);
    }

    std::string Mark(uint8_t seq) const { return Marker(tag_, seq); }

    const Journal& SharedJournal() const { return shared_journal_; }
    const Journal& PrivateJournal() const { return private_journal_; }

   private:
    void Publish(const Topic& topic, uint8_t seq, size_t row_bytes) {
        provider_->Publish(topic, [this, seq, row_bytes](WriteBuffer& buffer) {
            buffer.AppendByte(static_cast<uint8_t>(tag_));
            buffer.AppendByte(seq);
            buffer.AppendZeros(row_bytes - 2);
        });
    }

    char tag_;
    Topic shared_topic_;
    Topic private_topic_;
    Journal shared_journal_;
    Journal private_journal_;
    SubscriptionResult shared_result_;
    SubscriptionResult private_result_;
    // Declared last: destroyed first. See the class comment.
    std::shared_ptr<PubSubProvider> provider_;
};

// Wait until `journal` holds at least `n` entries, or the budget passes. Used
// only for the POSITIVE half of a claim — every negative assertion pays
// `kSettle` in full below rather than sampling.
bool WaitForCount(const Journal& journal, size_t n, std::chrono::milliseconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (journal.Count() < n) {
        if (std::chrono::steady_clock::now() >= deadline) return journal.Count() >= n;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

}  // namespace

// The forcing case. Two instances through one registry, one process, DIFFERENT
// domains, overlapping topic names, one bound, one shape each — and nothing
// crosses, inside the window the control below measured a real crossing in.
TEST(Registry, TwoInstancesTwoDomainsStayIsolated) {
    ProviderRegistry registry;
    RegisterFastDDSProvider(registry);

    Instance a(registry, kIsolationDomainA, kBound, SchemaId::kA, 'A');
    Instance b(registry, kIsolationDomainB, kBound, SchemaId::kB, 'B');

    ASSERT_EQ(a.AwaitSubscriptionsLive(), PubSubStatus::kOk) << a.SchemaWaitMessage();
    ASSERT_EQ(b.AwaitSubscriptionsLive(), PubSubStatus::kOk) << b.SchemaWaitMessage();

    // B publishes to the SHARED name first, so anything that was going to cross
    // had a head start; and consecutive publishes alternate instance AND topic
    // name, which is what reaches `Publish`'s one piece of cross-instance shared
    // mutable state (its `static thread_local` join scratch).
    b.PublishShared(0);
    a.PublishPrivate(1);
    b.PublishPrivate(1);
    a.PublishShared(0);

    // The positive halves first: each instance's own rows must arrive. A round
    // trip completing AFTER the foreign publish is what bounds the negative
    // assertion — nothing dead can pose as an isolated instance.
    EXPECT_TRUE(WaitForCount(a.SharedJournal(), 1, kClauseBudget))
        << "A's own row never reached A's subscription on the shared topic";
    EXPECT_TRUE(WaitForCount(b.SharedJournal(), 1, kClauseBudget))
        << "B's own row never reached B's subscription on the shared topic";

    // Then the full settle, paid in full rather than sampled: a foreign row is
    // being asserted absent, so the window has to be the one in which the
    // control observed a real crossing.
    std::this_thread::sleep_for(kSettle);

    // ONE observation per journal, and every claim about that journal derived
    // from it. Two snapshots of one journal are two different moments, so a case
    // taking several could report "the markers are exactly A's own rows" and
    // "the shapes are wrong" about two different histories, and the reader of
    // that red would have no way to tell.
    const std::vector<Journal::Entry> a_shared = a.SharedJournal().Snapshot();
    const std::vector<Journal::Entry> b_shared = b.SharedJournal().Snapshot();
    const std::vector<Journal::Entry> a_private = a.PrivateJournal().Snapshot();
    const std::vector<Journal::Entry> b_private = b.PrivateJournal().Snapshot();

    EXPECT_EQ(Markers(a_shared), (std::vector<std::string>{a.Mark(0)}))
        << "instance A's subscription on the shared topic name did not see exactly A's own row";
    EXPECT_EQ(Markers(b_shared), (std::vector<std::string>{b.Mark(0)}))
        << "instance B's subscription on the shared topic name did not see exactly B's own row";
    EXPECT_EQ(Markers(a_private), (std::vector<std::string>{a.Mark(1)}))
        << "instance A's private topic did not see exactly A's own row";
    EXPECT_EQ(Markers(b_private), (std::vector<std::string>{b.Mark(1)}))
        << "instance B's private topic did not see exactly B's own row";

    // Each subscriber received ITS OWN shape, on the topic name both declared
    // with a different one — from the same observation as the markers above.
    // Asserted as an outcome, never relied on as a separator (see the header
    // comment).
    EXPECT_EQ(Shapes(a_shared), (std::vector<int64_t>{1}))
        << "instance A was handed a schema that is not its own shape A";
    EXPECT_EQ(Shapes(b_shared), (std::vector<int64_t>{2}))
        << "instance B was handed a schema that is not its own shape B";
}

// The standing positive control, and the guard ON the forcing test. Same helper,
// same topic names, same `kBound`, same `kSettle`, ONE shape in both instances —
// differing only in that both sit on one domain. If this ever goes red the
// isolation case above has stopped meaning anything, whatever it reports.
TEST(Registry, TwoInstancesOneDomainDoInterfere) {
    ProviderRegistry registry;
    RegisterFastDDSProvider(registry);

    Instance a(registry, kControlDomain, kBound, SchemaId::kA, 'A');
    Instance b(registry, kControlDomain, kBound, SchemaId::kA, 'B');

    ASSERT_EQ(a.AwaitSubscriptionsLive(), PubSubStatus::kOk) << a.SchemaWaitMessage();
    ASSERT_EQ(b.AwaitSubscriptionsLive(), PubSubStatus::kOk) << b.SchemaWaitMessage();

    b.PublishShared(0);
    a.PublishShared(0);

    // Two rows on the shared name: A's own and B's. Waited out to the full
    // clause budget rather than to `kSettle`, and the elapsed time then measured
    // against `kSettle` separately — because the two reds need opposite
    // responses. "No crossing at all inside the budget" voids the isolation case
    // and is an ASSERT; "a real crossing, but slower than the window the
    // isolation case pays" is tuning and is an EXPECT. Waiting only to `kSettle`
    // conflated them, and reported the first when the truth was the second.
    const auto started = std::chrono::steady_clock::now();
    const bool crossed = WaitForCount(a.SharedJournal(), 2, kClauseBudget);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    // One observation, both claims derived from it: `crossed` and a separately
    // taken snapshot are two different moments, and a marginal crossing landing
    // between them made the case print "measured NO crossing" while the markers
    // it printed visibly contained the foreign row.
    const std::vector<std::string> markers = Markers(a.SharedJournal().Snapshot());
    const bool crossed_by_evidence =
        std::find(markers.begin(), markers.end(), b.Mark(0)) != markers.end();

    // The margin lives in the suite's own output, not in a plan document:
    // erosion of it toward `kSettle` is invisible unless every run reports the
    // number. Measured here it is **0 ms** — the two participants have already
    // matched by the time the publishes happen (both instances are constructed
    // and both schema waits have returned first), and Fast DDS then serves
    // same-process endpoints inline over intra-process delivery, so the foreign
    // row is in A's journal before `WaitForCount` looks. A recorded 0 is
    // therefore the healthy reading, not a missing measurement; the ~270 ms in
    // this item's plan and log is the whole CASE's runtime, construction and
    // teardown included, which is not the same quantity.
    RecordProperty("crossing_ms", static_cast<int>(elapsed.count()));

    ASSERT_TRUE(crossed || crossed_by_evidence)
        << "the control measured NO crossing on one domain AT ALL, within the full "
        << kClauseBudget.count()
        << "-second clause budget, so TwoInstancesTwoDomainsStayIsolated proves nothing: the two "
           "streams could not have met whatever the registry did";
    EXPECT_LE(elapsed, kSettle)
        << "a real crossing on one domain took at least " << elapsed.count() << " ms, outside the "
        << kSettle.count()
        << " ms window TwoInstancesTwoDomainsStayIsolated pays for its absence claim — widen "
           "kSettle (which strengthens both cases), do not narrow this control";
    EXPECT_NE(std::find(markers.begin(), markers.end(), b.Mark(0)), markers.end())
        << "instance B's row did not reach instance A on the SAME domain and the same topic name";
    EXPECT_NE(std::find(markers.begin(), markers.end(), a.Mark(0)), markers.end())
        << "instance A did not even see its own row";
}

// The same isolation claim under genuine contention: two threads, one per
// instance, `kConcurrentRows` rows each onto the identical shared topic name.
// The only case that can catch an unguarded shared map or a non-thread-local
// publish scratch by mis-delivery or by crash.
TEST(Registry, TwoInstancesStayIsolatedUnderConcurrentTraffic) {
    ProviderRegistry registry;
    RegisterFastDDSProvider(registry);

    Instance a(registry, kConcurrentDomainA, kBound, SchemaId::kA, 'A');
    Instance b(registry, kConcurrentDomainB, kBound, SchemaId::kB, 'B');

    ASSERT_EQ(a.AwaitSubscriptionsLive(), PubSubStatus::kOk) << a.SchemaWaitMessage();
    ASSERT_EQ(b.AwaitSubscriptionsLive(), PubSubStatus::kOk) << b.SchemaWaitMessage();

    // No ASSERT_/EXPECT_ inside these threads: a gtest failure in a spawned
    // thread records a failure without stopping the case, so a publish that
    // threw would be reported and then the case would carry on into a
    // comparison it can no longer interpret. The status comes back as data.
    std::string a_threw;
    std::string b_threw;
    {
        std::thread publisher_a([&] {
            try {
                for (uint8_t seq = 0; seq < kConcurrentRows; ++seq) a.PublishShared(seq);
            } catch (const std::exception& e) {
                a_threw = e.what();
            }
        });
        std::thread publisher_b([&] {
            try {
                for (uint8_t seq = 0; seq < kConcurrentRows; ++seq) b.PublishShared(seq);
            } catch (const std::exception& e) {
                b_threw = e.what();
            }
        });
        // JOINED BEFORE EITHER PROVIDER IS DESTROYED — a call in flight through
        // a dying provider is a use-after-free, and it is the one path by which
        // this case's own code could kill the process (spec §6 clause 5).
        publisher_a.join();
        publisher_b.join();
    }
    ASSERT_TRUE(a_threw.empty()) << "instance A's publisher thread threw: " << a_threw;
    ASSERT_TRUE(b_threw.empty()) << "instance B's publisher thread threw: " << b_threw;

    EXPECT_TRUE(WaitForCount(a.SharedJournal(), kConcurrentRows, kClauseBudget))
        << "instance A received " << a.SharedJournal().Count() << " of its own "
        << static_cast<int>(kConcurrentRows) << " rows";
    EXPECT_TRUE(WaitForCount(b.SharedJournal(), kConcurrentRows, kClauseBudget))
        << "instance B received " << b.SharedJournal().Count() << " of its own "
        << static_cast<int>(kConcurrentRows) << " rows";
    std::this_thread::sleep_for(kSettle);

    std::vector<std::string> expected_a;
    std::vector<std::string> expected_b;
    for (uint8_t seq = 0; seq < kConcurrentRows; ++seq) {
        expected_a.push_back(a.Mark(seq));
        expected_b.push_back(b.Mark(seq));
    }
    // One observation per journal, both claims about it derived from that one
    // (see the isolation case above).
    const std::vector<Journal::Entry> a_shared = a.SharedJournal().Snapshot();
    const std::vector<Journal::Entry> b_shared = b.SharedJournal().Snapshot();

    // Whole, and in order: one writer per reader, RELIABLE + KEEP_ALL, so a
    // reordering would itself be a contract violation rather than noise.
    EXPECT_EQ(Markers(a_shared), expected_a)
        << "instance A's shared-topic journal is not exactly A's own rows, in order";
    EXPECT_EQ(Markers(b_shared), expected_b)
        << "instance B's shared-topic journal is not exactly B's own rows, in order";
    EXPECT_EQ(Markers(a.PrivateJournal().Snapshot()), (std::vector<std::string>{}))
        << "instance A's private topic received a row nobody published to it";
    EXPECT_EQ(Markers(b.PrivateJournal().Snapshot()), (std::vector<std::string>{}))
        << "instance B's private topic received a row nobody published to it";
    EXPECT_EQ(Shapes(a_shared), (std::vector<int64_t>{1}))
        << "instance A was handed a schema that is not its own shape A";
    EXPECT_EQ(Shapes(b_shared), (std::vector<int64_t>{2}))
        << "instance B was handed a schema that is not its own shape B";
}

// The second axis of "different configs": each instance honours ITS OWN payload
// bound. Its own pair of domains, and each instance publishes only on its own
// PRIVATE topic to its own subscription — so the unequal bounds, which are an
// independent reason two endpoints never discover each other, confound nothing.
// This pair MAKES NO CROSSING CLAIM in either direction — the bound is part of
// the registered DDS type name, so it could not cross regardless. What it claims
// is that a row over one instance's bound is dropped there and delivered on the
// other.
//
// The middle row is dropped SILENTLY on the low-bound instance and does not
// throw: the overflow is caught inside `serialize()`, which zeroes the payload
// length, so the sample never enters history, `write()` returns non-OK and
// `SampleWriter` only logs it. That is pre-existing behaviour of the serialising
// publish flow (the one an empty document selects), pinned by
// `FastDDSPubSubProviderTest.DataSharingOversizedRowDoesNotThrow`; a typed
// `kPayloadTooLarge` exists only on the loaned flow, which both instances would
// need a `fletcher.loan_publish=true` document to select. So delivery is what is
// asserted here, in both directions, and no new timing number is introduced: the
// third row goes AFTER the oversized one and must arrive, so nothing dead can
// pose as a working instance.
TEST(Registry, TwoInstancesKeepTheirOwnPayloadBounds) {
    ProviderRegistry registry;
    RegisterFastDDSProvider(registry);

    Instance low(registry, kLowBoundDomain, kLowBound, SchemaId::kA, 'A');
    Instance high(registry, kHighBoundDomain, kHighBound, SchemaId::kA, 'B');

    ASSERT_EQ(low.AwaitSubscriptionsLive(), PubSubStatus::kOk) << low.SchemaWaitMessage();
    ASSERT_EQ(high.AwaitSubscriptionsLive(), PubSubStatus::kOk) << high.SchemaWaitMessage();

    for (Instance* instance : {&low, &high}) {
        instance->PublishPrivate(0);
        instance->PublishPrivate(1, kBetweenBoundsRowBytes);
        instance->PublishPrivate(2);
    }

    EXPECT_TRUE(WaitForCount(high.PrivateJournal(), 3, kClauseBudget))
        << "the high-bound instance received " << high.PrivateJournal().Count() << " of 3 rows";
    EXPECT_TRUE(WaitForCount(low.PrivateJournal(), 2, kClauseBudget))
        << "the low-bound instance received " << low.PrivateJournal().Count()
        << " of the 2 rows inside its bound";
    std::this_thread::sleep_for(kSettle);

    EXPECT_EQ(Markers(high.PrivateJournal().Snapshot()),
              (std::vector<std::string>{high.Mark(0), high.Mark(1), high.Mark(2)}))
        << "the 65536-byte-bound instance did not receive all three rows, so it is not honouring "
           "its own bound";
    EXPECT_EQ(Markers(low.PrivateJournal().Snapshot()),
              (std::vector<std::string>{low.Mark(0), low.Mark(2)}))
        << "the 4096-byte-bound instance's journal is not exactly the two rows inside its bound";
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
