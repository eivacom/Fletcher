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

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>

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
// message Holder {   // a message field and a repeated one, of generated classes
//   Player.Stats best = 1;  repeated Player.Stats history = 2;  map<string, Player.Stats> by_name =
//   3;
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

TEST(CsVisitor, MessageFieldsAreNullableReferencesToTheGeneratedClass) {
    // A message field can be absent, and null is how C# says so: the same fact the
    // schema records as a nullable struct column.
    DescriptorPool pool;
    const FileDescriptor* file = BuildFixture(pool);
    ASSERT_NE(file, nullptr);
    const std::string cs = CSharp(file);
    ExpectContains(cs, "    public Player_Stats? Stats { get; set; }\n");
    ExpectContains(cs, "    public Player_Stats? Best { get; set; }\n");
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
    ExpectContains(cs, "    public global::Eiva.Fletcher.Model.Timestamp At { get; set; }\n");
    ExpectContains(cs, "    public global::Eiva.Fletcher.Model.Duration Took { get; set; }\n");
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

TEST(CsVisitor, AMessageFromAnotherFileHasNoClassYetSoItsFieldIsAMarker) {
    // `repeated google.protobuf.Timestamp` is mapped by the IR as a list of the struct
    // seconds/nanos, and that struct is declared in timestamp.proto, not in this file.
    // Naming it would emit a type that does not exist, so the field is a comment that
    // names BIND-6e (cross-file) and ToArrow / FromArrow wait on it, as for any field
    // conversion cannot yet carry.
    DescriptorPool pool;
    const FileDescriptor* file = BuildFixture(pool);
    ASSERT_NE(file, nullptr);
    const std::string timed = ClassBody(CSharp(file), "Timed");
    ExpectContains(
        timed,
        "    // Not generated yet: field 'marks' (list) has no C# type here: a message from "
        "another file (BIND-6e).\n");
    EXPECT_EQ(timed.find(" Marks "), std::string::npos) << timed;
}

TEST(CsVisitor, EveryOtherFieldGetsAPropertyEvenWhereConversionWaits) {
    // 6c's first step gives every field whose type is generated here its property;
    // ToArrow / FromArrow for the composite ones follow. Only the cross-file field above
    // is a comment.
    DescriptorPool pool;
    const FileDescriptor* file = BuildFixture(pool);
    ASSERT_NE(file, nullptr);
    const std::string cs = CSharp(file);
    size_t markers = 0;
    for (size_t at = cs.find("// Not generated yet:"); at != std::string::npos;
         at = cs.find("// Not generated yet:", at + 1))
        ++markers;
    EXPECT_EQ(markers, 1u) << cs;
    EXPECT_EQ(cs.find("Not generated yet (BIND-6c): field"), std::string::npos) << cs;
}

TEST(CsVisitor, CsharpTokenChangesNoExistingOutputAndAddsOnlyItsOwnFiles) {
    // `csharp` adds the model and the native pair's file (D-BIND-76), and
    // `csharp,csharp_model_only` the model alone; neither changes another output.
    DescriptorPool pool;
    const FileDescriptor* file = BuildFixture(pool);
    ASSERT_NE(file, nullptr);
    for (const std::string base : {"", "ts", "ts,ipc", "schema_only"}) {
        const auto without = GenerateWith(file, base);
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
    ExpectContains(sample,
                   "        row.Flag = ((global::Apache.Arrow.BooleanArray)array.Fields[0])"
                   ".GetValue(index).GetValueOrDefault();\n");
    ExpectContains(sample,
                   "        row.Name = ((global::Apache.Arrow.StringArray)array.Fields[6])"
                   ".GetString(index) ?? \"\";\n");
    ExpectContains(sample,
                   "        row.Note = ((global::Apache.Arrow.StringArray)array.Fields[7])"
                   ".GetString(index);\n");
    ExpectContains(
        sample,
        "        { var a = (global::Apache.Arrow.BinaryArray)array.Fields[8]; row.Blob = "
        "a.IsNull(index) ? global::System.Array.Empty<byte>() : "
        "a.GetBytes(index).ToArray(); }\n");
    ExpectContains(sample,
                   "        row.Color = (Color)((global::Apache.Arrow.Int32Array)array.Fields[9])"
                   ".GetValue(index).GetValueOrDefault();\n");
    ExpectContains(sample,
                   "        row.Tint = ((global::Apache.Arrow.Int32Array)array.Fields[10])"
                   ".GetValue(index) is int v10 ? (Color)v10 : null;\n");
    ExpectContains(sample,
                   "        row.Maybe = ((global::Apache.Arrow.Int32Array)array.Fields[11])"
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
    // Stats stats = 12 (column 11): an absent message keeps its slot as a default
    // instance and a cleared validity bit; its children come from its own class.
    ExpectContains(player,
                   "            else { r11.Add(new Player_Stats()); m11.Append(false); }\n");
    ExpectContains(player,
                   "        var c11 = new global::Apache.Arrow.StructArray(t11, rows.Count, "
                   "Player_Stats.ToArrowColumns(r11), z11 > 0 ? m11.Build() : "
                   "global::Apache.Arrow.ArrowBuffer.Empty, z11, 0);\n");
    // map<string, int32> tags = 13 (column 12): keys and values in entry order.
    ExpectContains(player, "                k12.Append(x12.Key);\n");
    ExpectContains(player, "                b12.Append(x12.Value);\n");
    ExpectContains(
        player,
        "        var c12 = new global::Apache.Arrow.MapArray(t12, rows.Count, o12.Build(), "
        "kv12, global::Apache.Arrow.ArrowBuffer.Empty, 0, 0);\n");
    // Reading back: the list's slice, the struct's validity, the map's pairs.
    ExpectContains(
        player,
        "        { var s = (global::Apache.Arrow.StructArray)array.Fields[11]; row.Stats = "
        "s.IsNull(index) ? null : Player_Stats.FromArrow(s, index); }\n");
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
    ExpectContains(
        holder,
        "        var e1 = new global::Apache.Arrow.StructArray("
        "(global::Apache.Arrow.Types.StructType)t1.ValueDataType, f1.Count, "
        "Player_Stats.ToArrowColumns(f1), global::Apache.Arrow.ArrowBuffer.Empty, 0, 0);\n");
    ExpectContains(holder, "                f2.Add(x2.Value);\n");
    ExpectContains(
        holder,
        "x.Add(new global::System.Collections.Generic.KeyValuePair<string, Player_Stats>("
        "k.GetString(j) ?? \"\", Player_Stats.FromArrow(e, j)));\n");
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

TEST(CsVisitor, AFieldWithoutAClassBlocksConversionAndSaysWhy) {
    // A ToArrow that skipped 'marks' would build a batch disagreeing with its own Schema,
    // so the message gets its Schema and a marker naming the field and the reason.
    DescriptorPool pool;
    const FileDescriptor* file = BuildFixture(pool);
    ASSERT_NE(file, nullptr);
    const std::string timed = ClassBody(CSharp(file), "Timed");
    ExpectContains(timed, "    public static global::Apache.Arrow.Schema Schema { get; }");
    ExpectContains(
        timed,
        "    // ToArrow and FromArrow are not generated: field 'marks' is a message from "
        "another file (BIND-6e).\n");
    EXPECT_EQ(timed.find("ToArrow("), std::string::npos) << timed;
    EXPECT_EQ(timed.find("FromArrow("), std::string::npos) << timed;
}

TEST(CsVisitor, AMessageEmbeddingABlockedOneIsBlockedToo) {
    // Wraps embeds Timed, whose own conversion waits; converting Wraps would need Timed's
    // ToArrowColumns, which does not exist, so the block propagates and says so.
    DescriptorPool pool;
    const FileDescriptor* file = BuildFixture(pool);
    ASSERT_NE(file, nullptr);
    const std::string wraps = ClassBody(CSharp(file), "Wraps");
    ExpectContains(
        wraps,
        "    // ToArrow and FromArrow are not generated: field 'timed' is a message whose "
        "own conversion waits.\n");
    EXPECT_EQ(wraps.find("ToArrowColumns("), std::string::npos) << wraps;
}

TEST(CsVisitor, EveryConvertibleMessageGetsBothHalves) {
    // The other side of the two cases above: no message whose fields all convert is left
    // without ToArrow, ToArrowColumns and FromArrow.
    DescriptorPool pool;
    const FileDescriptor* file = BuildFixture(pool);
    ASSERT_NE(file, nullptr);
    const std::string cs = CSharp(file);
    for (const char* name : {"Player", "Player_Stats", "Sample", "Holder", "Clock", "Nested"}) {
        const std::string body = ClassBody(cs, name);
        EXPECT_NE(body.find(" ToArrow("), std::string::npos) << name;
        EXPECT_NE(body.find(" ToArrowColumns("), std::string::npos) << name;
        EXPECT_NE(body.find(" FromArrow("), std::string::npos) << name;
    }
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

// Parses tests/golden/csharp_pair.proto into `pool`, with the well-known files it
// imports copied in first.
const FileDescriptor* BuildPairFixture(DescriptorPool& pool) {
    const FileDescriptor* wkt_files[] = {
        google::protobuf::Timestamp::GetDescriptor()->file(),
        google::protobuf::Empty::GetDescriptor()->file(),
    };
    for (const FileDescriptor* wkt : wkt_files) {
        if (pool.FindFileByName(wkt->name()) != nullptr) continue;
        FileDescriptorProto copy;
        wkt->CopyTo(&copy);
        if (pool.BuildFile(copy) == nullptr) return nullptr;
    }
    const std::string source = ReadText(GoldenDir() / "csharp_pair.proto");
    if (source.empty()) {
        ADD_FAILURE() << "cannot read " << (GoldenDir() / "csharp_pair.proto");
        return nullptr;
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
    fdp.set_name("csharp_pair.proto");
    return pool.BuildFile(fdp);
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
                   "                _publisher.CreateTopic(Topic, Reading.Schema, options);\n");
    // D-BIND-78: per-row and synchronous, with the price where it is chosen.
    ExpectContains(pub, "about 3.8 microseconds a row, 19.6 times the generated C++ publisher");
    ExpectContains(pub, "        using var batch = Reading.ToArrow(new[] { row });\n");
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
    ExpectContains(native, "            handler(Reading.FromArrow(columns, 0), attachments);\n");
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
    ExpectContains(
        native,
        "// Skipped: Telemetry.Mark \xE2\x80\x94 Marks has no ToArrow / FromArrow: field "
        "'at' is a message from another file (BIND-6e).\n");
    EXPECT_EQ(native.find("Telemetry_MarkPublisher"), std::string::npos) << native;
}

TEST(CsVisitorPair, TheCommittedGoldensAreThePluginsOutput) {
    // Fletcher.Tests compiles these goldens; a byte of drift fails HERE, so the C# that
    // runs against the native runtime is always what the plugin emits today.
    // FLETCHER_CAPTURE_GOLDENS=1 rewrites them for review as source.
    DescriptorPool pool;
    const FileDescriptor* file = BuildPairFixture(pool);
    ASSERT_NE(file, nullptr);
    const auto files = GenerateWith(file, "csharp");
    for (const char* name : {"csharp_pair.fletcher.cs", "csharp_pair.fletcher.native.cs"}) {
        const std::string generated = FileOf(files, name);
        const auto path = GoldenDir() / name;
        if (const char* capture = std::getenv("FLETCHER_CAPTURE_GOLDENS");
            capture != nullptr && std::string(capture) == "1") {
            std::ofstream(path, std::ios::binary) << generated;
            continue;
        }
        EXPECT_EQ(ReadText(path), generated) << name << " differs from the plugin's output";
    }
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
    ExpectContains(nested, "Player_Stats.ToArrowColumns(f0_)");
    ExpectContains(nested, "Player_Stats.ToArrowColumns(f1__)");  // three levels deep
    ExpectContains(nested, "        var c0 = a0;\n");
    ExpectContains(nested, "x0.Add(Player_Stats.FromArrow(e0_, j0_));\n");
    ExpectContains(nested, "            x.Add(x0);\n");
    EXPECT_NE(nested.find(" ToArrow("), std::string::npos) << nested;
    EXPECT_NE(nested.find(" FromArrow("), std::string::npos) << nested;
}
