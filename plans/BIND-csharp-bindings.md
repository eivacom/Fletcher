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

All thirteen ruled. ✅ = settled.

| # | Decision | Status | Needs |
|---|---|---|---|
| 1 | **C# calls the existing C++ encode/decode** rather than reimplementing the codec | ✅ **agreed 2026-08-31** | — |
| 2 | **Native protocols come from the seam's registry**; BIND binds `PubSubProvider` once. The PDA gate was discharged by #126; the shim links and registers `inprocess` + `fastdds` + `xrce` | ✅ **agreed 2026-08-31**; gate discharged and built-ins ruled **2026-09-11** (Q3) | — |
| 3 | **TypeScript scope** = generated `Publisher`/`Subscriber` over the gateway (not a Node native binding) | ✅ **agreed 2026-08-31** | — |
| 4 | **ABI value-transfer = Arrow C Data Interface** (`ArrowArray` + `ArrowSchema`), with the three-layer encode surface (D-BIND-23) | ✅ **agreed 2026-09-11** (Q1) | — |
| 5 | **`Apache.Arrow` as a base C# dependency**; collapse `Core` + `Arrow` into one package | ✅ **agreed 2026-09-11** (rider on #4); collapse agreed with Q2: three packages | — |
| 6 | **Native artifact links nanoarrow only**, not Arrow C++ (package size) | ✅ **agreed 2026-09-11** (rider on #4) | — |
| 7 | **Accessor nesting depth**: match RBA's 2/3 cap | ✅ **agreed 2026-09-11** (Q6): match the cap; the asymmetry is RIR's to remove | — |
| 8 | Does **"all tests" (18786)** include the 98 protoc generator tests? | ✅ **agreed 2026-09-11** (Q10): no — generator tests stay C++ and are extended; provider-internal cases (24) also excluded, documented; 275 of 300 non-generator cases port | — |
| 9 | **"Make C# emit Rust-native accessor classes" (16353)** — confirm the reading | ✅ **agreed 2026-09-11** (Q19): read as *C# accessor classes equivalent to the Rust-native ones*; BIND-7 scope; ADO 16353 wording to be corrected | — |
| 10 | Is **BIND-Rust** in this round or the next? | ✅ **agreed 2026-09-11** (Q12): **next round** | — |
| 11 | **RID matrix** for `Eiva.Fletcher.Interop` | ✅ **agreed 2026-09-11** (Q8): `win-x64`, `linux-x64` at first release; `linux-arm64` first addition; macOS on demand | — |
| 12 | **LGPL relinking review** for shipping prebuilt native binaries | ✅ **owner assigned 2026-09-11** (Q9): the maintainer; decision due before BIND-9. The notice half is mechanised by #127's deployer | maintainer |
| 13 | **Component granularity & versioning** (one `dotnet/` component, `dotnet-v` tag, **`0.5.x`** series, **three** managed packages) | ✅ **agreed 2026-09-11** (Q2) | — |

Nothing gates kickoff. The one open non-engineering item is the LGPL relinking
decision (#12), which gates BIND-9's packaging design only.

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
| 18789 | plugin generates C# for **all** its outputs | BIND-6 (row), BIND-7 (Arrow view + accessor) |
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
- Lands as **several PRs**, one per stage boundary. Each must be independently
  green.
- No PR until its stage is green and reviewed; PR/merge is the **user's** step.
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

---

## Part 3 — Generator work, on the IR

GIR makes this the cheapest part of the round. The C# backend is a **third
backend pair**, mirroring the TS pair exactly:

| New file | Mirrors | Contents |
|---|---|---|
| `protoc/include/csharp_backend_type_table.hpp` + `.cpp` | `ts_backend_type_table.*` | The **only** place a C# type string may live. Keyed on `ir::LogicalType` + optional `ir::EnumIdentity` → `{cs_type_text, arrow_builder, …}`. Temporal types map **losslessly** (D-BIND-26). |
| `protoc/include/csharp_backend_visitor.hpp` + `.cpp` | `ts_backend_visitor.*` | Recursive IR visitor emitting the row POCO, a static `Apache.Arrow.Schema`, `ToArrow(IEnumerable<T>)` / `FromArrow(StructArray, int)`, and the typed `<Service>_<Method>Publisher`/`Subscriber` pair. **No wire bytes**: no `WriteTo`/`ReadFrom`. |
| `protoc/include/csharp_backend_view_visitor.hpp` + `.cpp` | `cpp_backend_view_visitor.*` | `<stem>.fletcher.arrow.cs` — typed row view over `(StructArray, int)` (BIND-7). |
| `protoc/include/csharp_backend_accessor_visitor.hpp` + `.cpp` | the IR, not `FieldKind` | `<stem>.fletcher.accessor.cs` — column-oriented `<Class>Accessor` with `TryMake` (BIND-7). |

Hard rules, straight from GIR and the rulings:

- **No C# type string on an IR node** (GIR-1). It goes in the type table or nowhere.
- **Reuse `cpp_backend::BuildFlattenedFieldList`** for the message walk, exactly as
  `TsVisitor` does, so the C# field set cannot drift from the schema field set.
- **Wire bytes unchanged** (GIR-2). Generated source bytes are new files, so there
  is no golden to re-baseline — but the no-drift test must prove the *existing*
  outputs are untouched when the new tokens are passed.
- **Enums**: a **real `enum`** with the proto symbols, int32-backed (GIR-7,
  D-BIND-8).
- **Temporal values are lossless** (D-BIND-26): `Timestamp` and `Duration` map to
  `long`-backed structs carrying the unit, with `ToDateTimeOffset()` /
  `ToTimeSpan()` as explicit conversions; `DateTime` ticks are 100 ns and would
  silently lose two digits of a nanosecond timestamp.
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
development plan §5). 18786 requires "C# versions of all tests as CI tests". Because
there is **one** codec, the ported tests are **conformance tests of the binding**,
not parity tests between two implementations.

| Bucket | Files (cases) | Total | Treatment (ruled 2026-09-11, Q10) |
|---|---|---|---|
| 1 codec / envelope / buffer / status | `test_positional_io` 19, `test_envelope` 11, `test_write_buffer` 11, `test_status_taxonomy` 3, `test_codec` 35, `test_codec_edge` 23, `test_codec_property_fuzz` 2 | **104** | Port as binding conformance over the ABI (BIND-3). `test_status_taxonomy` ports as "the C# enum matches `core/README.md`'s published table". |
| 2 gateway client | `test_schema_codec` 11, `test_publish_frame` 9, TS 36 | **56** | Port (BIND-8), true parity: managed code both sides. |
| 3 pub/sub semantics | `test_publisher_subscriber` 18, `test_segments` 5, `test_pubsub_arrow` 15 | **38** | Port over `inprocess` (BIND-4/5). |
| 4 native providers | Fast DDS: `test_fast_dds_pubsub_provider` 40, `test_profile_document` 23; XRCE: `test_xrce_provider` 6, `test_xrce_document` 9 — **port as driver-selection tests (78)**; `test_fletcher_sample_pub_sub_type` 24 — **excluded**, provider-internal (the Fast DDS `TypeSupport`, a C++ type no managed code touches) | **102** | 78 port; 24 excluded, documented. |
| 5 generator | `test_type_mapper` 36, `test_option_metadata` 33, `test_schema_builder` 9, `test_schema_visitor` 9, `test_ir` 8, `test_schema_codec_lockstep` 2, `test_ts_visitor` 1 | **98** | **Stay in C++** — they test a C++ generator; **extended** with `test_csharp_visitor`, `test_csharp_type_table` and the TS pub/sub emitter cases. |
| 6 no managed analogue | `test_owned_schema` 1 | **1** | Excluded, documented (an `ArrowSchemaDeepCopy` ENOMEM path). |
| Conformance (inherited, not a port target) | `integration-tests/pubsub-conformance` 80 cases; `CallerTier` 20 of them | — | The **oracle** seam §9 hands BIND. BIND writes a C# arm of `CallerTier` and adds cases to the C++ suite (§12.1 expects it); each C# case names the C++ case it mirrors and a script checks the mapping is total. |

**275 of 300 non-generator cases port to C#**; the 24 provider-internal cases and
`test_owned_schema` are the two documented exclusions; the 98 generator cases stay in
C++ and grow. **18786 is not closed before Bucket 4 is green over `fastdds` and
`xrce`.**

### What replaces the byte-parity harness

With one codec there is no second implementation to drift, so the harness is
**repurposed, not deleted**:

| Test | Proves |
|---|---|
| `c-abi/tests` — `NanoarrowCodec.ByteIdenticalToArrowBridge` | The nanoarrow codec produces the bytes `arrow-bridge`'s `Codec` produces across the whole `emit_vectors` scenario corpus: **one wire format**. |
| `integration-tests/binding-abi-conformance` | The C# wrapper round-trips **every** scenario through the ABI with the values C++ produces, reusing the existing corpus rather than a C#-only fixture. |
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
| `ci.integration-test.protoc-dotnet.yml` | `workflow_call` | `conan create protoc/.`, locate the binary in the Conan cache, export `FLETCHER_PROTOC_PLUGIN`, `dotnet test`. **Copy the discovery block from the Rust workflow verbatim** — the `find ~/.conan2/p -path '*/p/bin/fletcher-protoc' -type f -print -quit` + explicit-error-if-empty idiom is already hardened for both platforms. |
| `ci.integration-test.gateway-dotnet-end-to-end.yml` | `workflow_call` | Gateway exe + C# client; mirrors `ci.integration-test.gateway-end-to-end.yml`. |
| `ci.integration-test.gateway-fastdds-dotnet.yml` | `workflow_call` | Bucket-4 over `fastdds` and `xrce` by selector. Linux-only initially, matching the other FastDDS integration tests. |
| `cd.dotnet.yml` | `push` tag `dotnet-v[0-9]*.[0-9]*.[0-9]*` | `setup-devcontainer` → `ci.dotnet.yml` → `publish` → `create-release`, structurally identical to `cd.gateway-client.yml`. |

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

Three mechanical edits, all following the established pattern:

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
devcontainer image.

### CD — publishing to NuGet.org

Use **Trusted Publishing (OIDC)**, the direct analogue of the npm `--provenance`
flow already in `cd.gateway-client.yml`. No long-lived secret:

```yaml
publish:
  needs: build-and-test
  runs-on: ubuntu-latest
  permissions:
    id-token: write        # REQUIRED — without it the OIDC request fails silently
    contents: read
  steps:
    - uses: actions/checkout@v6
      with:
        sparse-checkout: |
          dotnet
          .github
    - uses: ./.github/actions/verify-tag-version-dotnet   # new composite action
      with:
        props-file: dotnet/Directory.Build.props
        tag-prefix: dotnet-v
    - uses: actions/download-artifact@v8.0.1
      with:
        name: dotnet-packages
        path: artifacts
    - uses: NuGet/login@v1
      id: login
      with:
        user: ${{ vars.NUGET_USER }}
    - run: >-
        dotnet nuget push 'artifacts/*.nupkg'
        --source https://api.nuget.org/v3/index.json
        --api-key ${{ steps.login.outputs.NUGET_API_KEY }}
        --skip-duplicate
```

One-time setup (**user action** — cannot be done from a PR): configure the
Trusted Publishing policy on NuGet.org for `eivacom/Fletcher` + workflow
`cd.dotnet.yml`, and reserve the `Eiva.Fletcher.*` package-ID prefix. Fallback if
Trusted Publishing is not viable for the org: a `NUGET_API_KEY` secret with the
same `dotnet nuget push`.

Also in `cd.dotnet.yml`:

- **Pre-releases need no dist-tag equivalent.** NuGet has no dist-tags; SemVer 2.0
  pre-release suffixes (`0.5.0-alpha`) are *inherently* opt-in on NuGet (excluded
  from resolution unless the consumer asks for pre-release), so the
  `Resolve dist-tag` step from the npm workflow has **no counterpart** and must
  not be transliterated.
- `ContinuousIntegrationBuild=true`, `Deterministic=true`, SourceLink, and
  `.snupkg` symbol packages pushed to the symbol server.
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
Track D (pipelines, docs)         BIND-9 (starts with BIND-0, closes last) · BIND-10
```

Critical path: **BIND-0 → BIND-1 → BIND-2 → BIND-3 → BIND-4**. BIND-2 is the long
pole. `protoc/` and `arrow-bridge/` are contended with the modernization branch's
pending commits (development plan P-7); the landing order is agreed at BIND-0.

Status: ⚪ not-started · 🔴 in-progress · 🟢 done (forcing test green + reviewed)
Kind: 🟪 spec · 🟦 impl · 🔬 proof · ⚙ pipelines · 📓 docs

| ID | Item | Track | Kind | Depends on | Forcing test | Status |
|---|---|---|---|---|---|---|
| BIND-0 | Kickoff: decisions recorded, skeleton `c-abi/` + `dotnet/` green in CI on an empty ABI, matrix committed | A/D | 🟪 | — | `ci.dotnet.yml` + `ci.c-abi.yml` green on both platforms | ⚪ |
| BIND-1 | The binding ABI header, reviewed as a specification (no implementation) | A | 🟪 | BIND-0 | `BindingAbi.CompilesAsC99AndIsSelfContained` | ⚪ |
| BIND-2 | Nanoarrow schema-driven codec + publish fusion + the oracle's ABI producer | A | 🟦 | BIND-1 | `NanoarrowCodec.ByteIdenticalToArrowBridge` + `CopyAccounting.BindingProducerWritesInPlace` | ⚪ |
| BIND-3 | `Eiva.Fletcher.Interop` + codec/Arrow tier in `Eiva.Fletcher` | A | 🟦 | BIND-2 | Bucket 1 green in C#; `Errors.EveryHardCaseKeepsItsMessage`; per-row publish benchmark recorded | ⚪ |
| BIND-4 | Pub/sub in `Eiva.Fletcher`: registry, `Publisher`, `Subscriber`, `SchemaArrival`, thunk discipline, error handling | A | 🟦 | BIND-3 | Bucket 3 over `inprocess`; Bucket 4 over `fastdds`/`xrce` by selector; C# arm of `CallerTier` | ⚪ |
| BIND-5 | `SubscriberArrow` batch-first + the copy oracle end-to-end from C# | A | 🔬 | BIND-4 | `pubsub-arrow` cases; copy oracle green with the **C#** producer | ⚪ |
| BIND-6 | C# backend on the IR: type table + visitor → `<stem>.fletcher.cs` | B | 🟦 | — (GIR) | `CsharpVisitor.*` in `protoc/tests`; no-drift test unchanged | ⚪ |
| BIND-7 | Arrow view + accessor emitters (`csharp_accessor`) + capstone third arm | B | 🟦 | BIND-6, BIND-3 | `accessor-capstone` C# arm `observed == expected`; `StructArray` windowing fixture at non-zero offset | ⚪ |
| BIND-T | TS `Publisher`/`Subscriber` emitter | C | 🟦 | — | `TsVisitor.DescriptorByteIdentical` still green + new emitter cases | ⚪ |
| BIND-8 | `Eiva.Fletcher.GatewayClient` (managed port; the codec exception) | C | 🟦 | — | Bucket 2 (56) green; `Package.GatewayClientHasNoRuntimesFolder` | ⚪ |
| BIND-9 | CI/CD: RID matrix, NuGet Trusted Publishing, size budget, licence files | D | ⚙ | BIND-0 (skeleton), all for release | `cd.dotnet.yml` dry run; packed-size check; asset-isolation + licence check | ⚪ |
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
- NuGet.org Trusted Publishing policy and the `Eiva.Fletcher.*` prefix requested
  (user action).

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
- **Byte identity**: `NanoarrowCodec.ByteIdenticalToArrowBridge` across the whole
  `emit_vectors` scenario corpus (D-BIND-11), against whichever `arrow-bridge`
  `Codec` `main` carries when it first runs.
- **The borrow rule is tested**: an array's `release` count stays zero across N
  publishes and is one after `fl_rows_unbind` plus the caller's release.
- Malformed-input parity with HARD-1..7 through `fl_decode_rows`; bounds checks not
  `#if DEBUG`-gated; every message preserved.
- `CopyAccounting.BindingProducerWritesInPlace` added to
  `integration-tests/pubsub-conformance`: a producer through `fl_rows_bind` and
  `fl_publisher_publish_row` scores `encode_copies == 0`, retiring the README's
  "stand-in" caveat for the client half.
- The **single-copy check** (D-BIND-17): the shim enumerates loaded modules for a
  second marker export and refuses to initialise, naming both.
- A packed-size budget for the shim in CI (D-BIND-1a rider ii).
- Prior art read first: commit `0050365` on `feature/fastdds_modernization/19645`
  (`EncodeRow(values, WriteBuffer&)`, `batch_decoder.hpp`), a design reference over
  Arrow C++, not something to link.

### BIND-3 — `Eiva.Fletcher.Interop` and the codec/Arrow tier

**As** a .NET developer, **I want** the managed codec and Arrow tier, **so that**
rows go to and from `RecordBatch`es with typed errors and no wire bytes in my code.

**Acceptance**

- `Eiva.Fletcher.Interop`: P/Invoke declarations; a `SafeHandle` per native
  handle; `NativeLibrary.SetDllImportResolver` with a diagnostic that names the
  missing RID; the ABI version handshake at load; the glibc floor stated for
  `linux-x64` (N-7); MSVC CRT linked statically.
- **Error handling per D-BIND-19** (development plan §3.5): one `ThrowIfFailed`
  site; `FletcherException {Status, Origin, Message}` with
  `FletcherFormatException` when `Origin` is the codec; a captured managed
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
- Dictionary handling per spec §"Dictionary Types": decode a dictionary field as
  its value type; reject nested value types with a clear error; nothing beyond
  that ahead of DICT (D-BIND-8).
- The UTF-16↔UTF-8 boundary stated in the XML docs and the oracle's scope note
  (D-BIND-1b).
- **Bucket 1 (104) green** as binding conformance tests;
  `integration-tests/binding-abi-conformance` round-trips every corpus scenario.
- **Per-row publish benchmark** against the C++ generated publisher over
  `inprocess`, allocations per row in the table (B-2). If the per-row cost is
  unacceptable, the only faster route is a D-BIND-1 STOP-AND-ASK, raised here.

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
  int)`, `Publish(TopicPath, BoundRows)`, `Publish(TopicPath, RowWriter)`,
  `PublishRaw`, `ListTopics`, `IDisposable`.
- `Subscriber`: `Subscribe(TopicPath, RowHandler) → SubscribeResult {Subscription,
  SchemaArrival}`, `Unsubscribe(Subscription)`, `AbsorbedCallbackFailures` (the
  managed counter), `DispatchAfterDelivery(Func<Task>)`, `IDisposable`, **no
  finaliser** (S-1). `Subscription` is a handle, not a `ulong`.
- **Thunk discipline per D-BIND-18**: per-subscription in-flight counter and
  `retired` flag, the last one out frees the `GCHandle`; thread-static marker;
  managed refusal of `Dispose` from a handler; managed refusal of a synchronous
  `Subscribe` to a new topic from a handler, pointing at `DispatchAfterDelivery`;
  every managed exception caught, counted and raised as `HandlerFaulted`
  (D-BIND-19 rule 5). The delegate signature takes `ReadOnlySpan<byte>` and ref
  structs so an `async` lambda cannot compile (N-3).
- **Mapping details per D-BIND-20**: `Timeout.Infinite`/`InfiniteTimeSpan` →
  `INT64_MAX`, other negatives `ArgumentOutOfRangeException`; `TopicPath` validated
  in UTF-8 bytes against the six rules and the 246-byte cap; `AttachmentsView`
  positional, no sorting; `SchemaWaitResult` with `Ok` + null as the schema-less
  mode, releasing nothing.
- Provider contracts preserved, not re-implemented: one callback per topic,
  schema-before-data with the schema-less exception, per-writer order, one callback
  at a time with no thread affinity, all three arguments borrowed for the call.
- Bounded-payload overflow surfaces as `FletcherException(PayloadTooLarge)`.
- Fast DDS **and** XRCE reachable by selector with **no per-transport C# code**.
- **Bucket 3 (38) over `inprocess`; Bucket 4 (78) over `fastdds` and `xrce`**;
  the **C# arm of `CallerTier`** with a total mapping onto the C++ cases; at least
  one case added to the C++ suite (seam §12.1).
- The two mapping traps tested: a key above U+E000 and a supplementary-plane key
  where UTF-16 and UTF-8 orders differ; `Wait(Timeout.InfiniteTimeSpan)` waits.

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
- **15 `pubsub-arrow` cases** ported over `inprocess`.
- **The copy oracle run with the C# producer** is this item's acceptance: zero-copy
  for rows and attachments, from managed code, falsifiable.

### BIND-6 — C# row emitter

**As** a .NET developer, **I want** typed row classes generated from my `.proto`,
**so that** I never hand-write the mapping to Arrow.

**Acceptance**

- `<stem>.fletcher.cs` per Part 3: a `sealed` POCO with nullable annotations, a
  real `enum` per proto enum (D-BIND-8), `static Schema Schema` built in code with
  metadata preserved, `static RecordBatch ToArrow(IEnumerable<T>)`,
  `static T FromArrow(StructArray, int)`. **No `WriteTo`/`ReadFrom`**: generated C#
  writes no wire bytes (D-BIND-1).
- Per service method, `<Service>_<Method>Publisher` (`Topic`, `TopicKey`,
  `Schema`, `Publish(T)`, `Publish(T, AttachmentsBuilder)`,
  `Publish(IEnumerable<T>)`) and `<Service>_<Method>Subscriber`
  (`Subscribe(Action<T, AttachmentsView>) → Subscription`), named exactly as the
  C++ pair; **no `SubscribeInPlace`** (Q16).
- Every mapped type in wire-format spec §"Proto to Arrow Type Mapping" is
  emitted: scalars, `repeated`, `map`, nested struct, nested list, the well-known
  types; **temporal types lossless** (D-BIND-26); maps as
  `IReadOnlyList<KeyValuePair<K,V>>` or a `Dictionary` refusing duplicates (G-3).
- Nullability annotations match the Arrow schema's nullability, or the capstone
  fails (G-5).
- Reflection-free: a NativeAOT publish of a consumer app succeeds with **zero**
  trim warnings.
- New cases in `protoc/tests/test_csharp_type_table.cpp` and
  `test_csharp_visitor.cpp`, mirroring the TS pair; the no-drift test proves every
  pre-existing output is byte-identical with the new tokens absent and present.
- Cross-file references resolve — reuse `CollectCrossFileIncludes` **read-only**
  for namespace qualification. Multi-file invocation emits no duplicate filename;
  a shared helper, if needed, is emitted **exactly once** from `GenerateAll()`.

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
- `<Class>View`: a `readonly struct` over `(StructArray, int)` with typed getters
  and `ToRow()`.
- `<Class>Accessor`: **validate-once / cast-once**, positional type-only validation
  (name- and nullable-flag-tolerant), no per-cell materialisation, cached columns
  own their buffers; `TryMake(RecordBatch, out)` and `TryMake(StructArray, out)`
  (the documented deviation from C++ `Make`-throws and Rust `Result`); row-bound
  `RowView` for struct fields; `Metadata(key)` (Q18); **nesting capped at RBA's
  2/3** (Q6).
- Full type parity: scalar, nullable scalar, struct, repeated scalar, repeated
  struct, map (scalar + message value), nested list to the cap, metadata.
- **C# joins `accessor-capstone` as the third language**, consuming the same
  `accessor_capstone.proto`, `accessor_capstone_fixture.json` and
  `accessor_capstone_expected.json`, asserting `observed == expected`. No
  expected values transcribed into the C# test.

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
- End-to-end coverage against a live gateway in the extended
  `integration-tests/protoc-gateway-client-ts`.
- Attachments and `Subscription`/unsubscribe are exposed, not hidden.

### BIND-8 — `Eiva.Fletcher.GatewayClient`

**As** a browser, WASM or lightweight .NET client, **I want** a managed WebSocket
client, **so that** I need no native assets at all.

**Acceptance**

- Text-frame control protocol + binary-frame data protocol per wire-format spec
  §"WebSocket Protocol"; sub-IDs parsed from **strings** (the JS-precision
  workaround is a wire contract, not a JS detail).
- `FletcherClient` with `TypedSchema<T>`-driven `SubscribeAsync<T>` /
  `PublishAsync<T>`, using real generics where TS uses a phantom type;
  `CancellationToken` on every async path; no `async void`; no sync-over-async.
- **This is the one managed-codec exception** (D-BIND-1): it speaks the WebSocket
  protocol, not the binding ABI, and therefore ports the positional codec into
  managed C#. `Envelope` and its (de)serialisation live here and nowhere else (Q15).
- **Carries no native assets, and CI asserts it**: the packed `.nupkg` contains no
  `runtimes/` entry (D-BIND-13).
- **Bucket 2 (56) ported**: all 36 TypeScript cases plus the 20 gateway C++ cases.
- Arrow IPC schema parse of the gateway's base64 `schemaIpc` payload via
  `Apache.Arrow.Ipc`, schema metadata preserved.

### BIND-9 — CI/CD pipelines and packaging

**As** a maintainer, **I want** the .NET tier built, tested and published by the
same machinery as everything else, **so that** it cannot rot.

**Acceptance**

- Part 5 as written: `ci.c-abi.yml`, `ci.dotnet.yml`, `ci.format-check-cs.yml`,
  the three integration lanes, `cd.dotnet.yml` with Trusted Publishing
  (`id-token: write`), `verify-tag-version-dotnet`, no dist-tag step;
  `ci.pr.yml` with 3 filters, 5 caller jobs, `pr_gate` `needs:` **and** `results:`;
  `ci.license-headers.yml` denylist updated.
- **RID matrix** `win-x64` + `linux-x64` fanning in to a single `pack`
  (D-BIND-27); `linux-arm64` as the first addition when wanted.
- **Licence files**: `LICENSE` + graph-derived `THIRD-PARTY-LICENSES.txt` staged
  every run via the #127 deployer; pack fails if either is missing. The
  **asset-isolation check** for `GatewayClient` rides in the same step.
- The **packed-size budget** for the shim.
- The **LGPL relinking decision** from the maintainer (Q9) recorded, with the
  NuGet README stating the relinking route and the NativeAOT static-linking caveat
  (N-8).
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
- Round archived to `docs/archive/BIND/`.

---

## Part 8 — Risks, and what is still open

The full register (31 items, five groups) is the development plan §6 and each item
names the round item that closes it. The ones worth reading first:

| Risk | Mitigation |
|---|---|
| **B-1 — there is no codec to wrap.** BIND-2 builds the nanoarrow codec; roughly the size of the 530-line Arrow C++ one | Byte identity against `arrow-bridge` across the corpus is the forcing test; prior art in commit `0050365`. |
| **B-2 — per-row publish pays for a one-row Arrow array**, and the copy oracle cannot see allocations | The bind step makes batching structural; BIND-3 benchmarks per-row against the C++ generated publisher; if unacceptable, the only faster route is a D-BIND-1 STOP-AND-ASK. |
| **B-3/Q3 — the shim's size and Fast DDS's shared-memory directories** on the consumer's machine | Measured at BIND-0 against the packed-size budget; the gateway is the static-linking precedent; stated in the README. |
| **N-5 — two `release` protocols** (C Data Interface vs owner-handle retain/release) | Received schemas deep-copied natively before import; `BoundRows.Dispose` unbinds before releasing; tested with a schema received twice. |
| **S-2 — the re-entrant `Unsubscribe` carve-out** and sibling handlers | The in-flight counter (D-BIND-18); a test mirroring `CallerTier.CancellingASiblingRunningOnAnotherThreadKeepsItPublished`. |
| **N-3 — `async` handlers** whose continuation outlives the borrowed arguments | Span/ref-struct delegate signature so an `async` lambda cannot compile; documented. |
| **P-1 — LGPL relinking** | Owner: the maintainer (Q9); the notice half is mechanised by #127; packaging waits for the decision, development does not. |
| **P-7 — `protoc/` and `arrow-bridge/` contended** with the modernization branch's pending commits, written against the pre-GIR generator | Landing order agreed at BIND-0; the no-drift test, `TsVisitor.DescriptorByteIdentical` and the codec byte-identity oracle prove neither party moved wire bytes. |
| **N-7 — runtime `DllNotFoundException`** on a missing RID or an older glibc | `SetDllImportResolver` naming the RID; the ABI version handshake; the glibc floor stated. |

### Open items

No decision is open. Three actions remain, none an engineering one:

1. **LGPL relinking decision** (owner: the maintainer) before BIND-9 designs
   packaging. BIND supplies the brief.
2. **NuGet.org Trusted Publishing** policy and the `Eiva.Fletcher.*` prefix (user
   action outside a PR), requested at BIND-0.
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
- **275 of 300** non-generator cases green in C#; the two exclusion classes
  documented; 18786 not closed before Bucket 4.
- Generator: every plugin output has a C# counterpart (row, Arrow view, accessor);
  TS gains `Publisher`/`Subscriber`; `TsVisitor.DescriptorByteIdentical` still
  green; the no-drift test proves no existing output byte changed.
- **Wire bytes unchanged** (GIR-2).
- `GatewayClient` verified to carry **no** native assets; the shim within its size
  budget; both licence files in the package.
- Docs updated incl. TD-009; round archived to `docs/archive/BIND/`.
