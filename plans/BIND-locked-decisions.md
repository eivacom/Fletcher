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
  ✅ **Settled 2026-10-15 (BIND-7a), by measurement: arrow-dotnet is C++'s case.** On
  Apache.Arrow 23, `StructArray.Fields` returns every child already windowed to the struct's
  `[offset, len)`: a struct sliced at offset 1, length 3 gives children at offset 1, length 3,
  their null counts adjusted. **The C# accessor does NOT re-slice.** Forcing test:
  `GeneratedAccessorTests.AStructArrayAtANonZeroOffsetReadsItsOwnRows` (nested structs included);
  a mutant that re-slices fails it. (BIND-6c-2 had already seen it for the model's `FromArrow`.)

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
  *(Clarified 2026-09-29 by D-BIND-71, which was that STOP-AND-ASK: the three are the dotnet
  component's. `Eiva.Fletcher.Protoc` belongs to the protoc component, as the npm shim does, and
  does not count against them.)*
  *(Amended by D-BIND-74, 2026-10-06: FOUR managed packages. The fourth, `Eiva.Fletcher.Model`
  (`Apache.Arrow` only, the lossless `Timestamp` and `Duration`), was that STOP-AND-ASK, ruled.
  Marked here 2026-10-14, review T-D9.)*

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

  **AMENDED 2026-09-25 BY D-BIND-50 AND D-BIND-51** (the BIND-4 review, B1 and B2). Two
  premises above were false. "Whoever brings the counter to zero after retirement frees"
  equated the seam's *begins* — passing the gate — with the managed counter's increment,
  which comes later; on the carve-out a SIBLING's `GCHandle` could be freed under a delivery
  on another thread. The free is now deferred to a second cancel from outside any delivery,
  which the seam makes wait for the drain (D-BIND-50). And the marker named one Subscriber
  where the seam's refusal is per PROVIDER and deliveries nest; it is now a per-thread stack
  of delivery frames, and the refusals ask whether any frame is on the same provider
  (D-BIND-51). Everything else in this entry stands.

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
accessors do, for capstone parity (Q18). *(Amended by D-BIND-95, 2026-10-14: C++ and Rust in fact
expose the schema metadata and per-field metadata by position, not `Metadata(key)`; C# mirrors them
and keeps `Metadata(key)` as a shortcut.)*

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

  **PART 2 SUPERSEDED 2026-09-25 BY D-BIND-52.** The seam grew the pair (#128, merged into
  this branch), so the shim no longer answers `kNotSupported`: it forwards to
  `Subscriber::SubscribeSchema`, ABI 0.4 → 0.5. Exposing it in C# is owed to a later ruling -
  **made by D-BIND-57**, which also brought the seam's per-topic options across.
  Parts 1, 3 and 4 stand as history.

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

  **Exceptions the maintainer has made to the carve-out**, each knowingly, so `main` keeps the
  defect until #129 lands:
  - **2026-09-25, D-BIND-53 as amended:** Fast DDS's silent drop of an oversized row.
  - **2026-10-02, the TypeScript topic path for a dotted package.** For `package eiva.nav;` the
    TypeScript backend emitted `'eiva/nav/Svc/Method'`, which the gateway splits into four
    segments, while C++ registers `{"eiva.nav", "Svc", "Method"}`: two different topics. Reproduced
    by `TopicPath.DottedPackageAgreesAcrossBackends`. The maintainer ruled C++'s form canonical
    (the package is one segment, dots kept), so the TypeScript backend changed. By the carve-out's
    own test this fix would go to `main` on its own (it is wanted without the bindings), but the
    maintainer put it in #129 because C#'s topics depend on the same rule (D-BIND-72 point 6). **Known cost, accepted:** until #129 lands, TypeScript generated on
    `main` from a dotted package with a service still cannot reach a C++ publisher.

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

- **D-BIND-41 — the glibc floor moves to BIND-9, and is MEASURED rather than stated.**
  *LOCKED BY THE MAINTAINER 2026-09-21,* raised at the end of BIND-3d by the right question:
  why do we need it at all?

  **Because glibc is backward compatible and not forward compatible, and NuGet has no fallback.**
  The shim builds in the devcontainer (`FROM ubuntu:24.04`, glibc 2.39) and links versioned
  symbols, so it will not load on glibc 2.31 (Ubuntu 20.04, Debian 11) or 2.28 (RHEL 8). The
  C++ side never had this problem and that is not luck: the `cd.*` lanes ship **Conan
  packages**, and a consumer whose profile does not match simply rebuilds from source — the
  mismatch is self-healing. A NuGet package has none of that. The `.so` under
  `runtimes/linux-x64/native/` is taken as-is or not at all, and .NET supports distributions
  older than this repository's build image, so a consumer can be fully supported by Microsoft
  and still fail at the **first P/Invoke, at run time, on their own machine**.

  **But it is a statement about a PACKAGE, and BIND-3 packages nothing.** Writing the floor
  into BIND-3 would have documented an artifact that does not exist for six more items. So the
  clause moves to BIND-9, beside the RID matrix, the packed-size budget and the CRT question
  D-BIND-38 already moved there.

  **What replaces it is stronger than the sentence it removes.** `ci.c-abi.yml`'s Linux leg now
  prints the highest `GLIBC_x.y` the built shim references — the oldest distribution that can
  load it — on every run, in the log and the step summary. **Printed, not enforced:** no floor
  is ruled, so there is nothing to fail against yet. This is the size ceiling's habit applied
  to a second property (*"each leg now prints baseline + headroom so growth shows as a number
  before it shows as a red build"*), and it means BIND-9 will state a MEASURED number instead
  of inferring one from the build image — which is usually wrong in the safe direction, since
  the symbols a binary actually reaches are older than the glibc it was built against.

  **If the measured floor turns out too high, the fix is the BUILD IMAGE** (manylinux, or an
  older Ubuntu), which is exactly why the question was worth asking now even though the answer
  lands later: changing the image after six more items is more disruptive than choosing it.
  Nothing in this ruling changes the image today.

  **No diagram change** (the fourth place): no shape, name, count or direction moves.

- **D-BIND-42 — `fl_blob_create` is added, and the ABI minor goes 1 → 2.** *LOCKED BY THE
  MAINTAINER 2026-09-21,* raised mid-BIND-4a when a test that should have been routine turned
  out to be unwritable.

  **THE SURFACE HAD NO WAY TO MAKE A BLOB.** All three blob-adjacent entry points assume you
  already hold one: `fl_blob_retain` and `fl_blob_release` operate on a blob, and
  `fl_attachments_value_at` / `_find` hand back one BORROWED from an existing set. Meanwhile
  `fl_attachments_builder_set` takes a `const fl_blob*` and RETAINS it, and `fl_blob`'s own
  rules refuse a non-NULL `data` with a NULL `owner` — "there is deliberately no view-only
  form". Put together: **a binding could only ever attach a blob it had RECEIVED from a
  delivery.** A publisher attaching a trace id of its own had nothing to call, which left
  BIND-4's "`Attachments` … a builder on publish" unreachable and the whole write end of the
  attachments surface dead code.

  **Found by writing the test, not by reading the header.** The read end was implemented and
  green over empty-valued entries before the gap surfaced; it surfaced when a valued entry
  needed a blob and there was no call that produced one. Worth recording as the mechanism: the
  header reviewed cleanly at BIND-1 because every declaration is individually sensible, and
  what was missing was a declaration nobody thought to look for.

  **The fix is an ADDITION, not a relaxation.** The alternative considered and rejected was
  letting `fl_attachments_builder_set` COPY when `owner` is NULL: that removes the new entry
  point at the cost of making the no-view-only-blob rule conditional on which function you are
  calling, and the header states that rule absolutely. `fl_blob_create` COPIES the caller's
  bytes and hands back a blob the caller owns and releases — the copy is the point, because it
  is what gives the bytes an owner whose lifetime the caller no longer has to reason about.
  Attachments are sidecar metadata; the row payload's zero-copy path is
  `fl_publisher_publish_row` and is untouched.

  **THE MINOR IS BUMPED BECAUSE THE HEADER'S OWN RULE SAYS SO:** *"any declaration here may
  change shape or disappear between 0.x releases … what the exemption does NOT license is a
  silent change — the version above is bumped, and the change is recorded in the round's
  decision log."* This is that record.

  **The bump has a consequence that must land in the SAME commit, and it does:** the managed
  handshake is an EXACT match while major is 0 (not `>=`), so `NativeLoader.HeaderVersionMinor`
  moves with it or every managed test fails at load. It also broke
  `TheHandshakeRefusesAnyOtherVersion`, which listed `(0, 2)` as "a newer minor, refused" — and
  `(0, 2)` is now the real version. **The test failing was correct; its cases being literals
  was not.** They are now derived as offsets from `NativeLoader.HeaderVersionMajor/Minor`, so
  the next bump moves them rather than breaking them.

  **No diagram change** (the fourth place): no shape, name, count or direction moves.

- **D-BIND-43 — `fl_schema_retain` is added, and the ABI minor goes 2 → 3.** *LOCKED BY THE
  MAINTAINER 2026-09-21,* raised while writing BIND-4a's delivery thunk. **D-BIND-42's
  mechanism, a second time, one slice later** — and that it recurred is the finding, not the
  coincidence.

  **THE DELIVERY CONTRACT ASKED FOR A CALL THAT DID NOT EXIST.** `fl_delivery_fn`'s
  documentation says every pointer is borrowed for the duration of the call and spells out what
  a handler does to keep each one: *"A handler that keeps the bytes copies them; one that keeps
  the schema retains it; one that keeps an attachment's blob retains that."* Two of those three
  were callable. `fl_schema_release` existed with **no counterpart anywhere in the header** —
  the only `retain` declared was `fl_blob_retain` — so a handler that wanted the schema past
  its frame had no sanctioned way to say so, and the one route left (go back to
  `fl_schema_arrival_wait`, which does hand out a new reference) requires holding an arrival a
  delivery handler need not have.

  **Found the same way D-BIND-42 was: by writing the code that has to call it.** The header
  reviewed cleanly at BIND-1 for the same reason as before — every declaration is individually
  sensible, and a MISSING one is invisible to a reader going declaration by declaration. What
  makes this instance sharper is that the surface was not merely incomplete but internally
  inconsistent: a release with no retain is a pair with one half, and nothing in a
  declaration-by-declaration review asks "what pairs with this?". **The standing lesson: an
  owner-handle type is reviewed as a PAIR, and a round that adds one should grep for its
  counterpart.**

  **Rejected alternative: correct the documentation instead.** That reading — a delivery
  handler may not keep the schema, and the durable route is the arrival — is coherent, cheaper,
  and needs no bump. It was rejected because it makes the schema the one borrowed thing in a
  delivery a handler cannot keep, for no reason the transport imposes, and because `fl_blob`
  and `fl_schema` are deliberately the same shape with the same idiom; giving them different
  rules about retaining would be a difference a binding author has to memorise rather than
  derive.

  **The two coupled consequences are the same as D-BIND-42's and land in the same commit:**
  `NativeLoader.HeaderVersionMinor` moves 2 → 3 (the managed handshake is an EXACT match while
  major is 0), and `TheHandshakeRefusesAnyOtherVersion` follows for free — its cases were
  derived as offsets from `HeaderVersionMajor/Minor` by D-BIND-42 precisely so the next bump
  would move them rather than break them. **It did. That is the first evidence the derivation
  was worth making.**

  **No diagram change** (the fourth place): no shape, name, count or direction moves.

- **D-BIND-44 — the managed `AttachmentsBuilder` OWNS its entries; the native builder is a
  transient it repopulates, and the sealed set is cached.** *LOCKED 2026-09-22 at the
  maintainer's direction,* raised during BIND-4b when two facts that are each correct on their
  own turned out to collide.

  **THE COLLISION.** `fl_attachments_builder_build` MOVES the pending entries into the sealed
  set and resets the builder (`*out = new fl_attachments(std::move(builder->pending));
  builder->pending = fletcher::Attachments();`), so a second build on the same builder yields an
  EMPTY set. Meanwhile the frozen public surface gives `AttachmentsBuilder` only `Set`, `Clear`
  and `Count` — **there is no `Build()`** — and every publish overload takes an
  `AttachmentsBuilder?`. Put together, the publish has to seal the caller's builder itself, and
  sealing it empties it.

  **WHAT THAT WOULD HAVE DONE, and why it is the round's worst failure shape.** A caller
  publishing ten rows with one builder would have attachments on row 0 and none on rows 1–9.
  **Silently**, because an empty attachments set is a legal thing to publish: no status, no
  refusal, nothing at either end to notice. It is the same class as the dictionary divergence
  and the sparse-checkout vacuity — a wrong answer that reads exactly like a right one.

  **THE DESIGN.** The entries live in MANAGED memory as key/value byte copies. `Build` is
  `internal`, creates a fresh `fl_attachments_builder*`, repopulates it from the managed
  entries, seals it, disposes the transient, and CACHES the resulting `fl_attachments*`.
  `Set` and `Clear` drop the cache. So N publishes with unchanged attachments cost ONE seal, a
  mutation between publishes is picked up rather than ignored, and the caller's builder is never
  emptied by an operation that does not say it empties things.

  **TWO ALTERNATIVES, both rejected.** (1) *Expose `Build()` and have the caller hold the sealed
  set.* This is arguably the cleaner model and it is what C++ does — but it changes a signature
  the public-surface document froze, and it moves a lifetime onto the caller for a type whose
  whole job is to be convenient. (2) *Seal per publish with no managed entries.* This is the
  literal reading of the frozen surface and it is the silent-loss behaviour above. Rejected on
  sight.

  **WHAT IT COSTS, stated rather than buried:** the value bytes are copied twice on the path
  from caller to wire — once into the builder's managed entry at `Set`, once by
  `fl_blob_create` at seal. The second copy is the ABI's own rule (D-BIND-42: there is
  deliberately no view-only blob), so only the first is ours. It happens once per mutation, not
  per publish, and attachments are sidecar metadata — the row payload's zero-copy path is
  `fl_publisher_publish_row` and is untouched by any of this.

  **NO ABI CHANGE AND NO VERSION BUMP.** Unlike D-BIND-42 and D-BIND-43, which this superficially
  resembles, nothing native moves: the header is right, the shim is right, and the semantics that
  bite are documented in `binding.h` already. **The difference is worth naming, because all three
  were found the same way and only two are the same kind of defect.** 42 and 43 were MISSING
  DECLARATIONS — the surface could not express something it described. This one is a collision
  between a correct native contract and a managed signature frozen without knowledge of it, and
  the fix belongs entirely on the managed side. A round that files all three under "the header
  was wrong" would go looking for a header change that is not owed.

  **The guard is a test, not the comment.** `SealingTwiceWithoutAMutationReusesTheSameSet` and
  `AMutationAfterSealingIsPickedUpRatherThanIgnored` pin both halves — correct reuse, and reuse
  that does not become a stale read. Without the second, the cache would be a new way to publish
  the wrong attachments.

  **No diagram change** (the fourth place): `AttachmentsBuilder` keeps the members the surface
  document draws, and `Build` is internal, so nothing public moves.

- **D-BIND-45 — the `RowWriter` publish overload takes `minBytes` EXPLICITLY, and the frozen
  surface is amended to match.** *LOCKED BY THE MAINTAINER 2026-09-22,* raised during BIND-4b-iii
  when the writer path turned out to need a number the public-surface document had nowhere to
  put.

  **THE GAP.** `fl_publisher_publish_raw` takes a `min_bytes`, and the seam makes room for it
  BEFORE the writer is invoked: `AppendInPlace` forces a refill if the window is short, and
  refuses with kPayloadTooLarge if the room cannot be produced — in which case the writer is not
  called at all. `min_bytes == 0` is refused outright ("a fill of no bytes names nothing").
  The public-surface document froze `Publish(TopicPath, RowWriter)` with no size argument.

  **WHY IT CANNOT BE DEFAULTED HONESTLY, which is the whole decision.** The writer is handed
  `room`, the WHOLE remaining window, and `room >= min_bytes` — that is the ABI's promise and it
  is what lets a variable-length producer avoid a second crossing. But a writer that needs more
  than `room` has NO RECOURSE: it cannot ask for more, and reporting more than `room` is a
  refusal that commits nothing. So a default that is too small does not degrade, it fails, and
  it fails at a size that depends on the transport's window rather than on anything the caller
  wrote. A caller stating the room they need is the only form that is not a guess.

  **THIS ONE CHANGES THE PUBLIC SURFACE, and that is why it is recorded rather than left in a
  file header.** D-BIND-44 was the same SHAPE — a managed signature frozen without knowledge of
  a native requirement — but its fix was entirely behind the surface, so the frozen table stayed
  true. This fix is visible: `Publish(TopicPath, RowWriter, int minBytes, AttachmentsBuilder?)`.
  **The surface document is amended in the same commit**, both the §2.4 row and the class
  diagram, because a frozen table that disagrees with the shipped API is worse than no table —
  the next reader cannot tell which one is current, and the table is the thing they will trust.

  **The addition is source-compatible in form but not in fact, and the distinction is worth
  stating:** `minBytes` sits before the optional `attachments`, so it is a required argument and
  a caller writing `Publish(topic, writer)` does not compile. That is deliberate. Making it
  optional would have reintroduced the guess it exists to remove, and there is no caller to
  break — nothing outside this round has ever compiled against this surface.

  **NO ABI CHANGE AND NO VERSION BUMP.** Like D-BIND-44 and unlike D-BIND-42/43, nothing native
  moves: the header is right and `fl_writer_fn`'s contract is already precise about `room`,
  commit-by-return and the two refusals.

  **Two smaller rulings ride along, both in the thunk and both about the value 0.**
  (1) `binding.h` reserves a writer's return of 0 as the binding's signal that its thunk
  captured an exception — so a managed `RowWriter` that returns 0 honestly would be
  indistinguishable from one that threw. The thunk converts it into an `InvalidOperationException`
  naming the rule, because a caller whose writer legitimately produced nothing deserves a message
  rather than a status. (2) `room` is CLAMPED to `int.MaxValue` rather than refused, since a
  `Span<byte>` cannot be longer; refusing would be this binding inventing a limit the ABI does
  not have.

  **No diagram change in the architecture set** (the fourth place): the class diagram in the
  public-surface document is amended, but no shape, name, count or direction moves in
  `BIND-architecture-diagrams.md`.

- **D-BIND-46 — `fl_schema_copy` is added, the ABI minor goes 3 → 4, and the THIRD occurrence
  is recorded as a pattern rather than as a third accident.** *LOCKED BY THE MAINTAINER
  2026-09-22,* raised at BIND-4c-i when `SchemaHandle.ToArrowSchema()` turned out to be
  unwritable.

  **THE HEADER PROMISED A CALL IT DID NOT DECLARE.** `fl_schema`'s own comment tells a binding
  that *"a binding that wants an Arrow schema of its own imports a COPY — in C#,
  `CArrowSchemaImporter.ImportSchema` over a deep copy the shim provides — and releases this
  handle."* **No declared function provided one.** The 43 entry points carried
  `fl_schema_retain` and `fl_schema_release` and nothing else, and the alternative the sentence
  rules out is the catastrophic one: `ImportSchema` TAKES OWNERSHIP, so handing it the shared
  pointer runs the Arrow C Data Interface release callback on a schema the provider is still
  delivering on — the mistake the same comment calls the single most expensive one available at
  this boundary.

  **THE FIX.** `fl_status fl_schema_copy(const fl_schema*, struct ArrowSchema* out, fl_error*)`
  deep-copies through `OwnedSchema::DeepCopy` — the seam's own, already used INBOUND by
  `fl_publisher_create_topic`, so the two directions cannot disagree about what a deep copy is.
  `out` is written only on success and the caller then owns it, releasing it the ordinary Arrow
  way. **So there are two releases and they are not interchangeable:** `fl_schema_release` for
  the handle, `out->release` for the copy, and never the other way round. A NULL schema is
  refused with kInvalidArgument, because a schema-less transport's kOk carries one and a caller
  reaching this call has skipped an answer it should have read.

  **THE MINOR GOES 3 → 4**, with the same two coupled consequences as D-BIND-42 and D-BIND-43:
  `NativeLoader.HeaderVersionMinor` moves in the same commit (the managed handshake is an exact
  match while major is 0), and the handshake test follows for free from D-BIND-42's derived
  offsets — the second time that derivation has paid for itself.

  **THIS IS THE THIRD MISSING DECLARATION, AND THAT IS THE ENTRY'S REAL SUBJECT.** 42
  (`fl_blob_create`), 43 (`fl_schema_retain`) and now 46 were all found the same way — by
  writing the code that had to CALL the thing — and all three share one root cause worth naming:
  **an owner-handle type whose OUTBOUND direction had no consumer until the binding grew one.**
  Inbound, every one of these types was exercised at BIND-2c and reviewed clean. A declaration
  that is missing is invisible to a review that walks the declarations that are present, and all
  three were invisible for exactly that reason.

  **SO A SWEEP WAS RUN RATHER THAN A FOURTH POINT FIX**, checking every owner/opaque type against
  the operations its own documentation describes: `fl_blob` (create/retain/release) ✓,
  `fl_attachments` and its builder ✓, `fl_schema_arrival` ✓, `fl_codec`/`fl_rows` ✓,
  `fl_provider`/`fl_publisher`/`fl_subscriber` ✓, `fl_string_list` ✓, `fl_error` ✓ — and
  `fl_schema`, which was the one gap. **The surface is now complete by that test.** Recording the
  sweep matters more than recording the fix: without it the next reader has three point fixes and
  no reason to believe a fourth is not waiting, and the round would keep paying the same tax one
  declaration at a time.

  **The BIND-1 review is not thereby impeached, and saying so is part of the record.** It read
  the header as a specification and every declaration in it was individually sensible; the
  category it could not see was absence. A review method that would have caught these is
  type-by-type completeness — the sweep above — and it is cheap enough that it should be run
  once per surface, at the point a consumer for the outbound direction first exists.

- **D-BIND-47 — bucket 3's two provider-level fan-out cases are EXCLUDED; the ported total goes
  278 → 276 of 302 and the exclusions 24 → 26.** *LOCKED BY THE MAINTAINER 2026-09-24,* raised
  at BIND-4d-ii when the port ran into two cases the managed surface cannot observe.

  **THE TWO CASES.** `UnsubscribeLastSubscriberUnsubscribesFromProvider` and
  `UnsubscribeWithRemainingSubscribersKeepsProviderSubscription` both assert
  `mock->unsubscribe_count`: the fan-out's PROVIDER-LEVEL bookkeeping, which decides whether the
  last local cancellation releases the transport subscription or leaves it open and quiet.

  **WHY THEY CANNOT PORT, and the reason is structural rather than awkward.** That bookkeeping
  lives in the C++ `fletcher::Subscriber`. The managed `Subscriber` WRAPS it through
  `fl_subscriber_*` and reimplements none of it — the fan-out, the dedup and the provider-level
  release are all one tier down. So a "port" would drive the same C++ code a second time and
  file the result as a test of the binding, which is the vacuity this round keeps refusing. On
  top of that the managed surface has NO VIEW of provider-level subscriptions at all: D-BIND-24
  exposes no registration and no enumeration on `ProviderRegistry`, deliberately, so there is
  nothing to observe even if it were the binding's to observe.

  **THE SAME REASONING THE BUCKET TABLE ALREADY USES**, for the 24 provider-internal Fast DDS
  cases: *"provider-internal (the Fast DDS `TypeSupport`, a C++ type no managed code touches)"*.
  These two are that category, found in a different bucket. Recording them as a second instance
  of one rule rather than as a new kind of exception is the point — **the test for exclusion is
  "does the managed surface implement this, or only wrap it?"**, and it should be applied to
  bucket 4 before that port starts rather than discovered inside it.

  **WHY A NUMBERED DECISION AND NOT A PARAGRAPH IN A TEST FILE.** The 302 denominator and the
  278 numerator are MAINTAINED FIGURES that the round has already corrected once under a
  maintainer ruling (2026-09-18, when 275 of 300 was found low by three). A second movement of
  the same figure by an implementer's judgement, recorded only in a test file's header, would be
  exactly the drift that correction existed to stop. The number now moves in the decision log,
  the tracker's three sites carry it, and the test file points at both.

  **NO CODE CONSEQUENCE AND NO ABI CHANGE.** Unlike 42/43/46 nothing native moves, and unlike
  44/45 no managed signature moves either; this decision changes only what the round counts as
  owed. Bucket 3's remaining 21 cases are ported and green (`BucketThreeConformanceTests.cs`
  carries the case-by-case mapping, including the 11 covered by earlier slices and named rather
  than duplicated).

  **A SECOND, SMALLER FINDING RIDES ALONG, and it is not an exclusion:** bucket 3's row lists
  `test_pubsub_arrow`'s 15 cases, but BIND-5's acceptance row claims them ("`pubsub-arrow`
  cases") and `SubscriberArrow` does not exist before BIND-5. So bucket 3 splits 23 / 15 across
  BIND-4d-ii and BIND-5. Those 15 still port — the total is unaffected — and the row now says
  where they land, so BIND-5 does not have to rediscover it.

  **No diagram change** (the fourth place): nothing in the architecture set counts test cases.

- **D-BIND-48 — BIND-4d-v's benchmark is TWO HARNESSES run by hand, and native allocations are
  MEASURED on Linux rather than enumerated.** *LOCKED BY THE MAINTAINER 2026-09-24,* two
  questions asked at the start of 4d-v, because D-BIND-37 moved the bullet but said nothing about
  its shape.

  **THE ARMS, one row type throughout** (the `Telemetry` message of
  `integration-tests/protoc-arrow-bridge/proto/pubsub.proto`, over `inprocess`, no subscriber):
  **C1** the generated C++ `TelemetryFeed_TelemetryStreamPublisher::Publish(row)` — the baseline
  the bullet names; **C2** `fl_publisher_publish_row` driven from C++ — the shim, `ToSegments` and
  the nanoarrow codec with no managed code; **M1** `Publisher.Publish(topic, rows, i)` over a
  pre-bound batch — the fused per-row path; **M2** one-row `RecordBatch` → `Bind` → `Publish` →
  dispose — B-2 as written, a lone row paying for an Arrow array; **M3** `Publish(topic, rows)`
  over N rows, reported per row — the batch mitigation B-2 names. The gaps C1→C2→M1→M2 attribute
  the cost layer by layer, so a D-BIND-1 question, if one is raised, arrives already pointing at
  its fix: a pre-converted topic handle, or a managed codec.

  **ARMS ADDED WHILE IMPLEMENTING, not ruled — stated apart so the ruling stays legible.**
  **C3** (bind → publish → unbind of a one-row array, from C++) and **C4**
  (`fl_publisher_publish_rows`, per row) are the native halves of M2 and M3, as C2 is of M1, so
  the Linux counts attribute every managed arm and not only M1. **M2a** builds and disposes
  M2's one-row batch with no bind and no publish, and **M2b** adds the C Data Interface export
  and its release: two decompositions of M2, so that M2a is Apache.Arrow's batch build, M2b − M2a
  its exporter, and M2 − M2b everything Fletcher does — measured rather than argued. **M4** — `Publish(topic, RowWriter, n)` copying the
  canonical bytes — is the FLOOR of the D-BIND-1 alternative: generated C# writing wire bytes,
  with the encoding replaced by a 40-byte copy. It is there so that a STOP-AND-ASK, if raised,
  arrives with the most it could buy back already measured.

  **TWO FACTS FOUND WHILE SHAPING IT.** The generated publisher's `TopicSegments()` is a
  function-local `static const`, so C1 pays NO per-publish segment conversion — `ToSegments` is a
  cost only the binding pays, not one shared with C++. And `InProcessPubSubProvider::Publish`
  allocates a `VectorWriteBuffer` and a joined key on every call, on EVERY arm, so that cost
  cancels in the comparison and must not be read as the binding's.

  **SHAPE (question 1).** A C++ harness in `c-abi/benchmarks/` — a standalone Conan consumer on
  `arrow-bridge/benchmarks`' pattern, Google Benchmark, `operator new` counting — and
  `dotnet/benchmarks/Fletcher.Benchmarks` on BenchmarkDotNet with `MemoryDiagnoser`, **outside
  `Fletcher.slnx`** for the reason the integration suites are. **Neither runs in CI**, as neither
  existing benchmark does; the method and the table are recorded beside the harnesses and the
  numbers in the tracker. Declined: a CI smoke lane (bit-rot cover for the price of three more
  gates), and a C#-only Stopwatch harness (no C1, so not "against the C++ generated publisher").

  **ALLOCATION COUNTING (question 2).** The shim keeps the dynamic CRT (D-BIND-38), and on
  Windows an `operator new` replaced in the benchmark executable does not see allocations made
  inside the DLL — so C2's count, and the native half of M1–M3, would be invisible there.
  **Timings come from Windows; allocation counts from a Linux run**, where the executable's
  `operator new` interposes over `libfletcher-c-abi.so`. Declined: enumerating what the path
  provably allocates (a claim where a measurement is available), and a Debug-CRT
  `_CrtSetAllocHook` build (rebuilds the whole static chain in Debug, whose paths may allocate
  differently from the ones shipped).

  **Consequences.** BIND-4's benchmark bullet and development plan B-2 point here; no code, ABI
  or diagram change. **What counts as "unacceptable" is NOT ruled here** — it is the maintainer's
  judgement on the table, and the D-BIND-1 STOP-AND-ASK stays where D-BIND-37 left it.

- **D-BIND-49 — the measured per-row publish cost is ACCEPTED; no D-BIND-1 STOP-AND-ASK is
  raised, and B-2 closes.** *LOCKED BY THE MAINTAINER 2026-09-25,* on the table in
  `c-abi/benchmarks/README.md` — the judgement D-BIND-37 moved to BIND-4 and D-BIND-48 left open.

  **WHAT WAS ACCEPTED, in the numbers it was judged on** (Windows, per row): the fused per-row path
  M1 at **378 ns, 1.95× the generated C++ publisher's 194 ns, with no managed allocation**, and the
  batch path M3 at **239 ns, 1.23×**. Every native allocation either path makes is the shim's — M1
  and M3 equal their C++ mirrors (7 and 5) exactly.

  **WHAT B-2 TURNED OUT TO BE, stated so nobody re-derives it.** Not `ToSegments` (72 ns) and not
  the managed crossing (80 ns): **a row that does not start out as Arrow** (M2) pays **3.8 µs,
  19.6×, 3.6 KB managed, 31 native allocations and Gen2 collections**, and about two-thirds of that
  is Apache.Arrow's own — building the one-row batch and exporting it, including **all** of the
  Gen2 collections (M2b shows them with no Fletcher code in the loop). No change inside Fletcher
  removes that part; only a path that never builds an array does.

  **WHY THAT DOES NOT RAISE THE STOP-AND-ASK.** D-BIND-1's alternative — generated C# writing wire
  bytes — buys NOTHING for a row that is already Arrow: M4, which is that path with the encoding
  replaced by a copy, costs what M1 costs (377 vs 378 ns). It would buy ~10× only for the lone
  non-Arrow row, and that case has a remedy that keeps one codec: publish it in a batch.

  **THE CONSEQUENCE, carried forward as a DESIGN INPUT to BIND-6, not a ruling on it: the generated
  publisher should publish BATCH-FIRST.** The frozen surface already has both shapes:
  `Publish(IEnumerable<T>)` is naturally the M3 shape (one `ToArrow`, one bind, one crossing), and
  a straightforward `Publish(T)` is the M2 shape. This decision does NOT remove or redefine the
  frozen `Publish(T)` — that would be a public-surface change, and it was not asked. What BIND-6
  owes is to make the cost visible where it is chosen: `Publish(T)`'s documentation states the
  per-row price and points at the batch form, and whether `Publish(T)` stays per-row or
  accumulates behind the caller is BIND-6's to settle, arriving with the number that motivates it.

  **NOT TAKEN, and still open to a later round on its own merits:** the shim's encoder lambda (three
  captured references, one libstdc++ heap allocation per publish; internal, no ABI change) and the
  pre-converted topic handle D-BIND-32 names (a pure ABI addition, worth at most the 72 ns C2 − C4
  shows). Neither was needed to accept, and neither reaches M2.

  **Consequences.** BIND-4's benchmark bullet is MET — 4d-v closes, and with it the last BIND-4
  item. Development plan B-2 closes. BIND-6's acceptance gains one bullet carrying the design input
  above. No code, ABI, public-surface or diagram change.

- **D-BIND-50 — a subscription cancelled from INSIDE a delivery on its own Subscriber is freed
  OFF-THREAD, after a second cancel from outside any delivery has waited for the drain. Amends
  D-BIND-18.** *LOCKED BY THE MAINTAINER 2026-09-25,* answering BLOCKER B1 of
  `plans/reviews/BIND-4-codereview.md`.

  **WHAT D-BIND-18 GOT WRONG, precisely.** Its rule — "because the seam guarantees no invocation
  *begins* after `Unsubscribe` returns, whoever brings the counter to zero after retirement frees
  the `GCHandle`" — equates two points that are not the same. The seam's "begins" is **passing the
  gate** (`pubsub/src/subscriber.cpp`: lock the gate, check `retired`, call). The managed counter
  is incremented **later**, in `TryEnter`, after the shim's thunk and the reverse-P/Invoke
  transition. On the blocking path the gap is harmless because native drains. On the carve-out —
  an `Unsubscribe` issued from inside a delivery on the same Subscriber — native returns without
  draining, and a **sibling** subscription whose delivery is between its gate and `TryEnter` on
  another thread can have its `GCHandle` freed under it. The self-cancel case was safe only
  because that thread had already incremented. Development plan S-2 stated the sibling case,
  asserted "the counter covers it", and named the test that would have shown otherwise; the test
  was not written.

  **THE RULE NOW.** When `Unsubscribe` runs on the carve-out (this thread is inside a delivery on
  this Subscriber, at any depth), it retires the subscription but does **not** free inline. It
  queues one thread-pool work item that calls `fl_subscriber_unsubscribe(id)` again — from outside
  any delivery, where the seam makes a cancellation of an id another thread is cancelling "wait
  for the same drain" (`subscriber.hpp`) — and frees the `GCHandle` when that returns. The work
  item holds a reference on the subscriber's SafeHandle, so a racing `Dispose` postpones the
  native destroy rather than invalidating the call. The blocking path and the self-cancel path
  are unchanged; `TryFree`'s interlocked exchange still makes "exactly one free" true, and which
  branch freed stays observable (the counters `ReentrancyTests` read gain a third, for the
  deferred branch).

  **WHY THIS AND NOT THE ALTERNATIVES.** Never freeing on the carve-out (until `Dispose`) is
  simpler and leaks a bounded amount per such cancel; declined. Fixing it natively — the shim
  holding a per-subscription reference from gate-pass until `on_delivery` returns — changes a
  documented ABI behaviour and an ABI minor, and BIND-Rust inherits it; declined, because the seam
  already provides the wait this needs, from another thread.

  **Consequences.** `Subscriber.cs`'s lifetime header and `binding.h`'s unsubscribe comment (which
  repeat the false premise) are corrected; development plan S-2 is restated; a test cancels a
  sibling from inside a handler while the sibling is between gate and handler on another thread.
  No ABI or public-surface change.

- **D-BIND-51 — the managed refusal layer is keyed on the PROVIDER, over every delivery frame on
  the thread. Amends D-BIND-18.** *LOCKED BY THE MAINTAINER 2026-09-25,* answering BLOCKER B2 of
  `plans/reviews/BIND-4-codereview.md`.

  **WHAT D-BIND-18 GOT WRONG.** Its thread-static marker names "the Subscriber whose thunk this
  thread is inside", and `Dispose`/`Subscribe` refuse when that is `this`. **The seam's rule is per
  provider and per thread** — `subscriber.cpp`'s destructor: *"A handler on subscriber X destroying
  subscriber Y over the same provider violates it"*, answered by rethrowing `kReentrantCall` out of
  a `noexcept` destructor. So disposing ANOTHER Subscriber on the same provider from a handler
  passed the managed check and terminated the process. And the marker holds only the innermost
  frame, so a nested delivery (a handler publishing on an `inprocess` provider that delivers
  synchronously) hid the outer one.

  **THE RULE NOW.** The marker becomes a per-thread **stack of delivery frames**, each recording
  its Subscriber and provider. `Subscriber.Dispose` and a `Subscribe` that needs a new
  provider-level subscription are refused when **any** frame on this thread is on the same
  provider — the seam's own question, asked in managed code first. D-BIND-50's carve-out predicate
  is the same stack's other question: any frame on **this** Subscriber. Messages still name
  `DispatchAfterDelivery`. A cross-provider call stays served, as the seam serves it.

  **SCOPE, checked rather than assumed.** `Publisher.Dispose` is not refused: `~Publisher` is
  defaulted and never enters the provider, so the widened §6 clause 5 has nothing to stop there.

  **Consequences.** Tests: disposing another Subscriber on the same provider from a handler is
  refused with a managed exception; the nested route is refused; a cross-provider dispose is
  served. No ABI or public-surface change.

- **D-BIND-52 — the shim IMPLEMENTS the schema-watch pair now; the C# surface does not expose it
  yet.** *LOCKED BY THE MAINTAINER 2026-09-25,* answering Q1 of
  `plans/reviews/BIND-4-codereview.md`.

  **WHY NOW.** D-BIND-29 declared `fl_subscriber_subscribe_schema` / `_unsubscribe_schema` and
  had them answer `FL_NOT_SUPPORTED` "until the seam provides them". The seam now does —
  `Subscriber::SubscribeSchema` / `UnsubscribeSchema`, from `f33973c` (#128), which reached this
  branch through the merge of `main` — so the stub's message ("the seam does not carry a schema
  watch yet") became false, and the header's own rule ("nothing here is ever stubbed out to
  return FL_NOT_SUPPORTED because it has not been written yet") had one standing exception left.

  **WHAT IT IS.** The two entry points forward to the seam's pair, whose contract `binding.h`
  already stated under "when implemented" and now states as the contract: never blocks; resolves
  when a publisher announces the topic, with the same arrival a subscribe would get; counted per
  subscriber and idempotent per topic; the LAST release ends a still-pending arrival with
  `FL_SUBSCRIPTION_ENDED`; always refused with `FL_REENTRANT_CALL` from inside a delivery on the
  subscriber's provider; `FL_NOT_SUPPORTED` — now meaning what the header says it means — from a
  transport with no schema channel (`inprocess`, and today every built-in but Fast DDS).
  **ABI 0.4 → 0.5**, because a binding learns what works from the version, and
  `NativeLoader.HeaderVersionMinor` moves in the same commit (the exact-minor handshake).

  **WHAT IT IS NOT.** No C# member calls it. The frozen public surface
  (`BIND-csharp-public-surface.md`) lists no schema watch, and adding one is a public-surface
  change this ruling does not make; managed exposure is **OWED**, for a later ruling. Rust, next
  round, binds the ABI and inherits the pair as it stands.

  **Declined:** exposing it in C# now (widens BIND-4 and the frozen surface without a separate
  ruling); deferring it and only correcting the message (keeps a declared-but-unimplemented pair
  in a header whose rule forbids one).

- **D-BIND-53 — BIND-4's bullet 8 is amended to what the binding owns; Fast DDS's silent drop of
  an oversized row is recorded as a PROVIDER limitation and left to its owner.** *LOCKED BY THE
  MAINTAINER 2026-09-25,* answering Q2 of `plans/reviews/BIND-4-codereview.md`.

  **WHAT WAS FOUND.** Writing bullet 8's missing test ("bounded-payload overflow surfaces as
  `FletcherException(PayloadTooLarge)`") showed it cannot hold over any transport a managed
  caller can reach. `inprocess` ignores `max_payload_bytes` by design. The Fast DDS provider's
  `Publish` always takes its serialize flow (`sample_writer.hpp`: the loaned flow, which throws
  `kPayloadTooLarge`, is "kept and unit-tested but not selected"), and on that flow an oversized
  row is **dropped and logged** — deliberately, pinned by
  `FastDDSPubSubProviderTest.DataSharingOversizedRowDoesNotThrow`. A 512-byte row through a
  provider bounded at 128 bytes published with no exception.

  **WHY THAT IS NOT THE BINDING'S TO FIX.** The seam spec makes the overflow mapping normative —
  a row that does not fit the transport's bound must not degrade into something "that tells a
  caller nothing it can act on" — and Fast DDS goes further than degrading it. But the provider
  is `main`'s (#128), its behaviour is a recorded choice with a test pinning it, and changing it
  from this branch would override that choice without its owner.

  **BULLET 8 NOW READS:** a `PayloadTooLarge` the seam or the shim REPORTS surfaces as
  `FletcherException(PayloadTooLarge)` — proven from C# by
  `ErrorTests.ARowThatDoesNotFitAFixedWindowIsPayloadTooLarge` (a real native status, from
  `fl_encode_row` into a fixed window too small for the row) and natively by c-abi's
  `Containment.OverflowIsPayloadTooLargeAtEitherOrigin`. The transport half is the provider's.

  **CARRIED FORWARD.** Whether the Fast DDS provider should report an oversized row — through
  its serialize flow, as it already reports other encoder failures as `kInternal` — is flagged as
  a separate task against `main`. If it changes, the transport test held out of BIND-4 (a Fast
  DDS provider bounded at 128 bytes, a 512-byte row, `PayloadTooLarge` expected) goes into
  `integration-tests/pubsub-transport-conformance` as written.

  **Declined:** changing the provider on this branch (crosses into a component `main` owns);
  leaving bullet 8 NOT MET until `main` moves (puts BIND-4's closure on another branch's
  schedule).

  **AMENDED BY THE MAINTAINER, 2026-09-25 (the same day): the provider fix is made ON THIS
  BRANCH.** The provider question went to its decision the same day — REPORT the overflow — and
  the fix was first written on a branch off `main`. The maintainer then ruled it belongs to the C#
  binding work and lives on `feature/csharp-bindings` only, reaching `main` when #129 does; the
  `main`-side branch is dropped. So the "declined" line above no longer holds for this change:
  `9255c19` makes the Fast DDS provider's serialize flow record an oversized row and `Publish`
  throw `kPayloadTooLarge` (an encoder that throws stays `kInternal`), replaces the pinned
  `DataSharingOversizedRowDoesNotThrow` with `AnOversizedRowIsPayloadTooLarge`, and updates the
  conformance case that asserted the silent drop. **Bullet 8 therefore holds in full again**,
  over a real transport: `CrossTransportTests.ABoundedPayloadOverflowSurfacesAsPayloadTooLarge`
  (a Fast DDS provider bounded at 128 bytes, a 512-byte row, `PayloadTooLarge`) joins the
  binding's-half test above. **Known cost, accepted:** until #129 lands, `main` keeps the silent
  drop, and every `main` consumer of the provider — the gateway and its TS clients included —
  with it.

- **D-BIND-54 — `FletcherFormatException` is keyed on the PAIR (codec origin, `InvalidArgument`),
  not on the origin alone.** *LOCKED BY THE MAINTAINER 2026-09-25,* on the nit left open by
  D-BIND-53's transport test.

  **WHAT WAS FOUND.** A row too large for the transport's bound reached C# as a
  `FletcherFormatException` carrying `PayloadTooLarge`. The status was right, the type wrong. The
  shim marks a fused publish's failure as CODEC-origin while the encoder writes the row
  (`binding.cpp`, the re-attribution inside the encoder lambda); the encoder's
  `std::overflow_error` becomes `FL_PAYLOAD_TOO_LARGE` by type (seam §5.1, at either origin); and
  `Errors.ThrowIfFailed` raised the format subclass for EVERY codec-origin failure. Both BIND-4
  tests of bullet 8 had to use `ThrowsAny` to pass, and said so.

  **WHY THE MAPPING, NOT THE ORIGIN.** The origin is TRUE — the encoder is what hit the window's
  end — and `Containment.OverflowIsPayloadTooLargeAtEitherOrigin` pins it. What was wrong is
  reading "codec" as "malformed". D-BIND-19 rule 1 already says the origin is how the wrapper
  knows to throw the format type "for a `kInvalidArgument` that came from `PositionalReader`";
  the code had widened that to the origin alone, and the header, the tracker, the development
  plan's sample and the public surface all copied the wider wording. The type's own contract
  (`FletcherException.cs`) is that malformed data is NOT something a caller can retry or
  reconfigure, and `PayloadTooLarge` is exactly that: valid data, a bound the caller can raise. A
  catch that discards bad input on `FletcherFormatException` would have discarded good rows. A
  codec-origin `kInternal` (an unexpected `std::exception` in the encoder) is a defect, not bad
  data, and falls on the same side.

  **THE RULE.** The managed throw site raises `FletcherFormatException` iff `Origin == Codec` AND
  `Status == InvalidArgument`. Everything else is a plain `FletcherException`, whose `Origin`
  still reads `Codec` where the shim said so — the origin is reported, not hidden. The shim is
  unchanged and there is **no ABI change**: only `binding.h`'s comment on `fl_origin` moves, from
  "a binding maps FL_ORIGIN_CODEC to its format exception" to the pair. Rust, next round, reads
  that comment and inherits the pair.

  **PROVEN BY** `ErrorTests.ARowThatDoesNotFitAFixedWindowIsPayloadTooLarge` (now exact type and
  `Origin == Codec`, on a real native status), the new
  `ErrorTests.OnlyACodecInvalidArgumentIsAFormatException` (`PayloadTooLarge` and `Internal` rows,
  a hand-built error, since no managed call provokes a codec `kInternal` on demand) and
  `CrossTransportTests.ABoundedPayloadOverflowSurfacesAsPayloadTooLarge` (exact type, over Fast
  DDS). Falsified: with the old origin-only condition restored, the build compiles and exactly
  those three unit rows fail. Every existing format-exception assertion is a codec
  `InvalidArgument` and is unchanged.

  **Declined:** clearing the origin in the shim when the encoder overflows (records something
  false, and breaks the containment test that pins overflow at either origin); a new
  `FletcherPayloadTooLargeException` subtype (widens the frozen surface for a distinction
  `Status` already carries).

- **D-BIND-55 — the public-surface note is reconciled with the shipped API: six rows amended to
  the code, three small members owed and built, `WaitAsync` built above `Wait`, `BlobHandle`
  deferred with a trigger.** *LOCKED BY THE MAINTAINER 2026-09-25,* in four questions, on the
  disagreement the BIND-4 conformance review listed and left undecided.

  **WHAT WAS FOUND, and what the review got wrong about it.** `BIND-csharp-public-surface.md` is
  the round's frozen C# reference, and it disagreed with the code in both directions. Re-derived
  by comparing every drawn type with the shipped public members, three corrections to the review:
  it said D-BIND-22 and the development plan promise `WaitAsync` — neither does; D-BIND-22 forbids
  a waiting mechanism BESIDE `Wait`, and only the note's diagram promised one. It missed four
  differences (`FletcherFormatException.Offset` drawn and never built; `FletcherCodec.DecodedSchema`,
  `PayloadBound.FramingBytes` and `FletcherOrigin` built and never drawn). And one item it listed as
  a naming difference had a consequence: the note drew `SchemaHandle : SafeHandle`, the code has a
  plain `IDisposable` with no finaliser, and every OTHER native handle in the binding is a
  `SafeHandle` — so an undisposed `Retain()` was the one native leak the managed tier allowed.

  **THE RULING.**
  1. **The note is amended to the code** where the code is right: `ProviderSelector.IsName` stays
     internal (a public kind invites a caller edit where the seam promises a configuration edit);
     `AttachmentsBuilder.Set` takes a span and copies, with a `string` overload and `Dispose`
     (D-BIND-44 — the value is not the caller's memory, so `ReadOnlyMemory` would misstate it);
     no `FletcherFormatException.Offset` (no ABI field carries one; the reader's message, which
     crosses verbatim, names the byte); no `SchemaArrival.Message` (a failed wait throws with the
     message, D-BIND-19 rule 4); and `HandlerFaulted` with its event args, `SchemaArrival.Dispose`,
     `SchemaWaitResult.HasSchema`, `DecodedSchema`, `FramingBytes` and `FletcherOrigin` are drawn.
  2. **Three small members are owed and built.** `SchemaWaitResult.IsSchemaless` — `!HasSchema` is
     also true for `Pending` and `SubscriptionEnded`, so reading it as "schema-less transport" is the
     mistake seam §7 exists to prevent. A **finaliser on an OWNED `SchemaHandle`** — it cannot be a
     `SafeHandle` (`fl_schema` is two pointers, and a borrowed handle must never release), but an
     owned one is now finalised; `fl_schema_release` is safe from any thread by the header's own
     contract. `static Diagnostics.AbsorbedTotal` — the managed mirror of C++
     `DeliveryChannel::AbsorbedTotal()`, the sum of every `Subscriber`'s count, meaningful because
     D-BIND-17 allows one copy per process. `TopicPath.Utf8ByteLength` is dropped from the note:
     `TopicPath.Of` already refuses an over-long path, and nothing needs the number.
  3. **`SchemaArrival.WaitAsync(TimeSpan, CancellationToken)` is built ABOVE `Wait`**, the helper
     D-BIND-22 permits: `Wait` in slices of at most 50 ms on the thread pool, the token checked
     between them, the same outcomes and no others. A refused timeout or a disposed arrival throws
     on the caller's stack; cancellation ends the task as cancelled
     (`OperationCanceledException`, managed-only, D-BIND-19 rule 4) and cancels nothing native.
     **Cost, accepted:** one pool thread for as long as the wait runs.
  4. **`BlobHandle` is DEFERRED, not dropped.** "Copy what you keep" stays the rule for attachment
     blobs; the planned shape (a `SafeHandle` over `fl_blob_retain`/`release`: `Length`, `CopyTo`,
     `Retain`, `Dispose`) is recorded in the note. **Trigger:** a consumer whose attachments are
     large enough that copying them out of the callback costs. BIND-5 does not need it —
     `SubscriberArrow` copies attachments into builders anyway.

  **PROVEN BY** `SchemaArrivalTests` (the three `IsSchemaless` shapes, all from a real
  `inprocess`; `WaitAsync` on an arrived, a schema-less, an infinite, a finite, a cancelled and a
  pre-cancelled wait, and the two synchronous refusals) and `ProcessWideTests`, which runs in a
  collection with parallelisation disabled because both of its subjects are process-wide: an
  undisposed owned schema is released by the finaliser, a disposed one is not released again, a
  BORROWED one never is, and `AbsorbedTotal` moves by exactly the per-instance count through both
  absorbing routes. **Falsified:** eight mutations, each compiling, each failing its test. Two of
  them had to remove BOTH defences to fail — `SuppressFinalize` and the finaliser's own guard each
  prevent a double or a borrowed release alone — and the tests are therefore guards on the pair.
  Managed 277/277 on both TFMs.

  **No ABI change and no version bump.** Every member is managed. **No diagram change in the fourth
  place:** `BIND-architecture-diagrams.md` draws no members; the note's own diagrams 5.1–5.3 move,
  validated under mermaid + jsdom.

  **Declined:** building `BlobHandle` now (a type and its tests for a need no consumer has stated);
  dropping `WaitAsync` (every async caller would write its own, with its own cancellation);
  leaving `SchemaHandle` without a finaliser and documenting the leak (the one native handle
  whose lifetime a caller could forget without a safety net).

- **D-BIND-56 — BIND-4's bucket 4 runs over XRCE-DDS too: the C# transport lane builds a
  MicroXRCEAgent from the shared recipe, the suite proves it owns it, and `xrce` joins the rows.
  BIND-4 is reopened until that lane is green.** *LOCKED BY THE MAINTAINER 2026-09-25,* in two
  questions, before BIND-5.

  **WHAT WAS FOUND.** Reporting progress against Feature 16353's requirements showed that **no C#
  had ever published or subscribed over XRCE-DDS.** The cross-transport suite ran `inprocess` and
  `fastdds`; its XRCE row asserted only a typed refusal with no Agent, and deferred the round trip
  to `integration-test-fastdds-xrce-interop` — which is C++. BIND-4's bullet 10 says "Bucket 4 over
  `fastdds` **and** `xrce`", the tracker says 18786 does not close before bucket 4 is green over
  both, and Hardware/ROTV can reach nothing but XRCE-DDS. The code review saw it as DEBT D15 ("XRCE
  is covered only as a typed refusal"); **the close-out re-grade then marked bullet 10 met without
  answering D15.** That was the re-grade's error, and it is corrected in the conformance review.

  **THE RULING.**
  1. **A BIND-4 follow-up, before BIND-5.** Bullet 10 is PARTIAL until the XRCE rows are green in CI,
     and BIND-4's tracker row goes back to 🔴 until then.
  2. **The Agent lives in the C# transport lane**, not as a C# step in the C++ interop lane, so the
     `xrce` rows run the SAME bodies as the other transports — which is what "no per-transport C#
     code" means when asserted. Concretely:
     * **One recipe.** The interop lane's MicroXRCEAgent block moves, unchanged, into
       `integration-tests/cmake/MicroXrceAgent.cmake`; the interop project includes it, and a new
       Agent-only project (`integration-tests/pubsub-transport-conformance/agent`) includes it too.
       So both lanes build one Agent version against one Fast DDS, and on Windows share ONE cache
       entry, keyed on that file. `integration-tests/pubsub-conformance` keeps its own copy — a
       known duplicate, recorded in the module's header.
     * **Ownership, proven in C#.** The fixture spawns the Agent (`udp4 -p 2020`, a port nothing
       else in the tree uses), waits until an XRCE session opens, then requires the OS to record
       THIS child as the port's holder — `GetExtendedUdpTable` on Windows, `/proc/net/udp` against
       the child's `/proc/<pid>/fd` on Linux, IPv4 only, foreign beats ours, a failed query is a
       refusal. The third copy of PDA-DEC-1H's rule, with the same two forcing tests as the C++
       copies (`AForeignAgentDoesNotSatisfyTheHarness`, `AFailedOwnershipQueryDoesNotSatisfyTheHarness`).
     * **The rows.** `xrce` joins every theory; a row's deployment configuration (the Agent's
       address and a fresh session key) is the only thing it varies, and no body reads it.
     * **One deployment case**: a row published by a C# XRCE client reaches a C# Fast DDS
       subscriber through the Agent, on a domain of its own, compared byte for byte with the
       codec's encoding.
     * **Absent an Agent, the XRCE rows FAIL, not skip,** and the other rows still run.

  **PROVEN LOCALLY** (Windows): the suite **21/21** — the five theories over three transports, the
  deployment case, both forcing tests, the overflow and vacuity guards; `integration-tests/
  fastdds-xrce-interop` still green through the shared recipe. **Falsified:** skipping the
  ownership proof fails the foreign-Agent test; treating a failed query as good enough fails its
  test; with no Agent configured exactly the eight XRCE cases fail and the thirteen others pass.
  `actionlint` clean. The recipe's COLD build path was not exercised locally — the Agent was already
  installed — and **is now CI-proven at `78ac363`** (`ci.pr` run 36144203527): all four Agent-building jobs (both
  lanes, both platforms) cloned and built it from source; the transport lane is **21/21** on Linux
  and Windows, the interop lane green through the shared recipe. BIND-4 re-closes on that run.

  **Cost, accepted:** the transport lane's Linux job builds the Agent from source every run
  (~10–15 minutes cold, as the interop lane's does); Windows builds it once per key.

  **Declined:** a C# step in the C++ interop lane (a second, XRCE-only set of C# cases instead of
  bucket 4's shared bodies, and a .NET SDK in a C++ lane); amending bullet 10 to "reachable by
  selector" (Hardware/ROTV's only transport would stay unproven from C#); folding it into BIND-5
  (BIND-4's own bullet would close under another item).

- **D-BIND-57 — per-topic options cross the ABI and C#, the schema watch is exposed in C#, bucket
  3's #128 cases are ported, and the test matrix is checked on every pull request. BIND-4 is
  reopened until that is green.** *LOCKED BY THE MAINTAINER 2026-09-25,* in four questions,
  before BIND-5.

  **WHAT WAS FOUND.** Laying out the `TopicOptions` gap for BIND-5 showed something larger. #128
  (FastDDS modernization) added `TopicOptions { profile, max_payload_bytes }` to the seam - a Fast
  DDS QoS profile per topic and a per-topic publisher bound - and the seam spec says a C form is
  owed for each of its methods; `binding.h` declared none, like D-BIND-42, 43 and 46 before it. And
  #128 reached this branch by the merge of `main` at 15:10 on 2026-09-24, FOUR HOURS AFTER bucket 3
  was ported against the 18-case `test_publisher_subscriber.cpp`. That file now has 44 cases;
  `test_pubsub_arrow` went 15 → 33, bucket 4's files 80 → 122 by driver selection, and a new
  `test_batch_decoder` sat in no bucket. Nothing re-derived the matrix after the merge, so both
  BIND-4 closes - including this morning's re-close on CI - claimed a bucket-3 file set that was 26
  cases short. The tables even said "these counts rot; re-run the command"; nobody did.

  **THE RULING.**
  1. **A BIND-4 follow-up, and BIND-4 reopens.** Its bucket-3 bullet names the FILE SET, and by that
     rule it is not met until the 26 are mapped.
  2. **The ABI gains a `*_with_options` pair, 0.5 → 0.6**: `fl_publisher_create_topic_with_options`
     and `fl_subscriber_subscribe_with_options`, taking a FIXED `fl_topic_options { fl_str profile;
     uint32_t max_payload_bytes; }` - the seam's struct field for field, fixed like
     `fl_provider_config` because widening it is a change to the seam. `NULL` is the all-empty value,
     and the two existing functions become that case of the new ones, so there is one body each. It
     mirrors the seam's own `*WithOptions` split and the header's append-only rule (existing
     functions never change signature). 46 entry points.
  3. **The schema watch is exposed in C# now, with the options**: `Subscriber.SubscribeSchema(TopicPath)
     → SchemaArrival` and `UnsubscribeSchema(TopicPath)`, beside `TopicOptions` and the overloads
     `Publisher.CreateTopic(topic, schema, TopicOptions?)` and `Subscriber.Subscribe(topic, handler,
     TopicOptions?)`. Options are NOT validated in managed code, for `ProviderConfig`'s reason -
     Fletcher does not know a provider's profiles or bounds - so a caller sees the seam's own
     statuses. A watch from inside a delivery on the provider is refused in managed code first,
     because the seam refuses it ALWAYS (D-BIND-51's pattern); a release from inside one passes the
     seam's `ReentrantCall` through, because only the LAST release enters the provider and this tier
     cannot tell which that is.
  4. **The matrix is checked**: `scripts/check_test_matrix.py` recounts every file both tables name,
     re-adds every row, fails on a test file in no bucket and on the conformance row, and runs on
     **every** pull request (`ci.check-test-matrix`) - not on a path filter, because a merge of
     `main` changes counts without putting those files in the PR's own diff. Its fixture test proves
     nine ways to go stale are each refused for their own reason. Both tables are re-derived: buckets
     1-4 hold **449** cases, **416** in scope, **33** excluded.

  **THE PORT.** Of #128's 26 `pubsub` cases: **12 over `inprocess`** in the unit lane
  (`BucketThreeOptionsTests.cs`) - `inprocess` overrides none of the option or watch methods, so it
  answers the seam's DEFAULT bodies the C++ cases pin through a data-only mock; **11 over real Fast
  DDS** in the transport lane (`TopicOptionsOverFastDdsTests.cs`), because only a provider that KNOWS
  options or has a schema channel can answer them; **3 EXCLUDED** - `SubscribeSchemaDoesNotCreate
  AProviderSubscription` (a mock's provider-level count, D-BIND-47's reason),
  `SubscribeSchemaWaitsForAnInFlightLastRelease` (it parks the PROVIDER inside its own release, and
  C# cannot implement one, D-BIND-24), and `SubscriberAcceptsASecondSubscriptionNamingASubsetOfThe
  Options` (its mock accepts a bound on a subscription, which every conforming provider refuses).
  Bucket 3's `pubsub` file set: 49, 44 mapped, 5 excluded. The C++ cases' provider-call counts are
  not observable from C#, so the Fast DDS mirrors make each refusal UNAMBIGUOUS instead: the refused
  value is one the provider would accept on its own (a valid bound, a profile the document
  defines), so the caller tier's field-wise check is the only thing that can refuse it.

  **FOUND WHILE PORTING.** The Fast DDS provider's named profiles are writer/reader PAIRS
  (`qos_defaults.cpp`) and are meant to be used as pairs. A named reader against the default writer
  was measured: `store_latest`/`store_history` (TRANSIENT_LOCAL) never match a VOLATILE writer, which
  is DDS's own QoS rule; but `latest`, compatible on paper, delivered NOTHING when subscribed before
  the topic was declared, and did when declared first - with matched pairs every order works. The
  tests use matched pairs. The subscribe-first `latest` case is recorded, not chased: it is the
  provider's, not the binding's.

  **PROVEN LOCALLY** (Windows): c-abi **76/76** (five new: `NULL` is the plain form, `inprocess`
  refuses a profile as NOT_SUPPORTED, a bound on a subscription is INVALID_ARGUMENT, Fast DDS takes a
  per-topic bound and refuses changing it, and refuses an undefined profile naming it); managed
  **286/286** on both TFMs; transport **32/32**. **Falsified:** eight compiling mutations - the
  publisher or subscriber dropping the options, the profile crossing a byte short, no managed refusal
  of a watch inside a delivery, `UnsubscribeSchema` never reaching native - each failing its test.
  **CI-confirmed at `bdd4fba`** (`ci.pr` run 36153470901, 50/50 on its second attempt): managed **286/286** on all four
  legs, transport **32/32** and c-abi **76/76** on both platforms, binding-abi **11/11**, pubsub-conformance
  **146/146**, the XRCE interop lane green. The first attempt failed ONE case on Windows -
  `XrceCrossProcess/ProviderConformance.SubscribeNeverBlocksSchemaArrivesLater`, "XRCE: failed to create
  subscriber participant (status=255)" just after the Agent established the session - in a suite this
  push did not touch (nothing under the providers, `pubsub`, `core` or the conformance suite changed;
  the lane ran only because `c-abi` did). It passed on the re-run and has passed on every earlier run of
  this branch: recorded as an intermittent XRCE failure, cause not established, not as fixed. BIND-4 re-closes on that run.

  **Declined:** freezing bucket 3 at the port (BIND-4's own file-set rule says otherwise); a new
  tracker item (the bullet is BIND-4's); changing the existing two functions' signatures (breaks the
  header's append-only rule for a one-form convenience); keeping the watch owed (nine cases would be
  recorded as owed rather than ported); re-deriving by hand (it was the hand that forgot).

- **D-BIND-58 — BIND-5's copy oracle scores the REAL managed publish through a test-only shim that
  registers the probe; the attachments claim is the DELIVERY view, and a zero-copy managed publish
  of attachments is deferred with a trigger.** *LOCKED BY THE MAINTAINER 2026-09-28,* in two
  questions, before BIND-5's code.

  **WHY THE BULLET COULD NOT BE MET AS WRITTEN.** It asked for "the copy oracle run with the C#
  producer ... zero-copy for rows and attachments, from managed code, falsifiable". The oracle
  scores through `SeamProbeProvider`, a harness-private C++ provider the shipped shim cannot select
  (D-BIND-24), which is why BIND-2 could measure only `fl_encode_row` into the probe's window
  (D-BIND-34). That route cannot catch either way a MANAGED producer loses zero-copy: `Publisher.
  Publish` staging the encoded row in managed memory, or the C Data Interface export copying the
  Arrow buffers. And managed attachments are copied on publish BY RULING - into the builder
  (D-BIND-44) and again by `fl_blob_create` (D-BIND-42 allows no view-only blob). `SubscriberArrow`
  copies borrowed rows by design, so no zero-copy claim covers it: the claim is publish, and the
  borrowed delivery view (`RowHandler`, `AttachmentsView`).

  **THE RULING.**
  1. **Rows: a test-only shim variant with the probe registered, never shipped.** Built from
     c-abi's existing object library plus `SeamProbeProvider` plus a small `fl_test_*` export set that
     reads the ledger, in a test header of its own - NOT `binding.h`. C# selects it by name and calls
     the real `Publisher.Publish`; the probe scores `encode_copies` from inside the encoder, by the
     same `Judge()` as every other leg, and a deliberately staging managed publish must score exactly
     1 as the negative control. D-BIND-34 rejected a registration seam IN THE SHIPPED shim; a separate
     test artifact does not reopen that door, and only one copy of Fletcher is loaded, so D-BIND-17
     holds. The probe moves out of `integration-tests/pubsub-conformance` into test support both can
     use, rather than being copied.
  2. **Attachments: the delivery view, now.** The bullet is amended: publishing attachments from C#
     copies (D-BIND-44, accepted - sidecar metadata); what is proven is that a handler's
     `AttachmentsView` hands it the transport's OWN bytes, by address, scored as
     `attachment_copies == 0`. **Deferred, with a trigger:** a zero-copy managed publish of
     attachments - an owned-by-callback blob over pinned managed memory (`fl_blob_wrap`), which would
     reopen D-BIND-42 and D-BIND-44 at ABI 0.7 - when a consumer's attachments are large enough that
     the copy costs. *(Note 2026-09-29: ABI 0.7 went to D-BIND-68's framed decode, so this would
     take the next ABI minor. The ruling is unchanged.)* The pattern `BlobHandle` follows (D-BIND-55).

  **Declined:** a managed mirror of D-BIND-34 (a native library lending a window to `fl_encode_row`
  would measure the codec path BIND-2 already measured and miss both managed failure modes);
  amending to proxies (pointer equality on the export and zero managed allocation per publish -
  kept in mind as cheap supporting tests, but an `ArrayPool`-staging publish passes an allocation
  check); building the attachment publish path now (ABI and lifetime surface for a need no consumer
  has stated).

- **D-BIND-59 — `SubscriberArrow` has ONE handler shape: per-row delivery is a batch of one, and
  a row's attachments arrive as owned `AttachmentsBuilder` copies.** *LOCKED BY THE MAINTAINER
  2026-09-28,* in two questions, at the start of BIND-5a.

  **THE QUESTIONS.** The public-surface note drew two handler delegates, `RowBatchHandler` for
  `Subscribe` and `RecordBatchHandler` for `SubscribeBatched`, and left open what a batch handler
  receives for attachments - the borrowed `AttachmentsView` cannot outlive a delivery, and a batch
  outlives every delivery it holds.

  **THE RULING.**
  1. **Per-row is `SubscribeBatched` with `MaxRows = 1`.** `Subscribe(topic, handler, topicOptions?)`
     takes the same `RecordBatchHandler` and flushes on every row, as C++'s per-row `SubscriberArrow`
     is a window of one. `RowBatchHandler` is dropped from the note. A caller who wants the borrowed
     row itself uses `Subscriber` and `RowHandler`, which is where D-BIND-58's delivery-view claim is.
  2. **Attachments are the existing `AttachmentsBuilder`, filled with owned copies,** index-aligned
     with the batch's rows, with read accessors added (`KeyAt`, `ValueAt`, `TryFind`). No new type.
  3. **Carried with it, not asked separately:** the handler OWNS the batch and disposes it; the batch
     is `RecordBatch?` - zero rows when a window held only dropped rows, NULL only when the topic's
     schema could not be opened (every row dropped for the life of the subscription); `SubscriberArrow`
     forwards `TopicOptions` on both forms and exposes `SubscribeSchema`/`UnsubscribeSchema`, so it
     answers the same bucket-3 cases `Subscriber` does (D-BIND-57). Dictionaries arrive as their value
     type (D-BIND-8), so `FinishFailureIsReportedNotFatal` is excluded - there is no re-fold to fail.

  **Declined:** a second delegate for the per-row form (two shapes for one window rule); a new
  owned-attachments type (the builder already holds exactly an owned key/value set).

- **D-BIND-60 — the shim refuses rows bound under a schema other than the topic's.** *LOCKED BY THE
  MAINTAINER 2026-09-28,* in two questions, when BIND-5a's port found the defect.

  **THE DEFECT.** C++'s `PublisherArrow` holds one codec per topic, so a type mismatch
  (`PublishTypeMismatchThrowsToCaller`) cannot reach the wire. A binding's `BoundRows` carry THEIR
  OWN codec, and `fl_publisher_publish_row`/`_rows` encoded them with it, unchecked: a probe
  published an int64 row onto an int32 topic and it was delivered, 9 bytes, decoded as garbage. A
  same-width mismatch (float32 for int32) is not even detectably wrong on arrival.

  **THE RULING.** Fixed **in the shim**, **inside 5a** (not a follow-up item). `fl_codec` computes its
  `DeclaredSchema` once; `fl_rows` carries it; `fl_publisher` records each topic's declaration after
  the seam accepts it; both publish entry points compare with the seam's own `ConflictsWith` and
  refuse FL_INVALID_ARGUMENT before encoding. A topic this publisher did not declare is left to the
  seam, as before. `binding.h` documents the rule on both functions; ABI stays 0.6 (no signature
  changes, and a publish that was accepted before was a silent corruption, not a contract). Tests:
  c-abi `Publisher.RowsBoundUnderAnotherSchemaAreRefusedOnBothPublishes` /
  `...UnderTheDeclaredSchemaAreServed`; C# `SubscriberArrowTests.RowsOfAnotherSchemaAreRefused...` /
  `APublishAfterARefusedOneStillDelivers`.

  **AN OPEN FINDING, NOT FIXED.** The first version threw the `PubSubError` while holding a
  `std::lock_guard` on the new mutex; the NEXT call then failed FL_INTERNAL, "resource deadlock would
  occur" - the guard's destructor had not run on the way out through `Contain`. Standalone MSVC repros
  (`/EHsc`, `/EHs`, the `catch(...)`-and-rethrow classification pattern) did not reproduce it. The
  shipped code computes under the lock and throws after release, which is correct whatever the cause.
  **Cause not established;** it may mean destructors are skipped on some shim error path, which would
  matter beyond this mutex. Owed: a follow-up that reproduces it inside the shim's own build flags.

  **Declined:** a managed-only check in `Publisher.Publish` (Rust and every later binding would owe it
  again, and it could not use the seam's comparison); deferring to a follow-up item (the port cannot
  claim `PublishTypeMismatchThrowsToCaller` without it).

  **The open finding's cause, established 2026-09-29 (BIND-5 review B1).**
  - **Where the helper was.** It had been declared in an unnamed namespace INSIDE `binding.cpp`'s
    `extern "C"` block, so it had C language linkage.
  - **Why that matters.** MSVC's `/EHsc`, the shim's setting, assumes a C-linkage function never
    throws, and may drop the unwind actions around it.
  - **Confirmed by experiment.** The first version's lock-held throw was rebuilt twice:
    - inside the block, it reproduced "resource deadlock would occur";
    - moved above the block, the same code passed.

    A standalone build in the same shape failed fast under `/EHsc` and unwound correctly under
    `/EHs`.
  - **Fixed.** The helper is moved above the block, and the block now carries the rule: nothing but
    `fl_*` definitions inside it. The shipped version released the lock before throwing, but still
    threw from C-linkage code, so every refused publish on Windows was undefined until this fix.
    This was not a Contain/Capture defect.
  - **And c-abi now builds with `/EHs` on MSVC**, by the maintainer's decision the same day. That
    removes the assumption itself: the original defect, rebuilt under `/EHs`, passes.

- **D-BIND-61 - the probe scores C#'s FUSED publish by the SOURCE of the window's payload bytes,
  seen through `WriteBuffer::Append`; the negative controls are a staging publish and a copied
  export.** *LOCKED BY THE MAINTAINER 2026-09-28,* in two questions, at the start of BIND-5b, after
  the mechanism was explained in detail.

  **WHY D-BIND-58 WAS NOT ENOUGH.** It said the probe scores `encode_copies` "from inside the
  encoder". The ledger gets that number from the PRODUCER reporting where it composed the row
  (`produced_at == the window's cursor`); BIND-2d's producer was harness code and could report. On
  the fused path (`Publisher.Publish(topic, rows, i)` -> `fl_publisher_publish_row`) the producer is
  the codec inside the shim, and nothing reports - so a correct publish and one that encoded into
  managed memory and went through `PublishRaw` both score `row_copies == 0` and neither yields an
  `encode_copies`.

  **THE RULING.**
  1. **Trace sources.** The codec writes a binary field with `WriteBinary` -> `Append(data, len)`,
     `data` pointing into the Arrow buffer the C Data export shares. `Append` is inline and reaches
     the virtual `AppendSlow(src, len)` only when the window is full, so the probe's window is kept
     EXACTLY full (`capacity == pos` between writes, over a fixed arena slot that never moves):
     every `Append`/`AppendByte`/`AppendFixed` is recorded with its source, `AppendZeros` as zeros,
     and `AppendInPlace` as a lent span whose contents have no source. The C# test supplies A, the
     address of the payload in its own batch; `encode_copies == 0` iff the payload arrived in one
     recorded `Append` with `src == A` and the payload's length. `Judge()` is unchanged:
     `produced_at` is the address the payload came from, `produced_in_window` means "no
     intermediate". `row_copies` is scored as today.
  2. **Controls, each exactly 1:** a STAGING publish (`FletcherCodec.Encode` into managed memory,
     then `PublishRaw` - the payload arrives through a lent span) and a COPIED EXPORT (A recorded,
     then a deep copy of the batch bound and published - the payload arrives from A' != A).

  **Bounds, stated with it.** Only variable-length fields carry provenance - scalars are read by
  value and appended from a stack temporary - so the leg uses a one-binary-field row, as BIND-2d
  did, and the claim is for payload bytes. A write the probe did not record must FAIL the leg: the
  probe asserts its records cover exactly `[0, Position())`, so an `Append` that one day bypasses
  the capacity check makes the leg red, not vacuously green. It proves the managed side added no
  intermediate; it trusts the codec to be the encoder, as every leg does.

  **Declined:** classifying by path (in-place fill = staged: a route check, blind to a copied
  export); scoring the `RowWriter` form only (leaves the main path unmeasured, the bullet unmet),
  and with it a staging-`RowWriter` control.

- **D-BIND-62 - the copy-oracle instrument moves into a TEST-ONLY package, `fletcher-copy-probe`,
  and c-abi builds the probe shim only under an option.** *LOCKED BY THE MAINTAINER 2026-09-28,* in
  one question, completing D-BIND-58's "test support both can use", which named no home.

  **WHY IT NEEDED A RULING.** CI builds c-abi with `conan create`, which sees only `c-abi/`, so c-abi
  cannot reach an instrument under `integration-tests/`; and the probe shim links c-abi's internal
  object library, so it can only be built in c-abi's own CMake.

  **THE RULING.** A new recipe, `integration-tests/copy-probe/` (`fletcher-copy-probe`), holds the
  instrument - the ledger, `Judge()`, `SeamProbeProvider` and D-BIND-61's tracing window; the gtest
  clauses and runners stay in `pubsub-conformance`, which requires the package. It is NEVER released.
  c-abi requires it only under a new `with_probe_shim` option, and only then builds
  `fletcher-c-abi-probe`: the object library, the probe registered as a builtin, and `fl_test_*`
  exports declared in a test header of their own, never `binding.h`. The dotnet and transport lanes
  build c-abi with the option; release packages are built without it and so cannot contain the probe.

  **Declined:** shipping the instrument as `fletcher/pubsub/testing/` headers (test code inside a
  product package); owning it in `c-abi/tests` (the lanes would dig a DLL out of Conan's build folder,
  and the conformance harness would depend on c-abi's test tree).

  **As built (BIND-5b), one sentence of the ruling narrowed:** it said "the dotnet and transport
  lanes build c-abi with the option". Only the lanes that LOAD the probe shim do: `ci.dotnet.yml`,
  for `Fletcher.CopyOracle.Tests`, and `ci.c-abi.yml`, for `c_abi_probe_tests`. The transport lane
  never loads it and builds c-abi as before. The pubsub-conformance lane creates the package, which
  its harness now requires.

- **D-BIND-63 - `SubscriberArrow`'s window has C++'s ceiling: a row that would take it past the
  ceiling flushes it first, as RowLimit, and a row that alone exceeds it is dropped and counted.**
  *LOCKED BY THE MAINTAINER 2026-09-28,* in one question, after a review comparing the C++ and C#
  calling sequences found the gap while 5b's CI ran.

  **THE GAP.** C++'s batcher flushes early when an append would overflow an Arrow 32-bit offset
  (`BatchCapacityExceeded`, 2^31-2 bytes or elements in one builder) and appends the row again. C#'s
  batcher, as 5a shipped it, appended raw rows to an `ArrayBufferWriter` with no bound. At about 2 GiB
  the copy threw, the Subscriber thunk absorbed it as a HANDLER fault rather than a counted drop, and
  every later row of that window was lost the same way. It is reachable with the defaults: 8000 rows
  and 1 minute on a topic of 1 MB binary rows reach 2 GiB after about 2000 rows.

  **THE RULING.** Mirror C++, with no new public API.
  1. The ceiling is C++'s number, `2^31-2`, lowered to `Array.MaxLength` (55 bytes less), the most one
     managed buffer can hold: `SubscriberArrow.DefaultWindowByteCeiling`. An internal constructor
     takes a lower one, for tests only.
  2. A row that would take the pending window past it flushes the window first, reported as
     `RowLimit` as C++ reports its split, and then starts the next window. The check is repeated
     after the flush, since another delivery may have added to the new window in between.
  3. A row that alone exceeds the ceiling is dropped and counted in `RowsDropped`.
  4. A failure to copy a row into the window is counted as a drop, never absorbed as a handler fault.

  **How it differs from C++, stated with it.** C# bounds the RAW window, because it holds raw rows
  until flush, while C++ bounds each column's builder. The raw bound implies the column bound, since
  a column's bytes are a subset of the rows'. So the same topic splits at the same place give or take
  the rows' framing bytes, never later than C++. What it does not bound, as C++ does, is ELEMENT
  counts: a list of bit-packed booleans can reach 2^31 elements in 256 MiB. That window fails to
  decode and is dropped whole and reported, not silently.

  **Corrected 2026-09-29 (BIND-5 review M-D2).** "A column's bytes are a subset of the rows'" is
  FALSE for null slots. The wire writes nothing for a null, so a null `FixedSizeList<float, 65536>`
  or a null fixed-width field decodes to far more than its raw bytes, and the raw ceiling does not
  bound decoded buffers there. Such a window fails to decode and, since D-BIND-66, is counted as
  dropped rows in a zero-row batch. It is never lost uncounted, but it is not split as C++ would
  split it. That limit stands beside the element-count one above.

  **Declined:** a public `BatchOptions.MaxBytes` (a knob with no C++ counterpart, and a surface
  change); documenting the limit only (rows lost as handler faults, and C++ behaving differently for
  the same topic).

- **D-BIND-64 - `SubscriberArrow` does not serialise its handler calls, matching C++; `Stop` waits
  for an in-flight timer flush, so no handler call runs after `Unsubscribe` or `Dispose` returns.**
  *LOCKED BY THE MAINTAINER 2026-09-29,* in two questions, answering the BIND-5 review's Q2 (code
  review B2).

  **THE DEFECT.** 5a held a lock, `_deliver`, across every handler call, to promise that a handler
  is never called twice at once - a guarantee C++'s `SubscriberArrow` never gave. It deadlocks. A
  timer-thread handler holds the lock; a delivery fills the window and waits for the lock while
  still inside its native delivery; the handler then cancels, and the native cancel waits for that
  delivery to drain. Reproduced.

  **THE RULING.** Match C++: no lock is held while the handler runs.
  1. A `Timeout` flush (timer thread), a `RowLimit` flush (delivery thread) and a `Closing` flush
     (the cancelling thread) may run the handler concurrently, and batches may complete out of
     order. Each window is still cut in order, under the state lock. This is documented in the
     header and the surface note, and handlers must be thread-safe.
  2. `Stop` waits for an in-flight timer flush to finish, unless it is running on that flush, as
     C++'s `Stop` joins its timer thread unless called from it. So no handler call runs after
     `Unsubscribe` or `Dispose` returns.
  3. **A limit shared with C++, recorded rather than fixed:** a delivery-thread handler that cancels
     while a timer-thread handler also cancels can still deadlock. `Stop` waits for the timer
     handler, and that handler's native cancel waits for the delivery.

  **Consequences.** The D-BIND-59 note's "a handler is never called twice at once" is withdrawn, as
  is the recorded divergence "one step stronger than C++". The B2 reproduction becomes a regression
  test, and a new test shows a timer flush and a row-limit flush overlapping.

  **Declined:** keeping the serialisation and deferring the native cancel until the handler returns
  (a C# guarantee C++ does not have, and more machinery); refusing a cancel from inside a timer or
  closing flush (breaks `UnsubscribeFromInsideATimeoutFlushIsSafe`, which C++ requires to work);
  dropping the lock without the wait (a timer handler could run after `Unsubscribe` returned, which
  C++ does not allow).

- **D-BIND-65 - the D-BIND-60 publish check compares WIRE LAYOUT, not IPC bytes.** *LOCKED BY THE
  MAINTAINER 2026-09-29,* in one question, answering the BIND-5 review's Q1.

  **WHY IT CHANGED.** D-BIND-60 compared the two schemas' IPC bytes with the seam's
  `DeclaredSchema::ConflictsWith`. That was stricter than needed: field names, metadata and
  nullability were refused although the wire bytes are identical. It was also weaker than it
  looked: a schema carrying a dictionary is not IPC-encodable, so `ConflictsWith` answered "not
  provably different" and the check was silently skipped for every such topic (review N-D3).

  **THE RULING.** Refuse exactly the rows that would encode differently from the topic's own codec:
  compare the codecs' field plans (`FieldPlan {type, fixed_size, children}`), the type tree the wire
  format is built from.
  - Names, metadata and nullability no longer cause refusals.
  - Dictionaries are checked by their plan instead of being skipped.
  - This is the closest to C++ `PublisherArrow`, which fails only when a value's type does not fit
    its codec.
  - The shim builds the declared schema's plan once, at `CreateTopic`, and caches the verdict per
    (rows, topic), which also takes the per-row cost off the hot path (review N-D4).

  `binding.h` documents the rule on both publish functions. This is not an ABI change: the same
  refusal and the same status, for a narrower and exact set of rows.

  **Declined:** keeping the strict byte comparison and documenting it (stricter than C++); changing
  only the documentation (leaves every dictionary topic unchecked).

- **D-BIND-66 - every row `SubscriberArrow` receives either arrives in a batch or is counted in
  `RowsDropped`; its intake never lets an exception reach the inner `Subscriber`.** *LOCKED BY THE
  MAINTAINER 2026-09-29,* in one question, answering the BIND-5 review's Q3 (code review B5, B6).

  **THE DEFECT.** `OnRow` runs as the inner `Subscriber`'s handler, so an exception out of it was
  absorbed there. The row was then lost without appearing in `RowsDropped` or in `SubscriberArrow`'s
  own fault counter or event. Two concrete causes:
  - an empty attachment key, which C++ accepts and C#'s `AttachmentsBuilder` refuses;
  - a schema Apache.Arrow's importer refuses with an exception `Resolve` did not catch.

  **THE RULING.** Match C++'s `AddRow`, which catches everything and counts the row as dropped.
  1. `OnRow` contains every exception itself and counts the row as dropped in the next batch's
     `RowsDropped`.
  2. The same holds for a flush's own work outside the caller's handler: the window copy, the decode,
     the import. A window lost there is counted, never lost uncounted, and never escapes onto the
     timer thread (review B5).
  3. `HandlerFaulted` and `AbsorbedCallbackFailures` stay reserved for the caller's own handler.
  4. The specific causes are fixed as well:
     - empty keys cross, as in C++, because the intake copies attachments without the builder's
       public-API refusal;
     - `Resolve` treats any import failure as an undecodable schema.

  **Declined:** surfacing the losses as handler faults (a fault in code the caller did not write,
  with `RowsDropped` still undercounting); reporting through both channels (one lost row reported
  twice, with two meanings).

- **D-BIND-67 - the bound-rows publishes refuse a topic THIS publisher did not declare,
  `FL_TOPIC_NOT_DECLARED`, as C++ `PublisherArrow` does.** *LOCKED BY THE MAINTAINER 2026-09-29,* in
  one question, answering the BIND-5 review's Q4.

  **THE GAP.** D-BIND-60 checked the rows only against a declaration made by the same `fl_publisher`,
  and left every other case to the seam. The seam's `Publisher` does not require a declaration, and
  providers differ: Fast DDS refuses an undeclared topic, while `inprocess` accepts one by default.
  So rows published through a second publisher, or to a topic nobody declared, went unchecked.
  C++ is stricter: `PublisherArrow::Publish(ArrowRow)` throws "no codec for topic" unless that
  `PublisherArrow` declared it.

  **THE RULING.** Match C++.
  - `fl_publisher_publish_row` and `fl_publisher_publish_rows` refuse `FL_TOPIC_NOT_DECLARED` unless
    this publisher declared the topic, so every codec-encoded row is checked against a known schema
    (D-BIND-65's wire layout).
  - The raw paths stay unchecked, as C++'s `PublishDirect` is: `fl_publisher_publish_raw`, and the
    managed `PublishRaw` and `RowWriter` forms.
  - `binding.h` states the rule on both functions. The status already exists, so this is not an ABI
    change, but it IS a behaviour change: C# code that publishes `BoundRows` through a `Publisher`
    that never called `CreateTopic` now fails. The existing suites are checked for that when this
    lands.

  **Declined:** leaving it to the seam (provider-dependent, looser than C++); keeping declarations
  per provider (looser than C++, with state shared across publishers).

- **D-BIND-68 - ABI 0.7 adds `fl_decode_rows_framed`: one native call decodes a window whose row
  boundaries are known, skipping and reporting each row that fails or does not end at its
  boundary.** *LOCKED BY THE MAINTAINER 2026-09-29,* in one question, while fixing the BIND-5
  review's B3 and B4.

  **WHY.** C++'s `BatchDecoder` decodes every row as it arrives, so it knows which rows are bad
  (only good rows count toward `max_rows`, B3) and where each message ends (a message that does not
  end on a row boundary is dropped, B4). C# holds raw rows and decodes the window in one native call
  at flush (D-BIND-25). The only decode, `fl_decode_rows(bytes, len, count)`, checks the total length
  and nothing per row. B3 could be patched from managed code with the existing fallback; B4 could
  not, because rows split across two messages still decode as a valid window.

  **THE RULING.**
  - **The call.** `fl_decode_rows_framed(codec, bytes, len, ends, count, out, valid, err)` decodes
    `count` rows, where row `i` occupies `[ends[i-1], ends[i])`.
  - **What counts as bad.** A row that fails to decode, or does not end exactly at its boundary, is
    skipped with `valid[i] = 0`. It is a bad row only when the failure is the reader's
    (`std::invalid_argument` or `std::out_of_range`). Anything else fails the whole call.
  - **What comes back.** `out` holds the good rows only, in order, and the call returns `FL_OK` even
    when rows were skipped.
  - **How a bad row is skipped:** by restart, not by a second walker. Rows are decoded in sequence;
    on a bad row the array is discarded and the rows already known good are decoded again, so the
    decoder stays the one source of truth. Bad rows are rare, so the restart costs nothing in the
    common case.
  - **Compatibility.** ABI 0.7 is append-only: the handshake's exact-minor check moves to 7, and
    `fl_decode_rows` is unchanged.
  - **Scope.** The managed side keeps the call internal to `FletcherCodec`; `SubscriberArrow` uses
    it, so the per-row fallback pass goes away.
  - **B3 on top of it.** A `RowLimit` flush that finds bad rows puts the good ones back at the front
    of the window and waits for more instead of delivering short. The drops ride with the batch
    that is finally delivered, as in C++.

  **Declined:** decoding each row alone with today's call (N native calls and N arrays per window,
  undoing D-BIND-25); fixing B3 alone and documenting B4 (a publisher of malformed data could
  misalign rows and attachments silently).

- **D-BIND-69 - generated C# takes its namespace and nesting from Fletcher's backends, and its
  identifiers from protoc's C# rule.** *LOCKED BY THE MAINTAINER 2026-09-29,* in one question, at
  the BIND-5 → BIND-6 boundary.

  **WHY.** Fletcher's generated code is self-contained: C++ lives under `fletcher_gen::<pkg>` in
  `<stem>.fletcher.pb.h`, references nothing of protoc's generated code, and names a nested message
  `Outer_Inner`, as TypeScript and Rust do. None of the BIND documents said what the C# backend does
  about three things:
  - **The namespace.** Honouring `option csharp_namespace` puts our `sealed class Player` in the
    same namespace as protoc's own `Player`, so a project that runs both generators fails with
    CS0101.
  - **Nesting.** protoc's C# nests as `Outer.Types.Inner`; every Fletcher backend flattens.
  - **Identifiers.** The generator sanitises no identifier anywhere. C++ is saved by its `set_` and
    trailing `_` decoration, and TypeScript by keywords being legal property names. C# is the first
    backend where a raw proto name can fail to compile: `message Player { string player = 1; }`
    gives CS0542, and a field named `class`, `event` or `lock` does not parse. Fletcher has no
    precedent here, so the rule is borrowed.

  **THE RULING.**
  1. **Namespace `Fletcher.Gen.<PascalPkg>`**, the C# spelling of `fletcher_gen::<pkg>`.
     `option csharp_namespace` is **ignored**, so our types never share a namespace with protoc's.
  2. **Nested messages stay flat as `Outer_Inner`**, the name C++, TypeScript and Rust already use,
     so a type has one name in every language. This differs from protoc's `Outer.Types.Inner` **on
     purpose**. Do not "fix" it toward the protobuf convention.
  3. **Properties are PascalCase, with protoc's `_` suffix when a name collides with its class**
     (`Player.Player_`). PascalCase also makes a keyword field legal (`class` → `Class`). This is
     protoc's C# rule, adopted because C# developers already know it.

  **Example.** `package integration; option csharp_namespace = "Eiva.Integration";` with
  `message Player { string player = 1; int32 class = 2; message Stats {} Stats stats = 3; }` emits
  `namespace Fletcher.Gen.Integration;`, `sealed class Player_Stats`, and `sealed class Player` with
  `Player_`, `Class` and `Stats` (of type `Player_Stats`).

  **Left to BIND-6, not ruled here:** how a dotted or snake_case package is PascalCased segment by
  segment, and what an empty package yields; a property colliding with a GENERATED member
  (`Schema`, `ToArrow`, `FromArrow`) or with another field after PascalCasing. Each is raised as a
  question if protoc's rule does not answer it. *(Another field after PascalCasing: ruled by D-BIND-89.)* **Separate questions at the same boundary:** enum
  member naming, and whether BIND-6 ships an MSBuild `.targets`.

  **Declined:** honouring `csharp_namespace` (CS0101 beside protoc's output); following protoc
  throughout, with `Outer.Types.Inner` (one type would be named differently in C# than in every
  other Fletcher language).
  *(Enum member naming: answered by D-BIND-70 the same day. The MSBuild question: answered by
  D-BIND-71.)*

- **D-BIND-70 - generated C# names enum members by protoc's C# rule: strip the enum-name prefix,
  then PascalCase.** *LOCKED BY THE MAINTAINER 2026-09-29,* in one question, at the BIND-5 →
  BIND-6 boundary.

  **WHY.** D-BIND-8 rules a real C# `enum`, int32-backed, and says nothing about member names. The
  only precedent is C++, which copies the proto names verbatim from the descriptor
  (`protoc/src/generator.cpp`, `EmitEnumClass`), so C++ callers write `Color::COLOR_RED`.
  TypeScript and Rust expose an enum field as int32 and emit no names. `EnumIdentity::symbols`
  holds the raw names (`protoc/src/ir.cpp`) and BIND-6 is its first reader. Verbatim names are
  unidiomatic in C#, and analysers flag the underscores (CA1707).

  **THE RULING.**
  - **Members:** strip the enum-name prefix, then PascalCase what is left, as protoc's C# generator
    does: `COLOR_UNSPECIFIED`, `COLOR_RED`, `COLOR_DARK_BLUE` in `enum Color` become
    `Unspecified`, `Red`, `DarkBlue`. This extends D-BIND-69's choice to borrow protoc's
    identifier rule.
  - **Numbers:** every member keeps its proto number verbatim, so wire bytes are unchanged and
    open proto3 enums still round-trip (D-BIND-8).
  - **The type name:** a nested enum is flattened like a nested message under D-BIND-69
    (`Player_Color`).
  - **The accepted cost:** the same value is `Color::COLOR_RED` in C++ and `Color.Red` in C#, so
    cross-language documentation states the mapping.

  **Left to BIND-6, answered by protoc's rule where it has one:** a remainder that starts with a
  digit, two members that collide after stripping, a value with no prefix to strip, and
  `allow_alias` duplicates. A case protoc's rule does not answer is raised as a question.

  **Declined:** verbatim like C++ (unidiomatic, analyser noise); PascalCase keeping the prefix
  (`ColorRed`, which repeats the type name in every member).

- **D-BIND-71 - C# consumers run the generator the way C++ and TypeScript consumers do: one plugin
  binary, a thin locator per ecosystem, and the consumer writes the protoc call. The C# locator is
  `Eiva.Fletcher.Protoc`, a NuGet package of the PROTOC component.** *LOCKED BY THE MAINTAINER
  2026-09-29,* in two questions, at the BIND-5 → BIND-6 boundary. This is D-BIND-14′'s
  STOP-AND-ASK for a fourth package, answered.

  **WHY.** The plan had no build story for C#. The first question offered no `.targets`, a fourth
  package, or a `.targets` that downloads the plugin; the maintainer asked instead for a pattern
  that matches what C++ and TypeScript already do and what Rust will do. Reading the tree showed
  one, never written down:
  - **One binary.** `cd.protoc.yml` releases `fletcher-protoc` once per `protoc-v` tag, as a Conan
    package and as statically linked `linux-x64` and `win-x64` binaries on GitHub Releases. Every
    language selects its output with `--fletcher_opt`.
  - **A thin locator per ecosystem, and nothing more.** C++: the Conan package's
    `fletcher-protoc::plugin` imported target (`protoc/cmake/fletcher-protoc-target.cmake`), which
    also sets `FLETCHER_PROTO_INCLUDE_DIR`. TypeScript: `@eiva/protoc-gen-fletcher`, released by the
    same `cd.protoc.yml`, whose bin downloads the matching binary. Rust, in-tree:
    `FLETCHER_PROTOC_PLUGIN`, read by `build.rs`. No ecosystem gets a "generate my protos" macro.
  - **The consumer writes the protoc call** in its own build tool, and takes protoc itself from its
    own ecosystem (Conan's `protobuf`, `@protobuf-ts/protoc`).

  **THE RULING.**
  1. **The pattern above is the rule for every language**, now written down.
  2. **C#'s locator is `Eiva.Fletcher.Protoc`**: it bundles the two plugin binaries (NuGet's norm,
     so a build needs no network) and sets one MSBuild property, **`$(FletcherProtocPlugin)`**, the
     counterpart of `fletcher-protoc::plugin`. A consumer references it with `PrivateAssets="all"`
     and writes an `Exec` of protoc before compile. protoc itself comes from the consumer's side,
     for example `Google.Protobuf.Tools`.
  3. **It is the protoc component's package**, versioned by `protoc-v` and released by
     `cd.protoc.yml` beside the npm shim. The dotnet component keeps its three packages
     (D-BIND-14′, clarified), and `cd.dotnet.yml` is unchanged.
  4. **Built in BIND-9**, with the rest of the packaging. BIND-6's `protoc-dotnet` test sets the same
     property from `FLETCHER_PROTOC_PLUGIN`, so its project file does not change when the package
     arrives.
  5. **Rust, when its locator comes, follows the same shape**: a build-dependency crate exposing the
     binary's path to `build.rs`.

  **Left to BIND-9:** how `Google.Protobuf.Tools` exposes protoc's path (verified, not assumed);
  whether the package also carries `fletcher/options.proto` and sets an include property, as the
  C++ locator's `FLETCHER_PROTO_INCLUDE_DIR` does (recommended, for parity); and the NuGet publish
  step in `cd.protoc.yml`, which reaches `nuget.eiva.com` only from D-BIND-28's self-hosted runner.
  **A consequence for D-BIND-28:** that runner now serves two tag-triggered publish jobs, not
  one. The property D-BIND-28 protects, that it never runs pull-request code, holds for both.
  **Added 2026-09-29, from ADO 18789's review (comment 23089):** BIND-9 also makes the plugin
  executable after NuGet extraction on Linux; tests the plugin (protobuf 3.21.12, proto3
  `optional` declared, editions not) against the protoc the consumer is told to use; and relies
  on the equal-`MAJOR.MINOR` rule to keep generated code (`protoc-v`) and runtime (`dotnet-v`)
  compatible, with a change to the generated surface bumping MINOR on both.

  **Declined:** no locator for the first release (every consumer would fetch the binary by hand);
  a Grpc.Tools-style `<FletcherProto Include>` item (more than any other language gets, and more
  to maintain); a `.targets` inside `Eiva.Fletcher` that downloads the plugin at build time (a
  runtime package carrying build logic, and builds that need GitHub).

- **D-BIND-72 - every language's generated output has one shape: per `.proto`, one file holding a
  row type per message, ONE schema bound to it, and the topics; every runtime consumes that same
  row type and schema. C#'s gateway client therefore takes the generated `Player` and
  `Player.Schema`, and gets no descriptor of its own.** *LOCKED BY THE MAINTAINER 2026-09-29,* in
  one question, after reading ADO 18789.

  **WHY.** 18789 asks for C# versions of **all** the plugin's outputs. The tracker mapped it to
  BIND-6 (row) and BIND-7 (view and accessor). But for TypeScript the plugin also emits a
  `TypedSchema<T>` per message and a topic constant per service method, which the gateway client
  consumes. BIND-8's `Eiva.Fletcher.GatewayClient` takes a `TypedSchema<T>` in
  `SubscribeAsync<T>`/`PublishAsync<T>` (public surface §5.4), and **no item generated one for
  C#**: a gateway user would have written descriptors by hand. Oliver Monberg-Jensen's design
  comment on 18789 (2026-09-21) proposed emitting only such a descriptor, bound to protoc's own
  `--csharp_out` class. The maintainer asked for an answer C++, C#, TypeScript and Rust can all
  follow. Reading the backends showed the shape already exists:

  | | Row type | Its schema | Topics, per service method | Typed pub/sub pair |
  |---|---|---|---|---|
  | C++ | `class Player` | `PlayerSchema()` (Arrow) | `TopicSegments()`, `TopicKey()`, **members of the pair** | native |
  | TypeScript | `interface Player` | `PlayerSchema: TypedSchema<Player>` | `Svc_MethodTopic`, a standalone constant | gateway (BIND-T) |
  | Rust | none yet (accessor only) | none | none | none |
  | C# | `sealed class Player` | `Player.Schema` (Arrow) | `Topic`, `TopicKey` | native (BIND-6), **gateway: none** |

  C# is the first language with TWO runtimes, which is why the gap appeared there first.

  **The shape is shared; the mechanism is not.** C++'s generated class IS the codec for its
  message: `EncodeTo`, `DecodeInto` and the byte constructors run `positional_io` directly. C#'s
  generated class writes no wire bytes: `ToArrow` hands a `RecordBatch` to the one C++ codec behind
  the C ABI (D-BIND-1), and the gateway client's managed codec is D-BIND-1's single exception.
  TypeScript's `TypedSchema<T>` drives the gateway client's codec in the same way.

  *Corrected 2026-09-29, the day it was ruled and before any BIND-6 code, after the maintainer asked
  for the C++ output to be compared with this entry: the table now says where each language keeps
  its topics (C++ has no standalone topic; it lives on the pair); the paragraph above was added; the
  "declined" reason said C++, TypeScript AND Rust own a row type, and Rust has none yet. Points 1 to
  5 are unchanged. **Point 6 is new, accepted by the maintainer the same day.***

  **THE RULING.**
  1. **The shape is the rule for every language:** per `.proto`, one generated file with a row type
     per message, one schema bound to it, and a topic per service method. Every runtime consumes
     that row type and schema, and **no runtime gets a model type or schema of its own.** Typed
     pub/sub pairs are emitted per runtime. Where the topic lives follows the language: on the pair
     in C++, as a constant in TypeScript, and in C#'s model layer, because both C# pairs need it.
  2. **C#'s gateway client consumes the same `Player` and `Player.Schema` as the native path.** It
     derives the wire descriptor it needs from the Arrow schema, whose fields already carry their
     `field_number` metadata. D-BIND-1's managed-codec exception stands; its input becomes the
     Arrow schema rather than a TypeScript-style descriptor. Public surface §5.4's `TypedSchema<T>`
     is superseded by this ruling and is redrawn by BIND-8.
  3. **The model layer depends on `Apache.Arrow` only.** The row type, its schema, `ToArrow`,
     `FromArrow` and the topics must compile without `Eiva.Fletcher`, so a gateway- or WASM-only
     application never pulls in native assets (D-BIND-13). The native pair references
     `Eiva.Fletcher`, the gateway pair `Eiva.Fletcher.GatewayClient`, and each is generated only
     when asked for. *(Amended twice; marked here 2026-10-14, review T-D9. By D-BIND-74: the model
     also references `Eiva.Fletcher.Model`, itself `Apache.Arrow` only, so a gateway- or WASM-only
     app still takes no native asset. By D-BIND-76: the native pair is generated by default and
     `csharp_model_only` leaves it out, as C++'s `schema_only` does; how the gateway pair is asked
     for is BIND-8's.)*
  4. **Who emits what.** BIND-6: the model layer and the native pair. BIND-8: the gateway pair,
     as BIND-T does for TypeScript. 18789 is met by BIND-6, BIND-7 and BIND-8 together.
  5. **Rust follows the same shape** when it gains a row type (round RIR).
  6. **C#'s topic segments follow C++'s**, since both publish on the same native bus: for
     `package eiva.nav;` they are `{"eiva.nav", "Svc", "Method"}`, and the gateway pair's string is
     `eiva.nav/Svc/Method`. TypeScript today emits `'eiva/nav/Svc/Method'`, which the gateway splits
     into four segments (read from the code, not yet reproduced). Which form is canonical is being
     settled separately, and C# follows that answer.
     **Settled 2026-10-02:** reproduced by `TopicPath.DottedPackageAgreesAcrossBackends`, and the
     maintainer ruled C++'s form canonical. The TypeScript backend now emits
     `'eiva.nav/Svc/Method'` (D-BIND-30's exceptions), so this point stands as written.

  **Left to BIND-6 and BIND-8:** the opt-token spelling that selects the pairs; how the model layer
  spells a topic without `Eiva.Fletcher`'s `TopicPath` (the native pair keeps `TopicPath`);
  whether the gateway client takes a `RecordBatch` from the typed pair (which calls
  `Player.ToArrow`) or constrains `T` itself; and the names of the gateway pair's types.

  **Declined:** a second, TypeScript-style `TypedSchema<Player>` beside the Arrow schema (two schema
  descriptions per message to keep in step); binding to protoc's `--csharp_out` class, as proposed
  on 18789 (C++ and TypeScript own their row type, it needs `Google.Protobuf`, and it
  reverses D-BIND-69's reasoning).

- **D-BIND-73 - protoc's C# naming rules, read from its source, are BIND-6's specification; a
  property is suffixed `_` on the class name, protoc's reserved members AND the generated class's
  own members.** *LOCKED BY THE MAINTAINER 2026-10-03,* in one question, at the start of BIND-6.

  **WHY.** D-BIND-69 and D-BIND-70 adopted "protoc's C# rule" and left the cases it might not
  answer to BIND-6. Reading protoc's source (`compiler/csharp/names.cc`, `csharp_helpers.cc`,
  `csharp_enum.cc`, on `main`, 2026-10-03) answers all but one, and that one needed a ruling:
  protoc suffixes a property not only when it equals the class name but also when it equals one of
  ITS OWN generated members, and our class has different members.

  **THE RULES, as protoc implements them:**
  - **Property:** `UnderscoresToPascalCase(field name)`: a letter after `_`, a digit or the start
    is upper-cased, every other character but letters and digits is dropped (`player_id` →
    `PlayerId`, `field2_x` → `Field2X`); a result that starts with a digit, from a name starting
    `_`, gets a leading `_`.
  - **Namespace:** `UnderscoresToCamelCase(package, cap first, keep '.')`, so `eiva.nav_v1` →
    `Eiva.NavV1`, prefixed `Fletcher.Gen.` (D-BIND-69). An empty package gives `Fletcher.Gen`.
    This answers D-BIND-69's open item on package PascalCasing.
  - **Enum member:** `TryRemovePrefix(enum name, value)` (case- and underscore-insensitive; the
    value is kept whole if the prefix does not match or nothing would remain), then
    `ShoutyToPascalCase`, then a leading `_` if the result starts with a digit. A name already used
    in the enum gets `_` appended until it is unique. Aliases (`allow_alias`) are all emitted with
    their numbers; C# allows duplicate values. protoc's `[OriginalName]` attribute is NOT emitted,
    since it comes from `Google.Protobuf`. This answers D-BIND-70's open edge cases.

  **THE RULING.** A property is suffixed `_` when its PascalCase name equals:
  1. the class name (D-BIND-69, protoc's rule);
  2. one of protoc's reserved member names: `Types`, `Descriptor`, `Equals`, `ToString`,
     `GetHashCode`, `WriteTo`, `Clone`, `CalculateSize`, `MergeFrom`, `OnConstruction`, `Parser`;
  3. one of the generated class's own members: `Schema`, `ToArrow`, `FromArrow`, and any member a
     later slice adds, which joins this list in the same commit. *(Extended by D-BIND-84: a MESSAGE
     with one of these names is suffixed too, and a property steps past the class's own name.)*

  Every property is therefore named exactly as protoc's output names it, which helps an
  application that maps between the two, and no field can collide with a member of ours.
  *(D-BIND-89: two fields that PascalCase alike are told apart by a further `_`, in field order,
  where protoc would declare the property twice.)*

  **Declined:** only the names our class actually has (some properties would differ from
  protoc's: a field `clone` stays `Clone` here and is `Clone_` there); protoc's list alone (a field
  named `schema` would not compile).

  **Note, measured 2026-10-08 (BIND-6e-3); the ruling is unchanged.** "Named exactly as protoc's
  output names it" holds for the protoc this was read from (`main`, 2026-10-03), NOT for the protoc
  3.21.12 this repository ships: that version's reserved list predates `Clone`, so its own C# for a
  field `clone` names the property `Clone` and fails to compile against its own `Clone()` (CS0102,
  seen with `integration-tests/protoc-dotnet/proto/model.proto`). Fletcher's `Clone_` is the current
  protoc's name and is correct; do not "align" it with 3.21.12. Other names in the list may differ
  the same way across protoc versions; none has been measured.

- **D-BIND-74 - the lossless `Timestamp` and `Duration` types live in a fourth managed package,
  `Eiva.Fletcher.Model`, that references `Apache.Arrow` alone; amends D-BIND-14' from three
  packages to four.** *LOCKED BY THE MAINTAINER 2026-10-05,* in one question at the start of
  slice 6c, **reopened the same day** and re-ruled after a challenge to the first answer.

  **WHY.** D-BIND-26 makes the temporal types `long`-backed structs that carry the unit, with
  `ToDateTimeOffset()` and `ToTimeSpan()`. D-BIND-72 forbids the model layer a reference to
  `Eiva.Fletcher`, so they cannot live there, and `Eiva.Fletcher.GatewayClient` would drag a
  WebSocket client in for two structs. Generated code lives in the consumer's assembly, so a
  type emitted beside it is defined once PER ASSEMBLY.

  **THE RULING.** `Eiva.Fletcher.Model` holds exactly `Timestamp` and `Duration`, the two
  well-known types that need a C# type of their own (wrappers map to nullable scalars; `Any` and
  `Struct` are refused), in namespace `Eiva.Fletcher.Model`. User-defined messages stay in the
  consumer's assembly. Anything else joining it needs its own ruling (dictionary support is
  deferred to DICT, D-BIND-8). It depends on `Apache.Arrow` only, ships from
  the `dotnet/` component at the shared version, and is published by `cd.dotnet.yml` with the
  others. Generated `.fletcher.cs` files reference it; `GenerateAll` emits no helper. protoc's own
  C# puts its `Timestamp` and `Duration` in the `Google.Protobuf` runtime package, namespace
  `Google.Protobuf.WellKnownTypes` (read in protocolbuffers/protobuf `main`, 2026-10-05);
  D-BIND-72 avoids that package for a different reason.

  **Declined:** emitting the types once per protoc run (the FIRST ruling, same day). Two
  assemblies that both run protoc each define `Timestamp` in the same namespace, and an app that
  references both gets CS0433 on the name; a value from one assembly's message is not the type the
  other's code expects. Emitting per file fails earlier still (CS0101 inside one assembly).

  **Consequences.** D-BIND-14' and D-BIND-28's "three packages" become four (BIND-9's publish
  list, `dotnet/README.md`). `Eiva.Fletcher` MAY reference `Eiva.Fletcher.Model`; the reverse
  never. The package name and namespace were chosen by the assistant and are open to the
  maintainer's rename until BIND-9.

- **D-BIND-75 - the shapes of a generated class's properties: a map is an ORDERED LIST OF PAIRS,
  and the rest follow from D-BIND-26, 69, 72 and 74.** *The map shape LOCKED BY THE MAINTAINER
  2026-10-07,* in one question at the start of slice 6c-1. The other shapes were stated with that
  question and are set out here for review with the commit that implements them.

  **WHY the map needed ruling.** A proto `map<K,V>` is an Arrow map, whose entry order is on the wire,
  and C++'s generated row class holds it as `std::vector<std::pair<K,V>>`. A C# `Dictionary<K,V>`
  enumerates in an order .NET leaves unspecified (insertion order in practice, until a removal) and
  cannot hold a duplicate key, so a C#-versus-C++ wire comparison (6d) could fail for a reason the
  model hides.

  **THE SHAPES** (all in `csharp_backend_type_table`, `CsFieldTypeOf`):
  - **map<K,V>** is `List<KeyValuePair<K,V>>`, never null, empty by default. Order and duplicates round
    trip. A lookup-by-key type of our own was declined: it would put a third thing in
    `Eiva.Fletcher.Model`, which D-BIND-74 limits to `Timestamp` and `Duration`.
  - **repeated T** is `List<T>`, never null, empty by default.
  - **A message field** is the generated class, `?` when the field can be absent. *(Amended by
    D-BIND-83: "can be absent" means `optional`; a plain message field is never null.)*
  - **Timestamp and Duration** are `global::Eiva.Fletcher.Model.Timestamp` / `Duration`, nullable
    exactly where the schema is: a plain field is always present (its Arrow column is non-nullable), an
    `optional` one is `?`. Wrappers stay nullable scalars. Named in full, so a message called
    `Timestamp` in the same file cannot shadow them.
  - **A message declared in another file has no class in this one**, so such a field is a comment
    naming BIND-6e (cross-file) and `ToArrow` / `FromArrow` wait on it. This includes
    `repeated google.protobuf.Timestamp`, which the IR maps as a list of the struct `seconds`/`nanos`,
    not as a list of timestamps: that is how the schema already treats it, and the C# type follows.
    *(Superseded by D-BIND-80 at BIND-6e; marked here 2026-10-14, review T-D9. Such a field is a
    property of the other file's class, named from `global::`, and its column is built through that
    class's public `ToArrow`; `repeated google.protobuf.Timestamp` is a `List` of the class generated
    from `timestamp.proto`, which a consumer generates too. No field is a comment any more.)*

  **A test that could not fail.** `TheModelReferencesNothingOfFletchers` looked for assembly names
  starting `Eiva.Fletcher`, but the assemblies are named after their projects (`Fletcher`,
  `Fletcher.Interop`, `Fletcher.Model`), so it could not have caught a reference to any of them. It is
  now `TheModelReferencesApacheArrowAndFletcherModelAndNothingElseOfFletchers`, looking for the real
  names. It surfaced when `Eiva.Fletcher.Model` was added as a reference and the assertion for it failed.

The next four were ruled on 2026-10-07 at the start of slice 6d, one question each, and answer
what D-BIND-49 and D-BIND-72 left to BIND-6.

- **D-BIND-76 - `--fletcher_opt=csharp` writes the native pair in its own file,
  `<stem>.fletcher.native.cs`, beside the model; `csharp_model_only` opts out. Amends D-BIND-72's
  "only when asked for" to "on by default, with an opt-out".** *LOCKED BY THE MAINTAINER
  2026-10-07, re-ruled the same day.* The first answer was a second token, `csharp_native`, to ask for
  the pair. The maintainer asked why there were two tokens where one was expected; the answer was
  D-BIND-72's Arrow-only model layer, and that the repository already has the other convention: C++'s
  header always carries its pub/sub pair, and `schema_only` opts out. C# now follows that.

  **THE RULING.** `csharp` writes `<stem>.fletcher.cs` (the model, `Apache.Arrow` only) and
  `<stem>.fletcher.native.cs` (the pair, which needs `Eiva.Fletcher`), the second even for a file with
  no pub/sub method, so the output set does not depend on the `.proto`'s contents.
  `csharp,csharp_model_only` writes the model alone, byte-identical to the model `csharp` writes: the
  token for a gateway, WASM or other Arrow-only project, and what `integration-tests/protoc-dotnet` now
  passes. The opt-out is a NEW token, not `schema_only`, because for C++ `schema_only` also drops the
  row classes, which C#'s model keeps. The pair stays in its own file, so the boundary is a file
  boundary either way. BIND-8 decides whether the gateway pair follows the same default.

  **Declined:** `csharp_native` to ask for the pair (the first answer: safest for Arrow-only projects,
  but more to type for the common native case and the reverse of C++'s convention); `csharp` always
  writing both files with no opt-out (an Arrow-only project would have to remove the native file from
  its build by hand); the pair appended to `<stem>.fletcher.cs` (one file would then differ between a
  native and an Arrow-only build).

- **D-BIND-77 - a service method's topic is spelled in the model layer as a static class per method,
  `<Svc>_<Method>Topic`, with `Segments` and `Key`.** *LOCKED BY THE MAINTAINER 2026-10-07.* D-BIND-72
  puts C#'s topic in the model layer, because the native pair and BIND-8's gateway pair both need it,
  and the model layer cannot use `Eiva.Fletcher`'s `TopicPath`. In `<stem>.fletcher.cs`:
  `public static class <Svc>_<Method>Topic { public static IReadOnlyList<string> Segments { get; } =
  new[] { "eiva.nav", "Svc", "Method" }; public const string Key = "eiva.nav/Svc/Method"; }`, the
  segments following C++'s (D-BIND-72 point 6). The native pair's `static TopicPath Topic` is
  `TopicPath.Of` over them. **Declined:** one class per service with a member per method (member names
  derived from method names, and a public array is mutable); constants on the input message's class (a
  message used by two methods would need two names, and the reserved-member list would grow).

- **D-BIND-78 - the generated `Publish(T)` stays per-row; its documentation states the price and
  points at `Publish(IEnumerable<T>)`.** *LOCKED BY THE MAINTAINER 2026-10-07,* settling what D-BIND-49
  left to BIND-6. `Publish(T)` builds a one-row batch, binds it and publishes it before returning, as
  C++'s `Publish(row)` does: when the call returns the row is on the bus and any failure has surfaced
  at the call. The price is D-BIND-49's M2, 3.8 us and 19.6x generated C++ on Windows, about two-thirds
  of it Apache.Arrow's; the batch form is M3, 239 ns per row. **Declined:** accumulating behind the
  caller (asynchronous failure, added latency, a flush every caller must remember, and a divergence
  from C++); a second, opt-in batching publisher (surface for a need nobody has stated).

- **D-BIND-79 - the generated subscriber takes a generated delegate,
  `<Svc>_<Method>Handler(<Msg> row, AttachmentsView attachments)`, not `Action<<Msg>,
  AttachmentsView>`; amends public surface 2.7.** *LOCKED BY THE MAINTAINER 2026-10-07.*
  `AttachmentsView` is a ref struct, and `Action<,>` accepts one only from .NET 9: a net8.0 consumer
  fails with CS9244 (measured 2026-10-07 on SDK 10.0.401; net10.0 compiles), while `Eiva.Fletcher`
  targets both (D-BIND-21). A generated delegate compiles on both, keeps the attachments BORROWED for
  the callback as the runtime's own `RowHandler` does, and still cannot be captured by an `async`
  lambda. A lambda at the call site reads as it would with `Action`. **Declined:** `Action<<Msg>,
  AttachmentsBuilder>` with an owned copy (a copy per row whether read or not); keeping `Action` and
  requiring .NET 9 for the pair (a net8.0 consumer could not use the generated subscriber).

The next one was ruled on 2026-10-07 at the start of slice 6e, cross-file references.

- **D-BIND-80 - a message from another file builds its struct column through that class's public
  `ToArrow`, never its internal `ToArrowColumns`; a message of the same file still lends
  `ToArrowColumns`.** *LOCKED BY THE MAINTAINER 2026-10-07.* Since BIND-6e a field may name a message
  from another file: its class is generated from that file (the consumer generates every file its
  imports reach, as C++ includes the other file's `.fletcher.pb.h`), in that file's namespace,
  `global::Fletcher.Gen.<Pkg>.<Name>` when the package differs. The consumer may compile that class into
  ANOTHER assembly, a contracts project, and from there an `internal` member is absent: a call to
  `ToArrowColumns` fails with CS0117 (measured by mutant in `integration-tests/protoc-dotnet`, whose
  `ProtocDotnetShared` assembly holds `shared.proto` and `timestamp.proto`). So the embedding class
  calls `<Other>.ToArrow(rows).Arrays` for the struct's children: the same arrays, built under the
  other class's own `Schema`, plus one `RecordBatch` object per such column per call. A class of the
  same file keeps the internal call, so every single-file output is byte-identical to before.
  `repeated google.protobuf.Timestamp` is the common case: the IR maps it as timestamp.proto's struct
  (D-BIND-75), so its element is `global::Fletcher.Gen.Google.Protobuf.Timestamp`, generated from
  timestamp.proto. **Declined:** making `ToArrowColumns` public with `[EditorBrowsable(Never)]` (one path,
  but a public member on every generated class that nobody should call); keeping it internal and
  requiring every file in one assembly (a contracts assembly is the usual way to share messages, and
  it would fail to compile).

- **D-BIND-81 - `Eiva.Fletcher`, `Eiva.Fletcher.Interop` and `Eiva.Fletcher.Model` declare
  `IsAotCompatible`; a NativeAOT consumer is published and RUN in `ci.dotnet`'s Linux job; and the AOT
  toolchain packages are pinned so one set of lock files holds on every SDK of the band.** *LOCKED BY
  THE MAINTAINER 2026-10-08, in three questions at slice 6e.* BIND-6's acceptance asks that "a NativeAOT
  publish of a consumer app succeeds with zero trim warnings". Measured first: a console app compiling
  the plugin's goldens (the model, the native pair, and timestamp.proto's class) against
  `Eiva.Fletcher` published with NativeAOT with ONE warning, IL3000, from `NativeLoader`'s use of
  `Assembly.Location`, which is empty in an AOT or single-file app; the generated code and Apache.Arrow
  23 raised none. `NativeLoader` now probes `AppContext.BaseDirectory`, the same directory in an
  ordinary build. *(Review G-D1, 2026-10-14: it probes beside the assembly first when the assembly
  has a file, then `AppContext.BaseDirectory`; IL3000 is suppressed on that one guarded read.)* Then:
  - **The claim.** The three libraries set `IsAotCompatible`, which turns the trim and AOT analysers on
    in every build (the IL3000 line put back fails the Interop build itself, 4 errors) and stamps each
    assembly `IsTrimmable`. `Eiva.Fletcher.GatewayClient` was not asked about and does not yet carry it.
  - **The evidence.** `dotnet/tests/Fletcher.AotSmoke`, in `Fletcher.slnx`, is that consumer: it
    publishes and receives a `Reading` and a cross-file `Marks` over `inprocess` and exits non-zero
    unless exactly the rows sent come back. `ci.dotnet`'s Linux job publishes it for net8.0 and net10.0
    (warnings are errors, so any IL warning fails the publish; gcc links, the image has no clang) and
    runs each binary against the shim already built there. **Declined:** a lane of its own (a second
    shim build for no new evidence); Windows too (twice the added time for a toolchain difference
    nothing so far suggests).
  - **The pin.** `IsAotCompatible` and `PublishAot` add implicit references to
    `Microsoft.NET.ILLink.Tasks` (which carries the analysers since .NET 8) and the ILCompiler packages,
    at versions the SDK bundles. CI's Linux image runs SDK 10.0.400 (packs 10.0.11 and 8.0.30) and its
    Windows runner rolls forward to 10.0.401 (10.0.12), so a lock file written on one failed
    `restore --locked-mode` on the other (NU1004, measured). `dotnet/Directory.Build.targets` fixes the
    pack versions at SDK 10.0.400's for every SDK, raised deliberately with `global.json`; 10.0.401
    restores locked against them, and without the pin fails with 8 x NU1004. **Declined:** `global.json`
    `rollForward: disable` (every machine on exactly one SDK patch); dropping the claim and keeping the
    smoke app out of the solution with no lock file (the guarantee would then be one Linux step).
  *(D-BIND-88: a `win-x64` AotSmoke publish and run, and a run over `fastdds`, are BIND-9's.)*
  The NativeAOT static-linking caveat for the LGPL (N-8) is unchanged: this publish loads the shim as a
  shared library beside the binary.

- **D-BIND-82 - with `csharp`, a file whose protoc C# namespace falls inside `Fletcher.Gen` is
  refused.** *LOCKED BY THE MAINTAINER 2026-10-08, at slice 6e.* D-BIND-69 keeps Fletcher's C# apart
  from protoc's by owning `Fletcher.Gen.<Package>` and ignoring `csharp_namespace`; measured in
  `integration-tests/protoc-dotnet/dotnet/ProtocDotnetBeside`, where both generators write into one
  project with no CS0101, and where honouring `csharp_namespace` or dropping the `Fletcher.Gen.` prefix
  each fails the build with CS0101. That separation holds only while protoc stays OUT of
  `Fletcher.Gen`, and a `.proto` can put it there: `option csharp_namespace = "Fletcher.Gen.Nav"`, or a
  package `fletcher.gen.nav` with no option (protoc's default namespace is the PascalCased package).
  So the plugin computes protoc's namespace as protoc does (`GetFileNamespace`: the option, else
  `UnderscoresToCamelCase(package)`), and when it is `Fletcher.Gen` or below it fails the file before
  any output is written, naming the file, the namespace and where it came from. A namespace that only
  shares the TEXT prefix (`Fletcher.Generated`) is accepted. Without `csharp` nothing changes.
  **Declined:** a warning on stderr (protoc's output is easy to miss, and the failure would surface
  later as CS0101); documenting the reservation only.
  *(Extended by D-BIND-91: a protoc TYPE whose full name is `Fletcher` or `Fletcher.Gen` is refused
  too.)*

The next ones were ruled on 2026-10-08, from the BIND-6 review (`plans/reviews/BIND-6-codereview.md`,
questions Q1 to Q10), and are numbered as their fixes land.

- **D-BIND-83 - a plain message field is never null: the property defaults to `new()` and is always
  written present, as C++ does; only an `optional` message field is `T?`. Amends D-BIND-75.**
  *LOCKED BY THE MAINTAINER 2026-10-08 (review Q1), fixing review blocker B1.* The schema C++
  generates makes a plain message field's struct column non-nullable, and C++'s row class writes it as
  `value_or(Cls())`, never a null; its decoder reads such a field without consulting the null bit.
  D-BIND-75 made the C# property `T?` and wrote `null` as a cleared validity bit, which the codec put
  on the wire as a null with no payload: reproduced 2026-10-08, C#'s bytes for a `CompositeCoverage`
  with `Scalars = null` made the C++ generated class throw `PositionalReader: buffer underrun`, while
  `Scalars = new()` gave C++'s own bytes. So now: a plain message field's property is non-null with
  `= new()`, its column carries no validity bitmap, `FromArrow` reads it unconditionally as C++'s
  decoder does, and a null forced past the annotation makes `ToArrow` throw `ArgumentException`
  naming the message, the field and the row before anything is published (review Q10a's rule for
  this case). An `optional` message field keeps D-BIND-75's shape. The proof is a C++-written golden,
  `coverage.CompositeCoverage.defaulted.v1.bin`, that C# must write byte for byte and read back.
  **Declined:** keeping `T?` and writing a default only on the wire (the annotation would advertise a
  null that can never arrive, and G-5 would stay unmet); a nullable column for C# only (the schema is
  C++'s by construction).

- **D-BIND-84 - a message named after one of the generated class's own members is suffixed `_`
  (`message Schema` is class `Schema_`); a property steps past the class's name in the same way.
  Extends D-BIND-73.** *LOCKED BY THE MAINTAINER 2026-10-08 (review Q2), fixing review blocker B3.*
  The generated class has its own static members: `Schema`, `ToArrow`, `FromArrow` and
  `ToArrowColumns`. D-BIND-73 keeps a PROPERTY from those names; nothing kept the CLASS from them, so
  `message Schema` declared `class Schema { static Schema Schema }`, which is CS0542 (reproduced
  2026-10-08). Now a message whose flat name is one of them is suffixed `_` everywhere it is named, as
  protoc suffixes a clash; the compiled name carries the `_`, so for these four names alone the C# type
  differs from the message name. An enum has no such members and keeps its name, and a nested
  `Outer.Schema` is `Outer_Schema`, which cannot clash. One list, `OwnMemberNames()`, feeds both rules,
  so a member a later slice adds is reserved for properties and types at once. Found while fixing it: a
  field `schema` of `message Schema` becomes property `Schema_` under D-BIND-73 and so meets the
  class's new name; the property now steps past the class name as well, `Schema__`. **Declined:**
  refusing such a file with an error, as D-BIND-82 does (a consumer with an existing `message Schema`
  would have to rename it for C++, TypeScript and protobuf users too).

- **D-BIND-85 - a map whose value is a flatten wrapper is UNSUPPORTED in the shared IR, with a
  reason, like a oneof or a map of `Any`.** *LOCKED BY THE MAINTAINER 2026-10-08 (review Q3), fixing
  review blocker B4.* A flatten wrapper (`option (fletcher.flatten) = true` and exactly one field) is
  always inlined where it is used, so no backend generates a class for it. `BuildMapNode` nevertheless
  built such a map value as a STRUCT node naming the wrapper, so every backend emitted code that
  names a class that does not exist: C# `W.ToArrowColumns`/`W.FromArrow` (CS0117), C++ `'W' was not
  declared` and `'WSchema' was not declared` (both measured 2026-10-08). The shape never compiled in
  any backend, so no working consumer depended on it. `BuildMapNode` now returns UNSUPPORTED ("map
  value '<W>' is a flatten wrapper, which has no Arrow mapping as a map value"). The consequence is
  the one every UNSUPPORTED node has: `ValidateNoUnsupportedIr` refuses the file before any backend
  writes anything, naming the field and the reason, for C++, TypeScript, Rust and C# alike.
  *(Correction 2026-10-14: the review's ruling table said such a field would be "dropped
  consistently". It is refused, as a oneof is; the ruling is the same, the stated consequence was
  wrong.)* A flatten message with more than one field is not a wrapper, is ignored as flatten, and
  stays a working struct map value. **Declined:** mapping it as `map<K, list<Leaf>>` (a new feature:
  every backend, the wire spec and the goldens; it may be taken up separately).

- **D-BIND-86 - G-5 ("nullability annotations match the Arrow schema's") covers FIELDS, not
  children.** *LOCKED BY THE MAINTAINER 2026-10-08 (review Q4); the exception below ruled
  2026-10-14.* Each property of a generated class is nullable exactly when its top-level field in the
  class's own `Schema` is. List items, map keys and map values follow the C++ row class's storage
  instead, which cannot hold a null (`std::vector<int32_t>`), so they are non-null in C# although
  nanoarrow's default marks a child nullable in the schema. One field is a named exception: an
  `optional` flatten-wrapper list (`optional StructListWrapper x`) has a nullable field, but C++
  stores it as `std::vector<Leaf>` (measured 2026-10-14), so C# keeps a non-null `List<Leaf>` and a
  null reads as empty. That is the review's accepted debt G-Q4; C# follows C++ rather than the
  schema. `Fletcher.Tests` `GeneratedNullabilityTests` pins both halves by reflection
  (`NullabilityInfoContext`) over every generated row class compiled into it, found by reflection so
  a later golden is covered unlisted; the exception is listed by name and must still be met.
  **Declined:** `List<Leaf>?` for that one shape (C# would then differ from C++'s row class, and null
  versus empty would round-trip in C# only); leaving the test red until every backend can hold the
  null.

- **D-BIND-87 - C# and C++ find a file's cross-file types through ONE walk,
  `CollectCrossFileMessages`, from which `CollectCrossFileIncludes` is derived.** *LOCKED BY THE
  MAINTAINER 2026-10-08 (review Q5, against the author's recommendation to keep the 6e-1
  deviation); the field-flatten consequence ruled 2026-10-14.* 6e-1 left `CollectCrossFileIncludes`
  (the D-RBA-10 single source of truth for C++ includes) uncalled, and C# decided "this message is
  from another file" (D-BIND-80: build its column through the class's public `ToArrow`) by comparing
  descriptors itself. Now the walk is `CollectCrossFileMessages(file)`, the set of messages from
  other files that `file`'s fields name; C++ maps each to its header (`CollectCrossFileIncludes`),
  and the C# visitor computes the set once per file and decides by membership. Namespace SPELLING
  (`CsTypeRef`) stays read from each type's own file; a set of dependencies cannot give a namespace.
  **Found while doing it (measured 2026-10-14):** the C++ walk did not look inside a field-level
  flatten, which inlines the flattened message's own fields into the host. For `host.proto`
  flattening `Spot` (spot.proto), whose field is a `Leaf` (parts.proto), the walk found `Spot` but
  not `Leaf`; C# through it would have called `Leaf.ToArrowColumns`, internal and so absent in
  another assembly (CS0117), where it had correctly called `Leaf.ToArrow`. C++ compiled only
  because `spot.fletcher.pb.h` includes `parts.fletcher.pb.h`. The walk now descends a field-level
  flatten exactly as `GatherFieldsImpl` and the enum-include walk do (with a guard against a
  self-flattening message). **Consequence for C++, ruled 2026-10-14:** for that shape the header
  now includes every header for the types it names directly, one more `#include` than before; the
  Q5 ruling had said C++ output stays byte-identical, and it does for every other input (no golden
  or corpus moved). **Declined:** a switch on the walk keeping C++ byte-identical for that shape
  too (the two backends would get different answers from the one function, which is what the
  ruling removes).

- **D-BIND-88 - the two NativeAOT notes 6e-2 left are BIND-9's, as acceptance there, and do not
  block BIND-6.** *LOCKED BY THE MAINTAINER 2026-10-08 (review Q6), as the tests reviewer
  recommended.* D-BIND-81 publishes and runs `Fletcher.AotSmoke` on Linux over `inprocess` only. Not
  yet shown: (1) a `win-x64` NativeAOT publish (MSVC links) and run against the Windows shim, which
  belongs with BIND-9's RID matrix; (2) an AOT-compiled delivery entering managed code on a Fast DDS
  thread, which has run only under the JIT, so a run over `fastdds`. Neither is a known defect: the
  AOT analysers cover the code statically, and nothing is known to break on Windows. Both are now
  bullets in BIND-9's acceptance, so BIND-9 cannot close without them. **Declined:** doing them in
  BIND-6 (the Windows leg needs the RID matrix BIND-9 builds; the transport run is test work for a
  shipped package).

- **D-BIND-89 - (a) two fields that PascalCase to one C# property are told apart by `_`, in field
  order; (b) a field-level flatten that inlines a name its host already has is refused for every
  backend.** *LOCKED BY THE MAINTAINER 2026-10-08 (review Q7); answers what D-BIND-69 left open.*
  Measured 2026-10-08: protoc 3.21.12 rejects such a pair in proto3 itself (the JSON names clash,
  case-insensitively), but accepts proto2 `foo_bar` + `fooBar`, for which C++ is fine and C#
  declared the property twice (CS0102); and a flatten that inlines a field named like a host field
  gave two schema columns of one name and broke C++ too (`set_x` declared twice).
  (a) `CsUniquePropertyName` names each property by D-BIND-73/84, then appends `_` while the name is
  already taken by an earlier property of the class or equals the class's name: proto2 `foo_bar`,
  `fooBar`, `FooBar` are `FooBar`, `FooBar_`, `FooBar__`. It covers a host field and an inlined one
  that PascalCase alike too (proto3 accepts that pair, the fields being in different messages).
  Column names and C++ are unchanged. (b) `ValidateFlattenedNamesUnique` runs beside
  `ValidateNoUnsupportedIr`, before any output, over every generated message's flattened field
  list, and refuses the first column name that occurs twice: "fields '<A>' and '<B>' (inlined by the
  field-level flatten '<F>') both become column 'x' of '<Host>'; rename one of them". Tests: golden
  `csharp_proto2.proto` compiled and run in `Fletcher.Tests`; `CsNames` and `CsVisitor` cases; and
  `GenErrors.FlattenedNameClashFailsBuild` (real protoc and plugin) in protoc-coverage. Both halves
  failed against the plugin built before the fix (three properties `FooBar`; the clash generated).
  **Declined:** for (a), refusing the file (protoc accepts it, and C++ works); for (b), renaming one
  column (the schema is C++'s, and a silent rename would surprise every consumer).

- **D-BIND-90 - a pub/sub method whose input is a flatten wrapper follows C++: no topic class and
  no pair, and C++'s skip line and reason.** *LOCKED BY THE MAINTAINER 2026-10-08 (review Q8).* A
  flatten wrapper has no class in C++, so C++ leaves it out of the messages it generates and skips the
  method: `// Skipped: Feed.Push — input message 'Batch' has no generated Arrow mapping in this
  file`. C# built the same set from `OrderedMessages` but kept wrappers in it, so it emitted
  `Feed_PushTopic` and, in the native file, a skip line of its own ("Batch has no ToArrow /
  FromArrow: it is a flatten wrapper."), measured 2026-10-14. `PubSubMethods` now leaves wrappers out
  as C++ does, so `ValidateServiceMethod` gives C++'s reason and both C# files write C++'s line; the
  native file's own wrapper branch is gone. TypeScript still emits the topic constant (measured); its
  divergence is recorded as debt in BIND-T. Test:
  `CsVisitorCrossFile.APairCarriesAnotherFilesMessageAndAFlattenWrapperIsSkippedAsInCpp` takes the
  line from the C++ header generated for the same file and requires it in both C# files.
  **Declined:** keeping the topic class (C# would name a topic no C++ publisher ever registers).

- **D-BIND-91 - with `csharp`, a file is also refused when protoc's C# would declare a TYPE whose
  full name is `Fletcher` or `Fletcher.Gen`. Extends D-BIND-82.** *LOCKED BY THE MAINTAINER
  2026-10-08 (review Q9).* D-BIND-82 refuses protoc's namespace inside `Fletcher.Gen`. The other
  direction: protoc declares each top-level message and enum as `<its namespace>.<Name>`, and when
  that is exactly one of our namespaces the type takes its name. Measured 2026-10-14, protoc's
  `--csharp_out` and our `csharp` compiled into one assembly: `package fletcher; message Gen`, and
  `option csharp_namespace = "Fletcher"; message Gen`, give CS0101 "The namespace 'Fletcher' already
  contains a definition for 'Gen'"; no package with `message Fletcher` gives CS0101 in the global
  namespace; the control `package nav; message Gen` compiles. `CsProtocNamespaceConflict` now
  checks each top-level message and enum of the file against both names, case-sensitively as C# is,
  and returns a reason naming the type, the namespace and where it came from; the generator already
  calls it before any output. Only top-level types: a nested one is `<Outer>.Types.<Name>` in
  protoc's C#, inside its outer class. **Declined:** refusing any type named `Fletcher` or `Gen`
  wherever it lives (`Eiva.Fletcher` collides with nothing: our code names its types from
  `global::`).

- **D-BIND-92 - the generated `ToArrow` refuses a null the caller forced past a non-nullable
  annotation, with an `ArgumentException` naming the class, the property and the row.** *LOCKED BY
  THE MAINTAINER 2026-10-08 (review Q10a); its scope widened by the maintainer 2026-10-14.* The ruling
  named a string or bytes field, a map key and a list element. Reading the generated code found the
  other places the annotations promise non-null (D-BIND-86) and that misbehaved the same way: a map
  value, a null collection property, an inner list of a nested list, and a null row; ruled
  2026-10-14 to refuse them all alike. Before: a null string was written as a null in a non-nullable
  column (or key, or element), a null `byte[]` silently as empty bytes, and a null message element,
  collection or row failed with a NullReferenceException inside the generated code. C++'s row class
  cannot hold any of these (`std::string`, `std::vector`), so C++ never throws, and its decoder would
  read a value that is not there. `ToArrowColumns` now opens with one pass over the rows that checks
  each such column; the public `ToArrow` refuses a null row; a plain message field keeps its own
  check (D-BIND-83). `row {i}` counts the rows the class converts, so for a nested message it is that
  message's place in the column being built. **Separately parked to BIND-9 (acceptance there), BIND-10
  documents it:** a RecordBatch built by hand at the Arrow tier is encoded as given in both
  languages, because no codec path checks a column's nullability. **Declined:** coercing a null to
  `""` or empty bytes (silently writes a value the caller never gave).

- **D-BIND-93 - a non-nullable Timestamp or Duration property starts at zero in its column's unit
  (`new Timestamp(0, TimeUnit.Nanosecond)`), so a fresh row and the same row read back compare
  equal; `Eiva.Fletcher.Model` is unchanged.** *LOCKED BY THE MAINTAINER 2026-10-08 (review Q10b).*
  `default(Timestamp)` is `(0, Second)`, and FromArrow returns the column's unit, `(0, Nanosecond)`:
  the same instant, but a record struct that compares unequal, so a fresh row did not survive a round
  trip equal. The unit (and a time zone, when one is set) comes from the IR's logical type, which is
  what the schema writes (nanoseconds, no zone, today). `Fletcher.Tests` checks it for every
  generated row class by reflection: a fresh instance read back has equal temporal values.
  **Declined:** changing the Model's default (a `record struct` cannot have one other than all-zero
  fields).

The next ones were ruled on 2026-10-14, at the start of BIND-7 (the C# Arrow view and RecordBatch
accessor), after a read-only map of the C++ and Rust accessors and the capstone harness.

- **D-BIND-94 - the C# accessor's factories are `TryMake(RecordBatch, out <Msg>Accessor?, out
  string? error)` and `TryMake(StructArray, out <Msg>Accessor?, out string? error)`: never thrown on
  bad input, and the reason returned.** *LOCKED BY THE MAINTAINER 2026-10-14 (BIND-7 Q1).* The plans
  called `TryMake` "the documented deviation from C++ `Make`-throws". That was wrong: C++'s `Make`
  returns `arrow::Result` with an `Invalid` status and its message and never throws (D-RBA-4), and
  Rust's `try_new` / `from_struct` return `Result<_, ArrowError>`. So the C# form keeps what both
  carry, the reason, in the shape .NET uses for a fallible factory. A null argument is a refusal like
  any other. **Declined:** `TryMake(batch, out accessor)` alone, as first drawn (the reason is lost);
  a throwing `Make` beside it (two ways to do one thing).

- **D-BIND-95 - the C# accessor's metadata mirrors C++ and Rust: `SchemaMetadata`,
  `FieldMetadata(int)` and `FieldMetadata(string)`, each an `IReadOnlyDictionary<string, string>` that
  is empty, never null, when absent; `Metadata(key)` stays as a shortcut into the schema metadata,
  null when the key is absent. Amends Q18.** *LOCKED BY THE MAINTAINER 2026-10-14 (BIND-7 Q2).* Q18
  ruled `Metadata(key)` "as the C++ and Rust accessors do". Read from the emitters, they do not: C++
  has `schema_metadata()`, `field_metadata(int)` and `field_metadata(name)` (nullptr when absent);
  Rust `schema_metadata()` and `field_metadata(i)` (an empty map when absent). The capstone compares
  the whole schema map and a positional array of field maps, which `Metadata(key)` alone could not
  produce. Empty-not-null follows Rust, so a consumer never null-checks a map. **Declined:** dropping
  `Metadata(key)` (Q18's convenience costs nothing); positional field access only, as Rust.

- **D-BIND-96 - a message the C# accessor cannot support still gets its `<Msg>Accessor`, as in C++:
  the unsupported field has a comment instead of a getter, and `TryMake` returns false with the
  reason.** *LOCKED BY THE MAINTAINER 2026-10-14 (BIND-7 Q3).* "Cannot support" means what it means
  for C++'s accessor: a list of lists deeper than the 2/3 cap (D-BIND-9), and a message whose
  conversion reaches one. C++ generates the class with a `Make` that always fails; Rust generates no
  struct at all, transitively, with a comment saying why (D-RBA-8). C# follows C++, as BIND-6 did
  throughout: code that names the class still compiles, and the refusal names the field.
  **Declined:** Rust's form (the class disappears, and with it every class that reaches it).
  *(Amended by D-BIND-104, 2026-10-15: C# also reads a list of lists with a scalar leaf, which C++
  refuses, so "cannot support" is C++'s set less that case. And since BIND-7 review T-D1 a message that
  reaches one which refuses refuses up front, naming the inner reason, with a comment in its view.)*

- **D-BIND-97 - `<Msg>View` is a `readonly struct` of (the accessor, a row), in
  `<stem>.fletcher.arrow.cs`, and the accessor's `RowView` IS that view; its getters read the
  accessor's cast-once columns in place, and `ToRow()` materialises the generated row class.
  Replaces the planned view over `(StructArray, int)`.** *LOCKED BY THE MAINTAINER 2026-10-14 (BIND-7
  Q4), after a measurement the maintainer asked for ("what about performance and zero copy?").*
  Measured on Apache.Arrow 23.0.0, one million calls each: reading `StructArray.Fields` allocates
  64 bytes on EVERY property read although it returns the same cached list, so a view over
  `(StructArray, int)` whose getters go through `Fields[i]` allocates 64 bytes per getter call (an
  `int`, a UTF-8 span, a byte span, a list), while the same reads from typed arrays cast once allocate
  0. So the efficient view is the accessor's columns plus a row, which is what the accessor's
  `RowView` already is; one type serves both. A view of a bare `StructArray` goes through `TryMake`
  once. Nothing is copied except where .NET requires it: a `string` getter decodes UTF-8 into UTF-16
  (40 bytes for a short string, measured), so a string field also gets a getter returning its UTF-8
  bytes as a `ReadOnlySpan<byte>`, which copies nothing; `ToRow()` builds the row class by design.
  C++'s view, for comparison, materialises an Arrow `Scalar` per column per row and has no `ToRow()`
  (its inverse is the free function `ToArrowRow`); it is emitted on every run, where C#'s comes with
  `csharp_accessor` alone. **Found while measuring:** BIND-6's generated `FromArrow(StructArray, int)`
  reads `array.Fields[n]` per column, so it allocates 64 bytes per column per row; recorded as debt.
  **Declined:** the view over `(StructArray, int)` with `Fields` fetched once at construction (64
  bytes per view, a cast per getter, and a second type beside `RowView`); no view at all.
  *(Amended by D-BIND-98, 2026-10-14: `ToRow()` moves from the view to the accessor as
  `ToRow(row)`, so the view has no members of its own beside the field properties.)*

- **D-BIND-98 - the C# accessor and view split their surfaces so that no field name can collide
  with a generated member: `<Msg>Accessor` has no per-field getters, only `TryMake`, `Length`,
  `IsNull(row)`, `At(row)` (returning `<Msg>View`), `ToRow(row)` and D-BIND-95's metadata API;
  `<Msg>View` has only the field properties, named exactly as on the model class (D-BIND-73, 84,
  89). Amends D-BIND-97.** *LOCKED BY THE MAINTAINER 2026-10-14 (BIND-7a), after asking why the rule
  was needed and why the split over a suffix.* A field getter beside the accessor's own members
  collides with them: `message Track { int32 length = 1; Point at = 2; }` would give a `Length`
  getter beside the row count `Length` and an `At` getter beside `At(row)`, CS0102 or a silent change
  of meaning, and a valid `.proto` must never break generation. With the split, the view's only
  members are the fields, so it reuses the model class's names and their rule unchanged: one name
  per field across the model class and the view. The accessor can gain members later without
  renaming any user's getter, which under a suffix rule would be a breaking change on upgrade
  (`Slice` added, a field `slice` becomes `Slice_`). `ToRow()` leaves the view for that reason (a
  field `to_row`). D-BIND-96's "a comment instead of a getter" applies to the view's property. The
  string field's UTF-8 span getter (D-BIND-97, slice 7c) has a field-derived name and goes through
  D-BIND-89's taken set like any property. The cost: unlike C++ (`acc.speed(row)` beside its
  `RowView`), a column loop reads `acc.At(row).Speed`; the view is a two-word `readonly struct` the
  JIT is expected to inline, which 7a measures. C++'s own accessor puts `num_rows()` and
  `is_null(row)` beside its field getters, and `recordbatch_accessor_emitter.cpp` showed no guard
  against a field of either name; whether one breaks it was not checked (not BIND's to fix).
  **Also ruled with it, 2026-10-15, after a full C++/C# comparison:** (1) the row index is `int`
  (`Length`, `IsNull(row)`, `At(row)`, `ToRow(row)`), as every Apache.Arrow array indexes with
  `int`, where C++ uses `int64_t`; (2) the view's property TYPES are the model class's too, not only
  its names: a C# `enum` (C++ gives a raw `int32` and `x_typed()`), D-BIND-26's lossless temporal
  structs, `T?` where C++ returns `std::optional`, and a nullable struct field as `<Inner>View?`,
  null on a null row, as C++'s `optional<RowView>`. Collection getters stay for 7b.
  **Declined:** C++-like per-field getters on the accessor with a second reserved list and a `_`
  suffix on clashes (two names for one field, and every new accessor member renames somebody's
  getter).

- **D-BIND-99 - `csharp_accessor` requires `csharp` (or `csharp,csharp_model_only`): given alone,
  generation fails with a message saying so, before any output is written.** *LOCKED BY THE
  MAINTAINER 2026-10-15 (BIND-7a).* The view's properties have the model class's types (D-BIND-98:
  its enums, `Eiva.Fletcher.Model.Timestamp` and `Duration`) and `ToRow(row)` returns the model
  class, so the two accessor files cannot compile without `<stem>.fletcher.cs`. Failing names the
  missing token at generation time instead of leaving a compile error in the user's build.
  **Declined:** the token implying the model (one token writing three files); writing the two files
  alone, as C++'s `accessor` does (C++'s accessor needs no row class, C#'s does).

- **D-BIND-100 - a message whose name equals another message's generated `<Msg>View` or
  `<Msg>Accessor` in the same C# namespace refuses generation under `csharp_accessor`, with an
  error naming both messages.** *LOCKED BY THE MAINTAINER 2026-10-15 (BIND-7a), after asking how two
  different messages could clash.* They are different messages; their GENERATED types clash:
  `message Track` gets the view struct `TrackView`, and `message TrackView` gets the model class
  `TrackView`, both in `Fletcher.Gen.<Package>`, which is CS0101. BIND-6 never derives a type name
  from another, so this is new with the view and accessor. Refused like D-BIND-91's namespace
  clashes: rare, and never silently renamed. **Declined:** suffixing the generated type
  (`TrackView_`), a name the user cannot predict without the rule. **Found while implementing
  (2026-10-15), refused the same way:** a field named after its own message's view
  (`Track.track_view` would be the property `TrackView` inside the struct `TrackView`, CS0542).
  Only the file and its imports are visible to the plugin, so a clash with an unimported file of
  the same package is left to the compiler. *(Amended 2026-10-15, BIND-7 review B2 and P-D1: the generated names
  of an IMPORTED file of the namespace are checked too, assuming it is generated with the same
  tokens; and a field the view has no property for, beyond the depth cap, is not refused for its
  name.)*

- **D-BIND-101 - a collection property of the C# view returns a borrowed window over the cast
  column, not the model's `List`: `ListView<T>` for a list, `MapView<TKey, TValue>` for a map, a
  list of lists as a `ListView` of `ListView`s; and a null element reads as C++ reads it.**
  *LOCKED BY THE MAINTAINER 2026-10-15 (BIND-7b). Narrows D-BIND-98's "the view's types are the
  model's", which left collection getters to 7b.* Returning the model's `List<T>` would allocate a
  list and copy every element on every read, which BIND-7's "no per-cell materialisation" forbids
  and C++ never does: its `ScalarSpan`, `StructSpan`, `ScalarMapSpan`, `StructMapSpan` and
  `NestedStructSpan` are borrowed windows. The C# windows are readonly structs with `Count`, an
  indexer, `IsNull(j)` (a map: `Key(j)`, `Value(j)`, `ValueIsNull(j)`), a `foreach` that does not
  allocate, and `ToList()`, which builds the model's `List<T>` for a scalar list and
  `List<KeyValuePair<TKey, TValue>>` for a scalar map on demand; a list of structs gives views,
  the model's rows coming from `ToRow` (7c). The property keeps the model's NAME. The window and
  element types live once in `Eiva.Fletcher.Model`, beside `Timestamp`, for the reason that
  package exists: a type generated into every consumer assembly would collide (CS0433).
  **Null elements, as C++:** Arrow list items and map values are nullable although no proto
  producer writes a null element. A scalar element reads as its value, its proto default when
  null, with `IsNull(j)` beside it; an inner list of a list of lists likewise (an empty window,
  `IsNull(j)` true); a struct element or a message map value reads as `<Msg>View?`, null on a null
  element, as C++'s `optional<RowView>`. Nothing a C++ or Rust producer may write is refused.
  **Declined:** the model's types (a copy per read); refusing a batch with a null element in
  `TryMake` (a legal batch from another producer would be refused).

- **D-BIND-102 - `accessor.ToRow(row)` is the model's `FromArrow` over the accessor's columns; a
  bytes field's view gets a zero-copy span getter as a string field does; and the model's
  `FromArrow` reads `StructArray.Fields` once per call.** *LOCKED BY THE MAINTAINER 2026-10-15
  (BIND-7c), after asking for the two `ToRow` designs' pros and cons.* (1) `TryMake` keeps the
  columns as one `StructArray` (the source itself, or a wrapper around a batch's columns, which
  copies nothing) and `ToRow(row)` returns `<Msg>.FromArrow(columns, row)`: one read path for a
  model row, so a row from `ToRow`, from `FromArrow` and from the generated subscriber are the same
  by construction (defaults, temporal recounting, null elements, G-Q4). It re-casts each column per
  call and reads `Fields` once per nested struct VALUE (64 bytes each: per element of a list of
  structs and per message map value, as BIND-7 review P-D2 corrected "per level"), small next
  to the row and lists it allocates by design; the zero-copy path is the view. Like `FromArrow`,
  it does not consult a row's own validity: a null row of a StructArray source reads its children's values,
  and `IsNull` says so. (2) D-BIND-97 gave a string field a `ReadOnlySpan<byte>` getter of its
  UTF-8 bytes, `<Property>Utf8`; a bytes field, whose `byte[]` property copies on every read, gets
  `<Property>Span` for the same reason. Both are named after every model property, by D-BIND-89's
  taken set (a field `name_utf8` keeps `NameUtf8` and the getter becomes `NameUtf8_`), and never
  the view's own name. A null value reads as an empty span: Apache.Arrow's `GetBytes` returns one even where the null's offsets span bytes (pinned by a test), so the getter needs no guard. (3) BIND-6's known debt is paid where
  `ToRow` now depends on it: `FromArrow` reads `array.Fields` once, not once per column (Apache.Arrow
  23 allocates 64 bytes on every read of it). **Declined:** `ToRow` generated from the accessor's
  cast columns (faster, but a second read path that must agree with `FromArrow` shape by shape);
  a span getter for strings only.

The next ones were ruled on 2026-10-15, on the questions of BIND-7's review
(`plans/reviews/BIND-7-codereview.md`, Q1 to Q5).

- **D-BIND-103 - an accessor BORROWS its source: it, its views, its windows and its span getters are
  valid only while the source batch or array is not disposed, and say so.** *LOCKED BY THE
  MAINTAINER 2026-10-15 (BIND-7 review Q1).* The accessor keeps the source's arrays, whose buffers are
  native memory that `RecordBatch.Dispose` frees, so a read after it reads freed memory. C++'s accessor
  owns its buffers through `shared_ptr` (D-RBA-7); .NET has no equivalent short of making the accessor
  own the batch. BIND-7's acceptance line "cached columns own their buffers" is C++'s, recorded as
  such. The rule is written on the generated accessor and view, on `ListView`, `MapView` and the span
  getters, and in the public surface. **Declined:** an accessor that owns its source and is
  `IDisposable` (every user disposes it, and the batch's own `Dispose` becomes ambiguous).

- **D-BIND-104 - C# reads a list of lists with a scalar leaf, which C++'s accessor refuses: a
  superset, kept. Amends D-BIND-96.** *LOCKED BY THE MAINTAINER 2026-10-15 (BIND-7 review Q2).* The
  depth cap is C++'s (3, Q6); the leaf set is wider, because the windows compose and nothing shared
  constrains it (the capstone has no scalar-leaf nested list). D-BIND-96's "cannot support" now means
  C++'s cases except this one. **Declined:** refusing it, as C++ does (GIR-10).

- **D-BIND-105 - a struct column, element or map value from another file is read through that file's
  generated accessor, so that file must be generated with `csharp_accessor` too; documented, and
  compiled in the protoc-dotnet lane.** *LOCKED BY THE MAINTAINER 2026-10-15 (BIND-7 review Q3).* It
  includes `repeated google.protobuf.Timestamp` and a map to it, which the IR keeps as a message. The
  plugin cannot see another run's parameters, so it cannot refuse at generation as D-BIND-99 does; as
  for BIND-6e's model, every file a file imports is generated with the same tokens. The lane generates
  `shared.proto` and `timestamp.proto` with the token and reads `model.proto`'s cross-file `Route`
  through its accessor. **Declined:** documenting it without a lane case.

- **D-BIND-106 - the non-null gate counts only the nulls under valid parents: a null struct whose
  non-nullable children hold nulls is accepted. Wider than C++.** *LOCKED BY THE MAINTAINER
  2026-10-15 (BIND-7 review Q4).* Arrow lets a null struct's children hold anything, nulls included
  (arrow-rs's `new_null_array`, a struct built from children and a mask), and D-BIND-101 promises that
  nothing a C++ or Rust producer may write is refused. The gate of a struct reached as a nullable
  field, a list element or a map value therefore counts a child's nulls only where the struct itself
  is valid; one bitmap pass in `TryMake`. A top-level column of a batch has no parent and is counted
  whole, as before. **Declined:** C++'s gate, which refuses such a batch.

- **D-BIND-107 - the runtime support types are for generated code: hidden from IntelliSense and free
  to change with the plugin within a MAJOR.MINOR; `ListView` and `MapView` themselves stay public
  API.** *LOCKED BY THE MAINTAINER 2026-10-15 (BIND-7 review Q5).* Generated code compiles in the
  consumer's assembly, so `IArrowElements`, `IArrowRows`, the `*Elements` readers and the windows'
  constructors and `Of` must be public; they are marked `[EditorBrowsable(Never)]` and documented as
  for generated code, and the `out` variance, which buys nothing for struct types, is dropped.
  **Declined:** supporting them as extension points.

The next ones were ruled on 2026-10-15, at the start of BIND-8 (`Eiva.Fletcher.GatewayClient`), after
a read-only map of the gateway's protocol (`gateway/src/ws_session.*`, the wire-format spec), the
TypeScript client (`gateway-client-ts/`) and the lanes that run a live gateway. They settle what
D-BIND-72 left to BIND-8.

- **D-BIND-108 - the gateway client works on Arrow `RecordBatch`es: its managed codec converts a
  batch to positional rows and back with the Arrow schema, and the generated gateway pair calls the
  model's `ToArrow` / `FromArrow`, as the native pair does.** *LOCKED BY THE MAINTAINER 2026-10-15
  (BIND-8 Q1).* D-BIND-72 lets the client take a `RecordBatch` from the typed pair or constrain `T`
  itself. At the batch level the gateway and native paths share the whole model layer and one codec
  shape (Arrow in, bytes out, as C++'s codec behind the C ABI), and the descriptor the codec needs is
  read from the schema's `field_number` metadata (D-BIND-72 point 2). **Declined:** a client
  generic over the row type that encodes it directly (a second row-to-bytes path beside `ToArrow`).

- **D-BIND-109 - the gateway pair is asked for with `csharp_gateway`, which needs `csharp` (as
  D-BIND-99), and written to `<stem>.fletcher.gateway.cs` as `<Svc>_<Method>GatewayPublisher` and
  `<Svc>_<Method>GatewaySubscriber`; it references `Eiva.Fletcher.GatewayClient` only and reuses the
  model's `<Svc>_<Method>Topic` (D-BIND-77).** *LOCKED BY THE MAINTAINER 2026-10-15 (BIND-8 Q2).*
  Distinct names let one project hold both pairs, and nothing native reaches a gateway-only build.
  **Declined:** the native pair's names in a `Gateway` sub-namespace (two types of one name, told
  apart by a `using`).

- **D-BIND-110 - the client has one control request awaiting its reply at a time, so an `error`
  reply always belongs to the request in flight.** *LOCKED BY THE MAINTAINER 2026-10-15 (BIND-8 Q3).*
  The gateway's `{"type":"error"}` carries no request id (`ws_session.cpp:132-142`). The TypeScript
  client queues pending requests and gives an error to the oldest one, which can blame the wrong
  call; serialising the requests (publishes included) makes the attribution certain. Data frames to
  subscribers are not requests and are never held back. **Declined:** the TypeScript queue
  (parity, at the price of a misattributed error).

- **D-BIND-111 - `PublishAsync` completes when the gateway acknowledges with `published`, or fails
  with the gateway's error.** *LOCKED BY THE MAINTAINER 2026-10-15 (BIND-8 Q4).* As the TypeScript
  client does; a refused publish is tied to its call. A PUBLISH frame carries one row
  (`publish_frame.hpp`), so publishing a batch sends one frame per row and awaits each
  acknowledgement in turn (D-BIND-110); pipelining is a later optimisation, measured first.
  **Declined:** completing when the frame is sent, with errors as an
  event.

- **D-BIND-112 - `create_topic` announces the schema as `schemaIpc`, the topic's Arrow IPC schema
  in base64, as the gateway's `subscribed` reply already carries it; the gateway declares the topic
  with that schema decoded by `DeserializeSchemaIpc`.** *LOCKED BY THE MAINTAINER 2026-10-16 (BIND-8
  Q5).* Measured first (`gateway/tests/test_schema_codec.cpp`, `GatewayDeclaredSchemaTest`): the
  JSON `schema` member keeps only field names and a short list of scalar types
  (`BuildArrowSchemaFromJson`), so a struct, list, map, Timestamp or Duration field is refused, and
  nullability and the `field_number` metadata are dropped. A topic's re-declaration is idempotent
  only for byte-identical Arrow IPC (`pubsub/internal/schema_conflict.hpp`), so even a FLAT message
  announced by a gateway client conflicts (`kSchemaConflict`) with the same message declared by a
  native C++ or C# publisher; the generated schema's own Arrow IPC round-trips to the native
  declaration. The C# client sends `schemaIpc` and not `schema`. The gateway keeps accepting the JSON
  `schema` for existing clients (gateway-client-ts) and uses `schemaIpc` when both are present.
  Changes the gateway (this team's component since 2026-10-16) and the wire-format spec's WebSocket section, in
  BIND-8b. **Declined:** completing the JSON parser for nested types (still lossy: units,
  nullability, metadata, so the conflict stays); announcing no schema (a subscriber relying on the
  gateway's schema then gets none).

- **D-BIND-113 - `FletcherClient` delivers a subscription's rows to a handler delegate, called on
  the client's receive loop with the decoded one-row `RecordBatch` and the `Envelope` it came in;
  the envelope is BORROWED (valid until the handler returns), as the native `AttachmentsView` is.**
  *LOCKED BY THE MAINTAINER 2026-10-16 (BIND-8 Q6).* The same style as the native `SubscriberArrow`
  handler and the TypeScript client's callback, and the cheapest: no copy of the attachments and no
  queue or thread hop per message, where an async stream would copy every attachment (its items
  outlive the receive buffer) and add a hop. The price is that a slow handler holds the receive loop,
  and with it the control replies on that connection; that is documented, and a handler that throws
  is counted and reported, not fatal, as natively. The generated gateway pair (8c) wraps the handler
  into a typed row. **Declined:** an `IAsyncEnumerable` stream (can be added later as an adapter over
  the delegate, paying its copy only where used); both from the start.

- **D-BIND-114 - `FletcherClient` takes a topic as its `string` key, the segments joined with `/`,
  exactly as the protocol carries it.** *LOCKED BY THE MAINTAINER 2026-10-16 (BIND-8 Q7).* As the
  TypeScript client does; the generated gateway pair passes the model's `<Svc>_<Method>Topic.Key`
  (D-BIND-77), so typed callers never spell one. The client refuses only what the gateway cannot
  route: an empty topic, or one longer than a PUBLISH frame's 65535 UTF-8 bytes. **Declined:** a
  segment list joined by the client (a second form for one wire string); a `GatewayTopic` type copying
  the native `TopicPath`'s checks, which this package cannot reference.

- **D-BIND-115 - a generated gateway pair BORROWS a `FletcherClient` the caller connects and
  disposes.** *LOCKED BY THE MAINTAINER 2026-10-19 (BIND-8 Q8).* One connection serves every pair of
  an app, as the gateway protocol and the TypeScript client are meant to be used: one receive loop,
  one gateway session, and requests from all pairs ordered through the client's one in flight
  (D-BIND-110). `<Svc>_<Method>GatewayPublisher.CreateAsync(client, ct)` declares the topic
  (`CreateTopicAsync` with the message's schema, as `schemaIpc`, D-BIND-112) and returns the
  publisher, with `PublishAsync(row)`, `(row, attachments)` and `(rows)`;
  `<Svc>_<Method>GatewaySubscriber(client)` has `SubscribeAsync(handler, ct)` returning the
  subscription's id and `UnsubscribeAsync(id, ct)`; the handler is `<Svc>_<Method>GatewayHandler(Msg
  row, Envelope envelope)`, the envelope borrowed (D-BIND-113). The pairs are not disposable; the
  client must outlive them, and handler faults are reported on the client. An app that wants a slow
  subscriber isolated gives it a client of its own. **Declined:** each pair owning its own client
  (self-contained and isolated, like the native pair, but one WebSocket per publisher and subscriber,
  no ordering across topics, and no way back to a shared connection).

The next ones were ruled during BIND-8d (the cross-language rows and packaging), from 2026-10-19.
First a measurement: for all 21 generated messages of five `.proto` files (`coverage.proto`'s
`CompositeCoverage` among them), the C# client's `schemaIpc` (Apache.Arrow's writer), decoded by the
gateway's `DeserializeSchemaIpc` and encoded again, gives exactly the bytes a native C++ publisher
declares (`SerializeSchemaIpc` of the generated schema). The raw C# bytes differ from C++'s in every
case, so the gateway's decode and re-encode is what makes a C# gateway declaration and a native one
of one topic agree; no fix was needed.

- **D-BIND-116 - the gateway-client rows of the cross-language matrix run in a lane of their own,
  `integration-tests/gateway-dotnet-crosslang`: a gateway over `fastdds`, the C# gateway client in
  the test process, and a C++ peer, a C# native peer and a TypeScript peer as child processes, all
  generated from one `.proto` over `coverage.proto`'s `CompositeCoverage`, one topic per
  direction.** *LOCKED BY THE MAINTAINER 2026-10-19 (BIND-8d Q1).* The C++ and C# native peers are
  other processes, so the gateway's provider is `fastdds`; the TypeScript peer joins the same gateway,
  so the whole matrix row of BIND-8 is read in one place. `CompositeCoverage` already reaches every
  mapped shape, and its fixture rows are already transcribed in C++ and C#. **Declined:** adding the
  peers to `gateway-dotnet-end-to-end` (mixes the client's own cases with the matrix, and makes that
  lane need the native shim and Node); the TypeScript row there over `inprocess` and the native rows
  in a new lane (two places to read one matrix row from).

- **D-BIND-117 - in the lane's TypeScript → C# row, the C# gateway client declares the topic with
  `schemaIpc` and the TypeScript peer publishes on it without declaring it; BIND-T gives TypeScript
  a full-fidelity declaration: the plugin embeds each message's Arrow IPC schema in the generated
  `.ts`, and `createTopic` sends it as `schemaIpc`.** *LOCKED BY THE MAINTAINER 2026-10-19 (BIND-8d
  Q2).* The TypeScript client announces only the JSON `schema`, which the gateway refuses for a
  struct, list, map, Timestamp or Duration field and which drops nullability and `field_number`
  (D-BIND-112), and over `fastdds` a topic declared with no schema never announces one, so native
  subscribers would never decode its rows. The gateway's one `Publisher` serves every WebSocket
  session, so a declaration by the C# client holds for the TypeScript publisher. The plugin already
  computes `SerializeSchemaIpc` of each schema (the `ipc` output), so the bytes TypeScript sends are
  the native declaration's exactly and its runtime needs no Arrow IPC writer (BIND-T's "no new
  runtime dependency"). When BIND-T lands, the lane's TypeScript peer declares its own topic.
  **Declined:** the fix inside 8d, or as its own slice before 8d's TypeScript row (BIND-T's area,
  and the workaround proves the rows meanwhile); a flat message for the TypeScript rows (misses
  nested, list, map and Timestamp shapes); the TypeScript client writing IPC with `apache-arrow`
  (a heavy runtime dependency); a complete JSON parser in the gateway (still lossy, D-BIND-112).

- **D-BIND-118 - the TypeScript backend imports a message of another `.proto` file by name: the
  generated `.ts` writes `import { <Msg> } from './<stem>.fletcher.js'` and `import type { I<Msg> }`
  for each message of another file its fields reach, where it wrote an empty placeholder
  (`import { /* cross-file types */ }`).** *LOCKED BY THE MAINTAINER 2026-10-19 (BIND-8d Q3).* Found
  building the lane's one `.proto`: every backend skips a pub/sub method whose message is declared
  in another file, so the lane streams a local `CrossRow { int32 seq = 1;
  integration.coverage.CompositeCoverage row = 2; }`, a field of another file's message, which C++
  and C# map (D-BIND-87) and TypeScript did not: the placeholder left `ICompositeCoverage` and
  `CompositeCoverage.fields` unresolved, so the file neither type-checks nor runs. No golden or test
  held a cross-file TypeScript import. Fixed in the plugin with a `test_ts_visitor` case; the lane is
  its end-to-end proof. **Declined:** a self-contained lane `.proto` (three new fixture
  transcriptions, and the TypeScript gap stays); the TypeScript row waiting for BIND-T.

- **D-BIND-119 - the TypeScript backend describes a singular flatten-wrapper field by what it
  carries, a list of the wrapper's element (`ILeaf[]`, `element: ... Leaf.fields`), not as a list
  of the wrapper (`IStructListWrapper[]`, `StructListWrapper.fields`).** *LOCKED BY THE MAINTAINER
  2026-10-19 (BIND-8d Q4).* Found by the lane: `StructListWrapper flattened_struct_list = 12;` is
  `list<Leaf>` in the schema and on the wire in C++ and C#, but TypeScript's descriptor said
  `list<struct<list<Leaf>>>`, so its decoder cannot read any non-empty such row a C++ or C# peer
  writes ("Offset is outside the bounds of the DataView" on protoc-coverage's own C++ goldens, which
  decode once the element is `Leaf`), and what it writes cannot be read back by them. The wrapper's
  name had been kept on purpose (GIR-7, `DeclaredWrapperFor`) to reproduce the TypeScript output of
  before the IR rewrite byte for byte; no test ever exchanged the shape. TypeScript now renders the
  IR's own struct identity, as C++ and C# do; the protoc-coverage TypeScript golden and
  `test_ts_visitor`'s first case move with it. **Declined:** sending those two fields empty in the
  TypeScript rows and carrying the fix as BIND-T debt; dropping the TypeScript rows until BIND-T.

- **D-BIND-120 - `Eiva.Fletcher.GatewayClient` takes no reference on `Eiva.Fletcher.Model`.**
  *LOCKED BY THE MAINTAINER 2026-10-19 (BIND-8d Q5).* The client's code uses nothing from it, and
  `Eiva.Fletcher` does not reference it either: only the GENERATED model needs `Model` (its
  `Timestamp` and `Duration`, D-BIND-74), and the consumer that generates it references `Model`
  itself, as both live lanes do. The package stays `Apache.Arrow`-only, its one dependency.
  **Declined:** a reference for a one-`PackageReference` gateway consumer (then `Eiva.Fletcher`
  would take the same, a BIND-9 packaging question).

The next ones were ruled at BIND-8e, the review of BIND-8 (`plans/reviews/BIND-8-codereview.md`),
from 2026-10-19.

- **D-BIND-121 - `FletcherClient.Completion` is a public `Task` that completes when the connection
  ends: successfully on `DisposeAsync`, faulted with the reason otherwise (the gateway closed, a send
  or receive failed); the client still never reconnects.** *LOCKED BY THE MAINTAINER 2026-10-19
  (BIND-8e Q1).* Without it a subscriber-only app could not tell that its subscriptions were gone:
  the end of the connection failed only a request in flight. An app awaits `Completion`, then builds
  a new client and subscribes again; reconnecting stays the app's, as in the TypeScript client.
  **Declined:** a `Disconnected` event (the same information, but a handler attached late misses
  it); automatic reconnection (backoff, replayed declarations, rows lost in the gap and D-BIND-110's
  ordering make it a design of its own).

- **D-BIND-122 - the TypeScript backend imports each other file as ONE namespace,
  `import * as <alias> from '<path relative to the importing file>'`, and names its messages through
  it (`<alias>.Leaf.fields`, `<alias>.ILeaf`); the alias is the imported file's path with every
  character that is not a letter or digit made `_`, then `_fletcher` (`dir/dep.proto` →
  `dir_dep_fletcher`). Amends D-BIND-118.** *LOCKED BY THE MAINTAINER 2026-10-19 (BIND-8e Q2).* The
  review found D-BIND-118's bare names collide: two imported files declaring `Leaf`, an imported name
  equal to a local message or to the runtime's `WireTypeId` / `TypedSchema`. A namespace per file
  makes every such clash impossible. The same fix (review B1) makes the path relative to the importing
  file's directory, so a file below the include root imports a module that exists. **Declined:** a
  named import aliased per name (collision-free too, longer lines); refusing a clash at generation
  (a legal set of `.proto` files could fail to generate).

- **D-BIND-123 - the gateway builds a `subscribed` reply before it keeps the subscription, releasing
  it if building the reply throws, and reads a `field_number` it cannot parse as absent; `create_topic`
  keeps accepting any well-formed Arrow IPC schema.** *LOCKED BY THE MAINTAINER 2026-10-19 (BIND-8e
  Q3).* Since D-BIND-112 any WebSocket client can declare a schema with arbitrary field metadata; a
  non-numeric `field_number` made `std::stoi` throw out of every later reply for that topic, after the
  subscription was already registered, so each `subscribe` failed and left a live subscription behind.
  **Declined:** also refusing such a schema on `create_topic` (native publishers' metadata is not
  checked so; the reply must survive it either way); recording it as debt.

- **D-BIND-124 - a gateway reply the client cannot parse affects only the request it answers: a
  `schemaIpc` Apache.Arrow cannot read is treated as absent (a subscriber that passed its own schema
  is unaffected; one relying on the gateway's gets the existing no-schema error), and an unknown or
  malformed reply fails the request in flight with a `GatewayException`, or is ignored when none is.
  The connection stays up.** *LOCKED BY THE MAINTAINER 2026-10-19 (BIND-8e Q4).* Before, any such
  reply ended the connection and every subscription on it, even when the subscriber never needed the
  gateway's schema; the TypeScript client ignores replies it cannot use. **Declined:** ending the
  connection (strict, but one exotic schema from a native publisher would stop every topic).

- **D-BIND-125 - the row handlers stay synchronous (D-BIND-113), and their documentation says so
  where a caller meets them: an `async` lambda compiles as `async void`, so an exception after its
  first `await` crashes the process and the envelope is reused; async work copies what it needs and
  hands it to the app's own queue.** *LOCKED BY THE MAINTAINER 2026-10-19 (BIND-8e Q5).* On
  `GatewayRowHandler`, `SubscribeAsync`, the generated `<Svc>_<Method>GatewayHandler` and the
  package README. **Declined:** an async overload (`Func<RecordBatch, Envelope, ValueTask>`): safer,
  but more surface on the client and the generated pair, for a pattern the docs can steer.

- **D-BIND-126 - `FletcherClientOptions` gains `ConfigureWebSocket`, an `Action<ClientWebSocketOptions>`
  called before connecting (headers and authentication, proxy, client and server certificates,
  keep-alive), and `MaxMessageBytes` (default 64 MiB): a frame larger than it ends the connection,
  with `Completion` (D-BIND-121) faulted naming the size, instead of growing the receive buffer
  without bound.** *LOCKED BY THE MAINTAINER 2026-10-19 (BIND-8e Q6).* With `Uri` alone the client
  could not reach a gateway behind an authenticating proxy or on `wss://` with a private CA, and one
  huge frame pinned a doubled buffer for the client's life (overflowing past 1 GiB). The hook is
  AOT-safe and adds no option per socket setting. **Declined:** the size cap alone, socket
  configuration later; deferring both.

- **D-BIND-127 - `FletcherClientOptions.ReplyTimeout`, a `TimeSpan?`, off by default: when set and a
  request's reply does not come within it, the connection ends and `Completion` faults naming the
  action and the time.** *LOCKED BY THE MAINTAINER 2026-10-19 (BIND-8e Q7).* The gate is held until
  the reply (D-BIND-110), so one lost reply would wedge every later call, and cancelling a call only
  frees its caller. Ending the connection, not just the call, is what keeps D-BIND-110's attribution
  sound: a late reply can never meet the next request. **Declined:** on by default (a slow publish
  under backpressure could trip it); documentation only.

- **D-BIND-128 - the client's internal JSON-schema code is deleted with its tests
  (`ControlMessages.SchemaToJson` and its writers, the wire-type table, `SubscribedReply.SchemaJson`),
  and Bucket 2 maps `test_schema_codec` to what the client owes since
  D-BIND-112: the `schemaIpc` it writes reads back as the same declaration (a managed test over the
  C++-written `.ipc` goldens, and the cross-language lane live); the gateway-internal cases (the
  conflict measurement, base64, `FieldToJson`) are recorded as exclusions with their reason.**
  *LOCKED BY THE MAINTAINER 2026-10-19 (BIND-8e Q8).* The class is `internal`, so no user could call
  it, and the client's one public way to announce a schema, `CreateTopicAsync`, always sends
  `schemaIpc`; its tests were counted as the port while proving nothing about what the client sends.
  The protocol is unchanged: the gateway keeps accepting and sending both forms (the TypeScript client
  uses the JSON one until BIND-T). **Declined:** moving the code to the test project as a reference;
  exposing the JSON form publicly (surface for a lossy form D-BIND-112 moved away from).
  *AMENDED BY THE MAINTAINER 2026-10-19:* `GatewayRowCodec.DecodeBatch` stays (internal, with its
  tests). It is row decoding, not JSON-schema code; review B4 listed it only because nothing called
  it yet. It is the building block of a batched gateway subscription, the parity the C++ and native
  C# batched `SubscriberArrow` have (slice 8f, ruled as D-BIND-133).

- **D-BIND-129 - the cross-language lane makes its peer → C# exchanges live, not replayed: before a
  peer publishes the fixture rows, the C# side has it publish probe rows (`seq` -1, never compared)
  until one arrives through the gateway, so the gateway's reader has matched the peer's writer; and a
  peer prints a row it cannot decode as `ERROR`, so it is never blamed on the Fast DDS defect. The
  durable profile stays as a safety net, and each retry attempt must deliver all four rows itself
  (review B3).** *LOCKED BY THE MAINTAINER 2026-10-19 (BIND-8e Q9).* Every peer → C# row went through
  a TRANSIENT_LOCAL backlog a new gateway reader replays, the very path Fast DDS 3.4's data-sharing
  defect breaks, and it did once on the green CI run; `gateway-fastdds-ts` remains that defect's
  guard. **Declined:** keeping the backlog path as a second guard.

- **D-BIND-130 - D-BIND-117's TypeScript half is done in BIND-8e, not BIND-T: the plugin writes each
  message's Arrow IPC schema (the bytes of its `ipc` output) into the generated `TypedSchema` as
  base64 `schemaIpc`, and `FletcherClient.createTopic(topic, typedSchema)` sends it as `schemaIpc`
  (a hand-built descriptor still sends the JSON `schema`); the cross-language lane's TypeScript peer
  then declares its own topic. Amends D-BIND-117.** *LOCKED BY THE MAINTAINER 2026-10-19 (BIND-8e
  Q10).* Until then a TypeScript and a C# publisher of one topic conflicted (the JSON declaration's
  bytes differ from the IPC one's), whichever declared second; BIND-T keeps its pair, which now
  declares with full fidelity. **Declined:** documenting the conflict until BIND-T.

- **D-BIND-131 - the C ABI codec refuses what the managed gateway codec refuses: a null map key, and
  a row whose dictionary index points at a null dictionary entry, each with an error naming the
  field, instead of encoding whatever bytes lie under the null.** *LOCKED BY THE MAINTAINER
  2026-10-19 (BIND-8e Q11).* Found by the review (`c-abi/src/nanoarrow_codec.cpp`); the managed
  codec's header claimed the same refusals as the native one, which held only one way. A C ABI case
  per refusal, and `ManagedCodecAgreesWithNativeTests` asserts both codecs refuse. **Declined:**
  recording it as debt with the header corrected.

- **D-BIND-132 - the defaults 8a and 8b took without a ruling are locked: (a) the managed codec
  accepts exactly the C ABI codec's types; (b) it refuses with `ArgumentException` (a schema or value
  the wire cannot carry) and `InvalidDataException` (bytes that do not decode); (c) a null struct
  decodes with its children null; (e) subscription ids are parsed from strings only, as the gateway
  writes and reads them; (g) a row that cannot be decoded is reported and counted, not thrown on the
  receive loop, and `GatewayHandlerFaultedEventArgs` gains a `Reason` (`HandlerThrew` or
  `RowUndecodable`), and `FletcherClient.DroppedRows` counts the rows dropped silently until now
  (early rows past the cap, rows for an unknown subscription, rows after an unsubscribe).** *LOCKED BY
  THE MAINTAINER 2026-10-19 (BIND-8e Q12).* The review found all five sound; (b) because the package
  cannot use `Eiva.Fletcher`'s exception types (D-BIND-120), (e) because the gateway requires it. The
  other three defaults are settled elsewhere: (d) and (h) went with the JSON-schema code (D-BIND-128),
  (f) "no reconnect" stands with `Completion` (D-BIND-121). **Declined:** (g) unchanged, the drops
  uncounted and one meaning for both causes.

- **D-BIND-133 - the gateway client gains a batched subscription, matching the native
  `SubscriberArrow.SubscribeBatched` (D-BIND-25) and C++'s batched `SubscriberArrow::Subscribe`:
  `FletcherClient.SubscribeBatchedAsync(topic, schema, GatewayBatchHandler handler,
  GatewayBatchOptions? options, CancellationToken)`, where `GatewayBatchHandler(RecordBatch batch,
  IReadOnlyList<Envelope> envelopes, GatewayBatchStatus status)`. A window's rows are kept as received
  and decoded at the flush with `GatewayRowCodec.DecodeBatch`; a window holding an undecodable row is
  decoded again row by row, and the bad rows are dropped and counted. Slice 8f, before BIND-8
  closes.** *LOCKED BY THE MAINTAINER 2026-10-19.* (a) Own types in `Eiva.Fletcher.GatewayClient`
  (`GatewayBatchOptions` with `MaxRows` 8000 and `Timeout` 1 min, `Timeout.InfiniteTimeSpan` for none,
  the native bounds and refusal; `GatewayBatchStatus` with `Reason` and `RowsDropped`;
  `GatewayBatchReason` `RowLimit`, `Timeout`, `Closing`), since the package cannot reference
  `Eiva.Fletcher` (D-BIND-120); names and defaults as native's. (b) The envelopes are parallel to the
  rows, a dropped row's left out. (c) The handler owns the batch. (d) A window of only dropped rows
  still delivers a zero-row batch. (e) One decode per window normally, a second pass only for a
  window with a bad row. (f) A window flushes early (`RowLimit`) before its raw rows pass 2^31-2 bytes
  (D-BIND-63's ceiling). (g) A `RowLimit` flush runs on the receive loop, a `Timeout` flush on a timer
  thread, a `Closing` flush on the thread that unsubscribes or ends the connection; ONE subscription's
  handler never runs concurrently with itself (a queue, no lock held across the handler), which is
  stricter than native's D-BIND-64. (h) `UnsubscribeAsync`, `DisposeAsync` and the connection ending
  each flush the pending window as `Closing` first. (i) A handler exception raises `HandlerFaulted`
  (`HandlerThrew`); an undecodable row raises it (`RowUndecodable`) and counts in `UndecodableRows`.
  (j) The per-row `SubscribeAsync` is unchanged. (k) TypeScript has no batched shape either: a BIND-T
  parity item. **Declined:** re-expressing `SubscribeAsync` as `MaxRows` = 1 (as native does) now; a
  types package shared by `Eiva.Fletcher` and the gateway client.

## Rulings of 2026-10-20 — BIND-9a, a preview for testers

BIND-9 is split: **BIND-9a** delivers a preview bundle to testers (the NuGet packages, the gateway and the plugin, as a per-push CI artifact; nothing released before the merge); **BIND-9b** is the rest of
BIND-9 as written (the `nuget.eiva.com` publish job and its self-hosted runner, the glibc floor
lowered if wanted, `Eiva.Fletcher.Protoc`, the packed-size budget, the Windows and `fastdds`
NativeAOT runs, the null-in-a-non-nullable-column question, the C#-native cross-language rows).
Facts checked before the questions were put: `ci.dotnet` already packs all four packages but
`Eiva.Fletcher.Interop` carries no shim yet; the released gateway (`gateway-v0.5.0-alpha`) cannot
accept `schemaIpc` on `create_topic` (D-BIND-112 exists on this branch only); the released plugin
(`protoc-v0.5.1-alpha`) has no C# backend; every release tag sits on `main`; the repository is
public.

- **D-BIND-134 - the first preview carries all four packages: `Eiva.Fletcher`,
  `Eiva.Fletcher.Interop`, `Eiva.Fletcher.Model` and `Eiva.Fletcher.GatewayClient`.** *LOCKED BY
  THE MAINTAINER 2026-10-20 (BIND-9a Q1).* So 9a owns what makes `Interop` usable from a package:
  the shim under `runtimes/win-x64/native/` and `runtimes/linux-x64/native/`, built per RID and
  packed once (D-BIND-27), and `LICENSE` + `THIRD-PARTY-LICENSES.txt` in every package. **The
  shim is the only unmanaged file, and it is self-contained:** Fletcher's core, pubsub and both
  providers, Fast DDS, Fast CDR, foonathan-memory, tinyxml2, Micro XRCE-DDS and nanoarrow are all
  linked into it (`c-abi/conanfile.py` forces the eProsima chain static). Measured on the Linux
  shim 2026-10-20: it loads only `libstdc++.so.6`, `libgcc_s.so.1`, `libm.so.6`, `libc.so.6` and the
  loader. Measured on the Windows shim by CI at `9d214c9` (`ci.pr` 39490321322): it loads only
  Windows system DLLs (`KERNEL32`, `ADVAPI32`, `WS2_32`, `MSWSOCK`, `IPHLPAPI`, `SHLWAPI`), the VC++
  runtime (`MSVCP140`, `MSVCP140_ATOMIC_WAIT`, `VCRUNTIME140`, `VCRUNTIME140_1`) and the UCRT
  (`api-ms-win-crt-*`); `psapi.dll`, expected from the link line, is not among them. Checked on
  every pack: the shim's dependency list (`objdump -p` on Linux, `dumpbin /dependents` on Windows)
  names only system libraries and the C/C++ runtime; the
  test-only probe shim (`fletcher-c-abi-probe`) is never packed; `THIRD-PARTY-LICENSES.txt` lists
  the libraries linked into the shim. **Declined:** `GatewayClient` and `Model` alone first (the
  managed pair, ready today).

- **D-BIND-135 - testers get a gateway that speaks to the C# client as gateway BINARIES in the
  per-push preview bundle (D-BIND-139), built from this branch exactly as `cd.gateway.yml` would
  release them (`gateway-linux.tar.gz`, `gateway-windows.zip`); no gateway release before the
  merge.** *LOCKED BY THE MAINTAINER 2026-10-20 (BIND-9a Q2, revised the same day).* First ruled as
  a gateway preview release through `cd.gateway.yml`; revised when the maintainer ruled that before
  the merge nothing but the per-push bundle is published. **Declined:** a preview release; testers
  building the gateway themselves.

- **D-BIND-136 - testers generate C# code with the plugin BINARIES in the per-push preview bundle:
  the standalone static `protoc-gen-fletcher` for Linux and Windows, as `cd.protoc.yml` would
  release them, and `fletcher/options.proto`; no plugin release (npm, Conan or GitHub) before the
  merge; `Eiva.Fletcher.Protoc` (D-BIND-71) stays in BIND-9b.** *LOCKED BY THE MAINTAINER
  2026-10-20 (BIND-9a Q3, revised the same day).* First ruled as a plugin preview through
  `cd.protoc.yml`, which would have put a version on the public npmjs.com that cannot be freely
  removed after 72 hours. **Declined:** a preview release; testers without generated code.

- **D-BIND-137 - no release tag of any kind is cut from `feature/csharp-bindings` before PR #129
  merges.** *LOCKED BY THE MAINTAINER 2026-10-20 (BIND-9a Q4, revised the same day).* First ruled
  as "`-preview.N` tags only"; with nothing released before the merge, no tag is needed. The CD
  workflows (`cd.dotnet`, `cd.gateway`, `cd.protoc`) run from `main` after the merge, as today.
  Stands beside D-BIND-30 (one PR for the round). **Declined:** prerelease tags from the branch.

- **D-BIND-138 - before the merge the only version is the per-build `0.6.0-pr.129.<run number>`
  (NuGet packages and the bundle); every Fletcher package moves to 0.6 together when PR #129
  merges, and the first release is `0.6.0-alpha`.** *LOCKED BY THE MAINTAINER 2026-10-20 (BIND-9a
  Q5, revised the same day).* MINOR moves because the gateway protocol changed across components
  (`schemaIpc` on `create_topic`, sent by the C# and TypeScript clients): a 0.5 gateway does not
  serve the new clients. Until the merge the bundle builds against the 0.5 core and pubsub, a
  recorded and temporary exception to "same MAJOR.MINOR everywhere". Amends decision #13's "`0.5.x`
  series". Semver ranks `0.6.0-pr.129.N` ABOVE `0.6.0-alpha`, so before the first `-alpha` is
  published the maintainer deletes the remaining preview bundles (they expire after 30 days anyway)
  and the testers are told to clear their NuGet caches; the bundle README says so from the start.
  **Declined:** the 0.5 line; a `0.6.0-preview.1` tag (first ruled, dropped with D-BIND-137); a first
  post-merge release that sorts above the previews (`0.6.1-alpha`).

- **D-BIND-139 - every push to `feature/csharp-bindings` publishes ONE preview bundle, a 30-day
  workflow artifact (served by GitHub as a zip) named `fletcher-preview-0.6.0-pr.129.<run>`, and
  nothing else before the merge.** *LOCKED BY THE MAINTAINER 2026-10-20 (BIND-9a, replacing the Q6
  recommendation of a runner with a manual-push fallback, then widened from NuGet-only the same
  day).* Contents: `nuget/` the four packages (the shim for both RIDs inside `Interop`, D-BIND-134);
  `gateway/` the gateway binaries (D-BIND-135); `protoc/` the plugin binaries and
  `fletcher/options.proto` (D-BIND-136); `README-preview.md` (versions and commit, the floors, the
  relinking route, how to use a local NuGet folder, "delete and clear caches when `0.6.0-alpha`
  ships"); `LICENSE` (and `COPYING`, D-BIND-152) at the root, with each component's
  `THIRD-PARTY-LICENSES.txt` inside it: each package, each gateway archive, `protoc/` (**amended
  2026-10-21**, BIND-9a review Q7: the ruling first listed one notice at the root, and each binary
  links a different set). On this branch `ci.pr` builds dotnet, gateway
  and protoc on every push whatever the push touched (the path filters stay for other branches),
  which lengthens every run. A version unique to each build, because NuGet caches a package by
  version and a tester would otherwise keep an old build silently. No tag, GitHub release, npm,
  Conan or NuGet-feed push before the merge; the `nuget.eiva.com` feed and its self-hosted runner
  (D-BIND-28) follow in BIND-9b. The repository is public, so the bundle is downloadable by anyone
  with a GitHub account: external publication, which is why D-BIND-140 came before the first
  bundle. **The bundle is a development aid for this branch, not a release channel:** BIND-9b
  prepares the merge-time release on the branch (the lockstep bump to `0.6.0-alpha`, `cd.dotnet.yml`
  to `nuget.eiva.com`, every supported package released by its CD workflow from `main`) and the
  merge removes the bundle job (tracker, BIND-9). **Declined:** a rolling GitHub prerelease as the
  carrier; building the bundle on request only (`workflow_dispatch` or a label); a NuGet-only bundle
  (testers need the gateway and the plugin).

- **D-BIND-140 - LGPL, for the preview: the shared-shim relinking route.** *LOCKED BY THE MAINTAINER
  2026-10-20 (BIND-9a Q7; answers Q9 for the preview).* The shim is a shared library the .NET
  assembly loads at run time, so an application is a combined work using a shared-library mechanism
  and LGPL-3.0 §4(d)(1) is met when a user can replace the shim with one rebuilt from the
  corresponding source, the public repository at the commit the build names; Fast DDS and XRCE
  inside the shim are Apache-2.0 and do not change that (development plan P-1). Every package
  carries `LICENSE` and the graph-derived `THIRD-PARTY-LICENSES.txt` (#127's deployer); each package
  README states the relinking route (replace `runtimes/{rid}/native/` with a shim built from the
  named commit, with the command) and the NativeAOT static-linking caveat (N-8); the artifact names
  its commit. An engineering route chosen by the maintainer, not a legal opinion. **Declined:**
  managed packages only until a legal answer exists.

- **D-BIND-141 - the shim ships as CI builds it, and the package README states both floors:
  linux-x64 needs glibc 2.38 (measured by `ci.c-abi` at `e268f1a`) and libstdc++ `GLIBCXX_3.4.32`,
  the GCC 13 runtime (measured on the shim 2026-10-20): Ubuntu 24.04, Debian 13, RHEL 10 or newer;
  win-x64 needs the VC++ 2015-2022 redistributable (the dynamic CRT, D-BIND-38; the shim loads
  `MSVCP140`, `MSVCP140_ATOMIC_WAIT`, `VCRUNTIME140`, `VCRUNTIME140_1`, measured at `9d214c9`) and
  otherwise only DLLs every Windows has.** *LOCKED BY THE MAINTAINER 2026-10-20 (BIND-9a
  Q8; the libstdc++ floor and the Windows list added the same day, when the maintainer asked
  whether the packages hold every unmanaged part).* Lowering the glibc floor (building the shim on
  an older image) stays BIND-9b work (D-BIND-41). **Declined:** lowering the floor before the first
  preview.

Three more, ruled 2026-10-21 while slices 9a-2 and 9a-4 were built:

- **D-BIND-142 - the four packages carry ONE README, `dotnet/README.package.md`, packed into each
  as its `README.md` (`PackageReadmeFile`), with the version and the commit filled in at pack time.**
  *LOCKED BY THE MAINTAINER 2026-10-21 (BIND-9a-2).* It opens with a table of the four packages
  (what each is, which carries native assets), then the native section for `Eiva.Fletcher` and
  `Eiva.Fletcher.Interop` (the floors of D-BIND-141, the relinking route of D-BIND-140 naming the
  commit, the NativeAOT caveat), marked as not applying to `GatewayClient` and `Model`. CI passes
  the commit (`-p:FletcherSourceCommit`); `scripts/check_dotnet_packages.sh` refuses a package
  without the README, with a placeholder left, without the floors and the relinking route, or
  naming another commit. **Declined:** four READMEs, one per package (the shared text repeated).

- **D-BIND-143 - a preview bundle names the commit it was built from, or is not made.** *LOCKED
  BY THE MAINTAINER 2026-10-21 (BIND-9a-2).* A `pull_request` run builds the PR head MERGED into
  `main`, while the bundle and the package READMEs name the head as the source to relink from
  (D-BIND-140). The two are one tree until `main` moves; `ci.pr`'s `preview-bundle` job compares
  them and fails when they differ, asking for `main` to be merged into the branch. Found while
  9a-2 wired the commit into the README; true of every bundle so far (`main` has not moved since
  the branch point). **Declined:** naming both commits; recording it and changing nothing.

- **D-BIND-144 - the package README walks from a `.proto` to publish and subscribe, and every code
  block in it is code a test compiles and runs.** *LOCKED BY THE MAINTAINER 2026-10-21 (BIND-9a-4).*
  `dotnet/README.package.md` (D-BIND-142) shows, in order: the `.proto`, the package references,
  the MSBuild target that runs `protoc` with the plugin, rows published and received natively,
  the same as Arrow batches through `SubscriberArrow` and the generated accessor, and rows and
  batches through the gateway with the generated gateway pair. Each block is a region, between
  `<snippet name="...">` markers, of the preview-bundle consumer's `fix.proto`, project or
  program, which CI builds against the bundled packages and runs on Linux and Windows;
  `scripts/check_readme_snippets.py`, run before the pack, fails when a block and its region
  differ or either is missing. A compact walkthrough, not the full guide, which stays BIND-10's.
  **Declined:** snippets copied by hand, accepting drift until BIND-10.

Eight more, ruled 2026-10-21 on the fresh-context review of BIND-9a
(`plans/reviews/BIND-9a-codereview.md`, questions Q1 to Q10; Q7 amends D-BIND-139 above, Q10 is
wording):

- **D-BIND-145 - every artifact of a bundle names one commit, the PR head.** *LOCKED BY THE
  MAINTAINER 2026-10-21 (review Q1).* A `pull_request` run checks out the merge ref, so the
  packages' nuspec `<repository commit>` and their SourceLink named a merge commit (`4353dcc` in
  `pr.129.338`) that disappears at the next push, while the README and `VERSION` named the head.
  When `bundle` is set, `ci.pr` passes the head to `ci.dotnet`, `ci.gateway` and `ci.protoc` as a
  new `ref` input that every checkout in them uses (D-BIND-143 already proves the two are one tree),
  and `check_dotnet_packages.sh` requires the nuspec to name the commit. **Declined:** recording it
  and keeping the merge commit.

- **D-BIND-146 - an Arrow subscriber that subscribed before the topic was declared: the seam's
  contract, not a defect.** *LOCKED BY THE MAINTAINER 2026-10-21 (review Q2: "treat it as a defect:
  reproduce it, fix it if it is one").* Reproduced in `Fletcher.Tests` and traced to the seam: a
  subscription's schema mode is fixed when `Subscribe` returns (`provider.hpp`, spec §7 clause 1),
  and the default `inprocess` loopback (`schema_carriage=as_declared`) answers a subscription made
  before the declaration with no schema for its whole life, so `SubscriberArrow` drops and counts
  every row. A schema-carrying transport (Fast DDS, `inprocess` with `schema_carriage=carried`)
  delivers the schema with the data, so a subscriber that joined first decodes: conformance clause
  1 (`SchemaBeforeDataAcrossHandoff`) for C++, two new `SubscriberArrowTests` for C#. The generated
  row subscriber brings its own schema and works in any order. The package README scopes "declare
  the publisher first" to the default `inprocess`, and `SubscriberArrow`'s comment no longer
  promises more. **Declined:** documenting it without the reproduction.

- **D-BIND-147 - every fenced code block in the package README is a snippet or explicitly exempt,
  and the syntax is written down.** *LOCKED BY THE MAINTAINER 2026-10-21 (review Q3, with the
  maintainer's addition).* A block is preceded by `<!-- snippet: NAME -->` (checked against the
  consumer's region) or `<!-- snippet: none -->` (not test code); an unmarked block fails, and so
  does a README with no snippet. The syntax is described in a maintainers' HTML comment at the top
  of `dotnet/README.package.md` (invisible to package readers), a "Snippets" section of the
  consumer's README and the checker's header. **Declined:** checking marked blocks only.

- **D-BIND-148 - the Windows shim's dependencies are an exact allow-list.** *LOCKED BY THE
  MAINTAINER 2026-10-21 (review Q4).* The measured list of D-BIND-134 (`KERNEL32`, `ADVAPI32`,
  `WS2_32`, `MSWSOCK`, `IPHLPAPI`, `SHLWAPI`, `MSVCP140`, `MSVCP140_ATOMIC_WAIT`, `VCRUNTIME140`,
  `VCRUNTIME140_1`) and `api-ms-win-crt-*`; a new DLL, or the debug runtime, fails and needs a
  ruling. With it (review B1) both platforms fail on a list that parses empty or lacks
  `KERNEL32.dll` / `libc.so.6`. **Declined:** the System32-presence heuristic.

- **D-BIND-149 - the linux-x64 floors are asserted: measured equals stated.** *LOCKED BY THE
  MAINTAINER 2026-10-21 (review Q5).* `scripts/shim_floors.env` holds `GLIBC_FLOOR` and
  `GLIBCXX_FLOOR`; `check_shim_dependencies.sh` requires the shim's highest `GLIBC_` and
  `GLIBCXX_` symbol versions to EQUAL them (a lower one fails too, so the README moves with it), and
  `check_dotnet_packages.sh` requires the package README to state them. **Declined:** leaving it to
  BIND-9b's floor work.

- **D-BIND-150 - a re-run that rebuilds gets a version of its own:
  `0.6.0-pr.129.<run>.<attempt>` when the attempt is above 1.** *LOCKED BY THE MAINTAINER
  2026-10-21 (review Q6).* Computed once, as `detect-changes`' `bundle-version` output, because
  `run_attempt` also rises on "re-run failed jobs", where the jobs that passed keep the first
  attempt's outputs: one output keeps every job of a run on one version in both kinds of re-run.
  **Declined:** documenting the reuse.

- **D-BIND-151 - the consumer runs `fastdds` from the packaged shim, and no route accepts more than
  was sent.** *LOCKED BY THE MAINTAINER 2026-10-21 (review Q8, with T9).* A fifth route publishes
  and receives over `fastdds` in one process on its own domain, after a probe row shows discovery is
  done; every queue-fed route waits briefly after the last row, so an extra or duplicate delivery
  fails it. **Declined:** leaving `fastdds` to BIND-9b.

- **D-BIND-152 - `COPYING`, the GPL-3.0 text the LGPL-3.0 incorporates, ships beside `LICENSE`
  everywhere.** *LOCKED BY THE MAINTAINER 2026-10-21 (review Q9).* In the repository root, every
  NuGet package, both gateway archives, the plugin's licence artifact and the bundle; checked where
  `LICENSE` is. Fast DDS's own vendored third-party code and Apache-2.0 `NOTICE` files are a BIND-9b
  item, verified before the first release. **Declined:** all three now.
