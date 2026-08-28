# DICT-2 — step-4b independent code review

**Diff base:** `HEAD` = `8b56d62` (STAGED, `git diff --cached HEAD`)
**Branch:** `feature/dictionary-option`
**Reviewer context:** fresh; design read (`plans/DICT-2-mapper-wiring-validation.md` rev 2),
spec read (`docs/dictionary-option-spec.md` §4/5/6/7.1), all changed sources read, plus
surrounding `ir.cpp` / `generator.cpp` / `cpp_backend_schema_visitor.cpp` context.

## What I executed (not just read)

* Built `fletcher_plugin_core` + `fletcher_proto_plugin_tests` + `fletcher-protoc` from the
  staged tree (existing `protoc/build`, MSVC Release). **Unit suite: 93 passed / 1 skipped
  (`SchemaVisitor.CaptureGoldens`), 0 failed.**
* **Mutation-checked the four axes the implementer reported** — each reds *exactly* the
  intended sub-cases and nothing else:

  | mutation | result |
  |---|---|
  | `SameDictionaryOption` drops `a.ordered == b.ordered` | RED: `Ctl.ord_wrap was accepted, expected a rejection` (+2 follow-ons in the same sub-case) |
  | R5 drops `current->field_count() == 1` | RED: `Ctl.rep_multi was rejected: ... 'd2.W2.k' inside a repeated (fletcher.flatten) wrapper` — the B1 false-positive guard |
  | R2 gate `!= SCALAR` → `== LIST` | RED: `Shapes.labels`, `StructDict.st`, `Shapes.oc` accepted + 3 kind-word assertions |
  | R1 excludes WKTs (`google.protobuf.*` prefix) | RED: `Ctl.wkt_ff was accepted, expected a rejection` |

  Source restored byte-identically afterwards (`git diff -- src/type_mapper.cpp` empty).
* **Two extra mutations I added** — see finding **S4**: both leave the suite GREEN.
* Ran the real `protoc` + plugin over every fixture in
  `integration-tests/protoc-coverage/proto/` (`ipc,ts`) — the only failures are the
  intended negative fixtures (3 dictionary + 4 `Any`-unsupported). No collateral damage.
* Ran the plugin on all pre-existing DICT-1.5 guard fixtures with both `accessor` and `ipc`
  and confirmed **the same verdicts and messages as before** for
  `coverage_dictionary{,_wkt,_flatten,_struct_child,_list_child,_map_child,_unsupported}`,
  and that `coverage_dictionary` + `ipc,ts` still exits 0 with all four artifacts.
* Ran the three new/retargeted ctest shapes by hand: all three produce the expected message
  under **both** `ipc` and `accessor` (so the `_passOrder` pin holds), with **no artifacts
  written** on rejection.
* Ran a battery of *legal* shapes (scalar / proto3-optional / WKT-without-`flatten_field` /
  `flatten_field` on a scalar / `repeated` multi-field flatten wrapper / `repeated`
  single-field wrapper over a plain message / struct child / plain repeated / map) —
  **all still generate at exit 0. I found no false positive.**
* `clang-format 18.1.3 --dry-run -Werror` clean on all 7 changed C++ files. New emitted
  strings are ASCII-only.

## Verdict

**No blocking findings.** Both halves do what the design says, the rules reject exactly
their own shapes on everything I could construct, and the retarget is honest. Four
should-fix items (one diagnostics defect, two rule-coverage holes with an inaccurate
disclosure, one test-coverage gap), plus P2s and nits.

---

## Should-fix

### S1 — R1's remedy text is unfollowable for the WKT shape it deliberately covers
*Confidence: high (reproduced). Severity: should-fix.*

```
# message A { google.protobuf.StringValue x = 1
#   [(fletcher.flatten_field) = true, (fletcher.dictionary) = {}]; }
--fletcher_out: field 'xf.adv.A.x': (fletcher.dictionary) cannot be combined with
(fletcher.flatten_field): ... ; move the option onto the inlined field or fields
```

The inlined field is `google.protobuf.StringValue.value` — in a read-only WKT file the
author **cannot** annotate it. The actionable remedies are *remove
`(fletcher.flatten_field)`* (the WKT dictionary is then legal — locked #9's acceptance
case, pinned in the same test file as `Wkt.s`) or *remove the dictionary*. The wording is
also second-best for `flatten_field`-over-a-`flatten`-wrapper (`Ctl.ff_flat`), where the
option is *already* on the inlined field. Suggest appending: `... ; remove
(fletcher.flatten_field), or move the option onto the inlined field or fields`.

### S2 — R2/R3 do not fire for a `(fletcher.flatten)` wrapper reached through a non-flattening context
*Confidence: high (reproduced). Severity: should-fix.*

`ValidateDictionaryDeclarations` skips `IsFlattenedWrapper(msg)` and
`FindIllegalDictionaryField` never descends into STRUCT / MAP-value children, so a
wrapper's fields are judged **only** through a field that actually flattens it. A `map`
value does not flatten (`BuildMapNode` → `MakeStructNode(val_msg)`, `ir.cpp:516`, no
flatten resolution), so:

```proto
message W { option (fletcher.flatten) = true;
            string v = 1 [(fletcher.dictionary) = {ordered: true}]; }
message M  { map<string, W> m = 1; }                 // --fletcher_opt=ipc -> rc=0, EMITTED
message M2 { map<string, W> m = 1; W direct = 2; }    // -> correctly rejected (via `direct`)
```

Verified: the first exits 0 and emits `M.ipc` + headers, i.e. locked #8's `ordered: true`
is **silently ignored** — precisely the outcome §4 exists to prevent. Cheapest candidate
fix: drop the `IsFlattenedWrapper(msg)` skip *in this pass only*. My analysis says that
breaks no legal proto (every wrapper-field shape illegal when judged on its own is also
illegal in every flattened usage; `W1i8.value` stays a legal scalar dictionary), but it
would additionally start rejecting the sibling gap the design deliberately leaves quiet
(`W {flatten; repeated V vs}`) — so it is a decision, not a drive-by. Alternative: judge
messages reachable as struct/map children.

### S3 — imported-message hole, and the disclosure covering it is factually wrong
*Confidence: high (reproduced). Severity: should-fix.*

The pass iterates `OrderedMessages(file)`, which returns only messages whose
`msg->file() == file` (`generator.cpp:175`), and does **not** walk the IR subtree. Both
sibling passes **do** walk it — `FindUnsupportedIr` and `FindDictionaryField` recurse
through STRUCT / MAP / LIST children and therefore *do* report fields of imported
messages. Verified asymmetry on one proto pair:

```
outer.proto imports inner.proto;
message Inner { string k = 1 [(dictionary) = {ordered: true}];
                repeated string tags = 2 [(dictionary) = {}]; }

protoc --fletcher_opt=ipc      outer.proto -> rc=0, emits outer.Outer.ipc + headers
protoc --fletcher_opt=accessor outer.proto -> rc=1, "field 'xf.inner.Inner.k': ... RecordBatch accessor / Rust backend"
```

The in-tree `coverage_dictionary_struct_child` + `accessor` run shows the same thing: it
reports `...dict_guard_struct_child_inner.DictStructChildInner.k`, a field in an imported
file. So an illegal declaration in a **shared imported** `.proto` that is never itself a
fletcher generation unit is never reported, and two illegal shapes are silently dropped
from the emitted column. The design doc's accepted-gaps bullet 5 says this "mirrors
`ValidateNoUnsupportedIr` / `OrderedMessages`" — the *loop* mirrors it, the *detection
depth* does not. Either add the same struct-child descent the sibling walks have, or
correct the disclosure (and spec §7.1) to state the real boundary.

### S4 — both of R4's "TWO TERMS ARE LOAD-BEARING" are unpinned by any test
*Confidence: high (mutation-verified). Severity: should-fix (test gap; the code is correct today).*

The two terms the PM asked about **are present and correctly placed**: `if
(field->is_repeated()) return std::nullopt;` first; `if (inner->is_repeated()) break;`
*after* that inner's option is pushed; the `visited` set terminates recursive protos; and
the comparison is on the **decoded** `DictionaryOption`, both members — so `{}` ≡
UNSPECIFIED ≡ INT32 all compare equal (confirmed by `Ctl.agree_dflt`). But:

* deleting `if (field->is_repeated()) return std::nullopt;` → **suite still GREEN**;
* deleting `if (inner->is_repeated()) break;` → **suite still GREEN**.

No fixture exercises either boundary. (For the first: to reach R4 on a repeated field the
*outer* field must carry no option — otherwise R2 fires on the LIST kind — so stealing
needs **two inner** declarations down the chain, which no fixture has.) Add:

* (a) `repeated W xs;` with `W {flatten; V v = 1 [(dict INT8)];}`, `V {flatten; string s = 1
  [(dict INT64)];}` → must report R5's "inside a repeated (fletcher.flatten) wrapper", not
  "conflicting". (I ran this shape end-to-end: it correctly reports R5 today.)
* (b) `W w = 1;` with `W {flatten; repeated V vs = 1 [(dict A)];}`, `V {flatten; string s = 1
  [(dict B)];}` → must **not** contain "conflicting" (advice "make them identical" would
  lead to silence).

---

## P2

### P2-1 — a third full IR build per field, unconditional
`DictionaryUnsupportedReason` calls `ir::BuildFieldIr(field)` for **every** field of every
generated message, even when no `(fletcher.dictionary)` exists anywhere in the file, and
`BuildFieldIr` on a message field constructs the whole struct subtree
(`BuildStructVariant` → `BuildFieldIr` per child). That is the third such pass after
`ValidateNoUnsupportedIr` and `ValidateBackendsSupportFields`. An **exact** cheap guard
exists: `node.facts.dictionary` can be true only if `HasFieldDictionary(field)` (BaseFacts)
or the field is a singular message whose target `HasMessageFlatten` (the only propagation
route — `BuildFlattenedSingular`/`ApplyDictionaryFacts`). Gate the IR build and R2/R3 on
that disjunction; R1/R4/R5 are descriptor-only and already short-circuit. Front-end only,
so low practical impact — but this is the pass that pays on a large no-dictionary corpus.

### P2-2 — the DICT-2 comment block was inserted *inside* DICT-1.5's paragraph
`generator.cpp:1943-1961`: the pre-existing paragraph "Reject, for the read-only RBA
C++/Rust backends, the two shapes they cannot yet represent ..." now sits directly above
`if (!ValidateDictionaryDeclarations(...))`, so it reads as documentation of the wrong
call. Put DICT-2's block first, or keep each comment adjacent to its own call.

### P2-3 — new test assertions can crash instead of failing
`FindIllegalDictionaryField(d2->FindMessageTypeByName("RecFf"))` (and the `Ctl`, `W2`,
`W1`, `Pair`, `MemberHolder`, `StructDict` lookups) dereference the result without
`ASSERT_NE(..., nullptr)`; a rename/typo segfaults the binary rather than failing the test.
`DictionaryUnsupportedReason` / `FindIllegalDictionaryField` themselves have no
null-descriptor guard — fine for the production callers (`msg->field(i)` is non-null), but
they are public API and the test path is the exposed one. (`ReasonFor` / `IsLegal` do
guard, correctly.)

### P2-4 — two "exhaustive switch ⇒ compile error" comments over-claim
`DictionaryIndexArrowTypeExpr` ("adding a fifth `ir::DictionaryIndexKind` must be a compile
error here") and `DictionaryKindWord` ("a new kind is a compile error here"). The build sets
no `/WX` or `-Werror` (`protoc/CMakeLists.txt` carries no warning flags; the only `--Werror`
in CI is clang-format's), and **both functions have a fallback `return` after the switch**,
so a new enumerator would silently map to `arrow::int32()` / "a non-scalar column". Soften
the comments, or add a `static_assert` on an enum-count sentinel / `-Werror=switch`.

### P2-5 — user-facing docs state the rejection unconditionally
`docs/fletcher-options.md`: "The plugin fails generation — for **every** option set ... —
when the option is declared: on a field that does not map to a scalar column ...". Given S2
and S3 that is not unconditional. Qualify with "when the declaring message is part of the
generation unit".

---

## Nits

1. `EXPECT_TRUE(Mentions(r, "ordered"))` in the R4 `ord_wrap` sub-case is decorative —
   *every* R4 message contains "ordered" via `DescribeDictionaryOption`. The sub-case's
   non-vacuity comes entirely from `ReasonFor`'s "was accepted" `ADD_FAILURE` (which the
   mutation confirms fires). Asserting `"ordered true"` **and** `"ordered false"` would make
   the intent self-evident. The fixture itself is correct: `Ctl.ord_wrap` is `{INT8,
   ordered: true}` against `W1i8.value`'s `{INT8}`, so `ordered` is the sole difference; and
   `Ctl.ord_agree` genuinely hits R3 (`EXPECT_FALSE(Mentions(r, "conflicting"))` pins it).
2. The B4a comment claims the new `EXPECT_TRUE(ir::BuildFieldIr(rec->FindFieldByName("p"))
   .facts.dictionary)` is "the ONLY assertion anywhere" pinning that route. The new R2
   struct sub-case (`StructDict.st`) depends on the same `node.facts = BaseFacts(field)`
   line. The retarget's coverage claim still checks out — `Rec.p` (`Pair`: 2 fields, no
   flatten, behind `flatten_field`) takes **exactly** the same `BuildSingularMessage` struct
   branch as the ctest's `DictFfGuard.w` (`DictFfWrap`: 2 fields, no flatten) — it is
   double-pinned, not unique.
3. Comment line references are off by one and will drift: `ir.cpp:545` is
   `node.facts.nullable = ...`; the `BaseFacts` line is 544.
4. `rec->FindFieldByName("p")` is re-evaluated on the line after `wrapper` already holds it;
   use `wrapper`.
5. `DictionaryUnsupportedReason` sits beside the legacy `UnsupportedReason` with a
   near-identical name and an opposite contract (optional-illegality vs always-text).
   `DictionaryIllegalReason` would match its own `FindIllegalDictionaryField`.
6. R4 renders the **decoded** option, so an author who wrote `= {}` is told "index int32,
   ordered false" — text they never typed. Intentional (spelling equality), but a
   "(defaults)" hint would reduce confusion.
7. R2 is the only message citing `docs/dictionary-option-spec.md section 4`; R1/R3/R4/R5
   cite nothing. Pick one convention.
8. `ValidateDictionaryDeclarations`'s `IsRecursive(msg)` term is dead — `OrderedMessages`
   already drops recursive messages (`generator.cpp:177`). Harmless, and it mirrors the
   sibling passes, whose own comments say as much.
9. `FindIllegalDictionaryFieldImpl`'s `visited` set is shared across sibling `flatten_field`
   wrappers, so the same wrapper message reached from two fields of one message is judged
   once. Behaviourally identical here (the message names the inner field's FQN either way),
   but slightly weaker than "first offender in declaration order".
10. When a dictionary sits on the inner field of a *singular* flatten chain that resolves to
    a LIST, R2's prefix names the outer using field ("field 'M.w': ... maps to a list
    column") while the declaration lives on `W.vs`. R4/R5 name the declaring field; naming
    it here too would be more actionable.
11. No end-to-end ctest for R4/R5 (unit only). The wiring is already proven by the R1/R2/R3
    ctests, so the value is low — noting for completeness.
12. Fixture hygiene checked and **clean**: `coverage_dictionary_{repeated,ordered}.proto`
    (both pre-committed at `HEAD`) and the rewritten `coverage_dictionary_field_flatten.proto`
    all carry the SPDX + copyright header, `syntax = "proto3"`, and distinct packages
    (`dict2_repeated` / `dict2_ordered` / `dict_guard_field_flatten`); each new `add_test`
    has its own `OUT_DIR` subdirectory (the S3 `ctest -j` race rule);
    `pool.AddLinked(StringValue::GetDescriptor()->file())` is present in the new
    `TypeMapperTest.DictionaryMappingAndRejections` (needed for `Ctl.wkt_ff`). No proto
    outside those ten fixtures uses the option, and `DictionaryUnsupportedReason` provably
    returns `nullopt` when no dictionary is declared anywhere on/under a field — so the rest
    of the corpus is unaffected by construction.

---

## Points explicitly confirmed correct (for triage)

* **No false positives found.** R5's `field_count() == 1` term is present and does its job:
  `repeated W2 rep_multi` where `W2` is a 2-field flatten wrapper carrying a scalar
  dictionary is **accepted** (unit + end-to-end), matching `ir.cpp:366-372`'s
  `List<Struct(W2)>`; the single-field case (`Holder.rep_inner_declared`, plus my own
  `repeated W` chain variants) is still **rejected**. `flatten_field` on a scalar,
  WKT-without-`flatten_field`, struct children, `repeated`/`map` without the option, and
  singular flatten wrappers over plain messages all stay legal.
* **R2 gates on the top-level `NodeKind::SCALAR` only**, never a subtree OR; I re-verified
  `NodeKind::SCALAR ⟺ FieldKind::SCALAR` (`ProjectIrToFieldMapping`'s SCALAR branch is the
  sole producer and never returns `nullopt`).
* **`*error` is set and `false` returned** — nothing throws at the protoc-plugin boundary;
  no artifacts are written on rejection (verified by listing `OUT_DIR` after each run);
  messages are FQN-prefixed and match the ctest harness's `MATCHES` conventions.
* **Pass ordering is as documented and observable**: `ValidateNoUnsupportedIr` still wins
  (`coverage_dictionary_unsupported.proto` reports the `Any` error even though the same
  message carries a legal dictionary), and DICT-2 wins over DICT-1.5 under `accessor` on
  `coverage_dictionary_field_flatten.proto`. DICT-1.5's text provably cannot contain
  "cannot be combined with", so the `_passOrder` substring is the hard pin it claims.
* **`ir::DictionaryModifier` deletion is safe** — no remaining reference anywhere.
* **`FieldMapping::warning`'s new "dead channel" comment is accurate** — the only reader
  left in the tree is `test_type_mapper.cpp:372-373`; no emitter reads it.
* **`is_dictionary` / `dict_index_type_expr` are written from exactly one place**, derived
  from the same node's facts, with `m.scalar` untouched (locked #7); the D1 assertions cover
  all four index kinds plus both UNSPECIFIED spellings, nullability, enums and WKTs.

---

# Addendum — step-4b re-review (rev 2, base `8b56d62`)

Re-reviewed after the fix round. Rebuilt plugin + tests from the staged tree:
**unit suite 93 passed / 1 skipped / 0 failed**; `clang-format 18.1.3 -Werror` clean on all
7 changed C++ files; pristine coverage-corpus sweep (27 protos, `ipc,ts`) unchanged — the
same 7 intended negative fixtures and nothing else.

## S1 — CONFIRMED FIXED (advice is now followable)

```
# xf.adv2:  google.protobuf.StringValue x = 1 [(flatten_field), (dictionary) = {INT16}]
rc=1  ... cannot be combined with (fletcher.flatten_field): ...
      ; remove (fletcher.flatten_field), or move the option onto the inlined field or fields

# following the FIRST remedy literally (drop flatten_field, keep the dictionary):
# xf.adv2fix
rc=0  files=[adv2fix.A.ipc adv2fix.fletcher.arrow.pb.h adv2fix.fletcher.pb.h]
```

The remedy that works for the WKT case is now the one listed first, and the ctest substring
`cannot be combined with` is untouched. Good.

## S4 — CONFIRMED FIXED, and both mutations red *only* their intended sub-case

| deletion | reds | text |
|---|---|---|
| `if (field->is_repeated()) return std::nullopt;` | exactly 2 assertions, both under `SCOPED_TRACE("S4a: ...")` (lines 1746, 1748) | `R4 stole a shape from R5: ... conflicting ... index int8 ... on 'd2.WD8.v' vs index int64 ... on 'd2.VD8.s'` |
| `if (inner->is_repeated()) break;` | exactly 1 assertion, under `SCOPED_TRACE("S4b: ...")` (line 1769) | `must NOT report a conflict ir.cpp never sees: ... 'd2.VD9.u' vs ... 'd2.UD9.s'` |

No other sub-case or test moved in either run (92 passed + 1 failed test-case, 1 skipped).
The `EXPECT_FALSE(r.has_value()) << ... << *r` in S4b is safe — gtest only evaluates the
streamed message on the failure branch, where `r` has a value.

Folded items spot-checked and correct: **P2-2** (DICT-2's comment now precedes its own call
at `generator.cpp:1956`, DICT-1.5's paragraph follows at 1958), **P2-3** (`MsgOrFail` /
`IllegalIn` return `<missing message>` instead of dereferencing null), **P2-4** (both
comments now say "raises a -Wswitch WARNING ... not a compile error (the build sets no
-Werror//WX)" — accurate), **P2-5** (the docs qualifier "in the `.proto` being generated"
plus an explicit three-shape disclosure), **nits 1–4**.

## S2 — decline ACCEPTED. The false positive is real; I reproduced it.

Answering (a) for my fix #1 and (b):

**(a) Yes — dropping the `IsFlattenedWrapper` skip fires R1 on a live dictionary.** I applied
exactly that mutation to `ValidateDictionaryDeclarations` and rebuilt:

```proto
message FfLeaf { option (fletcher.flatten) = true; string s = 1; }
message FfW    { option (fletcher.flatten) = true;
                 FfLeaf p = 1 [(fletcher.flatten_field) = true,
                               (fletcher.dictionary) = {index_type: DICTIONARY_INDEX_INT16}]; }
message Chains { FfW ff_inside_wrapper = 3; int32 n = 4; }
```
* pristine: `rc=0`, and the emitted schema is **one scalar column named
  `ff_inside_wrapper`** — i.e. the declaration is resolved through the chain and is the
  carrier DICT-3/RIR will read (the unit test's `is_dictionary` / `arrow::int16()`).
* mutation: `rc=1`, `field 'xf.ffw.FfW.p': (fletcher.dictionary) cannot be combined with
  (fletcher.flatten_field) ...` — **a false positive on a working proto.**

So the argument holds for the wrapper skip, decisively. `flatten_field` inside a
`(fletcher.flatten)` wrapper really is a no-op, because the wrapper's fields are resolved by
the *using* field through `BuildFlattenedSingular`, which never consults `HasFieldFlatten`.
I withdraw that suggestion.

**(b) No — `Chains.ff_inside_wrapper` does NOT red if the skip is dropped.** Under that
mutation the whole unit suite stays green (93 passed), and the coverage corpus sweep is
byte-identical to pristine (same 7 intended failures). The reason: the unit tests call
`DictionaryUnsupportedReason` / `FindIllegalDictionaryField` directly and never call
`ValidateDictionaryDeclarations`, which is where the skip lives (file-local in
`generator.cpp`, not exported).

The test does pin the *reason* well — `EXPECT_TRUE(IllegalIn(d2, "FfW").has_value())` states
the asymmetry explicitly, which is the right documentation. But it is the same category of
gap as the S4 one the implementer just closed: **a future dev who deletes the skip gets a
green suite and a green ctest run.** Cheapest closure, using the harness's existing second
mode: a `coverage_dictionary_ff_inside_wrapper.proto` fixture wired with
`EXPECT_SUCCESS` + `EXPECT_ARTIFACTS` (`run_backend_guard_check.cmake` already supports it,
and `coverage_dictionary.proto` already uses it). **New finding: P1-A (should-fix, test
coverage only — no behaviour change).**

## S3 — I disagree, narrowly and on evidence. The false-positive argument does not cover it.

*Confidence: high (reproduced both directions). Severity: should-fix on the recorded
rationale; P2 on the code (a tested narrow closure exists).*

The declined rationale — now in spec §7.1.1 — reads:

> `(fletcher.flatten_field)` inlining lives only in `GatherFieldsImpl` /
> `BuildFlattenedFieldListImpl` ... **inside** a flatten wrapper **or a struct child** it is
> a no-op, so the declaration is resolved through the chain and **honoured by emission**.

**The struct-child half of that sentence is wrong.** A struct child's schema is not built
from the IR's `StructNode` children — it is a deep copy of the child *message's own* schema
function, which is generated by the inlining walk. Verified on generated output:

```proto
message FfLeaf { option (fletcher.flatten) = true; string s = 1; }
message M   { FfLeaf p = 1 [(fletcher.flatten_field) = true]; int32 q = 2; }
message Top { M m = 1; }
```
```
MSchema():    children[0]="s"   children[1]="q"      <-- flatten_field IS inlined inside M
TopSchema():  ArrowSchemaDeepCopy(MSchema(), children[0]); SetName(children[0], "m")
```
And the corroborating structural fact: the **only** consumers of `ir::StructNode.fields` in
the tree are the three validation walks (`generator.cpp:1639` `FindUnsupportedIr`, `:1701`
`FindScalarLeafNestedList`, `:1746` `FindDictionaryField`). **No emitter reads them.** So the
IR view in which "a struct child keeps `p` as a scalar carrying the dictionary" never reaches
an artifact — inside a struct child the declaration is *dropped*, exactly as at top level, and
R1 is **sound** there.

My earlier mutation-B result was contaminated and I retract it: my first fixture put `M` in
the *same file*, where the pristine plugin already rejects `M.p` (correctly — `M` is
generated on its own, so its `flatten_field` is inlined). The honest fixture puts `M` in an
imported file, and then the current behaviour is:

```
protoc inner2.proto   -> rc=1   field 'xf.i2.M.p': ... cannot be combined with (flatten_field)
protoc outer2.proto   -> rc=0   (imports inner2; Top.m child schema = DeepCopy(MSchema()),
                                 i.e. exactly the layout in which that declaration is dropped)
```

**The same declaration is fatal or silently accepted depending only on which `.proto` protoc
was pointed at.** That is not a false-positive risk; it is an inconsistency.

### The tested narrow closure (mutation C)

12 lines in `FindIllegalDictionaryFieldImpl`: also descend into a singular-message child /
map-value child, **but never into an `IsFlattenedWrapper` message** (that is precisely the
S2 shape the false-positive argument protects) and never into an `IsRecursive` one:

| shape | pristine | mutation C | correct? |
|---|---|---|---|
| `ffw` (live int16 dict inside a flatten wrapper) | rc=0 | **rc=0** | yes — no false positive |
| `outer2` (imported `M.p`; `inner2` alone is rc=1) | rc=0 silent | **rc=1**, names `xf.i2.M.p` | yes — verdict now file-order-independent |
| `outer` (imported `Inner.k` `ordered: true` + repeated dict) | rc=0 silent | **rc=1**, names `xf.inner.Inner.k` | yes — closes S3 |
| `mapw2` (S2's map-value-of-a-flatten-wrapper) | rc=0 | rc=0 | S2 stays open, by design |
| `fp`, `layout` (legal batteries) | rc=0 | rc=0 | no false positive |
| coverage corpus (27 protos) | 7 intended failures | **same 7** | no regression |
| unit suite | 93 pass | **93 pass** | no churn |

So the two halves are **separable**: the false-positive argument justifies keeping the
`IsFlattenedWrapper` skip (S2), and does **not** justify the absence of struct/map-child
descent (S3). Bundling them into one "design item with its own false-positive sweep"
overstates the cost of the cheap half.

**What I am asking for (not a block):**
1. **Correct the rationale** in spec §7.1.1 and in the round doc: narrow "inside a flatten
   wrapper or a struct child" to **flatten wrappers only**, and state that struct-child /
   map-value / imported descent is independently sound because struct children are emitted
   from the child message's own inlining walk (with the `MSchema()` evidence above). As
   written, the next reader will conclude the cheap half is unsound when it is not — and that
   is a rationale a future item will design against.
2. **Either take mutation C now** (my preference, given it is 12 lines with zero observed
   churn), **or name the follow-up item and gate it to land before DICT-3 starts emitting
   dictionary encoding.** Today every one of these shapes emits a plain value column, so the
   hole costs nothing; the moment DICT-3 branches on `ir::FieldFacts.dictionary`, these same
   shapes begin emitting `dictionary(...)` columns with `ordered: true` silently discarded —
   silent acceptance becomes a silently wrong column type. "A later item" without that gate
   is the part I would push back on.

Caveats I owe you on mutation C, since it is a 20-minute experiment and not a designed
change: it is a **behaviour change with blast radius** (it will surface pre-existing illegal
declarations in shared imported protos — the point, but it needs a real corpus sweep beyond
this repo); and the descent re-walks a diamond child once per referring message, so it
interacts with the P2-1 cost item below.

## Response to the deliberate skips

* **P2-1 (`BuildFieldIr` gate) — I accept the skip**, and agree with the reason: my guard was
  exact only while `ApplyDictionaryFacts` remains the sole propagation route, and a future
  `ir.cpp` change would turn a stale guard into a **false negative** (silent acceptance),
  which is strictly worse than the cost. One forward note: if the S3 descent ever lands, the
  gate stops being an optimisation and starts being load-bearing for cost — revisit them
  together, not separately.
* **nit 5 (rename) — agreed, skip.** It is a named design contract.
* **nit 8 (dead `IsRecursive` term) — agreed, skip**, and note it stops being dead the moment
  any descent is added (a child reached by descent is not filtered by `OrderedMessages`), so
  keeping the mirror is the right call.
* **nits 6/7/9/10/11 — agreed, skip.** I would still like **nit 10** (R2 naming the outer
  using field rather than the declaring field, for a dictionary on the inner field of a
  singular chain resolving to a LIST) carried as a diagnostics item alongside the S2/S3
  follow-up, since both touch the same "which field do we name" question. Not blocking.

## Addendum finding list

| id | severity | item |
|---|---|---|
| P1-A | should-fix (test only) | nothing reds if the `IsFlattenedWrapper` skip is deleted — the pass has no test at all. Add an `EXPECT_SUCCESS` coverage fixture for the `FfW` / `ff_inside_wrapper` shape. |
| P1-B | should-fix (docs/rationale) | spec §7.1.1's "or a struct child" clause is factually wrong; struct children are emitted from the child message's own inlining walk (`MSchema()` evidence). Narrow it to flatten wrappers. |
| P2-A | P2 | S3 is closable now with a 12-line, zero-churn descent (mutation C, table above). Take it, or gate the follow-up to land before DICT-3 emits dictionary encoding. |
| — | confirmed | S1 fixed and followable; S4 fixed and both mutations red only their intended sub-case; P2-2/3/4/5 and nits 1–4 folded correctly; S2 decline accepted, suggestion withdrawn. |

---

# Addendum 2 — the list-element descent extension (rev 3, unstaged, base `8b56d62`)

Rebuilt plugin + tests from the working tree: **unit suite 93 passed / 1 skipped / 0 failed**;
`clang-format -Werror` clean; corpus sweep **12 accepted / 7 rejected**, all 7 intended —
matching the implementer's report. Sources restored byte-identically after every mutation
(`diff -q` against the pre-mutation baseline, zero `MUTATION` markers left).

## Verdict: the extension is SAFE. Keep it. Two doc-level corrections below.

### (a) Yes — the list element is safe by exactly the argument that made singular/map-value safe

Generated `TopSchema()` for `Top { repeated M xs = 1; map<string,M> mm = 2; M single = 3;
repeated NestWrap nested = 4; }` (where `M { FfLeaf p = 1 [(flatten_field)]; int32 q = 2; }`):

```
ArrowSchemaDeepCopy(MSchema().get(), children[0]->children[0]);                 // list element
ArrowSchemaDeepCopy(MSchema().get(), children[1]->children[0]->children[1]);    // map value
ArrowSchemaDeepCopy(MSchema().get(), children[2]);                              // singular
ArrowSchemaDeepCopy(MSchema().get(), children[3]->children[0]->children[0]);    // nested-list leaf
```
`MSchema()` emits `children[0]="s"`, `children[1]="q"` — M's own inlining walk, so the
`flatten_field` field really is inlined and its dictionary really is dropped, at the list
element exactly as at the singular position. The row path agrees: the decode side reads
`MSchema()->n_children` at the same four positions. And the corroborating fact is unchanged
repo-wide: **no emitter reads `ir::StructNode.fields`** (only the three validation walks).
The Rust/RBA emitter reaches child messages at the same four positions
(`recordbatch_accessor_emitter.cpp:1257/1264/1274`, via the child's own `GatherFields`), so
the same conclusion holds there. **No false positive found (see (c)).**

### (b) The `DeepCopyMessageStruct` invariant does NOT hold as stated — two corrections

**b1 (wording).** There are **two** syntactic call sites, not three
(`cpp_backend_schema_visitor.cpp:449` `case NodeKind::STRUCT`, `:468` `case NodeKind::MAP`),
covering **four** reachable positions (singular struct, `List<Struct>` element,
`List<List<...<Struct>>>` leaf, map value) — the LIST case has no call of its own, it recurses
into `EmitNodeType`. The comment's own parenthetical already says "see its NodeKind::STRUCT
and NodeKind::MAP cases", which contradicts "the three call sites". Say "three descriptor
positions, covering the four emission positions reached through two call sites".

**b2 (substantive — a deep-copy position the walk does not judge).** A struct leaf reached
*through* a `(fletcher.flatten)` wrapper is deep-copied by emission but excluded from the
descent by `!IsFlattenedWrapper(child)`. Reproduced twice, with `xf.li.Inner` carrying
`ordered: true` and a `repeated` dictionary:

```
# repeated <wrapper over a repeated imported message>
message NestWrap { option (fletcher.flatten) = true; repeated xf.li.Inner ms = 1; }
message Top      { repeated NestWrap nested = 1; int32 n = 2; }
protoc leaf_outer.proto  -> rc=0  (silent)   emits ArrowSchemaDeepCopy(
                                             ::fletcher_gen::xf::li::InnerSchema(),
                                             children[0]->children[0]->children[0])
protoc leaf_inner.proto  -> rc=1  field 'xf.li.Inner.k': ... ordered: true is not supported

# singular variant: message W { option (fletcher.flatten) = true; xf.li.Inner m = 1; }
protoc leaf_sing.proto   -> rc=0  (silent)   5 references to InnerSchema()
```
So **the file-choice inconsistency the extension exists to remove survives one wrapper hop
away.** Positive control: a *plain* (non-wrapper) intermediate chains correctly across files —
`Mid { xf.li.Inner i = 1; } Top { repeated Mid ms = 1; }` → rc=1 naming `xf.li.Inner.k`.

This is **not** an argument against the extension (which strictly reduces the inconsistency),
and the fix is **not** to descend into the wrapper — that reintroduces the R1 false positive on
the wrapper's own fields, which is now properly pinned. The fix is to follow the wrapper's
chain to the *leaf* descriptor (mirroring `BuildFlattenedRepeated` / `BuildFlattenedSingular`)
and descend into that. That is design work and belongs with the S2 follow-up.

**Ask:** add this shape to the disclosed set in spec §7.1.1 (it is currently *implied absent*
by "exactly the deep-copy sites") and fold it into the S2 follow-up's scope. Docs-only;
no code change requested. → **P1-C (should-fix, rationale/disclosure)**, **nit-A** for b1.

### (c) No legal shape is newly rejected

| battery | result |
|---|---|
| `legal2`: `repeated Dicty` / `map<string,Dicty>` / singular / `List<List<Struct(Dicty)>>` / multi-field wrapper behind `repeated` / `List<List<Scalar>>` / `flatten_field` scalar wrapper / plain repeated / plain map — **live int8 + WKT dictionaries inside the child at every position** | rc=0, 6 artifacts |
| `fp`, `layout`, `ffw`, `mapw2`, `listelem` (earlier batteries) | all rc=0 |
| coverage corpus (20 protos incl. the new fixture) | 12 accepted / 7 rejected, all intended |
| unit suite | 93 pass / 1 skip |

## Must-fixes verified

* **P1-A pin — genuinely closed, and it reproduces my finding exactly.** Dropping the
  top-level `IsFlattenedWrapper` skip: the plugin rejects
  `coverage_dictionary_wrapper_live.proto` (`...DictWrapLiveWrap.p`, 0 artifacts) → the
  `EXPECT_SUCCESS` ctest reds, while the **unit suite stays 93/93 green**. Dropping the
  descent's `!IsFlattenedWrapper(child)`: **both** red (ctest, plus
  `IllegalIn(d2, "HostMapWrapper")` at `test_type_mapper.cpp:1839`). That is exactly the
  "ctest catches what the unit suite cannot" property P1-A asked for.
* **P1-B rationale — fixed.** "or a struct child" is gone; the decision table keyed on "does
  some generated schema function inline this message's fields?" is the right axis and carries
  the `MSchema()` evidence.
* **P2-A — taken, plus the list-element extension, which I endorse.**
* Nit 10 carried with the `facts.proto_full_name` trap documented — good catch, that trap is
  real and I had not spotted it.
* **P2-1 re-check accepted:** `visited` does bound the descent to once per root, so the cost
  argument I raised does not escalate; and the exactness caveat is the right reason to leave
  the gate alone.

### Addendum 2 finding list

| id | severity | item |
|---|---|---|
| P1-C | should-fix (docs) | a struct leaf reached *through* a `(fletcher.flatten)` wrapper is a `DeepCopyMessageStruct` position the descent does not judge (reproduced, two shapes). Add to spec §7.1.1's disclosed set and to the S2 follow-up's scope; do **not** close it by descending into the wrapper. |
| nit-A | nit | "the three call sites of `DeepCopyMessageStruct`" — there are two call sites and four emission positions; reword to "three descriptor positions covering four emission positions reached through two call sites". |
| — | confirmed | extension safe: (a) yes, same mechanism, evidence above; (c) no new rejection of any legal shape; all three must-fixes verified, including both mutation directions of the new pin. |
