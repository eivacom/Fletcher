// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include <arrow/api.h>

#include <cassert>
#include <fletcher/pubsub/provider.hpp>
#include <fletcher/pubsub_arrow/publisher_arrow.hpp>
#include <fletcher/pubsub_arrow/subscriber_arrow.hpp>
#include <memory>

using namespace fletcher;

namespace {

class StubProvider : public PubSubProvider {
   public:
    void CreateTopic(const std::vector<std::string>& /*segments*/,
                     OwnedSchema /*schema*/) override {}

    void Publish(const std::vector<std::string>& /*segments*/, RowEncoder /*encoder*/,
                 const Attachments& /*attachments*/) override {}

    SubscriptionResult Subscribe(const std::vector<std::string>& /*segments*/,
                                 SubscribeCallback /*callback*/) override {
        return {};
    }

    void Unsubscribe(const std::vector<std::string>& /*segments*/) override {}
};

}  // namespace

int main() {
    auto provider = std::make_shared<StubProvider>();
    PublisherArrow pub(provider);
    SubscriberArrow sub(provider);

    std::shared_ptr<arrow::Schema> schema = arrow::schema({
        arrow::field("x", arrow::int32()),
    });

    pub.CreateTopic({"hello", "world"}, schema);
    assert(pub.HasTopic({"hello", "world"}));
    assert(pub.ListTopics().size() == 1);

    return 0;
}
