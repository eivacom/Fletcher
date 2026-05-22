// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include <arrow/api.h>

#include <cassert>
#include <fletcher/pubsub/pubsub.hpp>
#include <fletcher/pubsub_arrow/pubsub_arrow.hpp>
#include <memory>

using namespace fletcher;

namespace {

class StubProvider : public PubSub {
   public:
    void CreateTopic(const std::vector<std::string>& /*segments*/, OwnedSchema /*schema*/,
                     std::any /*config*/) override {}

    void Publish(const std::vector<std::string>& /*segments*/, RowEncoder /*encoder*/,
                 const Attachments& /*attachments*/) override {}

    SubscriptionResult Subscribe(const std::vector<std::string>& /*segments*/,
                                 SubscribeCallback /*callback*/, std::any /*config*/) override {
        return {};
    }

    void Unsubscribe(const std::vector<std::string>& /*segments*/) override {}
};

}  // namespace

int main() {
    PubSubArrow ps(std::make_shared<StubProvider>());

    std::shared_ptr<arrow::Schema> schema = arrow::schema({
        arrow::field("x", arrow::int32()),
    });

    ps.CreateTopic({"hello", "world"}, schema);
    assert(ps.HasTopic({"hello", "world"}));
    assert(ps.ListTopics().size() == 1);

    return 0;
}
