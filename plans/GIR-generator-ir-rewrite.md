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

- Base on **`main` after the HARD runtime-hardening PRs (#109–#116) merge** —
  HARD is orthogonal (runtime/pubsub) but touches the same wire path Phase 3
  exercises; confirm the base at kickoff. Suggested branch:
  `feature/generator-ir-rewrite`.
- The stale `feature/robustness_improvements` branch is **not** revived (its
  Phase-1 content is on `main` via #98).
- Rebased onto `main`, not merged (repo convention). PR split (each independently
  reviewable): GIR-1/2 (harness+oracles) → GIR-3/4 (IR + edge codec) → GIR-5..7
  (schema/IPC, view, TS) → GIR-8/9 (generator-behaviour + enum) → GIR-10/11
  (coverage). No PR until green + reviewed; PR/merge is the user's step.

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
| GIR-9 | #75 emit C++ enum symbols (typed accessors) | 🟨 | `EnumEmit.GeneratedEnumSymbolsRoundTrip` | ⚪ |
| GIR-10 | Codec edge/boundary + flatten/arbitrary-nesting coverage (3c/3d) | 🧪 | `CodecEdge.*` + `Nesting.ListOfListOfScalarRoundTrips` | ⚪ |
| GIR-11 | Property + fuzz (3e) | 🧪 | `Fuzz.DecodeRowSurvivesRandomTruncatedBuffers` + round-trip property | ⚪ |

Suite shape: new protoc unit TU group (`test_ir.cpp`, lands GIR-3); the compile-and-run
integration harness **landed at GIR-1** in `integration-tests/protoc-coverage/`
(`coverage.proto` + `coverage_future.proto`; ctest targets `coverage_harness_tests`
+ `coverage_accessor_tests`; tsc/rustc checks Skip when the toolchain is absent);
the Rust crate (`integration-tests/protoc-gen-fletcher-rust`) stays green (RBA
no-drift). Both wired into the config's inner-loop/full-suite commands.

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

GIR-1..GIR-11 forcing tests 🟢; the full protoc unit suite +
the new compile-and-run harness + the Rust crate green; **wire format
byte-identical** (Encode==EncodeRow + decode round-trip oracles green for every
migrated emitter; generated-source goldens re-baselined under review, RBA no-drift
golden re-baselined but still additive-gating); the RBA accessor left read-only
and unbroken; #55/#53-generated/#75 closed; nothing in the spec's out-of-scope
touched. On completion, BIND-5/6 are unblocked to build on the IR.
