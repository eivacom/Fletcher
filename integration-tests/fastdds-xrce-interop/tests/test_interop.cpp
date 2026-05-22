// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// fastdds-pubsub-provider <-> xrcedds-pubsub-provider interop test.
//
// Drives the deployment scenario from Fletcher's architecture: an MCU
// speaks XRCE-DDS over UDP to a MicroXRCEAgent, which bridges into the
// full DDS network where a vessel workstation runs FastDDS. Neither
// provider's unit tests exercise the bridged path — this suite is the
// only place that proves topic naming, envelope serialisation, and the
// /__schema companion topic stay byte-compatible across the Agent.
//
// The MicroXRCEAgent binary is built via ExternalProject_Add in this
// directory's CMakeLists.txt and its absolute path is injected as
// `MICRO_XRCE_AGENT_PATH`. The gtest Environment fixture below spawns
// it as a child process before the test cases run and kills it on
// tear-down — there's no separate sidecar or manual prerequisite.
//
// Each test uses its own XRCE session_key so the two tests can run
// concurrently against the same Agent.

#include <arrow/api.h>
#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <fletcher/fastdds_pubsub_provider/fast_dds_pubsub_provider.hpp>
#include <fletcher/pubsub_arrow/pubsub_arrow.hpp>
#include <fletcher/xrcedds_pubsub_provider/xrce_dds_pubsub_provider.hpp>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace fletcher;
using namespace std::chrono_literals;

namespace {

constexpr uint32_t kDdsDomain = 145;
constexpr const char* kAgentIp = "127.0.0.1";
constexpr uint16_t kAgentPort = 2018;

// Schema carries both field-level and top-level Arrow KeyValueMetadata
// so the /__schema companion-topic round-trip is verified with
// check_metadata = true, not just the structural type comparison.
std::shared_ptr<arrow::Schema> SensorSchema() {
    auto temp_meta = arrow::key_value_metadata({"unit"}, {"celsius"});
    auto schema_meta = arrow::key_value_metadata({"source"}, {"fletcher-interop"});
    return arrow::schema(
        {
            arrow::field("sensor_id", arrow::int32(), false),
            arrow::field("temperature", arrow::float64(), false, temp_meta),
            arrow::field("label", arrow::utf8(), false),
        },
        schema_meta);
}

ArrowRow SensorRow(int32_t id, double temp, const std::string& label) {
    return {
        std::make_shared<arrow::Int32Scalar>(id),
        std::make_shared<arrow::DoubleScalar>(temp),
        std::make_shared<arrow::StringScalar>(label),
    };
}

// Per-sample assertion helper used by both directions: verifies all
// three field types (int32, float64, utf8) so a wire-format bug that
// silently mangles any single field would surface immediately.
void ExpectRowEquals(const ArrowRow& row, int32_t expected_id, double expected_temp,
                     const std::string& expected_label) {
    ASSERT_EQ(row.size(), 3u);
    EXPECT_EQ(std::static_pointer_cast<arrow::Int32Scalar>(row[0])->value, expected_id);
    EXPECT_DOUBLE_EQ(std::static_pointer_cast<arrow::DoubleScalar>(row[1])->value, expected_temp);
    EXPECT_EQ(std::static_pointer_cast<arrow::StringScalar>(row[2])->ToString(), expected_label);
}

XrceConfig XrceConfigFor(uint32_t session_key) {
    XrceConfig cfg{};
    cfg.transport = XrceTransport::kUdp;
    cfg.agent_ip = kAgentIp;
    cfg.agent_port = kAgentPort;
    cfg.session_key = session_key;
    // Match the FastDDS-side participants on the same DDS domain so the
    // Agent-bridged XRCE participant lands on the same bus.
    cfg.domain_id = static_cast<uint16_t>(kDdsDomain);
    return cfg;
}

// ─────────────────────────────────────────────────────────────────────
// MicroXRCEAgent process manager — cross-platform fork / CreateProcess.
// Owns the Agent's lifetime for the entire test binary run.
// ─────────────────────────────────────────────────────────────────────
class MicroXRCEAgentEnv : public ::testing::Environment {
   public:
    void SetUp() override {
        SpawnAgent();
        WaitUntilReachable();
    }

    void TearDown() override { KillAgent(); }

   private:
    // The Agent binary links dynamically against libmicroxrcedds_agent
    // (and friends) installed in MICRO_XRCE_AGENT_LIB_DIR but not on the
    // system loader's default search path. We give *only the child* an
    // augmented loader path: on POSIX via setenv inside the post-fork
    // child (the parent's env is unaffected by copy-on-write), on
    // Windows via an explicit lpEnvironment block to CreateProcessA.
    // Mutating the test binary's own env would risk perturbing later
    // dlopen()s in the same process (e.g. Arrow / FastDDS plugins that
    // would otherwise resolve against the system libraries).
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
            FAIL() << "CreateProcess failed for " << path << " (GetLastError=" << GetLastError()
                   << ")";
        }
        process_handle_ = pi.hProcess;
        CloseHandle(pi.hThread);
#else
        pid_ = fork();
        if (pid_ < 0) {
            FAIL() << "fork() failed";
        }
        if (pid_ == 0) {
            // Child: set the loader path *here* (parent's env untouched
            // thanks to fork's copy-on-write), then exec.
            const char* current = std::getenv("LD_LIBRARY_PATH");
            const std::string updated =
                std::string(MICRO_XRCE_AGENT_LIB_DIR) + ":" + (current ? current : "");
            setenv("LD_LIBRARY_PATH", updated.c_str(), 1);

            const char* argv[] = {path.c_str(), "udp4", "-p", port_str.c_str(), nullptr};
            execv(path.c_str(), const_cast<char**>(argv));
            // execv only returns on failure.
            std::_Exit(127);
        }
#endif
    }

#ifdef _WIN32
    // Build an ANSI environment block (sequence of "K=V\0K=V\0...\0\0")
    // that mirrors the parent's environment but with PATH augmented to
    // include the Agent's install lib directory. The block is handed to
    // CreateProcessA via lpEnvironment so the child sees the augmented
    // PATH without us touching our own.
    std::string BuildChildEnvBlockWithAugmentedPath() {
        std::string new_path = MICRO_XRCE_AGENT_LIB_DIR;
        if (const char* current = std::getenv("PATH")) {
            new_path += ";";
            new_path += current;
        }

        char* env_strings = GetEnvironmentStringsA();
        if (!env_strings) {
            FAIL() << "GetEnvironmentStringsA returned null";
        }

        std::string block;
        bool path_written = false;
        for (char* p = env_strings; *p != '\0';) {
            const size_t len = std::strlen(p);
            // PATH is case-insensitive on Windows.
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
        block.push_back('\0');  // double-null terminator
        return block;
    }
#endif

    void WaitUntilReachable() {
        // Probe by constructing an XRCE session. The Agent takes a few
        // hundred ms to bind its UDP socket after fork; we poll with a
        // generous deadline. connect_timeout_ms is set above 1000 so
        // the underlying retry count is >= 1 (the implementation drops
        // values below 1000 to a single attempt that is sometimes too
        // short for a freshly-started Agent).
        std::string last_error;
        auto deadline = std::chrono::steady_clock::now() + 15s;
        while (std::chrono::steady_clock::now() < deadline) {
            try {
                XrceConfig cfg = XrceConfigFor(0xF0F0FFFF);
                cfg.connect_timeout_ms = 2000;
                XrceDDSPubSubProvider probe(cfg);
                return;
            } catch (const std::exception& e) {
                last_error = e.what();
                std::this_thread::sleep_for(50ms);
            }
        }
        FAIL() << "MicroXRCEAgent did not become reachable on " << kAgentIp << ":" << kAgentPort
               << " within 15 s. Last probe error: " << last_error;
    }

    void KillAgent() {
#ifdef _WIN32
        if (process_handle_) {
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

// Register globally so the Agent is up before any test runs and torn
// down after all tests finish.
::testing::Environment* const g_agent_env =
    ::testing::AddGlobalTestEnvironment(new MicroXRCEAgentEnv);

}  // namespace

// ─────────────────────────────────────────────────────────────────────
// Forward: XRCE publishes → FastDDS subscribes. The MCU→vessel path.
// ─────────────────────────────────────────────────────────────────────
TEST(FastDdsXrceInteropTest, XrcePublishReachesFastDDSSubscriber) {
    // Capture state must outlive the providers so a late DDS callback
    // during teardown cannot touch destroyed locals.
    std::mutex mu;
    std::condition_variable cv;
    std::vector<ArrowRow> rx_rows;

    auto fastdds = std::make_shared<FastDDSPubSubProvider>(kDdsDomain);
    auto xrce = std::make_shared<XrceDDSPubSubProvider>(XrceConfigFor(0xF0F00001));

    PubSubArrow xrce_pub(xrce);
    PubSubArrow fastdds_sub(fastdds);

    const auto schema = SensorSchema();
    const std::vector<std::string> topic{"interop", "sensor"};

    xrce_pub.CreateTopic(topic, schema);

    auto result = fastdds_sub.Subscribe(topic, [&](ArrowRow row, Attachments) {
        std::lock_guard<std::mutex> lk(mu);
        rx_rows.push_back(std::move(row));
        cv.notify_all();
    });

    // /__schema must round-trip the full schema including Arrow
    // KeyValueMetadata — anything weaker would let a CDR length-prefix
    // off-by-N or schema-IPC bug slip past the test.
    ASSERT_NE(result.schema, nullptr)
        << "schema must propagate via /__schema across the Agent bridge";
    EXPECT_TRUE(result.schema->Equals(*schema, /*check_metadata=*/true));

    // Three back-to-back publishes with distinct values across all
    // three field types. Reliable QoS + KEEP_ALL guarantees in-order
    // delivery from a single writer, so order can be asserted.
    const std::vector<std::tuple<int32_t, double, std::string>> samples = {
        {1, 23.5, "from-xrce-1"},
        {42, -7.125, "from-xrce-2"},
        {999, 100.0, "from-xrce-3"},
    };
    for (const auto& [id, temp, label] : samples) {
        xrce_pub.Publish(topic, SensorRow(id, temp, label));
    }

    {
        std::unique_lock<std::mutex> lk(mu);
        ASSERT_TRUE(cv.wait_for(lk, 10s, [&] { return rx_rows.size() >= samples.size(); }))
            << "XRCE → Agent → FastDDS delivery must complete within 10 s "
               "(received "
            << rx_rows.size() << "/" << samples.size() << ")";
    }
    fastdds_sub.Unsubscribe(result.subscription_id);

    ASSERT_EQ(rx_rows.size(), samples.size());
    for (size_t i = 0; i < samples.size(); ++i) {
        const auto& [id, temp, label] = samples[i];
        SCOPED_TRACE("sample " + std::to_string(i));
        ExpectRowEquals(rx_rows[i], id, temp, label);
    }
}

// ─────────────────────────────────────────────────────────────────────
// Reverse: FastDDS publishes → XRCE subscribes. The vessel→MCU path.
// Exercises a different subscribe implementation in
// XrceDDSPubSubProvider (global on_topic callback demultiplexed by
// reader object id), so this is not redundant with the forward test.
// ─────────────────────────────────────────────────────────────────────
TEST(FastDdsXrceInteropTest, FastDDSPublishReachesXrceSubscriber) {
    std::mutex mu;
    std::condition_variable cv;
    std::vector<ArrowRow> rx_rows;

    auto fastdds = std::make_shared<FastDDSPubSubProvider>(kDdsDomain);
    auto xrce = std::make_shared<XrceDDSPubSubProvider>(XrceConfigFor(0xF0F00002));

    PubSubArrow fastdds_pub(fastdds);
    PubSubArrow xrce_sub(xrce);

    const auto schema = SensorSchema();
    const std::vector<std::string> topic{"interop", "command"};

    fastdds_pub.CreateTopic(topic, schema);

    auto result = xrce_sub.Subscribe(topic, [&](ArrowRow row, Attachments) {
        std::lock_guard<std::mutex> lk(mu);
        rx_rows.push_back(std::move(row));
        cv.notify_all();
    });

    ASSERT_NE(result.schema, nullptr)
        << "schema must propagate via /__schema across the Agent bridge";
    EXPECT_TRUE(result.schema->Equals(*schema, /*check_metadata=*/true));

    const std::vector<std::tuple<int32_t, double, std::string>> samples = {
        {99, 12.5, "from-fastdds-1"},
        {0, -0.001, "from-fastdds-2"},
        {-3, 1.0e9, "from-fastdds-3"},
    };
    for (const auto& [id, temp, label] : samples) {
        fastdds_pub.Publish(topic, SensorRow(id, temp, label));
    }

    {
        std::unique_lock<std::mutex> lk(mu);
        ASSERT_TRUE(cv.wait_for(lk, 10s, [&] { return rx_rows.size() >= samples.size(); }))
            << "FastDDS → Agent → XRCE delivery must complete within 10 s "
               "(received "
            << rx_rows.size() << "/" << samples.size() << ")";
    }
    xrce_sub.Unsubscribe(result.subscription_id);

    ASSERT_EQ(rx_rows.size(), samples.size());
    for (size_t i = 0; i < samples.size(); ++i) {
        const auto& [id, temp, label] = samples[i];
        SCOPED_TRACE("sample " + std::to_string(i));
        ExpectRowEquals(rx_rows[i], id, temp, label);
    }
}
