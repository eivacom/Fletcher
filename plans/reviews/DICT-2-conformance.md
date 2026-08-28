# DICT-2 — step-4a architecture-conformance review (adversarial)

**Verdict: CONFORMS.** 0 blocking conformance items. Both implementer-flagged
deviations are **ACCEPTED**. Five non-blocking notes recorded (three of them are
"a comment/spec sentence claims more than the code does" — this round's dominant
defect class — plus one PM-side delivery-hygiene item).

* Diff base: `HEAD` = `8b56d62`, reviewed via `git diff --cached HEAD` (staged).
* Design: `plans/DICT-2-mapper-wiring-validation.md` rev 2 (body operative).
* Locked: `plans/DICT-locked-decisions.md` #5, #8, #9 (+ its 2026-08-28
  parenthetical), #11. Spec: `docs/dictionary-option-spec.md` §4, §5, §7.1.
* Method: read the whole staged diff; then **built and ran** the protoc unit suite
  (94 ctest / 93 pass / 1 pre-existing gated skip), built the plugin, ran it
  directly on every coverage fixture and on **six purpose-built probe protos**,
  ran **three source mutations** to prove the new assertions are non-vacuous, and
  **byte-compared 58 generated artifacts** against a plugin built from `HEAD`.
  Tree was restored to its exact staged state afterwards (`git status` unchanged,
  `git diff` empty).

---

## 1. FALSE POSITIVES — the worst failure mode. Clean.

R5 carries `current->field_count() == 1` in its chain-loop condition
(`type_mapper.cpp`, `InnerDeclaredUnderRepeatedFlatten`), mirroring `ir.cpp:376`,
and collects **inner** declarations only (`HasFieldDictionary(inner)` starting at
`msg->field(0)`; the outer field's declaration is never collected — that is R2's).

Verified, not assumed:

* **`Ctl.rep_multi` genuinely maps fine.** `EXPECT_TRUE(IsLegal(d2,"Ctl","rep_multi"))`
  passes, `FindIllegalDictionaryField(W2) == nullopt` passes, and at plugin level a
  standalone `repeated W2` proto (`W2` = flatten + `k [(dictionary)]` + `n`)
  **generates at exit 0**. More: with `--fletcher_opt=accessor` DICT-1.5's guard
  reports `multi.W2.k`, which proves the inner dictionary really does reach the
  struct child node on the IR — so "legal **and honoured by emission**" is true,
  not merely "legal".
* **Mutation proof.** Deleting `current->field_count() == 1` from R5's loop reds
  `TypeMapperTest.DictionaryMappingAndRejections` at the `rep_multi` /
  `FindIllegalDictionaryField(W2)` assertions. The guard is non-vacuous.
* **No false negative opened.** `Holder.rep_inner_declared` is still rejected by R5
  (test passes), and a standalone gap-2 proto (`repeated V vs`, `V` = flatten +
  1 field with the option) is rejected at plugin level with R5's text.
* **Two further false-positive probes, both correctly accepted:**
  `repeated W xs` / `W{flatten; V v}` / `V{k [(dictionary)]; n}` — R5's loop exits
  on `HasMessageFlatten(V) == false`, plugin exits 0, and DICT-1.5 sees `deep.V.k`;
  and the six existing DICT-1.5 fixtures (`_flatten`, `_structChild`,
  `_listChild`, `_mapChild`, `_wkt`, `coverage_dictionary.proto`) **all still
  report DICT-1.5's message**, not DICT-2's — so they do double duty as DICT-2
  false-positive guards exactly as the design claimed.
* Whole-suite check: every generation-eligible coverage proto still generates at
  exit 0 under `ipc,ts`. All five rules require a `(fletcher.dictionary)`
  declaration to fire, so no dictionary-free proto anywhere in the repo can be
  touched.

## 2. The three mandatory residual items — all landed.

**(a) `Ctl.ord_wrap` is no longer vacuous.** `test_type_mapper.cpp`'s
`kDict2Schema` spells it `{index_type: DICTIONARY_INDEX_INT8, ordered: true}` over
`W1i8`'s `{INT8}` leaf, so `ordered` is the **sole** difference, and `W1i8t` +
`Ctl.ord_agree` (`{INT8, ordered: true}` on both) exist and are asserted to hit
**R3, not R4** (`EXPECT_FALSE(Mentions(r, "conflicting"))`).
**Mutation proof:** reducing `SameDictionaryOption` to `a.index_kind == b.index_kind`
reds the `ord_wrap` block ("was accepted, expected a rejection"). Under the old
spelling that mutation would have passed — the residual item was real and is fixed.

**(b) R4 carries both terms.** `ConflictingFlattenChain` opens with
`if (field->is_repeated()) return std::nullopt;` and breaks with
`if (inner->is_repeated()) break;` **after** collecting that inner's own option.
Behaviourally confirmed: `repeated W xs` over a chain with two *disagreeing inner*
declarations reports **R5** ("declared on 'r4rep.W.v' inside a repeated
(fletcher.flatten) wrapper"), not R4 — i.e. R5 keeps the better message.

**(c) R5 is not generalised.** Its predicate still requires the **outer field** to
be `repeated`; the gap-2 sibling is disclosed in spec §7.1 (new normative bullet)
and left silent. Confirmed silent by probe (see note N2).

## 3. Locked #8 closure — exhaustive as implemented, both branches exercised.

As implemented the closure holds on a singular flatten chain: R4 compares the
**decoded** option on both members against `decls[0]` (transitively complete, and
the code says so), so unequal ⇒ R4; equal ⇒ the deepest-wins winner's `ordered`
equals every declaration's ⇒ it surfaces on the resolved node ⇒ R3. Both branches
are pinned by tests (`Ctl.ord_wrap` → R4 + an explicit
`EXPECT_FALSE(n.facts.dictionary_ordered)` proving R3 is blind to it;
`Ctl.ord_agree` → R3 and explicitly *not* R4), and mutation A shows the R4 branch
is mutation-sensitive. Plugin-level text confirmed:

```
field 'r4.M.w': conflicting (fletcher.dictionary) declarations reached through
(fletcher.flatten): index int8, ordered true on 'r4.M.w' vs index int8, ordered
false on 'r4.W.v'; make them identical or remove one
```

Where the closure does **not** reach: the disclosed gap-2 sibling (see N2). That
is a design-adjudicated, spec-recorded gap, not an implementation drift.

## 4. Enforcement shape — conforms.

`ValidateDictionaryDeclarations(file, error)` in `generator.cpp`'s anonymous
namespace: `OrderedMessages(file)`, same `IsRecursive(msg) || IsFlattenedWrapper(msg)`
skip predicate as `ValidateNoUnsupportedIr` / `ValidateBackendsSupportFields`,
sets `*error` and returns false. Call site sits **immediately above**
`ValidateBackendsSupportFields` and below `ValidateNoUnsupportedIr` / the metadata
resolver — one inserted line, no pre-existing order changed. `MapField` /
`ProjectIrToFieldMapping` stay **total** (the only change is two additive writes in
the `SCALAR` branch); no `MapField -> nullopt` rejection anywhere.

Verified at plugin level: `ipc`-only runs of all three negative fixtures fail with
DICT-2's messages (backend-independent); the `accessor` run of
`coverage_dictionary_field_flatten.proto` reports **DICT-2's** message, so
`_passOrder` is a hard pin; `coverage_dictionary_unsupported.proto` still reports
the `Any` error first (#55 unaffected).

## 5. The retarget did not lose coverage.

`GenErrors.DictionaryOnFlattenFieldWrapperRejected` (+ `_passOrder`) replace
`..._accessor_fieldFlatten`, and the mandatory
`EXPECT_TRUE(ir::BuildFieldIr(rec->FindFieldByName("p")).facts.dictionary);`
landed in the `ReadsDictionaryOption` D4b sub-case with the "this is now the ONLY
assertion" comment. Route verified in the tree: `Rec.p` is `Pair` (2 fields, no
message-level flatten) behind a `flatten_field` field, so `BuildFieldIr` →
`BuildSingularMessage` → `node.facts = BaseFacts(field);` at **`ir.cpp:545`** —
byte-for-byte the same line and the same shape `DictFfGuard.w` exercised.

`coverage_dictionary_field_flatten.proto`'s header no longer documents a check the
tests do not perform: the old "the only route … is `BaseFacts(w)` … the guard would
wrongly exit 0" paragraph is replaced by "THIS SHAPE IS NOW ILLEGAL (R1)", the
backend-independence + pass-order statement, and an explicit pointer to the D4b
sub-case that now owns the IR route.

All four ctests have their **own** `OUT_DIR` (`fieldFlatten`,
`fieldFlattenPassOrder`, `dict2Repeated`, `dict2Ordered`), and all `EXPECT_MESSAGE`
values are paren-free (`cannot be combined with`, `requires a scalar column`,
`ordered: true is not supported`) — each verified to be a substring of the actual
emitted text, and `cannot be combined with` verified absent from DICT-1.5's text.

## 6. `kDictSchema` unedited.

No hunk touches `test_type_mapper.cpp:789-879`. All DICT-1 sub-cases keep their
verbatim fixture; the only edits inside `ReadsDictionaryOption` are the B4a
assertion plus comment. New shapes live in `kDict2Schema`. DICT-1's committed
assertions (`on_both` leaf-wins, SF-1/SF-2 pins) all still pass.

## 7. Obligation 5 — descriptor reads are rejection-only.

Repo-wide grep of every `is_dictionary` / `dict_index_type_expr` /
`HasFieldDictionary` / `ReadFieldDictionaryOption` site: the **only** writer of the
two projection members is `ProjectIrToFieldMapping`'s `SCALAR` branch, from
`node.facts.dictionary` / `node.facts.dictionary_index_kind` on the same node. The
four descriptor reads in `type_mapper.cpp` (lines ~379, ~385, ~429, ~469) are all
inside R1/R4/R5. No emitter gained a descriptor read. `type_mapper.hpp` carries the
"REJECTION ONLY … do NOT copy that pattern into an emitter" banner the design
required. DICT-1.5's "guard-inspected ⊇ emittable" property can only shrink.

## 8. Deletion + scope.

`enum class DictionaryModifier` deleted from `ir.hpp`; repo-wide grep over
`*.cpp/hpp/h/rs/ts/txt/cmake` outside `plans/` leaves exactly **one** hit — the
rewritten comment at `generator.cpp:1733` ("the live carrier is
`ir::FieldFacts.dictionary`; there is no other dictionary carrier on the IR") —
which the task explicitly sanctions.

**Zero diff** to `recordbatch_accessor_emitter.*`, all Rust emission,
`protoc/include/fletcher/options.proto`, `option_reader.*`, `protoc/src/ir.cpp`,
and `cpp_backend_schema_visitor.*`. No `FieldKind` member added (locked #5). Every
staged path appears in the design's Files-to-touch; nothing outside it. clang-format
18.1.3 `--dry-run --Werror` exit 0 on all seven changed C++ files.

**D10's "zero change to generated bytes" — empirically confirmed.** Built a plugin
from `HEAD`'s six production files, generated all coverage protos under
`ipc,ts,accessor,rust` and `ipc,ts` with both binaries, `diff -r` over **58
artifacts**: identical.

---

## Adjudication of the two flagged deviations

**(a) R4's message names both FQNs instead of the design's "here". ACCEPTED.**
The implementer's reason checks out against the code: `decls[0]` is only the outer
field when the outer field itself declares. For a conflict *internal* to the chain
(`M{W w}`, `W{flatten; V v [(dict)]}`, `V{flatten; s [(dict)]}`) neither
disagreeing declaration is on the outer field, so "here" would be actively wrong
while the error prefix still names the outer field. The shipped text satisfies
every normative constraint in D9 — both members of both options rendered, FQNs,
ASCII, pairwise distinct, no paren-free-substring hazard — and is strictly more
informative. No re-litigation needed.

**(b) `Plain2` / `StructDict` added to `kDict2Schema`. ACCEPTED.**
The design's R2 "struct" row cited a `MemberHolder`-style field, but
`MemberHolder.m` carries **no** option (checked: `kDictSchema:876-878`), so no
struct-mapped field with the option existed anywhere, and `kDictSchema` was
off-limits. A third reason the implementer did not state and which makes the
separate message the *right* choice: putting a struct-dictionary field inside `Ctl`
would have collided with the `FindIllegalDictionaryField(Ctl) == r4` assertion,
which depends on `disagree` (field 6) being the first offender. Minimal, necessary,
and it keeps the R2 kind-word switch pinned (mutation C below).

---

## Non-blocking notes

**N1 — two comments claim a compile error the build does not produce.**
`cpp_backend_type_table.cpp` (`DictionaryIndexArrowTypeExpr`) says "Exhaustive
switch, no `default`: adding a fifth `ir::DictionaryIndexKind` **must be a compile
error here**", and `type_mapper.cpp`'s `DictionaryKindWord` says a new
`ir::NodeKind` "is a compile error here rather than a silently wrong message". Both
functions end in a fallback `return` (`"arrow::int32()"` / `"a non-scalar column"`),
and the protoc build sets no `-Werror` / `/WX` — so a new enumerator would produce
a `-Wswitch` **warning** and a silently generic string, not an error. The fallback
itself matches the tree's existing convention (`ir.cpp:126`), so the *code* is
fine; only the claim is over-stated. Reword to "…is a `-Wswitch` warning; the
fallback below is unreachable today" (or make the fallback abort).

**N2 — the locked-#8 residual hole is reachable, and D2's blanket sentence
over-claims.** Probed: `M{W w = 1;}` / `W{flatten; repeated V vs = 1;}` /
`V{flatten; string s = 1 [(dictionary)={INT8, ordered: true}]}` generates at
**exit 0 with no diagnostic**. This is precisely the gap-2 sibling that step-2
cycle-2 item R3 disclosed and that the approved design forbids closing in DICT-2,
so the **implementation conforms**. But D2's closing sentence — "no `ordered: true`
anywhere on a flatten chain can be silently swallowed" — is false for this shape,
and spec §7.1's new sibling bullet does not mention that `ordered: true` is among
the silently-dropped declarations. A reader of locked #8 ("`ordered: true` is
rejected at codegen in v1") would believe enforcement is absolute. **One sentence**
in spec §7.1's sibling bullet (or a dated parenthetical on locked #8) closes the
documentation gap; no code change.

**N3 — delivery hygiene, PM-side, not in the reviewed diff.** The two new fixtures
`coverage_dictionary_{repeated,ordered}.proto` are tracked, correct, and match the
design — but they landed in the **base** commit `8b56d62` ("docs(DICT/RIR): …"),
not with DICT-2. Consequences: `HEAD` currently carries two negative fixtures that
no test references, and DICT-2's commit will not contain its own fixtures. This is
the mirror image of DICT-1.5's untracked-fixture process note (which is why the
design said "`git add` them"). Consider a fixup before push, or record it in the
progress log.

**N4 — one undisclosed (harmless) addition to R5's predicate.**
`InnerDeclaredUnderRepeatedFlatten` opens with
`if (!field->is_repeated() || field->is_map()) return std::nullopt;`. The
`is_map()` term is not in the design's R5 predicate. Verified a no-op: a
synthesized map-entry message can never carry `(fletcher.flatten)`, so the loop
would not have been entered anyway. It narrows nothing and weakens no contract;
noted only because undisclosed predicate terms are how rules drift.

**N5 — both of R4's mandatory terms are present but unpinned by any test.**
Deleting `if (field->is_repeated()) return std::nullopt;` would leave the whole
suite green (`Holder.rep_inner_declared` collects only *one* declaration, so R4
finds no conflict and R5 still fires); same for `if (inner->is_repeated()) break;`.
The design mandated the code terms, not tests, so this is **not** a conformance
miss — but the shapes exist and are cheap: `repeated W xs` over
`W{flatten; V v [(dict)={INT8}]}` / `V{flatten; s [(dict)={INT64}]}` must report
**R5** (verified by hand at plugin level today), and
`M{W w}` / `W{flatten; repeated V vs [(dict)={INT8}]}` / `V{flatten; s [(dict)={INT64}]}`
must not report "conflicting". Two `Mentions` / `EXPECT_FALSE` lines would make
both load-bearing terms mutation-sensitive.

---

## Mutation evidence (all three reverted; tree verified clean afterwards)

| Mutation | Result |
|---|---|
| A — `SameDictionaryOption` → `index_kind` only | RED: `Ctl.ord_wrap` "was accepted, expected a rejection" + both `Mentions` assertions. Locked #8's wrapper-declared branch is genuinely pinned. |
| B — drop `current->field_count() == 1` from R5's loop | RED: `IsLegal(Ctl.rep_multi)` and `FindIllegalDictionaryField(W2)`. The false-positive guard is genuinely pinned. |
| C — R2 gate `kind != SCALAR` → `kind == LIST` | RED: `Shapes.labels`, `StructDict.st`, `Shapes.oc` accepted; map/struct kind-words and the `UNSUPPORTED` clause all red. R2's kind coverage is genuinely pinned. |

## Obligations discharged (progress-log inheritance)

1. `flatten_field` wrapper hole (DICT-1 D4b) → R1, full three-term predicate, WKT
   wrappers included, `Rec.s` still legal. Verified.
2. `BuildFlattenedRepeated` inner-declared drop (spec §7.1 gap 2) → R5, narrow,
   zero `ir.cpp` change, SF-1 pin still green. Verified.
3. Dead `ir::DictionaryModifier` deleted. Verified.
4. Wrapper/leaf conflict escalation → R4 hard error on disagreement, silence on
   agreement, **no** warning channel invented; the stale `FieldMapping::warning`
   doc is corrected to say the channel is dead. Verified.
5. Dictionary-ness stays IR-derived for mapping/emission; descriptor reads are
   rejection-only. Verified.

## Docs / plan conformance

* Spec §4: front-end-pass paragraph, the three new rejection rows, the corrected
  WKT row (no more dead `type_mapper.cpp:632-669` / `MapWellKnown` citation), the
  three-term predicate note, and the "do NOT exclude WKT wrappers" warning — all
  present.
* Spec §5: the "two schema emitters" paragraph is marked SUPERSEDED with D11
  finding 1 verbatim (including the definition-only function list and "DICT-3 must
  branch inside the visitor, on `ir::FieldFacts.dictionary`").
* Spec §7.1: STATUS block, gap-1 → R1, gap-2 → R5 narrowed to single-field
  wrappers, the multi-field-wrapper legality bullet, and the still-open sibling.
* `docs/fletcher-options.md`: the "Rejected declarations" paragraph covers all
  five rules plus both legality carve-outs.
* `plans/DICT-dictionary-option.md`: DICT-2 → 🟢, the `MapField`/`nullopt` Scope
  bullet corrected (required item 8), dead citations fixed, the five rules and the
  RIR carry-forward recorded, DICT-3's IR-derived constraint restated.

---

# Re-review after the fix round (2026-08-28) — the S2/S3 disclose-vs-close call

**Verdict: CONFORMS.** The disclosure decision is **correct and technically
justified** — I reproduced both holes *and* the false positive that the cheap fixes
would introduce, on the staged content. 0 blocking. **2 numbered should-fix items,
both documentation**, one of which is a safety precondition for DICT-3.

Method note — the working tree was **unstable** during this review: a concurrent
process was applying and reverting mutation markers in `protoc/src/generator.cpp`
(`// MUTATION A: IsFlattenedWrapper skip dropped`) and `protoc/src/type_mapper.cpp`
(`// MUTATION S4a: ...`) while I worked. The **index was always clean** (`git show :<path>`
carries no `MUTATION` marker in any of the five C++ files). So I exported the index
to an isolated tree (`git checkout-index --prefix=/c/tmp/d2rev-src/`), configured a
separate build against the same Conan toolchain, and ran **every** experiment there
— byte-verified `MATCH` against the staged blobs for both source files afterwards.
All results below are on staged content, and the shared tree was never touched by
me. See P1.

## (a) Is the false-positive argument correct against the tree? YES — proved.

The implementer's claim is that `(fletcher.flatten_field)` inlining exists only in
`GatherFieldsImpl` / `BuildFlattenedFieldListImpl`, so *inside* a flatten wrapper
it is a no-op and the declaration is honoured — making both cheap fixes fire R1 on
a live dictionary. Verified end-to-end with the real plugin:

```proto
message FfLeaf { option (fletcher.flatten) = true; string s = 1; }
message FfW    { option (fletcher.flatten) = true;
                 FfLeaf p = 1 [(fletcher.flatten_field) = true,
                               (fletcher.dictionary) = {index_type: DICTIONARY_INDEX_INT16}]; }
message M      { FfW f = 1; int32 n = 2; }
```

* **staged plugin, `--fletcher_opt=ipc`: rc 0**, emits `fp.fletcher.pb.h` +
  `fp.M.ipc` with one column `f`.
* **staged plugin, `--fletcher_opt=accessor`:** DICT-1.5's guard reports
  `field 'fp.FfLeaf.s'` — i.e. the resolved node really carries
  `facts.dictionary`. The dictionary is **live**, not decorative.
* **mutated plugin (isolated tree, `IsFlattenedWrapper` skip dropped), `ipc`: rc 1**
  — `field 'fp.FfW.p': (fletcher.dictionary) cannot be combined with
  (fletcher.flatten_field): ...`. A **false positive on a legal, emitting proto**.
* The same mutation **does** close S2: `s2.W.v` → R3 (`ordered: true is not
  supported`). So it is a genuine trade, exactly as the addendum states — not a
  free win that was declined.

4b's supporting analysis ("every wrapper-field shape illegal when judged on its own
is also illegal in every flattened usage") is **falsified by this shape**. The
implementer's refusal is right, and spec §7.1's own priority ("over-rejecting
permanently outlaws a legal proto") settles the direction. I also re-reproduced both
holes on the staged build: S2 (`map<string, W>` + wrapper-inner `ordered: true`) →
rc 0; S3 (`outer.proto` imports `inner.proto`) → rc 0 under `ipc` while `accessor`
reports `xf.inner.Inner.k`, and `inner.proto` generated on its own is correctly
rejected by R3.

## (b) Does `Chains.ff_inside_wrapper` genuinely pin it? NO. → item 1.

Dropping the skip in the isolated tree left the unit suite **completely green**
(94 tests, 93 passed, 1 pre-existing `SchemaVisitor.CaptureGoldens` skip — identical
to the unmutated run), and **no** protoc-coverage fixture newly failed. Reasons,
both structural:

* `ValidateDictionaryDeclarations` is declared in **no header** — it is file-local
  to `generator.cpp`, so no unit test can call it. The three assertions in that
  block call `MapField`, `DictionaryUnsupportedReason` and
  `FindIllegalDictionaryField` only.
* No coverage fixture contains the hazard shape: the only three fixtures with
  `flatten_field` are `coverage.proto`, `enum_coverage.proto` and
  `coverage_dictionary_field_flatten.proto`, and none of them puts
  `flatten_field` + `dictionary` on a field **inside** a `(fletcher.flatten)`
  wrapper. So no ctest covers it either.

What the block *does* pin is the **premise** (`is_dictionary` +
`arrow::int16()` — the declaration is live, so rejecting it is a false positive)
and the **asymmetry** (`IllegalIn(d2, "FfW").has_value()` — the rule set applied to
the wrapper itself *does* reject). That is genuinely the fact a future implementer
must read, and the comment above it is excellent. But the design addendum's clause
"and that assertion is now **in the forcing test** so the hazard cannot be
re-introduced by accident" is **not true as written** — the same over-claim class as
my N1/N2. Spec §7.1.1's parenthetical ("Pinned: ... maps to a SCALAR int16
dictionary; asserted in ...") is accurate and needs no change.

## (c) Is disclosing acceptable for DICT-2 against locked #8 / #9? YES, with one caveat.

Four reasons, in order of weight:

1. **Closure has a proven false-positive cost** (above), and both the spec and this
   round rate over-rejection worse than a missing diagnostic.
2. **No accepted proto gets a wrong artifact today.** `dictionary_ordered` is read
   in exactly one place repo-wide — R3 itself (`type_mapper.cpp:508`); it is
   written at `ir.cpp:64` / `:297` and read by nothing else. And no emitter reads
   `facts.dictionary` at all yet (only DICT-1.5's guard and the projection). So a
   silently-swallowed declaration produces a **byte-identical artifact** to the
   same proto without the option: the cost of S2/S3 is a **missing diagnostic**,
   not a malformed schema. Locked #8's operative promise ("`ordered: true` is
   rejected at codegen") is under-enforced as a *diagnostic*, which is a defect of
   degree, not of kind — and the same trade was already approved at step 2 for
   gap 2's sibling.
3. **The correct fix is genuinely design-scale** and is specified (R1 becomes
   `is_top_level`-gated + the sibling passes' subtree/imported descent + a
   false-positive sweep). That is an item, and the addendum says so.
4. **The disclosure is complete on all four surfaces** — see (d).

**Caveat (→ item 2).** S3 breaks a property the round has been implicitly relying
on. Verified: `outer2.proto` importing `Inner2 { repeated string tags = 1
[(dictionary) = {INT8}]; }` is **accepted at exit 0**, and the `accessor` run proves
the **LIST** node carries `facts.dictionary` (`field 'xg.inner.Inner2.tags'`). So a
**non-`SCALAR` node carrying `facts.dictionary` can reach emission**. D8 and the
story plan still instruct DICT-3 to "branch on `ir::FieldFacts.dictionary`" with no
kind gate — which, on that proto, would ask the schema visitor to emit
`dictionary(<idx>, <list>)`. Harmless today (nothing reads the fact); a
malformed-schema bug the moment DICT-3 lands.

## (d) Are the disclosures faithful and discoverable? YES.

* **Design step-4 addendum** — exact boundary, the three shapes with per-shape
  "why nothing judges it", the R1-soundness finding, both candidate fixes and why
  each opens the false positive, the recommended `is_top_level` shape, and an
  explicit "**locked #8 is not absolutely enforced**". Faithful.
* **Spec §7.1.1** — same content in normative voice, correctly placed next to
  gap 1 / gap 2, and it states the sibling-pass asymmetry.
* **Story plan** — a DISCLOSED BOUNDARY bullet with the false-positive reason and
  pointers to both documents.
* **`docs/fletcher-options.md`** — user-facing qualifier ("the qualifier in the
  first sentence is load-bearing"), naming the imported-message and map/struct-child
  cases and pointing at §7.1.1. This closes 4b's P2-5 properly.

**Risk 5's false claim is properly retired, and the replacement boundary is
accurate.** I verified each half independently on the staged build: the sibling
passes' walks *do* reach imported fields (`accessor` on `outer.proto` reports
`xf.inner.Inner.k`; on `outer2.proto`, `xg.inner.Inner2.tags`), `OrderedMessages`
yields only same-file messages, `FindIllegalDictionaryField` descends only through
`flatten_field` wrappers, and an illegal declaration **is** reported when its own
file is the generation unit (`inner.proto` + `ipc` → R3). The surviving mention is
the correction itself, explicitly marked **CORRECTED ... FACTUALLY WRONG and has
been removed**, and it now cites the reproduction. Accurate as stated.

## My earlier non-blocking items — all confirmed discharged

* **N1 — fixed.** Both comments now say `-Wswitch` **warning**, name the fallback
  `return` as what keeps it compiling, cite the tree's `ir.cpp:126` convention, and
  point at `-Werror=switch` as the alternative. No over-claim left.
* **N2 — fixed.** D2's closure sentence carries a dated **SCOPED** note naming all
  three boundary holes and the probe; spec §7.1 / §7.1.1 state that all three drop
  `ordered: true`. The scoping is honest rather than minimising.
* **N5 — fixed and independently mutation-verified by me** in the isolated tree:
  deleting `if (field->is_repeated()) return std::nullopt;` reds
  `Chains.r4_repeated_outer` on exactly the two assertions that matter
  (`Mentions(r, "inside a repeated")` and the `EXPECT_FALSE(Mentions(r,
  "conflicting"))` guard); deleting `if (inner->is_repeated()) break;` reds
  `Chains.r4_repeated_inner` (`r.has_value()`), and its failure text shows the exact
  pathology the term exists to prevent. Each reds on **exactly one** deletion; both
  restored → green.
* **N4 — no disclosure needed.** I re-derived it: a synthesized map-entry message
  can never carry `(fletcher.flatten)`, so `field->is_map()` in R5's early-out is a
  provable no-op. Leave it; it costs nothing and reads as defensive. Withdrawn.
* **N3** is the PM's (commit restructuring), as noted.

## Delta re-verification (staged content, isolated build)

| Check | Result |
|---|---|
| unit suite | 94 gtest cases, 93 pass, 1 pre-existing skip |
| R1's new remedy text | `... to dictionary-encode; remove (fletcher.flatten_field), or move the option onto the inlined field or fields` — still contains `cannot be combined with`, so both retargeted ctests' `EXPECT_MESSAGE` still match (verified against real plugin output); still ASCII; still pairwise distinct (the six-reason `std::set` assertion passes) |
| `generator.cpp` comment move | DICT-1.5's paragraph is back directly above its own call; DICT-2's above its own. No logic change; skip predicate intact in the staged blob (`IsRecursive(msg) \|\| IsFlattenedWrapper(msg)`) |
| rules changed? | **No.** R1–R5 logic is byte-identical to what I reviewed in round 1 apart from R1's remedy sentence — so every round-1 verification (mutations A/B/C, the false-positive sweep, the byte-identity of 58 generated artifacts) still stands |
| clang-format 18.1.3 on staged content | exit 0 |
| scope guard | staged set unchanged apart from the two review docs; still zero diff to `recordbatch_accessor_emitter.*`, Rust emission, `options.proto`, `option_reader.*`, `ir.cpp`, `cpp_backend_schema_visitor.*` |

---

## Items

**1 (should-fix, one line — the guard that justifies the deviation is not the pin
it is claimed to be).** In the design step-4 addendum, "and that assertion is now
in the forcing test **so the hazard cannot be re-introduced by accident**" is false:
dropping the `IsFlattenedWrapper` skip leaves the entire suite green (proved) —
`ValidateDictionaryDeclarations` is file-local so no unit test reaches it, and no
fixture carries the shape. Either (i) reword to "…so the *premise* is asserted: any
future close must keep `Chains.ff_inside_wrapper` mapping to a live `int16`
dictionary", or (ii) actually pin it with one plugin-level
`EXPECT_SUCCESS`/`EXPECT_ARTIFACTS` ctest on a fixture carrying the `FfW` shape,
which *does* red on the mutation. (i) is enough for DICT-2; (ii) is the real pin and
is ~15 lines of CMake plus a fixture.

**2 (should-fix, carry-forward with a real consequence).** S3 means DICT-2's pass
does **not** establish "only `SCALAR` nodes carrying `facts.dictionary` reach
emission": `Inner2 { repeated string tags [(dictionary)] }`, reached only as a
struct child from an importing file, is accepted at exit 0 with the fact on a
**LIST** node (verified via DICT-1.5's guard naming `xg.inner.Inner2.tags`). D8 and
`plans/DICT-dictionary-option.md`'s DICT-3 Scope bullet still say only "DICT-3 must
branch on `ir::FieldFacts.dictionary`". Add the kind gate to both: **DICT-3 must
branch on `facts.dictionary` AND `kind == NodeKind::SCALAR`**, because an accepted
proto can present a non-scalar node carrying the fact — otherwise the visitor would
emit `dictionary(<idx>, <list>)`. Harmless today (no emitter reads the fact); a
malformed schema the day DICT-3 lands. This is the one place where the disclosure is
incomplete in a way that can bite.

## Process note

**P1 — the working tree carried unstaged mutation markers while the review was
dispatched.** I observed `// MUTATION A: IsFlattenedWrapper skip dropped` in
`protoc/src/generator.cpp` and later `// MUTATION S4a: ...` in
`protoc/src/type_mapper.cpp`, appearing and disappearing during this review
(concurrent mutation verification). The index was clean throughout and the tree is
clean again now, so nothing is wrong with the deliverable — but this round has
already been burned twice by index/worktree divergence (DICT-1's unstaged fix,
DICT-1.5's untracked fixtures). Add the mechanical gate before committing:
`git diff --quiet && git status --porcelain` **and**
`git grep -n "MUTATION" -- protoc/` must both come back empty. Also worth adopting
what I did here: run review mutations in a `git checkout-index --prefix=` export, so
a reviewer and an implementer can never race on the same file.
