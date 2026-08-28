// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#pragma once

// TEST-ONLY helper: a DescriptorPool fed from .proto SOURCE TEXT, so custom
// options land in the descriptors exactly the way protoc produces them — the
// pool knows the extension, but the linked-in google::protobuf::FieldOptions C++
// class does not, so the value sits in the options message's UnknownFieldSet.
// That is the precise shape the DynamicMessage re-parse has to cope with in
// production (see protoc/src/option_reader.cpp).
//
// EVERYTHING HERE MUST BE `inline` / header-only: all protoc unit tests link
// into ONE binary (fletcher_proto_plugin_tests), so a non-inline definition
// duplicates symbols as soon as a second .cpp includes this header.
//
// MIGRATION DEFERRED, on purpose (step-4b N-12): two near-identical private
// copies of this fixture already exist — `CollectErrors`/`FixturePool` in
// test_option_metadata.cpp and `OptCollectErrors`/`OptFixturePool` in
// test_schema_visitor.cpp. Neither was migrated here, because DICT-1's own
// regression proof is that test_option_metadata.cpp stays green UNMODIFIED.
// If you need this helper in a third place, migrate those two instead of adding
// a fourth copy.
//
// POOL TRAP: AddFletcherOptions() builds the shipped fletcher/options.proto,
// which declares `flatten_field = 50000` on FieldOptions. Protobuf refuses two
// extensions of the same extendee at the same number in one pool, so it must
// NEVER share a pool with a fixture that declares its own 50000 FieldOptions
// extension (test_option_metadata.cpp, test_schema_visitor.cpp both do).
// Sharing this header is fine; sharing a pool is not.

#include <google/protobuf/compiler/parser.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/io/tokenizer.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

namespace fletcher::test {

class ProtoTextErrors : public google::protobuf::io::ErrorCollector {
   public:
    std::string text;
    void AddError(int line, int column, const std::string& message) override {
        text += std::to_string(line) + ":" + std::to_string(column) + ": " + message + "\n";
    }
    void AddWarning(int, int, const std::string&) override {}
};

class ProtoTextPool {
   public:
    ProtoTextPool() {
        google::protobuf::FileDescriptorProto fdp;
        google::protobuf::DescriptorProto::descriptor()->file()->CopyTo(&fdp);
        seeded_ = pool_.BuildFile(fdp) != nullptr;
        EXPECT_TRUE(seeded_) << "failed to seed descriptor.proto";
    }

    // ASSERT_* cannot be used in a constructor, so callers ASSERT on this to stop
    // a failed seed from cascading into confusing BuildFile errors (N-13).
    bool seeded() const { return seeded_; }

    // Compile .proto source text under `name`.
    const google::protobuf::FileDescriptor* Add(const std::string& name,
                                                const std::string& source) {
        google::protobuf::io::ArrayInputStream input(source.data(),
                                                     static_cast<int>(source.size()));
        ProtoTextErrors errors;
        google::protobuf::io::Tokenizer tokenizer(&input, &errors);
        google::protobuf::compiler::Parser parser;
        parser.RecordErrorsTo(&errors);

        google::protobuf::FileDescriptorProto fdp;
        if (!parser.Parse(&tokenizer, &fdp)) {
            ADD_FAILURE() << "parse of " << name << " failed:\n" << errors.text;
            return nullptr;
        }
        fdp.set_name(name);
        const google::protobuf::FileDescriptor* fd = pool_.BuildFile(fdp);
        if (fd == nullptr) ADD_FAILURE() << "BuildFile(" << name << ") failed:\n" << errors.text;
        return fd;
    }

    // Build an already-assembled FileDescriptorProto (the unknown-field
    // INJECTION route: mutate options before BuildFile; DescriptorBuilder copies
    // options wholesale, so injected unknown fields survive onto
    // FieldDescriptor::options() — the pattern used in test_ir.cpp:56-61).
    const google::protobuf::FileDescriptor* Build(
        const google::protobuf::FileDescriptorProto& fdp) {
        const google::protobuf::FileDescriptor* fd = pool_.BuildFile(fdp);
        if (fd == nullptr) ADD_FAILURE() << "BuildFile(" << fdp.name() << ") failed";
        return fd;
    }

    // Copy a linked-in generated file (e.g. wrappers.proto) into the pool.
    const google::protobuf::FileDescriptor* AddLinked(
        const google::protobuf::FileDescriptor* linked) {
        if (const auto* have = pool_.FindFileByName(linked->name())) return have;
        google::protobuf::FileDescriptorProto fdp;
        linked->CopyTo(&fdp);
        const google::protobuf::FileDescriptor* fd = pool_.BuildFile(fdp);
        if (fd == nullptr) ADD_FAILURE() << "BuildFile(" << linked->name() << ") failed";
        return fd;
    }

    const google::protobuf::DescriptorPool* pool() const { return &pool_; }

   private:
    google::protobuf::DescriptorPool pool_;
    bool seeded_ = false;
};

// Compiles the SHIPPED protoc/include/fletcher/options.proto TEXT (path from the
// FLETCHER_OPTIONS_PROTO_DIR compile definition, mirroring SCHEMA_GOLDEN_DIR)
// under the name "fletcher/options.proto", so test protos can
// `import "fletcher/options.proto"`. Compiling the shipped file — rather than an
// inline copy — is what makes a test that uses it also assert spec §2 and locked
// #2: a wrong extension number, a renamed sub-field or a renumbered enum value
// fails the test.
inline const google::protobuf::FileDescriptor* AddFletcherOptions(ProtoTextPool& pool) {
    const std::string path = std::string(FLETCHER_OPTIONS_PROTO_DIR) + "/fletcher/options.proto";
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        ADD_FAILURE() << "cannot open shipped options.proto at " << path;
        return nullptr;
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    return pool.Add("fletcher/options.proto", buf.str());
}

}  // namespace fletcher::test
