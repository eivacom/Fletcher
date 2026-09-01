// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// Subject registration: XRCE-DDS, in-process AND across a process boundary,
// both through one MicroXRCEAgent this binary owns for its whole run.
//
// Own Agent, own UDP port (2019) and own DDS domain (153), so the harness
// cannot collide with integration-tests/fastdds-xrce-interop (2018 / 145).
//
// If the Agent binary is missing, SetUp FAILS naming the path it looked at.
// It never skips: a protocol the suite cannot exercise has to be loud, or the
// suite silently certifies two providers and calls it three.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fletcher/xrcedds_pubsub_provider/xrce_dds_pubsub_provider.hpp>
#include <memory>
#include <string>
#include <thread>

#include "fletcher/conformance/suite.hpp"

#ifdef _WIN32
// clang-format off
#include <windows.h>
// clang-format on
#else
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fletcher {
namespace conformance {
namespace {

constexpr uint16_t kAgentPort = 2019;
constexpr uint16_t kDdsDomain = 153;
constexpr const char* kAgentIp = "127.0.0.1";
/// Distinct session keys: the parent's subscriber-side client and the peer
/// child's publisher-side client share one Agent.
constexpr uint32_t kLocalSessionKey = 0x51510001u;
constexpr uint32_t kPeerSubscriberSessionKey = 0x51510003u;
constexpr uint32_t kPeerPublisherSessionKey = 0x51510004u;

XrceConfig XrceConfigFor(uint32_t session_key) {
    XrceConfig config;
    config.agent_ip = kAgentIp;
    config.agent_port = kAgentPort;
    config.domain_id = kDdsDomain;
    config.session_key = session_key;
    config.connect_timeout_ms = 5000;
    return config;
}

std::shared_ptr<PubSubProvider> MakeXrce(uint32_t session_key) {
    return std::make_shared<XrceDDSPubSubProvider>(XrceConfigFor(session_key));
}

// ── The Agent, for this binary's whole run ──────────────────────────
// Lifted from integration-tests/fastdds-xrce-interop's fixture (same problem,
// same two platforms): the child gets an augmented loader path, our own
// environment is never touched.
class MicroXRCEAgentEnv : public ::testing::Environment {
   public:
    void SetUp() override {
        ASSERT_NE(std::strlen(MICRO_XRCE_AGENT_PATH), 0u)
            << "MICRO_XRCE_AGENT_PATH is empty — the CMake option "
               "FLETCHER_CONFORMANCE_XRCE is ON but no Agent path was configured";
        SpawnAgent();
        WaitUntilReachable();
    }

    void TearDown() override { KillAgent(); }

   private:
    void SpawnAgent() {
        const std::string path = MICRO_XRCE_AGENT_PATH;
        const std::string port_str = std::to_string(kAgentPort);
#ifdef _WIN32
        std::string env_block = BuildChildEnvBlockWithAugmentedPath();
        std::string cmd = "\"" + path + "\" udp4 -p " + port_str;
        STARTUPINFOA si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NEW_PROCESS_GROUP,
                            env_block.data(), nullptr, &si, &pi)) {
            FAIL() << "cannot start the MicroXRCEAgent at " << path << " (CreateProcess error "
                   << GetLastError()
                   << "). Build it, or configure with -DFLETCHER_CONFORMANCE_XRCE=OFF and know "
                      "that the XRCE subjects then do not exist.";
        }
        process_handle_ = pi.hProcess;
        CloseHandle(pi.hThread);
#else
        pid_ = fork();
        if (pid_ < 0) {
            FAIL() << "fork() failed spawning the MicroXRCEAgent";
        }
        if (pid_ == 0) {
            const char* current = std::getenv("LD_LIBRARY_PATH");
            const std::string updated =
                std::string(MICRO_XRCE_AGENT_LIB_DIR) + ":" + (current ? current : "");
            setenv("LD_LIBRARY_PATH", updated.c_str(), 1);
            const char* argv[] = {path.c_str(), "udp4", "-p", port_str.c_str(), nullptr};
            execv(path.c_str(), const_cast<char**>(argv));
            std::_Exit(127);
        }
#endif
    }

#ifdef _WIN32
    /// "K=V\0K=V\0...\0\0" mirroring our environment with PATH augmented, handed
    /// to CreateProcessA so only the child sees it.
    std::string BuildChildEnvBlockWithAugmentedPath() {
        std::string new_path = MICRO_XRCE_AGENT_LIB_DIR;
        if (const char* current = std::getenv("PATH")) {
            new_path += ";";
            new_path += current;
        }
        char* env_strings = GetEnvironmentStringsA();
        if (env_strings == nullptr) {
            ADD_FAILURE() << "GetEnvironmentStringsA returned null";
            return {};
        }
        std::string block;
        bool path_written = false;
        for (char* p = env_strings; *p != '\0';) {
            const size_t len = std::strlen(p);
            if (len >= 5 && _strnicmp(p, "PATH=", 5) == 0) {
                block.append("PATH=");
                block.append(new_path);
                path_written = true;
            } else {
                block.append(p, len);
            }
            block.push_back('\0');
            p += len + 1;
        }
        FreeEnvironmentStringsA(env_strings);
        if (!path_written) {
            block.append("PATH=");
            block.append(new_path);
            block.push_back('\0');
        }
        block.push_back('\0');
        return block;
    }
#endif

    void WaitUntilReachable() {
        std::string last_error;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (std::chrono::steady_clock::now() < deadline) {
            try {
                XrceConfig probe_config = XrceConfigFor(0x5151FFFFu);
                probe_config.connect_timeout_ms = 2000;
                XrceDDSPubSubProvider probe(probe_config);
                return;
            } catch (const std::exception& e) {
                last_error = e.what();
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
        FAIL() << "the MicroXRCEAgent did not become reachable on " << kAgentIp << ":" << kAgentPort
               << " within 20 s. Last probe error: " << last_error;
    }

    void KillAgent() {
#ifdef _WIN32
        if (process_handle_ != nullptr) {
            TerminateProcess(process_handle_, 0);
            WaitForSingleObject(process_handle_, 5000);
            CloseHandle(process_handle_);
            process_handle_ = nullptr;
        }
#else
        if (pid_ > 0) {
            kill(pid_, SIGTERM);
            int status = 0;
            waitpid(pid_, &status, 0);
            pid_ = -1;
        }
#endif
    }

#ifdef _WIN32
    HANDLE process_handle_ = nullptr;
#else
    pid_t pid_ = -1;
#endif
};

[[maybe_unused]] const bool kAgentRegistered = [] {
    ::testing::AddGlobalTestEnvironment(new MicroXRCEAgentEnv());
    return true;
}();

}  // namespace

INSTANTIATE_TEST_SUITE_P(
    XrceLocal, ProviderConformance,
    ::testing::Values(MakeLocalSubjectFactory("XrceLocal", "xrce", SchemaMode::kCarried,
                                              [] { return MakeXrce(kLocalSessionKey); })));

INSTANTIATE_TEST_SUITE_P(
    XrceCrossProcess, ProviderConformance,
    ::testing::Values(MakePeerSubjectFactory(
        "XrceCrossProcess", "xrce", SchemaMode::kCarried,
        [] { return MakeXrce(kPeerSubscriberSessionKey); }, CONFORMANCE_XRCE_PEER,
        {"--domain-id", std::to_string(kDdsDomain), "--agent-port", std::to_string(kAgentPort),
         "--session-key", std::to_string(kPeerPublisherSessionKey)})));

}  // namespace conformance
}  // namespace fletcher
