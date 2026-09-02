# PDA-DEC-7 — XRCE configured by document (`key=value`); `XrceConfig` retired

Oracle: [docs/pubsub-interface-spec.md](../docs/pubsub-interface-spec.md) §4 (clause 4),
§4.1, §4.2. Decisions 3, 8, 13, 14. Rulings 2026-09-02 ("Into the document" — the typed
core is exactly `{max_payload_bytes, domain_id}` and **the XRCE agent address becomes a
document line**), 2026-08-31 (configuration shape; retirement, not deprecation).
Predecessors: [PDA-DEC-4](PDA-DEC-4-provider-registry.md) (frozen `Create`),
[PDA-DEC-5](PDA-DEC-5-inprocess-builtin.md) (the `key=value` tolerance rules this item
adopts unchanged), [PDA-DEC-6](PDA-DEC-6-fastdds-by-document.md) (the same shape of
change, and the four mistakes this design is written to avoid).

## Summary

XRCE becomes selectable as the built-in name `xrce` through PDA-DEC-4's registry, and
every setting it has moves out of `XrceConfig` into a `key=value` document only this
provider reads — no parser, no dependency, ~180 lines of `<string>`. `XrceConfig` and
`XrceTransport` are **deleted**. Two fields go to the typed core, two are deleted
outright (nothing reads them), the rest become keys.

## Design

### 1. The registration, and the one construction API

```cpp
// include/fletcher/xrcedds_pubsub_provider/xrce_dds_pubsub_provider.hpp
void RegisterXrceProvider(ProviderRegistry& registry);          // registers "xrce"
class XrceDDSPubSubProvider : public PubSubProvider {
   public:
    explicit XrceDDSPubSubProvider(const ProviderConfig& config);   // no default arg
};
```

The header stops declaring any XRCE vocabulary (§4.1's closing sentence names
`XrceConfig` as owed here) and includes only `<fletcher/pubsub/provider.hpp>`. Typed
core: `max_payload_bytes == 0` → **65536**, bit-identical to today's
`kPayloadBytes<64*1024>`, because the bound is part of the registered DDS type name and a
different number silently stops discovery (decision 13); `domain_id` is `uint32_t` at the
seam and `uint16_t` on the XRCE wire, so a value above 65535 is **refused, never
narrowed** — `provider_registry.hpp:113-116` already demands exactly this.

The gateway is **not** touched: it does not link the XRCE client, and registration states
availability (a link-time fact) while `Create` performs selection (§4 clause 4). The
registering callers are the conformance XRCE subjects and the interop test.

### 2. The document: `key=value`, one line per setting

| Key | Values | Default | Was |
|---|---|---|---|
| `transport` | `udp`, `tcp` (`serial` → `kNotSupported`) | `udp` | `XrceTransport` |
| `agent` | `HOST:PORT`, one colon, port 1–65535 | `127.0.0.1:2018` | `agent_ip` + `agent_port` |
| `session_key` | decimal `uint32` | `2864434397` | `session_key` |
| `stream_history` | power of two, 1–32 | `4` | `stream_history` (unchecked) |
| `run_loop_ms` | decimal, 1–1000 | `10` | `run_loop_ms` (unchecked) |
| `connect_timeout_ms` | decimal, 0–60000 | `3000` | `int connect_timeout_ms` |

The address is **one** key, not two: with `agent_ip` and `agent_port` separate, a document
naming only the host silently keeps port 2018 — a half-specified address, which is the
"silence is load-bearing" trap PDA-DEC-6 paid for. One line cannot be half-given. The
ruling's wording ("becomes **a document line**") is taken literally.

A key the document does not mention keeps this provider's published default. That is
per-key authority, and it is **not** the no-merge rule PDA-DEC-6 needed: PDA-DEC-6 could
not see which policies an XML document mentioned, so any overlay rested on a fact the
substrate does not report. Here the reader *is* the substrate and knows exactly which
keys were present, so per-key authority is both implementable and testable. Ruling
satisfied — a supplied document is authoritative for what it covers.

Numbers are parsed once, into `uint64_t`, then range-checked per key: **no value is ever
narrowed silently.** Decimal only, everywhere — no `0x`, no sign, no whitespace — because
one total rule beats two, and every in-tree caller builds keys with `std::to_string`.

### 3. One format, two readers — deliberately not one shared reader

The tolerance rules are PDA-DEC-5's, adopted **verbatim**: `\n`-separated entries, a
trailing `\r` stripped, blank entries skipped, nothing else trimmed, no case folding, no
comments, and an embedded NUL refused up front (`in_process_provider.cpp:56-73` states
them; §4.1 records them). The *code* is not shared, and cannot be: the only places to put
a shared reader are `pubsub/` — which makes Fletcher itself carry a config parser,
decision 8's explicit stop-and-ask — or a new component the `<75 KB` Flash target must
link for the sake of 60 lines (TD-004/TD-007, §4.2). So: one format specified once in the
spec, two readers, and the drift is bounded by each reader's own refusal table asserting
the same rows. This is not a coexistence window; nothing here is scheduled for deletion.

The reader is a **pure function** in `src/internal/xrce_document.hpp` —
`XrceSettings ParseXrceDocument(const ProviderConfig&)` — not installed, and testable with
no Agent, no socket and no session. That is the whole reason this item's guards run in the
provider's own CI, which has no Agent.

### 4. Every document refusal is a construction-time refusal, before any I/O

PDA-DEC-6's header promised this and three refusals fired later, so §4.1 now carries a
disclosure clause. **This provider owes nothing under it, and the reason is structural:**
`ParseXrceDocument` runs to completion, over the whole document, before the constructor
touches a socket, a session or a buffer — and no XRCE key is topic-scoped, so there is no
"first `Publish`" moment at which a name first becomes known. Order in the constructor is
therefore: parse and validate everything → size buffers → init transport → create
session. The header says "every document refusal fires here, and here is why that is
total"; the only construction-time failure that is *not* a document refusal is the
transport itself (H1).

### 5. Typed refusals out of the constructor

Today the constructor throws `std::invalid_argument` / `std::runtime_error`. Through a
registered factory, `TranslateSeamFailure` (`core/.../status.hpp:133`) turns both into
**`kInternal`**, which tells an operator nothing. So: every document refusal is
`PubSubError(kInvalidArgument)` quoting the offending entry; `transport=serial` is
`kNotSupported` (PDA-DEC-4's ruling shape — "this build cannot do serial" is a different
operator action from a typo, which is `kInvalidArgument`); an unreachable Agent or a
failed transport init is `kTransportFailure`. `PubSubError` derives from
`std::runtime_error`, so the two existing `EXPECT_THROW(..., std::runtime_error)` rows stay
green — which is exactly why they are **tightened to the status**, not left as they are.

### 6. How I would know the document reached the transport

A guard that cannot fail is not a guard (PDA-DEC-6's measured failure). The address is
observable **without an Agent**: `uxr_init_tcp_transport` performs the TCP connect at init
and returns false when refused. So the forcing test owns a listening socket on an
ephemeral port, hands the provider `transport=tcp` + `agent=127.0.0.1:<that port>`, and
asserts **the listener accepted a connection** (latched, hard timeout) — then that the
constructor goes on to fail `kTransportFailure`, since nothing behind the socket speaks
XRCE. The negative row is an **empty** document, which must produce *no* connection: the
defaults are UDP:2018. A build that ignores the document reddens row 1; one that hard-codes
TCP-plus-that-port reddens row 2. Neither row can pass on defaults.

UDP, `domain_id` and `session_key` are proved end-to-end for free: `conformance_xrce`'s 24
cases and the interop suite's 3 run against an Agent on port **2019** (domain 153) and
**2018/domain 145** with per-instance session keys, all now supplied as documents. Ignore
the document and every one of them fails to connect or never discovers its peer.

## Corner cases forbidden

**Rung 1 — unrepresentable**

1. *Configuring XRCE from C++ without a document.* One constructor taking `ProviderConfig`;
   `XrceConfig`/`XrceTransport` are deleted, so "caller holds a typed XRCE config" cannot
   be written. No default argument either: a provider constructed from nothing is not a
   thing an operator can mean.
2. *A half-specified Agent address* — one `agent=HOST:PORT` line (§2).
3. *A silently narrowed number* — parse wide, range-check per key, refuse (§2).
4. *A serial device path or baud rate with no serial transport* — `serial_device` and
   `serial_baudrate` are deleted; they were reachable only through a transport that throws.
5. *A payload cap that caps nothing* — `XrceConfig::max_payload` (documented "Maximum
   payload size in bytes", default 512) is read **nowhere** in the provider; grep-verified,
   see P5. Deleted. One payload setting survives, the typed core, and it is observable
   because it is in the registered type name.
6. *A partially-configured provider* — validation completes before any I/O (§4), so
   "transport open, settings still being checked" does not exist.

**Rung 2 — refused typed, in the reader, before any I/O**

7. Embedded NUL; an entry with no `=`; an unknown key; a duplicate key; an unknown value →
   `kInvalidArgument` quoting the entry (PDA-DEC-5's rules).
8. A correctly-spelled key with surrounding whitespace (` agent =x`) → refused, not
   trimmed. PDA-DEC-6 had to add a fix cycle for "right setting, wrong place"; here the
   only defence is that nothing is tolerated silently.
9. `agent` without exactly one colon, an empty host, a port of 0 or above 65535, a
   non-numeric port → `kInvalidArgument`.
10. `stream_history` not a power of two in 1–32 (buffers are MTU×history, and the XRCE
    stream requires a power of two — today unchecked); `run_loop_ms` 0 or above 1000 (the
    pump holds the impl mutex for that quantum, so a large value starves every API call —
    `xrce_dds_pubsub_provider.cpp:398-406` documents the hazard); `connect_timeout_ms`
    above 60000 (the constructor blocks for it) → `kInvalidArgument`.
11. `transport=serial` → `kNotSupported`, before any transport exists.
12. `domain_id > 65535` → `kInvalidArgument`; a non-zero `max_payload_bytes` failing
    `IsPayloadBound` → `kInvalidArgument` (was `std::invalid_argument`, §5).

**Handled residue** — each with why it could not be forbidden

- **H1 — an unresolvable or unreachable host is a transport failure, not a document
  refusal.** *Why not forbidden:* the host string is handed to the XRCE client unchanged
  today, and Fletcher does not know what that client's resolver accepts; validating IPv4
  literals would refuse hostnames that currently work. It is still construction-time, and
  typed `kTransportFailure`. README.
- **H2 — an empty document means every published default** (Agent on `127.0.0.1:2018`).
  *Why not forbidden:* it is the meaning of a default-constructed `ProviderConfig`, which
  §4.1 fixes for every provider ("empty → the provider's own defaults"). The gateway
  already refuses an *empty `--provider-config` file* (PDA-DEC-6), which is where the
  "operator asked to be configured and wasn't" case actually lives.
- **H3 — a `session_key` colliding with another client on the same Agent is not
  detectable here.** *Why not forbidden:* uniqueness is a property of the Agent's whole
  client population, which this provider cannot observe. Pre-existing; README.

## Premises and stop conditions

- **P1 — `uxr_init_tcp_transport` connects at init and fails when refused,** so §6's
  listener observes the document's address with no Agent. **STOP-AND-ASK if it defers the
  connect:** the forcing test then cannot see the address without a live Agent, and the
  assertion must move into `conformance_xrce` — which makes the item's central guard
  gated on `FLETCHER_CONFORMANCE_XRCE` and an Agent build. Do not weaken the row to "the
  constructor threw"; that passes on defaults.
- **P2 — `PubSubError` from a factory reaches the caller intact** (PDA-DEC-5's P1, proved;
  §5 depends on it). **STOP-AND-ASK** if a bad document arrives as `kInternal`.
- **P3 — the registered type name and envelope bytes are untouched** when
  `max_payload_bytes` resolves to 65536 (decision 13). Proved by the interop suite, which
  matches a Fast DDS peer by type name. **STOP-AND-ASK** if the bound has to move.
- **P4 — `Create`/`Register` suffice as frozen** (§4 clause 2); **STOP-AND-ASK** if a
  registry change looks necessary. No `PubSubProvider` method is added, removed or
  reordered (decision 4).
- **P5 — `max_payload` is read nowhere** (grep over `xrcedds-pubsub-provider/`: three hits,
  all in the README and two config-struct tests). **STOP-AND-ASK** if the implementer finds
  a read: the field then has behaviour and deleting it is a disclosed loss, not a no-op.
- **P6 — no out-of-tree caller constructs `XrceDDSPubSubProvider` with `XrceConfig`.**
  Retirement is the ruling's pattern; this is a deliberate breaking change in a pilot-phase
  package.

## Forcing-test mapping

New TU `xrcedds-pubsub-provider/tests/test_xrce_document.cpp` (the existing
`target_include_directories(... ../src)` already makes the internal header reachable).
No Agent in any row.

| Test | Green by | Red for the right reason / mutation |
|---|---|---|
| **`XrceConfig.DocumentConfiguresTransport`** (forcing) | §6. Three rows against a test-owned TCP listener: `transport=tcp`+`agent=127.0.0.1:<bound port>` → **the listener accepts**, then `kTransportFailure`; **empty document → no connection within the same budget**; `transport=tcp` + a port nothing listens on → `kTransportFailure` and no accept | Does not compile before the change (no `ProviderConfig` ctor). **M1 ignore the document** → row 1 red. **M2 hard-code tcp+port** → row 2 red. **M3 read the host but not the port** → row 1 red. No row passes on defaults |
| `XrceConfig.DocumentRefusalsAreTypedAndQuoted` | Rung-2 items 7–12. One row per refusal, asserting the **status** and the quoted offending entry | M4 accept-and-default any single key → its row red. This is the guard that stops a reader which parses and shrugs |
| `XrceConfig.ToleranceRulesMatchTheLoopback` | §3. CRLF entry, blank line, trailing newline accepted; ` agent =x`, `AGENT=`, `#comment` refused | M5 add trimming or comment support → red. Pins the two readers to one rule set |
| `XrceConfig.PublishedDefaultsAreExact` | The README's starting-point block is **read from `README.md` on disk** (path injected as a compile definition) and must parse to a default-constructed `XrceSettings`, compared **whole-struct** | M6 change a default in code or a key's spelling in the README → red. PDA-DEC-6's drift guard held its own copy of the block it protected; this one cannot |
| `XrceConfig.SerialIsRefusedAsUnsupported` | Rung-2 item 11 — `kNotSupported`, no transport touched, sub-millisecond | M7 route serial into the transport switch → red. Replaces the ~1 s `SerialTransportNotImplemented` |
| `XrceConfig.AgentUnreachableIsATransportFailure` | §5 — the status, not just "it threw". Replaces `ConstructorThrowsWithoutAgent` | M8 leave `std::runtime_error` → red (arrives as `kInternal` through the factory) |
| `Registry.XrceResolvesAsABuiltIn` — in **`conformance_xrce`** | §1, mirroring `Registry.FastDdsResolvesAsABuiltIn`. `MakeProvider(registry, "xrce", cfg)` delivers a row through a base-typed handle | M9 register under `"xrcedds"` → unknown name → red. Not in `conformance_registry`, whose narrow link line is itself the "no transport SDK is reachable" guard |
| The 24 existing `conformance_xrce` cases + 3 interop tests (**changed, not added**) | §6 — all now document-configured, Agent on 2019/domain 153 and 2018/domain 145 | M1/M3 also redden all 27: no Agent on the default port, no discovery on the default domain. The end-to-end proof for UDP, domain and session key |

Inner loop: `ctest -R '^XrceConfig\.'` in the provider suite. Forcing test alone:
`ctest -R '^XrceConfig\.DocumentConfiguresTransport$'`. **Mandated full run:** provider
suite, `conformance_xrce` with `-DFLETCHER_CONFORMANCE_XRCE=ON` passed explicitly (a
cached `OFF` deletes the subjects silently), and `fastdds-xrce-interop`. `conan create`
reporting "Already installed!" is not a pass — re-create `pubsub` and the provider.

## Risks / Unknowns

- **P1 is the item's load-bearing bet** and it is a substrate fact, not a design choice;
  its stop condition is written above rather than left for the implementer to improvise.
- **The Agent's absence is a feature of this item's guards.** Everything except two rows
  runs with no Agent, so this stage does not enlarge the Agent-dependent surface. It also
  means the *format* is far better covered than the *transport*; the transport coverage
  that exists is the 27 pre-existing end-to-end cases, and they are not weakened.
- **Public surface: +2 / −3, net −1** — adds `RegisterXrceProvider` and the
  `ProviderConfig`-taking constructor; retires `XrceConfig` (12 fields), `XrceTransport`,
  and the `XrceConfig` constructor. `XrceSettings`/`ParseXrceDocument` are in `src/internal/`
  and are not surface. No coexistence window, no shim, nothing scheduled for later deletion.
- **Wire bytes unchanged** (decision 13): envelope, QoS and type name untouched; the bound
  resolves to the same 65536. No divergence fix here, so no stop-and-ask.
- **Spec touch, editorial:** §4 clause 4 records `xrce`; §4.1's closing sentence
  ("`XrceConfig` is the same shape of change, owed by PDA-DEC-7") is discharged and the
  XRCE key table lands beside the loopback's; §4.1's disclosure clause is answered with
  "nothing deferred, and why" (§4).
- **Assumed:** the two deleted fields (`max_payload`, `serial_*`) have no out-of-tree
  reader (P5, P6).

## Files-to-touch

**Changed**, under `xrcedds-pubsub-provider/` unless noted:
`include/fletcher/xrcedds_pubsub_provider/xrce_dds_pubsub_provider.hpp` ·
`src/xrce_dds_pubsub_provider.cpp` · `tests/{test_xrce_provider.cpp,discard_probe.cpp,CMakeLists.txt}` ·
`test_package/src/example.cpp` · `README.md` (the key table, the starting-point block the
drift guard reads, H1–H3, retirement note). Elsewhere:
`integration-tests/pubsub-conformance/{subjects/xrce_main.cpp,subjects/xrce_peer_main.cpp,src/registry.cpp,CMakeLists.txt}` ·
`integration-tests/fastdds-xrce-interop/{tests/test_interop.cpp,README.md}` ·
`pubsub/include/fletcher/pubsub/provider.hpp:73-77` and
`provider_registry.hpp:113-116` (both carry a forward note to this item) ·
`docs/pubsub-interface-spec.md` · root `README.md`.
**New:** `src/internal/xrce_document.{hpp,cpp}` · `tests/test_xrce_document.cpp`.

## Files-to-delete

- `struct XrceConfig`, `enum class XrceTransport`, and
  `XrceDDSPubSubProvider(const XrceConfig&)` → `ProviderConfig` + the `key=value`
  document. Field by field: `domain_id`, `payload_bound` → typed core (0 = unset → 65536);
  `transport`, `agent_ip`+`agent_port`, `session_key`, `stream_history`, `run_loop_ms`,
  `connect_timeout_ms` → document keys (§2); **`max_payload` → no replacement — it was
  read nowhere, so nothing is lost but the README's claim that it capped anything
  (disclosed)**; **`serial_device`, `serial_baudrate` → no replacement — reachable only
  through a transport that refuses, disclosed.**
- **Tests retired, each replaced:** `XrceProviderTest.{DefaultValues,CustomValues}` (they
  assert struct field defaults, including the inert one) → `PublishedDefaultsAreExact`,
  which reads the README from disk and compares whole-struct — strictly stronger;
  `SerialTransportNotImplemented` → `SerialIsRefusedAsUnsupported` (typed status, ~1 s
  faster); `ConstructorThrowsWithoutAgent` → `AgentUnreachableIsATransportFailure` (typed
  status). *Nothing retired without a replacement.*

## Numbers

Declared net lines **+1400 / −380**. Basis, deliberately not optimistic (PDA-DEC-6
declared +780 and landed +2870): reader ~180, provider/header ~110, tests ~620 (forcing
row + cross-platform listener ~200, refusal table ~200, tolerance ~80, README drift guard
~70, defaults ~70), caller migrations ~120, README ~150, spec ~40, CMake ~20. Provider
suite 11 → ~17 ctest entries; `conformance_xrce` stays **one** ctest entry, 24 → 25 gtest
cases. New public surface **net −1** (+2 / −3). Cycles 1/2.
