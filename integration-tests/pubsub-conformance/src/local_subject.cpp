// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The in-process subject: the publisher side calls CreateTopic/Publish on the
// same provider instance the subscriber side subscribed on. Provider-agnostic —
// it holds a PubSubProvider, so this TU links no provider and neither does the
// clause library it belongs to.

#include <fletcher/core/status.hpp>
#include <fletcher/core/write_buffer.hpp>
#include <fletcher/pubsub/owned_schema.hpp>
#include <memory>
#include <string>
#include <typeinfo>
#include <utility>

#include "fletcher/conformance/fixtures.hpp"
#include "fletcher/conformance/subject.hpp"

namespace fletcher {
namespace conformance {

std::string DescribeException(const std::exception& e) {
    std::string out = std::string(typeid(e).name()) + ": " + e.what();
    // The seam's numbered cause, carried into the string so a cross-process
    // subject can send back exactly what a local one saw (spec §5.1).
    if (const auto* typed = dynamic_cast<const PubSubError*>(&e)) {
        out += " [status=";
        out += PubSubStatusName(typed->status());
        out += "]";
    }
    return out;
}

namespace {

class LocalSubject : public ProviderSubject {
   public:
    LocalSubject(ProviderTraits traits, std::shared_ptr<PubSubProvider> provider)
        : traits_(std::move(traits)), provider_(std::move(provider)) {}

    const ProviderTraits& Traits() const override { return traits_; }

    Reply DeclareTopic(const Topic& topic, SchemaId schema) override {
        // Building the schema is OUR work, not the provider's, so it is done
        // outside the try and a failure here is a kHarnessFailure. Inside the
        // try it would have been reported as kRefusedByProvider, and clause 8 —
        // which asserts refused() precisely so that a broken harness cannot
        // satisfy it — would have passed because nanoarrow failed on our side.
        // That is the exact false-pass class the three-valued Reply exists to
        // eliminate, so it must not be reachable through the schema builder
        // either.
        OwnedSchema built;
        try {
            built = MakeConformanceSchema(schema);
        } catch (const std::exception& e) {
            return Reply::HarnessFailure("conformance: cannot build schema: " +
                                         DescribeException(e));
        }

        // Past this point every failure IS the provider refusing: there is no
        // pipe, no child and no deadline between here and CreateTopic.
        try {
            provider_->CreateTopic(topic, std::move(built));
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
