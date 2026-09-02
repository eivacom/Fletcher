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
//
// PDA-DEC-1H: the fixture proves it OWNS the Agent answering the port. Reaching an Agent is
// not enough, and neither is the spawned Agent still running - a leftover Agent from an
// interrupted run answers the reachability probe in milliseconds while our own child, which
// lost the bind, takes ~0.9 s to exit. This fixture had no liveness check at ALL before this
// item, so a leftover satisfied it outright. `AForeignAgentDoesNotSatisfyTheHarness` below is
// the witness, and it is the same case, the same guard and the same refusal sentence as
// `integration-tests/pubsub-conformance/subjects/xrce_main.cpp` - see the note on
// `UdpPortOwnership` for why the two are duplicated rather than shared.

#include <arrow/api.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <fletcher/core/internal/status_name.hpp>
#include <fletcher/fastdds_pubsub_provider/fast_dds_pubsub_provider.hpp>
#include <fletcher/pubsub_arrow/publisher_arrow.hpp>
#include <fletcher/pubsub_arrow/schema_import.hpp>
#include <fletcher/pubsub_arrow/subscriber_arrow.hpp>
#include <fletcher/xrcedds_pubsub_provider/xrce_dds_pubsub_provider.hpp>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#ifdef _WIN32
// clang-format off
// winsock2.h before windows.h (it has to win the WinSock version race), iphlpapi.h after both:
// GetExtendedUdpTable's MIB_UDPTABLE_OWNER_PID needs the address-family constants. Nothing
// here opens a socket, so ws2_32 is not linked - only iphlpapi is.
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
// clang-format on
#else
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifdef __linux__
#include <dirent.h>
#include <errno.h>

#include <fstream>
#include <sstream>
#endif

using namespace fletcher;
using namespace std::chrono_literals;

namespace {

// The Arrow tier no longer hands back an arrow::Schema: it hands back the seam's
// own SchemaArrival (one waiting mechanism, spec §3.4), and a caller that wants
// an arrow::Schema converts with fletcher::ImportArrowSchema — the one safe
// conversion, public precisely so nobody writes the unsafe one. Spelled out here
// once, so every call site below reads the same way.
std::shared_ptr<arrow::Schema> AwaitArrowSchema(const SchemaArrival& arrival,
                                                std::chrono::milliseconds budget) {
    SharedSchema nano;
    const PubSubStatus status = arrival.Wait(budget, &nano);
    EXPECT_EQ(status, PubSubStatus::kOk)
        << "schema arrival: " << internal::PubSubStatusName(status) << " " << arrival.Message();
    return ImportArrowSchema(nano);
}

constexpr uint32_t kDdsDomain = 145;
constexpr const char* kAgentIp = "127.0.0.1";
constexpr uint16_t kAgentPort = 2018;
/// The forcing test's own port, used by NOTHING else in the tree: 2018 is this fixture's
/// Agent, 2019 and 2119 belong to integration-tests/pubsub-conformance, and both platforms'
/// ephemeral ranges start far above this. The test needs a port it can put two Agents on
/// deliberately without disturbing the Agent the three interop cases need.
constexpr uint16_t kContestedPort = 2118;
/// The reachability probe's session-key base. A BASE and not one constant key, because the
/// forcing test stands up two more Agents and two of its probes hit the SAME Agent - reusing
/// one key there races the previous probe session's teardown over UDP.
constexpr uint32_t kProbeSessionBase = 0xF0F00000u;

uint32_t NextProbeSessionKey() {
    static std::atomic<uint32_t> counter{0};
    return kProbeSessionBase + (counter.fetch_add(1) & 0x0000FFFFu);
}

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

// PDA-DEC-7: the typed XRCE options struct is retired. What these three tests witness is the
// seam's TYPED CORE and the registered type name, not the document - they run their Agent on
// the DEFAULT port with the DEFAULT (UDP) transport, and their one distinguishing setting is
// `domain_id`. The document carries only the session key, which has to be unique per client on
// one Agent. The document's own end-to-end witness is the 24 `conformance_xrce` cases, which
// run on a non-default port and a non-default domain.
ProviderConfig XrceConfigFor(uint32_t session_key, uint16_t agent_port = kAgentPort) {
    ProviderConfig cfg;
    // Match the FastDDS-side participants on the same DDS domain so the
    // Agent-bridged XRCE participant lands on the same bus.
    cfg.domain_id = kDdsDomain;
    cfg.document = std::string("agent=") + kAgentIp + ":" + std::to_string(agent_port) +
                   "\nsession_key=" + std::to_string(session_key);
    return cfg;
}

// -----------------------------------------------------------------
// Who holds the UDP port (PDA-DEC-1H)
//
// DUPLICATED, NOT SHARED, and deliberately. Sharing this with
// `integration-tests/pubsub-conformance/subjects/xrce_main.cpp` needs a directory both
// harnesses can include from, and the two are separate CMake+Conan projects whose CI lanes
// check out disjoint sparse trees (`.github/workflows/ci.integration-test.*.yml`) with their
// own path filters in `ci.pr.yml`. A shared file would have to be added to four
// sparse-checkout blocks and two path filters, and a later edit to it that someone forgot to
// re-add would simply not retrigger the other lane - a guard you can forget to arm, which is
// the defect class this item exists to close.
//
// So the drift is guarded BEHAVIOURALLY instead, rather than by hope: each harness carries its
// own `AForeignAgentDoesNotSatisfyTheHarness`, asserting the same refusal in the same words, in
// its own CI lane. Break either copy and that copy's own test reddens. The block below was
// extracted verbatim from the conformance file, so the two start out identical.
// -----------------------------------------------------------------
enum class PortOwnership {
    kOurs,          ///< The process we spawned holds it. The only pass.
    kSomeoneElses,  ///< Some other process holds it — a leftover Agent, typically.
    kNobody,        ///< The OS records no holder at all.
    kUnprovable,    ///< This platform offers no way to ask.
};

#ifdef _WIN32
/// The OS's own UDP table, filtered to `port`. IPv4 only: the Agent is started as `udp4`, so
/// an IPv6 row on this port could not be the endpoint the suite certifies against.
PortOwnership UdpPortOwnership(uint16_t port, int64_t our_pid) {
    // Sized, then read — and the table can grow between the two calls, hence the retry.
    for (int attempt = 0; attempt < 4; ++attempt) {
        DWORD size = 0;
        DWORD rc = GetExtendedUdpTable(nullptr, &size, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0);
        if (rc != ERROR_INSUFFICIENT_BUFFER && rc != NO_ERROR) {
            return PortOwnership::kUnprovable;
        }
        std::vector<unsigned char> buffer(
            size < sizeof(MIB_UDPTABLE_OWNER_PID) ? sizeof(MIB_UDPTABLE_OWNER_PID) : size);
        size = static_cast<DWORD>(buffer.size());
        rc = GetExtendedUdpTable(buffer.data(), &size, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0);
        if (rc == ERROR_INSUFFICIENT_BUFFER) {
            continue;
        }
        if (rc != NO_ERROR) {
            return PortOwnership::kUnprovable;
        }
        const auto* table = reinterpret_cast<const MIB_UDPTABLE_OWNER_PID*>(buffer.data());
        bool ours = false;
        bool foreign = false;
        for (DWORD i = 0; i < table->dwNumEntries; ++i) {
            const MIB_UDPROW_OWNER_PID& row = table->table[i];
            // dwLocalPort carries the port in NETWORK byte order in its low two bytes. Swapped
            // by hand rather than with ntohs, so this file needs no winsock link library.
            const uint16_t row_port = static_cast<uint16_t>(((row.dwLocalPort & 0xFFu) << 8) |
                                                            ((row.dwLocalPort >> 8) & 0xFFu));
            if (row_port != port) {
                continue;
            }
            if (static_cast<int64_t>(row.dwOwningPid) == our_pid) {
                ours = true;
            } else {
                foreign = true;
            }
        }
        // Foreign beats ours: a port two processes both appear on is not a port this harness
        // can claim to own, whichever of them the datagrams actually reach. This is the case
        // the brief warned about — under SO_REUSEADDR two UDP sockets can share a port and
        // only one of them sees the traffic — and it is refused rather than guessed at.
        if (foreign) {
            return PortOwnership::kSomeoneElses;
        }
        return ours ? PortOwnership::kOurs : PortOwnership::kNobody;
    }
    return PortOwnership::kUnprovable;
}
#elif defined(__linux__)
/// The socket inodes bound to `port`, from /proc/net/udp and /proc/net/udp6. False only when
/// NEITHER file could be opened — the one case where the question is genuinely unanswerable.
bool UdpPortInodes(uint16_t port, std::set<std::string>* inodes) {
    bool read_any = false;
    for (const char* path : {"/proc/net/udp", "/proc/net/udp6"}) {
        std::ifstream file(path);
        if (!file) {
            continue;
        }
        read_any = true;
        std::string line;
        std::getline(file, line);  // The column header.
        while (std::getline(file, line)) {
            // sl / local_address / rem_address / st / tx_queue:rx_queue / tr:tm->when /
            // retrnsmt / uid / timeout / inode — tx-rx and tr-tm are colon-joined into one
            // field each, so `inode` is the tenth whitespace-separated token.
            std::istringstream fields(line);
            std::vector<std::string> token;
            std::string one;
            while (fields >> one) {
                token.push_back(one);
            }
            if (token.size() < 10) {
                continue;
            }
            const std::string& local = token[1];  // "0100007F:07E3" — the port in hex.
            const size_t colon = local.rfind(':');
            if (colon == std::string::npos) {
                continue;
            }
            if (std::strtoul(local.c_str() + colon + 1, nullptr, 16) != port) {
                continue;
            }
            inodes->insert(token[9]);
        }
    }
    return read_any;
}

/// The socket inodes `pid` has open. A pid that no longer exists holds no sockets, and that is
/// a real answer rather than an unanswerable one — a dead child cannot be the port's owner. A
/// PERMISSION refusal is the only unanswerable case, and it cannot arise for a child of this
/// process (same real uid, nothing setuid in the picture).
bool ProcessSocketInodes(int64_t pid, std::set<std::string>* inodes) {
    if (pid <= 0) {
        return true;
    }
    const std::string dir_path = "/proc/" + std::to_string(pid) + "/fd";
    DIR* dir = ::opendir(dir_path.c_str());
    if (dir == nullptr) {
        return errno != EACCES;
    }
    while (const dirent* entry = ::readdir(dir)) {
        char target[256];
        const std::string link = dir_path + "/" + entry->d_name;
        const ssize_t len = ::readlink(link.c_str(), target, sizeof(target) - 1);
        if (len <= 0) {
            continue;
        }
        target[len] = '\0';
        const std::string value(target);
        if (value.compare(0, 8, "socket:[") == 0 && value.back() == ']') {
            inodes->insert(value.substr(8, value.size() - 9));
        }
    }
    ::closedir(dir);
    return true;
}

PortOwnership UdpPortOwnership(uint16_t port, int64_t our_pid) {
    std::set<std::string> port_inodes;
    if (!UdpPortInodes(port, &port_inodes)) {
        return PortOwnership::kUnprovable;
    }
    if (port_inodes.empty()) {
        return PortOwnership::kNobody;
    }
    std::set<std::string> our_inodes;
    if (!ProcessSocketInodes(our_pid, &our_inodes)) {
        return PortOwnership::kUnprovable;
    }
    // Every socket on the port must be one of ours — the same "foreign beats ours" rule the
    // Windows branch applies, for the same reason.
    for (const std::string& inode : port_inodes) {
        if (our_inodes.count(inode) == 0) {
            return PortOwnership::kSomeoneElses;
        }
    }
    return PortOwnership::kOurs;
}
#else
PortOwnership UdpPortOwnership(uint16_t /*port*/, int64_t /*our_pid*/) {
    return PortOwnership::kUnprovable;
}
#endif

/// The one refusal an operator can act on, shared by every path that reaches it, so the
/// sentence does not depend on which platform noticed or on how it noticed.
std::string ForeignAgentRefusal(uint16_t port, const std::string& observed) {
    return std::string("UDP ") + kAgentIp + ":" + std::to_string(port) +
           " is not held by the MicroXRCEAgent this binary spawned (" + MICRO_XRCE_AGENT_PATH +
           ") — " + observed +
           ". A leftover Agent from an interrupted run answers this port and would certify the "
           "whole suite against itself, possibly on another DDS domain. Kill it (Windows: "
           "taskkill /IM MicroXRCEAgent.exe /F; POSIX: pkill MicroXRCEAgent) and re-run; the "
           "suite will not run against a foreign one.";
}

// -----------------------------------------------------------------
// MicroXRCEAgent process manager - cross-platform fork / CreateProcess.
// Owns the Agent's lifetime for the scope that created it.
//
// It reports failure as a STRING rather than through gtest's macros, because the forcing
// test below needs a bring-up that is EXPECTED to fail without that failing the test. RAII,
// not a remembered call: the destructor kills the child, so an Agent cannot outlive the scope
// that started it - the forcing test starts two and leaks neither.
// -----------------------------------------------------------------
class OwnedAgent {
   public:
    explicit OwnedAgent(uint16_t port) : port_(port) {
        failure_ = Spawn();
        if (!failure_.empty()) {
            return;
        }
        failure_ = WaitUntilReachable();
        if (!failure_.empty()) {
            return;
        }
        failure_ = ProveOwnership();
    }

    ~OwnedAgent() { Kill(); }

    OwnedAgent(const OwnedAgent&) = delete;
    OwnedAgent& operator=(const OwnedAgent&) = delete;

    /// True only when an Agent answers `port` AND the OS records the child THIS object
    /// spawned as the holder of it - or, on a platform that cannot be asked, when the child
    /// is at least still alive and `ownership_unprovable()` says so out loud.
    bool proven() const { return failure_.empty(); }
    const std::string& failure() const { return failure_; }
    bool ownership_unprovable() const { return ownership_unprovable_; }

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
    std::string Spawn() {
        const std::string path = MICRO_XRCE_AGENT_PATH;
        const std::string port_str = std::to_string(port_);
#ifdef _WIN32
        std::string env_block = BuildChildEnvBlockWithAugmentedPath();
        std::string cmd = "\"" + path + "\" udp4 -p " + port_str;
        STARTUPINFOA si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NEW_PROCESS_GROUP,
                            env_block.data(), nullptr, &si, &pi)) {
            return "CreateProcess failed for " + path +
                   " (GetLastError=" + std::to_string(GetLastError()) + ")";
        }
        process_handle_ = pi.hProcess;
        process_id_ = pi.dwProcessId;
        CloseHandle(pi.hThread);
        return {};
#else
        pid_ = fork();
        if (pid_ < 0) {
            return "fork() failed";
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
        // fork() always succeeds even when the binary does not exist, so on this platform a
        // missing Agent shows up only as the child's _Exit(127) - which used to be reported
        // here as a 15 s reachability timeout blaming the port. An Agent that LOST THE BIND to
        // a leftover lands here too, and DeadChildRefusal asks who holds the port before it
        // picks a story.
        {
            const auto give_up = std::chrono::steady_clock::now() + 1s;
            while (std::chrono::steady_clock::now() < give_up) {
                if (!Alive()) {
                    return DeadChildRefusal();
                }
                std::this_thread::sleep_for(20ms);
            }
        }
        return {};
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
            // ADD_FAILURE (not FAIL) because FAIL expands to a bare `return;`,
            // which MSVC rejects in this std::string-returning function (C2440);
            // gcc tolerates it. Record the failure, then return explicitly.
            ADD_FAILURE() << "GetEnvironmentStringsA returned null";
            return {};
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

    /// True while the Agent WE spawned is still running. Not the guard - see ProveOwnership -
    /// but what tells "nobody is there" from "somebody else is".
    bool Alive() {
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
        // Reaped here, so clear it: leaving pid_ set would send Kill a SIGTERM to a pid this
        // process no longer owns and then block in a waitpid that can never return. The exit
        // note is kept, because the pid is not.
        exit_note_ = WIFEXITED(status) ? " with status " + std::to_string(WEXITSTATUS(status))
                                       : std::string(" on a signal");
        pid_ = -1;
        return false;
#endif
    }

    int64_t ProcessId() const {
#ifdef _WIN32
        return static_cast<int64_t>(process_id_);
#else
        return static_cast<int64_t>(pid_);
#endif
    }

    /// Our Agent is gone. WHY decides what the operator should do, and the two causes are
    /// indistinguishable without asking who holds the port: a missing binary and a bind lost
    /// to a leftover Agent both surface as a child that exited at once.
    std::string DeadChildRefusal() {
        if (UdpPortOwnership(port_, ProcessId()) == PortOwnership::kSomeoneElses) {
            return ForeignAgentRefusal(port_,
                                       "the Agent we spawned exited before it could bind and "
                                       "another process holds the port");
        }
        return "the MicroXRCEAgent at " + std::string(MICRO_XRCE_AGENT_PATH) +
               " exited immediately" + exit_note_ + ". Build it, or check the ExternalProject.";
    }

    /// The guard: an Agent answers the port, AND the OS says the process we started is the one
    /// holding it. This fixture used to check neither half.
    std::string ProveOwnership() {
        // `kNobody` right after a probe was answered means the table has not caught up, so it
        // is given a bounded second before it is believed. The normal path answers `kOurs`
        // first time round, so it costs one table read and no wait at all.
        PortOwnership ownership = PortOwnership::kNobody;
        const auto give_up = std::chrono::steady_clock::now() + 1s;
        do {
            ownership = UdpPortOwnership(port_, ProcessId());
            if (ownership != PortOwnership::kNobody) {
                break;
            }
            std::this_thread::sleep_for(50ms);
        } while (std::chrono::steady_clock::now() < give_up);

        switch (ownership) {
            case PortOwnership::kOurs:
                return {};
            case PortOwnership::kSomeoneElses:
                return ForeignAgentRefusal(port_,
                                           "the OS records the port as held by another process, "
                                           "not by our child (pid " +
                                               std::to_string(ProcessId()) + ")");
            case PortOwnership::kNobody:
                return ForeignAgentRefusal(
                    port_,
                    "an Agent answered it but the OS records no process holding it at all, so "
                    "ownership cannot be established");
            case PortOwnership::kUnprovable:
                break;
        }
        // A counted, documented skip - this platform cannot answer the question, and refusing
        // anyway would fail a run with nothing wrong. Windows and Linux both answer, which is
        // every platform this project builds on; a third one gets this sentence rather than
        // silently losing the guard. Liveness is all that is left, so it is still checked, it
        // is named as the weaker thing it is, and the forcing test below counts the skip.
        ownership_unprovable_ = true;
        if (!Alive()) {
            return DeadChildRefusal();
        }
        std::cout << "[   INFO   ] UDP port ownership is unprovable on this platform (neither "
                     "GetExtendedUdpTable nor /proc/net/udp), so the Agent on "
                  << kAgentIp << ":" << port_
                  << " is only known to be ALIVE, not to be the one answering. A leftover "
                     "Agent holding this port would not be detected here."
                  << std::endl;
        return {};
    }

    std::string WaitUntilReachable() {
        // Probe by constructing an XRCE session. The Agent takes a few
        // hundred ms to bind its UDP socket after fork; we poll with a
        // generous deadline. connect_timeout_ms=2000 buys TWO ~1000 ms
        // handshake attempts, which is deliberate: one is occasionally too
        // short for an Agent that has just been forked. (Before PDA-DEC-7
        // fix cycle 1 the provider rounded the budget DOWN, so this bought
        // one attempt and anything <= 1000 bought none at all; the comment
        // here used to describe that mapping.)
        std::string last_error;
        auto deadline = std::chrono::steady_clock::now() + 15s;
        while (std::chrono::steady_clock::now() < deadline) {
            try {
                ProviderConfig cfg = XrceConfigFor(NextProbeSessionKey(), port_);
                cfg.document += "\nconnect_timeout_ms=2000";
                XrceDDSPubSubProvider probe(cfg);
                // Something answered. WHO answered is ProveOwnership's question, not this
                // loop's: a leftover Agent answers this probe happily, possibly on another
                // DDS domain, and all three interop cases would then run across its bridge.
                return {};
            } catch (const std::exception& e) {
                last_error = e.what();
            }
            // Nothing answered and our Agent is gone: no point spending the rest of the
            // budget. The same two stories as at spawn time, told apart the same way.
            if (!Alive()) {
                return DeadChildRefusal();
            }
            std::this_thread::sleep_for(50ms);
        }
        return "MicroXRCEAgent did not become reachable on " + std::string(kAgentIp) + ":" +
               std::to_string(port_) + " within 15 s. Last probe error: " + last_error;
    }

    void Kill() {
#ifdef _WIN32
        if (process_handle_) {
            TerminateProcess(process_handle_, 0);
            WaitForSingleObject(process_handle_, 5000);
            CloseHandle(process_handle_);
            process_handle_ = nullptr;
            process_id_ = 0;
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

    uint16_t port_;
    std::string failure_;
    bool ownership_unprovable_ = false;
    std::string exit_note_;
#ifdef _WIN32
    HANDLE process_handle_ = nullptr;
    DWORD process_id_ = 0;
#else
    pid_t pid_ = -1;
#endif
};

class MicroXRCEAgentEnv : public ::testing::Environment {
   public:
    void SetUp() override {
        agent_ = std::make_unique<OwnedAgent>(kAgentPort);
        ASSERT_TRUE(agent_->proven()) << agent_->failure();
    }

    void TearDown() override { agent_.reset(); }

   private:
    std::unique_ptr<OwnedAgent> agent_;
};

// Register globally so the Agent is up before any test runs and torn
// down after all tests finish.
::testing::Environment* const g_agent_env =
    ::testing::AddGlobalTestEnvironment(new MicroXRCEAgentEnv);

}  // namespace

// -----------------------------------------------------------------
// The fixture must own the Agent answering the port (PDA-DEC-1H).
//
// Measured on this project's primary platform: a second MicroXRCEAgent aimed at a port a
// first one already holds logs `bind error | port: N` and EXITS - it does not stay alive. But
// it takes ~0.9 s to get there, while the incumbent answers the reachability probe in
// milliseconds. Before this item this fixture asked NEITHER question: any Agent that answered
// UDP 2018 satisfied it, so all three interop cases could certify the XRCE/FastDDS bridge
// across a leftover Agent on another DDS domain.
//
// This runs the race deliberately. An Agent that IS ours on a port nothing else in the tree
// uses, then a second bring-up against the same port - which must refuse, and must say what an
// operator should do. Both Agents are scoped objects, so both die when this function leaves
// however it leaves: a leaked Agent is this item's own subject matter and would poison every
// later run.
//
// The twin of this case is `ConformanceXrce.AForeignAgentDoesNotSatisfyTheHarness` in
// integration-tests/pubsub-conformance/subjects/xrce_main.cpp. Same name, same assertions,
// same refusal text: that pairing is what keeps the two duplicated guards from drifting, since
// the two harnesses cannot share a file (see `UdpPortOwnership` above).
TEST(FastDdsXrceInteropTest, AForeignAgentDoesNotSatisfyTheHarness) {
    OwnedAgent incumbent(kContestedPort);
    ASSERT_TRUE(incumbent.proven())
        << "the test could not stand up its own Agent on UDP " << kContestedPort
        << ", so it proves nothing about a foreign one: " << incumbent.failure();
    if (incumbent.ownership_unprovable()) {
        // A counted, documented skip - the same shape as the leak probe in
        // xrcedds-pubsub-provider/tests/test_xrce_document.cpp. Where port ownership cannot be
        // established the bring-up falls back to liveness and would accept the incumbent by
        // design, so asserting a refusal here would be a red with nothing wrong. Windows and
        // Linux both answer, which is every platform this project builds on; the case stays
        // registered so a third one gets this sentence instead of silently losing the cover.
        GTEST_SKIP() << "UDP port ownership is unprovable on this platform (neither "
                        "GetExtendedUdpTable nor /proc/net/udp), so the bring-up cannot tell a "
                        "foreign Agent from its own and this case cannot be asserted";
    }

    OwnedAgent contender(kContestedPort);
    EXPECT_FALSE(contender.proven())
        << "the fixture certified an Agent it does not own: a foreign Agent holds UDP "
        << kContestedPort << " and the contender's own Agent could not have bound it";
    EXPECT_NE(contender.failure().find("is not held by the MicroXRCEAgent this binary spawned"),
              std::string::npos)
        << "the refusal does not say that the port belongs to something else: "
        << contender.failure();
    EXPECT_NE(contender.failure().find("Kill it"), std::string::npos)
        << "the refusal does not tell the operator what to do about it: " << contender.failure();
}

// ─────────────────────────────────────────────────────────────────────
// Forward: XRCE publishes → FastDDS subscribes. The MCU→vessel path.
// ─────────────────────────────────────────────────────────────────────
TEST(FastDdsXrceInteropTest, XrcePublishReachesFastDDSSubscriber) {
    // Capture state must outlive the providers so a late DDS callback
    // during teardown cannot touch destroyed locals.
    std::mutex mu;
    std::condition_variable cv;
    std::vector<ArrowRow> rx_rows;

    auto fastdds = std::make_shared<FastDDSPubSubProvider>(ProviderConfig{.domain_id = kDdsDomain});
    auto xrce = std::make_shared<XrceDDSPubSubProvider>(XrceConfigFor(0xF0F00001));

    PublisherArrow xrce_pub(xrce);
    SubscriberArrow fastdds_sub(fastdds);

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
    std::shared_ptr<arrow::Schema> sub_schema =
        AwaitArrowSchema(result.schema, std::chrono::seconds(15));
    ASSERT_NE(sub_schema, nullptr) << "schema must propagate via /__schema across the Agent bridge";
    EXPECT_TRUE(sub_schema->Equals(*schema, /*check_metadata=*/true));

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

    auto fastdds = std::make_shared<FastDDSPubSubProvider>(ProviderConfig{.domain_id = kDdsDomain});
    auto xrce = std::make_shared<XrceDDSPubSubProvider>(XrceConfigFor(0xF0F00002));

    PublisherArrow fastdds_pub(fastdds);
    SubscriberArrow xrce_sub(xrce);

    const auto schema = SensorSchema();
    const std::vector<std::string> topic{"interop", "command"};

    fastdds_pub.CreateTopic(topic, schema);

    auto result = xrce_sub.Subscribe(topic, [&](ArrowRow row, Attachments) {
        std::lock_guard<std::mutex> lk(mu);
        rx_rows.push_back(std::move(row));
        cv.notify_all();
    });

    std::shared_ptr<arrow::Schema> sub_schema =
        AwaitArrowSchema(result.schema, std::chrono::seconds(15));
    ASSERT_NE(sub_schema, nullptr) << "schema must propagate via /__schema across the Agent bridge";
    EXPECT_TRUE(sub_schema->Equals(*schema, /*check_metadata=*/true));

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

// ─────────────────────────────────────────────────────────────────────
// Subscriber-first: XRCE subscribes BEFORE any FastDDS publisher exists.
// Subscribe must be non-blocking (so the schema future is not yet ready)
// and must never throw when no publisher has announced the schema. Once
// the FastDDS publisher comes up — TRANSIENT_LOCAL on both the data and
// /__schema topics — the schema future resolves and the first callback
// fires with a non-null schema and the buffered rows, in order.
//
// This is the cross-language counterpart to the FastDDS provider's
// SubscribeBeforePublishDeliversWithSchema unit test: it proves the XRCE
// provider buffers data behind the async /__schema reader rather than
// blocking or dropping the early samples.
// ─────────────────────────────────────────────────────────────────────
TEST(FastDdsXrceInteropTest, XrceSubscribeBeforeFastDDSPublish) {
    std::mutex mu;
    std::condition_variable cv;
    std::vector<ArrowRow> rx_rows;

    auto fastdds = std::make_shared<FastDDSPubSubProvider>(ProviderConfig{.domain_id = kDdsDomain});
    auto xrce = std::make_shared<XrceDDSPubSubProvider>(XrceConfigFor(0xF0F00003));

    PublisherArrow fastdds_pub(fastdds);
    SubscriberArrow xrce_sub(xrce);

    const auto schema = SensorSchema();
    const std::vector<std::string> topic{"interop", "subfirst"};

    // Subscribe with no publisher present. Must return immediately without
    // throwing, and the schema future must not yet be ready.
    auto result = xrce_sub.Subscribe(topic, [&](ArrowRow row, Attachments) {
        std::lock_guard<std::mutex> lk(mu);
        rx_rows.push_back(std::move(row));
        cv.notify_all();
    });
    SharedSchema not_yet;
    EXPECT_EQ(result.schema.Wait(std::chrono::milliseconds(0), &not_yet), PubSubStatus::kPending)
        << "the schema arrival must be PENDING before any publisher announces one - not kOk with "
           "a null schema, which would mean this transport carries no schemas at all";

    // Bring up the FastDDS publisher and publish a known set of rows.
    fastdds_pub.CreateTopic(topic, schema);

    const std::vector<std::tuple<int32_t, double, std::string>> samples = {
        {7, 0.5, "subfirst-1"},
        {8, 1.5, "subfirst-2"},
        {9, 2.5, "subfirst-3"},
    };
    for (const auto& [id, temp, label] : samples) {
        fastdds_pub.Publish(topic, SensorRow(id, temp, label));
    }

    // The arrival answers once /__schema arrives — guaranteed non-null.
    std::shared_ptr<arrow::Schema> sub_schema =
        AwaitArrowSchema(result.schema, std::chrono::seconds(15));
    ASSERT_NE(sub_schema, nullptr) << "schema must propagate via /__schema across the Agent bridge";
    EXPECT_TRUE(sub_schema->Equals(*schema, /*check_metadata=*/true));

    {
        std::unique_lock<std::mutex> lk(mu);
        ASSERT_TRUE(cv.wait_for(lk, 10s, [&] { return rx_rows.size() >= samples.size(); }))
            << "subscriber-first delivery must complete within 10 s "
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
