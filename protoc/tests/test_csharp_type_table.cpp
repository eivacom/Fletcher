// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// BIND-6a: the C# backend lookup table and its naming rules.
//
// D-BIND-69, D-BIND-70 and D-BIND-73 adopt protoc's own C# naming rules, read from
// its source (compiler/csharp/names.cc, csharp_helpers.cc, csharp_enum.cc). These
// cases pin each rule as protoc implements it, so a reimplementation that is merely
// plausible fails here rather than in a consumer's build. The one deliberate
// difference, the generated class's own members joining the reserved list, has its
// own case.

#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <gtest/gtest.h>

#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "csharp_backend_type_table.hpp"

using namespace google::protobuf;
namespace cs = fletcher::csharp_backend;

namespace {

const FileDescriptor* BuildFile(DescriptorPool& pool, const std::string& package,
                                const std::vector<std::string>& field_names) {
    FileDescriptorProto fdp;
    fdp.set_name("names.proto");
    fdp.set_package(package);
    fdp.set_syntax("proto3");
    auto* m = fdp.add_message_type();
    m->set_name("Player");
    int number = 1;
    for (const auto& name : field_names) {
        auto* f = m->add_field();
        f->set_name(name);
        f->set_number(number++);
        f->set_type(FieldDescriptorProto::TYPE_INT32);
        f->set_label(FieldDescriptorProto::LABEL_OPTIONAL);
    }
    return pool.BuildFile(fdp);
}

const EnumDescriptor* BuildEnum(DescriptorPool& pool, const std::string& name,
                                const std::vector<std::pair<std::string, int>>& values,
                                bool allow_alias = false) {
    FileDescriptorProto fdp;
    fdp.set_name("enum_" + name + ".proto");
    fdp.set_package("e");
    fdp.set_syntax("proto3");
    auto* e = fdp.add_enum_type();
    e->set_name(name);
    if (allow_alias) e->mutable_options()->set_allow_alias(true);
    for (const auto& [value_name, number] : values) {
        auto* v = e->add_value();
        v->set_name(value_name);
        v->set_number(number);
    }
    const FileDescriptor* file = pool.BuildFile(fdp);
    return file ? file->enum_type(0) : nullptr;
}

std::vector<std::pair<std::string, int>> Members(const EnumDescriptor* e) {
    std::vector<std::pair<std::string, int>> out;
    for (const auto& m : cs::CsEnumMembers(e)) out.emplace_back(m.name, m.number);
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// UnderscoresToCamelCase / UnderscoresToPascalCase (names.cc)
// ---------------------------------------------------------------------------

TEST(CsNames, PascalCaseFollowsProtocUnderscoresToCamelCase) {
    EXPECT_EQ(cs::UnderscoresToCamelCase("player_id", true, false), "PlayerId");
    EXPECT_EQ(cs::UnderscoresToCamelCase("timestamp_ns", true, false), "TimestampNs");
    // A letter after a digit is capitalised; the digit itself is kept.
    EXPECT_EQ(cs::UnderscoresToCamelCase("field2_x", true, false), "Field2X");
    EXPECT_EQ(cs::UnderscoresToCamelCase("abc2def", true, false), "Abc2Def");
    // Upper-case letters are kept as written; underscores vanish.
    EXPECT_EQ(cs::UnderscoresToCamelCase("HTTPServer", true, false), "HTTPServer");
    EXPECT_EQ(cs::UnderscoresToCamelCase("__x__y", true, false), "XY");
    // A result starting with a digit, from a name starting '_', gets a leading '_'.
    EXPECT_EQ(cs::UnderscoresToCamelCase("_2d", true, false), "_2D");
}

TEST(CsNames, NamespaceIsFletcherGenPlusPascalPackageKeepingDots) {
    DescriptorPool pool;
    EXPECT_EQ(cs::CsNamespace(BuildFile(pool, "eiva.nav_v1", {"a"})), "Fletcher.Gen.Eiva.NavV1");
    DescriptorPool pool2;
    EXPECT_EQ(cs::CsNamespace(BuildFile(pool2, "test", {"a"})), "Fletcher.Gen.Test");
    DescriptorPool pool3;
    EXPECT_EQ(cs::CsNamespace(BuildFile(pool3, "", {"a"})), "Fletcher.Gen");
}

TEST(CsNames, CsharpNamespaceOptionIsIgnored) {
    FileDescriptorProto fdp;
    fdp.set_name("opt.proto");
    fdp.set_package("integration");
    fdp.set_syntax("proto3");
    fdp.mutable_options()->set_csharp_namespace("Eiva.Integration");
    DescriptorPool pool;
    const FileDescriptor* file = pool.BuildFile(fdp);
    ASSERT_NE(file, nullptr);
    EXPECT_EQ(cs::CsNamespace(file), "Fletcher.Gen.Integration");
}

// ---------------------------------------------------------------------------
// Property names (GetPropertyName, csharp_helpers.cc, plus D-BIND-73)
// ---------------------------------------------------------------------------

TEST(CsNames, PropertyIsPascalCaseOfTheFieldName) {
    DescriptorPool pool;
    const FileDescriptor* f = BuildFile(pool, "p", {"player_id", "class", "event", "lock"});
    ASSERT_NE(f, nullptr);
    const Descriptor* m = f->message_type(0);
    EXPECT_EQ(cs::CsPropertyName("player_id", m), "PlayerId");
    // C# keywords are lower case, so PascalCase alone makes them legal.
    EXPECT_EQ(cs::CsPropertyName("class", m), "Class");
    EXPECT_EQ(cs::CsPropertyName("event", m), "Event");
    EXPECT_EQ(cs::CsPropertyName("lock", m), "Lock");
}

TEST(CsNames, PropertyNamedAfterItsClassIsSuffixed) {
    DescriptorPool pool;
    const FileDescriptor* f = BuildFile(pool, "p", {"player"});
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(cs::CsPropertyName("player", f->message_type(0)), "Player_");
}

TEST(CsNames, ProtocReservedMemberNamesAreSuffixed) {
    DescriptorPool pool;
    const FileDescriptor* f = BuildFile(pool, "p", {"a"});
    ASSERT_NE(f, nullptr);
    const Descriptor* m = f->message_type(0);
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"types", "Types_"},
        {"descriptor", "Descriptor_"},
        {"equals", "Equals_"},
        {"to_string", "ToString_"},
        {"get_hash_code", "GetHashCode_"},
        {"write_to", "WriteTo_"},
        {"clone", "Clone_"},
        {"calculate_size", "CalculateSize_"},
        {"merge_from", "MergeFrom_"},
        {"on_construction", "OnConstruction_"},
        {"parser", "Parser_"},
    };
    for (const auto& [field, property] : cases)
        EXPECT_EQ(cs::CsPropertyName(field, m), property) << "field '" << field << "'";
}

TEST(CsNames, TheGeneratedClassOwnMembersAreSuffixed) {
    // D-BIND-73 point 3: the one deliberate addition to protoc's list.
    DescriptorPool pool;
    const FileDescriptor* f = BuildFile(pool, "p", {"a"});
    ASSERT_NE(f, nullptr);
    const Descriptor* m = f->message_type(0);
    EXPECT_EQ(cs::CsPropertyName("schema", m), "Schema_");
    EXPECT_EQ(cs::CsPropertyName("to_arrow", m), "ToArrow_");
    EXPECT_EQ(cs::CsPropertyName("from_arrow", m), "FromArrow_");
    // BIND-6c-2 added ToArrowColumns, the columns a message embedding this one reuses.
    EXPECT_EQ(cs::CsPropertyName("to_arrow_columns", m), "ToArrowColumns_");
    // A name merely CONTAINING a reserved word is left alone.
    EXPECT_EQ(cs::CsPropertyName("schema_version", m), "SchemaVersion");
}

TEST(CsNames, PropertiesOfOneClassAreMadeUniqueInFieldOrder) {
    // D-BIND-89 (a): names that PascalCase alike are suffixed `_` until unique, the earlier field
    // keeping the plain name; a suffix never lands on the class's own name (`Player_` is taken by
    // the field `player`'s property here, so `player_` steps past it). The names are passed as
    // strings, as proto3 itself would refuse such a message.
    DescriptorPool pool;
    const FileDescriptor* f = BuildFile(pool, "p", {"a"});
    ASSERT_NE(f, nullptr);
    const Descriptor* m = f->message_type(0);
    std::set<std::string> taken;
    EXPECT_EQ(cs::CsUniquePropertyName("foo_bar", m, taken), "FooBar");
    EXPECT_EQ(cs::CsUniquePropertyName("fooBar", m, taken), "FooBar_");
    EXPECT_EQ(cs::CsUniquePropertyName("FooBar", m, taken), "FooBar__");
    EXPECT_EQ(cs::CsUniquePropertyName("player", m, taken), "Player_");
    EXPECT_EQ(cs::CsUniquePropertyName("player_", m, taken), "Player__");
    EXPECT_EQ(cs::CsUniquePropertyName("other", m, taken), "Other");
    EXPECT_EQ(taken, (std::set<std::string>{"FooBar", "FooBar_", "FooBar__", "Player_", "Player__",
                                            "Other"}));
}

TEST(CsNames, ALowerCaseOnlyTypeNameIsWrittenVerbatim) {
    // BIND-6 review B2: a type name of lower-case ASCII letters only is `@name` in source, so a
    // C# keyword compiles and no other such name draws CS8981. The compiled name has no `@`.
    // Anything with a capital, a digit or `_` is left alone.
    FileDescriptorProto fdp;
    fdp.set_name("verbatim.proto");
    fdp.set_package("v");
    fdp.set_syntax("proto3");
    for (const char* name : {"row", "class", "Row", "row2", "outer"}) {
        auto* m = fdp.add_message_type();
        m->set_name(name);
    }
    fdp.mutable_message_type(4)->add_nested_type()->set_name("inner");
    auto* e = fdp.add_enum_type();
    e->set_name("level");
    e->add_value()->set_name("LEVEL_LOW");
    DescriptorPool pool;
    const FileDescriptor* file = pool.BuildFile(fdp);
    ASSERT_NE(file, nullptr);
    EXPECT_EQ(cs::CsTypeName(file->message_type(0)), "@row");
    EXPECT_EQ(cs::CsTypeName(file->message_type(1)), "@class");
    EXPECT_EQ(cs::CsTypeName(file->message_type(2)), "Row");
    EXPECT_EQ(cs::CsTypeName(file->message_type(3)), "row2");
    EXPECT_EQ(cs::CsTypeName(file->message_type(4)->nested_type(0)), "outer_inner");
    EXPECT_EQ(cs::CsTypeName(file->enum_type(0)), "@level");
    EXPECT_EQ(cs::CsTypeExpr(file->message_type(1)), "global::Fletcher.Gen.V.@class");
}

TEST(CsNames, AMessageNamedAfterAGeneratedMemberIsSuffixed) {
    // D-BIND-84: `message Schema` would declare a class with a static member `Schema` of its own
    // name (CS0542), so the class is `Schema_`; likewise ToArrow, FromArrow and ToArrowColumns. An
    // enum has no such members and keeps its name; a nested `Outer.Schema` is `Outer_Schema`,
    // which cannot clash. A field `schema` of `Schema_` would become property `Schema_` (reserved,
    // D-BIND-73) and so meet the class name: it steps past it, `Schema__`.
    FileDescriptorProto fdp;
    fdp.set_name("own.proto");
    fdp.set_package("own");
    fdp.set_syntax("proto3");
    for (const char* name : {"Schema", "ToArrow", "FromArrow", "ToArrowColumns", "Outer"}) {
        auto* m = fdp.add_message_type();
        m->set_name(name);
    }
    auto* schema_field = fdp.mutable_message_type(0)->add_field();
    schema_field->set_name("schema");
    schema_field->set_number(1);
    schema_field->set_type(FieldDescriptorProto::TYPE_INT32);
    schema_field->set_label(FieldDescriptorProto::LABEL_OPTIONAL);
    fdp.mutable_message_type(4)->add_nested_type()->set_name("Schema");
    DescriptorPool pool;
    const FileDescriptor* file = pool.BuildFile(fdp);
    ASSERT_NE(file, nullptr);
    FileDescriptorProto enums;
    enums.set_name("own_enum.proto");
    enums.set_package("own_enum");
    enums.set_syntax("proto3");
    auto* e = enums.add_enum_type();
    e->set_name("Schema");
    e->add_value()->set_name("SCHEMA_UNSPECIFIED");
    const FileDescriptor* enum_file = pool.BuildFile(enums);
    ASSERT_NE(enum_file, nullptr);
    EXPECT_EQ(cs::CsTypeName(file->message_type(0)), "Schema_");
    EXPECT_EQ(cs::CsTypeName(file->message_type(1)), "ToArrow_");
    EXPECT_EQ(cs::CsTypeName(file->message_type(2)), "FromArrow_");
    EXPECT_EQ(cs::CsTypeName(file->message_type(3)), "ToArrowColumns_");
    EXPECT_EQ(cs::CsTypeName(file->message_type(4)->nested_type(0)), "Outer_Schema");
    EXPECT_EQ(cs::CsTypeExpr(file->message_type(0)), "global::Fletcher.Gen.Own.Schema_");
    EXPECT_EQ(cs::CsPropertyName("schema", file->message_type(0)), "Schema__");
    EXPECT_EQ(cs::CsTypeName(enum_file->enum_type(0)), "Schema");
}

TEST(CsNames, NestedTypesAreFlattenedWithUnderscores) {
    FileDescriptorProto fdp;
    fdp.set_name("nested.proto");
    fdp.set_package("n");
    fdp.set_syntax("proto3");
    auto* outer = fdp.add_message_type();
    outer->set_name("Outer");
    auto* inner = outer->add_nested_type();
    inner->set_name("Inner");
    inner->add_nested_type()->set_name("Deep");
    auto* e = inner->add_enum_type();
    e->set_name("Color");
    e->add_value()->set_name("COLOR_UNSPECIFIED");
    DescriptorPool pool;
    const FileDescriptor* file = pool.BuildFile(fdp);
    ASSERT_NE(file, nullptr);
    const Descriptor* in = file->message_type(0)->nested_type(0);
    EXPECT_EQ(cs::CsTypeName(file->message_type(0)), "Outer");
    EXPECT_EQ(cs::CsTypeName(in), "Outer_Inner");
    EXPECT_EQ(cs::CsTypeName(in->nested_type(0)), "Outer_Inner_Deep");
    EXPECT_EQ(cs::CsTypeName(in->enum_type(0)), "Outer_Inner_Color");
}

TEST(CsNames, ProtocsNamespaceInsideFletcherGenIsRefused) {
    // BIND-6e: Fletcher.Gen is reserved for Fletcher's C# (D-BIND-69). protoc's own C# goes to
    // csharp_namespace when set, else to the PascalCased package; inside Fletcher.Gen its types
    // could share a namespace with Fletcher's (CS0101 in a project compiling both).
    struct Case {
        const char* package;
        const char* csharp_namespace;  // nullptr: not set
        bool refused;
    };
    const Case cases[] = {
        {"nav", "Fletcher.Gen", true},
        {"nav", "Fletcher.Gen.Nav", true},
        {"fletcher.gen.nav", nullptr, true},  // protoc's default: Fletcher.Gen.Nav
        {"fletcher.gen", nullptr, true},
        {"nav", "Fletcher.Generated", false},  // a prefix of the text, not of the namespace
        {"fletcher.generator", nullptr, false},
        {"nav", "Eiva.Nav", false},
        {"fletcher.gen.nav", "Eiva.Nav", false},  // the option wins over the package, as in protoc
        {"", nullptr, false},
    };
    int n = 0;
    for (const auto& c : cases) {
        FileDescriptorProto fdp;
        fdp.set_name("ns" + std::to_string(n++) + ".proto");
        fdp.set_package(c.package);
        fdp.set_syntax("proto3");
        if (c.csharp_namespace != nullptr)
            fdp.mutable_options()->set_csharp_namespace(c.csharp_namespace);
        DescriptorPool pool;
        const FileDescriptor* file = pool.BuildFile(fdp);
        ASSERT_NE(file, nullptr);
        const auto conflict = cs::CsProtocNamespaceConflict(file);
        EXPECT_EQ(conflict.has_value(), c.refused)
            << "package '" << c.package << "', csharp_namespace '"
            << (c.csharp_namespace ? c.csharp_namespace : "<unset>") << "'";
        if (conflict) {
            EXPECT_NE(conflict->find("reserved for Fletcher's generated C#"), std::string::npos)
                << *conflict;
            EXPECT_NE(conflict->find(file->name()), std::string::npos) << *conflict;
        }
    }
}

TEST(CsNames, AProtocTypeNamedLikeOurNamespaceIsRefused) {
    // D-BIND-91, the other direction of D-BIND-82: protoc declares each top-level message and enum
    // as <its namespace>.<Name>. When that IS `Fletcher` or `Fletcher.Gen`, the type takes the
    // name of our namespace: CS0101 in a project compiling both (measured 2026-10-14 for the first
    // three shapes below, against a control that compiles).
    struct Case {
        const char* package;
        const char* csharp_namespace;  // nullptr: not set
        const char* message;           // nullptr: none
        const char* enumeration;       // nullptr: none
        bool refused;
    };
    const Case cases[] = {
        {"fletcher", nullptr, "Gen", nullptr, true},  // Fletcher.Gen
        {"nav", "Fletcher", "Gen", nullptr, true},    // the option, as protoc reads it
        {"", nullptr, "Fletcher", nullptr, true},     // no namespace at all: Fletcher
        {"fletcher", nullptr, nullptr, "Gen", true},  // an enum is a type too
        {"", nullptr, nullptr, "Fletcher", true},
        {"nav", nullptr, "Gen", nullptr, false},  // Nav.Gen
        {"fletcher", nullptr, "Generator", nullptr, false},
        {"fletcher", nullptr, "gen", nullptr, false},  // C# names are case-sensitive
        {"nav", "Eiva", "Fletcher", nullptr, false},   // Eiva.Fletcher
        {"fletcher.gen", "Eiva.Nav", "Gen", nullptr, false},
    };
    int n = 0;
    for (const auto& c : cases) {
        FileDescriptorProto fdp;
        fdp.set_name("type" + std::to_string(n++) + ".proto");
        fdp.set_package(c.package);
        fdp.set_syntax("proto3");
        if (c.csharp_namespace != nullptr)
            fdp.mutable_options()->set_csharp_namespace(c.csharp_namespace);
        if (c.message != nullptr) fdp.add_message_type()->set_name(c.message);
        if (c.enumeration != nullptr) {
            auto* e = fdp.add_enum_type();
            e->set_name(c.enumeration);
            e->add_value()->set_name(std::string(c.enumeration) + "_UNSPECIFIED");
        }
        DescriptorPool pool;
        const FileDescriptor* file = pool.BuildFile(fdp);
        ASSERT_NE(file, nullptr) << fdp.name();
        const auto conflict = cs::CsProtocNamespaceConflict(file);
        EXPECT_EQ(conflict.has_value(), c.refused)
            << "package '" << c.package << "', csharp_namespace '"
            << (c.csharp_namespace ? c.csharp_namespace : "<unset>") << "', type '"
            << (c.message ? c.message : c.enumeration) << "'";
        if (conflict) {
            EXPECT_NE(conflict->find("would declare a type"), std::string::npos) << *conflict;
            EXPECT_NE(conflict->find("reserved for Fletcher's generated C#"), std::string::npos)
                << *conflict;
            EXPECT_NE(conflict->find(file->name()), std::string::npos) << *conflict;
        }
    }
}

TEST(CsNames, ATypeFromAnotherPackageIsQualifiedFromTheGlobalNamespace) {
    // BIND-6e: a field's type may be declared in another file. In the same package it shares
    // the namespace, so the flat name resolves; in another one it is named in full, from
    // global::, so no name in the referring file can shadow it.
    DescriptorPool pool;
    FileDescriptorProto shared;
    shared.set_name("shared.proto");
    shared.set_package("geo.shared_types");
    shared.set_syntax("proto3");
    auto* pos = shared.add_message_type();
    pos->set_name("Pos");
    pos->add_nested_type()->set_name("Fix");
    auto* q = shared.add_enum_type();
    q->set_name("Quality");
    q->add_value()->set_name("QUALITY_UNSPECIFIED");
    const FileDescriptor* other = pool.BuildFile(shared);
    ASSERT_NE(other, nullptr);

    FileDescriptorProto same;
    same.set_name("same.proto");
    same.set_package("geo.shared_types");
    same.set_syntax("proto3");
    const FileDescriptor* sibling = pool.BuildFile(same);
    ASSERT_NE(sibling, nullptr);

    FileDescriptorProto user;
    user.set_name("user.proto");
    user.set_package("app");
    user.set_syntax("proto3");
    const FileDescriptor* file = pool.BuildFile(user);
    ASSERT_NE(file, nullptr);

    const Descriptor* p = other->message_type(0);
    EXPECT_EQ(cs::CsTypeRef(p, other), "Pos");
    EXPECT_EQ(cs::CsTypeRef(p, sibling), "Pos");
    EXPECT_EQ(cs::CsTypeRef(p, file), "global::Fletcher.Gen.Geo.SharedTypes.Pos");
    EXPECT_EQ(cs::CsTypeRef(p->nested_type(0), file),
              "global::Fletcher.Gen.Geo.SharedTypes.Pos_Fix");
    EXPECT_EQ(cs::CsTypeRef(other->enum_type(0), file),
              "global::Fletcher.Gen.Geo.SharedTypes.Quality");

    // The enum's scalar type is named the same way, from the file being generated.
    fletcher::ir::LogicalType t{};
    t.kind = fletcher::ir::LogicalKind::INT32;
    fletcher::ir::EnumIdentity id;
    id.descriptor = other->enum_type(0);
    EXPECT_EQ(cs::CsLookupScalar(t, id, file).value().type_text,
              "global::Fletcher.Gen.Geo.SharedTypes.Quality");
    EXPECT_EQ(cs::CsLookupScalar(t, id, sibling).value().type_text, "Quality");
}

// ---------------------------------------------------------------------------
// Enum members (GetEnumValueName + the duplicate loop in csharp_enum.cc)
// ---------------------------------------------------------------------------

TEST(CsNames, EnumPrefixIsStrippedThenShoutyToPascalCase) {
    DescriptorPool pool;
    const EnumDescriptor* e = BuildEnum(
        pool, "Color", {{"COLOR_UNSPECIFIED", 0}, {"COLOR_RED", 1}, {"COLOR_DARK_BLUE", 2}});
    ASSERT_NE(e, nullptr);
    const std::vector<std::pair<std::string, int>> want = {
        {"Unspecified", 0}, {"Red", 1}, {"DarkBlue", 2}};
    EXPECT_EQ(Members(e), want);
}

TEST(CsNames, EnumPrefixMatchIgnoresCaseAndUnderscores) {
    DescriptorPool pool;
    const EnumDescriptor* e = BuildEnum(pool, "MyEnum", {{"MY_ENUM_A", 0}, {"MYENUM_B", 1}});
    ASSERT_NE(e, nullptr);
    const std::vector<std::pair<std::string, int>> want = {{"A", 0}, {"B", 1}};
    EXPECT_EQ(Members(e), want);
}

TEST(CsNames, EnumValueWithoutThePrefixIsKeptWhole) {
    DescriptorPool pool;
    const EnumDescriptor* e = BuildEnum(pool, "Color", {{"RED", 0}, {"DARK_BLUE", 1}});
    ASSERT_NE(e, nullptr);
    const std::vector<std::pair<std::string, int>> want = {{"Red", 0}, {"DarkBlue", 1}};
    EXPECT_EQ(Members(e), want);
}

TEST(CsNames, EnumValueThatIsOnlyThePrefixIsKeptWhole) {
    DescriptorPool pool;
    const EnumDescriptor* e = BuildEnum(pool, "Color", {{"COLOR", 0}});
    ASSERT_NE(e, nullptr);
    const std::vector<std::pair<std::string, int>> want = {{"Color", 0}};
    EXPECT_EQ(Members(e), want);
}

TEST(CsNames, EnumRemainderStartingWithADigitGetsALeadingUnderscore) {
    DescriptorPool pool;
    const EnumDescriptor* e = BuildEnum(pool, "Mode", {{"MODE_UNSPECIFIED", 0}, {"MODE_2D", 1}});
    ASSERT_NE(e, nullptr);
    const std::vector<std::pair<std::string, int>> want = {{"Unspecified", 0}, {"_2D", 1}};
    EXPECT_EQ(Members(e), want);
}

TEST(CsNames, EnumMembersCollidingAfterStrippingAreSuffixed) {
    // proto3 refuses COLOR_RED and RED side by side unless they are aliases with one
    // number ("same name if you ignore case and strip out the enum name prefix"), so
    // an alias pair is the collision protoc's duplicate loop meets in practice.
    DescriptorPool pool;
    const EnumDescriptor* e =
        BuildEnum(pool, "Color", {{"COLOR_UNSPECIFIED", 0}, {"COLOR_RED", 1}, {"RED", 1}}, true);
    ASSERT_NE(e, nullptr);
    const std::vector<std::pair<std::string, int>> want = {
        {"Unspecified", 0}, {"Red", 1}, {"Red_", 1}};
    EXPECT_EQ(Members(e), want);
}

TEST(CsNames, EnumAliasesAreAllEmittedWithTheirNumbers) {
    DescriptorPool pool;
    const EnumDescriptor* e = BuildEnum(
        pool, "Speed", {{"SPEED_UNSPECIFIED", 0}, {"SPEED_FAST", 1}, {"SPEED_QUICK", 1}}, true);
    ASSERT_NE(e, nullptr);
    const std::vector<std::pair<std::string, int>> want = {
        {"Unspecified", 0}, {"Fast", 1}, {"Quick", 1}};
    EXPECT_EQ(Members(e), want);
}

// ---------------------------------------------------------------------------
// Scalar lookup: the ONLY place a C# type string lives (GIR locked decision #1)
// ---------------------------------------------------------------------------

TEST(CsTypeTable, ScalarsMapToCSharpTypes) {
    using LK = fletcher::ir::LogicalKind;
    const std::vector<std::pair<LK, std::string>> cases = {
        {LK::BOOL, "bool"},      {LK::INT32, "int"},    {LK::INT64, "long"},
        {LK::UINT32, "uint"},    {LK::UINT64, "ulong"}, {LK::FLOAT32, "float"},
        {LK::FLOAT64, "double"}, {LK::UTF8, "string"},  {LK::BINARY, "byte[]"},
    };
    for (const auto& [kind, text] : cases) {
        fletcher::ir::LogicalType t{};
        t.kind = kind;
        const auto info = cs::CsLookupScalar(t, std::nullopt, nullptr);
        ASSERT_TRUE(info.has_value()) << text;
        EXPECT_EQ(info->type_text, text);
        EXPECT_EQ(info->is_reference, text == "string" || text == "byte[]") << text;
    }
}

TEST(CsTypeTable, AnEnumScalarIsItsGeneratedEnumType) {
    DescriptorPool pool;
    const EnumDescriptor* e = BuildEnum(pool, "Color", {{"COLOR_UNSPECIFIED", 0}});
    ASSERT_NE(e, nullptr);
    fletcher::ir::LogicalType t{};
    t.kind = fletcher::ir::LogicalKind::INT32;
    fletcher::ir::EnumIdentity id;
    id.descriptor = e;
    const auto info = cs::CsLookupScalar(t, id, e->file());
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->type_text, "Color");
    EXPECT_FALSE(info->is_reference);
}

TEST(CsTypeTable, ArrowTypeExpressionsCompileAgainstApacheArrow23) {
    // The expressions BIND-6b's schema sink renders. Each was compiled against
    // Apache.Arrow 23.0.0, whose DurationType has no public constructor.
    EXPECT_EQ(cs::CsArrowScalarTypeExpr(NANOARROW_TYPE_INT32).value_or(""),
              "global::Apache.Arrow.Types.Int32Type.Default");
    EXPECT_EQ(cs::CsArrowScalarTypeExpr(NANOARROW_TYPE_BINARY).value_or(""),
              "global::Apache.Arrow.Types.BinaryType.Default");
    EXPECT_FALSE(cs::CsArrowScalarTypeExpr(NANOARROW_TYPE_HALF_FLOAT).has_value());
    EXPECT_EQ(
        cs::CsArrowDateTimeTypeExpr(NANOARROW_TYPE_TIMESTAMP, NANOARROW_TIME_UNIT_NANO, nullptr)
            .value_or(""),
        "new global::Apache.Arrow.Types.TimestampType("
        "global::Apache.Arrow.Types.TimeUnit.Nanosecond, (string?)null)");
    EXPECT_EQ(
        cs::CsArrowDateTimeTypeExpr(NANOARROW_TYPE_TIMESTAMP, NANOARROW_TIME_UNIT_MICRO, "UTC")
            .value_or(""),
        "new global::Apache.Arrow.Types.TimestampType("
        "global::Apache.Arrow.Types.TimeUnit.Microsecond, \"UTC\")");
    EXPECT_EQ(
        cs::CsArrowDateTimeTypeExpr(NANOARROW_TYPE_DURATION, NANOARROW_TIME_UNIT_NANO, nullptr)
            .value_or(""),
        "global::Apache.Arrow.Types.DurationType.FromTimeUnit("
        "global::Apache.Arrow.Types.TimeUnit.Nanosecond)");
}

TEST(CsTypeTable, StringLiteralsEscapeWhatCannotAppearRaw) {
    EXPECT_EQ(cs::CsStringLiteral("plain"), "\"plain\"");
    EXPECT_EQ(cs::CsStringLiteral("a\"b\\c"), "\"a\\\"b\\\\c\"");
    EXPECT_EQ(cs::CsStringLiteral("line\nnext\ttab\r"), "\"line\\nnext\\ttab\\r\"");
    EXPECT_EQ(cs::CsStringLiteral(std::string("x\x01y", 3)), "\"x\\u0001y\"");
    // UTF-8 passes through: the generated file is UTF-8.
    EXPECT_EQ(cs::CsStringLiteral("h\xc3\xa9"), "\"h\xc3\xa9\"");
}

TEST(CsTypeTable, KindsNotYetMappedReturnNothing) {
    // Temporal types arrive with BIND-6c (D-BIND-26: lossless); until then the
    // table answers "not mapped" rather than a lossy guess.
    fletcher::ir::LogicalType t{};
    t.kind = fletcher::ir::LogicalKind::WKT_TIMESTAMP;
    EXPECT_FALSE(cs::CsLookupScalar(t, std::nullopt, nullptr).has_value());
    t.kind = fletcher::ir::LogicalKind::WKT_DURATION;
    EXPECT_FALSE(cs::CsLookupScalar(t, std::nullopt, nullptr).has_value());
}
