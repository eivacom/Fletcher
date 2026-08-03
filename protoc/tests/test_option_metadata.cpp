// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include <google/protobuf/compiler/parser.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/io/tokenizer.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include <gtest/gtest.h>
#include <nanoarrow/nanoarrow.h>

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "option_metadata.hpp"
#include "schema_builder.hpp"

namespace {

using fletcher::EscapeCppStringLiteral;
using fletcher::MetadataRule;
using fletcher::OptionMetadataResolver;
using google::protobuf::DescriptorPool;
using google::protobuf::FieldDescriptor;
using google::protobuf::FileDescriptor;
using google::protobuf::FileDescriptorProto;

TEST(EscapeCppStringLiteralTest, PrintableAsciiIsUnchanged) {
    // Identifiers and keys must escape to themselves — this is what makes the
    // pair-builder refactor byte-neutral for the four builtin metadata keys.
    EXPECT_EQ(EscapeCppStringLiteral("proto_package"), "proto_package");
    EXPECT_EQ(EscapeCppStringLiteral("ARROW:extension:name"), "ARROW:extension:name");
    EXPECT_EQ(EscapeCppStringLiteral("2.1"), "2.1");
    EXPECT_EQ(EscapeCppStringLiteral(""), "");
}

TEST(EscapeCppStringLiteralTest, QuotesAndBackslashesAreEscaped) {
    // The motivating case: an option carrying JSON, e.g. ext_metadata.
    EXPECT_EQ(EscapeCppStringLiteral(R"({"crs":"EPSG:4326"})"), R"({\"crs\":\"EPSG:4326\"})");
    EXPECT_EQ(EscapeCppStringLiteral("a\\b"), "a\\\\b");
    EXPECT_EQ(EscapeCppStringLiteral(R"(\")"), R"(\\\")");
}

TEST(EscapeCppStringLiteralTest, CommonControlCharactersUseShortEscapes) {
    EXPECT_EQ(EscapeCppStringLiteral("a\nb"), "a\\nb");
    EXPECT_EQ(EscapeCppStringLiteral("a\rb"), "a\\rb");
    EXPECT_EQ(EscapeCppStringLiteral("a\tb"), "a\\tb");
}

TEST(EscapeCppStringLiteralTest, OtherControlCharactersUseThreeDigitOctal) {
    EXPECT_EQ(EscapeCppStringLiteral(std::string("\x01")), "\\001");
    EXPECT_EQ(EscapeCppStringLiteral(std::string("\x1F")), "\\037");
    EXPECT_EQ(EscapeCppStringLiteral(std::string("\x7F")), "\\177");
}

TEST(EscapeCppStringLiteralTest, OctalEscapeIsNotGreedyAcrossFollowingCharacters) {
    // The reason octal is used instead of \x. A hex escape is unbounded, so
    // "\x01" followed by 'A' would be read back as the single character 0x1A.
    // Octal stops after exactly three digits, so 'A' stays a separate character.
    EXPECT_EQ(EscapeCppStringLiteral(std::string("\x01") + "A"), "\\001A");
    // Digits following the escape are the sharper case: \0011 must remain a
    // three-digit escape plus the character '1'.
    EXPECT_EQ(EscapeCppStringLiteral(std::string("\x01") + "1"), "\\0011");
}

TEST(EscapeCppStringLiteralTest, EmbeddedNulIsEscapedAsThreeDigitOctal) {
    // Three digits matter here too: "\0" followed by '1' would otherwise read
    // back as 0x01. Note the generated path's ArrowCharView is strlen-based, so
    // a value containing NUL is truncated at emission — the in-process path
    // truncates identically via c_str(), so the two paths still agree.
    const std::string with_nul(
        "a\0"
        "1",
        3);
    EXPECT_EQ(EscapeCppStringLiteral(with_nul), "a\\0001");
}

TEST(EscapeCppStringLiteralTest, NonAsciiBytesAreEscapedSoOutputStaysAscii) {
    // "°" is U+00B0 => UTF-8 0xC2 0xB0. Emitting the raw bytes would make the
    // generated header's meaning depend on the compiler's source encoding.
    const std::string degree = "\xC2\xB0";
    const std::string escaped = EscapeCppStringLiteral(degree);
    EXPECT_EQ(escaped, "\\302\\260");
    for (const unsigned char c : escaped) {
        EXPECT_LT(c, 0x80) << "escaped output must be pure ASCII";
    }
}

TEST(EscapeCppStringLiteralTest, MixedRealisticValueRoundTripsToExpectedLiteral) {
    const std::string value = "{\"crs\":\"EPSG:4326\",\"note\":\"line1\nline2\"}";
    EXPECT_EQ(EscapeCppStringLiteral(value),
              "{\\\"crs\\\":\\\"EPSG:4326\\\",\\\"note\\\":\\\"line1\\nline2\\\"}");
}

// ===========================================================================
// Fixture pool — compiles .proto SOURCE TEXT so the custom options land in the
// descriptors exactly the way protoc produces them: the pool knows the
// extension, but the linked-in google::protobuf::FieldOptions C++ class does
// not, so the value sits in the options message's UnknownFieldSet. That is the
// precise shape the DynamicMessage re-parse has to cope with in production.
// ===========================================================================

class CollectErrors : public google::protobuf::io::ErrorCollector {
   public:
    std::string text;
    void AddError(int line, int column, const std::string& message) override {
        text += std::to_string(line) + ":" + std::to_string(column) + ": " + message + "\n";
    }
    void AddWarning(int, int, const std::string&) override {}
};

class FixturePool {
   public:
    FixturePool() {
        FileDescriptorProto fdp;
        google::protobuf::DescriptorProto::descriptor()->file()->CopyTo(&fdp);
        EXPECT_NE(pool_.BuildFile(fdp), nullptr) << "failed to seed descriptor.proto";
    }

    const FileDescriptor* Add(const std::string& name, const std::string& source) {
        google::protobuf::io::ArrayInputStream input(source.data(),
                                                     static_cast<int>(source.size()));
        CollectErrors errors;
        google::protobuf::io::Tokenizer tokenizer(&input, &errors);
        google::protobuf::compiler::Parser parser;
        parser.RecordErrorsTo(&errors);

        FileDescriptorProto fdp;
        if (!parser.Parse(&tokenizer, &fdp)) {
            ADD_FAILURE() << "parse of " << name << " failed:\n" << errors.text;
            return nullptr;
        }
        fdp.set_name(name);
        const FileDescriptor* fd = pool_.BuildFile(fdp);
        if (fd == nullptr) ADD_FAILURE() << "BuildFile(" << name << ") failed:\n" << errors.text;
        return fd;
    }

    const DescriptorPool* pool() const { return &pool_; }

   private:
    DescriptorPool pool_;
};

// Option definitions. Neutral names throughout: the resolver must never key off
// any particular vocabulary, so the fixture deliberately avoids one.
constexpr const char* kOptionsProto = R"(
syntax = "proto3";
package fx;
import "google/protobuf/descriptor.proto";

message EncOpts { string ext_name = 1; }
message TypOpts { string ext_name = 3; }
extend google.protobuf.EnumValueOptions {
  EncOpts enc_opts = 60103;
  TypOpts typ_opts = 60102;
}

enum Kind {
  KIND_UNSPECIFIED = 0;
  KIND_ALPHA = 1 [(fx.typ_opts) = { ext_name: "fx.alpha" }];
  KIND_BETA  = 2;
}

enum Enc {
  ENC_UNSPECIFIED = 0;
  ENC_F64 = 10;
  ENC_GEO = 11 [(fx.enc_opts) = { ext_name: "geoarrow.point" }];
}

message Nested { string inner = 1; }

message ColOpts {
  Kind kind = 1;
  Enc enc = 2;
  string unit = 3;
  repeated string tags = 4;
  bool flag = 5;
  int32 count = 6;
  double ratio = 7;
  Nested nested = 8;
}

message TypeDef {
  Kind kind = 1;
  Enc enc = 2;
  string unit = 3;
  string group = 4;
}

extend google.protobuf.FieldOptions   { ColOpts col = 60100; }
extend google.protobuf.MessageOptions { TypeDef typ = 60101; }
)";

// Schema fixture. Declares flatten/flatten_field itself (the plugin reads them
// by extension NUMBER 50000, so it need not be fletcher/options.proto).
constexpr const char* kSchemaProto = R"(
syntax = "proto3";
package fxu;
import "fx.proto";
import "google/protobuf/descriptor.proto";

extend google.protobuf.MessageOptions { bool flatten = 50000; }
extend google.protobuf.FieldOptions   { bool flatten_field = 50000; }

message Stamp {
  option (fxu.flatten) = true;
  option (fx.typ) = { kind: KIND_ALPHA, enc: ENC_F64, unit: "s" };
  int64 value = 1;
}

message Coord {
  option (fx.typ) = { unit: "m" };
  double x = 1;
  double y = 2;
}

message Pos {
  option (fx.typ) = { kind: KIND_ALPHA, enc: ENC_GEO };
  Coord coord = 1 [(fxu.flatten_field) = true];
}

message Sample {
  option (fx.typ) = { group: "g0" };
  Stamp t = 1;
  Pos pos = 2;
  double heading = 3 [(fx.col) = { unit: "deg" }];
  repeated Stamp stamps = 4;
  map<string, double> extras = 5;
}
)";

// The canonical rule list. Note the ARROW:extension:name ordering: the
// kind-derived fallback is listed FIRST and the enc-derived preference SECOND,
// because precedence is last-non-empty-wins.
std::vector<std::string> CanonicalRules() {
    return {
        "metadata_from_option=message:fx.typ.group:x:group",
        "metadata_from_option=field_type:fx.typ.kind:x:kind",
        "metadata_from_option=field_type:fx.typ.unit:x:unit",
        "metadata_from_option=field:fx.col.unit:x:unit",
        "metadata_from_option=field_type:fx.typ.kind/fx.typ_opts.ext_name:ARROW:extension:name",
        "metadata_from_option=field_type:fx.typ.enc/fx.enc_opts.ext_name:ARROW:extension:name",
    };
}

std::string Join(const std::vector<std::string>& parts) {
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) out += ",";
        out += parts[i];
    }
    return out;
}

// Build a resolver from a parameter string. Fails the test on parse errors.
std::unique_ptr<OptionMetadataResolver> MakeResolver(const FixturePool& fx,
                                                     const std::string& parameter) {
    std::vector<MetadataRule> rules;
    std::string error;
    if (!fletcher::ParseMetadataRules(parameter, &rules, &error)) {
        ADD_FAILURE() << "ParseMetadataRules failed: " << error;
        return nullptr;
    }
    auto resolver = OptionMetadataResolver::Create(rules, fx.pool(), &error);
    if (!resolver) ADD_FAILURE() << "Create failed: " << error;
    return resolver;
}

// Collect a parse-or-compile failure message, if any.
std::string RuleError(const FixturePool& fx, const std::string& parameter) {
    std::vector<MetadataRule> rules;
    std::string error;
    if (!fletcher::ParseMetadataRules(parameter, &rules, &error)) return error;
    if (!OptionMetadataResolver::Create(rules, fx.pool(), &error)) return error;
    return "";
}

std::map<std::string, std::string> AsMap(
    const std::vector<std::pair<std::string, std::string>>& pairs) {
    return std::map<std::string, std::string>(pairs.begin(), pairs.end());
}

const FieldDescriptor* FieldOf(const FileDescriptor* file, const std::string& msg,
                               const std::string& field) {
    const auto* m = file->FindMessageTypeByName(msg);
    return m ? m->FindFieldByName(field) : nullptr;
}

// A fixture pair the resolver tests share.
struct Fx {
    FixturePool pool;
    const FileDescriptor* schema = nullptr;
    Fx() {
        pool.Add("fx.proto", kOptionsProto);
        schema = pool.Add("fxu.proto", kSchemaProto);
    }
};

std::string MetaValue(const ArrowSchema* s, const char* key) {
    if (!s->metadata || !ArrowMetadataHasKey(s->metadata, ArrowCharView(key))) return "<missing>";
    ArrowStringView value{};
    if (ArrowMetadataGetValue(s->metadata, ArrowCharView(key), &value) != NANOARROW_OK) {
        return "<error>";
    }
    return std::string(value.data, static_cast<size_t>(value.size_bytes));
}

const ArrowSchema* ChildNamed(const ArrowSchema* s, const std::string& name) {
    for (int64_t i = 0; i < s->n_children; ++i) {
        if (s->children[i]->name && name == s->children[i]->name) return s->children[i];
    }
    return nullptr;
}

// ===========================================================================
// Rule parsing
// ===========================================================================

TEST(MetadataRuleParseTest, ParsesScopePathAndKeyWithColonsInTheKey) {
    std::vector<MetadataRule> rules;
    std::string error;
    ASSERT_TRUE(fletcher::ParseMetadataRules(
        "ts,ipc,metadata_from_option=field_type:fx.typ.enc/fx.enc_opts.ext_name:ARROW:extension:"
        "name",
        &rules, &error))
        << error;
    ASSERT_EQ(rules.size(), 1u);
    EXPECT_EQ(rules[0].scope, MetadataRule::Scope::kFieldType);
    ASSERT_EQ(rules[0].steps.size(), 2u);
    EXPECT_EQ(rules[0].steps[0], "fx.typ.enc");
    EXPECT_EQ(rules[0].steps[1], "fx.enc_opts.ext_name");
    // Only the FIRST TWO colons are separators; the key keeps its own.
    EXPECT_EQ(rules[0].arrow_key, "ARROW:extension:name");
}

TEST(MetadataRuleParseTest, NonRuleTokensAreLeftAlone) {
    std::vector<MetadataRule> rules;
    std::string error;
    ASSERT_TRUE(
        fletcher::ParseMetadataRules("ts,ipc,accessor,rust,schema_only,banana", &rules, &error))
        << error;
    EXPECT_TRUE(rules.empty());
}

TEST(MetadataRuleParseTest, MalformedRulesAreHardErrors) {
    std::vector<MetadataRule> rules;
    std::string error;
    // Missing the second separator.
    EXPECT_FALSE(
        fletcher::ParseMetadataRules("metadata_from_option=field:fx.col.unit", &rules, &error));
    EXPECT_FALSE(
        fletcher::ParseMetadataRules("metadata_from_option=nope:fx.col.unit:k", &rules, &error));
    EXPECT_FALSE(fletcher::ParseMetadataRules("metadata_from_option=field::k", &rules, &error));
    EXPECT_FALSE(
        fletcher::ParseMetadataRules("metadata_from_option=field:fx.col.unit:", &rules, &error));
    EXPECT_FALSE(
        fletcher::ParseMetadataRules("metadata_from_option=field:fx.col//unit:k", &rules, &error));
}

TEST(MetadataRuleParseTest, ReservedArrowKeysAreRejected) {
    std::vector<MetadataRule> rules;
    std::string error;
    for (const char* key : {"proto_package", "proto_message", "field_number", "field_id"}) {
        rules.clear();
        EXPECT_FALSE(fletcher::ParseMetadataRules(
            std::string("metadata_from_option=field:fx.col.unit:") + key, &rules, &error))
            << "key " << key << " must be rejected";
        EXPECT_NE(error.find(key), std::string::npos);
    }
}

// ===========================================================================
// Rule compilation against the pool
// ===========================================================================

TEST(MetadataRuleCompileTest, StaticPathErrorsAreHardErrors) {
    Fx fx;
    // Unknown sub-field.
    EXPECT_NE(RuleError(fx.pool, "metadata_from_option=field:fx.col.nope:k"), "");
    // Terminal is a message.
    EXPECT_NE(RuleError(fx.pool, "metadata_from_option=field:fx.col.nested:k"), "");
    // Descends through a non-message.
    EXPECT_NE(RuleError(fx.pool, "metadata_from_option=field:fx.col.unit.more:k"), "");
    // '/' hop off a non-enum.
    EXPECT_NE(RuleError(fx.pool, "metadata_from_option=field:fx.col.unit/fx.enc_opts.ext_name:k"),
              "");
    // Scope disagrees with the extension's extendee (fx.col extends FieldOptions).
    EXPECT_NE(RuleError(fx.pool, "metadata_from_option=message:fx.col.unit:k"), "");
    // double is refused: its textual form is not reproducible across platforms.
    EXPECT_NE(RuleError(fx.pool, "metadata_from_option=field:fx.col.ratio:k"), "");
}

TEST(MetadataRuleCompileTest, MultiComponentSubFieldPathResolvesToTheLongestExtensionPrefix) {
    // "fx.col.nested.inner" must bind extension `fx.col` and sub-path
    // nested->inner. The compiler scans prefixes longest-first, so this also
    // covers it skipping "fx.col.nested.inner" and "fx.col.nested" (neither is
    // an extension) before settling on "fx.col".
    FixturePool local;
    local.Add("fx.proto", kOptionsProto);
    const FileDescriptor* f = local.Add("deep.proto", R"(
syntax = "proto3";
package dp;
import "fx.proto";
message M { double v = 1 [(fx.col) = { nested { inner: "deep" } }]; }
)");
    ASSERT_NE(f, nullptr);
    auto resolver = MakeResolver(local, "metadata_from_option=field:fx.col.nested.inner:x:inner");
    ASSERT_NE(resolver, nullptr);
    const auto m = AsMap(resolver->ForField(FieldOf(f, "M", "v"), {}));
    EXPECT_EQ(m.at("x:inner"), "deep");
}

TEST(MetadataRuleCompileTest, UnresolvableExtensionIsDroppedNotAnError) {
    Fx fx;
    // No file in this pool declares `absent.thing`, which is the normal case when
    // one flag list is applied to a corpus where only some files import the
    // option .proto. The rule is dropped; codegen continues.
    EXPECT_EQ(RuleError(fx.pool, "metadata_from_option=field:absent.thing.unit:k"), "");
    auto resolver = MakeResolver(fx.pool, "metadata_from_option=field:absent.thing.unit:k");
    ASSERT_NE(resolver, nullptr);
    const auto* heading = FieldOf(fx.schema, "Sample", "heading");
    ASSERT_NE(heading, nullptr);
    EXPECT_TRUE(resolver->ForField(heading, {}).empty());
}

// ===========================================================================
// Resolution semantics
// ===========================================================================

TEST(OptionMetadataTest, MessageScopeReadsTheMessagesOwnOptions) {
    Fx fx;
    auto resolver = MakeResolver(fx.pool, Join(CanonicalRules()));
    ASSERT_NE(resolver, nullptr);
    const auto m = AsMap(resolver->ForMessage(fx.schema->FindMessageTypeByName("Sample")));
    EXPECT_EQ(m.at("x:group"), "g0");
    // kind/unit are field_type-scoped, so they must NOT appear at schema level.
    EXPECT_EQ(m.count("x:kind"), 0u);
}

TEST(OptionMetadataTest, FieldTypeScopeInheritsFromTheFieldsMessageType) {
    Fx fx;
    auto resolver = MakeResolver(fx.pool, Join(CanonicalRules()));
    ASSERT_NE(resolver, nullptr);
    // `t` is typed Stamp; Stamp carries (fx.typ). This is the case a plain
    // field-scope copy cannot reach.
    const auto m = AsMap(resolver->ForField(FieldOf(fx.schema, "Sample", "t"), {}));
    EXPECT_EQ(m.at("x:kind"), "KIND_ALPHA");
    EXPECT_EQ(m.at("x:unit"), "s");
}

TEST(OptionMetadataTest, EnumSubFieldRendersAsTheVerbatimValueName) {
    Fx fx;
    auto resolver = MakeResolver(fx.pool, Join(CanonicalRules()));
    ASSERT_NE(resolver, nullptr);
    const auto m = AsMap(resolver->ForField(FieldOf(fx.schema, "Sample", "t"), {}));
    // Not "1", and not a stripped/lowercased form — Fletcher applies no
    // transformation to the declared name.
    EXPECT_EQ(m.at("x:kind"), "KIND_ALPHA");
}

TEST(OptionMetadataTest, EnumHopReadsTheEnumValuesOwnOptionExtension) {
    Fx fx;
    auto resolver = MakeResolver(fx.pool, Join(CanonicalRules()));
    ASSERT_NE(resolver, nullptr);
    // Pos declares enc = ENC_GEO, whose EnumValueOptions carry ext_name.
    const auto m = AsMap(resolver->ForField(FieldOf(fx.schema, "Sample", "pos"), {}));
    EXPECT_EQ(m.at("ARROW:extension:name"), "geoarrow.point");
}

TEST(OptionMetadataTest, EnumHopFallsBackWhenThePreferredHopYieldsNothing) {
    Fx fx;
    auto resolver = MakeResolver(fx.pool, Join(CanonicalRules()));
    ASSERT_NE(resolver, nullptr);
    // Stamp declares enc = ENC_F64, which has no enc_opts. The earlier
    // kind-derived rule therefore stands: fallback IS declaration order.
    const auto m = AsMap(resolver->ForField(FieldOf(fx.schema, "Sample", "t"), {}));
    EXPECT_EQ(m.at("ARROW:extension:name"), "fx.alpha");
}

TEST(OptionMetadataTest, FieldScopeOverridesInheritedFieldTypeScopePerKey) {
    Fx fx;
    auto resolver = MakeResolver(fx.pool, Join(CanonicalRules()));
    ASSERT_NE(resolver, nullptr);
    // `heading` is a bare double with its own (fx.col).unit.
    const auto m = AsMap(resolver->ForField(FieldOf(fx.schema, "Sample", "heading"), {}));
    EXPECT_EQ(m.at("x:unit"), "deg");
    // A scalar has no message type, so nothing is inherited.
    EXPECT_EQ(m.count("x:kind"), 0u);
}

TEST(OptionMetadataTest, RepeatedMessageFieldInheritsTheElementMessageOptions) {
    Fx fx;
    auto resolver = MakeResolver(fx.pool, Join(CanonicalRules()));
    ASSERT_NE(resolver, nullptr);
    const auto m = AsMap(resolver->ForField(FieldOf(fx.schema, "Sample", "stamps"), {}));
    EXPECT_EQ(m.at("x:unit"), "s");
}

TEST(OptionMetadataTest, MapFieldInheritsNothingBecauseMapEntryIsNeverACarrier) {
    Fx fx;
    auto resolver = MakeResolver(fx.pool, Join(CanonicalRules()));
    ASSERT_NE(resolver, nullptr);
    const auto* extras = FieldOf(fx.schema, "Sample", "extras");
    ASSERT_NE(extras, nullptr);
    ASSERT_TRUE(extras->is_map());
    EXPECT_TRUE(resolver->ForField(extras, {}).empty());
}

TEST(OptionMetadataTest, FlattenFieldWrapperContextReachesEachInlinedLeaf) {
    Fx fx;
    auto resolver = MakeResolver(fx.pool, Join(CanonicalRules()));
    ASSERT_NE(resolver, nullptr);
    // Pos.coord is (flatten_field), so x/y are inlined and the outer `coord`
    // descriptor is dropped from the field list — flatten_chain is the only way
    // back to Coord's (fx.typ).
    const auto* coord = FieldOf(fx.schema, "Pos", "coord");
    const auto* x = FieldOf(fx.schema, "Coord", "x");
    ASSERT_NE(coord, nullptr);
    ASSERT_NE(x, nullptr);
    EXPECT_TRUE(resolver->ForField(x, {}).empty()) << "no chain -> nothing to inherit";
    const auto m = AsMap(resolver->ForField(x, {coord}));
    EXPECT_EQ(m.at("x:unit"), "m");
}

TEST(OptionMetadataTest, AbsentAndEmptyOptionValuesProduceNoKey) {
    Fx fx;
    auto resolver = MakeResolver(fx.pool, Join(CanonicalRules()));
    ASSERT_NE(resolver, nullptr);
    // Coord declares only `unit`; kind is left at its proto3 default, which is
    // absence, not the string "KIND_UNSPECIFIED".
    const auto m = AsMap(resolver->ForField(FieldOf(fx.schema, "Pos", "coord"), {}));
    EXPECT_EQ(m.at("x:unit"), "m");
    EXPECT_EQ(m.count("x:kind"), 0u);
    EXPECT_EQ(m.count("ARROW:extension:name"), 0u);
}

TEST(OptionMetadataTest, RepeatedStringSubFieldJoinsWithCommas) {
    Fx fx;
    FixturePool local;
    local.Add("fx.proto", kOptionsProto);
    const FileDescriptor* f = local.Add("tags.proto", R"(
syntax = "proto3";
package tg;
import "fx.proto";
message M { double v = 1 [(fx.col) = { tags: "a", tags: "b" }]; }
)");
    ASSERT_NE(f, nullptr);
    auto resolver = MakeResolver(local, "metadata_from_option=field:fx.col.tags:x:tags");
    ASSERT_NE(resolver, nullptr);
    const auto m = AsMap(resolver->ForField(FieldOf(f, "M", "v"), {}));
    EXPECT_EQ(m.at("x:tags"), "a,b");
}

TEST(OptionMetadataTest, IntegralAndBoolSubFieldsRender) {
    Fx fx;
    FixturePool local;
    local.Add("fx.proto", kOptionsProto);
    const FileDescriptor* f = local.Add("scalars.proto", R"(
syntax = "proto3";
package sc;
import "fx.proto";
message M { double v = 1 [(fx.col) = { flag: true, count: 7 }]; }
)");
    ASSERT_NE(f, nullptr);
    auto resolver = MakeResolver(
        local,
        "metadata_from_option=field:fx.col.flag:x:flag,metadata_from_option=field:fx.col."
        "count:x:count");
    ASSERT_NE(resolver, nullptr);
    const auto m = AsMap(resolver->ForField(FieldOf(f, "M", "v"), {}));
    EXPECT_EQ(m.at("x:flag"), "true");
    EXPECT_EQ(m.at("x:count"), "7");
}

// ===========================================================================
// End-to-end through the in-process schema builder (the --fletcher_opt=ipc
// path). The generated-C++ path consumes the identical pair vector.
// ===========================================================================

TEST(OptionMetadataTest, BuiltinKeysSurviveAndMappedKeysAreAppended) {
    Fx fx;
    auto resolver = MakeResolver(fx.pool, Join(CanonicalRules()));
    ASSERT_NE(resolver, nullptr);

    nanoarrow::UniqueSchema schema =
        fletcher::BuildMessageSchema(fx.schema->FindMessageTypeByName("Sample"), resolver.get());

    EXPECT_EQ(MetaValue(schema.get(), "proto_package"), "fxu");
    EXPECT_EQ(MetaValue(schema.get(), "proto_message"), "Sample");
    EXPECT_EQ(MetaValue(schema.get(), "x:group"), "g0");

    const ArrowSchema* t = ChildNamed(schema.get(), "t");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(MetaValue(t, "field_number"), "1");
    EXPECT_EQ(MetaValue(t, "field_id"), "1");
    EXPECT_EQ(MetaValue(t, "x:kind"), "KIND_ALPHA");
    EXPECT_EQ(MetaValue(t, "ARROW:extension:name"), "fx.alpha");
}

TEST(OptionMetadataTest, FlattenedWrapperFieldCarriesTheWrapperMessagesMetadata) {
    Fx fx;
    auto resolver = MakeResolver(fx.pool, Join(CanonicalRules()));
    ASSERT_NE(resolver, nullptr);
    nanoarrow::UniqueSchema schema =
        fletcher::BuildMessageSchema(fx.schema->FindMessageTypeByName("Sample"), resolver.get());

    // Stamp is (flatten) with one int64 field, so `t` is a bare int64 column —
    // and it still carries Stamp's message-level annotation.
    const ArrowSchema* t = ChildNamed(schema.get(), "t");
    ASSERT_NE(t, nullptr);
    EXPECT_STREQ(t->format, "l");
    EXPECT_EQ(MetaValue(t, "x:unit"), "s");
}

TEST(OptionMetadataTest, InlinedLeavesInheritTheFlattenFieldWrapper) {
    Fx fx;
    auto resolver = MakeResolver(fx.pool, Join(CanonicalRules()));
    ASSERT_NE(resolver, nullptr);
    nanoarrow::UniqueSchema schema =
        fletcher::BuildMessageSchema(fx.schema->FindMessageTypeByName("Pos"), resolver.get());

    const ArrowSchema* x = ChildNamed(schema.get(), "x");
    const ArrowSchema* y = ChildNamed(schema.get(), "y");
    ASSERT_NE(x, nullptr);
    ASSERT_NE(y, nullptr);
    EXPECT_EQ(MetaValue(x, "field_id"), "1.1");
    EXPECT_EQ(MetaValue(x, "x:unit"), "m");
    EXPECT_EQ(MetaValue(y, "x:unit"), "m");
}

TEST(OptionMetadataTest, MapColumnCarriesOnlyTheBuiltinKeys) {
    Fx fx;
    auto resolver = MakeResolver(fx.pool, Join(CanonicalRules()));
    ASSERT_NE(resolver, nullptr);
    nanoarrow::UniqueSchema schema =
        fletcher::BuildMessageSchema(fx.schema->FindMessageTypeByName("Sample"), resolver.get());

    const ArrowSchema* extras = ChildNamed(schema.get(), "extras");
    ASSERT_NE(extras, nullptr);
    EXPECT_EQ(MetaValue(extras, "field_number"), "5");
    EXPECT_EQ(MetaValue(extras, "x:unit"), "<missing>");
    EXPECT_EQ(MetaValue(extras, "x:kind"), "<missing>");
}

TEST(OptionMetadataTest, NullResolverEmitsExactlyTheFourBuiltinKeys) {
    Fx fx;
    nanoarrow::UniqueSchema schema =
        fletcher::BuildMessageSchema(fx.schema->FindMessageTypeByName("Sample"), nullptr);
    EXPECT_EQ(MetaValue(schema.get(), "proto_package"), "fxu");
    EXPECT_EQ(MetaValue(schema.get(), "x:group"), "<missing>");
    const ArrowSchema* t = ChildNamed(schema.get(), "t");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(MetaValue(t, "field_id"), "1");
    EXPECT_EQ(MetaValue(t, "x:kind"), "<missing>");
}

}  // namespace
