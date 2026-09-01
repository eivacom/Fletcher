// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The gtest fixture the clauses are written against, the RAII subscription that
// makes the collector-lifetime bug unrepresentable, and the two macros that keep
// a failed setup step from being read as a contract violation.
//
// Split from fixtures.hpp so the peer children — which need the schemas and the
// row codec — link no test framework.

#ifndef FLETCHER_CONFORMANCE_SUITE_HPP_
#define FLETCHER_CONFORMANCE_SUITE_HPP_

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "fletcher/conformance/fixtures.hpp"
#include "fletcher/conformance/subject.hpp"

namespace fletcher {
namespace conformance {

/// A subscription that ends when it goes out of scope.
///
/// Every clause declares its `Collector` on the test-body stack, and the
/// provider outlives the test body (the fixture destroys it in TearDown). A
/// subscription that is still live when the collector's storage goes away means
/// a background DDS/XRCE delivery thread calls `Collector::Record` on a
/// destroyed stack frame: locking a destroyed mutex, appending to a destroyed
/// vector.
///
/// Relying on a trailing `Subject().Unsubscribe(topic)` in each clause does not
/// close that: every `ASSERT_`/`CONF_MUST_` failure path returns from the test
/// body without reaching it, so the bug fires exactly when a clause fails —
/// when a readable diagnosis matters most — and takes the rest of the binary's
/// clauses with it.
///
/// **Declare this AFTER the `Collector` it feeds.** Destruction runs in reverse
/// declaration order, so the subscription is torn down first and the collector
/// is guaranteed to outlive every callback that can reach it. That ordering is
/// the whole contract of this type; nothing else in the suite depends on a
/// clause remembering to unsubscribe.
class ScopedSubscription {
   public:
    ScopedSubscription(ProviderSubject& subject, Topic topic, SubscribeCallback callback)
        : subject_(subject), topic_(std::move(topic)) {
        result_ = subject_.Subscribe(topic_, std::move(callback));
    }

    ~ScopedSubscription() {
        // A destructor must not throw, and a provider's Unsubscribe may. There
        // is nothing to recover here: the clause has already made its
        // assertions, and the only job left is to stop deliveries.
        try {
            subject_.Unsubscribe(topic_);
        } catch (...) {
        }
    }

    ScopedSubscription(const ScopedSubscription&) = delete;
    ScopedSubscription& operator=(const ScopedSubscription&) = delete;

    /// The subscription's schema arrival (§7 clause 5).
    const SchemaArrival& Schema() const { return result_.schema; }

   private:
    ProviderSubject& subject_;
    Topic topic_;
    SubscriptionResult result_;
};

/// The suite is named literally `ProviderConformance` and value-parameterised
/// over subject factories, so a clause's gtest name reads
/// `<Subject>/ProviderConformance.<Clause>/0` (and its ctest name reads
/// `.../<Clause>/<Subject>`, because gtest_discover_tests substitutes the
/// printed parameter for the index). Either way
/// `ctest -R 'ProviderConformance\.<Clause>'` scopes to one clause across every
/// subject. A clause failing on one subject and passing on another IS the
/// divergence report; no ledger is kept.
class ProviderConformance : public ::testing::TestWithParam<SubjectFactory> {
   protected:
    void SetUp() override {
        subject_ = GetParam()();
        ASSERT_NE(subject_, nullptr);
    }

    void TearDown() override { subject_.reset(); }

    ProviderSubject& Subject() { return *subject_; }
    const ProviderTraits& Traits() const { return subject_->Traits(); }
    bool Carried() const { return Traits().schema_mode == SchemaMode::kCarried; }
    bool Retains() const { return Traits().retention == Retention::kRetainsPreSubscribe; }

    Topic Fresh(const std::string& clause) { return FreshTopic(clause); }

    /// The schema a DELIVERY clause declares its topic with. A schema-less
    /// subject declares no schema at all — otherwise the harness would hand the
    /// transport a schema and then assert it does not carry one, which is a
    /// contradiction rather than a test (§7 clause 1 sanctions the schema-less
    /// mode; it does not sanction being pushed into it).
    ///
    /// Clauses 7 and 8 do NOT use this: they observe only the REPLY to a
    /// declaration, and whether a transport later carries a schema is
    /// independent of whether a declaration is accepted. They declare the real
    /// A and B on every subject.
    SchemaId DataSchema() const { return Carried() ? SchemaId::kA : SchemaId::kNone; }

    /// The one deadline every wait in this clause shares.
    ///
    /// Anchored at the FIRST wait, not at SetUp. On a cross-process subject the
    /// synchronous pipe round-trips before the first wait would otherwise spend
    /// the budget — `PerWriterOrderIsMonotonic` makes 21 of them before its only
    /// wait — so on a loaded runner the wait could begin with almost none left
    /// and fail on harness throughput instead of on delivery behaviour. The
    /// design's property is preserved exactly: still ONE deadline per clause, so
    /// a clause cannot pass by spending N budgets.
    std::chrono::steady_clock::time_point Deadline() {
        if (!deadline_.has_value()) {
            deadline_ = std::chrono::steady_clock::now() + kClauseBudget;
        }
        return *deadline_;
    }

    /// What is left of `Deadline()`, as the duration a SchemaArrival wait takes.
    /// Never negative — a budget already spent polls rather than being refused,
    /// which is what the deadline form did too.
    std::chrono::milliseconds RemainingBudget() {
        const auto left = Deadline() - std::chrono::steady_clock::now();
        if (left <= std::chrono::steady_clock::duration::zero()) {
            return std::chrono::milliseconds::zero();
        }
        return std::chrono::duration_cast<std::chrono::milliseconds>(left);
    }

    /// Deadline for a wait that is EXPECTED to time out. Separate from
    /// `Deadline()` because it is a cost paid in full, not a ceiling.
    std::chrono::steady_clock::time_point SettleDeadline() const {
        return std::chrono::steady_clock::now() + kSettleBudget;
    }

   private:
    std::unique_ptr<ProviderSubject> subject_;
    std::optional<std::chrono::steady_clock::time_point> deadline_;
};

/// Declare / publish, failing the clause immediately on an unexpected error
/// rather than letting it read a missing row as a contract violation. Macros,
/// not helpers, so ASSERT_ aborts the CLAUSE rather than a helper frame.
#define CONF_MUST_DECLARE(topic, schema)                                                   \
    do {                                                                                   \
        ::fletcher::conformance::Reply conf_reply = Subject().DeclareTopic(topic, schema); \
        ASSERT_TRUE(conf_reply.ok()) << "DeclareTopic failed: " << conf_reply.detail;      \
    } while (false)

#define CONF_MUST_PUBLISH(topic, seq)                                                 \
    do {                                                                              \
        ::fletcher::conformance::Reply conf_reply = Subject().PublishRow(topic, seq); \
        ASSERT_TRUE(conf_reply.ok())                                                  \
            << "PublishRow(" << (seq) << ") failed: " << conf_reply.detail;           \
    } while (false)

}  // namespace conformance
}  // namespace fletcher

#endif  // FLETCHER_CONFORMANCE_SUITE_HPP_
