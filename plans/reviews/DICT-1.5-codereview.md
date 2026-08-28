# DICT-1.5 — step-4b code review

**Reviewer:** independent code review (fresh context)
**Diff base:** `HEAD` = `9c1cdc4`, staged tree (`git diff --cached HEAD`)
**Branch:** `feature/dictionary-option`
**Verdict:** **APPROVE with 2 should-fix items.** No blocking findings. The guard
predicate is correct on every reachable route I could construct, the front-end
ordering is right, nothing is emitted before the failure, and the harness's
truthiness handling is right. The two should-fix items are both test-strength,
not behaviour: one untested (but reachable) recursion edge, and a stale-artifact
hole in the new success mode's non-vacuity claim.

## What I actually executed (not just read)

Built the plugin (`cmake --build protoc/build --target fletcher-protoc --config
Release`, clean) and ran the real `protoc` + plugin by hand:

| Invocation | Result |
|---|---|
| 6 rejecting fixtures x `accessor` | all exit 1, each naming the right field FQN (`…DictScalarGuard.category`, `…DictWktGuard.label`, `…DictFlatWrap.kind`, `…DictFfGuard.w`, `…DictStructChildInner.k`, and the *Any* error for the unsupported fixture) |
| `coverage_dictionary.proto` x `rust` | exit 1, same message |
| `coverage_dictionary.proto` x `ipc,ts` | exit 0; all 3 asserted artifacts + the `.ipc` present and non-empty |
| OUT_DIR after all 6 rejecting runs | **empty** — confirms "fails before any artifact is emitted" |
| `run_backend_guard_check.cmake` GIR-10 path (`coverage_scalar_nested.proto`, `accessor`, only `EXPECT_MESSAGE`) | passes with the identical STATUS line — existing path unchanged in effect |
| script negative controls: `EXPECT_SUCCESS` without `EXPECT_ARTIFACTS`; bogus artifact name; reject mode without `EXPECT_MESSAGE`; `EXPECT_SUCCESS` on a fixture the plugin rejects | all four FATAL correctly (lines 54 / 100 / 61 / 86) |
| `clang-format --dry-run --Werror` on both touched C++ files (18.1.3) | clean |

Extra probes I wrote to attack the false-negative axis (see S1 / N5):
`repeated ImportedInner xs` → **rejected** (LIST→STRUCT edge works);
`map<string, ImportedInner> m` → **rejected** (MAP→STRUCT edge works);
`repeated FlatWrap xs` with the dictionary on the *message-level flatten
wrapper's inner field* → **exit 0, accessor emitted** (the documented gap; the
emitted schema is a plain `list<utf8>` and the accessor reads `list<utf8>`, so no
mis-read exists today).

## Predicate correctness (brief items 1 and 2)

- **Node-kind coverage is exhaustive.** `NodeKind` = {SCALAR, LIST,
  FIXED_SIZE_LIST, STRUCT, MAP, UNSUPPORTED}. SCALAR and UNSUPPORTED carry no IR
  children (`MakeUnsupported` sets only facts + reason); the other four are all
  descended. Matches `FindUnsupportedIr` edge-for-edge.
- **Every construction route is reachable from the field root.**
  `facts.dictionary` is written only at `ir.cpp:62` (`BaseFacts`) and `ir.cpp:295`
  (`ApplyDictionaryFacts`). All 20 `BaseFacts(` call sites produce nodes that end
  up in the returned tree as the root, a LIST/FIXED_SIZE_LIST element, a MAP
  key/value, or a `BuildStructVariant` child — all of which the walk visits. No
  BaseFacts-built node is discarded.
- **Interior `false` is not treated as authoritative.** The code returns only on
  `true` and otherwise descends unconditionally; there is no `else`, no early
  `return std::nullopt` on a false, and no depth cut-off. Correct per spec §7.1 /
  the `BaseFacts` placement rule.
- **Termination.** The IR is a finite tree: `BuildSingularMessage`,
  `BuildRepeatedMessage`, `BuildFlattenedRepeated` and `BuildMapNode` all convert
  recursive message types to `UNSUPPORTED` before recursing (`IsRecursive`), so
  `BuildStructVariant` cannot cycle and the walk cannot either. Depth is bounded
  by proto nesting depth, and the walk is strictly shallower than the
  `BuildFieldIr` that already built the tree, so it adds no new stack risk.
- **Scope matches the emitter.** `ValidateBackendsSupportFields`' message loop uses
  `IsRecursive(msg) || IsFlattenedWrapper(msg)`, byte-identical to
  `recordbatch_accessor_emitter.cpp:691/745`'s skip predicate and to
  `GenerateViewFile`/IPC. So the guard cannot over-reject on a message the accessor
  never emits, and cannot under-reject on one it does.
- **Ordering / no-emission.** The call sits after `ValidateNoUnsupportedIr` (top of
  `Generate()`) and `ParseMetadataRules`, and before the first `context->Open()`.
  Verified empirically (OUT_DIR empty). `*error` is set and `false` returned — no
  throw, no `std::exit`, no partial write.
- **Null / variant safety.** `msg->field(i)` is never null; `FindDictionaryField`
  is only reached with a tree built by `BuildFieldIr`. `std::get<>` + unchecked
  `*element` mirrors the two neighbouring walks; I checked every site that sets
  `kind` to LIST/MAP/STRUCT also sets the matching variant (LIST only via
  `MakeListOf`; the MAP node's variant is assigned on the single success path,
  every early return producing a *different* `MakeUnsupported` node), so
  `bad_variant_access` is unreachable today. See N6.
- **Determinism.** Pre-order, declaration order, key-before-value; error text is
  stable run to run. The nested-list complaint deliberately wins over the
  dictionary complaint within one field, as documented.
- **Cost.** One `BuildFieldIr` per top-level field, *shared* with
  `FindScalarLeafNestedList` — no second build. The added walk is O(nodes) with no
  allocation beyond the returned name. Nothing materially worse than expected (see
  N8 for a pre-existing observation).

## Findings

### Should-fix

**S1 — the LIST / FIXED_SIZE_LIST / MAP recursion edges are untested; delete all
three branches and every one of the 8 tests still passes.**
*Confidence: high. Severity: should-fix (test strength).*
This is the same class of gap the step-2 review's residual item #1 raised for the
STRUCT edge and that this revision correctly closed with
`coverage_dictionary_struct_child.proto`. No fixture has a `repeated` or `map`
field at all. Reachable shapes exist and matter: I verified with the real plugin
that `repeated <imported msg with a dictionary field>` and
`map<string, <imported msg with a dictionary field>>` are rejected **only** via
LIST→STRUCT / MAP→STRUCT descent (an in-file declaring message would be visited
directly by the message loop, which is why the imported form is the discriminating
one). Cost to close: one fixture that re-imports the *existing*
`coverage_dictionary_struct_child_inner.proto` with
`repeated …DictStructChildInner xs = 1;` (and optionally a `map` sibling) plus one
`add_test` with `EXPECT_FIELD=DictStructChildInner.k`. ~20 lines, no new inner
file. Alternative, if declined: disclose in D8 that these three edges have no
ctest and why that is accepted — the standard the plan set for itself in residual
item #1.

**S2 — `EXPECT_ARTIFACTS` is satisfiable by artifacts a *previous* run left behind,
so the success mode's advertised non-vacuity does not hold on a reused build
tree.**
*Confidence: high (demonstrated). Severity: should-fix.*
The script does `file(MAKE_DIRECTORY "${OUT_DIR}")` but never cleans it, and all 8
dictionary tests share `${CMAKE_CURRENT_BINARY_DIR}/dictionary-guard-generated`.
Demonstration: I hand-created `nope.h` in the out dir and ran the success mode with
`-DEXPECT_ARTIFACTS=nope.h` — it reported *"correctly succeeded with all expected
artifacts present: nope.h"*. So a regression that exits 0 while emitting nothing
still passes `GenErrors.DictionaryAcceptedWithoutRbaBackends` on any build tree
where the test has run once before — exactly the hole the docblock at lines 29-33
and 92-95 says the mode closes. (CI checks out fresh, so this bites local
iteration and re-runs, not the merge gate — hence should-fix, not blocking.)
Fix: `file(REMOVE_RECURSE "${OUT_DIR}")` immediately before the existing
`file(MAKE_DIRECTORY …)` (OUT_DIR is always a build-dir path supplied by
`add_test`), or delete just the `EXPECT_ARTIFACTS` entries before invoking protoc.
Giving the success test its own OUT_DIR is worth doing anyway so a future second
`EXPECT_SUCCESS` test cannot see the first one's output.

### Nits / P2

**N1 — GIR-10's two tests are behaviourally untouched (verified), but their
assertion is now less specific than it looks.** Their
`EXPECT_MESSAGE=not yet supported by the RecordBatch accessor / Rust backend` is a
substring of the *new* dictionary error too, and they pass no `EXPECT_FIELD`. They
therefore no longer pin *which* of the two guard predicates fired. Not a
regression in the tests' current outcome (their fixture has no dictionary field),
but now that `EXPECT_FIELD` exists, adding
`-DEXPECT_FIELD=ScalarNestedCoverage.nested_scalar_lists` (and/or putting
`scalar-leaf nested lists` in EXPECT_MESSAGE) is a two-line hardening of the family
this item is extending. *Confidence: high.*

**N2 — `coverage_dictionary_struct_child.proto` has an unused
`import "fletcher/options.proto";`.** Observed on every run:
`coverage_dictionary_struct_child.proto:17:1: warning: Import fletcher/options.proto
is unused.` — that warning lands in the `combined` stream the assertions match
against (harmless today, noise forever). The plan's "import in all of them"
convention does not apply to the one fixture that declares no option. Drop the
import. *Confidence: high.*

**N3 — step-2 residual item #2 not taken:** `ipc` is one of the two tokens under
test in `GenErrors.DictionaryAcceptedWithoutRbaBackends`, but
`coverage_dictionary.DictScalarGuard.ipc` is absent from `EXPECT_ARTIFACTS`. I
confirmed the plugin does emit it (520 bytes). One token to add.
*Confidence: high.*

**N4 — the call-site comment now under-describes the call.** `generator.cpp`
~1908-1913 still reads "GIR-10 (locked #3): reject scalar-leaf nested lists …"
while the pass it guards now rejects two shapes. The function's own docblock and
`type_mapper.hpp` were both updated; this one was missed. *Confidence: high.*

**N5 — the one accepted false negative, verified live, with a forward-compat
caveat.** `repeated W xs` where `W` is a message-level flatten wrapper whose
**inner** field declares the dictionary exits 0 and emits the accessor
(`BuildFlattenedRepeated` never calls `BuildFieldIr(inner)`). The diff documents
this in spec §7.1 gap 2 and the safety argument holds *today*: I confirmed the
emitted schema/header is a plain `list<utf8>`, so the accessor reads what was
emitted and no mis-read exists. The caveat worth one sentence somewhere DICT-3
will read: the safety rests entirely on the emitter consuming the *same IR node*.
If DICT-3 ever derives dictionary-ness from the descriptor (e.g.
`HasFieldDictionary`) rather than `facts.dictionary`, this shape silently becomes a
real mis-read with no test covering it. *Confidence: medium-high on the forward
risk; the present-tense claim is verified.*

**N6 — `std::get<>` + unchecked `*element` vs `ApplyDictionaryFacts`' `get_if` +
null check.** In a protoc plugin an uncaught `std::bad_variant_access` is a crash
rather than a `*error`. I verified it is unreachable on today's construction paths,
and matching the two neighbouring walks (`FindUnsupportedIr`,
`FindScalarLeafNestedList`) is the stronger consistency argument — recording it only
so the asymmetry with `ir.cpp` is a known choice. *Confidence: high that it is
unreachable; nit only.*

**N7 — plan/fixture drift.** Plan D8's table specifies
`optional string category = 1 [...]` for `coverage_dictionary.proto`; the committed
fixture has no `optional`. Behaviourally irrelevant here (presence only changes
nullability, and the guard is kind-agnostic), but the plan table is now inaccurate
for anyone reading it as the fixture spec. *Confidence: high.*

**N8 — per-field cost observation (pre-existing, not introduced).**
`ValidateNoUnsupportedIr` and `ValidateBackendsSupportFields` each build the full IR
per top-level field, and DICT-1's `BaseFacts` now performs a
`DynamicMessageFactory` reparse for **every option-carrying field** inside each of
those builds (the `ByteSizeLong()==0` fast path spares only option-less fields).
DICT-1.5 adds no new build and no new reparse — it reuses the node
`FindScalarLeafNestedList` already has — but it does make the second full walk
mandatory on the `accessor`/`rust` path. Nothing to change in this item; flagged so
the corpus-scale cost is attributed to DICT-1's reader, not mistaken for new.
*Confidence: high.*

**N9 — contradictory harness arguments are ignored rather than rejected.** In
`EXPECT_SUCCESS` mode, `EXPECT_MESSAGE`/`EXPECT_FIELD` are silently dropped
(documented at lines 27-28). A `FATAL_ERROR` on the combination would keep a future
copy-paste from writing a test whose assertion never runs. Low value.
*Confidence: high.*

**N10 — docs cosmetics.** `docs/fletcher-options.md:243` is one long unwrapped line
in a section that otherwise wraps at ~80 columns. Also the two guard messages now
differ in a trailing Oxford comma ("IPC schema, and TS backends do support them" vs
"IPC schema and TS backends accept dictionary fields"); harmless, but they read as a
pair. *Confidence: high.*

## Things I specifically checked and found correct

- **`coverage_dictionary_field_flatten.proto` is genuinely red-if-broken** (the
  defect the earlier revision had). The dictionary is on `w`; `DictFfWrap` carries
  none and both emit walks `continue` past `w` under the same
  `TYPE_MESSAGE && !is_repeated && HasFieldFlatten` predicate, so
  `BuildSingularMessage`'s `node.facts = BaseFacts(w)` is the only route. Verified
  live: the error names `…DictFfGuard.w`, i.e. the root-facts route, and nothing in
  the file could satisfy the expectation by another path. Its unique value is that
  route (the root check itself is also covered by the plain scalar fixture).
- **CMake truthiness.** `if(EXPECT_SUCCESS)` (undefined → false),
  `if(NOT DEFINED X OR X STREQUAL "")` (correct `NOT`-binds-tighter-than-`OR`
  precedence, and the second operand is safe on an undefined var), and
  `if(DEFINED EXPECT_FIELD AND NOT EXPECT_FIELD STREQUAL "")` — the deliberate
  non-truthiness form, so `0`/`OFF`/`*-NOTFOUND` cannot silently disable the
  assertion. All four negative controls FATAL as intended. Moving `EXPECT_MESSAGE`
  out of the `foreach(_req …)` loop is a *strengthening* (the old
  `if(NOT EXPECT_MESSAGE)` would have accepted nothing but also rejected a literal
  `0`); the `|`-not-`;` separator choice is right and now documented.
- **The 8 `add_test` cases do assert the field**, not just the exit code: 5 of the 6
  rejecting cases carry `EXPECT_FIELD` with the exact FQN tail, and the sixth
  (`DictionaryGuardDoesNotMaskUnsupportedType`) is pinned by an `EXPECT_MESSAGE` the
  dictionary error cannot satisfy, so an ordering regression cannot pass it. The
  `dictionary.*…*round RIR` and GIR-10 expectations are mutually unsatisfiable in
  the correct direction (the DICT text is the strict superset; see N1 for the loose
  direction).
- **Fixture hygiene:** SPDX + Copyright in the first two lines of all 7 new protos
  (whole-tree CI scan satisfied), `syntax = "proto3"` everywhere, 7 distinct
  packages, all "committed but not wired" and reachable only from their own ctest;
  no globbing anywhere in the coverage `CMakeLists.txt`, and no other build unit
  references `protoc-coverage/proto`. `OUT_DIR` stays out of `GENERATED_DIR`, so
  `check_no_value_or_die.cmake` / `validate_generated_ipc.cmake` are unaffected.
- **Citations in the new comments are accurate**: locked #11 exists and says what the
  code claims, `ir.cpp:62`/`:295` are the only two writers of `facts.dictionary`,
  and the "three walks extended together" forward-compat note names the right three.

---

# Confirmation pass — step-4b fix-set (2026-08-28)

Scope: confirmation only, not a re-review. Fix-set reviewed as the **unstaged**
working tree on top of the staged diff; base `9c1cdc4`.

**Both should-fix items are DISCHARGED.** One new finding (S3), introduced by the
S2 fix; false-red only, one-line fix.

## S1 — discharged (isolation mutation-verified independently)

I did not take the claim on report: I mutated `FindDictionaryField` myself,
rebuilt the plugin, and ran all 8 dictionary fixtures + `coverage_scalar_nested`
through the real protoc each time.

| Build | listChild | mapChild | other 7 fixtures |
|---|---|---|---|
| fix-set as delivered | exit 1, names `…DictStructChildInner.k` | exit 1, names `…DictStructChildInner.k` | unchanged, each naming its own field |
| `LIST` branch deleted | **exit 0** (test reds) | exit 1 | unchanged |
| `MAP` branch deleted | exit 1 | **exit 0** (test reds) | unchanged |

So each fixture reds for exactly its own edge, and neither reds for the wrong
reason: in the surviving-edge builds both still report `DictStructChildInner.k`,
which is what their `EXPECT_FIELD` asserts, so neither can pass by naming some
other offender. Both fixtures also genuinely require the STRUCT edge on top
(the declaration is on the *imported* `DictStructChildInner.k`, which
`OrderedMessages(file)` never visits), which is what makes them descent tests
rather than root-hit tests. Fixture hygiene on the two new protos is fine: SPDX +
copyright, proto3, distinct packages (`…dict_guard_list_child`,
`…dict_guard_map_child`), single used import each, unwired, header comment naming
their own ctest. Generator restored to the fix-set state afterwards, rebuilt, and
`clang-format --dry-run --Werror` is clean.

**`FIXED_SIZE_LIST` disclosure: accepted.** Confirmed by grep that `ir.cpp` — the
sole IR constructor — never names `FIXED_SIZE_LIST`/`FixedSizeListNode`, so no
`.proto` can reach that branch; a fixture is not merely missing, it is
unconstructible without the external linkage the plan explicitly refuses. The
comment mirrors `FindUnsupportedIr`'s pre-existing disclosure and attributes the
gap to all three walks, which is the right framing.

## S2 — discharged (my own repro re-run)

- **My repro now fails correctly.** Planted `nope.h` in `OUT_DIR`, ran
  `EXPECT_SUCCESS` mode with `EXPECT_ARTIFACTS=nope.h`: FATALs at line 107
  ("expected artifact … does not exist"). Before the fix this printed *"correctly
  succeeded"*.
- **No false-red on the legitimate case.** The real success invocation (now 4
  artifacts including `coverage_dictionary.DictScalarGuard.ipc`) passes, and
  passes again on an immediate second run (clean is idempotent).
- **`REMOVE_RECURSE` target is safe.** `OUT_DIR` is required non-empty by the
  existing `foreach(_req …)` guard, and the only two values any caller of this
  script passes are the dedicated `${CMAKE_CURRENT_BINARY_DIR}/{backend,dictionary}-guard-generated`.
  `GENERATED_DIR` (`…/generated`, scanned by `check_no_value_or_die.cmake` /
  `validate_generated_ipc.cmake`) and `unsupported-generated` (a different script)
  are never passed here, so nothing else can be deleted.

### S3 (new) — the clean *does* introduce one new failure mode: a cross-test race on the shared `OUT_DIR`

*Confidence: high (reproduced). Severity: should-fix — false-RED only, never
false-green; CI is serial today, so it is not a merge-gate risk. Downgrade to nit
if you prefer.*

10 tests now share `dictionary-guard-generated` and each one wipes it on entry, so
under `ctest -j` a sibling test can delete the `EXPECT_SUCCESS` test's artifacts
between its protoc write and its `EXISTS` check. Reproduced twice: 1/8 and 2/12
concurrent trials, failing at line 107 with
`expected artifact '…/coverage_dictionary.DictScalarGuard.ipc' does not exist` —
i.e. the *correct* assertion firing on the *wrong* cause. Only the `EXPECT_SUCCESS`
mode is exposed (it is the only mode that asserts on files); the reject-mode tests
assert on stderr text and never failed in the probe.

Not a CI risk as configured: conan's `cmake.test()` builds the `RUN_TESTS`/`test`
target (no `-j`), and no profile sets `CTEST_PARALLEL_LEVEL`, so tests run serially
on the gate. It bites local `ctest -j`, which is the natural way to run a 30-test
suite.

Fix (one line, and the other half of the original S2 recommendation): make
`OUT_DIR` unique per test rather than per family — e.g.
`-DOUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/dictionary-guard-generated/<fixture-stem>`
— or at minimum give `GenErrors.DictionaryAcceptedWithoutRbaBackends` its own
directory. That removes the race outright and keeps the clean.

## Folded nits — spot-checked, all good

- **N1 discharged and verified stronger than reported:** with
  `EXPECT_FIELD=ScalarNestedCoverage.nested_scalar_lists`, both
  `ScalarLeafNestedListRejectedBy_*` invocations still pass, *and* feeding the
  GIR-10 expectations a dictionary fixture now FATALs at line 144 ("did not name
  the expected field") instead of passing on the shared message tail. The two
  guards can no longer be confused.
- **N2 discharged:** `coverage_dictionary_struct_child.proto` now emits zero
  `unused import` warnings (grep count 0 on the raw protoc stderr).
- **N3 discharged:** `.ipc` in `EXPECT_ARTIFACTS`, verified present and asserted.
- **N4 discharged:** the `Generate()` call-site comment now names both shapes;
  the support/accept wording split (M4) is accurate — the non-RBA backends *accept*
  dictionary fields without yet dictionary-encoding them.
- **N7 discharged:** `optional` restored on `category` in both fixtures; both still
  reject with the same field names (re-run, not assumed).

## Skips — no disagreement

N5, N6, N8, N9, N10 are all correctly skipped for this item. N5 is the one I would
still like carried somewhere DICT-3 will read (its safety argument is "emitter and
guard consume the same IR node"; if DICT-3 sources dictionary-ness from the
descriptor instead of `facts.dictionary`, the `repeated <flatten wrapper>` shape
becomes a real silent mis-read) — that is a note on DICT-3's plan, not work here.

## One process reminder

`coverage_dictionary_list_child.proto` and `coverage_dictionary_map_child.proto`
are still **untracked** (`??`). They must be `git add`ed with the fix-set: the
license-header CI scan works off `git ls-files`, and two `add_test` cases reference
files that would not exist in a fresh clone.
