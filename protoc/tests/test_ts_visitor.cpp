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

#include <google/protobuf/compiler/code_generator.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/empty.pb.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <tuple>
#include <utility>

#include "generator.hpp"
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

// ---------------------------------------------------------------------------
// BIND-T-3: the generated pair (D-BIND-181) and the methods that get none (D-BIND-182). These
// run the real generator, so a case can compare TypeScript's output with the C++ header
// generated for the same file.
// ---------------------------------------------------------------------------

namespace {

class MemoryContext : public compiler::GeneratorContext {
   public:
    io::ZeroCopyOutputStream* Open(const std::string& filename) override {
        return new io::StringOutputStream(&files_[filename]);
    }
    const std::map<std::string, std::string>& files() const { return files_; }

   private:
    std::map<std::string, std::string> files_;
};

std::string Generated(const FileDescriptor* file, const std::string& opts,
                      const std::string& name) {
    MemoryContext ctx;
    std::string error;
    EXPECT_TRUE(fletcher::ArrowRowGenerator().Generate(file, opts, &ctx, &error)) << error;
    const auto it = ctx.files().find(name);
    EXPECT_NE(it, ctx.files().end()) << "no " << name << " emitted";
    return it == ctx.files().end() ? std::string() : it->second;
}

// package feed.v1;
// message Reading { int32 id = 1; optional double value = 2; }
// message Batch { option (fletcher.flatten) = true; repeated Reading items = 1; }
// message Node { Node next = 1; }   // recursive
// message Tick {}                   // nothing TypeScript can carry
// service Feed {
//   rpc Send(stream Reading) returns (google.protobuf.Empty);
//   rpc Push(stream Batch) returns (google.protobuf.Empty);
//   rpc Loop(stream Node) returns (google.protobuf.Empty);
//   rpc Beat(stream Tick) returns (google.protobuf.Empty);
//   rpc Ping(Reading) returns (google.protobuf.Empty);  // not pub/sub
// }
const FileDescriptor* BuildFeedFixture(DescriptorPool& pool) {
    FileDescriptorProto empty;
    Empty::descriptor()->file()->CopyTo(&empty);
    if (pool.BuildFile(empty) == nullptr) return nullptr;

    FileDescriptorProto fdp;
    fdp.set_name("feed.proto");
    fdp.set_package("feed.v1");
    fdp.set_syntax("proto3");
    fdp.add_dependency("google/protobuf/empty.proto");
    {
        auto* m = fdp.add_message_type();
        m->set_name("Reading");
        AddField(m, "id", 1, FieldDescriptorProto::TYPE_INT32);
        auto* value = AddField(m, "value", 2, FieldDescriptorProto::TYPE_DOUBLE);
        value->set_proto3_optional(true);
        value->set_oneof_index(0);
        m->add_oneof_decl()->set_name("_value");
    }
    {
        auto* m = fdp.add_message_type();
        m->set_name("Batch");
        SetMessageFlatten(m);
        AddField(m, "items", 1, FieldDescriptorProto::TYPE_MESSAGE,
                 FieldDescriptorProto::LABEL_REPEATED)
            ->set_type_name(".feed.v1.Reading");
    }
    {
        auto* m = fdp.add_message_type();
        m->set_name("Node");
        AddField(m, "next", 1, FieldDescriptorProto::TYPE_MESSAGE)->set_type_name(".feed.v1.Node");
    }
    fdp.add_message_type()->set_name("Tick");

    auto* feed = fdp.add_service();
    feed->set_name("Feed");
    for (const auto& [name, input, streaming] : {std::tuple{"Send", ".feed.v1.Reading", true},
                                                 {"Push", ".feed.v1.Batch", true},
                                                 {"Loop", ".feed.v1.Node", true},
                                                 {"Beat", ".feed.v1.Tick", true},
                                                 {"Ping", ".feed.v1.Reading", false}}) {
        auto* m = feed->add_method();
        m->set_name(name);
        m->set_input_type(input);
        m->set_output_type(".google.protobuf.Empty");
        m->set_client_streaming(streaming);
    }
    return pool.BuildFile(fdp);
}

void ExpectContains(const std::string& text, const std::string& needle) {
    EXPECT_NE(text.find(needle), std::string::npos) << "missing:\n"
                                                    << needle << "\n--- in:\n"
                                                    << text;
}

}  // namespace

TEST(TsPair, APubSubMethodGetsAPairNamedAsCpps) {
    DescriptorPool pool;
    const FileDescriptor* file = BuildFeedFixture(pool);
    ASSERT_NE(file, nullptr);
    const std::string header = Generated(file, "", "feed.fletcher.pb.h");
    const std::string ts = Generated(file, "ts", "feed.fletcher.ts");

    ExpectContains(header, "class Feed_SendPublisher");
    ExpectContains(header, "class Feed_SendSubscriber");
    ExpectContains(ts, "export class Feed_SendPublisher {\n");
    ExpectContains(ts, "export class Feed_SendSubscriber {\n");
}

// The pair is thin: the topic constant and the message's TypedSchema bound in, every call one
// FletcherClient call, the row typed as the message's interface.
TEST(TsPair, ThePairBindsTheTopicAndTheSchema) {
    DescriptorPool pool;
    const FileDescriptor* file = BuildFeedFixture(pool);
    ASSERT_NE(file, nullptr);
    const std::string ts = Generated(file, "ts", "feed.fletcher.ts");

    ExpectContains(ts, "export const Feed_SendTopic = 'feed/v1/Feed/Send';\n");
    // Review T5: aliased, so a message named FletcherClient (or BatchOptions) cannot clash.
    ExpectContains(ts,
                   "import type {\n"
                   "  BatchCallback as __FletcherBatchCallback,\n"
                   "  BatchOptions as __FletcherBatchOptions,\n"
                   "  FletcherClient as __FletcherClient,\n"
                   "} from '@eiva/fletcher-gateway-client';\n");
    // Review M6: annotated, as every other declaration in the file.
    ExpectContains(ts,
                   "  static readonly topic: typeof Feed_SendTopic = Feed_SendTopic;\n"
                   "  /** The schema rows travel under. */\n"
                   "  static readonly schema: TypedSchema<IReading> = Reading;\n");
    ExpectContains(ts, "  private constructor(client: __FletcherClient) {\n");
    ExpectContains(ts,
                   "  static async create(client: __FletcherClient): "
                   "Promise<Feed_SendPublisher> {\n"
                   "    await client.createTopic(Feed_SendTopic, Reading);\n"
                   "    return new Feed_SendPublisher(client);\n"
                   "  }\n");
    ExpectContains(ts,
                   "  publish(row: IReading, attachments?: Map<string, Uint8Array>): "
                   "Promise<void> {\n"
                   "    return this.client.publish(Feed_SendTopic, Reading, row, attachments);\n");
    ExpectContains(ts, "  constructor(client: __FletcherClient) {\n");
    ExpectContains(ts,
                   "    handler: (row: IReading, attachments: Map<string, Uint8Array>) => void,\n"
                   "  ): Promise<bigint> {\n"
                   "    return this.client.subscribe(Feed_SendTopic, Reading, handler);\n");
    ExpectContains(ts,
                   "  subscribeBatched(\n"
                   "    handler: __FletcherBatchCallback<IReading>,\n"
                   "    options?: __FletcherBatchOptions,\n"
                   "  ): Promise<bigint> {\n"
                   "    return this.client.subscribeBatched(Feed_SendTopic, Reading, handler, "
                   "options);\n");
    ExpectContains(ts,
                   "  unsubscribe(subscriptionId: bigint): Promise<void> {\n"
                   "    return this.client.unsubscribe(subscriptionId);\n");
}

// D-BIND-7: the pairs and their import come after everything the file had before them.
TEST(TsPair, ThePairsAreAppendedAfterTheTopicConstants) {
    DescriptorPool pool;
    const FileDescriptor* file = BuildFeedFixture(pool);
    ASSERT_NE(file, nullptr);
    const std::string ts = Generated(file, "ts", "feed.fletcher.ts");

    const size_t pairs = ts.find(
        "// Publisher and subscriber per pub/sub method (D-BIND-181).\n"
        "import type {\n");
    ASSERT_NE(pairs, std::string::npos) << ts;
    EXPECT_GT(pairs, ts.rfind("Topic = '")) << ts;
    EXPECT_GT(pairs, ts.rfind("// Skipped: ")) << ts;
    EXPECT_GT(pairs, ts.rfind("export const ")) << ts;
}

// D-BIND-182: a method whose input C++ generates no class for gets C++'s own line in TypeScript,
// and no topic constant and no pair. A flatten wrapper and a recursive message are the two.
TEST(TsPair, AWrapperOrRecursiveInputGetsCppsLineAndNoTopic) {
    DescriptorPool pool;
    const FileDescriptor* file = BuildFeedFixture(pool);
    ASSERT_NE(file, nullptr);
    const std::string header = Generated(file, "", "feed.fletcher.pb.h");
    const std::string ts = Generated(file, "ts", "feed.fletcher.ts");

    for (const auto& [method, input] : {std::pair{"Push", "Batch"}, {"Loop", "Node"}}) {
        const std::string skipped = std::string("// Skipped: Feed.") + method +
                                    " \xE2\x80\x94 input message '" + input +
                                    "' has no generated Arrow mapping in this file\n";
        ExpectContains(header, skipped);
        ExpectContains(ts, skipped);
        EXPECT_EQ(ts.find(std::string("Feed_") + method + "Topic"), std::string::npos) << ts;
        EXPECT_EQ(ts.find(std::string("Feed_") + method + "Publisher"), std::string::npos) << ts;
    }
    ExpectContains(ts, "// Skipped: Feed.Ping \xE2\x80\x94 request is not streaming");
    // Review T11: the wrapper keeps its own interface and TypedSchema (D-BIND-182).
    ExpectContains(ts, "export interface IBatch {\n");
    ExpectContains(ts, "export const Batch: TypedSchema<IBatch> = {\n");
}

// A message with no field TypeScript can carry has no TypedSchema to bind, so its method keeps
// its topic constant and gets a line instead of a pair.
TEST(TsPair, AnInputWithoutFieldsGetsATopicAndNoPair) {
    DescriptorPool pool;
    const FileDescriptor* file = BuildFeedFixture(pool);
    ASSERT_NE(file, nullptr);
    const std::string ts = Generated(file, "ts", "feed.fletcher.ts");

    ExpectContains(ts, "export const Feed_BeatTopic = 'feed/v1/Feed/Beat';\n");
    ExpectContains(ts,
                   "// No pair: Feed.Beat \xE2\x80\x94 input message 'Tick' has no field "
                   "TypeScript can carry\n");
    EXPECT_EQ(ts.find("Feed_BeatPublisher"), std::string::npos) << ts;
}

// A file with no pub/sub method imports nothing from the client beyond what it did before.
TEST(TsPair, AFileWithoutAPubSubMethodHasNoPairImport) {
    DescriptorPool pool;
    const FileDescriptor* file = BuildFixture(pool);
    ASSERT_NE(file, nullptr);
    const std::string ts = fletcher::ts_backend::TsVisitor(file).GenerateFile();
    EXPECT_EQ(ts.find("FletcherClient"), std::string::npos) << ts;
}

// Review M7: a file whose only pub/sub method gets `// No pair:` imports nothing for a pair.
TEST(TsPair, NoPairMeansNoPairImport) {
    DescriptorPool pool;
    FileDescriptorProto empty;
    Empty::descriptor()->file()->CopyTo(&empty);
    ASSERT_NE(pool.BuildFile(empty), nullptr);
    FileDescriptorProto fdp;
    fdp.set_name("beat.proto");
    fdp.set_package("beat");
    fdp.set_syntax("proto3");
    fdp.add_dependency("google/protobuf/empty.proto");
    fdp.add_message_type()->set_name("Tick");
    auto* m = fdp.add_service();
    m->set_name("Clock");
    auto* method = m->add_method();
    method->set_name("Beat");
    method->set_input_type(".beat.Tick");
    method->set_output_type(".google.protobuf.Empty");
    method->set_client_streaming(true);
    const FileDescriptor* file = pool.BuildFile(fdp);
    ASSERT_NE(file, nullptr);
    const std::string ts = Generated(file, "ts", "beat.fletcher.ts");

    ExpectContains(ts, "// No pair: Clock.Beat");
    EXPECT_EQ(ts.find("__FletcherClient"), std::string::npos) << ts;
}

// Review T3: the whole file for the Feed fixture, byte for byte, so a change to any byte before
// the pairs (D-BIND-7) fails here, as the protoc-coverage golden holds a file without pub/sub
// methods. FLETCHER_CAPTURE_GOLDENS=1 rewrites it for review as source.
TEST(TsPair, TheFeedFileIsItsGolden) {
    DescriptorPool pool;
    const FileDescriptor* file = BuildFeedFixture(pool);
    ASSERT_NE(file, nullptr);
    const std::string ts = Generated(file, "ts", "feed.fletcher.ts");
    const auto path = std::filesystem::path(SCHEMA_GOLDEN_DIR) / "ts_pair" / "feed.fletcher.ts";
    if (const char* capture = std::getenv("FLETCHER_CAPTURE_GOLDENS");
        capture != nullptr && std::string(capture) == "1") {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << ts;
        return;
    }
    std::ifstream in(path, std::ios::binary);
    const std::string golden((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
    EXPECT_EQ(golden, ts) << path << " differs from the plugin's output";
}
