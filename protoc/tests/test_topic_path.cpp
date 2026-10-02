// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// Cross-backend guard: the C++ and TypeScript backends must derive the SAME topic
// for a service method. The seam's identity is the '/'-joined segment list
// (docs/pubsub-interface-spec.md §3.5), and the gateway splits a client's topic
// string back into segments on '/' (gateway/src/ws_session.cpp, SplitTopic). So
// the TS constant, split on '/', must equal the C++ TopicSegments(), and must
// equal the C++ TopicKey() byte for byte. A dotted proto package is the case
// that can make the two disagree; a single-segment package is the control.
//
// Runs the real ArrowRowGenerator over an in-memory descriptor pool and captures
// its outputs through an in-memory GeneratorContext. Touches no golden.

#include <google/protobuf/compiler/code_generator.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/empty.pb.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <regex>
#include <string>
#include <vector>

#include "generator.hpp"

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

struct Outputs {
    std::string header;
    std::string ts;
};

// `package`; message Fix { int32 id = 1; };
// service Svc { rpc Method(stream Fix) returns (google.protobuf.Empty); }
Outputs Generate(const std::string& package) {
    DescriptorPool pool;
    FileDescriptorProto empty_fdp;
    Empty::descriptor()->file()->CopyTo(&empty_fdp);
    EXPECT_NE(pool.BuildFile(empty_fdp), nullptr);

    FileDescriptorProto fdp;
    fdp.set_name("topic.proto");
    fdp.set_package(package);
    fdp.set_syntax("proto3");
    fdp.add_dependency("google/protobuf/empty.proto");
    auto* msg = fdp.add_message_type();
    msg->set_name("Fix");
    auto* f = msg->add_field();
    f->set_name("id");
    f->set_number(1);
    f->set_type(FieldDescriptorProto::TYPE_INT32);
    f->set_label(FieldDescriptorProto::LABEL_OPTIONAL);
    auto* m = fdp.add_service()->add_method();
    fdp.mutable_service(0)->set_name("Svc");
    m->set_name("Method");
    m->set_input_type("." + package + ".Fix");
    m->set_output_type(".google.protobuf.Empty");
    m->set_client_streaming(true);

    const FileDescriptor* file = pool.BuildFile(fdp);
    EXPECT_NE(file, nullptr);
    if (file == nullptr) return {};

    MemoryContext ctx;
    std::string error;
    EXPECT_TRUE(fletcher::ArrowRowGenerator().Generate(file, "ts", &ctx, &error)) << error;

    Outputs out;
    for (const auto& [name, content] : ctx.files()) {
        if (name == "topic.fletcher.pb.h") out.header = content;
        if (name == "topic.fletcher.ts") out.ts = content;
    }
    EXPECT_FALSE(out.header.empty()) << "no C++ header emitted";
    EXPECT_FALSE(out.ts.empty()) << "no TypeScript file emitted";
    return out;
}

// The first TopicSegments() initializer list in the header (publisher and
// subscriber emit the same one), as its string literals.
std::vector<std::string> CppSegments(const std::string& header) {
    std::smatch body;
    const std::regex list(R"(kSegments = \{([^}]*)\};)");
    if (!std::regex_search(header, body, list)) return {};
    std::vector<std::string> segs;
    const std::regex lit("\"([^\"]*)\"");
    const std::string inner = body[1].str();
    for (auto it = std::sregex_iterator(inner.begin(), inner.end(), lit);
         it != std::sregex_iterator(); ++it)
        segs.push_back((*it)[1].str());
    return segs;
}

std::string CppTopicKey(const std::string& header) {
    std::smatch m;
    if (!std::regex_search(header, m, std::regex(R"re(kKey = "([^"]*)";)re"))) return {};
    return m[1].str();
}

std::string TsTopic(const std::string& ts) {
    std::smatch m;
    if (!std::regex_search(ts, m, std::regex(R"(export const Svc_MethodTopic = '([^']*)';)")))
        return {};
    return m[1].str();
}

// The gateway's split: every '/' separates two segments.
std::vector<std::string> SplitOnSlash(const std::string& s) {
    std::vector<std::string> out(1);
    for (char c : s) {
        if (c == '/')
            out.emplace_back();
        else
            out.back() += c;
    }
    return out;
}

void ExpectBackendsAgree(const std::string& package) {
    const Outputs out = Generate(package);
    const auto segments = CppSegments(out.header);
    const auto key = CppTopicKey(out.header);
    const auto ts = TsTopic(out.ts);
    ASSERT_FALSE(segments.empty()) << out.header;
    ASSERT_FALSE(key.empty()) << out.header;
    ASSERT_FALSE(ts.empty()) << out.ts;

    EXPECT_EQ(ts, key) << "TS topic constant and C++ TopicKey() differ for package '" << package
                       << "'";
    EXPECT_EQ(SplitOnSlash(ts), segments)
        << "the gateway would split the TS topic '" << ts
        << "' into a different segment list than C++ TopicSegments() for package '" << package
        << "'";
}

}  // namespace

TEST(TopicPath, SingleSegmentPackageAgreesAcrossBackends) { ExpectBackendsAgree("test"); }

TEST(TopicPath, DottedPackageAgreesAcrossBackends) { ExpectBackendsAgree("eiva.nav"); }
