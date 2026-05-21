// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include <fletcher/pubsub/driver.hpp>
#include <fletcher/pubsub/owned_schema.hpp>
#include <fletcher/pubsub/pubsub.hpp>
#include <fletcher/pubsub/schema_ipc.hpp>

#include <fletcher/core/write_buffer.hpp>

#include <nanoarrow/nanoarrow.h>

#include <cassert>
#include <memory>
#include <vector>

using namespace fletcher;

namespace {

class StubProvider : public PubSub {
 public:
    void CreateTopic(const std::vector<std::string>& /*segments*/,
                     OwnedSchema /*schema*/,
                     std::any /*config*/) override {}

    void Publish(const std::vector<std::string>& /*segments*/,
                 RowEncoder /*encoder*/,
                 const Attachments& /*attachments*/) override {}

    SubscriptionResult Subscribe(const std::vector<std::string>& /*segments*/,
                                 SubscribeCallback /*callback*/,
                                 std::any /*config*/) override {
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
    Driver driver(std::make_shared<StubProvider>());

    OwnedSchema schema = MakeSchema();
    assert(schema.valid());

    // Schema IPC round-trip.
    std::vector<uint8_t> serialized = SerializeSchemaIpc(schema.get());
    OwnedSchema restored = DeserializeSchemaIpc(serialized.data(), serialized.size());
    assert(restored.valid());

    // Driver topic registration + introspection.
    driver.CreateTopic({"hello", "world"}, std::move(schema));
    assert(driver.HasTopic({"hello", "world"}));
    assert(driver.ListTopics().size() == 1);

    return 0;
}
