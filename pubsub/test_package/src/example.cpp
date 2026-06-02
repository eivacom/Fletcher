// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include <nanoarrow/nanoarrow.h>

#include <cassert>
#include <fletcher/core/write_buffer.hpp>
#include <fletcher/pubsub/owned_schema.hpp>
#include <fletcher/pubsub/provider.hpp>
#include <fletcher/pubsub/publisher.hpp>
#include <fletcher/pubsub/schema_ipc.hpp>
#include <fletcher/pubsub/subscriber.hpp>
#include <memory>
#include <vector>

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

OwnedSchema MakeSchema() {
    OwnedSchema schema;
    ArrowSchemaInit(schema.get());
    ArrowSchemaSetTypeStruct(schema.get(), 1);
    ArrowSchemaSetName(schema->children[0], "x");
    ArrowSchemaSetType(schema->children[0], NANOARROW_TYPE_INT32);
    return schema;
}

}  // namespace

int main() {
    auto provider = std::make_shared<StubProvider>();
    Publisher publisher(provider);
    Subscriber subscriber(provider);

    OwnedSchema schema = MakeSchema();
    assert(schema.valid());

    // Schema IPC round-trip.
    std::vector<uint8_t> serialized = SerializeSchemaIpc(schema.get());
    OwnedSchema restored = DeserializeSchemaIpc(serialized.data(), serialized.size());
    assert(restored.valid());

    // Publisher topic registration + introspection.
    publisher.CreateTopic({"hello", "world"}, std::move(schema));
    assert(publisher.ListTopics().size() == 1);

    return 0;
}
