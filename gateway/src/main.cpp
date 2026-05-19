// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// fletcher-gateway — Fletcher's WebSocket gateway server.
//
// Distributed as a single executable. Has no public C++ API; the
// only supported interface is the gateway's WebSocket protocol (text
// JSON control frames + binary data frames). To run it:
//
//   gateway --port 9090 --config gateway.yml
//
// CLI:
//   --port N               TCP port to listen on (default 9090).
//   --bind-address ADDR    bind address (default 0.0.0.0).
//   --config FILE.yml      optional YAML file pre-declaring topics
//                          and their schemas (see test-config.yml in
//                          integration-tests/gateway-end-to-end/).
//                          Without a config, the gateway starts with
//                          no topics; clients must create them via
//                          the WebSocket protocol.
//
// Process lifecycle:
//   * Prints "READY <port>" on stdout once the gateway is accepting
//     connections so launchers can synchronise without polling the
//     socket.
//   * Reads stdin and exits cleanly on the literal line "stop".
//     Deterministic shutdown without relying on SIGTERM ordering
//     (Windows SIGTERM semantics differ from POSIX).
//
// Provider note:
//   The exe currently always uses an in-process loopback provider —
//   the only provider implementation gateway has. A real DDS-backed
//   provider is tracked separately; once it exists this same exe
//   will gain a `--provider TYPE` switch.

#include "gateway.hpp"

#include <pubsub/driver.hpp>
#include <pubsub/pubsub.hpp>
#include <pubsub/owned_schema.hpp>
#include <core/write_buffer.hpp>

#include <nanoarrow/nanoarrow.h>
#include <yaml-cpp/yaml.h>

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

// ─────────────────────────────────────────────────────────────────────
// InProcessProvider — the only provider implementation gateway ships
// with right now. Single callback per topic; Publish() invokes it
// synchronously, so a client publishing to a topic that has a local
// subscriber round-trips back via the gateway.
// ─────────────────────────────────────────────────────────────────────
class InProcessProvider : public fletcher::PubSub {
 public:
    void CreateTopic(const std::vector<std::string>& segments,
                     fletcher::OwnedSchema /*schema*/,
                     std::any /*config*/) override {
        std::lock_guard lock(mu_);
        topics_[Join(segments)] = {};
    }

    void Publish(const std::vector<std::string>& segments,
                 RowEncoder encoder,
                 const fletcher::Attachments& attachments) override {
        std::vector<uint8_t> buf;
        fletcher::VectorWriteBuffer wb(buf);
        encoder(wb);

        SubscribeCallback cb;
        {
            std::lock_guard lock(mu_);
            auto it = topics_.find(Join(segments));
            if (it == topics_.end()) {
                return;
            }
            cb = it->second;
        }
        if (cb) {
            cb(buf.data(), buf.size(), nullptr, attachments);
        }
    }

    fletcher::SubscriptionResult Subscribe(
        const std::vector<std::string>& segments,
        SubscribeCallback callback,
        std::any /*config*/) override {
        std::lock_guard lock(mu_);
        topics_[Join(segments)] = std::move(callback);
        return {};
    }

    void Unsubscribe(const std::vector<std::string>& segments) override {
        std::lock_guard lock(mu_);
        auto it = topics_.find(Join(segments));
        if (it != topics_.end()) {
            it->second = nullptr;
        }
    }

 private:
    std::mutex mu_;
    std::unordered_map<std::string, SubscribeCallback> topics_;

    static std::string Join(const std::vector<std::string>& segs) {
        std::string out;
        for (size_t i = 0; i < segs.size(); ++i) {
            if (i > 0) {
                out += '/';
            }
            out += segs[i];
        }
        return out;
    }
};

// ─────────────────────────────────────────────────────────────────────
// YAML schema → nanoarrow schema. Supported field types map 1:1 to
// the wire-type IDs the gateway's TS client recognises. Add to the
// switch when extending Fletcher's wire types.
// ─────────────────────────────────────────────────────────────────────
ArrowType ParseFieldType(const std::string& t) {
    if (t == "bool" || t == "boolean")  return NANOARROW_TYPE_BOOL;
    if (t == "int8")                    return NANOARROW_TYPE_INT8;
    if (t == "int16")                   return NANOARROW_TYPE_INT16;
    if (t == "int32")                   return NANOARROW_TYPE_INT32;
    if (t == "int64")                   return NANOARROW_TYPE_INT64;
    if (t == "uint8")                   return NANOARROW_TYPE_UINT8;
    if (t == "uint16")                  return NANOARROW_TYPE_UINT16;
    if (t == "uint32")                  return NANOARROW_TYPE_UINT32;
    if (t == "uint64")                  return NANOARROW_TYPE_UINT64;
    if (t == "float32" || t == "float") return NANOARROW_TYPE_FLOAT;
    if (t == "float64" || t == "double") return NANOARROW_TYPE_DOUBLE;
    if (t == "utf8"    || t == "string") return NANOARROW_TYPE_STRING;
    if (t == "binary")                  return NANOARROW_TYPE_BINARY;
    throw std::runtime_error("Unknown field type in config: " + t);
}

fletcher::OwnedSchema BuildSchemaFromYaml(const YAML::Node& fields) {
    fletcher::OwnedSchema schema;
    ArrowSchemaInit(schema.get());
    ArrowSchemaSetTypeStruct(schema.get(), static_cast<int64_t>(fields.size()));
    for (size_t i = 0; i < fields.size(); ++i) {
        const auto name = fields[i]["name"].as<std::string>();
        const auto type = fields[i]["type"].as<std::string>();
        ArrowSchemaSetName(schema->children[i], name.c_str());
        ArrowSchemaSetType(schema->children[i], ParseFieldType(type));
    }
    return schema;
}

// Split a "foo/bar/baz" topic name on '/' into the segments vector
// the Driver API expects.
std::vector<std::string> SplitTopicName(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == '/') {
            if (!cur.empty()) {
                out.push_back(std::move(cur));
                cur.clear();
            }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) {
        out.push_back(std::move(cur));
    }
    return out;
}

struct Args {
    std::string bind_address = "0.0.0.0";
    uint16_t    port = 9090;
    std::string config;
};

Args ParseArgs(int argc, char* argv[]) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            a.port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (arg == "--bind-address" && i + 1 < argc) {
            a.bind_address = argv[++i];
        } else if (arg == "--config" && i + 1 < argc) {
            a.config = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            std::printf(
                "Usage: %s [--port N] [--bind-address ADDR] [--config FILE.yml]\n",
                argv[0]);
            std::exit(0);
        } else {
            std::fprintf(stderr, "fletcher-gateway: unknown argument: %s\n",
                         arg.c_str());
            std::exit(2);
        }
    }
    return a;
}

void LoadTopicsFromConfig(fletcher::Driver& driver, const std::string& path) {
    YAML::Node cfg = YAML::LoadFile(path);
    if (!cfg["topics"]) {
        return;
    }
    for (const auto& t : cfg["topics"]) {
        const auto name = t["name"].as<std::string>();
        driver.CreateTopic(SplitTopicName(name),
                           BuildSchemaFromYaml(t["fields"]));
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    Args args;
    try {
        args = ParseArgs(argc, argv);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "fletcher-gateway: bad CLI: %s\n", e.what());
        return 2;
    }

    auto provider = std::make_shared<InProcessProvider>();
    auto driver = std::make_shared<fletcher::Driver>(provider);

    if (!args.config.empty()) {
        try {
            LoadTopicsFromConfig(*driver, args.config);
        } catch (const std::exception& e) {
            std::fprintf(stderr,
                         "fletcher-gateway: failed to load config '%s': %s\n",
                         args.config.c_str(), e.what());
            return 1;
        }
    }

    fletcher::GatewayOptions opts;
    opts.address = args.bind_address;
    opts.port    = args.port;

    fletcher::Gateway gw(driver, opts);
    gw.Start();

    std::printf("READY %u\n", args.port);
    std::fflush(stdout);

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "stop") {
            break;
        }
    }

    gw.Stop();
    return 0;
}
