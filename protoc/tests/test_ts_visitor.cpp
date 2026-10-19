// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// A singular flatten wrapper is described by what it carries (D-BIND-119, 2026-10-19).
//
// `StructListWrapper single = 2;` is List<Struct(Leaf)> in the IR, in the schema and on the
// wire, so TypeScript renders ILeaf[] / Leaf.fields, as it renders the NESTED case
// (`NestedStructListWrapper nested = 1;` -> ILeaf[][] / Leaf.fields). Until D-BIND-119 the
// single-level case kept the declared wrapper name (IStructListWrapper[] /
// StructListWrapper.fields, GIR-7 P2), a descriptor for a list of the WRAPPER, which no other
// language writes or reads (found by integration-tests/gateway-dotnet-crosslang). This in-memory
// unit test builds its own descriptor pool and never touches the schema/.ipc/view goldens.

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

FieldDescriptorProto* AddField(
    DescriptorProto* msg, const char* name, int number, FieldDescriptorProto::Type type,
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

TEST(TsVisitor, SingularFlattenWrapperIsAListOfItsElement) {
    DescriptorPool pool;
    const FileDescriptor* file = BuildFixture(pool);
    ASSERT_NE(file, nullptr);

    const std::string ts = fletcher::ts_backend::TsVisitor(file).GenerateFile();

    // Singular wrapper -> single list level: a list of the LEAF, in the type and the descriptor.
    EXPECT_NE(ts.find("single: ILeaf[];"), std::string::npos) << ts;
    EXPECT_NE(ts.find("{ name: 'single', fieldNumber: 2, wireType: WireTypeId.LIST, nullable: "
                      "false, element: { name: '', fieldNumber: 0, wireType: WireTypeId.STRUCT, "
                      "nullable: false, fields: Leaf.fields } }"),
              std::string::npos)
        << ts;
    EXPECT_EQ(ts.find("IStructListWrapper[]"), std::string::npos)
        << "a list of the wrapper, which the wire does not carry:\n"
        << ts;
    EXPECT_EQ(ts.find("fields: StructListWrapper.fields"), std::string::npos) << ts;

    // Singular wrapper -> NESTED list: the leaf too, through every level.
    EXPECT_NE(ts.find("nested: ILeaf[][];"), std::string::npos) << ts;
    EXPECT_EQ(ts.find("INestedStructListWrapper[][]"), std::string::npos) << ts;
}

// D-BIND-118: a message of ANOTHER file is imported by name, as a value (its TypedSchema,
// whose `.fields` the descriptor names) and as a type (its interface). Before, the import
// was an empty placeholder (`import { /* cross-file types */ }`), so the file neither
// type-checked nor ran. Reached here as a struct, a list element, a map value and a
// singular flatten wrapper; a struct's own fields are its file's business, not imported.
TEST(TsVisitor, AnotherFilesMessageIsImportedByName) {
    DescriptorPool pool;
    FileDescriptorProto dep;
    dep.set_name("dir/dep.proto");
    dep.set_package("dep");
    dep.set_syntax("proto3");
    {
        auto* m = dep.add_message_type();
        m->set_name("Leaf");
        AddField(m, "id", 1, FieldDescriptorProto::TYPE_INT32);
    }
    {
        auto* m = dep.add_message_type();
        m->set_name("Point");
        AddField(m, "x", 1, FieldDescriptorProto::TYPE_DOUBLE);
    }
    {
        auto* m = dep.add_message_type();
        m->set_name("Inner");
        AddField(m, "v", 1, FieldDescriptorProto::TYPE_INT32);
    }
    {
        auto* m = dep.add_message_type();
        m->set_name("Outer");
        AddField(m, "inner", 1, FieldDescriptorProto::TYPE_MESSAGE)->set_type_name(".dep.Inner");
    }
    {
        auto* m = dep.add_message_type();
        m->set_name("Wrapper");
        SetMessageFlatten(m);
        AddField(m, "values", 1, FieldDescriptorProto::TYPE_MESSAGE,
                 FieldDescriptorProto::LABEL_REPEATED)
            ->set_type_name(".dep.Leaf");
    }
    ASSERT_NE(pool.BuildFile(dep), nullptr);

    FileDescriptorProto user;
    user.set_name("user.proto");
    user.set_package("user");
    user.set_syntax("proto3");
    user.add_dependency("dir/dep.proto");
    {
        auto* m = user.add_message_type();
        m->set_name("Local");
        AddField(m, "n", 1, FieldDescriptorProto::TYPE_INT32);
    }
    {
        // Holder { dep.Outer outer = 1; repeated dep.Leaf leaves = 2;
        //          map<string, dep.Point> by_name = 3; dep.Wrapper wrapped = 4; Local local = 5; }
        auto* m = user.add_message_type();
        m->set_name("Holder");
        AddField(m, "outer", 1, FieldDescriptorProto::TYPE_MESSAGE)->set_type_name(".dep.Outer");
        AddField(m, "leaves", 2, FieldDescriptorProto::TYPE_MESSAGE,
                 FieldDescriptorProto::LABEL_REPEATED)
            ->set_type_name(".dep.Leaf");
        auto* entry = m->add_nested_type();
        entry->set_name("ByNameEntry");
        entry->mutable_options()->set_map_entry(true);
        AddField(entry, "key", 1, FieldDescriptorProto::TYPE_STRING);
        AddField(entry, "value", 2, FieldDescriptorProto::TYPE_MESSAGE)
            ->set_type_name(".dep.Point");
        AddField(m, "by_name", 3, FieldDescriptorProto::TYPE_MESSAGE,
                 FieldDescriptorProto::LABEL_REPEATED)
            ->set_type_name(".user.Holder.ByNameEntry");
        AddField(m, "wrapped", 4, FieldDescriptorProto::TYPE_MESSAGE)
            ->set_type_name(".dep.Wrapper");
        AddField(m, "local", 5, FieldDescriptorProto::TYPE_MESSAGE)->set_type_name(".user.Local");
    }
    const FileDescriptor* file = pool.BuildFile(user);
    ASSERT_NE(file, nullptr);

    const std::string ts = fletcher::ts_backend::TsVisitor(file).GenerateFile();

    // The flatten wrapper is a list of its element (D-BIND-119): it imports Leaf, not Wrapper.
    EXPECT_NE(ts.find("import { Leaf, Outer, Point } from './dir/dep.fletcher.js';\n"
                      "import type { ILeaf, IOuter, IPoint } from './dir/dep.fletcher.js';\n"),
              std::string::npos)
        << ts;
    EXPECT_EQ(ts.find("cross-file types"), std::string::npos) << ts;
    EXPECT_EQ(ts.find("Inner"), std::string::npos) << "a struct's own fields were imported:\n"
                                                   << ts;
    EXPECT_EQ(ts.find("Local }"), std::string::npos) << "a message of this file was imported:\n"
                                                     << ts;
    // The names the imports resolve, as the file uses them.
    EXPECT_NE(ts.find("outer: IOuter;"), std::string::npos) << ts;
    EXPECT_NE(ts.find("fields: Outer.fields"), std::string::npos) << ts;
    EXPECT_NE(ts.find("by_name: Map<string, IPoint>;"), std::string::npos) << ts;
    EXPECT_NE(ts.find("wrapped: ILeaf[];"), std::string::npos) << ts;
}

// A file whose messages name only their own file's messages imports nothing.
TEST(TsVisitor, OneFilesOwnMessagesImportNothing) {
    DescriptorPool pool;
    const FileDescriptor* file = BuildFixture(pool);
    ASSERT_NE(file, nullptr);
    const std::string ts = fletcher::ts_backend::TsVisitor(file).GenerateFile();
    EXPECT_EQ(ts.find(".fletcher.js'"), std::string::npos) << ts;
}
