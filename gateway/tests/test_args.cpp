// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "args.hpp"

namespace {

std::optional<gateway::Args> Parse(std::vector<const char*> argv, std::string& error) {
    argv.insert(argv.begin(), "gateway");
    return gateway::ParseArgs(static_cast<int>(argv.size()), argv.data(), error);
}

TEST(ParseReaderQos, DefaultProfile) {
    auto spec = gateway::ParseReaderQos("default");
    ASSERT_TRUE(spec.has_value());
    EXPECT_FALSE(spec->use_volatile);
}

TEST(ParseReaderQos, VolatileDefaultDepth) {
    auto spec = gateway::ParseReaderQos("volatile");
    ASSERT_TRUE(spec.has_value());
    EXPECT_TRUE(spec->use_volatile);
    EXPECT_EQ(spec->depth, 32);
}

TEST(ParseReaderQos, VolatileExplicitDepth) {
    auto spec = gateway::ParseReaderQos("volatile:8");
    ASSERT_TRUE(spec.has_value());
    EXPECT_TRUE(spec->use_volatile);
    EXPECT_EQ(spec->depth, 8);
}

TEST(ParseReaderQos, Rejects) {
    EXPECT_FALSE(gateway::ParseReaderQos("").has_value());
    EXPECT_FALSE(gateway::ParseReaderQos("transient").has_value());
    EXPECT_FALSE(gateway::ParseReaderQos("volatile:").has_value());
    EXPECT_FALSE(gateway::ParseReaderQos("volatile:0").has_value());
    EXPECT_FALSE(gateway::ParseReaderQos("volatile:-3").has_value());
    EXPECT_FALSE(gateway::ParseReaderQos("volatile:abc").has_value());
    EXPECT_FALSE(gateway::ParseReaderQos("volatile:8x").has_value());
}

TEST(ParseArgs, Defaults) {
    std::string error;
    auto args = Parse({}, error);
    ASSERT_TRUE(args.has_value()) << error;
    EXPECT_EQ(args->port, 9090);
    EXPECT_EQ(args->bind_address, "0.0.0.0");
    EXPECT_EQ(args->provider, "inprocess");
    EXPECT_EQ(args->domain_id, 0u);
    EXPECT_EQ(args->max_payload_bytes, 0u);
    EXPECT_FALSE(args->reader_qos.use_volatile);
}

TEST(ParseArgs, AllFlags) {
    std::string error;
    auto args =
        Parse({"--port", "19090", "--bind-address", "127.0.0.1", "--provider", "fastdds",
               "--domain-id", "151", "--max-payload-bytes", "65536", "--reader-qos", "volatile:16"},
              error);
    ASSERT_TRUE(args.has_value()) << error;
    EXPECT_EQ(args->port, 19090);
    EXPECT_EQ(args->bind_address, "127.0.0.1");
    EXPECT_EQ(args->provider, "fastdds");
    EXPECT_EQ(args->domain_id, 151u);
    EXPECT_EQ(args->max_payload_bytes, 65536u);
    EXPECT_TRUE(args->reader_qos.use_volatile);
    EXPECT_EQ(args->reader_qos.depth, 16);
}

TEST(ParseArgs, RejectsBadPayload) {
    std::string error;
    EXPECT_FALSE(Parse({"--max-payload-bytes", "0"}, error).has_value());
    EXPECT_FALSE(Parse({"--max-payload-bytes", "many"}, error).has_value());
    EXPECT_FALSE(Parse({"--max-payload-bytes", "-1"}, error).has_value());
}

TEST(ParseArgs, RejectsBadPort) {
    std::string error;
    EXPECT_FALSE(Parse({"--port", "0"}, error).has_value());
    EXPECT_FALSE(Parse({"--port", "65536"}, error).has_value());
    EXPECT_FALSE(Parse({"--port", "http"}, error).has_value());
}

TEST(ParseArgs, RejectsBadReaderQos) {
    std::string error;
    EXPECT_FALSE(Parse({"--reader-qos", "besteffort"}, error).has_value());
    EXPECT_TRUE(error.find("--reader-qos") != std::string::npos);
}

TEST(ParseArgs, RejectsUnknownProviderAndArg) {
    std::string error;
    EXPECT_FALSE(Parse({"--provider", "mqtt"}, error).has_value());
    EXPECT_FALSE(Parse({"--frobnicate"}, error).has_value());
}

TEST(ParseArgs, VersionAndHelpStopParsing) {
    std::string error;
    auto v = Parse({"--version", "--frobnicate"}, error);
    ASSERT_TRUE(v.has_value());
    EXPECT_TRUE(v->show_version);
    auto h = Parse({"--help"}, error);
    ASSERT_TRUE(h.has_value());
    EXPECT_TRUE(h->show_help);
}

}  // namespace
