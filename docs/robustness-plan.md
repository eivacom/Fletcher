# Robustness Improvements — Execution Plan

Tracking doc for the generator-hardening work-stream. Goal: bring the
generated-code layer and the full encode→decode chain up to the test-coverage
and correctness bar of a mature library (the standard we hold Apache Arrow to).

> **Status — Phases 2 & 3 LANDED in round GIR (2026-07-11).** **Phase 1** shipped
> as PR #98 (codec / positional-I/O decode hardening). **Phase 2** (the recursive,
> language-neutral generator IR + recursive-visitor emitters) and **Phase 3** (the
> compile-and-run harness, the encode/decode parity oracles, and the codec
> edge/boundary + property/fuzz suites) are now **DONE**, delivered as round
> **GIR** — items GIR-1..GIR-11 on `feature/generator-ir-rewrite`. Every migrated
> emitter is **wire byte-identical** to the flat generator (guarded by the GIR-2
> oracles), and the folded-in generator fixes closed **#55** (unsupported → build
> error), **#53-generated** (no `.ValueOrDie()` in emitter output), and **#75**
> (typed enum symbols). Context that shaped the design: **#96** added the
> in-process/IPC schema builder and **#106 (RBA)** the ~2,590-line
> RecordBatch-accessor emitter (C++ **and** Rust); the **BIND** (C#/Rust bindings)
> round builds on this IR, and the RBA emitter's reconciliation onto the IR is the
> deferred tail — round **RIR** ([RIR-rba-onto-ir.md](../plans/RIR-rba-onto-ir.md)).
> The sections below now record the **achieved end-state** — architecture and
> reference that outlive the round — not a forward plan; the per-item execution
> artifacts (`plans/GIR-*`) are archived. *Migrated into this spec from the GIR
> execution artifacts, 2026-07-11.*

## How it shipped (round GIR)

- Delivered as **11 rebased items** (GIR-1..GIR-11) on
  `feature/generator-ir-rewrite`, rebased on `main` — not merged (repo
  convention). The sequencing the plan demanded held: **harness first** (GIR-1,
  greenfield — there had been no compile-and-run harness, only `cat`/`md5` in
  `protoc/test_package`), **then oracles** (GIR-2 — `Encode()==EncodeRow()`
  byte-identity + a decode round-trip value-equality oracle), **then the IR +
  emitter migrations behind those guards** (GIR-3 IR + edge encode, GIR-4 edge
  decode, GIR-5 unified schema+IPC, GIR-6 Arrow view/`ToArrowRow`, GIR-7 TS),
  **then the folded generator fixes** (GIR-8 #55 + #53-generated, GIR-9 #75) and
  **coverage** (GIR-10 codec-edge + scalar-leaf nesting, GIR-11 property/fuzz). The
  RBA and BIND emitters were left outside this work-stream (see Sequencing).
- **Dependency edge: the IR → BIND-5/6** (generated Rust/C# rows/services) still
  stands — starting BIND's language emitters before the IR existed would have
  forced writing them against the flat model and rewriting immediately.

## Three phases

1. **Phase 1 — DONE (#98).** Runtime codec / positional-I/O decode hardening vs
   malformed input. Historical detail retained below; its byte-compatibility
   invariant remained a standing constraint on Phases 2/3.
2. **Phase 2 — DONE (round GIR).** The protoc generator now sits on a **recursive,
   language-neutral type IR + recursive-visitor emitters**.
3. **Phase 3 — DONE (round GIR; landed before/with Phase 2).** The generated-code
   layer and the entire encode→decode chain are rigorously tested; the harness +
   oracles guarded the Phase 2 rewrite.

---

## Phase 1 — Codec / positional-I/O correctness hardening — ✅ DONE (PR #98)

Merged to `main`. Delivered: overflow-safe reader bounds (`n > size - pos`) in
`row_reader.hpp` and the codec; oversized var-length `LEN` / list-map `COUNT`
rejection before allocating/looping; the `uint32_t`→`int` narrowing guard via a
`BitfieldBytes(int64_t)`; top-level full-buffer-consumption check in
`Codec::DecodeRow`; the same hardening in `core/positional_io.hpp`
(`PositionalReader`); map decode type fidelity (pass the schema's own map type);
first sparse-union round-trip test. `FixedSizeBinary` encode over-read was found
non-reachable (Arrow CHECK-validates width) and documented, not guarded.

**Standing constraint (carries into Phases 2/3):** `positional_io.hpp` and the
codec's `Reader` are two implementations of the same wire format and must stay
byte-compatible; keep them in lockstep. (The HARD round subsequently consolidated
`BitfieldBytes` into `core/detail/bitfield.hpp`, preserving the `int64_t`
overflow-safe intent — any further narrowing guard goes there.)

---

## Phase 2 — The generated-code layer (LANDED, round GIR)

The flat `FieldKind` model + hand-unrolled nested-list depth cases were replaced
with a **recursive, language-neutral Mapped-Type IR + recursive-visitor
emitters**. The defect this cured was not only "depth 2/3 is hand-unrolled" (true
only in the Arrow-scalar round-trip paths and the RBA accessor; several emitters
already looped over depth) — it was that `FieldMapping` was a **sum-type crammed
into a product-type** (six type-carrying members, most dead per kind), consumed by
a far larger emitter set than the original plan listed. `FieldMapping`/`FieldKind`
now survive only as a thin IR projection for the not-yet-migrated RBA accessor +
edge row-class emitters (see 2b; retired in round RIR).

### 2a. The IR is language-NEUTRAL, not C++-string-shaped

This was the single most important change, and the precondition for BIND. The old
`ScalarTypeInfo` carried literal C++ text (`arrow_type_expr="arrow::int32()"`,
`scalar_ctor="std::make_shared<arrow::Int32Scalar>(...)"`, `builder_type`). The
proof that this leaked: the **RBA Rust emitter reverse-engineers arrow-rs types by
string-parsing those C++ strings** (`recordbatch_accessor_emitter.cpp` re-parses
`arrow::timestamp(...)` to recover the time unit). BIND-5/6 (Rust + C# rows) would
have repeated that anti-pattern had the IR not been abstract.

- Each `Scalar` node carries an **abstract logical-type identity** — a
  `ScalarKind`/logical enum (`BOOL | INT8..64 | UINT8..64 | FLOAT16/32/64 | UTF8 |
  BINARY | FIXED_SIZE_BINARY(n) | DATE32/64 | TIMESTAMP{unit,tz} | TIME32/64{unit}
  | DURATION{unit} | DECIMAL{p,s} | INTERVAL{...}`), **not** a C++ type string.
  The C++-string tables become a **C++-backend** lookup keyed on that id; Rust,
  C#, and TS backends each own their own mapping table.
- The IR node grammar: `Scalar(kind, nullable, [enum-identity], [dict-modifier]) |
  List<T> | FixedSizeList<T,n> | Struct<fields> | Map<K,V> | Unsupported{reason}`,
  mirroring Arrow's `DataType` tree **and** carrying the language-neutral facts
  every backend needs: proto field number/name, source descriptor, optionality,
  WKT/logical distinctions (e.g. `Timestamp` stays "timestamp", wrappers stay
  "nullable scalar" — not collapsed to bare int64), per-field metadata, cross-file
  package/module references, and the unsupported reason. (The Arrow physical type
  and the TS `WireTypeId` are **derived per-backend** from the logical identity —
  they are **not** stored on the IR; see the landed note below.)
- **Add an `Enum` identity.** Enums collapse to `int32` at map time today with no
  identity downstream. The IR must preserve the enum descriptor/symbols (even if
  the C++ backend still lowers to int32), because #75 (enum-symbol emission) and
  BIND's idiomatic Rust/C# enums both need it. **#75 co-designs with this node.**
- **Model `Dictionary` as a scalar modifier, not a structural peer.** Per the DICT
  spec, a dictionary field stays `SCALAR`; only the schema visitor (and the RBA
  accessor) branch on it. Do **not** make it a `List`/`Struct`-style container in
  the grammar, or emitters will treat it as nesting.
- **Add `List<List<Scalar>>`.** Nested lists with a *scalar* leaf are unsupported
  today (`MapFlattenedRepeated` returns `nullopt`) — the `Unsupported{reason}`
  node fixes the `nullopt`-ambiguity by construction, and enabling scalar-leaf
  nested lists is a **new feature** (see the byte-identity scoping in Phase 3).
- Flatten resolution becomes IR construction (a chained-flatten wrapper yields
  nested `List(List(Struct))`; arbitrary **struct** depth falls out).
- **Folded in GEN/#75 by construction:** #55 (silent `// TODO`/`nullopt` for
  unsupported types) became the explicit `Unsupported{reason}` node → a clean
  build error; #75 became the `Enum` node. (GeoArrow CRS / #59 is **not** folded
  in — it is a **domain concern** owned by the Datamodel repo; Fletcher stays
  domain-unaware and the IR carries no CRS/geospatial metadata.)

**As landed (round GIR).** The IR shipped as `protoc/include/ir.hpp` +
`protoc/src/ir.cpp`. Concrete facts that outlive the round:

- **Single source of truth for optionality/dictionary.** `nullable` and
  `dictionary` live **only** in `IrNode.facts`; the node variants (`ScalarNode`,
  `ListNode`, `StructNode`, `MapNode`, `FixedSizeListNode`) never duplicate them,
  so the encode visitor and the IR→`FieldMapping` projection read from one home.
- **`LogicalType` carries the parameter bag**, not the backend: `LogicalKind` +
  `fixed_size_binary_width`, `time_unit`, `timezone`, `decimal_precision` /
  `decimal_scale`, and `interval_unit`. WKT stays distinct as its own
  `LogicalKind::WKT_TIMESTAMP` / `WKT_DURATION` and, for wrappers, a
  `WktKind::WRAPPER_*` marker (never collapsed to the bare scalar). An enum is
  `LogicalKind::INT32` + an `EnumIdentity` (descriptor + full name + symbol
  table), not a distinct kind.
- **The backend tables are concrete artifacts, keyed on `ir::LogicalType` (+
  optional `ir::EnumIdentity`):**
  - **C++** — `protoc/include/cpp_backend_type_table.{hpp,cpp}`, `CppScalarInfo`
    (adds `positional_write` / `positional_read`, `array_type`, `getter_type`, and
    a `value_is_buffer` bool over the old `ScalarTypeInfo`).
  - **TypeScript** — `ts_backend_type_table`, `TsScalarInfo{ts_type_text,
    wire_type_id}` (the first non-C++ backend table — GIR-7's language-neutrality
    proof-point).
  The **Arrow physical type** and the **TS `WireTypeId`** are **derived inside the
  backend** from `LogicalType` + `facts` (+ `enum_identity`), never stored on an IR
  node. BIND mirrors this shape for C#/Rust; round RIR's Rust accessor reuses
  BIND-Rust's table rather than building its own. *Landed in round GIR,
  2026-07-11.*

### 2b. Emitters as recursive visitors over the IR

Each visitor threads a context (target buffer var, current value expression,
indentation, fresh loop-var names) **plus backend-specific context and a helper
library** — the visitor framework is not a single generic string-recursion API
(RBA proves this: it emits validation, cached columns, borrowed spans, null
gates, and language-specific lifetimes). The **full** emitter set, with the
migrate/leave decision that was taken for each:

| Emitter | Disposition (as landed, round GIR) |
|---|---|
| Edge C++ encode/decode class (`Encode` / positional I/O) | **Migrated** (GIR-3 encode, GIR-4 decode) — the wire-critical path; encode byte-identical and decode value-identical under the GIR-2 oracles. |
| Arrow view + `ToArrowRow` | **Migrated** (GIR-6); the dead `SetFrom*` / `ToScalars` / `Make*Scalar_` helpers were deleted (0 callers, 0 generated hits). |
| nanoarrow schema emitter **+ its IPC-builder sibling** (`BuildMessageSchemaInto`/`BuildMessageSchema`) | **Migrated, UNIFIED** (GIR-5) — one IR schema-visitor renders C++ source on one path and executes nanoarrow in-process on the other, via a `SchemaSink` abstraction. This killed exactly the hand-kept-in-lockstep drift Phase 2 targeted; `.ipc` byte-compat held (`test_schema_builder.cpp` + 10 per-node-kind `.ipc` goldens green). |
| TS interface + descriptor | **Migrated** (GIR-7) — the round's language-neutrality proof-point (new `ts_backend` table, all TS strings off the IR). |
| **RBA C++ + Rust accessor** (`recordbatch_accessor_emitter.cpp`) | **Left read-only** — consumes a thin `FieldKind` projection of the IR; **not** rewritten (freshly merged, heavily tested, non-wire). Reconciled onto the IR in round **RIR**. Consequences: RBA keeps its depth-2/3 cap (arbitrary depth does not reach the accessor); GIR-10's scalar-leaf `List<List<scalar>>` is **rejected up front** for `accessor`/`rust` by `ValidateBackendsSupportFields` (see below); DICT-6 still patches dictionary reading into the flat accessor. |
| Publisher/subscriber **service** emitters | **IR-orthogonal** — they key on method/topic, not `FieldKind`; "one IR" did not amortize them (relevant to BIND-5/6's service helpers). |

- **`FieldKind` was not deleted at parity.** It stayed load-bearing for RBA (and
  DICT, when it lands). It survives as a **thin projection of the IR** through a
  long transition; the "delete the old switch at parity" step applied per-emitter,
  only to migrated emitters, and `FieldKind` itself retires only once RBA and the
  edge row-class emitters are reconciled onto the IR in round RIR.
- **Language-neutrality was proved by a non-C++ backend (GIR-7).** The TS emitter
  migrated onto a dedicated `ts_backend` table with all TS strings off the IR —
  the round's proof that 2a lets a second language be a visitor rather than a
  string-parse. (BIND's C#/Rust row emitters build on this; RBA's Rust *accessor*
  is reconciled later, in RIR, and was not the proving backend.)
- **#53-generated (closed, GIR-8).** The **9 emitter `.ValueOrDie()` sites** (7 in
  the Arrow view visitor + 2 in the generator's view constructors) — which produced
  **~26 `.ValueOrDie()` occurrences** across the generated view classes — now emit
  a checked helper `detail::FletcherValueOrThrow<T>()` (throws `std::runtime_error`
  on `!ok()`, `ValueUnsafe()` on `ok()` — value-identical) into the generated view
  headers. HARD did the runtime half; the generated half is fixed by-construction.
  A source guard (`check_no_value_or_die_in_emitters.cmake`) keeps every IR emitter
  TU `.ValueOrDie()`-free (RBA excluded — still read-only).
- **#55 (closed, GIR-8).** A front-end `ValidateNoUnsupportedIr()` /
  `FindUnsupportedIr` pass sets protoc `*error` on genuinely-unsupported **types**
  (`google.protobuf.Any`/`Struct`, real `oneof`, unsupported map key/value, bad
  flatten leaves) — a clean build error instead of a silent skip. Recursion stays
  **skipped / non-fatal** (unchanged); proto2 groups (→ INT32) are not flagged.

**As landed (round GIR) — cross-emitter machinery.**

- **Shared flatten walk `BuildFlattenedFieldList` (GIR-5).** A `FieldMapping`-free,
  language-neutral walk (in `cpp_backend_schema_visitor.{hpp,cpp}`) that mirrors
  `GatherFieldsImpl`: it builds the dotted `field_id` path and the leaf
  `field_number`, filters `UNSUPPORTED` and `FIXED_SIZE_LIST` nodes, and carries an
  optional `source_field` so a flattened wrapper's declared name can be recovered
  (needed by the TS visitor for `IStructListWrapper[]`). It is shared by **both**
  the schema visitor **and** the TS visitor — there is no second field walk.
- **`TopologicalVisit` enum-owner ordering edge + cycle guard (GIR-9).** A field
  typed as a *nested* enum forces its owning message to emit first (a nested enum
  cannot be forward-declared apart from its owner), even with no `TYPE_MESSAGE`
  edge between them. A shared in-progress `visiting` set guards against enum-owner
  cycles so protoc terminates deterministically rather than recursing forever (a
  genuine mutual-nested-enum cycle is inherently un-orderable in C++).
- **`nested_leaf_is_scalar` discriminator (GIR-10).** Scalar-leaf nested lists reach
  the `FieldKind`-consuming emitters by reusing `FieldKind::NESTED_LIST` plus an
  additive language-neutral `nested_leaf_is_scalar` bool — **no new enum value**, so
  the read-only RBA switch was untouched. Round RIR must carry this
  scalar-vs-struct-leaf distinction when it retires `FieldKind`.
- **Bridge end-state.** After GIR-7, **encode + decode + schema/IPC + view + TS are
  ALL direct IR emitters.** `FieldMapping`/`FieldKind` now serve **only** the RBA
  accessor emitter **and** the edge row-class setters/getters — the bridge fully
  retires in round RIR.
- **Scalar-leaf nested-list backend guard (GIR-10).** GIR-10 enabled scalar-leaf
  `List<List<scalar>>` on every backend **except** RBA. A front-end guard
  `ValidateBackendsSupportFields` / `FindScalarLeafNestedList` fails the plugin with
  a clear error if `--fletcher_opt=accessor,rust` is requested for a proto that
  contains such a field (the RBA C++/Rust emitters assume a struct leaf). Until RIR
  lifts the cap and removes the guard, such protos must omit `accessor,rust`.
  *Landed in round GIR, 2026-07-11.*

### 2c. Edge codec strategy — bespoke edge kept; descriptor-driven codec is BIND-2 scope

GIR kept emitting **bespoke recursive C++** for the **edge tier** (zero-Arrow-
dependency, typed classes over `positional_io` — required for MCU/device targets)
and migrated that path onto the IR (GIR-3/4). GIR did **not** build a
descriptor-driven codec — that remains **BIND-2 scope**, and was deliberately
**not foreclosed** (locked decision #8): **BIND-2 wraps "generated encode/decode
through a schema/descriptor-handle model,"** which is closer to a generic codec
than N bespoke encoders the C ABI must each wrap. The IR schema-visitor leaves a
clean path to *additionally* emit such a codec for the ABI surface BIND-2 needs;
that choice shapes BIND-2's ABI and is taken there, with the bespoke edge path
unaffected.

### Hard invariant + de-risking

- **Hard invariant: the WIRE format stays byte-identical** (the runtime `Codec`
  and released components are the contract), guarded by Phase 3b. Note the
  distinction: generated **source** bytes *will* change (that's the point) —
  goldens (incl. the RBA no-drift golden) are re-baselined under review; the
  golden's *purpose* survives, its *baseline* moves. "Same bytes" is the **wire**
  invariant, scoped to inputs the flat generator already supported (nested
  scalar-lists are new — no anchor; they get fresh round-trip tests).
- **De-risking:** IR + edge-encode vertical slice first, behind the 3a/3b guards;
  migrate emitter-by-emitter with the old path kept until each new one passes its
  **per-emitter oracle** (below); a temporary IR→`FieldMapping` adapter is allowed
  only as a migration bridge (RBA/IPC must not consume it permanently).

---

## Phase 3 — Comprehensive test coverage (LANDED, round GIR; landed before/with Phase 2)

The harness + oracles landed **before** the Phase 2 rewrite, so it was
test-guarded throughout. **Critical path honored: 3a → 3b → Phase 2.**

- **3a. Generator compile-and-run harness — GREENFIELD (GIR-1).** There had been no
  such harness (`test_package` only `cat`/`md5`'d output; the only generator tests
  were `test_type_mapper.cpp` + `test_schema_builder.cpp`). GIR-1 built the first
  harness that runs the plugin on a `coverage.proto` (every type / WKT / enum /
  nesting depth / flatten variant / service) and **actually compiles and executes**
  the generated output: C++ edge header, Arrow C++ view + `ToArrowRow`, TS (`tsc
  --noEmit`), the IPC schema output, and the RBA C++ accessor + Rust accessor
  crate — then builds a row and round-trips it.
- **3b. Parity oracles (the guard).**
  - **Encode:** `Encode() == EncodeRow()` across all protos (nulls set/unset,
    empty vs non-empty containers). This pins the exact wire bytes Phase 2
    perturbs against an oracle Phase 2 does not touch (the two encoders are
    genuinely independent).
  - **Decode (do not skip — byte-identity guards encode only):** a **round-trip
    value-equality oracle** (`encode → decode → Equals`) plus decode-of-known-
    golden-bytes; read back map keys/values and struct inner fields (never
    verified today).
  - **Per-emitter cutover oracle** (bind each to its emitter before deleting the
    old switch): edge encode → `Encode==EncodeRow`; decode → round-trip equality;
    schema + IPC → `test_schema_builder.cpp` + `.ipc` golden; TS → `tsc --noEmit`
    + descriptor golden; view/`ToArrowRow` → round-trip via the codec.
  - **No-drift across all output tokens:** baseline, `schema_only`, `ts`, `ipc`,
    `accessor`, `rust`, and combinations (RBA already requires existing bytes
    unchanged with/without the accessor flags).
- **3c. Codec edge/safety/boundary suite:** untested type families (sparse/dense
  union, intervals, time32/64, duration, decimals incl. negative, large/view,
  fixed-size-binary); boundaries (`INT*_MIN`, `UINT*_MAX`, NaN/±Inf/-0.0
  bit-compared, empty string, embedded NULs, multi-byte UTF-8); empty containers;
  wide null-bitfields (≥9 fields/elements, high-index nulls); all error/throw
  paths; encode→decode→encode determinism.
- **3d. Flatten + arbitrary-nesting** unit tests at the IR level (incl. the newly
  supported `List<List<scalar>>`), and schema-evolution **negative** tests (v1
  buffer decoded as v2 is detected, not silently corrupted).
- **3e. Property/fuzz testing:** a round-trip property test (random Arrow rows →
  encode → decode → `Equals`) and a `DecodeRow` fuzz harness over random /
  truncated buffers (exercises the Phase-1 safety fixes).

---

## Sequencing (relative to HARD, GEN/#75, DICT, BIND)

The order below was honored; HARD and round GIR have both landed.

1. **HARD** (runtime/pubsub hardening — orthogonal to the generator) merged; it
   neither blocked nor was blocked by Phase 2.
2. **Phase 3a harness → 3b oracles first** (GIR-1/2). Wire-format-defect GEN fixes
   landed **with** the GIR-2 baseline (so the oracle captured the *correct* bytes —
   e.g. the nullable-`bytes` `WriteBinary` corruption caught and fixed in GIR-2);
   structural / source-only GEN fixes were folded into Phase 2
   (fix-by-construction).
3. **The language-neutral IR (2a)** designed with #75 (Enum node) co-designed and
   BIND-5/6 + DICT needs consulted before writing emitters (GIR-3).
4. **Phase 2 rewrite (2b)** emitter-by-emitter behind per-emitter oracles; schema
   +IPC unified; GEN generator-behaviour fixes and #53-generated folded in; RBA
   left read-only (GIR-3..10).
5. **BIND:** BIND-1..4 (ABI + Rust/C# runtime crates) may run **in parallel** with
   the follow-on rounds (they are not generator work). **BIND-5/6 (generated
   Rust/C# rows + services) come strictly after this IR** — now unblocked. BIND-8
   (Arrow IPC exchange) is de-risked by the GIR-5 schema+IPC unification. Chain:
   **GIR → BIND-C# → BIND-Rust → RIR**.

**DICT ordering — SETTLED.** The question ("DICT before or after Phase 2") is
resolved by outcome: **round GIR shipped without DICT**, so **DICT builds on the
IR** (after). The IR already models a dictionary as a scalar modifier
(`facts.dictionary`, locked #7); DICT emits schema through the unified IR schema
visitor, and DICT-6 patches the still-read-only flat RBA accessor (reconciled in
RIR).

**GEN / #75 — DONE.** Per the plan's own throwaway-work rationale ("fixing the
generator on the flat model is throwaway because Phase 2 rewrites that layer"),
#55, #53-generated, and #75 were done **as part of Phase 2** (GIR-8/9) on the IR
emitters, not on the flat model first. All three are closed.

---

## Review provenance

This plan was refreshed 2026-07-10 after Phase 1 merged (#98) and `main` advanced
(#96 IPC builder, #106 RBA emitter), incorporating two independent external
reviews (Codex + Claude) commissioned as preparation for the BIND round. Both
endorsed the original design's direction — especially the byte-identity-first
discipline — and converged on the updates above; the one divergence (migrate the
RBA emitter onto the IR in Phase 2 vs. leave it read-only) was resolved in favour
of **leaving RBA read-only** and reconciling it later, to avoid destabilising a
freshly-merged, heavily-tested, non-wire surface.

Consolidated **2026-07-11 at the close of round GIR**: the LANDED end-state
(GIR-1..GIR-11) was migrated into this spec from the now-archived per-item
execution artifacts (`plans/GIR-*`), so the architecture and reference survive
their archival. The one divergence resolved as predicted — RBA stayed read-only
and its reconciliation onto the IR is round **RIR**.
