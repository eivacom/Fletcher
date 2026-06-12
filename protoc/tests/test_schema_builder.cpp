// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// BuildMessageSchema / SerializeSchemaIpc — the in-process counterparts of
// the generated <Class>Schema() functions and the runtime schema_ipc codec
// (--fletcher_opt=ipc). The byte-level parity with generated code is covered
// by the protoc-arrow-bridge integration test; these tests pin down the
// schema structure (formats, names, flags, metadata) and the plugin-level
// .ipc file emission.

#include <google/protobuf/compiler/code_generator.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include <gtest/gtest.h>
#include <nanoarrow/nanoarrow.h>
#include <nanoarrow/nanoarrow_ipc.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "generator.hpp"
#include "schema_builder.hpp"

using namespace fletcher;
using namespace google::protobuf;

// ===========================================================================
// Helpers
// ===========================================================================

namespace {

std::string Fmt(const ArrowSchema* s) { return s->format ? s->format : ""; }

std::string Name(const ArrowSchema* s) { return s->name ? s->name : ""; }

bool Nullable(const ArrowSchema* s) { return (s->flags & ARROW_FLAG_NULLABLE) != 0; }

std::string MetaValue(const ArrowSchema* s, const char* key) {
    if (!s->metadata || !ArrowMetadataHasKey(s->metadata, ArrowCharView(key))) {
        return "<missing>";
    }
    ArrowStringView value{};
    if (ArrowMetadataGetValue(s->metadata, ArrowCharView(key), &value) != NANOARROW_OK) {
        return "<error>";
    }
    return std::string(value.data, static_cast<size_t>(value.size_bytes));
}

// Deserialize IPC stream bytes back into a schema (reader counterpart of
// SerializeSchemaIpc, mirroring pubsub's DeserializeSchemaIpc).
nanoarrow::UniqueSchema DeserializeIpc(const std::vector<uint8_t>& data) {
    nanoarrow::UniqueSchema result;

    ArrowBuffer input;
    ArrowBufferInit(&input);
    if (ArrowBufferAppend(&input, data.data(), static_cast<int64_t>(data.size())) != NANOARROW_OK) {
        ArrowBufferReset(&input);
        ADD_FAILURE() << "buffer alloc failed";
        return result;
    }

    ArrowIpcInputStream stream{};
    if (ArrowIpcInputStreamInitBuffer(&stream, &input) != NANOARROW_OK) {
        ArrowBufferReset(&input);
        ADD_FAILURE() << "input stream init failed";
        return result;
    }

    ArrowArrayStream array_stream{};
    if (ArrowIpcArrayStreamReaderInit(&array_stream, &stream, nullptr) != NANOARROW_OK) {
        if (stream.release) stream.release(&stream);
        ADD_FAILURE() << "reader init failed";
        return result;
    }

    EXPECT_EQ(array_stream.get_schema(&array_stream, result.get()), NANOARROW_OK);
    array_stream.release(&array_stream);
    return result;
}

// Build a proto3 file with one message holding every supported scalar type.
const FileDescriptor* BuildAllScalars(DescriptorPool& pool) {
    FileDescriptorProto fdp;
    fdp.set_name("scalars.proto");
    fdp.set_package("unit");
    fdp.set_syntax("proto3");
    auto* msg = fdp.add_message_type();
    msg->set_name("AllScalars");

    const std::pair<const char*, FieldDescriptorProto::Type> kFields[] = {
        {"f_bool", FieldDescriptorProto::TYPE_BOOL},
        {"f_int32", FieldDescriptorProto::TYPE_INT32},
        {"f_int64", FieldDescriptorProto::TYPE_INT64},
        {"f_uint32", FieldDescriptorProto::TYPE_UINT32},
        {"f_uint64", FieldDescriptorProto::TYPE_UINT64},
        {"f_float", FieldDescriptorProto::TYPE_FLOAT},
        {"f_double", FieldDescriptorProto::TYPE_DOUBLE},
        {"f_string", FieldDescriptorProto::TYPE_STRING},
        {"f_bytes", FieldDescriptorProto::TYPE_BYTES},
    };
    int number = 1;
    for (const auto& [name, type] : kFields) {
        auto* f = msg->add_field();
        f->set_name(name);
        f->set_number(number++);
        f->set_type(type);
        f->set_label(FieldDescriptorProto::LABEL_OPTIONAL);
    }
    return pool.BuildFile(fdp);
}

// GeneratorContext that captures Open()ed files in memory.
class CapturingContext : public compiler::GeneratorContext {
   public:
    std::map<std::string, std::string> files;

    io::ZeroCopyOutputStream* Open(const std::string& filename) override {
        return new io::StringOutputStream(&files[filename]);
    }
};

}  // namespace

// ===========================================================================
// Schema structure
// ===========================================================================

TEST(SchemaBuilderTest, ScalarFormatsNamesAndMetadata) {
    DescriptorPool pool;
    const FileDescriptor* file = BuildAllScalars(pool);
    ASSERT_NE(file, nullptr);

    nanoarrow::UniqueSchema schema = BuildMessageSchema(file->message_type(0));

    EXPECT_EQ(Fmt(schema.get()), "+s");
    EXPECT_EQ(MetaValue(schema.get(), "proto_package"), "unit");
    EXPECT_EQ(MetaValue(schema.get(), "proto_message"), "AllScalars");

    ASSERT_EQ(schema->n_children, 9);
    const std::pair<const char*, const char*> kExpected[] = {
        {"f_bool", "b"},   {"f_int32", "i"},  {"f_int64", "l"},
        {"f_uint32", "I"}, {"f_uint64", "L"}, {"f_float", "f"},
        {"f_double", "g"}, {"f_string", "u"}, {"f_bytes", "z"},
    };
    for (int i = 0; i < 9; ++i) {
        const ArrowSchema* child = schema->children[i];
        EXPECT_EQ(Name(child), kExpected[i].first) << "child " << i;
        EXPECT_EQ(Fmt(child), kExpected[i].second) << "child " << i;
        // proto3 fields without `optional` are not nullable.
        EXPECT_FALSE(Nullable(child)) << "child " << i;
        EXPECT_EQ(MetaValue(child, "field_number"), std::to_string(i + 1)) << "child " << i;
        EXPECT_EQ(MetaValue(child, "field_id"), std::to_string(i + 1)) << "child " << i;
    }
}

TEST(SchemaBuilderTest, Proto2OptionalFieldIsNullable) {
    DescriptorPool pool;
    FileDescriptorProto fdp;
    fdp.set_name("optional.proto");
    auto* msg = fdp.add_message_type();
    msg->set_name("Msg");
    auto* f = msg->add_field();
    f->set_name("maybe");
    f->set_number(1);
    f->set_type(FieldDescriptorProto::TYPE_INT32);
    f->set_label(FieldDescriptorProto::LABEL_OPTIONAL);
    const FileDescriptor* file = pool.BuildFile(fdp);
    ASSERT_NE(file, nullptr);

    nanoarrow::UniqueSchema schema = BuildMessageSchema(file->message_type(0));
    ASSERT_EQ(schema->n_children, 1);
    EXPECT_TRUE(Nullable(schema->children[0]));
}

TEST(SchemaBuilderTest, TimestampMapsToNanoTimestamp) {
    DescriptorPool pool;

    FileDescriptorProto wkt;
    wkt.set_name("google/protobuf/timestamp.proto");
    wkt.set_package("google.protobuf");
    wkt.set_syntax("proto3");
    auto* ts = wkt.add_message_type();
    ts->set_name("Timestamp");
    auto* secs = ts->add_field();
    secs->set_name("seconds");
    secs->set_number(1);
    secs->set_type(FieldDescriptorProto::TYPE_INT64);
    secs->set_label(FieldDescriptorProto::LABEL_OPTIONAL);
    auto* nanos = ts->add_field();
    nanos->set_name("nanos");
    nanos->set_number(2);
    nanos->set_type(FieldDescriptorProto::TYPE_INT32);
    nanos->set_label(FieldDescriptorProto::LABEL_OPTIONAL);
    ASSERT_NE(pool.BuildFile(wkt), nullptr);

    FileDescriptorProto fdp;
    fdp.set_name("temporal.proto");
    fdp.set_syntax("proto3");
    fdp.add_dependency("google/protobuf/timestamp.proto");
    auto* msg = fdp.add_message_type();
    msg->set_name("Msg");
    auto* f = msg->add_field();
    f->set_name("stamp");
    f->set_number(1);
    f->set_type(FieldDescriptorProto::TYPE_MESSAGE);
    f->set_type_name(".google.protobuf.Timestamp");
    f->set_label(FieldDescriptorProto::LABEL_OPTIONAL);
    const FileDescriptor* file = pool.BuildFile(fdp);
    ASSERT_NE(file, nullptr);

    nanoarrow::UniqueSchema schema = BuildMessageSchema(file->message_type(0));
    ASSERT_EQ(schema->n_children, 1);
    EXPECT_EQ(Fmt(schema->children[0]), "tsn:");
}

TEST(SchemaBuilderTest, RepeatedScalarBecomesListWithNullableItem) {
    DescriptorPool pool;
    FileDescriptorProto fdp;
    fdp.set_name("repeated.proto");
    fdp.set_syntax("proto3");
    auto* msg = fdp.add_message_type();
    msg->set_name("Msg");
    auto* f = msg->add_field();
    f->set_name("values");
    f->set_number(1);
    f->set_type(FieldDescriptorProto::TYPE_INT32);
    f->set_label(FieldDescriptorProto::LABEL_REPEATED);
    const FileDescriptor* file = pool.BuildFile(fdp);
    ASSERT_NE(file, nullptr);

    nanoarrow::UniqueSchema schema = BuildMessageSchema(file->message_type(0));
    ASSERT_EQ(schema->n_children, 1);
    const ArrowSchema* list = schema->children[0];
    EXPECT_EQ(Fmt(list), "+l");
    EXPECT_FALSE(Nullable(list));
    ASSERT_EQ(list->n_children, 1);
    EXPECT_EQ(Name(list->children[0]), "item");
    EXPECT_EQ(Fmt(list->children[0]), "i");
    EXPECT_TRUE(Nullable(list->children[0]));
}

TEST(SchemaBuilderTest, NestedStructCopiesFieldsAndReplacesMetadata) {
    DescriptorPool pool;
    FileDescriptorProto fdp;
    fdp.set_name("nested.proto");
    fdp.set_package("unit");
    fdp.set_syntax("proto3");

    auto* inner = fdp.add_message_type();
    inner->set_name("Inner");
    auto* iv = inner->add_field();
    iv->set_name("value");
    iv->set_number(1);
    iv->set_type(FieldDescriptorProto::TYPE_STRING);
    iv->set_label(FieldDescriptorProto::LABEL_OPTIONAL);

    auto* outer = fdp.add_message_type();
    outer->set_name("Outer");
    auto* of = outer->add_field();
    of->set_name("inner");
    of->set_number(2);
    of->set_type(FieldDescriptorProto::TYPE_MESSAGE);
    of->set_type_name(".unit.Inner");
    of->set_label(FieldDescriptorProto::LABEL_OPTIONAL);

    const FileDescriptor* file = pool.BuildFile(fdp);
    ASSERT_NE(file, nullptr);

    nanoarrow::UniqueSchema schema = BuildMessageSchema(file->message_type(1));
    ASSERT_EQ(schema->n_children, 1);
    const ArrowSchema* child = schema->children[0];
    EXPECT_EQ(Fmt(child), "+s");
    EXPECT_EQ(Name(child), "inner");

    // The field's metadata replaces the copied Inner schema-level metadata
    // (mirrors the generated code: DeepCopy, then SetName/SetMetadata).
    EXPECT_EQ(MetaValue(child, "field_number"), "2");
    EXPECT_EQ(MetaValue(child, "proto_message"), "<missing>");

    ASSERT_EQ(child->n_children, 1);
    EXPECT_EQ(Name(child->children[0]), "value");
    EXPECT_EQ(Fmt(child->children[0]), "u");
    EXPECT_EQ(MetaValue(child->children[0], "field_number"), "1");
}

TEST(SchemaBuilderTest, MapFieldBecomesArrowMap) {
    DescriptorPool pool;
    FileDescriptorProto fdp;
    fdp.set_name("maps.proto");
    fdp.set_syntax("proto3");
    auto* msg = fdp.add_message_type();
    msg->set_name("Msg");

    auto* entry = msg->add_nested_type();
    entry->set_name("TagsEntry");
    entry->mutable_options()->set_map_entry(true);
    auto* kf = entry->add_field();
    kf->set_name("key");
    kf->set_number(1);
    kf->set_type(FieldDescriptorProto::TYPE_STRING);
    kf->set_label(FieldDescriptorProto::LABEL_OPTIONAL);
    auto* vf = entry->add_field();
    vf->set_name("value");
    vf->set_number(2);
    vf->set_type(FieldDescriptorProto::TYPE_INT64);
    vf->set_label(FieldDescriptorProto::LABEL_OPTIONAL);

    auto* f = msg->add_field();
    f->set_name("tags");
    f->set_number(1);
    f->set_type(FieldDescriptorProto::TYPE_MESSAGE);
    f->set_type_name("TagsEntry");
    f->set_label(FieldDescriptorProto::LABEL_REPEATED);

    const FileDescriptor* file = pool.BuildFile(fdp);
    ASSERT_NE(file, nullptr);

    nanoarrow::UniqueSchema schema = BuildMessageSchema(file->message_type(0));
    ASSERT_EQ(schema->n_children, 1);
    const ArrowSchema* map = schema->children[0];
    EXPECT_EQ(Fmt(map), "+m");
    ASSERT_EQ(map->n_children, 1);
    const ArrowSchema* entries = map->children[0];
    ASSERT_EQ(entries->n_children, 2);
    EXPECT_EQ(Name(entries->children[0]), "key");
    EXPECT_EQ(Fmt(entries->children[0]), "u");
    EXPECT_EQ(Name(entries->children[1]), "value");
    EXPECT_EQ(Fmt(entries->children[1]), "l");
}

// ===========================================================================
// IPC serialization
// ===========================================================================

TEST(SchemaBuilderTest, SerializeRoundTripsThroughIpcReader) {
    DescriptorPool pool;
    const FileDescriptor* file = BuildAllScalars(pool);
    ASSERT_NE(file, nullptr);

    nanoarrow::UniqueSchema schema = BuildMessageSchema(file->message_type(0));
    const std::vector<uint8_t> bytes = SerializeSchemaIpc(schema.get());
    ASSERT_FALSE(bytes.empty());

    // Arrow IPC stream: encapsulated messages start with the 0xFFFFFFFF
    // continuation marker; the stream ends with the 8-byte EOS marker.
    ASSERT_GE(bytes.size(), 12u);
    EXPECT_EQ(bytes[0], 0xFF);
    EXPECT_EQ(bytes[3], 0xFF);
    const std::vector<uint8_t> eos = {0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00};
    EXPECT_TRUE(std::equal(eos.begin(), eos.end(), bytes.end() - 8));

    nanoarrow::UniqueSchema round = DeserializeIpc(bytes);
    ASSERT_NE(round->release, nullptr);
    EXPECT_EQ(Fmt(round.get()), "+s");
    ASSERT_EQ(round->n_children, schema->n_children);
    for (int64_t i = 0; i < schema->n_children; ++i) {
        EXPECT_EQ(Name(round->children[i]), Name(schema->children[i]));
        EXPECT_EQ(Fmt(round->children[i]), Fmt(schema->children[i]));
        EXPECT_EQ(Nullable(round->children[i]), Nullable(schema->children[i]));
    }

    // Re-serializing the round-tripped schema reproduces the exact bytes.
    EXPECT_EQ(SerializeSchemaIpc(round.get()), bytes);
}

// ===========================================================================
// Plugin-level emission (--fletcher_opt=ipc)
// ===========================================================================

TEST(SchemaBuilderTest, GeneratorEmitsIpcFilesOnlyWhenRequested) {
    DescriptorPool pool;
    const FileDescriptor* file = BuildAllScalars(pool);
    ASSERT_NE(file, nullptr);

    ArrowRowGenerator generator;
    std::string error;

    CapturingContext without_opt;
    ASSERT_TRUE(generator.Generate(file, "", &without_opt, &error)) << error;
    EXPECT_TRUE(without_opt.files.count("scalars.fletcher.pb.h"));
    EXPECT_FALSE(without_opt.files.count("scalars.AllScalars.ipc"));

    CapturingContext with_opt;
    ASSERT_TRUE(generator.Generate(file, "ipc", &with_opt, &error)) << error;
    EXPECT_TRUE(with_opt.files.count("scalars.fletcher.pb.h"));
    ASSERT_TRUE(with_opt.files.count("scalars.AllScalars.ipc"));

    // The emitted file holds exactly the serialized schema bytes.
    nanoarrow::UniqueSchema schema = BuildMessageSchema(file->message_type(0));
    const std::vector<uint8_t> expected = SerializeSchemaIpc(schema.get());
    const std::string& written = with_opt.files.at("scalars.AllScalars.ipc");
    ASSERT_EQ(written.size(), expected.size());
    EXPECT_TRUE(std::equal(expected.begin(), expected.end(),
                           reinterpret_cast<const uint8_t*>(written.data())));
}

TEST(SchemaBuilderTest, GeneratorEmitsOneIpcFilePerMessage) {
    DescriptorPool pool;
    FileDescriptorProto fdp;
    fdp.set_name("multi.proto");
    fdp.set_package("unit");
    fdp.set_syntax("proto3");

    auto* first = fdp.add_message_type();
    first->set_name("First");
    auto* f1 = first->add_field();
    f1->set_name("a");
    f1->set_number(1);
    f1->set_type(FieldDescriptorProto::TYPE_INT32);
    f1->set_label(FieldDescriptorProto::LABEL_OPTIONAL);

    auto* second = fdp.add_message_type();
    second->set_name("Second");
    auto* nested = second->add_nested_type();
    nested->set_name("Inner");
    auto* nf = nested->add_field();
    nf->set_name("b");
    nf->set_number(1);
    nf->set_type(FieldDescriptorProto::TYPE_STRING);
    nf->set_label(FieldDescriptorProto::LABEL_OPTIONAL);
    auto* f2 = second->add_field();
    f2->set_name("inner");
    f2->set_number(1);
    f2->set_type(FieldDescriptorProto::TYPE_MESSAGE);
    f2->set_type_name(".unit.Second.Inner");
    f2->set_label(FieldDescriptorProto::LABEL_OPTIONAL);

    const FileDescriptor* file = pool.BuildFile(fdp);
    ASSERT_NE(file, nullptr);

    ArrowRowGenerator generator;
    std::string error;
    CapturingContext ctx;
    ASSERT_TRUE(generator.Generate(file, "ipc", &ctx, &error)) << error;

    EXPECT_TRUE(ctx.files.count("multi.First.ipc"));
    EXPECT_TRUE(ctx.files.count("multi.Second.ipc"));
    // Nested messages get their own schema file, named like their class.
    EXPECT_TRUE(ctx.files.count("multi.Second_Inner.ipc"));
}
