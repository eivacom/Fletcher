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

#include <atomic>
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
/// Session keys must be unique per client on one Agent, and every clause builds
/// a fresh client against the same long-lived Agent. A constant per subject
/// would mean 13 sequential sessions reusing one key, so each clause's
/// create_session races the previous session's teardown — which travels over
/// UDP. So keys are handed out from one counter at subject-construction time and
/// never reused within a run.
///
/// The bases keep the four roles distinguishable in an Agent log: subscriber
/// sides in 0x5151_1xxx / 0x5151_3xxx, publisher sides in 0x5151_2xxx.
constexpr uint32_t kLocalSessionBase = 0x51511000u;
constexpr uint32_t kPeerSubscriberSessionBase = 0x51513000u;
constexpr uint32_t kPeerPublisherSessionBase = 0x51512000u;
/// The probe in WaitUntilReachable, which is alive only before any subject.
constexpr uint32_t kProbeSessionKey = 0x5151FFFFu;

uint32_t NextSessionKey(uint32_t base) {
    static std::atomic<uint32_t> counter{0};
    return base + counter.fetch_add(1);
}

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
        // FAIL() only returns from the function it appears in, so SpawnAgent's
        // failure has to be re-checked here or SetUp goes on to spend the full
        // probe budget and reports the wrong thing.
        ASSERT_NO_FATAL_FAILURE(SpawnAgent());
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
        // fork() always succeeds even when the binary does not exist, so on this
        // platform a missing Agent shows up only as the child's _Exit(127).
        // Reap it here rather than letting WaitUntilReachable time out and blame
        // the port: rung 2 item 8 says the subject fails NAMING THE PATH.
        {
            int status = 0;
            const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            while (std::chrono::steady_clock::now() < give_up) {
                if (waitpid(pid_, &status, WNOHANG) == pid_) {
                    pid_ = -1;
                    FAIL() << "the MicroXRCEAgent at " << path << " exited immediately"
                           << (WIFEXITED(status)
                                   ? " with status " + std::to_string(WEXITSTATUS(status))
                                   : std::string(" on a signal"))
                           << ". Build it, or configure with -DFLETCHER_CONFORMANCE_XRCE=OFF and "
                              "know that the XRCE subjects then do not exist.";
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
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

    /// True while the Agent WE spawned is still running.
    bool SpawnedAgentAlive() {
#ifdef _WIN32
        return process_handle_ != nullptr &&
               WaitForSingleObject(process_handle_, 0) == WAIT_TIMEOUT;
#else
        if (pid_ <= 0) {
            return false;
        }
        int status = 0;
        return waitpid(pid_, &status, WNOHANG) == 0;
#endif
    }

    void WaitUntilReachable() {
        std::string last_error;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (std::chrono::steady_clock::now() < deadline) {
            try {
                XrceConfig probe_config = XrceConfigFor(kProbeSessionKey);
                probe_config.connect_timeout_ms = 2000;
                XrceDDSPubSubProvider probe(probe_config);
                // Something answered on the port. It has to be OUR Agent: a
                // leftover Agent from an interrupted run holds the same port and
                // would answer this probe happily, possibly on another DDS
                // domain, and the whole suite would then certify against it.
                ASSERT_TRUE(SpawnedAgentAlive())
                    << "UDP " << kAgentIp << ":" << kAgentPort
                    << " is answering but the MicroXRCEAgent this binary spawned ("
                    << MICRO_XRCE_AGENT_PATH
                    << ") is not running — something else already holds the port. Kill the "
                       "leftover Agent; the suite will not run against a foreign one.";
                return;
            } catch (const std::exception& e) {
                last_error = e.what();
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
        FAIL() << "the MicroXRCEAgent at " << MICRO_XRCE_AGENT_PATH
               << " did not become reachable on " << kAgentIp << ":" << kAgentPort
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
    ::testing::Values(MakeLocalSubjectFactory("XrceLocal", "xrce", SchemaMode::kCarried, [] {
        return MakeXrce(NextSessionKey(kLocalSessionBase));
    })));

INSTANTIATE_TEST_SUITE_P(
    XrceCrossProcess, ProviderConformance,
    ::testing::Values(MakePeerSubjectFactory(
        "XrceCrossProcess", "xrce", SchemaMode::kCarried,
        [] { return MakeXrce(NextSessionKey(kPeerSubscriberSessionBase)); }, CONFORMANCE_XRCE_PEER,
        // The peer's key is fixed per INSTANTIATE, so its clauses would reuse
        // one key across 13 child processes. A placeholder the factory rewrites
        // per instance is not expressible through MakePeerSubjectFactory's
        // by-value args, so the peer takes a BASE and adds the pid of the child
        // process itself — unique per child, which is what the Agent needs.
        {"--domain-id", std::to_string(kDdsDomain), "--agent-port", std::to_string(kAgentPort),
         "--session-key-base", std::to_string(kPeerPublisherSessionBase)})));

}  // namespace conformance
}  // namespace fletcher
