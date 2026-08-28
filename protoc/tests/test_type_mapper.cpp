// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/unknown_field_set.h>
#include <google/protobuf/wrappers.pb.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <set>
#include <string>

#include "cpp_backend_schema_visitor.hpp"
#include "ir.hpp"
#include "option_reader.hpp"
#include "proto_text_pool.hpp"
#include "type_mapper.hpp"

using namespace fletcher;
using namespace google::protobuf;

// ===========================================================================
// Helpers
// ===========================================================================

namespace {

const FileDescriptor* BuildSingleField(
    DescriptorPool& pool, const std::string& tag, FieldDescriptorProto::Type type,
    FieldDescriptorProto::Label label = FieldDescriptorProto::LABEL_OPTIONAL) {
    FileDescriptorProto fdp;
    fdp.set_name("test_" + tag + ".proto");
    fdp.set_syntax("proto3");
    auto* msg = fdp.add_message_type();
    msg->set_name("Msg");
    auto* f = msg->add_field();
    f->set_name("field");
    f->set_number(1);
    f->set_type(type);
    f->set_label(label);
    return pool.BuildFile(fdp);
}

const FieldDescriptor* First(const FileDescriptor* file) { return file->message_type(0)->field(0); }

// Build a file with two messages: Inner (one string field) and Outer (one
// message field referencing Inner).
const FileDescriptor* BuildNestedMsg(DescriptorPool& pool, const std::string& tag,
                                     FieldDescriptorProto::Label label) {
    FileDescriptorProto fdp;
    fdp.set_name("test_" + tag + ".proto");
    fdp.set_syntax("proto3");

    auto* inner = fdp.add_message_type();
    inner->set_name("Inner");
    auto* f1 = inner->add_field();
    f1->set_name("value");
    f1->set_number(1);
    f1->set_type(FieldDescriptorProto::TYPE_STRING);
    f1->set_label(FieldDescriptorProto::LABEL_OPTIONAL);

    auto* outer = fdp.add_message_type();
    outer->set_name("Outer");
    auto* f2 = outer->add_field();
    f2->set_name("inner");
    f2->set_number(1);
    f2->set_type(FieldDescriptorProto::TYPE_MESSAGE);
    f2->set_type_name(".Inner");
    f2->set_label(label);

    return pool.BuildFile(fdp);
}

// Build a file with a map<string, int32> field.
const FileDescriptor* BuildMapField(DescriptorPool& pool, const std::string& tag,
                                    FieldDescriptorProto::Type val_type) {
    FileDescriptorProto fdp;
    fdp.set_name("test_" + tag + ".proto");
    fdp.set_syntax("proto3");

    auto* msg = fdp.add_message_type();
    msg->set_name("Msg");

    // Synthetic MapEntry
    auto* entry = msg->add_nested_type();
    entry->set_name("FieldEntry");
    entry->mutable_options()->set_map_entry(true);
    auto* kf = entry->add_field();
    kf->set_name("key");
    kf->set_number(1);
    kf->set_type(FieldDescriptorProto::TYPE_STRING);
    kf->set_label(FieldDescriptorProto::LABEL_OPTIONAL);
    auto* vf = entry->add_field();
    vf->set_name("value");
    vf->set_number(2);
    vf->set_type(val_type);
    vf->set_label(FieldDescriptorProto::LABEL_OPTIONAL);

    auto* f = msg->add_field();
    f->set_name("field");
    f->set_number(1);
    f->set_type(FieldDescriptorProto::TYPE_MESSAGE);
    f->set_type_name("FieldEntry");
    f->set_label(FieldDescriptorProto::LABEL_REPEATED);

    return pool.BuildFile(fdp);
}

// Build a recursive message: TreeNode has repeated TreeNode children.
const FileDescriptor* BuildRecursiveMsg(DescriptorPool& pool) {
    FileDescriptorProto fdp;
    fdp.set_name("test_recursive.proto");
    fdp.set_syntax("proto3");

    auto* msg = fdp.add_message_type();
    msg->set_name("TreeNode");
    auto* f1 = msg->add_field();
    f1->set_name("value");
    f1->set_number(1);
    f1->set_type(FieldDescriptorProto::TYPE_STRING);
    f1->set_label(FieldDescriptorProto::LABEL_OPTIONAL);
    auto* f2 = msg->add_field();
    f2->set_name("children");
    f2->set_number(2);
    f2->set_type(FieldDescriptorProto::TYPE_MESSAGE);
    f2->set_type_name(".TreeNode");
    f2->set_label(FieldDescriptorProto::LABEL_REPEATED);

    return pool.BuildFile(fdp);
}

// Build a message with a real oneof (not synthetic proto3 optional).
const FileDescriptor* BuildOneofMsg(DescriptorPool& pool) {
    FileDescriptorProto fdp;
    fdp.set_name("test_oneof.proto");
    fdp.set_syntax("proto3");

    auto* msg = fdp.add_message_type();
    msg->set_name("Msg");

    auto* oo = msg->add_oneof_decl();
    oo->set_name("payload");

    auto* f1 = msg->add_field();
    f1->set_name("text");
    f1->set_number(1);
    f1->set_type(FieldDescriptorProto::TYPE_STRING);
    f1->set_label(FieldDescriptorProto::LABEL_OPTIONAL);
    f1->set_oneof_index(0);

    auto* f2 = msg->add_field();
    f2->set_name("code");
    f2->set_number(2);
    f2->set_type(FieldDescriptorProto::TYPE_INT32);
    f2->set_label(FieldDescriptorProto::LABEL_OPTIONAL);
    f2->set_oneof_index(0);

    return pool.BuildFile(fdp);
}

}  // namespace

// ===========================================================================
// Scalar type mappings (same as before, adapted to new struct)
// ===========================================================================

TEST(TypeMapperTest, MapFieldBool) {
    DescriptorPool pool;
    auto m = MapField(First(BuildSingleField(pool, "bool", FieldDescriptorProto::TYPE_BOOL)));
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->kind, FieldKind::SCALAR);
    EXPECT_EQ(m->scalar.arrow_type_expr, "arrow::boolean()");
    EXPECT_EQ(m->scalar.storage_type, "bool");
    EXPECT_EQ(m->scalar.builder_type, "arrow::BooleanBuilder");
    EXPECT_EQ(m->scalar.default_value, "false");
}

TEST(TypeMapperTest, MapFieldInt32) {
    DescriptorPool pool;
    auto m = MapField(First(BuildSingleField(pool, "int32", FieldDescriptorProto::TYPE_INT32)));
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->kind, FieldKind::SCALAR);
    EXPECT_EQ(m->scalar.arrow_type_expr, "arrow::int32()");
    EXPECT_EQ(m->scalar.storage_type, "int32_t");
}

TEST(TypeMapperTest, MapFieldSint32MapsToArrowInt32) {
    DescriptorPool pool;
    auto m = MapField(First(BuildSingleField(pool, "sint32", FieldDescriptorProto::TYPE_SINT32)));
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->scalar.arrow_type_expr, "arrow::int32()");
}

TEST(TypeMapperTest, MapFieldInt64) {
    DescriptorPool pool;
    auto m = MapField(First(BuildSingleField(pool, "int64", FieldDescriptorProto::TYPE_INT64)));
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->scalar.arrow_type_expr, "arrow::int64()");
}

TEST(TypeMapperTest, MapFieldUint32) {
    DescriptorPool pool;
    auto m = MapField(First(BuildSingleField(pool, "uint32", FieldDescriptorProto::TYPE_UINT32)));
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->scalar.arrow_type_expr, "arrow::uint32()");
}

TEST(TypeMapperTest, MapFieldUint64) {
    DescriptorPool pool;
    auto m = MapField(First(BuildSingleField(pool, "uint64", FieldDescriptorProto::TYPE_UINT64)));
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->scalar.arrow_type_expr, "arrow::uint64()");
}

TEST(TypeMapperTest, MapFieldFloat) {
    DescriptorPool pool;
    auto m = MapField(First(BuildSingleField(pool, "float", FieldDescriptorProto::TYPE_FLOAT)));
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->scalar.arrow_type_expr, "arrow::float32()");
}

TEST(TypeMapperTest, MapFieldDouble) {
    DescriptorPool pool;
    auto m = MapField(First(BuildSingleField(pool, "double", FieldDescriptorProto::TYPE_DOUBLE)));
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->scalar.arrow_type_expr, "arrow::float64()");
}

TEST(TypeMapperTest, MapFieldString) {
    DescriptorPool pool;
    auto m = MapField(First(BuildSingleField(pool, "string", FieldDescriptorProto::TYPE_STRING)));
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->scalar.arrow_type_expr, "arrow::utf8()");
    EXPECT_EQ(m->scalar.storage_type, "std::string");
    EXPECT_EQ(m->scalar.param_type, "std::string_view");
}

TEST(TypeMapperTest, MapFieldBytes) {
    DescriptorPool pool;
    auto m = MapField(First(BuildSingleField(pool, "bytes", FieldDescriptorProto::TYPE_BYTES)));
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->scalar.arrow_type_expr, "arrow::binary()");
}

TEST(TypeMapperTest, MapFieldEnumMapsToInt32) {
    DescriptorPool pool;
    FileDescriptorProto fdp;
    fdp.set_name("test_enum.proto");
    fdp.set_syntax("proto3");
    auto* e = fdp.add_enum_type();
    e->set_name("Color");
    e->add_value()->set_name("RED");
    auto* msg = fdp.add_message_type();
    msg->set_name("Msg");
    auto* f = msg->add_field();
    f->set_name("field");
    f->set_number(1);
    f->set_type(FieldDescriptorProto::TYPE_ENUM);
    f->set_label(FieldDescriptorProto::LABEL_OPTIONAL);
    f->set_type_name(".Color");
    auto* file = pool.BuildFile(fdp);
    ASSERT_TRUE(file);

    auto m = MapField(file->message_type(0)->field(0));
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->kind, FieldKind::SCALAR);
    EXPECT_EQ(m->scalar.arrow_type_expr, "arrow::int32()");
}

TEST(TypeMapperTest, ScalarCtorContainsValToken) {
    DescriptorPool pool;
    auto m = MapField(First(BuildSingleField(pool, "tok", FieldDescriptorProto::TYPE_INT32)));
    ASSERT_TRUE(m.has_value());
    EXPECT_NE(m->scalar.scalar_ctor.find("{val}"), std::string::npos);
}

TEST(TypeMapperTest, Proto3NonOptionalFieldIsNotNullable) {
    DescriptorPool pool;
    auto m = MapField(First(BuildSingleField(pool, "nopt", FieldDescriptorProto::TYPE_INT32)));
    ASSERT_TRUE(m.has_value());
    EXPECT_FALSE(m->nullable);
}

// ===========================================================================
// Repeated scalar fields
// ===========================================================================

TEST(TypeMapperTest, RepeatedInt32MapsToRepeatedScalar) {
    DescriptorPool pool;
    auto m = MapField(First(BuildSingleField(pool, "rep_i32", FieldDescriptorProto::TYPE_INT32,
                                             FieldDescriptorProto::LABEL_REPEATED)));
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->kind, FieldKind::REPEATED_SCALAR);
    EXPECT_FALSE(m->nullable);  // repeated fields are never null
    EXPECT_EQ(m->element.arrow_type_expr, "arrow::int32()");
    EXPECT_EQ(m->element.storage_type, "int32_t");
    EXPECT_EQ(m->element.builder_type, "arrow::Int32Builder");
}

TEST(TypeMapperTest, RepeatedStringMapsToRepeatedScalar) {
    DescriptorPool pool;
    auto m = MapField(First(BuildSingleField(pool, "rep_str", FieldDescriptorProto::TYPE_STRING,
                                             FieldDescriptorProto::LABEL_REPEATED)));
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->kind, FieldKind::REPEATED_SCALAR);
    EXPECT_EQ(m->element.arrow_type_expr, "arrow::utf8()");
    EXPECT_EQ(m->element.builder_type, "arrow::StringBuilder");
}

// ===========================================================================
// Nested message fields (struct)
// ===========================================================================

TEST(TypeMapperTest, SingularMessageMapsToStruct) {
    DescriptorPool pool;
    auto* file = BuildNestedMsg(pool, "struct", FieldDescriptorProto::LABEL_OPTIONAL);
    ASSERT_TRUE(file);
    // message_type(1) is Outer; field(0) is "inner"
    auto m = MapField(file->message_type(1)->field(0));
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->kind, FieldKind::STRUCT);
    EXPECT_EQ(m->nested_class, "Inner");
}

TEST(TypeMapperTest, NonOptionalStructIsNotNullable) {
    DescriptorPool pool;
    auto* file = BuildNestedMsg(pool, "struct_nonnull", FieldDescriptorProto::LABEL_OPTIONAL);
    ASSERT_TRUE(file);
    auto m = MapField(file->message_type(1)->field(0));
    ASSERT_TRUE(m.has_value());
    // In proto3 without 'optional' keyword, has_optional_keyword() is false.
    EXPECT_FALSE(m->nullable);
}

TEST(TypeMapperTest, RepeatedMessageMapsToRepeatedStruct) {
    DescriptorPool pool;
    auto* file = BuildNestedMsg(pool, "rep_struct", FieldDescriptorProto::LABEL_REPEATED);
    ASSERT_TRUE(file);
    auto m = MapField(file->message_type(1)->field(0));
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->kind, FieldKind::REPEATED_STRUCT);
    EXPECT_FALSE(m->nullable);
    EXPECT_EQ(m->nested_class, "Inner");
}

// ===========================================================================
// Map fields
// ===========================================================================

TEST(TypeMapperTest, MapStringInt32MapsToMap) {
    DescriptorPool pool;
    auto* file = BuildMapField(pool, "map_str_i32", FieldDescriptorProto::TYPE_INT32);
    ASSERT_TRUE(file);
    auto m = MapField(file->message_type(0)->field(0));
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->kind, FieldKind::MAP);
    EXPECT_FALSE(m->nullable);
    EXPECT_EQ(m->map_key.arrow_type_expr, "arrow::utf8()");
    EXPECT_EQ(m->map_key.storage_type, "std::string");
    EXPECT_FALSE(m->map_value_is_message);
    EXPECT_EQ(m->map_value.arrow_type_expr, "arrow::int32()");
}

TEST(TypeMapperTest, MapEmitsWarningAboutComputeSupport) {
    DescriptorPool pool;
    auto* file = BuildMapField(pool, "map_warn", FieldDescriptorProto::TYPE_INT32);
    ASSERT_TRUE(file);
    auto m = MapField(file->message_type(0)->field(0));
    ASSERT_TRUE(m.has_value());
    EXPECT_FALSE(m->warning.empty());
    EXPECT_NE(m->warning.find("compute"), std::string::npos);
}

// ===========================================================================
// Oneof fields
// ===========================================================================

TEST(TypeMapperTest, OneofFieldReturnsNullopt) {
    DescriptorPool pool;
    auto* file = BuildOneofMsg(pool);
    ASSERT_TRUE(file);
    // Both fields are in the oneof
    EXPECT_FALSE(MapField(file->message_type(0)->field(0)).has_value());
    EXPECT_FALSE(MapField(file->message_type(0)->field(1)).has_value());
}

TEST(TypeMapperTest, UnsupportedReasonOneofMentionsOneofName) {
    DescriptorPool pool;
    auto* file = BuildOneofMsg(pool);
    ASSERT_TRUE(file);
    auto reason = UnsupportedReason(file->message_type(0)->field(0));
    EXPECT_NE(reason.find("payload"), std::string::npos);
    EXPECT_NE(reason.find("oneof"), std::string::npos);
}

// ===========================================================================
// Recursive messages
// ===========================================================================

TEST(TypeMapperTest, SelfReferencingMessageIsRecursive) {
    DescriptorPool pool;
    auto* file = BuildRecursiveMsg(pool);
    ASSERT_TRUE(file);
    EXPECT_TRUE(IsRecursive(file->message_type(0)));
}

TEST(TypeMapperTest, FlatMessageIsNotRecursive) {
    DescriptorPool pool;
    auto* file = BuildSingleField(pool, "flat", FieldDescriptorProto::TYPE_INT32);
    ASSERT_TRUE(file);
    EXPECT_FALSE(IsRecursive(file->message_type(0)));
}

TEST(TypeMapperTest, FieldReferencingRecursiveMessageReturnsNullopt) {
    DescriptorPool pool;

    // Build two messages: TreeNode (recursive) and Wrapper (has a TreeNode field)
    FileDescriptorProto fdp;
    fdp.set_name("test_rec_field.proto");
    fdp.set_syntax("proto3");

    auto* tree = fdp.add_message_type();
    tree->set_name("TreeNode");
    auto* tf1 = tree->add_field();
    tf1->set_name("value");
    tf1->set_number(1);
    tf1->set_type(FieldDescriptorProto::TYPE_STRING);
    tf1->set_label(FieldDescriptorProto::LABEL_OPTIONAL);
    auto* tf2 = tree->add_field();
    tf2->set_name("children");
    tf2->set_number(2);
    tf2->set_type(FieldDescriptorProto::TYPE_MESSAGE);
    tf2->set_type_name(".TreeNode");
    tf2->set_label(FieldDescriptorProto::LABEL_REPEATED);

    auto* wrapper = fdp.add_message_type();
    wrapper->set_name("Wrapper");
    auto* wf = wrapper->add_field();
    wf->set_name("tree");
    wf->set_number(1);
    wf->set_type(FieldDescriptorProto::TYPE_MESSAGE);
    wf->set_type_name(".TreeNode");
    wf->set_label(FieldDescriptorProto::LABEL_OPTIONAL);

    auto* file = pool.BuildFile(fdp);
    ASSERT_TRUE(file);
    EXPECT_FALSE(MapField(file->message_type(1)->field(0)).has_value());
}

// ===========================================================================
// Nesting depth
// ===========================================================================

TEST(TypeMapperTest, FlatMessageHasDepthZero) {
    DescriptorPool pool;
    auto* file = BuildSingleField(pool, "flat_depth", FieldDescriptorProto::TYPE_INT32);
    ASSERT_TRUE(file);
    EXPECT_EQ(NestingDepth(file->message_type(0)), 0);
}

TEST(TypeMapperTest, OneLevelOfNestingHasDepthOne) {
    DescriptorPool pool;
    auto* file = BuildNestedMsg(pool, "depth1", FieldDescriptorProto::LABEL_OPTIONAL);
    ASSERT_TRUE(file);
    // Outer ->Inner (depth 1)
    EXPECT_EQ(NestingDepth(file->message_type(1)), 1);
}

// ===========================================================================
// ClassName
// ===========================================================================

TEST(TypeMapperTest, ClassNameTopLevelMessage) {
    DescriptorPool pool;
    auto* file = BuildSingleField(pool, "cls", FieldDescriptorProto::TYPE_INT32);
    ASSERT_TRUE(file);
    EXPECT_EQ(ClassName(file->message_type(0)), "Msg");
}

TEST(TypeMapperTest, ClassNameNestedMessageUsesUnderscoreSeparator) {
    DescriptorPool pool;
    FileDescriptorProto fdp;
    fdp.set_name("test_nested_cls.proto");
    fdp.set_syntax("proto3");
    auto* outer = fdp.add_message_type();
    outer->set_name("Outer");
    auto* inner = outer->add_nested_type();
    inner->set_name("Inner");
    auto* f = inner->add_field();
    f->set_name("x");
    f->set_number(1);
    f->set_type(FieldDescriptorProto::TYPE_INT32);
    f->set_label(FieldDescriptorProto::LABEL_OPTIONAL);

    auto* file = pool.BuildFile(fdp);
    ASSERT_TRUE(file);
    EXPECT_EQ(ClassName(file->message_type(0)->nested_type(0)), "Outer_Inner");
}

// ===========================================================================
// Cross-file message references
// ===========================================================================

namespace {

// Builds a "dep" file (dependency) containing a single message named DepMsg
// with one int32 field, optionally under the given proto package.
const FileDescriptor* BuildDepFile(DescriptorPool& pool, const std::string& filename,
                                   const std::string& package = "") {
    FileDescriptorProto dep;
    dep.set_name(filename);
    dep.set_syntax("proto3");
    if (!package.empty()) dep.set_package(package);
    auto* msg = dep.add_message_type();
    msg->set_name("DepMsg");
    auto* f = msg->add_field();
    f->set_name("value");
    f->set_number(1);
    f->set_type(FieldDescriptorProto::TYPE_INT32);
    f->set_label(FieldDescriptorProto::LABEL_OPTIONAL);
    return pool.BuildFile(dep);
}

// Builds a "consumer" file that has one field referencing DepMsg from dep_filename.
// field_label selects singular (LABEL_OPTIONAL) or repeated (LABEL_REPEATED).
// Returns the consumer FileDescriptor; the field is on message ConsumerMsg.
const FileDescriptor* BuildConsumerFile(DescriptorPool& pool, const std::string& consumer_filename,
                                        const std::string& dep_filename,
                                        const std::string& dep_full_type,
                                        FieldDescriptorProto::Label label,
                                        const std::string& package = "") {
    FileDescriptorProto fdp;
    fdp.set_name(consumer_filename);
    fdp.set_syntax("proto3");
    if (!package.empty()) fdp.set_package(package);
    fdp.add_dependency(dep_filename);

    auto* msg = fdp.add_message_type();
    msg->set_name("ConsumerMsg");
    auto* f = msg->add_field();
    f->set_name("dep_field");
    f->set_number(1);
    f->set_type(FieldDescriptorProto::TYPE_MESSAGE);
    f->set_type_name(dep_full_type);
    f->set_label(label);

    return pool.BuildFile(fdp);
}

}  // namespace

TEST(TypeMapperTest, CrossFileSingularMessageSamePackage) {
    DescriptorPool pool;
    ASSERT_TRUE(BuildDepFile(pool, "dep_same_pkg.proto", "mypkg"));

    auto* consumer =
        BuildConsumerFile(pool, "consumer_same_pkg.proto", "dep_same_pkg.proto", ".mypkg.DepMsg",
                          FieldDescriptorProto::LABEL_OPTIONAL, "mypkg");
    ASSERT_TRUE(consumer);

    const auto* fd = consumer->message_type(0)->field(0);
    auto m = MapField(fd);
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->kind, FieldKind::STRUCT);
    // Same package ->no global qualification needed; nested_header must be set.
    EXPECT_EQ(m->nested_class, "DepMsg");
    EXPECT_EQ(m->nested_header, "dep_same_pkg.fletcher.pb.h");
}

TEST(TypeMapperTest, CrossFileSingularMessageDifferentPackages) {
    DescriptorPool pool;
    ASSERT_TRUE(BuildDepFile(pool, "dep_other_pkg.proto", "other.pkg"));

    auto* consumer =
        BuildConsumerFile(pool, "consumer_diff_pkg.proto", "dep_other_pkg.proto",
                          ".other.pkg.DepMsg", FieldDescriptorProto::LABEL_OPTIONAL, "my.pkg");
    ASSERT_TRUE(consumer);

    const auto* fd = consumer->message_type(0)->field(0);
    auto m = MapField(fd);
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->kind, FieldKind::STRUCT);
    // Different package ->globally qualified.
    EXPECT_EQ(m->nested_class, "::fletcher_gen::other::pkg::DepMsg");
    EXPECT_EQ(m->nested_header, "dep_other_pkg.fletcher.pb.h");
}

TEST(TypeMapperTest, CrossFileSingularMessageNoPackageDep) {
    DescriptorPool pool;
    ASSERT_TRUE(BuildDepFile(pool, "dep_no_pkg.proto", ""));

    auto* consumer = BuildConsumerFile(pool, "consumer_no_dep_pkg.proto", "dep_no_pkg.proto",
                                       ".DepMsg", FieldDescriptorProto::LABEL_OPTIONAL, "my.pkg");
    ASSERT_TRUE(consumer);

    const auto* fd = consumer->message_type(0)->field(0);
    auto m = MapField(fd);
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->nested_class, "::fletcher_gen::DepMsg");
    EXPECT_EQ(m->nested_header, "dep_no_pkg.fletcher.pb.h");
}

TEST(TypeMapperTest, CrossFileRepeatedMessageSucceeds) {
    DescriptorPool pool;
    ASSERT_TRUE(BuildDepFile(pool, "dep_repeated.proto", "ext"));

    auto* consumer = BuildConsumerFile(pool, "consumer_repeated.proto", "dep_repeated.proto",
                                       ".ext.DepMsg", FieldDescriptorProto::LABEL_REPEATED, "mine");
    ASSERT_TRUE(consumer);

    const auto* fd = consumer->message_type(0)->field(0);
    auto m = MapField(fd);
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->kind, FieldKind::REPEATED_STRUCT);
    EXPECT_EQ(m->nested_class, "::fletcher_gen::ext::DepMsg");
    EXPECT_EQ(m->nested_header, "dep_repeated.fletcher.pb.h");
}

TEST(TypeMapperTest, SameFileMessageHasEmptyNestedHeader) {
    DescriptorPool pool;
    auto* file = BuildNestedMsg(pool, "same_file_header", FieldDescriptorProto::LABEL_OPTIONAL);
    ASSERT_TRUE(file);

    // Outer's "inner" field references Inner from the same file.
    const auto* fd = file->message_type(1)->field(0);
    auto m = MapField(fd);
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->kind, FieldKind::STRUCT);
    EXPECT_TRUE(m->nested_header.empty());
}

TEST(TypeMapperTest, CrossFileMapWithMessageValueSucceeds) {
    DescriptorPool pool;
    ASSERT_TRUE(BuildDepFile(pool, "dep_map_val.proto", "ext"));

    // Build a file with map<string, ext.DepMsg>
    FileDescriptorProto fdp;
    fdp.set_name("consumer_map_val.proto");
    fdp.set_syntax("proto3");
    fdp.set_package("mine");
    fdp.add_dependency("dep_map_val.proto");

    auto* msg = fdp.add_message_type();
    msg->set_name("ConsumerMsg");

    // Synthetic map entry
    auto* entry = msg->add_nested_type();
    entry->set_name("DepFieldEntry");
    entry->mutable_options()->set_map_entry(true);
    auto* key_f = entry->add_field();
    key_f->set_name("key");
    key_f->set_number(1);
    key_f->set_type(FieldDescriptorProto::TYPE_STRING);
    key_f->set_label(FieldDescriptorProto::LABEL_OPTIONAL);
    auto* val_f = entry->add_field();
    val_f->set_name("value");
    val_f->set_number(2);
    val_f->set_type(FieldDescriptorProto::TYPE_MESSAGE);
    val_f->set_type_name(".ext.DepMsg");
    val_f->set_label(FieldDescriptorProto::LABEL_OPTIONAL);

    auto* map_field = msg->add_field();
    map_field->set_name("dep_field");
    map_field->set_number(1);
    map_field->set_type(FieldDescriptorProto::TYPE_MESSAGE);
    map_field->set_type_name(".mine.ConsumerMsg.DepFieldEntry");
    map_field->set_label(FieldDescriptorProto::LABEL_REPEATED);

    auto* consumer = pool.BuildFile(fdp);
    ASSERT_TRUE(consumer);

    const auto* fd = consumer->message_type(0)->field(0);
    auto m = MapField(fd);
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->kind, FieldKind::MAP);
    EXPECT_TRUE(m->map_value_is_message);
    EXPECT_EQ(m->map_value_class, "::fletcher_gen::ext::DepMsg");
    EXPECT_EQ(m->map_value_header, "dep_map_val.fletcher.pb.h");
}

TEST(TypeMapperTest, CrossFileReferenceIsNoLongerUnsupported) {
    DescriptorPool pool;
    ASSERT_TRUE(BuildDepFile(pool, "dep_reason.proto", "ext"));

    auto* consumer = BuildConsumerFile(pool, "consumer_reason.proto", "dep_reason.proto",
                                       ".ext.DepMsg", FieldDescriptorProto::LABEL_OPTIONAL, "mine");
    ASSERT_TRUE(consumer);

    // MapField should now succeed for cross-file references.
    const auto* fd = consumer->message_type(0)->field(0);
    EXPECT_TRUE(MapField(fd).has_value());
}

// ===========================================================================
// DICT-1 forcing test: the (fletcher.dictionary) option surface + typed reader
// ===========================================================================
// SUITE NAME: `TypeMapperTest` is retained verbatim for tracker fidelity — this
// is round DICT's named forcing test — but type_mapper.{hpp,cpp} is NOT touched
// by DICT-1. The code actually under test is `option_reader` (the typed
// (fletcher.dictionary) reader) and `ir` (fact population + message-level
// flatten propagation).
//
// Two harness routes, both pre-existing in-tree patterns:
//   (1) SOURCE TEXT — compile the SHIPPED protoc/include/fletcher/options.proto
//       plus test protos that use it (proto_text_pool.hpp). The realistic route:
//       BuildFile's option interpreter resolves (fletcher.dictionary) against
//       the pool and stores it in the linked FieldOptions' UnknownFieldSet,
//       because the C++ class does not know the extension. Using the shipped
//       file means this test also asserts spec section 2 / locked #2 (number
//       50001, sub-field names, enum value names).
//   (2) UNKNOWN-FIELD INJECTION — mutate the FieldDescriptorProto's options
//       before BuildFile (test_ir.cpp:56-61 pattern). Required for the byte
//       sequences .proto source text cannot express: an undeclared enum number,
//       a truncated payload, a wrong wire type, and the extension-absent pool.

namespace {

using fletcher::ir::DictionaryIndexKind;
using fletcher::test::AddFletcherOptions;
using fletcher::test::ProtoTextPool;

// Route 2 primitives.
void InjectDictBytes(FieldDescriptorProto* f, const std::string& payload) {
    auto* opts = f->mutable_options();
    opts->GetReflection()->MutableUnknownFields(opts)->AddLengthDelimited(50001, payload);
}

void InjectBytesAt(FieldDescriptorProto* f, int number, const std::string& payload) {
    auto* opts = f->mutable_options();
    opts->GetReflection()->MutableUnknownFields(opts)->AddLengthDelimited(number, payload);
}

void InjectDictVarint(FieldDescriptorProto* f, uint64_t value) {
    auto* opts = f->mutable_options();
    opts->GetReflection()->MutableUnknownFields(opts)->AddVarint(50001, value);
}

// The serialized payload of a length-delimited unknown field on `field`'s
// options, or nullopt. Used to prove two sub-cases really do carry IDENTICAL
// bytes, so the only difference between them is which pool declares the number.
std::optional<std::string> UnknownPayloadAt(const FieldDescriptor* field, int number) {
    const auto& opts = field->options();
    const auto& unknown = opts.GetReflection()->GetUnknownFields(opts);
    for (int i = 0; i < unknown.field_count(); ++i) {
        const auto& uf = unknown.field(i);
        if (uf.number() == number &&
            uf.type() == google::protobuf::UnknownField::TYPE_LENGTH_DELIMITED) {
            return uf.length_delimited();
        }
    }
    return std::nullopt;
}

const FieldDescriptor* FieldByName(const FileDescriptor* file, const std::string& msg,
                                   const std::string& field) {
    const Descriptor* d = file->FindMessageTypeByName(msg);
    if (d == nullptr) return nullptr;
    return d->FindFieldByName(field);
}

FileDescriptorProto InjectedFile(const std::string& name) {
    FileDescriptorProto fdp;
    fdp.set_name(name);
    fdp.set_syntax("proto3");
    auto* msg = fdp.add_message_type();
    msg->set_name("Inj");
    return fdp;
}

FieldDescriptorProto* InjectedField(FileDescriptorProto* fdp, const std::string& name, int number) {
    auto* f = fdp->mutable_message_type(0)->add_field();
    f->set_name(name);
    f->set_number(number);
    f->set_type(FieldDescriptorProto::TYPE_STRING);
    f->set_label(FieldDescriptorProto::LABEL_OPTIONAL);
    return f;
}

// `DictionaryOptions{ index_type: INT16, ordered: true }` on the wire. Also the
// EXACT serialization of `zz.Foreign{ x: 2, y: true }` below — protobuf's wire
// format carries no type identity, which is why the reader's answer must (and
// does) hinge on which pool DECLARES 50001, not on the bytes.
constexpr const char* kI16OrderedBytes = "\x08\x02\x10\x01";
constexpr int kI16OrderedLen = 4;

// The realistic (route 1) fixture.
constexpr const char* kDictSchema = R"(
syntax = "proto3";
package dt;
import "fletcher/options.proto";
import "google/protobuf/wrappers.proto";

message Ev {
  string plain  = 1;
  string empty  = 2 [(fletcher.dictionary) = {}];
  string unspec = 3 [(fletcher.dictionary) = { index_type: DICTIONARY_INDEX_UNSPECIFIED }];
  string i8     = 4 [(fletcher.dictionary) = { index_type: DICTIONARY_INDEX_INT8 }];
  string i16    = 5 [(fletcher.dictionary) = { index_type: DICTIONARY_INDEX_INT16 }];
  string i32    = 6 [(fletcher.dictionary) = { index_type: DICTIONARY_INDEX_INT32 }];
  string i64    = 7 [(fletcher.dictionary) = { index_type: DICTIONARY_INDEX_INT64 }];
  string ord    = 8 [(fletcher.dictionary) = { ordered: true }];
  string ord16  = 9 [(fletcher.dictionary) = { index_type: DICTIONARY_INDEX_INT16,
                                               ordered: true }];
}

message WrapOuter {
  option (fletcher.flatten) = true;
  string value = 1;
}

message WrapInner {
  option (fletcher.flatten) = true;
  string value = 1 [(fletcher.dictionary) = { index_type: DICTIONARY_INDEX_INT8 }];
}

message WrapRepeated {
  option (fletcher.flatten) = true;
  repeated string values = 1;
}

message Holder {
  WrapOuter on_wrapper = 1 [(fletcher.dictionary) = { index_type: DICTIONARY_INDEX_INT64,
                                                      ordered: true }];
  WrapInner on_inner = 2;
  WrapInner on_both = 3 [(fletcher.dictionary) = { index_type: DICTIONARY_INDEX_INT64 }];
  WrapOuter on_none = 4;
  // wrapper whose single inner field is `repeated`: the propagated fact must
  // land on the LIST *and* its element (SF-2 placement rule).
  WrapRepeated on_rep_wrapper = 5 [(fletcher.dictionary) = {
                                       index_type: DICTIONARY_INDEX_INT16 }];
  // flattened REPEATED shapes (SF-1): outer-declared is carried, inner-declared
  // is dropped — today's behaviour, deliberately deferred to DICT-2.
  repeated WrapOuter rep_outer_declared = 6 [(fletcher.dictionary) = {
                                                 index_type: DICTIONARY_INDEX_INT16 }];
  repeated WrapInner rep_inner_declared = 7;
  // RR-1: `repeated <wrapper of a repeated scalar>` -> List<List<Scalar>>. The
  // INTERMEDIATE list level is produced by MakeListOf and gets DEFAULT facts.
  repeated WrapRepeated rep_nested = 8 [(fletcher.dictionary) = {
                                            index_type: DICTIONARY_INDEX_INT16 }];
}

message Pair {
  string a = 1;
  string b = 2;
}

message Rec {
  Pair p = 1 [(fletcher.flatten_field) = true,
              (fletcher.dictionary) = { index_type: DICTIONARY_INDEX_INT16 }];
  string s = 2 [(fletcher.flatten_field) = true,
                (fletcher.dictionary) = { index_type: DICTIONARY_INDEX_INT8 }];
}

message Wkt {
  google.protobuf.StringValue s = 1 [(fletcher.dictionary) = {
                                        index_type: DICTIONARY_INDEX_INT16 }];
}

// Non-scalar / non-singular carriers (SF-4).
message Shapes {
  repeated string tags = 1 [(fletcher.dictionary) = { index_type: DICTIONARY_INDEX_INT16 }];
  map<string, string> labels = 2 [(fletcher.dictionary) = {
                                      index_type: DICTIONARY_INDEX_INT8 }];
  oneof choice {
    string oc = 3 [(fletcher.dictionary) = { index_type: DICTIONARY_INDEX_INT64 }];
  }
}

message Member {
  string dict  = 1 [(fletcher.dictionary) = { index_type: DICTIONARY_INDEX_INT16 }];
  string plain = 2;
}

message MemberHolder {
  Member m = 1;
}
)";

// A FOREIGN message-typed FieldOptions extension, declared at `number`. The
// number is substituted so the SAME declaration + SAME payload bytes can be
// tested twice: at 50001 in a pool that does NOT know fletcher/options.proto,
// and at 60200 in a pool that DOES. That pair is what makes the reader's
// scoping observable in-suite rather than only under mutation.
std::string ForeignSchema(int number) {
    return R"(
syntax = "proto3";
package zz;
import "google/protobuf/descriptor.proto";

message Foreign { int32 x = 1; bool y = 2; }
extend google.protobuf.FieldOptions { Foreign foreign = )" +
           std::to_string(number) + R"(; }

message Msg {
  string f = 1 [(zz.foreign) = { x: 2, y: true }];
}
)";
}

}  // namespace

TEST(TypeMapperTest, ReadsDictionaryOption) {
    // ---- route 1: the shipped option surface + a realistic schema ----------
    ProtoTextPool pool;
    ASSERT_TRUE(pool.seeded());
    ASSERT_NE(AddFletcherOptions(pool), nullptr);
    ASSERT_NE(pool.AddLinked(google::protobuf::StringValue::GetDescriptor()->file()), nullptr);
    const FileDescriptor* schema = pool.Add("dict_schema.proto", kDictSchema);
    ASSERT_NE(schema, nullptr);

    // -- absence ------------------------------------------------------------
    {
        SCOPED_TRACE("absence -> no dictionary");
        const FieldDescriptor* f = FieldByName(schema, "Ev", "plain");
        ASSERT_NE(f, nullptr);
        EXPECT_FALSE(ReadFieldDictionaryOption(f).has_value());
        EXPECT_FALSE(HasFieldDictionary(f));
    }

    // -- presence + index-type round trip (decoded by enum SYMBOL name) -----
    struct IndexCase {
        const char* field;
        DictionaryIndexKind kind;
    };
    const IndexCase kIndexCases[] = {
        {"empty", DictionaryIndexKind::INT32},   // = {} -> defaults
        {"unspec", DictionaryIndexKind::INT32},  // UNSPECIFIED resolves to int32
        {"i8", DictionaryIndexKind::INT8},      {"i16", DictionaryIndexKind::INT16},
        {"i32", DictionaryIndexKind::INT32},    {"i64", DictionaryIndexKind::INT64},
    };
    for (const IndexCase& c : kIndexCases) {
        SCOPED_TRACE(std::string("index round trip: Ev.") + c.field);
        const FieldDescriptor* f = FieldByName(schema, "Ev", c.field);
        ASSERT_NE(f, nullptr);
        EXPECT_TRUE(HasFieldDictionary(f));
        auto d = ReadFieldDictionaryOption(f);
        ASSERT_TRUE(d.has_value());
        EXPECT_EQ(d->index_kind, c.kind);
        EXPECT_FALSE(d->ordered);
    }

    // -- ordered = true (proto3 omits a defaulted false, so this is the only
    //    thing that proves the `ordered` sub-field is decoded at all) --------
    {
        SCOPED_TRACE("source text: ordered: true");
        auto d = ReadFieldDictionaryOption(FieldByName(schema, "Ev", "ord"));
        ASSERT_TRUE(d.has_value());
        EXPECT_TRUE(d->ordered);
        EXPECT_EQ(d->index_kind, DictionaryIndexKind::INT32);

        auto d16 = ReadFieldDictionaryOption(FieldByName(schema, "Ev", "ord16"));
        ASSERT_TRUE(d16.has_value());
        EXPECT_TRUE(d16->ordered);
        EXPECT_EQ(d16->index_kind, DictionaryIndexKind::INT16);
    }

    // -- the facts land on ir::FieldFacts (locked #5 carrier) ---------------
    {
        SCOPED_TRACE("ir::FieldFacts population");
        ir::IrNode plain = ir::BuildFieldIr(FieldByName(schema, "Ev", "plain"));
        EXPECT_EQ(plain.kind, ir::NodeKind::SCALAR);
        EXPECT_FALSE(plain.facts.dictionary);
        EXPECT_EQ(plain.facts.dictionary_index_kind, DictionaryIndexKind::INT32);
        EXPECT_FALSE(plain.facts.dictionary_ordered);

        ir::IrNode ord16 = ir::BuildFieldIr(FieldByName(schema, "Ev", "ord16"));
        // dictionary is a scalar MODIFIER: no container peer appears.
        EXPECT_EQ(ord16.kind, ir::NodeKind::SCALAR);
        EXPECT_TRUE(ord16.facts.dictionary);
        EXPECT_EQ(ord16.facts.dictionary_index_kind, DictionaryIndexKind::INT16);
        EXPECT_TRUE(ord16.facts.dictionary_ordered);

        ir::IrNode i64 = ir::BuildFieldIr(FieldByName(schema, "Ev", "i64"));
        EXPECT_TRUE(i64.facts.dictionary);
        EXPECT_EQ(i64.facts.dictionary_index_kind, DictionaryIndexKind::INT64);
        EXPECT_FALSE(i64.facts.dictionary_ordered);
    }

    // -- SF-4: repeated / map / oneof / struct-member carriers --------------
    // Pins the PLACEMENT RULE asserted by BaseFacts' comment: the nodes built
    // from the field carries the fact, and gating is on the top-level kind.
    {
        SCOPED_TRACE("repeated scalar: LIST *and* element carry the fact");
        ir::IrNode n = ir::BuildFieldIr(FieldByName(schema, "Shapes", "tags"));
        ASSERT_EQ(n.kind, ir::NodeKind::LIST);
        EXPECT_TRUE(n.facts.dictionary);
        EXPECT_EQ(n.facts.dictionary_index_kind, DictionaryIndexKind::INT16);
        const auto& list = std::get<ir::ListNode>(n.node);
        ASSERT_NE(list.element, nullptr);
        EXPECT_EQ(list.element->kind, ir::NodeKind::SCALAR);
        EXPECT_TRUE(list.element->facts.dictionary);
        EXPECT_EQ(list.element->facts.dictionary_index_kind, DictionaryIndexKind::INT16);
    }
    {
        SCOPED_TRACE("map: the MAP node carries it; key/value nodes do not");
        ir::IrNode n = ir::BuildFieldIr(FieldByName(schema, "Shapes", "labels"));
        ASSERT_EQ(n.kind, ir::NodeKind::MAP);
        EXPECT_TRUE(n.facts.dictionary);
        EXPECT_EQ(n.facts.dictionary_index_kind, DictionaryIndexKind::INT8);
        const auto& mp = std::get<ir::MapNode>(n.node);
        ASSERT_NE(mp.key, nullptr);
        ASSERT_NE(mp.value, nullptr);
        // key/value are built from the synthetic MapEntry's own fields, which
        // carry no option — so a consumer must not look for the fact there.
        EXPECT_FALSE(mp.key->facts.dictionary);
        EXPECT_FALSE(mp.value->facts.dictionary);
    }
    {
        SCOPED_TRACE("oneof member: node is UNSUPPORTED but the fact is still read");
        ir::IrNode n = ir::BuildFieldIr(FieldByName(schema, "Shapes", "oc"));
        EXPECT_EQ(n.kind, ir::NodeKind::UNSUPPORTED);
        EXPECT_TRUE(n.facts.in_real_oneof);
        EXPECT_TRUE(n.facts.dictionary);
        EXPECT_EQ(n.facts.dictionary_index_kind, DictionaryIndexKind::INT64);
    }
    {
        SCOPED_TRACE("struct member: each member carries its own declaration");
        ir::IrNode n = ir::BuildFieldIr(FieldByName(schema, "MemberHolder", "m"));
        ASSERT_EQ(n.kind, ir::NodeKind::STRUCT);
        EXPECT_FALSE(n.facts.dictionary);  // the holder field declares nothing
        const auto& st = std::get<ir::StructNode>(n.node);
        ASSERT_EQ(st.fields.size(), 2u);
        ASSERT_NE(st.fields[0].type, nullptr);
        ASSERT_NE(st.fields[1].type, nullptr);
        EXPECT_TRUE(st.fields[0].type->facts.dictionary);
        EXPECT_EQ(st.fields[0].type->facts.dictionary_index_kind, DictionaryIndexKind::INT16);
        EXPECT_FALSE(st.fields[1].type->facts.dictionary);
    }

    // -- message-level (fletcher.flatten) propagation -----------------------
    {
        SCOPED_TRACE("flatten wrapper: option on the wrapper field reaches the inner node");
        ir::IrNode n = ir::BuildFieldIr(FieldByName(schema, "Holder", "on_wrapper"));
        EXPECT_EQ(n.kind, ir::NodeKind::SCALAR);
        EXPECT_TRUE(n.facts.dictionary);
        EXPECT_EQ(n.facts.dictionary_index_kind, DictionaryIndexKind::INT64);
        EXPECT_TRUE(n.facts.dictionary_ordered);
    }
    {
        SCOPED_TRACE("flatten wrapper: option on the inner field");
        ir::IrNode n = ir::BuildFieldIr(FieldByName(schema, "Holder", "on_inner"));
        EXPECT_TRUE(n.facts.dictionary);
        EXPECT_EQ(n.facts.dictionary_index_kind, DictionaryIndexKind::INT8);
    }
    {
        SCOPED_TRACE("flatten wrapper: BOTH declared with different index types -> leaf wins");
        ir::IrNode n = ir::BuildFieldIr(FieldByName(schema, "Holder", "on_both"));
        EXPECT_TRUE(n.facts.dictionary);
        EXPECT_EQ(n.facts.dictionary_index_kind, DictionaryIndexKind::INT8);  // leaf, not INT64
        EXPECT_FALSE(n.facts.dictionary_ordered);
    }
    {
        SCOPED_TRACE("flatten wrapper: neither declares -> no dictionary");
        ir::IrNode n = ir::BuildFieldIr(FieldByName(schema, "Holder", "on_none"));
        EXPECT_FALSE(n.facts.dictionary);
    }
    {
        // SF-2: propagation must not land the fact on the LIST alone — that would
        // disagree with BuildRepeatedScalarOrEnum, which writes both.
        SCOPED_TRACE("flatten wrapper over a repeated inner field: LIST *and* element");
        ir::IrNode n = ir::BuildFieldIr(FieldByName(schema, "Holder", "on_rep_wrapper"));
        ASSERT_EQ(n.kind, ir::NodeKind::LIST);
        EXPECT_TRUE(n.facts.dictionary);
        EXPECT_EQ(n.facts.dictionary_index_kind, DictionaryIndexKind::INT16);
        const auto& list = std::get<ir::ListNode>(n.node);
        ASSERT_NE(list.element, nullptr);
        EXPECT_TRUE(list.element->facts.dictionary);
        EXPECT_EQ(list.element->facts.dictionary_index_kind, DictionaryIndexKind::INT16);
    }

    // -- SF-1: the flattened-REPEATED path, pinned as it behaves TODAY.
    //    Outer-declared is carried on the outermost list and the leaf, NOT on
    //    intermediate levels (see the RR-1 block below);
    //    INNER-declared is dropped, because this path never calls
    //    BuildFieldIr(inner). Deliberately deferred to DICT-2 (see the comment on
    //    BuildFlattenedRepeated and spec 7.1); closing the gap flips this
    //    sub-case rather than silently changing behaviour.
    {
        SCOPED_TRACE("flattened repeated: outer-declared dictionary IS carried");
        ir::IrNode n = ir::BuildFieldIr(FieldByName(schema, "Holder", "rep_outer_declared"));
        ASSERT_EQ(n.kind, ir::NodeKind::LIST);
        EXPECT_TRUE(n.facts.dictionary);
        EXPECT_EQ(n.facts.dictionary_index_kind, DictionaryIndexKind::INT16);
        const auto& list = std::get<ir::ListNode>(n.node);
        ASSERT_NE(list.element, nullptr);
        EXPECT_TRUE(list.element->facts.dictionary);
    }
    {
        // RR-1: the placement rule is NOT "every node built from the field". Only
        // the OUTERMOST node and the LEAF get BaseFacts(field) here; every
        // intermediate list level comes from MakeListOf and keeps DEFAULT facts,
        // so a consumer walking down finds -> loses -> refinds the fact. Pinned so
        // the qualified rule (spec 7.1) is guarded, not merely asserted.
        SCOPED_TRACE("flattened repeated nested list: outer true / intermediate FALSE / leaf true");
        ir::IrNode n = ir::BuildFieldIr(FieldByName(schema, "Holder", "rep_nested"));
        ASSERT_EQ(n.kind, ir::NodeKind::LIST);
        EXPECT_TRUE(n.facts.dictionary);
        EXPECT_EQ(n.facts.dictionary_index_kind, DictionaryIndexKind::INT16);
        const auto& outer_list = std::get<ir::ListNode>(n.node);
        ASSERT_NE(outer_list.element, nullptr);
        ASSERT_EQ(outer_list.element->kind, ir::NodeKind::LIST);
        EXPECT_FALSE(outer_list.element->facts.dictionary)
            << "intermediate list level carries DEFAULT facts";
        const auto& mid_list = std::get<ir::ListNode>(outer_list.element->node);
        ASSERT_NE(mid_list.element, nullptr);
        EXPECT_EQ(mid_list.element->kind, ir::NodeKind::SCALAR);
        EXPECT_TRUE(mid_list.element->facts.dictionary)
            << "the leaf IS built from BaseFacts(field)";
        EXPECT_EQ(mid_list.element->facts.dictionary_index_kind, DictionaryIndexKind::INT16);
    }
    {
        SCOPED_TRACE("flattened repeated: INNER-declared dictionary is DROPPED (SF-1 gap)");
        const FieldDescriptor* fd = FieldByName(schema, "Holder", "rep_inner_declared");
        ASSERT_NE(fd, nullptr);
        // The declaration itself is readable on the inner field...
        const Descriptor* wrap = schema->FindMessageTypeByName("WrapInner");
        ASSERT_NE(wrap, nullptr);
        EXPECT_TRUE(HasFieldDictionary(wrap->field(0)));
        // ...but nothing on the repeated-flatten result carries it today.
        ir::IrNode n = ir::BuildFieldIr(fd);
        ASSERT_EQ(n.kind, ir::NodeKind::LIST);
        EXPECT_FALSE(n.facts.dictionary);
        const auto& list = std::get<ir::ListNode>(n.node);
        ASSERT_NE(list.element, nullptr);
        EXPECT_FALSE(list.element->facts.dictionary);
        // Contrast: the SINGULAR spelling of the same wrapper DOES carry it.
        EXPECT_TRUE(ir::BuildFieldIr(FieldByName(schema, "Holder", "on_inner")).facts.dictionary);
    }

    // -- D4b: the (fletcher.flatten_field) hole. The IR/projection behaviour
    //    pinned here is UNCHANGED by DICT-2: the projection still drops the
    //    wrapper's declaration. What DICT-2 added is a REJECTION of the shape
    //    (rule R1, TypeMapperTest.DictionaryMappingAndRejections) -- precisely
    //    BECAUSE this drop is real, so no accepted proto can reach it.
    {
        SCOPED_TRACE("flatten_field wrapper carrying the option: silently dropped (D4b)");
        const Descriptor* rec = schema->FindMessageTypeByName("Rec");
        ASSERT_NE(rec, nullptr);
        const FieldDescriptor* wrapper = rec->FindFieldByName("p");
        ASSERT_NE(wrapper, nullptr);
        EXPECT_TRUE(HasFieldDictionary(wrapper));  // the declaration IS readable
        // DICT-2 B4a: pins that BaseFacts(field) (ir.cpp:544) carries a
        // flatten_field wrapper's OWN declaration onto its IR node. It replaces
        // the coverage removed when
        // GenErrors.DictionaryRejectedBy_accessor_fieldFlatten was retargeted
        // from DICT-1.5's message to DICT-2's R1 (which is descriptor-based and
        // so does NOT exercise this route). Identical code path and shape as
        // that fixture's DictFfGuard.w: a 2-field, non-flatten message behind a
        // (fletcher.flatten_field) field. (Not the only pin on that line --
        // DictionaryMappingAndRejections' StructDict.st R2 sub-case leans on it
        // too, step-4b nit 2 -- but it is the only one on THIS shape.)
        EXPECT_TRUE(ir::BuildFieldIr(wrapper).facts.dictionary);
        auto records = cpp_backend::BuildFlattenedFieldList(rec);
        int dict_count = 0;
        int inlined = 0;
        for (const auto& r : records) {
            ASSERT_NE(r.node, nullptr);
            if (r.name == "a" || r.name == "b") {
                ++inlined;
                EXPECT_FALSE(r.node->facts.dictionary) << r.name;
            }
            if (r.node->facts.dictionary) ++dict_count;
        }
        EXPECT_EQ(inlined, 2) << "expected Pair.a/Pair.b to be inlined";
        // spec section 4 companion: flatten_field on a SCALAR is a no-op, so `s`
        // keeps its dictionary — that is the only dictionary in the list.
        EXPECT_EQ(dict_count, 1);
        ir::IrNode s = ir::BuildFieldIr(rec->FindFieldByName("s"));
        EXPECT_TRUE(s.facts.dictionary);
        EXPECT_EQ(s.facts.dictionary_index_kind, DictionaryIndexKind::INT8);
    }

    // -- WKT wrapper: nullable dictionary scalar (locked #9, DICT-2 depends) --
    {
        SCOPED_TRACE("google.protobuf.StringValue with the option");
        ir::IrNode n = ir::BuildFieldIr(FieldByName(schema, "Wkt", "s"));
        EXPECT_EQ(n.kind, ir::NodeKind::SCALAR);
        EXPECT_TRUE(n.facts.nullable);
        EXPECT_TRUE(n.facts.dictionary);
        EXPECT_EQ(n.facts.dictionary_index_kind, DictionaryIndexKind::INT16);
    }

    // ---- route 2: hand-encoded payloads, pool KNOWS the extension ----------
    // This pool is also where the probe's DISCRIMINATION is exercised (SF-3):
    // it holds, side by side, a valid payload, a corrupt one (probe fires), a
    // foreign-but-parseable one, a wrong-wire-type one (probe not reached), and
    // a foreign message-typed option DECLARED at a neighbouring number.
    {
        ProtoTextPool ipool;
        ASSERT_TRUE(ipool.seeded());
        ASSERT_NE(AddFletcherOptions(ipool), nullptr);

        // A foreign message-typed FieldOptions extension DECLARED at 60200 in the
        // very pool that declares fletcher.dictionary at 50001. Its payload bytes
        // are byte-identical to a valid DictionaryOptions{INT16, ordered} — proven
        // below — so if the reader keyed off anything other than the declared
        // extension it would read a dictionary here.
        const FileDescriptor* neighbour = ipool.Add("dict_neighbour.proto", ForeignSchema(60200));
        ASSERT_NE(neighbour, nullptr);

        FileDescriptorProto fdp = InjectedFile("dict_injected.proto");
        InjectedField(&fdp, "none", 1);
        InjectDictBytes(InjectedField(&fdp, "i16", 2), std::string("\x08\x02", 2));
        InjectDictBytes(InjectedField(&fdp, "ordered", 3), std::string("\x10\x01", 2));
        InjectDictBytes(InjectedField(&fdp, "i16_ordered", 4),
                        std::string(kI16OrderedBytes, kI16OrderedLen));
        InjectDictBytes(InjectedField(&fdp, "empty", 5), std::string());
        InjectDictBytes(InjectedField(&fdp, "bad_enum", 6), std::string("\x08\x09", 2));
        InjectDictBytes(InjectedField(&fdp, "truncated", 7), std::string("\x08", 1));
        InjectDictVarint(InjectedField(&fdp, "varint", 8), 1);
        // Foreign payload shapes AT 50001, in a pool that declares the extension:
        //  - `1a 05 01`: field 3, length-delimited, length 5 with 1 byte left ->
        //    the submessage parse fails -> whole re-parse fails -> probe fires.
        //  - `08 02 1a 03 61 62 63`: index_type INT16 plus an unknown field 3 =
        //    "abc" -> parses fine, so the DECLARED sub-field is DECODED (INT16, not
        //    defaults) even though the blob also carries foreign bytes. The
        //    differing result is what makes "the probe was not involved"
        //    observable (re-review P2-21).
        InjectDictBytes(InjectedField(&fdp, "foreign_corrupt", 9), std::string("\x1a\x05\x01", 3));
        InjectDictBytes(InjectedField(&fdp, "foreign_parseable", 10),
                        std::string("\x08\x02\x1a\x03\x61\x62\x63", 7));
        // Same bytes, NOT at 50001: must not be read at all.
        InjectBytesAt(InjectedField(&fdp, "wrong_number", 11), 50002,
                      std::string(kI16OrderedBytes, kI16OrderedLen));
        const FileDescriptor* inj = ipool.Build(fdp);
        ASSERT_NE(inj, nullptr);
        const Descriptor* m = inj->message_type(0);

        struct ByteCase {
            const char* field;
            bool present;
            DictionaryIndexKind kind;
            bool ordered;
            const char* why;
        };
        const ByteCase kByteCases[] = {
            {"none", false, DictionaryIndexKind::INT32, false, "no options at all"},
            // NOTE (N-15): the negative rows below (bad_enum, foreign_*) all
            // expect the DEFAULT kind, so they are only meaningful because these
            // positive rows share the loop. Do not delete them.
            {"i16", true, DictionaryIndexKind::INT16, false, "08 02"},
            {"ordered", true, DictionaryIndexKind::INT32, true, "10 01"},
            {"i16_ordered", true, DictionaryIndexKind::INT16, true, "08 02 10 01"},
            {"empty", true, DictionaryIndexKind::INT32, false, "zero-length submessage"},
            {"bad_enum", true, DictionaryIndexKind::INT32, false,
             "08 09: undeclared enum number -> unknowns -> default"},
            {"truncated", true, DictionaryIndexKind::INT32, false,
             "08: re-parse fails -> narrowed presence probe -> defaults"},
            {"foreign_corrupt", true, DictionaryIndexKind::INT32, false,
             "1a 05 01: foreign, unparseable -> probe fires -> declared, defaults"},
            {"foreign_parseable", true, DictionaryIndexKind::INT16, false,
             "08 02 + 1a 03 'abc': parseable -> INT16 DECODED (not defaults), which "
             "is what distinguishes this from the probe-driven rows"},
            // Wire-type mismatch on a KNOWN field is skipped into the unknowns,
            // so the parse succeeds and HasField(ext) is false: NOT a dictionary,
            // and the step-5 probe is never reached.
            {"varint", false, DictionaryIndexKind::INT32, false, "varint at #50001"},
            {"wrong_number", false, DictionaryIndexKind::INT32, false,
             "valid dictionary bytes at #50002 -> not read"},
        };
        for (const ByteCase& c : kByteCases) {
            SCOPED_TRACE(std::string("injected: ") + c.field + " (" + c.why + ")");
            const FieldDescriptor* f = m->FindFieldByName(c.field);
            ASSERT_NE(f, nullptr);
            auto d = ReadFieldDictionaryOption(f);
            EXPECT_EQ(d.has_value(), c.present);
            EXPECT_EQ(HasFieldDictionary(f), c.present);
            if (!c.present || !d.has_value()) continue;
            EXPECT_EQ(d->index_kind, c.kind);
            EXPECT_EQ(d->ordered, c.ordered);
            ir::IrNode n = ir::BuildFieldIr(f);
            EXPECT_TRUE(n.facts.dictionary);
            EXPECT_EQ(n.facts.dictionary_index_kind, c.kind);
            EXPECT_EQ(n.facts.dictionary_ordered, c.ordered);
        }

        // SF-3, half 1: a FOREIGN message-typed option, really DECLARED, in the
        // pool that also declares fletcher.dictionary. Same bytes as
        // `i16_ordered` above (asserted, not assumed) at a neighbouring number.
        {
            SCOPED_TRACE("declared foreign option at #60200 in the Fletcher-aware pool");
            const FieldDescriptor* fd = FieldByName(neighbour, "Msg", "f");
            ASSERT_NE(fd, nullptr);
            auto payload = UnknownPayloadAt(fd, 60200);
            ASSERT_TRUE(payload.has_value()) << "the foreign option must be present";
            EXPECT_EQ(*payload, std::string(kI16OrderedBytes, kI16OrderedLen))
                << "these bytes decode to {INT16, ordered} when they sit at 50001";
            EXPECT_FALSE(ReadFieldDictionaryOption(fd).has_value());
            EXPECT_FALSE(HasFieldDictionary(fd));
            EXPECT_FALSE(ir::BuildFieldIr(fd).facts.dictionary);
        }
    }

    // ---- route 2 WITHOUT route 1: no fletcher/options.proto in the pool ----
    // The narrowed presence probe must not fire on a bare field number: valid
    // bytes at #50001 with no Fletcher declaration behind them are NOT a
    // dictionary (R1 — a false positive would fabricate a schema).
    {
        SCOPED_TRACE("extension NOT in the pool -> nullopt");
        ProtoTextPool bare;
        ASSERT_TRUE(bare.seeded());
        FileDescriptorProto fdp = InjectedFile("dict_no_ext.proto");
        InjectDictBytes(InjectedField(&fdp, "i16", 1), std::string("\x08\x02", 2));
        const FileDescriptor* f = bare.Build(fdp);
        ASSERT_NE(f, nullptr);
        const FieldDescriptor* fd = f->message_type(0)->FindFieldByName("i16");
        ASSERT_NE(fd, nullptr);
        EXPECT_FALSE(ReadFieldDictionaryOption(fd).has_value());
        EXPECT_FALSE(HasFieldDictionary(fd));
        EXPECT_FALSE(ir::BuildFieldIr(fd).facts.dictionary);
    }
    {
        // SF-3, half 2: the SAME foreign declaration, now really occupying 50001
        // (only possible in a pool that does not declare fletcher.dictionary —
        // protobuf refuses two extensions of one extendee at one number). Same
        // bytes as the decoded `i16_ordered` case; opposite answer. The pair is
        // the no-false-positive property: the reader keys off the POOL's
        // declaration, never off the number or the bytes.
        SCOPED_TRACE("FOREIGN message-typed option DECLARED at #50001 -> nullopt");
        ProtoTextPool foreign;
        ASSERT_TRUE(foreign.seeded());
        const FileDescriptor* f = foreign.Add("dict_foreign.proto", ForeignSchema(50001));
        ASSERT_NE(f, nullptr);
        const FieldDescriptor* fd = FieldByName(f, "Msg", "f");
        ASSERT_NE(fd, nullptr);
        auto payload = UnknownPayloadAt(fd, 50001);
        ASSERT_TRUE(payload.has_value()) << "the foreign option must be present";
        EXPECT_EQ(*payload, std::string(kI16OrderedBytes, kI16OrderedLen))
            << "identical bytes to the i16_ordered sub-case, which decodes to {INT16, ordered}";
        EXPECT_FALSE(ReadFieldDictionaryOption(fd).has_value());
        EXPECT_FALSE(HasFieldDictionary(fd));
        EXPECT_FALSE(ir::BuildFieldIr(fd).facts.dictionary);
    }
}

// ===========================================================================
// DICT-2 forcing test: mapper wiring + the (fletcher.dictionary) legality rules
// ===========================================================================
// Design: plans/DICT-2-mapper-wiring-validation.md (rev 2).
//
// Two halves, one test:
//   * the POSITIVE projection (D1): FieldMapping gains `is_dictionary` +
//     `dict_index_type_expr`, DERIVED from ir::FieldFacts.dictionary. The VALUE
//     type, nullability and every other member are unchanged.
//   * the LEGALITY rules (D2/D3, R1-R5): DictionaryUnsupportedReason(field) and
//     FindIllegalDictionaryField(msg), the pure descriptor/IR predicates that
//     generator.cpp's ValidateDictionaryDeclarations turns into a fatal
//     front-end error. Rejection is NOT MapField -> nullopt (D0): nullopt is a
//     silent dropped column.
//
// kDictSchema is deliberately NOT edited (DICT-1's sub-cases depend on it
// verbatim); everything DICT-2 needs beyond it lives in kDict2Schema below.

namespace {

constexpr const char* kDict2Schema = R"(
syntax = "proto3";
package d2;
import "fletcher/options.proto";
import "google/protobuf/wrappers.proto";

enum Color { COLOR_UNSPECIFIED = 0; COLOR_RED = 1; }

message W1   { option (fletcher.flatten) = true; string value = 1; }
message W1i8 { option (fletcher.flatten) = true;
               string value = 1 [(fletcher.dictionary) = {index_type: DICTIONARY_INDEX_INT8}]; }
message W1i32{ option (fletcher.flatten) = true;
               string value = 1 [(fletcher.dictionary) = {index_type: DICTIONARY_INDEX_INT32}]; }
message W1i8t{ option (fletcher.flatten) = true;
               string value = 1 [(fletcher.dictionary) = {index_type: DICTIONARY_INDEX_INT8,
                                                          ordered: true}]; }
message W2   { option (fletcher.flatten) = true;
               string k = 1 [(fletcher.dictionary) = {}]; int32 n = 2; }
message PBad { repeated string tags = 1 [(fletcher.dictionary) = {}]; string ok = 2; }
message Plain2 { string a = 1; string b = 2; }

message Ctl {
  optional string opt_cat = 1 [(fletcher.dictionary) = {index_type: DICTIONARY_INDEX_INT8}];
  Color  color      = 2 [(fletcher.dictionary) = {}];
  W1     wrap_plain = 3 [(fletcher.dictionary) = {index_type: DICTIONARY_INDEX_INT16}];
  W1i8   agree      = 4 [(fletcher.dictionary) = {index_type: DICTIONARY_INDEX_INT8}];
  W1i32  agree_dflt = 5 [(fletcher.dictionary) = {}];
  W1i8   disagree   = 6 [(fletcher.dictionary) = {index_type: DICTIONARY_INDEX_INT64}];
  // R4 (the locked-#8 hole). index_type MUST be spelled INT8 here so `ordered` is
  // the ONLY difference from W1i8's leaf: with `= {ordered: true}` the outer
  // option decodes to INT32, the indexes would differ too, and an implementation
  // comparing index_kind ALONE would still reject it -- i.e. the assertion would
  // be vacuous for the property it exists to pin (step-2 cycle-2 item R1).
  W1i8   ord_wrap   = 7 [(fletcher.dictionary) = {index_type: DICTIONARY_INDEX_INT8,
                                                  ordered: true}];
  W1i8   ff_flat    = 8 [(fletcher.flatten_field) = true, (fletcher.dictionary) = {}];
  google.protobuf.StringValue wkt_ff = 9
                          [(fletcher.flatten_field) = true, (fletcher.dictionary) = {}];
  repeated W2 rep_multi = 10;
  repeated string plain_tags   = 11;
  map<string, string> plain_lbl = 12;
  W1 plain_wrap = 13;
  // the OTHER branch of D2's locked-#8 closure argument: outer and leaf are EQUAL
  // and both set ordered, so R4 must stay silent and R3 must fire.
  W1i8t  ord_agree  = 14 [(fletcher.dictionary) = {index_type: DICTIONARY_INDEX_INT8,
                                                   ordered: true}];
}

message StructDict { Plain2 st = 1 [(fletcher.dictionary) = {}]; }
message RecFf { PBad p = 1 [(fletcher.flatten_field) = true]; }

// ---- step-4 review S4: the two `is_repeated` terms in R4's loop ------------
// (a) `if (field->is_repeated()) return nullopt;`  -- WD8 has TWO disagreeing
//     INNER declarations down a single-field chain, and the OUTER field carries
//     none, so R2 cannot fire on the LIST: dropping the term makes R4 steal the
//     shape from R5 and report "conflicting" instead.
message VD8 { option (fletcher.flatten) = true;
              string s = 1 [(fletcher.dictionary) = {index_type: DICTIONARY_INDEX_INT64}]; }
message WD8 { option (fletcher.flatten) = true;
              VD8 v = 1 [(fletcher.dictionary) = {index_type: DICTIONARY_INDEX_INT8}]; }
// (b) `if (inner->is_repeated()) break;`  -- ir.cpp reads BaseFacts(WD9.vs) and
//     NOTHING below it, so the two declarations under `vs` are never used.
//     Dropping the term reports a "conflict" between them, with advice ("make
//     them identical") that leads to silence rather than a fix. `vs` itself
//     carries no option so R2 cannot fire on the LIST first.
//     This is ALSO spec 7.1 gap 2's disclosed SIBLING (silent + safe by
//     construction), so the same shape pins both facts.
message UD9 { option (fletcher.flatten) = true;
              string s = 1 [(fletcher.dictionary) = {index_type: DICTIONARY_INDEX_INT64}]; }
message VD9 { option (fletcher.flatten) = true;
              UD9 u = 1 [(fletcher.dictionary) = {index_type: DICTIONARY_INDEX_INT8}]; }
message WD9 { option (fletcher.flatten) = true; repeated VD9 vs = 1; }

// ---- step-4 review S2/S3: why R1 is sound ONLY at the top level of a
//      generated message, i.e. why the pass must KEEP its IsFlattenedWrapper
//      skip and cannot close S2/S3 by dropping it. `flatten_field` inlining
//      happens ONLY in GatherFieldsImpl / BuildFlattenedFieldListImpl, i.e. only
//      for a generated message's OWN columns. Inside a flatten wrapper it is a
//      no-op, so FfW.p's declaration is RESOLVED AND HONOURED (Chains
//      .ff_inside_wrapper maps to a SCALAR int16 dictionary) -- judging FfW on
//      its own would make R1 a FALSE POSITIVE.
message FfLeaf { option (fletcher.flatten) = true; string s = 1; }
message FfW { option (fletcher.flatten) = true;
              FfLeaf p = 1 [(fletcher.flatten_field) = true,
                            (fletcher.dictionary) = {index_type: DICTIONARY_INDEX_INT16}]; }

message Chains {
  repeated WD8 r4_repeated_outer = 1;
  WD9 r4_repeated_inner = 2;
  FfW ff_inside_wrapper = 3;
}

// ---- step-4 re-review: the S3 closure (struct / list-element / map-value
//      descent). BadChild stands in for a message in an IMPORTED file: before the
//      closure the SAME declaration was fatal when its own file was the
//      generation unit and silently accepted when only an importer was, i.e. the
//      verdict depended on which .proto protoc was pointed at.
message BadChild { string k = 1 [(fletcher.dictionary) = {ordered: true}]; }
message HostSingular { BadChild c = 1; int32 n = 2; }
message HostRepeated { repeated BadChild c = 1; }
message HostMap { map<string, BadChild> c = 1; }
// ...and the exclusion that keeps R1 sound. W1i8t is an IsFlattenedWrapper, so
// nothing inlines its fields and a declaration inside it is HONOURED -- the
// descent must NOT enter it. This is also spec 7.1.1's S2 shape, which stays OPEN
// BY DESIGN: closing it needs R1 to become top-level-gated, which is a design
// decision, so this assertion is meant to red if someone opens it by accident.
message HostMapWrapper { map<string, W1i8t> c = 1; }
)";

// Convenience: the reason for `msg.field`, failing loudly if it was accepted.
std::string ReasonFor(const FileDescriptor* file, const std::string& msg,
                      const std::string& field) {
    const FieldDescriptor* fd = FieldByName(file, msg, field);
    if (fd == nullptr) {
        ADD_FAILURE() << "no such field: " << msg << "." << field;
        return "<missing field>";
    }
    auto r = DictionaryUnsupportedReason(fd);
    if (!r) {
        ADD_FAILURE() << msg << "." << field << " was accepted, expected a rejection";
        return "<accepted>";
    }
    return *r;
}

bool IsLegal(const FileDescriptor* file, const std::string& msg, const std::string& field) {
    const FieldDescriptor* fd = FieldByName(file, msg, field);
    if (fd == nullptr) {
        ADD_FAILURE() << "no such field: " << msg << "." << field;
        return false;
    }
    auto r = DictionaryUnsupportedReason(fd);
    if (r) ADD_FAILURE() << msg << "." << field << " was rejected: " << *r;
    return !r.has_value();
}

bool Mentions(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

// P2-3: FindIllegalDictionaryField dereferences its argument, so a renamed or
// mistyped message name must FAIL the test, not segfault the whole binary.
const Descriptor* MsgOrFail(const FileDescriptor* file, const std::string& name) {
    const Descriptor* d = file->FindMessageTypeByName(name);
    EXPECT_NE(d, nullptr) << "no such message: " << name;
    return d;
}

// Same, for the message-level walk: returns "<missing message>" rather than
// dereferencing null.
std::optional<std::string> IllegalIn(const FileDescriptor* file, const std::string& name) {
    const Descriptor* d = MsgOrFail(file, name);
    if (d == nullptr) return std::string("<missing message>");
    return FindIllegalDictionaryField(d);
}

}  // namespace

TEST(TypeMapperTest, DictionaryMappingAndRejections) {
    ProtoTextPool pool;
    ASSERT_TRUE(pool.seeded());
    ASSERT_NE(AddFletcherOptions(pool), nullptr);
    ASSERT_NE(pool.AddLinked(google::protobuf::StringValue::GetDescriptor()->file()), nullptr);
    const FileDescriptor* d1 = pool.Add("dict_schema.proto", kDictSchema);
    ASSERT_NE(d1, nullptr);
    const FileDescriptor* d2 = pool.Add("dict2_schema.proto", kDict2Schema);
    ASSERT_NE(d2, nullptr);

    // =====================================================================
    // D1 -- the POSITIVE projection
    // =====================================================================
    {
        SCOPED_TRACE("D1: scalar dictionary projects onto is_dictionary + index expr");
        auto m = MapField(FieldByName(d1, "Ev", "i16"));
        ASSERT_TRUE(m.has_value());
        EXPECT_EQ(m->kind, FieldKind::SCALAR);
        EXPECT_TRUE(m->is_dictionary);
        EXPECT_EQ(m->dict_index_type_expr, "arrow::int16()");
        // locked #7: `scalar` stays the VALUE type.
        EXPECT_EQ(m->scalar.arrow_type_expr, "arrow::utf8()");
        EXPECT_FALSE(m->nullable);
    }
    {
        SCOPED_TRACE("D1: the four index kinds + the two UNSPECIFIED spellings");
        struct Row {
            const char* field;
            const char* expr;
        };
        const Row kRows[] = {
            {"empty", "arrow::int32()"}, {"unspec", "arrow::int32()"}, {"i8", "arrow::int8()"},
            {"i16", "arrow::int16()"},   {"i32", "arrow::int32()"},    {"i64", "arrow::int64()"},
        };
        for (const Row& r : kRows) {
            auto m = MapField(FieldByName(d1, "Ev", r.field));
            ASSERT_TRUE(m.has_value()) << r.field;
            EXPECT_TRUE(m->is_dictionary) << r.field;
            EXPECT_EQ(m->dict_index_type_expr, r.expr) << r.field;
        }
    }
    {
        SCOPED_TRACE("D1: nullability is preserved (proto3 optional)");
        auto m = MapField(FieldByName(d2, "Ctl", "opt_cat"));
        ASSERT_TRUE(m.has_value());
        EXPECT_EQ(m->kind, FieldKind::SCALAR);
        EXPECT_TRUE(m->is_dictionary);
        EXPECT_TRUE(m->nullable);
        EXPECT_EQ(m->dict_index_type_expr, "arrow::int8()");
        EXPECT_EQ(m->scalar.arrow_type_expr, "arrow::utf8()");
    }
    {
        SCOPED_TRACE("D1: an enum dictionary keeps its int32 VALUE type");
        auto m = MapField(FieldByName(d2, "Ctl", "color"));
        ASSERT_TRUE(m.has_value());
        EXPECT_EQ(m->kind, FieldKind::SCALAR);
        EXPECT_TRUE(m->is_dictionary);
        EXPECT_EQ(m->dict_index_type_expr, "arrow::int32()");
        EXPECT_EQ(m->scalar.arrow_type_expr, "arrow::int32()");
    }
    {
        // locked #9's acceptance case: a WKT wrapper WITHOUT flatten_field is a
        // valid NULLABLE dictionary. Contrast Ctl.wkt_ff below, which is R1.
        SCOPED_TRACE("D1: google.protobuf.StringValue dictionary (locked #9)");
        auto m = MapField(FieldByName(d1, "Wkt", "s"));
        ASSERT_TRUE(m.has_value());
        EXPECT_EQ(m->kind, FieldKind::SCALAR);
        EXPECT_TRUE(m->nullable);
        EXPECT_TRUE(m->is_dictionary);
        EXPECT_EQ(m->dict_index_type_expr, "arrow::int16()");
        EXPECT_EQ(m->scalar.arrow_type_expr, "arrow::utf8()");
    }
    {
        SCOPED_TRACE("D1: no false positive -- a plain scalar carries neither field");
        auto m = MapField(FieldByName(d1, "Ev", "plain"));
        ASSERT_TRUE(m.has_value());
        EXPECT_FALSE(m->is_dictionary);
        EXPECT_TRUE(m->dict_index_type_expr.empty());
    }

    // =====================================================================
    // R2 -- the kind gate (locked #9)
    // =====================================================================
    const std::string r2_list = ReasonFor(d1, "Shapes", "tags");
    const std::string r2_map = ReasonFor(d1, "Shapes", "labels");
    const std::string r2_struct = ReasonFor(d2, "StructDict", "st");
    const std::string r2_nested = ReasonFor(d1, "Holder", "rep_nested");
    const std::string r2_unsupported = ReasonFor(d1, "Shapes", "oc");
    {
        SCOPED_TRACE("R2: the kind-word names the column the field actually maps to");
        EXPECT_TRUE(Mentions(r2_list, "list")) << r2_list;
        EXPECT_TRUE(Mentions(r2_list, "dt.Shapes.tags")) << r2_list;
        EXPECT_TRUE(Mentions(r2_map, "map")) << r2_map;
        EXPECT_TRUE(Mentions(r2_struct, "struct")) << r2_struct;
        EXPECT_TRUE(Mentions(r2_nested, "list")) << r2_nested;
        // A oneof member has no Arrow mapping at all -- its own dedicated clause,
        // NOT the kind-word ("maps to an unsupported field type" reads oddly).
        EXPECT_TRUE(Mentions(r2_unsupported, "no Arrow mapping")) << r2_unsupported;
    }
    {
        // R2 gates on the TOP-LEVEL node only, never an OR over the subtree: a
        // scalar dictionary inside a STRUCT CHILD is legal (spec 7.1's closing
        // rule) and is judged on its own when its own message is walked.
        SCOPED_TRACE("R2: no subtree-OR -- a struct child's scalar dictionary is legal");
        EXPECT_TRUE(IsLegal(d1, "MemberHolder", "m"));
        EXPECT_TRUE(IsLegal(d1, "Member", "dict"));
        EXPECT_FALSE(IllegalIn(d1, "MemberHolder")) << "MemberHolder itself declares nothing";
    }

    // =====================================================================
    // R3 -- ordered: true (locked #8)
    // =====================================================================
    const std::string r3 = ReasonFor(d1, "Ev", "ord");
    {
        SCOPED_TRACE("R3: ordered: true on a plain scalar");
        EXPECT_TRUE(Mentions(r3, "ordered")) << r3;
        EXPECT_TRUE(Mentions(ReasonFor(d1, "Ev", "ord16"), "ordered"));
        // wrapper-declared ordered with NO leaf declaration: leaf-wins does not
        // apply, so the resolved node carries it and R3 sees it.
        EXPECT_TRUE(Mentions(ReasonFor(d1, "Holder", "on_wrapper"), "ordered"));
    }

    // =====================================================================
    // R1 -- (fletcher.flatten_field) + (fletcher.dictionary)  [spec 7.1 gap 1]
    // =====================================================================
    const std::string r1 = ReasonFor(d1, "Rec", "p");
    {
        SCOPED_TRACE("R1: the three-term wrapper predicate");
        EXPECT_TRUE(Mentions(r1, "flatten_field")) << r1;
        EXPECT_TRUE(Mentions(r1, "dt.Rec.p")) << r1;
        // THIRD TERM: flatten_field on a SCALAR is a documented no-op, and the
        // dictionary there is legal (spec 4; DICT-1 pins the IR side).
        EXPECT_TRUE(IsLegal(d1, "Rec", "s"));
    }
    {
        // D4 row 1: BuildFieldIr(ff_flat) is a SCALAR carrying the dictionary, so
        // R2 ALONE WOULD ACCEPT IT -- while both inlining walks `continue` past
        // the wrapper and emit a value-typed column. R1 is the only rule that
        // catches it.
        SCOPED_TRACE("R1: flatten_field OVER a message-level flatten wrapper (D4 row 1)");
        const FieldDescriptor* fd = FieldByName(d2, "Ctl", "ff_flat");
        ASSERT_NE(fd, nullptr);
        ir::IrNode n = ir::BuildFieldIr(fd);
        EXPECT_EQ(n.kind, ir::NodeKind::SCALAR) << "R2 alone would accept this";
        EXPECT_TRUE(Mentions(ReasonFor(d2, "Ctl", "ff_flat"), "flatten_field"));
    }
    {
        // D4 row 2 (B5): the WKT instance of the SAME "R2 accepts / emission
        // drops" class. Do NOT exclude WKTs from R1 -- that reopens the hole.
        SCOPED_TRACE("R1: StringValue + flatten_field + dictionary (D4 row 2)");
        ir::IrNode n = ir::BuildFieldIr(FieldByName(d2, "Ctl", "wkt_ff"));
        EXPECT_EQ(n.kind, ir::NodeKind::SCALAR) << "TryBuildWkt -> R2 alone would accept";
        EXPECT_TRUE(Mentions(ReasonFor(d2, "Ctl", "wkt_ff"), "flatten_field"));
    }

    // =====================================================================
    // R4 -- disagreeing declarations on one singular flatten chain
    // =====================================================================
    const std::string r4 = ReasonFor(d2, "Ctl", "disagree");
    {
        SCOPED_TRACE("R4: disagreeing index types");
        EXPECT_TRUE(Mentions(r4, "conflicting")) << r4;
        EXPECT_TRUE(Mentions(r4, "int64")) << r4;
        EXPECT_TRUE(Mentions(r4, "int8")) << r4;
    }
    {
        // The locked-#8 hole, and the reason R4's equality must compare BOTH
        // members of the DECODED option. `ordered` is the SOLE difference here.
        SCOPED_TRACE("R4: ordered is part of the equality (locked #8)");
        ir::IrNode n = ir::BuildFieldIr(FieldByName(d2, "Ctl", "ord_wrap"));
        ASSERT_EQ(n.kind, ir::NodeKind::SCALAR);
        EXPECT_TRUE(n.facts.dictionary);
        EXPECT_FALSE(n.facts.dictionary_ordered)
            << "leaf-wins discards the outer option wholesale, so R3 CANNOT see this";
        const std::string r = ReasonFor(d2, "Ctl", "ord_wrap");
        EXPECT_TRUE(Mentions(r, "conflicting")) << r;
        // `ordered` alone is decorative (every R4 message renders it); asserting
        // BOTH renderings is what pins that `ordered` participates in R4's
        // equality and that both sides are legible (step-4b nit 1).
        EXPECT_TRUE(Mentions(r, "ordered true")) << r;
        EXPECT_TRUE(Mentions(r, "ordered false")) << r;
        EXPECT_FALSE(Mentions(r, "int16")) << "index is INT8 on BOTH sides: " << r;
    }
    {
        // The OTHER branch of the closure argument: equal options that both set
        // ordered must surface on the resolved node and be reported by R3.
        SCOPED_TRACE("R3 (not R4): outer and leaf both {INT8, ordered: true}");
        ir::IrNode n = ir::BuildFieldIr(FieldByName(d2, "Ctl", "ord_agree"));
        ASSERT_EQ(n.kind, ir::NodeKind::SCALAR);
        EXPECT_TRUE(n.facts.dictionary_ordered);
        const std::string r = ReasonFor(d2, "Ctl", "ord_agree");
        EXPECT_TRUE(Mentions(r, "ordered")) << r;
        EXPECT_FALSE(Mentions(r, "conflicting")) << r;
    }
    {
        SCOPED_TRACE("R4: EQUAL options are not a conflict (spelling differences included)");
        EXPECT_TRUE(IsLegal(d2, "Ctl", "agree"));
        EXPECT_TRUE(IsLegal(d2, "Ctl", "agree_dflt"));  // {} == INT32
        // the legal wrapper-declared control (no ordered anywhere).
        EXPECT_TRUE(IsLegal(d2, "Ctl", "wrap_plain"));
        auto m = MapField(FieldByName(d2, "Ctl", "wrap_plain"));
        ASSERT_TRUE(m.has_value());
        EXPECT_TRUE(m->is_dictionary);
        EXPECT_EQ(m->dict_index_type_expr, "arrow::int16()");
    }

    // =====================================================================
    // R5 -- inner declaration under a SINGLE-FIELD repeated flatten wrapper
    // =====================================================================
    const std::string r5 = ReasonFor(d1, "Holder", "rep_inner_declared");
    {
        SCOPED_TRACE("R5: spec 7.1 gap 2 is now a hard error");
        EXPECT_TRUE(Mentions(r5, "dt.WrapInner.value")) << r5;
        EXPECT_TRUE(Mentions(r5, "repeated")) << r5;
        // ...and the IR still DROPS the fact (DICT-1's SF-1 pin is untouched):
        // R5 walks descriptors precisely because no IR node can see this.
        ir::IrNode n = ir::BuildFieldIr(FieldByName(d1, "Holder", "rep_inner_declared"));
        ASSERT_EQ(n.kind, ir::NodeKind::LIST);
        EXPECT_FALSE(n.facts.dictionary);
    }
    {
        // B1 -- THE false-positive guard. A MULTI-field flatten wrapper behind a
        // `repeated` field is a LEGAL, EMITTED shape: BuildFlattenedRepeated
        // returns List<Struct(W2)> without entering its chain loop, W2 is not
        // IsFlattenedWrapper so the pass validates it on its own, and W2.k's
        // scalar dictionary is honoured by BuildStructVariant.
        SCOPED_TRACE("R5: field_count() == 1 -- a multi-field wrapper is untouched");
        EXPECT_TRUE(IsLegal(d2, "Ctl", "rep_multi"));
        EXPECT_FALSE(IllegalIn(d2, "W2")) << "W2.k's scalar dictionary is legal";
        EXPECT_TRUE(IsLegal(d2, "W2", "k"));
    }

    // =====================================================================
    // S4 (step-4 review) -- R4's TWO load-bearing `is_repeated` terms.
    // Both were mandatory step-2 residuals and both were unpinned: deleting
    // either left the whole suite green. Each sub-case below reds on exactly one
    // deletion.
    // =====================================================================
    {
        // Term 1: `if (field->is_repeated()) return std::nullopt;`
        // Without it R4 collects WD8.v {INT8} and VD8.s {INT64}, finds a
        // "conflict", and STEALS the shape from R5 -- reporting a conflict
        // between two declarations that ir.cpp never compares, instead of R5's
        // accurate "the resulting column is a list".
        SCOPED_TRACE("S4a: a repeated outer field is R5's business, never R4's");
        const std::string r = ReasonFor(d2, "Chains", "r4_repeated_outer");
        EXPECT_TRUE(Mentions(r, "inside a repeated")) << r;
        EXPECT_TRUE(Mentions(r, "d2.WD8.v")) << r;
        EXPECT_FALSE(Mentions(r, "conflicting")) << "R4 stole a shape from R5: " << r;
    }
    {
        // Term 2: `if (inner->is_repeated()) break;` -- placed AFTER that inner's
        // own option is collected, because BuildFlattenedRepeated DOES read
        // BaseFacts(WD9.vs) and nothing below it. Without the break R4 reports a
        // conflict between VD9.u and UD9.s, NEITHER of which ir.cpp reads, with
        // advice ("make them identical") that leads to silence rather than a fix.
        //
        // This is ALSO spec 7.1 gap 2's disclosed SIBLING: a `repeated` hop
        // inside a singular chain. It stays SILENT by design (Risk 5) and is safe
        // by gap 2's own construction argument -- schema emission consumes the
        // identical node and drops the declaration too, so the column is a plain
        // list and no mis-read exists. DO NOT "close" this by generalising R5.
        SCOPED_TRACE("S4b: R4 stops where ir.cpp stops reading (== gap 2's sibling)");
        const FieldDescriptor* fd = FieldByName(d2, "Chains", "r4_repeated_inner");
        ASSERT_NE(fd, nullptr);
        ir::IrNode n = ir::BuildFieldIr(fd);
        ASSERT_EQ(n.kind, ir::NodeKind::LIST);
        EXPECT_FALSE(n.facts.dictionary) << "both declarations below `vs` are dropped by ir.cpp";
        auto r = DictionaryUnsupportedReason(fd);
        EXPECT_FALSE(r.has_value()) << "must NOT report a conflict ir.cpp never sees: " << *r;
    }
    {
        // Why the pass must KEEP its IsFlattenedWrapper skip, i.e. why step-4b's
        // S2/S3 cannot be closed by dropping it: R1 is sound ONLY for a generated
        // message's OWN columns. `flatten_field` inlining lives in
        // GatherFieldsImpl / BuildFlattenedFieldListImpl only; INSIDE a flatten
        // wrapper it is a no-op, so FfW.p's declaration is resolved and HONOURED.
        // Judging FfW on its own would fire R1 on FfW.p -- a FALSE POSITIVE on a
        // proto whose dictionary really is emitted.
        SCOPED_TRACE("R1 is top-level-only: a flatten_field decl INSIDE a wrapper is honoured");
        auto m = MapField(FieldByName(d2, "Chains", "ff_inside_wrapper"));
        ASSERT_TRUE(m.has_value());
        EXPECT_EQ(m->kind, FieldKind::SCALAR);
        EXPECT_TRUE(m->is_dictionary)
            << "the declaration is LIVE, so rejecting it is a false positive";
        EXPECT_EQ(m->dict_index_type_expr, "arrow::int16()");
        EXPECT_TRUE(IsLegal(d2, "Chains", "ff_inside_wrapper"));
        // ...and, for the record, the rule set applied to the wrapper ITSELF does
        // reject it. That asymmetry is exactly the hazard above.
        EXPECT_TRUE(IllegalIn(d2, "FfW").has_value())
            << "judging a flatten wrapper directly is UNSOUND for R1; the pass skips wrappers";
    }

    // =====================================================================
    // S3 closure (step-4 re-review) -- struct / list-element / map-value descent.
    // The walk judges a child message where emission deep-copies that child's own
    // schema function AND the child is named DIRECTLY by a field of a judged
    // message. A child behind a (fletcher.flatten) wrapper hop is NOT judged (the
    // wrapper exclusion is required for R1's soundness) -- a disclosed hole, see
    // docs/dictionary-option-spec.md section 7.1.1.
    // =====================================================================
    {
        SCOPED_TRACE("S3: a child message's illegal declaration is judged from the HOST");
        for (const char* host : {"HostSingular", "HostRepeated", "HostMap"}) {
            SCOPED_TRACE(host);
            auto e = IllegalIn(d2, host);
            ASSERT_TRUE(e.has_value()) << host << " accepted an illegal child declaration";
            EXPECT_TRUE(Mentions(*e, "d2.BadChild.k")) << *e;
            EXPECT_TRUE(Mentions(*e, "ordered")) << *e;
        }
        // ...and the declaring message still reports it on its own, unchanged: the
        // point of the closure is that the two verdicts now AGREE.
        auto own = IllegalIn(d2, "BadChild");
        ASSERT_TRUE(own.has_value());
        EXPECT_EQ(*own, *IllegalIn(d2, "HostSingular"))
            << "the verdict must not depend on which message the walk started from";
    }
    {
        // The exclusion that keeps R1 sound (and leaves spec 7.1.1's S2 open by
        // design). A flatten wrapper has NO generated schema function, so nothing
        // inlines its fields and a declaration inside it is HONOURED -- see the
        // Chains.ff_inside_wrapper block above and ctest
        // GenErrors.DictionaryLiveInsideFlattenWrapperAccepted, which is the pin
        // that actually reds (this pass is file-local; no unit test calls it).
        SCOPED_TRACE("S3 closure never descends into an IsFlattenedWrapper child (S2 stays open)");
        EXPECT_FALSE(IllegalIn(d2, "HostMapWrapper"))
            << "descending into a flatten wrapper would make R1 a FALSE POSITIVE";
    }

    // =====================================================================
    // D3 -- the message walk
    // =====================================================================
    {
        // The walk descends through a (fletcher.flatten_field) wrapper exactly as
        // the two inlining walks do, so it judges the fields that really become
        // columns -- PBad.tags is one of RecFf's columns.
        SCOPED_TRACE("D3: descent into a flatten_field wrapper judges the INLINED fields");
        auto e = IllegalIn(d2, "RecFf");
        ASSERT_TRUE(e.has_value());
        EXPECT_TRUE(Mentions(*e, "d2.PBad.tags")) << *e;
        EXPECT_TRUE(Mentions(*e, "list")) << *e;
    }
    {
        SCOPED_TRACE("D3: the FIRST offender in declaration order wins");
        auto e = IllegalIn(d2, "Ctl");
        ASSERT_TRUE(e.has_value());
        // Ctl fields 1-5 are legal; field 6 (`disagree`) is the first offender.
        EXPECT_EQ(*e, r4) << *e;
    }
    {
        SCOPED_TRACE("D3: a clean message is nullopt");
        EXPECT_FALSE(IllegalIn(d1, "Pair"));
        EXPECT_FALSE(IllegalIn(d2, "W1"));
    }

    // =====================================================================
    // Legal controls -- catches an implementation that rejects non-scalars
    // regardless of whether a dictionary is declared at all.
    // =====================================================================
    {
        SCOPED_TRACE("no-option controls: repeated / map / wrapper / struct");
        EXPECT_TRUE(IsLegal(d2, "Ctl", "plain_tags"));
        EXPECT_TRUE(IsLegal(d2, "Ctl", "plain_lbl"));
        EXPECT_TRUE(IsLegal(d2, "Ctl", "plain_wrap"));
        EXPECT_TRUE(IsLegal(d1, "Holder", "on_inner"));
        EXPECT_TRUE(IsLegal(d1, "Holder", "on_none"));
        EXPECT_TRUE(IsLegal(d1, "Ev", "plain"));
        EXPECT_TRUE(IsLegal(d1, "Ev", "i16"));
        EXPECT_TRUE(IsLegal(d2, "Plain2", "a"));
    }

    // =====================================================================
    // "each with a distinct reason" (story) -- all six texts pairwise distinct.
    // =====================================================================
    {
        SCOPED_TRACE("all six rule texts are pairwise distinct");
        const std::set<std::string> texts = {r1, r2_list, r2_unsupported, r3, r4, r5};
        EXPECT_EQ(texts.size(), 6u);
    }
}
