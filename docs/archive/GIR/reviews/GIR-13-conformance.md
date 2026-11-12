# GIR-13 — Step-4a architecture-conformance review (adversarial)

**Item:** GIR-13 — Option metadata on the IR schema visitor
**Design (operative):** `plans/GIR-13-option-metadata-on-ir.md` revision 3 (APPROVE)
**Locked decisions:** `plans/GIR-locked-decisions.md` #2, #3, #5, #9, #10
**Diff base:** `HEAD` = `9913274…` (`git diff --cached HEAD`; implementation is staged)
**Reviewed:** 2026-08-26 · branch `feature/generator-ir-rewrite`

## Verdict

**CONFORMS.** No blocking conformance items. Every load-bearing element of the
design is present in the shape the design specified, and — critically — I did not
take the tests' word for it: I **mutation-tested the guards** to prove they
discriminate the exact failure modes the design named. All of them discriminate.
Four non-blocking observations at the end.

## Method

Beyond reading the diff against the design, I:

1. Built and ran the protoc unit suite against the exact staged tree:
   **88 tests / 87 passed / 0 failed / 1 skipped** (`SchemaVisitor.CaptureGoldens`,
   which is skip-unless-capture by design).
2. **Mutation-tested** each of §6 T1–T3 by breaking exactly the property it claims
   to pin, rebuilding, and confirming red-for-the-right-reason (table below).
3. Empirically discharged **locked #2** for the *generated-source* path, not just
   the `.ipc` goldens (see "Locked #2" below).
4. Established that the **Conan integration lane really was run against the final
   code** (see "Integration evidence"), which discharges design risk #6 rather
   than leaving it as an admitted gap.
5. Restored the tree bit-for-bit afterwards (`git status --porcelain` identical to
   the pre-review snapshot; all mutations reverted via `git checkout --`), and
   rebuilt so the build tree matches the staged sources.

## Mutation results — do T1–T4 actually discharge the design?

| # | Mutation applied | Expected per design | Observed |
|---|---|---|---|
| M1 | `EmitNodeType` **MAP struct-value** site (design `:419`) passes `nullptr` instead of `resolver_` | T2's `byname→entries→value→x` row red | **T2 RED**, `x:meta` = `<missing>` on the map value **and** `x:group` lost on `value`; every other test green |
| M2 | `EmitNodeType` **STRUCT** site (design `:404`) passes `nullptr` | T2's `pos→x` and `path→item→x` rows red | **T2 RED**, both rows `<missing>` ("singular-struct deep copy", "list<struct> deep copy"); everything else green |
| M3 | flatten chain built **inner→outer** (`inner.insert(inner.begin(), fd)`) | T3 red on `x:unit`; nothing else | **T3 RED**: `x:unit` = `"mid"` (want `"inner"`). **All other 21 tests stayed green** — empirically confirming the design's claim that every other fixture in the tree is single-level and blind to orientation |
| M4 | key **not** escaped in `CppSchemaSink::SetMetadata` (value still escaped) | T1's key assertions red | **T1 RED**: "escaped Arrow KEY missing from generated source" + "raw (unescaped) key leaked into source" |
| M5 | escaping moved **into the visitor** (`FieldMetadata` escapes; sink emits raw) — the design's named silent-failure mode | T1 source assertions **pass**, in-process assertion **red** | **T1 RED exactly there**: `MetaValue(v, raw_key)` = `<missing>`, metadata held `{\"crs\":…}\\\x1\xC2\xB0` (escape sequences stored literally). Source assertions passed, as predicted |

Conclusions for the points I was asked to pressure-test:

- **The weak nominal forcing test is indeed weak — and it does not matter.**
  `OptionMetadataTest.FlattenFieldWrapperContextReachesEachInlinedLeaf` is pure
  resolver semantics; M3 (reversed chain) left it green. But T3 caught M3 alone,
  and T2 caught M1/M2 alone, so the design's §6 guards genuinely carry the item.
- **T1 asserts at the render layer, for key *and* value, with raw bytes in-process.**
  It inspects the string returned by `GenerateSchemaFunctionFromIr` — i.e. output of
  `CppSchemaSink::SetMetadata`, *below* the `RecordingSink` layer that made the old
  guard blind — and separately asserts `MetaValue(v, raw_key) == raw_value` on the
  in-process schema. M4 and M5 prove both halves are load-bearing.
- **T2 genuinely reaches `:419`, not only `:404`.** M1 proves it: breaking *only*
  the MAP struct-value site turns exactly the map row red. The `field:`-scope,
  chain-independent `x:meta` key (the revision-3 fix) was implemented as specified
  and is annotated in-code with a "do not simplify back to a `field_type:` key"
  warning, so risk #3 is closed rather than re-openable by accident.
- **T3 discriminates,** and is the *only* discriminator in the tree.

## Point-by-point conformance

### 1. Chain accumulation and orientation (§1) — CONFORMS
`BuildFlattenedFieldListImpl` (`protoc/src/cpp_backend_schema_visitor.cpp:66-110`)
gains the `flatten_chain` parameter **next to `id_prefix`** as the design required,
copies-and-`push_back`s **while descending** (outer→inner), and assigns
`rec.flatten_chain = flatten_chain` at the leaf. This matches
`OptionMetadataResolver::ForField`'s `rbegin()/rend()` scan
(`protoc/src/option_metadata.cpp:488-492`), which I re-read: candidates are
`[leaf, *chain.rbegin(), …]`, so the **last** chain element must be the innermost
wrapper. Correct as implemented; M3 pins it.
Public `BuildFlattenedFieldList(msg, id_prefix)` arity **unchanged**; the top-level
entry passes `{}` — as designed.
The wrong `source_field` header comment (`cpp_backend_schema_visitor.hpp:49-53`)
was corrected, as the design required.

### 2. One pair vector per node (§2) — CONFORMS
`SchemaVisitor::RootMetadata()` / `FieldMetadata()` exist as private members, build
builtins-first + resolver-extras-appended, guard on `resolver_ != nullptr`, and are
the single call site each. `Visit()` is otherwise unchanged. No de-duplication
added, no "skip SetMetadata" branch added, overlay order untouched.
(The design's `Append(pairs, …)` helper became an inline `pairs.insert(pairs.end(), …)`
— semantically identical, no drift.)

### 3. Both deep-copy sites carry the resolver (§3) — CONFORMS
`DeepCopyMessageStruct` gained `const OptionMetadataResolver* resolver` on the
`SchemaSink` interface and both implementations, **passed through rather than held
as sink state** (visitor remains the single owner). `CppSchemaSink` explicitly
`(void)resolver;` with the documented rationale. `NanoarrowSchemaSink` forwards it
into `BuildMessageSchemaIntoFromIr`. **Both** `EmitNodeType` sites pass `resolver_`
— STRUCT (also reached via the LIST recursion) and MAP struct value. Neither was
missed; M1/M2 prove each is individually observable. `RecordingSink` in
`test_schema_visitor.cpp` grew the parameter, as required.

### 4. Escaping — sink-local, uniform, octal (§4) — CONFORMS
`CppSchemaSink::SetMetadata` applies `EscapeCppStringLiteral` to **both** members of
**every** pair, unconditionally. `NanoarrowSchemaSink::SetMetadata` still writes raw
bytes via `ArrowCharView(key.c_str())` / `(value.c_str())` — asymmetry preserved
exactly as designed. `ArrowCharView` kept (no switch to explicit-length views).
The octal escaper is untouched (`option_metadata.{hpp,cpp}` unmodified), so no
octal→hex "simplification" and none of the 8 `EscapeCppStringLiteralTest` cases
weakened. `CppSchemaSink::SetName` left unescaped, as specified.

### 5. Option plumbing in `generator.cpp` (§5) — CONFORMS
- `#include "option_metadata.hpp"` added.
- `ParseMetadataRules(parameter, &metadata_rules, error)` runs **after** the
  `--fletcher_opt` token loop and **before** `ValidateBackendsSupportFields`, as
  designed (and after `ValidateNoUnsupportedIr`).
- **`resolver == nullptr` literally means "no rules"**: the resolver is created only
  inside `if (!metadata_rules.empty())`. This is exactly what the design resolved
  the revision-2 contradiction to, and it is what the locked-#2 argument rests on.
  Confirmed verbatim in the code.
- **Lifetime contract honoured.** The resolver is a `std::unique_ptr` **stack-local
  in `Generate()`**; every consumer takes a `const*` for the duration of that call.
  I grepped: no member on `ArrowRowGenerator`, no `static`/global, no cross-file
  cache, no new `mutable` in the visitor TU. `GenerateAll` simply loops `Generate`
  per file, so one resolver per `Generate()` as designed. The in-code comments
  restate the "memoising, not thread-safe, never hoist" contract at both the
  creation site and the `SchemaVisitor` ctor declaration.
- All threaded parameters carry an **explicit `= nullptr`** default
  (`GenerateFile`, `GenerateSchemaFunction`, `BuildMessageSchemaInto`,
  `BuildMessageSchema`, `GenerateSchemaFunctionFromIr`,
  `BuildMessageSchemaIntoFromIr`, `SchemaVisitor` ctor), so the nine one-arg
  `BuildMessageSchema` calls in `test_schema_builder.cpp` and the existing
  `test_schema_visitor.cpp` call sites still compile at today's arity — verified by
  the suite building and passing unchanged.
- `schema_builder.hpp:20-27` matches the design's required verbatim signature and
  includes `option_metadata.hpp`.
- I enumerated **every** caller of `GenerateFile` / `GenerateSchemaFunction` /
  `BuildMessageSchemaInto` / `BuildMessageSchema` / `GenerateSchemaFunctionFromIr`
  tree-wide: no production call site bypasses the resolver.

### 6. Dead helpers NOT revived — CONFIRMED
Tree-wide grep: `SetMetadataPairs` (`generator.cpp:887`), `SetScalarSchemaType`
(`:848`), `EmitNanoarrowTypeSetup` (`:644`), `RequireNestedMsg` (`:905`) each occur
exactly once as a definition, with **zero callers**. Nothing was wired into them;
none were removed either (removal is explicitly out of scope). The only other
occurrences are pre-existing explanatory comments.

### 7. Locked decisions
- **#2 (wire byte-identical / goldens must not move) — DISCHARGED, empirically.**
  `protoc/tests/golden/*.ipc` are untouched (`git status`) and
  `SchemaVisitor.CppAndIpcByteIdentical` + the 9 `SchemaBuilderTest` cases are green.
  Beyond that, I proved the *generated-source* half directly: with escaping
  disabled vs enabled and **no rules**, the plugin's output over the entire
  `integration-tests/protoc-arrow-bridge/proto` corpus (**76 artifacts** — `.pb.h`,
  `.arrow.pb.h`, `.ts`, `.ipc`) is **byte-identical**. So the only change that could
  perturb the no-rules path is provably a no-op, and under `resolver == nullptr`
  the two pair-vector expressions are bit-for-bit the pre-GIR-13 ones by inspection.
- **#3 (RBA read-only) — CONFORMS.** No change to
  `recordbatch_accessor_emitter.*`, `generator_internal.hpp`, or `FieldInfo`. No
  `flatten_chain` added to `FieldInfo` (the design's "Why not `FieldInfo`" holds).
- **#5 (ONE visitor, both sinks) — STRENGTHENED, not bypassed.** Both renderers
  consume one vector built in one place per scope; the resolver is passed *through*
  the sink operation so the two holders a refactor could desynchronise never come
  into existence.
- **#9 (red-first) — SUBSTANTIVELY SATISFIED.** The forcing suite
  (`test_option_metadata.cpp`, 32 tests) was link/compile-red at `HEAD` (TU unwired,
  no two-arg `BuildMessageSchema`), and T1–T4 were compile-red (the ctor/entry
  points took no resolver). The TU's contents are byte-for-byte unmodified, as the
  design required. My mutation table above independently reproduces the
  discriminating failure modes the design asked to be recorded. See observation N2.
- **#10 (scope = `protoc/` only) — CONFORMS.** Changed files:
  `protoc/{include,src,tests}` (6 files) + `plans/GIR-13-option-metadata-on-ir.md`
  (the item's own design doc, rewritten to revision 3 by step 1/2 — `HEAD`'s version
  was the 252-line orchestrator scoping note) + `plans/GIR-generator-ir-rewrite.md`
  (the tracker note on row `:100` that the design's Files-to-touch explicitly
  requires). **No** change to `docs/fletcher-options.md`, `option_metadata.{hpp,cpp}`,
  `protoc/tests/test_option_metadata.cpp`, `protoc/tests/golden/*`, or anything
  under `integration-tests/`. The untracked `docs/*-spec.md` and `plans/{DICT,BIND}-*`
  files are pre-existing artifacts of another round, present before this item and
  not part of its diff.

### Integration evidence (design risk #6) — RUN, and against the final code
The Conan lane **was** exercised: `integration-tests/protoc-arrow-bridge/build`
shows a build at 13:15–13:26 today and `Testing/Temporary/LastTest.log` records
**90 tests, 89 passed, 0 failed, 1 skipped**, including all of
`MetadataOptionsTest.*` (11), `MetadataNoDriftTest.*` (5) and `IpcParityTest.*` (5)
— i.e. `JsonValueSurvivesCppStringLiteralEscaping`,
`NestedStructChildMetadataSurvivesTheDeepCopy`,
`MappedMetadataIpcFileMatchesRuntimeSchemaBytes`,
`MappedMetadataOnFlattenedFieldsMatchesRuntimeSchemaBytes`,
`InertRulesLeaveEveryOutputByteIdentical`, and both non-zero-exit tests.
Because the plugin binary in that build's CMake cache is timestamped slightly
*before* the last source edit, I did **not** take the log at face value: I rebuilt
`fletcher-protoc` from the exact staged tree, re-ran the harness's own generation
command with `FLETCHER_METADATA_RULES` taken from its CMake cache, and `cmp`'d the
output against the artifacts the lane actually compiled —
`option_metadata.fletcher.pb.h`, `option_metadata.fletcher.arrow.pb.h` and
`option_metadata.{Sample,Pos,Coord}.ipc` are all **byte-identical**. The integration
evidence therefore does apply to the final code.

### Formatting / CI
Per-file clang-format (18.1.3) violation counts are **identical to `HEAD`**
(hpp 7→7, cpp 11→11, test 9→9) — i.e. the known local-vs-baseline env gotcha, with
**no new** formatting drift introduced. `generator.cpp` and `schema_builder.hpp` are
clang-format-clean.

## Non-blocking observations

**N1 — `generator.cpp`: the new comment block orphans the pre-existing GIR-10 comment.**
The GIR-13 block was spliced *between* the GIR-10 explanatory comment ("reject
scalar-leaf nested lists for the read-only RBA C++/Rust backends BEFORE any
artifact is emitted…") and the `ValidateBackendsSupportFields(...)` call it
documents. The GIR-10 comment now reads as a preamble to the metadata-rule code.
Cosmetic; fix by moving the GIR-13 block above the GIR-10 comment. Step-4b
territory.

**N2 — `plans/GIR-progress-log.md` has no GIR-13 entry yet.**
The design's Files-to-touch lists it ("progress entries"), and the design's
locked-#9 paragraph specifically asks to "record which failure mode each showed"
for the four flatten tests plus T1/T2/T3. No such record exists in the tree.
Existing GIR-1..GIR-11 entries cite their review files, so the log is evidently
written at close (step 5+) rather than step 4 — hence non-blocking. The PM can lift
the mutation table above verbatim as that record.

**N3 — cosmetic:** `test_option_metadata.cpp` was appended after
`test_schema_visitor.cpp` in the test-source list rather than in alphabetical
position. The list is not strictly alphabetical anyway.

**N4 — the implementation exceeds spec in two places** (both discharging
revision-3 non-blocking nits): T2 also asserts
`EXPECT_FALSE(HasMeta(pos, "proto_message"))`, making "the overlay **replaced**"
exact rather than merely consistent (nit 1); and T1 also asserts the
unset-sub-field silent-skip path on `Nasty.v` (nit 4). Both welcome.

## Files reviewed
- `protoc/include/cpp_backend_schema_visitor.hpp`
- `protoc/include/schema_builder.hpp`
- `protoc/src/cpp_backend_schema_visitor.cpp`
- `protoc/src/generator.cpp`
- `protoc/tests/CMakeLists.txt`
- `protoc/tests/test_schema_visitor.cpp`
- `plans/GIR-13-option-metadata-on-ir.md`, `plans/GIR-generator-ir-rewrite.md`
- context: `protoc/src/option_metadata.cpp`, `protoc/tests/test_option_metadata.cpp`,
  `protoc/src/type_mapper.cpp`,
  `integration-tests/protoc-arrow-bridge/{CMakeLists.txt,conanfile.py}`

---

# Step-4a RE-REVIEW after the fix cycle (2026-08-26)

**Trigger:** the step-4b fix cycle edited **five files the approved design
(revision 3) marks "no change"**. The PM correctly refused to adjudicate that
himself. Diff base unchanged (`HEAD` = `9913274`, all staged).

**Verdict: CONFORMS.** The deviation is **defensible and, for the two
`option_metadata.*` files, the only correct site.** It is **not** a locked-decision
breach. Rationale recorded below so a later reader knows why "port unchanged" was
overridden. Three non-blocking items (RR-1..RR-3).

## The five files

| File | Change | Adjudication |
|---|---|---|
| `protoc/src/option_metadata.cpp` (+16) | `EscapeCppStringLiteral` splits the literal when a decimal digit would follow a three-digit octal escape | **In scope, correct site** |
| `protoc/include/option_metadata.hpp` (+10) | documents the new output contract + corrects a stale `FieldInfo::flatten_chain` reference | **In scope** |
| `protoc/tests/test_option_metadata.cpp` (+58) | 2 stale expectations updated, 1 exhaustive guard added, 30 of the original 32 untouched | **In scope, not weakened** |
| `integration-tests/protoc-arrow-bridge/proto/option_metadata.proto` | `Pos.ext_meta` gains a non-ASCII byte pair followed by an ASCII digit, plus a control byte | **Generator test; strictly strengthened** |
| `integration-tests/protoc-arrow-bridge/tests/test_metadata_options.cpp` | expectation updated to the new fixture value | **Same** |

## Item 1 -- is editing `option_metadata.{hpp,cpp}` a breach?

**No. It is in scope under locked #10, and it is the only correct site.**

- **Locked #10 admits it literally.** #10's In-clause is "`protoc/` (generator,
  type_mapper, emitters)". `protoc/src/option_metadata.cpp` is inside `protoc/` and
  is emitter support -- it is already compiled into `fletcher_plugin_core`
  (`protoc/CMakeLists.txt:27`). No stop-and-ask is triggered.
- **The design's "no change" row was a *porting* statement, not a safety contract.**
  Read in context, the corrections table's point is "already present **and
  compiled** -- nothing to port. Do not edit them", i.e. it is refuting the scoping
  note's claim that these files needed porting from `main`. Contrast the rows that
  *are* contracts and carry their reason: `docs/fletcher-options.md` ("user-facing
  contract is unaltered"), `generator_internal.hpp` / RBA emitter / `golden/*.ipc`
  (locked #3 / #2). The `option_metadata.*` row carries no invariant.
- **The design's actual escaper contract is respected.** Risk #1 states the three
  prohibitions: do not weaken any of the 8 `EscapeCppStringLiteralTest` cases, do
  not move escaping out of the sink, do not switch octal to hex. The fix does none
  of these: escaping stays in `CppSchemaSink`, stays octal, and the cases are
  restated-or-strengthened (item 2). Design risk #7's escape hatch also points this
  way: "If a reviewer finds a divergence, fix the divergence rather than adapting
  the emitter to it."
- **The defect becomes reachable for the FIRST TIME in this item -- so it is this
  item's defect to fix.** I verified at `HEAD`: `EscapeCppStringLiteral` had **zero
  production callers** (`git grep` over `HEAD:protoc/src` and `HEAD:protoc/include`
  returns only its own declaration and definition). GIR-13 section 4 is what wires
  it into `CppSchemaSink::SetMetadata`. Before this item no generated header could
  contain an octal escape at all; after it, they can. The design itself makes "the
  generated header compiles" a GIR-13 acceptance property ("Without it the generated
  header does not even compile"). Shipping a header a consumer cannot build at
  `/W4 /WX` would therefore fail this item's own acceptance, not a neighbouring one.
- **It is the only correct site.** The hazard is a property of the escaper's
  *output*. The alternative -- post-processing in the sink -- would (a) put escaping
  knowledge in two places, against section 4's single-locus rule, and (b) leave the
  function still emitting C4125-hazardous output for any future consumer. The
  escaper's documented job is to be the exact inverse of C++ narrow string-literal
  decoding; C4125-safety belongs to it.
- **The contract change is safe.** The returned body may now contain a
  close-quote/space/open-quote sequence, so it is only valid inside exactly **one**
  pair of quotes. I enumerated every use tree-wide: there is exactly **one**
  production caller, `cpp_backend_schema_visitor.cpp:263-264`, which wraps it in
  `ArrowCharView(" ... ")`. All other occurrences are tests and comments. The new
  header comment states the constraint.
- **`docs/fletcher-options.md` correctly stays unchanged:** it documents no rendered
  C++ spelling and no escaping (grep for `ArrowCharView`, `octal`, `string literal`
  returns nothing), and the *decoded* metadata bytes are unchanged.

## Item 2 -- `test_option_metadata.cpp` vs decision #9

**No violation. The two changed expectations were stale by construction, not
weakened.**

- Decision #9 requires a red-first test that fails for the right reason; it does not
  freeze a test file. The design's "leave the TU byte-for-byte" instruction exists
  to stop an implementer *adapting tests to code*. That is the thing to check, and
  it did not happen:
  - `OctalEscapeIsNotGreedyAcrossFollowingCharacters`: the expectation moves from the
    unsplit form to the split form. Its stated purpose -- the following digit must
    remain a separate character, not be absorbed into the escape -- is not merely
    preserved but made *unconditional*: the split makes absorption impossible even
    for a hypothetically greedy reader.
  - `EmbeddedNulIsEscapedAsThreeDigitOctal`: same shape. Still exactly the
    three-digit octal form; purpose intact.
  Both old expectations literally *encode the buggy output*, so they cannot coexist
  with the fix. Stale, not weakened.
- **Counts (correcting the PM's brief):** the file went 32 to **33** tests; the diff
  has exactly **2 hunks**, touching 2 existing tests, so **30** of the original 32
  are untouched (not 31). None of the other 6 `EscapeCppStringLiteralTest` cases and
  none of the 4 `MetadataRuleParseTest` / 3 `MetadataRuleCompileTest` /
  17 `OptionMetadataTest` cases were altered.
- The added `OctalEscapeIsNeverTerminatedByADecimalDigit` is
  **expectation-independent**: besides fixed cases it scans the rendered output of
  all **256 bytes x 10 digits** and asserts structurally that no three-digit octal
  escape is ever followed by a decimal digit. That is a genuine strengthening -- it
  would catch a future "simplification" of the two expectations back to the buggy
  form.

## Item 3 -- the C4125 claim and the fix, verified independently

I did not take the finding on trust; all four checks used real MSVC (`cl` 17.14,
`/std:c++20`) and the real emitted artifact.

1. **C4125 is real and fatal at `/W4 /WX`.** A probe TU containing the unsplit form
   produced `warning C4125: decimal digit terminates octal escape sequence` plus
   `error C2220`, exit 2. The adjacent-literal (split) form on the next line
   produced **no** diagnostic.
2. **The split is byte-neutral -- confirmed by the compiler, not by argument.** In
   the same probe, `strlen` of both spellings is 2 and both bytes compare equal.
   (Phase-5 escape conversion precedes phase-6 concatenation.)
3. **End-to-end on the shipping artifact.** I regenerated
   `option_metadata.fletcher.pb.h` with a plugin built from the staged tree,
   extracted **all 134** emitted `ArrowCharView(...)` literals into a TU and
   compiled at `/W4 /WX`: **clean**. With the escaper fix reverted, the same pipeline
   yields `C4125` + `C2220` on exactly one line -- the non-ASCII-then-digit one. So
   the defect and the fix are both demonstrated on the real generated header, not
   just in a unit expectation.
4. **Locked #2 intact.** `protoc/tests/golden/*.ipc` untouched and
   `SchemaVisitor.CppAndIpcByteIdentical` green. Stronger: I re-ran my
   pre-fix-cycle **76-artifact no-rules corpus** (`.pb.h`, `.arrow.pb.h`, `.ts`,
   `.ipc` over every `protoc-arrow-bridge` proto) against the post-fix plugin --
   **all 76 byte-identical** to the snapshot taken before the fix cycle. The escaper
   change is unobservable on the no-rules path (builtins contain no octal escapes at
   all), and the fixture change is unobservable there too.

**Red-first for the fix (mutation MUT-A: split removed):** red at four independent
layers -- `EscapeCppStringLiteralTest.OctalEscapeIsNeverTerminatedByADecimalDigit`
(exhaustive scan), `EscapeCppStringLiteralTest.OctalEscapeIsNotGreedyAcrossFollowingCharacters`,
`SchemaVisitor.CppSinkEscapesResolverBytesAndInProcessKeepsThemRaw`
("generated source renders an octal escape terminated by a decimal digit (MSVC
C4125)") and `GeneratorMetadataPlumbing.GenerateFeedsTheResolverToBothEmittedArtifacts`
("MSVC C4125 in emitted header").

## Item 4 -- T4's non-discrimination is structural

**Confirmed structural, not a fixable defect.** T4 asserts
`TraceCpp(msg, resolver) == TraceNano(msg, resolver)`. Both traces come from the
**same** `SchemaVisitor`, so any resolver-plumbing bug perturbs both sides
identically and equality still holds. Mutation **MUT-B** (MAP struct-value site
passes `nullptr`) confirms it: **T4 stayed green**, T2 went red. Making T4 sensitive
would require replacing the A-vs-B equality with an expected literal trace, i.e. a
new trace golden -- a different test shape, and out of scope here. T2 remains the
discriminator, exactly as the design says ("Do NOT present T4 alone as evidence for
section 3 or section 4").

**RR-1 (non-blocking, factual comment defect).** The new
`RecordingSink::DeepCopyMessageStruct` comment claims the resolver-presence
annotation earns T4 that sensitivity: "Log WHETHER the resolver reached this call:
without it, dropping resolver_ at either EmitNodeType deep-copy site leaves T4
green." **MUT-B disproves it -- T4 is green WITH the annotation in place.** The
annotation is harmless and mildly self-documenting, but the comment overstates the
guard in exactly the direction that misleads a future reader into trusting T4.
Reword to say the annotation is documentation only and that T2 is the discriminator.

## Item 5 -- `GeneratorMetadataPlumbing` is toothed

Three tests, all mutation-verified, and they close a real gap: **no `SchemaVisitor.*`
test catches a resolver-to-`nullptr` regression in `generator.cpp`'s emission
calls** (previously only the Conan lane did).

| Mutation | Result |
|---|---|
| **MUT-C** -- `emit_ipc` loop passes `nullptr` to `BuildMessageSchema` | test (a) **RED** -- emitted `.ipc` 1824 bytes vs 2272 expected |
| **MUT-D** -- `GenerateFile` passes `nullptr` | test (b) **RED** on both header assertions; **all 5 `SchemaVisitor.*` tests stayed green**, i.e. non-redundant |
| **MUT-E** -- swallow `ParseMetadataRules` / `Create` errors | test 3 **RED** on all three cases (malformed token, reserved key `field_id`, unresolvable option path) |

The `CapturingContext` (in-memory `GeneratorContext`) is a clean way to drive
`ArrowRowGenerator::Generate` without the filesystem; `generator.hpp` is a
pre-existing, unmodified internal header.

## Integration lane -- re-run after the fix cycle, against the final code

My first review certified a 13:26 lane run; the fix cycle invalidated that, so I
re-checked. The lane **was** re-run: fixture and test edited 13:53, binaries built
13:57, `LastTest.log` written 13:58 -- **90 tests, 89 passed, 0 failed, 1 skipped**,
with `MetadataOptionsTest.JsonValueSurvivesCppStringLiteralEscaping` **OK** against
the new fixture value. That build's `option_metadata.fletcher.pb.h` contains the
split literal, and I compared that build's five metadata artifacts against a
regeneration from the staged tree -- **all byte-identical**. So the lane's evidence
applies to the final code.

**RR-2 (non-blocking, for the record).** No lane compiles at `/W4 /WX` (the
harness's `CMAKE_CXX_FLAGS` is `/MP28 /DWIN32 /D_WINDOWS /GR /EHsc`), so the C4125
*regression guard* is the hand-written source scanner
`RendersOctalEscapeFollowedByDigit` plus the exhaustive escaper test -- not a
compiler. I verified the scanner is faithful by compiling the real emitted literals
with `cl /W4 /WX` (item 3.3), so this is adequate today; noted so that (a) nobody
assumes the integration lane proves it, and (b) if a lane ever adopts `/W4 /WX` it
becomes a direct guard.

**RR-3 (non-blocking).** The design doc was **not** updated for the fix cycle -- it
still lists those five files as "no change", with no revision-4 rework log. The
rationale above is the record the PM asked for; a one-paragraph note in
`plans/GIR-13-option-metadata-on-ir.md` (or in the progress-log entry) pointing here
would keep the design self-consistent.

Also resolved since the first review: **observation N1 is fixed** -- the GIR-13
block in `generator.cpp` now sits *above* the GIR-10 comment, which is intact
directly above `ValidateBackendsSupportFields`, and the new comment explicitly
disclaims any ordering claim about `ValidateNoUnsupportedIr`.

## Re-review verification summary

- protoc unit suite: **92 tests, 91 passed, 0 failed, 1 skipped** (`CaptureGoldens`).
  Suites: TypeMapper 36, Ir 4, SchemaBuilder 9, SchemaVisitor 6,
  **GeneratorMetadataPlumbing 3**, EscapeCppStringLiteral **9**,
  MetadataRuleParse 4, MetadataRuleCompile 3, OptionMetadata 17.
- Conan integration lane: 90 / 89 passed / 0 failed / 1 skipped; artifacts verified
  byte-identical to the staged tree.
- Locked #2: goldens untouched; 76-artifact no-rules corpus byte-identical across the
  fix cycle.
- clang-format 18.1.3: `option_metadata.hpp`, `option_metadata.cpp`,
  `test_option_metadata.cpp`, `test_metadata_options.cpp`, `generator.cpp` all
  **0 violations**; `test_schema_visitor.cpp` 9, equal to `HEAD`'s 9 (known env
  gotcha). **No new drift.**
- Mutations MUT-A through MUT-E all reverted; `git diff` (worktree vs index) empty;
  build tree rebuilt to match the staged sources.
