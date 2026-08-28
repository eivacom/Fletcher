# DICT-1.5 — step-4a architecture-conformance review (adversarial)

**Verdict: CONFORMS.** 0 blocking. 5 non-blocking items (M1–M5) + 2 adjudicated
deviations, both **accepted**.

- Reviewed: staged tree, `git diff --cached HEAD`, `HEAD = 9c1cdc4`, branch
  `feature/dictionary-option`.
- Against: `plans/DICT-1.5-backend-support-guard.md` (APPROVEd cycle 2; revised body
  operative, D1's closed-set table binding), `plans/DICT-locked-decisions.md`
  (#5, #9, #10, #11), GIR locked #3, `docs/dictionary-option-spec.md` §4, §7.1.
- Working tree is fully staged (`git diff` empty, no untracked files).
- Not run: build/ctest. No plugin binary or current configured build exists on this
  box and Conan deps are not warm; every claim below is settled statically against
  the tree, and dynamic red-first/green evidence remains step-3/PM's.

---

## 1. No false negatives (the whole point) — VERIFIED

**Carrier writers, re-verified independently of the step-2 review.** Repo-wide
(`protoc/`, `core/`, excluding `protoc/tests/`), `facts.dictionary` is *written* in
exactly two places: `ir.cpp:62` (inside `BaseFacts`) and `ir.cpp:295` (inside
`ApplyDictionaryFacts`). Everything else is a read or a comment
(`ir.hpp:156`, `option_reader.hpp:76`, `ir.cpp:294`, `ir.cpp:332`) plus this item's
one new read at `generator.cpp:1735`. `ReadFieldDictionaryOption`
(`option_reader.cpp:146-192`) remains the sole reader of the option, and `BaseFacts`
its sole caller in `ir.cpp`.

**The 20 `BaseFacts(` call sites in `ir.cpp` are exactly**
`{77, 235, 251, 267, 314, 368, 398, 402, 416, 420, 436, 457, 468, 471, 482, 495,
506, 520, 545, 556}` — identical to D1's closed-set table, nothing missing, nothing
invented.

**The implemented predicate reaches all of them.** `ir::NodeKind` has exactly six
members (`ir.hpp:29-36`): SCALAR, LIST, FIXED_SIZE_LIST, STRUCT, MAP, UNSUPPORTED.
`FindDictionaryField` (`generator.cpp:1731-1751`) tests `node.facts.dictionary`
**before** any kind dispatch, then descends LIST→element, FIXED_SIZE_LIST→element,
MAP→key then value, STRUCT→every field. SCALAR and UNSUPPORTED are leaves that need
no edge. So every child-bearing kind has an edge and every node kind is checked:

| D1 row | Checked against the tree |
|---|---|
| `MakeUnsupported:77` | root/child + the pre-dispatch facts test → hit even though UNSUPPORTED is a leaf. |
| `TryBuildWkt:235/:251/:267` | `ir.cpp:264-275`: wrapper WKT returns a `SCALAR` **root** whose `facts = BaseFacts(field)`. Locked #9's `StringValue [(fletcher.dictionary)]` is a **root hit**, pinned by `coverage_dictionary_wkt.proto`. |
| `BuildSingularMessage:545` | root STRUCT, incl. a `flatten_field` wrapper field — see §2. |
| `BuildSingularScalarOrEnum:556` | root SCALAR. |
| `BuildRepeatedScalarOrEnum:468/:471` | root LIST + one element edge. |
| `BuildRepeatedMessage:457` | root LIST. |
| `BuildMapNode:482/:495/:506/:520` | root MAP + key/value edges (both implemented). |
| `BuildFlattenedRepeated:368/398/402/416/420/436` | root LIST + element edges through the `MakeListOf` chain; the intermediate default-facts levels are traversed because the walk never stops on `false`. |
| `BuildFlattenedSingular:314` + `ApplyDictionaryFacts:293-304` | the returned root, plus its element when LIST (`ir.cpp:299-303`). |
| `BuildStructVariant:571` | `MakeStructNode` (`ir.cpp:191-195`) builds `BuildStructVariant` **eagerly**, which calls `BuildFieldIr(f)` for **every** field (`ir.cpp:562-575`) → struct-field edge, transitively. **Now pinned** — see adjudication (b). |

**`TryBuildWkt` specifically** (the brief's named worry): `coverage_dictionary_wkt.proto`
declares `google.protobuf.StringValue label = 1 [(fletcher.dictionary) = {}]`.
`BuildFieldIr` step 5 (`ir.cpp:600-603`) tries `TryBuildWkt` first, `WrapperFor`
matches `google.protobuf.StringValue` (`ir.cpp:223`), and the returned node carries
`BaseFacts(field)` at `:267` with `proto_full_name = …DictWktGuard.label`. Root hit,
matching `EXPECT_FIELD=DictWktGuard.label`. Reachable and fixtured.

**Imported declarations really are readable.** `ReadFieldDictionaryOption` resolves the
extension from `field->file()->pool()` (`option_reader.cpp:159`), which is the pool
protoc builds from the whole `CodeGeneratorRequest` including transitive imports, and
`coverage_dictionary_struct_child_inner.proto` imports `fletcher/options.proto` itself.
So the struct-child fixture's declaration is genuinely readable and the test is not a
false red.

**Fail-soft direction preserved.** A declared-but-unparseable payload still yields
`DictionaryOption{}` (`option_reader.cpp:175`), i.e. `dictionary = true` → the guard
fires. Safe direction, as D6 claims.

**The only unreachable declaration** is the disclosed §7.1 gap 2
(`repeated W xs`, message-level-flatten `W`, inner-declared): `BuildFlattenedRepeated`
(`ir.cpp:363-437`) never calls `BuildFieldIr(inner)`, and `W` is skipped by the
message loop's `IsFlattenedWrapper` term. Confirmed safe by construction —
`GatherFieldsImpl`'s inline branch requires `!fd->is_repeated()`, so schema emission
consumes the identical node and drops the declaration too. Recorded in the spec by
this diff.

**Skip predicate is still identical to the emit loops'** (`IsRecursive(msg) ||
IsFlattenedWrapper(msg)`, `generator.cpp:1773`), unchanged by this diff, and
`IsFlattenedWrapper` is `HasMessageFlatten(msg) && msg->field_count() == 1`
(`type_mapper.cpp:304-306`).

**Conclusion:** `Guarded ⊇ EmittedAsDictionary` holds for the *implemented* predicate,
not just for the design's pseudocode. No false negative found beyond the disclosed and
now spec-recorded gap 2.

## 2. The wrapper-declared fixture is genuinely red-if-broken — VERIFIED

`coverage_dictionary_field_flatten.proto` is exactly D8's revised spelling:

```proto
message DictFfGuard {
  DictFfWrap w = 1 [(fletcher.flatten_field) = true, (fletcher.dictionary) = {}];
}
message DictFfWrap { string kind = 1; int32 n = 2; }   // no dictionary inside
```

Route, traced in the tree: `BuildFieldIr(w)` → step 5 → `TryBuildWkt` misses →
`BuildSingularMessage` → `DynamicWktUnsupportedReason` nullopt →
`HasMessageFlatten(DictFfWrap)` **false** (no message-level `flatten`) →
`IsRecursive` false → `MakeStructNode` + `node.facts = BaseFacts(field)` at
`ir.cpp:545`. Root hit, naming
`integration.coverage.dict_guard_field_flatten.DictFfGuard.w`;
`EXPECT_FIELD=DictFfGuard.w` matches (regex dots match literal dots; the distinct
package cannot interfere).

Discriminating, as required: `DictFfWrap` has no message-level flatten so it is *not*
`IsFlattenedWrapper` and **is** visited directly by the guard's message loop — and its
two fields carry no option. Both emit walks `continue` past `w`
(`GatherFieldsImpl` / `BuildFlattenedFieldListImpl`), so there is no projection-level
route. Therefore `BaseFacts(w)` at `ir.cpp:545` is the **sole** route: break it and no
node in the file carries the fact → plugin exits 0 → the script's `if(rc EQUAL 0)`
FATALs. Genuinely red-if-broken.

Nothing intercepts the shape earlier: `ValidateNoUnsupportedIr` sees only two SCALAR
children, no UNSUPPORTED.

By contrast `coverage_dictionary_flatten.proto` (inner-declared, `DictFlatWrap` **is**
`IsFlattenedWrapper` → skipped) is red-if-`ApplyDictionaryFacts`-broken, as D8 claims.
Both fixtures earn their place; neither is green-regardless.

## 3. `EXPECT_ARTIFACTS` is not vacuous — VERIFIED (one caveat, M2)

Script (`run_backend_guard_check.cmake`):

- `EXPECT_ARTIFACTS` is **required** whenever `EXPECT_SUCCESS` is truthy, and
  `EXPECT_MESSAGE` is required otherwise — so neither mode can run with **no**
  assertion. `EXPECT_MESSAGE` was correctly removed from the unconditional
  `foreach(_req …)` and re-required in the `else()` branch.
- Success branch asserts `rc EQUAL 0`, then for each `|`-split basename: `EXISTS`
  **and** `file(SIZE …)` non-zero. Both new FATAL branches print stdout+stderr.
  `if(EXPECT_SUCCESS)` is the mandated bare truthiness probe; `EXPECT_FIELD` uses the
  mandated `if(DEFINED … AND NOT … STREQUAL "")`. All of D8 script change 1+2 and
  step-2 note 6 discharged. The `|` convention is documented in the header (residual #3).
- **The three named artifacts are genuinely emitted under `--fletcher_opt=ipc,ts`**:
  `<stem>.fletcher.pb.h` unconditional (`generator.cpp:1916-1921`, `OutputFilename`
  `:50-52`); `<stem>.fletcher.arrow.pb.h` under `!schema_only` with non-empty view
  content (`:1924-1932`, `ViewOutputFilename` `:1468-1470`) — `DictScalarGuard` is a
  non-skipped message so `has_views` is true, i.e. not a conditionally-absent artifact
  that would red the test for the wrong reason; `<stem>.fletcher.ts` unconditional
  under `emit_ts` (`:1935-1942`, `TsOutputFilename` `:1412-1414`). Basenames land
  directly in `OUT_DIR` because `-I ${PROTO_DIR}` canonicalises `file->name()` to the
  bare fixture name. No false red.

Caveat M2 below: `OUT_DIR` is never cleaned.

## 4. Ordering is pinned, not incidental — VERIFIED

`ValidateNoUnsupportedIr` is the first statement of `Generate()`
(`generator.cpp:1850`); `ValidateBackendsSupportFields` is called at `:1913`; the
first `context->Open` is at `:1919`. The predicate lives *inside* the latter, so it
inherits the ordering with no code change (D4), and no artifact can be opened before
the verdict. `GenerateAll` (`:1996-2028`) still loops `Generate()` before opening the
shared `__rba.fletcher.rs`.

The test pins it rather than passing incidentally.
`coverage_dictionary_unsupported.proto` declares the dictionary field **first**
(`category = 1`) and the `Any` field **second** (`payload = 2`), and asserts
`EXPECT_MESSAGE=google.protobuf.Any is dynamically typed` — which only
`ValidateNoUnsupportedIr`'s `MakeUnsupported` reason can produce (`ir.cpp:88-89`).
Two distinct plausible drifts both red it: (a) hoisting the predicate into a pass
before `:1850`, (b) moving it into `ValidateNoUnsupportedIr`'s own field loop — in
both cases field 0's dictionary is complained about before field 1's `Any` is ever
classified, so the expected substring is absent. Field ordering (dictionary before
`Any`) is what makes it a pass-ordering test rather than a field-ordering test
(step-2 residual #6, honoured).

## 5. No parallel mechanism; GIR-10 behaviourally untouched — VERIFIED

- The predicate is a second `if` **inside** the existing per-field body of
  `ValidateBackendsSupportFields` (`generator.cpp:1774-1792`). No new function is
  called from `Generate()`. One `ir::BuildFieldIr` per field, shared with the GIR-10
  check; the added cost is one pointer walk over an already-materialised tree.
  Signature unchanged. Early-out `if (!emit_accessor && !emit_rust) return true;`
  untouched — which is what makes the no-false-positive criterion hold by construction.
- `FindDictionaryField` is inside `generator.cpp`'s anonymous namespace
  (`636`–`1798`), placed immediately after `FindScalarLeafNestedList` as D2 requires,
  and mirrors that predicate's if-chain/`std::get`/return-type idiom. File-local ⇒ RIR
  deletes one function + one `if` + the D8 tests atomically (locked #11's removal
  contract).
- Empty-name hazard handled exactly as D2 mandates:
  `if (auto e = FindDictionaryField(node))` tests the *optional*, then
  `e->empty() ? msg->field(i)->full_name() : *e`. No `assert(!e->empty())` (residual #5).
- **GIR-10's two tests:** `add_test` blocks not edited (diff is pure insertion after
  `endforeach()` at CMakeLists `:530`); they pass none of the new variables, so
  `if(EXPECT_SUCCESS)` is false, `EXPECT_MESSAGE` is still required and asserted, and
  `EXPECT_FIELD` is skipped. The one script reordering — `string(CONCAT combined …)`
  moved above the `if(rc EQUAL 0)` FATAL — is behaviour-neutral (`combined` is only
  read after that block). Their fixture `coverage_scalar_nested.proto` carries no
  dictionary option (repo-wide grep: the only `.proto` files using `fletcher.dictionary`
  are `options.proto` and the six new fixtures), and the nested-list check runs first
  in any case. **No regression risk to any wired generation unit**: no wired proto
  anywhere in the repo carries `(fletcher.dictionary)`, so the `coverage.proto`
  `ipc,accessor,ts,rust` unit and every other `accessor`/`rust` build stay green; and
  no protoc unit test invokes `ArrowRowGenerator::Generate` at all.
- Error text is D3/D5 **verbatim**, including the deliberate "accept dictionary
  fields" (not "do support them"). `EXPECT_MESSAGE=dictionary.*RecordBatch accessor /
  Rust backend.*round RIR` matches it and **cannot** be satisfied by GIR-10's message,
  which contains no `dictionary` token. (See M5 for the reverse direction.)

## 6. Unconditional descent; `DictionaryModifier` not read — VERIFIED

`FindDictionaryField` has no early return on `false`: the only returns are a hit or
the terminal `std::nullopt` after every edge is exhausted, so an interior
`dictionary == false` (spec §7.1 / `ir.cpp:53-58`: intermediate `MakeListOf` levels
keep DEFAULT facts) never truncates the search. The `MAP` branch checks key **then**
value; the `STRUCT` branch iterates all fields. Recursion terminates because
`IsRecursive` messages become UNSUPPORTED leaves inside `BuildFieldIr` and the tree is
already materialised and finite.

`ir::DictionaryModifier`: independently confirmed. All six occurrences in the staged
diff are prose/comments (five in `plans/DICT-1.5-backend-support-guard.md`, one in
`generator.cpp`'s new comment block); `grep` finds no code reference in
`generator.cpp`. The only carrier read is `node.facts.dictionary`. Locked #5 honoured.

The FLAGGED-CONFLICT paragraph, the PLACEMENT paragraph and the FORWARD-COMPAT
paragraph D2 mandated are all present in the shipped comment.

## 7. Scope — VERIFIED

- **Zero diff** to `protoc/src/recordbatch_accessor_emitter.{hpp,cpp}` and all Rust
  accessor emission (D7, GIR locked #3, locked #11). Also zero diff to
  `protoc/include/ir.hpp`, `protoc/src/ir.cpp`, `protoc/src/option_reader.cpp`,
  `protoc/src/cpp_backend_schema_visitor.cpp`, `protoc/src/type_mapper.cpp`.
  `type_mapper.hpp` is comment-only (D9's guard-note extension; no declaration or
  behaviour changed).
- Files-to-touch matches the design exactly, plus the two adjudicated additions below.
  No file outside the design's list is touched.
- `HasFieldDictionary` is **not** called from `generator.cpp` — the recorded
  story-wording deviation (Risk 6) is real and implemented as designed.
- Fixture conventions hold: SPDX line 1 + `Copyright (C) 2026 The Fletcher Authors`
  line 2 in all seven files (the license CI scans `head -n 10` and does not exempt
  `.proto`), `syntax = "proto3"`, distinct `package integration.coverage.dict_guard*`
  per fixture, and the "COMMITTED BUT NOT WIRED … invoked only by ctest <name>"
  comment. There is no `file(GLOB)` over `proto/` and no generation unit references
  any of them, so they stay unwired.
- `OUT_DIR` is the dedicated `${CMAKE_CURRENT_BINARY_DIR}/dictionary-guard-generated`,
  outside `GENERATED_DIR` (`:75`) where `check_no_value_or_die.cmake` and
  `validate_generated_ipc.cmake` scan.
- No duplicate `add_test` names in the file. clang-format 18.1.3
  `--output-replacements-xml` yields 0 replacements for both touched C++ files.
- Working tree fully staged; nothing unstaged or untracked (the DICT-1 process note
  "stage before dispatching any review" is honoured).

## Spec §7.1 edit — faithful, and not weakened for its real consumers

All three D9 edits are present and are scoping, not weakening:

1. **Gap 1** — replaced "Enforcement must be a front-end **descriptor** walk" with
   D9's prescribed wording *"a walk rooted at each message's own **declared** fields
   (descriptor or `ir::BuildFieldIr`) rather than a check on the projection's output"*,
   explicitly scoped to DICT-2 and explicitly leaving the choice to DICT-2. The false
   mandate locked #10 warns against is removed without removing the requirement. The
   DICT-1.5-is-unaffected note is accurate (verified in §2 above).
2. **Gap 2** — records that the DICT-1.5 guard cannot see the inner-declared
   `repeated`-flatten declaration and why that is safe (`GatherFieldsImpl` requires
   `!fd->is_repeated()` ⇒ emission consumes the identical node). Matches the tree.
3. **Closing rule** — the prohibition on "some node has `dictionary = true`" **stays
   binding** for kind/emission consumers *and* is explicitly extended to DICT-2's
   legality gate, with locked #9's reasoning spelled out ("a scalar dictionary declared
   inside a struct child stays legal … gate on the field's own mapped `FieldKind`, not
   on an ancestor's"). The exception is granted only to a **backend-availability
   rejection** predicate and is explicitly denied to kind/emission consumers and to
   DICT-2. This is the bounded form the cycle-2 reviewer applied inline; the
   implementation is faithful to it. Nothing in the edit lets an emitter OR over a
   subtree, and the interior-`false` warning paragraph above it is untouched.

Locked #9 is not weakened elsewhere either: the guard gates on `facts.dictionary`, not
on `FieldKind`, so it makes no legality judgement and cannot pre-empt DICT-2's. The
wrapper-WKT shape is treated as the valid shape #9 says it is, and is fixtured as a
**true positive** for the availability guard. The deliberate over-rejections
(dictionary on a `flatten_field` wrapper field) are Risk 3's disclosed and accepted
behaviour, not a #9 violation.

---

## Adjudication of the two implementer-flagged deviations

**(a) New `## (fletcher.dictionary)` section in `docs/fletcher-options.md` — ACCEPTED.**
D9 assumed the section existed; it did not. DICT-1 added only the registry row
(`| 50001 | FieldOptions | fletcher.dictionary |`) and the range note; the file's `##`
headings confirm no dictionary section before this diff. Adding the section is the
minimum way to deliver D9's required v1-limitation line, so this is
deviation-by-necessity, not scope creep: 19 lines, one file, no other doc touched, and
it is the file the design already listed in Files-to-touch. Content checked for
accuracy against the tree: extendee/number/type and `index_type`+`ordered` match
`protoc/include/fletcher/options.proto:33-45`; "presence is the trigger" matches
locked #1; "an `int32` index" matches locked #4; and the limitation paragraph reuses
D5's deliberate **"accept dictionary fields today"** rather than "support", so it will
not need editing when DICT-3 lands. No conflict with the registry table. Not blocking,
not a scope breach.

**(b) `coverage_dictionary_struct_child{,_inner}.proto` +
`GenErrors.DictionaryRejectedBy_accessor_structChild` — ACCEPTED, and genuinely
load-bearing.** The cycle-2 reviewer's residual #1 offered "add the two-file fixture
**or** disclose the uncovered edge"; the stronger option was explicitly required, and
this is it. Verified it isolates the edge as claimed: `DictStructChildInner` lives in
an imported file that is never passed to protoc, so `OrderedMessages(file)` drops it
at `generator.cpp:175` (`msg->file() != file`) and the guard's message loop never
visits it; `DictStructChildGuard.i` maps via `BuildSingularMessage` →
`MakeStructNode` → `BuildStructVariant` → `BuildFieldIr(k)` → `BaseFacts(k)`
(`ir.cpp:556`), so `FindDictionaryField`'s **STRUCT** branch is the sole route.
Delete that branch and exactly this test reds; before the addition, deleting it left
all seven tests green. `EXPECT_FIELD=DictStructChildInner.k` is the fully-qualified
tail and the distinct package cannot break it. The option in the imported file is
readable (pool-based resolution, `option_reader.cpp:159`), so this is not a false red.
Both files carry the full fixture-convention header and stay unwired. Within the
item's scope (it is a test for this item's own predicate) and it closes the last
undisclosed hole in D1's proof. Not blocking.

---

## Non-blocking items

**M1 — fixture text deviates from D8's table: `optional` dropped.** D8 spells
`DictScalarGuard { optional string category = 1 [...]; int32 seq = 2; }` and
`coverage_dictionary_unsupported.proto`'s `optional string category = 1 [...]`; both
shipped fixtures use bare `string category = 1`. Harmless — `BuildFieldIr` uses
`real_containing_oneof()` (`ir.cpp:585`), so a proto3 synthetic-optional oneof would
*also* have been fine, and either spelling reaches `BaseFacts` at `ir.cpp:556`; only
`facts.nullable` differs, which no assertion touches. Reported because it is a literal
divergence from the approved fixture table. Either restore `optional` or note the
simplification in the progress log.

**M2 — `EXPECT_ARTIFACTS` is airtight only on a clean build tree.**
`dictionary-guard-generated` is shared by all seven tests and is never cleaned
(`file(MAKE_DIRECTORY)` only). On an incremental re-run, artifacts left by a previous
green run of `GenErrors.DictionaryAcceptedWithoutRbaBackends` would satisfy
`EXISTS` + non-zero-size even if the plugin regressed to "exit 0, emit nothing" —
i.e. the exact vacuity step-2 B3 exists to close survives locally (CI's fresh build
dir is fine). The design only mandated exists+non-empty, so this is design-conformant;
still worth one line: `file(REMOVE_RECURSE "${OUT_DIR}")` before `MAKE_DIRECTORY` in
the `EXPECT_SUCCESS` path, or a per-test `OUT_DIR`.

**M3 — the `ipc` artifact is unasserted** (step-2 residual #2, a "consider", not
taken). `--fletcher_opt=ipc,ts` is under test but only the `.pb.h`, `.arrow.pb.h` and
`.ts` are checked; `coverage_dictionary.DictScalarGuard.ipc` (`IpcOutputFilename`
`:66-68`) is not. Cheap to add and it is the token whose output most directly becomes
DICT-3's dictionary schema.

**M4 — the rewritten leading comment says the other backends "support both shapes".**
For the dictionary shape D5 deliberately chose **"accept"** over "support", because
until DICT-3 the edge/view/IPC/TS backends emit a dictionary field value-typed.
The user-facing error text gets this right ("accept dictionary fields"); only the
internal comment at `generator.cpp:1765` carries the forward-dated wording D5 argued
against. One-word fix.

**M5 — GIR-10's `EXPECT_MESSAGE` is the family substring, not the shape-specific one.**
It is `not yet supported by the RecordBatch accessor / Rust backend`
(CMakeLists `:525`), which the *dictionary* error also contains — so the step-2
review's "the token `dictionary` appears in only one of the two messages, good
discrimination" holds only in the DICT→GIR direction. No live consequence (GIR-10's
fixture carries no dictionary option, and the nested-list check runs first), and this
item leaves those tests behaviourally identical, so it is not a DICT-1.5 deviation.
Noted so nobody relies on the overstated claim later; tightening GIR-10's expectation
to `scalar-leaf nested lists.*round RIR` would make the two families mutually
exclusive in both directions.

**Nits (no action needed):** the FORWARD-COMPAT comment cites
`generator.cpp:1612-1617` for `FindUnsupportedIr`'s note, which now sits at
~`:1616-1618`. The spec's closing rule retains "must **in all cases** gate …"
immediately before the clause that scopes it to kind/emission — reads as a mild
self-contradiction; dropping "in all cases" would be cleaner.
`docs/fletcher-options.md:243` is ~143 chars where the file otherwise wraps near 76
(no markdown linter in CI, so cosmetic only).
