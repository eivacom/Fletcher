// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The cross-process subject: the publisher side is a child process, the
// subscriber side is a provider instance in THIS process. There is no
// "subscribe" verb in the peer protocol — the peer publishes and cannot
// observe, and no clause needs it to, which is also what stops a clause
// quietly turning a cross-process subject into an in-process one.

#include <chrono>
#include <memory>
#include <string>
#include <utility>

#include "child_process.hpp"
#include "fletcher/conformance/fixtures.hpp"
#include "fletcher/conformance/subject.hpp"
#include "fletcher/pubsub/internal/segments.hpp"

namespace fletcher {
namespace conformance {

namespace {

/// Budget for one request/reply exchange, and for the child's initial READY.
/// A DDS participant plus (for XRCE) an Agent session handshake happens before
/// READY, so the startup budget is the larger of the two.
constexpr std::chrono::seconds kPeerRequestBudget{15};
constexpr std::chrono::seconds kPeerStartupBudget{45};

class PeerSubject : public ProviderSubject {
   public:
    PeerSubject(ProviderTraits traits, std::shared_ptr<PubSubProvider> subscriber_provider,
                const std::string& peer_exe, const std::vector<std::string>& peer_args)
        : traits_(std::move(traits)), provider_(std::move(subscriber_provider)) {
        child_ = std::make_unique<ChildProcess>(peer_exe, peer_args);
        std::optional<std::string> ready =
            child_->ReadLine(std::chrono::steady_clock::now() + kPeerStartupBudget);
        if (!ready.has_value() || *ready != "READY") {
            throw std::runtime_error(
                "conformance: peer " + peer_exe + " did not print READY (got: " +
                (ready.has_value() ? *ready : std::string("<eof/timeout>")) + ")");
        }
    }

    const ProviderTraits& Traits() const override { return traits_; }

    std::optional<std::string> DeclareTopic(const Topic& topic, SchemaId schema) override {
        const char* which = schema == SchemaId::kA ? "A" : (schema == SchemaId::kB ? "B" : "none");
        return Exchange("create " + internal::JoinSegments(topic) + " " + which);
    }

    std::optional<std::string> PublishRow(const Topic& topic, uint32_t seq) override {
        return Exchange("publish " + internal::JoinSegments(topic) + " " + std::to_string(seq));
    }

    SubscriptionResult Subscribe(const Topic& topic, SubscribeCallback callback) override {
        return provider_->Subscribe(topic, std::move(callback));
    }

    void Unsubscribe(const Topic& topic) override { provider_->Unsubscribe(topic); }

   private:
    /// One line out, one line back. A deadline expiry, an EOF or an unparseable
    /// reply are all failures of the request — never a retry and never a
    /// silently-degraded mode.
    std::optional<std::string> Exchange(const std::string& request) {
        std::optional<std::string> reply =
            child_->Request(request, std::chrono::steady_clock::now() + kPeerRequestBudget);
        if (!reply.has_value()) {
            return std::string("peer: no reply within budget for: ") + request;
        }
        if (*reply == "ok") {
            return std::nullopt;
        }
        if (reply->rfind("err ", 0) == 0) {
            return reply->substr(4);
        }
        return std::string("peer: unparseable reply '") + *reply + "' to: " + request;
    }

    ProviderTraits traits_;
    std::shared_ptr<PubSubProvider> provider_;
    std::unique_ptr<ChildProcess> child_;
};

}  // namespace

SubjectFactory MakePeerSubjectFactory(std::string label, std::string provider_name,
                                      SchemaMode schema_mode,
                                      std::function<std::shared_ptr<PubSubProvider>()> make,
                                      std::string peer_exe, std::vector<std::string> peer_args) {
    ProviderTraits traits = MakeTraits(std::move(provider_name), schema_mode);
    return SubjectFactory{
        std::move(label),
        [traits, make, peer_exe, peer_args]() -> std::unique_ptr<ProviderSubject> {
            return std::make_unique<PeerSubject>(traits, make(), peer_exe, peer_args);
        }};
}

}  // namespace conformance
}  // namespace fletcher
