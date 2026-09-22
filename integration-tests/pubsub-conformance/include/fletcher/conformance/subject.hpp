// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The subject abstraction: what a clause body of the conformance suite is
// allowed to see. Deliberately NOT a PubSubProvider& — a clause that could
// reach the provider could publish locally on a subject whose publisher lives
// in another process, and the whole point of the cross-process subjects is that
// they do not. Making that unrepresentable beats reviewing for it.

#ifndef FLETCHER_CONFORMANCE_SUBJECT_HPP_
#define FLETCHER_CONFORMANCE_SUBJECT_HPP_

#include <chrono>
#include <cstdint>
#include <exception>
#include <fletcher/pubsub/provider.hpp>
#include <functional>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

namespace fletcher {
namespace conformance {

/// A topic, in the seam's own vocabulary: a vector of segments.
using Topic = std::vector<std::string>;

/// The schemas a declaration may carry. `kA` is `struct<seq:int32>`, `kB` is
/// `struct<seq:int32,extra:float64>` — provably different shapes, so a
/// re-declaration from one to the other is a conflict no comparison can excuse.
/// `kNone` declares a topic with no schema at all.
enum class SchemaId { kNone, kA, kB };

/// Does this subject's transport carry schemas of its own?
///
/// A *usage* axis, chosen per subject: spec §7 clause 1 explicitly sanctions a
/// transport that passes null throughout, and the same transport is exercised in
/// both modes — the loopback registers `InProcessLocal` (kAbsent) and
/// `InProcessCarrying` (kCarried), one provider, two usages, same clauses.
enum class SchemaMode { kCarried, kAbsent };

/// Does this transport replay rows published before a subscriber existed?
///
/// Keyed by PROVIDER, never by subject — see RetentionForProvider.
enum class Retention { kRetainsPreSubscribe, kDropsPreSubscribe };

struct ProviderTraits {
    /// Provider name, e.g. "inprocess", "fastdds", "xrce". Identical for a
    /// provider's in-process and cross-process subjects.
    std::string provider;
    SchemaMode schema_mode;
    Retention retention;
};

/// The three outcomes of a DeclareTopic / PublishRow call.
///
/// Three, not two, and that is the whole point: a clause that asserts a
/// REFUSAL must not be satisfiable by a HARNESS failure. Clause 8 asserts that
/// a conflicting re-declaration is refused, and it is the clause the XRCE
/// CreateTopic fix exists to satisfy — if a dead peer, an expired deadline or a
/// garbled reply could satisfy it, the clause would pass exactly when the
/// harness broke, which is worse than having no clause.
enum class Outcome {
    kOk,
    /// The provider under test refused the call — an exception on a local
    /// subject, an `err` reply from the peer on a cross-process one.
    kRefusedByProvider,
    /// The HARNESS failed: no reply within budget, EOF from a dead child, a
    /// reply that could not be parsed. Never evidence about the provider.
    kHarnessFailure,
};

/// The reply to a DeclareTopic / PublishRow call. `detail` is "<type>: <what>"
/// for a provider refusal. The seam now HAS an exception taxonomy — every
/// provider throws PubSubError carrying a stable PubSubStatus (spec §5.1) — but
/// the clauses deliberately still assert only THAT a call was refused: a
/// cross-process subject can only carry a string back over its pipe, so asserting
/// on the status here would make the local and remote subjects test different
/// things. Pinning the numbers is `SeamVocabulary`s job.
struct Reply {
    Outcome outcome = Outcome::kOk;
    std::string detail;

    [[nodiscard]] bool ok() const { return outcome == Outcome::kOk; }
    [[nodiscard]] bool refused() const { return outcome == Outcome::kRefusedByProvider; }

    static Reply Ok() { return Reply{Outcome::kOk, {}}; }
    static Reply Refused(std::string why) {
        return Reply{Outcome::kRefusedByProvider, std::move(why)};
    }
    static Reply HarnessFailure(std::string why) {
        return Reply{Outcome::kHarnessFailure, std::move(why)};
    }
};

/// Retention for `provider`, from a table keyed by provider name.
///
/// Why a table and not a field a subject fills in: clause 6 asserts
/// all-or-nothing against this value, so a subject able to declare its own
/// retention could declare its way to green — locked decision 11's forbidden
/// "pinned divergence" wearing a trait. Keying by provider means a provider's
/// cross-process subject inherits whatever its in-process subject claims.
/// Throws for an unknown provider: a new subject must state its retention here,
/// where both of its subjects see the same answer. Never called during static
/// initialisation — the subject factories build their traits inside the factory
/// lambda, so an unknown provider surfaces as a readable test failure rather
/// than a std::terminate before main.
Retention RetentionForProvider(const std::string& provider);

/// Compose traits. Retention comes from the table; only schema_mode is the
/// subject's to choose.
ProviderTraits MakeTraits(std::string provider, SchemaMode schema_mode);

/// The seam's own callback type — the suite asserts against the real thing.
using SubscribeCallback = PubSubProvider::SubscribeCallback;

/// One provider, exercised one way. The publisher side may be another process;
/// the subscriber side is always this process and always this instance.
class ProviderSubject {
   public:
    virtual ~ProviderSubject() = default;

    virtual const ProviderTraits& Traits() const = 0;

    /// Declare `topic` with `schema`. See Reply: a clause asserting a refusal
    /// must test `refused()`, never merely "not ok".
    virtual Reply DeclareTopic(const Topic& topic, SchemaId schema) = 0;

    /// Publish one row carrying `seq`. Same reply convention as DeclareTopic.
    /// Safe to call concurrently from several threads; a cross-process subject
    /// serialises them onto its single request/reply pipe.
    virtual Reply PublishRow(const Topic& topic, uint32_t seq) = 0;

    /// Subscribe on this process's instance. May throw — a clause that expects
    /// a second subscription to be refused catches it itself.
    [[nodiscard]] virtual SubscriptionResult Subscribe(const Topic& topic,
                                                       SubscribeCallback callback) = 0;

    virtual void Unsubscribe(const Topic& topic) = 0;

    /// Schema-only subscribe/unsubscribe. LOCAL-ONLY on every provider
    /// (the class doc above already says the subscriber side is always this
    /// process and this instance, for every subject), so there is no peer-pipe
    /// protocol for them to need — each subject forwards straight to its own
    /// provider, exactly as it does for `Subscribe`/`Unsubscribe` above.
    [[nodiscard]] virtual SchemaArrival SubscribeSchema(const Topic& topic) = 0;

    virtual void UnsubscribeSchema(const Topic& topic) = 0;

    /// Options-carrying declare/subscribe. LOCAL-ONLY on every provider, for the same reason
    /// `SubscribeSchema`/`UnsubscribeSchema` are: the class doc above already says the
    /// subscriber side is always this process and this instance, for every subject — these verbs
    /// are local-only; there is no peer-pipe form. Each subject forwards straight to its own
    /// provider's `CreateTopicWithOptions` / `SubscribeWithOptions`, exactly as it does for the
    /// schema-only pair. `schema` is taken directly rather than as a `SchemaId`, unlike
    /// `DeclareTopic`, because there is no peer wire form to build one for.
    virtual void DeclareTopicWithOptions(const Topic& topic, OwnedSchema schema,
                                         const TopicOptions& options) = 0;

    [[nodiscard]] virtual SubscriptionResult SubscribeWithOptions(const Topic& topic,
                                                                  SubscribeCallback callback,
                                                                  const TopicOptions& options) = 0;

    /// Optional readiness hook. A clause calls this after `Subscribe` (and
    /// after whichever of `Subscribe` / `DeclareTopic` runs second, since a
    /// reader cannot match a writer that does not exist yet) and before its
    /// first `PublishRow`, so the row is never published into a reader that
    /// has not matched a writer. Bounded by `budget`; never sleeps.
    ///
    /// The default returns immediately — matched at `Subscribe`, which is true
    /// for every subject but the Fast DDS ones: the loopback has no discovery,
    /// and XRCE's agent-side QoS did not change (still retains). Only the Fast
    /// DDS local and cross-process subjects override this, over a
    /// `FastDDSStatusListener` recording `OnMatched` for the data (non-schema)
    /// reader endpoint.
    ///
    /// Not part of the seam itself: `PubSubProvider` carries no such signal
    /// (spec §7) — an application learns of a match, if it needs to, from a
    /// provider-specific status callback. This hook is a test-harness
    /// convenience over that, never called by a clause that deliberately
    /// publishes BEFORE subscribing (a late-joiner / retention clause).
    virtual void AwaitDataMatched(const Topic& /*topic*/, std::chrono::milliseconds /*budget*/) {}
};

/// "<type>: <what>" for `e`, with the numbered status appended when `e` is a
/// PubSubError — which, after PDA-DEC-3, it always is when it came from a
/// provider. Still not a type a clause switches on (see Reply): it is a
/// diagnostic string, so a cross-process subject can carry the same information
/// back over its pipe that a local one has in hand.
std::string DescribeException(const std::exception& e);

/// Built fresh for every clause, so no clause inherits another's topics,
/// readers or child process.
///
/// A struct rather than a bare std::function because it is a gtest test
/// PARAMETER: gtest prints the parameter into the test's name, and printing a
/// std::function falls back to a raw byte dump — which contains uninitialised
/// stack bytes, so the ctest test names would change from run to run. The label
/// makes them stable and readable.
struct SubjectFactory {
    std::string label;
    std::function<std::unique_ptr<ProviderSubject>()> make;

    /// True when `DeclareTopic` and `PublishRow` reach the SAME provider instance
    /// the subscriber side delivers from — i.e. a local subject. False on a peer
    /// subject, where those two go over the pipe to a child process and the
    /// provider under test never sees them.
    ///
    /// Only §6 clause 6's method-axis control needs this, and it needs it to stay
    /// honest rather than to branch on behaviour: a re-entrancy clause asserting
    /// that a call is REFUSED must not assert it where the call was never
    /// re-entrant in the first place. `Subscribe` and `Unsubscribe` are direct on
    /// every subject and need no such qualification.
    bool publishes_into_subject_instance = true;

    std::unique_ptr<ProviderSubject> operator()() const { return make(); }
};

inline void PrintTo(const SubjectFactory& factory, std::ostream* os) { *os << factory.label; }

/// Builds a subject whose publisher side calls the same provider instance the
/// subscriber side uses. `provider_name` keys the retention table.
SubjectFactory MakeLocalSubjectFactory(std::string label, std::string provider_name,
                                       SchemaMode schema_mode,
                                       std::function<std::shared_ptr<PubSubProvider>()> make);

/// Builds a subject whose publisher side is a child process at `peer_exe`,
/// driven over a request/reply pipe. `peer_args` are appended to its command
/// line (a domain id, an agent port, a session key).
SubjectFactory MakePeerSubjectFactory(std::string label, std::string provider_name,
                                      SchemaMode schema_mode,
                                      std::function<std::shared_ptr<PubSubProvider>()> make,
                                      std::string peer_exe, std::vector<std::string> peer_args);

}  // namespace conformance
}  // namespace fletcher

#endif  // FLETCHER_CONFORMANCE_SUBJECT_HPP_
