# PDA-DEC-9 — architecture-conformance review (step 4a, independent)

Reviewed: `5d06812` against base `7740a9d` (`git diff 7740a9d..5d06812`, 11 files).
Design: `plans/PDA-DEC-9-seam-spec-handoff.md` (APPROVE-WITH-DEBT(10), 2/2 cycles) ·
Locked: `plans/PDA-decouple-locked-decisions.md` (14) · Rulings: `plans/PDA-DEC-rulings.md`
(**32 entries**, derived `grep -c '^## ' plans/PDA-DEC-rulings.md`) · Debt register
`plans/reviews/design-debt.md` §PDA-DEC-9 + §PDA-DEC-9 cycle 2.

Verdict: **PASS-WITH-FINDINGS(3)** — 1 blocking, 2 non-blocking; see the foot of this file.

## Reproductions performed
All builds/runs below were done in an **isolated `git worktree` at `5d06812`** under the
scratchpad, not in the repo working tree: step 4b was running concurrently and I caught it
mid-mutation (`core/include/fletcher/core/status.hpp` carried a foreign
`kZzReviewMutationDoNotShip = 10` at 10:14, reverted seconds later). Mutating the shared tree
would have corrupted both reviews.

Profile `.conan-profiles/Windows-msvc194-x86_64-Release`, MSVC 19.4x, `conan install` +
`cmake --preset conan-default` + `cmake --build --preset conan-release`. Each mutation was built
with `--clean-first`: the scratchpad sits under `%TEMP%`, which MSBuild warns (MSB8029) breaks
incremental dependency tracking — an artefact of *my* sandbox location, not of the tree, and the
reason a plain incremental build after the first mutation relinked without recompiling.

### R1 — the compile red (the whole guard). REPRODUCED, verbatim

Appending `kThrowawayMutationDoNotShip = 10` to `PubSubStatus` and rebuilding `core_tests`:

```
test_status_taxonomy.cpp(74,5): error C4062: enumerator
'fletcher::PubSubStatus::kThrowawayMutationDoNotShip' in switch of enum
'fletcher::PubSubStatus' is not handled
```

A **compile** failure of `core_tests` (`error`, not `warning`), so design premise P3b's
stop-and-ask did not trigger and no `kLastStatus` sentinel was needed. `/we4062` **does** enable
this off-by-default warning as well as promote it — the mechanism the design bet on works on the
compiler that builds this lane.

### R2 — the flag is wired to one source file on the right target, and nothing global

`build/tests/core_tests.vcxproj` (generated, isolated build):

```xml
<ClCompile Include="...\tests\test_status_taxonomy.cpp">
  <TreatSpecificWarningsAsErrors Condition="'$(Configuration)|$(Platform)'=='Release|x64'">4062</TreatSpecificWarningsAsErrors>
  ... (Debug / MinSizeRel / RelWithDebInfo identical)
</ClCompile>
```

`test_envelope.cpp` and `test_positional_io.cpp` carry no such element. Whole-tree scan for a
global promotion (`/WX`, `-Werror`) over `CMakeLists.txt`/`*.cmake`/`*.py`, excluding `build/`:
the only hits are the new comment lines in `core/tests/CMakeLists.txt` itself. So the design's
"the tree has no global `/WX`" premise still holds, and the promotion is not inherited from
anywhere — it is exactly this one file, on `core_tests`, in all four configurations.

### R3 — `case` added, README row not. REPRODUCED (part 3)

With the enumerator *and* its `case` present and no README row:
`test_status_taxonomy.cpp(186)` — `StatusName(cast(rows.size()))` is
`"kThrowawayMutationDoNotShip"`, expected `""`. Suite red until the row is published.

### R4 — README side edited. REPRODUCED (part 2)

` | `kNotSupported` | 6 | ` → ` | `kNotSupportedX` | 6 | `, tree otherwise pristine:
`test_status_taxonomy.cpp(177)` — published row 6 `kNotSupportedX` vs enum `kNotSupported`.

### R5 — the two non-vacuity reds are real, not asserted

- README moved aside → `(156) ASSERT_FALSE(readme.empty())` fires ("could not read the published
  taxonomy from …"). This is the state the design cited as "red for the right reason today", and
  it is a **failure**, not an empty pass.
- Heading renamed to `## Error taxonomy (renamed)` → `(159) ASSERT_FALSE(rows.empty())` fires
  ("no taxonomy rows parsed … a missing or renamed section is a failure, not an empty pass").
  So `ParsePublishedRows` returning zero rows cannot be green — F5 (a doc-presence check that
  cannot fail) is genuinely shut, in both directions.

### R6 — counts, derived here, stated separately

- `ctest -C Release -N` in the repo build: **29 entries**, #29 `Taxonomy.PublishedNumbersMatchTheEnum`.
- `core_tests.exe --gtest_list_tests`: **29 cases** (6 suites).
- `ctest -C Release`: **29/29 passed**, 0 failed (0.69 s).
- Base `7740a9d`: `git show 7740a9d:core/tests/test_envelope.cpp | grep -cE '^TEST(_F)?\('` = 9,
  `test_positional_io.cpp` = 19 → **28**. So **28 → 29 entries and 28 → 29 cases**, one added
  test, entries and cases equal because `gtest_discover_tests` emits one ctest entry per case.
  The implementer's figures check out for both quantities.
- `core` genuinely recompiled on both passes here (fresh cache-independent build tree; the
  isolated `conan install` re-resolved `gtest/1.17.0` from cache and CMake compiled all three
  translation units — see the `--clean-first` runs above).

### R7 — the package (design premise P1). HOLDS, verified end to end

`conan create core -o "&:run_tests=True"` from the isolated worktree:

- export log: `Copied 1 '.md' file: README.md` — the new `exports_sources` entry works.
- `conan list "fletcher-core/0.5.0-alpha:*"` → package id
  **`da39a3ee5e6b4b0d3255bfef95601890afd80709`** (SHA-1 of the empty string, i.e. `info.clear()`).
  **Unchanged**, as claimed; nothing downstream sees a different `fletcher-core`.
- the *recipe revision* did move (`3860294dde3fecc2b7907bc0c93dea2c`) — inherent to touching
  `exports_sources`, harmless because `package_id()` clears, and **claimed nowhere** in the tree.
  `package()` is untouched in the diff and still copies `*.hpp` + `*.cmake` only, so the README
  reaches no consumer's package folder. The brief's narrowed wording ("the package's
  **contents** do not change", debt C3-4) is exactly right.
- the **cache** build compiled and ran the suite off the exported README:
  `.conan2/p/b/fletc45c54849feda1/b/README.md` present, its `LastTest.log` shows
  **29 `[ OK ]`, 0 FAILED**. That is the lane shape CI uses, so the disk read works where it has to.
- one earlier cache build (`fletcebd22b3ac6e01/b`) has `tests/test_status_taxonomy.cpp` and **no
  `README.md`** — physical corroboration of the "failed on the empty read before
  `exports_sources`" claim.
- `conan create`'s **test_package** stage failed *in my sandbox only* (`No CMAKE_CXX_COMPILER
  could be found` under the `%TEMP%` path). Settled as environmental: `conan test
  core/test_package fletcher-core/0.5.0-alpha` from the repo location **passes**, and this item
  touches neither `test_package`, `package()` nor `package_info()`.

### R8 — the labels, audited against what the tree can actually check

| Row | Landed label | Judgement |
|---|---|---|
| 1 | by-reading | Earned. No machine reads a doc comment for "normative and C-expressible"; `SeamVocabulary` is credited with *representability* only. Not needlessly weak. |
| 2a | mechanical (regression) / by-reading (addition) | Earned, both halves. Callers really do call `result.schema.Wait(timeout, &out)` (`pubsub/tests/test_publisher_subscriber.cpp:256`, `pubsub-arrow/tests/test_pubsub_arrow.cpp:129`, `gateway/src/ws_session.cpp:237`), so a `shared_future` *return* breaks the compile; an *added* member reddens nothing and no lane runs the grep. Design D2 said flat `mechanical`; the weakening is the honest direction (debt C3-5). |
| 2b | by-reading | Earned; the row states why an absence grep cannot settle coherence. |
| 3 | mechanical | Earned — R1/R3/R4/R5 — with the MSVC-only witness disclosed (C3-9). |
| 4 | mechanical | Earned. `provider_registry.hpp:292-297` is a namespace-scope `static_assert` on `decltype(&ProviderRegistry::Create)`, so every TU including the header evaluates it; both named Registry cases exist (`integration-tests/pubsub-conformance/src/registry.cpp:241` and `:313`). |
| 5 | by-construction, no machine check | Earned; nothing in the tree would notice an accessor being added. |
| 6 | by-reading | Earned. |

Headline **"two of the six are mechanical end to end (3 and 4)"** is honest, and it is stated
identically in `plans/PDA-decouple-interface.md`, the brief, the progress log and §12.2. No label
is too strong; none is needlessly weak.

### R9 — the converse sweep: what survived that should not have

- Every ordered deletion is **gone**. A whole-file scan of the spec for the deleted phrases
  (`claims stand`, `construction sites`, `19 occurrences`, `18 ProviderConfig`, `10 sites`,
  `re-derivable`, the `grep -E` recipe, `Measured, excluding`) returns **no hits**, and §10
  (lines 801-884) now contains **0** table rows.
- The retired-types claim that replaced them is **true as worded**: `FastDDSProviderOptions` and
  `XrceConfig` are declared nowhere and constructed nowhere — surviving matches are comments,
  gtest suite names and the live, unrelated helper `XrceConfigFor(...)` returning `ProviderConfig`
  (debt C3-6's precision, honoured).
- Public surface growth is **0**: the diff changes no header (`core/include/**` untouched,
  `status.hpp` byte-identical to base), adds no dependency, no `extern "C"`, no parser, and there
  is **no `kLastStatus`** anywhere in the tree.
- No `PubSubProvider` method, no ABI, no config parser; no test, clause or subject deleted,
  skipped or weakened. The only suite change is +1 case.
- The `PubSubStatus = 10` door is still shut: §12.1 ("making an append is itself a stop-and-ask,
  and the owner allocates"), `core/README.md` ("Making an append is a stop-and-ask against the
  spec and the owner allocates the number"), and §5.1's new "the table is the only enumeration"
  bullet. Two parallel rounds cannot both take 10 without meeting the owner.
- §4.1's qualifier is carried **verbatim** into §12.1 — compared `:416-419` against `:911-913`;
  only the nested double quotes become single quotes, which the nesting forces. §4.1 itself is
  untouched by the diff.
- §8.1's falsification sentence is restated **word-identical** in §9 (colon becomes an em dash)
  and is **not** promoted into a close gate.
- Design premises re-checked live: **P3** (ten values 0..9, `status.hpp:67-83`); **P5**
  (`FastDDSPubSubProvider(const ProviderConfig& = {})`, so both docs' `make_shared` examples still
  compile); **P4** (no lane can run on a branch push — all component and integration lanes are
  `workflow_call`, `ci.pr.yml` is `on: pull_request`, and the only `push`-triggered CI workflow is
  `ci.setup-devcontainer-image.yml`, restricted to `branches: [main]` plus `.devcontainer/**`).
- The 2026-09-03 ruling has **all three** obligations in §12.4, none softened: the evidence stated
  exactly ("one Windows machine, plus one WSL compile of the single platform-forked file"), both
  rounds instructed to treat Linux as **unverified**, and a Linux-only seam difference routed to
  the owner as a **stop-and-ask, not a local fix**. The words `portable`, `both platforms` and
  `CI-green` appear **nowhere in the whole spec** (F6). `ci.core.yml:55,107` really are the
  Windows and Linux `conan create ... -o run_tests=True` lines, so §12.4's "which is what would
  expose a diagnostic that fires under one compiler and not the other" is accurate, and the
  no-CI statement plus its `workflow_call` reason are stated in bold in the first two sentences —
  a reader cannot come away assuming CI backs the seam.
- §9's new oracle row names four suites that all exist: `TEST_P(ProviderConformance` x12,
  `TEST(CopyAccounting` x4 plus one `TEST_P`, `TEST(SeamVocabulary` x7, `TEST(Registry` x25.
- The new §7.4 snippet in `docs/architecture-overview.md` matches the tree's API as written
  (`ProviderConfig{max_payload_bytes, domain_id, document}`, `ProviderSelector::Parse(std::string)`,
  `ProviderRegistry::Create(selector, config) const`, `RegisterFastDDSProvider(ProviderRegistry&)`),
  and it landed in §7.4 as ordered.
- Files touched: **11**. Ten are the design's `Files-to-touch`; the eleventh,
  `plans/PDA-DEC-9-brief.md:52`, is ordered by debt **C3-4** verbatim. **No scope breach.** The
  tracker status cell is untouched (settled: correct).

## Findings

### 1. BLOCKING (one-clause fix) — a free-floating count survived into frozen contract text, and it is wrong

`docs/pubsub-interface-spec.md:945`, §12.2, immediately under the conditions table:

> That is why **five of these labels came down in review**, and why no new guard was invented at close.

Derived, not remembered. The cycle-1 design's D2 table
(`git show 9c69d23:plans/PDA-DEC-9-seam-spec-handoff.md`) labelled rows 1-6
`by-reading, mechanical, mechanical, mechanical, mechanical, by-reading`. The landed table is
`by-reading | mechanical(regression) + by-reading(addition) | by-reading | mechanical |
mechanical | by-construction | by-reading`. Only **condition 2** (split into 2a + 2b) and
**condition 5** changed: **three table rows, two of six conditions**. Rows 1, 3, 4 and 6 carry the
label they were designed with. "Five" is false on either counting.

Its origin looks diagnostic: the cycle-2 review's own heading reads *"B3 — the relabelling:
CLOSED on **five rows of six**, one label still over-claims"* — a count of rows that **earned**
their labels, not of labels that came down. That is exactly the "a real number quoted for the
wrong quantity" failure this round has already logged, now on its way into a frozen document.

The item's own progress-log entry for this very commit contradicts the sentence: it is headed
**"One label came down, again."** (`plans/PDA-decouple-progress-log.md`), which is the count
against the *approved* design (row 2a alone); against the cycle-1 draft it is two conditions /
three rows. Nothing supports five.

Why blocking rather than a record: design **F1** is unconditional for the spec — "A number
carries the command that derives it, inline, or it is not written" — and §12 is *contract*, not
one of the two record sections, so by §12.1's own rules correcting it after this PR is a
stop-and-ask. It is the one place in the item where the document breaks the rule the item exists
to impose.

**Acceptable fix:** drop the number — *"That is why labels came down rather than guards going up,
and why no new guard was invented at close."*

### 2. §12.1 says "there is no third" class and then creates one — and §11 is more than a record

Lines 893 versus 915-919: *"Two classes. **There is no third**, and no 'negotiable' text."* …
twelve lines later … *"**Two sections are records, not contract** … Fixing a fact in either is
ordinary maintenance."*

The carve-out itself is **ordered** (debt C3-8) and is right for §10, so this is not scope creep.
Two things about how it landed are:

- The two sentences contradict each other as written, and design **F7** forbids a third class. A
  reader of a frozen contract who meets both does not know which governs.
- **§11 is not only a record.** It carries prohibitions — *"no `extern "C"`, no C header, no
  `dlopen`, no version negotiation, no driver vtable, no host-callback struct"* and *"any change to
  the interface's method set (§2)"* — and "records, not contract … ordinary maintenance" reads as a
  licence to edit them. The round that wants exactly those is PDA-ABI. Mitigated but not fixed by
  locked decisions **4** and **14**, which repeat both prohibitions and are themselves
  stop-and-ask protected.

**Acceptable fix:** bound the permission to facts and keep the prohibitions frozen — *"correcting a
stale **fact** in §10 or §11 is ordinary maintenance; their prohibitions restate §2, §4 and the
round's scope and remain `frozen`"* — and reword the "no third" sentence as "two classes of
*contract text*".

### 3. The no-free-floating-count rule was scoped to two sections rather than to the document

§12.1: *"They carry one rule of their own instead: **a count that claims something about the
current tree is not representable in this document**…"* — attached to §10 and §11. Debt C3-8 asked
for the **tense** to be narrowed ("scope F1 to counts that claim something about the current
tree"), not for the rule's **reach** to shrink to two sections; design F1 binds the spec.

Live consequence: finding 1 — a bare count in §12 that the landed rule does not catch. (§4.1's
frozen "five of its twelve fields are **deleted outright**" is a past-tense record about a retired
type and stays fine under the narrowed tense rule, whichever sections it covers.)

**Acceptable fix (same touch as finding 1):** state it document-wide in §12.1 — *"a count anywhere
in this document carries its derivation command inline or is not written; past-tense records of
work that landed are exempt"* — and let §10 and §11 inherit it.

## RECORD (PM corrects in place; never blocking, no fix cycle)

- `docs/pubsub-interface-spec.md` §10: *"the **single** provider construction each shows,
  `make_shared<FastDDSPubSubProvider>()`, still compiles"* — the same commit adds a **second**
  provider-acquisition example to `docs/architecture-overview.md` §7.4 (`registry.Create(...)`).
  Drop "single". §12.1 makes §10 a record, so this is maintenance.
- `core/README.md:5`: *"Headers are located under `include/core/`"* — the actual path is
  `include/fletcher/core/`. Pre-existing; this item appended `status.hpp` to that list without
  correcting the prefix.
- `plans/PDA-decouple-progress-log.md`, last line: *"**Numbers.** Declared **+330 / -100**."*
  records only the declared figure. Landed is **+587 / -68** (`git diff --numstat
  7740a9d..5d06812`): `test_status_taxonomy.cpp` +188, progress log +89, spec +182/-49,
  `core/README.md` +37, `core/tests/CMakeLists.txt` +29/-2, TD-008 +16, architecture-overview
  +23/-3, plan +14/-11, conanfile +6, root README +2/-2, brief +1/-1. **Judged: not a scope
  breach** — every added line sits in a file the design ordered (ten `Files-to-touch` plus the
  brief line ordered by C3-4), no product code, 0 public surface; and the shortfall on deletions
  (68 against 100) is not under-delivery, since every ordered deletion is verified gone (R9). The
  log line should carry the landed number, in a round whose recorded pathology is remembered
  figures.

## Checked and cleared (nearest misses, one line each)

- §12.3's *"It is a permission, not a requirement, and it does not relieve §8.1's negative-control
  requirement"* is an addition the design did not order, but it keeps §8.1 in force **without**
  re-creating the close gate the 2026-09-01 *Ship the guard, hunt elsewhere* ruling relieved (that
  gate was PDA-DEC-1's, item-scoped, and §12.3 attributes it exactly as C3-10 asked). Cleared —
  and it is the item's nearest approach to a ruling contradiction.
- The design's instruction to flip this item's tracker cell to green was **declined** for the
  recorded process reason. Settled by the PM; not a defect.
- §12.3's "PDA-DEC-7's **four** unwitnessed document keys" is design-authored and checks out
  against the progress log ("four of six document keys had no guard that an accepted value lands
  anywhere", `plans/PDA-decouple-progress-log.md:631`).
- The promoted diagnostic **can** be lost by a later CMake edit with nothing reddening: delete the
  two blocks and an append compiles, part 2 stays green over the ten published rows, and part 3
  stays green because `StatusName(cast(10))` *is* `""`. No cheap machine guard exists for "a
  warning flag is still applied", the design chose this mechanism knowingly (P3b), and the comment
  in `core/tests/CMakeLists.txt` pins the property to that file. Named as residue, not a finding.

## Verdict

**PASS-WITH-FINDINGS(3)** — one blocking, two non-blocking, all three in `§12`'s own wording and
all three fixable in one touch of `docs/pubsub-interface-spec.md` with no code, test or design
change. The guard is real and was reddened three ways here; the package is untouched; the ruling's
three obligations landed intact; every ordered deletion is gone; public surface is 0.

1. **BLOCKING** — §12.2:945 "five of these labels came down in review" is a free-floating,
   **incorrect** count entering frozen contract text (three rows, two conditions). Fix: delete the
   number.
2. §12.1 asserts "there is no third" class and then creates one; "§11 is a record, not contract"
   reads as a licence over §11's ABI prohibitions. Fix: bound the permission to *facts*.
3. The no-count rule landed scoped to §10/§11 rather than to the document, which is what let
   finding 1 through. Fix: state it document-wide.
