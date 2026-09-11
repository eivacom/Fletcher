# BIND — Locked Decisions (digest)

Round **BIND** (C# bindings + generated TypeScript pub/sub). The small digest the
architect / architect-reviewer / compliance steps must honour. Full rationale:
[BIND-csharp-bindings.md](BIND-csharp-bindings.md); native transport:
[BIND-native-transport.md](BIND-native-transport.md).

**Upstream oracles that win on any conflict:**
[GIR-locked-decisions.md](GIR-locked-decisions.md) (the generator IR — merged) and
`docs/protocol-driver-abi-spec.md` + `plans/PDA-locked-decisions.md`
(the driver ABI — **in flight on `feature/protocol-driver-abi`**; both paths
resolve once PDA merges, so they are deliberately not links yet). A proposed deviation from anything here is
**STOP-AND-ASK**.

- **D-BIND-1 — ONE codec. The binding wraps it; it is never reimplemented.**
  *(Revised and **CONFIRMED BY THE MAINTAINER 2026-08-31**. Reverses the earlier
  managed-port decision; not an architect's assumption.)*
  The positional codec exists once, in C++, and is reached through a
  **descriptor-driven** binding-ABI surface — GIR-8: *"BIND-2 wraps 'generated
  encode/decode through a schema/descriptor-handle model', which is closer to a
  generic codec than N bespoke encoders the C ABI must each wrap."* Reinforced by
  PDA-L4 (zero-copy required) and ADO 18787 ("bindings against C ABI").
  Consequences:
  - **No second implementation of the positional wire format** is added, in any
    language, by this round.
  - The **bespoke recursive C++ edge path stays** (GIR-8) — MCU/device targets keep
    their zero-Arrow-dependency codec.
  - **`Eiva.Fletcher.GatewayClient` is the sole exception** and stays fully
    managed: the browser has no native option, and it is the existing
    `gateway-client-ts` design ported. It talks the WebSocket protocol, not the ABI.
  Proposing a managed reimplementation of the codec → STOP-AND-ASK.

- **D-BIND-1a — Value transfer across the binding ABI is the Arrow C Data
  Interface.** ✅ *CONFIRMED BY THE MAINTAINER 2026-09-11 (Q1), with both riders and together with D-BIND-23 below.*
  `Codec::EncodeRow` takes `std::vector<std::shared_ptr<arrow::Scalar>>` — a C++
  type tree that **cannot** cross a pure C ABI (PDA-L3). Values therefore cross as
  **`ArrowArray` + `ArrowSchema`** (C Data Interface), which:
  - represents **arbitrary struct/list/map nesting by construction**, so nested
    types need no bespoke ABI surface at all;
  - is what **nanoarrow already is** — `OwnedSchema` carries `ArrowSchema` through
    the pubsub layer today, so the edge tier is reachable without Arrow C++;
  - extends PDA-L5's mandated `ArrowSchema`-verbatim vocabulary rather than
    inventing a parallel one;
  - allows **batch** encode/decode (N rows per ABI call, not one).

  **A bespoke row-builder ABI** (`begin_struct` / `list_append` / `set_int32` / …)
  is the rejected alternative: it works, but the ABI surface then grows with the
  type system and every nesting construct needs its own entry points, ownership
  rules and tests. Drifting into it → STOP-AND-ASK.

  Two riders, confirmed with it on 2026-09-11:
  1. **`Apache.Arrow` becomes a base dependency of the C# row path**, so
     `Eiva.Fletcher.Core` cannot stay dependency-free. Judged acceptable: the C++
     edge/server split exists because MCUs cannot link Arrow C++, and **a .NET
     process is never the microcontroller** — so the two-tier split may not need
     mirroring in C# at all, possibly collapsing `Core` + `Arrow`.
  2. **The native artifact links nanoarrow only, not Arrow C++.**
     `arrow-bridge/codec.hpp` includes `<arrow/api.h>`, so wrapping *that* codec
     would drag full Arrow C++ into `Eiva.Fletcher.Interop`. GIR-8's
     descriptor-driven codec must stay nanoarrow-only to keep the NuGet package
     small; verified by a packed-size check in CI.

- **D-BIND-1b — The zero-copy guarantee has a stated boundary.** PDA-L4 zero-copy
  covers the **row buffer and attachments**. It does **not** cover individual
  managed scalar values: a C# `string` is UTF-16 and the wire is UTF-8, so every
  string/`bytes` field is transcoded — a real per-value allocation that no design
  removes. BIND-7's copy-accounting oracle must encode this boundary explicitly;
  otherwise it either fails spuriously or gets quietly fudged. Claiming
  end-to-end zero-copy including string values → STOP-AND-ASK.

- **D-BIND-1c — Errors keep HARD's diagnostics across the boundary.** HARD-1..7
  made the C++ codec *throw* with specific messages; exceptions cannot cross the
  ABI (PDA-L3). Every throw maps to a status code **plus the preserved message**,
  re-raised as a typed C# exception. A generic "encode failed" discards exactly
  what HARD-1..7 added → STOP-AND-ASK. Two lifetime protocols coexist and must not
  be conflated: the C Data Interface's own `release` semantics, and PDA's blob
  `retain`/`release`.

- **D-BIND-2 — Two ABIs, opposite directions; share vocabulary, never functions.**
  Per PDA-L5: the **driver ABI** (`fletcher/abi/driver.h`, Fletcher is caller,
  driver implements) and the **binding ABI** (`fletcher/abi/binding.h`, Fletcher is
  callee, app calls) are directional opposites. **No driver data-plane function is
  callable by a binding.** Bindings use **role 3** (driver management) only. BIND
  **reuses verbatim**: the buffer struct, the blob `retain`/`release` protocol,
  `ArrowSchema` (C Data Interface), and the error/version conventions. Designing a
  second buffer or blob representation → STOP-AND-ASK.
  **Shape as of 2026-08-31** (PDA branch reshaped it — re-verify at Stage A): the
  buffer is a **window plus refill hooks**,
  `{ctx, data, capacity, pos, grow, grow_zeros}`, **not** per-append function
  pointers. So a publish crosses the boundary **once per window refill, not once
  per append**. Patching happens inside the window (a driver may not move bytes
  below `pos` except inside `grow`, which must preserve them verbatim), and bounds
  are computed by **subtraction, never addition**. `grow_zeros` (null-bitfield
  placeholders) is part of the vocabulary.

- **D-BIND-2a — Everything the delivery callback receives is BORROWED.** As of
  2026-08-31 the C++ contract passes `data`, `schema` **and** `attachments` as
  borrowed-for-the-call (`const SharedSchema&`, `const Attachments&` — changed from
  by-value after measuring ~110 ns/sample of MSVC `unordered_map` sentinel
  allocation against 1.4 ns for the call itself). The C# binding therefore lets
  **none** of the three escape and copies any it wants to retain. Two further
  clauses BIND must honour, not re-implement:
  - **One callback at a time** per subscription — so no per-subscription locking is
    needed, but **no thread-affinity may be assumed**: the delivering thread may
    differ between samples.
  - **Schema-before-data has an exception** — a transport that carries no schemas
    at all (the gateway's in-process loopback, where the client brings its own)
    passes **null** throughout and must never mix the two. C# handles a null schema
    as a valid per-transport mode, not a contract violation.
  Letting a borrowed argument escape, or assuming thread affinity → STOP-AND-ASK.

- **D-BIND-3 — Native protocols come from PDA drivers, not from BIND.** BIND binds
  `PubSubProvider` once; Fast DDS, XRCE-DDS, InProcess and any future driver are
  inherited, selected at **runtime from config**. **No per-transport C# code
  exists.** BIND passes the **opaque driver-defined document** through untouched
  and **parses no protocol config** (PDA-L2 — the same explicit non-goal Fletcher
  itself holds). Stage D is **gated on PDA merging**; binding
  `FastDDSProviderOptions` (retired by PDA-L9) → STOP-AND-ASK.
  ✅ **Amended 2026-09-11 (Q3): the pub/sub stage is gated on NOTHING.** PDA-DEC merged as
  #126; BIND wraps `ProviderRegistry::Create(selector, config)` directly. **The native shim
  statically links and registers all three built-ins — `inprocess`, `fastdds`, `xrce`** —
  so a C# application reaches every transport Fletcher has today by selector name (ADO
  18689), with the gateway as the in-tree precedent for shipping Fast DDS statically. A
  **path** selector is forwarded and refuses with `kNotSupported` exactly as the C++
  registry does, until PDA-ABI installs its resolver via `SetPathResolver` inside the shim;
  nothing above changes then. Accepted costs: the per-RID asset carries Fast DDS, Fast CDR,
  foonathan_memory, tinyxml2, Micro XRCE-DDS and microcdr; Fast DDS needs writable
  shared-memory directories on the consumer's machine; the third-party notice lists all of
  it (generated by the #127 deployer, BIND-9). Dropping a built-in from the shim → STOP-AND-ASK.

- **D-BIND-4 — Zero-copy is required and must be FALSIFIABLE.** Rows **and**
  attachments, per PDA-L4. A **copy-accounting oracle** ships with BIND-2, not at
  the end of the round, and it is a first-class deliverable. Accepting "one copy at
  the boundary" anywhere on the row or attachment path → STOP-AND-ASK. Asserting
  zero-copy without the oracle is not acceptance.

- **D-BIND-5 — The C# generator backend is a THIRD IR BACKEND, built like the TS
  one.** Per GIR-1: `csharp_backend_type_table` is the **only** place a C# type
  string may live, keyed on `ir::LogicalType` (+ optional `ir::EnumIdentity`);
  `csharp_backend_visitor` is a recursive IR visitor. The message walk **reuses
  `cpp_backend::BuildFlattenedFieldList`** exactly as `TsVisitor` does, so the C#
  field set cannot drift from the schema field set. Putting a C#/`System.`/type
  string on an IR node → STOP-AND-ASK.

- **D-BIND-6 — Wire bytes never change; existing outputs never change.** GIR-2 and
  PDA-L12. BIND adds **new output files** and **new** emitter entry points; it does
  not edit an existing emitter. The no-drift test proves every pre-existing output
  is byte-identical both with the new opt tokens absent and present, and
  `TsVisitor.DescriptorByteIdentical` stays green through BIND-T. Any wire-byte
  change → STOP-AND-ASK.

- **D-BIND-7 — BIND-T is purely additive to the TS output.** Generated
  `<Service>_<Method>Publisher` / `Subscriber` classes are **appended** to
  `<stem>.fletcher.ts`, named exactly as the C++ generated pair, as thin wrappers
  over `FletcherClient`. Existing hand-wired usage keeps working; **no new runtime
  dependency** is introduced, so the browser path is unaffected. Interleaving into
  existing emitted content, or changing the descriptor output → STOP-AND-ASK.

- **D-BIND-8 — Enum is emitted as a real C# `enum`; Dictionary defers to DICT.**
  GIR-7 makes `Enum` a first-class IR identity with symbols preserved
  *"needed by #75 + BIND"*, so C# emits a typed `enum` marshalled as int32 (the
  C++ backend's flat int32 lowering is a backend choice, not an IR fact). Wire
  bytes are identical either way — if they are not, the enum emission is wrong.
  `Dictionary` is a scalar **modifier**, never a container; and because the DICT
  round **halted after DICT-2** (nanoarrow IPC rejects dictionary types), BIND
  **defers dictionary support to DICT's conclusion**. Inventing a representation
  ahead of DICT → STOP-AND-ASK.

- **D-BIND-9 — The accessor emitter builds on the IR, not on `FieldKind`.** RBA
  stays on the thin `FieldKind` projection this round (GIR-3) and round **RIR**
  migrates it, sequenced after BIND-Rust. A greenfield C# accessor on the IR gives
  RIR a target to migrate *toward* rather than another `FieldKind` consumer to
  unpick. Chain is locked: **GIR → BIND-C# → BIND-Rust → RIR**.
  **Depth-cap consequence (open — decide in BIND-6):**
  `recordbatch_accessor_emitter.cpp` caps nested lists at depth 2-or-3 and GIR-3
  keeps that cap, but the IR has **no** cap — so a C# accessor on the IR could
  support depth 4+ where C++ cannot, breaking capstone parity. Either match the
  2/3 cap or exceed it deliberately and document the asymmetry; letting it happen
  by accident → STOP-AND-ASK. Note this is an **accessor** limit only:
  `positional_io` recurses without a depth bound, so the **codec** supports
  arbitrary nesting regardless.
  ✅ **Decided 2026-09-11 (Q6): MATCH the 2/3 cap.** The C# accessor emitter caps nested lists
  exactly where `recordbatch_accessor_emitter.cpp` does, so `accessor-capstone` keeps one
  shared fixture for all three languages (D-BIND-11); the asymmetry between the IR (no cap)
  and the accessors (2/3) is recorded as RIR's to remove, not BIND's to exploit.

- **D-BIND-10 — `StructArray` child windowing is an OPEN EMPIRICAL QUESTION.**
  Arrow **C++** `StructArray::field(i)` returns children already sliced to
  `[offset, len)` (do **not** re-slice); **arrow-rs** `StructArray::columns()` is
  **not** pre-windowed (Rust **must** slice). **arrow-dotnet**'s
  `StructArray.Fields` is a **third unknown** and MUST be settled empirically in
  BIND-6 with a nested fixture at a **non-zero offset** as the forcing test. The
  finding is then recorded here as the locked rule. Writing nested getters before
  it is settled → STOP-AND-ASK. (RBA-2 carry-forward.)

- **D-BIND-11 — Parity is proven by shared fixtures and executable oracles, never
  by hand-transcription.** With one codec, the old two-way byte-parity harness is
  **repurposed** into an **ABI conformance suite** over the existing `emit_vectors`
  scenario corpus, plus the copy-accounting oracle (D-BIND-4). The **generated**
  artifacts are still genuinely independent per language, so C# joins
  `accessor-capstone` as the **third arm** against the *same* proto, the *same*
  fixture and the *same* committed oracle. No expected value may be transcribed
  into a C#-only fixture when a shared one exists.

- **D-BIND-12 — Test scope: 177/178 non-generator cases, staged; generator tests
  stay in C++.** 18786 requires "C# versions of all tests as CI tests". Buckets
  1–4 (60 + 56 + 31 + 30) port to C#; the sole exclusion is
  `pubsub/tests/test_owned_schema.cpp` (an `ArrowSchemaDeepCopy` ENOMEM path with
  no managed analogue), documented and not silent. Bucket 5 (77+ generator cases)
  **stays in C++** — it tests a C++ generator — and is *extended* with
  `test_csharp_visitor` / `test_csharp_type_table` and TS pub/sub cases. **This
  reading of "all tests" must be confirmed with the Feature owner**, not assumed.
  Bucket 3/4 cannot be reported green before Stage D, so **18786 must not be closed
  early**. Silently dropping any Bucket 1–4 case → STOP-AND-ASK.
  ✅ **Amended 2026-09-11 (Q10).** Matrix re-baselined at `6c541e9`: **300** non-generator
  cases (Bucket 1: 104 · 2: 56 · 3: 38 · 4: 102) and **98** generator cases, counted by
  `grep -c '^TEST\(_F\|_P\)\?('` per file (development plan §5). **Two exclusion classes,
  confirmed by the maintainer, both documented and neither silent:** (a) the 98 generator
  cases stay in C++ and are *extended* with `test_csharp_visitor` / `test_csharp_type_table`;
  (b) provider-internal cases — `test_fletcher_sample_pub_sub_type` (24, the Fast DDS
  `TypeSupport`) — test a C++ type no managed code touches and are not ported;
  `test_owned_schema` (1) stays excluded as before. Everything else ports. **18786 is not
  closed before Bucket 4 is green over `fastdds` and `xrce`.**

- **D-BIND-13 — Native assets are isolated to ONE package.**
  `Eiva.Fletcher.Interop` is the only package carrying `runtimes/{rid}/native/`;
  every other managed package is asset-free, and a CI check asserts
  `Eiva.Fletcher.GatewayClient` carries **no** `runtimes/` folder. This keeps the
  RID matrix and the **LGPL relinking review** scoped to one artifact and keeps a
  browser/WASM-adjacent story alive. Adding native assets to a second package →
  STOP-AND-ASK.
  *Owner of the LGPL relinking review: the maintainer (assigned 2026-09-11, Q9). Decision due
  before BIND-9 designs packaging; the notice half is mechanised by #127's graph-derived
  `THIRD-PARTY-LICENSES.txt`, which BIND-9 reuses for the shim.*

- **D-BIND-14 — One `dotnet/` component; `c-abi/` is separate.** The managed
  packages ship from **one** top-level `dotnet/` directory, versioned by **one**
  `Directory.Build.props` `<VersionPrefix>`, released by **one** `dotnet-v` tag and
  **one** `cd.dotnet.yml`, all at the **same** version. `c-abi/` is its own
  top-level Conan component because it is built by Conan/CMake, not by the
  solution. `MAJOR.MINOR` remains the cross-component compatibility marker.
  **Series:** the PDA branch has bumped the tree to `0.5.0-alpha`, so the .NET
  packages join the **`0.5.x`** line (not `0.4.x`, as the first draft assumed) —
  confirmed 2026-09-11 against the tree (`0.5.0-alpha` everywhere, XRCE `0.5.1-alpha`).
  ✅ **Amended 2026-09-11 (Q2): THREE managed packages, not six.**
  `Eiva.Fletcher.Interop` (native assets + P/Invoke; the only package with `runtimes/`,
  D-BIND-13 unchanged), `Eiva.Fletcher` (rows, codec, Arrow tier, pub/sub, PubSubArrow),
  `Eiva.Fletcher.GatewayClient` (managed, no native). With `Apache.Arrow` a base dependency
  (D-BIND-1a rider i) there is no dependency-free tier left for a `Core`/`Arrow`/`PubSub`
  split to protect. Namespaces inside `Eiva.Fletcher` keep the C++ component names
  (`Eiva.Fletcher.PubSub`, `Eiva.Fletcher.Arrow`, …) so a later split needs no renames.
  Adding a fourth package → STOP-AND-ASK.

- **D-BIND-15 — Malformed input throws; it never mis-reads.** Every hardening case
  HARD-1..7 added (truncated buffer, absurd length prefix, over-long list/map
  count, out-of-range null bitfield, failed schema deep-copy, swallowed serialize
  error) must surface as a typed exception on the C# side and must never read
  outside the input span. Bounds checks are **not** `#if DEBUG`-gated.

- **D-BIND-16 — CI/CD reuses existing machinery.** New workflows follow the in-tree
  patterns exactly: `workflow_call` + `devcontainer-image` input;
  Linux-in-container + Windows-native two-job split; per-job `concurrency` with
  `cancel-in-progress`; `ci.pr.yml` filters using **positive patterns only**; every
  new caller job added to `pr_gate`'s `needs:` **and** `results:`. The Conan-cache
  plugin-discovery block is **copied verbatim** from
  `ci.integration-test.protoc-gen-fletcher-rust.yml`. Publishing uses NuGet.org
  **Trusted Publishing** (OIDC, `id-token: write`) — the analogue of the existing
  npm `--provenance` flow — with a `NUGET_API_KEY` secret only as a documented
  fallback.

---

## Rulings of 2026-09-11 (folded from [BIND-csharp-development-plan.md](BIND-csharp-development-plan.md) §2 and §7)

D-BIND-17 to D-BIND-22 are proposals in the development plan and are inserted here,
in order, as their questions are ruled. D-BIND-23 was ruled with Q1 and lands first.

**Scope (Q12, 2026-09-11): BIND-Rust is the NEXT round.** This round ships the C# binding and
the generated TypeScript pub/sub only. GIR-3's chain GIR → BIND-C# → BIND-Rust → RIR stands;
the binding ABI header BIND-1 writes is the surface BIND-Rust will consume, so it benefits
from landing first. The remote `feature/rust-bindings` branch is noted, not adopted.

**Reading of Feature 16353 (Q19, 2026-09-11):** "Make C# emit Rust-native accessor classes" is
read as *C# accessor classes equivalent to the Rust-native ones* — BIND-7's `<Class>Accessor`
on the IR (D-BIND-9), joining `accessor-capstone` as the third language. The literal phrasing
is a copy-paste from the RBA feature; the ADO text is corrected to say what is meant.

- **D-BIND-17 — Exactly one copy of Fletcher per process, and a load-time check.**
  *LOCKED BY THE MAINTAINER 2026-09-11 (Q11).* From `delivery_frame.hpp` P1: the re-entrancy
  machinery is an `inline thread_local` in a header-only target, so every separately linked
  binary carrying Fletcher gets its own delivery-frame stack and the doors stop seeing each
  other's frames, silently. The native shim is therefore **one shared library** that
  statically links every Fletcher component it uses, and no other binary the package ships
  carries Fletcher code. At load the shim enumerates loaded modules (`EnumProcessModules` /
  `dl_iterate_phdr`) for a second export of its marker symbol and **refuses to initialise** if
  one is found, naming both modules — two shims in one process is the detectable case and the
  likely one. **Fact ruled with it (Q11): Flight's consumers are .NET processes hosting this
  binding only**, so the undetectable case — a C++ host that statically links Fletcher and
  also hosts the CLR, or a process mixing two language bindings — does not arise. Should a
  future consumer host Fletcher twice, P1's own note applies: **STOP-AND-ASK before design**,
  never a registration handshake papering over a forked thread-local. The .NET README states
  the rule.

- **D-BIND-18 — The delivery thunk owns the lifetime bookkeeping; re-entrant `Subscribe` is
  refused in managed code and the sanctioned route is an explicit async helper.** *LOCKED BY
  THE MAINTAINER 2026-09-11 (Q4).* From constraints 2, 3, 4, 5 and 8 of
  [BIND-csharp-inherited-constraints.md](BIND-csharp-inherited-constraints.md). Each managed
  subscription carries an atomic in-flight counter the thunk maintains (increment on entry,
  decrement on exit) and a `retired` flag set by `Unsubscribe`; because the seam guarantees no
  invocation *begins* after `Unsubscribe` returns, **whoever brings the counter to zero after
  retirement frees the `GCHandle`**, on the blocking and the re-entrant path alike. A
  thread-static "inside my own thunk" marker additionally: refuses `Dispose` of a `Subscriber`
  from a handler with a managed exception (the native answer is process termination, by design);
  refuses a **synchronous** `Subscribe` to a new topic from a handler with a managed exception
  naming `DispatchAfterDelivery`, whose `Task<Subscription>` carries any failure — chosen over
  the constraints note's "always queue", which would surface a refused topic name only through a
  failed `SchemaArrival` later; and makes the thunk catch every managed exception, count it in a
  per-instance `AbsorbedCallbackFailures` and raise an optional `HandlerFaulted` event, since
  native's count cannot see a managed catch. Silent queueing of a re-entrant `Subscribe` →
  STOP-AND-ASK.

- **D-BIND-19 — Error handling across the boundary: five rules.** *LOCKED BY THE MAINTAINER
  2026-09-11, widened from the writer-frame rule of constraints 6 and 7. Detail and code in the
  development plan §3.5.* **(1)** One numbered error type per side and the number AND the
  message always cross: `PubSubError(status, message)` ↔ `FletcherException {Status, Message}`,
  carried by a caller-owned `fl_error {status, origin, message, message_len}` out-parameter
  freed through `fl_error_dispose`, never a global or per-thread slot. `origin` (seam · codec ·
  callback) records which C++ catch arm fired; it is BIND's own field, not a change to the
  append-only numbers, and it is how the wrapper knows to throw D-BIND-15's
  `FletcherFormatException` for a `kInvalidArgument` that came from `PositionalReader` rather
  than from the seam. **(2)** No exception crosses in either direction, and each direction has
  exactly one containment site: `Translate()` in the shim (with §5.1's `std::overflow_error` →
  `kPayloadTooLarge` rule), the thunk prologue in the wrapper (a reflection test fails on any
  `[UnmanagedCallersOnly]` method without it). **(3)** A managed exception that caused a native
  failure is rethrown **as itself** via `ExceptionDispatchInfo`, never wrapped; native sees a
  real `PubSubError(kInternal, message)`. The writer-frame case is this rule's first instance:
  `AppendInPlace` is reachable from C# only through `PublishRaw` and the `RowWriter` overload
  (contract: returns bytes written); the thunk captures and returns 0, the C++ adapter throws so
  nothing commits, the wrapper rethrows the original; the `SIZE_MAX` route is not used.
  **(4)** Expected outcomes are values (`kPending`, `kSubscriptionEnded`, `Ok` + null →
  `SchemaWaitResult`); refusals the caller can fix are .NET argument exceptions thrown in managed
  code before the call, native re-checking the same rules (Q17), `OperationCanceledException`
  managed-only. **(5)** Absorbed handler failures are counted in the per-`Subscriber` counter
  AND raised as a `HandlerFaulted` event, never silent and never escaping. A second containment
  site, a wrapped user exception, a per-thread error slot, or a fixed-size message buffer →
  STOP-AND-ASK.

- **D-BIND-20 — The two mapping details are code, not comments.** *LOCKED BY THE MAINTAINER
  2026-09-11.* `Timeout.Infinite` / `Timeout.InfiniteTimeSpan` (−1) map to `INT64_MAX`, the
  seam's unbounded form, before the call; any other negative is an `ArgumentOutOfRangeException`
  raised in managed code (seam §3.4 refuses negatives with `kInvalidArgument` so that "negative
  means forever" cannot be invented by one boundary and not another). Every seam bound is
  **UTF-8 bytes**: `TopicPath` measures segments and the 246-byte joined cap in bytes before
  crossing, `string.Length` is never compared to a seam bound, and `AttachmentsView` reads the
  sequence Fletcher publishes through `KeyAt(i)` and offers no sorting, because `memcmp` order
  over UTF-8 differs from C#'s ordinal UTF-16 order (U+FF5E sorts before U+10000 in one and
  after it in the other). A managed comparer or a `Length` check standing in for a byte bound →
  STOP-AND-ASK.

- **D-BIND-21 — Toolchain: .NET 10 LTS SDK; libraries multi-target `net8.0;net10.0`.**
  *LOCKED BY THE MAINTAINER 2026-09-11 (Q7).* The SDK is pinned in `.devcontainer/Dockerfile`
  (a `DOTNET_SDK_VERSION` build arg beside the existing `NODE_VERSION` pin) and in
  `dotnet/global.json` with `rollForward: latestFeature`, so the container, CI and a
  developer's host cannot silently diverge. `net8.0` is kept as a target so a consumer still
  on .NET 8 can reference the packages until it moves; .NET 8 leaves support in November
  2026, and dropping the `net8.0` target is a later MINOR bump once consumers confirm, not a
  patch. `Apache.Arrow` is pinned to a version whose `Apache.Arrow.C` namespace exports and
  imports `CArrowArray`/`CArrowSchema` — verified at BIND-0, not assumed (N-9). Supersedes
  the 2026-08-31 plan's ".NET SDK 8.0.x (LTS)" in Part 5.

- **D-BIND-22 — Nothing crossing the binding ABI is a `std::shared_future`.** *LOCKED BY THE
  MAINTAINER 2026-09-11.* A future has no C form, which is why `SchemaArrival` replaced it (seam
  §3.4, ruling 2026-09-01 "one waiting mechanism, not two"); a binding-side bridge (a thread
  parked on `get()` calling back into managed) would be a second waiting mechanism with
  different semantics — no `kPending`, no `kOk` + null, `future_error` instead of
  `kSubscriptionEnded` — and BIND-C# and BIND-Rust would each build a different one. Seam
  §12.2 condition 2a admits no machine catches a future added beside `SchemaArrival`; this is
  BIND's half of that guard. A future appearing at the seam is a stop-and-ask against the spec,
  raised by BIND and never bridged by it.

- **D-BIND-23 — The encoder behind the binding ABI takes a bound array, a row index
  and a destination; arrays are borrowed, never consumed; no encode entry point
  returns bytes.** *LOCKED BY THE MAINTAINER 2026-09-11 (Q1), in all three layers.*
  The surface is three steps because each has a different cost and lifetime
  (development plan §3.2): `fl_codec_open` once per schema (schema borrowed and
  deep-copied), `fl_rows_bind` once per batch (the `ArrowArray` is **borrowed**: the
  codec never calls `release`, the caller unbinds and then releases), and per row or
  range `fl_publisher_publish_row(s)` fused into `Publish`'s `RowEncoder` so the row
  is written straight into the provider's window, `fl_encode_row` into the seam's
  window-plus-refill C form for bytes in hand, and `fl_decode_rows` as the mirror.
  Behind it `NanoarrowCodec{Bind, EncodeRow(const BoundRows&, int64_t, WriteBuffer&),
  DecodeRows}` on `positional_io`, nanoarrow only; on top `FletcherCodec.Bind →
  BoundRows` and `Publisher.Publish(topic, rows, i)`, with `BoundRows.Dispose()`
  unbinding before it releases. A returned buffer is the staging copy §8.1's negative
  control catches; a consumed array forces one export per row; per-value C arguments
  are the row-builder ABI D-BIND-1a rejected. Drifting into any of the three →
  STOP-AND-ASK. The existing C++ tree is driven as is: `PositionalWriter`/`Reader`,
  `WriteBuffer`, `Publisher::Publish`, `OwnedSchema::DeepCopy` and pubsub's exported
  nanoarrow change for none of this; `arrow-bridge`'s `Codec` stays the byte-identity
  oracle target and is not modified.

- **D-BIND-24 — A C# application never implements a provider.** *LOCKED BY THE MAINTAINER
  2026-09-11 (Q13).* `PubSubProvider` reaches C# as an **opaque handle** returned by
  `ProviderRegistry.Create(selector, config)`; there is no managed interface to derive from,
  and `ProviderRegistry::Register` and `SetPathResolver` are **not** on the managed surface,
  because all three would take callables native must enter from managed. Three reasons in
  rising order of force: a reverse-direction vtable roughly the size of the rest of the
  ABI, including handing a `WriteBuffer&` back into managed on every `Publish`; seam §9's
  own sentence that a transport authored outside C++ is a *driver*, which is PDA-ABI's
  boundary; and owner ruling 46 of 2026-09-05 that every protocol driver is C++. The shim
  registers its built-ins itself (D-BIND-3 as amended). Exposing an implementable provider
  interface, or a managed `Register`, → STOP-AND-ASK.

- **D-BIND-25 — The unit of one row in C# is a `RecordBatch` plus a row index, bound once as
  `BoundRows`; the Arrow tier is batch-first; no managed `ArrowRow` is invented.** *LOCKED BY
  THE MAINTAINER 2026-09-11 (Q14).* C++'s `ArrowRow` is `vector<shared_ptr<arrow::Scalar>>`
  and `Apache.Arrow` has no `Scalar` type, so it has no analogue. Consequences: `PublisherArrow`
  folds into `Publisher`, whose `Publish` is Arrow-typed through `BoundRows` (D-BIND-23);
  `SubscriberArrow.SubscribeBatched` with `RecordBatch` delivery is the primary receive shape;
  per-row Arrow delivery is a one-row batch (`BatchOptions.MaxRows = 1`); typed per-row
  delivery is the generated `<Svc>_<Method>Subscriber` decoding into a POCO via `FromArrow`.
  A managed scalar tree (`IReadOnlyList<object?>` or a Fletcher scalar hierarchy) would box
  every value and re-create on the receive side the per-value shape D-BIND-1a rejected on the
  send side → STOP-AND-ASK.

- **D-BIND-26 — Temporal values are LOSSLESS by default in generated C#.** *LOCKED BY THE
  MAINTAINER 2026-09-11 (Q5).* `DateTime`/`DateTimeOffset` ticks are 100 ns; an Arrow
  nanosecond timestamp would lose two digits on a round trip, silently, and show up as a
  wire-visible difference in the capstone. The C# type table therefore maps `Timestamp` to a
  `long`-backed struct carrying the unit (and timezone metadata where present) with
  `ToDateTimeOffset()` as an explicit, documented conversion, and `Duration` to a `long`-backed
  struct with `ToTimeSpan()`. Mapping either directly onto a 100 ns type → STOP-AND-ASK.

- **D-BIND-27 — RID matrix at first release: `win-x64` and `linux-x64`.** *LOCKED BY THE
  MAINTAINER 2026-09-11 (Q8).* The two the tree already builds and tests. `linux-arm64` is the
  first addition (cross-compilation or an ARM runner, to be decided when added); `osx-arm64`
  on demand, since it needs a macOS runner and a Fast DDS build the tree has never done. The
  glibc floor for `linux-x64` is stated in the package README (N-7). Adding a RID is additive
  and needs no ruling; shipping a RID the CI does not run is a STOP-AND-ASK.

**Interface rulings (Q15–Q18, 2026-09-11), recorded in
[BIND-csharp-public-surface.md](BIND-csharp-public-surface.md) §2 and binding on BIND-4/6/7/8:**
`Envelope` and its (de)serialisation live in `Eiva.Fletcher.GatewayClient` only; nothing on
the native path exposes them (Q15). The generated C# subscriber has **no** `SubscribeInPlace`;
its purpose in C++ is avoiding a per-sample allocation and the C# answer is batch delivery
(Q16). Topic segments and the provider selector are validated in managed code against the
same rules as native, in **UTF-8 bytes**, and native re-validates; the duplication is accepted
because a managed refusal carries a stack trace, and both sides are tested against one table
(Q17). The C# accessor exposes generic metadata access (`Metadata(key)`) as the C++ and Rust
accessors do, for capstone parity (Q18).
