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
/// transport that passes null throughout, and the same transport may later be
/// exercised in both modes (PDA-DEC-3 adds a schema-carrying loopback subject).
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
/// for a provider refusal — which is all a clause may assert about it, because
/// the seam has no exception taxonomy yet (PDA-DEC-3/9 invents one).
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
};

/// "<type>: <what>" for `e`. Deliberately not a type a clause can switch on:
/// the seam has no exception taxonomy yet, so a clause asserts THAT the
/// provider refused, never which way.
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
