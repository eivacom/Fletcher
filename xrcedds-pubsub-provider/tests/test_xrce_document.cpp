// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The XRCE provider's configuration document (PDA-DEC-7): `key=value`, one setting per line.
//
// NO AGENT IN ANY ROW. Six of these eight cases touch nothing but a pure function; the forcing
// case owns a plain TCP listening socket and the two constructor-refusal cases fail before or
// during transport init. That is deliberate: the FORMAT is guarded here, in the provider's own
// CI, and the TRANSPORT is guarded by the 24 Agent-gated `conformance_xrce` cases, which are
// now document-configured and would all redden if the document stopped reaching the client.
//
// Suite name is `XrceConfig` (the runbook's inner loop is `ctest -R '^XrceConfig\.'`), which is
// also the name of the struct this item retired - the tests outlive the type.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fletcher/core/status.hpp>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "fletcher/xrcedds_pubsub_provider/xrce_dds_pubsub_provider.hpp"
#include "internal/xrce_document.hpp"

#ifdef _WIN32
// clang-format off
#include <winsock2.h>
#include <ws2tcpip.h>
// clang-format on
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace fletcher;
using fletcher::internal::ParseXrceDocument;
using fletcher::internal::XrceSettings;
using fletcher::internal::XrceTransportKind;

namespace {

// The provider README, by absolute path from `target_compile_definitions` (the tree's convention
// for a test that reads a source-controlled artefact). `PublishedDefaultsAreExact` reads the
// published default document out of it instead of holding a copy, so the README's drift claim is
// enforced rather than asserted by hand. Baking the block in at CONFIGURE time (`file(READ)` /
// `configure_file`) would re-create exactly the held-copy defect PDA-DEC-6 paid a fix cycle for,
// so it is forbidden rather than tested: the read happens at RUN time, off disk.
#ifndef FLETCHER_XRCE_README_PATH
#error "FLETCHER_XRCE_README_PATH is not defined; see tests/CMakeLists.txt"
#endif
constexpr const char* kReadmePath = FLETCHER_XRCE_README_PATH;

std::string ReadWholeFile(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

// The first ``` fenced block after `marker`. Empty if either is missing - the caller asserts, so
// a renamed heading is a red test and never a silently skipped one.
std::string ExtractFencedBlockAfter(const std::string& text, const std::string& marker) {
    const size_t at = text.find(marker);
    if (at == std::string::npos) return {};
    const size_t fence = text.find("```", at);
    if (fence == std::string::npos) return {};
    const size_t body = text.find('\n', fence);
    if (body == std::string::npos) return {};
    const size_t close = text.find("\n```", body);
    if (close == std::string::npos) return {};
    return text.substr(body + 1, close - body - 1);
}

ProviderConfig ConfigWith(std::string document) {
    ProviderConfig config;
    config.document = std::move(document);
    return config;
}

// The status a `PubSubError` came out with, plus its message - both, because "it threw" is what
// the two retired constructor tests asserted and it is not enough: `PubSubError` derives from
// `std::runtime_error`, so `EXPECT_THROW(..., std::runtime_error)` stays green even if the
// provider goes back to throwing untyped and every refusal arrives at the caller as `kInternal`
// through `TranslateSeamFailure`.
struct Refusal {
    bool threw = false;
    bool typed = false;
    PubSubStatus status = PubSubStatus::kOk;
    std::string message;
};

template <typename Fn>
Refusal Catch(Fn&& fn) {
    Refusal out;
    try {
        fn();
    } catch (const PubSubError& e) {
        out.threw = true;
        out.typed = true;
        out.status = e.status();
        out.message = e.what();
    } catch (const std::exception& e) {
        out.threw = true;
        out.message = e.what();
    }
    return out;
}

void ExpectRefused(const std::string& document, PubSubStatus expected_status,
                   const std::string& quoted_entry) {
    const Refusal refusal = Catch([&] { ParseXrceDocument(ConfigWith(document)); });
    ASSERT_TRUE(refusal.threw) << "this document was ACCEPTED: [" << document << "]";
    ASSERT_TRUE(refusal.typed) << "refused with an untyped exception, which reaches a caller as "
                                  "kInternal through TranslateSeamFailure: "
                               << refusal.message;
    EXPECT_EQ(refusal.status, expected_status) << refusal.message;
    // The offending entry has to be IN the message: an operator with a fifty-line document and
    // a diagnostic that says only "bad document" has to bisect it by hand.
    EXPECT_NE(refusal.message.find("\"" + quoted_entry + "\""), std::string::npos)
        << "the refusal does not quote the offending entry [" << quoted_entry
        << "]: " << refusal.message;
}

// ---------------------------------------------------------------------------------------------
// A test-owned TCP listener on an EPHEMERAL port.
//
// This is what makes the forcing case falsifiable, and it is structural rather than clever: the
// port is chosen by the kernel at run time, so it is no default of Fletcher's, of the XRCE
// client's or of an Agent's, and NO build can hard-code its way to an accept. A build that
// ignores the document, or reads the host and keeps port 2018, or reads the address but stays on
// UDP, cannot reach this socket.
//
// It relies on one substrate fact, confirmed against Micro XRCE-DDS Client v3.0.1 before this
// test was written: `uxr_init_tcp_transport` -> `uxr_init_tcp_platform` performs a blocking
// `connect()` during INIT (`tcp_transport_windows.c` / `tcp_transport_posix.c`) and returns
// false when every candidate address fails. There is no lazy-connect machinery in this client.
// ---------------------------------------------------------------------------------------------
class TcpListener {
   public:
    TcpListener() {
#ifdef _WIN32
        WSADATA wsa_data;
        wsa_ = (WSAStartup(MAKEWORD(2, 2), &wsa_data) == 0);
        if (!wsa_) return;
#endif
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (!Valid(fd_)) return;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;  // the kernel picks; nothing in any build knows this number
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return;
        if (::listen(fd_, 4) != 0) return;

        sockaddr_in bound{};
#ifdef _WIN32
        int len = static_cast<int>(sizeof(bound));
#else
        socklen_t len = sizeof(bound);
#endif
        if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&bound), &len) != 0) return;
        port_ = ntohs(bound.sin_port);

        // The accept runs on its own thread and LATCHES. The provider's connect and the
        // accept race by nature, so the test asserts against the latch with a deadline rather
        // than calling accept at a moment of its choosing.
        thread_ = std::thread([this] {
            while (!stop_.load(std::memory_order_relaxed)) {
                const auto client = ::accept(fd_, nullptr, nullptr);
                if (!Valid(client)) break;  // the listening socket was closed: we are done
                accepted_.fetch_add(1, std::memory_order_relaxed);
                // Held, not closed: closing immediately would let the client's connect()
                // succeed and its first send() fail, which is a different story from "nothing
                // behind this socket speaks XRCE".
                clients_.push_back(client);
            }
        });
    }

    ~TcpListener() {
        stop_.store(true, std::memory_order_relaxed);
        if (Valid(fd_)) CloseOne(fd_);  // unblocks accept()
        if (thread_.joinable()) thread_.join();
        for (const auto client : clients_) CloseOne(client);
#ifdef _WIN32
        if (wsa_) WSACleanup();
#endif
    }

    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;

    bool ok() const { return Valid(fd_) && port_ != 0; }
    uint16_t port() const { return port_; }
    int accepted() const { return accepted_.load(std::memory_order_relaxed); }

    // Wait for the latch, up to `budget`. Returns as soon as a connection has arrived.
    bool WaitForAccept(std::chrono::milliseconds budget) const {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (std::chrono::steady_clock::now() < deadline) {
            if (accepted() > 0) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return accepted() > 0;
    }

   private:
#ifdef _WIN32
    using Socket = SOCKET;
    static bool Valid(Socket s) { return s != INVALID_SOCKET; }
    static void CloseOne(Socket s) { ::closesocket(s); }
    Socket fd_ = INVALID_SOCKET;
    bool wsa_ = false;
#else
    using Socket = int;
    static bool Valid(Socket s) { return s >= 0; }
    static void CloseOne(Socket s) { ::close(s); }
    Socket fd_ = -1;
#endif
    uint16_t port_ = 0;
    std::atomic<bool> stop_{false};
    std::atomic<int> accepted_{0};
    std::vector<Socket> clients_;
    std::thread thread_;
};

}  // namespace

// =============================================================================================
// FORCING TEST - the document reaches the transport, observed through a real socket
// =============================================================================================
//
// Row 1 carries the falsifiability alone. The mutations it reddens, each measured through the
// mechanism it uses rather than asserted:
//   M1  ignore the document entirely  -> the client dials UDP 127.0.0.1:2018, the listener
//                                        never accepts -> RED. (This is how red-for-the-right-
//                                        reason was established for this item.)
//   M3  read `agent`'s host but keep port 2018 -> ephemeral port never reached -> RED.
//   M10 read `agent` but stay on `transport=udp` -> no TCP connect at all -> RED.
//   C2-2 read the document correctly but pass a hard-coded 3000 ms to
//        `uxr_create_session_retries` -> the accept still happens, so the wall-clock bound below
//        is the ONLY thing that sees it: 3000 ms means 2 attempts of ~1000 ms each, while
//        `connect_timeout_ms=0` means 0 retries, i.e. one send and an immediate `false`
//        (`wait_session_status`, session.c:742-746). Measured cost of the correct build is a
//        few milliseconds; the bound is 1000 ms, which is ~20x headroom over the truth and 2x
//        clear of the mutation.
//
// Row 2 (empty document) is a HARNESS CONTROL, not a build guard, and is not claimed to be one:
// no build can be reddened by it, because the ephemeral port cannot be hard-coded. Its jobs are
// (a) prove row 1's accept latch is not stale and holds no cross-row state, and (b) carry H2's
// only witness - an empty document means every published default, so it must NOT be refused as
// `kInvalidArgument` (review C2-3). It is environment-sensitive by nature: an Agent on UDP 2018
// changes what it DOES (construction succeeds) but not what it asserts.
TEST(XrceConfig, DocumentConfiguresTransport) {
    TcpListener listener;
    ASSERT_TRUE(listener.ok()) << "could not open a loopback TCP listener for the test";

    // -- Row 1: the document names THIS socket, and the client dials it -----------------------
    const std::string document =
        "transport=tcp\nagent=127.0.0.1:" + std::to_string(listener.port()) +
        "\nconnect_timeout_ms=0";

    const auto started = std::chrono::steady_clock::now();
    const Refusal refusal = Catch([&] { XrceDDSPubSubProvider provider(ConfigWith(document)); });
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);

    EXPECT_TRUE(listener.WaitForAccept(std::chrono::seconds(5)))
        << "nothing ever connected to 127.0.0.1:" << listener.port()
        << ", which the document named - so the document did not reach the transport";

    // Then: the constructor fails, because nothing behind that socket speaks XRCE. Typed, not
    // merely "it threw".
    ASSERT_TRUE(refusal.threw) << "the constructor SUCCEEDED against a socket that speaks no "
                                  "XRCE, which cannot happen";
    ASSERT_TRUE(refusal.typed) << refusal.message;
    EXPECT_EQ(refusal.status, PubSubStatus::kTransportFailure) << refusal.message;

    // C2-2's guard: the deadline is the operator's, not a constant in the constructor.
    EXPECT_LT(elapsed.count(), 1000)
        << "connect_timeout_ms=0 means one attempt with no wait, so this must be near-instant; "
           "~2000 ms means the constructor is using its own hard-coded budget instead of the "
           "document's. Elapsed: "
        << elapsed.count() << " ms";

    // -- Row 2: the harness control ------------------------------------------------------------
    const int accepted_after_row1 = listener.accepted();
    const Refusal empty = Catch([&] { XrceDDSPubSubProvider provider(ConfigWith("")); });

    // H2: an empty document is every published default, never a refusal. If it fails at all it
    // fails at the TRANSPORT (no Agent on the default UDP port); if an Agent happens to be
    // listening on 127.0.0.1:2018 it succeeds outright, and H2 holds even more plainly.
    if (empty.threw) {
        ASSERT_TRUE(empty.typed) << empty.message;
        EXPECT_EQ(empty.status, PubSubStatus::kTransportFailure)
            << "an empty document must mean every published default (spec 4.1, H2), so it can "
               "only fail at the transport - never as kInvalidArgument: "
            << empty.message;
    }
    EXPECT_EQ(listener.accepted(), accepted_after_row1)
        << "the defaults are UDP 127.0.0.1:2018, so an empty document must not reach this "
           "listener; row 1's latch is stale or something is keeping cross-row state";
}

// =============================================================================================
// The reader boundary, closed WHOLE-STRUCT
// =============================================================================================
//
// A refusal table only stops a reader that fails to refuse. It says nothing about one that
// accepts a value, range-checks it and then drops it on the floor - the defect class the
// 2026-09-02 "whole quality-of-service" ruling's review note names ("must mandate the form and
// assert it, or this ruling is unfalsifiable"), and the one that cost PDA-DEC-6 a cycle.
//
// So: one document setting ALL FOUR keys away from their defaults, compared field-for-field
// against the expected `XrceSettings`. `operator==` is defaulted, so the comparison is total
// over the fields that exist and M11 (range-check a key, then use a hard-coded value) reddens in
// exactly the field that was dropped. M12 (assign `connect_timeout_ms` from the wrong source)
// reddens too, and a wrong-TYPE source no longer compiles at all, because the one duration left
// is `std::chrono::milliseconds` rather than a plain integer.
//
// What this row does NOT do, said plainly (review C2-4): it does not police GROWTH. A fifth
// field a future parser forgets to assign would compare default-against-default here and stay
// green. What polices growth is the rule that a key arrives WITH its witness.
TEST(XrceConfig, EveryKeySetNonDefaultLandsWholeStruct) {
    const XrceSettings parsed = ParseXrceDocument(ConfigWith(
        "transport=tcp\nagent=10.1.2.3:7401\nsession_key=305419896\nconnect_timeout_ms=250"));

    XrceSettings expected;
    expected.transport = XrceTransportKind::kTcp;
    expected.agent_host = "10.1.2.3";
    expected.agent_port = 7401;
    expected.session_key = 0x12345678u;  // 305419896
    expected.connect_timeout = std::chrono::milliseconds(250);

    // Every field differs from the default, so no field can pass by accident.
    EXPECT_TRUE(parsed == expected)
        << "a document setting all four keys did not land whole. transport="
        << static_cast<int>(parsed.transport) << " agent=" << parsed.agent_host << ":"
        << parsed.agent_port << " session_key=" << parsed.session_key
        << " connect_timeout_ms=" << parsed.connect_timeout.count();
    EXPECT_FALSE(parsed == XrceSettings{})
        << "the expectation is not distinguishable from the defaults, so this row proves nothing";
}

// =============================================================================================
// The connect budget, converted where it can be pinned: ms -> whole ~1000 ms attempts
// =============================================================================================
//
// This row exists because the ENDS of a range are not the range. Before fix cycle 1 the
// conversion was `floor((ms - 1) / 1000)` and lived in the constructor, where nothing without a
// socket could see it; the only value ever tested was `0`, where floor and ceil agree. Everything
// from 1 to 1000 ms therefore bought ZERO attempts - and 0 attempts is not "one quick attempt",
// it is `wait_session_status` sending one datagram and returning without ever listening
// (session.c:742-746), after which `uxr_create_session_retries` reports failure unconditionally.
// So a third of the published 0-60000 range could never connect to a healthy Agent, and the
// diagnostic said "is the Agent running?" while it was (review 4b B1 / 4a F3).
//
// Every interior row below is red under that mapping: 1, 250, 500, 999 and 1000 all mapped to 0,
// and 1001, 1500, 3000 and 60000 were each one attempt short - the published default spent
// ~2000 ms of its 3000 ms budget. `0` is the one value both mappings agree on, and it stays
// legal: "send once, do not wait" is a useful thing to ask for, which is why the fix is a ceiling
// rather than a refusal of 1..1000.
TEST(XrceConfig, ConnectTimeoutBudgetBuysWholeAttempts) {
    using fletcher::internal::SessionAttempts;
    using std::chrono::milliseconds;

    // 0 is the only budget that may map to 0 attempts, because 0 attempts cannot connect.
    EXPECT_EQ(SessionAttempts(milliseconds(0)), 0u)
        << "connect_timeout_ms=0 means send once and do not wait - the one value that is allowed "
           "to be unable to connect";

    // The interior. Under the old floor mapping every one of these was one attempt too few, and
    // the first four were zero.
    EXPECT_EQ(SessionAttempts(milliseconds(1)), 1u);
    EXPECT_EQ(SessionAttempts(milliseconds(250)), 1u);
    EXPECT_EQ(SessionAttempts(milliseconds(500)), 1u);
    EXPECT_EQ(SessionAttempts(milliseconds(999)), 1u);
    EXPECT_EQ(SessionAttempts(milliseconds(1000)), 1u);
    EXPECT_EQ(SessionAttempts(milliseconds(1001)), 2u);
    EXPECT_EQ(SessionAttempts(milliseconds(1500)), 2u);
    EXPECT_EQ(SessionAttempts(milliseconds(2000)), 2u);
    EXPECT_EQ(SessionAttempts(milliseconds(3000)), 3u) << "the PUBLISHED DEFAULT: a 3000 ms "
                                                          "budget must spend three attempts, not "
                                                          "the two the old mapping bought";
    EXPECT_EQ(SessionAttempts(milliseconds(59999)), 60u);
    EXPECT_EQ(SessionAttempts(milliseconds(60000)), 60u) << "the top of the published range";

    // Not a hidden second rule: nothing in 1..60000 may buy zero attempts. Stated as a property
    // over the whole range rather than only at the rows above, because the defect this replaces
    // was exactly "a value inside the range that the rows happened to miss".
    for (int64_t ms = 1; ms <= 60000; ++ms) {
        ASSERT_GE(SessionAttempts(milliseconds(ms)), 1u)
            << "connect_timeout_ms=" << ms
            << " is accepted by the reader but buys no attempt, so it can never connect";
        ASSERT_EQ(SessionAttempts(milliseconds(ms)), static_cast<size_t>((ms + 999) / 1000))
            << "connect_timeout_ms=" << ms << " does not round up to whole attempts";
    }

    // And the reader's own range check is what keeps this function's input in that band, so the
    // two belong together: above 60000 never reaches it.
    ExpectRefused("connect_timeout_ms=60001", PubSubStatus::kInvalidArgument,
                  "connect_timeout_ms=60001");
}

// =============================================================================================
// Every refusal, typed and quoting the offending entry (rung-2 cases 9-13)
// =============================================================================================
//
// M4 - accept-and-default any single key - reddens that key's row. This half stops a reader that
// fails to refuse; the accept-and-discard half is the whole-struct row above.
TEST(XrceConfig, DocumentRefusalsAreTypedAndQuoted) {
    // -- Structure -----------------------------------------------------------------------------
    ExpectRefused("agent", PubSubStatus::kInvalidArgument, "agent");
    ExpectRefused("transport", PubSubStatus::kInvalidArgument, "transport");
    ExpectRefused("agent=127.0.0.1:2018\nagent=127.0.0.1:2019", PubSubStatus::kInvalidArgument,
                  "agent=127.0.0.1:2019");
    ExpectRefused("stream_history=8", PubSubStatus::kInvalidArgument, "stream_history=8");
    ExpectRefused("run_loop_ms=50", PubSubStatus::kInvalidArgument, "run_loop_ms=50");
    ExpectRefused("max_payload=512", PubSubStatus::kInvalidArgument, "max_payload=512");
    ExpectRefused("serial_device=COM3", PubSubStatus::kInvalidArgument, "serial_device=COM3");
    ExpectRefused("agent_ip=127.0.0.1", PubSubStatus::kInvalidArgument, "agent_ip=127.0.0.1");

    // -- Values --------------------------------------------------------------------------------
    ExpectRefused("transport=udp4", PubSubStatus::kInvalidArgument, "transport=udp4");
    ExpectRefused("agent=127.0.0.1", PubSubStatus::kInvalidArgument, "agent=127.0.0.1");
    ExpectRefused("agent=127.0.0.1:2018:9", PubSubStatus::kInvalidArgument,
                  "agent=127.0.0.1:2018:9");
    ExpectRefused("agent=:2018", PubSubStatus::kInvalidArgument, "agent=:2018");
    ExpectRefused("agent=", PubSubStatus::kInvalidArgument, "agent=");
    ExpectRefused("agent=127.0.0.1:", PubSubStatus::kInvalidArgument, "agent=127.0.0.1:");
    ExpectRefused("agent=127.0.0.1:0", PubSubStatus::kInvalidArgument, "agent=127.0.0.1:0");
    ExpectRefused("agent=127.0.0.1:65536", PubSubStatus::kInvalidArgument, "agent=127.0.0.1:65536");
    ExpectRefused("agent=127.0.0.1:20a8", PubSubStatus::kInvalidArgument, "agent=127.0.0.1:20a8");
    // Wide-then-check, never narrow: 67554 is 2018 + 65536, so a `uint16_t` parse would accept
    // it as port 2018 and connect to the right port for the wrong reason.
    ExpectRefused("agent=127.0.0.1:67554", PubSubStatus::kInvalidArgument, "agent=127.0.0.1:67554");
    ExpectRefused("connect_timeout_ms=60001", PubSubStatus::kInvalidArgument,
                  "connect_timeout_ms=60001");
    ExpectRefused("connect_timeout_ms=-1", PubSubStatus::kInvalidArgument, "connect_timeout_ms=-1");
    ExpectRefused("session_key=4294967296", PubSubStatus::kInvalidArgument,
                  "session_key=4294967296");
    ExpectRefused("session_key=0xAABBCCDD", PubSubStatus::kInvalidArgument,
                  "session_key=0xAABBCCDD");

    // -- An embedded NUL, refused up front -----------------------------------------------------
    // The seam's document is length-authoritative (spec 4.2) and carries a NUL unchanged, so one
    // can legitimately arrive here; this provider's format has no representation for it, and the
    // refusal message could not quote it anyway (it travels through `what()` -> `c_str()`).
    const std::string with_nul = std::string("transport=udp\0agent=1.2.3.4:5", 29);
    const Refusal nul_refusal = Catch([&] { ParseXrceDocument(ConfigWith(with_nul)); });
    ASSERT_TRUE(nul_refusal.threw) << "a document containing a NUL was accepted";
    ASSERT_TRUE(nul_refusal.typed) << nul_refusal.message;
    EXPECT_EQ(nul_refusal.status, PubSubStatus::kInvalidArgument) << nul_refusal.message;
    EXPECT_NE(nul_refusal.message.find("NUL"), std::string::npos) << nul_refusal.message;

    // -- The typed core: refused by the CONSTRUCTOR, and before any I/O ------------------------
    // Both were `std::invalid_argument` before this item, which reaches a caller as `kInternal`
    // through a registry factory. Neither row can touch a socket: validation completes first,
    // which is also why they cost microseconds rather than a connect timeout.
    ProviderConfig wide_domain;
    wide_domain.domain_id = 65536;
    const Refusal domain = Catch([&] { XrceDDSPubSubProvider provider(wide_domain); });
    ASSERT_TRUE(domain.threw) << "domain_id 65536 was accepted; it does not fit the XRCE wire";
    ASSERT_TRUE(domain.typed) << domain.message;
    EXPECT_EQ(domain.status, PubSubStatus::kInvalidArgument) << domain.message;
    EXPECT_NE(domain.message.find("65536"), std::string::npos) << domain.message;

    ProviderConfig odd_bound;
    odd_bound.max_payload_bytes = 4097;  // not a multiple of 4
    const Refusal bound = Catch([&] { XrceDDSPubSubProvider provider(odd_bound); });
    ASSERT_TRUE(bound.threw) << "max_payload_bytes 4097 was accepted";
    ASSERT_TRUE(bound.typed) << bound.message;
    EXPECT_EQ(bound.status, PubSubStatus::kInvalidArgument) << bound.message;
}

// =============================================================================================
// Tolerance: the same rules as the loopback's reader, because there is one FORMAT
// =============================================================================================
//
// Spec 4.1 (as landed by PDA-DEC-5) is the single tolerance oracle for both in-tree `key=value`
// readers, and this row is the XRCE half of the pin; `pubsub`'s loopback tests are the other.
// The two readers are deliberately separate CODE (locked decision 8 - a shared one would put a
// config parser in Fletcher, or add a component the <75 KB Flash target must link for sixty
// lines), so this is how their drift is bounded.
//
// M5 - add trimming, case folding or comment support - reddens here.
TEST(XrceConfig, ToleranceRulesMatchTheLoopback) {
    // Accepted: CRLF entries, blank lines, a trailing newline. All three occur in a document an
    // operator authored on this project's primary platform, and the same text must mean the same
    // thing in every build.
    XrceSettings crlf;
    ASSERT_NO_THROW(crlf = ParseXrceDocument(ConfigWith(
                        "transport=tcp\r\n\r\nagent=127.0.0.1:2019\r\nconnect_timeout_ms=1\r\n")));
    EXPECT_EQ(crlf.transport, XrceTransportKind::kTcp);
    EXPECT_EQ(crlf.agent_host, "127.0.0.1");
    EXPECT_EQ(crlf.agent_port, 2019);
    EXPECT_EQ(crlf.connect_timeout, std::chrono::milliseconds(1));

    // An empty document is the defaults, not an error (H2).
    EXPECT_TRUE(ParseXrceDocument(ConfigWith("")) == XrceSettings{});
    EXPECT_TRUE(ParseXrceDocument(ConfigWith("\n\r\n\n")) == XrceSettings{});

    // ONE whitespace rule, and it refuses (fix cycle 1, review 4b S2): any byte below 0x21
    // INSIDE an entry. Every row below used to be decided by a DIFFERENT check - the key lookup,
    // the decimal parse - and one of them was not decided at all: `agent= 127.0.0.1:2018` was
    // accepted with the space kept in the host and failed a layer down in the client's resolver,
    // which is the one component this provider deliberately knows nothing about (H1). The state
    // is now unrepresentable rather than documented, so these rows are one rule rather than four
    // coincidences.
    ExpectRefused(" agent=127.0.0.1:2018", PubSubStatus::kInvalidArgument, " agent=127.0.0.1:2018");
    ExpectRefused("agent =127.0.0.1:2018", PubSubStatus::kInvalidArgument, "agent =127.0.0.1:2018");
    ExpectRefused("agent= 127.0.0.1:2018", PubSubStatus::kInvalidArgument, "agent= 127.0.0.1:2018");
    ExpectRefused("agent=127.0.0.1 :2018", PubSubStatus::kInvalidArgument, "agent=127.0.0.1 :2018");
    ExpectRefused("agent=127.0.0.1:2018 ", PubSubStatus::kInvalidArgument, "agent=127.0.0.1:2018 ");
    ExpectRefused("transport=tcp\t", PubSubStatus::kInvalidArgument, "transport=tcp\t");
    ExpectRefused("session_key= 7", PubSubStatus::kInvalidArgument, "session_key= 7");
    // A CR that is NOT the trailing one: the strip is a line-terminator tolerance, not a licence
    // to put a control byte in the middle of an address.
    ExpectRefused("agent=127.0.0.1\r:2018", PubSubStatus::kInvalidArgument,
                  "agent=127.0.0.1\r:2018");
    // ...and the SEPARATORS keep working, which is what this rule is careful not to touch: the
    // CRLF document accepted above already proves the trailing strip, and a blank-only document
    // is still the defaults.
    EXPECT_TRUE(ParseXrceDocument(ConfigWith("transport=tcp\r\n")).transport ==
                XrceTransportKind::kTcp);

    // No case folding.
    ExpectRefused("AGENT=127.0.0.1:2018", PubSubStatus::kInvalidArgument, "AGENT=127.0.0.1:2018");
    ExpectRefused("transport=TCP", PubSubStatus::kInvalidArgument, "transport=TCP");

    // No comments. A `#` line is not a comment, it is an entry with no `=`.
    ExpectRefused("# the agent\nagent=127.0.0.1:2018", PubSubStatus::kInvalidArgument,
                  "# the agent");
    ExpectRefused("agent=127.0.0.1:2018 # the agent", PubSubStatus::kInvalidArgument,
                  "agent=127.0.0.1:2018 # the agent");
}

// =============================================================================================
// The README's published defaults are the code's defaults - read OFF DISK
// =============================================================================================
//
// The README publishes the full default document as the operator's copy-paste starting point.
// This reads that block out of README.md at RUN time and parses it: it must come out equal,
// whole-struct, to a default-constructed `XrceSettings`. So editing EITHER the README block or a
// default in the code, alone, turns this red (M6) - including a key's spelling.
//
// It never skips. An unreadable README is a HARD failure naming the path, because a test that
// silently skips when its subject is missing is how the fastdds equivalent's first version
// managed to assert nothing under `conan create` (README.md was not in `exports_sources`; it is
// in this package's now, for the same reason).
TEST(XrceConfig, PublishedDefaultsAreExact) {
    const std::string readme = ReadWholeFile(kReadmePath);
    ASSERT_FALSE(readme.empty())
        << "could not read the provider README at " << kReadmePath
        << " - this test compares the README's published defaults against the code, so it cannot "
           "run without it (the path arrives via target_compile_definitions, and README.md is "
           "exported by conanfile.py so the cache build reads the same file the repository holds)";

    const std::string published =
        ExtractFencedBlockAfter(readme, "#### The published default document");
    ASSERT_FALSE(published.empty())
        << "found no fenced block under '#### The published default document' in " << kReadmePath
        << " - if that section was renamed, this test's marker moves with it";

    // A truncated or mis-anchored extraction must fail loudly rather than quietly compare a
    // fragment: an empty document parses to the defaults, so the block's own shape is asserted
    // before the comparison that would otherwise pass on nothing.
    for (const char* key : {"transport=", "agent=", "session_key=", "connect_timeout_ms="}) {
        EXPECT_NE(published.find(key), std::string::npos)
            << "the published default document no longer mentions " << key
            << " - extracted block: [" << published << "]";
    }

    XrceSettings from_readme;
    ASSERT_NO_THROW(from_readme = ParseXrceDocument(ConfigWith(published)))
        << "the README's published default document is not one this provider accepts: ["
        << published << "]";
    EXPECT_TRUE(from_readme == XrceSettings{})
        << "the README's published defaults no longer match the code's. README says transport="
        << static_cast<int>(from_readme.transport) << " agent=" << from_readme.agent_host << ":"
        << from_readme.agent_port << " session_key=" << from_readme.session_key
        << " connect_timeout_ms=" << from_readme.connect_timeout.count();
}

// =============================================================================================
// Serial: nameable, and refused DISTINCTLY from a typo
// =============================================================================================
//
// Replaces `XrceProviderTest.SerialTransportNotImplemented`, which asserted only that something
// derived from `std::runtime_error` came out - true of `PubSubError` whatever its status, so it
// could not tell "this build cannot do serial" from "unknown key". Owner ruling 2026-09-02:
// a valid-but-unsupported selection must not look like a mistake.
//
// M7 - route serial into the transport switch instead - reddens this: it would arrive as
// `kTransportFailure` (or as a `std::runtime_error` before that), not `kNotSupported`. Costs
// microseconds: no transport object is ever built.
TEST(XrceConfig, SerialIsRefusedAsUnsupported) {
    const Refusal refusal = Catch([&] { ParseXrceDocument(ConfigWith("transport=serial")); });
    ASSERT_TRUE(refusal.threw) << "transport=serial was accepted";
    ASSERT_TRUE(refusal.typed) << refusal.message;
    EXPECT_EQ(refusal.status, PubSubStatus::kNotSupported)
        << "serial must be refused as unsupported, distinctly from an unknown value's "
           "kInvalidArgument: "
        << refusal.message;
    EXPECT_NE(refusal.message.find("\"transport=serial\""), std::string::npos) << refusal.message;

    // And it is refused through the constructor too, before any transport exists.
    const Refusal via_ctor =
        Catch([&] { XrceDDSPubSubProvider provider(ConfigWith("transport=serial")); });
    ASSERT_TRUE(via_ctor.typed) << via_ctor.message;
    EXPECT_EQ(via_ctor.status, PubSubStatus::kNotSupported) << via_ctor.message;
}

// =============================================================================================
// An unreachable Agent is a TRANSPORT failure - and registration gets an Agent-free witness
// =============================================================================================
//
// Replaces `XrceProviderTest.ConstructorThrowsWithoutAgent`, which asserted "it threw". The
// status is the point: before this item the constructor threw `std::runtime_error`, which
// arrives at a caller through `TranslateSeamFailure` as `kInternal` and tells an operator
// nothing about what to do next (M8).
//
// Routed through `RegisterXrceProvider` + `Create("xrce", ...)` rather than a direct
// constructor call, so built-in registration gets a witness that needs no Agent: register under
// any other name (M14) and this row reddens in the provider's own CI as an unknown selector,
// instead of only in the Agent-gated conformance suite.
//
// Port 19999 is named explicitly. The default address would be 127.0.0.1:2018 - the interop
// suite's own Agent port - and an Agent there would make construction SUCCEED and redden this
// row for the wrong reason (review C2-5).
//
// `connect_timeout_ms=1`, not `=0`, since fix cycle 1: at a 0 ms budget the client sends one
// datagram and never listens, so construction failed whether or not anything answered on 19999
// and the `ASSERT_EQ(provider, nullptr)` below was DEAD - the row claimed to witness
// unreachability and could not (review 4a F4). One millisecond is one whole ~1000 ms attempt
// (`SessionAttempts`), so the handshake is now genuinely awaited and genuinely unanswered: that
// costs this row ~1 s and buys the assertion back. It stays Agent-INSENSITIVE - nothing in this
// tree runs an Agent on 19999, and if something did, the message says so instead of failing on
// the status.
TEST(XrceConfig, AgentUnreachableIsATransportFailure) {
    ProviderRegistry registry;
    RegisterXrceProvider(registry);

    const ProviderConfig config = ConfigWith("agent=127.0.0.1:19999\nconnect_timeout_ms=1");

    const Refusal refusal = Catch([&] {
        std::shared_ptr<PubSubProvider> provider =
            registry.Create(ProviderSelector::Parse("xrce"), config);
        // Reachable only if something answers XRCE on 19999 within the one attempt this
        // budget buys, which would be a broken machine rather than a broken build - say so
        // instead of failing on the status.
        ASSERT_EQ(provider, nullptr) << "something is answering XRCE on 127.0.0.1:19999";
    });
    ASSERT_TRUE(refusal.threw) << "constructing against an unreachable Agent did not fail";
    ASSERT_TRUE(refusal.typed) << "an untyped exception reaches the caller as kInternal: "
                               << refusal.message;
    EXPECT_EQ(refusal.status, PubSubStatus::kTransportFailure) << refusal.message;
    // Not an unknown-name refusal: that is `kInvalidArgument` and would mean the provider is
    // registered under some other name than "xrce".
    EXPECT_EQ(refusal.message.find("no built-in provider named"), std::string::npos)
        << "\"xrce\" did not resolve through the registry: " << refusal.message;
}
