// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The in-process subject: the publisher side calls CreateTopic/Publish on the
// same provider instance the subscriber side subscribed on. Provider-agnostic —
// it holds a PubSubProvider, so this TU links no provider and neither does the
// clause library it belongs to.

#include <fletcher/core/write_buffer.hpp>
#include <memory>
#include <string>
#include <typeinfo>
#include <utility>

#include "fletcher/conformance/fixtures.hpp"
#include "fletcher/conformance/subject.hpp"

namespace fletcher {
namespace conformance {

std::string DescribeException(const std::exception& e) {
    return std::string(typeid(e).name()) + ": " + e.what();
}

namespace {

class LocalSubject : public ProviderSubject {
   public:
    LocalSubject(ProviderTraits traits, std::shared_ptr<PubSubProvider> provider)
        : traits_(std::move(traits)), provider_(std::move(provider)) {}

    const ProviderTraits& Traits() const override { return traits_; }

    // On a local subject every failure IS the provider refusing: there is no
    // pipe, no child and no deadline between the clause and CreateTopic, so
    // Outcome::kHarnessFailure is unreachable here by construction.
    Reply DeclareTopic(const Topic& topic, SchemaId schema) override {
        try {
            provider_->CreateTopic(topic, MakeConformanceSchema(schema));
            return Reply::Ok();
        } catch (const std::exception& e) {
            return Reply::Refused(DescribeException(e));
        } catch (...) {
            return Reply::Refused("unknown exception: CreateTopic");
        }
    }

    Reply PublishRow(const Topic& topic, uint32_t seq) override {
        try {
            provider_->Publish(topic, [seq](WriteBuffer& buf) { EncodeRow(buf, seq); });
            return Reply::Ok();
        } catch (const std::exception& e) {
            return Reply::Refused(DescribeException(e));
        } catch (...) {
            return Reply::Refused("unknown exception: Publish");
        }
    }

    SubscriptionResult Subscribe(const Topic& topic, SubscribeCallback callback) override {
        return provider_->Subscribe(topic, std::move(callback));
    }

    void Unsubscribe(const Topic& topic) override { provider_->Unsubscribe(topic); }

   private:
    ProviderTraits traits_;
    std::shared_ptr<PubSubProvider> provider_;
};

}  // namespace

SubjectFactory MakeLocalSubjectFactory(std::string label, std::string provider_name,
                                       SchemaMode schema_mode,
                                       std::function<std::shared_ptr<PubSubProvider>()> make) {
    // Traits are composed INSIDE the lambda, not here: this function runs during
    // static initialisation (INSTANTIATE_TEST_SUITE_P), where
    // RetentionForProvider's throw-on-unknown-provider would be a
    // std::terminate before main instead of the readable message it composes.
    return SubjectFactory{
        std::move(label), [provider_name, schema_mode, make]() -> std::unique_ptr<ProviderSubject> {
            return std::make_unique<LocalSubject>(MakeTraits(provider_name, schema_mode), make());
        }};
}

}  // namespace conformance
}  // namespace fletcher
