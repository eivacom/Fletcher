// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// GIR-7 P2 guard: declared-wrapper-name recovery must apply ONLY at the outer
// singular-wrapper level.
//
// A singular flatten wrapper that collapses to a SINGLE list level keeps its
// declared wrapper name (IStructListWrapper[] / StructListWrapper.fields). A
// singular flatten wrapper that collapses to a NESTED list keeps the inner leaf
// identity (ILeaf[][] / Leaf.fields) — the wrapper name must NOT propagate through
// the nested list levels. No active coverage fixture exercises the nested-singular
// shape, so this in-memory unit test pins the branch that the byte golden cannot.
// It builds its own descriptor pool and never touches the schema/.ipc/view goldens.

#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <gtest/gtest.h>

#include <string>

#include "ts_backend_visitor.hpp"

using namespace google::protobuf;

namespace {

// (fletcher.flatten) / (fletcher.flatten_field) extension number; the mapper reads
// it from the options' unknown-field set (see type_mapper.cpp:FindBoolOption), so
// an in-memory fixture can set it directly without linking the extension.
constexpr int kFlattenOptionNumber = 50000;

FieldDescriptorProto* AddField(DescriptorProto* msg, const char* name, int number,
                               FieldDescriptorProto::Type type,
                               FieldDescriptorProto::Label label = FieldDescriptorProto::LABEL_OPTIONAL) {
    auto* f = msg->add_field();
    f->set_name(name);
    f->set_number(number);
    f->set_type(type);
    f->set_label(label);
    return f;
}

void SetMessageFlatten(DescriptorProto* msg) {
    auto* opts = msg->mutable_options();
    opts->GetReflection()->MutableUnknownFields(opts)->AddVarint(kFlattenOptionNumber, 1);
}

const FileDescriptor* BuildFixture(DescriptorPool& pool) {
    FileDescriptorProto fdp;
    fdp.set_name("guard.proto");
    fdp.set_package("guard");
    fdp.set_syntax("proto3");

    // Leaf { int32 id = 1; }
    {
        auto* m = fdp.add_message_type();
        m->set_name("Leaf");
        AddField(m, "id", 1, FieldDescriptorProto::TYPE_INT32);
    }
    // StructListWrapper { option (fletcher.flatten) = true; repeated Leaf values = 1; }
    {
        auto* m = fdp.add_message_type();
        m->set_name("StructListWrapper");
        SetMessageFlatten(m);
        AddField(m, "values", 1, FieldDescriptorProto::TYPE_MESSAGE,
                 FieldDescriptorProto::LABEL_REPEATED)
            ->set_type_name(".guard.Leaf");
    }
    // NestedStructListWrapper { option (fletcher.flatten) = true;
    //                           repeated StructListWrapper values = 1; }
    {
        auto* m = fdp.add_message_type();
        m->set_name("NestedStructListWrapper");
        SetMessageFlatten(m);
        AddField(m, "values", 1, FieldDescriptorProto::TYPE_MESSAGE,
                 FieldDescriptorProto::LABEL_REPEATED)
            ->set_type_name(".guard.StructListWrapper");
    }
    // Holder { NestedStructListWrapper nested = 1;   // singular -> List<List<Struct>>
    //          StructListWrapper single = 2; }       // singular -> List<Struct>
    {
        auto* m = fdp.add_message_type();
        m->set_name("Holder");
        AddField(m, "nested", 1, FieldDescriptorProto::TYPE_MESSAGE)
            ->set_type_name(".guard.NestedStructListWrapper");
        AddField(m, "single", 2, FieldDescriptorProto::TYPE_MESSAGE)
            ->set_type_name(".guard.StructListWrapper");
    }
    return pool.BuildFile(fdp);
}

}  // namespace

TEST(TsVisitor, SingularNestedFlattenWrapperKeepsLeafName) {
    DescriptorPool pool;
    const FileDescriptor* file = BuildFixture(pool);
    ASSERT_NE(file, nullptr);

    const std::string ts = fletcher::ts_backend::TsVisitor(file).GenerateFile();

    // Singular wrapper -> single list level: keeps the DECLARED wrapper name (this
    // also guards the fix against over-correcting the byte-golden #12/#18 case).
    EXPECT_NE(ts.find("single: IStructListWrapper[];"), std::string::npos)
        << "singular single-level wrapper should keep the declared name:\n" << ts;

    // Singular wrapper -> NESTED list: keeps the LEAF identity, NOT the wrapper.
    EXPECT_NE(ts.find("nested: ILeaf[][];"), std::string::npos)
        << "singular nested wrapper should render the leaf type:\n" << ts;
    EXPECT_EQ(ts.find("INestedStructListWrapper[][]"), std::string::npos)
        << "declared wrapper name leaked through a nested list:\n" << ts;
}
