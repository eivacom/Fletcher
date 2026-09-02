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
//
// PDA-DEC-1H: and it is not enough that the Agent we spawned is ALIVE. The guard below
// proves, AT BRING-UP, that the OS records OUR child as the holder of the port being
// certified — a one-shot snapshot in the environment's SetUp, not a running invariant, so an
// Agent that appears after it is not caught. Liveness alone was a race, measured and
// reproduced by `ConformanceXrce.AForeignAgentDoesNotSatisfyTheHarness` below, which is where
// that story is written down.

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
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "fletcher/conformance/suite.hpp"

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

namespace fletcher {
namespace conformance {
namespace {

constexpr uint16_t kAgentPort = 2019;
constexpr uint16_t kDdsDomain = 153;
constexpr const char* kAgentIp = "127.0.0.1";
/// The forcing test's own port, used by NOTHING else in the tree: 2018 is
/// integration-tests/fastdds-xrce-interop, 2019 is this suite's real Agent, and both
/// platforms' ephemeral ranges start far above this. The test needs a port it can put two
/// Agents on deliberately without disturbing the Agent the rest of the run needs.
constexpr uint16_t kContestedPort = 2119;
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
/// The reachability probe's base. A BASE and not one constant key, because the forcing test
/// stands up more than one Agent and two of its probes hit the SAME Agent — reusing one key
/// there would race the previous probe session's teardown over UDP, which is the very thing
/// the per-clause counter above exists to avoid.
constexpr uint32_t kProbeSessionBase = 0x50000000u;

uint32_t NextSessionKey(uint32_t base) {
    static std::atomic<uint32_t> counter{0};
    return base + (counter.fetch_add(1) & kSessionKeyMask);
}

// PDA-DEC-7: the typed XRCE options struct is retired. The Agent address, the session key and
// the connect budget are now lines in this provider's own `key=value` document; the DDS domain
// stays in the seam's typed core. These 24 cases are the document's end-to-end witness: with an
// Agent on 2019 and discovery on domain 153, a build that stopped reading the document would
// dial the default 127.0.0.1:2018 on domain 0 and every one of them would redden.
ProviderConfig XrceConfigFor(uint32_t session_key, int connect_timeout_ms = 5000,
                             uint16_t agent_port = kAgentPort) {
    ProviderConfig config;
    config.domain_id = kDdsDomain;
    config.document = std::string("agent=") + kAgentIp + ":" + std::to_string(agent_port) +
                      "\nsession_key=" + std::to_string(session_key) +
                      "\nconnect_timeout_ms=" + std::to_string(connect_timeout_ms);
    return config;
}

std::shared_ptr<PubSubProvider> MakeXrce(uint32_t session_key) {
    return std::make_shared<XrceDDSPubSubProvider>(XrceConfigFor(session_key));
}

// ── Who holds the UDP port ──────────────────────────────────────────
//
// The question the harness has to answer is not "is our child alive" but "is our child the
// process the OS records as the holder of the port we are about to certify". Both platforms
// this project builds on can answer it from a table they already keep — no second Agent, no
// dependency beyond a system library already on the box, no build option, and nothing an
// operator has to remember to switch on.
//
// This block is byte-identical to the one in
// `integration-tests/fastdds-xrce-interop/tests/test_interop.cpp` and is deliberately
// duplicated rather than shared. NOT for a CI reason — both lanes already sparse-checkout and
// path-filter the provider directories, so a shared header under one of them would cost no
// workflow edits at all. The reason is that these two harnesses consume
// `xrcedds-pubsub-provider` as a PACKAGE and reach outside their own directory for nothing, so
// sharing would mean exporting a test-only header from a shipped package. Drift is guarded by
// each harness carrying its own `AForeignAgentDoesNotSatisfyTheHarness` and its own
// `AFailedOwnershipQueryDoesNotSatisfyTheHarness` — which WILL guard the two copies once
// each lane runs, and has not yet: both integration lanes are `workflow_call` from the
// PR-triggered `ci.pr.yml` and this branch has had no CI run, so the pairing is checked
// locally only and the Linux /proc half is verified by local compilation (g++ 13.3 under WSL,
// `-Wall -Wextra` clean, correct verdicts on six machine states). See the longer note in the
// interop copy.
//
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

// ── The Agent, for this binary's whole run ──────────────────────────
// Lifted from integration-tests/fastdds-xrce-interop's fixture (same problem,
// same two platforms): the child gets an augmented loader path, our own
// environment is never touched.
//
// It reports failure as a STRING rather than through gtest's macros, because the forcing test
// below needs a bring-up that is EXPECTED to fail without that failing the test. RAII, not a
// remembered call: the destructor kills the child, so an Agent cannot outlive the scope that
// started it — the forcing test starts two and leaks neither.
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
            return "cannot start the MicroXRCEAgent at " + path + " (CreateProcess error " +
                   std::to_string(GetLastError()) +
                   "). Build it, or configure with -DFLETCHER_CONFORMANCE_XRCE=OFF and know "
                   "that the XRCE subjects then do not exist.";
        }
        process_handle_ = pi.hProcess;
        process_id_ = pi.dwProcessId;
        CloseHandle(pi.hThread);
        return {};
#else
        pid_ = fork();
        if (pid_ < 0) {
            return "fork() failed spawning the MicroXRCEAgent";
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
        //
        // An Agent that LOST THE BIND to a leftover also lands here, and used to be reported
        // as "build it" — advice about a binary that plainly exists. DeadChildRefusal asks
        // who holds the port before it picks a story.
        {
            const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            while (std::chrono::steady_clock::now() < give_up) {
                if (!Alive()) {
                    return DeadChildRefusal();
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
        return {};
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

    /// True while the Agent WE spawned is still running. No longer the guard — see
    /// ProveOwnership — but still what tells "nobody is there" from "somebody else is".
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
        // Reaped here, so clear it: leaving pid_ set would send KillAgent a
        // SIGTERM to a pid this process no longer owns and then block in a
        // waitpid that can never return. The exit note is kept, because the pid is not.
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
        // The child is dead either way, so a failed query cannot turn this into a pass — but
        // it does subtract from what is known, so it is said rather than dropped.
        const std::string aside =
            query_error.empty()
                ? std::string()
                : " (and who holds the port could not be established: " + query_error + ")";
        return "the MicroXRCEAgent at " + std::string(MICRO_XRCE_AGENT_PATH) +
               " exited immediately" + exit_note_ + aside +
               ". Build it, or configure with -DFLETCHER_CONFORMANCE_XRCE=OFF and know that "
               "the XRCE subjects then do not exist.";
    }

    /// The guard: an Agent answers the port, AND the OS says the process we started is the one
    /// holding it. Liveness proved neither half. Taken once, here, at bring-up.
    std::string ProveOwnership() {
        // `kNobody` right after a probe was answered means the table has not caught up, so it
        // is given a bounded second before it is believed. The normal path answers `kOurs`
        // first time round, so it costs one table read and no wait at all.
        PortOwnership ownership = PortOwnership::kNobody;
        std::string query_error;
        const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        do {
            ownership = g_udp_port_ownership_query(port_, ProcessId(), &query_error);
            if (ownership != PortOwnership::kNobody) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
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
        std::string last_error;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (std::chrono::steady_clock::now() < deadline) {
            try {
                // The suite's own document with a shorter budget on the end. Rebuilding the
                // address and session key here instead would be a second copy of the thing
                // `XrceConfigFor` exists to own (review 4b nit 5); a duplicate key is a
                // refusal, so the shorter budget has to REPLACE the factory's, which is why
                // the factory takes it as an argument.
                const ProviderConfig probe_config =
                    XrceConfigFor(NextSessionKey(kProbeSessionBase), 2000, port_);
                XrceDDSPubSubProvider probe(probe_config);
                // Something answered. WHO answered is ProveOwnership's question, not this
                // loop's — asking `Alive()` here WAS the defect: our own doomed child takes
                // TENS OF MILLISECONDS to reach its bind error and exit (measured 28-89 ms;
                // see the note on the forcing test for the method and for the ~0.9 s figure
                // this used to carry), and a leftover answers this probe inside that window,
                // so liveness was still true and the suite ran on.
                return {};
            } catch (const std::exception& e) {
                last_error = e.what();
            }
            // Nothing answered and our Agent is gone: no point spending the rest of the
            // budget. The same two stories as at spawn time, told apart the same way.
            if (!Alive()) {
                return DeadChildRefusal();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return "the MicroXRCEAgent at " + std::string(MICRO_XRCE_AGENT_PATH) +
               " did not become reachable on " + kAgentIp + ":" + std::to_string(port_) +
               " within 20 s. Last probe error: " + last_error;
    }

    void Kill() {
#ifdef _WIN32
        if (process_handle_ != nullptr) {
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
        ASSERT_NE(std::strlen(MICRO_XRCE_AGENT_PATH), 0u)
            << "MICRO_XRCE_AGENT_PATH is empty — the CMake option "
               "FLETCHER_CONFORMANCE_XRCE is ON but no Agent path was configured";
        agent_ = std::make_unique<OwnedAgent>(kAgentPort);
        ASSERT_TRUE(agent_->proven()) << agent_->failure();
    }

    void TearDown() override { agent_.reset(); }

   private:
    std::unique_ptr<OwnedAgent> agent_;
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

// -- The harness must own the Agent answering the port (PDA-DEC-1H) ----
//
// Measured on this project's primary platform before this test was written: a second
// MicroXRCEAgent aimed at a port a first one already holds logs `bind error | port: N` and
// EXITS — it does not stay alive. It gets there in TENS OF MILLISECONDS.
//
// THE ~0.9 s THIS COMMENT USED TO CLAIM WAS A MEASUREMENT ARTIFACT, corrected here rather
// than quietly dropped. It came from `Measure-Command { Start-Process -Wait }`, which times
// PowerShell's launch-and-poll wrapper: that wrapper reports ~1,025 ms for `cmd /c exit`, a
// process that does nothing, so the ~1 s was the instrument's floor and never the Agent.
// What the number IS: the child's own OS lifetime, `Process.ExitTime - Process.StartTime`
// read after `WaitForExit()`, with an incumbent holding the port — 28-89 ms over nine
// trials in two independent sessions, which agrees with the Agent's own log (2.7 ms from
// `bind error` to `server stopped`). A plausible number from the wrong instrument is worse
// than no number.
//
// The correction makes the defect SHARPER. The incumbent answers the reachability probe in
// milliseconds and the old guard's two probes were ~16 ms apart, so at the instant it asked
// `WaitForSingleObject(handle, 0)` our own doomed child was still running — true in 7 of
// those 9 trials. Both halves of a liveness guard held, and the suite would certify its
// cases against an Agent it does not own. The race is ~10-90 ms wide, not ~900 ms: a coin
// flip rather than a near-certainty, which is a materially different risk statement about
// every XRCE green reported before this item.
//
// This runs that race deliberately. An Agent that IS ours on a port nothing else in the tree
// uses, then a second bring-up against the same port — which must refuse, and must say what an
// operator should do about it. Both Agents are scoped objects, so both die when this function
// leaves however it leaves: a leaked Agent is this item's own subject matter and would poison
// every later run.
TEST(ConformanceXrce, AForeignAgentDoesNotSatisfyTheHarness) {
    OwnedAgent incumbent(kContestedPort);
    ASSERT_TRUE(incumbent.proven())
        << "the test could not stand up its own Agent on UDP " << kContestedPort
        << ", so it proves nothing about a foreign one: " << incumbent.failure();
    OwnedAgent contender(kContestedPort);
    EXPECT_FALSE(contender.proven())
        << "the bring-up certified an Agent it does not own: a foreign Agent holds UDP "
        << kContestedPort << " and the contender's own Agent could not have bound it";
    EXPECT_NE(contender.failure().find("is not held by the MicroXRCEAgent this binary spawned"),
              std::string::npos)
        << "the refusal does not say that the port belongs to something else: "
        << contender.failure();
    EXPECT_NE(contender.failure().find("Kill it"), std::string::npos)
        << "the refusal does not tell the operator what to do about it: " << contender.failure();
}

// -- A failed ownership query must refuse, not fall back (PDA-DEC-1H) --
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
// accept as proof. Its twin is `AFailedOwnershipQueryDoesNotSatisfyTheHarness` in
// integration-tests/fastdds-xrce-interop/tests/test_interop.cpp.
TEST(ConformanceXrce, AFailedOwnershipQueryDoesNotSatisfyTheHarness) {
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
