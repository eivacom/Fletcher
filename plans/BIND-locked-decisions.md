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

## Where a ruling has to land — FOUR places, not three

Written down 2026-09-18 because the fourth was being missed, every time, for six
weeks. A ruling is recorded the same turn it is made in:

1. **this digest** — the entry itself;
2. **[BIND-csharp-bindings.md](BIND-csharp-bindings.md)** — the decision table and
   the affected item's rows;
3. **[BIND-csharp-development-plan.md](BIND-csharp-development-plan.md)** — §2 and §7;
4. **[BIND-architecture-diagrams.md](BIND-architecture-diagrams.md)** — **if the
   ruling changes a shape, a name, a count or a direction.**

The fourth is the one that rots, and it rots invisibly: prose contradicts itself in
a way a reader notices, while a diagram just *draws the old design* and looks
authoritative doing it. Measured on 2026-09-18, when Copilot's review of #129 and
the sweep around it found **eight** stale claims in that one file — edges labelled
*"REUSES verbatim"* into the driver ABI five weeks after D-BIND-2′ forbade sharing a
declaration, a value-transfer hop still marked *"PROVISIONAL — open decision 1"*
after Q1 ruled it, a package diagram showing two packages Q2 had collapsed out of
existence, and a subscribe sequence showing the generated class parsing wire bytes
through `ReadFrom` — **the managed codec D-BIND-1 refused**, drawn as if it were the
spec, in the diagram a BIND-4/6 implementer would read first.

None of it reached the code: BIND-1 and BIND-2 were built against the rulings, not
against the pictures. But that is an implementer knowing better than the record,
which is not a control — and BIND-Rust reads these files next round.

**Two habits that go with it.** A ruling that supersedes an earlier one **marks the
old entry SUPERSEDED rather than deleting it** (the convention D-BIND-33 set), so
someone who remembers the old rule can see why it changed and when. And an
acceptance bullet **names a file set, never a count** — counts are invalidated by
any rebase that adds a case upstream, silently (BIND-0 review, F1; observed twice).

**Validate a diagram edit by PARSING it**, never by eye: mermaid + jsdom, and check
the file for CR bytes. A stray `;` or CR breaks rendering silently, long after the
commit.

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

- **D-BIND-2 — ⛔ SUPERSEDED 2026-09-05 by D-BIND-2′, and re-verified against the
  shipped header 2026-09-18.** The development plan recorded the supersession in
  its §1 table from the start (*"D-BIND-2 is stale in wording … see D-BIND-2′"*);
  **this digest did not**, and since this is the file later stages are pointed at,
  a reader who consulted only the entry below got the REJECTED design as a locked
  requirement — complete with a STOP-AND-ASK threat for doing the right thing.
  Found by Copilot's review of #129. BIND-1 built the header correctly anyway, but
  an implementer knowing better than the digest is not a control, and BIND-Rust
  binds the same ABI next round off this file.

  **What the rule actually is — D-BIND-2′:** the binding ABI is derived from the
  **seam spec and from nothing else**. The C spellings of `Blob`, `Attachments`,
  `SharedSchema`, `SchemaArrival`, the write-buffer window and writer, topic
  segments, selector and document, and the status enum are each **constructed from
  the C++ value** on BIND's side, per §3.2's *"conceptual, never a memory image"*.
  **No header is shared with PDA-ABI**, and PDA-ABI's `fletcher_status` is never
  seen by C#. The binding ABI is pure C because P/Invoke requires it, not because
  the driver ABI is. Verified at BIND-1's spec review: no include, no shared struct
  tag, no layout claim.

  **What survives the supersession**, each re-checked against
  `c-abi/include/fletcher/abi/binding.h` rather than carried forward on trust:
  the two ABIs are **directional opposites**; **no driver data-plane function is
  callable by a binding** (reinforced by D-BIND-24, which also closes the registry
  door); the buffer is a **window plus a refill hook** rather than per-append
  function pointers, so a publish crosses **once per window refill, not once per
  append**; bytes below `pos` may not move except inside a refill, which preserves
  them verbatim; and bounds are computed by **subtraction, never addition**.

  **What did NOT survive, beyond the shared-header rule:** the old entry said the
  driver ABI has **three roles** and that bindings use role 3 — it has **two**, and
  selection is seam surface. It named `fletcher/abi/driver.h` as though it existed;
  there is no such header in the tree. And it listed the window as
  `{ctx, data, capacity, pos, grow, grow_zeros}` with *"`grow_zeros` is part of the
  vocabulary"* — the shipped `fl_write_window` has **one** hook, `grow`. That entry
  asked to be *"re-verified at Stage A"*; this is that re-verification, and the
  answer is that two of its three concrete claims had moved.

  > ~~Original text, 2026-08-31, kept for history: *Per PDA-L5 … Bindings use role
  > 3 (driver management) only. BIND reuses verbatim: the buffer struct, the blob
  > `retain`/`release` protocol, `ArrowSchema` (C Data Interface), and the
  > error/version conventions. Designing a second buffer or blob representation →
  > STOP-AND-ASK.*~~ Nothing in the tree cited this entry except its own heading,
  > so replacing it broke no cross-reference — checked before rewriting.

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
  **AMENDED 2026-09-21 BY D-BIND-39 — the Dictionary clause is SUPERSEDED for the
  ENCODE/DECODE path.** The premise was checked and does not hold there: the DICT
  halt is a `nanoarrow_ipc` limitation, and the binding path never touches IPC —
  `fl_codec_open` takes an `ArrowSchema` over the C Data Interface and the shim
  references no nanoarrow IPC at all. The STOP-AND-ASK is answered by the same
  check: `docs/wire-format-specification.md` §"Dictionary Types" ALREADY fixes the
  representation (encode the value type, one value per row; refuse a non-scalar
  value type), so implementing it invents nothing. **DICT still owns** re-folding
  decoded values into a `DictionaryArray` and anything involving dictionary IPC.
  The enum half of this decision is untouched.

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
  ✅ **Amended 2026-09-11 (Q10).** Matrix re-baselined at `6c541e9`: **302** non-generator
  cases (Bucket 1: 104 · 2: 56 · 3: 38 · 4: **104**) and **98** generator cases, counted by
  `grep -c '^TEST\(_F\|_P\)\?('` per file (development plan §5). **Two exclusion classes,
  confirmed by the maintainer, both documented and neither silent:** (a) the 98 generator
  cases stay in C++ and are *extended* with `test_csharp_visitor` / `test_csharp_type_table`;
  (b) provider-internal cases — `test_fletcher_sample_pub_sub_type` (24, the Fast DDS
  `TypeSupport`) — test a C++ type no managed code touches and are not ported;
  `test_owned_schema` (1) stays excluded as before. Everything else ports. **18786 is not
  closed before Bucket 4 is green over `fastdds` and `xrce`.**

  ✅ **Arithmetic corrected 2026-09-18, denominator RULED BY THE MAINTAINER: 302, so 278
  port.** Two independent errors had made the derived figure *"275 of 300"*, low by three,
  and it had stood for seven weeks in four places including the round's own exit criteria.
  **(i)** The subtraction was wrong: the total is **buckets 1–4**, and `test_owned_schema`
  is **bucket 6**, so *"300 less the 24 provider-internal cases and `test_owned_schema`"*
  removed a case the total never held. **(ii)** Bucket 4 was carrying `test_xrce_document`
  at 9 where the file has 11 — BIND-0's review finding F1, propagating. The bucket figure
  above is corrected to 104 for the same reason.

  **Both readings of "non-generator" converge on 278 ported**, which is why only the
  denominator needed a ruling: counting bucket 6 in gives 303 − 24 − 1 = 278, counting it
  out gives 302 − 24 = 278. Ruled **302**, buckets 1–4, with the 24 provider-internal cases
  as the only subtraction from it. The round-exit bullet that carried this number now names
  the FILE SETS instead (`dd8b034`'s rule), so it cannot rot again. **ADO 18786** carried the old
  figure in its scope comment (id 22604) and is corrected there by comment **23048**; the
  story is **Active**, not closed — it is the one that *must not* be closed before the
  native-provider bucket is green over `fastdds` and `xrce`.

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

- **D-BIND-28 — NuGet packages are published to EIVA's internal feed first; NuGet.org is a
  later, separate step.** *LOCKED BY THE MAINTAINER 2026-09-14.* `cd.dotnet.yml` pushes the
  three packages and their `.snupkg` symbol packages to `https://nuget.eiva.com/v3/index.json`.
  Facts established 2026-09-14 and binding on BIND-9: the feed is a **BaGetter** server
  (Kestrel; service index exposes `PackagePublish/2.0.0` at `/api/v2/package` and
  `SymbolPackagePublish/4.9.0` at `/api/v2/symbol`); **reads are anonymous**, **publish requires
  an API key** (`PUT /api/v2/package` answers 401 without one), carried as a GitHub Actions
  secret and never in the tree; and the host resolves to a **private address (10.x)**, so
  GitHub-hosted runners cannot reach it — **ruled 2026-09-14: the publish job runs on a
  self-hosted runner inside the EIVA network, and `cd.dotnet.yml` mirrors the Conan CD
  workflows of this repository** (`cd.core.yml` and siblings: tag push → `setup-devcontainer`
  → the component's `ci.*.yml` → one publish job that verifies the tag version, downloads the
  packed artifacts, pushes them and creates the GitHub Release with them attached). Build,
  test and pack stay on GitHub-hosted runners; only the `publish` job moves, and it runs
  nothing but that job — never pull-request code, since the workflow is tag-triggered. The
  repo uses only GitHub-hosted runners today, so the runner's registration, label,
  `NUGET_EIVA_API_KEY` secret and upkeep are BIND-9 deliverables (tracker Part 5). Everything else in the CD design is unchanged:
  `verify-tag-version-dotnet`, `ContinuousIntegrationBuild`/`Deterministic`/SourceLink, no
  dist-tag step, `PackageLicenseExpression=LGPL-3.0-or-later`. NuGet.org Trusted Publishing
  (OIDC) and the `Eiva.Fletcher.*` prefix reservation are **deferred** to the day the packages
  go external; the workflow is written so that adding NuGet.org is a second `push` step, not a
  redesign. **Consequence for the LGPL review (D-BIND-13 note):** distribution through an
  internal feed to EIVA's own applications is not distribution to third parties, so the
  relinking question loses its urgency for this round; it must be answered *before* the first
  external publication, and the packages carry the licence files regardless (BIND-9).

- **D-BIND-29 — BIND does not wait for PR #128; the two branches interleave by item, not by
  round.** *LOCKED BY THE MAINTAINER 2026-09-15.* This is P-7's landing order, and it is ruled
  against the branch as it actually stands rather than as the 2026-08-31 plan described it.

  **What was checked on 2026-09-15** (re-derive before relying on any of it): PR #128 "FastDDS
  Modernization" is a **draft**, 79 files, **+10629/−639**, seven commits, opened 2026-09-14 and
  not pushed to since; CI 42 pass with `fastdds-pubsub-provider / build-linux` **red** on
  `FastDDSPubSubProviderTest.SubscribeFirstBurstDeliveredInOrder`; two automated reviews and no
  human review decision. Its merge base is `6c541e9`, one merge behind `main`.

  **Three of the plan's P-7 premises had expired.** (a) The `protoc` collision is no longer a
  re-do: commit `b95026c` rebased that work onto the IR, leaving **+416/−56** across seven files,
  of which BIND-6/7 touch only `generator.cpp`. (b) `SubscribeSchema` no longer carries a
  `std::shared_future` — it returns a `SchemaArrival`, so the D-BIND-22 conflict the plan feared
  is already resolved on the branch, and both schema-only methods are `virtual` **with default
  bodies**, i.e. an append-only extension. (c) A collision the plan never mentioned is live: #128
  bumps `pubsub`, `arrow-bridge`, `protoc` and the Fast DDS provider to **0.5.1-alpha**, and
  `c-abi/conanfile.py` pins three of those **exactly**.

  **The ruling, in four parts.**
  1. **#129 (BIND-0) lands first.** It is green and comparatively small, and three of its changes
     are foundational for both branches: `fPIC` on the static libraries (without which nothing
     links a shared object — #128 needs it the moment `c-abi/` is on `main`), the XRCE SIGPIPE
     fix in shipping code, and `*.c` under `ci.format-check-cpp`. #128's only textual overlap with
     it is a two-line comment in `xrce_dds_pubsub_provider.cpp`.
  2. **BIND-1 proceeds now and specifies the schema-watch pair into `binding.h`** — the C
     spellings of `SubscribeSchema`/`UnsubscribeSchema`, **declared and answering `kNotSupported`**
     until the seam provides them. That is the pattern already locked for path selectors until
     PDA-ABI (D-BIND-24), and it is what stops a header *reviewed as a specification* from being
     reopened at BIND-4. The cost of being wrong is bounded by the header's own pre-1.0 exemption.
  3. **BIND-2 is where the real dependency sits** — it proves the nanoarrow codec byte-identical
     to `arrow-bridge`'s `Codec`, which #128 rewrites (+404/−139, plus `BatchDecoder`). The order
     is re-examined **at the BIND-1 → BIND-2 boundary**, by which time BIND-1's spec-and-review has
     bought days for free: if #128 has landed, BIND-2 writes its oracle once against the final
     codec; if it has not, BIND-2 proceeds anyway and its byte-identity oracle becomes a standing
     guard that #128's rewrite moved no wire byte — a benefit to that branch, not a cost to this
     round.
  4. **Whoever lands second updates the pins.** Three `requires` lines in `c-abi/conanfile.py` go
     to `0.5.1-alpha`. Cheap, but it reddens the `c-abi` lane on the merge commit if unexpected,
     so #128 is told in advance.

  **Re-ruling triggers.** If #128's `SubscribeSchema` stops returning a `SchemaArrival` (or the
  pair stops being defaulted virtuals), part 2 is void and BIND-1 is a STOP-AND-ASK under
  D-BIND-22. If #128 is still unmerged when BIND-2 is ready to start, that is not a re-ruling —
  part 3 already says what happens.

  **What this ruling explicitly refuses:** making a freshly started round's critical path wait on
  a draft pull request of ten thousand lines that is red on Linux and has no human review
  decision. Waiting converts another branch's unknown schedule into this round's, and BIND-2 —
  the round's long pole — is the item that would idle.

- **D-BIND-30 — round BIND lands as ONE pull request; a fix that is not about the bindings
  goes to `main` on its own.** *LOCKED BY THE MAINTAINER 2026-09-16.* This reverses the plan's
  Branch strategy bullet ("several PRs, one per stage boundary"), which was written without
  checking what this repository actually does.

  **The evidence that settled it.** Every round here has landed as a single PR, squash-merged,
  leaving `main` with one commit per round: **#125** (GIR) +25105/−2633 across 143 files in
  **28** commits, open four days; **#126** (PDA-DEC) +60146/−2372 across 255 files in **100**
  commits, open five days. Small standalone fixes get their own PR instead — **#127**, +357/−13,
  opened and merged the same day. The BIND plan was the only document proposing per-stage PRs.

  **Why it is also right on the merits, not just by precedent:** BIND's intermediate states have
  no value on the trunk. What BIND-0 produces is an empty `c-abi/` component, three empty NuGet
  packages and two lanes that test almost nothing — scaffolding. Under one PR per round, `main`
  goes from "no C# bindings" to "C# bindings" once, and never carries a half-built one. Review
  granularity is untouched: in this project it lives per item in the tracker and `plans/reviews/`,
  which is where every round's review record already is.

  **The carve-out, and its test.** A defect the round *finds* in shipping code is not part of the
  round. It goes to `main` on its own small PR, #127-shaped, rather than waiting weeks for BIND-10.
  The test is whether the fix would still be wanted if the bindings were cancelled. BIND-0 set the
  precedent both ways: the **XRCE SIGPIPE fix** became **#130** (a live hazard for any
  `transport=tcp` consumer — wanted regardless), while `fPIC` and the `*.c` format-check glob
  stayed in the round (neither matters until `c-abi/` exists).

  **Consequences.** #129 is retitled to the round and stays a draft until BIND-10 closes, rebased
  onto `main` at item boundaries. #130 rides in #129 until it merges, then drops out on the next
  rebase (identical patch). The plan's Branch strategy section is corrected in the same commit.

- **D-BIND-31 — BIND-2c implements the codec surface AND the publisher chain; the subscriber
  half, attachments, blobs and schema arrival stay with BIND-4.** *LOCKED BY THE MAINTAINER
  2026-09-17.* The plan did not say who implements the shim's ~40 entry points, and the two
  readings available were three times apart in size.

  **The contradiction that forced the question.** `c-abi/src/builtins.cpp`, written at BIND-0,
  says *"DELETE THIS when `fl_registry_create` lands (**BIND-4**)"* — assigning provider creation
  to BIND-4. But BIND-2's own acceptance requires, in slice 2d, that
  `CopyAccounting.BindingProducerWritesInPlace` score `encode_copies == 0` for *"a producer
  through `fl_rows_bind` and `fl_publisher_publish_row`"* — which cannot run without a provider
  and a publisher. BIND-3's and BIND-4's acceptance is written entirely in MANAGED terms, so no
  item owned the native work either way.

  **What 2c therefore contains:** `fl_error` + `fl_error_dispose` and the one `Translate`
  containment site per direction (D-BIND-19 rule 1); the codec surface (`fl_codec_open/close`,
  `fl_rows_bind/unbind`, `fl_encode_row`, `fl_decode_rows`); the `fl_write_window` → `WriteBuffer`
  adapter and the `fl_writer_fn` thunk; and the minimum publisher chain that makes the fusion
  real — `fl_provider_create/destroy`, `fl_publisher_create/destroy`, `create_topic`,
  `list_topics`, `publish_raw`, `publish_row`, `publish_rows`, plus `fl_string_list_*`.

  **What 2c does NOT contain**, and BIND-4 gains: the whole subscriber surface, the attachments
  builder and accessors, blob retain/release, and `fl_schema_arrival_*`. The internal
  `fl_attachments` type is defined in 2c so `publish_row`'s signature is honoured rather than
  faked, but nothing can construct one until BIND-4's builder lands, so the shim accepts NULL
  (meaning none) and nothing else can reach it.

  **Why this boundary and not the literal one.** The round's own slicing rule is that a slice
  ends where a PROPERTY BECOMES PROVABLE. Under the codec-only reading, BIND-2's headline
  property — the publish fusion writing straight into the transport's window — would be written
  but unexercisable until BIND-4, and 2d's copy-oracle row would have to move out of BIND-2
  entirely. A slice that ships an untestable headline is not a slice boundary.

  **Consequences.** `builtins.cpp`'s "DELETE THIS … (BIND-4)" comment is corrected to BIND-2c in
  the same commit that references `BuiltinRegistry()` from `fl_provider_create` — which is the
  honest reference the comment asks for. BIND-4's acceptance keeps its managed wording; the
  native subscriber entry points are named there explicitly so the gap does not reopen.

- **D-BIND-34 - the copy oracle measures `fl_encode_row` writing into the provider's own
  window, not `fl_publisher_publish_row`.** *LOCKED BY THE MAINTAINER 2026-09-17.*

  **Why the acceptance as written is not constructible.** BIND-2's 2d bullet asks for *a producer
  through `fl_rows_bind` and `fl_publisher_publish_row`* scoring `encode_copies == 0`. The copy
  oracle samples `produced_at` from inside the `RowEncoder` frame, so it can only measure a
  producer it hands a `WriteBuffer` to. `SeamProbeProvider` - the instrumented provider the ledger
  is built around - is a `PubSubProvider` the conformance harness owns, and **D-BIND-24
  deliberately closed the registration door** that would let anything outside put it in the shim's
  builtin registry. `fl_publisher_publish_row` resolves its provider through `fl_provider_create`,
  so it can never publish into the probe, and it owns the encoder frame itself.

  **What is measured instead.** Inside the harness's `RowEncoder`, `AppendInPlace` lends a span of
  the probe's own window; an `fl_write_window` is built over that span and `fl_encode_row` is
  called against a real `fl_codec_open` + `fl_rows_bind`. That is the SHIPPED codec, reached
  through the SHIPPED C ABI, composing a row inside the transport's window.

  **What that retires and what it does not.** It retires the README's caveat in substance - the
  producer is no longer a stand-in written for the harness, it is the binding's own encoder behind
  the C boundary. It does not exercise the publisher wrapper. That step moves no bytes: `Publish`
  hands the provider's window to the same encoder, which is structural in `binding.cpp` and
  already covered by BIND-2c's tests. A copy claim the oracle cannot score is not improved by
  routing a test through code that does not touch the bytes.

  **Rejected:** a test-only registration seam in the shim, which would reopen D-BIND-24's door in
  the shipped artifact, against a round that has been strict about the shim exporting only what
  `binding.h` declares.

  **Consequences.** BIND-2's 2d acceptance is amended to name `fl_encode_row`; the test keeps the
  name `CopyAccounting.BindingProducerWritesInPlace`. `integration-tests/pubsub-conformance` gains
  a TEST-ONLY dependency on `fletcher-c-abi`, and its CI lane gains the matching `conan create` in
  the same commit - the lesson `fPIC` and `arrow-bridge` each cost a cycle.

- **D-BIND-33 — the self-hosted runner and `NUGET_EIVA_API_KEY` belong to BIND-9 only; BIND-0's
  copy of that bullet is struck as a DUPLICATE.** *LOCKED BY THE MAINTAINER 2026-09-17.*

  BIND-0's acceptance carried *"The self-hosted publish runner registered in the EIVA network and
  the `NUGET_EIVA_API_KEY` secret created on `nuget.eiva.com` (user actions, D-BIND-28)"*, marked
  ⚪ and annotated **"due before BIND-9"** — an item's acceptance holding something whose own
  deadline is a later item, which is a defect in how the bullet was placed rather than a debt
  BIND-0 owes.

  **It is not moved, because BIND-9 already has it**, and in a more complete form: *"The
  self-hosted runner registered, labelled, documented (what it runs, what it must have installed),
  and used by no other job"*, with the secret named in BIND-9's first bullet alongside
  `cd.dotnet.yml`'s publish job. So this strikes a duplicate; nothing is lost and nothing relaxes.

  **Why it matters beyond tidiness.** BIND-0 is the round's kickoff and every other item depends
  on it. Leaving it unclosable on a bullet nobody can act on before BIND-9 would have held the
  first item of the round open for the whole round, which makes 🟢 mean less everywhere else.

- **D-BIND-32 — an `fl_error` a CALLBACK fills is BORROWED to the shim: the callback keeps the
  message bytes alive until it returns, the shim copies what it needs and frees nothing.**
  *LOCKED BY THE MAINTAINER 2026-09-17,* answering BLOCKER B1 of BIND-1's spec review, and
  **stated on `fl_error` itself rather than on `fl_grow_fn`** so the next callback that takes an
  `fl_error*` inherits it instead of reopening the question.

  **The contradiction it closes.** `fl_error` says the message *"is heap-allocated by the shim"*
  and is released with `fl_error_dispose`; `fl_grow_fn`, 260 lines further down the same header,
  hands an `fl_error*` to a function the CALLER wrote. Both cannot hold. Under the first the shim
  allocates and frees; under the second the binding allocates and the shim's `delete[]` frees —
  and those are one heap only while both sides share a C runtime, which BIND-3 intends them not to
  (the shim was then expected to link the MSVC CRT statically; **D-BIND-38 kept it dynamic,
  and the rule stands on the stronger ground the header now states — allocator PROVENANCE,
  not runtime linkage: a `delete[]` is correct only for this shim's own `new[]`, whatever
  runtime either side links**). BIND-2c's first implementation called
  `fl_error_dispose` on a callback-filled error, which is heap corruption under the configuration
  the round means to ship.

  **Why the borrow reading and not the alternatives.** It is the discipline `fl_str` already uses
  everywhere it appears, so it adds no rule a binding author does not already know; it needs no new
  exports, where a caller-supplied disposer or shim-exported `fl_alloc`/`fl_free` both widen the
  surface; and it is satisfiable without allocating at all — static storage, or a buffer the
  binding owns for the duration of the call.

  **Performance, asked and answered.** The copy lands on the FAILURE path: `grow` returning
  non-`FL_OK` means the encode is aborting and a C++ exception is about to be thrown and a publish
  failed. One short-string construction is noise beside the throw it precedes, and nothing is
  published. The borrow-and-copy cost that IS on the hot path is a different one — `ToSegments`
  materialising a `std::vector<std::string>` per `fl_publisher_publish_row`, which
  `fl_publisher_publish_rows` already hoists out of its loop. That is risk **B-2**, and BIND-3's
  per-row publish benchmark decides it with numbers (**moved to BIND-4 on 2026-09-21 by
  D-BIND-37** — a publish benchmark needs a managed publisher). If it must be paid down, the fix is a
  pre-converted topic handle following this ABI's own open-once shape (`fl_codec_open` +
  `fl_rows_bind`), which is a pure addition and therefore append-only-safe — not something to add
  on suspicion.

  **Consequences.** The rule goes in `binding.h` on `fl_error`, with a cross-reference from
  `fl_grow_fn`; no signature, struct or enumerator changes, so no version bump and no append-only
  question. `WindowBuffer::Grow` already behaves this way and its "open question" comment becomes a
  statement of the rule. B1 of the spec review is answered; **B2 and B3 remain, so BIND-1 stays
  🔴.**

- **D-BIND-35 — BIND-2's byte-identity oracle runs over the codec's OWN Arrow fixture corpus;
  the acceptance's `emit_vectors` reference is struck as a wrong-shape corpus.**
  *LOCKED BY THE MAINTAINER 2026-09-18,* ruling deviation 1 of BIND-2's conformance review — the
  question BIND-2's state table parked *"for the BIND-2 review"* since 2a.

  **What the bullet asked for and why it cannot carry the property.** It asked for byte identity
  *"across the whole `emit_vectors` scenario corpus"*. Reading that corpus settles it two ways.
  **Shape:** `emit_vectors.cpp` encodes through the **protoc-generated row class**
  (`telemetry.fletcher.pb.h`, `row.Encode()`) and emits base64 for the TypeScript side to decode
  and re-encode. It is a *proto-row* corpus serving a cross-language check; BIND-2's codec takes an
  **Arrow array**, so running the bullet over it means hand-translating each scenario into an Arrow
  array first — after which the comparison is still nanoarrow against `arrow-bridge`, because that
  is the only other Arrow-array encoder in the tree. The generated C++ encoder never enters it.
  **Size:** three scenarios (`basic`, `zero`, `negative`) over ONE flat message — an int32, a
  double, a string, a bool and a repeated int32. No struct, no map, no fixed-size list, no
  timestamp, no duration, no null composite. Substituting it would REDUCE coverage on every axis
  the wire format has.

  **What runs instead, and why it is the stronger test.** Five hand-written Arrow fixtures of three
  rows each: the mapping's scalars at their extremes, timestamp and duration, a nested struct in
  its three distinct states, the three composites each populated / empty / null, and a fixed-size
  list (added by 2b because that arm of both switches was otherwise untested). Every arm of both
  switches is covered. The counts are asserted deliberately — 15 comparisons for byte identity, 5
  fixtures for the round trip — so the corpus cannot shrink silently.

  **What is NOT struck.** D-BIND-11's `emit_vectors` reference stands exactly where it was written:
  on the **cross-language generated-artifact property**, where a proto-row corpus is the right
  shape and the three arms are genuinely independent implementations. This ruling narrows one
  acceptance bullet, not D-BIND-11.

  **The gain deliberately not taken here.** Adding the three telemetry scenarios as a SIXTH fixture
  would chain nanoarrow ≡ `arrow-bridge` ≡ generated C++ ≡ TS over the same bytes, which no single
  test does today. It needs the generated C++ row class inside the c-abi test binary, so it belongs
  with **BIND-5/7**, where the generated tiers are the subject — not to BIND-2.

  **Consequences.** Acceptance bullet 4, Part 4's test-matrix row and development plan §3.2's
  forcing test are restated against the fixture corpus; no code and no test changes.

- **D-BIND-36 — the wire format's map keys are SCALAR ONLY, refused at `fl_codec_open` by field
  name.** *LOCKED BY THE MAINTAINER 2026-09-18,* ruling deviation 2 of BIND-2's conformance review
  — the narrowing BIND-2b introduced over what BIND-2a's encoder accepted, raised under the round's
  STOP-AND-ASK rule.

  **Why the decoder cannot take a composite key.** The wire is `[COUNT][keys][value bitfield]
  [values]`: a map's keys ALL arrive before its values. nanoarrow finishes a struct element only
  when every child is exactly one longer than the struct, so an entry's key and value must be
  appended together and the keys have to be staged until their values show up. A scalar stages as a
  pointer into the row buffer (`ScalarValue`); a composite key would have to stage a half-built
  array, which is a second decoder.

  **Why refusing costs nothing.** Proto restricts map keys to integral, bool and string, so the
  mapping this ABI exists to serve produces none. The refusal is at OPEN rather than at row 4711,
  and it names the field.

  **Why a decision and not a note.** It is a statement about the wire format's REACH, not about one
  codec's convenience: BIND-Rust binds the same codec next round and inherits the same constraint,
  and an encoder that can write what no decoder can read is the kind of asymmetry that gets
  "fixed" by someone who does not know why it is there. Pinned by
  `NanoarrowCodec.RefusesAMapWithACompositeKeyNamingTheField`.

  **Consequences.** None in code — the refusal and its test already exist. BIND-2's deviation 2
  closes; `docs/wire-format-specification.md` is NOT amended by this ruling (the option was
  offered and not taken), so the constraint lives in this digest and in the refusal's own message.

- **D-BIND-37 — BIND-3's per-row publish benchmark moves to BIND-4, whole.**
  *LOCKED BY THE MAINTAINER 2026-09-21,* raised while slicing BIND-3.

  **Why it could not stay.** The bullet asks for a benchmark *"against the C++ generated
  publisher over `inprocess`"*, and a **publish** benchmark needs a managed publish path.
  The managed `Publisher` is BIND-4's by the tracker's own item split. More pointedly, the
  risk it exists to settle is **B-2** — `ToSegments` materialising a `std::vector<std::string>`
  on every `fl_publisher_publish_row` — which is a per-publish cost and therefore not
  measurable at all before a publisher exists. The bullet was asking BIND-3 to measure
  something BIND-3 cannot reach.

  **The alternatives, and why they were declined.** Benchmarking `fl_encode_row` alone in
  BIND-3 would have produced a number, but not the one B-2 turns on. Giving BIND-3 a
  minimal publish path would have had it straddle the item boundary — the same shape
  D-BIND-31 had to rule for BIND-2c, and worth avoiding twice.

  **The accepted cost, stated rather than hidden:** BIND-3 now carries **no performance
  evidence at all**. Its acceptance is correctness and conformance only, and the first
  numbers for the managed path arrive at BIND-4. If the per-row cost turns out to be
  unacceptable there, the only faster route is generated C# writing wire bytes, which is a
  **D-BIND-1 STOP-AND-ASK** and not a local fix — unchanged by this move, only relocated.

  **Consequences.** The bullet moves verbatim into BIND-4's acceptance; both item tables and
  the development plan's B-2 entry (*"measure and decide in BIND-4"*, previously split
  across the two) follow. No code, no interface and no diagram changes — the four-places
  rule's fourth place does not apply, checked.

- **D-BIND-38 — the shim keeps the DYNAMIC MSVC CRT; the static-CRT question moves to
  BIND-9.** *LOCKED BY THE MAINTAINER 2026-09-21,* raised at the start of BIND-3a when the
  one-line acceptance clause turned out to be a profile-wide change.

  **What the clause actually asked for.** `compiler.runtime` is a Conan PROFILE setting, not
  a CMake property, so "the shim links the MSVC CRT statically" means rebuilding the shim
  **and every static library inside it** — `fast-dds`, `fast-cdr`, `foonathan-memory`,
  `tinyxml2`, `asio`, the four Fletcher components and their transitives — from source,
  because objects with mismatched runtimes cannot be linked into one binary. With the C++
  tests staying on dynamic that is two full builds of the eProsima chain per lane that
  touches the shim, carried through seven remaining items.

  **Why it buys almost nothing here.** The managed boundary never crosses heaps either way:
  C# does not `free()` native memory, it calls `fl_error_dispose`, which allocates and
  releases inside the shim. And a static CRT does not remove the two-heaps hazard — **it
  creates it**, between the shim and any other native module in the process, which is
  exactly what B1 found in `test_containment.cpp`. Staying dynamic removes that class rather
  than deferring it. Against it: a static CRT means every CRT security fix needs the shim
  rebuilt and reshipped rather than flowing through Windows Update, the shim grows, and it
  diverges from every other component in this repository.

  **What it does buy is a packaging property** — possibly no VC++ redistributable
  prerequisite on an end user's machine — and packaging is BIND-9's: the RID matrix, the
  packed-size budget, the licence and relinking question and the NuGet README all live
  there. **Moved there, to be decided with packaging in hand and evidence about what the
  .NET runtime's own install already guarantees, rather than assumed now.**

  **The consequence that is not free, and is handled rather than ignored:** D-BIND-32's
  recorded justification argued from the static CRT. That premise is gone. The RULE does not
  move — it now rests on allocator **provenance**, which is stronger and independent of
  packaging: the shim's `delete[]` is correct only for memory the shim's own `new[]`
  produced, and a grow callback may hand over a pinned managed buffer, a `malloc` block, a
  Rust allocation or static storage, none of which a `delete[]` releases however either side
  links its runtime. Restated in `binding.h`, in `write_window.hpp` and on D-BIND-32 itself.
  B1's fix stays for the same reason: two separately linked copies pairing one's `new[]`
  with the other's `delete[]` is wrong under a shared runtime too.

  **Consequences.** BIND-3's acceptance loses the clause; BIND-9's gains the question. No
  profile changes, no rebuild, no size re-measurement. Comment-only changes in `binding.h`
  — no signature, enumerator or struct moved, so the append-only rule does not bind and no
  version bump is required.

- **D-BIND-39 — a BINDING carries the PROTO MAPPING; the shim's codec implements the spec's
  dictionary rule; bucket 1 is not a binding port target.** *LOCKED BY THE MAINTAINER
  2026-09-21,* raised at the start of BIND-3d when "port bucket 1 to C#" turned out to be
  asking the binding to reimplement a tier it does not serve. The maintainer's question was
  the right one and is recorded because it is the question every future binding will ask:
  **must C++, C# and Rust support identical types?**

  **THE ANSWER IS YES, AND ON THE GENERATED PATH IT IS ALREADY TRUE** — but only once the
  two codecs are named apart, which no BIND document did until now:

  | | `arrow-bridge::Codec` | `NanoarrowCodec` (the shim) |
  |---|---|---|
  | Tier | **Arrow-native** | **the binding tier** |
  | Callers | hand-written C++ that already holds arbitrary Arrow data | every binding — C# today, Rust next round, through `fl_codec_open` |
  | Type set | everything the wire format can frame: unions, decimals, intervals, half-float, string/binary view, fixed-size binary | the proto mapping, and a little more |
  | Tested by | `test_codec` 35, `test_codec_edge` 23, `test_codec_property_fuzz` 2 — **this is bucket 1** | `test_nanoarrow_codec`, `test_binding_entry_points` |

  **Checked field by field against `docs/wire-format-specification.md` §"Proto to Arrow Type
  Mapping":** the mapping produces `bool`, `int32/64`, `uint32/64`, `float/double`, `utf8`,
  `binary`, `enum`→int32, `timestamp(ns)`, `duration(ns)`, the eleven nullable wrappers,
  `struct`, `list`, `map`. The shim's codec accepts **every one of them and more**
  (`int8/16`, `uint8/16`, `large_string`, `large_binary`, `date32/64`, `time32/64`,
  `fixed_size_list`). So generated C++, C#, TS and Rust already agree, and **no binding is
  short of a type its generator can emit.**

  **The types bucket 1 tests that the binding refuses are not a parity gap, because no
  generator in any language can emit them.** `oneof` is in the spec's own §"Unsupported
  Types" table ("Arrow union is not supported by Parquet and has limited compute kernel
  coverage"), so unions reach the codec only from hand-written Arrow. Decimals, intervals,
  half-float and the view types have no proto construct at all. Widening the shim's codec to
  carry them would build support no `.proto` can reach — considered and rejected.

  **THE ONE REAL DIVERGENCE IS DICTIONARIES, AND IT IS FROM THE SPEC, NOT FROM AN ACCEPTANCE
  BULLET.** §"Dictionary Types" is normative: a `DICTIONARY` field "is encoded as its **value
  type**, one value per row — the indices are a columnar optimization that has no meaning in
  a single row", and the value type must be scalar, with nested value types "rejected with a
  clear error". `arrow-bridge` implements exactly that
  (`CodecTest.DictionaryFieldTransfersValueType`, `.DictionaryScalarEncodesResolvedValue`,
  `.DictionaryNestedValueTypeRejected`). **The shim's codec refuses every dictionary at
  open**, so a C# caller cannot send data a C++ caller can, over the same wire format. That
  is the parity failure the maintainer's question was pointing at.

  **The premise of the deferral does not hold, and this was VERIFIED rather than argued.**
  D-BIND-8 defers dictionaries because "the DICT round halted after DICT-2 (nanoarrow IPC
  rejects dictionary types)". That is a limitation of **`nanoarrow_ipc`**, and the binding
  path never reaches it: `fl_codec_open` takes an `ArrowSchema` over the **C Data
  Interface**, and **the shim uses no nanoarrow IPC at all** — zero references to it in
  `c-abi/src/**` or `c-abi/conanfile.py`. nanoarrow's own header carries the whole facility
  this needs: `ArrowArrayView.dictionary` is a view of the values, `storage_type` is the
  index type, and `ArrowArrayViewInitFromSchema` builds the tree. BIND-3c supplies the
  empirical half: `CodecTests.ADictionaryColumnIsRefusedByName` passes, which means nanoarrow
  parsed a dictionary schema over C Data and reached the refusal **by field name**.

  **D-BIND-8 IS AMENDED, NOT OVERTURNED** (marked rather than deleted, per D-BIND-33). Its
  Dictionary clause is superseded **for the encode/decode path only**, on the ground that
  settles its own STOP-AND-ASK: the spec ALREADY fixes the representation, so implementing it
  invents nothing. **DICT still owns** re-folding decoded values back into a
  `DictionaryArray` (the batched subscriber, BIND-5) and anything involving dictionary IPC.

  **Consequences.**
  1. The shim's codec gains dictionary support per the spec: plan a dictionary field as its
     **value type**, refuse a non-scalar value type naming the field, resolve index→value at
     encode. C++ cases mirroring `arrow-bridge`'s three.
  2. **A decoded dictionary field comes back as its VALUE type**, so a codec's bind schema and
     its decode schema are no longer the same object. The managed tier exposes both; nothing
     may assume `DecodeBatch` returns rows shaped like `Schema`.
  3. BIND-3c's `ADictionaryColumnIsRefusedByName` is **replaced** by a round-trip case plus
     the nested-value-type refusal. A refusal test for behaviour that is now supported would
     lock in the defect.
  4. **Bucket 1 stops being a BIND-3 port target.** It is `arrow-bridge`'s Arrow-native suite.
     BIND-3's obligation is restated as: **every type the PROTO MAPPING produces round-trips
     through the binding** — the same suite C#, Rust and TS each run, which is what makes the
     parity claim testable rather than asserted. `test_envelope` (11) was already BIND-8's by
     §2.6 and `WriteBufferInPlace`'s writer cases are BIND-4's by surface.
  5. The two-tier table above is the record. It is written here, in the tracker, in the dev
     plan and in the diagrams' support table, because its absence is what let "port bucket 1"
     stand unchallenged for six weeks.

- **D-BIND-40 — `binding-abi-conformance` reuses the CODEC'S OWN corpus, emitted by C++ on
  every run.** *LOCKED BY THE MAINTAINER 2026-09-21,* the open corpus question from D-BIND-39's
  session. The acceptance already said "reusing the existing corpus rather than a C#-only
  fixture"; what was open was WHICH corpus, and the two candidates were not equivalent.

  **Ruled: the five Arrow fixtures in `codec_corpus.hpp`** — `scalars`, `temporal`, `nested`,
  `composites`, `fixed_size` — the same objects `NanoarrowCodec.ByteIdenticalToArrowBridge`
  compares the two C++ encoders over. **Rejected: `emit_vectors.cpp`'s scenario corpus**, for
  the reason D-BIND-35 already gave when it struck that reference from BIND-2's acceptance —
  it encodes through the protoc-generated row class, three scenarios over one flat message, so
  it is the wrong SHAPE and narrower.

  **The corpus is INCLUDED, not copied.** `codec_corpus.hpp` was lifted out of
  `test_nanoarrow_codec.cpp` so one definition serves two consumers; the emitter includes it
  across the tree and the c-abi test keeps using it unchanged. A second copy would drift, and
  both copies would keep passing while covering different things — which is worse than no
  corpus, because it reads as coverage. The header is gtest-free and reports a failure by
  THROWING, because one consumer is a gtest binary and the other is a plain executable, and a
  fixture that cannot be built must not quietly become an empty batch.

  **What the suite can and cannot ask, stated because it is easy to overclaim.** Every binding
  calls ONE codec, so "do C# and C++ encode the same bytes" is not a question it could fail.
  The question it DOES ask is one layer up: **two independent Arrow implementations build the
  same logical batch — do they hand that codec the same thing?** Arrow C++ builds the fixtures
  and writes them as IPC; `Apache.Arrow` rebuilds its own arrays from that IPC and exports them
  across the C Data Interface. Offsets, validity bitmaps, child ordering, buffer padding and
  the empty-vs-null distinction all have to agree, or the codec reads the difference onto the
  wire as a subscriber decoding into the wrong values. Correctness of the bytes stays
  `ByteIdenticalToArrowBridge`'s job, in C++, against a genuinely second implementation.

  **Emitted on every run, never committed.** A committed corpus is a golden the thing it
  checks is free to drift away from. The C# side DISCOVERS its cases from the emitter's
  `manifest.txt`, so a fixture added to the corpus is exercised without anyone remembering to
  add it — and `TheCorpusWasEmitted` is the vacuity guard, because both theories are driven by
  a file and a theory with no cases passes.

  **The reference bytes come through the C ABI**, not through `NanoarrowCodec` directly: this
  is the BINDING ABI's conformance suite, a binding reaches the codec only through
  `fl_codec_open`/`fl_rows_bind`/`fl_encode_row`, and producing the reference any other way
  would certify a path no binding takes.

  **No diagram change** (the fourth place): the deployment view draws the packages a consumer
  takes, and this is a test component that ships in nothing.
