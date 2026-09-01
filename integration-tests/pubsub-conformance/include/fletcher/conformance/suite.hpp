// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The gtest fixture the clauses are written against, and the two macros that
// keep a failed setup step from being read as a contract violation.
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

#include "fletcher/conformance/fixtures.hpp"
#include "fletcher/conformance/subject.hpp"

namespace fletcher {
namespace conformance {

/// The suite is named literally `ProviderConformance` and value-parameterised
/// over subject factories, so a clause's full name reads
/// `<Subject>/ProviderConformance.<Clause>/0` and
/// `ctest -R 'ProviderConformance\.<Clause>'` scopes to one clause across every
/// subject. A clause failing on one subject and passing on another IS the
/// divergence report; no ledger is kept.
class ProviderConformance : public ::testing::TestWithParam<SubjectFactory> {
   protected:
    void SetUp() override {
        subject_ = GetParam()();
        ASSERT_NE(subject_, nullptr);
        deadline_ = std::chrono::steady_clock::now() + kClauseBudget;
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
    std::chrono::steady_clock::time_point Deadline() const { return deadline_; }
    std::chrono::steady_clock::time_point SettleDeadline() const {
        return std::chrono::steady_clock::now() + kSettleBudget;
    }

   private:
    std::unique_ptr<ProviderSubject> subject_;
    std::chrono::steady_clock::time_point deadline_;
};

/// Declare / publish, failing the clause immediately on an unexpected error
/// rather than letting it read a missing row as a contract violation. Macros,
/// not helpers, so ASSERT_ aborts the CLAUSE rather than a helper frame.
#define CONF_MUST_DECLARE(topic, schema)                                             \
    do {                                                                             \
        std::optional<std::string> conf_err = Subject().DeclareTopic(topic, schema); \
        ASSERT_FALSE(conf_err.has_value()) << "DeclareTopic failed: " << *conf_err;  \
    } while (false)

#define CONF_MUST_PUBLISH(topic, seq)                                                              \
    do {                                                                                           \
        std::optional<std::string> conf_err = Subject().PublishRow(topic, seq);                    \
        ASSERT_FALSE(conf_err.has_value()) << "PublishRow(" << (seq) << ") failed: " << *conf_err; \
    } while (false)

}  // namespace conformance
}  // namespace fletcher

#endif  // FLETCHER_CONFORMANCE_SUITE_HPP_
