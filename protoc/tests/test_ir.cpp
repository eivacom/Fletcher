// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// GIR-3 forcing test. Proves the recursive, language-neutral mapped-type IR:
//   - scalar logical identity (LogicalKind), nullable from facts
//   - enum identity preserved (descriptor + symbol table) while storage is INT32
//   - WKT logical distinctions (Timestamp/Duration/wrappers are NOT generic ints)
//   - dictionary is a scalar MODIFIER (facts.dictionary), never a container
//   - nesting (List / Struct / Map / nested List via flatten chains)
//   - Unsupported{reason} replaces the former nullopt ambiguity
//   - C++ strings live ONLY in the cpp_backend lookup, never on an IR node
//   - single-source: MapField == ProjectIrToFieldMapping(BuildFieldIr(...))

#include <google/protobuf/any.pb.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/duration.pb.h>
#include <google/protobuf/struct.pb.h>
#include <google/protobuf/timestamp.pb.h>
#include <google/protobuf/wrappers.pb.h>
#include <gtest/gtest.h>

#include <sstream>
#include <string>

#include "cpp_backend_decode_visitor.hpp"
#include "cpp_backend_type_table.hpp"
#include "ir.hpp"
#include "type_mapper.hpp"

using namespace fletcher;
using namespace google::protobuf;

namespace {

// Copy the well-known type files this test imports into a local pool. Referencing
// the linked-in C++ WKT types forces their descriptors to register, so the file
// descriptors are always available (unlike a bare generated_pool name lookup,
// which can miss if the well-known object files were never otherwise referenced).
void AddWktDeps(DescriptorPool& pool) {
    const FileDescriptor* files[] = {
        google::protobuf::Timestamp::GetDescriptor()->file(),
        google::protobuf::Duration::GetDescriptor()->file(),
        google::protobuf::Int32Value::GetDescriptor()->file(),  // wrappers.proto
        google::protobuf::Any::GetDescriptor()->file(),
        google::protobuf::Struct::GetDescriptor()->file(),
    };
    for (const FileDescriptor* fd : files) {
        if (pool.FindFileByName(fd->name()) != nullptr) continue;  // e.g. wrappers shared
        FileDescriptorProto p;
        fd->CopyTo(&p);
        ASSERT_NE(pool.BuildFile(p), nullptr) << fd->name();
    }
}

// Mark a message with the (fletcher.flatten) message option (field number 50000)
// via an unknown varint field, matching how type_mapper detects it.
void SetMessageFlatten(DescriptorProto* msg) {
    auto* opts = msg->mutable_options();
    opts->GetReflection()->MutableUnknownFields(opts)->AddVarint(50000, 1);
}

const ir::ScalarNode& AsScalar(const ir::IrNode& n) {
    return std::get<ir::ScalarNode>(n.node);
}
const ir::ListNode& AsList(const ir::IrNode& n) { return std::get<ir::ListNode>(n.node); }
const ir::StructNode& AsStruct(const ir::IrNode& n) { return std::get<ir::StructNode>(n.node); }
const ir::MapNode& AsMap(const ir::IrNode& n) { return std::get<ir::MapNode>(n.node); }
const ir::UnsupportedNode& AsUnsupported(const ir::IrNode& n) {
    return std::get<ir::UnsupportedNode>(n.node);
}

FieldDescriptorProto* AddField(DescriptorProto* msg, const std::string& name, int number,
                               FieldDescriptorProto::Type type,
                               FieldDescriptorProto::Label label =
                                   FieldDescriptorProto::LABEL_OPTIONAL) {
    auto* f = msg->add_field();
    f->set_name(name);
    f->set_number(number);
    f->set_type(type);
    f->set_label(label);
    return f;
}

}  // namespace

// ===========================================================================
// Main forcing suite
// ===========================================================================

TEST(IrTest, BuildsLanguageNeutralIr) {
    DescriptorPool pool;

    // ---- (1) scalar logical identity --------------------------------------
    FileDescriptorProto sfp;
    sfp.set_name("ir_scalars.proto");
    sfp.set_syntax("proto3");
    auto* sm = sfp.add_message_type();
    sm->set_name("Scalars");
    AddField(sm, "b", 1, FieldDescriptorProto::TYPE_BOOL);
    AddField(sm, "i32", 2, FieldDescriptorProto::TYPE_INT32);
    AddField(sm, "i64", 3, FieldDescriptorProto::TYPE_INT64);
    AddField(sm, "u32", 4, FieldDescriptorProto::TYPE_UINT32);
    AddField(sm, "u64", 5, FieldDescriptorProto::TYPE_UINT64);
    AddField(sm, "f", 6, FieldDescriptorProto::TYPE_FLOAT);
    AddField(sm, "d", 7, FieldDescriptorProto::TYPE_DOUBLE);
    AddField(sm, "s", 8, FieldDescriptorProto::TYPE_STRING);
    AddField(sm, "by", 9, FieldDescriptorProto::TYPE_BYTES);
    // proto3 `optional int32 opt_i32` needs its synthetic single-member oneof.
    auto* opt_oneof = sm->add_oneof_decl();
    opt_oneof->set_name("_opt_i32");
    auto* opt_f = AddField(sm, "opt_i32", 10, FieldDescriptorProto::TYPE_INT32);
    opt_f->set_proto3_optional(true);
    opt_f->set_oneof_index(0);
    const FileDescriptor* sfile = pool.BuildFile(sfp);
    ASSERT_NE(sfile, nullptr);
    const Descriptor* scalars = sfile->message_type(0);

    auto kind_of = [&](int idx) {
        const auto ir = ir::BuildFieldIr(scalars->field(idx));
        EXPECT_EQ(ir.kind, ir::NodeKind::SCALAR);
        return AsScalar(ir).logical_type.kind;
    };
    EXPECT_EQ(kind_of(0), ir::LogicalKind::BOOL);
    EXPECT_EQ(kind_of(1), ir::LogicalKind::INT32);
    EXPECT_EQ(kind_of(2), ir::LogicalKind::INT64);
    EXPECT_EQ(kind_of(3), ir::LogicalKind::UINT32);
    EXPECT_EQ(kind_of(4), ir::LogicalKind::UINT64);
    EXPECT_EQ(kind_of(5), ir::LogicalKind::FLOAT32);
    EXPECT_EQ(kind_of(6), ir::LogicalKind::FLOAT64);
    EXPECT_EQ(kind_of(7), ir::LogicalKind::UTF8);
    EXPECT_EQ(kind_of(8), ir::LogicalKind::BINARY);

    // nullable comes from facts, and only from facts.
    EXPECT_FALSE(ir::BuildFieldIr(scalars->field(1)).facts.nullable);   // plain int32
    EXPECT_TRUE(ir::BuildFieldIr(scalars->field(9)).facts.nullable);    // optional int32
    // NOTE: the IR carries no C++ type string — ir::ScalarNode exposes only
    // logical_type + enum_identity (there is no arrow_type_expr/storage_type/…).

    // ---- (2) enum identity -------------------------------------------------
    FileDescriptorProto efp;
    efp.set_name("ir_enum.proto");
    efp.set_syntax("proto3");
    auto* col = efp.add_enum_type();
    col->set_name("Color");
    auto* v0 = col->add_value();
    v0->set_name("RED");
    v0->set_number(0);
    auto* v1 = col->add_value();
    v1->set_name("GREEN");
    v1->set_number(1);
    auto* em = efp.add_message_type();
    em->set_name("HasEnum");
    AddField(em, "color", 1, FieldDescriptorProto::TYPE_ENUM)->set_type_name(".Color");
    const FileDescriptor* efile = pool.BuildFile(efp);
    ASSERT_NE(efile, nullptr);

    const auto enum_ir = ir::BuildFieldIr(efile->message_type(0)->field(0));
    ASSERT_EQ(enum_ir.kind, ir::NodeKind::SCALAR);
    const auto& enum_scalar = AsScalar(enum_ir);
    EXPECT_EQ(enum_scalar.logical_type.kind, ir::LogicalKind::INT32);
    ASSERT_TRUE(enum_scalar.enum_identity.has_value());
    EXPECT_EQ(enum_scalar.enum_identity->full_name, "Color");
    ASSERT_EQ(enum_scalar.enum_identity->symbols.size(), 2u);
    EXPECT_EQ(enum_scalar.enum_identity->symbols[0].name, "RED");
    EXPECT_EQ(enum_scalar.enum_identity->symbols[0].number, 0);
    EXPECT_EQ(enum_scalar.enum_identity->symbols[1].name, "GREEN");
    EXPECT_EQ(enum_scalar.enum_identity->symbols[1].number, 1);

    // ---- (3) WKT distinctions ---------------------------------------------
    DescriptorPool wpool;
    AddWktDeps(wpool);
    FileDescriptorProto wfp;
    wfp.set_name("ir_wkt.proto");
    wfp.set_syntax("proto3");
    wfp.add_dependency("google/protobuf/timestamp.proto");
    wfp.add_dependency("google/protobuf/duration.proto");
    wfp.add_dependency("google/protobuf/wrappers.proto");
    auto* wm = wfp.add_message_type();
    wm->set_name("Wkt");
    AddField(wm, "ts", 1, FieldDescriptorProto::TYPE_MESSAGE)
        ->set_type_name(".google.protobuf.Timestamp");
    AddField(wm, "du", 2, FieldDescriptorProto::TYPE_MESSAGE)
        ->set_type_name(".google.protobuf.Duration");
    AddField(wm, "wi32", 3, FieldDescriptorProto::TYPE_MESSAGE)
        ->set_type_name(".google.protobuf.Int32Value");
    AddField(wm, "wstr", 4, FieldDescriptorProto::TYPE_MESSAGE)
        ->set_type_name(".google.protobuf.StringValue");
    const FileDescriptor* wfile = wpool.BuildFile(wfp);
    ASSERT_NE(wfile, nullptr);
    const Descriptor* wkt = wfile->message_type(0);

    const auto ts_ir = ir::BuildFieldIr(wkt->field(0));
    ASSERT_EQ(ts_ir.kind, ir::NodeKind::SCALAR);
    EXPECT_EQ(AsScalar(ts_ir).logical_type.kind, ir::LogicalKind::WKT_TIMESTAMP);
    EXPECT_NE(AsScalar(ts_ir).logical_type.kind, ir::LogicalKind::INT64);
    ASSERT_TRUE(AsScalar(ts_ir).logical_type.time_unit.has_value());
    EXPECT_EQ(*AsScalar(ts_ir).logical_type.time_unit, ir::TimeUnit::NANO);
    EXPECT_EQ(ts_ir.facts.wkt, ir::WktKind::TIMESTAMP);

    const auto du_ir = ir::BuildFieldIr(wkt->field(1));
    ASSERT_EQ(du_ir.kind, ir::NodeKind::SCALAR);
    EXPECT_EQ(AsScalar(du_ir).logical_type.kind, ir::LogicalKind::WKT_DURATION);
    ASSERT_TRUE(AsScalar(du_ir).logical_type.time_unit.has_value());
    EXPECT_EQ(*AsScalar(du_ir).logical_type.time_unit, ir::TimeUnit::NANO);
    EXPECT_EQ(du_ir.facts.wkt, ir::WktKind::DURATION);

    const auto wi32_ir = ir::BuildFieldIr(wkt->field(2));
    ASSERT_EQ(wi32_ir.kind, ir::NodeKind::SCALAR);
    EXPECT_EQ(AsScalar(wi32_ir).logical_type.kind, ir::LogicalKind::INT32);
    EXPECT_TRUE(wi32_ir.facts.nullable);  // wrappers exist to express nullable T
    EXPECT_EQ(wi32_ir.facts.wkt, ir::WktKind::WRAPPER_INT32);

    const auto wstr_ir = ir::BuildFieldIr(wkt->field(3));
    ASSERT_EQ(wstr_ir.kind, ir::NodeKind::SCALAR);
    EXPECT_EQ(AsScalar(wstr_ir).logical_type.kind, ir::LogicalKind::UTF8);
    EXPECT_TRUE(wstr_ir.facts.nullable);
    EXPECT_EQ(wstr_ir.facts.wkt, ir::WktKind::WRAPPER_STRING);

    // ---- (4) dictionary is a scalar modifier, never a container -----------
    // No DICT proto annotation exists in-tree yet, so cover the invariant at the
    // IR-model level: a dictionary-modified scalar stays a SCALAR node.
    ir::IrNode dict_node;
    dict_node.kind = ir::NodeKind::SCALAR;
    dict_node.facts.dictionary = true;
    dict_node.node = ir::ScalarNode{ir::LogicalType{ir::LogicalKind::UTF8, {}, {}, {}, {}, {}, {}},
                                    std::nullopt};
    EXPECT_EQ(dict_node.kind, ir::NodeKind::SCALAR);
    EXPECT_TRUE(dict_node.facts.dictionary);
    EXPECT_TRUE(std::holds_alternative<ir::ScalarNode>(dict_node.node));
    EXPECT_FALSE(std::holds_alternative<ir::MapNode>(dict_node.node));
    EXPECT_FALSE(std::holds_alternative<ir::StructNode>(dict_node.node));
    EXPECT_FALSE(std::holds_alternative<ir::ListNode>(dict_node.node));
    // BuildFieldIr never fabricates a dictionary without an annotation.
    EXPECT_FALSE(ir::BuildFieldIr(scalars->field(1)).facts.dictionary);

    // ---- (5) nesting -------------------------------------------------------
    FileDescriptorProto nfp;
    nfp.set_name("ir_nest.proto");
    nfp.set_syntax("proto3");

    auto* inner = nfp.add_message_type();
    inner->set_name("Inner");
    AddField(inner, "value", 1, FieldDescriptorProto::TYPE_STRING);

    // Flatten wrapper: repeated Inner values = 1.
    auto* wrap = nfp.add_message_type();
    wrap->set_name("StructListWrapper");
    SetMessageFlatten(wrap);
    AddField(wrap, "values", 1, FieldDescriptorProto::TYPE_MESSAGE,
             FieldDescriptorProto::LABEL_REPEATED)
        ->set_type_name(".Inner");

    auto* host = nfp.add_message_type();
    host->set_name("Host");
    AddField(host, "rep_i32", 1, FieldDescriptorProto::TYPE_INT32,
             FieldDescriptorProto::LABEL_REPEATED);
    AddField(host, "inner", 2, FieldDescriptorProto::TYPE_MESSAGE)->set_type_name(".Inner");
    AddField(host, "rep_inner", 3, FieldDescriptorProto::TYPE_MESSAGE,
             FieldDescriptorProto::LABEL_REPEATED)
        ->set_type_name(".Inner");
    // map<string,int32> and map<string,Inner>
    auto* e_si = host->add_nested_type();
    e_si->set_name("MapScalarEntry");
    e_si->mutable_options()->set_map_entry(true);
    AddField(e_si, "key", 1, FieldDescriptorProto::TYPE_STRING);
    AddField(e_si, "value", 2, FieldDescriptorProto::TYPE_INT32);
    AddField(host, "map_scalar", 4, FieldDescriptorProto::TYPE_MESSAGE,
             FieldDescriptorProto::LABEL_REPEATED)
        ->set_type_name(".Host.MapScalarEntry");
    auto* e_ss = host->add_nested_type();
    e_ss->set_name("MapStructEntry");
    e_ss->mutable_options()->set_map_entry(true);
    AddField(e_ss, "key", 1, FieldDescriptorProto::TYPE_STRING);
    AddField(e_ss, "value", 2, FieldDescriptorProto::TYPE_MESSAGE)->set_type_name(".Inner");
    AddField(host, "map_struct", 5, FieldDescriptorProto::TYPE_MESSAGE,
             FieldDescriptorProto::LABEL_REPEATED)
        ->set_type_name(".Host.MapStructEntry");
    // flatten wrapper used singular (repeated inner) -> List<Struct>
    AddField(host, "flat_single", 6, FieldDescriptorProto::TYPE_MESSAGE)
        ->set_type_name(".StructListWrapper");
    // flatten wrapper used repeated -> List<List<Struct>>
    AddField(host, "flat_nested", 7, FieldDescriptorProto::TYPE_MESSAGE,
             FieldDescriptorProto::LABEL_REPEATED)
        ->set_type_name(".StructListWrapper");
    // real oneof -> Unsupported
    auto* oo = host->add_oneof_decl();
    oo->set_name("choice");
    AddField(host, "a", 8, FieldDescriptorProto::TYPE_INT32)->set_oneof_index(0);
    AddField(host, "b", 9, FieldDescriptorProto::TYPE_STRING)->set_oneof_index(0);

    const FileDescriptor* nfile = pool.BuildFile(nfp);
    ASSERT_NE(nfile, nullptr);
    const Descriptor* h = nfile->message_type(2);

    // repeated int32 -> List<Scalar(INT32)>
    const auto rep_i32 = ir::BuildFieldIr(h->field(0));
    ASSERT_EQ(rep_i32.kind, ir::NodeKind::LIST);
    ASSERT_EQ(AsList(rep_i32).element->kind, ir::NodeKind::SCALAR);
    EXPECT_EQ(AsScalar(*AsList(rep_i32).element).logical_type.kind, ir::LogicalKind::INT32);

    // Inner inner -> Struct (identity preserved)
    const auto inner_ir = ir::BuildFieldIr(h->field(1));
    ASSERT_EQ(inner_ir.kind, ir::NodeKind::STRUCT);
    EXPECT_EQ(AsStruct(inner_ir).identity.full_name, "Inner");
    EXPECT_EQ(AsStruct(inner_ir).identity.descriptor, nfile->message_type(0));

    // repeated Inner -> List<Struct>
    const auto rep_inner = ir::BuildFieldIr(h->field(2));
    ASSERT_EQ(rep_inner.kind, ir::NodeKind::LIST);
    ASSERT_EQ(AsList(rep_inner).element->kind, ir::NodeKind::STRUCT);
    EXPECT_EQ(AsStruct(*AsList(rep_inner).element).identity.full_name, "Inner");

    // map<string,int32> -> Map<Scalar(UTF8), Scalar(INT32)>
    const auto map_scalar = ir::BuildFieldIr(h->field(3));
    ASSERT_EQ(map_scalar.kind, ir::NodeKind::MAP);
    EXPECT_EQ(AsScalar(*AsMap(map_scalar).key).logical_type.kind, ir::LogicalKind::UTF8);
    ASSERT_EQ(AsMap(map_scalar).value->kind, ir::NodeKind::SCALAR);
    EXPECT_EQ(AsScalar(*AsMap(map_scalar).value).logical_type.kind, ir::LogicalKind::INT32);

    // map<string,Inner> -> Map<Scalar(UTF8), Struct>
    const auto map_struct = ir::BuildFieldIr(h->field(4));
    ASSERT_EQ(map_struct.kind, ir::NodeKind::MAP);
    EXPECT_EQ(AsMap(map_struct).value->kind, ir::NodeKind::STRUCT);

    // flatten singular wrapper (repeated inner) -> List<Struct>
    const auto flat_single = ir::BuildFieldIr(h->field(5));
    ASSERT_EQ(flat_single.kind, ir::NodeKind::LIST);
    EXPECT_EQ(AsList(flat_single).element->kind, ir::NodeKind::STRUCT);

    // flatten repeated wrapper -> List<List<Struct>>
    const auto flat_nested = ir::BuildFieldIr(h->field(6));
    ASSERT_EQ(flat_nested.kind, ir::NodeKind::LIST);
    ASSERT_EQ(AsList(flat_nested).element->kind, ir::NodeKind::LIST);
    EXPECT_EQ(AsList(*AsList(flat_nested).element).element->kind, ir::NodeKind::STRUCT);

    // real oneof -> Unsupported{reason}
    const auto oneof_ir = ir::BuildFieldIr(h->field(7));
    ASSERT_EQ(oneof_ir.kind, ir::NodeKind::UNSUPPORTED);
    EXPECT_FALSE(AsUnsupported(oneof_ir).reason.empty());
    EXPECT_NE(AsUnsupported(oneof_ir).reason.find("oneof"), std::string::npos);

    // ---- (6) Unsupported reasons ------------------------------------------
    FileDescriptorProto ufp;
    ufp.set_name("ir_unsupported.proto");
    ufp.set_syntax("proto3");
    ufp.add_dependency("google/protobuf/any.proto");
    ufp.add_dependency("google/protobuf/struct.proto");
    auto* tree = ufp.add_message_type();
    tree->set_name("Tree");
    AddField(tree, "children", 1, FieldDescriptorProto::TYPE_MESSAGE,
             FieldDescriptorProto::LABEL_REPEATED)
        ->set_type_name(".Tree");
    auto* um = ufp.add_message_type();
    um->set_name("U");
    AddField(um, "any", 1, FieldDescriptorProto::TYPE_MESSAGE)
        ->set_type_name(".google.protobuf.Any");
    AddField(um, "st", 2, FieldDescriptorProto::TYPE_MESSAGE)
        ->set_type_name(".google.protobuf.Struct");
    AddField(um, "rec", 3, FieldDescriptorProto::TYPE_MESSAGE)->set_type_name(".Tree");
    const FileDescriptor* ufile = wpool.BuildFile(ufp);
    ASSERT_NE(ufile, nullptr);
    const Descriptor* u = ufile->message_type(1);

    for (int i = 0; i < 3; ++i) {
        const auto un = ir::BuildFieldIr(u->field(i));
        ASSERT_EQ(un.kind, ir::NodeKind::UNSUPPORTED) << "field " << i;
        EXPECT_FALSE(AsUnsupported(un).reason.empty()) << "field " << i;
    }
    EXPECT_NE(AsUnsupported(ir::BuildFieldIr(u->field(0))).reason.find("Any"), std::string::npos);
    EXPECT_NE(AsUnsupported(ir::BuildFieldIr(u->field(1))).reason.find("Struct"),
              std::string::npos);
    EXPECT_NE(AsUnsupported(ir::BuildFieldIr(u->field(2))).reason.find("recursive"),
              std::string::npos);
}

// ===========================================================================
// (7) C++ backend lookup: strings live here, not on IR nodes
// ===========================================================================

TEST(IrTest, CppBackendLookupOwnsTypeStrings) {
    ir::LogicalType i32{ir::LogicalKind::INT32, {}, {}, {}, {}, {}, {}};
    const auto& info = cpp_backend::LookupScalar(i32, std::nullopt);
    EXPECT_EQ(info.arrow_type_expr, "arrow::int32()");
    EXPECT_EQ(info.storage_type, "int32_t");
    EXPECT_EQ(info.positional_write, "WriteInt32");

    ir::LogicalType utf8{ir::LogicalKind::UTF8, {}, {}, {}, {}, {}, {}};
    EXPECT_EQ(cpp_backend::LookupScalar(utf8, std::nullopt).positional_write, "WriteString");

    ir::LogicalType bin{ir::LogicalKind::BINARY, {}, {}, {}, {}, {}, {}};
    EXPECT_EQ(cpp_backend::LookupScalar(bin, std::nullopt).positional_write, "WriteBinary");

    ir::LogicalType wts{ir::LogicalKind::WKT_TIMESTAMP, {}, ir::TimeUnit::NANO, {}, {}, {}, {}};
    EXPECT_EQ(cpp_backend::LookupScalar(wts, std::nullopt).positional_write, "WriteTimestamp");

    // Enum-as-INT32 carries a static_cast in its scalar ctor, distinguishing it
    // from a plain INT32 while still lowering to int32_t storage.
    ir::EnumIdentity eid;
    eid.full_name = "Color";
    const auto& enum_info = cpp_backend::LookupScalar(i32, eid);
    EXPECT_EQ(enum_info.storage_type, "int32_t");
    EXPECT_NE(enum_info.scalar_ctor.find("static_cast"), std::string::npos);
}

// ===========================================================================
// (8) single-source: MapField is a thin wrapper over BuildFieldIr + projection
// ===========================================================================

namespace {

const FileDescriptor* BuildScalarField(DescriptorPool& pool, const std::string& tag,
                                       FieldDescriptorProto::Type type,
                                       FieldDescriptorProto::Label label =
                                           FieldDescriptorProto::LABEL_OPTIONAL) {
    FileDescriptorProto fdp;
    fdp.set_name("ss_" + tag + ".proto");
    fdp.set_syntax("proto3");
    auto* msg = fdp.add_message_type();
    msg->set_name("Msg");
    AddField(msg, "field", 1, type, label);
    return pool.BuildFile(fdp);
}

void ExpectSameScalar(const ScalarTypeInfo& a, const ScalarTypeInfo& b) {
    EXPECT_EQ(a.arrow_type_expr, b.arrow_type_expr);
    EXPECT_EQ(a.storage_type, b.storage_type);
    EXPECT_EQ(a.param_type, b.param_type);
    EXPECT_EQ(a.scalar_ctor, b.scalar_ctor);
    EXPECT_EQ(a.default_value, b.default_value);
    EXPECT_EQ(a.builder_type, b.builder_type);
    EXPECT_EQ(a.scalar_type, b.scalar_type);
    EXPECT_EQ(a.value_is_buffer, b.value_is_buffer);
}

}  // namespace

TEST(IrTest, MapFieldEqualsProjectionOfBuildFieldIr) {
    DescriptorPool pool;
    const FieldDescriptor* f =
        BuildScalarField(pool, "i32", FieldDescriptorProto::TYPE_INT32)->message_type(0)->field(0);

    // BuildFieldIr is deterministic for the same descriptor.
    const auto ir1 = ir::BuildFieldIr(f);
    const auto ir2 = ir::BuildFieldIr(f);
    ASSERT_EQ(ir1.kind, ir2.kind);
    EXPECT_EQ(AsScalar(ir1).logical_type.kind, AsScalar(ir2).logical_type.kind);

    // MapField() == ProjectIrToFieldMapping(BuildFieldIr(...)).
    const auto via_mapfield = MapField(f);
    const auto via_projection = ProjectIrToFieldMapping(ir::BuildFieldIr(f), f->file());
    ASSERT_TRUE(via_mapfield.has_value());
    ASSERT_TRUE(via_projection.has_value());
    EXPECT_EQ(via_mapfield->kind, via_projection->kind);
    EXPECT_EQ(via_mapfield->nullable, via_projection->nullable);
    ExpectSameScalar(via_mapfield->scalar, via_projection->scalar);
}

// ===========================================================================
// (9) GIR-4 edge DECODE visitor: IR-driven positional reads. These are focused
// SHAPE assertions on cpp_backend::EmitFieldDecodeFromIr (byte/behaviour-
// identical migration of the former FieldMapping-switch EmitFieldDecode); the
// cross-fixture value oracle lives in the coverage harness, not here.
// ===========================================================================

namespace {

std::string DecodeOf(const ir::IrNode& node, const std::string& member, size_t idx,
                     const FileDescriptor* ctx) {
    std::ostringstream o;
    cpp_backend::EmitFieldDecodeFromIr(o, node, member, idx, ctx);
    return o.str();
}

bool Contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

TEST(IrTest, EdgeDecodeVisitorEmitsPositionalReads) {
    DescriptorPool pool;
    FileDescriptorProto fp;
    fp.set_name("ir_decode.proto");
    fp.set_syntax("proto3");

    auto* inner = fp.add_message_type();
    inner->set_name("Inner");
    AddField(inner, "value", 1, FieldDescriptorProto::TYPE_STRING);

    // Flatten wrapper (repeated Inner) → List<List<Struct>> when used repeated.
    auto* wrap = fp.add_message_type();
    wrap->set_name("StructListWrapper");
    SetMessageFlatten(wrap);
    AddField(wrap, "values", 1, FieldDescriptorProto::TYPE_MESSAGE,
             FieldDescriptorProto::LABEL_REPEATED)
        ->set_type_name(".Inner");

    auto* host = fp.add_message_type();
    host->set_name("Host");
    AddField(host, "i32", 1, FieldDescriptorProto::TYPE_INT32);
    AddField(host, "s", 2, FieldDescriptorProto::TYPE_STRING);
    AddField(host, "by", 3, FieldDescriptorProto::TYPE_BYTES);
    AddField(host, "inner", 4, FieldDescriptorProto::TYPE_MESSAGE)->set_type_name(".Inner");
    AddField(host, "rep_inner", 5, FieldDescriptorProto::TYPE_MESSAGE,
             FieldDescriptorProto::LABEL_REPEATED)
        ->set_type_name(".Inner");
    AddField(host, "rep_i32", 6, FieldDescriptorProto::TYPE_INT32,
             FieldDescriptorProto::LABEL_REPEATED);
    auto* e_si = host->add_nested_type();
    e_si->set_name("MapScalarEntry");
    e_si->mutable_options()->set_map_entry(true);
    AddField(e_si, "key", 1, FieldDescriptorProto::TYPE_STRING);
    AddField(e_si, "value", 2, FieldDescriptorProto::TYPE_INT32);
    AddField(host, "map_scalar", 7, FieldDescriptorProto::TYPE_MESSAGE,
             FieldDescriptorProto::LABEL_REPEATED)
        ->set_type_name(".Host.MapScalarEntry");
    auto* e_ss = host->add_nested_type();
    e_ss->set_name("MapStructEntry");
    e_ss->mutable_options()->set_map_entry(true);
    AddField(e_ss, "key", 1, FieldDescriptorProto::TYPE_STRING);
    AddField(e_ss, "value", 2, FieldDescriptorProto::TYPE_MESSAGE)->set_type_name(".Inner");
    AddField(host, "map_struct", 8, FieldDescriptorProto::TYPE_MESSAGE,
             FieldDescriptorProto::LABEL_REPEATED)
        ->set_type_name(".Host.MapStructEntry");
    AddField(host, "flat_nested", 9, FieldDescriptorProto::TYPE_MESSAGE,
             FieldDescriptorProto::LABEL_REPEATED)
        ->set_type_name(".StructListWrapper");

    const FileDescriptor* file = pool.BuildFile(fp);
    ASSERT_NE(file, nullptr);
    const Descriptor* h = file->message_type(2);
    const FileDescriptor* ctx = file;

    const ir::IrNode i32 = ir::BuildFieldIr(h->field(0));
    const ir::IrNode str = ir::BuildFieldIr(h->field(1));
    const ir::IrNode by = ir::BuildFieldIr(h->field(2));
    const ir::IrNode inner_ir = ir::BuildFieldIr(h->field(3));
    const ir::IrNode rep_inner = ir::BuildFieldIr(h->field(4));
    const ir::IrNode map_scalar = ir::BuildFieldIr(h->field(6));
    const ir::IrNode map_struct = ir::BuildFieldIr(h->field(7));
    const ir::IrNode flat_nested = ir::BuildFieldIr(h->field(8));

    // (a) non-nullable scalar: direct read, no null check; uses backend lookup.
    EXPECT_EQ(DecodeOf(i32, "i32_", 1, ctx), "        i32_ = r.ReadInt32();\n");
    EXPECT_FALSE(Contains(DecodeOf(i32, "i32_", 1, ctx), "IsNull"));
    EXPECT_TRUE(Contains(
        DecodeOf(i32, "i32_", 1, ctx),
        cpp_backend::LookupScalar(std::get<ir::ScalarNode>(i32.node).logical_type, std::nullopt)
            .positional_read));

    // (b) nullable scalar: gated on IsNull(idx) (facts.nullable is the source).
    ir::IrNode i32_null = ir::BuildFieldIr(h->field(0));
    i32_null.facts.nullable = true;
    EXPECT_EQ(DecodeOf(i32_null, "opt_", 2, ctx),
              "        if (!r.IsNull(2)) opt_ = r.ReadInt32();\n");

    // (c) string is COPIED into an owned std::string, not borrowed.
    EXPECT_EQ(DecodeOf(str, "s_", 2, ctx), "        s_ = std::string(r.ReadString());\n");

    // (d) binary is COPIED (reinterpret_cast into owned string), not borrowed.
    EXPECT_EQ(DecodeOf(by, "by_", 3, ctx),
              "        { auto [p, n] = r.ReadBinary();\n"
              "          by_.emplace(reinterpret_cast<const char*>(p), n); }\n");

    // (e) non-nullable struct: block, ReadStruct(<Class>Schema()->n_children), emplace.
    EXPECT_EQ(DecodeOf(inner_ir, "inner_", 4, ctx),
              "        { auto sr = r.ReadStruct(InnerSchema()->n_children);\n"
              "          inner_.emplace(sr); }\n");

    // (f) nullable struct: IsNull gate, then ReadStruct, then optional::emplace.
    ir::IrNode inner_null = ir::BuildFieldIr(h->field(3));
    inner_null.facts.nullable = true;
    EXPECT_EQ(DecodeOf(inner_null, "oi_", 6, ctx),
              "        if (!r.IsNull(6)) {\n"
              "            auto sr = r.ReadStruct(InnerSchema()->n_children);\n"
              "            oi_.emplace(sr);\n"
              "        }\n");

    // (g) single-level repeated struct: inline emplace_back(sr), and NO null gate
    //     (REPEATED_STRUCT never gates on the null bit — APPROVE-note invariant).
    const std::string rs = DecodeOf(rep_inner, "rs_", 5, ctx);
    EXPECT_EQ(rs,
              "        { auto lh = r.ReadListHeader();\n"
              "          rs_.clear();\n"
              "          rs_.reserve(lh.count);\n"
              "          for (uint32_t li_ = 0; li_ < lh.count; ++li_) {\n"
              "            auto sr = r.ReadStruct(InnerSchema()->n_children);\n"
              "            rs_.emplace_back(sr);\n"
              "          } }\n");
    EXPECT_FALSE(Contains(rs, "IsNull"));

    // (h) map: key pass, then ReadMapValueBitfield (MUST NOT be skipped), then
    //     value pass; key-first/value-second; moved key into vector-of-pairs.
    const std::string ms = DecodeOf(map_scalar, "ms_", 7, ctx);
    EXPECT_TRUE(Contains(ms, "auto vbf = r.ReadMapValueBitfield(count);"));
    EXPECT_TRUE(Contains(ms, "keys_.emplace_back(r.ReadString());"));
    EXPECT_TRUE(Contains(ms, "ms_.emplace_back(std::move(keys_[mi_]), r.ReadInt32());"));
    // key extraction precedes the value bitfield, which precedes value extraction.
    EXPECT_LT(ms.find("keys_.emplace_back"), ms.find("ReadMapValueBitfield"));
    EXPECT_LT(ms.find("ReadMapValueBitfield"), ms.find("ms_.emplace_back"));

    const std::string mst = DecodeOf(map_struct, "mst_", 8, ctx);
    EXPECT_TRUE(Contains(mst, "auto vbf = r.ReadMapValueBitfield(count);"));
    EXPECT_TRUE(Contains(mst, "mst_.emplace_back(std::move(keys_[mi_]), Inner(sr));"));

    // (i) nested list (List<List<Struct>>): resize + indexed assignment with
    //     FRESH loop vars at each depth; struct innermost; no null gate.
    const std::string nl = DecodeOf(flat_nested, "nl_", 9, ctx);
    EXPECT_TRUE(Contains(nl, "auto lh_0 = r.ReadListHeader();"));
    EXPECT_TRUE(Contains(nl, "nl_.resize(lh_0.count);"));
    EXPECT_TRUE(Contains(nl, "auto lh_1 = r.ReadListHeader();"));
    EXPECT_TRUE(Contains(nl, "nl_[i_0].resize(lh_1.count);"));
    EXPECT_TRUE(Contains(nl, "nl_[i_0][i_1] = Inner(sr);"));
    EXPECT_FALSE(Contains(nl, "push_back"));  // nested list uses resize, not push_back
    EXPECT_FALSE(Contains(nl, "IsNull"));

    // (j) Unsupported node: a diagnostic comment, never a silent decode body.
    ir::IrNode un;
    un.kind = ir::NodeKind::UNSUPPORTED;
    un.node = ir::UnsupportedNode{"map value type unsupported"};
    const std::string uo = DecodeOf(un, "x_", 0, ctx);
    EXPECT_TRUE(Contains(uo, "unsupported field skipped: map value type unsupported"));
    EXPECT_FALSE(Contains(uo, "ReadStruct"));
    EXPECT_FALSE(Contains(uo, "ReadInt32"));
}
