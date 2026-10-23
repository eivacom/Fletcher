// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include <gtest/gtest.h>
#include <nanoarrow/nanoarrow.h>

#include <fletcher/core/status.hpp>
#include <fletcher/pubsub/in_process_provider.hpp>
#include <fletcher/pubsub/owned_schema.hpp>
#include <fletcher/pubsub/provider_registry.hpp>
#include <fletcher/pubsub/publisher.hpp>
#include <fletcher/pubsub/schema_ipc.hpp>
#include <memory>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <vector>

#include "schema_codec.hpp"

using fletcher::gateway::BuildArrowSchemaFromJson;
using fletcher::gateway::NanoarrowTypeToWireType;
using fletcher::gateway::WireTypeToNanoarrowType;
using json = nlohmann::json;

// ---------------------------------------------------------------------------
// BuildArrowSchemaFromJson — happy path
// ---------------------------------------------------------------------------

TEST(BuildArrowSchemaFromJsonTest, BuildsStructWithScalarFields) {
    json schema = {
        {"fields", json::array({
                       {{"name", "id"}, {"wireType", 0x05}},     // INT64
                       {{"name", "value"}, {"wireType", 0x0B}},  // DOUBLE
                       {{"name", "label"}, {"wireType", 0x0C}},  // STRING
                   })},
    };

    auto owned = BuildArrowSchemaFromJson(schema);

    ASSERT_NE(owned.get(), nullptr);
    EXPECT_EQ(owned->n_children, 3);
    EXPECT_STREQ(owned->children[0]->name, "id");
    EXPECT_STREQ(owned->children[1]->name, "value");
    EXPECT_STREQ(owned->children[2]->name, "label");
}

TEST(BuildArrowSchemaFromJsonTest, BuildsEmptyStruct) {
    json schema = {{"fields", json::array()}};
    auto owned = BuildArrowSchemaFromJson(schema);

    ASSERT_NE(owned.get(), nullptr);
    EXPECT_EQ(owned->n_children, 0);
}

// ---------------------------------------------------------------------------
// BuildArrowSchemaFromJson — error paths. These are the actual reason
// to unit-test this function: the integration test only sends
// well-formed protoc-gen schemas, so the error messages are otherwise
// untested.
// ---------------------------------------------------------------------------

TEST(BuildArrowSchemaFromJsonTest, ThrowsWhenFieldsKeyMissing) {
    json schema = {{"name", "Telemetry"}};
    EXPECT_THROW(BuildArrowSchemaFromJson(schema), std::invalid_argument);
}

TEST(BuildArrowSchemaFromJsonTest, ThrowsWhenFieldsIsNotArray) {
    json schema = {{"fields", "not an array"}};
    EXPECT_THROW(BuildArrowSchemaFromJson(schema), std::invalid_argument);
}

TEST(BuildArrowSchemaFromJsonTest, ThrowsWhenFieldHasNoName) {
    json schema = {
        {"fields", json::array({
                       {{"wireType", 0x05}},
                   })},
    };
    try {
        BuildArrowSchemaFromJson(schema);
        FAIL() << "expected std::invalid_argument";
    } catch (const std::invalid_argument& e) {
        // The error must name the index so the client can locate the bad field.
        const std::string what = e.what();
        EXPECT_NE(what.find("index 0"), std::string::npos) << what;
        EXPECT_NE(what.find("name"), std::string::npos) << what;
    }
}

TEST(BuildArrowSchemaFromJsonTest, ThrowsWhenFieldNameIsNotString) {
    json schema = {
        {"fields", json::array({
                       {{"name", 42}, {"wireType", 0x05}},
                   })},
    };
    EXPECT_THROW(BuildArrowSchemaFromJson(schema), std::invalid_argument);
}

TEST(BuildArrowSchemaFromJsonTest, ThrowsWhenFieldHasNoWireType) {
    json schema = {
        {"fields", json::array({
                       {{"name", "id"}, {"wireType", 0x05}},
                       {{"name", "broken"}},
                   })},
    };
    try {
        BuildArrowSchemaFromJson(schema);
        FAIL() << "expected std::invalid_argument";
    } catch (const std::invalid_argument& e) {
        const std::string what = e.what();
        EXPECT_NE(what.find("index 1"), std::string::npos) << what;
        EXPECT_NE(what.find("wireType"), std::string::npos) << what;
    }
}

TEST(BuildArrowSchemaFromJsonTest, ThrowsWhenFieldWireTypeIsNotInteger) {
    json schema = {
        {"fields", json::array({
                       {{"name", "id"}, {"wireType", "INT64"}},
                   })},
    };
    EXPECT_THROW(BuildArrowSchemaFromJson(schema), std::invalid_argument);
}

TEST(BuildArrowSchemaFromJsonTest, ThrowsOnUnsupportedWireType) {
    // 0x20 = STRUCT. Nested types are not supported in publisher-
    // supplied schemas; the parser must reject them rather than
    // silently producing a broken nanoarrow setup.
    json schema = {
        {"fields", json::array({
                       {{"name", "nested"}, {"wireType", 0x20}},
                   })},
    };
    EXPECT_THROW(BuildArrowSchemaFromJson(schema), std::invalid_argument);
}

TEST(BuildArrowSchemaFromJsonTest, ThrowsOnUnknownWireType) {
    json schema = {
        {"fields", json::array({
                       {{"name", "mystery"}, {"wireType", 0xFE}},
                   })},
    };
    EXPECT_THROW(BuildArrowSchemaFromJson(schema), std::invalid_argument);
}

// ---------------------------------------------------------------------------
// WireType ↔ ArrowType round-trip. One test for every scalar wireType
// the gateway claims to support — guards against the two switch
// statements drifting apart.
// ---------------------------------------------------------------------------

TEST(WireTypeRoundTripTest, ScalarWireTypesRoundTripThroughNanoarrowType) {
    constexpr int kScalarWireTypes[] = {
        0x01,  // BOOL
        0x02,  // INT8
        0x03,  // INT16
        0x04,  // INT32
        0x05,  // INT64
        0x06,  // UINT8
        0x07,  // UINT16
        0x08,  // UINT32
        0x09,  // UINT64
        0x0A,  // FLOAT
        0x0B,  // DOUBLE
        0x0C,  // STRING
        0x0D,  // BINARY
        0x18,  // LARGE_STRING
        0x19,  // LARGE_BINARY
    };

    for (int wt : kScalarWireTypes) {
        enum ArrowType t = WireTypeToNanoarrowType(wt);
        int round_tripped = NanoarrowTypeToWireType(t);
        EXPECT_EQ(round_tripped, wt)
            << "wireType 0x" << std::hex << wt << " did not round-trip through NanoarrowType";
    }
}

// ---------------------------------------------------------------------------
// A JSON-announced schema against the schema a native publisher declares.
//
// BIND-8 (2026-10-16), measured at the maintainer's request before the
// create_topic protocol is changed. A native C++ or C# publisher declares its
// GENERATED Arrow schema: non-null proto3 scalars, each carrying its
// `field_number` metadata. A gateway client can only announce the JSON
// descriptor, and BuildArrowSchemaFromJson keeps names and scalar types and
// nothing else. Re-declaring a topic is idempotent only for byte-identical
// Arrow IPC (pubsub/internal/schema_conflict.hpp), so the two declarations of
// ONE topic conflict even for a flat message. The schema's Arrow IPC, which the
// gateway already sends to subscribers as `schemaIpc`, round-trips to the same
// bytes and does not.
// ---------------------------------------------------------------------------

namespace {

// `message Reading { int64 id = 1; double value = 2; string label = 3; }` as the
// generator declares it: non-null fields, each with its field_number.
fletcher::OwnedSchema GeneratedReadingSchema() {
    fletcher::OwnedSchema schema;
    ArrowSchemaInit(schema.get());
    EXPECT_EQ(ArrowSchemaSetTypeStruct(schema.get(), 3), NANOARROW_OK);
    const struct {
        const char* name;
        enum ArrowType type;
        const char* number;
    } fields[] = {{"id", NANOARROW_TYPE_INT64, "1"},
                  {"value", NANOARROW_TYPE_DOUBLE, "2"},
                  {"label", NANOARROW_TYPE_STRING, "3"}};
    for (int i = 0; i < 3; ++i) {
        ArrowSchema* child = schema->children[i];
        EXPECT_EQ(ArrowSchemaSetType(child, fields[i].type), NANOARROW_OK);
        EXPECT_EQ(ArrowSchemaSetName(child, fields[i].name), NANOARROW_OK);
        child->flags &= ~ARROW_FLAG_NULLABLE;
        ArrowBuffer metadata;
        EXPECT_EQ(ArrowMetadataBuilderInit(&metadata, nullptr), NANOARROW_OK);
        EXPECT_EQ(ArrowMetadataBuilderAppend(&metadata, ArrowCharView("field_number"),
                                             ArrowCharView(fields[i].number)),
                  NANOARROW_OK);
        EXPECT_EQ(ArrowSchemaSetMetadata(child, reinterpret_cast<const char*>(metadata.data)),
                  NANOARROW_OK);
        ArrowBufferReset(&metadata);
    }
    return schema;
}

// The descriptor a gateway client announces for that message: what
// Eiva.Fletcher.GatewayClient's ControlMessages writes (and gateway-client-ts's
// generated descriptor carries the same members).
json ClientDescriptorForReading() {
    return json{
        {"fields",
         json::array({
             {{"name", "id"}, {"nullable", false}, {"wireType", 0x05}, {"fieldNumber", 1}},
             {{"name", "value"}, {"nullable", false}, {"wireType", 0x0B}, {"fieldNumber", 2}},
             {{"name", "label"}, {"nullable", false}, {"wireType", 0x0C}, {"fieldNumber", 3}},
         })}};
}

std::unique_ptr<fletcher::Publisher> InProcessPublisher() {
    fletcher::ProviderRegistry registry;
    fletcher::RegisterInProcessProvider(registry);
    return std::make_unique<fletcher::Publisher>(registry.Create(
        fletcher::ProviderSelector::Parse("inprocess"), fletcher::ProviderConfig{}));
}

}  // namespace

TEST(GatewayDeclaredSchemaTest,
     AJsonAnnouncedSchemaConflictsWithTheNativeDeclarationOfAFlatMessage) {
    const std::vector<std::string> topic = {"bind8", "reading"};

    // Native first, gateway client second: the order of a C++ service already up.
    {
        auto publisher = InProcessPublisher();
        publisher->CreateTopic(topic, GeneratedReadingSchema());
        try {
            publisher->CreateTopic(topic, BuildArrowSchemaFromJson(ClientDescriptorForReading()));
            FAIL() << "the JSON-announced schema was accepted as the native declaration's equal";
        } catch (const fletcher::PubSubError& e) {
            EXPECT_EQ(e.status(), fletcher::PubSubStatus::kSchemaConflict) << e.what();
        }
    }

    // Gateway client first, native second: the conflict is symmetric.
    {
        auto publisher = InProcessPublisher();
        publisher->CreateTopic(topic, BuildArrowSchemaFromJson(ClientDescriptorForReading()));
        try {
            publisher->CreateTopic(topic, GeneratedReadingSchema());
            FAIL() << "the native declaration was accepted as the JSON-announced schema's equal";
        } catch (const fletcher::PubSubError& e) {
            EXPECT_EQ(e.status(), fletcher::PubSubStatus::kSchemaConflict) << e.what();
        }
    }
}

TEST(GatewayDeclaredSchemaTest, TheJsonPathLosesNullabilityAndFieldNumbersAndRefusesTimestamps) {
    // Why the bytes differ: the descriptor says nullable false and carries the
    // field number; the schema built from it is nullable and has no metadata.
    fletcher::OwnedSchema built = BuildArrowSchemaFromJson(ClientDescriptorForReading());
    for (int64_t i = 0; i < built->n_children; ++i) {
        EXPECT_NE(built->children[i]->flags & ARROW_FLAG_NULLABLE, 0)
            << "field " << i << ": nullable false was honoured";
        EXPECT_EQ(built->children[i]->metadata, nullptr)
            << "field " << i << ": the field number was kept";
    }

    // And a flat message with a google.protobuf.Timestamp field (wireType 0x10)
    // cannot be announced at all.
    json with_timestamp = {{"fields", json::array({{{"name", "at"}, {"wireType", 0x10}}})}};
    EXPECT_THROW(BuildArrowSchemaFromJson(with_timestamp), std::invalid_argument);
}

TEST(GatewayDeclaredSchemaTest, TheSchemasArrowIpcRoundTripsToTheNativeDeclaration) {
    // The direction the gateway already uses toward subscribers (`schemaIpc`):
    // the generated schema's own Arrow IPC, decoded, is the same declaration.
    const std::vector<std::string> topic = {"bind8", "reading"};
    fletcher::OwnedSchema native = GeneratedReadingSchema();
    const std::vector<uint8_t> ipc = fletcher::SerializeSchemaIpc(native.get());

    auto publisher = InProcessPublisher();
    publisher->CreateTopic(topic, std::move(native));
    EXPECT_NO_THROW(
        publisher->CreateTopic(topic, fletcher::DeserializeSchemaIpc(ipc.data(), ipc.size())));
}

// ---------------------------------------------------------------------------
// D-BIND-112: create_topic announces the schema as `schemaIpc`.
// ---------------------------------------------------------------------------

using fletcher::gateway::Base64Decode;
using fletcher::gateway::Base64Encode;
using fletcher::gateway::DeclaredSchemaFromCreateTopic;

namespace {

// A generated schema the JSON path cannot carry: a nested message and a
// Timestamp, `message Fix { Position at = 1; google.protobuf.Timestamp when = 2; }`.
fletcher::OwnedSchema GeneratedNestedSchema() {
    fletcher::OwnedSchema schema;
    ArrowSchemaInit(schema.get());
    EXPECT_EQ(ArrowSchemaSetTypeStruct(schema.get(), 2), NANOARROW_OK);

    ArrowSchema* at = schema->children[0];
    EXPECT_EQ(ArrowSchemaSetTypeStruct(at, 2), NANOARROW_OK);
    EXPECT_EQ(ArrowSchemaSetName(at, "at"), NANOARROW_OK);
    EXPECT_EQ(ArrowSchemaSetType(at->children[0], NANOARROW_TYPE_DOUBLE), NANOARROW_OK);
    EXPECT_EQ(ArrowSchemaSetName(at->children[0], "lat"), NANOARROW_OK);
    EXPECT_EQ(ArrowSchemaSetType(at->children[1], NANOARROW_TYPE_DOUBLE), NANOARROW_OK);
    EXPECT_EQ(ArrowSchemaSetName(at->children[1], "lon"), NANOARROW_OK);

    ArrowSchema* when = schema->children[1];
    EXPECT_EQ(
        ArrowSchemaSetTypeDateTime(when, NANOARROW_TYPE_TIMESTAMP, NANOARROW_TIME_UNIT_NANO, "UTC"),
        NANOARROW_OK);
    EXPECT_EQ(ArrowSchemaSetName(when, "when"), NANOARROW_OK);
    when->flags &= ~ARROW_FLAG_NULLABLE;
    return schema;
}

json CreateTopicWithIpc(const fletcher::OwnedSchema& schema) {
    const std::vector<uint8_t> ipc = fletcher::SerializeSchemaIpc(schema.get());
    return json{{"action", "create_topic"},
                {"topic", "bind8/x"},
                {"schemaIpc", Base64Encode(ipc.data(), ipc.size())}};
}

}  // namespace

TEST(Base64Test, RoundTripsEveryLengthModuloThree) {
    for (size_t n = 0; n <= 7; ++n) {
        std::vector<uint8_t> bytes(n);
        for (size_t i = 0; i < n; ++i) bytes[i] = static_cast<uint8_t>(0xF0 + i);
        EXPECT_EQ(Base64Decode(Base64Encode(bytes.data(), bytes.size())), bytes) << "length " << n;
    }
    const std::string hello = "hello";
    EXPECT_EQ(Base64Encode(reinterpret_cast<const uint8_t*>(hello.data()), hello.size()),
              "aGVsbG8=");
}

TEST(Base64Test, RefusesWhatIsNotStrictBase64) {
    for (const char* bad : {"abc", "ab=c", "a===", "=abc", "ab!d", "aGVsbG8=aGVs"}) {
        EXPECT_THROW(static_cast<void>(Base64Decode(bad)), std::invalid_argument) << bad;
    }
}

TEST(GatewayDeclaredSchemaTest, ACreateTopicWithSchemaIpcDeclaresTheNativeSchemaItself) {
    // The counterpart of the conflict above: the same flat message, announced
    // the D-BIND-112 way, is the native declaration's equal, in either order.
    const std::vector<std::string> topic = {"bind8", "reading"};
    {
        auto publisher = InProcessPublisher();
        publisher->CreateTopic(topic, GeneratedReadingSchema());
        EXPECT_NO_THROW(publisher->CreateTopic(
            topic, DeclaredSchemaFromCreateTopic(CreateTopicWithIpc(GeneratedReadingSchema()))));
    }
    {
        auto publisher = InProcessPublisher();
        publisher->CreateTopic(
            topic, DeclaredSchemaFromCreateTopic(CreateTopicWithIpc(GeneratedReadingSchema())));
        EXPECT_NO_THROW(publisher->CreateTopic(topic, GeneratedReadingSchema()));
    }
}

TEST(GatewayDeclaredSchemaTest, SchemaIpcCarriesNestedAndTimestampFields) {
    const std::vector<std::string> topic = {"bind8", "fix"};
    auto publisher = InProcessPublisher();
    publisher->CreateTopic(topic, GeneratedNestedSchema());

    fletcher::OwnedSchema declared =
        DeclaredSchemaFromCreateTopic(CreateTopicWithIpc(GeneratedNestedSchema()));
    ASSERT_EQ(declared->n_children, 2);
    EXPECT_STREQ(declared->children[0]->format, "+s");
    EXPECT_STREQ(declared->children[1]->format, "tsn:UTC");
    EXPECT_NO_THROW(publisher->CreateTopic(topic, std::move(declared)));
}

TEST(GatewayDeclaredSchemaTest, SchemaIpcWinsOverTheJsonSchemaAndNeitherDeclaresNone) {
    json both = CreateTopicWithIpc(GeneratedReadingSchema());
    both["schema"] = ClientDescriptorForReading();
    const std::vector<std::string> topic = {"bind8", "reading"};
    auto publisher = InProcessPublisher();
    publisher->CreateTopic(topic, GeneratedReadingSchema());
    EXPECT_NO_THROW(publisher->CreateTopic(topic, DeclaredSchemaFromCreateTopic(both)));

    fletcher::OwnedSchema none =
        DeclaredSchemaFromCreateTopic(json{{"action", "create_topic"}, {"topic", "t"}});
    EXPECT_EQ(none->release, nullptr) << "a create_topic without a schema declared one";
}

TEST(GatewayDeclaredSchemaTest, AMalformedSchemaIpcIsRefused) {
    const std::string not_ipc = "aGVsbG8=";  // valid base64 of "hello"
    for (const json& bad :
         {json{{"schemaIpc", 42}}, json{{"schemaIpc", "!!!!"}}, json{{"schemaIpc", not_ipc}}}) {
        EXPECT_THROW(static_cast<void>(DeclaredSchemaFromCreateTopic(bad)), std::invalid_argument)
            << bad.dump();
    }
}

TEST(GatewayDeclaredSchemaTest, AnExplicitNullIsTheMemberLeftOut) {
    // Review of BIND-8: the spec calls both members optional, and a client whose JSON writer
    // keeps absent optionals sends them as null; that was refused as "must be a base64 string".
    json null_ipc = json{{"schemaIpc", nullptr}, {"schema", ClientDescriptorForReading()}};
    fletcher::OwnedSchema from_json = DeclaredSchemaFromCreateTopic(null_ipc);
    ASSERT_NE(from_json->release, nullptr) << "the JSON schema beside a null schemaIpc was ignored";
    EXPECT_EQ(from_json->n_children, ClientDescriptorForReading()["fields"].size());

    fletcher::OwnedSchema none =
        DeclaredSchemaFromCreateTopic(json{{"schemaIpc", nullptr}, {"schema", nullptr}});
    EXPECT_EQ(none->release, nullptr) << "two nulls declared a schema";
}

// ---------------------------------------------------------------------------
// The schema a `subscribed` reply describes (ArrowSchemaToJson), BIND-8d.
// ---------------------------------------------------------------------------

namespace {

void SetOneMetadata(ArrowSchema* field, const char* key, const char* value) {
    ArrowBuffer metadata;
    ASSERT_EQ(ArrowMetadataBuilderInit(&metadata, nullptr), NANOARROW_OK);
    ASSERT_EQ(ArrowMetadataBuilderAppend(&metadata, ArrowCharView(key), ArrowCharView(value)),
              NANOARROW_OK);
    ASSERT_EQ(ArrowSchemaSetMetadata(field, reinterpret_cast<const char*>(metadata.data)),
              NANOARROW_OK);
    ArrowBufferReset(&metadata);
}

}  // namespace

TEST(ArrowSchemaToJsonTest, AFieldWithMetadataButNoFieldNumberHasNone) {
    // nanoarrow's ArrowMetadataGetValue leaves its out-value unchanged, and returns OK, for an
    // absent key. `b` has metadata but no field_number, like a generated list element; it is
    // `a`'s sibling, so the old uninitialised read in FieldToJson found `a`'s "1" in the same
    // stack slot (or garbage: a std::bad_alloc out of a `subscribed` reply, measured live).
    fletcher::OwnedSchema schema;
    ArrowSchemaInit(schema.get());
    ASSERT_EQ(ArrowSchemaSetTypeStruct(schema.get(), 2), NANOARROW_OK);
    ASSERT_EQ(ArrowSchemaSetType(schema->children[0], NANOARROW_TYPE_INT32), NANOARROW_OK);
    ASSERT_EQ(ArrowSchemaSetName(schema->children[0], "a"), NANOARROW_OK);
    SetOneMetadata(schema->children[0], "field_number", "1");
    ASSERT_EQ(ArrowSchemaSetType(schema->children[1], NANOARROW_TYPE_INT32), NANOARROW_OK);
    ASSERT_EQ(ArrowSchemaSetName(schema->children[1], "b"), NANOARROW_OK);
    SetOneMetadata(schema->children[1], "fletcher.note", "no field number here");

    const json j = fletcher::gateway::ArrowSchemaToJson(schema.get());
    ASSERT_EQ(j["fields"].size(), 2u) << j.dump();
    EXPECT_EQ(j["fields"][0]["fieldNumber"], 1) << j.dump();
    EXPECT_FALSE(j["fields"][1].contains("fieldNumber")) << j.dump();
}

TEST(ArrowSchemaToJsonTest, AFieldNumberThatIsNotAWholeInt32IsReadAsAbsent) {
    // D-BIND-123: since D-BIND-112 any WebSocket client declares a schema's metadata through
    // `schemaIpc`; the old std::stoi threw out of every `subscribed` reply for that topic.
    for (const char* bad : {"abc", "99999999999", "7x", "", " 7"}) {
        fletcher::OwnedSchema schema;
        ArrowSchemaInit(schema.get());
        ASSERT_EQ(ArrowSchemaSetTypeStruct(schema.get(), 1), NANOARROW_OK);
        ASSERT_EQ(ArrowSchemaSetType(schema->children[0], NANOARROW_TYPE_INT32), NANOARROW_OK);
        ASSERT_EQ(ArrowSchemaSetName(schema->children[0], "x"), NANOARROW_OK);
        SetOneMetadata(schema->children[0], "field_number", bad);

        json j;
        ASSERT_NO_THROW(j = fletcher::gateway::ArrowSchemaToJson(schema.get()))
            << "'" << bad << "'";
        EXPECT_FALSE(j["fields"][0].contains("fieldNumber")) << "'" << bad << "': " << j.dump();
    }
}
