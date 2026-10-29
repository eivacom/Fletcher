# BIND — C# / .NET Bindings — Execution Plan

Round plan + tracker for **C# / .NET bindings** over a C ABI, generated C# code
from `fletcher-protoc`, a managed Apache Arrow tier, a managed gateway WebSocket
client, **generated TypeScript publisher/subscriber classes**, the port of the
existing test suites, and the CI/CD pipelines to build, test and publish it all.

**Revision 2026-09-11.** First written 2026-08-31 against the unmerged
`feature/protocol-driver-abi` branch; re-planned 2026-09-08 in
[BIND-csharp-development-plan.md](BIND-csharp-development-plan.md) after PDA-DEC
merged as #126; **nineteen questions ruled by the maintainer on 2026-09-11** and
folded into this file and into
[BIND-locked-decisions.md](BIND-locked-decisions.md) (D-BIND-1 … D-BIND-27). This
file is again the single source. The development plan stays as the record of the
re-plan (its §1 re-verification, §3.2 codec design, §3.5 error handling, §6 risk
register), and [BIND-csharp-public-surface.md](BIND-csharp-public-surface.md) is the
C++ → C# interface reference with the class diagrams.

**Oracles** (they win on any contradiction):
[docs/pubsub-interface-spec.md](../docs/pubsub-interface-spec.md) (**the seam —
frozen**; a shape that cannot be mirrored is a stop-and-ask against it) ·
[docs/wire-format-specification.md](../docs/wire-format-specification.md) (wire
behaviour) · [docs/recordbatch-accessor-spec.md](../docs/recordbatch-accessor-spec.md)
(accessor tier) · [GIR-locked-decisions.md](GIR-locked-decisions.md) (the generator
IR — merged). PDA-ABI ([PDA-ABI-protocol-driver-abi.md](PDA-ABI-protocol-driver-abi.md))
is a **sibling** round below the seam, not an oracle: neither round depends on the
other (seam §1).
Locked decisions for this round: [BIND-locked-decisions.md](BIND-locked-decisions.md).
What the seam obliges a binding to do:
[BIND-csharp-inherited-constraints.md](BIND-csharp-inherited-constraints.md).
[BIND-native-transport.md](BIND-native-transport.md) is superseded and kept as history.
This file is both the `plan_path` (the tracker) and the `user_stories_path`.

## Goal

Ship the **C# / .NET binding** for Fletcher plus **generated TypeScript
publisher/subscriber** classes, so that:

1. A **C# application** is a full pub/sub client: encode/decode rows, consume
   Arrow `RecordBatch`es, publish and subscribe, and **select its transport at
   runtime** by name (`inprocess`, `fastdds`, `xrce`, and any driver PDA-ABI later
   adds by path) from configuration, with no protocol SDK on the consumer's build
   and no Fletcher rebuild.
2. A **TypeScript application** gets generated `Publisher` / `Subscriber` classes
   over the gateway, reaching parity with the C++ generated pub/sub surface it
   lacks today.
3. Every output the protoc plugin delivers has a C# counterpart.

BIND-Rust is the **next** round (Q12); the binding ABI this round writes is the
surface it will consume.

---

## Decision status at a glance

All nineteen ruled. ✅ = settled.

| # | Decision | Status | Needs |
|---|---|---|---|
| 1 | **C# calls the existing C++ encode/decode** rather than reimplementing the codec | ✅ **agreed 2026-08-31** | — |
| 2 | **Native protocols come from the seam's registry**; BIND binds `PubSubProvider` once. The PDA gate was discharged by #126; the shim links and registers `inprocess` + `fastdds` + `xrce` | ✅ **agreed 2026-08-31**; gate discharged and built-ins ruled **2026-09-11** (Q3) | — |
| 3 | **TypeScript scope** = generated `Publisher`/`Subscriber` over the gateway (not a Node native binding) | ✅ **agreed 2026-08-31** | — |
| 4 | **ABI value-transfer = Arrow C Data Interface** (`ArrowArray` + `ArrowSchema`), with the three-layer encode surface (D-BIND-23) | ✅ **agreed 2026-09-11** (Q1) | — |
| 5 | **`Apache.Arrow` as a base C# dependency**; collapse `Core` + `Arrow` into one package | ✅ **agreed 2026-09-11** (rider on #4); collapse agreed with Q2: three packages | — |
| 6 | **Native artifact links nanoarrow only**, not Arrow C++ (package size) | ✅ **agreed 2026-09-11** (rider on #4) | — |
| 7 | **Accessor nesting depth**: match RBA's 2/3 cap | ✅ **agreed 2026-09-11** (Q6): match the cap; the asymmetry is RIR's to remove | — |
| 8 | Does **"all tests" (18786)** include the 98 protoc generator tests? | ✅ **agreed 2026-09-11** (Q10): no — generator tests stay C++ and are extended; provider-internal cases (24) also excluded, documented; **276 of 302** non-generator cases port *(was 278; bucket 3's two provider-level fan-out cases joined the excluded set 2026-09-24)* — *corrected 2026-09-18: the figure was 275 of 300, low by three. `test_owned_schema` was subtracted from a total that never contained it (the 300 is buckets 1–4; it is bucket 6), and bucket 4 was carrying a stale `test_xrce_document` count. Denominator ruled 302 by the maintainer* | — |
| 9 | **"Make C# emit Rust-native accessor classes" (16353)** — confirm the reading | ✅ **agreed 2026-09-11** (Q19): read as *C# accessor classes equivalent to the Rust-native ones*; BIND-7 scope; ADO 16353 wording to be corrected | — |
| 10 | Is **BIND-Rust** in this round or the next? | ✅ **agreed 2026-09-11** (Q12): **next round** | — |
| 11 | **RID matrix** for `Eiva.Fletcher.Interop` | ✅ **agreed 2026-09-11** (Q8): `win-x64`, `linux-x64` at first release; `linux-arm64` first addition; macOS on demand | — |
| 12 | **LGPL relinking review** for shipping prebuilt native binaries | ✅ **owner assigned 2026-09-11** (Q9): the maintainer; decision due before BIND-9. The notice half is mechanised by #127's deployer. **Ruled for the preview 2026-10-20 (D-BIND-140):** the shared-shim relinking route, licence files in every package, the route and the NativeAOT caveat in each README | — |
| 13 | **Component granularity & versioning** (one `dotnet/` component, `dotnet-v` tag, **`0.5.x`** series, **three** managed packages) | ✅ **agreed 2026-09-11** (Q2). **Amended 2026-10-20 (D-BIND-138):** before the merge only per-build `0.6.0-pr.129.<run>` bundles; every package moves to 0.6 together when #129 merges, first released as `0.6.0-alpha` | — |
| 14 | **Landing order against PR #128** (P-7): does BIND wait for the FastDDS modernization branch? | ✅ **ruled 2026-09-15** (Q20, D-BIND-29): **no** — #129 lands first; BIND-1 specs the schema-watch pair as `kNotSupported`; the order is re-examined at the BIND-1 → BIND-2 boundary. **Schema-watch clause superseded 2026-09-25 (D-BIND-52): the shim forwards to the seam's pair, ABI 0.5** | — |
| 15 | **PR granularity for the round** — one PR, or one per stage boundary? | ✅ **ruled 2026-09-16** (D-BIND-30): **one PR for the round** (#129), matching #125 and #126; a fix unrelated to the bindings goes to `main` on its own, as #130 did |
| 21 | **The composite map-key refusal** — 2b narrowed what 2a's encoder accepted | ✅ **ruled 2026-09-18** (D-BIND-36): **map keys are scalar only**, refused at `fl_codec_open` by field name. A statement about the wire format's reach, not one codec's convenience — BIND-Rust inherits it. No code change; the refusal and its test already exist | — |
| 20 | **Does byte identity widen to the `emit_vectors` corpus the acceptance names?** | ✅ **ruled 2026-09-18** (D-BIND-35): **no — the acceptance is amended.** That corpus is proto-row shaped (it encodes through the generated row class) and is 3 scenarios over one flat message, so it is both the wrong shape for an Arrow-array codec and narrower than the five fixtures built. D-BIND-11's own `emit_vectors` reference is untouched | — |
| 19 | **How the copy oracle measures a binding producer** | ✅ **ruled 2026-09-17** (D-BIND-34): `fl_encode_row` into the probe's own window — `fl_publisher_publish_row` cannot reach the instrumented provider, because D-BIND-24 closed the registration door it would need |
| 18 | **BIND-0's self-hosted-runner bullet** — unmet, and due before BIND-9 | ✅ **ruled 2026-09-17** (D-BIND-33): **struck from BIND-0 as a duplicate** — BIND-9 already carries it, more completely. Nothing moves and nothing relaxes |
| 17 | **Who frees a message a CALLBACK puts in an `fl_error`** | ✅ **ruled 2026-09-17** (D-BIND-32): the callback **borrows** it — keeps the bytes alive until it returns, the shim copies and frees nothing. Stated on `fl_error`, not on `fl_grow_fn`. Answers B1 of BIND-1's spec review |
| 16 | **Who implements the shim's ~40 entry points** — BIND-2c, or BIND-4 as `builtins.cpp` says? | ✅ **ruled 2026-09-17** (D-BIND-31): **2c takes the codec surface + the publisher chain**; the subscriber half, attachments, blobs and schema arrival stay with BIND-4. Forced by 2d's copy-oracle acceptance, which needs a live publisher | — |

Nothing gates kickoff. The one open non-engineering item was the LGPL relinking
decision (#12); it is ruled for the preview (D-BIND-140, 2026-10-20).

---

## Baseline — `main` at `0a56829`, checked 2026-09-11

The 2026-08-31 draft tracked an unmerged branch; that section is retired. What the
tree carries now, and what it means for BIND (full re-verification in the
development plan §1):

- **PDA-DEC merged as #126** (`6c541e9`). The seam is frozen (spec §12.1):
  `ProviderRegistry` (`Create`/`Register`/`SetPathResolver`), `ProviderConfig`
  (typed core `{max_payload_bytes, domain_id}` plus an opaque document), InProcess
  promoted to a built-in in `pubsub/`, Fast DDS configured by its own XML document,
  XRCE by `key=value`, `FastDDSProviderOptions` **retired**; the delivery callback
  borrows `data`, `schema` and `attachments`; re-entry is refused on all four
  provider methods (`kReentrantCall = 10`); callbacks are contained and counted;
  `Unsubscribe` blocks with one carve-out; `WriteBuffer::AppendInPlace` exists with
  no production caller yet.
- **PDA-ABI has not started and BIND does not need it.** BIND wraps the registry;
  a path selector refuses with `kNotSupported` until PDA-ABI installs its resolver,
  which is additive (§4 clause 2).
- **#127** (`0a56829`) is packaging-only: the gateway release archive now ships
  `LICENSE` and a Conan-graph-derived `THIRD-PARTY-LICENSES.txt`. BIND-9 reuses the
  deployer.
- **There is no nanoarrow, descriptor-driven codec in the tree to wrap.** The two
  encoders that exist are `arrow-bridge`'s `Codec` (Arrow C++ scalars) and the
  generated C++ `EncodeTo` over `positional_io.hpp`. BIND-2 builds the third driver
  of the one wire format, on `positional_io`, over the Arrow C Data Interface.
- All components are at `0.5.0-alpha` (XRCE at `0.5.1-alpha`); the .NET packages
  join `0.5.x`.
- Test counts re-baselined: **300** non-generator, **98** generator cases (Part 4).

---

## Part 0 — Requirements and inherited constraints

### ADO requirements

This round implements **Feature 16353** "Create C# bindings" (Epic **17681**
"Mature Fletcher"). This plan is the deliverable of **User Story 18689**, whose
acceptance criterion is *"Valid Implementation plan fulfilling C# requirements."*
Feature 16353 blocks Feature **20061** (Flight — refactor services onto Fletcher).
Flight's consumers are .NET processes hosting this binding only (Q11).

| Source | Requirement | Where addressed |
|---|---|---|
| 16353 | bindings for the **codec**, **PubSubProvider**, **PubSubArrow** | BIND-2/BIND-3 (codec over the binding ABI), BIND-4 (provider/pub-sub), BIND-5 (Arrow tier) |
| 16353 | "Make C# emit Rust-native accessor classes" | BIND-7 — ruled (Q19) as *C# accessor classes equivalent to the Rust-native ones*; the ADO text is corrected |
| 16353 | port instead of bind where a port is better | Applied to the **Arrow tier** and the **gateway client** — see [Part 1](#part-1--the-approach) |
| 18689 | bindings are **generic** | Satisfied structurally: the codec is schema-driven, and transport selection is a runtime string through the seam's registry |
| 18689 | support the **required native protocols** | **Fast DDS + XRCE-DDS as built-ins inside the shim** (Q3), selectable by name; every later PDA-ABI driver by path, with no BIND change |
| 18786 | C# versions of **all tests** as CI tests | [Part 4](#part-4--test-strategy); two exclusion classes ruled (Q10) |
| 18787 | bindings **against C ABI** so C# tests go green | BIND-1 … BIND-4 |
| 18788 | publish as NuGet, integrated into CI/CD | BIND-9 |
| 18789 | plugin generates C# for **all** its outputs | BIND-6 (row type, schema, topics, native pair), BIND-7 (Arrow view + accessor), BIND-8 (gateway pair). **BIND-8's part was missing until D-BIND-72 (2026-09-29)** |
| *(2026-08-31)* | **subscribe via TypeScript** | **BIND-T** — generated TS `Publisher`/`Subscriber` over the gateway |

### Constraints inherited from GIR (merged, `8ef1d0e`)

The generator was rewritten onto a **language-neutral IR** in round GIR, and its
locked decisions bind this round:

- **GIR-1 — the IR carries no language strings.** Each backend owns its own lookup
  table keyed on `ir::LogicalType`. `ts_backend_type_table` +
  `ts_backend_visitor` are explicitly *"the proof-point that a second language
  backend consumes the same IR through its own table."* **The C# backend is a
  third pair, built the same way.** Putting a C# type string on an IR node is a
  stop-and-ask.
- **GIR-2 — the wire format is byte-identical.** Generated *source* bytes may
  change; wire bytes may not. Applies to every emitter this round adds.
- **GIR-3 — the RBA accessor stays on a thin `FieldKind` projection** this round;
  the RBA↔IR reconciliation is round **RIR**, sequenced *after* BIND-Rust. The
  downstream chain is locked: **GIR → BIND-C# → BIND-Rust → RIR**.
- **GIR-7 — `Enum` is a first-class IR identity with symbols preserved**,
  specifically *"needed by #75 + BIND"*. C# emits a real `enum`, not the C++
  backend's int32 lowering — wire bytes unchanged. `Dictionary` is a scalar
  **modifier**, never a container.
- **GIR-8 — the descriptor-driven codec is BIND-2 scope, deliberately not
  foreclosed.** Verbatim: *"BIND-2 wraps 'generated encode/decode through a
  schema/descriptor-handle model,' which is closer to a generic codec than N
  bespoke encoders the C ABI must each wrap."* The bespoke recursive C++ edge
  path stays for MCU/device targets and is unaffected.

### Constraints inherited from the seam (PDA-DEC, merged)

The seam spec is frozen and BIND mirrors it; it may not change it (§1). What binds
BIND, by section:

- **§1 — no shared C header between the two ABIs.** BIND derives its C types from
  the seam's C++ types, never from PDA-ABI's, and vice versa. The two will look
  alike because both are views of the same types; that is a consequence, not a
  coupling (D-BIND-2′).
- **§2 — the four provider methods are the method set.** Adding one is a
  stop-and-ask. BIND wraps `Publisher`/`Subscriber` above them and the registry
  beside them.
- **§3 — every crossing type has a C-expressible ownership rule**: the write
  window plus refill hook and `AppendInPlace` (§3.1), `Blob` as owner plus span and
  `Attachments` as a positional, byte-ordered sequence (§3.2), `SharedSchema` as
  an owner-handle pair (§3.3), `SchemaArrival` with its typed outcomes (§3.4),
  topic segments with six refusals and a 246-byte cap (§3.5). The C form is
  conceptual, never a memory image.
- **§4 — selection is seam surface**, therefore binding surface: `Create(selector,
  config)` with a name-or-path selector and a typed core plus opaque document.
  Fletcher parses nothing (§4.2); neither does C#. The registry publishes no name
  query, and C# must not add one (owner ruling 2026-09-06).
- **§5 — one error type, one stable number, and the message crosses with it.**
  `PubSubStatus` is append-only; `PubSubError` is the only exception that leaves
  the seam; callbacks must not throw and are absorbed and counted (§5.3).
- **§6/§7 — threading, lifetime and delivery**: serialized per subscription on any
  thread; all callback arguments borrowed; destruction requires quiescence;
  re-entry refused everywhere; `Unsubscribe` blocks with the same-`Subscriber`
  carve-out; a schema-less transport passes null throughout.
- **§8 — zero-copy is a property of the seam and must be falsifiable.** The
  copy-accounting oracle in `integration-tests/pubsub-conformance` is inherited,
  and BIND adds a real producer to it.
- **§9 — what BIND owes**: mirror §3, §5, §6, §7; implement the caller side of
  `Publisher`/`Subscriber` plus §4 selection; inherit the four oracle suites plus
  `CallerTier`; add cases; change nothing.
- The eight obligations the seam leaves specifically to a binding are listed in
  [BIND-csharp-inherited-constraints.md](BIND-csharp-inherited-constraints.md) and
  each has a mechanism in D-BIND-17 … D-BIND-20.

---

## Branch strategy

- Branch: **`feature/csharp-bindings`**, created 2026-09-11 from `main` at
  `0a56829`. Per repo convention, PR branches are **rebased** onto `main`, not
  merged.
- Lands as **ONE pull request for the whole round** — #129 — as #125 (GIR) and
  #126 (PDA-DEC) each landed theirs. *Corrected 2026-09-16 (D-BIND-30); this
  bullet previously said "several PRs, one per stage boundary", which no round in
  this repository has ever done.* `main` gets one commit per round, and never a
  half-built binding: an empty `c-abi/` and three empty NuGet packages are
  scaffolding, not something the trunk benefits from carrying. Review granularity
  is unaffected — in this project it lives per item in this tracker and
  `plans/reviews/`, not in PR boundaries.
- **The exception is a fix that is not about the bindings at all.** A defect this
  round happens to find in shipping code goes to `main` on its own small PR, as
  #127 did — it should not wait weeks for a round to close. Precedent set at
  BIND-0: #130, the XRCE SIGPIPE fix.
- Each item must still be independently green *on the branch* before the next
  starts; PR/merge is the **user's** step.
- The first commit on the branch is the plan set (the eight `plans/BIND-*` files);
  `plans/pubsub-api-changes.md` stays out (deferred with the interface update).
- Completed rounds are archived to `docs/archive/<ROUND>/` (the convention HARD
  established) — BIND-10 does this at the end.

---

## Part 1 — The approach

### One codec, N language wrappers

> ✅ **AGREED 2026-08-31.** C# calls the existing C++ encode/decode rather than
> reimplementing it. Sharpened 2026-09-11 (Q1): the one codec is `positional_io`,
> and BIND-2 adds a **schema-driven C++ driver of it** over nanoarrow's
> `ArrowArrayView`; no wire byte is written by managed code except in
> `Eiva.Fletcher.GatewayClient`.

**TWO DRIVERS, ONE WIRE FORMAT — and which one a binding reaches (D-BIND-39,
2026-09-21).** "One codec" is true of the FORMAT and misleading about the code, and
the gap cost a round of confusion at BIND-3d, so it is written down here:

| | `arrow-bridge::Codec` | `NanoarrowCodec` (the shim) |
|---|---|---|
| Tier | **Arrow-native** | **the binding tier** |
| Callers | hand-written C++ already holding arbitrary Arrow data | every binding — C# now, Rust next round |
| Types | everything the wire format frames: unions, decimals, intervals, half-float, string/binary view, fixed-size binary | **the proto mapping**, plus `int8/16`, `uint8/16`, `large_string/binary`, `date32/64`, `time32/64`, `fixed_size_list` |
| Bucket 1 tests | **this one** | not this one |

**Every type a generator can emit, every binding carries.** Checked field by field
against `docs/wire-format-specification.md`: the proto mapping produces `bool`,
`int32/64`, `uint32/64`, `float/double`, `utf8`, `binary`, `enum`→int32,
`timestamp(ns)`, `duration(ns)`, the nullable wrappers, `struct`, `list`, `map` —
and the shim's codec accepts all of them and more. So **C++, C#, TypeScript and
Rust generated code agree by construction**, which is the parity property that
matters; the Arrow-native extras are unreachable from any `.proto` (`oneof` is in
the spec's own §"Unsupported Types", and decimals, intervals, half-float and the
view types are not proto constructs at all).

Three things settled it:

1. **GIR-8 already scoped it.** The descriptor-driven codec is explicitly *"BIND-2
   scope"*.
2. **Seam §8 requires zero-copy for rows and attachments**, enforced by a
   copy-accounting oracle. A managed codec on the far side of a native transport
   means the row crosses the boundary twice; one native codec writing into the
   provider's window keeps the single zero-copy path.
3. **ADO 18787 names the mechanism** — *"bindings against C ABI such that C#
   tests go green."*

So the adopted split is:

| Layer | Mechanism | Rationale |
|---|---|---|
| **Codec** (positional I/O) | **C ABI binding** over a *schema-driven* nanoarrow codec (BIND-2) | One wire-format implementation, **zero drift by construction**, proven by byte identity against `arrow-bridge`. A fixed, small ABI surface regardless of how many `.proto` messages exist (GIR-8). |
| **Transport** | **C ABI binding** over the seam's registry and `Publisher`/`Subscriber` | Runtime selection by string; every provider in the shim, and later every PDA-ABI driver, for free. |
| **Arrow tier** | **managed**, over `Apache.Arrow`, batch-first (D-BIND-25) | 16353's port-where-better clause. An official .NET Arrow implementation exists; binding Arrow C++ would be strictly worse, and the C Data Interface makes the handoff free. |
| **Generated code** (row classes, views, accessors) | **generated C#** from the IR | No runtime cost; the emitters are new backend tables + visitors, cheap now that GIR exists. Generated C# writes no wire bytes: rows go to and from Arrow (`ToArrow`/`FromArrow`). |
| **Gateway client** | **managed** (existing pure-TS design, ported) | Browser/WASM has no native option; the WebSocket path must stay dependency-free. The one managed codec, by exception. |

**What this buys:** the drift risk disappears, so the cross-language byte-parity
harness shrinks from a load-bearing correctness proof to an ABI conformance test.

**What it costs, stated plainly:** a NuGet package with native assets cannot run
in **WASM**, and NativeAOT/Unity/mobile support depends on the RID matrix rather
than being automatic. Browser and WASM clients keep using the gateway WebSocket
path, which is what the TypeScript work serves. A single-row publish builds a
one-row Arrow array before crossing (risk B-2 in the development plan §6); the
bind step in the codec surface makes batching structural, and the per-row cost is
measured in BIND-3 against the C++ generated publisher.

### Native protocols come from the seam's registry, not from BIND

**BIND does not bind Fast DDS or XRCE-DDS individually.** It binds
`ProviderRegistry::Create` once and the `PubSubProvider` handle it returns:

```
  C# app ──► binding ABI ──► Fletcher C++ ──► built-in providers in the shim:  inprocess · fastdds · xrce
   (this round)              (registry,       └─ SetPathResolver seat ──► PDA-ABI drivers by path (later)
                              Publisher/Subscriber, codec)
```

Consequences:

- The Fast DDS **QoS marshalling problem is gone**: config is an opaque
  provider-defined document (Fast DDS's own XML profile text, `key=value` for XRCE)
  that C# passes through unread, exactly as Fletcher does (seam §4.2).
- **No per-transport binding work.** The shim statically links and registers all
  three built-ins (Q3), so a C# application reaches every transport Fletcher has
  today by selector name. The gateway is the in-tree precedent for shipping Fast
  DDS statically.
- New protocols added later as PDA-ABI drivers require **zero** BIND work: a path
  selector is forwarded and today refuses with `kNotSupported`; when PDA-ABI lands,
  the shim calls `SetPathResolver` and nothing above changes.
- **No gate.** The pub/sub stage waits for nothing (D-BIND-3 as amended).

### TypeScript gets generated pub/sub over the gateway

The plugin emits TS interfaces, `TypedSchema` descriptors and service topic
constants — but **no** `Publisher`/`Subscriber` classes, so TS callers hand-wire
`client.subscribe(topic, Schema, cb)` while C++ callers get generated
`<Service>_<Method>Publisher` / `Subscriber`. **BIND-T** closes that gap with a
new TS backend emitter. It stays **WebSocket-only** and dependency-free, so it
keeps working in the browser — no native assets, no WASM exclusion.

---

## Part 2 — Components and packages

```
c-abi/                              ← NEW top-level Conan component `fletcher-c-abi`
  include/fletcher/abi/binding.h    ← the BINDING ABI: pure C, versioned, append-only structs
  src/                              ← nanoarrow codec · publish fusion · registry/publisher/subscriber entry points
  tests/                            ← byte identity vs arrow-bridge · compiles-as-C99 · borrow rule
dotnet/
  Fletcher.slnx                     ← XML solution format (comment-capable → license header OK)
  Directory.Build.props             ← single <VersionPrefix> for all managed packages
  .editorconfig · global.json
  src/
    Fletcher.Interop/               → Eiva.Fletcher.Interop        (P/Invoke surface + native assets; the ONLY runtimes/)
    Fletcher/                       → Eiva.Fletcher                (rows, codec, Arrow tier, pub/sub, SubscriberArrow)
    Fletcher.GatewayClient/         → Eiva.Fletcher.GatewayClient  (managed WebSocket — no native assets)
  tests/…
```

| Package | Native assets? | Depends on |
|---|---|---|
| `Eiva.Fletcher.Interop` | **yes** — `runtimes/{rid}/native/` (`win-x64`, `linux-x64` at first release) | — |
| `Eiva.Fletcher` | no | `Interop`, `Apache.Arrow` |
| `Eiva.Fletcher.GatewayClient` | **no** | `Apache.Arrow` (IPC schema parse) |

**Amended 2026-10-05 (D-BIND-74): FOUR managed packages.** `Eiva.Fletcher.Model`
(`Apache.Arrow` only; the lossless `Timestamp` and `Duration`, the two well-known types
that need a C# type of their own) joins the three below, so generated code in separate
assemblies shares one type. User-defined messages stay in the consumer's assembly.

**Three packages, not six** (Q2, D-BIND-14 as amended). With `Apache.Arrow` a base
dependency there is no dependency-free tier left for a `Core`/`Arrow`/`PubSub`
split to protect. Namespaces inside `Eiva.Fletcher` keep the C++ component names
(`Eiva.Fletcher.PubSub`, `Eiva.Fletcher.Arrow`, …) so a later split needs no
renames. `Interop` is the *only* package carrying native assets, so the RID matrix
and the LGPL question are isolated to one artifact (D-BIND-13). `GatewayClient`
has no native dependency at all — that is what keeps a browser/WASM-adjacent story
alive and mirrors `@eiva/fletcher-gateway-client`.

**One `dotnet/` component, one version source, one `dotnet-v` tag** — as
`gateway-client-ts` ships one npm package from one directory. `c-abi/` is a
separate top-level component because it is built by Conan/CMake, not by the
solution. Both join the **`0.5.x`** series.

### Two ABIs, derived from one seam, sharing no header

Seam §1 and PDA-ABI decision 2 say the same thing from each side: **no shared C
header**. Both boundaries are views of the same C++ types and will look alike; the
resemblance is a consequence, not a coupling.

| | Driver ABI (PDA-ABI) | Binding ABI (BIND) |
|---|---|---|
| Position | **below** the seam | **above** the seam |
| Fletcher's role | **caller** | **callee** |
| Implemented by | the driver (C++ on both sides, ruling 46) | Fletcher, in the shim |
| Called by | Fletcher only | applications, through `Eiva.Fletcher.Interop` |
| Header | `fletcher/abi/driver.h` | `fletcher/abi/binding.h` — **pure C**, because P/Invoke requires it |
| Derived from | the seam spec | the seam spec |

BIND's C spellings of `Blob` (owner handle plus retain/release), `Attachments`
(positional `size`/`KeyAt`/`ValueAt`), `SharedSchema` ({owner, `const ArrowSchema*`}),
`SchemaArrival` (`wait(arrival, timeout_ms, out)`), the write window and writer,
topic segments (pointer+length pairs), selector and document (pointer+length,
length authoritative) and the status enum (numbers from `core/README.md`'s
published table) are each **constructed from the C++ value** on BIND's side
(D-BIND-2′). PDA-ABI's `fletcher_status` is never seen by C#.

### Value transfer — decided (Q1, D-BIND-1a + D-BIND-23)

The codec's own C++ surface (`EncodeRow(const std::vector<std::shared_ptr<arrow::Scalar>>&)`)
cannot cross a C ABI, and returning a byte vector is the whole-row copy the oracle's
`StagingProducerIsCaught` control catches. Values therefore cross as **Arrow C Data
Interface** arrays, in a three-step surface whose full design, code and rationale
are in the development plan §3.2:

| Step | Entry point | Cost and lifetime |
|---|---|---|
| Open | `fl_codec_open(const ArrowSchema*)` | once per schema; validates against the mapping, precomputes the field plan; schema borrowed and deep-copied |
| Bind | `fl_rows_bind(codec, const ArrowArray*)` | once per batch; nanoarrow validates every buffer here; the array is **borrowed, never consumed** |
| Publish | `fl_publisher_publish_row(s)(…, rows, i, …)` | per row or range; the codec runs **inside `Publish`'s `RowEncoder`** and writes straight into the provider's window: `encode_copies == 0` |
| Encode | `fl_encode_row(rows, i, fl_write_window*)` | bytes in hand (WAL, tests, relay); one crossing per refill |
| Decode | `fl_decode_rows(codec, bytes, len, count, ArrowArray*)` | the mirror; a fresh array C# imports and owns; callable from inside the delivery thunk |

Behind it `NanoarrowCodec{Bind, EncodeRow(const BoundRows&, int64_t, WriteBuffer&),
DecodeRows}` on `positional_io`, nanoarrow only; on top `FletcherCodec.Bind →
BoundRows` and `Publisher.Publish(topic, rows, i)`, with `BoundRows.Dispose()`
unbinding before it releases. No encode entry point returns bytes; no per-value C
argument exists; the bespoke row-builder ABI stays rejected. The three-option
analysis that led here is recorded in the development plan; the alternative under
which "nested structures get worse" would be true is the one not chosen.

Nothing in the existing C++ tree changes for this: `PositionalWriter`/`Reader`,
`WriteBuffer`, `Publisher::Publish`, `OwnedSchema::DeepCopy` and pubsub's exported
nanoarrow are driven as is; `arrow-bridge`'s `Codec` stays the byte-identity oracle
target and is not modified. Files to touch are listed under BIND-2 in the
development plan §4.

### Plugin additions (no new component)

| `--fletcher_opt` token | Output | Backend |
|---|---|---|
| `csharp` | `<stem>.fletcher.cs` | `csharp_backend_*` (new) |
| `csharp_accessor` | `<stem>.fletcher.accessor.cs` + `<stem>.fletcher.arrow.cs` | `csharp_backend_*` (new) |
| `ts` *(existing, extended)* | `<stem>.fletcher.ts` — **now incl. `Publisher`/`Subscriber`** | `ts_backend_*` (extended) |

**How consumers run it (D-BIND-71, ruled 2026-09-29).** One plugin binary per
`protoc-v` release; a thin locator per ecosystem that only exposes its path; the
consumer writes the protoc call in its own build.

| Language | Locator | Exposes |
|---|---|---|
| C++ | Conan package `fletcher-protoc` | CMake target `fletcher-protoc::plugin` |
| TypeScript | npm `@eiva/protoc-gen-fletcher` | the `protoc-gen-fletcher` bin |
| C# | NuGet `Eiva.Fletcher.Protoc` (**new, protoc component, BIND-9**) | MSBuild property `$(FletcherProtocPlugin)` |
| Rust | in-tree `build.rs` today; a build-dependency crate later | `FLETCHER_PROTOC_PLUGIN` |

`Eiva.Fletcher.Protoc` is released by `cd.protoc.yml`, not `cd.dotnet.yml`, so the
dotnet component keeps its managed packages (four since D-BIND-74).

---

## Part 3 — Generator work, on the IR

GIR makes this the cheapest part of the round. The C# backend is a **third
backend pair**, mirroring the TS pair exactly:

| New file | Mirrors | Contents |
|---|---|---|
| `protoc/include/csharp_backend_type_table.hpp` + `.cpp` | `ts_backend_type_table.*` | The **only** place a C# type string may live. Keyed on `ir::LogicalType` + optional `ir::EnumIdentity` → `{cs_type_text, arrow_builder, …}`. Temporal types map **losslessly** (D-BIND-26). |
| `protoc/include/csharp_backend_visitor.hpp` + `.cpp` | `ts_backend_visitor.*` | Recursive IR visitor emitting the row POCO, a static `Apache.Arrow.Schema`, `ToArrow(IEnumerable<T>)` / `FromArrow(StructArray, int)`, and the typed `<Service>_<Method>Publisher`/`Subscriber` pair. **No wire bytes**: no `WriteTo`/`ReadFrom`. |
| `protoc/include/csharp_backend_accessor_visitor.hpp` + `.cpp` | `recordbatch_accessor_emitter.*`, `cpp_backend_view_visitor.*` | Both BIND-7 files, from one emitter (two were planned): `<stem>.fletcher.accessor.cs` — column-oriented `<Class>Accessor` with `TryMake(…, out accessor, out error)` (D-BIND-94) — and `<stem>.fletcher.arrow.cs` — `<Msg>View`, a `readonly struct` of (accessor, row) that is also the accessor's `RowView`, with only the field properties (D-BIND-97 and 98; was "over `(StructArray, int)`"). |
| `protoc/include/csharp_backend_columns.hpp` + `.cpp` | — | What each column converts as (`ClassifyColumn`), shared by the model's ToArrow / FromArrow and the accessor, so the two cannot drift (moved from `csharp_backend_visitor.cpp` at BIND-7a, unchanged). |

Hard rules, straight from GIR and the rulings:

- **No C# type string on an IR node** (GIR-1). It goes in the type table or nowhere.
- **Reuse `cpp_backend::BuildFlattenedFieldList`** for the message walk, exactly as
  `TsVisitor` does, so the C# field set cannot drift from the schema field set.
- **Wire bytes unchanged** (GIR-2). Generated source bytes are new files, so there
  is no golden to re-baseline — but the no-drift test must prove the *existing*
  outputs are untouched when the new tokens are passed.
- **Enums**: a **real `enum`** with the proto symbols, int32-backed (GIR-7,
  D-BIND-8). **Member names by protoc's C# rule (D-BIND-70, ruled 2026-09-29):**
  strip the enum-name prefix and PascalCase the rest, so `COLOR_RED` is `Color.Red`
  where C++ has `Color::COLOR_RED`. Numbers are verbatim; a nested enum's type name
  is flat, `Player_Color` (D-BIND-69).
- **Temporal values are lossless** (D-BIND-26): `Timestamp` and `Duration` map to
  `long`-backed structs carrying the unit, with `ToDateTimeOffset()` /
  `ToTimeSpan()` as explicit conversions; `DateTime` ticks are 100 ns and would
  silently lose two digits of a nanosecond timestamp. **Where they live (D-BIND-74,
  ruled 2026-10-05, after the first answer was reopened):** in a fourth managed package,
  `Eiva.Fletcher.Model`, which references `Apache.Arrow` alone, so the model layer stays
  Arrow-only (D-BIND-72) and one type serves every assembly that runs protoc.
- **Dictionary**: a scalar **modifier**, never a container (GIR-7); C# dictionary
  support is **deferred to whatever DICT concludes** (D-BIND-8).
- **The accessor emitter builds on the IR directly**, not on `FieldKind`
  (D-BIND-9), and **matches RBA's 2/3 nesting cap** (Q6) so `accessor-capstone`
  keeps one shared fixture; the asymmetry is RIR's to remove.
- **No `SubscribeInPlace`** in the generated C# subscriber (Q16): its C++ purpose is
  avoiding a per-sample allocation and the C# answer is batch delivery. The
  generated publisher offers `Publish(<Msg>)`, `Publish(<Msg>, attachments)` and
  `Publish(IEnumerable<<Msg>>)`, which binds once and publishes N.
- Multi-file invocation emits no duplicate filename (the RBA-1 failure mode); a
  shared helper, if needed, is emitted **exactly once** from `GenerateAll()`.
- **Names (D-BIND-69, ruled 2026-09-29).** Namespace `Fletcher.Gen.<PascalPkg>`,
  ignoring `option csharp_namespace`; nested messages flat as `Outer_Inner`, like
  every other Fletcher backend and **unlike protoc's `Outer.Types.Inner`, on
  purpose**; PascalCase properties with protoc's `_` suffix when a name collides
  with its class. Namespace and nesting follow Fletcher; identifiers follow protoc,
  because Fletcher had no precedent.
- **One shape for every language (D-BIND-72, ruled 2026-09-29).** Per `.proto`, one
  file with a row type per message, one schema bound to it, and the topics; every
  runtime consumes them, and none gets its own model type or schema. In C# the model
  layer compiles against `Apache.Arrow` only; the native pair (BIND-6) and the gateway
  pair (BIND-8) are generated on request, each referencing its own runtime package.

### BIND-T — TypeScript `Publisher` / `Subscriber`

`ts_backend_visitor` today emits *"per-message interface + TypedSchema, service
topic constants"*. BIND-T extends it with, per service method:

```ts
export class SensorFeed_StreamPublisher {
  constructor(client: FletcherClient) {}
  async publish(row: ISensorReading, attachments?: Attachments): Promise<void>;
}

export class SensorFeed_StreamSubscriber {
  constructor(client: FletcherClient) {}
  async subscribe(cb: (row: ISensorReading, att: Attachments) => void): Promise<Subscription>;
}
```

- Thin wrappers over the existing `FletcherClient.publish` / `.subscribe`, binding
  the topic constant and the `TypedSchema<T>` so callers stop passing both by hand.
- **Purely additive**: the existing hand-wired call style keeps working, and the
  existing descriptor byte-identity golden (`TsVisitor.DescriptorByteIdentical`)
  must stay green — the new classes are appended, never interleaved.
- **No new runtime dependency**, so the browser path is unaffected.
- Mirrors the C++ generated `<Service>_<Method>Publisher` / `Subscriber` naming
  exactly, so the two languages read the same.

---

## Part 4 — Test strategy

Counts are `grep -c "^TEST\(_F\|_P\)\?("` per file at `6c541e9`, so they are cases,
not parameterised instances, and reproducible from the tree (command in the
development plan §5). **Re-derived at BIND-0 (2026-09-14, branch base `0a56829`)
by that command**: every bucket total below is unchanged; the one correction is
the conformance suite's `CallerTier`, which is **21** cases and not 20.

**Re-derived 2026-09-25 (D-BIND-57), and now CHECKED.** #128 grew five files and added one, and
reached this branch by the merge of `main` on 2026-09-24 - after bucket 3 had been ported - so
every number below that it touched was wrong for a day and two BIND-4 closes. From here the
table is guarded: `scripts/check_test_matrix.py` recounts every file named here, re-adds every
row, fails on a test file in no bucket, and runs on every pull request (`ci.check-test-matrix`),
not only on those that touch a test - because a merge of `main` changes counts without putting
those files in the PR's own diff.


18786 requires "C# versions of all tests as CI tests". Because there is **one**
codec, the ported tests are **conformance tests of the binding**, not parity
tests between two implementations.

| Bucket | Files (cases) | Total | Treatment (ruled 2026-09-11, Q10) |
|---|---|---|---|
| 1 codec / envelope / buffer / status | `test_positional_io` 19, `test_envelope` 11, `test_write_buffer` 11, `test_status_taxonomy` 3, `test_codec` 70, `test_codec_edge` 30, `test_codec_property_fuzz` 2, `test_batch_decoder` 22 | **168** | **Not a case-by-case port target since D-BIND-39**: BIND-3's obligation is that every type the PROTO MAPPING produces round-trips through the binding, because `test_codec*` is `arrow-bridge`'s Arrow-native suite. #128 doubled `test_codec` and added `test_batch_decoder` (`arrow-bridge`'s own batch decoder); both stay out of scope for that reason. `test_status_taxonomy` ports as "the C# enum matches `core/README.md`'s published table". `test_codec_edge` grew by 7 in BIND-9b-2 (D-BIND-156's null refusal), out of scope for the same reason. |
| 2 gateway client | `test_schema_codec` 23, `test_publish_frame` 9, TS 38 | **70** | Port (BIND-8), true parity: managed code both sides. **Re-mapped at the BIND-8 review (D-BIND-128):** `test_schema_codec` tests the GATEWAY's own C++ (its JSON schema parser, the declared-schema measurement, base64, `FieldToJson`), all 23 recorded as gateway-internal; what the client owes it since D-BIND-112 is that its `schemaIpc` reads back as the same declaration, held by `ControlMessagesTests.BuildCreateTopicAnnouncesTheSchemaAsSchemaIpcAndNotAsJson` over every C++ `.ipc` golden and live by `gateway-dotnet-crosslang`. `test_publish_frame` and the TypeScript cases (two added by D-BIND-130) port one for one. **TS 38 is the suite BIND-8 ported; BIND-T (2026-10-29) added 36 TypeScript cases that mirror C#'s, not port targets (74 in `gateway-client-ts` now, review T6).** |
| 3 pub/sub semantics | `test_publisher_subscriber` 44, `test_segments` 5, `test_pubsub_arrow` 33 | **82** | Port over `inprocess`. **Split confirmed 2026-09-24: the `pubsub` cases are BIND-4's; `test_pubsub_arrow`'s are BIND-5's, whose acceptance row claims them and whose `SubscriberArrow` did not exist before it.** **#128 added 26 `pubsub` cases and 18 `pubsub-arrow` ones (D-BIND-57)**; of the 49 `pubsub` cases, **44 are mapped** - 12 of #128's over `inprocess`, 11 over real Fast DDS in the transport lane, because only a provider that knows options or has a schema channel can answer them - **and 5 are EXCLUDED**, 2 by D-BIND-47 and 3 by D-BIND-57. D-BIND-47's two: `UnsubscribeLastSubscriberUnsubscribesFromProvider` and `UnsubscribeWithRemainingSubscribersKeepsProviderSubscription` assert `mock->unsubscribe_count`, the fan-out's provider-level bookkeeping, which lives in the C++ `Subscriber` the binding WRAPS and does not reimplement - porting them would run that C++ a second time and call it a test of the binding, and the managed surface has no view of provider-level subscriptions by design (D-BIND-24). Reasoning and the full case-by-case mapping: `dotnet/tests/Fletcher.Tests/BucketThreeConformanceTests.cs`. |
| 4 native providers | Fast DDS: `test_fast_dds_pubsub_provider` 71, `test_profile_document` 33; XRCE: `test_xrce_provider` 7, `test_xrce_document` 11 — **port as driver-selection tests (122)**; `test_fletcher_sample_pub_sub_type` 29 — **excluded**, provider-internal (the Fast DDS `TypeSupport`, a C++ type no managed code touches) | **151** | 122 by driver selection - ONE body over every transport (`integration-tests/pubsub-transport-conformance`), not a port per case; 28 excluded, documented. #128 added 45 of these. |
| 5 generator | `test_type_mapper` 36, `test_option_metadata` 33, `test_schema_builder` 9, `test_schema_visitor` 9, `test_ir` 8, `test_schema_codec_lockstep` 2, `test_topic_path` 3, `test_ts_visitor` 14, `test_csharp_type_table` 26, `test_csharp_visitor` 63 | **203** | **Stay in C++** — they test a C++ generator; **extended** with `test_csharp_visitor`, `test_csharp_type_table` and the TS pub/sub emitter cases. |
| 6 no managed analogue | `test_owned_schema` 1 | **1** | Excluded, documented (an `ArrowSchemaDeepCopy` ENOMEM path). |
| Conformance (inherited, not a port target) | `integration-tests/pubsub-conformance` 95 cases; `CallerTier` 22 of them | — | The **oracle** seam §9 hands BIND. BIND writes a C# arm of `CallerTier` and adds cases to the C++ suite (§12.1 expects it); each C# case names the C++ case it mirrors and a script checks the mapping is total. |

**276 of 302 non-generator cases port to C#** — the 302 is buckets 1–4. *Corrected
2026-09-24 from 278: bucket 3 turned out to carry two cases that cannot be ported, for the
structural reason its row now gives, so the exclusions are the 24 provider-internal ones PLUS
those 2.* The subtraction from the total is those 26 cases. `test_owned_schema` is bucket 6
and was never inside the total, which is where the old **275 of 300** came from: it
subtracted a case the total did not hold, and bucket 4 was carrying a stale
`test_xrce_document` count besides. Both exclusion classes stay documented; the 98
generator cases stay in C++ and grow. **18786 is not closed before Bucket 4 is green over `fastdds` and
`xrce`.**

**Re-counted 2026-09-25 (D-BIND-57): 416 of 449.** Buckets 1-4 now hold 449 cases (161 + 56 + 82
+ 150); 33 are excluded - the 28 provider-internal ones and bucket 3's 5. Read the number for what
it is since D-BIND-39 and bucket 4's one-body design: cases IN SCOPE for C#, not C# test methods.
Bucket 1 is answered by the proto-mapping round trip and bucket 4 by one body over every
transport. `TS 36` in bucket 2 is carried forward, not re-counted - the checker counts C++ only.

### What replaces the byte-parity harness

With one codec there is no second implementation to drift, so the harness is
**repurposed, not deleted**:

| Test | Proves |
|---|---|
| `c-abi/tests` — `NanoarrowCodec.ByteIdenticalToArrowBridge` | The nanoarrow codec produces the bytes `arrow-bridge`'s `Codec` produces across the codec's own Arrow fixture corpus — every arm of both switches, counts asserted (D-BIND-35): **one wire format**. |
| `integration-tests/binding-abi-conformance` | The C# wrapper round-trips **every** scenario through the ABI with the values C++ produces, reusing the existing corpus rather than a C#-only fixture. **Corpus ruled (D-BIND-40, 2026-09-21): the codec's own five Arrow fixtures, lifted into `c-abi/tests/codec_corpus.hpp` and INCLUDED by the emitter so one definition serves both consumers.** What it asks is not "do C# and C++ encode the same bytes" — there is one codec, so that could not fail — but whether two INDEPENDENT Arrow implementations hand that codec the same batch. Emitted every run, never committed. |
| **Copy-accounting oracle**, extended | `CopyAccounting.BindingProducerWritesInPlace` (BIND-2, a producer through the ABI from C) and the same run with the **C# producer** (BIND-5): zero-copy for rows and attachments is falsifiable, not asserted. The oracle's scope note states the UTF-16→UTF-8 transcoding boundary (D-BIND-1b). |
| C# arm of `accessor-capstone` | C# joins as the third language against the same proto, fixture and committed oracle. Fully load-bearing: the accessor is **generated C#**, an independent implementation. |
| C# arm of `CallerTier` | The tier BIND actually wraps, mapped case for case onto the C++ suite. |
| `integration-tests/gateway-dotnet-end-to-end` | Live gateway ↔ C# client, control + data frames. |
| `integration-tests/protoc-gateway-client-ts` *(extended)* | BIND-T's generated TS pub/sub classes against a live gateway. |

The **generated** artifacts (row classes, views, accessors, TS pub/sub) remain
genuinely independent implementations per language, so cross-language parity
testing still matters *there* — it is only the hand-written codec that collapses
to one.

---

## Part 5 — CI/CD design

### Toolchain provisioning

The devcontainer pins Node 24 in its Dockerfile and CI's `setup-node` tracks it
"in lock-step"; Rust, by contrast, is *not* in the image and each Rust CI job
installs a pinned toolchain via rustup. .NET follows the **Node** pattern (Q7,
D-BIND-21):

- Add a pinned **.NET 10 LTS SDK** to `.devcontainer/Dockerfile` via
  `dotnet-install.sh`, with a `DOTNET_SDK_VERSION` build arg beside the existing
  `NODE_VERSION` / `PRETTIER_VERSION` pins.
- `dotnet/global.json` pins the same band with `rollForward: latestFeature`, so
  the container, CI and a developer's host cannot silently diverge.
- Libraries multi-target **`net8.0;net10.0`** so a consumer still on .NET 8 can
  reference them until it moves; .NET 8 leaves support in November 2026, and
  dropping `net8.0` is a later MINOR bump.
- `Apache.Arrow` pinned to a version whose `Apache.Arrow.C` namespace exports and
  imports `CArrowArray`/`CArrowSchema`, verified at BIND-0.
- Set `DOTNET_CLI_TELEMETRY_OPTOUT=1` and `DOTNET_NOLOGO=1` in the image.

Rationale: the .NET SDK is a first-class dev dependency (a developer doing
"Reopen in Container" must be able to `dotnet test`), whereas Rust is exercised
by exactly one integration test. This costs ~200 MB in the image and removes a
toolchain-install step from every .NET job.

### New workflows

| File | Trigger | Shape |
|---|---|---|
| `ci.c-abi.yml` | `workflow_call` (`devcontainer-image` input) | Conan build of `fletcher-c-abi` on both platforms; the codec byte-identity test, the compiles-as-C99 test, the packed-size budget for the shim, and the conformance suite's ABI subject. Runs on an **empty** ABI from BIND-0 onward, so the Linux signal is standing before real code exists. |
| `ci.dotnet.yml` | `workflow_call` (`devcontainer-image` input) | **linux** job in the container + **windows** job native with `actions/setup-dotnet` — the exact two-job split `ci.integration-test.protoc-gen-fletcher-rust.yml` uses. `dotnet restore --locked-mode` → `build -c Release` → `test --logger trx` → `pack`. Uploads `*.nupkg` + `*.snupkg`. The pack step stages `LICENSE` and `THIRD-PARTY-LICENSES.txt` and fails if either is missing (see below). |
| `ci.format-check-cs.yml` | `pull_request`, paths `dotnet/**`, `.editorconfig` | `dotnet format --verify-no-changes --severity warn`. Standalone and self-triggering, **not** in the `pr_gate` aggregation — same as `ci.format-check-ts.yml`. |
| `ci.integration-test.protoc-dotnet.yml` | `workflow_call` | `conan create protoc/.`, locate the binary in the Conan cache, export `FLETCHER_PROTOC_PLUGIN`, `dotnet test`. **Copy the discovery block from the Rust workflow verbatim** — the `find ~/.conan2/p -path '*/p/bin/fletcher-protoc' -type f -print -quit` + explicit-error-if-empty idiom is already hardened for both platforms. **Built at BIND-6b-2 (2026-10-05)**, with the block copied and one change, said in the file: the Windows leg converts both paths with `cygpath -w`, because MSBuild reads them as Windows paths and Git Bash's `find` returns `/c/...`. Wired into `ci.pr.yml`'s filter, caller jobs and `pr_gate` (`needs:` and `results:`), and into `ci.format-check-cs`'s project list. |
| `ci.integration-test.gateway-dotnet-end-to-end.yml` | `workflow_call` | Gateway exe + C# client; mirrors `ci.integration-test.gateway-end-to-end.yml`. |
| `ci.integration-test.gateway-fastdds-dotnet.yml` | `workflow_call` | Bucket-4 over `fastdds` and `xrce` by selector. Linux-only initially, matching the other FastDDS integration tests. |
| `cd.dotnet.yml` | `push` tag `dotnet-v[0-9]*.[0-9]*.[0-9]*` | `setup-devcontainer` → `ci.dotnet.yml` → `publish` → `create-release`, structurally identical to `cd.gateway-client.yml`. |

**Landed at BIND-0** (2026-09-14): `ci.c-abi.yml`, `ci.dotnet.yml`,
`ci.format-check-cs.yml` and the `ci.pr.yml` wiring for the first two. The four
integration-test workflows land with the items that give them something to run
(`protoc-dotnet` with BIND-6/7, the two gateway ones with BIND-8, the FastDDS one
with BIND-4), and `cd.dotnet.yml` with BIND-9: a `uses:` pointing at a workflow
file that does not exist yet fails the whole PR run, so they cannot be wired
ahead of themselves.

Two measurements ride in `ci.c-abi.yml` from the kickoff rather than waiting for
BIND-9, because both are properties of the artifact and not of the packaging: the
**per-RID size** of the shim (reported into the job summary on both platforms; no
threshold is enforced until BIND-9 sets one against real numbers) and, on Linux,
the **export table**, which is asserted to contain nothing outside the `fl_`
prefix. See the open item in Part 8 for why that assertion is Linux-only.

### Licence files in the package (after #127)

The native shim statically links the same permissively licensed stack the gateway
does (Fast DDS, Fast CDR, foonathan_memory, tinyxml2, Micro XRCE-DDS, microcdr,
nanoarrow) plus Fletcher's own LGPL components, so `Eiva.Fletcher.Interop` owes the
same two files the gateway archive carries since #127: the repository `LICENSE` and
a `THIRD-PARTY-LICENSES.txt`. **Reuse `gateway/deployers/third_party_licenses.py`
as is**, run against the shim's Conan package with
`-c tools.graph:skip_binaries=False` (mandatory: a static library's dependencies
have no package folder otherwise) and the whole host graph rather than
`dependencies.host` (which drops skipped header-only packages that are nonetheless
in the binary). Stage both files into the package on **every** CI run and fail the
pack step if either is missing, mirroring `ci.gateway.yml`'s archive check. The
`GatewayClient` asset-isolation check already opens the `.nupkg`, so the licence
check rides in the same step.

### `ci.pr.yml` wiring

Three mechanical edits, all following the established pattern. **BIND-0 did the
first two lanes** (`c-abi`, `dotnet`); `integration-dotnet` and its callers follow
with the items that own them:

1. Three new `detect-changes` outputs + `dorny/paths-filter` blocks —
   **positive patterns only** (the file's own comment explains why `!` negations
   break `predicate-quantifier: some`):
   - `c-abi`: `c-abi/**`, `core/**`, `pubsub/**`, plus the shared plumbing paths
   - `dotnet`: `dotnet/**`, `c-abi/**`, `.github/workflows/ci.pr.yml`,
     `.github/workflows/ci.dotnet.yml`,
     `.github/workflows/ci.setup-devcontainer-image.yml`,
     `.github/actions/pull-devcontainer-image/**`, `.devcontainer/**`
   - `integration-dotnet`: `protoc/**`, `dotnet/**`,
     `integration-tests/protoc-dotnet/**`, plus the shared plumbing paths
2. Five new caller jobs (`c-abi`, `dotnet`, `integration-test-protoc-dotnet`,
   `integration-test-gateway-dotnet-end-to-end`,
   `integration-test-gateway-fastdds-dotnet`), each
   `needs: [detect-changes, setup-devcontainer]`, `if:` on its filter output,
   `secrets: inherit`.
3. All five added to `pr_gate`'s `needs:` **and** its `results:` list — the
   `pr-gate` action counts `skipped` as a pass, so an omission from `results`
   silently stops gating.

This takes `ci.pr.yml` from 19 to 24 conditional jobs; they only run on
`c-abi/`, `dotnet/` or `protoc/` changes, and they share the one pre-built
devcontainer image. At BIND-0 it stands at 21 (the two new lanes wired, gated and
listed in `pr_gate`'s `results:`).

### CD — publishing to `nuget.eiva.com` (D-BIND-28)

> **⛔ Superseded 2026-10-28 by D-BIND-176:** the packages go to nuget.org, pushed by Trusted
> Publishing from a GitHub-hosted `nuget-org` job in `cd.dotnet.yml` and `cd.protoc.yml`; there
> is no self-hosted runner, no `cd.nuget-feed.yml` and no `NUGET_EIVA_*` setting. The rest of this
> section is the history.
>
> **Built by BIND-9b-6 (2026-10-23), and changed from the sketch below by D-BIND-153 and
> D-BIND-162 to 164:** the Release is created on a GitHub-hosted runner from the packages the gate
> tested, and the feed push is its own job, `cd.nuget-feed.yml`, skipped until the runner exists;
> the gate (`ci.package-consumer.yml`) runs first, with the gateway and protoc built beside dotnet;
> a dispatch is a dry run. `.github/workflows/cd.dotnet.yml` is the reference, not the YAML below.

**Mirror the Conan CD workflows exactly** (`cd.core.yml` and its siblings): a tag push
runs `setup-devcontainer` → `ci.dotnet.yml` (build, test, pack, upload artifacts) →
one `publish` job that verifies the tag against the version, downloads the packed
artifacts, pushes them, and creates the GitHub Release with the packages attached.
Two deltas from the Conan shape, both ruled 2026-09-14: the publish target is EIVA's
**internal feed**, and because that host resolves to a **private address** the
`publish` job runs on a **self-hosted runner inside the EIVA network** — the only
self-hosted job in the repository, apart from `cd.protoc.yml`'s NuGet push of
`Eiva.Fletcher.Protoc` (D-BIND-71), which is tag-triggered in the same way. Build, test and pack stay on GitHub-hosted runners.

```yaml
name: cd.dotnet
on:
  push:
    tags:
      - 'dotnet-v[0-9]*.[0-9]*.[0-9]*'
jobs:
  setup-devcontainer:
    uses: ./.github/workflows/ci.setup-devcontainer-image.yml
    secrets: inherit
  release:
    needs: setup-devcontainer
    uses: ./.github/workflows/ci.dotnet.yml
    with:
      devcontainer-image: ${{ needs.setup-devcontainer.outputs.image }}
    secrets: inherit
  publish:
    needs: release
    runs-on: [self-hosted, eiva-network]      # label fixed when the runner is registered (BIND-9)
    permissions:
      contents: write
    steps:
      - uses: actions/checkout@v6
        with:
          sparse-checkout: |
            dotnet
            .github
      - uses: ./.github/actions/verify-tag-version-dotnet   # new composite action, see below
        with:
          props-file: dotnet/Directory.Build.props
          tag-prefix: dotnet-v
      - uses: actions/download-artifact@v8.0.1
        with:
          name: dotnet-packages
          path: artifacts
      - name: Push packages to nuget.eiva.com
        run: >-
          dotnet nuget push 'artifacts/*.nupkg'
          --source https://nuget.eiva.com/v3/index.json
          --api-key ${{ secrets.NUGET_EIVA_API_KEY }}
          --skip-duplicate
      - name: Push symbol packages
        run: >-
          dotnet nuget push 'artifacts/*.snupkg'
          --source https://nuget.eiva.com/v3/index.json
          --api-key ${{ secrets.NUGET_EIVA_API_KEY }}
          --skip-duplicate
      - name: Create GitHub Release
        env:
          GH_TOKEN: ${{ github.token }}
        run: |
          gh release create "${{ github.ref_name }}"             --title "${{ github.ref_name }}"             --generate-notes             artifacts/*.nupkg artifacts/*.snupkg
```

**Feed facts, checked 2026-09-14:** `nuget.eiva.com` is a **BaGetter** server (Kestrel).
Its service index at `/v3/index.json` exposes `PackagePublish/2.0.0` at
`/api/v2/package` and `SymbolPackagePublish/4.9.0` at `/api/v2/symbol`, so
`dotnet nuget push` routes `.snupkg` files to the symbol endpoint by itself. Reads are
anonymous; `PUT /api/v2/package` answers **401 without an API key**. The host resolves
to a `10.x` address, which is why GitHub-hosted runners cannot push to it.

**One-time setup (user actions, outside a PR):** register the self-hosted runner in the
EIVA network with the label used above, with the .NET SDK (D-BIND-21) and `gh`
installed; create an API key on `nuget.eiva.com` and store it as the
`NUGET_EIVA_API_KEY` repository secret. The runner executes **only this job**: no
build, no test, and never pull-request code — `cd.dotnet.yml` is tag-triggered, so
`pull_request` events cannot reach it. Its label, what it runs and what it must have
installed are documented in `dotnet/README.md` (BIND-10). **As built (9b-6, D-BIND-163), three
admin steps, none needed for a release:** (1) register the runner with the labels
`self-hosted` and `eiva-network`, with bash, the .NET SDK and `gh`; (2) store a `nuget.eiva.com`
API key as the `NUGET_EIVA_API_KEY` repository secret; (3) set the repository variable
`NUGET_EIVA_RUNNER` to `true`. Then dispatch `cd.nuget-feed.yml` with the tag of every release
cut before step 3. The runner runs only `cd.nuget-feed.yml`'s push job, documented in that file.

**Consumers** add the feed to their `nuget.config`
(`<add key="eiva" value="https://nuget.eiva.com/v3/index.json" />`). Pre-release
versions are opt-in there exactly as on NuGet.org.

**NuGet.org later.** When the packages go external, add a second push step with Trusted
Publishing (OIDC: `id-token: write`, `NuGet/login@v1`, the analogue of the npm
`--provenance` flow in `cd.gateway-client.yml`) and request the `Eiva.Fletcher.*`
prefix; nothing else in the workflow changes. The LGPL relinking decision (Q9) is due
before *that* step, not before this one (D-BIND-28). **Moved forward 2026-10-20:** the
repository is public, so BIND-9a's per-push bundle (D-BIND-139) is already external; Q9 is
ruled for it (D-BIND-140).

Also in `cd.dotnet.yml`:

- **Pre-releases need no dist-tag equivalent.** NuGet has no dist-tags; SemVer 2.0
  pre-release suffixes (`0.5.0-alpha`) are *inherently* opt-in on NuGet (excluded
  from resolution unless the consumer asks for pre-release), so the
  `Resolve dist-tag` step from the npm workflow has **no counterpart** and must
  not be transliterated.
- `ContinuousIntegrationBuild=true`, `Deterministic=true`, SourceLink, and
  `.snupkg` symbol packages pushed to the feed's symbol endpoint.
- `PackageLicenseExpression=LGPL-3.0-or-later` in `Directory.Build.props`.

### `verify-tag-version-dotnet` composite action

`verify-tag-version-npm` shells out to `node -p require(...)`; the .NET analogue
reads `<VersionPrefix>` / `<VersionSuffix>` out of `Directory.Build.props`. Keep
it dependency-free (an `xmllint` / `sed` extract in bash) so the action does not
need the SDK on the runner.

### `ci.license-headers.yml` denylist additions

Headers are checked on every tracked file minus a denylist; `.md` is already
excluded wholesale. C# additions:

| File | Header? | Action |
|---|---|---|
| `*.cs`, `*.csproj`, `*.props`, `*.slnx`, `nuget.config` | ✅ yes | `//` for `.cs`, `<!-- -->` for the XML ones. **`.slnx`, not `.sln`** — the legacy `.sln` format cannot carry comments; the new XML `.slnx` can. |
| `global.json` | ❌ strict JSON parser | **denylist** — same reason as `package.json` |
| `packages.lock.json` | ❌ generated, strict JSON | **denylist** — same reason as `package-lock.json` |
| `.editorconfig` | ✅ `#` comments | header |

Generated `.fletcher.cs` files carry the header from the emitter itself, and are
gitignored in the integration tests.

---

## Part 6 — Sequencing + work-item tracker

Locked upstream chain (GIR-3): **GIR → BIND-C# → BIND-Rust → RIR.** GIR is merged,
so BIND-C# is unblocked. Nothing in the round waits on PDA-ABI. Following the
2026-09-05 process ruling ("ceremony is expensive; fewer, larger items"), items are
fewer and larger than in the first draft.

```
Track A (native + managed core)   BIND-0 → BIND-1 → BIND-2 → BIND-3 → BIND-4 → BIND-5
Track B (generator, protoc/)      BIND-6 → BIND-7        (BIND-7 needs BIND-3's Arrow import)
Track C (managed-only)            BIND-T · BIND-8         (independent; can start day one)
Track D (pipelines, docs)         BIND-9a (preview) → BIND-9b (starts with BIND-0, closes last) · BIND-10
```

Critical path: **BIND-0 → BIND-1 → BIND-2 → BIND-3 → BIND-4**. BIND-2 is the long
pole. `protoc/` and `arrow-bridge/` are contended with the modernization branch's
pending commits (development plan P-7); the landing order is agreed at BIND-0.

Status: ⚪ not-started · 🔴 in-progress · 🟢 done (forcing test green + reviewed)
Kind: 🟪 spec · 🟦 impl · 🔬 proof · ⚙ pipelines · 📓 docs

| ID | Item | Track | Kind | Depends on | Forcing test | Status |
|---|---|---|---|---|---|---|
| BIND-0 | Kickoff: decisions recorded, skeleton `c-abi/` + `dotnet/` green in CI on an empty ABI, matrix committed | A/D | 🟪 | — | `ci.dotnet.yml` + `ci.c-abi.yml` green on both platforms | 🟢 |
| BIND-1 | The binding ABI header, reviewed as a specification (no implementation) | A | 🟪 | BIND-0 | `BindingAbi.CompilesAsC99AndIsSelfContained` | 🟢 |
| BIND-2 | Nanoarrow schema-driven codec + publish fusion + the oracle's ABI producer | A | 🟦 | BIND-1 | `NanoarrowCodec.ByteIdenticalToArrowBridge` + `CopyAccounting.BindingProducerWritesInPlace` | 🟢 |
| BIND-3 | `Eiva.Fletcher.Interop` + codec/Arrow tier in `Eiva.Fletcher` | A | 🟦 | BIND-2 | Every PROTO-MAPPING type round-trips through the binding (D-BIND-39, was "Bucket 1 green"); `ErrorTests.EveryHardCaseKeepsItsMessage` | 🟢 |
| BIND-4 | Pub/sub in `Eiva.Fletcher`: registry, `Publisher`, `Subscriber`, `SchemaArrival`, thunk discipline, error handling | A | 🟦 | BIND-3 | Bucket 3 over `inprocess`; Bucket 4 over `fastdds`/`xrce` by selector; C# arm of `CallerTier`; per-row publish benchmark recorded (D-BIND-37) | 🟢 (reopened twice on 2026-09-25 and re-closed on CI: D-BIND-56 at `78ac363`, bucket 4 over `xrce`; D-BIND-57 at `bdd4fba`, bucket 3's 49-case file set mapped and the matrix checked) |
| BIND-5 | `SubscriberArrow` batch-first + the copy oracle end-to-end from C# | A | 🔬 | BIND-4 | `pubsub-arrow` cases; copy oracle green with the **C#** producer (D-BIND-58) | 🟢 (closed on CI at `a43c384`, `ci.pr` run 36563607601, 50/50, after a fresh-context review whose 9 BLOCKERs were fixed; D-BIND-58 to 68; conformance 11 of 11) |
| BIND-6 | C# backend on the IR: type table + visitor → `<stem>.fletcher.cs` | B | 🟦 | — (GIR) | `CsVisitor.*`, `CsVisitorPair.*`, `CsVisitorCrossFile.*`, `CsNames.*` and `CsTypeTable.*` in `protoc/tests` (named `CsharpVisitor.*` until review T-D5 corrected it, 2026-10-14); no-drift test unchanged | 🟢 (closed on CI at `63ab057`, `ci.pr` 38575041994, after a fresh-context review whose 5 BLOCKERs were fixed and 10 questions ruled, D-BIND-83 to 93; the minor debt carried as known, listed in BIND-6's section) |
| BIND-7 | Arrow view + accessor emitters (`csharp_accessor`) + capstone third arm | B | 🟦 | BIND-6, BIND-3 | `accessor-capstone` C# arm `observed == expected`; `StructArray` windowing fixture at non-zero offset | 🟢 (closed on CI at `268dceb`, `ci.pr` 38746753749, after a fresh-context review whose 2 BLOCKERs were fixed and 5 questions ruled, D-BIND-103 to 107; rulings during the round D-BIND-94 to 102; the minor debt carried as known, listed in BIND-7's section) |
| BIND-T | TS `Publisher`/`Subscriber` emitter | C | 🟦 | — | `TsVisitor.DescriptorByteIdentical` still green + new emitter cases | 🔴 (rulings D-BIND-181 to 184, 2026-10-28; slices T-1 to T-5) |
| BIND-8 | `Eiva.Fletcher.GatewayClient` (managed port; the codec exception) | C | 🟦 | — | Bucket 2 green (the Part 4 file set, not a count); `Package.GatewayClientHasNoRuntimesFolder` | 🟢 (closed on CI at `750d79d`, `ci.pr` 39375700668, 56/56, every side workflow green, after a fresh-context review whose 5 BLOCKERs were fixed and 12 questions ruled, D-BIND-121 to 132; rulings during the round D-BIND-108 to 120, and D-BIND-133 (slice 8f, batched delivery); one finding after the review fixed (A1); performance carried as known debt, listed in BIND-8's section) |
| BIND-9a | A preview for testers (split from BIND-9 2026-10-20): one bundle per push to this branch, a 30-day CI artifact with the four NuGet packages (shim for both RIDs), the gateway binaries and the plugin binaries; nothing released before the merge (D-BIND-134 to 152) | D | ⚙ | BIND-3, BIND-8 | A consumer test using only the bundle publishes natively, generates code with the bundled plugin and reaches the bundled gateway, on Linux and Windows; `Interop` carries exactly the two shims; licence files in every package | 🟢 (closed on CI at `133631a`, `ci.pr` 39592797595, 60/60 on its third attempt after two one-off Windows timing failures in unchanged tests, every side workflow green, after a fresh-context review whose 6 BLOCKERs were fixed and 10 questions ruled, D-BIND-145 to 152; rulings during the item D-BIND-134 to 144; the review's deferred items listed in BIND-9b) |
| BIND-9b | The rest of BIND-9: publish to nuget.org by Trusted Publishing (D-BIND-176; was `nuget.eiva.com` from a self-hosted job, D-BIND-28), size budget, glibc floor, `Eiva.Fletcher.Protoc`, NativeAOT on Windows and `fastdds`, C#-native cross-language rows | D | ⚙ | BIND-9a, all for release | `cd.dotnet.yml` dry run; packed-size check; asset-isolation + licence check | 🟢 (closed on CI at `9c9e2bc`, `ci.pr` 40702620300, 61/61 on the first attempt, every side workflow green, after a fresh-context review whose 4 BLOCKERs were fixed and 5 questions ruled, D-BIND-175 to 179; rulings during the item D-BIND-153 to 174; D-BIND-180 from a reported issue; the dry runs, the bump, the retirement and the tags are the merge runbook's, after BIND-T and BIND-10 (D-BIND-174); the review's DEBT and NITs listed in BIND-9b's section) |
| BIND-10 | Docs, TD-009, archive to `docs/archive/BIND/` | D | 📓 | all | docs review | ⚪ |

---

## Part 7 — Items (user stories + acceptance)

### BIND-0 — Kickoff

**As** the round's implementer, **I want** the decisions recorded, the two new
components building empty in CI, and the test matrix committed, **so that** the
first automated run happens before any real code exists.

**Acceptance**

- The 2026-09-11 rulings are in `BIND-locked-decisions.md` (D-BIND-1 … D-BIND-27)
  and this file; ADO 18689 points at the committed plan; 16353's accessor wording is
  corrected.
- `.devcontainer/Dockerfile` gains the .NET 10 LTS SDK via `DOTNET_SDK_VERSION`;
  `dotnet/global.json` pins the same band.
- `c-abi/` builds a shim exporting only `fl_binding_abi_version()`; `dotnet/`
  builds three empty packages; `ci.c-abi.yml` and `ci.dotnet.yml` are green on both
  platforms. Seam §12.4's lesson: the first lane run found seven defects local
  green could not.
- Part 4's matrix is re-derived by the stated command and committed.
- `Apache.Arrow` is pinned and its C Data Interface export/import verified for the
  mapping's types, nested ones included (N-9).
- The per-RID size of the shim with all three built-ins is measured and recorded
  against the packed-size budget.
- Landing order with the modernization branch's pending `arrow-bridge`/`protoc`
  commits agreed (P-7).
- ~~The self-hosted publish runner registered in the EIVA network and the
  `NUGET_EIVA_API_KEY` secret created on `nuget.eiva.com`~~ — **struck 2026-09-17
  (D-BIND-33) as a duplicate of BIND-9's own bullet.** NuGet.org Trusted Publishing
  is deferred until the packages go external (D-BIND-28), also BIND-9's.

**State — 2026-09-14 (implementation started)**

| Acceptance bullet | State |
|---|---|
| Rulings recorded (D-BIND-1 … D-BIND-28); ADO 18689 / 16353 updated | 🟢 done 2026-09-11 / 14 |
| `.devcontainer/Dockerfile` gains .NET 10 LTS via `DOTNET_SDK_VERSION`; `dotnet/global.json` pins the same band | 🟢 `10.0.400`, `rollForward: latestFeature`; image builds locally |
| `c-abi/` builds a shim exporting `fl_binding_abi_version()` | 🟢 Windows: `conan create` green, 3 ctest rows pass (`VersionMatchesHeader`, `VersionIsPackedMajorMinor`, `CompilesAsC99AndIsSelfContained`), `test_package` links a **C** program against the package |
| `dotnet/` builds three empty packages | 🟢 `Eiva.Fletcher{,.Interop,.GatewayClient}` 0.5.0-alpha pack on `net8.0;net10.0`; `dotnet format --verify-no-changes` clean |
| `ci.c-abi.yml` + `ci.dotnet.yml` green on **both** platforms | 🟢 **green on both platforms at `7820e82`, 51/51 on draft PR #129** (2026-09-16). The item stays 🔴 in the tracker only because 🟢 there means green **and reviewed**, and no item of this round has been reviewed yet. How it got there: **the first Linux run earned the item its keep immediately** (seam §12.4, risk P-6): `c-abi / build-linux` failed at the link with `relocation R_X86_64_TPOFF32 against '__tls_guard' can not be used when making a shared object`, because no `fletcher-*` recipe declared an `fPIC` option — nothing in the tree had ever linked those archives into a shared object before. Fixed by adding the option to `pubsub`, `fastdds-pubsub-provider` and `xrcedds-pubsub-provider` (not header-only `core`); `ci.format-check-cpp` also failed on two unformatted spots and now covers `*.c` too. Windows was green throughout, which is precisely the asymmetry the lane exists to close |
| Part 4 matrix re-derived and committed | 🟢 all bucket totals unchanged; `CallerTier` corrected 20 → 21 |
| `Apache.Arrow` pinned and its C Data Interface verified for every mapping type, nested included (N-9) | 🟢 **23.0.0**, pinned exactly (`[23.0.0]`). 17 types round-trip — 9 scalars, timestamp with and without timezone, duration, `struct`, `list<int32>`, `list<struct>`, `map<utf8,int32>` — plus schema and field metadata; 18 cases on both TFMs |
| Per-RID shim size measured against the budget | 🟢 `win-x64` **7.61 MiB** with all three built-ins; `linux-x64` from the first lane run. Reported by the lane on every run; no threshold until BIND-9 |
| Landing order with `feature/fastdds_modernization/19645` agreed (P-7) | 🟢 **ruled 2026-09-15 (D-BIND-29)**: BIND does not wait for PR #128. #129 first; BIND-1 declares the schema-watch pair as `kNotSupported`; re-examined at the BIND-1 → BIND-2 boundary; whoever lands second moves `c-abi`'s three pins to `0.5.1-alpha`. **Schema-watch clause superseded 2026-09-25 (D-BIND-52)** |
| ~~Self-hosted publish runner + `NUGET_EIVA_API_KEY` (D-BIND-28)~~ | ⛔ **STRUCK 2026-09-17 (D-BIND-33): a duplicate of BIND-9's own bullet**, which carries it more completely (registered, labelled, documented, used by no other job). It was annotated "due before BIND-9" from the start — acceptance for an item cannot hold something whose deadline is a later item. Nothing moves and nothing relaxes |

**Conformance review 2026-09-17 — CONFORMS**, 9 live bullets, 9 met
(`plans/reviews/BIND-0-conformance.md`). Three findings carried forward, none
blocking: **F1** the Part 4 matrix is stale by two cases (`test_xrce_document` is 11,
the matrix says 9, so bucket 4 is 80 not 78) because #130 rebased underneath the
kickoff — and BIND-3/BIND-4 state their acceptance in terms of those totals, so the
mechanism will bite again at the next rebase; **F2** bullet 3's "exports only
`fl_binding_abi_version()`" was true at BIND-0 by accident of an empty ABI and is true
now only because of `cmake/exports.map`; **F3** the 7.61 MiB shim size is a kickoff
baseline measured against an empty ABI, not a current fact. Not checked: the ADO items,
the Arrow round-trip test's results, `linux-x64`'s stored size.

One finding came out of the kickoff that the plan had not anticipated: the shim's
Windows export table (Part 8, Open items).

---

### BIND-1 — The binding ABI header, reviewed as a specification

**As** a binding author, **I want** a stable, versioned C boundary over Fletcher,
**so that** C# now, and Rust next round, bind one surface.

**Acceptance**

- `c-abi/include/fletcher/abi/binding.h` is **pure C** (C99), self-contained,
  versioned, append-only structs within a major; the pre-1.0 no-compatibility
  exemption and the deprecation policy are stated **in the header**.
- Contents: status (numbers from `core/README.md`'s published table, `kOk` …
  `kReentrantCall`); the caller-owned `fl_error {status, origin, message,
  message_len}` and `fl_error_dispose` (D-BIND-19 rule 1); registry `create` with
  a pointer+length selector and a `{max_payload_bytes, domain_id, document}` config
  (the shim registers its own built-ins; no managed `Register`/`SetPathResolver`,
  D-BIND-24); publisher and subscriber handles; `schema_arrival_wait` with the
  five typed outcomes; blob owner-handle retain/release; attachments as positional
  `size`/`key_at`/`value_at` and a builder; the write window
  `{ctx, data, capacity, pos, grow}` and the writer
  `size_t (*)(void* ctx, uint8_t* dst, size_t room)`; the delivery callback; the
  codec surface of Part 2 (`fl_codec_open/close`, `fl_rows_bind/unbind`,
  `fl_publisher_publish_row/rows`, `fl_encode_row`, `fl_decode_rows`); and the
  **single-copy marker symbol** (D-BIND-17).
- Every type is **derived from the seam spec**, constructed from the C++ value,
  never shared with or defined in terms of PDA-ABI's header (D-BIND-2′).
- Nothing crossing is a `std::shared_future` (D-BIND-22).
- No existing generated output's bytes change; no existing emitter behaviour
  changes.
- Reviewed as a spec, like PDA-ABI-1: the ownership wording is the expensive thing
  to get wrong.

**State — 2026-09-16**

| Acceptance bullet | State |
|---|---|
| `binding.h` is pure C99, self-contained, versioned, append-only; exemption + deprecation policy stated in the header | 🟢 committed `696a42f`; `BindingAbi.CompilesAsC99AndIsSelfContained` green on both platforms (MSVC holds it to C11, gcc to C99 — see `c-abi/tests/CMakeLists.txt`) |
| Contents: status codes, `fl_error` + dispose, registry create, handles, `schema_arrival_wait`, blob retain/release, attachments, write window + writer, delivery callback, the codec surface, the single-copy marker | 🟢 all present in `696a42f` |
| Every type derived from the seam spec, never shared with PDA-ABI's header (D-BIND-2′) | 🟢 |
| Nothing crossing is a `std::shared_future` (D-BIND-22) | 🟢 |
| No generated output's bytes change; no emitter behaviour changes | 🟢 the header touches no emitter |
| **Reviewed as a specification** | 🟢 **done, two cycles, `plans/reviews/BIND-1-design-review.md`.** 3 BLOCKERs opened and all 3 closed (B1 → D-BIND-32; B2 stale status paragraph; B3 export-table claim), none needing a signature change. The three expensive wordings the plan named — `fl_schema`'s never-call-Arrow's-release warning, the write window's disclosed residue, attachments' never-sort rule — were all reviewed and KEPT, with the reasoning recorded so they are not reopened. 4 DEBT + 2 NITs carried forward; by this repo's convention DEBT does not loop the design |
| *(raised by BIND-2c, 2026-09-17)* **Who frees a message a CALLBACK put in an `fl_error`** | 🟢 **RULED 2026-09-17 (D-BIND-32): the callback BORROWS it** — keeps the bytes alive until it returns, the shim copies and frees nothing; stated on `fl_error` so the next such callback inherits it. `binding.h` updated. The question as it stood: `fl_error` says the message *"is heap-allocated by the shim"* and is released with `fl_error_dispose` — but `fl_grow_fn` hands an `fl_error*` to a CALLER's callback, inverting it: allocated by the binding, freed by the shim's `delete[]`. Those are one heap only while both sides share a CRT, and BIND-3 intends the shim to link the MSVC CRT **statically**, which is exactly when they are not. 2c takes the survivable side — copy the message, never free it: that leaks at worst under the shim-allocates reading, where disposing would corrupt a heap under the caller-allocates one. **Recommendation:** say that a `grow` callback which sets `message` keeps those bytes alive until it returns and the shim copies them, matching the borrow discipline `fl_str` already uses everywhere else in the ABI. The alternatives — a caller-supplied disposer, or shim-exported `fl_alloc`/`fl_free` — both widen the surface |

### BIND-2 — The nanoarrow codec, the publish fusion, and the oracle's producer

**As** a .NET developer, **I want** encode/decode over the ABI, **so that** there
is exactly one wire-format implementation and my rows are written straight into
the transport window.

**Acceptance**

- `c-abi/src/nanoarrow_codec.{hpp,cpp}`: `NanoarrowCodec(const ArrowSchema&)`
  (deep copy, field plan), `Bind(const ArrowArray&) -> BoundRows`
  (`ArrowArrayViewSetArray` + schema equality, once per batch),
  `EncodeRow(const BoundRows&, int64_t, WriteBuffer&)` driving `PositionalWriter`,
  `DecodeRows(bytes, len, count, ArrowArray*)` driving `PositionalReader` and
  nanoarrow's builders. One `switch` per mapping row, recursive for list, struct
  and map. Links `fletcher-core`, `fletcher-pubsub` (nanoarrow) and the three
  providers; **never** `arrow-bridge`.
- The publish fusion: `fl_publisher_publish_row(s)` calls `Publisher::Publish`
  with a `RowEncoder` lambda that runs `EncodeRow` into the provider's window.
- `fl_encode_row` through a `fl_write_window` → `WriteBuffer` adapter; the C writer
  adapter for `PublishRaw` closes over `ctx` and throws `PubSubError(kInternal, …)`
  when the managed thunk reports a captured exception (D-BIND-19 rule 3).
- **Byte identity**: `NanoarrowCodec.ByteIdenticalToArrowBridge` across the
  codec's own **Arrow fixture corpus** — five fixtures of three rows each,
  covering every arm of both switches, with the comparison counts asserted
  deliberately (15 for identity, 5 for the round trip) so the corpus cannot
  shrink silently. **Amended 2026-09-18 (D-BIND-35)**: this bullet named the
  `emit_vectors` scenario corpus, which is proto-row shaped (it encodes through
  the generated row class) and is three scenarios over one flat message — the
  wrong shape for an Arrow-array codec, and narrower than what was built.
  D-BIND-11's `emit_vectors` reference stands where it was written, on the
  cross-language generated-artifact property. The oracle is `arrow-bridge`'s
  `Codec` at the version this component pins; the versioning rule moves that pin
  in lockstep with any wire-format change.
- **The borrow rule is tested**: an array's `release` count stays zero across N
  publishes and is one after `fl_rows_unbind` plus the caller's release.
- Malformed-input parity with HARD-1..7 through `fl_decode_rows`; bounds checks not
  `#if DEBUG`-gated; every message preserved.
- `CopyAccounting.BindingProducerWritesInPlace` added to
  `integration-tests/pubsub-conformance`: a producer through `fl_codec_open`,
  `fl_rows_bind` and **`fl_encode_row`** — writing into a span of the probe's own
  window — scores `encode_copies == 0`, retiring the README's "stand-in" caveat
  for the client half. **Amended 2026-09-17 (D-BIND-34)**: it named
  `fl_publisher_publish_row`, which cannot reach the instrumented probe because
  D-BIND-24 closed the registration door that would put the probe in the shim's
  registry, and which owns the encoder frame the ledger samples from.
- The **single-copy check** (D-BIND-17): the shim enumerates loaded modules for a
  second marker export and refuses to initialise, naming both.
- A packed-size budget for the shim in CI (D-BIND-1a rider ii).
- Prior art read first: commit `0050365` on `feature/fastdds_modernization/19645`
  (`EncodeRow(values, WriteBuffer&)`, `batch_decoder.hpp`), a design reference over
  Arrow C++, not something to link.

**The item is built in four slices**, because it is the round's long pole and a
single commit of it would be unreviewable. The slice boundaries are where a
property becomes provable, not where the code happens to divide:

| Slice | Content | Commit | State |
|---|---|---|---|
| **2a** | `NanoarrowCodec(schema)`, `BoundRows`, `EncodeRow` — the encode half | `28cdcd4` (+ `7820e82`, which builds `arrow-bridge` in the c-abi lane because it is the oracle) | 🟢 green, 🔍 reviewed 2026-09-18 |
| **2b** | `DecodeRows(bytes, len, count, ArrowArray*)` — the decode half, and the malformed-input parity property | `bec139e` | 🟢 green, 🔍 reviewed 2026-09-18 |
| **2c** | `fl_error` + the one `Translate` containment site; the codec surface; the write-window and writer adapters; and the publisher chain — provider/publisher create + destroy, `create_topic`, `list_topics`, `publish_raw`, `publish_row(s)`, `fl_string_list_*` (**scope ruled 2026-09-17, D-BIND-31**) | `3b13cb5` (+ `0811091`, the ELF export table) | 🟢 green, 🔍 reviewed 2026-09-18 |
| **2d** | The copy-oracle producer, the single-copy check, the packed-size budget | `b95affc` (+ `b6b89b3`, `f9d3ece`, `5762239`) | 🟢 green, 🔍 reviewed 2026-09-18 |

**State — 2026-09-18.** Reviewed at `5762239`: `plans/reviews/BIND-2-codereview.md` and
`plans/reviews/BIND-2-conformance.md`. **CONFORMS on 9 of 10 bullets** (bullet 4 amended by
D-BIND-35), and **one BLOCKER is open** — B1, a cross-module `new[]`/`delete[]` in
`test_containment.cpp` that is invisible today. *(It was expected to fire when BIND-3
linked the MSVC CRT statically; D-BIND-38 keeps the CRT dynamic, so the hazard is
deferred rather than imminent — the fix landed anyway, because pairing `new[]` in one
module with `delete[]` in another is wrong under a shared runtime too.)*

| Acceptance bullet | State |
|---|---|
| `nanoarrow_codec.{hpp,cpp}`: deep copy + field plan, `Bind`, `EncodeRow`, `DecodeRows`; one `switch` per mapping row, recursive; links core/pubsub/providers and **never** `arrow-bridge` | 🟢 2a + 2b. The field plan was materialised in 2b as a `FieldPlan` tree (`PlanNode`): encode reads the type tree off the `ArrowArrayView`, decode has no array to read it from |
| The publish fusion: `fl_publisher_publish_row(s)` with a `RowEncoder` lambda | ⚪ 2c |
| `fl_encode_row` through a `fl_write_window` → `WriteBuffer` adapter; the C writer adapter for `PublishRaw` | ⚪ 2c |
| **Byte identity** against `arrow-bridge` | 🟢 2a, **but over a different corpus than this bullet names** — see the deviations below |
| The borrow rule is tested | 🟢 2a — `BindBorrowsTheArrayAndNeverConsumesIt` |
| Malformed-input parity with HARD-1..7 **through `fl_decode_rows`**; bounds checks not `#if DEBUG`-gated; every message preserved | 🟢 2b + 2c. 2b discharged it **at the codec level**: `DecodeRefusalsComeFromTheReader` walks a four-byte `0xFF` window across a valid encoding and requires every refusal to carry the reader's `"PositionalReader:"` prefix, so the HARD-1..7 hardening covers the binding by construction rather than by a second taxonomy. **2c landed `fl_decode_rows` as the pass-through that row predicted, and the property carries unchanged**: `MalformedBytesCarryTheReadersMessageAndTheCodecOrigin` exercises it through the export table — the reader's message forwarded verbatim, `FL_ORIGIN_CODEC`, and `out` untouched on failure — with an untruncated control. The no-`#if DEBUG` clause is mechanical: every bound in `positional_io.hpp` is an unconditional `if (…) throw` |
| `CopyAccounting.BindingProducerWritesInPlace` in `pubsub-conformance` | 🟢 2d, measuring `fl_encode_row` into the probe's own window (D-BIND-34), with `BindingProducerStagingIsCaught` as its live negative control — the first draft of the leg recorded the LENT SPAN and so scored zero by construction; the control is what catches that |
| The single-copy check (D-BIND-17) | 🟢 2d. `fl_single_copy_marker` + a load-time scan; a poisoned shim refuses through the one containment site rather than aborting at load (a shared library that fails to initialise takes the host down with no diagnostic a managed runtime can surface). Tested with a real decoy module on Windows and verified in a `gcc:13` container for Linux, which found two false-positive classes Windows structurally cannot express: `dlopen(nullptr)` is the GLOBAL SCOPE not the executable (a healthy process scored 2), and `dlsym` on a library handle searches its dependency chain (now deduplicated by symbol address) |
| A packed-size budget for the shim in CI | 🟢 2d — **a ceiling per platform, each set from that platform's own measured baseline**: win-x64 12 MiB over 7.61 MiB measured, linux-x64 20 MiB over 13.79 MiB measured, each leg printing baseline + headroom so growth shows as a number before it shows as a red build. 2d first set ONE ceiling for both from the Windows figure and the Linux leg rejected it on the next run — that figure had been in the job log since BIND-0 and was simply not read (`5762239`). This is D-BIND-1a rider ii's check (the shim links nanoarrow, never Arrow C++), NOT BIND-9's release budget: the failure mode it catches is tens of megabytes, so each ceiling sits far above ordinary growth and trips only on the mistake it is named for. BIND-9 still sets the release number |
| Prior art read first | 🟢 |

**Both deviations from the acceptance above are RULED, 2026-09-18, at BIND-2's
conformance review:**

1. **The byte-identity corpus is not the one the bullet names** — ✅ **ruled
   (D-BIND-35): the acceptance is amended, the test stands.** 2a built **four
   hand-written Arrow fixtures of three rows each** — the mapping's scalars at
   their extremes, timestamp and duration, a nested struct in its three distinct
   states, and the three composites each with a populated / empty / null row — to
   which 2b added a fifth, a fixed-size list, because that arm of both switches
   was otherwise untested; 15 comparisons for byte identity, 5 fixtures for the
   round trip, both counts asserted. The review read `emit_vectors.cpp` and found
   the bullet's corpus to be the **wrong shape** (it encodes through the
   protoc-generated row class, so it is proto rows rather than Arrow arrays) and
   **narrower** (three scenarios over one flat message: int32, double, string,
   bool, repeated int32 — no struct, map, fixed-size list, timestamp, duration or
   null composite). Substituting it would have reduced coverage on every axis.
   D-BIND-11's own `emit_vectors` reference is untouched: it sits on the
   cross-language generated-artifact property, which is what that corpus is for.
   The chain worth building — those three scenarios as a sixth fixture, joining
   nanoarrow ≡ `arrow-bridge` ≡ generated C++ ≡ TS over the same bytes — needs the
   generated row class in the c-abi test binary and belongs to **BIND-5/7**.
2. **2b narrowed what the codec accepts** — ✅ **ruled (D-BIND-36): map keys are
   scalar only.** A map's key must be a scalar, refused at open by field name. The
   wire is `[COUNT][keys][value bitfield][values]`, and nanoarrow finishes a
   struct element only when every child is exactly one longer, so an entry's key
   and value must be appended together and the keys have to be staged; a composite
   key cannot be staged without a second decoder. The proto mapping produces none
   (proto restricts map keys to integral, bool and string). Locked as a decision
   rather than a note because it is a statement about the **wire format's reach**
   and BIND-Rust binds the same codec next round. No code change: the refusal and
   `RefusesAMapWithACompositeKeyNamingTheField` already exist.

### BIND-3 — `Eiva.Fletcher.Interop` and the codec/Arrow tier

**As** a .NET developer, **I want** the managed codec and Arrow tier, **so that**
rows go to and from `RecordBatch`es with typed errors and no wire bytes in my code.

**Acceptance**

- `Eiva.Fletcher.Interop`: P/Invoke declarations; a `SafeHandle` per native
  handle; `NativeLibrary.SetDllImportResolver` with a diagnostic that names the
  missing RID; the ABI version handshake at load. ~~the glibc floor stated for
  `linux-x64` (N-7)~~ — **MOVED TO BIND-9 (D-BIND-41, 2026-09-21)**: a floor is a
  statement about a PACKAGE, and nothing is packaged until BIND-9. What BIND-3
  leaves behind is better than the sentence it removes — `ci.c-abi.yml` now
  MEASURES the floor on every Linux build and prints it, so BIND-9 states a number
  rather than guessing one from the build image. **The MSVC CRT stays DYNAMIC and the profile is untouched
  (D-BIND-38, 2026-09-21)**; whether the shipped shim should link it statically is
  a packaging question and moved to BIND-9.
- **Error handling per D-BIND-19** (development plan §3.5): one `ThrowIfFailed`
  site; `FletcherException {Status, Origin, Message}` with
  `FletcherFormatException` when `Origin` is the codec **and** `Status` is
  `InvalidArgument` (narrowed to that pair by D-BIND-54, 2026-09-25 — a
  codec-origin `PayloadTooLarge` or `Internal` is a plain
  `FletcherException`); a captured managed
  exception rethrown as itself; a reflection test that every
  `[UnmanagedCallersOnly]` method carries the containment wrapper (N-1).
- `FletcherCodec(Schema)` → `fl_codec_open`; `Bind(RecordBatch) → BoundRows`
  (`Dispose` unbinds **then** releases the export); `Encode(BoundRows, int,
  IBufferWriter<byte>)`; `Decode`, `DecodeBatch`. No method returns encoded bytes.
- C Data Interface export/import via `Apache.Arrow.C`; a received schema is
  **deep-copied natively** before import because the importer consumes (N-5);
  `SchemaHandle.ToArrowSchema()` is the one safe conversion.
- Arrow IPC schema parse of the gateway's `schemaIpc` payload via
  `Apache.Arrow.Ipc`, with custom metadata preserved (`proto_package`,
  `proto_message`, `field_number`); the **IPC parity test** — `.ipc` bytes emitted
  by `--fletcher_opt=ipc` parse to a schema whose fields, types, nullability and
  metadata key order match.
  **MET 2026-09-21 (BIND-3d), over all ten checked-in goldens:**
  `SchemaIpcTests.EveryGeneratedDescriptorCarriesTheSpecsMetadata` asserts the
  spec's own keys — `proto_package` and `proto_message` on the schema,
  `field_number` on EVERY field — and
  `.ASchemaSurvivesAnIpcRoundTripFieldForField` compares fields, types,
  nullability and metadata KEY ORDER against the schema re-serialised from what
  was parsed, so it needs no hand-written expectation to drift from. Key order is
  checked separately because Arrow metadata is an ordered list rather than a map:
  a crossing that rebuilt it from a dictionary would reorder it without losing
  anything, which reads as equal under any comparison that sorts. Carries a
  vacuity guard — comparing two key orders is trivially satisfied when both are
  empty.
- Dictionary handling per spec §"Dictionary Types": decode a dictionary field as
  its value type; reject nested value types with a clear error; nothing beyond
  that ahead of DICT (D-BIND-8).
  **CONFIRMED AND MADE REACHABLE 2026-09-21 (D-BIND-39).** This bullet was right
  and the implementation was not: the shim's codec refused every dictionary at
  open, so a C# caller could not send what a C++ caller could over the same
  format. D-BIND-8's deferral rested on the DICT halt, which is a `nanoarrow_ipc`
  limitation the binding path never reaches — the schema arrives over the C Data
  Interface and the shim references no nanoarrow IPC at all. **The bullet stands
  as written and BIND-3d implements it**: plan a dictionary field as its value
  type, refuse a non-scalar value type naming the field, resolve index→value at
  encode. Re-folding decoded values into a `DictionaryArray` stays DICT's/BIND-5's.
  Consequence: a decoded dictionary field arrives as its **value type**, so a
  codec's bind schema and its decode schema are different objects and the managed
  tier exposes both.
- The UTF-16↔UTF-8 boundary stated in the XML docs and the oracle's scope note
  (D-BIND-1b). **XML-DOC HALF MET 2026-09-21 (BIND-3d)**: stated on
  `FletcherCodec` — every wire length is a count of UTF-8 BYTES rather than of
  characters or UTF-16 code units, and the transcode happens in `Apache.Arrow` on
  the way into an array rather than at this boundary, so the copy accounting does
  not count it. **The oracle's scope note is BIND-5's**, where the oracle first
  runs with the C# producer. **SCOPE-NOTE HALF MET 2026-09-29, after BIND-5's
  close**: stated in the header of `dotnet/tests/Fletcher.CopyOracle.Tests/CopyOracleTests.cs`
  and in pubsub-conformance's README, "What green does NOT prove". BIND-5's
  close-out had missed it. **The bullet is now met in full.**
- ~~**Bucket 1 green** as binding conformance tests — the FILE SET named in Part 4's
  matrix, not a count.~~ **REPLACED 2026-09-21 (D-BIND-39).** The FILE-SET wording
  fixed the wrong problem: bucket 1 is `arrow-bridge`'s **Arrow-native** suite, so
  porting it asks the binding to implement a tier it does not serve — roughly half
  of it is unions, decimals, intervals, half-float, view types and fixed-size
  binary, none of which any `.proto` can produce, plus `test_envelope` (BIND-8 by
  §2.6) and `WriteBufferInPlace`'s writer cases (BIND-4 by surface). F1's point
  survives intact and is why this is still not stated as a number.
  **The obligation is now: every type the PROTO MAPPING produces round-trips
  through the binding** — the mapping table in `docs/wire-format-specification.md`
  is the file set, and it is the same suite C#, Rust and TypeScript each run, which
  is what makes the cross-language parity claim testable rather than asserted.
  `integration-tests/binding-abi-conformance` round-trips every corpus scenario
  with the values C++ produces.

### BIND-4 — Pub/sub in `Eiva.Fletcher`

**As** a C# application, **I want** to select a transport by name, publish and
subscribe, **so that** I am a full Fletcher client with no protocol SDK on my build.

**Acceptance**

- `ProviderSelector.Parse` (same name-or-path rule as native, re-checked natively),
  `ProviderConfig {uint MaxPayloadBytes; uint DomainId; ReadOnlyMemory<byte>
  Document}` (bytes, length authoritative, C# parses nothing),
  `ProviderRegistry.Create` → opaque `PubSubProviderHandle` (D-BIND-24);
  `PayloadBound.IsValid/Min/Max`. **No name query** on the registry.
- `Publisher`: `CreateTopic(TopicPath, Schema)`, `Publish(TopicPath, BoundRows,
  int)`, `Publish(TopicPath, BoundRows)`, `Publish(TopicPath, RowWriter, int
  minBytes)` (the `minBytes` since D-BIND-45 — this bullet still read
  `Publish(TopicPath, RowWriter)` until the BIND-4 review), `PublishRaw`,
  `ListTopics`, `IDisposable`.
  **The attachments argument carries a managed design decision (D-BIND-44,
  2026-09-22): `AttachmentsBuilder` owns its entries and CACHES the sealed set,
  because `fl_attachments_builder_build` empties the native builder** — a publish
  that sealed the caller's builder per row would attach them to row 0 and silently
  to nothing after. No ABI change; unlike D-BIND-42/43 the native side is correct.
- `Subscriber`: `Subscribe(TopicPath, RowHandler) → SubscribeResult {Subscription,
  SchemaArrival}`, `Unsubscribe(Subscription)`, `AbsorbedCallbackFailures` (the
  managed counter), `DispatchAfterDelivery(Func<Task>)`, `IDisposable`, **no
  finaliser** (S-1). `Subscription` is a handle, not a `ulong`.
- **The native half of this item** (D-BIND-31): the shim's subscriber entry points
  (`fl_subscriber_create/destroy/subscribe/unsubscribe`, the schema-watch pair as
  `kNotSupported` per D-BIND-29), the attachments builder and accessors, blob
  retain/release, and `fl_schema_arrival_*`. BIND-2c built the codec surface and
  the publisher chain; everything else on the ABI is this item's, and is named
  here so the gap the ruling closed does not reopen.
  **Plus one entry point the header did not have (D-BIND-42, 2026-09-21):
  `fl_blob_create`.** Every other blob call assumes you already hold a blob, so a
  binding could only attach one it had RECEIVED from a delivery — leaving "a
  builder on publish", three bullets down, unreachable. Found by writing the test
  rather than by reading the header. The ABI minor goes 1 → 2 as the header's own
  rule requires, and the managed handshake matches EXACTLY while major is 0, so
  `NativeLoader.HeaderVersionMinor` moves in the same commit.
  **And a second one the header did not have (D-BIND-43, 2026-09-21):
  `fl_schema_retain`.** `fl_schema_release` was declared with no counterpart, so the
  delivery contract's own sentence — a handler that keeps the schema retains it —
  named a call that did not exist. Found the same way, by writing the thunk that
  has to call it. Minor 2 → 3, with the same coupled managed handshake; the
  handshake test's cases, derived as offsets by D-BIND-42, moved on their own.
  **The native half of BIND-4 is COMPLETE: 46 declared entry points at ABI 0.6 (D-BIND-57's
  per-topic-options pair; 44 at 0.5), 46 defined,
  checked mechanically in both directions** (43 at ABI 0.3; D-BIND-46's
  `fl_schema_copy` made it 44 at 0.4, and this count was stale until the BIND-4
  review). **The schema-watch pair is implemented since D-BIND-52 (2026-09-25,
  ABI 0.5):** the seam grew it (#128), so the `kNotSupported` stub the clause
  above names now forwards to it; **exposed in C# by D-BIND-57** as
  `Subscriber.SubscribeSchema` / `UnsubscribeSchema`.
- **Thunk discipline per D-BIND-18**: per-subscription in-flight counter and
  `retired` flag, the last one out frees the `GCHandle`; thread-static marker;
  managed refusal of `Dispose` from a handler; managed refusal of a synchronous
  `Subscribe` to a new topic from a handler, pointing at `DispatchAfterDelivery`;
  every managed exception caught, counted and raised as `HandlerFaulted`
  (D-BIND-19 rule 5). The delegate signature takes `ReadOnlySpan<byte>` and ref
  structs so an `async` lambda cannot compile (N-3).
  **Amended 2026-09-25 after the BIND-4 review (B1, B2):** a carve-out cancel
  frees off-thread after a second cancel has waited for the drain (D-BIND-50),
  and the refusals are keyed on the PROVIDER over a per-thread stack of delivery
  frames (D-BIND-51). Each with the test the review found missing.
- **Mapping details per D-BIND-20**: `Timeout.Infinite`/`InfiniteTimeSpan` →
  `INT64_MAX`, other negatives `ArgumentOutOfRangeException`; `TopicPath` validated
  in UTF-8 bytes against the six rules and the 246-byte cap; `AttachmentsView`
  positional, no sorting; `SchemaWaitResult` with `Ok` + null as the schema-less
  mode, releasing nothing.
- Provider contracts preserved, not re-implemented: one callback per topic,
  schema-before-data with the schema-less exception, per-writer order, one callback
  at a time with no thread affinity, all three arguments borrowed for the call.
- Bounded-payload overflow surfaces as `FletcherException(PayloadTooLarge)`.
  **Amended 2026-09-25 (D-BIND-53):** a `PayloadTooLarge` the seam or the shim
  REPORTS surfaces as `FletcherException(PayloadTooLarge)` — proven from C# on a
  real native status. No transport a managed caller can reach reports one today:
  `inprocess` ignores bounds, and Fast DDS drops and logs an oversized row
  (deliberate, test-pinned, `main`'s provider). That half is flagged to the
  provider's owner. **Amended the same day:** the provider fix is made on this
  branch (`9255c19`; Fast DDS reports the overflow as `kPayloadTooLarge`), so
  the bullet holds in full over a real transport too
  (`CrossTransportTests.ABoundedPayloadOverflowSurfacesAsPayloadTooLarge`).
  **And as exactly `FletcherException` (D-BIND-54, the same day):** both
  tests now pin the type, not just the status — the overflow used to arrive as
  the `FletcherFormatException` subclass because the encoder hit the bound
  under a codec origin.
- Fast DDS **and** XRCE reachable by selector with **no per-transport C# code**.
- **Bucket 3 over `inprocess`; Bucket 4 over `fastdds` and `xrce`** — again the FILE
  SETS from Part 4 rather than counts, for the reason BIND-3's bullet gives. Bucket
  4's own total moved 102 → 104 between the matrix being derived and BIND-2 closing,
  which is the mechanism, observed - and it struck again at #128 (D-BIND-57): bucket 3's
  `pubsub` file set went 23 → 49 after it was ported, 26 of them per-topic options and
  the schema watch. They are mapped - 12 over `inprocess`, 11 over Fast DDS in the
  transport lane - and 3 more are excluded, so 44 of 49 are mapped and 5 excluded
  (**met at `bdd4fba`**, `ci.pr` run 36153470901);
  the **C# arm of `CallerTier`** with a total mapping onto the C++ cases; at least
  one case added to the C++ suite (seam §12.1).
  **Reopened 2026-09-25 (D-BIND-56):** bucket 4 had never run over `xrce` - its
  row asserted a typed refusal with no Agent, and the close-out re-grade missed
  it. The transport lane now builds a MicroXRCEAgent from the shared recipe
  (`integration-tests/cmake/MicroXrceAgent.cmake`), the suite proves it owns it,
  and `xrce` joins every theory, plus one case across the Agent's bridge to Fast
  DDS. **Met 2026-09-25 at `78ac363`** (`ci.pr` run 36144203527): the transport lane 21/21 on
  Linux and Windows, the Agent built cold from the shared recipe on both.
- The two mapping traps tested: a key above U+E000 and a supplementary-plane key
  where UTF-16 and UTF-8 orders differ; `Wait(Timeout.InfiniteTimeSpan)` waits.
- **Per-row publish benchmark** against the C++ generated publisher over
  `inprocess`, allocations per row in the table (B-2). If the per-row cost is
  unacceptable, the only faster route is a D-BIND-1 STOP-AND-ASK, raised here.
  **Moved here from BIND-3 on 2026-09-21 (D-BIND-37)**: a *publish* benchmark needs
  a managed publish path, and the managed `Publisher` is this item's. B-2 is about
  the per-publish `ToSegments` cost, so it cannot be measured before the publisher
  exists. BIND-3 therefore carries no performance evidence, which is the accepted
  cost of keeping the item boundary where the tracker draws it.
  **Shape ruled 2026-09-24 (D-BIND-48):** a C++ harness in `c-abi/benchmarks/` and
  BenchmarkDotNet in `dotnet/benchmarks/`, both run by hand, outside CI; timings
  on Windows, allocation counts measured on Linux because the Windows count cannot
  see into the DLL. Ten arms: the five ruled (C1 generated C++, C2 the shim from
  C++, M1 fused per row, M2 one-row batch per publish, M3 batch per row) and five
  added while implementing (C3, C4, M2a, M2b, M4 — D-BIND-48 says why).
  **Measured 2026-09-25** (method and full table: `c-abi/benchmarks/README.md`):
  per row, C1 **194 ns**, M1 **378 ns (1.95×, 0 B managed)**, M3 **239 ns
  (1.23×)**, M2 **3.8 µs (19.6×, 3.6 KB managed, Gen2 collections)**, M4 377 ns.
  M2 is two-thirds Apache.Arrow (build 1.2 µs, C Data Interface export 1.3–1.5 µs)
  and one-third Fletcher (1.1–1.3 µs, ~0.74 µs of it native). Native allocations:
  M1 = C2 = 7, M3 = C4 = 5, C1 = 4. **ACCEPTED 2026-09-25 (D-BIND-49): no D-BIND-1
  STOP-AND-ASK, B-2 closes; BIND-6 carries a batch-first design input.**
- **After close — the public surface reconciled (D-BIND-55, 2026-09-25).** The
  conformance review's surface-versus-code list, ruled: six rows of the note amended
  to the code; `SchemaWaitResult.IsSchemaless`, a finaliser on an OWNED
  `SchemaHandle` and `Diagnostics.AbsorbedTotal` built; `SchemaArrival.WaitAsync`
  built above `Wait`; `BlobHandle` deferred until a consumer's attachments are large
  enough to cost a copy. Proven by `SchemaArrivalTests` and `ProcessWideTests`, each
  falsified by a compiling mutation. No ABI change.

### BIND-5 — `SubscriberArrow`, batch-first, and the oracle from C#

**As** a server developer, **I want** the Arrow convenience layer over the
provider, **so that** I subscribe in `RecordBatch` terms without hand-writing the
codec step.

**Acceptance**

- `SubscriberArrow.SubscribeBatched(TopicPath, RecordBatchHandler, BatchOptions)`
  is the primary shape (D-BIND-25): borrowed rows copied, N rows decoded per
  native call, `RecordBatch` delivered with parallel attachments and a
  `BatchStatus {Reason, RowsDropped}`; `BatchOptions {MaxRows = 8000, Timeout =
  1 min}`; per-row Arrow delivery is `MaxRows = 1`.
- `PublisherArrow` folds into `Publisher` (already Arrow-typed via `BoundRows`).
- Dictionary re-folding deferred to DICT (D-BIND-8).
- **The `test_pubsub_arrow` file set** ported over `inprocess` - 33 cases at D-BIND-57,
  which re-derived it from 15 after #128; the options and schema-watch cases among them
  use what D-BIND-57 added.
  **Surface ruled 2026-09-28 (D-BIND-59):** `Subscribe` and `SubscribeBatched` take the one
  `RecordBatchHandler` (per-row is `MaxRows = 1`; `RowBatchHandler` dropped); attachments are owned
  `AttachmentsBuilder` copies aligned with the rows; the handler owns a `RecordBatch?`;
  `TopicOptions` on both forms and the schema watch are forwarded. **The port found a defect
  (D-BIND-60):** the shim published rows bound under ANOTHER schema unchecked; both publish entry
  points now refuse them FL_INVALID_ARGUMENT - the lock-across-throw it surfaced is an OPEN finding,
  cause not established. **5a, CI-confirmed at `0da07c7`** (`ci.pr` run 36405073609, 50/50 on its first attempt): 32 of 33 mapped (27 in
  `dotnet/tests/Fletcher.Tests/SubscriberArrowTests.cs`, whose header holds the mapping, the rest
  answered by existing publisher/options rows and one Fast DDS case in the transport lane),
  `FinishFailureIsReportedNotFatal` excluded by D-BIND-8; managed **313/313** on all four
  legs, transport **33/33** and c-abi **78/78** on both platforms (the 8 `xrce` cases, which need a
  MicroXRCEAgent, ran only in CI), binding-abi **11/11**, pubsub-conformance **146/146**; locally, six
  managed mutations and the shim check's removal each caught.
- **The copy oracle run with the C# producer** is this item's acceptance: zero-copy
  for rows and attachments, from managed code, falsifiable.
  **Amended 2026-09-28 (D-BIND-58)**, because it could not be met as written:
  * **Rows:** a TEST-ONLY shim variant registers `SeamProbeProvider` (never shipped; its
    `fl_test_*` exports live in a test header, not `binding.h`), and C#'s real
    `Publisher.Publish` into it scores `encode_copies == 0`; a deliberately staging managed
    publish scores exactly 1 as the negative control. The probe moves into test support
    shared with `integration-tests/pubsub-conformance`.
  * **Attachments:** the DELIVERY view - a handler's `AttachmentsView` is the transport's own
    bytes, by address, `attachment_copies == 0`. Publishing attachments from C# copies
    (D-BIND-44, accepted); a zero-copy managed publish (`fl_blob_wrap` over pinned memory, at
    the next ABI minor; ABI 0.7 went to D-BIND-68) is deferred until a consumer's attachments are
    large enough to cost a copy.
  * `SubscriberArrow` copies borrowed rows by design, so no zero-copy claim covers it.
  * **How the fused publish is scored (D-BIND-61):** by the SOURCE of the window's payload bytes,
    seen through `WriteBuffer::Append` on an always-full probe window; `encode_copies == 0` iff the
    payload arrived from the C# batch's own buffer. Controls, each exactly 1: a staging publish
    (`Encode` + `PublishRaw`) and a copied export.
  * **Where it lives (D-BIND-62):** a test-only package, `fletcher-copy-probe`, shared with
    `pubsub-conformance`; c-abi builds `fletcher-c-abi-probe` only under `with_probe_shim`.
  * **5b, committed at `38d6d78`:** `dotnet/tests/Fletcher.CopyOracle.Tests` (5 cases, in
    `Fletcher.slnx`) runs C#'s real `Publisher.Publish` into the probe: the fused publish scores
    `encode_copies == 0` and `row_copies == 0`, the staging and copied-export controls score 1, and
    a handler's `AttachmentsView` carries the loaned bytes by address (`attachment_copies == 0`, with
    its own control). The same claims at the C level are in `c_abi_probe_tests` (7). Local results:
    copy-probe **13/13**, c-abi **86/86** (78 + `TheShippedShimHasNoProbe` + 7), conformance
    CopyAccounting/SeamVocabulary **28/28**, managed **313/313** plus oracle **5/5** on net8 and
    net10. Mutations caught: `Publisher.Publish` staging the row, `Bind` exporting a copy, and five
    mutations of the instrument's scoring and fault rules.
    **CI-confirmed at `38d6d78`** (`ci.pr` run 36443379137, 50/50 on its first attempt): copy-probe **13/13** and c-abi **86/86** on both platforms, managed **313/313** and the oracle **5/5** on all four legs, pubsub-conformance **146/146**, transport **33/33** and binding-abi **11/11** on both platforms.
    The Linux probe shim, its version script and the staged `libfletcher-c-abi.so` name all held on their first run.
    D-BIND-63 followed at `255ad43` and moved managed to 316, CI-confirmed there (`ci.pr` run
    36447834274, 50/50 on its first attempt: managed **316/316** and the oracle **5/5** on all four legs).
  * **Found by the C++/C# sequence review during 5b's CI, and fixed (D-BIND-63):** the 5a batcher
    had no byte ceiling. Its window now splits where C++'s does (`Array.MaxLength`, just under C++'s
    2^31-2), and a row alone past the ceiling is a counted drop.
  * **Handler calls are not serialised (D-BIND-64, from the BIND-5 review's Q2):** as in C++,
    handlers may run concurrently and batches may complete out of order, and `Stop` waits for an
    in-flight timer flush. This replaces 5a's lock across the handler, which deadlocked.
  * **The D-BIND-60 check compares wire layout (D-BIND-65, from the review's Q1):** the codecs'
    field plans, so names, metadata and nullability do not refuse, and dictionary schemas are checked
    rather than skipped. The verdict is cached per (rows, topic).
  * **No row is lost uncounted (D-BIND-66, from the review's Q3):** the intake and the flush contain
    their own failures and count them in `RowsDropped`, as C++'s `AddRow` does.
  * **Bound rows need this publisher's declaration (D-BIND-67, from the review's Q4):** the fused
    publishes refuse `TopicNotDeclared` otherwise, as C++ `PublisherArrow` does; the raw paths stay
    unchecked.
  * **Framed decode, ABI 0.7 (D-BIND-68, for review B3 and B4):** `fl_decode_rows_framed` decodes a
    window whose row boundaries are known, skipping and reporting each bad or misframed row, in one
    native call. Only good rows count toward `MaxRows`. Three cases were added; managed is
    now **316/316**, and two mutations are each caught.
  * **After close, 2026-09-29: the oracle's scope note.** BIND-3 handed BIND-5 the statement
    that the UTF-16↔UTF-8 transcode (D-BIND-1b) happens in `Apache.Arrow`, outside what the
    oracle scores. The close-out missed it. It is now in the header of `CopyOracleTests.cs` and
    in pubsub-conformance's README. Documentation only; no result changes.

### BIND-6 — C# row emitter

**As** a .NET developer, **I want** typed row classes generated from my `.proto`,
**so that** I never hand-write the mapping to Arrow.

**Acceptance**

- `<stem>.fletcher.cs` per Part 3: a `sealed` POCO with nullable annotations, a
  real `enum` per proto enum (D-BIND-8), `static Schema Schema` built in code with
  metadata preserved, `static RecordBatch ToArrow(IEnumerable<T>)`,
  `static T FromArrow(StructArray, int)`. **No `WriteTo`/`ReadFrom`**: generated C#
  writes no wire bytes (D-BIND-1).
- **Names per D-BIND-69:** namespace `Fletcher.Gen.<PascalPkg>` whatever
  `csharp_namespace` says; nested messages `Outer_Inner`; PascalCase properties with
  a `_` suffix on a clash with the class name, so a field named after its message or
  after a C# keyword compiles. Test cases for each, including a `.proto` that sets
  `csharp_namespace` and one whose type is generated beside protoc's C# output in one
  project without CS0101. **The second met by 6e-3 (2026-10-08, D-BIND-82):**
  `integration-tests/protoc-dotnet/dotnet/ProtocDotnetBeside`.
- **protoc's naming rules as specified in D-BIND-73**, read from its source: properties by
  `UnderscoresToPascalCase`, suffixed `_` on the class name, protoc's eleven reserved member
  names and the generated class's own members (`Schema`, `ToArrow`, `FromArrow`); the namespace
  by `UnderscoresToCamelCase` keeping dots; enum members by `TryRemovePrefix` and
  `ShoutyToPascalCase`, with duplicates suffixed `_`. A test case per rule.
- **Enum members per D-BIND-70:** prefix stripped, then PascalCase (`COLOR_DARK_BLUE`
  → `DarkBlue`), each with its proto number. Test cases for the edge cases protoc's
  rule answers (a remainder starting with a digit, members colliding after stripping,
  no prefix, `allow_alias`), and a round trip showing the wire bytes match C++'s.
- **The model layer per D-BIND-72:** the row type, `Schema`, `ToArrow`, `FromArrow`
  and the topics compile against `Apache.Arrow` alone. A test project that references
  neither `Eiva.Fletcher` nor `Eiva.Fletcher.Interop` builds the generated model, and
  the native pair is left out with `csharp_model_only`. *(This read "emitted only when
  requested"; D-BIND-76 made the native file the default and the model-only file the opt-out,
  as C++'s `schema_only` is. Corrected 2026-10-14, review T-D5.)* Topic segments follow C++'s form,
  `{"eiva.nav", "Svc", "Method"}` for a dotted package (D-BIND-72 point 6), with a test
  that a C# and a C++ publisher of one method register the same topic.
- **Build integration per D-BIND-71:** `integration-tests/protoc-dotnet` runs protoc
  from an MSBuild `Exec` before compile, with the plugin from
  `$(FletcherProtocPlugin)`, set from `FLETCHER_PROTOC_PLUGIN`. That is the recipe a
  consumer uses once `Eiva.Fletcher.Protoc` sets the property (BIND-9), and the
  project file does not change when it does.
- Per service method, `<Service>_<Method>Publisher` (`Topic`, `TopicKey`,
  `Schema`, `Publish(T)`, `Publish(T, AttachmentsBuilder)`,
  `Publish(IEnumerable<T>)`) and `<Service>_<Method>Subscriber`
  (`Subscribe(<Service>_<Method>Handler) → Subscription`; ~~`Action<T, AttachmentsView>`~~,
  which a net8.0 consumer cannot compile, D-BIND-79), named exactly as the
  C++ pair; **no `SubscribeInPlace`** (Q16). **Ruled 2026-10-07:** `--fletcher_opt=csharp` writes
  the pair to `<stem>.fletcher.native.cs` beside the model, and `csharp_model_only` opts out, as
  C++'s `schema_only` does (D-BIND-76); the topic
  lives in the model layer as `<Service>_<Method>Topic` with `Segments` and `Key` (D-BIND-77).
- **Batch-first, per D-BIND-49's design input.** `Publish(IEnumerable<T>)` is the
  M3 shape (one `ToArrow`, one bind, one crossing — 1.23× generated C++); a
  per-row `Publish(T)` that binds one row is the M2 shape (3.8 µs, 19.6×, Gen2
  collections, two-thirds of it Apache.Arrow's). `Publish(T)` stays on the frozen
  surface; its documentation states that price and points at the batch form, and
  whether it stays per-row or accumulates behind the caller is settled here, with
  `c-abi/benchmarks/README.md` as the evidence. **Settled 2026-10-07 (D-BIND-78): it stays
  per-row**, synchronous as C++'s `Publish(row)` is, with the price in its documentation.
- Every mapped type in wire-format spec §"Proto to Arrow Type Mapping" is
  emitted: scalars, `repeated`, `map`, nested struct, nested list, the well-known
  types; **temporal types lossless** (D-BIND-26); maps as
  `List<KeyValuePair<K,V>>`, order and duplicates kept (G-3, ruled as D-BIND-75: the
  `Dictionary` option was refused, and a settable `List` rather than an
  `IReadOnlyList` matches the other collection properties).
- Nullability annotations match the Arrow schema's nullability, or the capstone
  fails (G-5). **Scoped by D-BIND-86 (2026-10-14):** fields only; children non-null as C++
  stores them; one named exception (the `optional` flatten-wrapper list, G-Q4). Pinned by
  `GeneratedNullabilityTests`.
- Reflection-free: a NativeAOT publish of a consumer app succeeds with **zero**
  trim warnings. **Met by 6e-2 (2026-10-08, D-BIND-81):** `dotnet/tests/Fletcher.AotSmoke`,
  published for net8.0 and net10.0 with warnings as errors and run against the shim in
  `ci.dotnet`'s Linux job.
- New cases in `protoc/tests/test_csharp_type_table.cpp` and
  `test_csharp_visitor.cpp`, mirroring the TS pair; the no-drift test proves every
  pre-existing output is byte-identical with the new tokens absent and present.
- Cross-file references resolve — reuse `CollectCrossFileIncludes` **read-only**
  for namespace qualification. Multi-file invocation emits no duplicate filename;
  a shared helper, if needed, is emitted **exactly once** from `GenerateAll()`.
  **Met by 6e-1 (2026-10-07), with one deviation stated:** `CollectCrossFileIncludes` is
  untouched but not called. It returns C++ header paths, and C# needs no include list:
  `CsTypeRef` names a type from its own descriptor's file (`CsNamespace` of that file).
  No shared helper is needed (`Eiva.Fletcher.Model` holds Timestamp and Duration,
  D-BIND-74), and a two-file protoc call writes two distinct files
  (`ProtocDotnetShared`). **Deviation removed by D-BIND-87 (2026-10-14):** C# now
  reads the walk `CollectCrossFileIncludes` is derived from (`CollectCrossFileMessages`).
- **6a, 2026-10-03:** `--fletcher_opt=csharp` emits `<stem>.fletcher.cs` through
  `csharp_backend_type_table` + `csharp_backend_visitor` on the IR: the namespace, an
  `enum` per proto enum, a `sealed class` per message with a property per SCALAR field
  (types, nullability, D-BIND-69/70/73 names). Composite and temporal fields are marked
  `// Not generated yet (BIND-6c)` rather than dropped. The file opens with an
  `<auto-generated>` block: without it, a consumer building with analysers as errors
  fails on names our rules produce by design (CA1707, CA1819, CA1069), measured.
  `test_csharp_type_table` (18) and `test_csharp_visitor` (7, including the no-drift
  case) are green, 126/126 in the suite; five mutations are each caught by the test
  named for them; generated C# from a 16-field `.proto` compiles with every analyser
  as an error.
- **6b-1, 2026-10-05:** every class gets `static Schema Schema`, rendered by a third
  `SchemaSink`, `CsSchemaSink`, driven by the same `SchemaVisitor` as C++'s two: the
  fields, nullability and metadata (`metadata_from_option` extras included) are C++'s by
  construction, nanoarrow's list/map child defaults reproduced, a nested message
  inlined as the in-process sink copies it. A message whose fields are all mapped
  scalars gets `ToArrow(IEnumerable<T>)` and `FromArrow(StructArray, int)`; any other
  message gets its `Schema` and a marker naming the first field conversion waits for
  (6c), so no `ToArrow` can build a batch its own `Schema` disagrees with. Measured
  outside the suite: the `Schema` of five messages (lists, an inline struct, a map, a
  list of structs, Timestamp, Duration, a wrapper) equals the `.ipc` the plugin writes
  from nanoarrow, field by field and key order included, and the comparison fails on
  a planted difference; three rows with nulls and extremes round-trip; it all builds
  against Apache.Arrow 23.0.0 with every analyser as an error. 133/133 in the suite;
  six mutations caught. **6b-2** (the `protoc-dotnet` project and lane) makes that
  measurement a test.
- **6b-2, 2026-10-05:** `integration-tests/protoc-dotnet` and
  `ci.integration-test.protoc-dotnet`. The project references Apache.Arrow and xunit
  and nothing of Fletcher's; its `FletcherGen` target is D-BIND-71's recipe and runs
  protoc itself. 16 cases: each generated `Schema` against the `.ipc` from the same run
  (with a case proving the comparison can fail), the `ToArrow`/`FromArrow` round trip
  with nulls and extremes, the slice boundary, the namespace and names, enum numbers, and
  no Fletcher reference in the compiled assembly. 16/16 on Linux locally; a plugin built
  with a map key rendered nullable makes exactly the `Player` schema case fail, naming
  the field. **This meets the D-BIND-71 bullet above and D-BIND-72's model-layer
  bullet**; the wire-bytes comparison with C++ waits for the native pair (6d).
  *(Corrected 2026-10-14, review T-D5: it met the model-layer HALF of that bullet. The bullet's
  test that a C# and a C++ publisher register the same topic did not exist until review fix B5,
  2026-10-13, which made `test_topic_path.cpp`'s `ExpectBackendsAgree` generate C# too. The wire
  comparison came with 6d-2's `CoverageWireBytesTests`.)*
- **6c, first step, 2026-10-06 (D-BIND-74):** `Eiva.Fletcher.Model` is scaffolded at
  `dotnet/src/Fletcher.Model` with its tests in `tests/Fletcher.Model.Tests`: the lossless
  `Timestamp` and `Duration` over Arrow's `TimeUnit`, `Apache.Arrow` only. 47 cases per
  TFM, six mutants caught.
- **6c-1, 2026-10-07 (D-BIND-75):** every field whose type is generated in the file gets its
  property: `List<T>` for a repeated field, `List<KeyValuePair<K,V>>` for a map (order and
  duplicates kept, as C++'s pairs do), the generated class (`?` when absent) for a message,
  and the `Eiva.Fletcher.Model` types for Timestamp and Duration, nullable where the schema
  is. A message from another file has no class yet, so its field is a comment naming BIND-6e.
  `ToArrow` / `FromArrow` still wait for 6c-2. 139/139 plugin tests (clang-format-18 clean),
  and `integration-tests/protoc-dotnet` 20/20 on Linux with `Fletcher.Model` referenced and
  every analyser as an error.
- **6c-2, 2026-10-07:** every message whose fields all convert gets `ToArrow`, `FromArrow`
  and `internal ToArrowColumns`, the columns a message embedding it as a struct reuses:
  lists (offsets plus the element array), nested messages (an absent one keeps its slot with
  a cleared validity bit), maps (entries in order, duplicates kept) and Timestamp/Duration.
  Every Arrow type a column is built with is read from the class's own `Schema`. A value is
  written in the column's unit through `Eiva.Fletcher.Model`'s new `WithUnit`, which refuses
  to drop a remainder, so it comes back as the same instant counted in that unit. A field
  without a class, or a message embedding one whose conversion waits, blocks conversion with
  a marker naming the field and the reason. `ToArrowColumns` joins D-BIND-73's reserved
  member list. 144/144 plugin tests; `protoc-dotnet` 24/24 on Linux, including every batch
  through the Arrow IPC stream format, which checks each column's layout where
  `RecordBatch`'s constructor does not; eight mutants of the generated C# each caught by
  those C# tests, none by a build failure; `Fletcher.Model.Tests` 61/61 on both TFMs.
- **6d-1, 2026-10-07 (D-BIND-76 to 79):** `csharp` adds a `<Svc>_<Method>Topic` class per pub/sub
  method to the model file and writes `<stem>.fletcher.native.cs` (unless `csharp_model_only`) with,
  per method, a
  `<Svc>_<Method>Handler` delegate, a `<Svc>_<Method>Publisher` (declares its topic on
  construction; per-row `Publish(T)` with its price documented; `Publish(T, AttachmentsBuilder)`;
  batch `Publish(IEnumerable<T>)`) and a `<Svc>_<Method>Subscriber` (decodes with the class's own
  schema, as C++ does; `Subscribe`, `Unsubscribe`, `HandlerFaulted`). Methods are eligible by
  C++'s own `ValidateServiceMethod`; one whose message cannot convert gets its topic and a
  marker saying why it has no pair. The plugin's output for `protoc/tests/golden/csharp_pair.proto`
  is committed as goldens, held to the plugin byte for byte, and compiled into `Fletcher.Tests`,
  where the pair runs over the native shim: 337/337 on net8.0 and net10.0 (11 new), six
  mutants of the generated pair each caught, none by a build failure. 150/150 plugin tests.
  **Still owed (6d-2):** the C#-versus-C++ wire-bytes comparison.
- **6c-3, 2026-10-07:** lists of lists, which flatten wrappers around a repeated message produce
  (`repeated StructListWrapper` is list<list<struct>>), found by 6d-2's first probe: such a field had
  no property and a marker blaming another file. Now `List<List<T>>` to any depth, built level by
  level from the inner lists' lengths and read back with each list array's own offsets; a field
  without a type says why in the classifier's words. 153/153 plugin tests.
- **6d-2, 2026-10-07:** C# against C++ on the wire, with no C++ build beside the shim: the C++ side
  is `integration-tests/protoc-coverage`'s committed `.v1.bin` goldens, the generated C++ class's
  `Encode()` of the coverage fixture rows. The plugin's C# for the same `coverage.proto` is committed
  there as `golden/coverage.fletcher.cs`, held to the plugin by that lane's `CsGolden.ModelByteIdentical`,
  and compiled into `Fletcher.Tests`, whose `CoverageWireBytesTests` builds each fixture row in C#
  (a transcription of `coverage_fixture.hpp`), encodes it through `ToArrow` and the native codec, and
  compares the bytes with all twelve goldens; then decodes each golden, reads it through `FromArrow`
  and re-encodes it to the same bytes. 25/25 on net8.0 and net10.0, first run; five mutants (a fixture
  value, a map put in sorted order, an absent message written present, depth-3 lists dropped, a
  duration off by one nanosecond) each caught. The sorted-map mutant is D-BIND-75's evidence on the
  wire. `ci.dotnet`'s sparse checkouts and `ci.pr`'s dotnet filter carry the coverage goldens.
- **6e-1, 2026-10-07 (D-BIND-80): types from another file.** A message or enum declared in another
  file is named by `CsTypeRef`: bare when both files share a namespace, else
  `global::Fletcher.Gen.<Pkg>.<Name>`; enums too, which before 6e were named bare and would not have
  compiled from another package. A message from another file converts when its own class does, and
  its struct column is built through that class's public `ToArrow(rows).Arrays` (D-BIND-80), since the
  class may sit in another assembly where `internal ToArrowColumns` is absent; a same-file class still
  lends `ToArrowColumns`, so single-file output is unchanged. `repeated google.protobuf.Timestamp` is
  now a `List` of the class generated from timestamp.proto. After 6e no `.proto` reaches the "not
  generated" marker: the IR drops what it cannot map, so the one remaining reason a method gets no
  pair is a flatten-wrapper input. Tests: the plugin, 156/156 (one name rule, three cross-package
  cases, four cases that pinned the old refusal rewritten); the golden `Marks` now has its pair, and
  the plugin's `google/protobuf/timestamp.fletcher.cs` is a golden beside it, so `Fletcher.Tests` runs
  a cross-file pair over the native shim (362/362 on net8.0 and net10.0); `protoc-dotnet` gains
  `ProtocDotnetShared`, an assembly holding `shared.proto` and timestamp.proto, and `Route`, which uses
  them as a field, a list, a map value, an enum and a list of enums: its `Schema` equals the `.ipc`, it
  round-trips and survives the IPC stream, 28/28 first run. Four mutants: children built in reverse
  (two tests fail, no compile error); D-BIND-80 undone (CS0117 ×5); the flag lost for elements only
  (CS0117 ×3); every name bare (CS0246 ×7).
- **6e-2, 2026-10-08 (D-BIND-81): NativeAOT.** Measured first: a consumer of the generated pair
  published with NativeAOT raised one warning, IL3000, from `NativeLoader`'s `Assembly.Location`
  (empty in an AOT app), and none from the generated code or Apache.Arrow 23; the loader now probes
  `AppContext.BaseDirectory`. *(Review G-D1, 2026-10-14: beside the assembly too, for plugin hosts.)* `Eiva.Fletcher`, `.Interop` and `.Model` declare `IsAotCompatible`, so
  the analysers run in every build: the old line put back fails the Interop build (IL3000 ×4).
  `dotnet/tests/Fletcher.AotSmoke` (in `Fletcher.slnx`) publishes and receives a `Reading` and a
  cross-file `Marks` and exits non-zero unless exactly those rows return; `ci.dotnet`'s Linux job
  publishes it for net8.0 and net10.0 (gcc links) and runs it against the shim. The implicit ILLink and
  ILCompiler packages are pinned in `dotnet/Directory.Build.targets` to SDK 10.0.400's versions, since
  CI's Windows runner (SDK 10.0.401) would otherwise fail the Linux-written lock files with NU1004:
  measured, 10.0.401 restores locked with the pin and fails without it (NU1004 ×8). The whole Linux
  step rehearsed in the devcontainer (CopyOracle 5/5, Fletcher.Tests 362/362 both TFMs, both AOT
  binaries OK, pack); locked restore and build on Windows with SDK 10.0.401 clean. **Open notes for
  the 6e review (not defects, not yet proven):** (1) **Windows NativeAOT is unexercised.** D-BIND-81
  put the publish-and-run step on Linux only; a Windows publish links with MSVC, nothing is known to
  break, but no Windows AOT binary has been built or run. (2) **The smoke app reaches only the
  `inprocess` transport.** Fast DDS and XRCE live inside the shim, so AOT does not touch them, but
  their deliveries enter managed code on foreign transport threads, which has run only under the JIT;
  the AOT analysers cover that code statically, at compile time. **Both parked to BIND-9 by D-BIND-88
  (review Q6, 2026-10-14), as acceptance bullets there.**
- **6e-3, 2026-10-08 (D-BIND-82): beside protoc's own C#.** `ProtocDotnetBeside` runs protoc's
  `--csharp_out` and Fletcher's `csharp` in ONE protoc call over `beside.proto` (sets
  `csharp_namespace`) and `shared.proto` (sets none) into one directory, compiles both into one
  assembly with Google.Protobuf 3.21.12, and tests the namespaces, the file names (distinct even
  without case) and both round trips; 4/4. Two plugin mutants (honour `csharp_namespace`; drop the
  `Fletcher.Gen.` prefix) each fail its build with CS0101. Not `model.proto`: its field `clone` makes
  protoc 3.21.12's OWN C# fail (CS0102), because that protoc's reserved list predates `Clone`, while
  D-BIND-73 was read from protoc's `main`; Fletcher's `Clone_` is right for current protoc. With
  `csharp`, the plugin now refuses a file whose protoc namespace (the option, else the PascalCased
  package) is `Fletcher.Gen` or below, before any output (D-BIND-82). Plugin 158/158 in the generator
  bucket (159 in the suite); two mutants of the guard each caught by its own case. **Still owed by
  6e:** the item review.
- **The item review, 2026-10-08:** `plans/reviews/BIND-6-codereview.md` and
  `BIND-6-conformance.md`. Three fresh-context reviewers; 5 BLOCKERs, all verified; ten questions,
  each ruled (D-BIND-83 to 93 as their fixes land). BIND-6 does not close until B1 to B5 are fixed.
- **Review fix B1, 2026-10-09 (D-BIND-83): a plain message field is never null.** The property is
  `T = new()`; its column has no validity bitmap; `FromArrow` reads it unconditionally; a forced
  `null!` makes `ToArrow` throw, naming the message, field and row. `optional` keeps `T?`. The
  evidence is a new C++-written golden, `coverage.CompositeCoverage.defaulted.v1.bin`, from
  `MakeCompositeDefaulted()` (nothing set): the parity oracle holds C++'s row and Arrow paths to it,
  and `CoverageWireBytesTests` requires C#'s `new CompositeCoverage()` to write it byte for byte and
  read it back. A mutant restoring the old shape fails exactly that write case, with no compile error.
  No existing golden moved. Plugin 160 in the suite (bucket 159, one new case for the `optional`
  path); Fletcher.Tests 365/365 on both TFMs; protoc-coverage 21/21; ProtocDotnet 28/28; Beside 4/4.
  (`982a4f3` re-wrapped one line of that commit that failed `ci.format-check-cpp`: the local format
  step had covered `protoc/` only, and now runs over the whole repo as CI does.)
- **Review fix B2, 2026-10-13: a class in a code expression is named in full.** In a method body a
  bare class name bound first to a member or local of the same name: `Waypoint.FromArrow(..)` in a
  class with a property `Waypoint` of another type (CS0120), `row.FromArrow(..)` beside the local
  `row` (CS0176), `Topic.Schema` inside the pair for a message called Topic (CS1061). Method bodies
  now use `CsTypeExpr`, `global::<namespace>.<Name>`; declarations keep the short name, which a member
  cannot capture in a type position. Found on the way: the compiler's CS8981 ("lower-case type names
  may become reserved") fails a warnings-as-errors consumer for `message row`, `<auto-generated>`
  does not silence it, and a `message class` (a keyword) never compiled at all. A type name made of
  lower-case letters only, which every C# keyword is, is now written `@name` in source: measured, `class
  @row` and `class @class` build on net8.0 and net10.0 with every warning an error, and the compiled
  name keeps no `@`, so it is still the .proto's (D-BIND-69, 73). (A pragma disabling CS8981 was tried
  first and replaced at the maintainer's question: it hid the warning but left a future keyword able to
  break the build.) The evidence is a new committed golden, `protoc/tests/golden/csharp_names.proto` (a
  field named after a message as a list, a map, beside a scalar, optional, and singular as the control;
  messages `row`, `array`, `class`, `Topic` and an enum `level`), held to the plugin byte for byte and
  compiled and run in `Fletcher.Tests` (`GeneratedNamesTests`, including the Topic pair over the shim).
  Plugin 162 in the suite (bucket 161, two new cases); Fletcher.Tests 369/369 on both TFMs;
  protoc-coverage 21/21; ProtocDotnet 28/28; Beside 4/4; C++ format clean over the whole repo.
  Mutants: bare names again fail nine plugin cases, the golden check among them; no `@` fails three.
- **Review fix B3, 2026-10-13 (D-BIND-84): a message named after a generated member is suffixed.**
  `message Schema` is class `Schema_` (likewise `ToArrow`, `FromArrow`, `ToArrowColumns`), so it no
  longer declares a member of its own name (CS0542). A field `schema` in it, `Schema_` by D-BIND-73,
  steps past the class name to `Schema__`. One list, `OwnMemberNames()`, now feeds the property rule
  and the type rule. The names golden gains `Schema` and `FromArrow` as a field, a list element and a
  pair input, compiled and run in `Fletcher.Tests` (the `Schema_` pair publishes and delivers). Plugin
  163 in the suite (bucket 162, one new case); Fletcher.Tests 370/370 on both TFMs; protoc-coverage
  21/21; ProtocDotnet 28/28; Beside 4/4; C++ format clean over the whole repo. Mutants: no type suffix,
  and no step-past for the property, each fail the new case and the golden check.
- **Review fix B4, 2026-10-14 (D-BIND-85): a map whose value is a flatten wrapper is UNSUPPORTED.**
  The shared IR built such a value as a struct naming the wrapper, which no backend generates, so C#
  (CS0117) and C++ (`'W' was not declared`) both emitted code that did not compile. `BuildMapNode`
  now returns UNSUPPORTED with a reason, and `ValidateNoUnsupportedIr` refuses the file for every
  backend, naming the field, as for a map of `Any`. The ruling table had said the field would be
  dropped; it is refused (D-BIND-85 records the correction). `test_ir` gains the shape inside its
  existing case (no new `TEST`, counts unchanged); protoc-coverage gains
  `GenErrors.MapFlattenWrapperFailsBuild` (real protoc and plugin on a new unwired fixture), which
  fails against the plugin built before the fix. No golden moved. Plugin 163/163; protoc-coverage
  22/22; Fletcher.Tests 370/370 on both TFMs; ProtocDotnet 28/28; Beside 4/4; C++ format clean over
  the whole repo.
- **Review ruling Q4, 2026-10-14 (D-BIND-86): G-5 covers fields, by test.** New
  `Fletcher.Tests` `GeneratedNullabilityTests` finds every generated row class compiled into the
  assembly by reflection, pairs each property with its schema field in declaration order (the counts
  must agree), and requires the property to be nullable exactly when the field is; a second case
  requires every list item, map key and map value to be non-null. Measured first: one mismatch only,
  the `optional` flatten-wrapper list (G-Q4); the maintainer ruled it a named exception, which the
  test requires still to be met. No generator change. Fletcher.Tests 372/372 on both TFMs. Mutants:
  a flipped field flag, a nullable list element, and the exception's field made non-nullable each
  fail their case, and each compiles.
- **Review ruling Q5, 2026-10-14 (D-BIND-87): one cross-file walk for both backends.** The walk
  behind `CollectCrossFileIncludes` is now `CollectCrossFileMessages` (the messages from other files
  that a file's fields name); C++ maps each to its header, and `CsVisitor` computes the set once and
  decides D-BIND-80's "from another file" by membership. Wiring it exposed a gap, measured by a new
  case first: the walk did not look inside a field-level flatten, so a message the flattened
  message's own fields name (`Leaf`, a third file) was missed, and C# would have called
  `Leaf.ToArrowColumns` (CS0117 across assemblies). The walk now descends a field-level flatten as
  `GatherFieldsImpl` does; for that shape C++ gains one redundant direct `#include` (ruled by the
  maintainer 2026-10-14). No golden or corpus moved. New case
  `CsVisitorCrossFile.AMessageInlinedByAFieldFlattenFromAnotherFileIsFoundByTheSharedWalk` (also
  pins the C++ include set). Plugin 164/164 (bucket 163); protoc-coverage 22/22; Fletcher.Tests
  372/372 on both TFMs; ProtocDotnet 28/28; Beside 4/4; C++ format clean over the whole repo.
- **Review ruling Q7, 2026-10-14 (D-BIND-89): names that collide.** (a) C# properties that
  PascalCase alike (proto2 `foo_bar` + `fooBar`, or a host field and an inlined one) are suffixed
  `_` in field order by `CsUniquePropertyName`; new golden `csharp_proto2.proto` (`FooBar`,
  `FooBar_`, `FooBar__`) compiled and round-tripped in `Fletcher.Tests`. (b) A field-level flatten
  inlining a name the host already has is refused for every backend by
  `ValidateFlattenedNamesUnique`, naming both fields; `GenErrors.FlattenedNameClashFailsBuild` in
  protoc-coverage. Both failed first against the plugin before the fix. No existing golden moved.
  Plugin 166/166 (bucket 165, one case each in `test_csharp_type_table` and `test_csharp_visitor`);
  protoc-coverage 23/23; Fletcher.Tests 373/373 on both TFMs; ProtocDotnet 28/28; Beside 4/4; C++
  and C# format clean.
- **Review ruling Q8, 2026-10-14 (D-BIND-90): a flatten-wrapper pub/sub input follows C++.**
  `PubSubMethods` now leaves flatten wrappers out of the generated set as C++ does, so such a method
  gets no topic class and no pair, and both C# files write C++'s line (`// Skipped: Feed.Push —
  input message 'Batch' has no generated Arrow mapping in this file`); the native file's own wrapper
  branch is removed. Measured before and after on a probe `.proto` through real protoc: before, C#
  emitted `Feed_PushTopic` and its own reason; after, its lines equal C++'s. TypeScript still emits
  the topic constant: recorded as debt in BIND-T. The existing case was rewritten in place
  (`...AndAFlattenWrapperIsSkippedAsInCpp`, counts unchanged) to take the line from the C++ header
  for the same file. No golden moved. Plugin 166/166; protoc-coverage 23/23; Fletcher.Tests 373/373
  on both TFMs; ProtocDotnet 28/28; Beside 4/4; C++ format clean over the whole repo.
- **Review ruling Q9, 2026-10-14 (D-BIND-91): a protoc type named like our namespace.** Measured
  first, protoc's C# and ours compiled into one assembly: `package fletcher; message Gen`, the same
  through `csharp_namespace = "Fletcher"`, and `message Fletcher` with no package each give CS0101;
  the control `package nav; message Gen` compiles. `CsProtocNamespaceConflict` (D-BIND-82) now also
  refuses a file whose top-level message or enum protoc would declare as `Fletcher` or
  `Fletcher.Gen`. After the fix the same probe refuses the three shapes in the plugin and the control
  still compiles. New case `CsNames.AProtocTypeNamedLikeOurNamespaceIsRefused` (10 shapes, messages
  and enums, near misses included). No golden moved. Plugin 167/167 (bucket 166); protoc-coverage
  23/23; Fletcher.Tests 373/373 on both TFMs; ProtocDotnet 28/28; Beside 4/4; C++ format clean over
  the whole repo.
- **Review ruling Q10, 2026-10-14 (D-BIND-92, D-BIND-93): forced nulls and temporal defaults.**
  `ToArrowColumns` opens with one pass that refuses, with an `ArgumentException` naming class,
  property and row, a null forced into a non-nullable string or bytes field, a list element at any
  depth, a map key or value, or a collection; the public `ToArrow` refuses a null row (scope widened
  by the maintainer from the ruling's field, key and element). Before, such a null was written into
  a non-nullable column, became empty bytes, or failed with a NullReferenceException in generated
  code. A non-nullable Timestamp or Duration now starts at zero in its column's unit, so a fresh row
  reads back equal. The C# goldens move by design (every class's ToArrow; the check pass; the two
  initialisers). New `Fletcher.Tests` `GeneratedNullRefusalTests` (13 refusal cases over the
  coverage golden, the unit default, and a reflection round trip of a fresh row of EVERY generated
  class): all 15 fail against the goldens of `2ece077`, which compile. Two plugin cases updated to
  pin the new text; `ProtocDotnet`'s default-Timestamp assertion updated. The Arrow-tier gap is a new
  BIND-9 acceptance bullet. Plugin 167/167; protoc-coverage 23/23; Fletcher.Tests 388/388 on both
  TFMs; ProtocDotnet 28/28; Beside 4/4; C++ and C# format clean.
- **Review debt P-D1, 2026-10-14: three more line breaks escaped in C# string literals.** C# ends a
  line at U+0085, U+2028 and U+2029 as well as at CR and LF, so an option-metadata value (or a time
  zone) holding one, passed through raw by `CsStringLiteral`, made the generated C# fail with CS1010.
  Measured first in a one-file project (raw: CS1010 "Newline in constant"; escaped: compiles). They
  are now written `\u0085`, `\u2028`, `\u2029`; their neighbours (U+00A9, U+2026) still pass
  through. The existing case gains five assertions (counts unchanged); a mutant without the escapes
  fails it. No golden moved. Plugin 167/167; C++ format clean over the whole repo.
- **Review debt G-D1, 2026-10-14: the shim is looked for beside the assembly too.** Since 6e-2
  `NativeLoader` probed only `AppContext.BaseDirectory`, which in a plugin-style host (a PowerShell
  module, an MSBuild task, a custom AssemblyLoadContext) is the HOST's directory, not the one holding
  the package's `runtimes/<rid>/native/`. It now probes beside the assembly first, when
  `Assembly.Location` is non-empty (IL3000 suppressed on that one guarded read), then beside the
  application, once when they are the same; a NativeAOT or single-file app has no assembly directory
  and probes the application's alone. New case
  `NativeLoaderTests.TheShimIsLookedForBesideTheAssemblyThenBesideTheApplication` on the pure candidate list (a second shim copy cannot be loaded in one
  process: the single-copy marker refuses it). Mutants: without the suppression the Interop build
  fails (IL3000 ×4); probing the application alone fails the case. Fletcher.Tests 389/389 on both
  TFMs; AotSmoke published and run OK (warnings as errors); C# format clean.
- **Review debt P-D3, 2026-10-14: closed as unreachable, by measurement.** C++ notes a field "with
  no Arrow mapping, absent from the schema" only when the IR cannot represent it yet does not mark it
  UNSUPPORTED, and the IR builds no such node; every UNSUPPORTED field refuses the whole file for all
  backends first (GIR-8, D-BIND-85). Probed through real protoc (flatten wrappers around a map and a
  repeated scalar, singular and repeated; a recursive message): both backends map or omit the same
  fields, with no note in either. Closed without a code change by the maintainer's choice.
- **Review debt T-D4, 2026-10-14: the protoc-dotnet lane's change filter names
  `dotnet/Directory.Build.targets`.** `Fletcher.Model`, which the lane builds, takes its pinned AOT
  toolchain packs from that file (D-BIND-81), so a change to it alone did not run the lane. The
  sparse checkout already took all of `dotnet/`; every other filter takes `dotnet/**`. actionlint
  clean (its one finding, `queue: max` in `ci.setup-devcontainer-image.yml`, is pre-existing).
- **Review debt T-D1, 2026-10-14: the public-surface note matches what BIND-6 generates.**
  `BIND-csharp-public-surface.md` §2.7 now names the model file and the native file,
  `csharp_model_only`, the `<Svc>_<Method>Topic` class, the naming and nullability rulings since
  6a (D-BIND-80, 83, 84, 86, 89, 92, 93) and the skip rule (D-BIND-90), and gives the pair's real
  members (options, `Unsubscribe`, `HandlerFaulted`, `AbsorbedCallbackFailures`, `const TopicKey`);
  the consumer recipe's glob is `gen/**/*.fletcher*.cs`, which takes the native file and a model
  generated into `gen/google/protobuf/`. §5.3's diagram draws the topic class, the handler delegate
  and those members, and marks the view and accessor as BIND-7's (parse and render checked under
  mermaid@11).
- **Review debt T-D2, 2026-10-14: the architecture diagrams draw BIND-6 as built.** Diagram 2 of
  `BIND-architecture-diagrams.md`: the native pair is `<<built>>` and emitted by default
  (`csharp_model_only` leaves it out, D-BIND-76), replacing "emitted only when asked for"; a topic
  box in the model file is shared by the native and gateway pairs (D-BIND-77); the model box adds
  cross-file types (D-BIND-80), nullability (D-BIND-83, 86) and the forced-null refusal (D-BIND-92);
  the Model box and the prose below it say built. Diagram 9 was already current. Parses and renders
  under mermaid@11.
- **Review debt T-D3, 2026-10-14: the no-drift case covers every output.** The case proving
  `csharp` changes no other output by a byte now also runs over `accessor`, `rust` and
  `ts,ipc,accessor,rust`, and requires those bases to write their `.accessor.pb.h` and `.rs` files.
  No drift found; mutants making `csharp` alter the accessor header or the Rust module each fail
  it. No new case (counts unchanged). Plugin 167/167; C++ format clean over the whole repo.
- **Review debt T-D5, 2026-10-14: four stale texts corrected.** The D-BIND-72 bullet's "native pair
  emitted only when requested" (D-BIND-76 made it the default; `csharp_model_only` opts out); the 6b-2
  note's claim to meet the whole D-BIND-72 bullet (the topic test came with B5, the wire comparison
  with 6d-2); the BIND-6 exit gate's `CsharpVisitor.*`, in this tracker and the dev plan, which no
  suite is called (`CsVisitor`, `CsVisitorPair`, `CsVisitorCrossFile`, `CsNames`, `CsTypeTable`);
  and the protoc-dotnet README's "no conversion until BIND-6c" and "wire comparison with 6d". Each
  correction is marked where it stands.
- **Review debt T-D9, 2026-10-14: every amended decision points to its amender.** Swept the digest
  for every amends / extends / supersedes / reverses relation and checked each target. Four were
  unmarked and now carry a dated note: D-BIND-75's cross-file bullet (superseded by D-BIND-80),
  D-BIND-72 point 3 (by D-BIND-76, native pair by default, and by D-BIND-74, the Model reference),
  and D-BIND-14's three packages (four, by D-BIND-74).
- **Review fix B5, 2026-10-13: C# registers C++'s topic, by test.** `test_topic_path.cpp`'s
  `ExpectBackendsAgree` generated C++ and TypeScript only, and the C# topic was checked against
  hand-written literals, so a change to C++'s form would have passed. It now also generates `csharp`
  and requires `<Svc>_<Method>Topic.Segments` and `.Key` to equal C++'s `TopicSegments()` and
  `TopicKey()`, for a single-segment and a dotted package. A mutant writing the package in its C#
  namespace form (`Eiva.Nav`) fails both cases. No new case: the two existing ones now cover three
  backends. Plugin 162/162; C++ format clean over the whole repo.
- **Open note, requested by the maintainer 2026-10-13: cross-language pub/sub, end to end.** B5 proves
  that C# and C++ GENERATE the same topic; nothing yet proves that a publisher in one language and a
  subscriber in another actually exchange rows. Wanted: every pair of languages that has a pub/sub
  surface, in both directions, through the generated pairs, over a real transport. The matrix today:

  | Publisher → subscriber | Path | Covered today |
  |---|---|---|
  | C++ ↔ C++ | Fast DDS | ✅ `pubsub-arrow-fastdds` |
  | C++ ↔ TypeScript | Fast DDS + gateway | ✅ both directions, `gateway-fastdds-ts` |
  | TypeScript ↔ TypeScript | gateway | ✅ `gateway-end-to-end` |
  | **C# (native) ↔ C++** | Fast DDS | ❌ |
  | **C# (native) ↔ TypeScript** | Fast DDS + gateway | ❌ |
  | **C# (native) ↔ C# (native)** | Fast DDS, two processes | ❌ (in-process `inprocess` only, `GeneratedPairTests`) |
  | **C# gateway client ↔ C++ / TypeScript / C# native** | gateway | ❌ (the client is BIND-8's, not built yet) |

  Rust has no pub/sub surface (its backend is the read-only accessor) and so no row. Each new row would
  send rows of one shared `.proto` that reaches every mapped shape (scalars, lists, maps, nested and
  `optional` messages, Timestamp/Duration), compare field by field on the receiving side, and run on
  Linux and Windows. **Owner to be ruled.** Suggested: the C#-native rows as their own lane in BIND-9
  (CI), the gateway-client rows inside BIND-8's acceptance, and the whole matrix named in BIND-10's
  docs. **Owner RULED by the maintainer 2026-10-14, at BIND-6's close: split by readiness, as
  suggested.** The three C#-native rows are an acceptance bullet of BIND-9 (their own CI lane, Linux
  and Windows), the gateway-client rows one of BIND-8, and BIND-10 documents the whole matrix; neither
  BIND-8 nor BIND-9 closes without its rows. **Since then every row is covered:** the gateway-client
  rows by BIND-8d (D-BIND-116), the three C#-native rows, both directions, by BIND-9b-4 (D-BIND-161,
  in the same lane rather than one of their own), all in `integration-tests/gateway-dotnet-crosslang`.
- **BIND-6 CLOSED 2026-10-14, with this debt carried out as KNOWN (the maintainer's choice: record,
  not fix).** Every review BLOCKER (B1 to B5), all ten rulings (D-BIND-83 to 93) and every debt the
  review marked important (P-D1, P-D3, G-D1, T-D1 to T-D5, T-D9) are done; the rest stays open, each
  with the item where it would naturally be picked up, none of them an acceptance bullet:
  - **P-D2** `ConversionBlocker` re-walks shared sub-messages (no memo): a plugin speed matter, not
    seen to matter at today's sizes.
  - **P-D4** an exception from the schema sink would crash the plugin instead of returning `*error`;
    unreachable today.
  - **G-D2** D-BIND-81 pins the ILCompiler pack, not the runtime pack it is used with; an AOT publish
    on an SDK other than 10.0.400 mixes 10.0.11 and 10.0.12. CI is unaffected. BIND-9, with the SDK
    pin. **Closed by BIND-9b-3 (2026-10-22):** not harmless, the mix fails the publish, which the
    new Windows leg met; the runtime pack is now pinned with the AOT packs.
  - **G-D3** the generated subscriber builds a `StructType` per delivery; build it once. BIND-7 or a
    performance pass.
  - **G-D4** decode failures reach `HandlerFaulted` as if the handler had thrown; document them or
    tell them apart. BIND-10, or the next runtime change.
  - **T-D6** the Arrow-only model is proven on net10.0 only (protoc-dotnet's TFM). BIND-9's matrix.
  - **T-D8** the generated subscriber's schema handling (S-9) has never run over a transport without
    a schema channel. BIND-9's cross-language lane.
  - **The NITs** of `plans/reviews/BIND-6-codereview.md` (P-N1 to P-N5, four runtime, T-N1 to T-N4),
    except "a null element in `Publish(IEnumerable<T>)` throws an NRE", which D-BIND-92 resolved: the
    pair publishes through `ToArrow`, which now refuses a null row with an `ArgumentException`.
  - **Found 2026-10-14, at BIND-7's start (D-BIND-97's measurement):** the generated
    `FromArrow(StructArray, int)` reads `array.Fields[n]` per column, and Apache.Arrow 23's
    `StructArray.Fields` allocates 64 bytes on every read, so `FromArrow` allocates 64 bytes per
    column per row. Fix: read `Fields` once per call. **Paid at BIND-7c (D-BIND-102), 2026-10-15.**

### BIND-7 — Arrow view + accessor emitters + capstone third arm

**As** an analytics developer, **I want** the C# counterparts of
`<stem>.fletcher.arrow.pb.h` and `<stem>.fletcher.accessor.pb.h`, **so that** every
output the plugin delivers has a C# form and I read batches without per-cell
allocation.

**Acceptance**

- `<stem>.fletcher.arrow.cs` and `<stem>.fletcher.accessor.cs` under the one
  `csharp_accessor` token.
- **The `StructArray.Fields` windowing question is resolved empirically first**
  and the finding recorded in `BIND-locked-decisions.md` (D-BIND-10), with a nested
  fixture at a **non-zero offset** as the forcing test.
- `<Class>View`: a `readonly struct` of (the accessor, a row), whose only public
  members are the field properties, named and typed as on the model class, reading
  the accessor's cast-once columns in place; it is also the accessor's `RowView`,
  and `ToRow(row)` is on the accessor (D-BIND-98). A string field has a `string`
  getter and a zero-copy UTF-8 `ReadOnlySpan<byte>` getter. *(Ruled 2026-10-14,
  D-BIND-97, replacing "a `readonly struct` over `(StructArray, int)`": measured,
  `StructArray.Fields` allocates 64 bytes per read, so such a view's every getter
  would allocate.)*
- `<Class>Accessor`: **validate-once / cast-once**, positional type-only validation
  (name- and nullable-flag-tolerant), no per-cell materialisation, cached columns
  own their buffers; `TryMake(RecordBatch, out accessor, out error)` and
  `TryMake(StructArray, out accessor, out error)`, never throwing on bad input
  (D-BIND-94; this read "the documented deviation from C++ `Make`-throws and Rust
  `Result`", but C++ returns a status, never throwing, D-RBA-4); row-bound `RowView`
  for struct fields; no per-field getters on the accessor, which has `Length`,
  `IsNull(row)`, `At(row)` and `ToRow(row)` over `int` rows, so no field name can
  collide with a member (D-BIND-98, amending D-BIND-97); `SchemaMetadata`,
  `FieldMetadata(int)`, `FieldMetadata(string)` and the shortcut `Metadata(key)`
  (D-BIND-95, amending Q18); a message it cannot support keeps its class, with a
  `TryMake` that refuses and says why (D-BIND-96);
  **nesting capped at RBA's 2/3** (Q6).
- Full type parity: scalar, nullable scalar, struct, repeated scalar, repeated
  struct, map (scalar + message value), nested list to the cap, metadata.
- **C# joins `accessor-capstone` as the third language**, consuming the same
  `accessor_capstone.proto`, `accessor_capstone_fixture.json` and
  `accessor_capstone_expected.json`, asserting `observed == expected`. No
  expected values transcribed into the C# test.

**Plan, agreed 2026-10-14 at the start.** A read-only map of the C++ and Rust accessors and the
capstone came first; four questions were ruled before any code (D-BIND-94 to 97, above), and a
fifth, member-name collisions, at 7a's start (D-BIND-98). Slices:

- **7a:** the `csharp_accessor` token and its two files; the accessor's scalars, nullable scalars and
  structs (with the view as its row type), both `TryMake`s, the metadata API, and D-BIND-10 recorded
  with its forcing test (a nested `StructArray` at a non-zero offset; 6c-2 already measured that
  `Fields` returns children sliced to the parent). Golden, plugin cases, runtime cases, no-drift.
- **7b:** collections: repeated scalars and messages, maps with scalar and message values, lists of
  lists to the 2/3 cap, and D-BIND-96's refusal beyond it.
- **7c:** the accessor's `ToRow(row)` (D-BIND-98) and the UTF-8 span getters.
- **7d (pushed 2026-10-15, `8e97ebb`):** the capstone's C# arm, in the protoc-dotnet lane (its sparse checkout and `ci.pr`'s filter
  gain `integration-tests/accessor-capstone/**`), and the README's third arm. **Also found by the
  map:** the C++ (`integration-protoc-arrow`) and Rust lanes' filters do not list that directory
  either, so a change to the capstone files alone runs neither; 7d adds it to all three.
- **7e:** fresh-context review and close (`plans/reviews/BIND-7-codereview.md`, `BIND-7-conformance.md`):
  2 blockers fixed, rulings D-BIND-103 to 107. **For a consumer (D-BIND-105):** a struct, list or
  map element from another file, `repeated google.protobuf.Timestamp` included, is read through that
  file's accessor, so every file a file imports is generated with `csharp_accessor` too, as BIND-6e
  asks of the model; the protoc-dotnet lane compiles such a case. The accessor borrows its source
  (D-BIND-103).

**Closed 2026-10-15 on CI at `268dceb` (`ci.pr` 38746753749, 52/52).** Known debt carried out, by
the maintainer's choice, from `plans/reviews/BIND-7-codereview.md`:

- **T-D2** the capstone's C# arm takes its input types from C#'s own `Schema`; compare
  `CapstoneBatch.Schema` with the C++ `.ipc` (add `ipc` to the capstone's Exec). BIND-9's lane work.
- **T-D3** coverage gaps: `MapView` foreach allocation; bool/double/enum windows in the measured
  loop; a null inner list at depth 3 and under a scalar leaf; `ToRow` of a null StructArray row;
  refusals from a StructArray source; `@` and `Schema_` messages, proto2, empty messages, a
  field-level flatten, map keys bool/uint32/uint64 and enum/bytes map values.
- **R-D2** malformed offsets throw from a getter, and `ListView.Of` / `MapView.Of` check on every
  read (C++ has the same exposure).
- **R-D3** `default(<Msg>View)` throws on every property; document it.
- **P-D3** `FieldsOf` runs several times per message; `FileHeader` is duplicated.
- **The NITs** of the review.

### BIND-T — TypeScript `Publisher` / `Subscriber`

**As** a TypeScript developer, **I want** generated publisher/subscriber classes,
**so that** I do not hand-wire the topic constant and schema on every call.

**Acceptance**

- Per service method, `<Service>_<Method>Publisher` and
  `<Service>_<Method>Subscriber` emitted into `<stem>.fletcher.ts`, named exactly
  as the C++ generated pair.
- Thin wrappers over `FletcherClient.publish` / `.subscribe`; the topic constant
  and `TypedSchema<T>` are bound in, so the row type is inferred at the call site.
- **Purely additive** — `TsVisitor.DescriptorByteIdentical` stays green; existing
  hand-wired usage keeps working; no new runtime dependency.
- New emitter cases in `protoc/tests/test_ts_visitor.cpp`; `tsc` typecheck passes.
- End-to-end coverage against a live gateway ~~in the extended
  `integration-tests/protoc-gateway-client-ts`~~ **in the existing live lanes (D-BIND-184):**
  `gateway-end-to-end`'s protoc-gen test over both providers, and `gateway-dotnet-crosslang`'s
  TypeScript peer on the generated pair; `protoc-gateway-client-ts` stays byte-compat and gains
  a typecheck of the pair.
- Attachments and `Subscription`/unsubscribe are exposed, not hidden.
- **A flatten-wrapper input, debt from D-BIND-90 (2026-10-14).** For a method whose
  input is a flatten wrapper, C++ writes no pair and the line `// Skipped: Svc.Method
  — input message 'W' has no generated Arrow mapping in this file`, and C# now does
  the same in both its files, with no topic class. TypeScript still emits the topic
  constant (`export const Feed_PushTopic = 'q8/Feed/Push';`, measured). BIND-T's pair
  follows C++ (no pair, C++'s line); whether the constant goes too is BIND-T's to
  rule, since removing it changes existing TS output.
- **Batched delivery, a parity item from D-BIND-133 (2026-10-19).** The C# gateway client
  gained `SubscribeBatchedAsync` (windows flushed by row count, deadline and close, as native
  `SubscriberArrow`); `gateway-client-ts` still delivers row by row. BIND-T decides whether its
  client and pair gain the same shape.
- **The null refusal, a parity item from D-BIND-156 (2026-10-22).** The two C++ codecs and the
  C# gateway client refuse a batch with a null in a non-nullable field (its parent present) at
  bind, naming the field and the row; `gateway-client-ts` still encodes it, and a C++ subscriber
  behind the gateway then reads a value that is not there. BIND-T gives it the same refusal, with
  the same message, and a test.
- **A full-fidelity topic declaration (D-BIND-117, ruled 2026-10-19 at BIND-8d; MOVED INTO
  BIND-8e by D-BIND-130, so BIND-T inherits it done and its pair declares with it).** The
  plugin embeds each message's Arrow IPC schema (the bytes its `ipc` output writes,
  `SerializeSchemaIpc` of the generated schema) in the generated `.ts`, as base64 on
  the `TypedSchema`, and `FletcherClient.createTopic(topic, typedSchema)` sends it as
  `schemaIpc` (the JSON `schema` stays for hand-built descriptors). Today TypeScript
  can declare only a flat message, and even that conflicts with the same message's
  native declaration (D-BIND-112's measurement). Then `gateway-dotnet-crosslang`'s
  TypeScript peer declares its own topic instead of relying on the C# client's.

**BIND-T rulings, 2026-10-28** (D-BIND-181 to 184, asked one at a time after mapping the
TypeScript generator, `gateway-client-ts` and the lanes that run them): the pair has C#'s gateway
pair's shape, `create(client)` declaring the topic, `subscribe` returning the id (181); a method
whose input is a flatten wrapper gets no TypeScript topic constant, only C++'s skip line, an
exception to D-BIND-6 and 7 named in the release notes (182); the TypeScript client and the
generated subscriber gain D-BIND-133's batched delivery, and the client an `onError` hook that
both paths use (183); the live coverage runs in the existing gateway lanes (184). The null refusal
needs no ruling: D-BIND-156 already gives TypeScript the same refusal and message.

**Slices** (proposed with the rulings):

- **T-1 the null refusal** (D-BIND-156): `encodePositional` refuses a null in a non-nullable field
  whose parent is present, with the C++ codecs' message (`field '<path>' is not nullable, but the
  row has a null there; …`); `gateway-client-ts` tests for a top-level, nested, list-element and
  map-value null, and a null below an absent parent still encoding. *As built, see below.*
- **T-2 `onError` and batched delivery** (D-BIND-183): `FletcherClientOptions.onError`,
  `undecodableRows`, the per-row path moved onto them; `subscribeBatched` with its flush rules,
  tested with fake timers.
- **T-3 the generated pair** (D-BIND-181, 182): the emitter, the wrapper skip, the emitter cases
  in `test_ts_visitor.cpp` (the skip line compared with C++'s header for the same file, as
  D-BIND-90's C# test), the coverage goldens regenerated, the pair typechecked in
  `protoc-gateway-client-ts`.
- **T-4 the live lanes** (D-BIND-184): `gateway-end-to-end`'s protoc-gen test on the pair
  (publish with attachments, subscribe, unsubscribe, batched) over both providers; the crosslang
  TypeScript peer on `CrossLang_TsToCsPublisher` and siblings, its `uncaughtException` hook
  replaced by `onError`.
- **T-5 docs, review and close:** `gateway-client-ts/README.md` and `protoc/README.md`, then a
  fresh-context review as for every item.

- **T-1, 2026-10-29 (D-BIND-156): the TypeScript client refuses a null where the schema promises a
  value.** `encodePositional`, so every `publish`, throws before anything is sent when a field
  marked not nullable is `null` or absent (`undefined`) while its parent is present:
  `encodePositional: field '<path>' is not nullable, but the row has a null there; a decoder of
  this schema has nowhere to put it (D-BIND-156)`, the single-row wording of `arrow-bridge`'s
  codec (one call encodes one row, so there is no row index to name), the path in the C# gateway
  client's form (`s.x`, `l.item.x`, `m.value.x`). The row and every struct share one walk
  (`writeFields`), so the check is the encoder's own and costs nothing on a valid row. **Found
  while building it:** only a NAMED field's `nullable` is read. The generated element and
  map-value descriptors write `nullable: false` as a placeholder (`TsVisitor::ElementDescriptor`,
  output D-BIND-6 keeps byte-identical), while the native schema, and so C++ and C#, marks every
  list item and map value nullable; reading the placeholder would refuse rows C++ and C# accept.
  So a null element or map value encodes as before, and a null in a field of a present element
  or map value is refused by its path. Tests (`wire-encode.test.ts`, 7 new, 45 in all): top-level,
  absent, struct field, list-element field and map-value field refused; a null below a null
  struct and a null element and map value accepted. Two mutants caught (no refusal: 5 fail; the
  element path without `item`: 1 fails). `gateway-client-ts/README.md` states the refusal. Every
  TypeScript publisher in the integration tests (end-to-end, fastdds-ts, the crosslang peer, the
  byte-compat vectors) sends complete rows, read before the change; CI runs them.
- **T-1 done on CI 2026-10-29** (`56c7844`, `ci.pr` 40848132221, 61/61 on the first attempt,
  every side workflow green): every TypeScript lane green on Linux and Windows, so no existing
  TypeScript publisher sent an incomplete row. **Left for later, found in passing:** a `null` map
  key in TypeScript is written as the string `"null"` (the encoder's `writeScalar` has no
  refusal), where C++ and C# refuse it (D-BIND-131).
- **T-2, 2026-10-29 (D-BIND-183): `onError`, `undecodableRows` and `subscribeBatched` in the
  TypeScript client.** `FletcherClientOptions.onError(fault)` with `reason`
  (`'HandlerThrew' | 'RowUndecodable'`), `subscriptionId` and `error`; without it,
  `console.error`; what it throws is ignored. Both paths decode and call through one `deliver`,
  so a row that does not decode or a callback that throws is reported and the next row goes on;
  before, either escaped `ws.onmessage` as an uncaught exception. `subscribeBatched(topic, schema,
  callback, options?)` keeps one `Batcher` per subscription (`src/batcher.ts`), D-BIND-133's
  rules as C#'s `GatewayBatcher` has them: `RowLimit` at `maxRows`, `Timeout` armed by the
  window's first row or drop and anchored to the previous flush, `Closing` on `unsubscribe`
  (before the request is sent), `close` and the WebSocket closing; a dropped row counted in
  `rowsDropped`, a window of only drops delivered empty; options refused before anything is sent.
  **Found while building it:** `setTimeout` waits at most 2^31-1 ms (a longer delay fires at
  once), so TypeScript's longest finite `timeoutMs` is that, about 24.8 days
  (`MAX_BATCH_TIMEOUT_MS`), not C#'s 49.7; D-BIND-183's "native's bounds" is amended so. The
  crosslang TypeScript peer now prints its `ERROR` line from `onError`, since an undecodable row
  no longer escapes to its `uncaughtException` hook (moved here from T-4, or the lane would have
  lost that report between the slices). Tests (`batched.test.ts`, 21 new, 66 in all, green in the
  devcontainer, fake timers including `performance`): each flush reason and its edges, the
  anchored deadline and a steady stream, `Infinity`, `maxRows` 0, the three ways to end, parallel
  attachments, drops and an empty window, a throwing callback, the refused options, and on the
  per-row path an undecodable row, a throwing callback, `console.error` without `onError`, and an
  `onError` that throws. Six mutants caught (the deadline not anchored, an only-drops window
  skipped, `unsubscribe` or the connection end not flushing, the callback's throw passed on,
  `onError`'s throw escaping); a seventh was equivalent and is not counted.
  `gateway-client-ts/README.md` documents both. The generated subscriber's `subscribeBatched` is
  T-3's.
- **T-3, 2026-10-29 (D-BIND-181, 182, 184): the generated TypeScript pair.** `TsVisitor` appends,
  after everything the file had (D-BIND-7), one `import type { BatchCallback, BatchOptions,
  FletcherClient }` (ES imports are hoisted wherever written, so the header stays as it was) and
  per pub/sub method `<Svc>_<Method>Publisher` (static `topic` and `schema`; private constructor;
  `static async create(client)` calls `createTopic(topic, schema)`, so with `schemaIpc`;
  `publish(row, attachments?)`) and `<Svc>_<Method>Subscriber` (`subscribe(handler)`,
  `subscribeBatched(handler, options?)`, `unsubscribe(id)`), each call one `FletcherClient` call,
  the row typed as the message's interface; no parameter properties, so the output stays
  erasable TypeScript. D-BIND-182 as ruled: the topic check's message set leaves out recursive
  messages and flatten wrappers, so those methods get C++'s line and no constant. **Found while
  building it:** a message with no field TypeScript can carry gets no `TypedSchema`
  (`GenerateMessage` writes nothing), so its method keeps its topic constant and gets `// No pair:
  Svc.Method — input message 'M' has no field TypeScript can carry` instead of a pair that would
  not compile; C++ writes a pair over an empty row. Unit cases (`test_ts_visitor.cpp`, `TsPair`,
  6 new, 202 in the plugin suite): the pair named as C++'s, its bound text line by line, appended
  after the constants, the wrapper and recursive skips with the line taken from the C++ header
  generated for the same file, the empty input, and no import without a method. The protoc-coverage
  goldens have no pub/sub method and do not move. **The lane** (`protoc-gateway-client-ts`): a
  TypeScript-only `proto/pair.proto` (one method, with the well-known-types include found as
  `gateway-fastdds-ts` finds it) and `test/pair.test.ts`, which drives the generated pair over a
  stub gateway (the statics, `create`'s declaration with `schemaIpc`, `publish`'s frame and
  attachments, `subscribe` and `unsubscribe`, `subscribeBatched` with its options) and holds
  three `@ts-expect-error` lines; both CI legs now run `npm run typecheck` after `npm test`. **Found
  while wiring it:** the lane's `typecheck` script had never run on CI and failed on two things
  unrelated to the pair, `rootDir` excluding the client source `paths` points at, and
  `find-binary.ts`'s `Dirent` type under the current `@types/node`; both fixed. Rehearsed in the
  devcontainer: the lane built from empty `build/`, 8 tests green, the typecheck green, and a
  mutant that types the pair's rows as `any` fails it on all three lines. The test matrix counts
  `test_ts_visitor` 12, generator total 201. `protoc/README.md` describes the pair and the skip.
  The live lanes are T-4's.
- **T-3 done on CI 2026-10-29** (`49b3b50`, `ci.pr` 40860437527, 61/61 on the first attempt,
  every side workflow green): the new `npm run typecheck` step green on both legs of
  `protoc-gateway-client-ts`, and `gateway-dotnet-crosslang`'s `tsc --noEmit` green over its
  generated file, which now holds the pairs.
- **T-4, 2026-10-29 (D-BIND-184): the generated pair through a live gateway.**
  **`gateway-end-to-end`:** `telemetry.proto` gains `service TelemetryFeed` with three methods
  (`Report`, `Windows`, `Deadline`), one per case so each has its own topic, and the lane's CMake
  the well-known-types include `gateway-fastdds-ts` uses; `protoc-gen.test.ts` gains three cases,
  run over both providers: a typed row with attachments through `create`, `publish` and
  `subscribe`, then nothing after `unsubscribe` (a second client's subscriber is the sentinel that
  tells the case the later row was delivered); `subscribeBatched` with `maxRows` 2 delivering two
  `RowLimit` windows in order; and with a 200 ms deadline, every row in a `Timeout` window, in
  order, however the rows fall. **`gateway-dotnet-crosslang`:** the TypeScript peer publishes
  through `CrossLang_TsToCsPublisher` and `CrossLang_TsToNativePublisher` (whose `create` declares
  the topics, as `createTopic` did) and subscribes through `CrossLang_CsToTsSubscriber` and
  `CrossLang_NativeToTsSubscriber`, so the TypeScript rows to and from C# and C++ go through the
  pair. Rehearsed in the devcontainer: `gateway-end-to-end` 37 tests green (13 in
  `protoc-gen.test.ts`, the six new over the in-process and Fast DDS providers);
  `gateway-dotnet-crosslang` `tsc --noEmit` clean over the peer and all 11 rows green, the four
  with a TypeScript end among them. **Found:** that
  lane's `typecheck` script (not run on CI) fails only on the two faults T-3 fixed in
  `protoc-gateway-client-ts` (`rootDir`, `find-binary.ts`'s `Dirent`), none in the new cases;
  left as found.
- **T-4 done on CI 2026-10-29** (`e577332`, `ci.pr` 40872463656, 61/61 on the first attempt, every
  side workflow green): the pair cases green over both providers on Linux and Windows, and the
  crosslang rows through the pairs on both legs.
- **T-5, review 2026-10-29** (`plans/reviews/BIND-T-codereview.md`, `BIND-T-conformance.md`, at
  `e577332` with T-5's README edits; three fresh-context reviewers): no defect in the emitter's
  message set or the batcher's rules, 2 BLOCKERs in shipped text (B1 the client README's
  hand-built `subscribe` example does not compile, checked: TS18046; B2 the batched deadline
  described as counted from the window's first row, in the `.d.ts`, the README and D-BIND-183) and
  5 questions, all ruled: an async handler's rejection reported through `onError` (D-BIND-185);
  `unsubscribe` stops delivery at once, as C# (186); a method whose input has no field TypeScript
  can carry keeps its constant and gets `// No pair:` (187); the changes for a 0.5.x user in the
  Release notes and both npm READMEs, with the same-MAJOR.MINOR line (188); the null map key
  refused (189). **To fix before the close, with the blockers and rulings:** T3 (a golden of a file
  with pairs), T4/M5 (`gateway-end-to-end` type-checked on CI), T5/M8 (the pair's import aliased so
  a message named `FletcherClient` compiles), T6 (bucket 2's TypeScript count), L6 to L8, M6, M7,
  M9, the nits L9 to L13, T11 and T12, and T10's in-flight and refused `unsubscribe` with Q2.
  **Carried as known:** M10 (the well-known-types CMake block copied three times; rows after a
  batcher closes not counted), T9 (the live unsubscribe check cannot see the gateway side), the rest
  of T10.
- **T-5, the review's fixes, 2026-10-29.** **Blockers:** B1, the client README's hand-built example
  names its row type (`subscribe<Position>(…)`, as the repository's own tests do) and
  `client.ts`'s overload comment says the row is `unknown` otherwise; B2, the deadline is described
  as built (armed by the window's first row, due `timeoutMs` after the previous window) in the
  JSDoc and the README, and D-BIND-183 gains a correction note. **Rulings:** D-BIND-185,
  `runCallback` reports a returned promise's rejection as `HandlerThrew`; D-BIND-186, `unsubscribe`
  removes the subscription (closing a batched one) before it sends; D-BIND-187, recorded in
  `protoc/README.md`; D-BIND-188, runbook step 7's `protoc` and new `gateway-client` notes, and both
  npm READMEs (the pair, the same MAJOR.MINOR); D-BIND-189, `encodePositional` refuses a null map
  key with C#'s wording. **Debts and nits:** T3, `TsPair.TheFeedFileIsItsGolden` holds the Feed
  fixture's whole TypeScript output byte for byte (`protoc/tests/golden/ts_pair/feed.fletcher.ts`,
  captured with `FLETCHER_CAPTURE_GOLDENS=1`; `protoc/tests/golden/` added to `.prettierignore`,
  as the licence check already skips it); T4/M5, `gateway-end-to-end` gets T-3's two fixes and
  `npm run typecheck` on both legs; T5/M8, the pair's import is aliased (`FletcherClient as
  __FletcherClient` and the two batch types), so no message name clashes; M6, the statics are
  annotated (`typeof <Topic>`, `TypedSchema<IRow>`); M7, the import is written only when a pair
  is (`TsPair.NoPairMeansNoPairImport`); M9, an old socket's late `onclose` returns at once when
  the client has a newer socket; T6, bucket 2 says what its `TS 38` counts; L6, the README states
  the three differences from C#; L7, L8; L9, a "Member" table; L10, every refused option; L11, the
  example shows `positions.proto`; L12, repeated and map fields must be `[]` and `new Map()`, and
  the long line is wrapped; L13, the generated subscriber comment names `console.error` and a
  rejecting promise; T11, the wrapper's interface and schema asserted; T12, `publish` sends nothing
  when the encoder refuses. **Tests added:** `gateway-client-ts` 8 (an async per-row and batch
  callback rejecting, an in-flight row after `unsubscribe`, a refused `unsubscribe`, the late
  `onclose`, `publish` refusing, a null string and an undefined int map key), 74 in all; plugin 2,
  204 in all. Four mutants caught (the promise not followed, the subscription removed only after
  the reply, the null key written, the socket guard gone). The test matrix counts `test_ts_visitor`
  14, generator total 203.

### BIND-8 — `Eiva.Fletcher.GatewayClient`

**As** a browser, WASM or lightweight .NET client, **I want** a managed WebSocket
client, **so that** I need no native assets at all.

**Acceptance**

- Text-frame control protocol + binary-frame data protocol per wire-format spec
  §"WebSocket Protocol"; sub-IDs parsed from **strings** (the JS-precision
  workaround is a wire contract, not a JS detail).
- `FletcherClient` with ~~`TypedSchema<T>`-driven~~ `SubscribeAsync<T>` /
  `PublishAsync<T>`, using real generics where TS uses a phantom type;
  `CancellationToken` on every async path; no `async void`; no sync-over-async.
  **Amended 2026-09-29 (D-BIND-72):** the client consumes the GENERATED row type and
  its Arrow `Schema`, the same ones the native path uses, and derives its wire
  descriptor from that schema's `field_number` metadata. There is no C# `TypedSchema<T>`.
  **As built (D-BIND-108, D-BIND-112, D-BIND-128):** the client is not generic: it works on
  Arrow `RecordBatch`es, and the typing is the generated gateway pair's. It announces a schema
  as its Arrow IPC (`schemaIpc`), not as a descriptor, and its row codec is positional, so no
  descriptor is derived; the JSON-descriptor code 8a wrote went at the review. Rows arrive one
  at a time (`SubscribeAsync`) or in windows, as native's batched `SubscriberArrow` delivers them
  (`SubscribeBatchedAsync`, D-BIND-133).
- **The generated gateway pair (D-BIND-72):** per service method, a typed
  publisher/subscriber over `FletcherClient`, emitted by the C# backend as BIND-T does
  for TypeScript. It references `Eiva.Fletcher.GatewayClient` only, so a WASM
  consumer builds without native assets. Test cases in `protoc/tests`, and an
  end-to-end round trip against a live gateway.
- **This is the one managed-codec exception** (D-BIND-1): it speaks the WebSocket
  protocol, not the binding ABI, and therefore ports the positional codec into
  managed C#. `Envelope` and its (de)serialisation live here and nowhere else (Q15).
- **Carries no native assets, and CI asserts it**: the packed `.nupkg` contains no
  `runtimes/` entry (D-BIND-13).
- **Bucket 2 ported** — the FILE SET named in Part 4's matrix, not a count, for the
  reason BIND-3's and BIND-4's bullets give: all the TypeScript cases plus the
  gateway C++ ones (`test_schema_codec`, `test_publish_frame`).
- Arrow IPC schema parse of the gateway's base64 `schemaIpc` payload via
  `Apache.Arrow.Ipc`, schema metadata preserved.
- **Cross-language pub/sub, the gateway-client rows (ruled 2026-10-14, at BIND-6's close).** The C#
  gateway client exchanges rows, in both directions and through the generated gateway pair, with C++
  and TypeScript over the gateway, and with the C# native pair. Same shared `.proto` and field-by-field
  comparison as BIND-9's C#-native rows; the matrix is in BIND-6's open note.

**Plan, agreed 2026-10-15 at the start.** A read-only map of the gateway (`gateway/src/ws_session.*`
is the protocol's authority; the spec omits the `unsubscribed` and `published` replies and the
optional `schema` of `create_topic`), the TypeScript client and the live-gateway lanes came first,
and four questions were ruled (D-BIND-108 to 111: a `RecordBatch`-level codec; the `csharp_gateway`
token and names; one request in flight; `PublishAsync` awaits the ack). Slices:

- **8a:** the managed codec: envelope, PUBLISH / MESSAGE frames, the positional row codec driven by
  the Arrow schema's `field_number` metadata, the JSON schema a `create_topic` sends, and `schemaIpc`
  read with `Apache.Arrow.Ipc`. Bucket 2's codec files ported (`test_envelope`, `test_publish_frame`,
  `test_schema_codec`, the TS envelope, wire-encode, object-backend and ws-protocol tests), and byte
  parity with C++-written vectors.
  **Done 2026-10-16.** `dotnet/src/Fletcher.GatewayClient`: `Envelope` (public, the only new public
  type; C++'s order and last-wins rules), and internal until 8b decides the client's surface:
  `GatewayRowCodec` (Bind / Encode / Decode / DecodeBatch over `ArrayData`, the C ABI codec's accept
  list and refusals), `GatewayFrames`, `ControlMessages` (requests, replies, the schema descriptor,
  `schemaIpc`), `WireTypeId`. New project `dotnet/tests/Fletcher.GatewayClient.Tests` (173 cases, no
  native assets): byte parity with protoc-coverage's 13 C++ goldens both ways, through the fixture
  rows now shared with Fletcher.Tests (`dotnet/tests/Shared/CoverageFixture.cs`); Bucket 2 ported
  except the TS client file (8b): `test_publish_frame` 9 (`GatewayFramesTests`), `test_schema_codec`
  11 as properties of the descriptor the client writes (`ControlMessagesTests`, `WireTypeTests`), TS
  envelope 5 (`EnvelopeTests`, with `test_envelope`'s 11), wire-encode 8 and object-backend 3
  (`RowCodecTests`), ws-protocol 14 (`ControlMessagesTests`, `GatewayFramesTests`). The wire-type
  table is read from the gateway's and the TS client's source and held to both. Fletcher.Tests gains
  `ManagedCodecAgreesWithNativeTests` for the accepted layouts no golden covers. 14 mutants, all
  killed. **Two findings, for the maintainer:** (1) the native codec mis-encodes a SLICED batch's
  struct, list, map and fixed-size-list columns (`c-abi/src/nanoarrow_codec.cpp` indexes children
  without the parent's offset, and `ArrowArrayViewListChildOffset` does not add it): it writes
  another row's values. The managed codec is right. **Fixed 2026-10-16** (maintainer's choice, before
  8b): `Physical(view, index)` adds the view's own offset where the codec reads children or list and
  map offsets (struct, list, large list, fixed-size list, map and its entries); the C ABI suite gains
  `ASlicedBatchEncodesEachRowAsTheWholeBatchDoes` (every corpus fixture sliced at each start, each
  row against arrow-bridge's encoding of the unsliced row; the reverted fix fails it), and the C#
  differential case is unskipped. (2) The gateway's `create_topic` accepts a schema of top-level
  SCALAR fields only (`schema_codec.cpp` `BuildArrowSchemaFromJson`), so a generated message with any
  nested field cannot announce its schema there; 8b must decide what `CreateTopicAsync` sends.
  **Ruled 2026-10-16 as D-BIND-112** after a measurement: the gateway's JSON path also drops
  nullability and field numbers and refuses Timestamp/Duration, so even a flat message announced
  through it conflicts with the native declaration (`GatewayDeclaredSchemaTest`, three cases in
  `gateway/tests/test_schema_codec.cpp`; 20 cases with 8b's schemaIpc ones). `create_topic` gains `schemaIpc`; the C# client
  sends it. The gateway is this team's component since 2026-10-16, so no outside sign-off.
- **8b:** `FletcherClient` over `ClientWebSocket` (§2.8's surface, `CancellationToken` on every
  call), the TS client tests ported, and `integration-tests/gateway-dotnet-end-to-end` against a live
  gateway over `inprocess` and `fastdds`. With D-BIND-112: the gateway accepts `schemaIpc` on
  `create_topic` (a gateway case declaring a native schema and the client's IPC on one topic, no
  conflict), the wire-format spec's WebSocket section documents it, and `CreateTopicAsync` sends it.
  The gateway side is done (`a481f6d`). Subscriptions deliver to a handler delegate with the
  decoded one-row `RecordBatch` and a borrowed `Envelope`, on the receive loop (D-BIND-113,
  2026-10-16); topics are `string` keys (D-BIND-114). **`FletcherClient` done 2026-10-16** (in the
  tree): over `ClientWebSocket` behind an internal transport seam, one request in flight with the
  gate held until its reply is read (a cancelled call's late reply cannot reach the next call),
  publish row by row on `published`, `create_topic` with `schemaIpc`, subscriptions registered by the
  receive loop with rows that precede the `subscribed` reply kept and replayed, handler faults and
  undecodable rows counted and reported, no reconnect. Tested against an in-memory gateway
  (`FakeGateway`): the TS client file's 6 cases ported plus 15 more; 6 mutants, all killed.
  Fletcher.GatewayClient.Tests 204. **The live lane done 2026-10-16:**
  `integration-tests/gateway-dotnet-end-to-end` builds the gateway exe, generates the model of
  `proto/fix.proto` (nested message, Timestamp, repeated, map) with the plugin, and runs 6 cases
  against the real gateway over `inprocess` and `fastdds` (12): a generated round trip field for
  field, a same-schema re-declaration accepted and a different one refused (C#'s `schemaIpc` read
  by nanoarrow), a subscriber without a schema, attachments, a refusal, unsubscribe. Announcing
  the schema as JSON instead fails 10 of the 12. Workflow
  `ci.integration-test.gateway-dotnet-end-to-end.yml` (Linux in the devcontainer, Windows), wired
  into `ci.pr.yml` (filter, caller, gate) and `ci.format-check-cs.yml`. **8b is done** once that
  lane is green on CI.
- **8c:** the generated gateway pair (D-BIND-109): golden, plugin cases, a live round trip. The pair
  borrows a `FletcherClient` the caller connects and disposes (D-BIND-115, 2026-10-19).
  **Done 2026-10-19:** `csharp_gateway` (refused without `csharp`, before any output) writes
  `<stem>.fletcher.gateway.cs`: per pub/sub method a `<Svc>_<Method>GatewayHandler(Msg, Envelope)`,
  a `GatewayPublisher` (`CreateAsync(client)` declaring the topic with the schema, `PublishAsync`
  of a row, a row with attachments, rows) and a `GatewaySubscriber` (`SubscribeAsync(handler)`,
  `UnsubscribeAsync`), naming nothing native; skipped methods keep C++'s reason. Golden
  `protoc/tests/golden/csharp_pair.fletcher.gateway.cs` (held by `TheCommittedGoldensAreThePluginsOutput`),
  four `CsGateway` plugin cases (visitor 62, bucket 5 188), `GeneratedGatewayPairTests` (6) compiling
  the golden against the in-memory gateway (Fletcher.GatewayClient.Tests 210), and a typed live round
  trip in `gateway-dotnet-end-to-end` (now 14). The native test projects exclude the gateway golden
  from their golden glob. Two golden mutants killed.
- **8d:** the cross-language rows (C# gateway client ↔ C++, TypeScript and the C# native pair), and
  packaging: `IsAotCompatible`, the `Eiva.Fletcher.Model` reference, the no-`runtimes/` check
  (`Package.GatewayClientHasNoRuntimesFolder`).
  **Measured first, 2026-10-19:** the risk that a C# gateway client's `create_topic` conflicts with a
  native declaration of the same topic (the check compares exact Arrow IPC bytes,
  `pubsub/internal/schema_conflict.hpp`). For all 21 generated messages of `fix.proto`,
  `coverage.proto`, `csharp_pair`, `csharp_proto2` and `csharp_accessor`, the C# `schemaIpc` decoded by
  the gateway's `DeserializeSchemaIpc` and encoded again equals the plugin's C++ `.ipc` (the native
  declaration's bytes) byte for byte; the raw C# bytes differ in every case, so the gateway's
  re-encode is what makes them agree. No fix needed. The lane is ruled D-BIND-116 (2026-10-19):
  `integration-tests/gateway-dotnet-crosslang`, a gateway over `fastdds`, the C# gateway client in the
  test process, C++, C# native and TypeScript peers as child processes, one `.proto` over
  `coverage.proto`'s `CompositeCoverage`, one topic per direction. The TypeScript client cannot
  declare such a topic (its JSON `schema` refuses nested and Timestamp fields, D-BIND-112), so in the
  TypeScript → C# row the C# client declares it with `schemaIpc` and the TypeScript peer publishes
  on it undeclared; BIND-T gives TypeScript `schemaIpc` (D-BIND-117, 2026-10-19). Every backend
  skips a pub/sub method whose message is in another file, so the lane streams a local `CrossRow {
  seq, CompositeCoverage row }`; TypeScript could not reference another file's message at all (an
  empty placeholder import), fixed in the plugin (D-BIND-118, 2026-10-19). The lane then found two
  more defects, both fixed here: TypeScript described a singular flatten-wrapper field as a list of
  the wrapper where the wire carries a list of its element, so it could not exchange such a row
  with C++ or C# (D-BIND-119, 2026-10-19); and the gateway's `subscribed` reply read an
  uninitialised `ArrowStringView` for a field with metadata but no `field_number`
  (`schema_codec.cpp` `FieldToJson`; nanoarrow leaves the value unchanged for a missing key), which
  failed a subscription with `std::bad_alloc` once the topic's schema was already declared.
  **Done 2026-10-19 (in the tree):** `integration-tests/gateway-dotnet-crosslang` (CrossLang tests
  6: C# gateway ↔ C++, ↔ C# native, ↔ TypeScript, each direction; C++ peer `cpp/`, C# native peer
  `dotnet/NativePeer`, TypeScript peer `ts/`; the receiving side compares, `RowCompare.cs`,
  `row_compare.hpp`, `compare.ts`), workflow `ci.integration-test.gateway-dotnet-crosslang.yml`
  (Linux in the devcontainer, Windows) wired into `ci.pr.yml` and `ci.format-check-cs.yml`. Green
  three runs in a row in the devcontainer; three lane mutants each caught by exactly their rows.
  This closes the matrix rows "C# gateway client ↔ C++ / TypeScript / C# native" of BIND-6's open
  note. Plugin: real cross-file TypeScript imports (D-BIND-118) and flatten-wrapper lists
  (D-BIND-119), `test_ts_visitor` 3, the protoc-coverage TypeScript golden moved by those two
  fields; gateway: `FieldToJson` fixed, `test_schema_codec` 21 (the new case fails on the old code).
  Packaging: `IsAotCompatible` on `Eiva.Fletcher.GatewayClient` (an AOT-unsafe call fails its
  build), no `Eiva.Fletcher.Model` reference (D-BIND-120; the packed package's one dependency is
  `Apache.Arrow`), and `ci.dotnet`'s step `Package.GatewayClientHasNoRuntimesFolder` on the packed
  `.nupkg` (no `runtimes/` entry, no dependency on `Eiva.Fletcher` or `Eiva.Fletcher.Interop`; both
  mutants caught). **8d is done on CI** (`89f9a34`, `ci.pr` 39248253992, 56/56).
- **8e:** fresh-context review and close.
  **Review 2026-10-19** (`plans/reviews/BIND-8-codereview.md`, `BIND-8-conformance.md`, at `89f9a34`):
  5 BLOCKERs, all checked by the author (TypeScript cross-file imports relative to the include root;
  a cancelled `SubscribeAsync` installing its handler under an id the caller never gets; the
  cross-language retry pooling two partial attempts, which happened on the green run; the
  `test_schema_codec` port testing a JSON descriptor no shipped path writes; a client test that
  cannot fail), 12 questions, DEBT, NITs. Rulings so far: the client exposes `Completion`, a `Task`
  that ends with the connection and carries its reason, and still never reconnects (D-BIND-121);
  TypeScript imports each other file as one namespace, by a path relative to the importing file
  (D-BIND-122, amends D-BIND-118); the gateway builds a `subscribed` reply before keeping the
  subscription and reads an unparseable `field_number` as absent (D-BIND-123); a reply the client
  cannot parse fails only the request it answers, never the connection (D-BIND-124); the handlers
  stay synchronous and their docs warn about `async` lambdas (D-BIND-125); the options gain a
  `ConfigureWebSocket` hook and `MaxMessageBytes` (D-BIND-126), and an optional `ReplyTimeout` that
  ends the connection (D-BIND-127); the client's unreachable JSON-schema code goes, and Bucket 2
  maps `test_schema_codec` to the client's `schemaIpc` reading back exactly, with the
  gateway-internal cases recorded as exclusions (D-BIND-128); the lane's peer → C# rows wait for a
  probe row to arrive first, so they travel live rather than from the Fast DDS backlog (D-BIND-129);
  TypeScript's `schemaIpc` (D-BIND-117) moves from BIND-T into 8e, so the TypeScript peer declares
  its own topic (D-BIND-130); the C ABI codec refuses a null map key and a null dictionary entry,
  as the managed one does (D-BIND-131); the unruled 8a/8b defaults are locked, `HandlerFaulted`
  gains a `Reason` and the client a `DroppedRows` counter (D-BIND-132). All 12 questions ruled
  (D-BIND-121 to 132). DEBT the maintainer chose to fix in 8e: the cheap fixes (CI filters,
  `"schemaIpc": null`, the e2e lane's gateway stderr, stale comments and one-line NITs), client
  robustness (a call during `ConnectAsync`, a locally ended connection's socket, `DisposeAsync`'s
  edges, any decode exception per row, a token cancelled before the send), and tests and AOT
  (AotSmoke links the gateway client and golden, deeper metadata assertions, a frame above 64 KiB,
  a gateway-pair name-clash check); performance is carried as known debt.
  **Fixed 2026-10-19 (in the tree):** all five blockers, all twelve rulings and the chosen debt.
  Client: `Completion`, abandoned subscribes released, replies contained, `ConfigureWebSocket`,
  `MaxMessageBytes` with a buffer that shrinks back, `ReplyTimeout`, `HandlerFaulted.Reason`,
  `DroppedRows`, a call during `ConnectAsync` refused, a locally ended socket aborted, dispose
  safe from a handler, the JSON-schema code deleted while `DecodeBatch` stays (D-BIND-128
  amended; Fletcher.GatewayClient.Tests 198, 13 new cases; 13 mutants). Plugin: TypeScript
  namespace imports by a relative path and `schemaIpc` on each `TypedSchema` (the three protoc-coverage TypeScript goldens gain it), and a refusal of two
  generated C# types of one name (`test_ts_visitor` 6, `test_csharp_visitor` 63). TypeScript
  client: `createTopic` sends a generated schema's `schemaIpc` (38). Gateway: the `subscribed`
  reply built before the subscription is kept, a tolerant `field_number`, `"schemaIpc": null` as
  absent (`test_schema_codec` 23). C ABI: the two refusals (96). Lanes: the cross-language lane
  live after a probe, each retry whole, the TypeScript peer declaring its own topic; the
  end-to-end lane gains a 100 KiB row (in-process) and the odd-`field_number` case, and forwards
  the gateway's stderr (17). `AotSmoke` NativeAOT-compiles the gateway client and its generated
  pair. **Done on CI** at `7e2acca` (`ci.pr` 39281811974, 56/56, every side workflow green).
- **8f:** a batched gateway subscription (D-BIND-133, ruled 2026-10-19 when the maintainer asked
  why 8e had deleted `DecodeBatch`: C++ and the native C# pair deliver batches, the gateway client
  only rows). `FletcherClient.SubscribeBatchedAsync` with `GatewayBatchHandler`,
  `GatewayBatchOptions` (8000 rows, one minute, as native), `GatewayBatchStatus` and
  `GatewayBatchReason`; a window is decoded by one `DecodeBatch`, a bad row costs its window a
  second pass and is counted; flushes at `MaxRows`, at the deadline (anchored to the previous
  flush), before the byte ceiling, and as `Closing` on unsubscribe, dispose and the end of the
  connection; one subscription's handler never concurrent with itself.
  **Done 2026-10-19 (in the tree):** `GatewayBatch.cs`; `BatchedSubscriptionTests` 18
  (Fletcher.GatewayClient.Tests 216, 16 mutants caught); the end-to-end lane's batched
  case over both providers (19); `AotSmoke` drives a window through the batcher. **Done on CI**
  at `7e2acca`, with 8e.
  **Found after the review, fixed 2026-10-20 (review A1):** two writes could reach the socket at
  once (the next request's send over one still returning, `DisposeAsync`'s close over a send);
  every write now takes a write lock. Fletcher.GatewayClient.Tests 219, three new deterministic
  cases, a mutant per lock caught; on CI at `750d79d` (`ci.pr` 39375700668, 56/56).

**BIND-8 CLOSED 2026-10-20** on CI at `750d79d` (`ci.pr` 39375700668, 56/56, every side workflow
green). **Known debt carried**, by the review's IDs (`plans/reviews/BIND-8-codereview.md`, audited
2026-10-20; every other DEBT and NIT item is fixed):

- **C-D7** performance, which the maintainer chose not to fix in 8e: per-row allocation in the
  managed decode, and a codec planned per `PublishAsync` call.
- **G-D2** no size cap on a `schemaIpc` the gateway decodes.
- **G-D4** (unverified) a struct-mapped WKT import in generated TypeScript may name a module users
  do not generate; to check in BIND-T.
- **T-D4** the cross-language lane's C++ comparer is hand-written, so a new field is skipped there.
- **T-D6** the two gateway lanes use fixed ports (19194, 19196).
- **T-D7** the gateway test for the old uninitialised read depends on stack layout.
- **N6** `NoSchemaException` drops the `ArgumentException` it is built from; **N9** the client's
  `ObjectDisposedException` is not in its XML docs; **N14** `AssertAgree` does not dispose its batch.

TypeScript batched delivery is a BIND-T parity item (D-BIND-133 k).

The Fast DDS legs share `gateway-fastdds-ts`'s exposure to Fast DDS 3.4.0's data-sharing defect
(`fastdds-pubsub-provider/README.md`). Ruled by the maintainer 2026-10-16: keep that test as the
defect's guard, make its failure message name the defect, retry the exchange once; a Fast DDS 3.6.x
upgrade is planned separately. The change was made in a session of its own and taken onto this branch (`fccbac6`, 2026-10-16).

### BIND-9 — CI/CD pipelines and packaging

**As** a maintainer, **I want** the .NET tier built, tested and published by the
same machinery as everything else, **so that** it cannot rot.

**Split 2026-10-20 into BIND-9a and BIND-9b** (rulings D-BIND-134 to 152). Testers are
waiting for packages, so **BIND-9a** delivers a preview first; **BIND-9b** is everything in the
acceptance below that 9a does not do.

**BIND-9a — a preview for testers.** *As* a tester, *I want* the C# packages, a gateway that
speaks to them and a plugin that generates C# code, *so that* I can build against the bindings
before the round merges.

- **What it ships** (D-BIND-134 to 136, 139): ONE preview bundle per push to this branch, a
  30-day workflow artifact (a zip) `fletcher-preview-0.6.0-pr.129.<run>`: `nuget/` the four
  packages; `gateway/` the gateway binaries for Linux and Windows, as `cd.gateway.yml` would
  release them; `protoc/` the standalone plugin binaries for Linux and Windows and
  `fletcher/options.proto`, as `cd.protoc.yml` would release them; `README-preview.md`; `LICENSE`
  and `COPYING` at the root, and each component's `THIRD-PARTY-LICENSES.txt` inside it (amended
  2026-10-21, review Q7; D-BIND-152). On this branch `ci.pr` builds dotnet, gateway and protoc on every
  push, whatever the push touched. `Eiva.Fletcher.Protoc` stays in 9b.
- **Nothing released before the merge** (D-BIND-137, 138): no tag, GitHub release, npm, Conan or
  NuGet-feed push; the only version is the per-build `0.6.0-pr.129.<run>`. **Temporary:** after
  the merge the release is `0.6.0-alpha`, which semver ranks BELOW those builds, so the remaining
  bundles are deleted first (they expire after 30 days anyway) and testers clear their NuGet caches;
  the bundle README says so.
- **The unmanaged part** (D-BIND-134): the shim is the only native file, and it is
  self-contained: Fletcher's core, pubsub and providers, the eProsima chain, Micro XRCE-DDS and
  nanoarrow are linked into it. On Linux it loads only `libstdc++`, `libgcc_s`, `libm`, `libc`
  (measured 2026-10-20); on Windows only system DLLs (`KERNEL32`, `ADVAPI32`, `WS2_32`, `MSWSOCK`,
  `IPHLPAPI`, `SHLWAPI`), the VC++ runtime and the UCRT (measured by CI at `9d214c9`).
- **Licence and floors** (D-BIND-140, 141): `LICENSE` and `THIRD-PARTY-LICENSES.txt` in every
  package; each README states the relinking route, the NativeAOT caveat, glibc 2.38 and libstdc++
  `GLIBCXX_3.4.32` (GCC 13) for linux-x64, and the VC++ redistributable for win-x64; the bundle
  names its commit.
- **Forcing tests:** a consumer project using only the unpacked bundle generates its C# code with
  the bundled plugin, restores from the bundled packages alone, publishes and subscribes natively
  through `Eiva.Fletcher` (the shim loaded from the package) and reaches the bundled gateway
  through `Eiva.Fletcher.GatewayClient`, on Linux and Windows; `Interop` carries
  exactly `runtimes/win-x64/native/` and `runtimes/linux-x64/native/`, and `GatewayClient` and
  `Model` carry none; the shim's dependency list (`objdump -p`, `dumpbin /dependents`) names only
  system libraries and the C/C++ runtime; the probe shim is in no package; every package carries
  both licence files, and `THIRD-PARTY-LICENSES.txt` lists the libraries linked into the shim.
- **Slices:** 9a-0 the shim for both RIDs in one pack, the licence files and the package checks;
  9a-1 the bundle job in `ci.pr` (dotnet, gateway and protoc built on every push to this branch,
  the per-build version, the 30-day artifact with `README-preview.md`); 9a-2 package READMEs and
  the consumer tests over the bundle; 9a-3 a short tester note pointing at the newest bundle;
  9a-4 (added 2026-10-21, D-BIND-144) the package README's walkthrough from a `.proto` to publish
  and subscribe, its code checked against the consumer test.
  `cd.dotnet.yml` and the feed are BIND-9b.
- **9a-0 done on CI 2026-10-20** (`9d214c9`, `ci.pr` 39490321322, 57/57, every side workflow
  green, first attempt): `ci.dotnet` gains a `pack` job that needs both legs. Each leg checks its
  shim's load-time dependencies (`scripts/check_shim_dependencies.sh`: Linux measured
  `libstdc++`, `libgcc_s`, `libm`, `libc` only; Windows by `dumpbin /dependents`, first measured on
  CI), writes its notice with the gateway's deployer and hands both on. The pack job requires the
  two notices to list the same libraries, packs the four projects once with both shims in
  `Interop` (`FletcherShimLinuxX64`, `FletcherShimWinX64`, both or neither) and both licence files
  in every package (`FletcherLicenceDir`), takes an optional `package-version`, and runs
  `scripts/check_dotnet_packages.sh` (which absorbs `Package.GatewayClientHasNoRuntimesFolder`).
  **Found on the way:** the deployer read only the Conan graph, so four Apache-2.0 libraries
  compiled in from source were missing from every notice that links them, the released gateway's
  included: nanoarrow (with flatcc) vendored in `fletcher-pubsub`, Micro-CDR and the Micro
  XRCE-DDS Client fetched by `fletcher-xrcedds-pubsub-provider`. Those recipes now package each
  one's licence under `licenses/vendored/<name>/` with a `vendored.json` (versions read from
  `nanoarrow.h` and the `GIT_TAG` pins), the deployer gives each a section naming the package
  that compiled it in, and the pack job requires nanoarrow, Micro-CDR, Micro XRCE-DDS and Fast
  DDS in the notice. Rehearsed in the devcontainer: the real Linux shim and notice, a real pack
  with a stand-in Windows shim, all checks green; four mutants caught (no licence files, no
  shims, one shim, a shim loading an extra library). On CI the Windows shim's list was measured
  for the first time (system DLLs, the VC++ runtime, the UCRT; no `psapi.dll`), both legs' notices
  named the same eight libraries, and `Interop` carries both shims (the packages artifact grew
  from 0.4 to 8.9 MB).
- **9a-1 done on CI 2026-10-20** (`8c54be5`, `ci.pr` 39499932051, 58/58, every side workflow
  green): `ci.pr`'s `detect-changes` sets `bundle` when the PR's branch is
  `feature/csharp-bindings`; then `dotnet`, `gateway` and `protoc` run on every push, `dotnet` packs
  at `0.6.0-pr.<PR>.<run>` (its `package-version` input), and `ci.gateway` and `ci.protoc` take a
  `bundle` input that opens their tag-gated release steps, so the archives and binaries a release
  would ship are built. A new `preview-bundle` job (in `pr_gate`) downloads them and runs
  `scripts/assemble_preview_bundle.sh`, which adds `protoc/fletcher/options.proto`, `LICENSE`,
  `VERSION` and `README-preview.md` (from `scripts/preview/README-preview.md`: packages from a
  local folder, running the gateway, generating code, the floors, the relinking route, clearing
  caches when `0.6.0-alpha` ships), checks that nothing is missing, and uploads
  `fletcher-preview-0.6.0-pr.<PR>.<run>` for 30 days. **Found on the way:** no plugin artifact had
  ever carried a licence notice, though the plugin links protobuf and zlib statically;
  `ci.protoc` now writes one on every run with the same deployer, and the bundle carries it
  (attaching it to `cd.protoc`'s release is BIND-9b). Rehearsed in the devcontainer: the four
  workflows parse, the plugin's notice lists protobuf and zlib, the assembly passes complete and
  fails naming a missing gateway archive. The first push (`3ad543d`) failed `ci.action-lint`: the
  new step expanded `github.head_ref` into its script, an untrusted input there; `8c54be5` passes
  it through the environment. **The first bundle**, `fletcher-preview-0.6.0-pr.129.337` (19.7 MB),
  was downloaded and checked: the four packages and their symbol packages at that version,
  `Interop` with both shims and both licence files; both gateway archives with `LICENSE` and a
  notice that now lists nanoarrow; the plugin binaries, `options.proto` and the plugin's notice
  (protobuf, zlib); `VERSION` naming the commit; the README with its placeholders filled. In the
  devcontainer the bundled plugin generated C# (`.fletcher.cs`, `.native.cs`, `.gateway.cs`) from a
  small `.proto`, and the bundled gateway started (`READY`).
- **9a-2 done on CI 2026-10-21** (`5e41f62`, `ci.pr` 39567194940, 60/60, every side workflow
  green, first attempt; on CI the consumer passed on `ubuntu-24.04` and `windows-2022` with .NET
  8.0.32 and 10.0.13 against bundle `pr.129.338`, the shim loaded from the package's
  `runtimes/<rid>/native/`, and all four packages passed the README check): **the package README** (D-BIND-142),
  `dotnet/README.package.md`, packed into all four packages by `dotnet/Directory.Build.targets` as
  `README.md` with the version and the commit (`FletcherSourceCommit`, passed by `ci.dotnet`'s pack
  job: the PR head, or the pushed commit) filled in: which package does what, the floors
  (D-BIND-141), the relinking route with `git checkout <commit>` and the NativeAOT caveat
  (D-BIND-140); `scripts/check_dotnet_packages.sh` now also requires the README, its facts and the
  commit. **The consumer test**, `integration-tests/preview-bundle-consumer`: `run.sh` takes an
  unpacked bundle, restores the `Eiva.Fletcher*` packages from its `nuget/` alone (package source
  mapping, an empty cache), generates C# with its plugin (protoc from `Google.Protobuf.Tools`, as
  `README-preview.md` says), and runs `PreviewConsumer` for .NET 8 and .NET 10: three rows of a
  message with a nested message, a Timestamp, a list and a map published and received natively
  over `inprocess`, the loaded shim checked to be the package's `runtimes/<rid>/native/` asset,
  then the same rows through the bundle's gateway with the generated gateway pair. In `ci.pr` a
  `preview-consumer` job runs it after `preview-bundle` on plain `ubuntu-24.04` and `windows-2022`
  runners with only that folder checked out; `pr_gate` needs it; the C# format check covers it
  (unrestored, as its packages exist only in a bundle). **Found on the way** (D-BIND-143): CI
  builds the PR head merged into `main`, but the bundle names the head; `preview-bundle` now fails
  when the two trees differ. Rehearsed over the first bundle (`pr.129.337`): green on Windows (this
  host) and Linux (devcontainer, net8.0 rolled forward to the only runtime there); mutants caught
  (`Interop` without the Linux shim: `DllNotFoundException`; no `Eiva.Fletcher.Model` in the bundle:
  NU1101, nuget.org not considered); a real four-package pack passes the README checks, and four
  mutants fail them (another commit, no commit, a floor missing, no README); actionlint clean.
- **9a-3 done on CI 2026-10-21** (`8f5b911`, with 9a-4): the tester note,
  `dotnet/PREVIEW-TESTERS.md`, linked from `dotnet/README.md` and from a "For testers" section at the top of PR #129's description
  (ruled by the maintainer: the repository file, versioned with the branch, plus the PR, where the
  download sits). What the bundle holds, where to get the newest one (the `ci.pr` runs for the
  branch filtered to successful ones, the run's Summary > Artifacts; or `gh run list` and
  `gh run download`), the three steps of `README-preview.md` in short, the platform requirements,
  moving to a newer bundle, what to do when `0.6.0-alpha` ships, and where to report problems.
  Both download routes were followed as written and fetched `pr.129.338`. The note goes with the
  bundle job at the merge (BIND-9b).
- **9a-4 done on CI 2026-10-21** (`8f5b911`, `ci.pr` 39580014921, 60/60, every side workflow green,
  first attempt: 7 snippets matched, four routes green on both runners, .NET 8 and 10, against
  bundle `pr.129.339`; asked for by the maintainer while reviewing 9a-3,
  D-BIND-144): `dotnet/README.package.md` gains "From a `.proto` to publish and subscribe", six
  steps with seven code blocks: the `.proto`, the package references, the MSBuild target that runs
  `protoc` with the plugin (options explained), rows natively (providers by name), Arrow batches
  through `SubscriberArrow` and the generated accessor, and rows and batches through the gateway.
  Each block is a `<snippet name="...">` region of the consumer, which was reshaped for it: one
  function per route, `fix.proto` moved beside the project, `csharp_accessor` generated, the
  properties renamed for a reader (`FletcherVersion`, `FletcherPlugin`, `FletcherProtoInclude`).
  The consumer now checks four routes, two of them new (native Arrow batch, gateway batch).
  `scripts/check_readme_snippets.py` compares block and region and runs in `ci.dotnet`'s pack job
  before the pack; `ci.pr`'s dotnet filter covers it and the consumer. **Found by writing the
  snippet:** an Arrow subscriber that subscribes before the topic's publisher exists gets no
  schema, and its window is flushed as `null` with every row dropped; the snippet declares the
  publisher first, the README says so, and the `SchemaArrival` is disposed. Rehearsed over bundle
  `pr.129.338`: four routes green on Windows (this host) and Linux (devcontainer), .NET 8 and 10;
  the checker passes (7 snippets) and fails on four mutants (a README line edited, a snippet not
  shown, an unknown name, the code edited); the packed README is byte-identical to the source with
  version and commit filled in; the README pack checks pass; `dotnet format` and actionlint clean.
- **Review 2026-10-21** (`plans/reviews/BIND-9a-codereview.md`, `BIND-9a-conformance.md`; three
  fresh-context reviewers): 6 BLOCKERs, all reproduced and fixed, and 10 questions, all ruled
  (D-BIND-145 to 152, the D-BIND-139 amendment, the VC++ wording). The blockers: the shim
  dependency check passed on an empty list (B1, now it must name `libc.so.6` / `KERNEL32.dll`); a
  fork's PR from a branch of the same name would have built a bundle the tester note picked (B2,
  the flag now needs this repository and PR #129, and the note starts from the PR); the
  relinking route failed as written (B3, now the exact commands, rehearsed from a clean copy, and
  `c-abi/README.md` corrected); the plugin's notice lacked its vendored nanoarrow and flatcc (B4,
  the protoc recipe packages the licence and `ci.protoc` asserts it); and two README claims were
  wrong (B5 the load-time check is the ABI only; B6 native batch handlers are not serialised).
  **Found by the fixes:** the packed-README comparison (M10) caught MSBuild turning a `\` in the
  README into `/` on Linux, which would have shipped a broken relinking command; Q2 is the seam's
  documented contract, not a defect (D-BIND-146). **The fixes on CI** at `133631a` (`ci.pr`
  39592797595): every side workflow green first time; `ci.pr` green 60/60 on its third attempt.
  Attempt 1 failed `Fletcher.GatewayClient.Tests` `AWindowFlushesWhenItsDeadlinePasses` on Windows
  net10.0 (two rows expected inside a 200 ms window; the second arrived late), now given a 2 s
  window; attempt 2 failed `Fletcher.Tests` `SubscriberArrowTests.OnlyDroppedRowsStillDeliverAnEmptyBatch`
  on Windows net8.0 (a 100 ms timeout flush not within 5 s), investigated (the path reads correct;
  92 local Windows runs, a one-core pool and a saturated CPU among them, did not reproduce it;
  thread-pool starvation on the 4-core runner the likeliest cause) and put on PR #129's watch list
  on the maintainer's ruling. On CI for the first time and green: the exact Windows dependency list
  against the real `dumpbin`, the PR head checked out (the nuspec names it), the `fastdds` route on
  both runners and both frameworks, `COPYING` in every artifact, the plugin's notice with nanoarrow,
  and D-BIND-150 itself: the partial re-runs kept the run's version, `pr.129.340`.
- **BIND-9a CLOSED 2026-10-22.** Testers have a bundle per push (`dotnet/PREVIEW-TESTERS.md`), the
  packages carry a README whose code CI runs, and everything the review left is in BIND-9b below.

**BIND-9b — the rest**, the acceptance below: the `nuget.eiva.com` publish job and its
self-hosted runner, the packed-size budget, lowering the glibc floor if wanted, the Windows and
`fastdds` NativeAOT runs, the null-in-a-non-nullable-column question, `Eiva.Fletcher.Protoc`, and
the C#-native cross-language rows. From the BIND-9a review: Fast DDS's own vendored third-party
code and Apache-2.0 `NOTICE` files verified and carried before the first release (D-BIND-152);
the relinking route executed by a test on both platforms, the floors tested on a minimal machine,
the plugin's notice compared across platforms and attached to `cd.protoc`'s release (and the npm
package), a lockfile with the release, and a vendored-licence check for the gateway (review T12,
T13, T14, M6, M7, L14).

**BIND-9b rulings, 2026-10-22** (D-BIND-153 to 160, asked one at a time): the release never waits
on the self-hosted runner, as `cd.dotnet` always attaches the packages to the GitHub Release and
the feed push is a job of its own (153); Ubuntu 24.04 and later only, so the Linux floor stays
(154); the MSVC runtime stays dynamic (155); the C++ codec refuses a null in a non-nullable column
(156); a 12 MB packed ceiling on `Interop`, the Linux shim stripped, N-10 accepted (157);
`Eiva.Fletcher.Protoc` carries `options.proto` and wires it into `Grpc.Tools` projects (158);
after the merge the consumer test runs on the PACKED packages, in `ci.pr` and as a gate in
`cd.dotnet` before anything is published (159). **Slices (160):** 9b-0 the rulings; 9b-1
packaging hardening; 9b-2 the codec's null refusal; 9b-3 NativeAOT on Windows and over
`fastdds`; 9b-4 the C#-native cross-language lane; 9b-5 `Eiva.Fletcher.Protoc`; 9b-6
`cd.dotnet.yml`; 9b-7 the licence research and the 9a review's remaining debts (and, added at
9b-4's CI, the Windows net8.0 `SubscriberArrow` timer-flush failures); 9b-8 the merge
preparation, a fresh-context review and the close.

- **9b-1 done on CI 2026-10-22** (`ed2ddce`, `ci.pr` 39798973114, every workflow green: the strip
  step 14,628,480 -> 11,594,728 bytes with 47 exports unchanged, `Interop` 7,999,043 bytes packed,
  `package-consumer` green on both runners; all three parts, on the maintainer's
  ruling against moving the third to 9b-8): **the stripped shim**, `ci.dotnet`'s Linux leg strips
  the packaged copy (`strip --strip-unneeded`), requires its exported symbols to be unchanged, and
  from there runs the dependency and floor check, the tests and the AOT smoke on that copy, the one
  that ships (14.6 to 11.6 MB, all 47 `fl_*` exports kept; the Conan package keeps its symbols);
  **the ceiling**, `check_dotnet_packages.sh` fails a packed `Eiva.Fletcher.Interop` above
  12,000,000 bytes; **the consumer on packed packages for every pull request**, `detect-changes`
  gains a `consumer` output (the round's branch, or the `dotnet`, `gateway` or `protoc` filter),
  which runs those three lanes with their release artifacts (the input renamed `bundle` to
  `release-artifacts`); `preview-bundle` and `preview-consumer` become `package-bundle` and
  `package-consumer`, which on this branch keep the head-commit check and the 30-day
  `fletcher-preview-*` upload and elsewhere assemble the same folder for the run (one day,
  `ci-package-bundle-*`); the pack job now names the commit it checked out (`inputs.ref` or the
  event's), as on a non-bundle pull request that is the merge commit and the nuspec check would
  otherwise fail. Rehearsed in the devcontainer: exports unchanged, the stripped shim self-contained
  at the stated floors, `Fletcher.Tests` 423/423 on it, the packages checked, a ceiling mutant
  caught, the folder assembled naming its branch; actionlint clean.
- **9b-2 done on CI 2026-10-22** (`6ca8ed2` + `553cfb3`, `ci.pr` 39852936786, 60/60, every workflow
  green; `6ca8ed2` alone failed `ci.check-test-matrix`: `test_codec_edge` grew to 30 cases and the
  bucket 1 tables still said 23, fixed in `553cfb3`; D-BIND-156, widened by the maintainer to the
  C# gateway client): a null in a field the schema marks not nullable, where its parent value is
  present, is refused before anything is encoded, naming the field's path and (for a batch) the
  row. **Three codecs, one rule:** `c-abi`'s `BoundRows` refuses the whole batch at bind (so C#'s
  `Publisher` and `FletcherCodec`); `arrow-bridge`'s `Codec::EncodeRow` refuses the row before
  writing it (C++'s `PublisherArrow`), with a field-aware checker beside its type-driven encoders;
  `GatewayRowCodec.Bind` refuses the batch (the C# gateway client). Each walks values with its
  encoder's own index rules (`Physical` and offsets, sliced batches included); a null below a null
  parent is no value and is accepted; map keys keep their own refusal at encode (D-BIND-131); the
  common case costs a null count per field and no row walk. Tests: `c-abi` 7 (top level, a
  present struct's field, a null below a null struct, a list element, a map value, nullable nulls
  still encode, a slice's offset), `arrow-bridge` 7, `Fletcher.Tests` 5 (both codecs refuse the
  same four shapes and agree on a null below a null struct), `Fletcher.GatewayClient.Tests` 3.
  Rehearsed: `arrow-bridge` 125/125 and `c-abi` 96/96 built with their tests in the devcontainer,
  `Fletcher.Tests` 428/428 on both frameworks against the rebuilt shim, the gateway client 222/222
  on this host, clang-format-18 and `dotnet format` clean. The TypeScript gateway client is a
  BIND-T parity item.
- **9b-3 done on CI 2026-10-23** (`2420e11`, `ci.pr` 39860825935, 60/60, every workflow green on
  the first attempt; both legs' logs show both TFMs publishing and the `fastdds` row delivered on a
  transport thread after 2 to 3 publishes). **9b-3, 2026-10-22 (D-BIND-88, D-BIND-160): NativeAOT
  on Windows and over `fastdds`.**
  **Windows:** `ci.dotnet`'s Windows leg publishes `Fletcher.AotSmoke` with NativeAOT for net8.0
  and net10.0 (ILC, then the MSVC linker, which the ILCompiler package finds with vswhere) and runs
  it against the shim that leg built, warnings as errors as on Linux. Measuring it found **G-D2
  real for publishing:** the SDK picks a self-contained app's runtime pack at its own latest patch,
  and ILC refuses a `System.Private.CoreLib` newer than its own, so on any SDK but 10.0.400 (the
  Windows runner rolls forward) the publish failed. `Directory.Build.targets` now pins
  `RuntimeFrameworkVersion` to the AOT packs' versions for every `PublishAot` project. **A real
  transport (the maintainer's ruling: two processes, both legs):** `Fletcher.AotSmoke fastdds`
  relaunches itself as `fastdds-subscribe <topic>` on a GUID topic and republishes one row every
  100 ms until the child exits (30 s deadline; the child's is 35 s). Two processes because Fast
  DDS delivers a sample written in the same process on the writer's own thread. The child decodes
  through `FletcherCodec` and the generated `Reading.FromArrow`, as the generated subscriber does
  (the generated classes fix their topic), and exits 0 only on the expected row, delivered off its
  main thread. Rehearsed: on this Windows host (VS 18 MSVC, SDK 10.0.402, the bundle 338 shim) and
  in the devcontainer (SDK 10.0.400, gcc), both TFMs publish with no IL diagnostics, the inprocess
  smoke passes, and over `fastdds` the row arrives on a transport thread after 2 to 4 publishes;
  the solution builds clean with the pin, and `dotnet format` is clean. Two mutants are caught: the
  child on another topic (30 s deadline, 299 published, exit 1) and a different row (the child
  refuses it, exit 1).
- **9b-4 done on CI 2026-10-23** (`1087014`, `ci.pr` 39924157760, 60/60 on attempt 2, every
  workflow green; the lane's 11 tests passed first time on Windows and Linux). Attempt 1 failed one
  test 9b-4 does not touch: `Fletcher.Tests` `SubscriberArrowTests.ATimerHandlerThatCancelsDuringADeliveryDoesNotDeadlock`
  on Windows net8.0 ("the timeout flush never ran": a 20 ms batch timer not firing within 5 s),
  the same symptom as the watch-listed `OnlyDroppedRowsStillDeliverAnEmptyBatch` (BIND-9a's close).
  On the maintainer's ruling: the failed job re-run (green), the test added to PR #129's watch
  list, and, a second test of the class failing the same way, **an investigation item for 9b-7**:
  `SubscriberArrow`'s timeout flushes on the Windows runner under .NET 8 (thread-pool starvation
  the likeliest cause, several tests blocking pool threads on purpose).
- **9b-4, 2026-10-23 (D-BIND-161): the C#-native cross-language rows.** In BIND-8d's lane
  (`integration-tests/gateway-dotnet-crosslang`), on the maintainer's ruling, as five topics of
  `crosslang.proto` driven peer to peer: `NativeToCpp` and `CppToNative` (Fast DDS),
  `NativeToTs` and `TsToNative` (the C# native pair over Fast DDS, the TypeScript client through
  the gateway), and `NativeToNative`, to a second C# native peer (`--role secondary`, which
  receives only that topic; the first does not, so the rows cross processes). The test process
  sends `probe <topic>` until the receiving peer reports one, then `publish <topic>`, one retry
  as before (D-BIND-129, review B3); the receiving peer compares with its own transcription of the
  fixture. Peers now listen on more than one topic, so their lines name the topic
  (`<topic> RECV <seq> ...`) and their commands name one; the six gateway-client tests changed to
  match. Rehearsed in the devcontainer: the lane's 11 tests pass; two mutants are caught (the C#
  native peer sending each row under the next row's `seq`: the C++ and second C# peers report
  every field that differs; `NativeToCpp`'s publisher writing on `CsToCpp`: the C++ peer's rows
  arrive under the other topic and the test fails after its retry); clang-format, prettier,
  `dotnet format` and the test matrix clean. Windows is CI's (no local Conan cache for it).
- **9b-5 done on CI 2026-10-23** (`9d4640d` + the fix `89ecb87`, `ci.pr` 39938498743, 61/61 on
  the first attempt, every workflow green): `pack-nuget` packed and checked
  `Eiva.Fletcher.Protoc.0.6.0-pr.129.349`, the bundle carries it, and `package-consumer` ran both
  consumers (the Fletcher one through the package, `GrpcConsumer` through `Grpc.Tools`) on Ubuntu
  24.04 and Windows, .NET 8 and 10; the cross-language lane 11/11 on both runners.
- **9b-5, 2026-10-23 (D-BIND-71, D-BIND-158): `Eiva.Fletcher.Protoc`.** `protoc/nuget/` packs,
  from what `ci.protoc` already builds and nothing else: the two static plugins under
  `tools/linux-x64/` and `tools/win-x64/`, `include/fletcher/options.proto`, the plugin's
  `LICENSE`, `COPYING` and `THIRD-PARTY-LICENSES.txt`, a README, and `build/` props and targets.
  The props set `$(FletcherProtocPlugin)` (this machine's plugin; empty, with warning
  `FLETCHERPROTOC001`, off linux-x64 and win-x64) and `$(FletcherProtoInclude)`, each only if the
  project has not set it (the in-tree lanes keep setting the plugin from
  `FLETCHER_PROTOC_PLUGIN`, D-BIND-71's point 4). The targets `chmod +x` the plugin on Linux before
  `PrepareForBuild`, and in a project with `<Protobuf>` items put `$(FletcherProtoInclude)` in
  front of every item's `AdditionalImportDirs` and compile `options.proto` as a messages-only item
  rooted there (opt-out `FletcherCompileOptionsProto=false`). `options.proto` gains
  `option csharp_namespace = "Eiva.Fletcher.Protobuf"`. A development dependency (a reference is
  `PrivateAssets="all"` by default), no dependencies, nothing a project loads. **Packed** by a new
  `ci.protoc` job, `pack-nuget`, whenever the release artifacts are built, at the protoc
  component's version or (new input `package-version`) the preview bundle's, and checked by
  `scripts/check_protoc_package.sh` (both plugins byte for byte the release binaries, the tree's
  `options.proto`, the licences, exactly the two build files, no `lib/`, `ref/` or `runtimes/`,
  no dependencies, a development dependency naming its commit). `cd.protoc`'s NuGet push is
  9b-6's. **The bundle** holds it in `nuget/`, at the bundle's version (assembly checks that in a
  preview bundle; elsewhere it is the protoc component's own, so the consumer reads it off the
  file). **The consumer test** (`package-consumer`, Linux and Windows) now generates the
  Fletcher consumer's C# with the plugin the package locates (the README's snippets changed with
  it), and a second project, `GrpcConsumer`, is a standard gRPC project: `Grpc.Tools` 2.80.0 (its
  own protoc, 31.1) compiles a `.proto` that imports `fletcher/options.proto` and sets both
  options, and the program reads both back through `Eiva.Fletcher.Protobuf.OptionsExtensions`.
  **D-BIND-71's open points, settled:** `Google.Protobuf.Tools` 3.36.1 exposes protoc as
  `$(protoc_linux64)`, `$(protoc_windows64)` and the like, in its build targets (so after a
  project's own properties), and through `GeneratePathProperty` as `$(PkgGoogle_Protobuf_Tools)`,
  which the README uses; the plugin (protobuf 3.21.12) works with the protoc of
  `Google.Protobuf.Tools` 3.36.1 and of `Grpc.Tools` 2.80.0; the executable bit, measured: NuGet
  of SDK 10.0.400 extracts every file as `rwxr--r--`, so the plugin already runs for the user
  who restored it, and the `chmod` is for everyone else and for clients that extract without it.
  Rehearsed with a package packed from bundle 338's plugins: the package check passes; both
  consumers pass on this Windows host and in the devcontainer, .NET 8 and 10; mutants caught: no
  import folder (`reading.proto` cannot import `fletcher/options.proto`), no compiled
  `options.proto` (`Eiva.Fletcher.Protobuf` is missing); the no-`chmod` mutant survives, as
  measured above. protoc's own suite 195/195 and test_package 3/3 with the new `options.proto`;
  the README snippets, `dotnet format` and actionlint clean. **First CI run (`9d4640d`, `ci.pr`
  39933647250):** `pack-nuget` packed and checked the package (`0.6.0-pr.129.348`), but
  `package-consumer` failed on both runners: MSBuild could not import the packed targets, whose
  comment, edited after the rehearsal, said `rwxr--r--`, and XML forbids `--` in a comment. The
  comment is fixed and `check_protoc_package.sh` now parses both build files as XML (a mutant with
  the old comment is refused); both consumers re-rehearsed green on Windows and Linux. The same run
  also failed `CSharpNativeToCpp` once on Linux (no probe in two 20 s attempts, the Fast DDS
  "0 rows" kind), passing on Windows and in every earlier run: the new row's first such failure.
- **9b-6, 2026-10-23 (D-BIND-153, D-BIND-159, D-BIND-162 to 164): the release workflows.** Three
  rulings first, one at a time: the gate builds the gateway and protoc from the tag's tree (162),
  the feed push is skipped until the runner exists (163), the dry run is a dispatch on `main`
  (164). **`cd.dotnet.yml`** (new): on a `dotnet-v*` tag, `ci.dotnet`, `ci.gateway` and
  `ci.protoc` (release artifacts), then the gate, `ci.package-consumer.yml`; only then `release`
  (GitHub-hosted: the tag checked against `Directory.Build.props` by the new composite action
  `verify-tag-version-dotnet`, the four `.nupkg` and four `.snupkg` checked present and attached to
  the GitHub Release) and `feed` (`cd.nuget-feed.yml`, when `NUGET_EIVA_RUNNER` is `'true'`). A
  dispatch runs up to the gate. **`ci.package-consumer.yml`** (new): the package-bundle and
  package-consumer jobs, moved out of `ci.pr.yml` unchanged but for three inputs (`preview`,
  `version`, `head-sha`) in place of `detect-changes`' outputs; `ci.pr` calls it as one job
  (`pr_gate` lists it), and the preview bundle keeps its name. **`cd.nuget-feed.yml`** (new):
  `workflow_call` and `workflow_dispatch` with a tag; a GitHub-hosted job fails with the reason
  unless `NUGET_EIVA_RUNNER` is `'true'`, then the `[self-hosted, eiva-network]` job downloads the
  Release's `.nupkg` and `.snupkg` with `gh` and pushes them (`--skip-duplicate`, symbols to the
  feed's symbol endpoint). **`cd.protoc.yml`**: `ci.protoc` always with release artifacts (so a
  dispatch packs too), npm and the Release only on a tag, `Eiva.Fletcher.Protoc` checked present and
  attached to the Release, and the same `feed` job. **Also:** `.github/actionlint.yaml` declares
  the `eiva-network` label; `ci.action-lint` runs the tag check's test (seven cases and the
  repository's own props); `ci.pr`'s `dotnet` filter gains what assembles and tests the folder.
  Not exercisable before the merge: the Release, the feed and the dispatch (D-BIND-164); checked
  here: actionlint over every workflow and the tag check's test, both clean; the reused jobs run on
  this push. **First CI run (`c13c809`, `ci.pr` 39968536854):** the moved jobs green at once
  (`package-consumer / package-bundle`, both consumers on both runners), `ci.action-lint` with the
  tag check's test green; one failure, not 9b-6's: the cross-language lane's `CSharpNativeToCpp`
  on Linux again (no probe in two 20 s attempts), its second failure in three Linux runs
  (`9d4640d`, `c13c809`), always this row, never on Windows. **Investigated, on the maintainer's
  ruling (2026-10-23):** not reproduced locally in 30 runs in the devcontainer limited to two CPUs
  (15 of the row alone, 15 of the whole lane). It is the one new row whose subscriber (the C++
  peer, subscribed at the lane's start) exists in another process before its publisher (the C#
  native peer) declares the topic; the publisher writes its schema sample once and the C++
  side's schema reader matches it later, the late-joiner case the Fast DDS 3.4 data-sharing defect
  breaks, and a lost schema sample would explain why no probe ever arrives (rows are republished,
  the schema is not). Not proven: the C++ peer now prints Fast DDS's match and loss statuses as
  `STATUS` lines (a `FastDDSStatusListener`), and a failed delivery's message shows each peer's
  lines for the topic, so the next failure says whether the schema channel matched, whether a data
  reader followed, or whether samples were lost. **The next failure did (`fed14f7`, `ci.pr`
  39971702812, every other job and workflow green):** `STATUS gateway.crosslang/CrossLang/NativeToCpp
  schema reader matched 1 (1)` and nothing more: the schema channel matched and the schema sample
  never arrived, so no data reader was ever opened. That is Fast DDS 3.4.0's data-sharing reader
  defect dropping a late-joining reader's `TRANSIENT_LOCAL` backlog, here the one sample the schema
  channel holds, and it hangs any subscription that starts before a same-host publisher in another
  process declares its topic. **Fixed in the provider, on the maintainer's ruling (D-BIND-165):**
  the `__schema` reader turns data-sharing off (`MakeSchemaChannelReaderQos`, a unit test pins it,
  the provider README says why); the schema writer and the data endpoints keep `AUTOMATIC`. Local
  runs cannot show the fix (the failure never reproduced here); they show no regression, and the
  proof is the lane staying green on the Linux runner over several runs. **9b-6 done on CI
  2026-10-26** (`c13c809`, with `fed14f7`, `acdf706` and `53457c6`): `acdf706`'s `ci.pr`
  (40302936835) 61/61, and `53457c6` every workflow green on the first attempt (`ci.pr`
  40306845298, 61/61; `acdf706`'s one red, `ci.check-test-matrix`, was the provider's new test
  missing from bucket 4's count, fixed there). `CSharpNativeToCpp` passed on Linux in both runs
  since the fix, after three failures in four before it; it stays watched.
- **9b-7, part 1, 2026-10-26 (D-BIND-152, D-BIND-166): the licence research.** Read off Fast DDS
  3.4.0's source: it compiles in, outside the Conan graph, nlohmann/json 3.11.3 (MIT, discovery
  server database), filewatch (MIT, file watching), PEGTL 2.8.3 (MIT, SQL filters and the IDL
  parser; three headers Fast DDS updated say BSL-1.0) and Boost.Interprocess and Boost.Thread 1.73
  (BSL-1.0, the shared-memory transport); optionparser and android-ifaddrs are not in the library.
  Fast DDS, Fast CDR, Micro-CDR and Micro XRCE-DDS carry no `NOTICE`; nanoarrow 0.8.0 and flatcc do,
  and the vendored copies had dropped both. They had also dropped the `LICENSE` of flatcc's portable
  headers' folder (Apache-2.0, crediting the MathGeoLib author too), and four of those headers carry
  their own (`pstdint.h` BSD-3-Clause, three MIT); found while showing the maintainer the notice.
  **Built, on the maintainer's ruling:** nanoarrow's `NOTICE.txt`, flatcc's `NOTICE`, and
  `flatcc/portable/LICENSE` and `LICENSES.txt` beside nanoarrow's licence in `pubsub/` and `protoc/`
  (both recipes package them); `fastdds-pubsub-provider/third_party/fast-dds-3.4.0/` with the four
  libraries' texts, which the recipe packages as vendored entries (`via: fast-dds 3.4.0`; the
  deployer prints "Compiled in from source by: fast-dds 3.4.0 (texts packaged by
  fletcher-fastdds-pubsub-provider/…)") and without which it fails, so `FAST_DDS_VERSION` cannot
  move without a re-check; `scripts/check_third_party_notice.sh`, run on the shim's notice
  (`ci.dotnet`), the plugin's (`ci.protoc`) and, new, the gateway's on both legs (`ci.gateway`, the
  9a review's vendored-licence check). **M7 closed with it:** `ci.pr`'s `gateway` filter gains
  `core/`, `pubsub/` and `fastdds-pubsub-provider/`, and its `dotnet` filter those and
  `xrcedds-pubsub-provider/`, so a change that drops a vendored licence reruns the lanes that ship
  it. Rehearsed in the devcontainer: pubsub, the provider, protoc, the shim and the gateway rebuilt;
  the three notices written as CI writes them and all three checks pass (the shim's lists twelve
  libraries, the four new ones among them); mutants caught: a notice without flatcc's `NOTICE`, one without the portable
  headers' licences, and the provider recipe without its licence folder (fails `package()` naming the re-check);
  actionlint clean (it caught an apostrophe ending `ci.protoc`'s quoted script, fixed).
- **9b-7, part 2, 2026-10-26 (D-BIND-167 to 169): Fast DDS 3.4.0 to 3.4.3.** Raised by the
  maintainer: 3.4.1 to 3.4.3 fix about nineteen CVEs that the shim and the gateway would otherwise
  ship statically, and it does not fix the data-sharing defect (#6490 merged after 3.4.3, on a line
  now end of life), so D-BIND-165 and the gateway lane's retry stay. Measured: ConanCenter's 3.4.3
  recipe resolves the same graph as 3.4.0 (fast-cdr 2.3.0, foonathan-memory 0.7.3, asio 1.29.0,
  tinyxml2 10.0.0), so no other pin or notice entry moves; its `thirdparty/` is byte-identical for
  the four compiled-in libraries, used by the same `src/cpp` files, its `LICENSE` identical and no
  `NOTICE` (compared against eProsima's v3.4.3 tag), so the licence folder only renames to
  `fast-dds-3.4.3`. All five pins move (the provider's `FAST_DDS_VERSION`, its benchmarks,
  `fastdds-xrce-interop`, `pubsub-conformance`, `pubsub-transport-conformance`'s agent) and the
  forward-looking version lines with them; measurements that name 3.4.0 stay. **Two findings, each
  ruled:** 3.4.3 refuses an infinite reliable `max_blocking_time` (eProsima's "Fix several CVEs"),
  which broke the built-in `lossless` profile, now 30 s (D-BIND-168); and 3.4.3 is ConanCenter's
  prebuilt binary, built against an older glibc, so the shim's glibc floor drops to 2.34 and is
  restated (D-BIND-169; the C++ runtime keeps the distributions at Ubuntu 24.04 and later).
  Rehearsed in the devcontainer on 3.4.3: the provider's suite 133/133 (after D-BIND-168; 132/133
  before, `LosslessDeliversEverySampleInOrder` refused at `create_datawriter`); the shim, the gateway
  and the three notices rebuilt and checked (`fast-dds 3.4.3`, `filewatch as vendored by fast-dds
  3.4.3`); the stripped shim self-contained at glibc 2.34 and `GLIBCXX_3.4.32`; the cross-language
  lane 5 of 5 on two CPUs; the NativeAOT smoke over `inprocess` and `fastdds`; clang-format clean.
  The other Fast DDS lanes (conformance, XRCE interop, `gateway-fastdds-ts`) and Windows meet 3.4.3
  first on CI. **On CI:** `205abce`'s `ci.pr` (40334195069) passed every Fast DDS lane on 3.4.3,
  Windows included, the gateway notice check and the floor; its one red was the timer flake below.
- **9b-7, part 3, 2026-10-26 (D-BIND-170): the Windows .NET 8 timer flakes.** `205abce`'s `ci.pr`
  failed `dotnet / windows` on net8.0 `SubscriberArrowTests.OnlyDroppedRowsStillDeliverAnEmptyBatch`
  ("no report of the dropped row", the 100 ms flush not within 5 s; net10.0 428/428 in the same
  job), the test's second failure and the class's third. On the maintainer's ruling, investigated
  before any re-run. **The cause, reproduced:** the deadline is a `System.Threading.Timer` callback
  on the thread pool, whose minimum is the core count (four on the runner); the suite parks pool
  workers on purpose, so with four test classes running at once every worker can be blocked when a
  flush falls due, and the callback waits for the pool's starvation injection: 500 ms per thread
  on a quiet machine, (threads x 1 s) per thread once the machine's CPU passes 80 percent, which
  the Windows job reaches with four test processes on four cores (the net8.0 leg runs at that
  peak; both legs run on the same .NET 10 runtime). A probe on a 4-core pool with four workers
  blocked: 506 ms idle, 4061 ms saturated, 113 ms with a minimum of 32; the suite under eighteen
  busy processes with `DOTNET_PROCESSOR_COUNT=4`: 8 of 10 runs failed a timer test at 5.0 s (this
  one three times; `FlushesAtTheTimeout`, `UnsubscribingFromInsideATimeoutFlushIsSafe`,
  `ATimeoutFlushAndARowLimitFlushMayOverlap`, `UnsubscribeWaitsForATimerHandlerStillRunning`,
  `ATimerHandlerThatCancelsDuringADeliveryDoesNotDeadlock` the others), 15 of 15 green at 0.11 to
  0.16 s with the minimum raised. The passing net10.0 legs of the three failing CI runs show the
  100 ms case at 0.14, 0.52, 1.13 and 2.90 s, the same tail; a 20-core pool never reproduces it,
  which is why 92 local runs saw nothing. **Found beside it:** both target frameworks wrote
  `TestResults/<os>.trx`, so the passing net10.0 leg overwrote the failing net8.0 leg's file in
  every one of the three failures, and no failure's per-test timings ever reached the artifact.
  **Built:** `ThreadPoolMinThreads` 32 in `Fletcher.Tests` and `Fletcher.GatewayClient.Tests` (the
  gateway client's `AWindowFlushesWhenItsDeadlinePasses`, once red on the same runner, is the same
  mechanism; its 2 s window stays), `LogFilePrefix` on both `ci.dotnet` legs (`linux_net8.0_<stamp>.trx`
  and so on; the upload globs match as before). The library is unchanged; the 5 s waits stay.
  Rehearsed: both projects rebuilt, the knob in all four runtime configs; under the saturation that
  failed 8 of 10 before, `Fletcher.Tests` 5 of 5 and `Fletcher.GatewayClient.Tests` 3 of 3 green
  (the gateway case at its 2.0 s window); actionlint clean (it caught two apostrophes in the Linux
  leg's comment, inside its single-quoted script).
- **9b-7 parts 1 to 3 done on CI 2026-10-26** (`0847e07`, `ci.pr` 40343220117, 60/60 on attempt
  2, every side workflow green; bundle `fletcher-preview-0.6.0-pr.129.355`). The timer fix read on
  CI: every `dotnet` job green on the first attempt, the net8.0 Windows case in 0.12 s, and one trx
  per framework uploaded by both legs. The first attempt's one red was new and elsewhere:
  `integration-test-gateway-end-to-end / windows`, "a valid --provider-config document reaches the
  provider and the gateway starts", the gateway over Fast DDS exiting with 0xC0000005 before READY
  and writing nothing to stderr, the lane's first failure in 29 runs on this branch; on the
  maintainer's ruling re-run (green) and watched, to be investigated if it recurs (the repository
  knows that a stale shared-memory segment under `C:\ProgramData\eprosima\fastdds_interprocess`
  produces this crash at `create_participant`). `205abce`'s own run (40334195069) had every Fast
  DDS lane green on 3.4.3, Windows included; its one red was the timer flake part 3 fixed.
- **9b-7, part 4, 2026-10-26: review T10, closed by 9b-5.** The review's finding (2026-10-21) was
  that the bundle's loose `protoc/fletcher/options.proto` is never used by the consumer. Since 9b-5
  the `options.proto` a tester receives is the package's: `Eiva.Fletcher.Protoc` carries
  `include/fletcher/options.proto`, the Fletcher consumer refuses to build unless the package's
  `FletcherProtoInclude` holds it, and the gRPC consumer imports it from the package and reads both
  options back, on both runners and both frameworks in every `ci.pr` run (D-BIND-158). The loose
  copy in the bundle keeps one use, the outside-NuGet route of `README-preview.md`'s hand-run
  `protoc` command (`-I path/to/bundle/protoc`), which no test executes: that command is T12's
  item, so the loose copy's exercise belongs to T12, and the copy itself retires with the bundle at
  the merge. Noted for the release: `cd.protoc` attaches the two plugin binaries and the Protoc
  package, no loose `options.proto`; outside NuGet the file is in the package's `include/` and in
  the `fletcher-protoc` Conan package. No code changes.
- **9b-7, part 5, 2026-10-26: review T13 and M6, the plugin's notice.** The review found the
  plugin's one notice read off the Linux graph only, its content unasserted, the Windows graph
  never compared; and the notice reached neither the GitHub Release nor the npm shim (M6). The
  content assertion came with part 1 (`check_third_party_notice.sh` on `ci.protoc`: protobuf,
  zlib, nanoarrow and the Apache NOTICE files). **Built, on the maintainer's scope ruling:**
  `ci.protoc`'s Windows leg writes the notice through the same deployer off its own graph and runs
  the same check on every run, uploading it as `protoc-notice-windows` when release artifacts are
  built; `pack-nuget` then requires the two legs' notices to name the same libraries, the shim's
  comparison verbatim (`ci.dotnet`'s pack job), and ships the Linux one as before. `cd.protoc`
  attaches `LICENSE`, `COPYING` and `THIRD-PARTY-LICENSES.txt` to the Release beside the binaries
  (the asset check requires them), and stages the same three files into `protoc/npm` before
  `npm publish`, refusing to publish unless `npm pack --dry-run` lists all three; `package.json`'s
  `files` names them, the npm README says what they are, and `.gitignore` keeps the staged copies
  out of the tree. The Windows notice artifact is named outside the `fletcher-protoc-*` pattern
  the release downloads, so the two same-named files never collide. Rehearsed: actionlint clean
  on both workflows; in the devcontainer, the deployer off the cached `fletcher-protoc` writes the
  notice (nanoarrow 0.8.0, protobuf 3.21.12, zlib 1.3.2) and the check passes; the comparison
  catches a notice with zlib dropped from its list; the npm check passes with the three files
  staged (node 24.15.0, the tarball lists them) and refuses a `package.json` whose `files` omits
  the notice. Not rehearsed locally: the Windows leg's deployer run (no local MSVC build of the
  plugin); it is the shape `ci.dotnet`'s Windows leg runs on every push, and meets CI first.
- **9b-7 part 5 done on CI 2026-10-26** (`e5b56fa`, with `6063be6` and `e4653de`; `ci.pr`
  40361897100, 61/61 on the first attempt, every side workflow green; bundle
  `fletcher-preview-0.6.0-pr.129.356`). The Windows leg's deployer wrote its notice covering three
  packages and the check passed; `pack-nuget`'s comparison found the two legs' lists identical
  (nanoarrow 0.8.0, protobuf 3.21.12, zlib 1.3.2); the `protoc-notice-windows` artifact exists
  beside `fletcher-protoc-licences`. The Release and npm steps run only on a `protoc-v*` tag, so
  their first execution is the merge-time release (9b-8's dry run exercises the build and the
  comparison, never npm or the Release, D-BIND-164).
- **9b-7, part 6, 2026-10-27 (D-BIND-171): review T12, closed without a new test.** The review
  found neither the packages' relinking route nor `README-preview.md`'s outside-NuGet `protoc`
  command executed by any test (T10's loose `options.proto` folded in here). A test of both, read
  from the text a reader receives, was ruled, built and rehearsed (the `protoc` half green on Linux
  and Windows; the relinking route green from empty Conan homes on both, both frameworks on the
  rebuilt shim; seven mutants caught), then withdrawn on the maintainer's review: `ci.dotnet`
  already builds the shim the route's way on both platforms and runs the whole suite on it on every
  pull request that can change it, so the test added only the README's seven lines at the cost of a
  native build in the release gate; and `README-preview.md` and the loose `options.proto` retire at
  the merge. **Kept:** `dotnet/README.package.md`'s relinking route now names CMake 3.15 or newer and
  the profile's compiler (GCC 13; the Visual Studio 2022 C++ tools, MSVC 14.4x): the Windows
  rehearsal failed first for want of CMake, which it did not name. The consumer test's README notes
  that two runs on one network disturb each other's `fastdds` route (fixed domain), found when two
  rehearsal containers crossed and failed on duplicate rows. Rehearsal notes, for whoever runs the
  route by hand on a workstation with Visual Studio 2026: the 2022 toolset beside it needs
  `tools.microsoft.msbuild:vs_version=18` in Conan's global config, CMake on PATH (VS ships one),
  and a short work folder (a deep one hits MSBuild's path limit in a test package).
- **9b-7, part 7, 2026-10-27 (D-BIND-172): review T14, the floors on a minimal machine.** The
  review found the floors never tested on a machine without development tools (no Visual Studio,
  no compilers). Scoping it first showed the floors hold: the consumer built from bundle
  `pr.129.356`, run in Microsoft's .NET runtime images, passed every route on Ubuntu 24.04 (glibc
  2.39, `GLIBCXX_3.4.33`) on both frameworks, and was refused on Debian 12 (glibc 2.36, GCC 12's
  runtime). It also showed the loader's message for that refusal blaming the RID. **Ruled and
  built:** the floors are the two Conan profiles' (the maintainer's principle; glibc stays
  measured), stated so in the package, preview and gateway READMEs and `shim_floors.env`; the
  gateway's own glibc floor, 2.38 (four `__isoc23_strto*` symbols from code compiled in the
  devcontainer), is stated for the first time; `NativeLoader.LoadFromCandidates` tells a shim the OS
  refused (its reason, the profile's floor) from no shim at all (the RID message, kept), with three
  unit tests and `check_dotnet_packages.sh` requiring the loader to state the floors;
  `integration-tests/preview-bundle-consumer/minimal.sh`, run by `ci.package-consumer`'s Linux leg
  after `run.sh` over its work folder, runs that consumer at the floor (Ubuntu 24.04, both
  frameworks, all routes) and below it (Debian 12, must fail quoting the OS's
  `GLIBCXX_3.4.32' not found` and the floor). Windows keeps the exact `dumpbin` list, on the
  maintainer's ruling. **The code generator**, on the maintainer's ruling after comparing with
  `main`: its prebuilt binary has the same floors (glibc 2.34, `GLIBCXX_3.4.32`; the VC++
  redistributable on Windows), as on `main`, and no README said so; `protoc/README.md`, the npm
  and the Protoc NuGet READMEs now do, and the `ci.protoc` and `protoc-plugin-cli` comments that
  claimed no DLL dependencies are corrected. **Rehearsed:** the loader tests 11/11 on Windows on both frameworks against
  the CI-built shim; `minimal.sh` green over the devcontainer-built consumer with the fixed loader
  in place (built at the bundle's version), from a scratch copy that hands Docker Desktop a Windows
  path (Git Bash's `/c/...` is not mapped; on CI's Linux runner the script runs as committed);
  mutants caught: the old loader (the RID message), and a "below the floor" image that passes;
  the loader-floor guard catches a drifted number; `dotnet format`, actionlint and the README
  snippet check clean. **Done on CI:** `78ab5b2`, every workflow green on the first attempt
  (`ci.pr` 40504806485, 61/61, bundle `pr.129.358`); `minimal.sh`'s first CI run, over packages
  carrying the fixed loader, passed on `runtime:8.0-noble` and `runtime:10.0-noble` (the shim
  loaded from `runtimes/linux-x64/native/`) and was refused on `runtime:8.0-bookworm-slim` (glibc
  2.36) with the OS's reason and the floor in the message.
- **9b-7, part 8, 2026-10-27 (D-BIND-173): review L14, the shim's lockfile with the release.**
  The relinking route took the third-party packages from ConanCenter as it stands on the day
  (versions pinned, recipe revisions and build tools not). **Ruled (option A) and built:**
  `ci.dotnet` writes the lockfile of the graph each leg builds the shim from, third-party
  packages only, and checks it with `scripts/check_conan_lockfile.sh` (an empty Conan home, the
  five fletcher recipes exported, the shim's graph resolved from ConanCenter with
  `--lockfile-partial`: exactly the locked packages, revisions included; seconds, no build);
  the pack job puts `fletcher-shim-<rid>.conan.lock` beside the packages,
  `check_dotnet_packages.sh` requires both, `cd.dotnet` attaches them to the Release, and the
  package README's relinking route downloads and passes it. The gateway and the code generator
  get no lockfile, on the maintainer's ruling: they have no relinking route (standalone
  executables, linked into nobody's work) and link only permissively licensed third-party code,
  whose notice they already ship (D-BIND-173). **Rehearsed** in the devcontainer: the check
  green from an empty home (6 packages: asio, cmake, fast-cdr, fast-dds, foonathan-memory,
  tinyxml2) and five mutants caught; the README's route with the lockfile, from an empty home,
  built the shim with the locked `foonathan-memory` revision (2023, not ConanCenter's newest);
  actionlint, shellcheck and the README snippet check clean. **On CI** (`f99bbee`): the lockfile
  written and checked green on both legs, Windows included (the same six packages; CI resolved
  `foonathan-memory#5d07f0...`, ConanCenter's newest); `dotnet / pack` red: on Linux MSBuild
  stamped the README's `\` line continuations as `/` and the packed-README comparison failed all
  four packages. Fixed on the maintainer's ruling: single-line commands, no backslash in the
  README, the reason in `Directory.Build.targets` (D-BIND-173). **Done on CI:** `d52dd42`, every
  workflow green on the first attempt (`ci.pr` 40526778017, 61/61, bundle `pr.129.360`): the
  lockfile check green on both legs (the same six packages), the packed README of all four
  packages matching, both lockfiles beside the packages and in the bundle's `nuget/`, and both
  consumers (with `minimal.sh`) green on Linux and Windows.
- **9b-8, 2026-10-27 (D-BIND-174): the fresh-context review and the merge runbook.** On the
  maintainer's ruling (option A) 9b-8 is the review of BIND-9b and the runbook below; the
  `0.6.0-alpha` bump and the preview retirement are the last two commits before the merge, after
  BIND-T and BIND-10, and the first dry runs move to the runbook's first post-merge step.
  **Review** (`plans/reviews/BIND-9b-codereview.md`, `BIND-9b-conformance.md`, at `b23f472`; three
  fresh-context reviewers): every ruling 153 to 173 met in the tree (154 as amended by 169), no
  defect in the machinery or the tests, 4 BLOCKERs in shipped text (B1 the Protoc README's snippet
  fails as written, B2 "Windows NativeAOT is not yet exercised" in every package, B3 the relinking
  route's CMake floor is 3.20 not 3.15, B4 the npm README swaps `COPYING` and `LICENSE`) and 5
  questions (Q1 the LGPL analysis for the release and for code compiled into the consumer's binary,
  Q2 the feed runner and key reachable from pull-request code in a public repository, Q3 the Protoc
  package released without the consumer gate, Q4 where users get the packages after the
  retirement, Q5 the gate's non-preview path first running at the retirement). Two DEBT items
  are already folded into the runbook: the loose `options.proto` (M4) and the dry runs dispatched
  one after the other (M3). **Rulings:** Q1 (b), an additional permission under GPL-3.0 section 7
  for the managed assemblies compiled into an application, `options.proto`'s generated code and
  the generator's output (`LICENSE-EXCEPTION`, a draft until EIVA legal has reviewed it; its
  wiring gates the bump; D-BIND-175, `3c436ad`). Q2, recast by the maintainer's correction that
  the packages go to nuget.org (a public repository on GitHub-hosted runners cannot reach
  `nuget.eiva.com`): Trusted Publishing from a GitHub-hosted job in the environment `nuget-org`,
  no self-hosted runner, no feed workflow (D-BIND-176, `09294f9`; built: the `nuget-org` jobs of
  `cd.dotnet` and `cd.protoc`, `cd.nuget-feed.yml` and `.github/actionlint.yaml` deleted; the
  npm jobs in the environment `npm`, `439fe2c`). Q3 (b): `cd.protoc` passes the consumer gate
  before npm, the Release or nuget.org, building the dotnet packages and the gateway for it
  (D-BIND-177, `e660083`). Q4: "Getting the packages" in `dotnet/README.md` and a sentence in
  `README.package.md` step 2 and `protoc/nuget/README.md` (nuget.org; D-BIND-178, `1d2c8e9`).
  Q5 (A): the retirement commit's `package-consumer` and the dry runs on `main` rehearse the
  gate's non-preview path, written into runbook steps 2 and 4 (D-BIND-179, `37d5d9f`). All five
  questions are ruled. **Blockers fixed:** B1 the `Eiva.Fletcher.Protoc` README shows the consumer
  test's two csproj regions, checked by `ci.protoc`'s `pack-nuget`; B2 Windows NativeAOT is
  exercised, and both READMEs say so; B3 CMake 3.20 in the relinking route; B4 the npm README's
  licence files the right way round (`2f07789`). **A reported issue:** the C# preview named
  `nexus.topics.v1/LivePoints/Snapshot` where the released TypeScript plugin named
  `nexus/topics/v1/LivePoints/Snapshot`; on the maintainer's ruling every generator now splits the
  package on `.`, through one function, and the 2026-10-02 dotted form is superseded (D-BIND-180;
  the 0.6.0-alpha Release notes name who must rebuild, runbook step 7). **On CI:** the rulings'
  commits went green as they were pushed; the blockers' fixes and the topic change together at
  `9c9e2bc` (`ci.pr` 40702620300, 61/61 on the first attempt, every side workflow green, bundle
  `pr.129.362`). `84e7ed2` alone failed `ci.check-test-matrix` (the plans said `test_topic_path`
  2, the tree had 3), fixed by `9c9e2bc`, whose push cancelled that run's `ci.pr`. On CI for the
  first time and green: the README snippet check on `Eiva.Fletcher.Protoc`'s README, and
  slash-only topics in every lane that names one (the C#, C++ and TypeScript cross-language rows,
  both gateway end-to-end lanes, the coverage lane and the TypeScript client), on Linux and
  Windows.
- **BIND-9b CLOSED 2026-10-28.** On the branch, everything BIND-9b set out to build is built and
  green: the packages hardened and inside their budget, the floors stated and tested on a minimal
  machine, NativeAOT on both platforms and over Fast DDS, the C#-native cross-language rows,
  `Eiva.Fletcher.Protoc`, and the release workflows with their gates. What is left runs at the
  merge, by D-BIND-174, and is the runbook below: `LICENSE-EXCEPTION`'s wiring after EIVA legal
  (D-BIND-175), the `0.6.0-alpha` bump and the preview retirement as the last two commits after
  BIND-T and BIND-10, then the dry runs on `main`, the nuget.org and npm setup, and the tags.
  **The review's DEBT and NITs, by ID** (`plans/reviews/BIND-9b-codereview.md`): in the runbook,
  M3 and M4 (already) and L7 and M9 with the retirement commit, which removes what they name;
  fixed in the close, T7 (D-BIND-154 marked amended by 169, here and in the decisions); BIND-10's,
  T2 (the docs name a nested field one way, or the two codecs are aligned); carried as known, M5,
  T1, T3, T4, L8, L9, L10/T5 and the NITs M6, M7, M8, M10, M11, M12, T8, T9, T12, L-N.

**How test builds reach testers during development, and what changes at the merge** (recorded
2026-10-20 at the maintainer's request, D-BIND-137 to 139). Until PR #129 merges, the per-push
preview bundle is the ONLY way anything built here reaches a tester: a 30-day CI artifact, never
a tag, a release or a registry push. It is a development aid for this branch, not a release
channel. **BIND-9b's acceptance includes the merge-time release, prepared on the branch and run
from `main` after the merge:**

- **Version bump in lockstep to `0.6.0-alpha`** (D-BIND-138): every `conanfile.py` version and
  cross-reference pin, `gateway/VERSION`, the `package.json` files (`gateway-client-ts`,
  `protoc/npm`) and `dotnet/Directory.Build.props`, together, in the merge.
- **Every supported package published by its CD workflow from a tag on `main`**: `cd.dotnet.yml`
  (new in 9b) pushes the four NuGet packages and their `.snupkg` to nuget.org by Trusted
  Publishing (D-BIND-176; until then this bullet said `nuget.eiva.com` from a self-hosted runner)
  and attaches them to a GitHub Release; `cd.gateway.yml`, `cd.protoc.yml`, the
  npm client and the Conan components release `0.6.0-alpha` as they do today, `cd.protoc.yml`
  also publishing `Eiva.Fletcher.Protoc`. Each workflow's dry run is green before its tag.
- **The preview bundle retired:** the `ci.pr` bundle job and its "build dotnet, gateway and
  protoc on every push to this branch" condition are removed in the merge, so `main` keeps only
  the path-filtered CI; with them go the 30-day upload, `scripts/preview/`'s tester README and
  the tester note `dotnet/PREVIEW-TESTERS.md` (9a-3). The `package-bundle` and
  `package-consumer` jobs STAY: they test the packed packages on every pull request that can
  change them (D-BIND-159, wired in 9b-1).
- **Previews cleared before the first `-alpha`:** the remaining bundles deleted and testers told to
  clear their NuGet caches (D-BIND-138).

**The merge runbook (9b-8, D-BIND-174).** The bump and the retirement are the last two commits on
the branch, after BIND-T and BIND-10, each shown and pushed on its own and green on CI before the
merge; the rest runs from `main`. Taken at `b23f472`; re-check every list against the tree when it
runs, since BIND-T and BIND-10 land in between.

1. **The bump commit, `0.6.0-alpha` in lockstep** (D-BIND-138; the shared MAJOR.MINOR rule):
   - the recipes' `version` and every `fletcher-*` pin: `core`, `pubsub`, `arrow-bridge`,
     `pubsub-arrow`, `fastdds-pubsub-provider`, `xrcedds-pubsub-provider`, `protoc`, `c-abi`
     (shipped inside `Interop`, never tagged) and `integration-tests/copy-probe`; `gateway`
     pins by range and needs no edit;
   - `gateway/VERSION`, `gateway-client-ts/package.json`, `protoc/npm/package.json` (and their
     `package-lock.json` root entries, through `npm install --package-lock-only`);
   - `dotnet/Directory.Build.props` (`VersionPrefix` 0.6.0, `VersionSuffix` alpha), then the
     dotnet lock files that record the `Interop` project range (`[0.5.0-alpha, )` in
     `Fletcher.Tests`, `CopyOracle.Tests`, `AotSmoke`, `Benchmarks`), re-written by a
     `dotnet restore --force-evaluate`, never by hand;
   - the install snippets in `README.md` and the component READMEs (`core`, `pubsub`,
     `arrow-bridge`, `pubsub-arrow`, `fastdds-pubsub-provider`, `xrcedds-pubsub-provider`).
     **Left as they are:** the benchmark READMEs' recorded runs (they name the revision measured),
     the vendored code, and the third-party `0.5.x` in the two `Cargo.lock` files;
   - check: `git grep -nE "0\.5\.[0-9]+-alpha"` outside `plans/` finds only what is left on
     purpose.
2. **The retirement commit** (D-BIND-139, D-BIND-159):
   - `ci.pr`: the `bundle` step, its `bundle` and `bundle-version` outputs, the
     `bundle == 'true'` term in `consumer`, the `package-version` and `ref` inputs they feed to
     the `dotnet`, `protoc` and `gateway` calls, the `preview` and `version` inputs to
     `package-consumer`, and the `scripts/preview/**` filter line; the path filters then decide
     alone, as on `main`;
   - `ci.package-consumer.yml`: the `preview`, `version` and `head-sha` inputs and the 30-day
     upload; the version comes from the packed `Interop`, as on every other pull request today,
     and the 1-day hand-over artifact stays;
   - `scripts/assemble_preview_bundle.sh` STAYS (the package-bundle job assembles with it) but
     loses `README-preview.md` and its placeholder check, the loose `options.proto` (its copy and
     its `need` line; D-BIND-171; `run.sh` and `minimal.sh` never read it, and
     `ci.package-consumer.yml`'s `protoc/include/fletcher` sparse-checkout entry goes with it) and
     the `feature/csharp-bindings` default in `VERSION`, and is renamed with its callers if the
     word "preview" is to go; `scripts/preview/` and `dotnet/PREVIEW-TESTERS.md` are deleted;
     whether the `ref` and `package-version` inputs of `ci.dotnet`, `ci.protoc` and `ci.gateway`
     go with their callers is decided in the commit (review M9);
   - `integration-tests/preview-bundle-consumer/` STAYS (it is the consumer test, `minimal.sh`
     included); a rename is cosmetic and goes with the script's or not at all;
   - every document that points at the preview, the package README, `dotnet/README.md` and the
     workflow comments among them (`git grep -niE "preview bundle|PREVIEW-TESTERS|README-preview"`);
   - **the gate's first non-preview run** (review Q5, T10; D-BIND-179): this commit's own `ci.pr`
     run is the first to take `ci.package-consumer.yml`'s non-preview path (the version read off
     the packed `Interop`, `BUNDLE_PROTOC_VERSION_MUST_MATCH=0`, the 1-day artifact). The commit
     touches `dotnet/` and `ci.package-consumer.yml`, so the `dotnet` filter runs
     `package-consumer`; it must be green, and its job list read to confirm it ran, before the
     merge.
3. **Merge** PR #129 (out of draft, its body refreshed, the watch list read).
4. **Dry runs on `main`** (D-BIND-164): dispatch `cd.dotnet` and `cd.protoc`, one after the other
   (both call `ci.protoc`, `ci.dotnet` and `ci.gateway`, whose concurrency groups cancel a second
   run on the same ref, and both run the gate, D-BIND-177; review M3), both green and recorded
   in this tracker before any tag. They are the release gates' rehearsal (D-BIND-179): the same
   `ci.package-consumer.yml` path the tags take, on the commit they will take it on.
5. **Clear the previews** (D-BIND-138): delete the remaining `fletcher-preview-*` artifacts and tell
   the testers to clear their NuGet caches, because `0.6.0-pr.129.N` sorts above `0.6.0-alpha`.
6. **nuget.org and GitHub set up for the push** (D-BIND-176), before any tag, because the first
   push of a version is final: on nuget.org, EIVA's organization has the `Eiva.` ID prefix
   reserved and two Trusted Publishing policies, owned by the organization and created by a
   maintainer who stays a member (owner `eivacom`, repository `Fletcher`, workflow files
   `cd.dotnet.yml` and `cd.protoc.yml`, environment `nuget-org`, packages `Eiva.Fletcher*`); on
   GitHub, the environment `nuget-org` deployable from the `dotnet-v*` and `protoc-v*` tags only,
   with the variable `NUGET_USER` (that member's nuget.org profile name), the environment `npm`
   deployable from the `protoc-v*` and `gateway-client-v*` tags only, and a tag ruleset that
   lets only the maintainers create, move or delete release tags (`dotnet-v*`, `protoc-v*` and
   `gateway-client-v*` at least); on npmjs.com, the Trusted Publisher of
   `@eiva/protoc-gen-fletcher` and of `@eiva/fletcher-gateway-client` names the environment `npm`.
   GitHub creates a missing environment, unprotected, the first time a job names it, so the
   `npm` environment's tag rule is set before the next `protoc` or `gateway-client` tag.
7. **Tag `0.6.0-alpha`**, each tag on the merge commit and its workflow green before the next:
   `core`, `pubsub`, `arrow-bridge`, `pubsub-arrow`, `fastdds-pubsub-provider`,
   `xrcedds-pubsub-provider`, then `gateway`, `gateway-client`, `protoc` (also
   `Eiva.Fletcher.Protoc`, to nuget.org), and `dotnet` last (the four packages to nuget.org).
   Every CD builds from its own tree, so the order is for reading the results, not a build
   dependency. The nuget.org login and push run for the first time here; a failed `nuget-org`
   job is re-run after the cause is fixed (`--skip-duplicate`). The `protoc` and `dotnet`
   Release notes say that a generated topic is now one segment per package part (D-BIND-180):
   C++ applications generated from a dotted package with 0.5.x must be rebuilt with 0.6.0-alpha
   together, or they do not see each other. The `protoc` notes also say that TypeScript no longer
   emits a topic constant for a method whose input is a flatten wrapper or a recursive message
   (D-BIND-182): an app importing one stops compiling, as no C++ or C# code ever used that topic;
   and that a generated `.fletcher.ts` with a pub/sub method now holds a publisher and a
   subscriber that need `@eiva/fletcher-gateway-client` 0.6, so plugin and client move together
   (D-BIND-188). The `gateway-client` notes (D-BIND-188) say: `publish` and `encodePositional`
   now refuse a null or absent non-nullable field (every non-`optional` field of a generated
   proto3 schema; a repeated field must be `[]`, a map `new Map()`) and a null map key
   (D-BIND-156, D-BIND-189), where they used to send the row; a callback that throws or whose
   promise rejects, and a row that does not decode, now go to the new `onError` option, or
   `console.error`, instead of escaping as an uncaught exception (D-BIND-183, D-BIND-185);
   `unsubscribe` stops delivery at once, so a row in flight is no longer delivered (D-BIND-186);
   and the new `subscribeBatched`, `undecodableRows`, `MAX_BATCH_TIMEOUT_MS` and their types.

**Acceptance**

- Part 5 as written: `ci.c-abi.yml`, `ci.dotnet.yml`, `ci.format-check-cs.yml`,
  the three integration lanes, `cd.dotnet.yml` mirroring the Conan CD shape and
  publishing to nuget.org by Trusted Publishing from a GitHub-hosted `nuget-org` job
  (D-BIND-176, which superseded D-BIND-28's `nuget.eiva.com` and self-hosted runner),
  packages also attached to the GitHub Release, `verify-tag-version-dotnet`, no dist-tag step;
  `ci.pr.yml` with 3 filters, 5 caller jobs, `pr_gate` `needs:` **and** `results:`;
  `ci.license-headers.yml` denylist updated.
- **RID matrix** `win-x64` + `linux-x64` fanning in to a single `pack`
  (D-BIND-27); `linux-arm64` as the first addition when wanted.
- **Licence files**: `LICENSE` + graph-derived `THIRD-PARTY-LICENSES.txt` staged
  every run via the #127 deployer; pack fails if either is missing. The
  **asset-isolation check** for `GatewayClient` rides in the same step.
- The **packed-size budget** for the shim. **RULED 2026-10-22 (D-BIND-157): 12 MB packed for `Interop`,
  the Linux shim stripped, N-10 accepted.**
- **The glibc floor for `linux-x64`, stated here (D-BIND-41, moved from BIND-3).** **RULED 2026-10-22 (D-BIND-154): Ubuntu 24.04 and later only; the floor stays glibc 2.38 and `GLIBCXX_3.4.32`, asserted by D-BIND-149.** **Amended 2026-10-26 by D-BIND-169: the shim's stated glibc floor is 2.34 (Fast DDS 3.4.3's build); the gateway's stays 2.38; still Ubuntu 24.04 and later only.**
  The number is not guessed: `ci.c-abi.yml`'s Linux leg prints the highest
  `GLIBC_x.y` the built shim references, which is the oldest distribution that can
  load it, on every run. State THAT number in the package README, and decide
  whether it is low enough — if it is not, the answer is to build the shim on an
  older image, which is a packaging decision and belongs with the RID matrix
  rather than with the codec. **Why this is a NuGet problem and not a Conan one:**
  the C++ lanes ship Conan packages, so a consumer whose profile does not match
  rebuilds from source and the mismatch is self-healing; a `.so` under
  `runtimes/linux-x64/native/` is taken as-is or not at all, and .NET supports
  distributions older than this repository's build image — so a consumer can be
  fully supported by Microsoft and still fail at the first P/Invoke.
- **The MSVC CRT: dynamic or static, decided here (D-BIND-38, moved from BIND-3).** **RULED 2026-10-22 (D-BIND-155): dynamic, the 14.40+ redistributable a stated requirement.**
  The question is a packaging one — whether a consumer needs the VC++
  redistributable on the target machine — and it is settled with **evidence** about
  what the .NET runtime's own install already guarantees, not by assumption. Static
  costs a from-source rebuild of the shim's entire chain (the runtime is a Conan
  profile setting, so every static library inside the shim must match), grows the
  artifact against the size ceiling above, and means CRT security fixes require
  reshipping rather than flowing through Windows Update. It also reintroduces the
  two-heaps hazard between the shim and other native modules that BIND-2's B1
  found. Dynamic is the status quo and needs no work.
- **NativeAOT where BIND-6 did not reach (D-BIND-88, parked here from the BIND-6
  review, Q6).** **Met by 9b-3 (`2420e11`, green on CI 2026-10-23): both notes
  below.** D-BIND-81 publishes and runs `Fletcher.AotSmoke` on Linux over
  `inprocess` only. BIND-9 owns the two open notes 6e-2 left:
  1. **`win-x64`:** publish AotSmoke with NativeAOT (MSVC links) and run it against
     the Windows shim, in the RID matrix's Windows leg, for net8.0 and net10.0.
  2. **A real transport:** run AotSmoke (or a sibling) over `fastdds`, so a delivery
     enters AOT-compiled managed code on a Fast DDS thread, which until now has run
     only under the JIT. The test-body rules for Fast DDS apply (wait with a
     deadline, republish until the first row arrives, GUID topics).
- **A null in a non-nullable column, at the Arrow tier (parked here with D-BIND-92,
  review Q10a).** The generated `ToArrow` now refuses a caller-forced null, but a
  RecordBatch a caller builds by hand (the Arrow tier: `PublisherArrow`, the codec)
  is encoded as given in BOTH languages: no codec path checks a column's nullability,
  and C++'s generated decoder then reads a value that is not there. Decide here
  whether the codec refuses such a batch (in C++, for every binding) or the contract
  is documented as the caller's; BIND-10 documents whichever is ruled. **RULED 2026-10-22
  (D-BIND-156): the C++ codec refuses it, naming the column.**
- **Cross-language pub/sub, the C#-native rows (ruled 2026-10-14, at BIND-6's close).**
  **Where, RULED AGAIN 2026-10-23 (D-BIND-161): in `integration-tests/gateway-dotnet-crosslang`,
  BIND-8d's lane, as five peer-to-peer topics, not a lane of its own.** **Met by 9b-4 (`1087014`,
  green on CI 2026-10-23).** A lane of
  its own, on Linux and Windows, in which the GENERATED native pairs exchange rows over Fast DDS
  across processes: C# → C++ and C++ → C#; C# → TypeScript and TypeScript → C# through the gateway;
  C# → C# between two processes. One shared `.proto` reaching every mapped shape (scalars, lists,
  maps, nested and `optional` messages, Timestamp/Duration), compared field by field on the receiving
  side. The test-body rules for Fast DDS apply. The matrix and the reason for it are in BIND-6's open
  note (requested by the maintainer 2026-10-13).
- ~~The **self-hosted runner** registered, labelled, documented (what it runs, what it
  must have installed), and used by no other job than the NuGet publish jobs:
  `cd.dotnet.yml`'s and, since D-BIND-71, `cd.protoc.yml`'s. Both are tag-triggered
  and never run pull-request code.~~ **Struck 2026-10-28 by D-BIND-176:** there is no
  self-hosted runner. In its place, the nuget.org and GitHub setup of runbook step 6: the
  `nuget-org` environment admits only the release tags, and a tag ruleset limits who cuts them.
- **`Eiva.Fletcher.Protoc` (D-BIND-71)**, the C# locator for the plugin: the two
  plugin binaries bundled per RID and a `build/*.props` setting
  `$(FletcherProtocPlugin)`. Packed and published by `cd.protoc.yml` under
  `protoc-v`, beside the npm shim; its NuGet push is a tag-triggered publish job of
  its own on D-BIND-28's self-hosted runner. Settle here: how `Google.Protobuf.Tools` exposes
  protoc, and whether the package also carries `fletcher/options.proto` with an
  include property, as the C++ locator does. A consumer-shaped test builds a project
  from the packed package alone. **RULED 2026-10-22 (D-BIND-158): it carries `options.proto`
  and wires it into `Grpc.Tools` projects; no hard dependency on either tools package.** **Built,
  packed, checked and consumer-tested by 9b-5 (`89ecb87`, green on CI 2026-10-23), the three
  review points below included; its NuGet push from `cd.protoc` is 9b-6's.** **Three more, from Oliver Monberg-Jensen's review on
  ADO 18789 (comment 23089, 2026-09-21), each checked against the tree 2026-09-29:**
  * **The executable bit.** NuGet extraction is reported not to keep it, so on Linux
    the `build/*.props` makes the plugin executable before protoc runs. The
    consumer-shaped test runs on Linux, so it proves this either way.
  * **Which protoc.** The plugin links protobuf 3.21.12 statically
    (`protoc/conanfile.py`), and current `Google.Protobuf.Tools` ships a much newer
    protoc. The plugin protocol is stable across versions, but no CI run has combined
    the two. The consumer-shaped test runs against the protoc the recommended package
    ships. The plugin declares `FEATURE_PROTO3_OPTIONAL` only
    (`protoc/include/generator.hpp`), so a `.proto` using `edition = "2023"` is
    expected to be refused. The package README states that.
  * **Generated code and runtime stay in step** by the tree's versioning rule: equal
    `MAJOR.MINOR` means compatible, and PATCH floats per component. The plugin ships
    under `protoc-v` and the runtime under `dotnet-v`, so the test also builds the
    generated code against the runtime packages of the same `MAJOR.MINOR`, and a change
    to the generated surface bumps MINOR on both.
- The **LGPL relinking decision** from the maintainer (Q9) is due before the first
  *external* publication (D-BIND-28); the NuGet README states the relinking route
  and the NativeAOT static-linking caveat (N-8) regardless.
- `dotnet restore --locked-mode` with a committed `packages.lock.json`;
  `actions/*` pinned to the tags this repo already uses.

### BIND-10 — Docs and archive

**Acceptance**

- `dotnet/README.md`: build, test, consume, the three packages, TFM matrix, the
  single-copy rule, licence files.
- `c-abi/README.md`: the header's ownership rules, the codec surface, how a second
  binding consumes it.
- `README.md`: repository-layout table (+ `c-abi/`, `dotnet/` rows), mermaid
  dependency graph, generated-file table (+2 rows), releasing table (+ `dotnet-v`
  row), and a **"Consume in .NET"** quick-start.
- `CONTRIBUTING.md`: layout-table rows, **NuGet naming row**, license-header
  guidance for `.cs` / `.csproj` / `.slnx`.
- `docs/wire-format-specification.md`: a **"Proto to C# Type Mapping"** section
  beside the TypeScript one, including the lossless temporal rule.
- `docs/recordbatch-accessor-spec.md`: C# alongside C++ / Rust, incl. the
  `TryMake` deviation and the `StructArray` windowing finding.
- `docs/technology-decisions.md`: **TD-009 — the .NET binding** (TD-008 is the
  seam), recording Part 1's rationale.
- `integration-tests/pubsub-conformance/README.md`: the client half's "stand-in"
  caveat retired.
- **The cross-language pub/sub matrix (ruled 2026-10-14):** every publisher → subscriber pair of
  languages with a pub/sub surface, the lane that proves each, documented beside the language
  support table.
- Round archived to `docs/archive/BIND/`.

---

## Part 8 — Risks, and what is still open

The full register (32 items after BIND-0 added N-10, five groups) is the
development plan §6, and each item names the round item that closes it. The ones
worth reading first:

| Risk | Mitigation |
|---|---|
| **B-1 — there is no codec to wrap.** BIND-2 builds the nanoarrow codec; roughly the size of the 530-line Arrow C++ one | Byte identity against `arrow-bridge` across the corpus is the forcing test; prior art in commit `0050365`. |
| **B-2 — per-row publish pays for a one-row Arrow array**, and the copy oracle cannot see allocations | The bind step makes batching structural; BIND-3 benchmarks per-row against the C++ generated publisher; if unacceptable, the only faster route is a D-BIND-1 STOP-AND-ASK. |
| **B-3/Q3 — the shim's size and Fast DDS's shared-memory directories** on the consumer's machine | **Measured at BIND-0: `win-x64` 7.61 MiB** (Release, all three built-ins statically linked), reported into the job summary on every run; `linux-x64` from the first lane run. No threshold enforced until BIND-9 sets one against these numbers. The gateway is the static-linking precedent; stated in the README. |
| **N-5 — two `release` protocols** (C Data Interface vs owner-handle retain/release) | Received schemas deep-copied natively before import; `BoundRows.Dispose` unbinds before releasing; tested with a schema received twice. |
| **S-2 — the re-entrant `Unsubscribe` carve-out** and sibling handlers | The in-flight counter (D-BIND-18); a test mirroring `CallerTier.CancellingASiblingRunningOnAnotherThreadKeepsItPublished`. **Amended 2026-09-25 (D-BIND-50):** the counter alone did not cover a sibling between the gate and its increment, so a carve-out cancel now frees only after a second cancel has waited for the drain. |
| **N-3 — `async` handlers** whose continuation outlives the borrowed arguments | Span/ref-struct delegate signature so an `async` lambda cannot compile; documented. |
| **P-1 — LGPL relinking** | Owner: the maintainer (Q9); the notice half is mechanised by #127. **Ruled for the preview 2026-10-20 (D-BIND-140):** the shared-shim relinking route. |
| **P-7 — `protoc/` and `arrow-bridge/` contended** with PR #128 (FastDDS modernization) | **Ruled 2026-09-15 (D-BIND-29): BIND does not wait.** Re-derived against the branch as it stands: the `protoc` collision has shrunk to +416/−56 (its work was rebased onto the IR), `SubscribeSchema` now returns a `SchemaArrival` rather than a `shared_future`, and the live collision is instead #128's version bump to `0.5.1-alpha` against `c-abi`'s exact pins. #129 lands first; BIND-1 specs the schema-watch pair as `kNotSupported` (superseded 2026-09-25 by D-BIND-52: the shim forwards to the seam's pair); the order is re-examined at the BIND-1 → BIND-2 boundary. The no-drift test, `TsVisitor.DescriptorByteIdentical` and BIND-2's byte-identity oracle prove neither party moved wire bytes. |
| **N-7 — runtime `DllNotFoundException`** on a missing RID or an older glibc | `SetDllImportResolver` naming the RID; the ABI version handshake; the glibc floor stated. |

### Open items

**Found at BIND-0, carried to BIND-9 — the shim's Windows export table.** Symbol
visibility is hidden and, on Linux, `--exclude-libs ALL` localises everything that
arrived from a static archive, so there the shim exports exactly what `binding.h`
declares and `ci.c-abi.yml` asserts it. **Windows exports 3531 names**: ConanCenter's
`fast-dds` static build is compiled with `EPROSIMA_USER_DLL_EXPORT`, so
`libfastdds-3.4.lib` carries 303003 `/EXPORT` directives that the linker honours when
it pulls those objects into a DLL — and a module-definition file does **not** suppress
them (MSVC merges `/DEF` exports with `__declspec(dllexport)` ones; measured both
ways). Nothing resolves incorrectly because of it — Windows binds imports per module,
so there is no ELF-style interposition, and P/Invoke finds `fl_binding_abi_version` by
name — but the export table is bloat the packed-size budget pays for, and the "exports
only what the header declares" property is currently Linux-only. Fixing it means
changing how the dependency is built, which is a BIND-9 question with the size budget
in hand, not a BIND-0 one.

**All three of BIND-2's decisions are now closed.** Two were raised by BIND-2b and
ruled at BIND-2's conformance review on 2026-09-18 (detail in BIND-2's state
table): the **composite map-key refusal** is locked as D-BIND-36 (map keys are
scalar only — a statement about the wire format's reach, which BIND-Rust
inherits), and **byte identity does not widen to the `emit_vectors` corpus** —
D-BIND-35 amends the acceptance instead, because that corpus is proto-row shaped
and narrower than the fixtures built. The third was raised by BIND-2c and is a
question about the HEADER rather than about any implementation — **who frees a
message a callback put in an `fl_error`** — settled at BIND-1's spec review as
D-BIND-32, where it is recorded in full.

**What BIND-2's reviews leave open** is one BLOCKER, and it is in test code:
`test_containment.cpp` fills an `fl_error` through the object library's copy of
the containment site (`new[]` in the test binary) and releases it with the shim's
exported `fl_error_dispose` (`delete[]` in the shim). One heap today, two the
moment the shim stops sharing a runtime with the test binary — which is D-BIND-32's own argument,
mirrored. Two lines to fix, and it belongs to BIND-2 rather than to BIND-3.

Three actions remain besides, none an engineering one:

1. **LGPL relinking decision** (owner: the maintainer): **ruled for the preview 2026-10-20
   (D-BIND-140)**, since the repository is public and BIND-9a's per-push bundle is already
   external (D-BIND-139).
2. ~~**The self-hosted publish runner** in the EIVA network and the `NUGET_EIVA_API_KEY`
   secret on `nuget.eiva.com` (user actions outside a PR, D-BIND-28). NuGet.org Trusted
   Publishing and the `Eiva.Fletcher.*` prefix are deferred until the packages go external.~~
   **Replaced 2026-10-28 by D-BIND-176:** nuget.org by Trusted Publishing; the prefix
   reservation, the two policies, the environment and the tag ruleset are runbook step 6.
3. **ADO**: 18689 pointed at the committed plan; 16353's accessor wording corrected.

---

## Definition of done (round)

- BIND-0 … BIND-10 and BIND-T green, reviewed, logged in
  `plans/BIND-progress-log.md`, pushed.
- **One wire format**: byte identity between the nanoarrow codec and `arrow-bridge`
  across the full scenario corpus; no managed code writes wire bytes except
  `GatewayClient`.
- **Zero-copy proven from C#**: the copy oracle green with the C# producer for rows
  and attachments, the UTF-16↔UTF-8 boundary stated in its scope note.
- **Generic transport proven**: `fastdds` and `xrce` reachable from C# by selector
  with no per-transport C# code; a path selector refuses with `kNotSupported` until
  PDA-ABI installs a resolver.
- **Single copy enforced**: the shim refuses to load beside a second marker.
- Every seam-inherited obligation (constraints 1–8, the two mapping details) has a
  named test in the C# suite, and the C# `CallerTier` arm maps totally onto the C++
  one.
- **Buckets 1–4 green in C#** — the FILE SETS named in Part 4's matrix, not a count,
  for the reason BIND-3's, BIND-4's and BIND-8's bullets give: a number here is
  invalidated by any rebase that adds a case upstream (BIND-0 review, F1), and this
  bullet carried a wrong one for seven weeks. As re-derived 2026-09-18 that is **278
  of 302**, the 302 being buckets 1–4 and the 24 provider-internal cases the only
  subtraction. Both exclusion classes documented; 18786 not closed before Bucket 4.
- Generator: every plugin output has a C# counterpart (row, Arrow view, accessor);
  TS gains `Publisher`/`Subscriber`; `TsVisitor.DescriptorByteIdentical` still
  green; the no-drift test proves no existing output byte changed.
- **Wire bytes unchanged** (GIR-2).
- `GatewayClient` verified to carry **no** native assets; the shim within its size
  budget; both licence files in the package.
- Docs updated incl. TD-009; round archived to `docs/archive/BIND/`.
- **Released properly after the merge** (2026-10-20, BIND-9b): every package at `0.6.0-alpha`,
  bumped in lockstep; each supported package published by its CD workflow from `main` (the NuGet
  packages to nuget.org, D-BIND-176); the per-push preview bundle job retired and the preview
  bundles deleted.
