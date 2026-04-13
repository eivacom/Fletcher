#include <gtest/gtest.h>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>

#include "type_mapper.hpp"

using namespace fletcher;
using namespace google::protobuf;

// ===========================================================================
// Helpers
// ===========================================================================

namespace {

const FileDescriptor* BuildSingleField(
    DescriptorPool& pool,
    const std::string& tag,
    FieldDescriptorProto::Type type,
    FieldDescriptorProto::Label label = FieldDescriptorProto::LABEL_OPTIONAL)
{
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

const FieldDescriptor* First(const FileDescriptor* file) {
    return file->message_type(0)->field(0);
}

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
    EXPECT_EQ(m->scalar.storage_type,    "bool");
    EXPECT_EQ(m->scalar.builder_type,    "arrow::BooleanBuilder");
    EXPECT_EQ(m->scalar.default_value,   "false");
}

TEST(TypeMapperTest, MapFieldInt32) {
    DescriptorPool pool;
    auto m = MapField(First(BuildSingleField(pool, "int32", FieldDescriptorProto::TYPE_INT32)));
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->kind, FieldKind::SCALAR);
    EXPECT_EQ(m->scalar.arrow_type_expr, "arrow::int32()");
    EXPECT_EQ(m->scalar.storage_type,    "int32_t");
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
    EXPECT_EQ(m->scalar.storage_type,    "std::string");
    EXPECT_EQ(m->scalar.param_type,      "std::string_view");
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
    auto m = MapField(First(BuildSingleField(
        pool, "rep_i32", FieldDescriptorProto::TYPE_INT32,
        FieldDescriptorProto::LABEL_REPEATED)));
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->kind, FieldKind::REPEATED_SCALAR);
    EXPECT_FALSE(m->nullable);  // repeated fields are never null
    EXPECT_EQ(m->element.arrow_type_expr, "arrow::int32()");
    EXPECT_EQ(m->element.storage_type,    "int32_t");
    EXPECT_EQ(m->element.builder_type,    "arrow::Int32Builder");
}

TEST(TypeMapperTest, RepeatedStringMapsToRepeatedScalar) {
    DescriptorPool pool;
    auto m = MapField(First(BuildSingleField(
        pool, "rep_str", FieldDescriptorProto::TYPE_STRING,
        FieldDescriptorProto::LABEL_REPEATED)));
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->kind, FieldKind::REPEATED_SCALAR);
    EXPECT_EQ(m->element.arrow_type_expr, "arrow::utf8()");
    EXPECT_EQ(m->element.builder_type,    "arrow::StringBuilder");
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
    EXPECT_EQ(m->nested_class, "InnerArrowRow");
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
    EXPECT_EQ(m->nested_class, "InnerArrowRow");
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
    EXPECT_EQ(m->map_key.arrow_type_expr,   "arrow::utf8()");
    EXPECT_EQ(m->map_key.storage_type,      "std::string");
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
    EXPECT_EQ(ClassName(file->message_type(0)), "MsgArrowRow");
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
    EXPECT_EQ(ClassName(file->message_type(0)->nested_type(0)), "Outer_InnerArrowRow");
}

// ===========================================================================
// Cross-file message references
// ===========================================================================

namespace {

// Builds a "dep" file (dependency) containing a single message named DepMsg
// with one int32 field, optionally under the given proto package.
const FileDescriptor* BuildDepFile(DescriptorPool& pool,
                                   const std::string& filename,
                                   const std::string& package = "") {
    FileDescriptorProto dep;
    dep.set_name(filename);
    dep.set_syntax("proto3");
    if (!package.empty())
        dep.set_package(package);
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
const FileDescriptor* BuildConsumerFile(DescriptorPool& pool,
                                        const std::string& consumer_filename,
                                        const std::string& dep_filename,
                                        const std::string& dep_full_type,
                                        FieldDescriptorProto::Label label,
                                        const std::string& package = "") {
    FileDescriptorProto fdp;
    fdp.set_name(consumer_filename);
    fdp.set_syntax("proto3");
    if (!package.empty())
        fdp.set_package(package);
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

    auto* consumer = BuildConsumerFile(
        pool, "consumer_same_pkg.proto", "dep_same_pkg.proto",
        ".mypkg.DepMsg", FieldDescriptorProto::LABEL_OPTIONAL, "mypkg");
    ASSERT_TRUE(consumer);

    const auto* fd = consumer->message_type(0)->field(0);
    auto m = MapField(fd);
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->kind, FieldKind::STRUCT);
    // Same package ->no global qualification needed; nested_header must be set.
    EXPECT_EQ(m->nested_class, "DepMsgArrowRow");
    EXPECT_EQ(m->nested_header, "dep_same_pkg.fletcher.pb.h");
}

TEST(TypeMapperTest, CrossFileSingularMessageDifferentPackages) {
    DescriptorPool pool;
    ASSERT_TRUE(BuildDepFile(pool, "dep_other_pkg.proto", "other.pkg"));

    auto* consumer = BuildConsumerFile(
        pool, "consumer_diff_pkg.proto", "dep_other_pkg.proto",
        ".other.pkg.DepMsg", FieldDescriptorProto::LABEL_OPTIONAL, "my.pkg");
    ASSERT_TRUE(consumer);

    const auto* fd = consumer->message_type(0)->field(0);
    auto m = MapField(fd);
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->kind, FieldKind::STRUCT);
    // Different package ->globally qualified.
    EXPECT_EQ(m->nested_class, "::other::pkg::DepMsgArrowRow");
    EXPECT_EQ(m->nested_header, "dep_other_pkg.fletcher.pb.h");
}

TEST(TypeMapperTest, CrossFileSingularMessageNoPackageDep) {
    DescriptorPool pool;
    ASSERT_TRUE(BuildDepFile(pool, "dep_no_pkg.proto", ""));

    auto* consumer = BuildConsumerFile(
        pool, "consumer_no_dep_pkg.proto", "dep_no_pkg.proto",
        ".DepMsg", FieldDescriptorProto::LABEL_OPTIONAL, "my.pkg");
    ASSERT_TRUE(consumer);

    const auto* fd = consumer->message_type(0)->field(0);
    auto m = MapField(fd);
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->nested_class, "::DepMsgArrowRow");
    EXPECT_EQ(m->nested_header, "dep_no_pkg.fletcher.pb.h");
}

TEST(TypeMapperTest, CrossFileRepeatedMessageSucceeds) {
    DescriptorPool pool;
    ASSERT_TRUE(BuildDepFile(pool, "dep_repeated.proto", "ext"));

    auto* consumer = BuildConsumerFile(
        pool, "consumer_repeated.proto", "dep_repeated.proto",
        ".ext.DepMsg", FieldDescriptorProto::LABEL_REPEATED, "mine");
    ASSERT_TRUE(consumer);

    const auto* fd = consumer->message_type(0)->field(0);
    auto m = MapField(fd);
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->kind, FieldKind::REPEATED_STRUCT);
    EXPECT_EQ(m->nested_class, "::ext::DepMsgArrowRow");
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
    EXPECT_EQ(m->map_value_class, "::ext::DepMsgArrowRow");
    EXPECT_EQ(m->map_value_header, "dep_map_val.fletcher.pb.h");
}

TEST(TypeMapperTest, CrossFileReferenceIsNoLongerUnsupported) {
    DescriptorPool pool;
    ASSERT_TRUE(BuildDepFile(pool, "dep_reason.proto", "ext"));

    auto* consumer = BuildConsumerFile(
        pool, "consumer_reason.proto", "dep_reason.proto",
        ".ext.DepMsg", FieldDescriptorProto::LABEL_OPTIONAL, "mine");
    ASSERT_TRUE(consumer);

    // MapField should now succeed for cross-file references.
    const auto* fd = consumer->message_type(0)->field(0);
    EXPECT_TRUE(MapField(fd).has_value());
}
