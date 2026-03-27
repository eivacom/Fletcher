#include <catch2/catch_all.hpp>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>

#include "type_mapper.hpp"

using namespace arrow_row_plugin;
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

TEST_CASE("MapField: bool") {
    DescriptorPool pool;
    auto m = MapField(First(BuildSingleField(pool, "bool", FieldDescriptorProto::TYPE_BOOL)));
    REQUIRE(m.has_value());
    CHECK(m->kind == FieldKind::SCALAR);
    CHECK(m->scalar.arrow_type_expr == "arrow::boolean()");
    CHECK(m->scalar.storage_type    == "bool");
    CHECK(m->scalar.builder_type    == "arrow::BooleanBuilder");
    CHECK(m->scalar.default_value   == "false");
}

TEST_CASE("MapField: int32") {
    DescriptorPool pool;
    auto m = MapField(First(BuildSingleField(pool, "int32", FieldDescriptorProto::TYPE_INT32)));
    REQUIRE(m.has_value());
    CHECK(m->kind == FieldKind::SCALAR);
    CHECK(m->scalar.arrow_type_expr == "arrow::int32()");
    CHECK(m->scalar.storage_type    == "int32_t");
}

TEST_CASE("MapField: sint32 maps to arrow::int32") {
    DescriptorPool pool;
    auto m = MapField(First(BuildSingleField(pool, "sint32", FieldDescriptorProto::TYPE_SINT32)));
    REQUIRE(m.has_value());
    CHECK(m->scalar.arrow_type_expr == "arrow::int32()");
}

TEST_CASE("MapField: int64") {
    DescriptorPool pool;
    auto m = MapField(First(BuildSingleField(pool, "int64", FieldDescriptorProto::TYPE_INT64)));
    REQUIRE(m.has_value());
    CHECK(m->scalar.arrow_type_expr == "arrow::int64()");
}

TEST_CASE("MapField: uint32") {
    DescriptorPool pool;
    auto m = MapField(First(BuildSingleField(pool, "uint32", FieldDescriptorProto::TYPE_UINT32)));
    REQUIRE(m.has_value());
    CHECK(m->scalar.arrow_type_expr == "arrow::uint32()");
}

TEST_CASE("MapField: uint64") {
    DescriptorPool pool;
    auto m = MapField(First(BuildSingleField(pool, "uint64", FieldDescriptorProto::TYPE_UINT64)));
    REQUIRE(m.has_value());
    CHECK(m->scalar.arrow_type_expr == "arrow::uint64()");
}

TEST_CASE("MapField: float") {
    DescriptorPool pool;
    auto m = MapField(First(BuildSingleField(pool, "float", FieldDescriptorProto::TYPE_FLOAT)));
    REQUIRE(m.has_value());
    CHECK(m->scalar.arrow_type_expr == "arrow::float32()");
}

TEST_CASE("MapField: double") {
    DescriptorPool pool;
    auto m = MapField(First(BuildSingleField(pool, "double", FieldDescriptorProto::TYPE_DOUBLE)));
    REQUIRE(m.has_value());
    CHECK(m->scalar.arrow_type_expr == "arrow::float64()");
}

TEST_CASE("MapField: string") {
    DescriptorPool pool;
    auto m = MapField(First(BuildSingleField(pool, "string", FieldDescriptorProto::TYPE_STRING)));
    REQUIRE(m.has_value());
    CHECK(m->scalar.arrow_type_expr == "arrow::utf8()");
    CHECK(m->scalar.storage_type    == "std::string");
    CHECK(m->scalar.param_type      == "std::string_view");
}

TEST_CASE("MapField: bytes") {
    DescriptorPool pool;
    auto m = MapField(First(BuildSingleField(pool, "bytes", FieldDescriptorProto::TYPE_BYTES)));
    REQUIRE(m.has_value());
    CHECK(m->scalar.arrow_type_expr == "arrow::binary()");
}

TEST_CASE("MapField: enum maps to int32") {
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
    REQUIRE(file);

    auto m = MapField(file->message_type(0)->field(0));
    REQUIRE(m.has_value());
    CHECK(m->kind == FieldKind::SCALAR);
    CHECK(m->scalar.arrow_type_expr == "arrow::int32()");
}

TEST_CASE("MapField: scalar_ctor contains {val} token") {
    DescriptorPool pool;
    auto m = MapField(First(BuildSingleField(pool, "tok", FieldDescriptorProto::TYPE_INT32)));
    REQUIRE(m.has_value());
    CHECK(m->scalar.scalar_ctor.find("{val}") != std::string::npos);
}

TEST_CASE("MapField: proto3 non-optional field is not nullable") {
    DescriptorPool pool;
    auto m = MapField(First(BuildSingleField(pool, "nopt", FieldDescriptorProto::TYPE_INT32)));
    REQUIRE(m.has_value());
    CHECK_FALSE(m->nullable);
}

// ===========================================================================
// Repeated scalar fields
// ===========================================================================

TEST_CASE("MapField: repeated int32 → REPEATED_SCALAR") {
    DescriptorPool pool;
    auto m = MapField(First(BuildSingleField(
        pool, "rep_i32", FieldDescriptorProto::TYPE_INT32,
        FieldDescriptorProto::LABEL_REPEATED)));
    REQUIRE(m.has_value());
    CHECK(m->kind == FieldKind::REPEATED_SCALAR);
    CHECK_FALSE(m->nullable);  // repeated fields are never null
    CHECK(m->element.arrow_type_expr == "arrow::int32()");
    CHECK(m->element.storage_type    == "int32_t");
    CHECK(m->element.builder_type    == "arrow::Int32Builder");
}

TEST_CASE("MapField: repeated string → REPEATED_SCALAR") {
    DescriptorPool pool;
    auto m = MapField(First(BuildSingleField(
        pool, "rep_str", FieldDescriptorProto::TYPE_STRING,
        FieldDescriptorProto::LABEL_REPEATED)));
    REQUIRE(m.has_value());
    CHECK(m->kind == FieldKind::REPEATED_SCALAR);
    CHECK(m->element.arrow_type_expr == "arrow::utf8()");
    CHECK(m->element.builder_type    == "arrow::StringBuilder");
}

// ===========================================================================
// Nested message fields (struct)
// ===========================================================================

TEST_CASE("MapField: singular message → STRUCT") {
    DescriptorPool pool;
    auto* file = BuildNestedMsg(pool, "struct", FieldDescriptorProto::LABEL_OPTIONAL);
    REQUIRE(file);
    // message_type(1) is Outer; field(0) is "inner"
    auto m = MapField(file->message_type(1)->field(0));
    REQUIRE(m.has_value());
    CHECK(m->kind == FieldKind::STRUCT);
    CHECK(m->nested_class == "InnerArrowRow");
}

TEST_CASE("MapField: non-optional struct is not nullable") {
    DescriptorPool pool;
    auto* file = BuildNestedMsg(pool, "struct_nonnull", FieldDescriptorProto::LABEL_OPTIONAL);
    REQUIRE(file);
    auto m = MapField(file->message_type(1)->field(0));
    REQUIRE(m.has_value());
    // In proto3 without 'optional' keyword, has_optional_keyword() is false.
    CHECK_FALSE(m->nullable);
}

TEST_CASE("MapField: repeated message → REPEATED_STRUCT") {
    DescriptorPool pool;
    auto* file = BuildNestedMsg(pool, "rep_struct", FieldDescriptorProto::LABEL_REPEATED);
    REQUIRE(file);
    auto m = MapField(file->message_type(1)->field(0));
    REQUIRE(m.has_value());
    CHECK(m->kind == FieldKind::REPEATED_STRUCT);
    CHECK_FALSE(m->nullable);
    CHECK(m->nested_class == "InnerArrowRow");
}

// ===========================================================================
// Map fields
// ===========================================================================

TEST_CASE("MapField: map<string, int32> → MAP") {
    DescriptorPool pool;
    auto* file = BuildMapField(pool, "map_str_i32", FieldDescriptorProto::TYPE_INT32);
    REQUIRE(file);
    auto m = MapField(file->message_type(0)->field(0));
    REQUIRE(m.has_value());
    CHECK(m->kind == FieldKind::MAP);
    CHECK_FALSE(m->nullable);
    CHECK(m->map_key.arrow_type_expr   == "arrow::utf8()");
    CHECK(m->map_key.storage_type      == "std::string");
    CHECK_FALSE(m->map_value_is_message);
    CHECK(m->map_value.arrow_type_expr == "arrow::int32()");
}

TEST_CASE("MapField: map emits a warning about compute support") {
    DescriptorPool pool;
    auto* file = BuildMapField(pool, "map_warn", FieldDescriptorProto::TYPE_INT32);
    REQUIRE(file);
    auto m = MapField(file->message_type(0)->field(0));
    REQUIRE(m.has_value());
    CHECK_FALSE(m->warning.empty());
    CHECK(m->warning.find("compute") != std::string::npos);
}

// ===========================================================================
// Oneof fields
// ===========================================================================

TEST_CASE("MapField: oneof field returns nullopt") {
    DescriptorPool pool;
    auto* file = BuildOneofMsg(pool);
    REQUIRE(file);
    // Both fields are in the oneof
    CHECK_FALSE(MapField(file->message_type(0)->field(0)).has_value());
    CHECK_FALSE(MapField(file->message_type(0)->field(1)).has_value());
}

TEST_CASE("UnsupportedReason: oneof mentions the oneof name") {
    DescriptorPool pool;
    auto* file = BuildOneofMsg(pool);
    REQUIRE(file);
    auto reason = UnsupportedReason(file->message_type(0)->field(0));
    CHECK(reason.find("payload") != std::string::npos);
    CHECK(reason.find("oneof") != std::string::npos);
}

// ===========================================================================
// Recursive messages
// ===========================================================================

TEST_CASE("IsRecursive: self-referencing message is recursive") {
    DescriptorPool pool;
    auto* file = BuildRecursiveMsg(pool);
    REQUIRE(file);
    CHECK(IsRecursive(file->message_type(0)));
}

TEST_CASE("IsRecursive: flat message is not recursive") {
    DescriptorPool pool;
    auto* file = BuildSingleField(pool, "flat", FieldDescriptorProto::TYPE_INT32);
    REQUIRE(file);
    CHECK_FALSE(IsRecursive(file->message_type(0)));
}

TEST_CASE("MapField: field referencing recursive message returns nullopt") {
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
    REQUIRE(file);
    CHECK_FALSE(MapField(file->message_type(1)->field(0)).has_value());
}

// ===========================================================================
// Nesting depth
// ===========================================================================

TEST_CASE("NestingDepth: flat message has depth 0") {
    DescriptorPool pool;
    auto* file = BuildSingleField(pool, "flat_depth", FieldDescriptorProto::TYPE_INT32);
    REQUIRE(file);
    CHECK(NestingDepth(file->message_type(0)) == 0);
}

TEST_CASE("NestingDepth: one level of nesting has depth 1") {
    DescriptorPool pool;
    auto* file = BuildNestedMsg(pool, "depth1", FieldDescriptorProto::LABEL_OPTIONAL);
    REQUIRE(file);
    // Outer → Inner (depth 1)
    CHECK(NestingDepth(file->message_type(1)) == 1);
}

// ===========================================================================
// ClassName
// ===========================================================================

TEST_CASE("ClassName: top-level message") {
    DescriptorPool pool;
    auto* file = BuildSingleField(pool, "cls", FieldDescriptorProto::TYPE_INT32);
    REQUIRE(file);
    CHECK(ClassName(file->message_type(0)) == "MsgArrowRow");
}

TEST_CASE("ClassName: nested message uses underscore separator") {
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
    REQUIRE(file);
    CHECK(ClassName(file->message_type(0)->nested_type(0)) == "Outer_InnerArrowRow");
}
