# DICT-2 — Mapper wiring + validation (design)

Round DICT · item DICT-2 · branch `feature/dictionary-option` (base `59edf9c`)
Story: [DICT-dictionary-option.md](DICT-dictionary-option.md) § "DICT-2 — Mapper wiring + validation"
Spec: [docs/dictionary-option-spec.md](../docs/dictionary-option-spec.md) §4, §6, §7.1
Locked: [DICT-locked-decisions.md](DICT-locked-decisions.md) #5, #8, #9, #11
Forcing test: `TypeMapperTest.DictionaryMappingAndRejections`

> **Revision 2 (2026-08-28)** — reworked after the step-2 review below
> (5 blocking + 3 required, all addressed; see
> [Step-2 rework](#step-2-rework-2026-08-28--how-each-item-was-addressed)).

---

## Summary

Wire `(fletcher.dictionary)` into the flat `FieldMapping` projection (`is_dictionary`
+ `dict_index_type_expr`, derived from `ir::FieldFacts.dictionary`, value type
unchanged), and add **one** front-end validation pass that fatally rejects every
illegal declaration: non-`SCALAR` mapped kind (locked #9), `ordered: true`
(locked #8), a `(fletcher.flatten_field)` wrapper carrying the option (spec §7.1
gap 1), an inner declaration under a **single-field** `repeated` flatten wrapper
(gap 2), and a *disagreeing* wrapper/leaf pair. Also deletes the dead
`ir::DictionaryModifier`.

---

## Design

### D0 — Where enforcement lives, and why NOT `MapField() -> nullopt`

The story's Scope says "`MapField`/`UnsupportedReason`: reject the option on
non-`SCALAR`-mapped fields". **Taken literally that is wrong on this tree, and
this design deliberately does not do it.** `ProjectIrToFieldMapping` returning
`nullopt` is not a rejection — it is a *silent drop*:

* `GatherFieldsImpl` (`generator.cpp:615-620`) turns `nullopt` into a
  `skipped_comment` line in the generated header and continues; exit code 0.
* The schema walk does not call the projection at all. It uses
  `IsSchemaRepresentable` (`cpp_backend_schema_visitor.cpp:31-61`), kept "in exact
  structural lockstep" with `ProjectIrToFieldMapping` *by hand*. So a projection
  that starts rejecting dictionary shapes without a mirrored edit there makes the
  **row header drop a field the schema still declares** — precisely the row/schema
  drift GIR-5 existed to remove.
* Even with both edited, the user-visible result is a **missing column**, which is
  worse than the value-typed column we are trying to prevent.

Therefore: **legality is a fatal front-end pass**, in the family that already owns
"fail the plugin before any artifact is written" — `ValidateNoUnsupportedIr` (#55)
and `ValidateBackendsSupportFields` (GIR-10 / DICT-1.5). The projection keeps its
current total behaviour and gains only the *positive* dictionary fields.

The rule itself lives in **`type_mapper.{hpp,cpp}`** next to `UnsupportedReason`
(the module the story names, and the module the forcing test's suite name claims),
so the forcing test is a genuine unit test of `type_mapper` and the generator pass
is a four-line loop.

The story's Scope bullet must be corrected to match (required item 8): the same
sentence, with "reject … → `nullopt`" replaced by "reject via the front-end
validation pass; `MapField` stays total".

### D1 — The positive path: projection + one backend string

`type_mapper.hpp`, on `FieldMapping` (additive, defaults preserve today's value;
`FieldMapping` is never positionally aggregate-initialised repo-wide, so adding
members is safe):

```cpp
    // DICT-2 (spec section 5): DERIVED projection of ir::FieldFacts.dictionary,
    // the ONE canonical carrier (locked #5). NOT a second source of truth: never
    // written from a descriptor read, never written by an emitter. `scalar` stays
    // the VALUE type (locked #7) — nothing here changes storage/setter/getter/
    // wire/TS behaviour.
    // Set ONLY when kind == FieldKind::SCALAR: locked #9 makes the option legal
    // only there, ValidateDictionaryDeclarations rejects every other kind
    // fatally before any emitter runs, and a `list(dictionary(...))` carrier is
    // explicitly out of scope (spec section 8).
    // NAMED CONSUMER — round RIR's IR-based RecordBatch accessor. Spec section
    // 5.1 specifies its type gate in EXACTLY this spelling:
    // arrow::dictionary(<dict_index_type_expr>, <scalar.arrow_type_expr>).
    // These fields CANNOT reach today's read-only RBA emitter: DICT-1.5 fails the
    // plugin for --fletcher_opt=accessor,rust on any dictionary proto (locked
    // #11), and RIR removes that guard in the same change that consumes them.
    bool is_dictionary = false;
    std::string dict_index_type_expr;  // e.g. "arrow::int16()"; empty iff !is_dictionary
```

`ProjectIrToFieldMapping`, `NodeKind::SCALAR` branch only:

```cpp
            if (node.facts.dictionary) {
                m.is_dictionary = true;
                m.dict_index_type_expr =
                    cpp_backend::DictionaryIndexArrowTypeExpr(node.facts.dictionary_index_kind);
            }
```

`cpp_backend_type_table.{hpp,cpp}` gains the only new backend string (GIR locked
#1: Arrow type text lives *only* in this table; DICT-1's hand-off put this mapping
here explicitly):

```cpp
// DICT-2: the declared index type of a dictionary column as a C++ Arrow type
// expression ("arrow::int8()" .. "arrow::int64()"). Exhaustive 4-case switch, no
// `default`. Deliberately NOT routed through LookupScalar: no proto type maps to
// int8/int16, so those logical kinds have no CppScalarInfo row and feeding an
// index kind to LookupScalar would be a category error.
std::string DictionaryIndexArrowTypeExpr(ir::DictionaryIndexKind kind);
```

No emitter reads `is_dictionary` **in DICT-2** — DICT-3 branches on
`facts.dictionary` inside the one `cpp_backend::SchemaVisitor` (see D8/D11). The
fields still belong here, and this is categorically different from the dead
`ir::DictionaryModifier` that D7 deletes:

* `dict_index_type_expr`'s `arrow::...()` spelling is the exact form spec §5.1
  names for the accessor's dictionary type gate, and that spelling is what
  `recordbatch_accessor_emitter.cpp` / `cpp_backend_view_visitor.cpp` consume — so
  the field has a **named consumer in round RIR**, whereas `DictionaryModifier`
  had none, ever;
* they are the surface spec §5's implementation hook specifies;
* they are derived-on-demand from the IR, so they cannot drift from the carrier;
* they give the forcing test a cheap, non-vacuous observation point for "value
  type unchanged / nullability preserved / index type correct".

**Carry-forward to record in the plan (so it cannot orphan):** RIR consumes
`FieldMapping.is_dictionary` / `dict_index_type_expr` for the accessor's
`dictionary(idx, val)` gate, in the same change that removes the DICT-1.5 guard.

### D2 — The legality rule (the class, not the fixture)

`type_mapper.hpp`:

```cpp
// DICT-2 (spec sections 4/6, locked #8/#9): why a (fletcher.dictionary)
// declaration reachable from `field` is illegal, or nullopt when it is legal.
// `field` must be a field that would actually become a column of a generated
// message (a message's own declared field, or a field inlined through a
// (fletcher.flatten_field) wrapper — see FindIllegalDictionaryField).
std::optional<std::string> DictionaryUnsupportedReason(
    const google::protobuf::FieldDescriptor* field);

// First illegal declaration among `msg`'s own columns, in declaration order,
// descending through (fletcher.flatten_field) wrappers exactly as the two field
// walks do. nullopt when `msg` is clean.
std::optional<std::string> FindIllegalDictionaryField(
    const google::protobuf::Descriptor* msg);
```

`DictionaryUnsupportedReason` applies these rules **in this order** (first match
wins). The order is root-cause-first *for one declaration*: fixing the reported
defect on that declaration never just reveals a differently-worded complaint about
the same declaration. It is **not** an absolute property across a whole shape —
e.g. `repeated W xs [(dictionary)]` whose inner field also declares reports R2,
and once the outer option is deleted R5 reports the inner one. Both messages are
true and each names its own declaration.

| # | Rule | Fires when | Owner |
|---|---|---|---|
| R1 | **`flatten_field` wrapper** | `field->type()==TYPE_MESSAGE && !field->is_repeated() && HasFieldFlatten(field) && HasFieldDictionary(field)` | spec §7.1 gap 1 (obligation 1) |
| R2 | **kind gate** | `node.facts.dictionary && node.kind != ir::NodeKind::SCALAR` (`UNSUPPORTED` gets its own wording, see D9) | locked #9, spec §4 |
| R3 | **`ordered: true`** | `node.facts.dictionary && node.facts.dictionary_ordered` | locked #8, spec §6 |
| R4 | **conflicting chain** | two or more declarations on one **singular, single-field** message-level-flatten chain whose **decoded `DictionaryOption`s are not equal** (`index_kind` **and** `ordered`) | obligation 4 |
| R5 | **inner-declared under a `repeated` flatten wrapper** | `field->is_repeated() && TYPE_MESSAGE && HasMessageFlatten(type) && type->field_count() == 1` and some **inner** field on that chain carries the option | spec §7.1 gap 2 (obligation 2) |

where `node = ir::BuildFieldIr(field)` (built once, reused by R2/R3).

Load-bearing details:

* **R1's predicate is the full three-term one**, byte-identical to the predicate
  both inlining walks use (`generator.cpp:606-607`,
  `cpp_backend_schema_visitor.cpp:78-79`). `HasFieldFlatten` alone would wrongly
  reject `string s = 2 [(flatten_field), (dictionary)]`, which spec §4 says is a
  documented **no-op + legal dictionary** (DICT-1 pins it: `Rec.s`).
* **R1 precedes R2** because a `flatten_field` wrapper's `BuildFieldIr` node *is*
  a `STRUCT` (`BaseFacts(w)` at `ir.cpp:545` reads the wrapper's own option), so
  R2 would fire too — with the misleading text "maps to a struct column", for a
  field that is never emitted as a struct. R1's text names the real cause. (R2 is
  *not* a safety net for R1 either — see D4.)
* **R2 gates on the TOP-LEVEL node only.** Never an OR over the subtree. Spec
  §7.1's closing rule is explicit that the subtree-OR is licensed *only* for
  DICT-1.5's backend-availability predicate; for a legality gate, over-rejecting
  permanently outlaws a legal proto (a scalar dictionary inside a struct child).
  Each such inner field is judged on **its own** mapped kind, when its own
  message is walked (D3).
* **`NodeKind::SCALAR` is exactly locked #9's `FieldKind::SCALAR`.** Verified in
  `ProjectIrToFieldMapping` (`type_mapper.cpp:154-162`): `NodeKind::SCALAR` is the
  only producer of `FieldKind::SCALAR`, and it always produces it. Gating on the
  IR kind is therefore the same gate, and additionally well-defined for the kinds
  where the projection returns `nullopt` (`FIXED_SIZE_LIST`, `UNSUPPORTED`).
* Kind-word for the R2 message comes from an **exhaustive switch over
  `ir::NodeKind`** (no `default`): `LIST` → "a list column", `MAP` → "a map
  column", `STRUCT` → "a struct column", `FIXED_SIZE_LIST` → "a fixed-size-list
  column", `SCALAR` unreachable; `UNSUPPORTED` uses the dedicated clause in D9
  rather than a kind-word (it would otherwise read "maps to an unsupported field
  type").
* **R4/R5 walk descriptors, not the IR, and that is required, not lazy.** The IR
  keeps only the *winner* of a conflict (`ApplyDictionaryFacts` / the
  `if (!inner_ir.facts.dictionary)` guard at `ir.cpp:332-336`), and
  `BuildFlattenedRepeated` never reads the inner field at all — so neither
  condition is visible on any IR node. Both walks mirror their `ir.cpp`
  counterpart's loop shape **including its `field_count() == 1` term**:

  ```cpp
  // R4 — mirrors BuildFlattenedSingular's recursion (ir.cpp:309-338).
  // Collect the OUTER field's option too: a conflict is between the outer field
  // and the chain, and R2/R3 do not compare anything.
  // TWO TERMS ARE LOAD-BEARING (step-2 cycle-2 item R2 below): R4 is the
  // SINGULAR-chain rule, and it must stop where ir.cpp stops READING.
  if (field->is_repeated()) return nullopt;          // repeated outer -> R5's business
  decls = { ReadFieldDictionaryOption(field) if present };
  msg = field->message_type();
  while (HasMessageFlatten(msg) && msg->field_count() == 1 && visited.insert(msg).second) {
      inner = msg->field(0);
      if (auto d = ReadFieldDictionaryOption(inner)) decls.push_back(*d);
      // BuildFieldIr(inner) routes a REPEATED inner to BuildRepeatedMessage ->
      // BuildFlattenedRepeated, which reads BaseFacts(inner) and then NEVER reads
      // a deeper field. So `inner`'s own option counts (collected above) but the
      // chain below it is not read by ir.cpp and must not be collected here:
      // collecting it would make R4 report a "conflict" between two declarations
      // NEITHER of which is used, with advice ("make them identical") that leads
      // to silence rather than a fix.
      if (inner->is_repeated()) break;
      if (inner->type() != TYPE_MESSAGE) break;
      msg = inner->message_type();
  }
  // illegal iff any two entries of `decls` are unequal (index_kind, ordered)

  // R5 — mirrors BuildFlattenedRepeated's chain loop (ir.cpp:374-427). Same loop,
  // same `field_count() == 1` term (LOAD-BEARING: see D5), but collects INNER
  // declarations ONLY — a declaration on the outer field is R2's business, and
  // double-reporting it here would give the shape two different messages.
  ```

  Both carry a `visited` set of `const Descriptor*`. The containing message of a
  self-referential flatten wrapper is already `IsRecursive` and skipped by the
  pass (D3), so a cycle is unreachable today — the set is a cheap standing
  guarantee, **not** a claimed fix for the unguarded loops in `ir.cpp` (leave
  those alone; out of scope).
* **R4's equality is on the decoded `DictionaryOption`, both members.** Two
  consequences, both required:
  * `= {}`, `{index_type: DICTIONARY_INDEX_UNSPECIFIED}` and
    `{index_type: DICTIONARY_INDEX_INT32}` are **equal** — spelling differences
    are not conflicts (locked #4 resolves UNSPECIFIED → int32 in the reader).
  * Comparing `index_kind` alone would leave **locked #8 unenforced** for a
    wrapper-declared `ordered`. Leaf-wins at `ir.cpp:332-336` discards the whole
    outer option, so for `W w [(dictionary) = {ordered: true}]` over a leaf
    declaring `{index_type: INT8}` the resolved node has
    `dictionary_ordered == false` and **R3 cannot see it**. R4 is the only rule
    that catches that proto. Closure argument: if the two options are *equal* and
    either sets `ordered`, the resolved node carries `ordered == true` and R3
    fires; if they differ in any member, R4 fires.
    **SCOPED (step-4 review N2, 2026-08-28):** the closure holds for every chain
    R4 actually walks, i.e. every **singular, single-field** flatten chain judged
    from a generated message's own column. It is **not** absolute over all protos:
    the closure sentence previously read "no `ordered: true` anywhere on a flatten
    chain can be silently swallowed", which is false for the **two** disclosed
    detection-boundary holes (gap 2's sibling, and step-4's S2 — a flatten wrapper
    reached through a `map` value / struct child; step-4's S3 was **closed**, see
    the addendum) — both of which drop `ordered: true` silently. Probed: `M{W w}` / `W{flatten; repeated V
    vs}` / `V{flatten; string s [(dictionary)={INT8, ordered: true}]}` generates at
    exit 0 with no diagnostic.

### D3 — The walk and the pass

`FindIllegalDictionaryField(msg)` — mirrors the inlining walks so it sees exactly
the fields that become columns:

```cpp
for each i in msg->field_count():
    fd = msg->field(i)
    if (auto e = DictionaryUnsupportedReason(fd)) return e;        // includes R1
    if (fd->type()==TYPE_MESSAGE && !fd->is_repeated() && HasFieldFlatten(fd))
        recurse into fd->message_type()   // R1 already cleared this wrapper
```
(with a `visited` descriptor set on the recursion, same rationale as D2.)

`generator.cpp` (anonymous namespace, beside its two siblings):

```cpp
// DICT-2: reject illegal (fletcher.dictionary) declarations BEFORE any artifact
// is written. Backend-INDEPENDENT (unlike ValidateBackendsSupportFields): an
// illegal declaration is a defect in the .proto, not a backend gap, so this runs
// for every option set including schema_only.
bool ValidateDictionaryDeclarations(const google::protobuf::FileDescriptor* file,
                                    std::string* error) {
    for (const auto* msg : OrderedMessages(file)) {
        if (IsRecursive(msg) || IsFlattenedWrapper(msg)) continue;   // mirror emit
        if (auto e = FindIllegalDictionaryField(msg)) { *error = *e; return false; }
    }
    return true;
}
```

The skip predicate is the same `IsRecursive || IsFlattenedWrapper` the emit loops
and both existing passes use, so validation fires only on messages that are
actually generated. A flatten wrapper's inner declaration is still judged — via
the *using* field's resolved node (R2/R3/R4) or via R5 — so skipping the wrapper
loses nothing. A declared-but-never-used wrapper is not judged; that mirrors emit
(nothing is generated for it) and is recorded in Risks.

**Call site and ordering** — inserted in `Generate()` immediately above the
existing `ValidateBackendsSupportFields` call (`generator.cpp:1917`), i.e.:

```
ValidateNoUnsupportedIr (#55)  ->  ParseMetadataRules / resolver  ->
ValidateDictionaryDeclarations (DICT-2)  ->  ValidateBackendsSupportFields (DICT-1.5)
```

* **After #55**, for the same reason DICT-1.5 gave: a genuinely unsupported field
  type must report its own error rather than be masked by an option-legality
  complaint. (`GenErrors.DictionaryGuardDoesNotMaskUnsupportedType` keeps passing
  unchanged and now also covers this pass.)
* **Before DICT-1.5**, deliberately: DICT-1.5's remedy is "regenerate without
  `--fletcher_opt=accessor,rust`", which for an *illegal* declaration is actively
  misleading advice — following it yields a second error. DICT-2's message is the
  root cause and must win. See D4 for the one test this reorders.
* Relative order of everything pre-existing is unchanged (one inserted line).

### D4 — Obligation 1: the `flatten_field` wrapper hole, and DICT-1.5

The hole (DICT-1 design D4b): both field walks `continue` past a
`(fletcher.flatten_field)` wrapper, so the wrapper's node never reaches the
*projection*, so no projection-level gate can ever fire — and spec §4 requires
**rejection** (N inlined columns, no single column to encode).

**Resolution: R1, a descriptor-level rule inside the raw-descriptor walk of D3.**
D4b's required "raw-descriptor walk" is exactly `FindIllegalDictionaryField`: it
is rooted at each message's own declared fields, never at the projection's output,
and it re-derives the wrapper predicate from the descriptor rather than trusting a
walk that drops it. The known-hazard framing from GIR-13 is honoured by *not*
reusing either dropping walk and by pinning the predicate's three terms in a test
(`Rec.s` must stay legal, `Rec.p` must be rejected).

**R1 is not redundant with R2.** There are *two* distinct shapes where R2 accepts
and emission drops the fact — R1 is the only rule that catches either:

| Shape | `BuildFieldIr` (what R2 sees) | What the inlining walks emit |
|---|---|---|
| `W w [(flatten_field), (dictionary)]` where `W` also has message-level `(fletcher.flatten)` + 1 field | `BuildSingularMessage` → `BuildFlattenedSingular` → **`SCALAR`** with the wrapper's dictionary propagated (`ir.cpp:332-336`) → R2 **accepts** | `continue` past `w`, record `BuildFieldIr(inner)`, which carries **nothing** → a **value-typed** column |
| `google.protobuf.StringValue s [(flatten_field), (dictionary)]` | `TryBuildWkt` (`ir.cpp:601`) → nullable **`SCALAR`** carrying the dictionary → R2 **accepts** | both walks gate only on `TYPE_MESSAGE && !is_repeated && HasFieldFlatten`, so they inline `StringValue.value` as a plain **non-nullable `utf8`** column, dictionary-less |

The WKT row is called out explicitly because it looks like locked #9's
"a wrapper carrying the option is a valid nullable dictionary" acceptance case.
It is not: locked #9 is about a WKT wrapper **without** `flatten_field`
(`coverage_dictionary_wkt.proto`, still legal and still a DICT-1.5 true positive).
**Excluding WKTs from R1 would silently reopen the hole** — do not "fix" it.

**Interaction with DICT-1.5, and which fires first.** DICT-1.5's guard *does* see
the wrapper shape (it calls `ir::BuildFieldIr` directly, so `BaseFacts(w)` is
read) but only when `accessor`/`rust` is requested, and it runs later. So:

* **DICT-2 fires first, always, for every option set.** Correct: the declaration
  is illegal irrespective of backends.
* Consequence, accepted and executed deliberately:
  `GenErrors.DictionaryRejectedBy_accessor_fieldFlatten` (which asserts the
  DICT-1.5 message on `coverage_dictionary_field_flatten.proto`) **would go red**.
  DICT-2 **retargets** it: rename to
  `GenErrors.DictionaryOnFlattenFieldWrapperRejected`, `FLETCHER_OPT=ipc` (proving
  the rejection is backend-independent), `EXPECT_MESSAGE` = DICT-2's R1 text,
  `EXPECT_FIELD=DictFfGuard.w` unchanged; **plus** a sibling
  `..._passOrder` with `FLETCHER_OPT=accessor` asserting the **DICT-2** message
  still wins, which pins the ordering decision above.
* **The retarget DOES remove one real check, and DICT-2 replaces it.** That ctest
  is today the **only** assertion anywhere that
  `ir::BuildFieldIr(<flatten_field wrapper>).facts.dictionary` is true: DICT-1's
  D4b sub-case (`test_type_mapper.cpp:1131-1158`) asserts only
  `HasFieldDictionary(wrapper)` and the *projection-level* drop via
  `BuildFlattenedFieldList` — never the IR read. R1 is descriptor-based, so it
  does not cover that route either. Two cheap, mandatory replacements:
  * **(a)** add `EXPECT_TRUE(ir::BuildFieldIr(rec->FindFieldByName("p")).facts.dictionary);`
    to that D4b sub-case (an assertion, not merely the comment the earlier
    revision planned), keeping DICT-1.5's `FindDictionaryField` wrapper edge
    guarded;
  * **(b)** rewrite `coverage_dictionary_field_flatten.proto`'s header comment
    (lines 7-14), which after the retarget documents a route the tests no longer
    exercise ("the only route … is `BaseFacts(w)` … the guard would wrongly exit
    0"). A fixture whose docstring describes a check it no longer performs is this
    round's dominant defect class (DICT-1.5 process notes 1-3). The new comment
    states: this shape is now **illegal** (R1), the plugin rejects it before any
    backend guard runs, and the IR route it used to pin is asserted in
    `TypeMapperTest.ReadsDictionaryOption`.
  With (a)+(b) the substantive claim stands, stated precisely: post-R1 the shape
  is unreachable from any *accepted* proto, so DICT-1.5's wrapper route becomes
  **dead information rather than unguarded safety** — and the route itself stays
  pinned at the unit level.
* Every other edge of `FindDictionaryField`'s walk keeps its own isolating fixture
  (`_flatten`, `_structChild`, `_listChild`, `_mapChild`, `_wkt`, root scalar) —
  and all of those remain **legal** under DICT-2, so they double as DICT-2
  false-positive guards: their `EXPECT_MESSAGE` is DICT-1.5's text, so if DICT-2
  wrongly rejected any of them the test goes red.
* DICT-1's D4b pinning sub-case does **not** flip (the plan predicted a flip):
  DICT-2 changes no IR and no walk, so the IR still drops the wrapper declaration
  at the projection. It gains assertion (a) plus one comment line explaining that
  the shape is now rejected by R1 *because* the projection drop is real.

### D5 — Obligation 2: `BuildFlattenedRepeated`'s inner-declared drop

The deferral's reasoning **still holds**: all seven return sites
(`ir.cpp:367,387,397,414,415,430,434`) are `LIST`/`UNSUPPORTED`, R2 rejects both,
and emission drops the fact identically (`GatherFieldsImpl`'s inline branch
requires `!fd->is_repeated()`), so there is no mis-read and never was.

**The gap's stated cost — "the rejection cannot *fire*, so it stays quiet instead
of loud" — is now payable, so DICT-2 pays it (R5).** Reasons: DICT-2 owns the
error channel the gap was deferred to; it removes the asymmetry the spec complains
about (`W w` errors, `repeated W xs` is silent); and it costs ~12 lines in the
walk R4 already needs, with **zero IR change** — so DICT-1's SF-1 pin ("flattened
repeated: INNER-declared dictionary is DROPPED") stays **green**. The IR behaviour
it pins is unchanged; only the plugin's verdict on that proto changes.

**R5's scope is narrow, and the earlier revision got this wrong.** Revision 1
claimed "there is no legal shape R5 can hit". **That is false without the
`field_count() == 1` term**, and the false positive would permanently outlaw a
working proto — the exact failure spec §7.1 warns against:

```proto
message W  { option (fletcher.flatten) = true;
             string k = 1 [(fletcher.dictionary) = {}];   // LEGAL and EMITTED
             int32  n = 2; }
message M  { repeated W xs = 1; }
```

Here `BuildFlattenedRepeated` returns `List<Struct(W)>` at `ir.cpp:366-372` (the
chain loop is never entered, because `msg->field_count() != 1`), `W` is **not**
`IsFlattenedWrapper` so the pass validates `W` on its own, and `W`'s fields are
ordinary struct children built with `BuildFieldIr` by `BuildStructVariant` — so
`W.k`'s scalar dictionary is legal *and honoured by emission* (spec §7.1's closing
rule: a scalar dictionary inside a struct child stays legal). R5 must therefore:

* carry `type->field_count() == 1` and keep the same term inside its chain loop
  (mirroring `ir.cpp:376`), so a multi-field wrapper is never touched;
* collect **inner** declarations only (the outer field's is R2's);
* be pinned by a **negative** forcing-test case for exactly the proto above
  → `nullopt`.

Restated soundly: R5 fires only on chains `BuildFlattenedRepeated` actually walks,
and every node those chains produce is `LIST`/`UNSUPPORTED`, so within that scope
every inner declaration is illegal under locked #9 and R5 cannot over-reject.

Nothing beyond R5, its negative test, and the spec edit is needed. Explicitly
**not** done: teaching `BuildFlattenedRepeated` to read the inner field (that
would change the IR, risk the GIR-10 nested-list shapes, and flip DICT-1's pin for
no user-visible gain).

### D6 — Obligation 4: the conflict/escalation policy

**Decision: a *disagreeing* pair is a hard error (R4); an *agreeing* pair stays
silent.** DICT-1's D4 snippet is not treated as a requirement, and **no warning
channel is invented.**

Rationale:

* The three options were silent / a real channel / a hard error. A real channel is
  not available cheaply and correctly: `facts.warning` and `FieldMapping.warning`
  are written in eleven places and **read nowhere** post-GIR-5,
  `type_mapper.hpp:42`'s doc is stale, and the only other protoc-plugin channels
  are `*error` (fatal) or raw stderr (which the plugin never uses and no harness
  asserts on). Building a warning channel is a round of its own, and a warning
  nobody reads is the defect class this round keeps tripping over.
* Silence is wrong on *disagreement* specifically: leaf-wins means the author's
  own field annotation is **discarded with no trace** (`Holder.on_both`: the field
  says INT64, the emitted column is INT8), and for `ordered` it means **locked #8
  goes unenforced** (D2's closure argument).
* A hard error on disagreement has **no false-positive class**: when the two
  decoded options are equal, precedence is unobservable and nothing is lost, so
  the error only ever fires on a genuine contradiction. Fix is obvious and local:
  delete one, or make them agree.
* Bonus property worth stating: it makes DICT-1's leaf-wins precedence
  *unobservable in accepted output*, so the round never has to relitigate whether
  leaf-wins or field-wins is right — and `ir.cpp`'s resolution stays untouched as
  the defined fallback (DICT-1's `on_both` unit assertion,
  `test_type_mapper.cpp:1046-1052`, stays green; the *proto* is now a codegen
  error, which is a pure superset).

### D7 — Obligation 3: delete the dead `ir::DictionaryModifier`

Verified unreferenced repo-wide on this tree: the only non-plan/non-archive hits
are the declaration itself (`ir.hpp:103-106`) and one comment
(`generator.cpp:1732`). No test, no Rust/TS, no CMake, no golden.

* Delete `enum class DictionaryModifier` from `ir.hpp`.
* Update `generator.cpp:1732`'s comment — "`ir::DictionaryModifier` is DEAD (its
  deletion is DICT-2's)" becomes stale the moment it is deleted; replace with a
  positive statement ("the live carrier is `ir::FieldFacts.dictionary`; there is
  no other dictionary carrier on the IR").
* Grep for `DictionaryModifier` in the final diff to prove zero code references
  remain (plan/review/archive markdown stays as history).

### D8 — Obligation 5: dictionary-ness stays IR-derived

**Stated loudly, as required.** DICT-2 introduces descriptor-based reads
(`HasFieldDictionary` / `ReadFieldDictionaryOption` in R1/R4/R5) **only inside the
rejection pass**. It introduces **no** descriptor-based source for the *mapping*
or for *emission*:

* the only writer of `FieldMapping.is_dictionary` is `ProjectIrToFieldMapping`,
  from `node.facts.dictionary` on the same node;
* nothing in DICT-2 makes an emitter read a descriptor for dictionary-ness;
* DICT-3 must still branch on `facts.dictionary` **AND
  `node.kind == ir::NodeKind::SCALAR`** (recorded in the plan under DICT-3's Scope;
  repeated in D11). **The kind gate is not belt-and-braces** (step-4 re-review, 4a
  item 2): DICT-2's pass does **not** establish "only `SCALAR` nodes carrying
  `facts.dictionary` reach emission". An **accepted** proto can present the fact on
  a **LIST** node through the surviving S2 route —
  `M {map<string, W> m = 1;}` with
  `W {flatten; repeated string vals = 1 [(fletcher.dictionary) = {INT8}];}`
  generates at exit 0, and `--fletcher_opt=accessor` proves the fact is live on
  that LIST (DICT-1.5's guard names `xf.ns.W.vals`). Branching on
  `facts.dictionary` alone would make the schema visitor emit
  `dictionary(<idx>, <list>)`. Harmless today (no emitter reads the fact); a
  malformed schema the day DICT-3 lands.

DICT-1.5's superset property (`plans/DICT-1.5-backend-support-guard.md` D1 /
Risk 2) is therefore intact, and **strengthened**: DICT-2 only *shrinks* the set
of shapes that can reach emission (R1–R5 are all fatal), so "guard-inspected ⊇
emittable" cannot be broken by this item. Gap 2 becomes a hard error **for the
shape it names** — a declaration inside a single-field flatten wrapper reached
through a `repeated` field — which removes that shape from the emittable set. Its
sibling (a declaration below a `repeated` hop *inside* a singular chain) stays
silent and stays safe by the identical construction argument; see Risk 5.

The reverse direction is also worth pinning: the descriptor reads in R1/R4/R5 sit
in the *category* spec §7.1 already licenses (rejection predicates may
over-approximate) — and with R5's `field_count() == 1` term none of them actually
over-approximates: each targets a shape illegal under locked #9 by construction.

### D9 — Error-text catalogue (all `"field '<fqn>': ..."`, ASCII only)

| Rule | Text (shape) |
|---|---|
| R1 | `(fletcher.dictionary) cannot be combined with (fletcher.flatten_field): the wrapper's fields are inlined as separate columns, so there is no single column to dictionary-encode; move the option onto the inlined field(s)` |
| R2 | `(fletcher.dictionary) requires a scalar column, but this field maps to <kind-word> (see docs/dictionary-option-spec.md section 4); remove the option` |
| R2 (`UNSUPPORTED` node) | `(fletcher.dictionary) is declared on a field that has no Arrow mapping at all; remove the option (the field's own unsupported-type error is reported first when generating)` |
| R3 | `(fletcher.dictionary) with ordered: true is not supported in v1 (the runtime re-fold produces a first-seen-order dictionary); remove ordered or set it to false` |
| R4 | `conflicting (fletcher.dictionary) declarations reached through (fletcher.flatten): index <a-index>, ordered <a-ordered> here vs index <b-index>, ordered <b-ordered> on '<other fqn>'; make them identical or remove one` |
| R5 | `(fletcher.dictionary) declared on '<inner fqn>' inside a repeated (fletcher.flatten) wrapper: the resulting column is a list, which cannot be dictionary-encoded; remove the option` |

All six must be pairwise distinct; each must name the offending field's
**fully-qualified** proto name (the existing passes' convention; the ctest harness
asserts `Message.field`); R4 must render **both** members of both options (so the
`ordered`-only conflict of D2 is legible). ASCII only — no em-dash/section-sign in
emitted strings.

### D10 — No-drift, cost, concurrency

* **Zero change to generated bytes** for every existing proto: no emitter reads
  the new `FieldMapping` fields, the IR is untouched, and the projection's
  existing outputs are byte-identical. Coverage goldens, the RBA no-drift golden,
  and the `.ipc` bytes are unaffected. The only behavioural delta is *new fatal
  errors* for protos that were previously accepted-and-mis-emitted.
* Cost: the pass is O(fields) per generated message with one `BuildFieldIr` per
  field (the same per-field cost the two existing passes already pay) plus a
  bounded chain walk. DICT-1's `ByteSizeLong() == 0` fast path means options-free
  fields pay nothing.
* Concurrency: none introduced. Everything is a pure function of descriptors;
  no statics, no caches. Same single-threaded `Generate()` guarantee DICT-1
  documented (`ReadFieldDictionaryOption` touches protobuf's benign cached-size).

### D11 — Scope guard + hand-off to DICT-3

**Untouched; any diff is a stop-and-ask:**
`protoc/src/recordbatch_accessor_emitter.*`, all Rust accessor emission (round
RIR's), `protoc/include/fletcher/options.proto` and the reader
(`option_reader.*` — DICT-1's, landed; DICT-2 only *calls* it),
`protoc/src/ir.cpp` (no IR behaviour change; `ir.hpp` only loses the dead enum),
`cpp_backend_schema_visitor.*` (DICT-3's), and every runtime component
(`arrow-bridge`, `pubsub-arrow` — locked #6). No `FieldKind` member added
(locked #5). DICT-1.5's `FindDictionaryField` / `ValidateBackendsSupportFields`
logic is unchanged (only the new call sits above it).

**For DICT-3, two findings from grounding this design:**

1. **Spec §5 / the story's "there are TWO schema emitters" is stale.** Post-GIR-5,
   `GenerateSchemaFunction` ignores its `fields` argument and delegates to
   `cpp_backend::GenerateSchemaFunctionFromIr`, and `BuildMessageSchemaInto`
   delegates to `cpp_backend::BuildMessageSchemaIntoFromIr` — one visitor, two
   sinks. Grep on this tree shows `EmitNanoarrowTypeSetup` (`generator.cpp:644`),
   `SetScalarSchemaType` (`:848`), `SetMetadataPairs` (`:887`),
   `RequireNestedMsg` (`:905`) and `ArrowTypeExpr` (`:260`) with **definition-only
   hits** — i.e. dead post-GIR-5 (confirmed by the step-2 reviewer). DICT-3 must
   branch inside the visitor and must **not** "fix" those functions; deleting them
   is nobody's task in this round.
2. DICT-3 adds the nanoarrow counterpart next to D1's helper
   (`DictionaryIndexNanoarrowType(ir::DictionaryIndexKind) -> ArrowType`) and
   reads **`facts.dictionary` gated on `kind == ir::NodeKind::SCALAR`**, per D8 —
   the kind gate is load-bearing, see D8 for the accepted proto that carries the
   fact on a LIST.

---

## Forcing-test mapping

### `TypeMapperTest.DictionaryMappingAndRejections` (protoc/tests/test_type_mapper.cpp)

Harness: `proto_text_pool.hpp` + `AddFletcherOptions` (route 1 — compiles the
shipped `options.proto`, so the test also re-asserts spec §2). It reuses the
existing `kDictSchema` messages (`Ev`, `Wkt`, `Shapes`, `Rec`, `Holder`,
`MemberHolder`) **only where they fit unmodified**, and adds a self-contained
`kDict2Schema` for everything else. `kDictSchema` is *not* edited (DICT-1's
sub-cases depend on it verbatim).

`kDict2Schema` (new file in the same pool, shapes named by the rule they pin):

```proto
syntax = "proto3";  package d2;
import "fletcher/options.proto";  import "google/protobuf/wrappers.proto";

enum Color { COLOR_UNSPECIFIED = 0; COLOR_RED = 1; }

message W1   { option (fletcher.flatten) = true; string value = 1; }                  // no inner decl
message W1i8 { option (fletcher.flatten) = true;
               string value = 1 [(fletcher.dictionary) = {index_type: DICTIONARY_INDEX_INT8}]; }
message W1i32{ option (fletcher.flatten) = true;
               string value = 1 [(fletcher.dictionary) = {index_type: DICTIONARY_INDEX_INT32}]; }
message W1i8t{ option (fletcher.flatten) = true;   // INT8 *and* ordered, for the R3 branch
               string value = 1 [(fletcher.dictionary) = {index_type: DICTIONARY_INDEX_INT8,
                                                          ordered: true}]; }
message W2   { option (fletcher.flatten) = true;                                      // TWO fields
               string k = 1 [(fletcher.dictionary) = {}]; int32 n = 2; }
message PBad { repeated string tags = 1 [(fletcher.dictionary) = {}]; string ok = 2; }

message Ctl {
  optional string opt_cat = 1 [(fletcher.dictionary) = {index_type: DICTIONARY_INDEX_INT8}];
  Color  color      = 2 [(fletcher.dictionary) = {}];
  W1     wrap_plain = 3 [(fletcher.dictionary) = {index_type: DICTIONARY_INDEX_INT16}]; // legal control
  W1i8   agree      = 4 [(fletcher.dictionary) = {index_type: DICTIONARY_INDEX_INT8}];   // equal -> legal
  W1i32  agree_dflt = 5 [(fletcher.dictionary) = {}];                                    // {} == INT32 -> legal
  W1i8   disagree   = 6 [(fletcher.dictionary) = {index_type: DICTIONARY_INDEX_INT64}];  // R4
  // R4 (locked #8 hole). index_type MUST be spelled INT8 here so `ordered` is the
  // ONLY difference from W1i8's leaf: with `= {ordered: true}` the outer decodes to
  // INT32, the indexes differ too, and an implementation comparing index_kind ALONE
  // would still reject it -- i.e. the assertion would be vacuous for the property
  // it exists to pin (step-2 cycle-2 item R1).
  W1i8   ord_wrap   = 7 [(fletcher.dictionary) = {index_type: DICTIONARY_INDEX_INT8,
                                                  ordered: true}];
  W1i8   ff_flat    = 8 [(fletcher.flatten_field) = true, (fletcher.dictionary) = {}];   // R1 (D4 row 1)
  google.protobuf.StringValue wkt_ff = 9
                          [(fletcher.flatten_field) = true, (fletcher.dictionary) = {}]; // R1 (D4 row 2)
  repeated W2 rep_multi = 10;                                                            // R5 NEGATIVE
  repeated string plain_tags   = 11;                                                     // no-option controls
  map<string, string> plain_lbl = 12;
  W1 plain_wrap = 13;
  // the OTHER branch of D2's locked-#8 closure argument: outer and leaf are EQUAL
  // and both set ordered, so R4 must stay silent and R3 must fire.
  W1i8t  ord_agree  = 14 [(fletcher.dictionary) = {index_type: DICTIONARY_INDEX_INT8,
                                                   ordered: true}];
}
message RecFf { PBad p = 1 [(fletcher.flatten_field) = true]; }                           // D3 descent
```

| Assertion | Design element |
|---|---|
| scalar `Ev.i16` → `kind==SCALAR`, `is_dictionary`, `dict_index_type_expr=="arrow::int16()"`, `scalar.arrow_type_expr=="arrow::utf8()"` (value type unchanged), `nullable==false` | D1 |
| `Ev.empty` / `Ev.unspec` → `"arrow::int32()"`; INT8/INT32/INT64 rows | D1 + `DictionaryIndexArrowTypeExpr` |
| `Ctl.opt_cat` → `is_dictionary && nullable` (nullability preserved), index `arrow::int8()` | D1 |
| `Ctl.color` (enum) → `is_dictionary`, value type `arrow::int32()` | D1 |
| **WKT** `Wkt.s` (`StringValue`, INT16, no `flatten_field`) → `SCALAR`, `nullable`, `is_dictionary`, index `arrow::int16()`, value `arrow::utf8()` | D1, locked #9 true positive |
| non-dictionary scalar → `!is_dictionary && dict_index_type_expr.empty()` | D1 (no false positive) |
| `Shapes.tags` (repeated) → reason mentions "list"; `Shapes.labels` (map) → "map"; `MemberHolder`-style struct field with the option → "struct"; `Holder.rep_nested` (nested list) → "list" | R2 |
| `Ev.ord`, `Ev.ord16` (`ordered: true` on a plain scalar) → reason mentions "ordered" | R3 |
| `Rec.p` (`flatten_field` + dictionary) → reason mentions `flatten_field`; **`Rec.s` (scalar + both) → nullopt (legal)** | R1 three-term predicate |
| `Ctl.ff_flat` (`flatten_field` over a `flatten` wrapper) → rejected — R2 alone would accept it (`SCALAR`) | R1, D4 row 1 |
| `Ctl.wkt_ff` (`StringValue` + `flatten_field` + dictionary) → rejected; contrast with `Wkt.s` above, which stays legal | R1, D4 row 2 (**B5**) |
| `Ctl.disagree` → reason mentions "conflicting" + both index types; `Ctl.ord_wrap` (**same index, `ordered` the only difference**) → reason mentions "conflicting" **and** `ordered`, and R3 provably cannot see it (assert `BuildFieldIr(ord_wrap).facts.dictionary_ordered == false` in the same block); `Ctl.agree` and `Ctl.agree_dflt` → **nullopt** | R4, D2's decoded-equality + locked-#8 closure |
| `Ctl.ord_agree` (outer and leaf both `{INT8, ordered: true}`) → reason mentions **"ordered"**, not "conflicting" — the R3 branch of the closure argument, so both branches are pinned and neither rule can be dropped in favour of the other | R3 + D2's closure |
| `Ctl.wrap_plain` (wrapper-declared, **no** `ordered`) → nullopt — the legal wrapper control. `Holder.on_wrapper` is **not** usable here: it carries `ordered: true` in `kDictSchema:823-824`, so it belongs to the R3 row | D6 / B3 |
| `Holder.rep_inner_declared` → rejected (gap 2 closed); same block re-asserts the IR still drops the fact | R5, D5 |
| **`Ctl.rep_multi` → nullopt**, and `FindIllegalDictionaryField(W2) == nullopt` — a multi-field `repeated` flatten wrapper whose inner scalar dictionary is legal and emitted | R5's `field_count()==1` term (**B1**) |
| `FindIllegalDictionaryField(RecFf)` returns the R2 error naming `d2.PBad.tags` — the descent into a `flatten_field` wrapper judges *inlined* fields | D3 |
| legal controls: plain scalar dict, `Holder.on_inner`, `Ctl.plain_tags` / `plain_lbl` / `plain_wrap` (repeated/map/wrapper **without** the option) → all nullopt | catches an implementation that rejects non-scalars regardless of `facts.dictionary` |
| all six reason strings pairwise distinct (collected into a `std::set`) | "each with a distinct reason" (story) |
| `FindIllegalDictionaryField(msg)` returns the **first** offender in declaration order | D3 determinism |

Also, in the **existing** `TypeMapperTest.ReadsDictionaryOption` D4b sub-case
(`test_type_mapper.cpp:1131-1158`), add
`EXPECT_TRUE(ir::BuildFieldIr(rec->FindFieldByName("p")).facts.dictionary);` plus
one comment line — this is the replacement for the coverage the ctest retarget
removes (D4, **B4a**), not a nicety.

Red-first: compile-red first (`is_dictionary` / `DictionaryUnsupportedReason` do
not exist), then behavioural red with the rule stubbed to `nullopt` and the
projection stubbed to `false`.

### Plugin-level (integration-tests/protoc-coverage), reusing `run_backend_guard_check.cmake` unchanged

`EXPECT_MESSAGE` values follow the convention documented at
`CMakeLists.txt:548-550`: **no literal parentheses** (they open ERE capture groups
under `MATCHES`), so match on the option name without parens or on a paren-free
phrase.

| Test | Fixture / opts | Asserts |
|---|---|---|
| `GenErrors.DictionaryOnFlattenFieldWrapperRejected` (retarget of `..._accessor_fieldFlatten`) | `coverage_dictionary_field_flatten.proto`, `FLETCHER_OPT=ipc`, `EXPECT_MESSAGE=cannot be combined with`, `EXPECT_FIELD=DictFfGuard.w` | non-zero exit with R1's message and no RBA backend requested — the rejection is backend-independent (D4) |
| `GenErrors.DictionaryOnFlattenFieldWrapperRejected_passOrder` (new) | same fixture, `FLETCHER_OPT=accessor`, **same** `EXPECT_MESSAGE=cannot be combined with` | DICT-2 wins over DICT-1.5. The substring is one DICT-1.5's message provably cannot contain (its text is "not yet supported by the RecordBatch accessor / Rust backend … regenerate without"), so the pin is hard, not soft (required item 6) |
| `GenErrors.DictionaryOnRepeatedRejected` (new fixture `coverage_dictionary_repeated.proto`) | `FLETCHER_OPT=ipc`, `EXPECT_MESSAGE=requires a scalar column` | R2 fires with no RBA backend requested |
| `GenErrors.DictionaryOrderedRejected` (new fixture `coverage_dictionary_ordered.proto`) | `FLETCHER_OPT=ipc`, `EXPECT_MESSAGE=ordered: true is not supported` | R3 / locked #8 is wired into `Generate()` |
| `GenErrors.DictionaryAcceptedWithoutRbaBackends` (existing, unchanged) | `coverage_dictionary.proto`, `ipc,ts`, `EXPECT_SUCCESS` | **no false positive**: a legal scalar dictionary still exits 0 with artifacts |
| `GenErrors.DictionaryGuardDoesNotMaskUnsupportedType` (existing, unchanged) | `coverage_dictionary_unsupported.proto` | #55 still reports the `Any` error first, now across three passes |
| `GenErrors.DictionaryRejectedBy_{accessor,rust,accessor_wkt,accessor_flatten,accessor_structChild,accessor_listChild,accessor_mapChild}` (existing, unchanged) | — | all seven fixtures are **legal** under DICT-2 (verified field by field), so DICT-1.5's messages still win — which makes them DICT-2 false-positive guards too |

Each new `add_test` gets its **own** `OUT_DIR` subdirectory (DICT-1.5 should-fix
S3: a shared `OUT_DIR` races under `ctest -j`).

---

## Risks / Unknowns

1. **Deviation from the story's wording, recorded (not silent):** the story says
   "`MapField`/`UnsupportedReason`: reject … → `nullopt`". This design keeps
   `MapField` total and makes rejection a fatal pass, because `nullopt` is a
   silent dropped column and would force a mirrored edit in
   `IsSchemaRepresentable` to avoid row/schema drift (D0). Locked #9's substance
   (accept iff mapped kind is `SCALAR`; otherwise a codegen error with a clear
   reason) is honoured; its phrase "with a clear `UnsupportedReason`" is read as
   "a clear reason string", **not** as "routed through the literal
   `UnsupportedReason()` function" — whose sole consumer (`generator.cpp:619`)
   writes a comment and continues at exit 0, i.e. exactly the outcome spec §4
   exists to prevent. **Ruling: ACCEPTED by step-2** as not a deviation; the plan
   correction (item 8) fixes the story's Scope bullet so the conformance reviewer
   does not re-open it.
2. **One existing test is deliberately retargeted** (`..._accessor_fieldFlatten`,
   D4), with its lost IR-route coverage replaced by a unit assertion (B4a) and its
   fixture docstring rewritten (B4b). The alternative — moving DICT-2's pass after
   `ValidateBackendsSupportFields` — would mean a plain `ipc` run reports the
   illegal declaration while an `accessor` run reports a backend gap for the same
   proto. Worse diagnostics; rejected on purpose.
3. **R4/R5 add descriptor-based dictionary reads.** Bounded to the rejection pass
   (D8). The failure mode to watch in review is someone later copying that
   pattern into an emitter — the code comments must say "rejection only", the same
   way `FindDictionaryField`'s comment does.
4. **Assumption: `NodeKind::SCALAR` ⟺ `FieldKind::SCALAR`** (verified at
   `type_mapper.cpp:154-162`). If a future kind projects onto `FieldKind::SCALAR`,
   R2's gate must follow it. Worth a mutation check in review (change R2 to
   `kind != LIST` and confirm the map/struct sub-cases red).
5. **Accepted gaps and secondary routes, disclosed:**
   * a `(fletcher.flatten)` wrapper that is declared but never used is not
     validated (mirrors emit — nothing is generated for it);
   * ~~an illegal declaration inside an **imported** message is reported when that
     message's own file is generated, not here.~~ **CLOSED by the step-4 re-review
     (S3):** the walk now descends into struct / list-element / map-value children,
     so an imported message's illegal declaration is reported from the importing
     file too, and the verdict no longer depends on which `.proto` protoc was
     pointed at. Retained below because the *original disclosure was factually
     wrong* and that correction is the history worth keeping.
     **CORRECTED (step-4 review S3, 2026-08-28): the parenthetical that used to
     read "mirrors `ValidateNoUnsupportedIr` / `OrderedMessages`" was FACTUALLY
     WRONG and has been removed.** Only the *outer loop* is shared. The two
     sibling passes' detection walks (`FindUnsupportedIr`, `FindDictionaryField`)
     recurse through STRUCT / LIST / MAP children, so they **do** report fields of
     imported messages; `FindIllegalDictionaryField` descends only through
     `(fletcher.flatten_field)` wrappers, so it does not. Reproduced: `outer.proto`
     importing an `inner.proto` whose `Inner.k` declares `{ordered: true}` and
     whose `Inner.tags` is a `repeated` dictionary exits **0** under
     `--fletcher_opt=ipc`, while the same pair under `--fletcher_opt=accessor`
     reports `field 'xf.inner.Inner.k': ...` from DICT-1.5's guard. See the
     step-4 addendum below for the precise boundary and why closing it is
     design-scale;
   * `FIXED_SIZE_LIST` is in R2's kind-word switch but unconstructible from any
     `.proto`, mirroring the disclosed gap the three IR walks already share;
   * a *used* `(fletcher.flatten)` wrapper whose inner field carries
     `flatten_field` + `dictionary` (`W {flatten; P p = 1 [(flatten_field),
     (dictionary)];}`) is rejected by **R2 on the using field** with the
     "struct column" wording rather than by R1, because `IsFlattenedWrapper(W)`
     skips `W` and D3 descends into `flatten_field` wrappers only from a judged
     message. Rejected either way; the wording is second-best, not wrong.
   * **Spec §7.1 gap 2 has a sibling that R5 does not close, and DICT-2 discloses
     rather than fixes it** (step-2 cycle-2 item R3). R5's predicate requires the
     *outer field* to be `repeated`; the `repeated` hop can instead sit **inside a
     singular chain** — `W w = 1;` where `W {flatten; repeated V vs = 1;}` and the
     declaration lives on `V`'s field. `BuildFieldIr(W.vs)` then routes to
     `BuildFlattenedRepeated`, which reads `BaseFacts(W.vs)` and no deeper field, so
     the declaration is dropped exactly as in gap 2; `W` is `IsFlattenedWrapper` so
     the pass never judges `W.vs` directly, and R5 cannot see it from `w`. **Safe by
     the same construction as gap 2**: schema emission consumes the identical node,
     so it drops the declaration too — the column is a plain list, DICT-1.5's guard
     and the accessor agree, and no mis-read exists. Cost is the same as gap 2's
     original cost: the rejection stays quiet. Closing it would need R5 generalised
     to walk singular chains for repeated hops, which is a scope increase with no
     safety gain; record it in spec §7.1 next to gap 2 instead.
6. **`FieldMapping.is_dictionary` / `dict_index_type_expr` have no production
   reader in DICT-2** (D1). **Ruling: keep both** — `dict_index_type_expr`'s
   `arrow::...()` spelling is precisely what spec §5.1 specifies for RIR's
   accessor type gate, so the fields have a *named* future consumer, unlike the
   `DictionaryModifier` this item deletes. Recorded as an explicit RIR
   carry-forward in the plan so it cannot orphan; the earlier "drop both fields"
   alternative is **not** taken.
7. No **STOP-AND-ASK**: nothing here needs a locked decision changed, and no
   public API (runtime, options surface, wire format) changes — the additions are
   additive plugin-internal C++.

---

## Files-to-touch

**Production**
* `protoc/include/type_mapper.hpp` — `FieldMapping::is_dictionary` +
  `dict_index_type_expr` (with D1's named-consumer comment); declare
  `DictionaryUnsupportedReason`, `FindIllegalDictionaryField`; refresh the stale
  `warning` doc comment (line 42) to say the channel is not rendered (D6).
* `protoc/src/type_mapper.cpp` — projection wiring (SCALAR branch), R1–R5, the
  message walk; `#include "option_reader.hpp"`.
* `protoc/include/cpp_backend_type_table.hpp` / `protoc/src/cpp_backend_type_table.cpp`
  — `DictionaryIndexArrowTypeExpr`.
* `protoc/src/generator.cpp` — `ValidateDictionaryDeclarations` + its call above
  `ValidateBackendsSupportFields`; refresh the `DictionaryModifier` comment
  (`:1732`).
* `protoc/include/ir.hpp` — delete `enum class DictionaryModifier` (lines 103-106).

**Tests**
* `protoc/tests/test_type_mapper.cpp` — new
  `TypeMapperTest.DictionaryMappingAndRejections` (+ `kDict2Schema`); **and** the
  B4a assertion + comment inside the existing `ReadsDictionaryOption` D4b
  sub-case (`:1131-1158`). `kDictSchema` itself is not edited.
* `integration-tests/protoc-coverage/CMakeLists.txt` — retarget the
  `fieldFlatten` test (new name, `ipc`, new `EXPECT_MESSAGE`), add the three new
  `add_test`s, each with its **own** `OUT_DIR`.
* `integration-tests/protoc-coverage/proto/coverage_dictionary_field_flatten.proto`
  — **rewrite the header comment** (lines 7-14) per B4b: the shape is now illegal
  (R1), rejected before any backend guard, and the IR route it used to pin is
  asserted in `TypeMapperTest.ReadsDictionaryOption`.
* `integration-tests/protoc-coverage/proto/coverage_dictionary_repeated.proto`,
  `coverage_dictionary_ordered.proto` — new fixtures (header comment naming the
  rule they isolate, following the existing fixtures' style). `git add` them —
  untracked fixtures were a DICT-1.5 process failure.

**Docs**
* `docs/dictionary-option-spec.md` — §7.1: gap 1 closed as a rejection (R1) and
  gap 2 closed as a rejection **for single-field wrappers reached through a
  `repeated` field only** (R5), with the IR-level drops unchanged and still pinned,
  plus (i) the multi-field-wrapper case named as legal and (ii) the still-open
  sibling of gap 2 recorded (a `repeated` hop inside a *singular* chain — Risk 5,
  silent and safe by construction); §4: enforcement is a front-end pass, not
  projection `nullopt`,
  and `flatten_field` + `dictionary` is rejected (including on a WKT wrapper);
  §5: mark the stale "two schema emitters" paragraph as superseded by the single
  post-GIR-5 visitor (D11 finding 1).
* `plans/DICT-dictionary-option.md` — DICT-2 row → 🟢 when green; correct the dead
  `type_mapper.cpp:632-669` / `MapWellKnown` / `MapStructField` citations **and**
  the "`MapField`/`UnsupportedReason`: reject … → `nullopt`" Scope bullet (item 8);
  add the RIR carry-forward from D1.
* `docs/fletcher-options.md` — one line under `(fletcher.dictionary)`: the option
  is rejected on non-scalar fields, with `ordered: true`, on a `flatten_field`
  wrapper (WKT wrappers included), when two declarations on one flatten chain
  disagree, and when declared inside a single-field `repeated` flatten wrapper.

---

---

## Step-4 review addendum (2026-08-28) — the pass's detection boundary

Both step-4 reviews returned **0 blocking** across two cycles. Final state: **S1,
S3 and S4 are FIXED in code/tests**; **S2 remains open by design** and is
disclosed here and in spec §7.1.1. The resolution table at the end lists every
item, including the deliberate skips.

> **Revision 2 (re-review cycle).** Cycle 1 of this addendum declined S3 on a
> false-positive argument that turned out to cover only *half* the shape. The
> declined rationale said `flatten_field` "is a no-op inside a flatten wrapper
> **or a struct child**". **The struct-child half was wrong** (4b's P1-B) and has
> been corrected below; S3 is now **closed**. The flatten-wrapper half is right,
> was independently proved, and is now pinned by a ctest.

### The rule that makes the boundary decidable

`(fletcher.flatten_field)` inlining exists in **exactly two places** —
`GatherFieldsImpl` (`generator.cpp`) and `BuildFlattenedFieldListImpl`
(`cpp_backend_schema_visitor.cpp`) — and both build a **generated message's own
top-level column list** (both recurse, so nested `flatten_field` chains are
inlined too). Whether R1 is sound at a given position is therefore exactly the
question *"does some generated schema function inline this message's fields?"*:

| Position | Inlined by a generated schema function? | R1 sound there? |
|---|---|---|
| a generated message's own field | yes, by its own `<Cls>Schema()` | **yes** |
| a field of a **struct / list-element / map-value child** | yes — the child's schema is `ArrowSchemaDeepCopy(<Child>Schema())`, and `<Child>Schema()` is built by the child's **own** inlining walk | **yes** |
| a field of an `IsFlattenedWrapper` message | **no** — no schema function is generated for a wrapper, so nothing inlines its fields; `flatten_field` there is a genuine no-op and the declaration is **resolved and honoured** | **NO** |

Corroborating structural fact (4b): the only readers of `ir::StructNode.fields`
repo-wide are the three validation walks (`FindUnsupportedIr`,
`FindScalarLeafNestedList`, `FindDictionaryField`). **No emitter reads them.** So
the "IR view" in which a struct child keeps a `flatten_field` field as a scalar
carrying the dictionary never reaches an artifact — inside a struct child the
declaration is dropped exactly as at top level.

Verified on generated output for
`FfLeaf {flatten; string s = 1;}` / `M {FfLeaf p = 1 [(flatten_field)]; int32 q = 2;}`
/ `Top {M m = 1;}`: `MSchema()` emits children `s`, `q` (so `flatten_field` **is**
inlined inside `M`), and `TopSchema()` is `ArrowSchemaDeepCopy(MSchema())`.

### S3 — CLOSED for a directly-named child; one wrapper-hop residual (P1-C)

`FindIllegalDictionaryFieldImpl` now also descends into a **singular-message
child**, a **list element** and a **map value**, never into an
`IsFlattenedWrapper` and never into an `IsRecursive` message.

**Stated accurately (corrected — the earlier "exactly the three call sites"
wording over-claimed, 4b's P1-C/nit-A).** `cpp_backend_schema_visitor` calls
`DeepCopyMessageStruct` from **two** call sites (`case NodeKind::STRUCT` at
`cpp_backend_schema_visitor.cpp:449` and `case NodeKind::MAP` at `:468`; the
`LIST` case *recurses* into `EmitNodeType` rather than calling), reaching **four**
emission positions: (1) a singular nested struct field, (2) the element of
`List<Struct>`, (3) the struct **leaf** of `List<List<...<Struct>>>`, (4) a map
value.

The walk judges a position when the child message is named **directly by a field
of a judged message** — (1), (2), (4). It does **not** judge a position whose
child is reached **through a `(fletcher.flatten)` wrapper hop**, because
`!IsFlattenedWrapper(child)` cuts the walk at the wrapper. That excludes (3)
outright (a nested list is only constructible through a wrapper hop, via
`BuildFlattenedRepeated`) and excludes (1)/(2)/(4) whenever a wrapper sits between.

**So the file-choice inconsistency survives one wrapper hop** — reproduced both
ways:

```
protoc leaf_inner.proto   -> rc=1  field 'xf.li.Inner.k': ... ordered: true is not supported
protoc leaf_nest.proto    -> rc=0  Top{repeated NestWrap} / NestWrap{flatten; repeated xf.li.Inner ms}
                                   ...yet emits ArrowSchemaDeepCopy(InnerSchema(),
                                      schema->children[0]->children[0]->children[0])
protoc leaf_sing.proto    -> rc=0  Top{W w} / W{flatten; xf.li.Inner m}
                                   ...yet emits ArrowSchemaDeepCopy(InnerSchema(), schema->children[0])
```

**This is not an argument against the extension, and it is explicitly NOT fixable
by descending into the wrapper** — that reintroduces the R1 false positive the new
ctest guards. The correct shape is to follow the wrapper **chain to its leaf
message** and judge the **leaf**, never the wrapper's own fields; that is strictly
smaller than gating R1 and could land first. Carried with the S2 follow-up and
disclosed in spec §7.1.1's table.

What the closure does fix: the same declaration used to be fatal or silently
accepted depending only on which `.proto` protoc was pointed at.

| probe | before | after |
|---|---|---|
| `protoc inner2.proto` (`M.p` = `flatten_field` + dictionary) | rc=1 | rc=1 |
| `protoc outer2.proto` (merely *imports* `inner2`) | **rc=0 silent** | **rc=1**, names `xf.i2.M.p` |
| `protoc outer.proto` (imports `Inner.k` `ordered: true`) | rc=0 silent | **rc=1**, names `xf.inner.Inner.k` |
| `protoc maptop.proto` (map value of imported `Inner`) | rc=0 silent | **rc=1** |
| `protoc replist.proto` (`repeated` imported `Inner`) | rc=0 silent | **rc=1** |
| `protoc leaf_nest.proto` / `leaf_sing.proto` (imported `Inner` behind a **wrapper hop**) | rc=0 silent | rc=0 silent — **P1-C residual, disclosed** |
| `ffw.proto` (live int16 dict inside a flatten wrapper) | rc=0 | **rc=0** — no false positive |
| `mapw2.proto` / `nonscalar.proto` (S2) | rc=0 | rc=0 — open by design |
| coverage corpus (18 protos, `ipc`) | 7 intended rejections | **the same 7** |
| unit suite | 93 pass | **93 pass** |

**Deviation disclosed:** the **list-element** position is an addition beyond the
12-line closure the re-review measured (which covered singular + map value).
Rationale: it is the same `DeepCopyMessageStruct` code path and the same argument,
and without it `repeated <imported Inner>` stayed silent while
`<imported Inner>` was rejected — the identical file-choice inconsistency, one
field shape over. Probed (`replist.proto`, above) and corpus-swept; no false
positive. One line to revert if unwanted.

The `visited` set now does real work: it terminates cycles **and** collapses a
diamond (a child reached from two fields of one root is judged once). Determinism
is unaffected — a child's fields are judged identically whichever path reaches
them, and every message names the offending declaration's own FQN. Note that
`ValidateDictionaryDeclarations`'s `IsRecursive(msg)` term stops being dead with
this change (a child reached by descent is not filtered by `OrderedMessages`), so
4b's nit-8 "keep the mirror" call was right for a second reason.

### S2 — OPEN BY DESIGN, and now pinned

One of **three** surviving holes (S2, gap 2's sibling, and P1-C's wrapper-hop
residual above), all three downstream of the same wrapper exclusion. S2 proper: a
`(fletcher.flatten)` **wrapper** reached through a **non-flattening** context — a `map` value or a struct/list child. A `map` value
does not flatten (`BuildMapNode` → `MakeStructNode(val_msg)`, `ir.cpp:516`), and
the descent above deliberately refuses to enter a wrapper, so nothing judges the
wrapper's fields:

```proto
message W { option (fletcher.flatten) = true;
            string v = 1 [(fletcher.dictionary) = {ordered: true}]; }
message M { map<string, W> m = 1; }        // --fletcher_opt=ipc -> rc=0, EMITTED
```

Closing it requires **R1 to become context-dependent** (`is_top_level`-gated), so
that the *other* rules can be applied inside a wrapper without R1 firing on a
declaration that is genuinely honoured there. That is a design decision with its
own false-positive sweep, not a patch — 4a and 4b both agree the wrapper exclusion
must stay until then. Also still open, unchanged and previously approved: gap 2's
sibling (a `repeated` hop inside a *singular* flatten chain). Both silently drop
`ordered: true` as well as an index type, so **locked #8 is not absolutely
enforced**; see the SCOPED note on D2.

**The guard is now a real pin (was not).** Both reviewers found independently that
the cycle-1 unit assertion was **inert**: `ValidateDictionaryDeclarations` is
file-local, no unit test calls it, and dropping the wrapper skip left the whole
suite green. Fixed with a plugin-level fixture + ctest:

* `integration-tests/protoc-coverage/proto/coverage_dictionary_wrapper_live.proto`
  carries the live-dictionary-inside-a-flatten-wrapper shape;
* ctest `GenErrors.DictionaryLiveInsideFlattenWrapperAccepted`
  (`FLETCHER_OPT=ipc`, `EXPECT_SUCCESS`, three `EXPECT_ARTIFACTS`).

Mutation-verified both ways: dropping the **top-level** `IsFlattenedWrapper(msg)`
skip → ctest **RED** (`rc=1` on `...DictWrapLiveWrap.p`) while the unit suite stays
**93/93 green** (exactly the reviewers' finding); dropping the descent's
`!IsFlattenedWrapper(child)` term → ctest **RED** *and* the unit suite red (via
`HostMapWrapper`). So the earlier claim "the hazard cannot be re-introduced by
accident" is now **true, by that ctest** — the unit block pins only the *premise*
(`Chains.ff_inside_wrapper` maps to a live `int16` dictionary) and the *asymmetry*
(`IllegalIn(d2, "FfW")` does reject).

### Carry-forward to DICT-3 — the kind gate is now part of the contract

**DICT-3 must branch on `facts.dictionary` AND `node.kind == ir::NodeKind::SCALAR`**,
not on `facts.dictionary` alone. This is recorded in D8 and in the story plan's
DICT-3 Scope bullet. Reason: an **accepted** proto can still present a
**non-`SCALAR`** node carrying the fact, via the surviving S2 route. Verified after
the closure:

```proto
message W { option (fletcher.flatten) = true;
            repeated string vals = 1 [(fletcher.dictionary) = {index_type: DICTIONARY_INDEX_INT8}]; }
message M { map<string, W> m = 1; }
```
`--fletcher_opt=ipc` → **rc=0** (accepted); `--fletcher_opt=accessor` → DICT-1.5's
guard names `xf.ns.W.vals`, proving the **LIST** node carries `facts.dictionary`
and reaches emission. Without the kind gate the schema visitor would emit
`dictionary(<idx>, <list>)`. Harmless today (no emitter reads the fact); a
malformed schema the day DICT-3 lands.

### Resolution table

| Item | Resolution |
|---|---|
| **S1** R1's remedy unfollowable for the WKT shape | **Fixed.** R1 now reads "… ; remove (fletcher.flatten_field), or move the option onto the inlined field or fields". `cannot be combined with` (the ctest substring) unchanged; confirmed followable by 4b. |
| **S2** flatten wrapper reached through a non-flattening context | **Open by design**, disclosed here + spec §7.1.1. Closing needs R1 `is_top_level`-gated. Now pinned by ctest `GenErrors.DictionaryLiveInsideFlattenWrapperAccepted` (mutation-verified). |
| **S3** imported-message hole + factually wrong disclosure | **CLOSED for a directly-named child** (struct / list-element / map-value descent, wrapper- and recursion-excluded). The cycle-1 "or a struct child" rationale is **corrected** above with the `MSchema()` + `DeepCopyMessageStruct` evidence; Risk 5's original false "mirrors `ValidateNoUnsupportedIr`" claim stays retired. **Residual: P1-C**, below. |
| **4b P1-C** the "exactly the deep-copy sites" invariant over-claims; a struct leaf behind a wrapper hop is deep-copied but excluded | **Fixed as a disclosure** (docs only). The invariant is restated in terms of the **four** emission positions and which one is not judged, with both reproductions; added to spec §7.1.1's table; the correct fix shape (follow the wrapper **chain to its leaf**, never descend into the wrapper) is recorded and carried with S2. Same correction applied to the `type_mapper.{hpp,cpp}` and test comments. |
| **4b nit-A** call-site count wrong (two call sites, four positions) | **Fixed** everywhere the claim appeared: `type_mapper.cpp`, `type_mapper.hpp`, `test_type_mapper.cpp`, spec §7.1.1, this addendum, the story plan. The `LIST` case recurses rather than calling, which is why positions — not call sites — are what the argument rests on. |
| **S4** R4's two `is_repeated` terms unpinned | **Fixed.** `Chains.r4_repeated_outer` (must report R5, not "conflicting") and `Chains.r4_repeated_inner` (must report nothing) red on exactly one deletion each. |
| **4a item 1 / 4b P1-A** the guard was not the pin it claimed | **Fixed** with the plugin-level fixture + ctest above, not a reword. |
| **4b P1-B** "or a struct child" is factually wrong | **Fixed** wherever it appeared (this addendum, spec §7.1.1, story plan). |
| **4a item 2** DICT-3 needs a kind gate | **Fixed.** D8 and the story plan's DICT-3 bullet now say `facts.dictionary` **AND** `kind == SCALAR`, with the live S2-route example above. |
| **4a N1 / 4b P2-4** "compile error" over-claim | **Fixed.** Both comments say `-Wswitch` warning, name the fallback, point at `-Werror=switch`. |
| **4a N2** locked-#8 closure over-claims | **Fixed.** D2 carries a dated SCOPED note; spec §7.1 names `ordered: true` among the silently dropped declarations. |
| **4a N4** undisclosed `is_map()` term in R5 | **WITHDRAWN by 4a.** No disclosure needed. |
| **4b P2-2 / P2-3 / P2-5, nits 1–4** | **Fixed** (comment placement; `MsgOrFail`/`IllegalIn` null guards; user-doc qualifier — now updated again since imported messages *are* judged; `ord_wrap` asserts both `ordered` renderings; B4a claim softened; `ir.cpp:545`→`:544`; `wrapper` reused). |
| **4b list-element extension** (my deviation beyond the measured 12 lines) | **CONFIRMED SAFE by 4b and kept:** it verified `ArrowSchemaDeepCopy(MSchema(), ...)` at all four child positions, decode reading `MSchema()->n_children` at the same positions, no emitter reading `ir::StructNode.fields`, and the Rust/RBA emitter reaching those positions via the child's own `GatherFields`; plus a battery with live int8/WKT dictionaries inside the child at every position — **no legal shape newly rejected**. |
| **4b nit 10** R2 names the outer using field, not the declaring one | **CARRIED as a diagnostics follow-up**, next to S2 (both are the same "which field do we name" question), per 4b's request. Recorded with the trap: `node.facts.proto_full_name` looks like the answer but is the *resolved chain's* field, which is **not** the declaring field when the option was propagated from the outer field by `ApplyDictionaryFacts` — so a naive fix would name a field that carries no declaration. Needs the declaring field threaded through, or the reason string to name both. |
| **4b P2-1** gate the third `BuildFieldIr` on a cheap disjunction | **RE-CHECKED after the closure, still SKIPPED.** The descent does raise the cost (a child is now walked once per referring root; `visited` bounds it to once per root, and the three sibling passes already pay a per-field `BuildFieldIr`). But the objection that decided it is unchanged and 4b agreed with it: the guard is exact only while `ApplyDictionaryFacts` remains the sole propagation route, so a future `ir.cpp` change silently turns it into a **false negative**. Cost work should land with a measurement and a `static_assert`-style tie to the propagation route, not as a drive-by here. |
| **4b nit 5** rename to `DictionaryIllegalReason` | **SKIPPED.** Named design contract (D2 + the header). |
| **4b nits 6, 7, 9, 11** | **SKIPPED** (wording preference / low value / scope), all accepted by 4b. Nit 8's `IsRecursive` term is kept and is no longer dead (see S3 above). |

### Process gate (4a's P1)

Review mutations were run in the live worktree, so `// MUTATION`/`// MUT-` markers
were transiently visible to a concurrent reviewer. Confirmed clean before hand-back:
`git grep -n "MUT" -- protoc/` returns nothing, the index is empty, and every
mutation was reverted from a pristine copy. Adopting 4a's suggestion for future
rounds: run mutations in a `git checkout-index --prefix=` export.

## Step-2 rework (2026-08-28) — how each item was addressed

| Item | Where |
|---|---|
| **B1** R5 over-rejects (missing `field_count()==1`) | D2 R5 row + R5 loop sketch; **D5 rewritten** with the counter-example and the corrected soundness argument; forcing-test row `Ctl.rep_multi` → nullopt; spec §7.1 edit narrowed to single-field wrappers |
| **B2** R4 equality must include `ordered`, on the decoded option | D2's "R4's equality is on the decoded `DictionaryOption`, both members" bullet (with the locked-#8 closure argument); D9's R4 text renders both members; forcing-test row `Ctl.ord_wrap` (+ asserts `dictionary_ordered == false` so R3's blindness is pinned) |
| **B3** `Holder.on_wrapper` is not a legal control | forcing-test table: legal wrapper control is now `Ctl.wrap_plain` (`kDict2Schema`), and `on_wrapper` is explicitly assigned to the R3 row; `kDictSchema` untouched |
| **B4** retarget loses the `BaseFacts(w)` IR-route check | D4's third bullet: (a) mandatory `EXPECT_TRUE(ir::BuildFieldIr(...).facts.dictionary)` in the D4b sub-case, (b) fixture header rewrite; both in Files-to-touch; claim restated as "dead information rather than unguarded safety" |
| **B5** R1 also fires on WKT + `flatten_field` | D4's two-row table (with the "do not exclude WKTs" warning); forcing-test row `Ctl.wkt_ff` contrasted with `Wkt.s`; docs bullet |
| **6** `EXPECT_MESSAGE` convention + hard `_passOrder` substring | plugin-level table preamble and rows (`cannot be combined with`) |
| **7** ruling (b) folded in | D1 comment (named RIR consumer + cannot reach RBA), D1 closing paragraph, D1 carry-forward line, Risk 6 |
| **8** story Scope bullet correction | D0 closing paragraph + Files-to-touch plan bullet |
| Nit: root-cause-first not absolute | D2 preamble, scoped to "one declaration" with the R2→R5 example |
| Nit: flatten wrapper whose inner carries `flatten_field` | Risk 5, fourth bullet |
| Nit: `UNSUPPORTED` wording | dedicated R2 clause in D9 + D2's kind-word bullet |
| Verified-claim bonus (fixtures double as false-positive guards) | D4 final bullet + plugin-level table last row |

---

## Step-2 re-review, cycle 2 (2026-08-28)

**Verdict: APPROVE.** All five blocking and all three required items from cycle 1
are **discharged in substance, not acknowledged** — each was re-derived against the
tree rather than read off the rework table. Three residual items were found while
verifying B1/B2; all three are localized, fully specified, and **already applied
inline to this document** (marked "step-2 cycle-2 item R1/R2/R3"). They are
mandatory at implementation, not optional.

### Discharge verified

* **B1 — discharged, and the false-positive fix does NOT open a false negative.**
  Re-derived both directions. *Legal shape now accepted:* for `Ctl.rep_multi`
  (`repeated W2`, `W2` = flatten + 2 fields) `BuildRepeatedMessage` →
  `BuildFlattenedRepeated` → `field_count() != 1` → `List<Struct(W2))` with
  `BaseFacts(rep_multi)` (no option) at `ir.cpp:366-372`, so R2/R3 see nothing, R4's
  loop never runs, R5's new `field_count()==1` term excludes it, and `W2.k`'s scalar
  dictionary is judged legal when the pass validates `W2` on its own — and it *is*
  honoured by emission (`BuildStructVariant` builds each child with `BuildFieldIr`).
  *Illegal shape still rejected:* `Holder.rep_inner_declared` (`repeated WrapInner`,
  `field_count()==1`) still collects the inner declaration → R5 fires. The narrowing
  is exactly to the set `BuildFlattenedRepeated`'s chain loop walks, and outside that
  set nothing is dropped — so there is no shape that lost its error.
* **B2 — the closure argument is genuinely exhaustive.** Checked over an arbitrary
  chain with declarations D0 (outer) … Dn: `ir.cpp:332-336` makes the *deepest*
  declaration the sole winner, so the resolved node's `ordered` is the winner's. If
  all decoded options are equal, the winner's `ordered` equals every declaration's →
  any `ordered: true` surfaces on the resolved node → R3. If any two differ → R4.
  The two branches are exhaustive and mutually exclusive, and the single-declaration
  case falls in the first branch (that is `Holder.on_wrapper` → R3). Cycle cut-offs
  are unreachable (`IsRecursive` skip). Locked #8 is therefore enforced on
  wrapper-declared `ordered`. *But the test that pins it was vacuous — see R1 below.*
* **B3 — discharged.** `Ctl.wrap_plain` re-derived legal: no `flatten_field` (R1 no),
  resolves through `BuildFlattenedSingular` to SCALAR + INT16 (R2 no), `ordered`
  false (R3 no), one collected declaration (R4 no), not repeated (R5 no) → `nullopt`.
  `Holder.on_wrapper` correctly reassigned to the R3 row. `kDictSchema` is untouched,
  so every DICT-1 sub-case keeps its verbatim fixture.
* **B4 — the assertion does restore the removed coverage.** `Rec.p` is `Pair p`
  (2 fields, **no** message-level flatten) behind a `flatten_field` field, so
  `BuildFieldIr(Rec.p)` → `BuildSingularMessage` → `BaseFacts(p)` at **`ir.cpp:545`**
  — the identical code path, on the identical shape, that
  `coverage_dictionary_field_flatten.proto`'s `DictFfGuard.w` exercised. So
  `EXPECT_TRUE(ir::BuildFieldIr(rec->FindFieldByName("p")).facts.dictionary)` is a
  faithful unit-level replacement for the retargeted ctest's IR route, and D4's
  "dead information rather than unguarded safety" phrasing is now accurate.
  B4b's fixture-header rewrite is specified with its replacement content.
* **B5 — discharged.** The two-row "R2 accepts / emission drops" table matches the
  tree (`ir.cpp:601` `TryBuildWkt` vs the walks' three-term predicate at
  `generator.cpp:606-607` / `cpp_backend_schema_visitor.cpp:78-79`), the
  "do not exclude WKTs" warning is explicit, and `Ctl.wkt_ff` is contrasted with the
  still-legal `Wkt.s`.
* **Required 6/7/8 — discharged.** `cannot be combined with` is paren-free, is
  present in R1's D9 text, and is absent from DICT-1.5's message ("not yet supported
  by the RecordBatch accessor / Rust backend … regenerate without"), so `_passOrder`
  is a hard pin. `requires a scalar column` and `ordered: true is not supported` are
  likewise paren-free and match D9. Ruling (b) is folded into D1's comment, D1's
  closing list, the carry-forward line and Risk 6. The story-Scope correction is in
  D0 and in Files-to-touch.

### Residual items — applied inline, mandatory at implementation

1. **R1 — `Ctl.ord_wrap` was vacuous for the property it pins.** As written
   (`= {ordered: true}`) the outer option decodes to **INT32** while `W1i8`'s leaf is
   **INT8**, so the two options differ in *both* members: an implementation
   comparing `index_kind` **alone** would still reject it, and the assertion that is
   supposed to prove `ordered` participates in R4's equality would pass anyway.
   Fixed inline: the outer option is now `{index_type: DICTIONARY_INDEX_INT8,
   ordered: true}`, making `ordered` the sole difference and the check
   mutation-sensitive. Also added `Ctl.ord_agree` over the new `W1i8t` (outer and
   leaf both `{INT8, ordered: true}`) → must report **R3**, not R4, so *both*
   branches of B2's closure argument are pinned and neither rule can later be
   dropped in favour of the other.
2. **R2 — R4's loop sketch was missing two terms its own normative row requires.**
   The R4 row says "**singular**, single-field … chain", but the sketch neither
   excluded a repeated outer field nor stopped at a repeated hop. Consequences,
   both real: (i) `repeated W xs` over a single-field wrapper with two disagreeing
   *inner* declarations would report R4 instead of R5, stealing the better message;
   (ii) for `W w` → `W {flatten; repeated V vs}` → deeper disagreeing declarations,
   R4 would report a "conflict" between two declarations **neither of which
   `ir.cpp` reads**, with advice ("make them identical") that leads to silence
   rather than a fix. Fixed inline: `if (field->is_repeated()) return nullopt;` and
   `if (inner->is_repeated()) break;` *after* collecting that inner's own option
   (`BaseFacts(inner)` **is** read by `BuildFlattenedRepeated`; nothing below it is).
   R4 now mirrors `BuildFlattenedSingular`'s actual read set exactly.
3. **R3 — spec §7.1 gap 2 has a sibling R5 does not close; disclose it.** R5 keys on
   the *outer field* being `repeated`, so a declaration below a `repeated` hop inside
   a **singular** chain is still dropped and still silent. Verified safe by gap 2's
   own construction argument (emission consumes the identical node and drops it too,
   so the column is a plain list and no mis-read exists). Added as a Risk 5 bullet,
   D8's "removes it from the emittable set entirely" narrowed to the shape gap 2
   actually names, and the spec §7.1 edit in Files-to-touch extended to record it.
   **Do not** generalise R5 in DICT-2 — scope increase, no safety gain.

### Nit (no doc change needed)

* `kDict2Schema` imports `google/protobuf/wrappers.proto` for `Ctl.wkt_ff`, so the
  new test must call `pool.AddLinked(google::protobuf::StringValue::GetDescriptor()->file())`
  the way `ReadsDictionaryOption` does. Omitting it fails loudly at pool-add time,
  so this is a convenience note, not a risk.

Nothing else changed between revisions that weakens a cycle-1 verification: the
verified items (D0, D4/R1's mechanism, `NodeKind::SCALAR` ⟺ `FieldKind::SCALAR`,
D6's dead warning channel, D7, the SF-1/`on_both` pins, D11 finding 1, and
"all seven DICT-1.5 fixtures stay legal") are all still stated correctly.

---

## Step-2 review (2026-08-28)

**Verdict: NEEDS-REWORK** — 5 blocking + 3 required items. No locked-decision
deviation and no STOP-AND-ASK: locked #5/#8/#9/#11 are all honoured in substance
(see the rulings below). The core technical spine of this design is *correct and
was verified against the tree*, including its sharpest claim (D4's "R2 alone is
insufficient"). The blocking items are two real over/under-rejection holes in
R4/R5, one factual error in the forcing-test mapping, one genuine gap in the
"no DICT-1.5 coverage is lost" argument, and one under-documented R1 sub-case
that invites a wrong "fix".

### Verified against the tree (not the prose)

* **D4 / R1 is load-bearing — claim confirmed.** For
  `W w [(flatten_field),(dictionary)]` where `W` also carries message-level
  `(fletcher.flatten)` with one field: `BuildFieldIr(w)` → `BuildSingularMessage`
  (`ir.cpp:538`) → `BuildFlattenedSingular` (`:309-338`) → the inner node with the
  wrapper's dictionary propagated at `:332-336` → **`SCALAR`**, which R2 accepts;
  meanwhile both inlining walks (`generator.cpp:606-607`,
  `cpp_backend_schema_visitor.cpp:78-79`) `continue` past `w` and build from the
  inner field, which carries nothing → a value-typed column. R1 is not redundant.
* **D0 — both halves confirmed.** `nullopt` becomes a `skipped_comment` and
  generation continues at exit 0 (`generator.cpp:615-620`); the schema walk never
  calls the projection and instead uses `IsSchemaRepresentable`
  (`cpp_backend_schema_visitor.cpp:31-61`), which its own comment declares a
  by-hand mirror. A projection-level rejection really would drift row vs schema.
* **Risk 4's assumption confirmed.** `NodeKind::SCALAR` is the only producer of
  `FieldKind::SCALAR` and always produces it (`type_mapper.cpp:154-162`), so R2's
  IR-kind gate *is* locked #9's mapped-`FieldKind` gate.
* **D6 confirmed.** Nothing reads `FieldMapping.warning` or `facts.warning`
  anywhere (writers only: `type_mapper.cpp:159/169/184/194/217/248`,
  `ir.cpp:316/370/461/527/549`). No warning channel exists; inventing one is
  correctly refused, matching DICT-1's carry-forward. `Holder.on_both`'s unit
  assertion (`test_type_mapper.cpp:1046-1052`) is IR-level and stays green.
* **D7 confirmed.** `DictionaryModifier` has exactly two non-plan hits:
  `ir.hpp:103-106` and the `generator.cpp:1732` comment.
* **Obligation 2 / R5 confirmed.** The SF-1 pin (`test_type_mapper.cpp:1112-1129`)
  asserts IR behaviour only and stays green with zero `ir.cpp` change. All seven
  `BuildFlattenedRepeated` return sites (`ir.cpp:367,387,397,414,415,430,434`) are
  LIST/UNSUPPORTED.
* **D8 confirmed.** The mapping's only dictionary writer is the projection from
  `node.facts.dictionary`; no second source is created; DICT-1.5's superset
  property can only shrink.
* **Pass ordering is sound and is pinned.** #55 still runs first
  (`generator.cpp:1851`); `ParseMetadataRules` order unchanged; the DICT-2-first
  advice is strictly more actionable (removing the illegal option ends the
  dialogue, whereas DICT-1.5's remedy yields a second error). `_passOrder` pins it.
* **"All seven existing DICT-1.5 fixtures stay legal under DICT-2" — verified
  field by field** across all nine `coverage_dictionary*.proto`. Bonus worth
  stating in the doc: `_wkt`/`_flatten`/`_structChild`/`_listChild`/`_mapChild`
  double as **DICT-2 false-positive guards**, because their `EXPECT_MESSAGE` is
  DICT-1.5's text — if DICT-2 wrongly rejected them they go red.
* **D11 finding 1 confirmed** (`EmitNanoarrowTypeSetup`, `SetScalarSchemaType`,
  `ArrowTypeExpr` are definition-only). `FieldMapping` is never positionally
  aggregate-initialised repo-wide, so the two additive members are safe.

### Rulings on the two flagged interpretations

**(a) locked #9's "clear `UnsupportedReason`" = a reason *string*. ACCEPTED —
not a deviation, not a STOP-AND-ASK.** `UnsupportedReason()`
(`type_mapper.cpp:275-297`) has exactly one consumer, `generator.cpp:619`, where
its text becomes a `skipped_comment` and generation continues at **exit 0**.
Routing locked #9's rejection through it would produce the dropped-column outcome
spec §4 exists to prevent, so the literal reading contradicts the decision's own
operative words ("a codegen error with a clear ... reason"). **PM note (not a
blocker):** add a dated parenthetical to locked #9 — as was done for #5 and #10 —
so this is not re-litigated by the conformance reviewer.

**(b) `FieldMapping.is_dictionary` / `dict_index_type_expr` with no production
reader after DICT-2. ACCEPTED — keep both fields; do NOT take Risk 6's
alternative.** But the design under-sells the reason. `dict_index_type_expr` in
the `arrow::...()` spelling is *precisely* what spec §5.1 names for the accessor's
dictionary type gate ("the declared `dict_index_type_expr` index over the field's
value type", C++ `arrow::dictionary(idx, val)`), and that spelling is consumed at
~40 sites in `recordbatch_accessor_emitter.cpp` plus
`cpp_backend_view_visitor.cpp`. So the field has a **named consumer in round
RIR**, which is categorically different from `DictionaryModifier` (which had none,
ever). Required: say this in D1/Risk 6 and record it as an explicit RIR
carry-forward so it cannot become an orphan.

### Blocking items

1. **R5 over-rejects a legal proto — add the `field_count() == 1` term and
   correct D5's supporting claim.** D5 asserts "It cannot over-reject … **every**
   declaration on that chain is illegal under locked #9. There is no legal shape
   R5 can hit." That is **false** when the flatten wrapper has more than one
   field: `BuildFlattenedRepeated` returns `List<Struct(W)>` at `ir.cpp:366-372`
   (chain loop not entered), `W` is *not* `IsFlattenedWrapper` so it is validated
   on its own, and `W`'s fields become ordinary struct children whose scalar
   dictionary is **legal** (spec §7.1's closing rule) *and honoured by emission*
   (`BuildStructVariant` builds each child with `BuildFieldIr`). R5 as tabled
   (`field->is_repeated() && TYPE_MESSAGE && HasMessageFlatten(type)` + "some
   field on the flatten chain carries the option") would reject it — permanently
   outlawing a legal proto, the exact failure spec §7.1 warns against. Fix: R5's
   walk must mirror `ir.cpp:376`'s loop **including `current->field_count() == 1`**,
   collect **inner** declarations only (not the outer field's — that is R2's), and
   the forcing test must add the negative: `repeated W xs` with
   `W {flatten; string k = 1 [(dictionary)]; int32 n = 2;}` → **nullopt**.
   State the same `field_count() == 1` term explicitly for R4's loop.

2. **R4's equality must include `ordered`, and must compare the DECODED option.**
   This is load-bearing for **locked #8**, not cosmetic: leaf-wins at
   `ir.cpp:332-336` discards the wrapper field's whole option, so
   `W w [(dictionary) = {ordered: true}]` over a leaf that declares
   `{index_type: INT8}` yields a resolved node with `dictionary_ordered == false`
   — **R3 cannot see it**, and R4 is the only rule that catches it. If R4 is
   implemented comparing `index_kind` alone (a plausible reading: D9's R4 example
   text names only the index), `ordered: true` is silently swallowed and locked #8
   is violated. Also pin that comparison is on the decoded `DictionaryOption`, so
   `= {}`, `{index_type: DICTIONARY_INDEX_UNSPECIFIED}` and
   `{index_type: DICTIONARY_INDEX_INT32}` are **equal** (not a conflict). Add the
   exact case to the forcing test: `ordered: true` on the wrapper field + an INT8
   leaf declaration → rejected, reason mentions "conflicting" and `ordered`.

3. **Forcing-test factual error: `Holder.on_wrapper` is not a legal control.** The
   table's last "legal controls" row lists it as `nullopt`, but in the existing
   `kDictSchema` (`protoc/tests/test_type_mapper.cpp:823-824`) `on_wrapper`
   carries `{index_type: DICTIONARY_INDEX_INT64, ordered: true}`, so
   `BuildFieldIr` yields a SCALAR with `dictionary_ordered == true` and **R3
   rejects it**. Either move it to the R3 row or add an `ordered`-free
   wrapper-declared field to `kDict2Schema` and use that as the control. (The
   design explicitly plans to *reuse* `kDictSchema`, so this is not a free
   rename.)

4. **The retarget argument has one real gap: nothing else pins the
   `BaseFacts(w)`-for-a-`flatten_field`-wrapper IR route.** D4 claims "the
   retargeted test still proves the declaration is seen" — but by a *different
   mechanism*: R1 is descriptor-based (`HasFieldDictionary`), whereas the fixture's
   own header comment and DICT-1.5 depend on `ir::BuildFieldIr(w)` carrying the
   fact. Checked: the D4b sub-case (`test_type_mapper.cpp:1131-1158`) asserts only
   `HasFieldDictionary(wrapper)` and the *projection-level drop*
   (`BuildFlattenedFieldList`), never `BuildFieldIr(wrapper).facts.dictionary`. So
   `GenErrors.DictionaryRejectedBy_accessor_fieldFlatten` is today the **only**
   check on that IR route, and the retarget removes it. Required (both, cheap):
   (a) add the one-line assertion
   `EXPECT_TRUE(ir::BuildFieldIr(rec->FindFieldByName("p")).facts.dictionary);`
   to that sub-case — not just the planned comment; (b) **rewrite the fixture
   header comment** in `integration-tests/protoc-coverage/proto/coverage_dictionary_field_flatten.proto`
   (lines 7-14), which after retargeting documents a route the tests no longer
   exercise ("the only route … is BaseFacts(w) … the guard would wrongly exit 0").
   A fixture whose own docstring describes a check it no longer performs is the
   dominant defect class of this round (DICT-1.5 process notes 1-3).
   With (a)+(b) the design's substantive claim stands: post-R1 the shape is
   illegal, so it is unreachable from any accepted proto and the DICT-1.5 route
   becomes dead information rather than unguarded safety. Say it that way.

5. **Document that R1 also fires on a WKT wrapper field carrying `flatten_field`,
   and add the row.** `google.protobuf.StringValue s = 1 [(flatten_field),
   (dictionary)]` satisfies R1's three-term predicate, and correctly so: both
   inlining walks gate only on `TYPE_MESSAGE && !is_repeated && HasFieldFlatten`,
   so they inline `StringValue.value` as a plain non-nullable `utf8` column, while
   `BuildFieldIr` routes through `TryBuildWkt` (`ir.cpp:601`) to a nullable
   `SCALAR` carrying the dictionary — a **second instance of D4's exact
   "R2 accepts / emission drops" class**. Left undocumented this reads as a
   locked-#9 acceptance case ("a wrapper carrying the option is a valid nullable
   dictionary"), and the obvious "fix" — excluding WKTs from R1 — silently
   reopens the hole. Add it to D4 and to the forcing-test table.

### Required (non-blocking, must land in the doc)

6. **New `EXPECT_MESSAGE` values must follow the convention already documented at
   `integration-tests/protoc-coverage/CMakeLists.txt:548-550`** — no literal
   parentheses (ERE capture groups under `MATCHES`) — and the `_passOrder`
   sibling's substring must be one DICT-1.5's message provably cannot match
   (e.g. `cannot be combined with`), otherwise the ordering pin is soft.
7. Fold ruling (b) into D1/Risk 6 (named RIR consumer + explicit carry-forward),
   and add to D1's comment that `is_dictionary` cannot reach the RBA emitter
   because DICT-1.5 rejects `accessor`/`rust` for any dictionary proto.
8. Files-to-touch: the plan correction must also fix DICT-2's Scope bullet
   "`MapField`/`UnsupportedReason`: reject … → `nullopt`", not only the dead
   `type_mapper.cpp:632-669` citations — otherwise the story keeps contradicting
   the shipped design and the conformance reviewer re-opens D0.

### Nits (fix inline if convenient)

* The "root-cause-first ordering means removing the reported defect never reveals
  a differently-worded complaint" property is not absolute: for
  `repeated W xs [(dictionary)]` where `W`'s inner field also declares, R2 fires,
  and after the outer option is removed R5 fires. Scope the claim to "the same
  *declaration*".
* A *used* `(fletcher.flatten)` wrapper whose inner field carries
  `flatten_field` + `dictionary` (`W {flatten; P p = 1 [(flatten_field),
  (dictionary)];}`) is rejected by **R2 on the using field** with the "struct
  column" wording rather than by R1, because `IsFlattenedWrapper(W)` skips `W` and
  D3 only descends into `flatten_field` wrappers from a judged message. Rejected
  either way; worth one line under Risk 5.
* R2's `UNSUPPORTED` kind-word renders as "requires a scalar column, but this
  field maps to an unsupported field type" for a oneof member (`Shapes.oc`). Reads
  oddly but is unreachable at plugin level (#55 fires first); consider a dedicated
  clause.
