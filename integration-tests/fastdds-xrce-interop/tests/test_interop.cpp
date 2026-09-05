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
// Each test uses its own XRCE session_key - a key is unique per client on
// one Agent - so the cases can run against the same Agent without their
// sessions colliding. Five cases live here: the three interop directions
// and the fixture's own two guards, `AForeignAgentDoesNotSatisfyTheHarness`
// and `AFailedOwnershipQueryDoesNotSatisfyTheHarness`.
//
// PDA-DEC-1H: the fixture proves, AT BRING-UP, that it OWNS the Agent answering the port - a
// one-shot snapshot in SetUp and not a running invariant, so an Agent that turns up after it
// is not caught. Reaching an Agent is not enough, and neither is the spawned Agent still
// running - a leftover Agent from an
// interrupted run answers the reachability probe in milliseconds while our own child, which
// lost the bind, takes tens of milliseconds to exit (28-89 ms measured; the ~0.9 s this note
// used to claim was a PowerShell wrapper artifact - see the note on
// `AForeignAgentDoesNotSatisfyTheHarness` below for the method). This fixture had no liveness check
// at ALL before this item, so a leftover satisfied it outright.
// `AForeignAgentDoesNotSatisfyTheHarness` below is the witness, and it is the same case, the same
// guard and the same refusal sentence as
// `integration-tests/pubsub-conformance/subjects/xrce_main.cpp` - see the note on
// `UdpPortOwnership` for why the two are duplicated rather than shared.

#include <arrow/api.h>
#include <gtest/gtest.h>

#include <atomic>
#include <cerrno>
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
///
/// `0xF0FF0000`, and the two high bytes matter: the probe hands out one key PER ATTEMPT and
/// the fixture's Agent routinely needs several attempts while it binds, so a base of
/// `0xF0F00000` (as first landed) walked its first three keys straight onto the three fixed
/// keys `0xF0F00001/2/3` that the interop cases use on that same Agent - a stale-client
/// collision at `create_session`. `0xF0FF0000 + (counter & 0xFFFF)` spans `0xF0FF0000`..
/// `0xF0FFFFFF`, which cannot reach them, and this file's rule that each test owns its own
/// session key holds again.
constexpr uint32_t kProbeSessionBase = 0xF0FF0000u;

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
// DUPLICATED, NOT SHARED, and deliberately - but NOT for the CI reason first recorded here.
// That reason was checked and is false: both lanes' workflows already sparse-checkout and
// path-filter `core`, `pubsub`, `fastdds-pubsub-provider` and `xrcedds-pubsub-provider`, so a
// shared header under one of those would reach both lanes and retrigger both with ZERO
// workflow edits. Struck rather than reworded, because a constraint that does not exist is
// worse than no comment.
//
// The real cost is smaller and different: the two harnesses are separate CMake+Conan projects
// that consume `xrcedds-pubsub-provider` as a PACKAGE and reach outside their own directory
// for nothing at all. Sharing means exporting a test-only header from a shipped provider
// package (an `exports_sources` entry plus an install line, shipped to every consumer) or
// giving up that self-containment. Declining that trade for ~165 duplicated lines of
// test-support code is the call taken here.
//
// The drift is therefore guarded BEHAVIOURALLY: each harness carries its own
// `AForeignAgentDoesNotSatisfyTheHarness` and its own
// `AFailedOwnershipQueryDoesNotSatisfyTheHarness`, asserting the same refusals in the same
// words, in its own CI lane. Break either copy and that copy's own tests redden.
//
// STATED IN THE TENSE IT DESERVES (compliance review F4): those lanes WILL guard the two
// copies once they run, and they have not run yet. Both integration lanes are `workflow_call`
// from the PR-triggered `ci.pr.yml`, and opening the PR is the owner's step, so this branch
// has had no CI run at all - `gh run list --branch feature/protocol-driver-abi` is empty. By
// design, not breakage, but it means the pairing is so far checked locally only, and the Linux
// /proc half below is verified by local compilation rather than by a lane: g++ 13.3 under WSL,
// `-Wall -Wextra` clean, correct verdicts on six machine states including a dead pid.
//
// What the pairing pins is "a foreign Agent is refused with operator-actionable text, and a
// failed query is refused too" - it would NOT catch the two copies producing different
// refusals, since both refusal strings contain the substrings asserted. The block below is
// byte-identical to the conformance file's, verified as such.
// -----------------------------------------------------------------
// `kNobody` is kept apart from `kSomeoneElses` deliberately: an Agent that answered a probe
// while the OS lists no holder at all would mean the table cannot be trusted, and that
// deserves its own sentence rather than being told as a leftover-Agent story.
//
// There is deliberately NO "unprovable, carry on" state. Two different things used to share
// one: a platform with no way to ask, and a platform whose query FAILED. The first is now a
// COMPILE error (the `#error` below), the second is `kQueryFailed` — a REFUSAL that names the
// OS error. An earlier revision of this guard folded both into a tolerated state that fell
// back to bare liveness and returned success, which re-admitted, through the guard's own error
// path, the exact defect the guard exists to close: a run certified without ownership proved.
enum class PortOwnership {
    kOurs,          ///< The process we spawned holds it. The only pass.
    kSomeoneElses,  ///< Some other process holds it — a leftover Agent, typically.
    kNobody,        ///< The OS records no holder at all.
    kQueryFailed,   ///< The OS could not be asked. A refusal; `*query_error` says why.
};

#ifdef _WIN32
/// The OS's own UDP table, filtered to `port`. IPv4 only: the Agent is started as `udp4`, so
/// an IPv6 row on this port could not be the endpoint the suite certifies against.
///
/// `GetExtendedUdpTable` returns the OS error code directly rather than through
/// `GetLastError()`, so `rc` IS the error a failure is reported with.
PortOwnership UdpPortOwnership(uint16_t port, int64_t our_pid, std::string* query_error) {
    // Sized, then read — and the table can grow between the two calls, hence the retry.
    for (int attempt = 0; attempt < 4; ++attempt) {
        DWORD size = 0;
        DWORD rc = GetExtendedUdpTable(nullptr, &size, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0);
        if (rc != ERROR_INSUFFICIENT_BUFFER && rc != NO_ERROR) {
            *query_error =
                "GetExtendedUdpTable failed sizing the table (error " + std::to_string(rc) + ")";
            return PortOwnership::kQueryFailed;
        }
        std::vector<unsigned char> buffer(
            size < sizeof(MIB_UDPTABLE_OWNER_PID) ? sizeof(MIB_UDPTABLE_OWNER_PID) : size);
        size = static_cast<DWORD>(buffer.size());
        rc = GetExtendedUdpTable(buffer.data(), &size, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0);
        if (rc == ERROR_INSUFFICIENT_BUFFER) {
            continue;
        }
        if (rc != NO_ERROR) {
            *query_error =
                "GetExtendedUdpTable failed reading the table (error " + std::to_string(rc) + ")";
            return PortOwnership::kQueryFailed;
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
    *query_error =
        "GetExtendedUdpTable reported ERROR_INSUFFICIENT_BUFFER on four consecutive attempts";
    return PortOwnership::kQueryFailed;
}
#elif defined(__linux__)
/// The socket inodes bound to `port`, from /proc/net/udp. IPv4 ONLY — the same row set the
/// Windows branch asks for (AF_INET) and for the same reason: the Agent is started as `udp4`,
/// so an IPv6 row on this port could not be the endpoint the suite certifies against.
///
/// /proc/net/udp6 is deliberately NOT read, and used to be. Reading it made this branch
/// strictly stricter than the Windows one while both copies' comments claimed one rule: with
/// our child holding 127.0.0.1:P and an unrelated process holding [::1]:P v6-only, Linux
/// answered kSomeoneElses where Windows answered kOurs (compliance review F1, reproduced under
/// WSL). That is a false REFUSAL and never a false pass, so nothing was ever certified
/// wrongly — but two blocks presented as one rule have to BE one rule, and a narrower guard
/// that is genuinely uniform beats a wider one that silently differs.
///
/// False only when the file could not be opened — the one case where the question could not be
/// put at all, which is a refusal and not a pass.
bool UdpPortInodes(uint16_t port, std::set<std::string>* inodes, std::string* query_error) {
    errno = 0;
    std::ifstream file("/proc/net/udp");
    if (!file) {
        *query_error = "/proc/net/udp could not be opened (errno " + std::to_string(errno) + ": " +
                       std::strerror(errno) + ")";
        return false;
    }
    std::string line;
    std::getline(file, line);  // The column header.
    while (std::getline(file, line)) {
        // sl / local_address / rem_address / st / tx_queue:rx_queue / tr:tm->when / retrnsmt /
        // uid / timeout / inode — tx-rx and tr-tm are colon-joined into one field each, so
        // `inode` is the tenth whitespace-separated token.
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
    return true;
}

/// The socket inodes `pid` has open. A pid that no longer exists holds no sockets, and that is
/// a real answer rather than an unanswerable one — a dead child cannot be the port's owner. A
/// PERMISSION refusal is the only unanswerable case, and it cannot arise for a child of this
/// process (same real uid, nothing setuid in the picture); if it somehow does, it refuses.
bool ProcessSocketInodes(int64_t pid, std::set<std::string>* inodes, std::string* query_error) {
    if (pid <= 0) {
        return true;
    }
    const std::string dir_path = "/proc/" + std::to_string(pid) + "/fd";
    DIR* dir = ::opendir(dir_path.c_str());
    if (dir == nullptr) {
        if (errno == EACCES) {
            *query_error = dir_path +
                           " could not be read (EACCES), so the sockets our own child holds "
                           "cannot be listed";
            return false;
        }
        return true;
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

PortOwnership UdpPortOwnership(uint16_t port, int64_t our_pid, std::string* query_error) {
    std::set<std::string> port_inodes;
    if (!UdpPortInodes(port, &port_inodes, query_error)) {
        return PortOwnership::kQueryFailed;
    }
    if (port_inodes.empty()) {
        return PortOwnership::kNobody;
    }
    std::set<std::string> our_inodes;
    if (!ProcessSocketInodes(our_pid, &our_inodes, query_error)) {
        return PortOwnership::kQueryFailed;
    }
    // Every socket on the port must be one of ours — the same "foreign beats ours" rule the
    // Windows branch applies, over the same row set (IPv4 only on both platforms, since the
    // v6 read went), for the same reason.
    for (const std::string& inode : port_inodes) {
        if (our_inodes.count(inode) == 0) {
            return PortOwnership::kSomeoneElses;
        }
    }
    return PortOwnership::kOurs;
}
#else
// No runtime state to fall back to means no third platform can silently lose the guard:
// porting one of the two queries above is a precondition for building this harness. This was a
// runtime `kUnprovable` that degraded to liveness and PASSED; a build-time refusal is the same
// decision, taken where somebody has to read it.
// clang-format off
#error "PDA-DEC-1H: no UDP port-ownership query for this platform. Port GetExtendedUdpTable (Windows) or /proc/net/udp (Linux) here rather than dropping the guard: this harness must not certify a run against an Agent it cannot prove it owns."
// clang-format on
#endif

/// The query above, reached through ONE indirection so a test can force the `kQueryFailed`
/// arm. That arm is this item's central deletion — an earlier revision fell back to bare
/// liveness there and PASSED — and until this seam existed nothing in either harness reached
/// it: re-introducing that fallback reddened NO test, so the refusal was safe only by
/// inspection (compliance review F3). `AFailedOwnershipQueryDoesNotSatisfyTheHarness` swaps
/// this pointer for a stub that fails, and asserts the refusal, so the fallback cannot come
/// back unnoticed. The default is the real query; that one test is the only assignment in the
/// tree, and no build option or environment variable can reach it.
using UdpPortOwnershipQuery = PortOwnership (*)(uint16_t port, int64_t our_pid,
                                                std::string* query_error);
UdpPortOwnershipQuery g_udp_port_ownership_query = &UdpPortOwnership;

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

/// The refusal when the OS query itself failed. Deliberately NOT the foreign-Agent sentence:
/// this one says ownership could not be established, which under this guard's rule is just as
/// disqualifying. Falling back to "our child is still alive" here is precisely what this item
/// measured as insufficient, so an unanswerable query fails the run out loud.
std::string UnprovenOwnershipRefusal(uint16_t port, const std::string& query_error) {
    return std::string("cannot establish which process holds UDP ") + kAgentIp + ":" +
           std::to_string(port) + " — " + query_error +
           ". The harness refuses rather than falling back to \"the Agent we spawned is still "
           "alive\": that liveness check is the measured defect this guard replaced, so a "
           "query that cannot be answered is a failed run and not a quiet one. If it persists, "
           "the OS port table is unreadable on this machine and nothing can be certified here.";
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

    ~OwnedAgent() {
        // A destructor may not ASSERT, and this one runs both from MicroXRCEAgentEnv::TearDown
        // and from the two extra Agents `AForeignAgentDoesNotSatisfyTheHarness` scopes locally.
        // ADD_FAILURE is non-fatal and throws nothing, so it is safe here and puts the reason in
        // the gtest report; the stderr copy is what survives in the ctest log if gtest's own
        // summary is cut short - which is precisely the case a stuck child produces.
        const std::string note = Kill();
        if (!note.empty()) {
            std::cerr << note << std::endl;
            ADD_FAILURE() << note;
        }
    }

    OwnedAgent(const OwnedAgent&) = delete;
    OwnedAgent& operator=(const OwnedAgent&) = delete;

    /// True only when an Agent answered `port` AND the OS records the child THIS object
    /// spawned as the holder of it. There is no third answer: a platform that cannot be asked
    /// does not compile, and a query that FAILS is a refusal like any other — so `proven()`
    /// false always carries a `failure()` sentence to print.
    ///
    /// SCOPE: this is a BRING-UP proof, not a running invariant. It is a point-in-time
    /// snapshot taken in the constructor and never re-taken, so a foreign Agent that appears
    /// AFTER it is not caught here. What makes that acceptable is that such an Agent cannot
    /// take a port our child already holds without our child dying first — not that the
    /// window is shut.
    bool proven() const { return failure_.empty(); }
    const std::string& failure() const { return failure_; }

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
        if (env_strings == nullptr) {
            // No gtest macro here. This helper runs from a CONSTRUCTOR, including the two the
            // forcing test makes, where a non-fatal failure would redden that test instead of
            // the bring-up that actually failed. The child needs nothing from our environment
            // except the augmented loader path, so hand it exactly that: an environment block
            // is "K=V\0...\0\0", hence the two terminators.
            std::string only_path = "PATH=" + new_path;
            only_path.push_back('\0');
            only_path.push_back('\0');
            return only_path;
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
        if (process_handle_ == nullptr) {
            return false;
        }
        if (WaitForSingleObject(process_handle_, 0) == WAIT_TIMEOUT) {
            return true;
        }
        // Exited, so drop the pid while keeping the handle (Kill still has to close it).
        // Windows recycles pids: a pid held past its process's death could match a stranger's
        // row in the UDP table and read back as kOurs, which is the one shape in which this
        // guard could pass wrongly. Pid 0 matches nothing.
        process_id_ = 0;
        return false;
#else
        if (pid_ <= 0) {
            return false;
        }
        int status = 0;
        const pid_t reaped = waitpid(pid_, &status, WNOHANG);
        if (reaped == 0) {
            return true;
        }
        // Reaped here, so clear it: leaving pid_ set would send Kill a SIGTERM to a pid this
        // process no longer owns and then block in a waitpid that can never return. The exit
        // note is kept, because the pid is not.
        if (reaped < 0) {
            // waitpid wrote nothing to `status` (ECHILD, say), so reading WIFEXITED off it
            // would report "exited with status 0" for a failure to ask in the first place.
            exit_note_ =
                " (exit status unknown: waitpid failed, errno " + std::to_string(errno) + ")";
        } else {
            exit_note_ = WIFEXITED(status) ? " with status " + std::to_string(WEXITSTATUS(status))
                                           : std::string(" on a signal");
        }
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
        std::string query_error;
        if (g_udp_port_ownership_query(port_, ProcessId(), &query_error) ==
            PortOwnership::kSomeoneElses) {
            return ForeignAgentRefusal(port_,
                                       "the Agent we spawned exited before it could bind and "
                                       "another process holds the port");
        }
        // The child is dead either way, so a failed query cannot turn this into a pass - but
        // it does subtract from what is known, so it is said rather than dropped.
        const std::string aside =
            query_error.empty()
                ? std::string()
                : " (and who holds the port could not be established: " + query_error + ")";
        return "the MicroXRCEAgent at " + std::string(MICRO_XRCE_AGENT_PATH) +
               " exited immediately" + exit_note_ + aside +
               ". Build it, or check the ExternalProject.";
    }

    /// The guard: an Agent answers the port, AND the OS says the process we started is the one
    /// holding it. This fixture used to check neither half. Taken once, here, at bring-up.
    std::string ProveOwnership() {
        // `kNobody` right after a probe was answered means the table has not caught up, so it
        // is given a bounded second before it is believed. The normal path answers `kOurs`
        // first time round, so it costs one table read and no wait at all.
        PortOwnership ownership = PortOwnership::kNobody;
        std::string query_error;
        const auto give_up = std::chrono::steady_clock::now() + 1s;
        do {
            ownership = g_udp_port_ownership_query(port_, ProcessId(), &query_error);
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
            case PortOwnership::kQueryFailed:
                // NOT "unknown, so proceed". This guard exists because an Agent whose
                // ownership is unproved must not certify a run, and a query that failed has
                // proved nothing. Falling back to liveness here — which an earlier revision
                // did, with one INFO line and a pass — would re-admit the defect through the
                // guard's own error path.
                return UnprovenOwnershipRefusal(port_, query_error);
        }
        // Unreachable: every enumerator returns above. Kept so that adding an enumerator
        // without handling it becomes a refusal rather than a silent pass.
        return UnprovenOwnershipRefusal(
            port_, "the port-ownership query gave an answer this guard cannot read");
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

    /// Terminate the Agent this object spawned AND reap it. Bounded on both platforms, and it
    /// SAYS SO when it cannot: the empty string means reaped, anything else is a note naming the
    /// pid and every stage that was tried.
    ///
    /// The POSIX branch used to be `kill(pid_, SIGTERM); waitpid(pid_, &status, 0);` - an
    /// UNBOUNDED blocking wait, sitting right beside a Windows branch that bounded the same wait
    /// at 5000 ms. An Agent that does not die on SIGTERM (one wedged in the kernel, one whose
    /// handler never runs, one stopped) hung this destructor FOREVER, on Linux only, in fixture
    /// teardown. That is the identical shape that cost PR #126 a cancelled 2 h 04 m job in the
    /// xrcedds provider suite; the ctest TIMEOUT this directory now carries bounds the damage but
    /// reports it as a bare timeout with no clue which call stalled, which is why the bound
    /// belongs here as well.
    ///
    /// Two rules this round has already paid for:
    ///   * Do not rely on a signal to unblock a blocked call. `close()` does not wake a blocked
    ///     `accept()` on Linux (see the TcpListener note in
    ///     xrcedds-pubsub-provider/tests/test_xrce_document.cpp), and SIGTERM is a REQUEST that a
    ///     wedged child need never honour. So every wait here is a `waitpid(WNOHANG)` poll
    ///     against a deadline - never a blocking `waitpid`, not even after SIGKILL, which is
    ///     uninterceptable but still cannot reap a task parked in an uninterruptible sleep.
    ///   * The child must be REAPED. A zombie is not a fix: it keeps the pid allocated, and this
    ///     fixture's whole ownership guard is "does the OS record OUR pid as holding the port".
    ///
    /// Sleep rather than spin between polls, for the reason `ChildProcess::Shutdown` in
    /// integration-tests/pubsub-conformance/src/child_process.cpp records: a tight
    /// `waitpid(WNOHANG)` + yield loop burned a whole core for the full budget on a hanging
    /// child, on the same loaded runner this suite has to be quick on.
    [[nodiscard]] std::string Kill() {
#ifdef _WIN32
        if (process_handle_ == nullptr) {
            return {};
        }
        const DWORD doomed = process_id_;
        std::string note;
        TerminateProcess(process_handle_, 0);
        if (WaitForSingleObject(process_handle_, 5000) != WAIT_OBJECT_0) {
            note = "MicroXRCEAgent (pid " + std::to_string(doomed) +
                   ") was still not gone 5 s after TerminateProcess. Its handle is closed here, "
                   "so it is leaked to the OS; if it still holds UDP port " +
                   std::to_string(port_) + " the next run's ProveOwnership will refuse to start.";
        }
        CloseHandle(process_handle_);
        process_handle_ = nullptr;
        process_id_ = 0;
        return note;
#else
        if (pid_ <= 0) {
            return {};
        }
        const pid_t doomed = pid_;
        // Ask, then insist. SIGTERM lets the Agent close its socket; SIGKILL cannot be declined.
        for (const int sig : {SIGTERM, SIGKILL}) {
            // An ESRCH here means it has already died and is only waiting to be collected, which
            // the poll below does. So the return value carries no decision and is discarded.
            static_cast<void>(::kill(doomed, sig));
            const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            for (;;) {
                int status = 0;
                const pid_t reaped = ::waitpid(doomed, &status, WNOHANG);
                if (reaped == doomed || (reaped < 0 && errno != EINTR)) {
                    // Collected, or unwaitable (ECHILD - Alive() already collected it). Either
                    // way this pid is no longer ours, so drop it: signalling a pid we do not own
                    // is how a recycled pid gets a stranger's process killed.
                    pid_ = -1;
                    return {};
                }
                if (std::chrono::steady_clock::now() >= give_up) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
        // Both stages spent. Deliberately NOT falling back to a blocking `waitpid` - that is the
        // defect this function exists to remove. Drop the pid so no later call can block on it,
        // and hand the caller a sentence it can act on, because an unreaped Agent still holding
        // the port is otherwise the NEXT run's mystery failure.
        pid_ = -1;
        return "MicroXRCEAgent (pid " + std::to_string(doomed) +
               ") could not be reaped: SIGTERM, then a 5 s waitpid(WNOHANG) poll, then SIGKILL, "
               "then another 5 s poll - it neither exited nor became waitable in 10 s. It is "
               "left unreaped and may still hold UDP port " +
               std::to_string(port_) +
               "; if it does, the next run's ProveOwnership will refuse to start rather than "
               "test across a stranger's Agent.";
#endif
    }

    uint16_t port_;
    std::string failure_;
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
// first one already holds logs `bind error | port: N` and EXITS - it does not stay alive. It
// gets there in TENS OF MILLISECONDS.
//
// THE ~0.9 s THIS COMMENT USED TO CLAIM WAS A MEASUREMENT ARTIFACT, corrected here rather
// than quietly dropped. It came from `Measure-Command { Start-Process -Wait }`, which times
// PowerShell's launch-and-poll wrapper: that wrapper reports ~1,025 ms for `cmd /c exit`, a
// process that does nothing, so the ~1 s was the instrument's floor and never the Agent.
// What the number IS: the child's own OS lifetime, `Process.ExitTime - Process.StartTime`
// read after `WaitForExit()`, with an incumbent holding the port - 28-89 ms over nine trials
// in two independent sessions, agreeing with the Agent's own log (2.7 ms from `bind error` to
// `server stopped`). A plausible number from the wrong instrument is worse than no number.
//
// The correction makes the defect sharper, and for THIS fixture it barely matters how wide
// the race is: before this item the fixture asked NEITHER question, so any Agent that
// answered UDP 2018 satisfied it outright and all three interop cases could certify the
// XRCE/FastDDS bridge across a leftover Agent on another DDS domain, no race required.
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

// -----------------------------------------------------------------
// A failed ownership query must refuse, not fall back (PDA-DEC-1H).
//
// The guard's other arm, and this item's central deletion: when the OS query itself FAILS, an
// earlier revision printed one INFO line, fell back to "the Agent we spawned is still alive"
// and PASSED, re-admitting through the guard's own error path the exact defect the guard
// exists to close. That deletion was proved once BY HAND — stub a failure, watch the ctest
// entry fail, revert the stub — and the compliance review (F3) pointed out that a reverted
// stub is evidence and not a guard: nothing in the tree reached `kQueryFailed`, so
// re-introducing the fallback would have reddened nothing.
//
// This is that guard. The query is reached through one function pointer
// (`g_udp_port_ownership_query`); this test points it at a stub that fails, stands up an Agent
// that IS ours on a port nothing else uses, and requires the bring-up to refuse anyway. Make
// `kQueryFailed` fall back to liveness and the first assertion reddens, because that Agent is
// genuinely alive and genuinely ours — liveness is exactly what this guard refuses to
// accept as proof. Its twin is
// `ConformanceXrce.AFailedOwnershipQueryDoesNotSatisfyTheHarness` in
// integration-tests/pubsub-conformance/subjects/xrce_main.cpp.
TEST(FastDdsXrceInteropTest, AFailedOwnershipQueryDoesNotSatisfyTheHarness) {
    // Restored by a destructor and not at the end of the body: an ASSERT below returns early,
    // and a stub left installed would fail every later bring-up in this binary.
    struct RestoreQuery {
        ~RestoreQuery() { g_udp_port_ownership_query = &UdpPortOwnership; }
    } restore_query;
    g_udp_port_ownership_query = [](uint16_t, int64_t, std::string* query_error) {
        *query_error = "the port-ownership query was forced to fail by this test";
        return PortOwnership::kQueryFailed;
    };

    OwnedAgent agent(kContestedPort);
    ASSERT_FALSE(agent.proven())
        << "ownership could not be established and the bring-up passed anyway — that is the "
           "fall-back-to-liveness this item deleted, and the Agent it would have certified is "
           "only trusted because it is alive";
    EXPECT_NE(agent.failure().find("cannot establish which process holds UDP"), std::string::npos)
        << "the refusal does not say that ownership could not be established: " << agent.failure();
    EXPECT_NE(agent.failure().find("forced to fail by this test"), std::string::npos)
        << "the refusal does not carry the query error that caused it: " << agent.failure();
    EXPECT_NE(agent.failure().find("refuses rather than falling back"), std::string::npos)
        << "the refusal does not say it declined to fall back to liveness: " << agent.failure();
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
