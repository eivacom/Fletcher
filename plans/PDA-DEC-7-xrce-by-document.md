# PDA-DEC-7 — XRCE configured by document (`key=value`); `XrceConfig` retired

Oracle: [docs/pubsub-interface-spec.md](../docs/pubsub-interface-spec.md) §4 (clause 4), §4.1,
§4.2. Decisions 3, 8, 13, 14. Rulings 2026-09-02 ("Into the document" — the typed core is
exactly `{max_payload_bytes, domain_id}` and **the XRCE agent address becomes a document
line**), 2026-08-31 (configuration shape; retirement, not deprecation). Predecessors:
[PDA-DEC-4](PDA-DEC-4-provider-registry.md) (frozen `Create`),
[PDA-DEC-5](PDA-DEC-5-inprocess-builtin.md) (tolerance rules, adopted unchanged),
[PDA-DEC-6](PDA-DEC-6-fastdds-by-document.md) (same shape; mistakes avoided here). Revision 1
closes cycle 1's B1 (§2, §7, §8) and folds in DEBT-1..-9.

## Summary

XRCE becomes selectable as the built-in name `xrce` through PDA-DEC-4's registry, and every
setting it has moves out of `XrceConfig` into a `key=value` document only this provider reads
— no parser, no dependency, ~150 lines of `<string>`. `XrceConfig` and `XrceTransport` are
**deleted**. Of its 12 fields: 2 go to the typed core, 5 become the 4 document keys, and
**5 are deleted outright** — nothing in the tree sets or reads them, so a key for any would
be a range-check with nothing behind it.

## Design

### 1. The registration, and the one construction API

The header (`include/fletcher/xrcedds_pubsub_provider/xrce_dds_pubsub_provider.hpp`) declares
exactly two things: `void RegisterXrceProvider(ProviderRegistry&)`, which registers the name
`"xrce"`, and `explicit XrceDDSPubSubProvider(const ProviderConfig&)` — **no default
argument**. It stops declaring any XRCE vocabulary (§4.1's closing sentence names `XrceConfig`
as owed here) and includes only `provider.hpp` + `provider_registry.hpp` (where
`ProviderConfig`/`ProviderRegistry` live) + `payload_bound.hpp`, kept while the header
advises `kPayloadBytes<N>`. Typed core: `max_payload_bytes == 0` → **65536**, bit-identical
to today's `kPayloadBytes<64*1024>`, because the bound is part of the registered DDS type
name and a different number silently stops discovery (decision 13); `domain_id` is
`uint32_t` at the seam and `uint16_t` on the wire, so above 65535 is **refused, never
narrowed** — `provider_registry.hpp:113-116` demands exactly this.

The gateway is **not** touched: it does not link the XRCE client, and registration states
availability (a link-time fact) while `Create` performs selection (§4 clause 4). The
registering callers are the conformance XRCE subjects and the interop test.

### 2. The document: `key=value`, one line per setting

| Key | Values | Default | Was |
|---|---|---|---|
| `transport` | `udp`, `tcp` (`serial` → `kNotSupported`) | `udp` | `XrceTransport` |
| `agent` | `HOST:PORT`, one colon, port 1–65535 | `127.0.0.1:2018` | `agent_ip` + `agent_port` |
| `session_key` | decimal `uint32` | `2864434397` | `session_key` |
| `connect_timeout_ms` | decimal, 0–60000, held as `std::chrono::milliseconds` | `3000 ms` | `int connect_timeout_ms` |

**Four keys, not six.** `stream_history` and `run_loop_ms` are typed fields today that **no
in-tree caller sets** and nothing in the tree observes, so a key for either would be a
range-check with nothing behind it — the "guard that cannot fail" this item exists to avoid.
They are **deleted, not keyed**: fixed constants in the provider (`kStreamHistory = 4`,
`kRunLoopQuantum = 10ms`), rung 1 — an operator cannot get them wrong because they cannot be
said. Disclosed loss: the stream depth and pump quantum are no longer settable at all. If
either is ever wanted it arrives as a key **with** its witness (the output buffer is
`MTU × history`, reachable through `src/internal/xrce_test_hook.hpp`), never before.
*Authority:* the round's delete-first lean default; nothing observable changes.
`connect_timeout_ms`, the only duration left, is `std::chrono::milliseconds` in
`XrceSettings`, so it cannot be assigned from or to a neighbouring integer — the field-swap
class is a compile error, not a test row.

The address is **one** key, not two: separate `agent_ip`/`agent_port` let a document name only
the host and silently keep port 2018 — the half-specified address that is PDA-DEC-6's "silence
is load-bearing" trap. One line cannot be half-given; the ruling's "becomes **a document
line**" is taken literally. An unmentioned key keeps this provider's published default —
**per-key authority**, which is *not* PDA-DEC-6's no-merge problem: there the substrate never
reported which policies a document set, so any overlay rested on an unavailable fact; here the
reader *is* the substrate and knows exactly which keys were present, so per-key authority is
implementable and testable. Numbers are parsed once into `uint64_t`, then range-checked per
key: **no value is ever narrowed silently.** Decimal only — no `0x`, no sign, no whitespace —
because one total rule beats two, and every caller builds keys with `std::to_string`.

### 3. One format, two readers — deliberately not one shared reader

The tolerance rules are PDA-DEC-5's, adopted **verbatim**: `\n`-separated entries, a trailing
`\r` stripped, blank entries skipped, nothing else trimmed, no case folding, no comments, an
embedded NUL refused up front. **Spec §4.1 is the single tolerance oracle for both readers.**
The *code* is not shared and cannot be: a shared reader belongs either in `pubsub/` —
Fletcher carrying a config parser, decision 8's explicit stop-and-ask — or in a new component
the `<75 KB` Flash target must link for 60 lines (§4.2). Review cycle 1 confirmed this is
forced, not convenient; do not re-propose it. Drift is bounded by each reader's refusal table
asserting the same §4.1 rows. No coexistence window; nothing scheduled for later deletion.
The reader is a **pure function** in `src/internal/xrce_document.hpp` —
`XrceSettings ParseXrceDocument(const ProviderConfig&)` — not installed, testable with no
Agent, socket or session, which is why this item's guards run in the provider's own CI.

### 4. Every document refusal is a construction-time refusal, before any I/O

PDA-DEC-6's header promised this and three refusals fired later, so §4.1 now carries a
disclosure clause. **This provider owes nothing under it, and structurally so:**
`ParseXrceDocument` runs to completion over the whole document before the constructor touches
a socket, session or buffer, and no XRCE key is topic-scoped, so there is no "first `Publish`"
moment at which a name first becomes known. Constructor order: validate everything → size
buffers → init transport → create session. The only construction-time failure that is *not* a
document refusal is the transport itself (H1). Review cycle 1 checked all three deferral
routes and confirms nothing is owed.

### 5. Typed refusals out of the constructor

Today the constructor throws `std::invalid_argument` / `std::runtime_error`, which
`TranslateSeamFailure` (`core/.../status.hpp:133`) turns into **`kInternal`** through a
registered factory — telling an operator nothing. So every document refusal becomes a
`PubSubError` quoting the offending entry: `kInvalidArgument`, except `transport=serial` →
`kNotSupported` ("this build cannot do serial" is a different operator action from a typo);
an unreachable Agent or failed transport init → `kTransportFailure`. `PubSubError` derives from
`std::runtime_error`, so the existing `EXPECT_THROW(..., std::runtime_error)` rows would stay
green regardless — which is exactly why they are **tightened to the status**.

### 6. How I would know the document reached the transport

A guard that cannot fail is not a guard (PDA-DEC-6's measured failure). The address is
observable **without an Agent**: `uxr_init_tcp_transport` connects at init and returns false
when refused (P1). So the forcing test owns a listening socket on an **ephemeral** port,
hands the provider `transport=tcp` + `agent=127.0.0.1:<that port>` + `connect_timeout_ms=0`,
and asserts **the listener accepted a connection** (latched, hard timeout), then that the
constructor fails `kTransportFailure` because nothing behind the socket speaks XRCE. Row 1
carries the falsifiability alone, and structurally: the port is chosen at run time, so it is
no default of Fletcher's, the client's or an Agent's, and no build can hard-code it. The
empty-document row is a **harness control**, not a build guard — it proves row 1's accept latch
is not stale and holds no cross-row state; it is environment-sensitive (an Agent on UDP 2018
changes what it *does*, not what it asserts) and cannot shorten its own budget, so it pays the
default 3000 ms. Budget the case at **4–5 s**.

### 7. Every accepted value is witnessed where it lands, not only where it is refused

A refusal table only stops a reader that fails to refuse; it says nothing about one that
accepts a value and discards it — the C2-1 defect class ("must mandate the form and assert
it, or this ruling is unfalsifiable"), which cost PDA-DEC-6 a cycle. So the reader boundary is
closed by **one whole-struct row**, the mirror of `PublishedDefaultsAreExact` (~25 lines, no
Agent, no socket): a document setting **all four keys** non-default must parse to an
`XrceSettings` **equal field-for-field** to the expected value. `XrceSettings` is an aggregate
with a defaulted `operator==`, so the comparison is total by construction — a field added
later and left unassigned is caught without editing the row. Downstream, each surviving key has
its own witness: `transport`+`agent` in §6 row 1 through a real socket; `session_key` in the 24
`conformance_xrce` cases (unique per subject — ignore it and they collide);
`connect_timeout_ms` in row 1's single attempt. The two keys with no witness were **deleted**
(§2), not guarded.

### 8. Decided, not open — recorded with authority

1. **The inert `max_payload` cap is deleted.** A documented 512-byte cap read nowhere.
   *Authority:* the round's delete-first lean default; nothing observable changes.
2. **`transport=serial` is nameable but distinctly refused** — `kNotSupported`, separate from
   an unknown key's `kInvalidArgument`. *Authority:* the owner's 2026-09-02 ruling "Accept it,
   fail distinctly"; a valid-but-unsupported selection must not look like a typo.
3. **Document tolerance is strict.** *Authority:* spec §4.1 as landed by PDA-DEC-6; the
   lenient alternative would contradict it.

## Corner cases forbidden

**Rung 1 — unrepresentable**

1. *Configuring XRCE from C++ without a document* — one `ProviderConfig` constructor, no
   default argument; `XrceConfig`/`XrceTransport` deleted, so "caller holds a typed XRCE
   config" cannot be written.
2. *A half-specified Agent address* — one `agent=HOST:PORT` line (§2).
3. *A silently narrowed number* — parse wide, range-check per key, refuse (§2).
4. *A serial device path or baud rate with no serial transport* — both deleted; reachable
   only through a transport that throws.
5. *A payload cap that caps nothing* — `max_payload` deleted (P5); the surviving payload
   setting is the typed core, observable in the registered type name.
6. *A partially-configured provider* — validation completes before any I/O (§4). *A setting
   accepted and then discarded* — the two keys nothing could observe are **deleted**, not
   range-checked; the four remaining are compared whole-struct (§7).
7. *A swapped duration field* — the one duration is `std::chrono::milliseconds`, so assigning
   it from or to a plain integer does not compile.
8. *An IPv6 agent address* — the one-colon rule refuses `[::1]:2018` and `::1`. Not a
   regression (the client is initialised `UXR_IPv4`, `:362,369`) but a **disclosed
   foreclosure**: IPv4 literals and hostnames only; IPv6 is a separate change that must also
   move off `UXR_IPv4`. README, beside H1.

**Rung 2 — refused typed, in the reader, before any I/O**

9. Embedded NUL; an entry with no `=`; an unknown key (the two deleted names are now typos
   like any other); a duplicate key; an unknown value → `kInvalidArgument` quoting the entry.
10. A correctly-spelled key with surrounding whitespace (` agent =x`) → refused, not trimmed
    (PDA-DEC-6 paid a fix cycle for "right setting, wrong place").
11. `agent` without exactly one colon, an empty host, a non-numeric port, or a port of 0 or
    above 65535 → `kInvalidArgument`; likewise `connect_timeout_ms` above 60000.
12. `transport=serial` → `kNotSupported`, before any transport exists.
13. `domain_id > 65535`, or a non-zero `max_payload_bytes` failing `IsPayloadBound` →
    `kInvalidArgument` (both were `std::invalid_argument`, §5).

**Handled residue** — each with why it could not be forbidden

- **H1 — an unreachable or unresolvable host is a transport failure, not a document refusal.**
  *Why not forbidden:* the host is handed to the client unchanged today and Fletcher does not
  know what that resolver accepts; requiring IPv4 literals would refuse hostnames that work
  now. Still construction-time, typed `kTransportFailure`. README.
- **H2 — an empty document means every published default** (Agent on `127.0.0.1:2018`).
  *Why not forbidden:* that is the meaning §4.1 fixes for a default-constructed
  `ProviderConfig` in every provider. "Operator asked to be configured and wasn't" lives in
  the gateway, which already refuses an empty `--provider-config` file (PDA-DEC-6).
- **H3 — a `session_key` colliding with another client on one Agent.** *Why not forbidden:*
  uniqueness is a property of the Agent's client population, unobservable here. Pre-existing.

## Premises and stop conditions

- **P1 — `uxr_init_tcp_transport` connects at init and fails when refused,** so §6's listener
  observes the document's address with no Agent. **Not machine-verified** (the client is
  FetchContent'd, no source on disk); all reachable evidence is consistent with it — confirm
  before writing the listener. **STOP-AND-ASK if it defers the connect:** the assertion must
  then move into `conformance_xrce`, gating the central guard on an Agent build. Do not weaken
  the row to "the constructor threw" — that passes on defaults.
- **P2 — `PubSubError` from a factory reaches the caller intact** (verified: `status.hpp:136`
  rethrows untouched). **STOP-AND-ASK** if a bad document arrives as `kInternal`.
- **P3 — the type name and envelope bytes are untouched** when `max_payload_bytes` resolves
  to 65536 (verified bit-for-bit in review). **STOP-AND-ASK** if the bound has to move.
- **P4 — `Create`/`Register` suffice as frozen** (§4 clause 2); no `PubSubProvider` method is
  added, removed or reordered (decision 4). **STOP-AND-ASK** if a registry change looks needed.
- **P5 — `max_payload`, `serial_*`, `stream_history`, `run_loop_ms` are set nowhere and read
  nowhere in `src/`** (verified for the first two in review; the implementer greps the other
  two before deleting). **STOP-AND-ASK** on any read or caller: the field then has behaviour
  and deleting it is a disclosed loss, not a no-op.
- **P6 — no out-of-tree caller constructs `XrceDDSPubSubProvider` with `XrceConfig`.**
  Retirement is the ruling's pattern; a deliberate break in a pilot-phase package.

## Forcing-test mapping

New TU `xrcedds-pubsub-provider/tests/test_xrce_document.cpp` (`tests/CMakeLists.txt:13-14`
already puts `../src` on the include path). No Agent in any row.

| Test | Green by | Red for the right reason / mutation |
|---|---|---|
| **`XrceConfig.DocumentConfiguresTransport`** (forcing) | §6. Row 1 against a test-owned TCP listener: `transport=tcp`+`agent=127.0.0.1:<bound port>`+`connect_timeout_ms=0` → **the listener accepts**, then `kTransportFailure`. Control row: empty document → **no** connection (defaults are UDP:2018) | Does not compile before the change (no `ProviderConfig` ctor). **M1 ignore the document entirely** → row 1 never accepts → red. **M3 read `agent`'s host but keep port 2018** → row 1 never accepts → red. **M10 read `agent` but keep `transport=udp`** → no TCP connect → row 1 red. The port is ephemeral, so no build can hard-code its way green; the control row cannot be reddened by a build and is not claimed to be a build guard (DEBT-2) |
| **`XrceConfig.EveryKeySetNonDefaultLandsWholeStruct`** (B1) | §7. One document setting **all four** keys non-default (`transport=tcp`, `agent=10.1.2.3:7401`, `session_key=305419896`, `connect_timeout_ms=250`) → `ParseXrceDocument` result **`==`** the expected `XrceSettings`, whole-struct | **M11 range-check a key and then use a hard-coded value** (the exact build B1 describes) → the parsed struct differs in that field → red. **M12 assign `connect_timeout_ms` from the wrong source** → red, and a wrong-*type* source no longer compiles (§2). A field added later and left unassigned is caught without editing the row |
| `XrceConfig.DocumentRefusalsAreTypedAndQuoted` | Rung-2 items 9–13. One row per refusal, asserting the **status** and the quoted offending entry | M4 accept-and-default any single key → its row red. Stops a reader that fails to refuse; the *accept-and-discard* half is the row above |
| `XrceConfig.ToleranceRulesMatchTheLoopback` | §3. CRLF entry, blank line, trailing newline accepted; ` agent =x`, `AGENT=`, `#comment` refused | M5 add trimming or comment support → red. Pins the two readers to spec §4.1, the single tolerance oracle for both |
| `XrceConfig.PublishedDefaultsAreExact` | The README's starting-point block is **read from `README.md` on disk at run time** (path injected as a compile definition; `README.md` added to `conanfile.py`'s `exports_sources`, as `fastdds-pubsub-provider/conanfile.py:33-38` already does) and must parse to a default-constructed `XrceSettings`, compared **whole-struct**. Unreadable → **hard failure naming the path**, never `GTEST_SKIP` | M6 change a default in code or a key's spelling in the README → red. M13 bake the block in at configure time (`file(READ)`/`configure_file`) → re-creates PDA-DEC-6's held-copy defect, so it is forbidden, not tested |
| `XrceConfig.SerialIsRefusedAsUnsupported` | Rung-2 item 12 — `kNotSupported`, no transport touched, sub-millisecond | M7 route serial into the transport switch → red (`kTransportFailure`, not `kNotSupported`). Replaces the ~1 s `SerialTransportNotImplemented` |
| `XrceConfig.AgentUnreachableIsATransportFailure` | §5 — the status, not just "it threw"; constructed through `RegisterXrceProvider` + `Create("xrce", cfg)`, so registration gets an Agent-free witness too. Replaces `ConstructorThrowsWithoutAgent` | M8 leave `std::runtime_error` → arrives as `kInternal` through the factory → red. M14 register under the wrong name → unknown name, `kInvalidArgument` → red in the provider's own CI |
| `Registry.XrceResolvesAsABuiltIn` — in **`conformance_xrce`** | §1, mirroring `Registry.FastDdsResolvesAsABuiltIn`. `MakeProvider(registry, "xrce", cfg)` delivers a row through a base-typed handle | M9 register under `"xrcedds"` → unknown name → red. Not in `conformance_registry`, whose narrow link line is itself the "no transport SDK is reachable" guard |
| The 24 existing `conformance_xrce` cases + 3 interop tests (**changed, not added**) | §6 — all now document-configured, Agent on 2019/domain 153 and 2018/domain 145 | M1/M3 redden all **24** conformance cases: no Agent on the default port 2018, no discovery on the default domain. The end-to-end proof for UDP and session key. The interop 3 run their Agent *on* the default port and differ only in `domain_id` (typed core), so they witness the core and the type name, not the document |

Inner loop: `ctest -R '^XrceConfig\.'`. **Mandated full run:** provider suite,
`conformance_xrce` with `-DFLETCHER_CONFORMANCE_XRCE=ON` passed explicitly (a cached `OFF`
deletes the subjects silently), and `fastdds-xrce-interop`. `conan create` reporting "Already
installed!" is not a pass — re-create `pubsub` and the provider.

## Risks / Unknowns

- **P1 is the item's load-bearing bet** — a substrate fact, not a design choice, and not
  machine-verifiable from this tree; its stop condition is pre-written above.
- **The Agent's absence is a feature of these guards** — all but two rows need none. The
  *format* is therefore far better covered than the *transport*, whose coverage is the 24
  pre-existing `conformance_xrce` cases (plus 3 interop witnessing the typed core), unweakened.
- **The two deleted keys are a real, small narrowing.** A C++ caller could set
  `stream_history` / `run_loop_ms` today and after this cannot. No caller does and no test
  observes either; keeping them buys a range-check nothing witnesses. README-disclosed,
  recoverable later *with* a witness.
- **Public surface: +2 / −3, net −1** — adds `RegisterXrceProvider` and the
  `ProviderConfig` constructor; retires `XrceConfig` (12 fields), `XrceTransport` and the
  `XrceConfig` constructor. `XrceSettings`/`ParseXrceDocument` live in `src/internal/` and are
  not surface. No coexistence window, no shim, nothing scheduled for later deletion.
- **Spec touch, editorial** (wire bytes unchanged — decision 13, P3): §4 clause 4 records
  `xrce`; §4.1's `XrceConfig` debt is discharged, the XRCE key table lands beside the
  loopback's, and the disclosure clause is answered with "nothing deferred, and why" (§4).

## Files-to-touch

**Changed**, under `xrcedds-pubsub-provider/` unless noted:
`include/fletcher/xrcedds_pubsub_provider/xrce_dds_pubsub_provider.hpp` ·
`src/xrce_dds_pubsub_provider.cpp` · `tests/{test_xrce_provider.cpp,discard_probe.cpp,CMakeLists.txt}` ·
`conanfile.py` (export `README.md` for the drift guard) · `test_package/src/example.cpp` ·
`README.md` (key table, the block the guard reads, deleted settings, IPv4-only, H1–H3).
Elsewhere:
`integration-tests/pubsub-conformance/{subjects/xrce_main.cpp,subjects/xrce_peer_main.cpp,src/registry.cpp,CMakeLists.txt}` ·
`integration-tests/fastdds-xrce-interop/{tests/test_interop.cpp,README.md}` ·
`pubsub/include/fletcher/pubsub/{provider.hpp:73-77,provider_registry.hpp:113-116}` (forward
notes to this item) · `docs/pubsub-interface-spec.md` · root `README.md`.
**New:** `src/internal/xrce_document.{hpp,cpp}` · `tests/test_xrce_document.cpp`.

## Files-to-delete

- `struct XrceConfig`, `enum class XrceTransport`, `XrceDDSPubSubProvider(const XrceConfig&)`
  → `ProviderConfig` + the document. Field by field: `domain_id`, `payload_bound` → typed core
  (0 = unset → 65536); `transport`, `agent_ip`+`agent_port`, `session_key`,
  `connect_timeout_ms` → the four keys (§2); **`max_payload` → no replacement — read nowhere,
  so nothing is lost but the README's claim that it capped anything (disclosed)**;
  **`serial_device`, `serial_baudrate` → no replacement — reachable only through a transport
  that refuses (disclosed)**; **`stream_history`, `run_loop_ms` → no replacement, behaviour
  gone, disclosed — now the constants every caller already used, not settable at all (§2).**
- **Tests retired, each replaced:** `XrceProviderTest.{DefaultValues,CustomValues}` (struct
  field defaults, including the inert one) → `PublishedDefaultsAreExact` + the whole-struct
  B1 row, strictly stronger; `SerialTransportNotImplemented` → `SerialIsRefusedAsUnsupported`
  (typed status, ~1 s faster); `ConstructorThrowsWithoutAgent` →
  `AgentUnreachableIsATransportFailure` (typed status, through the registry).

## Numbers

Declared net lines **+1900 / −400** — raised from +1400: the review measured my two largest
items ~1.5× under for this tree's comment density (PDA-DEC-6 declared +780, landed +2870).
Basis: reader ~150 (four keys, not six), provider/header ~110, tests ~1000 (forcing row +
cross-platform listener ~250, refusal table ~350, whole-struct row ~25, tolerance ~100, drift
guard ~90, defaults ~90, registry-routed refusal ~10), caller migrations ~150, README ~180,
spec ~50, CMake + `conanfile.py` ~30; dropping two keys saves ~40, B1's row adds ~25.
Provider suite **10 gtest cases − 4 retired + 7 new = 13** plus the MSVC nodiscard probe =
**14 ctest entries**; `conformance_xrce` stays **one** entry, 24 → 25 gtest cases. New public
surface **net −1** (+2 / −3), counted and confirmed. Cycles 2/2.
