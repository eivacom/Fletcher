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

// D-BIND-118, D-BIND-122: a message of ANOTHER file is named through ONE namespace import of
// that file, by a path relative to the importing file. Before D-BIND-118 the import was an empty
// placeholder (`import { /* cross-file types */ }`), so the file neither type-checked nor ran;
// D-BIND-118's bare names could clash. Reached here as a struct, a list element, a map value and
// a singular flatten wrapper; a struct's own fields are its file's business, not named.
TEST(TsVisitor, AnotherFilesMessageIsNamedThroughItsNamespace) {
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

    EXPECT_NE(ts.find("import * as dir_dep_fletcher from './dir/dep.fletcher.js';\n"),
              std::string::npos)
        << ts;
    EXPECT_EQ(ts.find("cross-file types"), std::string::npos) << ts;
    EXPECT_EQ(ts.find("Inner"), std::string::npos) << "a struct's own fields were named:\n" << ts;
    // The names, as the file uses them; the flatten wrapper is a list of its element (D-BIND-119).
    EXPECT_NE(ts.find("outer: dir_dep_fletcher.IOuter;"), std::string::npos) << ts;
    EXPECT_NE(ts.find("fields: dir_dep_fletcher.Outer.fields"), std::string::npos) << ts;
    EXPECT_NE(ts.find("leaves: dir_dep_fletcher.ILeaf[];"), std::string::npos) << ts;
    EXPECT_NE(ts.find("by_name: Map<string, dir_dep_fletcher.IPoint>;"), std::string::npos) << ts;
    EXPECT_NE(ts.find("wrapped: dir_dep_fletcher.ILeaf[];"), std::string::npos) << ts;
    EXPECT_NE(ts.find("local: ILocal;"), std::string::npos) << "this file's message qualified:\n"
                                                            << ts;
}

namespace {

// `name` declaring `message <msg> { int32 id = 1; }` in `package`.
FileDescriptorProto OneMessageFile(const char* name, const char* package, const char* msg) {
    FileDescriptorProto f;
    f.set_name(name);
    f.set_package(package);
    f.set_syntax("proto3");
    auto* m = f.add_message_type();
    m->set_name(msg);
    AddField(m, "id", 1, FieldDescriptorProto::TYPE_INT32);
    return f;
}

}  // namespace

// Review B1 (BIND-8e): the module path is relative to the IMPORTING file, which is written at its
// own `.proto` path, so a file below the include root imports a module that exists: the same
// directory as `./`, a parent's or a sibling's through `../`.
TEST(TsVisitor, AnImportIsRelativeToTheImportingFile) {
    DescriptorPool pool;
    ASSERT_NE(pool.BuildFile(OneMessageFile("a/dep.proto", "adep", "Leaf")), nullptr);
    ASSERT_NE(pool.BuildFile(OneMessageFile("y/z/dep.proto", "ydep", "Leaf")), nullptr);
    FileDescriptorProto user;
    user.set_name("a/b/user.proto");
    user.set_package("user");
    user.set_syntax("proto3");
    user.add_dependency("a/dep.proto");
    user.add_dependency("y/z/dep.proto");
    auto* m = user.add_message_type();
    m->set_name("Holder");
    AddField(m, "up", 1, FieldDescriptorProto::TYPE_MESSAGE)->set_type_name(".adep.Leaf");
    AddField(m, "across", 2, FieldDescriptorProto::TYPE_MESSAGE)->set_type_name(".ydep.Leaf");
    const FileDescriptor* file = pool.BuildFile(user);
    ASSERT_NE(file, nullptr);

    const std::string ts = fletcher::ts_backend::TsVisitor(file).GenerateFile();
    EXPECT_NE(ts.find("import * as a_dep_fletcher from '../dep.fletcher.js';"), std::string::npos)
        << ts;
    EXPECT_NE(ts.find("import * as y_z_dep_fletcher from '../../y/z/dep.fletcher.js';"),
              std::string::npos)
        << ts;

    FileDescriptorProto sibling;
    sibling.set_name("a/user2.proto");
    sibling.set_package("user2");
    sibling.set_syntax("proto3");
    sibling.add_dependency("a/dep.proto");
    auto* s = sibling.add_message_type();
    s->set_name("Holder");
    AddField(s, "same", 1, FieldDescriptorProto::TYPE_MESSAGE)->set_type_name(".adep.Leaf");
    const FileDescriptor* same_dir = pool.BuildFile(sibling);
    ASSERT_NE(same_dir, nullptr);
    EXPECT_NE(fletcher::ts_backend::TsVisitor(same_dir).GenerateFile().find(
                  "import * as a_dep_fletcher from './dep.fletcher.js';"),
              std::string::npos);
}

// Review Q2 (D-BIND-122): a name declared in two imported files and locally is three names, one
// per namespace; bare imports would have clashed (TS2300, TS2440).
TEST(TsVisitor, OneNameInThreeFilesDoesNotClash) {
    DescriptorPool pool;
    ASSERT_NE(pool.BuildFile(OneMessageFile("p.proto", "p", "Leaf")), nullptr);
    ASSERT_NE(pool.BuildFile(OneMessageFile("q.proto", "q", "Leaf")), nullptr);
    FileDescriptorProto user = OneMessageFile("user.proto", "user", "Leaf");
    user.add_dependency("p.proto");
    user.add_dependency("q.proto");
    auto* m = user.add_message_type();
    m->set_name("Holder");
    AddField(m, "p", 1, FieldDescriptorProto::TYPE_MESSAGE)->set_type_name(".p.Leaf");
    AddField(m, "q", 2, FieldDescriptorProto::TYPE_MESSAGE)->set_type_name(".q.Leaf");
    AddField(m, "mine", 3, FieldDescriptorProto::TYPE_MESSAGE)->set_type_name(".user.Leaf");
    const FileDescriptor* file = pool.BuildFile(user);
    ASSERT_NE(file, nullptr);

    const std::string ts = fletcher::ts_backend::TsVisitor(file).GenerateFile();
    EXPECT_NE(ts.find("p: p_fletcher.ILeaf;"), std::string::npos) << ts;
    EXPECT_NE(ts.find("q: q_fletcher.ILeaf;"), std::string::npos) << ts;
    EXPECT_NE(ts.find("mine: ILeaf;"), std::string::npos) << ts;
    EXPECT_EQ(ts.find("import { Leaf"), std::string::npos) << "a named import is back:\n" << ts;
}

// A file whose messages name only their own file's messages imports nothing.
TEST(TsVisitor, OneFilesOwnMessagesImportNothing) {
    DescriptorPool pool;
    const FileDescriptor* file = BuildFixture(pool);
    ASSERT_NE(file, nullptr);
    const std::string ts = fletcher::ts_backend::TsVisitor(file).GenerateFile();
    EXPECT_EQ(ts.find(".fletcher.js'"), std::string::npos) << ts;
}

// D-BIND-130: a TypedSchema carries the schema IPC it is given, verbatim, and one it is not given
// carries none (the generator gives every message the `ipc` output covers).
TEST(TsVisitor, ATypedSchemaCarriesItsSchemaIpcWhenGivenOne) {
    DescriptorPool pool;
    const FileDescriptor* file = BuildFixture(pool);
    ASSERT_NE(file, nullptr);
    const Descriptor* leaf = file->FindMessageTypeByName("Leaf");
    ASSERT_NE(leaf, nullptr);

    const std::string ts = fletcher::ts_backend::TsVisitor(file, {{leaf, "QUJD"}}).GenerateFile();
    EXPECT_NE(ts.find("  protoMessage: 'Leaf',\n  schemaIpc: 'QUJD',\n};"), std::string::npos)
        << ts;
    EXPECT_NE(ts.find("  protoMessage: 'Holder',\n};"), std::string::npos) << ts;
    EXPECT_EQ(fletcher::ts_backend::TsVisitor(file).GenerateFile().find("schemaIpc"),
              std::string::npos);
}
