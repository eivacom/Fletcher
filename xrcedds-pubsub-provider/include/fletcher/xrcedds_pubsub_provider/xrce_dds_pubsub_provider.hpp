// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 The Fletcher Authors
//
// NO XRCE vocabulary may appear in this header, and none does: it declares exactly two things,
// a registration function and one constructor over `ProviderConfig`. `XrceConfig` and
// `XrceTransport` are **retired, not deprecated** — no coexistence window, no shim, nothing
// scheduled for later deletion (owner ruling 2026-08-31, applied to Fast DDS first and named
// for XRCE by spec §4.1's closing sentence). Everything the struct used to carry is either the
// seam's typed core or a line in this provider's own `key=value` document.
#ifndef FLETCHER_INCLUDE_XRCE_DDS_PUBSUB_PROVIDER_HPP_
#define FLETCHER_INCLUDE_XRCE_DDS_PUBSUB_PROVIDER_HPP_

#include <cstdint>
// The bound advice below tells a caller to write `kPayloadBytes<N>`, so the header owes them the
// declaration: nothing else here pulls it in, and an out-of-tree TU that took the advice would
// otherwise not compile (Fast DDS review 4a F7, the same mistake made once already).
#include <fletcher/pubsub/payload_bound.hpp>
#include <fletcher/pubsub/provider.hpp>
// `ProviderConfig` and `ProviderRegistry` both live here, not in provider.hpp.
#include <fletcher/pubsub/provider_registry.hpp>
#include <memory>
#include <string>
#include <vector>

namespace fletcher {

/// Make XRCE-DDS selectable as `"xrce"` (spec §4 clause 4).
///
/// Idempotence is NOT offered: a second call is refused by `ProviderRegistry::Register`
/// (`kInvalidArgument`) — a registry means one transport per name for its whole life.
///
/// Registration states *availability*, which is a link-time fact; `Create` performs *selection*.
/// The gateway does not call this, because it does not link the XRCE client.
void RegisterXrceProvider(ProviderRegistry& registry);

/// PubSubProvider transport backed by eProsima Micro XRCE-DDS Client.
///
/// Requires a running XRCE-DDS Agent process (e.g. MicroXRCEAgent); the client reaches it over
/// UDP or TCP. Schema delivery uses a companion `__schema` topic with RELIABLE QoS, matching the
/// Fast DDS provider pattern.
///
/// ── How it is configured (spec §4.1, owner rulings 2026-08-31 / 2026-09-02) ─────────────────
/// `ProviderConfig` and nothing else. There are no runtime setters and no typed XRCE options
/// struct: what an operator can choose, they choose in the document.
///
///  - `domain_id` — the DDS domain the Agent creates this client's participant on. It must match
///    any DDS peer's, or the two never meet. `uint32_t` at the seam and `uint16_t` on the XRCE
///    wire, so a value **above 65535 is refused, never narrowed** — a truncated domain id is a
///    wrong answer with no error.
///  - `max_payload_bytes` — the row payload bound this client's DDS topics advertise; **0 means
///    unset** and resolves to 65536. It is part of the registered DDS type name, so it must
///    equal the `max_payload_bytes` of any Fast DDS peer or the two never discover each other
///    and no diagnostic says so. A value `IsPayloadBound` rejects is refused with
///    `PubSubError(kInvalidArgument)` before any socket. Write it as `kPayloadBytes<N>` to be
///    told at compile time instead.
///  - `document` — **`key=value`, one setting per line**, read only by this provider (locked
///    decision 8: Fletcher gains no parser and no config dependency). An empty document means
///    every published default, which is what every caller got before this existed.
///
/// ── The four keys ───────────────────────────────────────────────────────────────────────────
/// | key | values | default |
/// |---|---|---|
/// | `transport` | `udp`, `tcp` (`serial` is nameable and refused `kNotSupported`) | `udp` |
/// | `agent` | `HOST:PORT` — exactly one colon, port 1–65535 | `127.0.0.1:2018` |
/// | `session_key` | decimal `uint32`, unique per client on one Agent | `2864434397` |
/// | `connect_timeout_ms` | decimal 0–60000 | `3000` |
///
/// The address is **one** key: two would let a document name only the host and silently keep
/// port 2018, and a half-specified address is exactly the silence this shape exists to remove.
/// An unmentioned key keeps this provider's published default. The provider's README publishes
/// the full default document, and a test reads it out of that README so it cannot drift.
///
/// Tolerance is strict (spec §4.1, as landed by PDA-DEC-5, the single oracle for both in-tree
/// `key=value` readers): `\n`-separated entries, a trailing `\r` stripped, blank entries
/// skipped, **nothing else trimmed**, no case folding, no comments. ` agent =x` is refused
/// rather than trimmed — right setting, wrong place, said out loud.
///
/// ── Refused, and all of it before any I/O ───────────────────────────────────────────────────
/// **Every** document refusal is a construction-time refusal, and structurally so: the document
/// is read to completion before the constructor touches a socket, session or buffer, and no key
/// here is topic-scoped, so there is no "first `Publish`" moment at which one first becomes
/// checkable. Unlike Fast DDS, this provider defers nothing under spec §4.1's disclosure
/// clause. A constructed provider is one whose whole document has been read.
///
/// `kInvalidArgument`: an embedded NUL; an entry with no `=`; an unknown key (`stream_history`
/// and `run_loop_ms` are now unknown names like any other); a duplicate key; an unknown value; a
/// key with stray whitespace; an `agent` without exactly one colon, with an empty host or with a
/// port outside 1–65535; a `connect_timeout_ms` above 60000; a `domain_id` above 65535; an
/// unusable `max_payload_bytes`. `kNotSupported`: `transport=serial`. `kTransportFailure`: a
/// transport that will not initialise, and an Agent that does not answer within
/// `connect_timeout_ms` — including an unresolvable hostname, which the client's resolver
/// decides, not Fletcher.
///
/// ── Not settable at all any more (disclosed narrowing) ──────────────────────────────────────
/// The XRCE reliable-stream history depth and the run-loop pump quantum were typed fields and
/// are now fixed at their previous values. Nothing in the tree set either and no test could
/// observe either, so a key for one would have been a range-check with nothing behind it. If
/// either is ever wanted it comes back **with** a test proving it took effect. Likewise gone:
/// a documented 512-byte payload cap that capped nothing, and two serial settings reachable
/// only through a transport that refuses.
class XrceDDSPubSubProvider : public PubSubProvider {
   public:
    /// No default argument, deliberately: "an XRCE client configured from C++ without a
    /// document" is not a thing a caller can write.
    explicit XrceDDSPubSubProvider(const ProviderConfig& config);

    ~XrceDDSPubSubProvider() override;

    XrceDDSPubSubProvider(const XrceDDSPubSubProvider&) = delete;
    XrceDDSPubSubProvider& operator=(const XrceDDSPubSubProvider&) = delete;

    void CreateTopic(const std::vector<std::string>& topic_segments, OwnedSchema schema) override;

    void Publish(const std::vector<std::string>& topic_segments, const RowEncoder& encoder,
                 const Attachments& attachments = {}) override;

    // [[nodiscard]] is NOT inherited from the PubSubProvider base declaration and
    // the diagnostic keys off the STATIC type at the call site, so the annotation
    // must be repeated on every concrete override or it never fires where
    // applications actually call (#56).
    [[nodiscard]] SubscriptionResult Subscribe(const std::vector<std::string>& topic_segments,
                                               SubscribeCallback callback) override;

    void Unsubscribe(const std::vector<std::string>& topic_segments) override;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace fletcher

#endif  // FLETCHER_INCLUDE_XRCE_DDS_PUBSUB_PROVIDER_HPP_
