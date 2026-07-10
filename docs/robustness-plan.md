# Robustness Improvements — Execution Plan

Tracking doc for the generator-hardening work-stream. Goal: bring the
generated-code layer and the full encode→decode chain up to the test-coverage
and correctness bar of a mature library (the standard we hold Apache Arrow to).

> **Status (refreshed 2026-07-10).** **Phase 1 is done and merged to `main` as
> PR #98** (codec / positional-I/O decode hardening vs malformed input). The
> remaining work is **Phase 2** (generator IR rewrite) and **Phase 3** (generator
> + encode→decode test coverage), neither implemented. This refresh reflects an
> external design review (Codex + independent Claude) after `main` advanced past
> the original plan — **#96** added an in-process/IPC schema builder and **#106
> (RBA)** added a ~2,590-line RecordBatch-accessor emitter (C++ **and** Rust) — and
> with the knowledge that the **BIND** (Rust/C# bindings) round will be built on
> top of Phase 2. The local `feature/robustness_improvements` branch is stale
> (its Phase-1 content landed via #98); do not revive it — resume Phase 2/3 on a
> fresh branch off `main` (after the HARD runtime-hardening PRs land).

## Branch / PR strategy

- Rebased on `main`, not merged (repo convention). Phase 2 is now much larger
  than first scoped (it must account for the IPC builder and the RBA emitter, and
  make the IR usable by future language backends), so split it:
  - **PR A — Phase 3a harness (greenfield).** There is **no** compile-and-run
    harness today (`protoc/test_package` only `cat`s/`md5`s generated output); this
    PR builds the first one that compiles and *executes* generated code.
  - **PR B — Phase 3b oracles.** `Encode()==EncodeRow()` byte-identity **and** a
    decode round-trip value-equality oracle. Must land **before** the rewrite.
  - **PR C — Phase 2 IR + edge encode/decode** (guarded by A/B).
  - **PR D — Phase 2 remaining emitters:** unified schema+IPC visitor, Arrow
    view/`ToArrowRow`, TS.
  - RBA and BIND emitters are handled outside this work-stream (see Sequencing).
- **Dependency edge: Phase 2 (IR) → BIND-5/6** (generated Rust/C# rows/services).
  Starting BIND's language emitters before the IR exists forces writing them
  against the flat model and rewriting them immediately.

## Three phases

1. **Phase 1 — DONE (#98).** Runtime codec / positional-I/O decode hardening vs
   malformed input. Historical detail retained below; its byte-compatibility
   invariant remains a standing constraint on Phases 2/3.
2. **Phase 2 — TODO.** Refactor the protoc generator onto a **recursive,
   language-neutral type IR + recursive-visitor emitters**.
3. **Phase 3 — TODO (lands before/with Phase 2).** Rigorously test the
   generated-code layer and the entire encode→decode chain; the harness + oracles
   guard the Phase 2 rewrite.

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

## Phase 2 — Refactor the generated-code layer

Replace the flat `FieldKind` model + hand-unrolled nested-list depth cases with a
**recursive, language-neutral Mapped-Type IR + recursive-visitor emitters**. The
real defect is not only "depth 2/3 is hand-unrolled" (that is true only in the
Arrow-scalar round-trip paths and the RBA accessor; several emitters already loop
over depth) — it is that `FieldMapping` is a **sum-type crammed into a
product-type** (six type-carrying members, most dead per kind), consumed by a far
larger emitter set than the original plan listed.

### 2a. The IR must be language-NEUTRAL, not C++-string-shaped

This is the single most important change, and the precondition for BIND. Today
`ScalarTypeInfo` carries literal C++ text (`arrow_type_expr="arrow::int32()"`,
`scalar_ctor="std::make_shared<arrow::Int32Scalar>(...)"`, `builder_type`). The
proof that this leaks: the **RBA Rust emitter reverse-engineers arrow-rs types by
string-parsing those C++ strings** (`recordbatch_accessor_emitter.cpp` re-parses
`arrow::timestamp(...)` to recover the time unit). BIND-5/6 (Rust + C# rows) will
repeat that anti-pattern unless the IR is abstract.

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
  "nullable scalar" — not collapsed to bare int64), Arrow physical type, TS
  `WireTypeId`, per-field metadata, cross-file package/module references, and the
  unsupported reason.
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
- **Folds in GEN/#75 by construction:** #55 (silent `// TODO`/`nullopt` for
  unsupported types) becomes the explicit `Unsupported{reason}` node → a clean
  build error; #75 becomes the `Enum` node; #59 (GeoArrow CRS) becomes part of the
  IR **metadata** model (implementation may follow, but do not hard-code CRS into
  old string paths first).

### 2b. Emitters as recursive visitors over the IR

Each visitor threads a context (target buffer var, current value expression,
indentation, fresh loop-var names) **plus backend-specific context and a helper
library** — the visitor framework is not a single generic string-recursion API
(RBA proves this: it emits validation, cached columns, borrowed spans, null
gates, and language-specific lifetimes). Enumerate the **full** emitter set and
take an explicit migrate/leave decision per emitter:

| Emitter | Phase 2 disposition |
|---|---|
| Edge C++ encode/decode class (`Encode`/`SetFrom*`, positional I/O) | **Migrate** (the wire-critical path; guarded by 3b) |
| Arrow view + `ToArrowRow` | **Migrate** |
| nanoarrow schema emitter **+ its IPC-builder sibling** (`BuildMessageSchemaInto`/`BuildMessageSchema`) | **Migrate, UNIFIED** — one IR schema-visitor renders C++ source on one path and executes nanoarrow in-process on the other. Their headers already say they must be kept "in lockstep"; unifying kills exactly the drift Phase 2 targets. Guard `.ipc` byte-compat (`test_schema_builder.cpp` stays green). |
| TS interface + descriptor | **Migrate** |
| **RBA C++ + Rust accessor** (`recordbatch_accessor_emitter.cpp`) | **LEAVE read-only** — consumes a thin `FieldKind` projection of the IR; **not** rewritten in Phase 2 (freshly merged, heavily tested, non-wire). Reconcile onto the IR in a later, separate step. Consequences to state: RBA keeps its depth-2/3 cap (arbitrary depth does not reach the accessor in Phase 2), and DICT-6 still patches dictionary reading into the flat accessor. |
| Publisher/subscriber **service** emitters | **IR-orthogonal** — they key on method/topic, not `FieldKind`; "one IR" does not amortize them (relevant to BIND-5/6's service helpers). |

- **`FieldKind` is not deleted at Phase-2 parity.** It is now load-bearing for RBA
  (and DICT, if it lands). It survives as a **thin projection of the IR** through a
  long transition; the "delete the old switch at parity" step applies per-emitter,
  only to migrated emitters, and `FieldKind` itself is removed only once RBA is
  later reconciled.
- **Prove language-neutrality with a non-C++ backend early.** The first new
  IR backend is BIND's Rust **row** emitter (or a throwaway Rust spike in Phase 2)
  — this is what validates that 2a actually lets a second language be added as a
  visitor rather than by string-parsing. (RBA's Rust *accessor* is reconciled
  later; it is not the Phase-2 proving backend.)
- **Fold #53-generated:** the 25 generator-emitted `.ValueOrDie()` sites become a
  checked-result helper in the **new emitter conventions** (HARD did the runtime
  half; the generated half is fixed by-construction in the rewritten emitters).

### 2c. Edge codec strategy — decide with BIND-2 in view

Keep emitting **bespoke recursive C++** for the **edge tier** (zero-Arrow-
dependency, typed classes over `positional_io` — required for MCU/device targets).
But do **not** defer the runtime-codec question: **BIND-2 wraps "generated
encode/decode through a schema/descriptor-handle model,"** which is closer to a
generic codec than N bespoke encoders the C ABI must each wrap. Take a position:
the IR schema-visitor can emit **both** — bespoke recursive C++ for the edge, and
a **descriptor-driven codec** for the ABI surface BIND-2 needs. This choice shapes
BIND-2's ABI, so make it in Phase 2, not "as a possible follow-up."

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

## Phase 3 — Comprehensive test coverage (lands before/with Phase 2)

The harness + oracles must land **before** the Phase 2 rewrite so it is
test-guarded. **Critical path: 3a → 3b → Phase 2.**

- **3a. Generator compile-and-run harness — GREENFIELD.** There is no such harness
  today (`test_package` only `cat`s/`md5`s output; the only generator tests are
  `test_type_mapper.cpp` + `test_schema_builder.cpp`). Build the first harness
  that runs the plugin on a `coverage.proto` (every type / WKT / enum / nesting
  depth / flatten variant / service) and **actually compiles and executes** the
  generated output: C++ edge header, Arrow C++ view + `ToArrowRow`, TS (`tsc
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

1. **Merge HARD** (runtime/pubsub hardening — orthogonal to the generator; the
   7 stacked PRs). It neither blocks nor is blocked by Phase 2.
2. **Phase 3a harness → 3b oracles.** Land any GEN fix that is a **wire-format**
   defect here (so the byte-identity baseline captures the *correct* bytes, not
   buggy ones). Triage each GEN item (#55, #59, #53-generated) by the deciding
   question: *does it corrupt bytes that ship today?* If yes → fix before/with 3b.
   If it is structural/schema/source-only and reachable via code Phase 2 rewrites
   → fold into Phase 2 (fix-by-construction).
3. **Design the language-neutral IR (2a)** — co-designing #75 (Enum node) and
   consulting BIND-5/6 + DICT needs before writing emitters.
4. **Phase 2 rewrite (2b)** emitter-by-emitter behind per-emitter oracles; unify
   schema+IPC; fold in GEN generator-behaviour fixes and #53-generated; leave RBA
   read-only.
5. **BIND:** BIND-1..4 (ABI + Rust/C# runtime crates) may run **in parallel** with
   steps 2–4 (they are not generator work). **BIND-5/6 (generated Rust/C# rows +
   services) come strictly after Phase 2.** BIND-8 (Arrow IPC exchange) is
   de-risked by the 2b schema+IPC unification.

**DICT ordering (record an explicit call):** DICT emits schema on the flat
generator and DICT-6 patches the flat RBA accessor. By the throwaway-work
principle, DICT-schema-emission is a candidate to fold into Phase 2; at minimum
decide whether DICT lands **before** Phase 2 (then Phase 2 migrates `is_dictionary`
into the IR) or **after** (built on the IR directly). Left unstated, DICT and
Phase 2 collide on the same schema emitters.

**GEN / #75:** by the plan's own Phase-1 rationale ("fixing the generator now is
throwaway work because Phase 2 rewrites that layer"), do GEN generator-behaviour
fixes and #75 **as part of Phase 2**, not on the flat model first.

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
