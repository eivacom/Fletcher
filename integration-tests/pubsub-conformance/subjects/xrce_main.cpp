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
#include <fletcher/core/write_buffer.hpp>
#include <fletcher/pubsub/provider_registry.hpp>
#include <fletcher/xrcedds_pubsub_provider/xrce_dds_pubsub_provider.hpp>
#include <memory>
#include <string>
#include <thread>
#include <vector>

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
/// One byte per role in the top octet, 24 bits of room underneath. The peer
/// child derives its key from its own pid, and a 12-bit slice of that collided
/// on any 4096-multiple pid gap — which is one wrap of the pid space, not a
/// remote possibility on a long-lived runner. 24 bits covers Linux's default
/// 22-bit pid_max outright and every practical Windows pid.
constexpr uint32_t kLocalSessionBase = 0x51000000u;
constexpr uint32_t kPeerSubscriberSessionBase = 0x52000000u;
constexpr uint32_t kPeerPublisherSessionBase = 0x53000000u;
/// The registry case's own base (`Registry.XrceResolvesAsABuiltIn`), so it cannot reuse
/// a key any conformance clause is holding on the same Agent.
constexpr uint32_t kRegistrySessionBase = 0x54000000u;
/// The mask the peer applies to its pid; also the ceiling on a parent-side
/// counter, though only ~13 of those are handed out per run.
constexpr uint32_t kSessionKeyMask = 0x00FFFFFFu;
/// The probe in WaitUntilReachable, alive only before any subject exists.
constexpr uint32_t kProbeSessionKey = 0x50FFFFFFu;

uint32_t NextSessionKey(uint32_t base) {
    static std::atomic<uint32_t> counter{0};
    return base + (counter.fetch_add(1) & kSessionKeyMask);
}

// PDA-DEC-7: the typed XRCE options struct is retired. The Agent address, the session key and
// the connect budget are now lines in this provider's own `key=value` document; the DDS domain
// stays in the seam's typed core. These 24 cases are the document's end-to-end witness: with an
// Agent on 2019 and discovery on domain 153, a build that stopped reading the document would
// dial the default 127.0.0.1:2018 on domain 0 and every one of them would redden.
ProviderConfig XrceConfigFor(uint32_t session_key) {
    ProviderConfig config;
    config.domain_id = kDdsDomain;
    config.document = std::string("agent=") + kAgentIp + ":" + std::to_string(kAgentPort) +
                      "\nsession_key=" + std::to_string(session_key) + "\nconnect_timeout_ms=5000";
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
        if (waitpid(pid_, &status, WNOHANG) == 0) {
            return true;
        }
        // Reaped here, so clear it: leaving pid_ set would send KillAgent a
        // SIGTERM to a pid this process no longer owns and then block in a
        // waitpid that can never return.
        pid_ = -1;
        return false;
#endif
    }

    void WaitUntilReachable() {
        std::string last_error;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (std::chrono::steady_clock::now() < deadline) {
            try {
                ProviderConfig probe_config = XrceConfigFor(kProbeSessionKey);
                probe_config.document = std::string("agent=") + kAgentIp + ":" +
                                        std::to_string(kAgentPort) +
                                        "\nsession_key=" + std::to_string(kProbeSessionKey) +
                                        "\nconnect_timeout_ms=2000";
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

// -- XRCE resolves as a built-in NAME (spec section 4 clause 4) --------
//
// This test lives HERE and not in `conformance_registry`, for the reason the Fast DDS twin
// gives (`subjects/fastdds_main.cpp`): that binary's link line is deliberately narrow - it
// names `fletcher-pubsub` and NO transport SDK, so no DDS or XRCE vocabulary resolves from
// there - and linking the XRCE client into it to register one provider would destroy exactly
// the guard the narrowness IS. This binary already links the provider, already owns an Agent
// and already holds a RESOURCE_LOCK, so the test is free here and destructive there.
//
// What it asserts is the only claim `RegisterXrceProvider` makes: the name "xrce" resolves
// through `ProviderRegistry::Create` - the SAME call a driver path will go through in PDA-ABI -
// and what comes back delivers a row through a base-typed handle. Nothing below names
// `XrceDDSPubSubProvider`; register it under any other name (M9) and this reddens as an unknown
// selector.
TEST(Registry, XrceResolvesAsABuiltIn) {
    ProviderRegistry registry;
    RegisterXrceProvider(registry);

    const ProviderConfig config = XrceConfigFor(NextSessionKey(kRegistrySessionBase));

    std::shared_ptr<PubSubProvider> provider =
        registry.Create(ProviderSelector::Parse("xrce"), config);
    ASSERT_NE(provider, nullptr) << "\"xrce\" did not resolve to a provider";

    const std::vector<std::string> topic{"registry", "xrce-probe"};
    provider->CreateTopic(topic, MakeConformanceSchema(SchemaId::kA));

    std::vector<uint8_t> received;
    std::atomic<bool> delivered{false};
    SubscriptionResult result = provider->Subscribe(
        topic, [&](const uint8_t* data, size_t len, const SharedSchema&, const Attachments&) {
            received.assign(data, data + len);
            delivered.store(true);
        });

    // XRCE carries the schema on its companion __schema topic, so this is a real wait.
    SharedSchema schema;
    ASSERT_EQ(result.schema.Wait(std::chrono::seconds(20), &schema), PubSubStatus::kOk)
        << result.schema.Message();
    ASSERT_NE(schema, nullptr);

    provider->Publish(topic, [](WriteBuffer& buffer) {
        buffer.AppendByte(0x17);
        buffer.AppendByte('R');
        buffer.AppendByte('O');
        buffer.AppendByte('W');
    });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!delivered.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(delivered.load()) << "the row never reached the subscriber";
    // Written out as a literal rather than by running the encoder again: a guard that compares
    // a buffer with itself asserts nothing.
    EXPECT_EQ(received, (std::vector<uint8_t>{0x17, 'R', 'O', 'W'}))
        << "the delivered bytes are not what was published";
}

}  // namespace conformance
}  // namespace fletcher

// Own main rather than gtest_main: SIGPIPE's disposition has to be set before
// any thread exists, and this binary spawns a peer child whose death must reach
// a clause as a typed failure rather than as exit=141.
int main(int argc, char** argv) {
    fletcher::conformance::IgnoreSigPipeOnce();
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
