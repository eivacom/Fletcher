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

#include <pubsub_arrow/pubsub_arrow.hpp>
#include <fast_dds_pubsub_provider.hpp>
#include <xrce_dds_pubsub_provider.hpp>

#include <arrow/api.h>
#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
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
constexpr uint16_t   kAgentPort = 2018;

std::shared_ptr<arrow::Schema> SensorSchema() {
    return arrow::schema({
        arrow::field("sensor_id",   arrow::int32(),   false),
        arrow::field("temperature", arrow::float64(), false),
        arrow::field("label",       arrow::utf8(),    false),
    });
}

ArrowRow SensorRow(int32_t id, double temp, const std::string& label) {
    return {
        std::make_shared<arrow::Int32Scalar>(id),
        std::make_shared<arrow::DoubleScalar>(temp),
        std::make_shared<arrow::StringScalar>(label),
    };
}

XrceConfig XrceConfigFor(uint32_t session_key) {
    XrceConfig cfg{};
    cfg.transport          = XrceTransport::kUdp;
    cfg.agent_ip           = kAgentIp;
    cfg.agent_port         = kAgentPort;
    cfg.session_key        = session_key;
    // Match the FastDDS-side participants on the same DDS domain so the
    // Agent-bridged XRCE participant lands on the same bus.
    cfg.domain_id          = static_cast<uint16_t>(kDdsDomain);
    return cfg;
}

// ─────────────────────────────────────────────────────────────────────
// MicroXRCEAgent process manager — cross-platform fork / CreateProcess.
// Owns the Agent's lifetime for the entire test binary run.
// ─────────────────────────────────────────────────────────────────────
class MicroXRCEAgentEnv : public ::testing::Environment {
 public:
    void SetUp() override {
        PrependAgentLibDirToLoaderPath();
        SpawnAgent();
        WaitUntilReachable();
    }

    void TearDown() override {
        KillAgent();
    }

 private:
    void PrependAgentLibDirToLoaderPath() {
        // The Agent binary links dynamically against libmicroxrcedds_agent
        // (and friends) that are installed alongside it but not on the
        // system loader's default search path. Prepend the install lib
        // directory to the right env var before spawning so the child
        // can find them.
        const std::string lib_dir = MICRO_XRCE_AGENT_LIB_DIR;
#ifdef _WIN32
        const char* current = std::getenv("PATH");
        const std::string updated =
            lib_dir + ";" + (current ? current : "");
        _putenv_s("PATH", updated.c_str());
#else
        const char* current = std::getenv("LD_LIBRARY_PATH");
        const std::string updated =
            lib_dir + ":" + (current ? current : "");
        setenv("LD_LIBRARY_PATH", updated.c_str(), 1);
#endif
    }

    void SpawnAgent() {
        const std::string path = MICRO_XRCE_AGENT_PATH;
        const std::string port_str = std::to_string(kAgentPort);
#ifdef _WIN32
        std::string cmd = "\"" + path + "\" udp4 -p " + port_str;
        STARTUPINFOA si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                            CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &si, &pi)) {
            FAIL() << "CreateProcess failed for " << path
                   << " (GetLastError=" << GetLastError() << ")";
        }
        process_handle_ = pi.hProcess;
        CloseHandle(pi.hThread);
#else
        pid_ = fork();
        if (pid_ < 0) {
            FAIL() << "fork() failed";
        }
        if (pid_ == 0) {
            // Child: replace image with the Agent.
            const char* argv[] = {
                path.c_str(), "udp4", "-p", port_str.c_str(), nullptr};
            execv(path.c_str(), const_cast<char**>(argv));
            // execv only returns on failure.
            std::_Exit(127);
        }
#endif
    }

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
        FAIL() << "MicroXRCEAgent did not become reachable on "
               << kAgentIp << ":" << kAgentPort
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
    bool got_row = false;
    ArrowRow rx_row;

    auto fastdds = std::make_shared<FastDDSPubSubProvider>(kDdsDomain);
    auto xrce    = std::make_shared<XrceDDSPubSubProvider>(XrceConfigFor(0xF0F00001));

    PubSubArrow xrce_pub(xrce);
    PubSubArrow fastdds_sub(fastdds);

    const auto schema = SensorSchema();
    const std::vector<std::string> topic{"interop", "sensor"};

    xrce_pub.CreateTopic(topic, schema);

    auto result = fastdds_sub.Subscribe(topic, [&](ArrowRow row, Attachments) {
        std::lock_guard<std::mutex> lk(mu);
        rx_row = std::move(row);
        got_row = true;
        cv.notify_all();
    });

    ASSERT_NE(result.schema, nullptr)
        << "schema must propagate via /__schema across the Agent bridge";
    EXPECT_TRUE(result.schema->Equals(*schema, /*check_metadata=*/false));

    xrce_pub.Publish(topic, SensorRow(42, 23.5, "from-xrce"));

    {
        std::unique_lock<std::mutex> lk(mu);
        ASSERT_TRUE(cv.wait_for(lk, 10s, [&] { return got_row; }))
            << "XRCE → Agent → FastDDS delivery must complete within 10 s";
    }
    fastdds_sub.Unsubscribe(result.subscription_id);

    ASSERT_EQ(rx_row.size(), 3u);
    EXPECT_EQ(std::static_pointer_cast<arrow::Int32Scalar>(rx_row[0])->value, 42);
    EXPECT_EQ(
        std::static_pointer_cast<arrow::StringScalar>(rx_row[2])->ToString(),
        "from-xrce");
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
    bool got_row = false;
    ArrowRow rx_row;

    auto fastdds = std::make_shared<FastDDSPubSubProvider>(kDdsDomain);
    auto xrce    = std::make_shared<XrceDDSPubSubProvider>(XrceConfigFor(0xF0F00002));

    PubSubArrow fastdds_pub(fastdds);
    PubSubArrow xrce_sub(xrce);

    const auto schema = SensorSchema();
    const std::vector<std::string> topic{"interop", "command"};

    fastdds_pub.CreateTopic(topic, schema);

    auto result = xrce_sub.Subscribe(topic, [&](ArrowRow row, Attachments) {
        std::lock_guard<std::mutex> lk(mu);
        rx_row = std::move(row);
        got_row = true;
        cv.notify_all();
    });

    ASSERT_NE(result.schema, nullptr)
        << "schema must propagate via /__schema across the Agent bridge";
    EXPECT_TRUE(result.schema->Equals(*schema, /*check_metadata=*/false));

    fastdds_pub.Publish(topic, SensorRow(99, 12.5, "from-fastdds"));

    {
        std::unique_lock<std::mutex> lk(mu);
        ASSERT_TRUE(cv.wait_for(lk, 10s, [&] { return got_row; }))
            << "FastDDS → Agent → XRCE delivery must complete within 10 s";
    }
    xrce_sub.Unsubscribe(result.subscription_id);

    ASSERT_EQ(rx_row.size(), 3u);
    EXPECT_EQ(std::static_pointer_cast<arrow::Int32Scalar>(rx_row[0])->value, 99);
    EXPECT_EQ(
        std::static_pointer_cast<arrow::StringScalar>(rx_row[2])->ToString(),
        "from-fastdds");
}
