# GIR — Generator IR Rewrite (Robustness Phase 2/3) — Execution Plan

Round plan + tracker for the generator rewrite onto a recursive, language-neutral
type IR + recursive-visitor emitters, plus the encode→decode test coverage that
guards it. This is the unfinished **robustness Phase 2 + Phase 3** (Phase 1
merged as #98).

Spec: [docs/robustness-plan.md](../docs/robustness-plan.md) (the refreshed
Phase-2/3 design — read it first).
Locked decisions: [GIR-locked-decisions.md](GIR-locked-decisions.md).
This file is both the `plan_path` (tracker) and the `user_stories_path`.

## Goal

Replace the flat `FieldKind` model (a sum-type crammed into a product-type) and
the hand-unrolled nested-list depth cases with **one recursive, language-neutral
Mapped-Type IR** that every generator emitter visits — so a second language
backend is added as a visitor, not by string-parsing C++ type text. The wire
format stays **byte-identical** (hard invariant); the rewrite is guarded by a
compile-and-run harness + encode/decode parity oracles that land first. This is
the foundation BIND's Rust/C# row emitters (BIND-5/6) build on.

## Branch strategy

- **GIR-1..GIR-11 (closed 2026-07-11)** were based on `main` at `5b36534`.
- **GIR-13 bases on the `hard/3-7-consolidated` branch (PR #124), NOT on
  `main`.** This is a locked base — see below.
- The stale `feature/robustness_improvements` branch is **not** revived (its
  Phase-1 content is on `main` via #98).
- Rebased, not merged (repo convention) — onto the base named per item above. PR
  split (each independently reviewable): GIR-1/2 (harness+oracles) → GIR-3/4 (IR +
  edge codec) → GIR-5..7 (schema/IPC, view, TS) → GIR-8/9 (generator-behaviour +
  enum) → GIR-10/11 (coverage) → GIR-13 (option metadata). No PR until green +
  reviewed; PR/merge is the user's step.

### GIR-13 base — `hard/3-7-consolidated`, not `main`

Rebase `feature/generator-ir-rewrite` onto **`hard/3-7-consolidated`** and do
GIR-13 there. Do **not** wait for PR #124 to merge, and do **not** base GIR-13 on
`main`.

Why this is safe — verified 2026-08-26:

- **Zero file overlap.** The set of files HARD-3..7 changes and the set GIR
  changes are disjoint (`comm -12` over both diffs returns nothing). HARD touches
  `core/`, `pubsub/`, the two providers, and `arrow-bridge/src/`; GIR touches
  `protoc/` plus `arrow-bridge/include/.../arrow_row_view.hpp` and
  `arrow-bridge/tests/`.
- **HARD contributes no conflicts.** Trial-merging GIR onto
  `hard/3-7-consolidated` conflicts in exactly the same four files as merging onto
  `main` — `generator_internal.hpp`, `schema_builder.hpp`, `generator.cpp`,
  `tests/CMakeLists.txt` — all of them #121, i.e. GIR-13's own work.
- `hard/3-7-consolidated` is a pure fast-forward from `main`, so this base
  includes everything on `main` (#111, #121, #122) as well as HARD-3..7.

**Consequence to accept:** until #124 merges, a PR from this branch carries
the nine HARD-3..7 commits in its diff as well as GIR's. Once #124 lands, they are already
in `main` and the GIR PR shows only GIR's changes. If #124's scope changes in
review, rebase this branch onto whatever supersedes it — not onto bare `main`,
which would drop the HARD fixes GIR-13 is being built against.

## Sequencing

Strictly linear; each item's forcing test 🟢 before the next. **The harness +
oracles (GIR-1, GIR-2) MUST precede any IR work** — they are the guard.

```
GIR-1  Phase 3a harness (greenfield)   →  GIR-2  Phase 3b parity oracles       →
GIR-3  language-neutral IR + edge encode →  GIR-4  edge decode on the IR        →
GIR-5  unified schema+IPC visitor      →  GIR-6  Arrow view + ToArrowRow        →
GIR-7  TS interface + descriptor       →  GIR-8  #55 + #53-generated (errors)   →
GIR-9  #75 enum symbols                →  GIR-10 codec edge/boundary + nesting  →
GIR-11 property/fuzz
```

GIR-13 was added after the round closed (see the tracker) and is sequenced last,
on its own base — it depends on nothing in GIR-1..GIR-11 beyond the IR schema
visitor GIR-5 built.

---

## Work-item tracker

Status: ⚪ not-started · 🔴 in-progress · 🟢 done (forcing test green + reviewed)
Kind: 🟩 test-guard · 🟦 IR/emitter migration (byte-identity-guarded) · 🟨 feature/fix · 🧪 coverage

| Item | Title | Kind | Forcing test | Status |
|------|-------|------|--------------|--------|
| GIR-1 | Phase 3a: generator compile-and-run harness | 🟩 | `CoverageHarness.GeneratedCppCompilesEncodesAndReconstructs` | 🟢 |
| GIR-2 | Phase 3b: encode byte-identity + decode round-trip oracles | 🟩 | `ParityOracle.EncodeEqualsEncodeRowAndRoundTrips` | 🟢 |
| GIR-3 | Language-neutral IR + edge **encode** vertical slice | 🟦 | `IrTest.BuildsLanguageNeutralIr` + GIR-2 encode oracle stays green | 🟢 |
| GIR-4 | Edge **decode** emitter on the IR | 🟦 | `CoverageHarness.GeneratedCppCompilesEncodesAndReconstructs` (all fixtures) stays green — IR-driven edge decoder | 🟢 |
| GIR-5 | Unified schema + IPC visitor (one IR schema-visitor) | 🟦 | `SchemaVisitor.CppAndIpcByteIdentical` (+ `test_schema_builder` green) | 🟢 |
| GIR-6 | Arrow view + `ToArrowRow` on the IR | 🟦 | `ViewVisitor.RoundTripsViaCodec` | 🟢 |
| GIR-7 | TS interface + descriptor on the IR | 🟦 | `TsVisitor.DescriptorByteIdentical` (+ `tsc --noEmit`) | 🟢 |
| GIR-8 | #55 unsupported→build error + #53-generated no-`ValueOrDie` | 🟨 | `GenErrors.UnsupportedTypeFailsBuild` (+ Repeated/Map variants) + `GenErrors.NoValueOrDieInEmitterSources`/`…InIrGeneratedCode` | 🟢 |
| GIR-9 | #75 emit C++ enum symbols (typed accessors) | 🟨 | `EnumEmit.GeneratedEnumSymbolsRoundTrip` | 🟢 |
| GIR-10 | Codec edge/boundary + flatten/arbitrary-nesting coverage (3c/3d) | 🧪 | `CodecEdge.*` + `Nesting.ListOfListOfScalarRoundTrips` | 🟢 |
| GIR-11 | Property + fuzz (3e) | 🧪 | `Fuzz.DecodeRowSurvivesRandomTruncatedBuffers` + round-trip property | 🟢 |
| GIR-13 | #121 option metadata on the IR schema visitor | 🟨 | § 6 `SchemaVisitor.CppSinkEscapesResolverBytes…` (T1) + `…NestedStructGrandchildren…` (T2) + `…TwoLevelFlattenFieldChain…` (T3) + `GeneratorMetadataPlumbing.*`, with the `MetadataOptions`/`MetadataNoDrift`/`IpcParity` set. (`OptionMetadataTest.FlattenFieldWrapperContextReachesEachInlinedLeaf` was the nominal forcing test but goes green on the CMake wiring line alone — see the progress log.) | 🟢 |

Suite shape: new protoc unit TU group (`test_ir.cpp`, lands GIR-3); the compile-and-run
integration harness **landed at GIR-1** in `integration-tests/protoc-coverage/`
(`coverage.proto` + `coverage_future.proto`; ctest targets `coverage_harness_tests`
+ `coverage_accessor_tests`; tsc/rustc checks Skip when the toolchain is absent);
the Rust crate (`integration-tests/protoc-gen-fletcher-rust`) stays green (RBA
no-drift). GIR-13 adds `GeneratorMetadataPlumbing.*` (3) + T1–T4 to `test_schema_visitor.cpp` and wires the pre-existing 33-test `test_option_metadata.cpp` into `protoc/tests/CMakeLists.txt`. Both wired into the config's inner-loop/full-suite commands.

---

## Items (user stories + acceptance)

> Design detail for each item lives in [docs/robustness-plan.md](../docs/robustness-plan.md)
> §Phase-2/§Phase-3; the design step expands per-item design docs from it.

### GIR-1 — Phase 3a: compile-and-run harness (greenfield)
**Story.** As a generator maintainer I have a harness that runs the plugin on a
`coverage.proto` (every type / WKT / enum / nesting depth / flatten variant /
service) and **actually compiles and executes** the generated output — C++ edge
header, Arrow view + `ToArrowRow`, TS (`tsc --noEmit`), IPC schema, RBA C++
accessor + Rust accessor crate — then builds a row and reconstructs it.
**Forcing test.** `CoverageHarness.GeneratedCppCompilesEncodesAndReconstructs`:
red today (nothing compiles/runs generated code — `test_package` only `cat`s/`md5`s).
**Acceptance.** Robustness-plan §3a. This is a **hard prerequisite** for GIR-2 and
everything after.

### GIR-2 — Phase 3b: parity oracles (the guard)
**Story.** Before any rewrite, the wire contract is pinned. **Encode:**
`Encode() == EncodeRow()` across all coverage protos (nulls set/unset, empty vs
non-empty containers). **Decode:** a round-trip value-equality oracle
(`encode → decode → Equals`) + decode-of-known-golden-bytes; read back map
keys/values + struct inner fields (never verified today).
**Forcing test.** `ParityOracle.EncodeEqualsEncodeRowAndRoundTrips`.
**Acceptance.** Robustness-plan §3b. Byte-identity guards encode; the round-trip
oracle guards decode. Must be green before GIR-3.

### GIR-3 — Language-neutral IR + edge encode (vertical slice)
**Story.** `type_mapper` builds a recursive **language-neutral** IR
(`Scalar(logical-kind, nullable, [enum-identity], [dict-modifier]) | List<T> |
FixedSizeList<T,n> | Struct | Map<K,V> | Unsupported{reason}`) carrying abstract
logical-type identity + descriptor/optionality/WKT/metadata/wire-id facts (no C++
type strings in the IR); the edge C++ **encode** emitter becomes a recursive
visitor over it, with the C++-string tables moved to a **C++-backend** lookup
keyed on the logical id.
**Forcing test.** `IrTest.BuildsLanguageNeutralIr` (IR unit tests incl. enum
identity preserved, WKT logical distinctions, `Unsupported{reason}` for the
previously-`nullopt` cases) **and** GIR-2's encode byte-identity oracle stays
green with the IR-driven encoder.
**Acceptance.** Robustness-plan §2a + the encode row of §2b. Old path kept behind;
temporary IR→`FieldMapping` adapter allowed as a bridge only.

### GIR-4 — Edge decode on the IR
**Story.** The edge C++ **decode** emitter (`SetFrom*`/extraction) becomes an IR
visitor. **Forcing test.** GIR-2's decode round-trip oracle stays green for the
IR-driven decoder. **Acceptance.** §2b decode.

### GIR-5 — Unified schema + IPC visitor
**Story.** The nanoarrow schema emitter **and** its in-process/IPC sibling
(`BuildMessageSchemaInto`/`BuildMessageSchema`) are emitted from **one** IR
schema-visitor — one path renders C++ source, the other executes nanoarrow
in-process — killing the hand-maintained lockstep.
**Forcing test.** `SchemaVisitor.CppAndIpcByteIdentical` + `test_schema_builder`
green + `.ipc` byte-compat. **Acceptance.** §2b (schema+IPC), robustness-plan A2.

### GIR-6 — Arrow view + `ToArrowRow` on the IR
**Story.** The Arrow view getters + `ToArrowRow` become IR visitors.
**Forcing test.** `ViewVisitor.RoundTripsViaCodec`. **Acceptance.** §2b (view).

### GIR-7 — TS interface + descriptor on the IR
**Story.** The TS interface + `SchemaDescriptor` become an IR visitor with a
**TS-backend** logical→`WireTypeId` table.
**Forcing test.** `TsVisitor.DescriptorByteIdentical` + `tsc --noEmit`.
**Acceptance.** §2b (TS).

### GIR-8 — #55 unsupported→build error + #53-generated no-`ValueOrDie`
**Story.** An unsupported proto→Arrow mapping becomes a clean protoc **build
error** (the `Unsupported{reason}` node → `AddError`/`*error`), not a silent
`// TODO`; the generator-emitted `.ValueOrDie()` sites become a checked-result
helper in the new emitter conventions.
**Forcing test.** `GenErrors.UnsupportedTypeFailsBuild` (a proto with an
unsupported type fails the plugin with a clear message) + a grep/build assertion
that generated code contains **zero** `.ValueOrDie()`. **Acceptance.**
robustness-plan §2a/§2b; closes #55 + the generated half of #53.

### GIR-9 — #75 emit C++ enum symbols
**Story.** Every proto `enum` emits a C++ enum (per #75's design — `enum class :
int32_t` + typed accessors) from the `Enum` IR node, package-scope + nested.
**Forcing test.** `EnumEmit.GeneratedEnumSymbolsRoundTrip` (typed accessor returns
the matching enumerator; wire byte-identical). **Acceptance.** closes #75; storage
stays int32 (no wire change).

### GIR-10 — Codec edge/boundary + arbitrary-nesting coverage (3c/3d)
**Story.** Fill the codec test gaps: untested type families (sparse/dense union,
intervals, time32/64, duration, decimals incl. negative, large/view,
fixed-size-binary); boundaries (`INT*_MIN`/`UINT*_MAX`, NaN/±Inf/-0.0, embedded
NULs, multi-byte UTF-8); wide null-bitfields; the newly-supported
`List<List<scalar>>`; schema-evolution negative tests.
**Forcing test.** `CodecEdge.*` + `Nesting.ListOfListOfScalarRoundTrips` (may find
real bugs → red-first; otherwise regression guards). **Acceptance.** §3c/§3d.

### GIR-11 — Property + fuzz (3e)
**Story.** A round-trip property test (random Arrow rows → encode → decode →
`Equals`) + a `DecodeRow` fuzz harness over random/truncated buffers.
**Forcing test.** `Fuzz.DecodeRowSurvivesRandomTruncatedBuffers` + the property
test. **Acceptance.** §3e; exercises the Phase-1 safety fixes.

> **GeoArrow CRS (#59) is out of scope for GIR — and for Fletcher.** CRS /
> GeoArrow extension metadata is a **domain concern**; Fletcher is domain-unaware
> and the Datamodel repo owns it. #59 is not a generator item here; the IR carries
> no CRS/geospatial metadata. (Removed 2026-07-10 per maintainer directive.)

### GIR-13 — #121 option metadata on the IR schema visitor

**Story.** As a user who maps custom proto options into Arrow schema metadata via
`--fletcher_opt=metadata_from_option=...`, the feature works exactly as specified
in [docs/fletcher-options.md](../docs/fletcher-options.md) when the schema is
rendered by the IR schema-visitor — including an annotation declared on a
`(fletcher.flatten_field)` wrapper reaching each leaf inlined through it, and an
option value containing arbitrary bytes producing a valid generated header.

**Why it exists.** #121 landed on `main` on 2026-08-04, *after* this branch cut at
`5b36534`. It threads an `OptionMetadataResolver` through the **flat** generator —
the machinery GIR-5 replaced with one IR visitor + two sinks. The resolver must be
re-threaded into the visitor. This is the sole blocker on merging the round; it
also makes #121's "both paths consume the identical pair vector" invariant
structural rather than hand-maintained.

**Scope.** Re-add the `metadata_from_option` option plumbing (absent on this
branch); restore the `BuildMessageSchema` resolver parameter dropped by GIR-5;
carry the flatten chain through the visitor's own flatten walk
(`BuildFlattenedFieldListImpl` currently discards the wrapper, so `ForField`'s
chain argument is unreachable); apply `EscapeCppStringLiteral` in
`CppSchemaSink::SetMetadata`, which today writes metadata values raw; preserve
builtins-first/extras-appended ordering at both call sites.
`option_metadata.{hpp,cpp}` port across **unchanged**.
Full design: [GIR-13-option-metadata-on-ir.md](GIR-13-option-metadata-on-ir.md).

**Forcing test.** `OptionMetadataTest.FlattenFieldWrapperContextReachesEachInlinedLeaf`
— red while the flatten chain is dropped. The forcing tests for this item
**already exist on `main`**; the item ports #121's suites and makes them pass
against the IR emitter rather than authoring new ones.

**Acceptance.** All 32 `test_option_metadata` tests green (incl. the 8
`EscapeCppStringLiteralTest` cases), #121's 3 integration TUs green,
`SchemaVisitor.CppAndIpcByteIdentical` still green **with a resolver active**, and
the 10 `protoc/tests/golden/*.ipc` goldens **byte-identical** (they carry no rules
— a change there means the resolver leaked into the no-rules path). No change to
`docs/fletcher-options.md`: the user-facing contract is unaltered.

---

## Downstream (out of this round)

The generator's forward roadmap after GIR: **GIR → BIND-C# → BIND-Rust → RIR**.
- **BIND-C#** (first binding round) — C# row/service emitters on the IR; the first
  non-C++ IR backend, which proves the IR's language-neutrality (decision #1).
- **BIND-Rust** (second) — Rust row/service emitters on the IR; establishes the
  Rust logical-type table.
- **RIR (RBA↔IR)** — [RIR-rba-onto-ir.md](RIR-rba-onto-ir.md): migrate the RBA
  accessor emitter (left read-only here per decision #3) onto the IR, reusing
  GIR's C++ table + BIND-Rust's Rust table, and **retire `FieldKind`**. Gated
  after BIND-Rust.

## Definition of done (round)

GIR-1..GIR-11 **and GIR-13** forcing tests 🟢 (GIR-1..GIR-11 closed 2026-07-11;
GIR-13 added 2026-08-26 when #121 landed on `main` after this branch cut — the
round reopened for it, so the archived-at-close artifacts for GIR-1..GIR-11 stay
in `docs/archive/GIR/` while this tracker and the progress log came back to
`plans/`); the full protoc unit suite +
the new compile-and-run harness + the Rust crate green; **wire format
byte-identical** (Encode==EncodeRow + decode round-trip oracles green for every
migrated emitter; generated-source goldens re-baselined under review, RBA no-drift
golden re-baselined but still additive-gating); the RBA accessor left read-only
and unbroken; #55/#53-generated/#75 closed; nothing in the spec's out-of-scope
touched. On completion, BIND-5/6 are unblocked to build on the IR.
