// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// BIND-6a: the C# backend visitor, driven through the real ArrowRowGenerator with
// `--fletcher_opt=csharp` over an in-memory descriptor pool.
//
// Slice 6a emits the model's shape: the file header, the D-BIND-69 namespace, one
// `public enum` per proto enum (D-BIND-70/73) and one `public sealed class` per
// message with a property per SCALAR field. Composite and temporal fields belong to
// 6c and are marked in the output rather than dropped silently, so the slice
// boundary is visible in what a consumer would read. `Schema`, `ToArrow` and
// `FromArrow` are 6b's.
//
// The no-drift case is the additive guarantee every opt token has had since RBA-1:
// adding `csharp` changes no existing output by a byte and adds exactly one file.

#include <google/protobuf/compiler/code_generator.h>
#include <google/protobuf/compiler/parser.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/duration.pb.h>
#include <google/protobuf/empty.pb.h>
#include <google/protobuf/io/tokenizer.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include <google/protobuf/timestamp.pb.h>
#include <google/protobuf/wrappers.pb.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "generator.hpp"
#include "generator_internal.hpp"

using namespace google::protobuf;

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

// package integration; option csharp_namespace = "Eiva.Integration";
// enum Color { COLOR_UNSPECIFIED = 0; COLOR_RED = 1; }
// message Player {
//   enum Mode { MODE_UNSPECIFIED = 0; MODE_2D = 1; }
//   message Stats { int32 goals = 1; }
//   int32 id = 1;  string name = 2;  optional string label = 3;  bytes data = 4;
//   Color color = 5;  string player = 6;  int32 class = 7;  repeated int32 scores = 8;
//   optional double speed = 9;  Mode mode = 10;  string schema = 11;
//   Stats stats = 12;  map<string, int32> tags = 13;
// }
// message Sample {   // every scalar kind, so it gets ToArrow / FromArrow (BIND-6b)
//   bool flag = 1;  int64 big = 2;  uint32 small = 3;  uint64 huge = 4;  float ratio = 5;
//   double value = 6;  string name = 7;  optional string note = 8;  bytes blob = 9;
//   Color color = 10;  optional Color tint = 11;  optional int32 maybe = 12;
// }
// message Timed {   // the well-known types (BIND-6c)
//   google.protobuf.Timestamp at = 1;  google.protobuf.Duration took = 2;
//   google.protobuf.Int32Value boxed = 3;  repeated google.protobuf.Timestamp marks = 4;
//   optional google.protobuf.Timestamp maybe_at = 5;
// }
// message Holder {   // message fields of generated classes: plain, optional, repeated, map value
//   Player.Stats best = 1;  repeated Player.Stats history = 2;
//   map<string, Player.Stats> by_name = 3;  optional Player.Stats maybe_best = 4;
// }
// message Clock {   // the temporal types where every field converts (BIND-6c-2)
//   google.protobuf.Timestamp at = 1;  google.protobuf.Duration took = 2;
//   optional google.protobuf.Timestamp maybe_at = 3;  google.protobuf.Int32Value boxed = 4;
// }
// message Wraps { Timed timed = 1; }   // embeds a message whose conversion is blocked
// message StatsRow { option (fletcher.flatten) = true; repeated Player.Stats values = 1; }
// message StatsGrid { option (fletcher.flatten) = true; repeated StatsRow rows = 1; }
// message Nested {   // lists of lists, as flatten wrappers nest them (BIND-6c-3)
//   repeated StatsRow table = 1;  repeated StatsGrid cube = 2;
// }
const FileDescriptor* BuildFixture(DescriptorPool& pool) {
    // The well-known files the fixture imports, copied into the local pool. Naming a
    // linked-in C++ WKT type forces its descriptor to register.
    const FileDescriptor* wkt_files[] = {
        google::protobuf::Timestamp::GetDescriptor()->file(),
        google::protobuf::Duration::GetDescriptor()->file(),
        google::protobuf::Int32Value::GetDescriptor()->file(),  // wrappers.proto
    };
    for (const FileDescriptor* wkt : wkt_files) {
        if (pool.FindFileByName(wkt->name()) != nullptr) continue;
        FileDescriptorProto copy;
        wkt->CopyTo(&copy);
        if (pool.BuildFile(copy) == nullptr) return nullptr;
    }

    FileDescriptorProto fdp;
    fdp.set_name("player.proto");
    fdp.set_package("integration");
    fdp.set_syntax("proto3");
    fdp.add_dependency("google/protobuf/timestamp.proto");
    fdp.add_dependency("google/protobuf/duration.proto");
    fdp.add_dependency("google/protobuf/wrappers.proto");
    fdp.mutable_options()->set_csharp_namespace("Eiva.Integration");

    auto* color = fdp.add_enum_type();
    color->set_name("Color");
    auto* v0 = color->add_value();
    v0->set_name("COLOR_UNSPECIFIED");
    v0->set_number(0);
    auto* v1 = color->add_value();
    v1->set_name("COLOR_RED");
    v1->set_number(1);

    auto* m = fdp.add_message_type();
    m->set_name("Player");
    auto* mode = m->add_enum_type();
    mode->set_name("Mode");
    auto* m0 = mode->add_value();
    m0->set_name("MODE_UNSPECIFIED");
    m0->set_number(0);
    auto* m1 = mode->add_value();
    m1->set_name("MODE_2D");
    m1->set_number(1);
    auto* stats = m->add_nested_type();
    stats->set_name("Stats");
    AddField(stats, "goals", 1, FieldDescriptorProto::TYPE_INT32);

    AddField(m, "id", 1, FieldDescriptorProto::TYPE_INT32);
    AddField(m, "name", 2, FieldDescriptorProto::TYPE_STRING);
    auto* label = AddField(m, "label", 3, FieldDescriptorProto::TYPE_STRING);
    label->set_proto3_optional(true);
    label->set_oneof_index(0);
    AddField(m, "data", 4, FieldDescriptorProto::TYPE_BYTES);
    AddField(m, "color", 5, FieldDescriptorProto::TYPE_ENUM)->set_type_name(".integration.Color");
    AddField(m, "player", 6, FieldDescriptorProto::TYPE_STRING);
    AddField(m, "class", 7, FieldDescriptorProto::TYPE_INT32);
    AddField(m, "scores", 8, FieldDescriptorProto::TYPE_INT32,
             FieldDescriptorProto::LABEL_REPEATED);
    auto* speed = AddField(m, "speed", 9, FieldDescriptorProto::TYPE_DOUBLE);
    speed->set_proto3_optional(true);
    speed->set_oneof_index(1);
    AddField(m, "mode", 10, FieldDescriptorProto::TYPE_ENUM)
        ->set_type_name(".integration.Player.Mode");
    AddField(m, "schema", 11, FieldDescriptorProto::TYPE_STRING);
    AddField(m, "stats", 12, FieldDescriptorProto::TYPE_MESSAGE)
        ->set_type_name(".integration.Player.Stats");
    auto* entry = m->add_nested_type();
    entry->set_name("TagsEntry");
    entry->mutable_options()->set_map_entry(true);
    AddField(entry, "key", 1, FieldDescriptorProto::TYPE_STRING);
    AddField(entry, "value", 2, FieldDescriptorProto::TYPE_INT32);
    AddField(m, "tags", 13, FieldDescriptorProto::TYPE_MESSAGE,
             FieldDescriptorProto::LABEL_REPEATED)
        ->set_type_name(".integration.Player.TagsEntry");
    m->add_oneof_decl()->set_name("_label");
    m->add_oneof_decl()->set_name("_speed");

    auto* s = fdp.add_message_type();
    s->set_name("Sample");
    AddField(s, "flag", 1, FieldDescriptorProto::TYPE_BOOL);
    AddField(s, "big", 2, FieldDescriptorProto::TYPE_INT64);
    AddField(s, "small", 3, FieldDescriptorProto::TYPE_UINT32);
    AddField(s, "huge", 4, FieldDescriptorProto::TYPE_UINT64);
    AddField(s, "ratio", 5, FieldDescriptorProto::TYPE_FLOAT);
    AddField(s, "value", 6, FieldDescriptorProto::TYPE_DOUBLE);
    AddField(s, "name", 7, FieldDescriptorProto::TYPE_STRING);
    auto* note = AddField(s, "note", 8, FieldDescriptorProto::TYPE_STRING);
    note->set_proto3_optional(true);
    note->set_oneof_index(0);
    AddField(s, "blob", 9, FieldDescriptorProto::TYPE_BYTES);
    AddField(s, "color", 10, FieldDescriptorProto::TYPE_ENUM)->set_type_name(".integration.Color");
    auto* tint = AddField(s, "tint", 11, FieldDescriptorProto::TYPE_ENUM);
    tint->set_type_name(".integration.Color");
    tint->set_proto3_optional(true);
    tint->set_oneof_index(1);
    auto* maybe = AddField(s, "maybe", 12, FieldDescriptorProto::TYPE_INT32);
    maybe->set_proto3_optional(true);
    maybe->set_oneof_index(2);
    s->add_oneof_decl()->set_name("_note");
    s->add_oneof_decl()->set_name("_tint");
    s->add_oneof_decl()->set_name("_maybe");

    auto* timed = fdp.add_message_type();
    timed->set_name("Timed");
    AddField(timed, "at", 1, FieldDescriptorProto::TYPE_MESSAGE)
        ->set_type_name(".google.protobuf.Timestamp");
    AddField(timed, "took", 2, FieldDescriptorProto::TYPE_MESSAGE)
        ->set_type_name(".google.protobuf.Duration");
    AddField(timed, "boxed", 3, FieldDescriptorProto::TYPE_MESSAGE)
        ->set_type_name(".google.protobuf.Int32Value");
    AddField(timed, "marks", 4, FieldDescriptorProto::TYPE_MESSAGE,
             FieldDescriptorProto::LABEL_REPEATED)
        ->set_type_name(".google.protobuf.Timestamp");
    auto* maybe_at = AddField(timed, "maybe_at", 5, FieldDescriptorProto::TYPE_MESSAGE);
    maybe_at->set_type_name(".google.protobuf.Timestamp");
    maybe_at->set_proto3_optional(true);
    maybe_at->set_oneof_index(0);
    timed->add_oneof_decl()->set_name("_maybe_at");

    auto* holder = fdp.add_message_type();
    holder->set_name("Holder");
    AddField(holder, "best", 1, FieldDescriptorProto::TYPE_MESSAGE)
        ->set_type_name(".integration.Player.Stats");
    AddField(holder, "history", 2, FieldDescriptorProto::TYPE_MESSAGE,
             FieldDescriptorProto::LABEL_REPEATED)
        ->set_type_name(".integration.Player.Stats");
    auto* by_name = holder->add_nested_type();
    by_name->set_name("ByNameEntry");
    by_name->mutable_options()->set_map_entry(true);
    AddField(by_name, "key", 1, FieldDescriptorProto::TYPE_STRING);
    AddField(by_name, "value", 2, FieldDescriptorProto::TYPE_MESSAGE)
        ->set_type_name(".integration.Player.Stats");
    AddField(holder, "by_name", 3, FieldDescriptorProto::TYPE_MESSAGE,
             FieldDescriptorProto::LABEL_REPEATED)
        ->set_type_name(".integration.Holder.ByNameEntry");
    auto* maybe_best = AddField(holder, "maybe_best", 4, FieldDescriptorProto::TYPE_MESSAGE);
    maybe_best->set_type_name(".integration.Player.Stats");
    maybe_best->set_proto3_optional(true);
    maybe_best->set_oneof_index(0);
    holder->add_oneof_decl()->set_name("_maybe_best");

    auto* clock = fdp.add_message_type();
    clock->set_name("Clock");
    AddField(clock, "at", 1, FieldDescriptorProto::TYPE_MESSAGE)
        ->set_type_name(".google.protobuf.Timestamp");
    AddField(clock, "took", 2, FieldDescriptorProto::TYPE_MESSAGE)
        ->set_type_name(".google.protobuf.Duration");
    auto* clock_maybe = AddField(clock, "maybe_at", 3, FieldDescriptorProto::TYPE_MESSAGE);
    clock_maybe->set_type_name(".google.protobuf.Timestamp");
    clock_maybe->set_proto3_optional(true);
    clock_maybe->set_oneof_index(0);
    clock->add_oneof_decl()->set_name("_maybe_at");
    AddField(clock, "boxed", 4, FieldDescriptorProto::TYPE_MESSAGE)
        ->set_type_name(".google.protobuf.Int32Value");

    auto* wraps = fdp.add_message_type();
    wraps->set_name("Wraps");
    AddField(wraps, "timed", 1, FieldDescriptorProto::TYPE_MESSAGE)
        ->set_type_name(".integration.Timed");

    // 6c-3: flatten wrappers around a repeated message, which nest into lists of lists.
    // (fletcher.flatten) is option 50000 on the message, set here as an unknown varint,
    // which is how the type mapper reads it.
    const auto set_flatten = [](DescriptorProto* m) {
        auto* opts = m->mutable_options();
        opts->GetReflection()->MutableUnknownFields(opts)->AddVarint(50000, 1);
    };
    auto* stats_row = fdp.add_message_type();
    stats_row->set_name("StatsRow");
    set_flatten(stats_row);
    AddField(stats_row, "values", 1, FieldDescriptorProto::TYPE_MESSAGE,
             FieldDescriptorProto::LABEL_REPEATED)
        ->set_type_name(".integration.Player.Stats");
    auto* stats_grid = fdp.add_message_type();
    stats_grid->set_name("StatsGrid");
    set_flatten(stats_grid);
    AddField(stats_grid, "rows", 1, FieldDescriptorProto::TYPE_MESSAGE,
             FieldDescriptorProto::LABEL_REPEATED)
        ->set_type_name(".integration.StatsRow");
    auto* nested = fdp.add_message_type();
    nested->set_name("Nested");
    AddField(nested, "table", 1, FieldDescriptorProto::TYPE_MESSAGE,
             FieldDescriptorProto::LABEL_REPEATED)
        ->set_type_name(".integration.StatsRow");
    AddField(nested, "cube", 2, FieldDescriptorProto::TYPE_MESSAGE,
             FieldDescriptorProto::LABEL_REPEATED)
        ->set_type_name(".integration.StatsGrid");

    return pool.BuildFile(fdp);
}

std::map<std::string, std::string> GenerateWith(const FileDescriptor* file,
                                                const std::string& opts) {
    MemoryContext ctx;
    std::string error;
    EXPECT_TRUE(fletcher::ArrowRowGenerator().Generate(file, opts, &ctx, &error))
        << "opts '" << opts << "': " << error;
    return ctx.files();
}

std::string CSharp(const FileDescriptor* file) {
    const auto files = GenerateWith(file, "csharp");
    const auto it = files.find("player.fletcher.cs");
    EXPECT_NE(it, files.end()) << "no player.fletcher.cs emitted";
    return it == files.end() ? std::string() : it->second;
}

void ExpectContains(const std::string& cs, const std::string& needle) {
    EXPECT_NE(cs.find(needle), std::string::npos) << "missing:\n" << needle << "\n--- in:\n" << cs;
}

// The text of one generated class, from `public sealed class <name>` to its
// closing brace at column 0, so a case can say what a class does NOT contain.
std::string ClassBody(const std::string& cs, const std::string& name) {
    const std::string head = "public sealed class " + name + "\n{\n";
    const size_t start = cs.find(head);
    if (start == std::string::npos) return {};
    const size_t end = cs.find("\n}\n", start);
    return cs.substr(start, end == std::string::npos ? std::string::npos : end + 3 - start);
}

constexpr const char* kField = "new global::Apache.Arrow.Field(";
constexpr const char* kMeta =
    "new global::System.Collections.Generic.KeyValuePair<string, string>[] { ";

}  // namespace

TEST(CsVisitor, FileHeaderAndNamespaceIgnoringCsharpNamespace) {
    DescriptorPool pool;
    const FileDescriptor* file = BuildFixture(pool);
    ASSERT_NE(file, nullptr);
    const std::string cs = CSharp(file);
    // The file must OPEN with the <auto-generated> marker: Roslyn's analysers skip
    // only files that do, and a consumer building with analysers as errors would
    // otherwise fail on names our rules produce by design (CA1707, CA1819, CA1069).
    EXPECT_EQ(cs.rfind("// <auto-generated>\n", 0), 0u) << cs;
    ExpectContains(cs,
                   "//     Generated by fletcher-protoc. DO NOT EDIT.\n"
                   "//     Source: player.proto\n"
                   "// </auto-generated>\n#nullable enable\n");
    ExpectContains(cs, "namespace Fletcher.Gen.Integration;\n");
    EXPECT_EQ(cs.find("Eiva.Integration"), std::string::npos) << cs;
}

TEST(CsVisitor, EnumsAreDeclaredWithProtocMemberNamesAndFlatNestedNames) {
    DescriptorPool pool;
    const FileDescriptor* file = BuildFixture(pool);
    ASSERT_NE(file, nullptr);
    const std::string cs = CSharp(file);
    ExpectContains(cs, "public enum Color : int\n{\n    Unspecified = 0,\n    Red = 1,\n}\n");
    ExpectContains(cs, "public enum Player_Mode : int\n{\n    Unspecified = 0,\n    _2D = 1,\n}\n");
}

TEST(CsVisitor, MessagesAreSealedClassesAndNestedOnesAreFlat) {
    DescriptorPool pool;
    const FileDescriptor* file = BuildFixture(pool);
    ASSERT_NE(file, nullptr);
    const std::string cs = CSharp(file);
    ExpectContains(cs, "public sealed class Player\n{\n");
    // Only how the class opens: since BIND-6b its Schema and conversion follow.
    ExpectContains(cs, "public sealed class Player_Stats\n{\n    public int Goals { get; set; }\n");
}

TEST(CsVisitor, ScalarPropertiesCarryTypesAndNullability) {
    DescriptorPool pool;
    const FileDescriptor* file = BuildFixture(pool);
    ASSERT_NE(file, nullptr);
    const std::string cs = CSharp(file);
    ExpectContains(cs, "    public int Id { get; set; }\n");
    ExpectContains(cs, "    public string Name { get; set; } = \"\";\n");
    ExpectContains(cs, "    public string? Label { get; set; }\n");
    ExpectContains(cs,
                   "    public byte[] Data { get; set; } = global::System.Array.Empty<byte>();\n");
    ExpectContains(cs, "    public Color Color { get; set; }\n");
    ExpectContains(cs, "    public double? Speed { get; set; }\n");
    ExpectContains(cs, "    public Player_Mode Mode { get; set; }\n");
}

TEST(CsVisitor, PropertyNamesFollowDBind73) {
    DescriptorPool pool;
    const FileDescriptor* file = BuildFixture(pool);
    ASSERT_NE(file, nullptr);
    const std::string cs = CSharp(file);
    ExpectContains(cs, "    public string Player_ { get; set; } = \"\";\n");
    ExpectContains(cs, "    public int Class { get; set; }\n");
    ExpectContains(cs, "    public string Schema_ { get; set; } = \"\";\n");
}

TEST(CsVisitor, ListsAreNonNullListsThatStartEmpty) {
    DescriptorPool pool;
    const FileDescriptor* file = BuildFixture(pool);
    ASSERT_NE(file, nullptr);
    const std::string cs = CSharp(file);
    ExpectContains(cs,
                   "    public global::System.Collections.Generic.List<int> Scores { get; set; }"
                   " = new();\n");
    ExpectContains(cs,
                   "    public global::System.Collections.Generic.List<Player_Stats> History"
                   " { get; set; } = new();\n");
}

TEST(CsVisitor, AMessageFieldIsNullableOnlyWhereItsColumnIs) {
    // D-BIND-83: as C++'s row class does, a plain message field is always present (its
    // column is non-nullable), so the property is never null and starts as a default
    // instance; only an `optional` one, whose column is nullable, can be absent.
    DescriptorPool pool;
    const FileDescriptor* file = BuildFixture(pool);
    ASSERT_NE(file, nullptr);
    const std::string cs = CSharp(file);
    ExpectContains(cs, "    public Player_Stats Stats { get; set; } = new();\n");
    ExpectContains(cs, "    public Player_Stats Best { get; set; } = new();\n");
    ExpectContains(cs, "    public Player_Stats? MaybeBest { get; set; }\n");
    EXPECT_EQ(cs.find("Player_Stats? Stats"), std::string::npos) << cs;
}

TEST(CsVisitor, MapsAreOrderedListsOfPairsNotDictionaries) {
    // D-BIND-75: entry order is on the wire and C++'s row class holds ordered pairs,
    // so a Dictionary, whose enumeration order .NET leaves unspecified, is refused.
    DescriptorPool pool;
    const FileDescriptor* file = BuildFixture(pool);
    ASSERT_NE(file, nullptr);
    const std::string cs = CSharp(file);
    ExpectContains(cs,
                   "    public global::System.Collections.Generic.List<"
                   "global::System.Collections.Generic.KeyValuePair<string, int>> Tags"
                   " { get; set; } = new();\n");
    ExpectContains(cs,
                   "    public global::System.Collections.Generic.List<"
                   "global::System.Collections.Generic.KeyValuePair<string, Player_Stats>> ByName"
                   " { get; set; } = new();\n");
    EXPECT_EQ(cs.find("Dictionary<"), std::string::npos) << cs;
}

TEST(CsVisitor, TimestampAndDurationUseTheModelPackageNotDateTime) {
    // D-BIND-26 and D-BIND-74: lossless, and named in full so a message called
    // Timestamp in the same file cannot shadow them. They are nullable exactly where
    // the schema says so: a plain Timestamp field is always present (its Arrow column is
    // non-nullable), an `optional` one is not.
    DescriptorPool pool;
    const FileDescriptor* file = BuildFixture(pool);
    ASSERT_NE(file, nullptr);
    const std::string cs = CSharp(file);
    // D-BIND-93: a plain one starts at zero in its column's unit, so it reads back equal.
    ExpectContains(cs,
                   "    public global::Eiva.Fletcher.Model.Timestamp At { get; set; } = new "
                   "global::Eiva.Fletcher.Model.Timestamp(0, global::Apache.Arrow.Types.TimeUnit."
                   "Nanosecond);\n");
    ExpectContains(cs,
                   "    public global::Eiva.Fletcher.Model.Duration Took { get; set; } = new "
                   "global::Eiva.Fletcher.Model.Duration(0, global::Apache.Arrow.Types.TimeUnit."
                   "Nanosecond);\n");
    ExpectContains(cs, "    public global::Eiva.Fletcher.Model.Timestamp? MaybeAt { get; set; }\n");
    EXPECT_EQ(cs.find("DateTime"), std::string::npos) << cs;
    EXPECT_EQ(cs.find("TimeSpan"), std::string::npos) << cs;
}

TEST(CsVisitor, AWrapperIsANullableScalar) {
    DescriptorPool pool;
    const FileDescriptor* file = BuildFixture(pool);
    ASSERT_NE(file, nullptr);
    ExpectContains(CSharp(file), "    public int? Boxed { get; set; }\n");
}

TEST(CsVisitor, ARepeatedTimestampIsAListOfTheClassTimestampProtoGenerates) {
    // BIND-6e: `repeated google.protobuf.Timestamp` is mapped by the IR as a list of the
    // struct seconds/nanos declared in timestamp.proto, not as a list of timestamps, so the
    // element is that file's class, as C++ names ::fletcher_gen::google::protobuf::Timestamp
    // and includes its header. The consumer generates timestamp.proto too.
    DescriptorPool pool;
    const FileDescriptor* file = BuildFixture(pool);
    ASSERT_NE(file, nullptr);
    const std::string timed = ClassBody(CSharp(file), "Timed");
    ExpectContains(
        timed,
        "    public global::System.Collections.Generic.List<"
        "global::Fletcher.Gen.Google.Protobuf.Timestamp> Marks { get; set; } = new();\n");
}

TEST(CsVisitor, EveryFieldTheMappingProducesGetsAProperty) {
    // Since BIND-6e no field is a comment: a type from another file is named in full.
    DescriptorPool pool;
    const FileDescriptor* file = BuildFixture(pool);
    ASSERT_NE(file, nullptr);
    const std::string cs = CSharp(file);
    EXPECT_EQ(cs.find("// Not generated yet"), std::string::npos) << cs;
    EXPECT_EQ(cs.find("are not generated"), std::string::npos) << cs;
}

TEST(CsVisitor, CsharpTokenChangesNoExistingOutputAndAddsOnlyItsOwnFiles) {
    // `csharp` adds the model and the native pair's file (D-BIND-76), and
    // `csharp,csharp_model_only` the model alone; neither changes another output. Review T-D3:
    // over every other output the plugin has, the RBA accessor and Rust ones included.
    DescriptorPool pool;
    const FileDescriptor* file = BuildFixture(pool);
    ASSERT_NE(file, nullptr);
    for (const std::string base :
         {"", "ts", "ts,ipc", "schema_only", "accessor", "rust", "ts,ipc,accessor,rust"}) {
        const auto without = GenerateWith(file, base);
        // Not vacuous: a base that names a backend really writes its files.
        const auto writes = [&](const std::string& suffix) {
            for (const auto& entry : without)
                if (entry.first.size() >= suffix.size() &&
                    entry.first.compare(entry.first.size() - suffix.size(), suffix.size(),
                                        suffix) == 0)
                    return true;
            return false;
        };
        if (base.find("accessor") != std::string::npos)
            EXPECT_TRUE(writes(".accessor.pb.h")) << "'" << base << "' wrote no accessor";
        if (base.find("rust") != std::string::npos)
            EXPECT_TRUE(writes(".rs")) << "'" << base << "' wrote no Rust";
        const std::string prefix = base.empty() ? "" : base + ",";
        for (const std::string token : {"csharp", "csharp,csharp_model_only"}) {
            const auto with = GenerateWith(file, prefix + token);
            const size_t added = token == "csharp" ? 2 : 1;
            ASSERT_EQ(with.size(), without.size() + added) << "'" << prefix + token << "'";
            for (const auto& [name, content] : without) {
                const auto it = with.find(name);
                ASSERT_NE(it, with.end()) << name << " vanished with '" << prefix + token << "'";
                EXPECT_EQ(it->second, content)
                    << name << " changed with '" << prefix + token << "'";
            }
            EXPECT_EQ(with.count("player.fletcher.cs"), 1u) << "'" << prefix + token << "'";
            EXPECT_EQ(with.count("player.fletcher.native.cs"), added - 1)
                << "'" << prefix + token << "'";
            // BIND-7: `csharp_accessor` adds the accessor and the view, and changes nothing else,
            // the model included.
            const auto with_accessor = GenerateWith(file, prefix + token + ",csharp_accessor");
            ASSERT_EQ(with_accessor.size(), with.size() + 2) << "'" << prefix + token << "'";
            for (const auto& [name, content] : with) {
                const auto it = with_accessor.find(name);
                ASSERT_NE(it, with_accessor.end()) << name << " vanished with csharp_accessor";
                EXPECT_EQ(it->second, content) << name << " changed with csharp_accessor";
            }
            EXPECT_EQ(with_accessor.count("player.fletcher.accessor.cs"), 1u);
            EXPECT_EQ(with_accessor.count("player.fletcher.arrow.cs"), 1u);
        }
    }
}

// ---------------------------------------------------------------------------
// BIND-6b: Schema, ToArrow, FromArrow
// ---------------------------------------------------------------------------

TEST(CsVisitor, SchemaCarriesTheRootAndFieldMetadataCppWrites) {
    DescriptorPool pool;
    const FileDescriptor* file = BuildFixture(pool);
    ASSERT_NE(file, nullptr);
    const std::string player = ClassBody(CSharp(file), "Player");
    ExpectContains(player,
                   "    public static global::Apache.Arrow.Schema Schema { get; } = "
                   "new global::Apache.Arrow.Schema(\n"
                   "        new global::Apache.Arrow.Field[]\n        {\n");
    // proto_package / proto_message on the schema; field_number / field_id on
    // every field: the pairs SchemaVisitor gives every sink, in its order.
    ExpectContains(player, std::string("        },\n        ") + kMeta +
                               "new(\"proto_package\", \"integration\"), "
                               "new(\"proto_message\", \"Player\") });\n");
    ExpectContains(player, std::string("            ") + kField +
                               "\"id\", global::Apache.Arrow.Types.Int32Type.Default, false, " +
                               kMeta +
                               "new(\"field_number\", \"1\"), new(\"field_id\", \"1\") }),\n");
    ExpectContains(player, std::string(kField) +
                               "\"label\", global::Apache.Arrow.Types.StringType.Default, true, ");
}

TEST(CsVisitor, SchemaRendersListsStructsAndMapsAsNanoarrowLaysThemOut) {
    DescriptorPool pool;
    const FileDescriptor* file = BuildFixture(pool);
    ASSERT_NE(file, nullptr);
    const std::string player = ClassBody(CSharp(file), "Player");
    // A list's child is "item", nullable: nanoarrow's default, which the
    // visitor never overrides.
    ExpectContains(player, std::string(kField) +
                               "\"scores\", new global::Apache.Arrow.Types.ListType(" + kField +
                               "\"item\", global::Apache.Arrow.Types.Int32Type.Default, true)), ");
    // A nested message is copied inline, as the in-process sink deep-copies it.
    ExpectContains(player, std::string(kField) +
                               "\"stats\", new global::Apache.Arrow.Types.StructType("
                               "new global::Apache.Arrow.Field[] { " +
                               kField +
                               "\"goals\", global::Apache.Arrow.Types.Int32Type.Default, false, ");
    // A map's key is non-nullable and its value nullable, as nanoarrow sets them.
    ExpectContains(
        player, std::string(kField) + "\"tags\", new global::Apache.Arrow.Types.MapType(" + kField +
                    "\"key\", global::Apache.Arrow.Types.StringType.Default, false), " + kField +
                    "\"value\", global::Apache.Arrow.Types.Int32Type.Default, true)), ");
}

TEST(CsVisitor, ScalarOnlyMessageGetsToArrow) {
    // ToArrow materialises the rows once and builds the batch from ToArrowColumns, which
    // a message embedding this one as a struct reuses (BIND-6c-2). One builder per column.
    DescriptorPool pool;
    const FileDescriptor* file = BuildFixture(pool);
    ASSERT_NE(file, nullptr);
    const std::string sample = ClassBody(CSharp(file), "Sample");
    ExpectContains(
        sample,
        "    public static global::Apache.Arrow.RecordBatch ToArrow("
        "global::System.Collections.Generic.IEnumerable<Sample> rows)\n    {\n"
        "        global::System.ArgumentNullException.ThrowIfNull(rows);\n"
        "        var all = rows as global::System.Collections.Generic.IReadOnlyList<Sample>"
        " ?? new global::System.Collections.Generic.List<Sample>(rows);\n"
        "        for (var i = 0; i < all.Count; i++)\n"
        "            if (all[i] is null) throw new global::System.ArgumentException($\"Sample row "
        "{i} "
        "is null.\", nameof(rows));\n"
        "        return new global::Apache.Arrow.RecordBatch(Schema, ToArrowColumns(all), "
        "all.Count);\n    }\n");
    ExpectContains(sample,
                   "    internal static global::Apache.Arrow.IArrowArray[] ToArrowColumns("
                   "global::System.Collections.Generic.IReadOnlyList<Sample> rows)\n");
    ExpectContains(sample, "        var b0 = new global::Apache.Arrow.BooleanArray.Builder();\n");
    ExpectContains(sample, "            b0.Append(row.Flag);\n");
    ExpectContains(sample,
                   "            if (row.Note is { } x7) b7.Append(x7); else b7.AppendNull();\n");
    ExpectContains(sample, "            b8.Append((global::System.ReadOnlySpan<byte>)row.Blob);\n");
    // D-BIND-92: a null forced into the non-optional bytes field is refused, never written empty.
    ExpectContains(
        sample,
        "            if (row.Blob is null) throw new global::System.ArgumentException($\"Sample."
        "Blob is null in row {i}, but the field is not optional.\", nameof(rows));\n");
    ExpectContains(sample, "            b9.Append((int)row.Color);\n");
    ExpectContains(
        sample,
        "            if (row.Tint is { } x10) b10.Append((int)x10); else b10.AppendNull();\n");
    ExpectContains(
        sample,
        "        return new global::Apache.Arrow.IArrowArray[] { c0, c1, c2, c3, c4, c5, c6, "
        "c7, c8, c9, c10, c11 };\n");
}

TEST(CsVisitor, ScalarOnlyMessageGetsFromArrow) {
    DescriptorPool pool;
    const FileDescriptor* file = BuildFixture(pool);
    ASSERT_NE(file, nullptr);
    const std::string sample = ClassBody(CSharp(file), "Sample");
    ExpectContains(sample,
                   "    public static Sample FromArrow(global::Apache.Arrow.StructArray array, int "
                   "index)\n");
    // D-BIND-102: Fields is read once per call, not once per column.
    ExpectContains(sample, "        var fields = array.Fields;\n");
    ExpectContains(sample,
                   "        row.Flag = ((global::Apache.Arrow.BooleanArray)fields[0])"
                   ".GetValue(index).GetValueOrDefault();\n");
    ExpectContains(sample,
                   "        row.Name = ((global::Apache.Arrow.StringArray)fields[6])"
                   ".GetString(index) ?? \"\";\n");
    ExpectContains(sample,
                   "        row.Note = ((global::Apache.Arrow.StringArray)fields[7])"
                   ".GetString(index);\n");
    ExpectContains(sample,
                   "        { var a = (global::Apache.Arrow.BinaryArray)fields[8]; row.Blob = "
                   "a.IsNull(index) ? global::System.Array.Empty<byte>() : "
                   "a.GetBytes(index).ToArray(); }\n");
    ExpectContains(sample,
                   "        row.Color = (Color)((global::Apache.Arrow.Int32Array)fields[9])"
                   ".GetValue(index).GetValueOrDefault();\n");
    ExpectContains(sample,
                   "        row.Tint = ((global::Apache.Arrow.Int32Array)fields[10])"
                   ".GetValue(index) is int v10 ? (Color)v10 : null;\n");
    ExpectContains(sample,
                   "        row.Maybe = ((global::Apache.Arrow.Int32Array)fields[11])"
                   ".GetValue(index);\n");
}

TEST(CsVisitor, ListsStructsAndMapsConvertWithTheirSchemasOwnTypes) {
    // BIND-6c-2: every Arrow type a composite column is built with is read back from the
    // class's own Schema, so a ToArrow cannot build a column its Schema disagrees with.
    DescriptorPool pool;
    const FileDescriptor* file = BuildFixture(pool);
    ASSERT_NE(file, nullptr);
    const std::string player = ClassBody(CSharp(file), "Player");
    // repeated int32 scores = 8 (column 7): offsets, then the element builder.
    ExpectContains(player,
                   "        var t7 = (global::Apache.Arrow.Types.ListType)"
                   "Schema.GetFieldByIndex(7).DataType;\n");
    ExpectContains(player, "            foreach (var x7 in row.Scores) b7.Append(x7);\n");
    ExpectContains(player, "            o7.Append(b7.Length);\n");
    ExpectContains(
        player,
        "        var c7 = new global::Apache.Arrow.ListArray(t7, rows.Count, o7.Build(), e7, "
        "global::Apache.Arrow.ArrowBuffer.Empty, 0, 0);\n");
    // Stats stats = 12 (column 11), a plain message field: always present, so no validity
    // bitmap, and a null forced past the annotation is refused with the row (D-BIND-83);
    // its children come from its own class.
    ExpectContains(
        player,
        "            r11.Add(rows[i].Stats ?? throw new global::System.ArgumentException("
        "$\"Player.Stats is null in row {i}, but the field is not optional: it is "
        "always present.\", nameof(rows)));\n");
    ExpectContains(player,
                   "        var c11 = new global::Apache.Arrow.StructArray(t11, rows.Count, "
                   "global::Fletcher.Gen.Integration.Player_Stats.ToArrowColumns(r11), "
                   "global::Apache.Arrow.ArrowBuffer.Empty, 0, "
                   "0);\n");
    EXPECT_EQ(player.find("m11"), std::string::npos) << player;
    // map<string, int32> tags = 13 (column 12): keys and values in entry order.
    ExpectContains(player, "                k12.Append(x12.Key);\n");
    ExpectContains(player, "                b12.Append(x12.Value);\n");
    ExpectContains(
        player,
        "        var c12 = new global::Apache.Arrow.MapArray(t12, rows.Count, o12.Build(), "
        "kv12, global::Apache.Arrow.ArrowBuffer.Empty, 0, 0);\n");
    // Reading back: the list's slice, the plain struct read as present (D-BIND-83), the
    // map's pairs.
    ExpectContains(player,
                   "        { var s = (global::Apache.Arrow.StructArray)fields[11]; row.Stats = "
                   "global::Fletcher.Gen.Integration.Player_Stats.FromArrow(s, index); }\n");
    ExpectContains(player,
                   "    for (var j = start; j < end; j++) x.Add(new "
                   "global::System.Collections.Generic.KeyValuePair<string, int>(k.GetString(j) ?? "
                   "\"\", e.GetValue(j).GetValueOrDefault()));\n");
}

TEST(CsVisitor, ListsAndMapsOfMessagesReuseTheElementClass) {
    DescriptorPool pool;
    const FileDescriptor* file = BuildFixture(pool);
    ASSERT_NE(file, nullptr);
    const std::string holder = ClassBody(CSharp(file), "Holder");
    ExpectContains(holder, "            f1.AddRange(row.History);\n");
    ExpectContains(holder,
                   "        var e1 = new global::Apache.Arrow.StructArray("
                   "(global::Apache.Arrow.Types.StructType)t1.ValueDataType, f1.Count, "
                   "global::Fletcher.Gen.Integration.Player_Stats.ToArrowColumns(f1), "
                   "global::Apache.Arrow.ArrowBuffer.Empty, 0, 0);\n");
    ExpectContains(holder, "                f2.Add(x2.Value);\n");
    ExpectContains(
        holder,
        "x.Add(new global::System.Collections.Generic.KeyValuePair<string, "
        "global::Fletcher.Gen.Integration.Player_Stats>("
        "k.GetString(j) ?? \"\", global::Fletcher.Gen.Integration.Player_Stats.FromArrow(e, "
        "j)));\n");
}

TEST(CsVisitor, AnOptionalMessageKeepsItsSlotAndItsValidityBit) {
    // `optional Player.Stats maybe_best = 4` (column 3): its column is nullable, so an absent
    // one keeps its slot as a default instance with a cleared validity bit, and reads back
    // as null. The plain field beside it (column 0) has no bitmap at all (D-BIND-83).
    DescriptorPool pool;
    const FileDescriptor* file = BuildFixture(pool);
    ASSERT_NE(file, nullptr);
    const std::string holder = ClassBody(CSharp(file), "Holder");
    ExpectContains(holder,
                   "            if (row.MaybeBest is { } x3) { r3.Add(x3); m3.Append(true); }\n");
    ExpectContains(holder,
                   "            else { r3.Add(new "
                   "global::Fletcher.Gen.Integration.Player_Stats()); m3.Append(false); }\n");
    ExpectContains(holder,
                   "row.MaybeBest = s.IsNull(index) ? null : "
                   "global::Fletcher.Gen.Integration.Player_Stats.FromArrow(s, index); }\n");
    ExpectContains(
        holder,
        "row.Best = global::Fletcher.Gen.Integration.Player_Stats.FromArrow(s, index); }\n");
    EXPECT_EQ(holder.find("m0"), std::string::npos) << holder;
}

TEST(CsVisitor, TimestampAndDurationAreRecountedInTheColumnsUnitExactly) {
    // D-BIND-26: writing goes through WithUnit, which refuses to drop a remainder, so a
    // digit is never lost on the way in; reading returns the column's unit and zone.
    DescriptorPool pool;
    const FileDescriptor* file = BuildFixture(pool);
    ASSERT_NE(file, nullptr);
    const std::string clock = ClassBody(CSharp(file), "Clock");
    ExpectContains(clock,
                   "        var t0 = (global::Apache.Arrow.Types.TimestampType)"
                   "Schema.GetFieldByIndex(0).DataType;\n");
    ExpectContains(clock, "            v0.Append(row.At.WithUnit(t0.Unit).Value);\n");
    ExpectContains(
        clock,
        "        var c0 = new global::Apache.Arrow.TimestampArray(t0, v0.Build(), z0 > 0 ? "
        "m0.Build() : global::Apache.Arrow.ArrowBuffer.Empty, rows.Count, z0, 0);\n");
    ExpectContains(
        clock,
        "            if (row.MaybeAt is { } x2) { v2.Append(x2.WithUnit(t2.Unit).Value); "
        "m2.Append(true); }\n");
    ExpectContains(
        clock,
        "        var c1 = new global::Apache.Arrow.DurationArray(t1, v1.Build(), z1 > 0 ? "
        "m1.Build() : global::Apache.Arrow.ArrowBuffer.Empty, rows.Count, z1, 0);\n");
    ExpectContains(clock,
                   "        row.At = new global::Eiva.Fletcher.Model.Timestamp("
                   "a.GetValue(index).GetValueOrDefault(), t.Unit, t.Timezone);\n");
    ExpectContains(clock,
                   "        row.MaybeAt = a.GetValue(index) is long v ? new "
                   "global::Eiva.Fletcher.Model.Timestamp(v, t.Unit, t.Timezone) : null;\n");
    ExpectContains(clock,
                   "        row.Took = new global::Eiva.Fletcher.Model.Duration("
                   "a.GetValue(index).GetValueOrDefault(), t.Unit);\n");
}

TEST(CsVisitor, AMessageFromAnotherFileConvertsThroughItsPublicToArrow) {
    // D-BIND-80: the other file's class may be compiled into another assembly, where its
    // internal ToArrowColumns is out of reach, so its public ToArrow builds the children.
    // Reading back goes through its public FromArrow, as for any message.
    DescriptorPool pool;
    const FileDescriptor* file = BuildFixture(pool);
    ASSERT_NE(file, nullptr);
    const std::string timed = ClassBody(CSharp(file), "Timed");
    ExpectContains(timed,
                   "        var e3 = new global::Apache.Arrow.StructArray("
                   "(global::Apache.Arrow.Types.StructType)t3.ValueDataType, f3.Count, "
                   "global::Fletcher.Gen.Google.Protobuf.Timestamp.ToArrow(f3).Arrays, "
                   "global::Apache.Arrow.ArrowBuffer.Empty, 0, 0);\n");
    ExpectContains(timed, "global::Fletcher.Gen.Google.Protobuf.Timestamp.FromArrow(e, j)");
    EXPECT_EQ(timed.find("Timestamp.ToArrowColumns("), std::string::npos) << timed;
}

TEST(CsVisitor, AMessageOfThisFileStillLendsItsInternalColumns) {
    // Wraps embeds Timed, declared in the same file: its children come from the internal
    // ToArrowColumns, so the output for a single file is what it was before BIND-6e.
    DescriptorPool pool;
    const FileDescriptor* file = BuildFixture(pool);
    ASSERT_NE(file, nullptr);
    const std::string wraps = ClassBody(CSharp(file), "Wraps");
    ExpectContains(wraps,
                   "rows.Count, global::Fletcher.Gen.Integration.Timed.ToArrowColumns(r0), ");
    EXPECT_EQ(wraps.find(".ToArrow(r0)"), std::string::npos) << wraps;
}

TEST(CsVisitor, EveryConvertibleMessageGetsBothHalves) {
    // No message whose fields all convert is left without ToArrow, ToArrowColumns and
    // FromArrow; since BIND-6e that is every message of the fixture but the flatten
    // wrappers, which are inlined where they are used.
    DescriptorPool pool;
    const FileDescriptor* file = BuildFixture(pool);
    ASSERT_NE(file, nullptr);
    const std::string cs = CSharp(file);
    for (const char* name :
         {"Player", "Player_Stats", "Sample", "Timed", "Holder", "Clock", "Wraps", "Nested"}) {
        const std::string body = ClassBody(cs, name);
        EXPECT_NE(body.find(" ToArrow("), std::string::npos) << name;
        EXPECT_NE(body.find(" ToArrowColumns("), std::string::npos) << name;
        EXPECT_NE(body.find(" FromArrow("), std::string::npos) << name;
    }
}

// ===========================================================================
// BIND-6e: types from another file, in another package.
// ===========================================================================

namespace {

// shared.proto:  package geo.shared_types;
//   enum Quality { QUALITY_UNSPECIFIED = 0; QUALITY_GOOD = 1; }
//   message Pos { double lat = 1; }
// track.proto:   package app;  import "shared.proto";  import "google/protobuf/empty.proto";
//   message Track {
//     geo.shared_types.Pos at = 1;  repeated geo.shared_types.Pos path = 2;
//     map<string, geo.shared_types.Pos> by_name = 3;  geo.shared_types.Quality q = 4;
//     repeated geo.shared_types.Quality qs = 5;
//   }
//   message Batch { option (fletcher.flatten) = true; repeated geo.shared_types.Pos items = 1; }
//   service Feed {
//     rpc Send(stream Track) returns (google.protobuf.Empty);
//     rpc Push(stream Batch) returns (google.protobuf.Empty);
//   }
const FileDescriptor* BuildCrossFileFixture(DescriptorPool& pool) {
    const FileDescriptor* empty = google::protobuf::Empty::GetDescriptor()->file();
    FileDescriptorProto empty_copy;
    empty->CopyTo(&empty_copy);
    if (pool.BuildFile(empty_copy) == nullptr) return nullptr;

    FileDescriptorProto shared;
    shared.set_name("shared.proto");
    shared.set_package("geo.shared_types");
    shared.set_syntax("proto3");
    auto* quality = shared.add_enum_type();
    quality->set_name("Quality");
    auto* q0 = quality->add_value();
    q0->set_name("QUALITY_UNSPECIFIED");
    q0->set_number(0);
    auto* q1 = quality->add_value();
    q1->set_name("QUALITY_GOOD");
    q1->set_number(1);
    AddField(shared.add_message_type(), "lat", 1, FieldDescriptorProto::TYPE_DOUBLE);
    shared.mutable_message_type(0)->set_name("Pos");
    if (pool.BuildFile(shared) == nullptr) return nullptr;

    FileDescriptorProto fdp;
    fdp.set_name("track.proto");
    fdp.set_package("app");
    fdp.set_syntax("proto3");
    fdp.add_dependency("shared.proto");
    fdp.add_dependency("google/protobuf/empty.proto");
    auto* track = fdp.add_message_type();
    track->set_name("Track");
    AddField(track, "at", 1, FieldDescriptorProto::TYPE_MESSAGE)
        ->set_type_name(".geo.shared_types.Pos");
    AddField(track, "path", 2, FieldDescriptorProto::TYPE_MESSAGE,
             FieldDescriptorProto::LABEL_REPEATED)
        ->set_type_name(".geo.shared_types.Pos");
    auto* entry = track->add_nested_type();
    entry->set_name("ByNameEntry");
    entry->mutable_options()->set_map_entry(true);
    AddField(entry, "key", 1, FieldDescriptorProto::TYPE_STRING);
    AddField(entry, "value", 2, FieldDescriptorProto::TYPE_MESSAGE)
        ->set_type_name(".geo.shared_types.Pos");
    AddField(track, "by_name", 3, FieldDescriptorProto::TYPE_MESSAGE,
             FieldDescriptorProto::LABEL_REPEATED)
        ->set_type_name(".app.Track.ByNameEntry");
    AddField(track, "q", 4, FieldDescriptorProto::TYPE_ENUM)
        ->set_type_name(".geo.shared_types.Quality");
    AddField(track, "qs", 5, FieldDescriptorProto::TYPE_ENUM, FieldDescriptorProto::LABEL_REPEATED)
        ->set_type_name(".geo.shared_types.Quality");

    auto* batch = fdp.add_message_type();
    batch->set_name("Batch");
    batch->mutable_options()
        ->GetReflection()
        ->MutableUnknownFields(batch->mutable_options())
        ->AddVarint(50000, 1);  // (fletcher.flatten) = true, as BuildFixture sets it
    AddField(batch, "items", 1, FieldDescriptorProto::TYPE_MESSAGE,
             FieldDescriptorProto::LABEL_REPEATED)
        ->set_type_name(".geo.shared_types.Pos");

    auto* feed = fdp.add_service();
    feed->set_name("Feed");
    for (const auto& [name, input] :
         {std::pair<const char*, const char*>{"Send", ".app.Track"}, {"Push", ".app.Batch"}}) {
        auto* m = feed->add_method();
        m->set_name(name);
        m->set_input_type(input);
        m->set_output_type(".google.protobuf.Empty");
        m->set_client_streaming(true);
    }
    return pool.BuildFile(fdp);
}

std::string Emitted(const std::map<std::string, std::string>& files, const std::string& name) {
    const auto it = files.find(name);
    EXPECT_NE(it, files.end()) << "no " << name << " emitted";
    return it == files.end() ? std::string() : it->second;
}

}  // namespace

TEST(CsVisitorCrossFile, TypesFromAnotherPackageAreNamedInFull) {
    DescriptorPool pool;
    const FileDescriptor* file = BuildCrossFileFixture(pool);
    ASSERT_NE(file, nullptr);
    const std::string track =
        ClassBody(Emitted(GenerateWith(file, "csharp"), "track.fletcher.cs"), "Track");
    const std::string pos = "global::Fletcher.Gen.Geo.SharedTypes.Pos";
    const std::string quality = "global::Fletcher.Gen.Geo.SharedTypes.Quality";
    ExpectContains(track, "    public " + pos + " At { get; set; } = new();\n");  // D-BIND-83
    ExpectContains(track, "    public global::System.Collections.Generic.List<" + pos +
                              "> Path { get; set; } = new();\n");
    ExpectContains(track,
                   "    public global::System.Collections.Generic.List<global::System.Collections."
                   "Generic.KeyValuePair<string, " +
                       pos + ">> ByName { get; set; } = new();\n");
    ExpectContains(track, "    public " + quality + " Q { get; set; }\n");
    ExpectContains(track, "    public global::System.Collections.Generic.List<" + quality +
                              "> Qs { get; set; } = new();\n");
}

TEST(CsVisitorCrossFile, EveryStructColumnOfAnotherFileIsBuiltByItsPublicToArrow) {
    // D-BIND-80, at each place a child class builds columns: a message field, a list
    // element and a map value. An enum from the other file crosses as int32 both ways.
    DescriptorPool pool;
    const FileDescriptor* file = BuildCrossFileFixture(pool);
    ASSERT_NE(file, nullptr);
    const std::string cs = Emitted(GenerateWith(file, "csharp"), "track.fletcher.cs");
    const std::string track = ClassBody(cs, "Track");
    const std::string pos = "global::Fletcher.Gen.Geo.SharedTypes.Pos";
    ExpectContains(track, "rows.Count, " + pos + ".ToArrow(r0).Arrays, ");
    ExpectContains(track, "f1.Count, " + pos + ".ToArrow(f1).Arrays, ");
    ExpectContains(track, "f2.Count, " + pos + ".ToArrow(f2).Arrays, ");
    ExpectContains(track, pos + ".FromArrow(");
    ExpectContains(track, "(int)row.Q");
    ExpectContains(track, "(global::Fletcher.Gen.Geo.SharedTypes.Quality)");
    EXPECT_EQ(track.find(pos + ".ToArrowColumns("), std::string::npos) << track;
    // Nothing of shared.proto is declared again here: it is generated from its own file.
    EXPECT_EQ(cs.find("class Pos"), std::string::npos) << cs;
    EXPECT_EQ(cs.find("enum Quality"), std::string::npos) << cs;
}

TEST(CsVisitorCrossFile, APairCarriesAnotherFilesMessageAndAFlattenWrapperIsSkippedAsInCpp) {
    // Track converts, so Feed.Send gets its pair. Batch is a flatten wrapper: inlined where it is
    // used, it has no class of its own in C++, so C++ skips Feed.Push with its reason. D-BIND-90:
    // C# follows C++, with C++'s own line in both files and no topic class.
    DescriptorPool pool;
    const FileDescriptor* file = BuildCrossFileFixture(pool);
    ASSERT_NE(file, nullptr);
    const std::string skipped =
        "// Skipped: Feed.Push \xE2\x80\x94 input message 'Batch' has no generated Arrow mapping "
        "in "
        "this file\n";
    ExpectContains(Emitted(GenerateWith(file, ""), "track.fletcher.pb.h"), skipped);

    const auto files = GenerateWith(file, "csharp");
    const std::string model = Emitted(files, "track.fletcher.cs");
    const std::string native = Emitted(files, "track.fletcher.native.cs");
    ExpectContains(native, "public sealed class Feed_SendPublisher");
    ExpectContains(model, "public static class Feed_SendTopic\n");
    ExpectContains(native, skipped);
    ExpectContains(model, skipped);
    EXPECT_EQ(native.find("Feed_PushPublisher"), std::string::npos) << native;
    EXPECT_EQ(model.find("Feed_PushTopic"), std::string::npos) << model;
}

// D-BIND-87: C# decides "from another file" through CollectCrossFileMessages, the walk C++
// derives its includes from. A field-level flatten inlines the flattened message's own fields
// into the host, so a message THOSE fields name is a column of the host too:
//   parts.proto: package geo.parts;  message Leaf { int32 id = 1; }
//   spot.proto:  package geo.spots;  message Spot { geo.parts.Leaf leaf = 1; double x = 2; }
//   host.proto:  package app;        message Host { geo.spots.Spot spot = 1
//   [(fletcher.flatten_field) = true]; }
TEST(CsVisitorCrossFile, AMessageInlinedByAFieldFlattenFromAnotherFileIsFoundByTheSharedWalk) {
    DescriptorPool pool;
    FileDescriptorProto parts;
    parts.set_name("parts.proto");
    parts.set_package("geo.parts");
    parts.set_syntax("proto3");
    auto* leaf = parts.add_message_type();
    leaf->set_name("Leaf");
    AddField(leaf, "id", 1, FieldDescriptorProto::TYPE_INT32);
    ASSERT_NE(pool.BuildFile(parts), nullptr);

    FileDescriptorProto spots;
    spots.set_name("spot.proto");
    spots.set_package("geo.spots");
    spots.set_syntax("proto3");
    spots.add_dependency("parts.proto");
    auto* spot = spots.add_message_type();
    spot->set_name("Spot");
    AddField(spot, "leaf", 1, FieldDescriptorProto::TYPE_MESSAGE)->set_type_name(".geo.parts.Leaf");
    AddField(spot, "x", 2, FieldDescriptorProto::TYPE_DOUBLE);
    ASSERT_NE(pool.BuildFile(spots), nullptr);

    FileDescriptorProto hosts;
    hosts.set_name("host.proto");
    hosts.set_package("app");
    hosts.set_syntax("proto3");
    hosts.add_dependency("spot.proto");
    auto* host = hosts.add_message_type();
    host->set_name("Host");
    auto* flat = AddField(host, "spot", 1, FieldDescriptorProto::TYPE_MESSAGE);
    flat->set_type_name(".geo.spots.Spot");
    flat->mutable_options()
        ->GetReflection()
        ->MutableUnknownFields(flat->mutable_options())
        ->AddVarint(50000, 1);  // (fletcher.flatten_field) = true
    const FileDescriptor* file = pool.BuildFile(hosts);
    ASSERT_NE(file, nullptr);

    const Descriptor* leaf_desc = pool.FindMessageTypeByName("geo.parts.Leaf");
    ASSERT_NE(leaf_desc, nullptr);
    EXPECT_EQ(fletcher::CollectCrossFileMessages(file).count(leaf_desc), 1u);
    // C++ derives its includes from the same set: the host names Leaf, so it includes Leaf's header
    // itself rather than through Spot's.
    EXPECT_EQ(fletcher::CollectCrossFileIncludes(file),
              (std::set<std::string>{"parts.fletcher.pb.h", "spot.fletcher.pb.h"}));

    const std::string body =
        ClassBody(Emitted(GenerateWith(file, "csharp"), "host.fletcher.cs"), "Host");
    const std::string leaf_cls = "global::Fletcher.Gen.Geo.Parts.Leaf";
    ExpectContains(body, leaf_cls + ".ToArrow(");
    EXPECT_EQ(body.find(leaf_cls + ".ToArrowColumns("), std::string::npos) << body;
}

// D-BIND-89 (a): a host field and a field a field-level flatten inlines may PascalCase alike, which
// protoc accepts in proto3 too (they are in different messages). The later property is suffixed:
//   message Inner { int32 fooBar = 1; }
//   message Host  { int32 foo_bar = 1; Inner inner = 2 [(fletcher.flatten_field) = true]; }
TEST(CsVisitor, AnInlinedFieldThatPascalCasesLikeAHostFieldGetsTheNextFreeName) {
    DescriptorPool pool;
    FileDescriptorProto fdp;
    fdp.set_name("inline_names.proto");
    fdp.set_package("app");
    fdp.set_syntax("proto3");
    auto* inner = fdp.add_message_type();
    inner->set_name("Inner");
    AddField(inner, "fooBar", 1, FieldDescriptorProto::TYPE_INT32);
    auto* host = fdp.add_message_type();
    host->set_name("Host");
    AddField(host, "foo_bar", 1, FieldDescriptorProto::TYPE_INT32);
    auto* flat = AddField(host, "inner", 2, FieldDescriptorProto::TYPE_MESSAGE);
    flat->set_type_name(".app.Inner");
    flat->mutable_options()
        ->GetReflection()
        ->MutableUnknownFields(flat->mutable_options())
        ->AddVarint(50000, 1);  // (fletcher.flatten_field) = true
    const FileDescriptor* file = pool.BuildFile(fdp);
    ASSERT_NE(file, nullptr);

    const std::string body =
        ClassBody(Emitted(GenerateWith(file, "csharp"), "inline_names.fletcher.cs"), "Host");
    ExpectContains(body, "    public int FooBar { get; set; }\n");
    ExpectContains(body, "    public int FooBar_ { get; set; }\n");
    // Each column is read from and written to its own property.
    ExpectContains(body, "row.FooBar)");
    ExpectContains(body, "row.FooBar_)");
    ExpectContains(body, "row.FooBar_ = ");
}

TEST(CsVisitorCrossFile, AFileWhoseProtocNamespaceIsInsideFletcherGenIsRefusedBeforeAnyOutput) {
    // BIND-6e: with `csharp`, a file whose protoc C# would land in Fletcher.Gen fails the whole
    // file before anything is written, so no half-generated set is left behind. Without
    // `csharp` there is no Fletcher C# to collide with, and nothing changes.
    FileDescriptorProto fdp;
    fdp.set_name("reserved.proto");
    fdp.set_package("nav");
    fdp.set_syntax("proto3");
    fdp.mutable_options()->set_csharp_namespace("Fletcher.Gen.Nav");
    AddField(fdp.add_message_type(), "lat", 1, FieldDescriptorProto::TYPE_DOUBLE);
    fdp.mutable_message_type(0)->set_name("Fix");
    DescriptorPool pool;
    const FileDescriptor* file = pool.BuildFile(fdp);
    ASSERT_NE(file, nullptr);

    MemoryContext ctx;
    std::string error;
    EXPECT_FALSE(fletcher::ArrowRowGenerator().Generate(file, "csharp", &ctx, &error));
    EXPECT_NE(error.find("reserved.proto: protoc's C# for this file would be in namespace "
                         "'Fletcher.Gen.Nav' (from option csharp_namespace)"),
              std::string::npos)
        << error;
    EXPECT_TRUE(ctx.files().empty()) << ctx.files().size() << " file(s) written";

    EXPECT_FALSE(GenerateWith(file, "").empty());
}

// ===========================================================================
// BIND-6d: topics in the model file, and the native pair in its own file.
// Driven by tests/golden/csharp_pair.proto, whose generated C# is committed beside
// it and compiled by dotnet/tests/Fletcher.Tests against the native runtime.
// ===========================================================================

namespace {

class CollectErrors : public io::ErrorCollector {
   public:
    std::string text;
    void AddError(int line, int column, const std::string& message) override {
        text += std::to_string(line) + ":" + std::to_string(column) + ": " + message + "\n";
    }
    void AddWarning(int, int, const std::string&) override {}
};

std::filesystem::path GoldenDir() { return std::filesystem::path(SCHEMA_GOLDEN_DIR); }

std::string ReadText(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Parses `source` as the file `name` into `pool`, with the well-known files the goldens import
// copied in first.
const FileDescriptor* BuildSource(DescriptorPool& pool, const std::string& name,
                                  const std::string& source) {
    const FileDescriptor* wkt_files[] = {
        google::protobuf::Timestamp::GetDescriptor()->file(),
        google::protobuf::Duration::GetDescriptor()->file(),
        google::protobuf::Empty::GetDescriptor()->file(),
    };
    for (const FileDescriptor* wkt : wkt_files) {
        if (pool.FindFileByName(wkt->name()) != nullptr) continue;
        FileDescriptorProto copy;
        wkt->CopyTo(&copy);
        if (pool.BuildFile(copy) == nullptr) return nullptr;
    }
    // BIND-7b: a golden with a flatten wrapper imports Fletcher's options, which extend
    // descriptor.proto; both are parsed in first, from the plugin's own copy.
    if (source.find("\"fletcher/options.proto\"") != std::string::npos &&
        pool.FindFileByName("fletcher/options.proto") == nullptr) {
        const FileDescriptor* descriptor = DescriptorProto::descriptor()->file();
        if (pool.FindFileByName(descriptor->name()) == nullptr) {
            FileDescriptorProto copy;
            descriptor->CopyTo(&copy);
            if (pool.BuildFile(copy) == nullptr) return nullptr;
        }
        const std::string options =
            ReadText(GoldenDir() / ".." / ".." / "include" / "fletcher" / "options.proto");
        if (options.empty() || BuildSource(pool, "fletcher/options.proto", options) == nullptr) {
            ADD_FAILURE() << "cannot build fletcher/options.proto";
            return nullptr;
        }
    }
    io::ArrayInputStream input(source.data(), static_cast<int>(source.size()));
    CollectErrors errors;
    io::Tokenizer tokenizer(&input, &errors);
    compiler::Parser parser;
    parser.RecordErrorsTo(&errors);
    FileDescriptorProto fdp;
    if (!parser.Parse(&tokenizer, &fdp)) {
        ADD_FAILURE() << "parse failed:\n" << errors.text;
        return nullptr;
    }
    fdp.set_name(name);
    return pool.BuildFile(fdp);
}

// Parses tests/golden/<name> into `pool` (BuildSource).
const FileDescriptor* BuildGoldenFixture(DescriptorPool& pool, const std::string& name) {
    const std::string source = ReadText(GoldenDir() / name);
    if (source.empty()) {
        ADD_FAILURE() << "cannot read " << (GoldenDir() / name);
        return nullptr;
    }
    return BuildSource(pool, name, source);
}

// The pair fixture every CsVisitorPair case uses.
const FileDescriptor* BuildPairFixture(DescriptorPool& pool) {
    return BuildGoldenFixture(pool, "csharp_pair.proto");
}

std::string FileOf(const std::map<std::string, std::string>& files, const std::string& name) {
    const auto it = files.find(name);
    EXPECT_NE(it, files.end()) << "no " << name << " emitted";
    return it == files.end() ? std::string() : it->second;
}

}  // namespace

TEST(CsVisitorPair, TheModelFileSpellsEachTopicAsAStaticClass) {
    // D-BIND-77: Segments in C++'s form (the package one segment, dots kept) and Key.
    DescriptorPool pool;
    const FileDescriptor* file = BuildPairFixture(pool);
    ASSERT_NE(file, nullptr);
    const std::string cs = FileOf(GenerateWith(file, "csharp"), "csharp_pair.fletcher.cs");
    ExpectContains(cs, "public static class Telemetry_ReportTopic\n{\n");
    ExpectContains(cs,
                   "    public static global::System.Collections.Generic.IReadOnlyList<string> "
                   "Segments { get; } = new[] { \"golden.pair\", \"Telemetry\", \"Report\" };\n");
    ExpectContains(cs, "    public const string Key = \"golden.pair/Telemetry/Report\";\n");
    // Ping is not pub/sub, and says why in C++'s words.
    ExpectContains(cs,
                   "// Skipped: Telemetry.Ping \xE2\x80\x94 request is not streaming (pub/sub "
                   "requires 'stream' on request)\n");
    EXPECT_EQ(cs.find("Telemetry_PingTopic"), std::string::npos) << cs;
    // The model file names nothing of Eiva.Fletcher but the Model package: it compiles
    // against Apache.Arrow alone (D-BIND-72, D-BIND-74).
    for (size_t at = cs.find("Eiva.Fletcher."); at != std::string::npos;
         at = cs.find("Eiva.Fletcher.", at + 1))
        EXPECT_EQ(cs.compare(at, 20, "Eiva.Fletcher.Model."), 0) << cs.substr(at, 40);
}

TEST(CsVisitorPair, CsharpWritesThePairUnlessModelOnlyIsAsked) {
    // D-BIND-76, re-ruled: `csharp` writes the pair in its own file, as C++'s header
    // always carries its pair; `csharp_model_only` opts out, as `schema_only` does for
    // C++, and leaves the model file byte-identical.
    DescriptorPool pool;
    const FileDescriptor* file = BuildPairFixture(pool);
    ASSERT_NE(file, nullptr);
    const auto both = GenerateWith(file, "csharp");
    const auto model_only = GenerateWith(file, "csharp,csharp_model_only");
    ASSERT_EQ(both.count("csharp_pair.fletcher.native.cs"), 1u);
    EXPECT_EQ(model_only.count("csharp_pair.fletcher.native.cs"), 0u);
    EXPECT_EQ(both.size(), model_only.size() + 1);
    for (const auto& [name, content] : model_only) {
        ASSERT_EQ(both.count(name), 1u) << name;
        EXPECT_EQ(both.at(name), content) << name << " differs between csharp and model-only";
    }
    // The opt-out means nothing without `csharp`: it adds no file of its own.
    DescriptorPool pool2;
    const FileDescriptor* file2 = BuildPairFixture(pool2);
    ASSERT_NE(file2, nullptr);
    EXPECT_EQ(GenerateWith(file2, "csharp_model_only").size(), GenerateWith(file2, "").size());
}

TEST(CsVisitorPair, ThePublisherDeclaresItsTopicAndPublishesPerRowAndInBatches) {
    DescriptorPool pool;
    const FileDescriptor* file = BuildPairFixture(pool);
    ASSERT_NE(file, nullptr);
    const std::string native =
        FileOf(GenerateWith(file, "csharp"), "csharp_pair.fletcher.native.cs");
    const std::string pub =
        ClassBody(native, "Telemetry_ReportPublisher : global::System.IDisposable");
    ASSERT_FALSE(pub.empty()) << native;
    ExpectContains(pub,
                   "    public static global::Eiva.Fletcher.TopicPath Topic { get; } = "
                   "global::Eiva.Fletcher.TopicPath.Of([.. Telemetry_ReportTopic.Segments]);\n");
    ExpectContains(pub, "    public const string TopicKey = Telemetry_ReportTopic.Key;\n");
    ExpectContains(pub,
                   "                _publisher.CreateTopic(Topic, "
                   "global::Fletcher.Gen.Golden.Pair.Reading.Schema, options);\n");
    // D-BIND-78: per-row and synchronous, with the price where it is chosen.
    ExpectContains(pub, "about 3.8 microseconds a row, 19.6 times the generated C++ publisher");
    ExpectContains(pub,
                   "        using var batch = "
                   "global::Fletcher.Gen.Golden.Pair.Reading.ToArrow(new[] { row });\n");
    ExpectContains(pub, "        _publisher.Publish(Topic, bound, 0, attachments);\n");
    // The batch form: one ToArrow, one bind, one crossing.
    ExpectContains(
        pub,
        "    public void Publish(global::System.Collections.Generic.IEnumerable<Reading> "
        "rows)\n");
    ExpectContains(pub, "        _publisher.Publish(Topic, bound);\n");
}

TEST(CsVisitorPair, TheSubscriberTakesAGeneratedDelegateNotAnAction) {
    // D-BIND-79: Action<Reading, AttachmentsView> does not compile for net8.0.
    DescriptorPool pool;
    const FileDescriptor* file = BuildPairFixture(pool);
    ASSERT_NE(file, nullptr);
    const std::string native =
        FileOf(GenerateWith(file, "csharp"), "csharp_pair.fletcher.native.cs");
    ExpectContains(native,
                   "public delegate void Telemetry_ReportHandler(Reading row, "
                   "global::Eiva.Fletcher.AttachmentsView attachments);\n");
    ExpectContains(
        native,
        "    public global::Eiva.Fletcher.Subscription Subscribe(Telemetry_ReportHandler "
        "handler, global::Eiva.Fletcher.TopicOptions? options = null)\n");
    ExpectContains(
        native,
        "            handler(global::Fletcher.Gen.Golden.Pair.Reading.FromArrow(columns, 0), "
        "attachments);\n");
    EXPECT_EQ(native.find("Action<"), std::string::npos) << native;
    // Q16: no such member (the doc comment names it, to say where to go instead).
    EXPECT_EQ(native.find(" SubscribeInPlace("), std::string::npos) << native;
}

TEST(CsVisitorPair, AMethodWithoutAPairSaysWhy) {
    DescriptorPool pool;
    const FileDescriptor* file = BuildPairFixture(pool);
    ASSERT_NE(file, nullptr);
    const std::string native =
        FileOf(GenerateWith(file, "csharp"), "csharp_pair.fletcher.native.cs");
    ExpectContains(native,
                   "// Skipped: Telemetry.Ping \xE2\x80\x94 request is not streaming (pub/sub "
                   "requires 'stream' on request)\n");
    EXPECT_EQ(native.find("Telemetry_PingPublisher"), std::string::npos) << native;
    // Since BIND-6e, Mark, whose rows carry timestamp.proto's struct, has its pair.
    ExpectContains(native, "public sealed class Telemetry_MarkPublisher");
}

TEST(CsVisitorPair, TheCommittedGoldensAreThePluginsOutput) {
    // Fletcher.Tests compiles these goldens; a byte of drift fails HERE, so the C# that
    // runs against the native runtime is always what the plugin emits today.
    // FLETCHER_CAPTURE_GOLDENS=1 rewrites them for review as source.
    DescriptorPool pool;
    const FileDescriptor* file = BuildPairFixture(pool);
    ASSERT_NE(file, nullptr);
    auto files = GenerateWith(file, "csharp");
    // BIND-6e: Marks names timestamp.proto's class, so its model is a golden too, generated
    // as a consumer's protoc call generates it, from the same pool.
    const FileDescriptor* timestamp = pool.FindFileByName("google/protobuf/timestamp.proto");
    ASSERT_NE(timestamp, nullptr);
    for (auto& [name, content] : GenerateWith(timestamp, "csharp,csharp_model_only"))
        files.emplace(name, content);
    // BIND-6 review B2: the names a bare class name in an expression would bind to.
    DescriptorPool names_pool;
    const FileDescriptor* names = BuildGoldenFixture(names_pool, "csharp_names.proto");
    ASSERT_NE(names, nullptr);
    for (auto& [name, content] : GenerateWith(names, "csharp")) files.emplace(name, content);
    // D-BIND-89 (a): proto2 fields that PascalCase to one property.
    DescriptorPool proto2_pool;
    const FileDescriptor* proto2 = BuildGoldenFixture(proto2_pool, "csharp_proto2.proto");
    ASSERT_NE(proto2, nullptr);
    for (auto& [name, content] : GenerateWith(proto2, "csharp")) files.emplace(name, content);
    // BIND-7a: the accessor and the view, beside their model.
    DescriptorPool accessor_pool;
    const FileDescriptor* accessor = BuildGoldenFixture(accessor_pool, "csharp_accessor.proto");
    ASSERT_NE(accessor, nullptr);
    for (auto& [name, content] : GenerateWith(accessor, "csharp,csharp_model_only,csharp_accessor"))
        files.emplace(name, content);
    // BIND-8c: the gateway pair for the pair fixture's methods, compiled into
    // Fletcher.GatewayClient.Tests.
    for (auto& [name, content] : GenerateWith(file, "csharp,csharp_model_only,csharp_gateway"))
        files.emplace(name, content);
    for (const char* name :
         {"csharp_pair.fletcher.cs", "csharp_pair.fletcher.native.cs",
          "csharp_pair.fletcher.gateway.cs", "google/protobuf/timestamp.fletcher.cs",
          "csharp_names.fletcher.cs", "csharp_names.fletcher.native.cs",
          "csharp_proto2.fletcher.cs", "csharp_accessor.fletcher.cs",
          "csharp_accessor.fletcher.accessor.cs", "csharp_accessor.fletcher.arrow.cs"}) {
        const std::string generated = FileOf(files, name);
        const auto path = GoldenDir() / name;
        if (const char* capture = std::getenv("FLETCHER_CAPTURE_GOLDENS");
            capture != nullptr && std::string(capture) == "1") {
            std::filesystem::create_directories(path.parent_path());
            std::ofstream(path, std::ios::binary) << generated;
            continue;
        }
        EXPECT_EQ(ReadText(path), generated) << name << " differs from the plugin's output";
    }
}

TEST(CsVisitorPair, AClassInACodeExpressionIsNamedInFull) {
    // BIND-6 review B2: in `RouteList`, the property `Waypoint` is a List<Waypoint>, so a bare
    // `Waypoint.ToArrowColumns(..)` bound to the property and did not compile (CS0120); in the
    // pair for a message called `Topic`, `Topic.Schema` bound to the pair's own Topic (CS1061).
    // Expressions now name the class from global::; declarations keep the short name, which a
    // member cannot capture in a type position.
    DescriptorPool pool;
    const FileDescriptor* file = BuildGoldenFixture(pool, "csharp_names.proto");
    ASSERT_NE(file, nullptr);
    const auto files = GenerateWith(file, "csharp");
    const std::string route = ClassBody(FileOf(files, "csharp_names.fletcher.cs"), "RouteList");
    const std::string waypoint = "global::Fletcher.Gen.Golden.Names.Waypoint";
    ExpectContains(route, waypoint + ".ToArrowColumns(f0)");
    ExpectContains(route, waypoint + ".FromArrow(e, j)");
    ExpectContains(
        route,
        "    public global::System.Collections.Generic.List<Waypoint> Waypoint { get; set; } "
        "= new();\n");
    const std::string native = FileOf(files, "csharp_names.fletcher.native.cs");
    ExpectContains(native, "Schema => global::Fletcher.Gen.Golden.Names.Topic.Schema;\n");
    ExpectContains(native,
                   "handler(global::Fletcher.Gen.Golden.Names.Topic.FromArrow(columns, 0), ");
    ExpectContains(native, "    public void Publish(Topic row) => Publish(row, null);\n");
    // A type named in lower-case letters only is written `@name`, everywhere it is named: a
    // keyword (`class`) would not compile otherwise, and any other such name draws CS8981.
    const std::string model = FileOf(files, "csharp_names.fletcher.cs");
    ExpectContains(model, "public sealed class @row\n");
    ExpectContains(model, "public sealed class @class\n");
    ExpectContains(model, "public enum @level : int\n");
    ExpectContains(model, "    public @class K { get; set; } = new();\n");
    ExpectContains(model, "global::Fletcher.Gen.Golden.Names.@class.FromArrow(");
    EXPECT_EQ(model.find("CS8981"), std::string::npos) << "no pragma: the names are escaped";
}

TEST(CsVisitorPair, TheTwoCsharpTokensFoldIntoThreeStates) {
    // D-BIND-76: `csharp` and the modifier `csharp_model_only` mean three things, held as
    // one CsharpOutput. Order does not matter, and the modifier alone means no C#: the
    // fourth combination two booleans would allow cannot be represented.
    using fletcher::CsharpOutput;
    const std::pair<const char*, CsharpOutput> cases[] = {
        {"", CsharpOutput::None},
        {"ts,ipc", CsharpOutput::None},
        {"csharp", CsharpOutput::ModelAndPair},
        {"ts,csharp,ipc", CsharpOutput::ModelAndPair},
        {"csharp,csharp_model_only", CsharpOutput::ModelOnly},
        {"csharp_model_only,csharp", CsharpOutput::ModelOnly},
        {"csharp_model_only", CsharpOutput::None},
    };
    for (const auto& [parameter, expected] : cases) {
        fletcher::PluginOptions options;
        std::string error;
        ASSERT_TRUE(fletcher::ParsePluginParameter(parameter, &options, &error)) << error;
        EXPECT_EQ(options.csharp, expected) << "'" << parameter << "'";
    }
}

// ===========================================================================
// BIND-6c-3: lists of lists, which flatten wrappers around a repeated message produce.
// Before 6c-3 such a field got no property and a marker blaming the wrong cause.
// ===========================================================================

TEST(CsVisitor, AListOfListsIsAListOfListsNotADroppedField) {
    DescriptorPool pool;
    const FileDescriptor* file = BuildFixture(pool);
    ASSERT_NE(file, nullptr);
    const std::string nested = ClassBody(CSharp(file), "Nested");
    ExpectContains(nested,
                   "    public global::System.Collections.Generic.List<"
                   "global::System.Collections.Generic.List<Player_Stats>> Table { get; set; } = "
                   "new();\n");
    ExpectContains(
        nested,
        "    public global::System.Collections.Generic.List<"
        "global::System.Collections.Generic.List<global::System.Collections.Generic.List<"
        "Player_Stats>>> Cube { get; set; } = new();\n");
    EXPECT_EQ(nested.find("Not generated yet"), std::string::npos) << nested;
}

TEST(CsVisitor, AListOfListsConvertsLevelByLevel) {
    // Each level's offsets come from the inner lists' lengths and its child is built from
    // their elements flattened, down to the element class's own columns; reading walks the
    // same levels with each list array's own offsets.
    DescriptorPool pool;
    const FileDescriptor* file = BuildFixture(pool);
    ASSERT_NE(file, nullptr);
    const std::string nested = ClassBody(CSharp(file), "Nested");
    ExpectContains(nested, "        foreach (var row in rows) v0.Add(row.Table);\n");
    ExpectContains(nested,
                   "        var t0 = (global::Apache.Arrow.Types.ListType)"
                   "Schema.GetFieldByIndex(0).DataType;\n");
    ExpectContains(nested, "global::Fletcher.Gen.Integration.Player_Stats.ToArrowColumns(f0_)");
    ExpectContains(
        nested,
        "global::Fletcher.Gen.Integration.Player_Stats.ToArrowColumns(f1__)");  // three levels deep
    ExpectContains(nested, "        var c0 = a0;\n");
    ExpectContains(nested,
                   "x0.Add(global::Fletcher.Gen.Integration.Player_Stats.FromArrow(e0_, j0_));\n");
    ExpectContains(nested, "            x.Add(x0);\n");
    EXPECT_NE(nested.find(" ToArrow("), std::string::npos) << nested;
    EXPECT_NE(nested.find(" FromArrow("), std::string::npos) << nested;
}

// ===========================================================================
// BIND-7a: the C# accessor and view, under `csharp_accessor` (D-BIND-94 to 100).
// ===========================================================================

namespace {

// Generates `source` (the file `accessor_case.proto`) with `opts`, returning false and the
// error when the generator refuses it.
bool GenerateSource(const std::string& source, const std::string& opts,
                    std::map<std::string, std::string>* files, std::string* error) {
    DescriptorPool pool;
    const FileDescriptor* file = BuildSource(pool, "accessor_case.proto", source);
    if (file == nullptr) {
        ADD_FAILURE() << "fixture does not build";
        return false;
    }
    MemoryContext ctx;
    const bool ok = fletcher::ArrowRowGenerator().Generate(file, opts, &ctx, error);
    *files = ctx.files();
    return ok;
}

std::map<std::string, std::string> AccessorGolden() {
    DescriptorPool pool;
    const FileDescriptor* file = BuildGoldenFixture(pool, "csharp_accessor.proto");
    EXPECT_NE(file, nullptr);
    if (file == nullptr) return {};
    return GenerateWith(file, "csharp,csharp_model_only,csharp_accessor");
}

// The text of one generated struct, as ClassBody gives a class.
std::string StructBody(const std::string& cs, const std::string& name) {
    const std::string head = "public readonly struct " + name + "\n{\n";
    const size_t start = cs.find(head);
    if (start == std::string::npos) return {};
    const size_t end = cs.find("\n}\n", start);
    return cs.substr(start, end == std::string::npos ? std::string::npos : end + 3 - start);
}

// The text of `<msg>Accessor`, which implements IArrowRows<<msg>View> (D-BIND-101).
std::string AccessorBody(const std::string& cs, const std::string& msg) {
    const std::string head = "public sealed class " + msg +
                             "Accessor : global::Eiva.Fletcher.Model.IArrowRows<" + msg +
                             "View>\n{\n";
    const size_t start = cs.find(head);
    if (start == std::string::npos) return {};
    const size_t end = cs.find("\n}\n", start);
    return cs.substr(start, end == std::string::npos ? std::string::npos : end + 3 - start);
}

// Every member declared `    public ` in a body, up to its body, initialiser or parameters:
// `public int Length`, `public static bool TryMake`.
std::vector<std::string> PublicMembers(const std::string& body) {
    std::vector<std::string> out;
    size_t pos = 0;
    while ((pos = body.find("\n    public ", pos)) != std::string::npos) {
        pos += 5;
        const size_t end = body.find_first_of("{=(\n", pos + 7);
        std::string member = body.substr(pos, end - pos);
        while (!member.empty() && member.back() == ' ') member.pop_back();
        out.push_back(member);
    }
    return out;
}

}  // namespace

// ===========================================================================
// BIND-8c: the gateway pair, `csharp_gateway` (D-BIND-109, D-BIND-115). Its golden is
// csharp_pair.fletcher.gateway.cs, held above with the others and compiled into
// Fletcher.GatewayClient.Tests, where it runs against an in-memory gateway.
// ===========================================================================

TEST(CsGateway, TheTokenIsParsedAndOtherTokensAreUnchanged) {
    for (const auto& [parameter, expected] : {std::pair<const char*, bool>{"", false},
                                              {"csharp", false},
                                              {"csharp_accessor", false},
                                              {"csharp,csharp_gateway", true},
                                              {"csharp_gateway,csharp", true}}) {
        fletcher::PluginOptions options;
        std::string error;
        ASSERT_TRUE(fletcher::ParsePluginParameter(parameter, &options, &error)) << error;
        EXPECT_EQ(options.csharp_gateway, expected) << "'" << parameter << "'";
        EXPECT_EQ(options.csharp_accessor, std::string(parameter) == "csharp_accessor")
            << "'" << parameter << "'";
    }
}

TEST(CsGateway, WithoutTheModelItIsRefusedBeforeAnyOutput) {
    // D-BIND-109: the pair converts with the model class, so the token alone is refused.
    const std::string source = "syntax = \"proto3\";\npackage a;\nmessage M { int32 x = 1; }\n";
    for (const std::string opts :
         {"csharp_gateway", "ts,csharp_gateway", "csharp_model_only,csharp_gateway"}) {
        std::map<std::string, std::string> files;
        std::string error;
        EXPECT_FALSE(GenerateSource(source, opts, &files, &error)) << opts;
        EXPECT_NE(error.find("csharp_gateway needs csharp"), std::string::npos) << error;
        EXPECT_NE(error.find("D-BIND-109"), std::string::npos) << error;
        EXPECT_TRUE(files.empty()) << opts << " wrote " << files.size() << " files";
    }
    for (const std::string opts :
         {"csharp,csharp_gateway", "csharp,csharp_model_only,csharp_gateway"}) {
        std::map<std::string, std::string> files;
        std::string error;
        EXPECT_TRUE(GenerateSource(source, opts, &files, &error)) << opts << ": " << error;
    }
}

TEST(CsGateway, TheTokenAddsOnlyItsOwnFileAndChangesNoOther) {
    DescriptorPool pool;
    const FileDescriptor* file = BuildPairFixture(pool);
    ASSERT_NE(file, nullptr);
    for (const std::string base : {"csharp", "csharp,csharp_model_only"}) {
        const auto without = GenerateWith(file, base);
        const auto with = GenerateWith(file, base + ",csharp_gateway");
        ASSERT_EQ(with.count("csharp_pair.fletcher.gateway.cs"), 1u) << base;
        EXPECT_EQ(with.size(), without.size() + 1) << base;
        for (const auto& [name, content] : without) {
            ASSERT_EQ(with.count(name), 1u) << name;
            EXPECT_EQ(with.at(name), content) << name << " changed under csharp_gateway";
        }
    }
}

TEST(CsGateway, ThePairBorrowsAGatewayClientAndNamesNothingNative) {
    // D-BIND-109: a gateway or WASM project builds it without Eiva.Fletcher; D-BIND-115: it
    // borrows the FletcherClient it is given. Skipped methods keep C++'s reason, as the
    // native pair's do.
    DescriptorPool pool;
    const FileDescriptor* file = BuildPairFixture(pool);
    ASSERT_NE(file, nullptr);
    const std::string gateway =
        FileOf(GenerateWith(file, "csharp,csharp_gateway"), "csharp_pair.fletcher.gateway.cs");
    for (size_t at = gateway.find("global::Eiva.Fletcher."); at != std::string::npos;
         at = gateway.find("global::Eiva.Fletcher.", at + 1)) {
        EXPECT_EQ(gateway.compare(at, 36, "global::Eiva.Fletcher.GatewayClient."), 0)
            << "names a native type: " << gateway.substr(at, 60);
    }
    ExpectContains(gateway,
                   "public delegate void Telemetry_ReportGatewayHandler(Reading row, "
                   "global::Eiva.Fletcher.GatewayClient.Envelope envelope);\n");
    ExpectContains(gateway, "public sealed class Telemetry_ReportGatewayPublisher\n");
    ExpectContains(gateway, "public sealed class Telemetry_ReportGatewaySubscriber\n");
    ExpectContains(gateway, "    public const string TopicKey = Telemetry_ReportTopic.Key;\n");
    ExpectContains(gateway,
                   "CreateAsync(global::Eiva.Fletcher.GatewayClient.FletcherClient client, ");
    ExpectContains(gateway, "await client.CreateTopicAsync(TopicKey, ");
    EXPECT_EQ(gateway.find("IDisposable"), std::string::npos) << "the pair borrows its client";
    ExpectContains(gateway, "// Skipped: Telemetry.Ping — ");
}

TEST(CsGateway, TwoTypesOfOneNameAreRefusedBeforeAnyOutput) {
    // Review of BIND-8 (D-BIND-100's stance): `S.M`'s gateway pair and `S.MGateway`'s native one
    // would both be `S_MGatewayPublisher`, and a message can be named like a pair type; both
    // were CS0101 in the consumer's build, and are refused at generation now.
    const std::string head =
        "syntax = \"proto3\";\npackage a;\nimport \"google/protobuf/empty.proto\";\n"
        "message Row { int32 x = 1; }\n";
    const std::string two_pairs = head +
                                  "service S { rpc M(stream Row) returns (google.protobuf.Empty); "
                                  "rpc MGateway(stream Row) returns (google.protobuf.Empty); }\n";
    {
        std::map<std::string, std::string> files;
        std::string error;
        EXPECT_FALSE(GenerateSource(two_pairs, "csharp,csharp_gateway", &files, &error));
        EXPECT_NE(error.find("'S_MGatewayPublisher'"), std::string::npos) << error;
        EXPECT_NE(error.find("D-BIND-100"), std::string::npos) << error;
        EXPECT_TRUE(files.empty()) << "wrote " << files.size() << " files";
    }
    {
        // Without the native pair, or without the gateway pair, the names do not meet.
        for (const std::string opts : {"csharp,csharp_model_only,csharp_gateway", "csharp"}) {
            std::map<std::string, std::string> files;
            std::string error;
            EXPECT_TRUE(GenerateSource(two_pairs, opts, &files, &error)) << opts << ": " << error;
        }
    }
    {
        const std::string message_like_a_topic =
            head +
            "message S_MTopic { int32 y = 1; }\n"
            "service S { rpc M(stream Row) returns (google.protobuf.Empty); }\n";
        std::map<std::string, std::string> files;
        std::string error;
        EXPECT_FALSE(
            GenerateSource(message_like_a_topic, "csharp,csharp_model_only", &files, &error));
        EXPECT_NE(error.find("'S_MTopic'"), std::string::npos) << error;
    }
}

TEST(CsAccessor, TheTokenIsParsedAndOtherTokensAreUnchanged) {
    for (const auto& [parameter, expected] : {std::pair<const char*, bool>{"", false},
                                              {"csharp", false},
                                              {"accessor", false},
                                              {"csharp,csharp_accessor", true},
                                              {"csharp_accessor,csharp", true}}) {
        fletcher::PluginOptions options;
        std::string error;
        ASSERT_TRUE(fletcher::ParsePluginParameter(parameter, &options, &error)) << error;
        EXPECT_EQ(options.csharp_accessor, expected) << "'" << parameter << "'";
        // The C++ accessor's token is its own: `accessor` never means the C# one, nor back.
        EXPECT_EQ(options.accessor, std::string(parameter) == "accessor")
            << "'" << parameter << "'";
    }
}

TEST(CsAccessor, WithoutTheModelItIsRefusedBeforeAnyOutput) {
    // D-BIND-99: the view's types and ToRow are the model's, so the token alone is refused.
    const std::string source = "syntax = \"proto3\";\npackage a;\nmessage M { int32 x = 1; }\n";
    for (const std::string opts :
         {"csharp_accessor", "ts,csharp_accessor", "csharp_model_only,csharp_accessor"}) {
        std::map<std::string, std::string> files;
        std::string error;
        EXPECT_FALSE(GenerateSource(source, opts, &files, &error)) << opts;
        EXPECT_NE(error.find("csharp_accessor needs csharp"), std::string::npos) << error;
        EXPECT_NE(error.find("D-BIND-99"), std::string::npos) << error;
        EXPECT_TRUE(files.empty()) << opts << " wrote " << files.size() << " files";
    }
    for (const std::string opts :
         {"csharp,csharp_accessor", "csharp,csharp_model_only,csharp_accessor"}) {
        std::map<std::string, std::string> files;
        std::string error;
        EXPECT_TRUE(GenerateSource(source, opts, &files, &error)) << opts << ": " << error;
    }
}

TEST(CsAccessor, AGeneratedNameThatIsAlsoADeclaredTypeIsRefused) {
    // D-BIND-100: two different messages, one generated name. Each is refused naming both,
    // before any output; without the token the same file generates.
    const std::pair<const char*, const char*> clashes[] = {
        {"message Track { int32 x = 1; }\nmessage TrackView { int32 y = 1; }\n",
         "message 'a.TrackView'"},
        {"message Track { int32 x = 1; }\nmessage TrackAccessor { int32 y = 1; }\n",
         "message 'a.TrackAccessor'"},
        {"message Track { int32 x = 1; }\nenum TrackView { TRACK_VIEW_UNSPECIFIED = 0; }\n",
         "enum 'a.TrackView'"},
        {"message Outer { message Inner { int32 x = 1; } Inner i = 1; }\n"
         "message Outer_InnerView { int32 y = 1; }\n",
         "message 'a.Outer_InnerView'"},
    };
    for (const auto& [body, named] : clashes) {
        const std::string source = std::string("syntax = \"proto3\";\npackage a;\n") + body;
        std::map<std::string, std::string> files;
        std::string error;
        EXPECT_FALSE(GenerateSource(source, "csharp,csharp_accessor", &files, &error)) << body;
        EXPECT_NE(error.find(named), std::string::npos) << error;
        EXPECT_NE(error.find("D-BIND-100"), std::string::npos) << error;
        EXPECT_TRUE(files.empty()) << body;
        EXPECT_TRUE(GenerateSource(source, "csharp", &files, &error)) << body << ": " << error;
    }
}

TEST(CsAccessor, AFieldNamedAfterItsOwnViewIsRefused) {
    // D-BIND-100: `Track.track_view` would be the property TrackView of the struct TrackView.
    const std::string source =
        "syntax = \"proto3\";\npackage a;\nmessage Track { int32 track_view = 1; }\n";
    std::map<std::string, std::string> files;
    std::string error;
    EXPECT_FALSE(GenerateSource(source, "csharp,csharp_accessor", &files, &error));
    EXPECT_NE(error.find("field 'track_view'"), std::string::npos) << error;
    EXPECT_NE(error.find("D-BIND-100"), std::string::npos) << error;
}

TEST(CsAccessor, TheAccessorHasOnlyItsFixedMembers) {
    // D-BIND-98: no per-field member on the accessor, so no field name can collide with one.
    const auto files = AccessorGolden();
    const std::string body =
        AccessorBody(FileOf(files, "csharp_accessor.fletcher.accessor.cs"), "Sample");
    ASSERT_FALSE(body.empty());
    const std::string dict =
        "global::System.Collections.Generic.IReadOnlyDictionary<string, string>";
    const std::vector<std::string> expected = {
        "public int Length",
        "public bool IsNull",
        "public SampleView At",
        "public Sample ToRow",
        "public " + dict + " SchemaMetadata",
        "public " + dict + " FieldMetadata",
        "public " + dict + " FieldMetadata",
        "public string? Metadata",
        "public static bool TryMake",
        "public static bool TryMake",
    };
    EXPECT_EQ(PublicMembers(body), expected) << body;
}

TEST(CsAccessor, TheViewHasTheModelsPropertiesWithTheirNamesAndTypes) {
    // D-BIND-98: one name per field across the model and the view, and the model's type, a
    // struct field being the struct's view (`?` where the model's is nullable).
    const auto files = AccessorGolden();
    const std::string model = ClassBody(FileOf(files, "csharp_accessor.fletcher.cs"), "Sample");
    const std::string view =
        StructBody(FileOf(files, "csharp_accessor.fletcher.arrow.cs"), "SampleView");
    ASSERT_FALSE(model.empty());
    ASSERT_FALSE(view.empty());
    std::vector<std::string> expected;
    for (auto p : PublicMembers(model)) {
        if (p.find(" static ") != std::string::npos) continue;  // Schema, ToArrow, FromArrow
        for (const std::string s : {"Position ", "Position? ", "Sample_Note "}) {
            if (p.rfind("public " + s, 0) != 0) continue;
            const size_t cut = s.find_first_of("? ");
            p = "public " + s.substr(0, cut) + "View" + p.substr(7 + cut);
            break;
        }
        expected.push_back(p);
    }
    // The span getters are the view's own (D-BIND-102), checked by their own case.
    auto members = PublicMembers(view);
    members.erase(std::remove_if(members.begin(), members.end(),
                                 [](const std::string& m) {
                                     return m.find("ReadOnlySpan<byte>") != std::string::npos;
                                 }),
                  members.end());
    EXPECT_EQ(members, expected) << view;
}

TEST(CsAccessor, AColumnIsValidatedAgainstTheModelsSchema) {
    // Positional and type-only, against the model class's own Schema, with the non-null gate on
    // non-nullable columns only (D-RBA-4), and a struct through its own accessor.
    const auto files = AccessorGolden();
    const std::string body =
        AccessorBody(FileOf(files, "csharp_accessor.fletcher.accessor.cs"), "Sample");
    ExpectContains(body,
                   "if (columns[0] is not global::Apache.Arrow.Int32Array c0 || "
                   "!SameType(c0.Data.DataType, "
                   "global::Fletcher.Gen.Golden.Accessor.Sample.Schema.GetFieldByIndex(0)."
                   "DataType))");
    ExpectContains(body, "error = $\"Sample column 0 'id': non-nullable, found {n0} nulls.\";");
    EXPECT_EQ(body.find("'maybe_id': non-nullable"), std::string::npos) << body;
    ExpectContains(body,
                   "if (!global::Fletcher.Gen.Golden.Accessor.PositionAccessor.TryMake(c12, out "
                   "var a12, out var e12))");
    ExpectContains(body, "a.Unit == e.Unit && a.Timezone == e.Timezone");
    EXPECT_EQ(body.find("Refusal"), std::string::npos) << "Sample reads every column";
}

TEST(CsAccessor, AColumnItCannotReadKeepsTheClassAndRefusesWithTheReason) {
    // D-BIND-96 and Q6: a list nested 4 deep is beyond the cap of 3, so Hyper's TryMake refuses
    // naming it, and the view says so where its property would be.
    const auto files = AccessorGolden();
    const std::string body =
        AccessorBody(FileOf(files, "csharp_accessor.fletcher.accessor.cs"), "Hyper");
    ExpectContains(body,
                   "private static readonly string? Refusal = \"Hyper column 1 'cells': a list "
                   "nested 4 deep, beyond the accessor's cap of 3.\";");
    ExpectContains(body, "if (Refusal is not null)");
    const std::string view =
        StructBody(FileOf(files, "csharp_accessor.fletcher.arrow.cs"), "HyperView");
    ExpectContains(view, "    public int Id => ");
    ExpectContains(view, "    // Not read: field 'cells' is a list nested 4 deep");
}

TEST(CsAccessor, ACollectionPropertyIsAWindowOverItsColumn) {
    // D-BIND-101: a list, a map and a list of lists read as borrowed windows over the cast
    // column, a struct element as its view (null on a null element), with the model's names.
    const auto files = AccessorGolden();
    const std::string view =
        StructBody(FileOf(files, "csharp_accessor.fletcher.arrow.cs"), "TrackView");
    const std::string m = "global::Eiva.Fletcher.Model.";
    ExpectContains(view, "    public " + m + "ListView<int> Ints => " + m +
                             "ListView<int>.Of(_accessor._c1, _row, _accessor._e1);\n");
    ExpectContains(view, "    public " + m + "ListView<PositionView?> Points => ");
    ExpectContains(view, "    public " + m + "ListView<Quality> Qualities => ");
    ExpectContains(view, "    public " + m + "MapView<string, int> Scores => " + m +
                             "MapView<string, int>.Of(_accessor._c8, _row, _accessor._k8, "
                             "_accessor._e8);\n");
    ExpectContains(view, "    public " + m + "MapView<string, PositionView?> Places => ");
    ExpectContains(view, "    public " + m + "MapView<long, string> Labels => ");
    ExpectContains(view, "    public " + m + "ListView<" + m + "ListView<PositionView?>> Grid => ");
    ExpectContains(view, "    public " + m + "ListView<" + m + "ListView<" + m +
                             "ListView<PositionView?>>> Cube => ");
    ExpectContains(view, "    public " + m + "ListView<" + m + "ListView<int>> Matrix => ");
    // G-Q4: nullable in its column, not in the model, so a null row reads as an empty window.
    ExpectContains(view, "    public " + m +
                             "ListView<PositionView?> MaybePoints => "
                             "_accessor._c14.IsNull(_row) ? default : ");
    EXPECT_EQ(view.find("// Not read"), std::string::npos) << view;
}

TEST(CsAccessor, AListAndAMapAreValidatedByTypeIdThenElementByElement) {
    // MapArray derives from ListArray, so the class alone would take a map for a list; every
    // element child is checked against the model Schema's element type, a struct element by its
    // own accessor.
    const auto files = AccessorGolden();
    const std::string body =
        AccessorBody(FileOf(files, "csharp_accessor.fletcher.accessor.cs"), "Track");
    ExpectContains(body,
                   "if (columns[1] is not global::Apache.Arrow.ListArray c1 || "
                   "c1.Data.DataType.TypeId != global::Apache.Arrow.Types.ArrowTypeId.List)");
    ExpectContains(body,
                   "if (columns[8] is not global::Apache.Arrow.MapArray c8 || "
                   "c8.Data.DataType.TypeId != global::Apache.Arrow.Types.ArrowTypeId.Map)");
    ExpectContains(body,
                   "if (c1.Values is not global::Apache.Arrow.Int32Array x1 || "
                   "!SameType(x1.Data.DataType, ((global::Apache.Arrow.Types.ListType)"
                   "global::Fletcher.Gen.Golden.Accessor.Track.Schema.GetFieldByIndex(1)."
                   "DataType).ValueDataType))");
    ExpectContains(body, "error = Mismatch(\"Track column 8 'scores' key\", ");
    ExpectContains(body,
                   "if (!global::Fletcher.Gen.Golden.Accessor.PositionAccessor.TryMake(x7, "
                   "out var a7, out var e7))");
    ExpectContains(body,
                   "var l11 = new global::Eiva.Fletcher.Model.ListElements<PositionView?>(x11, "
                   "l11_);");
    ExpectContains(body, "result._k8 = l8k;");
    EXPECT_EQ(body.find("Refusal"), std::string::npos) << "Track reads every column";
}

TEST(CsAccessor, ACollectionViewKeepsTheModelsPropertyNames) {
    // D-BIND-98 for names, D-BIND-101 for the types: one name per field across model and view.
    const auto files = AccessorGolden();
    const auto names = [](const std::vector<std::string>& members) {
        std::vector<std::string> out;
        for (const auto& member : members)
            if (member.find(" static ") == std::string::npos)
                out.push_back(member.substr(member.rfind(' ') + 1));
        return out;
    };
    const std::string model = ClassBody(FileOf(files, "csharp_accessor.fletcher.cs"), "Track");
    const std::string view =
        StructBody(FileOf(files, "csharp_accessor.fletcher.arrow.cs"), "TrackView");
    ASSERT_FALSE(model.empty());
    EXPECT_EQ(names(PublicMembers(view)), names(PublicMembers(model))) << view;
}

TEST(CsAccessor, ToRowIsTheModelsFromArrowOverTheAccessorsColumns) {
    // D-BIND-102: one read path for a model row. The columns are the StructArray source itself
    // or a batch's columns wrapped, copying nothing.
    const auto files = AccessorGolden();
    const std::string body =
        AccessorBody(FileOf(files, "csharp_accessor.fletcher.accessor.cs"), "Sample");
    ExpectContains(body,
                   "    public Sample ToRow(int row)\n    {\n        CheckRow(row);\n"
                   "        return global::Fletcher.Gen.Golden.Accessor.Sample.FromArrow(_rows, "
                   "row);\n    }\n");
    ExpectContains(body,
                   "validity ?? new global::Apache.Arrow.StructArray(new "
                   "global::Apache.Arrow.Types.StructType(fields), length,\n"
                   "                columns, global::Apache.Arrow.ArrowBuffer.Empty, 0));\n");
}

TEST(CsAccessor, AStringOrBytesFieldHasASpanGetterNamedAfterEveryProperty) {
    // D-BIND-102: `<Property>Utf8` for a string, `<Property>Span` for bytes, after every model
    // property by the D-BIND-89 taken set, so a field `name_utf8` keeps the name NameUtf8.
    const std::string source =
        "syntax = \"proto3\";\npackage a;\n"
        "message M { string name = 1; string name_utf8 = 2; bytes data = 3; optional string note = "
        "4; }\n";
    std::map<std::string, std::string> files;
    std::string error;
    ASSERT_TRUE(GenerateSource(source, "csharp,csharp_accessor", &files, &error)) << error;
    const std::string view = StructBody(FileOf(files, "accessor_case.fletcher.arrow.cs"), "MView");
    const std::string span = "    public global::System.ReadOnlySpan<byte> ";
    ExpectContains(view, "    public string Name => ");
    ExpectContains(view, "    public string NameUtf8 => _accessor._c1.GetString(_row) ?? \"\";\n");
    ExpectContains(view, span + "NameUtf8_ => _accessor._c0.GetBytes(_row);\n");
    ExpectContains(view, span + "NameUtf8Utf8 => _accessor._c1.GetBytes(_row);\n");
    ExpectContains(view, span + "DataSpan => _accessor._c2.GetBytes(_row);\n");
    // A nullable one too: Apache.Arrow's GetBytes reads a null as empty (a runtime test pins it).
    ExpectContains(view, span + "NoteUtf8 => _accessor._c3.GetBytes(_row);\n");
}

TEST(CsAccessor, FromArrowReadsANullListAsEmptyAtEveryLevel) {
    // B1 of BIND-7's review: Arrow lets a null list's offsets span elements, so the model's
    // FromArrow (and ToRow, which is it) reads a null list, map or inner list as empty, as the
    // view does (D-BIND-101), instead of the elements its offsets span.
    const auto files = AccessorGolden();
    const std::string track = ClassBody(FileOf(files, "csharp_accessor.fletcher.cs"), "Track");
    ExpectContains(track, "var end = l.IsNull(index) ? start : l.ValueOffsets[index + 1];");
    ExpectContains(track, "var end = m.IsNull(index) ? start : m.ValueOffsets[index + 1];");
    ExpectContains(track, "(l.IsNull(index) ? l.ValueOffsets[index] : l.ValueOffsets[index + 1])");
    ExpectContains(track, "(e0.IsNull(j0) ? e0.ValueOffsets[j0] : e0.ValueOffsets[j0 + 1])");
}

namespace {

// Generates `main` (accessor_case.proto, importing dep.proto) with `opts`, `dep` built first.
bool GenerateWithImport(const std::string& dep, const std::string& main, const std::string& opts,
                        std::string* error) {
    DescriptorPool pool;
    if (BuildSource(pool, "dep.proto", dep) == nullptr) {
        ADD_FAILURE() << "dep.proto does not build";
        return false;
    }
    const FileDescriptor* file = BuildSource(pool, "accessor_case.proto", main);
    if (file == nullptr) {
        ADD_FAILURE() << "accessor_case.proto does not build";
        return false;
    }
    MemoryContext ctx;
    return fletcher::ArrowRowGenerator().Generate(file, opts, &ctx, error);
}

}  // namespace

TEST(CsAccessor, AClashWithAnImportedFilesNamesIsRefusedBothWays) {
    // B2 of BIND-7's review: D-BIND-100 leaves only UNIMPORTED files to the compiler, so an
    // imported file's generated view clashing with a message here is refused, and the reverse.
    const std::string header = "syntax = \"proto3\";\npackage a;\n";
    const std::string import = "import \"dep.proto\";\n";
    std::string error;
    EXPECT_FALSE(GenerateWithImport(header + "message A { int32 x = 1; }\n",
                                    header + import + "message AView { int32 y = 1; }\n",
                                    "csharp,csharp_accessor", &error));
    EXPECT_NE(error.find("message 'a.A' would get the type 'AView'"), std::string::npos) << error;
    EXPECT_NE(error.find("message 'a.AView'"), std::string::npos) << error;
    error.clear();
    EXPECT_FALSE(GenerateWithImport(header + "message AView { int32 y = 1; }\n",
                                    header + import + "message A { int32 x = 1; }\n",
                                    "csharp,csharp_accessor", &error));
    EXPECT_NE(error.find("D-BIND-100"), std::string::npos) << error;
    // Another package is another namespace: no clash.
    error.clear();
    EXPECT_TRUE(GenerateWithImport("syntax = \"proto3\";\npackage b;\nmessage A { int32 x = 1; }\n",
                                   header + import + "message AView { int32 y = 1; }\n",
                                   "csharp,csharp_accessor", &error))
        << error;
}

TEST(CsAccessor, AFieldWithoutAGetterIsNotRefusedForItsName) {
    // P-D1 of BIND-7's review: a field the view has no property for (a list beyond the cap)
    // cannot clash with the view's name, so it is not refused.
    const std::string source =
        "syntax = \"proto3\";\npackage a;\nimport \"fletcher/options.proto\";\n"
        "message P { int32 x = 1; }\n"
        "message W1 { option (fletcher.flatten) = true; repeated P items = 1; }\n"
        "message W2 { option (fletcher.flatten) = true; repeated W1 rows = 1; }\n"
        "message W3 { option (fletcher.flatten) = true; repeated W2 planes = 1; }\n"
        "message Track { repeated W3 track_view = 1; }\n";
    std::map<std::string, std::string> files;
    std::string error;
    ASSERT_TRUE(GenerateSource(source, "csharp,csharp_accessor", &files, &error)) << error;
    const std::string view =
        StructBody(FileOf(files, "accessor_case.fletcher.arrow.cs"), "TrackView");
    ExpectContains(view, "    // Not read: field 'track_view' is a list nested 4 deep");
}

TEST(CsAccessor, AMessageReachingOneThatRefusesRefusesUpFront) {
    // T-D1 of BIND-7's review, D-BIND-96: HyperHolder reaches Hyper, whose accessor always
    // refuses, so HyperHolder's refuses before reading anything, naming Hyper's reason, and its
    // view has a comment, not a property, for each such field.
    const auto files = AccessorGolden();
    const std::string body =
        AccessorBody(FileOf(files, "csharp_accessor.fletcher.accessor.cs"), "HyperHolder");
    ExpectContains(body,
                   "Refusal = \"HyperHolder column 1 'hyper': a message whose accessor refuses "
                   "(Hyper column 1 'cells': a list nested 4 deep, beyond the accessor's cap of "
                   "3).\";");
    const std::string view =
        StructBody(FileOf(files, "csharp_accessor.fletcher.arrow.cs"), "HyperHolderView");
    ExpectContains(view, "    // Not read: field 'hyper' is a message whose accessor refuses");
    ExpectContains(view, "    // Not read: field 'hypers' is a message whose accessor refuses");
    EXPECT_EQ(view.find("HyperView"), std::string::npos) << view;
}

TEST(CsAccessor, TheNonNullGateCountsOnlyUnderValidParentsAndMalformedInputIsRefused) {
    // D-BIND-106 and R-D1 of BIND-7's review, in the emitted code.
    const auto files = AccessorGolden();
    const std::string body =
        AccessorBody(FileOf(files, "csharp_accessor.fletcher.accessor.cs"), "Sample");
    ExpectContains(body, "var n0 = NullsUnderValidRows(c0, validity);");
    ExpectContains(body, "if (column.IsNull(i) && !parent.IsNull(i)) nulls++;");
    ExpectContains(body,
                   "catch (global::System.Exception exception) when "
                   "(IsMalformedInput(exception))");
    ExpectContains(body, "column {i}: the array is null.");
}
