# DICT — Dictionary Column Option — Execution Plan

Round plan + tracker for adding the `(fletcher.dictionary)` protoc option.
Spec: [docs/dictionary-option-spec.md](../docs/dictionary-option-spec.md).
Locked decisions: [DICT-locked-decisions.md](DICT-locked-decisions.md).
This file is both the `plan_path` (the tracker) and the `user_stories_path`.

> **Post-GIR re-plan (2026-08-28) — READ THIS FIRST.** This plan was authored
> before round GIR rewrote the generator onto the language-neutral IR, and parts
> of it describe a tree that no longer exists. Verified against the post-GIR tree:
>
> | The plan says | Reality after GIR |
> |---|---|
> | DICT-3 "Schema emission (**nanoarrow + Arrow**)" — two emitters | GIR-5 unified them into **one** `cpp_backend::SchemaVisitor` with two sinks (C++ source / in-process nanoarrow). `BuildMessageSchemaInto` no longer exists in source. DICT-3 is now **one** change, not two kept in lockstep |
> | DICT-2 cites `type_mapper.cpp:632-669`, `MapWellKnown`, `MapStructField` | All gone; `type_mapper.cpp` is 465 lines and that classification moved into `ir.cpp`. The option must be read into the **IR builder**, and dictionary-ness carried on `FieldFacts` |
> | Branch strategy: base on #79 or `feature/robustness_improvements`; "`main` does not have it" | Both stale, and resolved favourably — the dictionary runtime `BuildDictionaryColumn` is on `main` (#79 merged), and the RBA accessor is on `main` (#106 merged) |
> | DICT-6 patches the flat RBA accessor | **DICT-6 is removed.** Replaced by **DICT-1.5** (a front-end guard) with the accessor work folded into round **RIR** — see below |
>
> **Still valid and load-bearing:** GIR deliberately pre-built the hook this round
> needs — `FieldFacts.dictionary` exists in [`protoc/include/ir.hpp`](../protoc/include/ir.hpp)
> per GIR locked decision #7 ("a dictionary field stays SCALAR with a modifier —
> it is **not** a structural container peer of List/Struct"). Extension **50001**
> is still free (`50000` is taken twice: `flatten` on MessageOptions,
> `flatten_field` on FieldOptions). The wire-format and runtime-unchanged
> invariants are untouched by GIR.
>
> **Opportunity to check at kickoff.** Locked decision #10 mandates a hand-rolled
> unknown-field walker specifically to avoid linking `options.pb.cc`. PR #121 has
> since landed a *general* option reader that solves the same problem by
> reflection over a `DynamicMessage` built from the `DescriptorPool`
> ([`protoc/src/option_metadata.cpp`](../protoc/src/option_metadata.cpp)). If that
> mechanism can read `(fletcher.dictionary)`, DICT-1 should reuse it and decision
> #10 retires rather than being reimplemented.

## Goal

Let a `.proto` field declare a dictionary-encoded Arrow column via
`[(fletcher.dictionary) = {...}]`, generating a schema the existing batched
`PubSubArrow::Subscribe` re-folds into a `DictionaryArray`. Generator + options
only — **wire format and runtime re-fold are unchanged** (hard invariant).

## Branch strategy

- **Base: `feature/dictionary-option`, STACKED on `feature/generator-ir-rewrite`
  (PR #125) — not on `main`.** *(Settled 2026-08-28.)* DICT must be written
  against the post-GIR generator, and #125 is still open pending review, so
  basing on `main` today would write the round against emitters that no longer
  exist. This is the same pattern GIR-13 used on the HARD branch, which rebased
  conflict-free afterwards. **When #125 merges, rebase onto `main`** with
  `git rebase --onto origin/main <gir-tip>`; the GIR commits drop out via the
  squash. Until then a PR from this branch carries GIR's commits in its diff too.
- Both dependencies this plan originally hedged about are already satisfied: the
  dictionary runtime (`BuildDictionaryColumn` in
  `pubsub-arrow/src/subscriber_arrow.cpp`, via #79) and the RBA accessor (via
  #106) are both on `main`. The pre-GIR candidate bases — #79 or
  `feature/robustness_improvements` — are obsolete; neither branch is live.
- DICT must land **after GIR** because it emits schema through the unified IR
  schema-visitor and carries dictionary-ness on the IR's `FieldFacts`. Basing on
  a pre-GIR tree would write the round against emitters that no longer exist.
- **Position in the roadmap: DICT runs BEFORE the binding rounds.**

  ```
  GIR ──▶ DICT ──▶ BIND-C# ──▶ BIND-Rust ──▶ RIR
  ```

  The reason is to avoid multiplying per-language work. DICT-5 exists because a
  language's *typed descriptor surface* has to know about dictionary columns —
  so every binding round that ships before DICT is another backend DICT would
  have to retrofit. Running DICT first means BIND-C# and BIND-Rust implement
  dictionary support once, natively, instead of being built dictionary-blind and
  patched afterwards.
- Rebased, not merged (repo convention). No PR until the round is green and
  reviewed; the PR/merge is the user's step.

## Interdependency with RIR — how the accessor half is closed

The original DICT-6 extended the **flat** RBA accessor to read dictionary
columns. That is duplicated effort: round **RIR** migrates that accessor onto the
IR and retires `FieldKind`, so a dictionary patch written against the flat
accessor would have to be re-migrated almost immediately.

This round therefore splits the accessor concern:

- **DICT-1.5 (in this round)** — a front-end guard that *refuses* to emit an
  accessor for a dictionary field, with a clear actionable error. Nothing invalid
  is ever generated, and the limitation is explicit rather than silent.
- **The accessor support itself moves to RIR**, where it is written once against
  the IR-based accessor. RIR also removes the DICT-1.5 guard.

This mirrors exactly what GIR-10 did for scalar-leaf nested lists
(`ValidateBackendsSupportFields` / `FindScalarLeafNestedList` in `generator.cpp`),
so DICT-1.5 extends an existing mechanism rather than inventing one.

**Consequence: RIR becomes the single round that closes every deferred
interdependency** — RBA C++/Rust accessor onto the IR, `FieldKind` retirement,
scalar-leaf nested lists, *and* dictionary columns, removing both front-end
guards together. Nothing else is left dangling behind it. See
[RIR-rba-onto-ir.md](RIR-rba-onto-ir.md).

## Sequencing

Strictly linear; each item's forcing test must be 🟢 before the next starts:

```
DICT-1  options + reader        →  DICT-1.5 accessor/rust front-end guard  →
DICT-2  mapper + validation     →  DICT-3   schema emission (ONE IR visitor) →
DICT-4  end-to-end roundtrip    →  DICT-5   TS + docs
```

DICT-1.5 sits immediately after the option becomes readable and before any
emission work, so no later item in the round can emit an accessor for a
dictionary field even transiently. The accessor's dictionary *support* is not in
this round at all — it is in RIR (see above).

---

## Work-item tracker

Status: ⚪ not-started · 🔴 in-progress · 🟢 done (forcing test green + reviewed)

| Item | Title | Forcing test | Status |
|------|-------|--------------|--------|
| DICT-1 | Option surface + reader | `TypeMapperTest.ReadsDictionaryOption` | 🟢 |
| DICT-1.5 | Front-end guard: reject `accessor`/`rust` for dictionary fields | `GenErrors.DictionaryRejectedBy_{accessor,rust}` | 🟢 |
| DICT-2 | Mapper wiring + validation | `TypeMapperTest.DictionaryMappingAndRejections` | ⚪ |
| DICT-3 | Schema emission (ONE IR schema-visitor) | `DictionaryTest.SchemaIsDictionaryType` | ⚪ |
| DICT-4 | End-to-end roundtrip via batched Subscribe | `DictionaryTest.RoundtripRefoldsToDictionaryArray` | ⚪ |
| DICT-5 | TypeScript descriptor + docs | `DictionaryTest.TsDescriptorUsesValueType` | ⚪ |

Suite shape: integration `+2 .proto` (`dictionary.proto`), `+1` test TU
(`tests/test_dictionary.cpp`), `+1` protoc unit TU group in
`test_type_mapper.cpp`. **DICT-1.5** adds its two negative plugin-exit tests
beside GIR-10's existing backend-guard negatives in
`integration-tests/protoc-coverage` (`GenErrors.*`), plus a dictionary-bearing
fixture proto there. DICT-1 landed `protoc/tests/proto_text_pool.hpp` (an all-`inline`, per-test source-text pool helper) and a new `option_reader` module. **No accessor test TU and no Rust-crate change** — the
accessor suites appear only as RBA no-drift checks, since this round does not
touch the accessor emitter (the old DICT-6 moved to RIR).

---

## Items (user stories + acceptance)

### DICT-1 — Option surface + reader

**Story.** As a schema author I can write `[(fletcher.dictionary) = {...}]` on a
field and the plugin can read its `index_type`/`ordered` values.

**Scope.**
- `protoc/include/fletcher/options.proto`: add `DictionaryIndexType` enum,
  `DictionaryOptions` message, and `extend FieldOptions { ... dictionary = 50001; }`
  (spec §2).
- `type_mapper.cpp`: `constexpr int kDictionaryOptionNumber = 50001;` plus a
  reader that parses the **length-delimited** unknown field #50001 into a small
  `DictionaryOption { DictIndex index; bool ordered; }` (spec §7), and
  `bool HasFieldDictionary(const FieldDescriptor*)`. Declare the public reader in
  `type_mapper.hpp`.

**Forcing test** (`protoc/tests/test_type_mapper.cpp`): build a `FieldOptions`
carrying #50001 as a hand-encoded `DictionaryOptions` payload
(`index_type=INT16` → bytes `08 02`) on a programmatically-built field; assert
the reader returns `{INT16, ordered=false}`. **Add a second sub-case that
encodes `ordered=true` (field 2 varint: bytes `10 01`, optionally with the index
byte → `08 02 10 01`) and assert the reader reports `ordered=true`** — proto3
omits a defaulted `false` on the wire, so without this the test cannot prove the
`ordered` field is parsed at all (and DICT-2's rejection of `ordered:true` would
rest on an unexercised reader path). Assert absence → no-dictionary, and that a
malformed/empty payload resolves to defaults (int32, ordered=false).

**Acceptance.** Reader round-trips index types {unspecified→int32, int8, int16,
int32, int64} and the `ordered` flag; `HasFieldDictionary` is false when the
option is absent.

### DICT-1.5 — Front-end guard: reject `accessor` / `rust` for dictionary fields

**Story.** As a schema author who puts `(fletcher.dictionary)` on a field and
also asks for `--fletcher_opt=accessor` or `rust`, I get a clear, immediate
plugin error naming the limitation — never generated accessor code that compiles
and then reads the column wrongly.

**Why this exists.** The RBA accessor's storage and getters are hard-wired to
concrete value arrays (`LookupScalarArray`, exact value-type positional gate,
`value(row)`); they cannot read a `dictionary(idx, val)` column. Teaching them to
is real work, and round **RIR** is about to migrate that whole emitter onto the
IR — so writing the dictionary support against the flat accessor now means
writing it twice. This item makes the gap *loud and safe* instead, and RIR closes
it for good.

**Precedent — extend, do not invent.** GIR-10 hit exactly this shape with
scalar-leaf nested lists and solved it with a front-end pass:
`ValidateBackendsSupportFields` / `FindScalarLeafNestedList` in
[`protoc/src/generator.cpp`](../protoc/src/generator.cpp), which fails the plugin
*before any emission* with "not supported by the RecordBatch accessor / Rust
backend (tracked for RIR)". DICT-1.5 adds a dictionary predicate to that same
pass. Do **not** add a second, parallel validation mechanism.

**Scope.**
- `generator.cpp`: extend the existing `ValidateBackendsSupportFields` pass with
  a dictionary predicate, reusing DICT-1's `HasFieldDictionary` reader. Error
  text must name the option, the field, and that it is tracked for RIR.
- Ordering: the guard runs front-end, before emission, and **after**
  `ValidateNoUnsupportedIr` (#55) so a genuinely unsupported type still reports
  its own error first rather than being masked by a backend-shape complaint.
- The RBA emitter itself is **untouched** (GIR locked #3 — it stays read-only
  until RIR).

**Forcing test** (red-first): `GenErrors.DictionaryRejectedBy_accessor` and
`GenErrors.DictionaryRejectedBy_rust` — run the plugin on a proto with a
dictionary field and `--fletcher_opt=accessor` / `rust`; assert a non-zero exit
and an error mentioning the field and RIR. Red before the guard exists because
the plugin currently exits 0 and emits an accessor that mis-reads the column.

**Acceptance.** A dictionary proto **without** `accessor`/`rust` still generates
everything else normally (no false positive — mirror GIR-10's check that
`coverage.proto` with full opts still exits 0). A non-dictionary proto **with**
`accessor,rust` is unaffected. The error is actionable without reading the source.

### DICT-2 — Mapper wiring + validation

**Story.** As a schema author, a dictionary option on a valid scalar field
produces a dictionary mapping, and on an invalid field a clear error.

**Scope.**
- `type_mapper.hpp`: add `bool is_dictionary = false;` + `std::string
  dict_index_type_expr;` to `FieldMapping` (spec §5). `FieldKind` stays `SCALAR`.
- `MapScalarField`: when `HasFieldDictionary`, set `is_dictionary` and
  `dict_index_type_expr` (from `index_type`); leave `scalar` as the value type;
  preserve `nullable`.
- **Acceptance is keyed on the _mapped_ `FieldKind`, not the raw proto type.**
  Accept the option exactly when `MapField` resolves the field to
  `FieldKind::SCALAR` — which by construction includes singular scalars, enums,
  **and well-known wrapper messages** (`google.protobuf.StringValue`,
  `Int32Value`, …). `MapField` routes WKT wrappers through `MapWellKnown` to a
  nullable `FieldKind::SCALAR` _before_ the struct branch
  ([type_mapper.cpp:632-669](../protoc/src/type_mapper.cpp#L632-L669)), so a
  wrapper carrying `(fletcher.dictionary)` is a **valid nullable dictionary**
  (`dictionary(<idx>, <inner>)`, nullable) — gating on the raw
  `field->type()==TYPE_MESSAGE` would wrongly reject it. Real struct messages
  (`MapStructField` → `STRUCT`), `repeated`, `map`, and `NESTED_LIST` fields are
  rejected because they map to a non-`SCALAR` kind (spec §4, decision #9).
- `MapField`/`UnsupportedReason`: reject the option on non-`SCALAR`-mapped fields
  (struct message, `repeated`, `map`) and reject `ordered: true`, each with a
  distinct reason (spec §4, §6).

**Forcing test** (`protoc/tests/test_type_mapper.cpp`): a scalar field with the
option → `mapping.is_dictionary`, correct `dict_index_type_expr`, value-type
`scalar` unchanged, nullability preserved; **a WKT-wrapper field
(`google.protobuf.StringValue` with the option) → accepted as a nullable
dictionary** (`is_dictionary`, value type `utf8`, `nullable`); the option on a
repeated / struct-message / map field → `nullopt` with the matching
`UnsupportedReason`; `ordered:true` → `nullopt` + reason.

### DICT-3 — Schema emission (ONE IR schema-visitor)

**Story.** The generated `<Cls>Schema()` declares the column as
`dictionary(<index>, <value>)` with nullability preserved; all row-oriented
output stays value-typed.

**Scope.**
- **There are TWO schema emitters and BOTH must branch on `is_dictionary`**
  (post-#96): (1) `EmitNanoarrowTypeSetup`
  ([generator.cpp:900](../protoc/src/generator.cpp#L900), used by
  `GenerateSchemaFunction` for the generated `<Cls>Schema()` C++ source), and
  (2) `BuildMessageSchemaInto`
  ([generator.cpp:1225](../protoc/src/generator.cpp#L1225), the `--fletcher_opt=ipc`
  runtime schema builder whose scalar branch emits
  `SetScalarSchemaType(child, fi.mapping.scalar.arrow_type_expr)` at
  [generator.cpp:1241](../protoc/src/generator.cpp#L1241)). Updating only
  `EmitNanoarrowTypeSetup` leaves the IPC-emitted schema value-typed — a silent
  miss. When `is_dictionary`, both emit `dictionary(<declared index>, <value>)`
  with the value-type child and nullability preserved.
- Audit `ArrowTypeExpr` ([generator.cpp:195](../protoc/src/generator.cpp#L195)):
  its scalar branch returns the value type and is used in row-oriented contexts
  (e.g. `MakeNullScalar`) — confirm it is **not** a third schema-type site that
  needs the branch; if any schema-type string is emitted through it, fix that too.
- Any separate server-tier Arrow schema/`ToArrowRow` path: confirm per-row
  scalars stay **value-type** (codec accepts them; spec §5) — change only if a
  schema-type string is emitted there.
- **Keep reading dictionary-ness from `ir::FieldFacts.dictionary`, not from the
  descriptor.** DICT-1.5's backend-availability guard
  ([FindDictionaryField](../protoc/src/generator.cpp), spec §7.1 gap 2) is only
  safe to miss the inner-declared `repeated <message-level-flatten wrapper>`
  shape *because* schema emission consumes the identical IR node and therefore
  drops the same declaration — no mis-read exists today. If DICT-3 ever derives
  `is_dictionary` from a second, descriptor-based source (e.g.
  `HasFieldDictionary` on the field directly) instead of the IR's
  `facts.dictionary`, that shape would silently start emitting
  `dictionary(idx, val)` with **no guard and no test** catching the now-real
  mis-read (step-4b code review, N5). Re-verify DICT-1.5's superset property
  (`plans/DICT-1.5-backend-support-guard.md` D1 / Risk 2) before making that
  change, or close the IR gap first.

**Forcing test** (`integration-tests/protoc-arrow-bridge`, new `dictionary.proto`
+ `tests/test_dictionary.cpp`, mirroring `test_flatten.cpp`): assert
`DictionaryEventSchema()` field `category` has `type_id == DICTIONARY`, index
type == declared, value type == `utf8`, and the nullable flag matches the proto
`optional`-ness.

**Build wiring.** Add `dictionary` to `PROTO_STEMS` and
`tests/test_dictionary.cpp` to the `integration_tests` target in
[integration-tests/protoc-arrow-bridge/CMakeLists.txt](../integration-tests/protoc-arrow-bridge/CMakeLists.txt).

### DICT-4 — End-to-end roundtrip via batched Subscribe

**Story.** Rows built from a generated edge class, published with the generated
schema, come back from the batched subscriber as a `DictionaryArray`.

**Runtime preflight (grounding the "wire carries values" invariant).** Before
relying on the existing runtime, confirm both codec directions already handle a
dictionary field as its value type (no runtime change needed): **encode** accepts
a dictionary- or value-scalar and transfers the value
([codec.cpp:389-395](../arrow-bridge/src/codec.cpp#L389-L395),
[scalar_codec.cpp:178-188](../arrow-bridge/src/scalar_codec.cpp#L178-L188)) and
**decode** unwraps the dictionary type and decodes the plain value scalar
([scalar_codec.cpp:305-311](../arrow-bridge/src/scalar_codec.cpp#L305-L311)).
These are confirmed present on this branch; if either regresses, DICT-4 is the
forcing test that catches it. `EnsureDictionaryValueSupported`
([scalar_codec.cpp:32-52](../arrow-bridge/src/scalar_codec.cpp#L32-L52)) already
rejects non-scalar dictionary value types at runtime, mirroring DICT-2's codegen
rejection (decision #9).

**Scope.** Proves DICT-1..3 compose with the **existing** runtime. Requires
**build wiring**: the `protoc-arrow-bridge` integration harness currently links
only `fletcher-arrow-bridge` + `fletcher-pubsub` — add `find_package(fletcher-pubsub-arrow)`
and link it in
[integration-tests/protoc-arrow-bridge/CMakeLists.txt](../integration-tests/protoc-arrow-bridge/CMakeLists.txt)
(and add `pubsub-arrow` to the component build list — already reflected in the
config's verification commands). No new production code expected. If the item
surfaces a gap (e.g. a non-int32 index fails the re-fold cast), fix the minimal
generator/schema cause, not the runtime (runtime re-fold is out of scope / locked).

**Forcing test** (`tests/test_dictionary.cpp`): publish `"red","blue","red"`
(and a null) through `PublisherArrow` + generated schema; batched `Subscribe`
→ column is `DictionaryArray`, length 4, dictionary length 2, repeated value
shares an index, null preserved, declared index type honored. Mirrors
`DictionaryColumnRefoldedToDictionaryArray` / `DictionaryColumnPreservesNulls`
in [pubsub-arrow/tests/test_pubsub_arrow.cpp](../pubsub-arrow/tests/test_pubsub_arrow.cpp)
but driven by **generated** code.

### DICT-5 — TypeScript descriptor + docs

**Story.** TS clients see the dictionary field as its value type; the option is
documented.

**Scope.**
- Confirm `EmitTsFieldDescriptor` emits the **value-type** `WireTypeId` for a
  dictionary field (expected: no code change — verify, spec §5).
- Docs: add the `(fletcher.dictionary)` section + the `50001 / FieldOptions`
  registry row to
  [docs/fletcher-options.md](../docs/fletcher-options.md).

**Forcing test:** assert the generated TS descriptor for a dictionary field
carries the value type's `WireTypeId` (use the existing TS-generation assertion
path if present; otherwise a generator-level assertion). Docs change is verified
by review.

### DICT-6 — MOVED to round RIR (2026-08-28)

**This item is no longer part of DICT.** Extending the generated RBA accessor
(C++ `.fletcher.accessor.pb.h` + arrow-rs `.fletcher.rs`) to read dictionary
columns now lives in round **RIR**, alongside the migration of that same emitter
onto the IR.

**Why it moved.** Written here, it would patch the *flat* accessor — and RIR
retires the flat model days later, so the work would be done twice against two
different substrates. In RIR it is written once, against the IR-based accessor,
sharing the type-table machinery BIND-Rust establishes for arrow-rs types.

**What replaces it in this round:** [DICT-1.5](#dict-15--front-end-guard-reject-accessor--rust-for-dictionary-fields),
a front-end guard that refuses to emit an accessor for a dictionary field with a
clear error. Nothing invalid is generated in the interim, and the limitation is
explicit.

**What RIR must deliver** (requirements carried over verbatim in intent, so
nothing is lost in the hand-off):

- Positional type gate expects `dictionary(<index>, <value>)`.
- Cast-once, offset-preserving cache of the `DictionaryArray`, **plus** the
  downcast values array for resolution.
- Null-before-index getter keeping the **value-typed** signature unchanged
  (utf8 → `std::string_view` / `&str`, int32 → `int32_t` / `i32`, …), so the
  dictionary encoding stays invisible at the API. Borrowed returns tie to the
  cached dictionary's values buffer.
- Index-type → key-array mapping driven by the option's `index_type`.
- RBA contracts preserved: proto-non-nullable dictionary fields keep
  `null_count() == 0`; proto-optional ones surface nulls via `optional`/`Option`;
  never panics (`Status`/`Result`); C++ and Rust read the same batch identically.
- **Removal of the DICT-1.5 guard** in the same change that adds the support, so
  the guard cannot outlive its subject.

Tracked in [RIR-rba-onto-ir.md](RIR-rba-onto-ir.md).

## Definition of done (round)

All **six** forcing tests 🟢 — DICT-1, **DICT-1.5**, DICT-2, DICT-3, DICT-4,
DICT-5 (the accessor item moved to RIR, so the count is unchanged but the set is
not). Full integration + protoc unit suites green; wire format byte-identical for
non-dictionary fields (no regression in existing `test_*` schema/roundtrip
assertions); a dictionary column round-trips through the batched subscriber and
re-folds to a `DictionaryArray`; the TypeScript descriptor uses the value type;
**no drift in existing accessor output** (RBA no-drift golden green — this round
does not touch the accessor emitter at all); docs updated; nothing in §8 of the
spec touched.

**Explicitly NOT in this round's definition of done:** reading a dictionary
column through the generated C++/Rust accessor. DICT-1.5 guarantees the attempt
fails loudly instead; RIR delivers the capability and removes the guard.
