// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// The provider's configuration document: `key=value`, one setting per line (locked decision 8,
// spec §4.1/§4.2; owner ruling 2026-09-02 "into the document"). Fletcher gains no parser and
// this provider gains no dependency — the whole reader is `<string>` plus `<chrono>`.
//
// ── Why this is NOT the loopback's reader, and must not become it ────────────────────────────
// The FORMAT is shared and specified once, in spec §4.1; the CODE is deliberately duplicated.
// A shared reader could only live in `pubsub/` — Fletcher carrying a config parser, which
// decision 8 makes a stop-and-ask — or in a new component the `<75 KB` Flash target would have
// to link for sixty lines (§4.2). The tree has already made this exact call for a ONE-LINE
// helper: `in_process_provider.cpp:44-47` declines to share `Quoted` for the same reason. Do
// not re-propose sharing it; design review cycle 1 confirmed it is forced, not convenient.
// Drift is bounded by both readers' tolerance tests asserting the same spec §4.1 rows
// (`XrceConfig.ToleranceRulesMatchTheLoopback` here).
//
// ── Tolerance: the loopback's rules, one of them made stricter ─────────────────────────────────
// `\n`-separated entries; a trailing `\r` on an entry is stripped (a document authored
// on this project's primary platform is CRLF, and the same text must mean the same thing in
// every build); a blank entry — a blank line, or the trailing newline — is skipped;
// **nothing else is trimmed**, no case folding, no comments. An embedded NUL is refused up
// front: the refusal message is built by concatenation and read back through
// `what()`/`c_str()`, which stops dead at the first NUL, so quoting around a byte the
// diagnostic channel cannot carry would be dishonest.
//
// One rule is STRONGER than "nothing is trimmed", because that rule turned out to be weaker
// than it sounds: any byte below 0x21 INSIDE an entry — a space, a tab, a mid-entry CR — is
// refused outright (fix cycle 1, review 4b S2). Trimming nothing left `agent= 127.0.0.1:2018`
// representable, with the space kept in the host, to be rejected a layer down by the resolver
// this provider deliberately knows nothing about (H1); refusing it here makes the state
// unrepresentable instead of documented. It is a rule about bytes INSIDE an entry, never about
// the separators between them, so CRLF documents and blank lines are unaffected.
//
// ── Everything else is refused, before any I/O ───────────────────────────────────────────────
// An unknown key (including `stream_history` and `run_loop_ms`, which this item deleted — they
// are now typos like any other), a duplicate key, an entry with no `=`, an unknown value, a
// half-specified or out-of-range address, a number out of its key's range: all
// `PubSubError(kInvalidArgument)` quoting the offending entry. `transport=serial` is the one
// exception — `kNotSupported`, because "this build cannot do serial" is a different operator
// action from a typo (owner ruling 2026-09-02, "accept it, fail distinctly").
//
// This function is PURE: no socket, no session, no Agent, no global state. That is what lets
// this item's guards run in the provider's own CI, and it is what makes
// `ParseXrceDocument` runnable to completion before the constructor touches anything (spec
// §4.1's disclosure clause: this provider defers no document refusal, and no key here is
// topic-scoped, so it cannot).

#ifndef FLETCHER_XRCE_SRC_INTERNAL_XRCE_DOCUMENT_HPP_
#define FLETCHER_XRCE_SRC_INTERNAL_XRCE_DOCUMENT_HPP_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fletcher/pubsub/provider_registry.hpp>
#include <string>

namespace fletcher {
namespace internal {

/// The transports this build can actually open. There is no `kSerial`: `transport=serial` is
/// nameable in a document and refused with `kNotSupported` by the reader, so a settings value
/// meaning "serial" is unrepresentable and the constructor has no arm that could route one at a
/// transport it does not have (rung-1 case 4).
enum class XrceTransportKind { kUdp, kTcp };

/// Everything the four document keys decide, and nothing else — the typed core
/// (`max_payload_bytes`, `domain_id`) stays on `ProviderConfig` and is validated by the
/// constructor.
///
/// An aggregate with a **defaulted** `operator==`, which is what
/// `XrceConfig.EveryKeySetNonDefaultLandsWholeStruct` compares: total over the fields that
/// exist, so a key the reader range-checks and then fails to assign cannot hide.
///
/// Growth rule (design §2, review C2-4): a new key arrives **with its witness**, never before.
/// The defaulted `==` does not police growth on its own — a sixth field the parser forgot would
/// compare default-against-default and stay green.
struct XrceSettings {
    /// `transport` — `udp` (default) or `tcp`.
    XrceTransportKind transport = XrceTransportKind::kUdp;

    /// The host half of `agent=HOST:PORT`. Handed to the XRCE client unchanged, so whatever its
    /// resolver accepts works: an IPv4 literal or a hostname. **IPv4 only** — the client is
    /// initialised `UXR_IPv4`, and the one-colon rule refuses `[::1]:2018` and `::1` rather than
    /// half-accepting them. Disclosed foreclosure, not a regression (README, H-list).
    std::string agent_host = "127.0.0.1";

    /// The port half of `agent=HOST:PORT`. 1–65535; parsed wide and range-checked, never
    /// narrowed.
    uint16_t agent_port = 2018;

    /// `session_key` — must be unique per client on one Agent (H3: uniqueness is a property of
    /// the Agent's client population and is unobservable from here).
    uint32_t session_key = 0xAABBCCDDu;

    /// `connect_timeout_ms` — the budget for the initial session handshake, 0–60000 ms.
    ///
    /// `std::chrono::milliseconds` and not an integer **on purpose**: it is the only duration
    /// left, and a field swap with a neighbouring number is now a compile error rather than a
    /// test row (design §2, rung-1 case 7). Granularity is coarse and the conversion to what
    /// the client actually takes — a COUNT of ~1000 ms attempts — is `SessionAttempts` below,
    /// which is where the arithmetic lives so a test can pin it without a socket.
    std::chrono::milliseconds connect_timeout{3000};

    bool operator==(const XrceSettings&) const = default;
};

/// How many session-creation attempts a millisecond budget buys.
///
/// `uxr_create_session_retries` does not take a duration; it takes a **total attempt count**,
/// and each attempt costs up to `UXR_CONFIG_MIN_SESSION_CONNECTION_INTERVAL` (1000 ms, fixed in
/// the client's generated `config.h`) because that is how long it listens before resending.
/// Two facts about that argument decide this function:
///
///  - `0` means *send one datagram and do not listen at all* — `wait_session_status` returns
///    early with `last_requested_status` still `UXR_STATUS_NONE`, so `uxr_create_session_retries`
///    reports failure unconditionally (`session.c:742-746`, client 3.0.1). It is a legal and
///    useful value — "do not wait" — but it can never connect, so nothing above 0 may map to it.
///  - the count is not a count of RETRIES on top of a first try, so `n` means `n` attempts.
///
/// The mapping is therefore a **ceiling**: `0 → 0`, `1..1000 → 1`, `1001..2000 → 2`,
/// `3000 → 3`, `60000 → 60`. Rounding down instead (as this provider did until PDA-DEC-7 fix
/// cycle 1) made every accepted budget from 1 to 1000 ms buy ZERO attempts — a documented,
/// in-range value that could never connect to a healthy Agent, while the diagnostic blamed the
/// Agent — and under-spent every larger budget by one attempt. It survived because only `=0`
/// was tested; the interior of the range is now a table
/// (`XrceConfig.ConnectTimeoutBudgetBuysWholeAttempts`).
///
/// Pure, and deliberately HERE rather than in the constructor: this was the only arithmetic in
/// this item that lived outside the reader, and it was the only arithmetic that was wrong.
size_t SessionAttempts(std::chrono::milliseconds budget);

/// Read `config.document`. Throws `PubSubError` — `kInvalidArgument` for everything except
/// `transport=serial`, which is `kNotSupported`. An EMPTY document is not an error: it means
/// every published default (spec §4.1, H2).
XrceSettings ParseXrceDocument(const ProviderConfig& config);

}  // namespace internal
}  // namespace fletcher

#endif  // FLETCHER_XRCE_SRC_INTERNAL_XRCE_DOCUMENT_HPP_
