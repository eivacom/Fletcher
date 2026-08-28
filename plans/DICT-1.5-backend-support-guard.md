# DICT-1.5 — Front-end guard: reject `accessor` / `rust` for dictionary fields

Round DICT · item DICT-1.5 · design step (step 1) · **revised 2026-08-28 after
step-2 NEEDS-REWORK** (see "Rework log" and the review section at the end)
Story + acceptance: [DICT-dictionary-option.md](DICT-dictionary-option.md) §
"DICT-1.5 — Front-end guard".
Spec: [docs/dictionary-option-spec.md](../docs/dictionary-option-spec.md) §5.1
(amendment 2026-08-28) and §7.1.
Locked decisions: [DICT-locked-decisions.md](DICT-locked-decisions.md) — #5, #9, #10,
**#11**.
Forcing tests: `GenErrors.DictionaryRejectedBy_accessor`,
`GenErrors.DictionaryRejectedBy_rust`.

## Summary

Add a **dictionary predicate to GIR-10's existing `ValidateBackendsSupportFields`
front-end pass** so that requesting `--fletcher_opt=accessor` or `rust` for a
proto containing a `(fletcher.dictionary)` field fails the plugin, before any
artifact is emitted, with an error naming the field, the option, and round RIR.
Detection is **IR-based** (a recursive walk of `ir::BuildFieldIr(...)` nodes
reading `facts.dictionary`), because the IR is the same substrate schema emission
reads — so the guard's field set is provably a **superset** of the set of columns
that can be emitted as `dictionary(idx, val)`. No new validation mechanism, no
second dictionary carrier, and the RBA emitters are not touched.

## Design

### D1 — Detection substrate: **IR-based**, not descriptor-based

The item's central question is soundness: a guard that silently misses a
wrapper-declared dictionary is worse than no guard. The answer is that the guard
must be IR-based, and here is the argument rather than the assertion.

**The safety property to prove.** The guard exists to prevent one thing: emitting
an accessor whose positional type gate and getters assume a value-typed column
for a column the schema emitter declares as `dictionary(idx, val)`. So the
property is not "detect every `(fletcher.dictionary)` token in the file"; it is:

> for every column the schema emitters can emit as a dictionary, the guard fires.

Formally: `Guarded ⊇ EmittedAsDictionary`. Over-approximation is safe (a spurious
rejection is a loud, fixable error); under-approximation is the unsafe direction
(silent mis-read).

**Why the IR is the right substrate.** `EmittedAsDictionary` is defined by the IR,
not by the descriptors: DICT-3's schema emission branches on IR-derived
dictionary-ness (`ir::FieldFacts.dictionary` → the `FieldMapping.is_dictionary`
projection / `cpp_backend::SchemaVisitor`). A declaration the IR drops is a
declaration the *schema* drops too, so the emitted column is value-typed and the
accessor reading it as a value column is **correct** — there is nothing to guard.
A descriptor walk would therefore be sound in the wrong direction: it would reject
protos whose declaration the emitter silently discards (pure false positives) while
still not being tied to what emission actually does.

**Node-set containment (the superset claim, checked against the tree).** The guard
iterates, per file:

```
for msg in OrderedMessages(file):
    if IsRecursive(msg) || IsFlattenedWrapper(msg): continue      # identical to the emit loops
    for i in 0..msg->field_count():
        walk(ir::BuildFieldIr(msg->field(i)))                     # full subtree
```

The RBA emitters iterate the *same* message set with the *same* skip predicate
(`recordbatch_accessor_emitter.cpp:691-692`, `:745-746`, `:2052-2053`) and then take
`GatherFields(msg)`; the schema visitor mirrors `GatherFieldsImpl` in
`BuildFlattenedFieldListImpl` (`cpp_backend_schema_visitor.cpp:66-109`). Those two
walks differ from the guard's in exactly one way: for a field-level
`(fletcher.flatten_field)` wrapper field `w` they `continue` past `w` and recurse
into `w->message_type()`'s fields, calling `ir::BuildFieldIr(inner)`. The guard
instead builds `ir::BuildFieldIr(w)`, which — verified — is a `STRUCT` node whose
`facts = BaseFacts(w)` (`ir.cpp:544-546`) and whose children are built eagerly by
`BuildStructVariant` → `ir::BuildFieldIr(inner)` for every inner field
(`ir.cpp:191-195`, `:562-575`). A walk that recurses into `STRUCT` children
therefore visits **every node the emitters visit, plus the wrapper node itself**,
for chains of any depth. Hence `Guarded ⊇ EmittedAsDictionary`. The extra node
(`w` itself) can only produce the over-approximating direction.

**The full proof obligation, as a closed set** *(added for step-2 item 5 — the
paragraph above only argues the place the two walks differ, which is weaker than the
obligation reviewers must be able to check)*. `facts.dictionary` can become true in
exactly two places: a `BaseFacts(field)` call site (`BaseFacts` at `ir.cpp:61-65` is
the **sole** reader of the option) and `ApplyDictionaryFacts` (`ir.cpp:293-304`).
So the guard is complete **iff**, for every such site, the node it populates is
reachable from some `ir::BuildFieldIr(msg->field(i))` root of a non-skipped message
along the D2 walk's edges (list element, fixed-size-list element, map key/value,
struct field). Enumerating every site in `ir.cpp`:

| Site | Node it populates | Reached by the walk via |
|---|---|---|
| `MakeUnsupported` `:77` | `UNSUPPORTED` (oneof member, unsupported type) | the root itself, or a struct-field / element edge. Moot in practice — `ValidateNoUnsupportedIr` is fatal first (D4) — but covered. |
| `TryBuildWkt` `:235` (`Timestamp`), `:251` (`Duration`), **`:267` (wrapper WKTs)** | singular `SCALAR` | the root itself. **`:267` is locked decision #9's explicitly-valid shape**: `google.protobuf.StringValue f [(fletcher.dictionary)]` maps to a nullable `SCALAR`, DICT-3 will emit `dictionary(idx, utf8)`, and the untouched accessor would mis-read it — a **true positive** with no fixture until now. Fixture added in D8 (`coverage_dictionary_wkt.proto`). |
| `BuildSingularMessage` `:545` | `STRUCT` — **including a `flatten_field` wrapper field** | the root itself |
| `BuildSingularScalarOrEnum` `:556` | singular `SCALAR` / enum | the root itself |
| `BuildRepeatedScalarOrEnum` `:468`, `:471` | `LIST` **and** its element | root + one list-element edge |
| `BuildRepeatedMessage` `:457` | `LIST` of `STRUCT` | the root itself |
| `BuildMapNode` `:482` | `MAP` | the root itself |
| `BuildMapNode` `:495` / `:506` / `:520` | map key, enum value, scalar value | map key/value edges. Built from the **synthetic map-entry** fields, which no author can annotate, so they can never actually carry the fact; covered anyway. |
| `BuildFlattenedRepeated` `:368`, `:398`, `:402`, `:416`, `:420`, `:436` | outermost `LIST` + leaf; intermediate levels keep DEFAULT facts | root + list-element edges — which is exactly why D2 must not stop on an interior `false` |
| `BuildFlattenedSingular` `:314` + `ApplyDictionaryFacts` `:332-336` | the resolved singular node, and its element when that node is a `LIST` | the root itself + one list-element edge |
| `BuildStructVariant` `:571` (every field of a nested / imported / map-value message) | every child node | struct-field edges, transitively |

Every site lands on a node the D2 walk visits. The only declaration not covered is
one that is never *read* at all — `BuildFlattenedRepeated` makes no `BaseFacts(inner)`
call, spec §7.1 gap 2 — treated below. **Maintenance consequence:** if a future change
adds a `BaseFacts` call site under a new child-bearing node kind, D2 must grow an edge
for it (see D2's forward-compat note).

**This is why the DICT-1 carry-forward's wrapper hole is DICT-2's problem and not
ours.** That hole is a *projection-level* hole: `GatherFieldsImpl`
(`generator.cpp:598-610`) and `BuildFlattenedFieldListImpl` `continue` past `w`
before `ProjectIrToFieldMapping` ever sees it, so DICT-2's mapped-`FieldKind` gate
cannot fire for a dictionary declared **on** a `flatten_field` wrapper field — which
is why spec §7.1 gap 1 says that enforcement needs a descriptor walk. DICT-1.5 does
not use those functions. It calls `ir::BuildFieldIr` on each message's own declared
fields (exactly as `ValidateNoUnsupportedIr` and `FindScalarLeafNestedList` already
do), so `BaseFacts(w)` reads the option and the guard fires. **The guard is not
subject to gap 1.** Spec §7.1's "must be a descriptor walk" sentence is scoped to
DICT-2's rejection; D9 adds one clarifying sentence so a reviewer cannot read it as
binding here. This route is pinned by D8's `coverage_dictionary_field_flatten.proto`,
whose dictionary is declared **on the wrapper field itself** — the only shape that can
tell a working `BaseFacts(w)` from a broken one.

**The one declaration the IR cannot see, and why that is safe.** Spec §7.1 gap 2:
`ir::BuildFlattenedRepeated` never calls `BuildFieldIr(inner)`, so for
`repeated W xs` where `W` is a *message-level* flatten wrapper whose single inner
field declares the dictionary, no node carries the fact — and `W` is skipped by the
message loop's `IsFlattenedWrapper` term, so it is not reached that way either. This
declaration is invisible to the guard. It is nevertheless **safe to miss**, and by
construction rather than by luck: schema emission reads the same IR (and
`GatherFieldsImpl`'s inline branch requires `!fd->is_repeated()`, `:606`, so it
consumes the *identical* node), so it also drops the declaration and emits a plain
`list<...>` column. The accessor reads a `list<...>` as a `list<...>`. No mis-read
exists. (Independently, that shape maps to `LIST`, which DICT-2 rejects as
non-`SCALAR` — but the guard's safety does not depend on DICT-2 landing.) Disclosed in
Risks and recorded next to the existing gap in the spec.

**Conclusion: IR-based. No raw-descriptor walk in this item.** Adding one would add
a second detection substrate whose disagreements with the IR are exactly the false
positives above.

### D2 — `FindDictionaryField`: the predicate

New file-local helper in `generator.cpp`'s anonymous namespace, placed immediately
after `FindScalarLeafNestedList` (so the two backend-availability predicates read
as a pair):

```cpp
// DICT-1.5 (locked #11): locate the first node reachable from `node` that carries
// a (fletcher.dictionary) declaration; returns the DECLARING field's fully
// qualified proto name (which may be EMPTY if that node has default facts — the
// caller substitutes the field descriptor's name, see the call site).
//
// PLACEMENT (spec §7.1 / the placement table on ir.cpp's BaseFacts): a dictionary
// declaration is a FIELD-level fact landed per SHAPE, NOT on every node of a
// subtree. The INTERMEDIATE list levels of a nested-list shape keep DEFAULT facts,
// so an interior `dictionary == false` is NOT authoritative. This walk therefore
// descends UNCONDITIONALLY into every child and never treats a false as a reason
// to stop: it is a pure OR over the reachable nodes, not a downward search that
// can be cut off. Do NOT "optimise" it into an early return on false.
//
// This "some node has dictionary == true" shape is deliberate and is the ONE
// context in which it is correct: this is a REJECTION predicate, where
// over-approximating is the safe direction. Spec §7.1's closing rule ("gate on the
// top-level node's kind, not on 'some node has dictionary = true'") governs
// KIND / EMISSION decisions, where an OR would silently change a column's type.
// Do not copy this walk into an emitter.
//
// FORWARD COMPAT (mirrors FindUnsupportedIr, generator.cpp:1614-1617): any future
// child-bearing NodeKind MUST be added here as well as to FindUnsupportedIr and
// FindScalarLeafNestedList — three walks that are now extended together. A missing
// edge here is a silently under-approximating guard.
//
// The live carrier is ir::FieldFacts.dictionary. ir::DictionaryModifier is DEAD
// (its deletion is DICT-2's) and is deliberately NOT read here.
std::optional<std::string> FindDictionaryField(const ir::IrNode& node) {
    if (node.facts.dictionary) return node.facts.proto_full_name;
    if (node.kind == ir::NodeKind::LIST)
        return FindDictionaryField(*std::get<ir::ListNode>(node.node).element);
    if (node.kind == ir::NodeKind::FIXED_SIZE_LIST)
        return FindDictionaryField(*std::get<ir::FixedSizeListNode>(node.node).element);
    if (node.kind == ir::NodeKind::MAP) {
        const auto& m = std::get<ir::MapNode>(node.node);
        if (auto e = FindDictionaryField(*m.key)) return e;
        return FindDictionaryField(*m.value);
    }
    if (node.kind == ir::NodeKind::STRUCT) {
        for (const auto& f : std::get<ir::StructNode>(node.node).fields)
            if (auto e = FindDictionaryField(*f.type)) return e;
    }
    return std::nullopt;
}
```

Load-bearing details:

- **Which nodes are inspected** (the brief's point 2): *every* node reachable from
  the field's root node — root, list elements at every level, fixed-size-list
  elements, map key and value, and struct fields recursively. The predicate is
  `facts.dictionary` on each such node, OR-ed. `SCALAR` and `UNSUPPORTED` are leaves
  (checked, not descended). Nothing anywhere gates on "the top-level node has it" and
  nothing stops on an interior `false` — that is precisely the RR-1 trap: for
  `repeated W xs [(fletcher.dictionary)]` with `W{flatten; repeated string values}`
  the shape is outer LIST `true` / intermediate LIST **false** / leaf SCALAR `true`,
  and the top-level `true` makes this instance trivial, while an inner-declared
  variant would be found only by unconditional descent.
- **FLAGGED CONFLICT with spec §7.1's closing rule** *(step-2 item 2 — raised rather
  than resolved silently)*. §7.1 ends: "*Consumers must in all cases gate on the
  top-level node's kind rather than on 'some node has `dictionary = true`'*."
  `FindDictionaryField` is literally the forbidden shape. **The design's position:**
  that sentence was written for **emission / kind** consumers, where an OR over a
  subtree would change a column's declared Arrow type on evidence that does not belong
  to that column — a correctness bug. A **rejection** predicate has the opposite error
  asymmetry: over-approximating costs a loud, fixable error, under-approximating costs
  a silent mis-read. So the rule is right for its intended consumers and wrong for this
  one, and the fix is to **scope the sentence, not weaken it** (D9). The design does not
  claim authority to reinterpret the spec unilaterally; if step-2/4a prefers the
  sentence to stand unqualified, the only conforming alternative is to make the guard
  gate on the *top-level* node's `facts.dictionary` alone, which would silently miss
  every **struct-nested** declaration — D1's `BuildStructVariant:571` row: a dictionary
  on a field of a struct-typed child (including an **imported** message, which
  `OrderedMessages` excludes via `msg->file() != file`, `generator.cpp:175`, so the
  struct-field edge is its *only* detection route) — i.e. it would break the safety
  property this item exists to provide. *(Note the precise miss set: a **propagated**
  declaration is not among it, because `ApplyDictionaryFacts` lands the fact on the
  returned root node itself, `ir.cpp:293-304`; the load-bearing miss is the struct-child
  edge. Stated exactly so the flagged conflict rests on a checkable example.)* Recorded
  so the choice is reviewed, not assumed.
- **Style mirrors `FindScalarLeafNestedList`** (if-chain, `std::get`, same return
  type) so the pair is diffed as one idiom.
- **Empty-name hazard.** The returned `std::optional<std::string>` can hold an empty
  string (a node with default facts can in principle be flagged). `if (auto e = ...)`
  is true for an empty string, so the call site must test `e->empty()`, never `!e`.
- `ir::DictionaryModifier` is not referenced (brief point 3).

### D3 — Hook: extend the existing pass, add no new one

`ValidateBackendsSupportFields` (`generator.cpp:1715-1733`) gains a second check
inside its existing per-field body — one traversal, one `BuildFieldIr` per field, no
new function called from `Generate()`:

```cpp
        for (int i = 0; i < msg->field_count(); ++i) {
            auto node = ir::BuildFieldIr(msg->field(i));
            if (auto e = FindScalarLeafNestedList(node)) {           // GIR-10, unchanged
                *error = ...;
                return false;
            }
            if (auto e = FindDictionaryField(node)) {                // DICT-1.5
                const std::string name = e->empty() ? msg->field(i)->full_name() : *e;
                *error = "field '" + name +
                         "': (fletcher.dictionary) columns are not yet supported by the "
                         "RecordBatch accessor / Rust backend (tracked for round RIR); "
                         "regenerate without --fletcher_opt=accessor,rust (the edge, "
                         "Arrow view, IPC schema and TS backends accept dictionary fields)";
                return false;
            }
        }
```

- The function's existing early-out `if (!emit_accessor && !emit_rust) return true;`
  is what makes the no-false-positive acceptance criterion hold **by construction**:
  with `ipc,ts` (or no opts) the pass is a no-op. Do not move that early-out.
- **Diagnostic determinism.** First offending field in `OrderedMessages` × field
  declaration order wins; within one field, the GIR-10 nested-list complaint wins
  over the dictionary complaint. Stated so tests can pin an exact field name.
  GIR-10's two existing tests are unaffected (their fixture declares no dictionary),
  which the round's regression run confirms rather than assumes.
- Update the function's leading comment block to say it now guards **two** shapes,
  and cite locked #11 alongside locked #3.

### D4 — Ordering relative to `ValidateNoUnsupportedIr` (#55)

The requirement — the guard must run after `ValidateNoUnsupportedIr`, so a genuinely
unsupported type reports its own error instead of being masked — is satisfied
**by placement, with no code change**: `ValidateNoUnsupportedIr` is the first
statement of `ArrowRowGenerator::Generate()` (`generator.cpp:1787`) and
`ValidateBackendsSupportFields` is called at `generator.cpp:1850`, after option-token
parsing and metadata-rule compilation. Because DICT-1.5 adds the predicate *inside*
the latter, it inherits the ordering.

This matters concretely, not theoretically: `ir::MakeUnsupported` uses
`BaseFacts(field)`, so an `UNSUPPORTED` node **does** carry `facts.dictionary` — e.g.
a dictionary on a oneof member, or a dictionary field sitting in a message that also
has a `google.protobuf.Any` field. Without the ordering the guard could pre-empt the
real diagnosis. D8 turns this from a claim into a test
(`GenErrors.DictionaryGuardDoesNotMaskUnsupportedType`).

Do **not** hoist the check earlier or into `ValidateNoUnsupportedIr`: it depends on
`emit_accessor` / `emit_rust`, which are only known after token parsing, and
`ValidateNoUnsupportedIr` is deliberately backend-independent.

### D5 — Error text contract

Requirements from the story: name the option, name the field, say it is tracked for
RIR, be actionable without reading the source. The text in D3 satisfies all four and
deliberately reuses GIR-10's "not yet supported by the RecordBatch accessor / Rust
backend" phrasing so the two guards read as one family.

One deliberate wording deviation from GIR-10: GIR-10 says the other backends "do
support them". At DICT-1.5 time the edge/IPC/TS backends *accept* a dictionary field
but still emit it value-typed (DICT-3 adds the `dictionary(idx, val)` schema type),
so "do support them" would be a forward-dated claim. The text says **"accept
dictionary fields"**, which is true both before and after DICT-3 and needs no edit
when DICT-3 lands.

### D6 — Error/edge paths, safety, concurrency

- **Fail-soft reader interacts correctly.** DICT-1's `ReadFieldDictionaryOption` is
  fail-soft: a *declared but unreadable* option resolves to defaults with
  `dictionary = true`. The guard therefore fires for a corrupt payload too — the safe
  direction (loud rejection, not silent value-typed emission). No error channel is
  needed in the reader; this item does not change it.
- **No emission before the verdict.** The pass runs before the first `context->Open`
  (`generator.cpp:1852` onward), so a rejected proto produces **no** partial output
  file. `GenerateAll` (`:1933-1963`) loops `Generate()` per file *before* opening the
  shared `__rba.fletcher.rs`, so the invariant is exact rather than approximate.
- **Concurrency: none introduced.** The predicate is a pure function of the
  descriptor/IR with no mutable state; `ValidateBackendsSupportFields` remains a
  per-`Generate()` stack-local call. It does not touch the deliberately
  non-thread-safe `OptionMetadataResolver`.
- **Cost.** No additional `ir::BuildFieldIr` calls (the node is already built for the
  nested-list check); the only added work is one pointer walk over an
  already-materialised tree. `BuildFieldIr` performs a serialize + `DynamicMessage`
  re-parse via `BaseFacts` **only for fields that carry at least one option**
  (`option_reader.cpp:150-156` short-circuits on `opts.ByteSizeLong() == 0`, which is
  the shared default instance for an options-less field), and that cost was introduced
  by DICT-1 and is unchanged here; noted so nobody attributes it to this item and so
  nobody "optimises" against a wrong cost model.
- **Recursion termination.** `IsRecursive` messages become `UNSUPPORTED` leaves inside
  `BuildFieldIr`, so the struct recursion cannot loop; the walk only traverses an
  already-materialised finite tree.

### D7 — Explicitly not touched (scope guard)

`protoc/src/recordbatch_accessor_emitter.{hpp,cpp}` and all Rust accessor emission:
**zero diff** (GIR locked #3, DICT locked #11 — RIR owns them). This item only
*prevents* emission. Also zero diff: `ir.hpp` / `ir.cpp` (the carrier already
exists), `type_mapper.{hpp,cpp}` behaviour, and the dead `ir::DictionaryModifier`
(deleted by DICT-2, not read here).

### D8 — Tests, fixtures and harness wiring

All new tests live in `integration-tests/protoc-coverage`, beside GIR-10's
`GenErrors.ScalarLeafNestedListRejectedBy_*`, and reuse
`cmake/run_backend_guard_check.cmake`. Fixtures follow the GIR-8 idiom already
established in that directory ("each shape in its own single-message fixture so one
protoc invocation isolates one shape and the error must name that shape's field") and
are **not wired into any generation unit** — each is invoked only by its own test.

**Fixture file conventions** (match `proto/coverage_unsupported.proto` exactly — CI
runs whole-tree header scans, so these are not optional):

- `// SPDX-License-Identifier: LGPL-3.0-or-later` + `// Copyright (C) 2026 The Fletcher Authors`
  header, then `syntax = "proto3";`.
- A distinct package per fixture: `package integration.coverage.dict_guard;`,
  `…dict_guard_wkt;`, `…dict_guard_flatten;`, `…dict_guard_field_flatten;`,
  `…dict_guard_unsupported;` — distinct so two fixtures can never collide in one pool
  and so the expected field names below are unambiguous.
- A header comment reading "COMMITTED BUT NOT WIRED into any generation unit; invoked
  only by ctest `GenErrors.<name>`", mirroring `coverage_unsupported.proto:11-13`.
- `import "fletcher/options.proto";` in all of them; the unsupported one also imports
  `google/protobuf/any.proto`.

**Script changes** (`cmake/run_backend_guard_check.cmake`), all additive and all
leaving GIR-10's two existing tests on a byte-identical path because they pass none of
the new variables:

1. Optional `EXPECT_FIELD` — when set, the combined stdout+stderr must also match it.
   Mirrors `run_unsupported_generation_check.cmake`'s existing two-assertion shape, and
   is what lets the tests assert the error *names the field* as the story requires.
   Guard it as `if(DEFINED EXPECT_FIELD AND NOT EXPECT_FIELD STREQUAL "")`, **not** a
   bare truthiness probe, so a value like `0`/`OFF`/`N`/`*-NOTFOUND` cannot silently
   disable the assertion.
2. Optional `EXPECT_SUCCESS` (`if(EXPECT_SUCCESS)`) — invert the verdict:
   - require `rc EQUAL 0`, and
   - **require the expected artifacts to exist and be non-empty** — new
     `EXPECT_ARTIFACTS`, a `|`-joined list of basenames relative to `OUT_DIR`, each
     checked with `EXISTS` and a non-zero `file(SIZE ...)`.
     *This is the point of the mode:* `rc EQUAL 0` alone passes just as happily if the
     plugin emitted nothing at all, which is exactly the regression the acceptance
     criterion ("still generates **everything else** normally") is about.
   - `EXPECT_MESSAGE` moves out of the unconditional required-argument `foreach` and is
     required only when `EXPECT_SUCCESS` is falsy; `EXPECT_ARTIFACTS` is required when
     it is truthy.
   - Both new FATAL branches print `stdout`/`stderr` like the existing one does.
   - Keeping this in one script keeps ONE place that owns the protoc invocation shape
     (plugin flag, three `-I` roots, out dir) instead of a near-duplicate; the header
     comment is extended to say the script asserts **both** directions of the same
     guard.

**Fixtures** (`integration-tests/protoc-coverage/proto/`):

| Fixture | Shape it isolates | First offending field |
|---|---|---|
| `coverage_dictionary.proto` | plain singular scalar dictionary: `DictScalarGuard { optional string category = 1 [(fletcher.dictionary) = {index_type: DICTIONARY_INDEX_INT16}]; int32 seq = 2; }` | `DictScalarGuard.category` |
| `coverage_dictionary_wkt.proto` | **locked #9's valid wrapper-WKT dictionary**: `DictWktGuard { google.protobuf.StringValue label = 1 [(fletcher.dictionary) = {}]; }` (imports `google/protobuf/wrappers.proto`) — reaches `TryBuildWkt` `ir.cpp:267` | `DictWktGuard.label` |
| `coverage_dictionary_flatten.proto` | message-level flatten wrapper, **inner**-declared: `DictFlattenGuard { DictFlatWrap w = 1; }` + `DictFlatWrap { option (fletcher.flatten) = true; string kind = 1 [(fletcher.dictionary) = {}]; }` | `DictFlatWrap.kind` |
| `coverage_dictionary_field_flatten.proto` | **field-level flatten wrapper, declared ON THE WRAPPER FIELD** — the DICT-1 carry-forward's exact shape: `DictFfGuard { DictFfWrap w = 1 [(fletcher.flatten_field) = true, (fletcher.dictionary) = {}]; }` + `DictFfWrap { string kind = 1; int32 n = 2; }` (no dictionary inside) | `DictFfGuard.w` |
| `coverage_dictionary_unsupported.proto` | ordering: `optional string category = 1 [(fletcher.dictionary) = {}];` **and** `google.protobuf.Any payload = 2;` in one message | (expects the *Any* error) |

Why each shape earns its place:

- **`coverage_dictionary_flatten.proto`** is load-bearing for D1's propagation claim:
  `DictFlatWrap` is skipped by the message loop's `IsFlattenedWrapper` term, so the
  **only** route to `kind`'s declaration is
  `BuildFieldIr(w)` → `BuildFlattenedSingular` → `ApplyDictionaryFacts` on the resolved
  inner node. Break the propagation and this test goes red.
- **`coverage_dictionary_field_flatten.proto`** is load-bearing for the *wrapper-field*
  route (step-2 item 1 — the previous revision got this wrong). `DictFfWrap` has **no**
  message-level `flatten`, so `IsFlattenedWrapper` is false, `DictFfWrap` is in
  `OrderedMessages`, and it is visited **directly**. An inner-declared dictionary would
  therefore be found even with `BaseFacts(w)` and the `STRUCT` recursion both broken —
  the test would look like protection and provide none. Declaring the dictionary **on
  `w` itself** is the one shape both emit walks `continue` past
  (`GatherFieldsImpl:606-610`, `BuildFlattenedFieldListImpl:78-90`), so only
  `BaseFacts(w)` (`ir.cpp:545`) can see it — a genuine red-if-broken test, and it also
  pins that `DictFfWrap` itself contains no dictionary so the expectation cannot be
  satisfied by the wrong route. Expected field name is `DictFfGuard.w`.
- **`coverage_dictionary_wkt.proto`** must be its own fixture, not a second field in
  `coverage_dictionary.proto`: the guard stops at the **first** offender, so a shared
  fixture would leave the WKT path unexercised (step-2 item 5).

**ctest cases** (added to `integration-tests/protoc-coverage/CMakeLists.txt`
immediately after the GIR-10 `foreach(_gopt accessor rust)` block):

| Test | Invocation | Assertion |
|---|---|---|
| `GenErrors.DictionaryRejectedBy_accessor` | `coverage_dictionary.proto`, `--fletcher_opt=accessor` | non-zero exit; `EXPECT_MESSAGE=dictionary.*RecordBatch accessor / Rust backend.*round RIR`; `EXPECT_FIELD=DictScalarGuard.category` |
| `GenErrors.DictionaryRejectedBy_rust` | same fixture, `--fletcher_opt=rust` | same |
| `GenErrors.DictionaryRejectedBy_accessor_wkt` | `coverage_dictionary_wkt.proto`, `accessor` | same message; `EXPECT_FIELD=DictWktGuard.label` |
| `GenErrors.DictionaryRejectedBy_accessor_flatten` | `coverage_dictionary_flatten.proto`, `accessor` | same message; `EXPECT_FIELD=DictFlatWrap.kind` |
| `GenErrors.DictionaryRejectedBy_accessor_fieldFlatten` | `coverage_dictionary_field_flatten.proto`, `accessor` | same message; `EXPECT_FIELD=DictFfGuard.w` |
| `GenErrors.DictionaryAcceptedWithoutRbaBackends` | `coverage_dictionary.proto`, `--fletcher_opt=ipc,ts`, `EXPECT_SUCCESS=ON`, `EXPECT_ARTIFACTS=coverage_dictionary.fletcher.pb.h\|coverage_dictionary.fletcher.arrow.pb.h\|coverage_dictionary.fletcher.ts` | exit 0 **and** all three artifacts exist and are non-empty — no false positive, and not vacuous |
| `GenErrors.DictionaryGuardDoesNotMaskUnsupportedType` | `coverage_dictionary_unsupported.proto`, `accessor` | non-zero exit; `EXPECT_MESSAGE=google.protobuf.Any is dynamically typed` — proves D4's ordering |

The two named forcing tests carry the token dimension (`accessor` / `rust`); the three
shape fixtures use `accessor` only, since the token dimension is proven orthogonal by
the first pair (one token-independent predicate, one call site). `OUT_DIR` is a
dedicated `${CMAKE_CURRENT_BINARY_DIR}/dictionary-guard-generated`, so no output lands
in `GENERATED_DIR` (`${CMAKE_CURRENT_BINARY_DIR}/generated`, CMakeLists `:75`) where
`check_no_value_or_die.cmake` and `validate_generated_ipc.cmake` scan.

Note on `EXPECT_MESSAGE` / `EXPECT_FIELD` being CMake regexes: `MATCHES` is ERE, so
parentheses in `(fletcher.dictionary)` would be capture groups. The expectations above
deliberately avoid parentheses and use `.*` to span, while the *emitted* text still
spells `(fletcher.dictionary)` literally for the human reader. Dots in the field
expectations are regex `.` and match the literal dots.

**Acceptance item covered by existing tests, not new ones:** "a non-dictionary proto
with `accessor,rust` is unaffected" is already enforced by the main `coverage.proto`
generation unit, which builds with `--fletcher_opt=ipc,accessor,ts,rust` and fails the
build on a non-zero exit — the same argument GIR-10 relied on. No new test.

**No protoc unit test.** `FindDictionaryField` / `ValidateBackendsSupportFields` are
file-local to `generator.cpp` (anonymous namespace) and reachable only through the
plugin binary, exactly as for GIR-10's predicate; the plugin-exit ctests are the
coverage. Do not add external linkage just to unit-test it.

**Red-first evidence.** Before the guard, the plugin exits 0 for all four rejecting
fixtures under `accessor`/`rust`, so `run_backend_guard_check.cmake`'s
`if(rc EQUAL 0)` FATALs and both forcing tests are red with a message that names the
unexpected success. Two of the seven are **not** red-first and must be reported as
such rather than presented as evidence: `GenErrors.DictionaryGuardDoesNotMaskUnsupportedType`
(green before and after — an ordering regression guard) and
`GenErrors.DictionaryAcceptedWithoutRbaBackends` (green before and after — a
no-false-positive guard).

### D9 — Docs

- `docs/dictionary-option-spec.md` §7.1 — three edits, all scoping, none weakening:
  1. **Gap 1:** scope "enforcement must be a front-end descriptor walk" to DICT-2's
     mapped-kind rejection, and record that DICT-1.5's backend guard is **IR-based**
     and unaffected by gap 1 because it calls `ir::BuildFieldIr` on each message's own
     declared fields (so `BaseFacts(w)` reads the wrapper's option) rather than
     consuming `GatherFieldsImpl`'s output. Word the surviving requirement as *"a walk
     rooted at each message's own **declared** fields (descriptor or
     `ir::BuildFieldIr`) rather than a check on the projection's output"* — otherwise
     DICT-2 reads the text as a mandate to write a second bespoke descriptor walker,
     which is the drift locked #10 warns against. The choice stays DICT-2's; what must
     not survive is a false mandate.
  2. **Gap 2:** record that the DICT-1.5 guard cannot see that declaration and why
     that is safe — schema emission consumes the identical node
     (`GatherFieldsImpl:606` requires `!is_repeated`), so it drops the declaration
     too and no mis-read exists.
  3. **Closing rule:** scope "*consumers must in all cases gate on the top-level
     node's kind rather than on 'some node has `dictionary = true`'*" to **kind /
     emission** consumers, and add that a **rejection** predicate
     (`FindDictionaryField`) deliberately does the opposite because
     over-approximation is its safe direction. The rule stays binding for emitters;
     only its scope is stated. (This is the D2 flagged conflict; the spec edit is
     where it is resolved on the record.)
     **Bound the exception explicitly** — it covers only a **backend-availability**
     guard, whose over-approximation costs one regeneratable error and no wrong
     output. It must **not** read as licence for DICT-2's **legality** gate to OR over
     a subtree: that gate decides whether a declaration is *valid*, so
     over-approximation there permanently rejects a legal proto, and locked #9
     requires it to gate on the field's own **mapped `FieldKind`** (a scalar
     dictionary inside a struct child stays legal — the ancestor's kind is not
     evidence about it). Without this clause the scoping edit trades one silent
     hazard for a licence to over-reject.
- `docs/fletcher-options.md`: in the `(fletcher.dictionary)` section, add the v1
  limitation line — a dictionary proto must omit `--fletcher_opt=accessor,rust` until
  round RIR, and the plugin enforces it. (Small; keeps the error actionable from the
  docs as well as from the message.)
- `protoc/include/type_mapper.hpp:88-95`: extend the existing note about the
  `ValidateBackendsSupportFields` / `FindScalarLeafNestedList` guard to mention the
  dictionary predicate, so the comment does not read as the pass's full inventory.

## Forcing-test mapping

| Forcing test | Design element that turns it green |
|---|---|
| `GenErrors.DictionaryRejectedBy_accessor` | D3's dictionary branch in `ValidateBackendsSupportFields` fires for `emit_accessor` on the D8 `coverage_dictionary.proto` fixture; D2's `FindDictionaryField` finds `facts.dictionary` on the singular `SCALAR` node built from `category`; D5's text supplies the `dictionary … RecordBatch accessor / Rust backend … round RIR` substring and `DictScalarGuard.category` for `EXPECT_FIELD`; the pass returns `false` at `generator.cpp:1850`, before the first `context->Open`, so protoc exits non-zero with no output. |
| `GenErrors.DictionaryRejectedBy_rust` | Identical path via the same function's `emit_rust` parameter — one predicate, both tokens, so no second code path exists to diverge. |
| Acceptance: dictionary proto **without** `accessor`/`rust` still generates | D3 keeps `ValidateBackendsSupportFields`' existing `if (!emit_accessor && !emit_rust) return true;` early-out, so the pass is a no-op for `ipc,ts`. Pinned by `GenErrors.DictionaryAcceptedWithoutRbaBackends`, whose `EXPECT_SUCCESS` mode asserts the three expected artifacts exist and are non-empty (D8) — exit 0 alone would be vacuous. |
| Acceptance: non-dictionary proto with `accessor,rust` unaffected | D2's predicate only fires on `facts.dictionary`, which is false everywhere absent the option; pinned by the existing `coverage.proto` `ipc,accessor,ts,rust` generation unit. |
| Acceptance/ordering: unsupported type is not masked | D4 — the predicate lives inside the pass already called after `ValidateNoUnsupportedIr`; pinned by `GenErrors.DictionaryGuardDoesNotMaskUnsupportedType`. |
| Class coverage beyond the two named tests | D1's closed-set `BaseFacts` table (every site the fact can land on) + D2's unconditional descent (lists at every level, fixed-size lists, map key/value, struct fields, arbitrarily deep `flatten_field` chains), pinned by D8's three extra shape fixtures: wrapper-WKT (locked #9, `ir.cpp:267`), message-flatten propagation, and dictionary declared **on** a `flatten_field` wrapper field. |
| No drift in accessor output | D7 — zero diff in the RBA emitters; the existing RBA no-drift golden stays green. |

## Risks / Unknowns

1. **Disclosed detection gap (not a STOP-AND-ASK).** Spec §7.1 gap 2 —
   `repeated W xs` with `W` a message-level flatten wrapper whose *inner* field
   declares the dictionary — is invisible to this guard. D1 argues it is safe because
   the same IR feeds schema emission, so the emitted column is not a dictionary and
   there is nothing to mis-read. If a reviewer disagrees with that argument, the
   consequence is not a redesign of this item: it is closing the IR gap (DICT-2's
   `BuildFlattenedRepeated` carry-forward), which fixes emission and detection
   together. Recorded in the spec by D9 so it cannot be lost.
2. **Assumption: DICT-3 will read dictionary-ness from the IR** (`facts.dictionary` →
   `is_dictionary`), which is what makes `Guarded ⊇ EmittedAsDictionary` hold. This is
   locked decision #5, so the assumption is contractual rather than speculative — but
   if DICT-3 ever introduced a second, descriptor-derived source of dictionary-ness for
   schema emission, this guard's superset property would need re-checking. Worth one
   line in DICT-3's design.
3. **Over-rejection is intentional.** A dictionary on a `flatten_field` wrapper field,
   on a `repeated`/`map`/struct field, or with `ordered: true` will trip this guard
   under `accessor`/`rust` even though DICT-2 will later reject those outright. Before
   DICT-2 lands the guard is the only complaint; after, DICT-2's error wins only if it
   runs earlier — it does not, so the guard's message may be the one the user sees for
   an already-invalid proto. Acceptable (both messages are actionable and the proto is
   invalid either way), and worth a sentence in DICT-2's design so the two diagnostics
   are ordered on purpose rather than by accident.
4. **Modifying a GIR-10 test script.** D8 extends `run_backend_guard_check.cmake` with
   three optional variables. The alternative — a separate `run_generation_success_check.cmake`
   — was rejected to avoid a second copy of the protoc invocation shape. Risk is
   contained: GIR-10's two `add_test` blocks are not edited and pass none of the new
   variables, so their code path is unchanged; the `foreach(_req ...)` relaxation for
   `EXPECT_MESSAGE` is the one line to review carefully, and the `if(DEFINED … AND NOT
   … STREQUAL "")` form is mandated in D8 so a falsy-looking value cannot silently
   disable an assertion.
5. **Field-name expectations depend on `proto_full_name`** being the *fully qualified*
   proto name (package-qualified). The fixtures' expectations use the
   `Message.field` tail only, and D8 mandates a distinct package per fixture, so a
   package prefix cannot break them; dots in `EXPECT_FIELD` are regex `.` and match
   literal dots.
6. **No STOP-AND-ASK.** The design extends the mandated pass, honours locked #5
   (`FieldFacts` as the only carrier read), #9 (the wrapper-WKT dictionary is treated
   as the valid shape it is, and is a true positive here), #10 (no new option-reading
   mechanism), and #11 (RBA untouched, guard removable by RIR in one place). No public
   API change, no version bump: `FindDictionaryField` is file-local and
   `ValidateBackendsSupportFields`' signature is unchanged. Two deviations are
   **recorded rather than left to be discovered**:
   - **Story-wording deviation (step-2 item 4).** The story's Scope says "reusing
     DICT-1's `HasFieldDictionary` reader". The design **never calls
     `HasFieldDictionary`**; it reads `ir::FieldFacts.dictionary`, which *is* that
     reader's output as landed by `BaseFacts` (`ir.cpp:61-65`) — and reading the IR
     rather than re-reading the descriptor is precisely what makes the superset
     property in D1 hold (`option_reader.hpp:76-80` says as much: the descriptor-based
     overload "cannot see a dictionary that reached a node through flatten
     propagation"). Same substrate, one reader, no second option parse. Step-4a will
     diff against the story text, so this is flagged, not silent.
   - **Spec §7.1 closing-rule conflict** — see D2's flagged-conflict bullet and D9's
     spec edit 3. Raised for review, not resolved unilaterally.
7. **RIR removal contract.** The guard is exactly one `if` block plus one file-local
   function plus the D8 tests/fixtures, all named for DICT-1.5, so RIR can delete it
   atomically as locked #11 requires. `plans/RIR-rba-onto-ir.md` already anticipates
   "the DICT-1.5 dictionary predicate" — no plan edit needed.

## Files-to-touch

**Modified**

- `protoc/src/generator.cpp` — add `FindDictionaryField` (anon namespace, after
  `FindScalarLeafNestedList` at `:1705`); add the dictionary branch inside
  `ValidateBackendsSupportFields`' field loop (`:1720-1730`); extend that function's
  leading comment (`:1707-1714`) to cover both guarded shapes and cite locked #11.
- `integration-tests/protoc-coverage/cmake/run_backend_guard_check.cmake` — optional
  `EXPECT_FIELD`; optional `EXPECT_SUCCESS` + required-with-it `EXPECT_ARTIFACTS`
  (exists + non-empty); `EXPECT_MESSAGE` required only when not `EXPECT_SUCCESS`;
  `if(DEFINED … AND NOT … STREQUAL "")` truthiness form; stdout/stderr printed in the
  new FATAL branches; header comment extended.
- `integration-tests/protoc-coverage/CMakeLists.txt` — the seven `add_test` cases of
  D8 after the GIR-10 `foreach(_gopt accessor rust)` block (`:517-530`).
- `docs/dictionary-option-spec.md` — §7.1 gap 1, gap 2 and closing-rule scoping edits
  (D9).
- `docs/fletcher-options.md` — `(fletcher.dictionary)` v1 limitation line (D9).
- `protoc/include/type_mapper.hpp` — extend the guard note at `:88-95` (D9).

**Added** (all with SPDX + copyright header, `syntax = "proto3"`, distinct package,
"committed but not wired" comment — D8)

- `integration-tests/protoc-coverage/proto/coverage_dictionary.proto`
- `integration-tests/protoc-coverage/proto/coverage_dictionary_wkt.proto`
- `integration-tests/protoc-coverage/proto/coverage_dictionary_flatten.proto`
- `integration-tests/protoc-coverage/proto/coverage_dictionary_field_flatten.proto`
- `integration-tests/protoc-coverage/proto/coverage_dictionary_unsupported.proto`

**Explicitly unchanged (verify zero diff)**

- `protoc/src/recordbatch_accessor_emitter.{hpp,cpp}` and all Rust accessor emission
  (scope guard; a diff here is a stop-and-ask).
- `protoc/include/ir.hpp`, `protoc/src/ir.cpp` (carrier already exists;
  `ir::DictionaryModifier` stays untouched and unread — DICT-2 deletes it).
- `protoc/src/option_reader.cpp`, `protoc/src/cpp_backend_schema_visitor.cpp`,
  `protoc/src/type_mapper.cpp`.

## Rework log — step-2 NEEDS-REWORK addressed (2026-08-28)

| Item | Grade | Where it is now addressed |
|---|---|---|
| 1 — field-flatten fixture pinned nothing | BLOCKING | D8: `coverage_dictionary_field_flatten.proto` now declares the dictionary **on the wrapper field** (`w = 1 [(fletcher.flatten_field) = true, (fletcher.dictionary) = {}]`, no dictionary inside `DictFfWrap`), expecting `DictFfGuard.w`; the "why each shape earns its place" note explains why the previous inner-declared spelling was green-regardless. D1 cross-references it. |
| 2 — unflagged design↔spec conflict | BLOCKING | New **flagged-conflict** bullet in D2 (states the conflict, the position, and the cost of the conforming alternative) + a comment paragraph in `FindDictionaryField` + D9 spec edit 3, which scopes §7.1's closing rule to kind/emission consumers without weakening it. Also listed in Risk 6. |
| 3 — vacuous `EXPECT_SUCCESS` | BLOCKING | D8 script change 2 adds `EXPECT_ARTIFACTS` (exists + non-zero size), required whenever `EXPECT_SUCCESS` is set; the ctest table and the forcing-test mapping row both name the three artifacts. |
| 4 — story-wording deviation unrecorded | REQUIRED | Risk 6 first sub-bullet, with the `option_reader.hpp:76-80` citation explaining why reading `facts.dictionary` is what makes D1's property hold. |
| 5 — proof must cover all `BaseFacts` sites incl. `TryBuildWkt`; locked #9 shape unfixtured | REQUIRED | D1's new closed-set table (12 rows, every `BaseFacts`/`ApplyDictionaryFacts` site with its reaching edge) + new fixture `coverage_dictionary_wkt.proto` and test `GenErrors.DictionaryRejectedBy_accessor_wkt`; locked #9 added to the header's locked-decision list. |
| 6 — CMake truthiness / print streams | non-blocking | D8 script changes 1 and 2; Risk 4. |
| 7 — fixture conventions | non-blocking | New "Fixture file conventions" block in D8 (SPDX, `proto3`, distinct package, not-wired comment); Files-to-touch "Added" heading restates it; Risk 5 now rests on it. |
| 8 — D9 spec wording must not become a false mandate for DICT-2 | non-blocking | D9 edit 1 prescribes the replacement wording ("rooted at each message's own **declared** fields (descriptor or `ir::BuildFieldIr`)") and explicitly leaves the choice to DICT-2. |
| 9 — mirror `FindUnsupportedIr`'s forward-compat note | non-blocking | FORWARD COMPAT paragraph in `FindDictionaryField`'s comment; maintenance sentence at the end of D1's table. |
| 10 — two inline fixes by the reviewer | n/a | Retained (D1 now cites **D9**; D6's cost bullet keeps the `ByteSizeLong() == 0` fast path). |

Unchanged by this rework: D1's substrate decision, D3's hook, D4's ordering, D5's
error text, D7's scope guard. No stop-and-ask arose; the one genuine tension (item 2)
is flagged in-doc rather than escalated, because the conforming alternative is
described and the resolution is a spec **scoping** edit, not a locked-decision
deviation. If step-2 disagrees with that judgement, item 2 becomes the stop-and-ask.

## Step-2 review (2026-08-28)

**Verdict: NEEDS-REWORK** — no stop-and-ask, no locked-decision deviation, no
parallel validation mechanism. The substrate choice is right and the four
load-bearing claims survive checking against the tree. What fails is (a) one
design→spec conflict that is *not* flagged, and (b) two tests that do not pin the
claims the doc says they pin. All five required changes are cheap; none touches D1's
architecture.

### What I verified (against the tree at `9c1cdc4`, not the prose)

- **Claim 1 — SUPERSET (`Guarded ⊇ EmittedAsDictionary`): HOLDS.** The guard's message
  set is `OrderedMessages(file)` minus `IsRecursive || IsFlattenedWrapper`, byte-identical
  to every emit loop (`generator.cpp:1360`, `:1478`, `:1578`, `:1887`;
  `recordbatch_accessor_emitter.cpp:692`, `:746`, `:2053`) and to
  `ValidateNoUnsupportedIr` (`:1655`). `OrderedMessages` itself excludes map-entry,
  cross-file and recursive messages for everyone (`:160-180`). The two consumer walks
  (`GatherFieldsImpl:598-621`, `BuildFlattenedFieldListImpl:66-109`) both end at
  `ir::BuildFieldIr(fd)` for either a declared field or an inner field of a
  `flatten_field` chain, and every such node is reachable from the guard's roots.
  Checked the paths D1 does not name: a nested/imported struct reaches its fields via
  `BuildStructVariant` → `BuildFieldIr(f)` for **all** fields (`ir.cpp:562-575`), a
  map value message becomes `MakeStructNode(val_msg)` (`ir.cpp:516`) whose children are
  likewise all fields, a `flatten_field` wrapper that *also* carries message-level
  `flatten` routes to `BuildFlattenedSingular` which propagates the outer option
  (`ir.cpp:332-336`), and a wrapper whose target is `Any`/`Struct`/recursive becomes
  `UNSUPPORTED` and is fatal in `ValidateNoUnsupportedIr` *before* the guard, so no
  emission happens at all. Extension fields and synthetic map-entry fields are outside
  `msg->field_count()` for guard and emitters alike. No false negative found.
- **Claim 2 — "the DICT-1 wrapper hole does not apply": HOLDS in the code.**
  `BuildFieldIr(w)` for a singular `flatten_field` wrapper field reaches
  `BuildSingularMessage` → `MakeStructNode(msg)` + `node.facts = BaseFacts(field)`
  (`ir.cpp:544-546`), and `BaseFacts` is the sole reader of the option
  (`ir.cpp:61-65`). So the wrapper's own declaration *is* seen, and the inner fields
  are seen via `BuildStructVariant`. Confirmed for chains of any depth. **But no
  fixture exercises it — see item 1.**
- **Claim 3 — §7.1 gap 2 is safe by construction: HOLDS.** `repeated W xs` with
  message-level-flatten `W` routes to `BuildRepeatedMessage` → `BuildFlattenedRepeated`
  (`ir.cpp:450`, `:363-437`), which never calls `BuildFieldIr(inner)`; and
  `GatherFieldsImpl`'s inline branch requires `!fd->is_repeated()` (`:606`), so
  **emission consumes the identical node**. Emission cannot see what the guard cannot
  see. Verified for the schema visitor too (`:78-92`, same predicate). The one thing
  that would break this is a descriptor-derived second source of dictionary-ness in
  DICT-3 — the doc's Risk 2, correctly contractual under locked #5.
- **Claim 4 — ordering needs no code change: HOLDS.** `ValidateNoUnsupportedIr` at
  `:1787` (first statement of `Generate()`), `ValidateBackendsSupportFields` at `:1850`,
  first `context->Open` at `:1856`. `GenerateAll` (`:1933-1963`) loops `Generate()`
  *before* opening the shared `__rba.fletcher.rs`, so "no artifact opened before the
  verdict" is exact, not approximate. The proposed ordering test does pin the order
  (hoisting the predicate ahead of `:1787` flips `EXPECT_MESSAGE` to the dictionary
  text and the test goes red), and the design correctly declines to call it red-first
  evidence.
- **Also confirmed:** predicate lives inside GIR-10's pass, no new call from
  `Generate()`, no second mechanism (no stop-and-ask). `ir::DictionaryModifier`
  (`ir.hpp:103-106`) is dead and unread; the live carrier is `ir::FieldFacts.dictionary`
  (`ir.hpp:157`). Cost is one pointer walk — nothing to measure. Zero planned diff to
  `recordbatch_accessor_emitter.*` / Rust emission. Fail-soft reader really does yield
  `dictionary = true` for a corrupt-but-declared payload (`option_reader.cpp:175`), so
  the guard fires in the safe direction. `option_reader` holds no statics (per-call
  `DynamicMessageFactory`, `:165`), so "no concurrency introduced" is accurate.
  `GENERATED_DIR` is `${CMAKE_CURRENT_BINARY_DIR}/generated` (CMakeLists `:75`), so the
  proposed sibling `dictionary-guard-generated` out dir is outside every scan; there is
  no `file(GLOB)` over `proto/`, so unwired fixtures are safe. GIR-10's own
  `EXPECT_MESSAGE` cannot be satisfied by the dictionary text and vice versa (the token
  `dictionary` appears in only one of the two messages) — good discrimination.

### Required changes

1. **BLOCKING — the field-flatten fixture does not pin the claim D8 says it pins.**
   D8 asserts "the field-flatten fixture pins that the `flatten_field` `continue` in
   the emit walks cannot hide a dictionary from this guard". It does not.
   `DictFfWrap` has no *message-level* `flatten`, so `IsFlattenedWrapper` is false
   (`type_mapper.cpp:304-306`), `DictFfWrap` is in `OrderedMessages` and is visited
   **directly** by the guard's own message loop — the test would stay green even if
   `BaseFacts(w)` and the `STRUCT` recursion were both broken. The shape that actually
   exercises the DICT-1 carry-forward is a dictionary declared **on** the
   `flatten_field` wrapper *field*: `DictFfWrap w = 1 [(fletcher.flatten_field) = true,
   (fletcher.dictionary) = {}]`, which `GatherFieldsImpl`/`BuildFlattenedFieldListImpl`
   `continue` past and which only `BaseFacts(w)` can see. Add that fixture/case
   (expected field name `DictFfGuard.w`); keep the inner-declared one if you like (it
   is a genuine true positive, since the inlined inner node *is* what emission
   consumes) but stop claiming it proves the wrapper route.
2. **BLOCKING — an unflagged design↔spec conflict.** Spec §7.1 closes with
   "**Consumers must in all cases gate on the top-level node's kind rather than on
   'some node has `dictionary = true`'**". `FindDictionaryField` is exactly a
   "some node has `dictionary == true`" consumer. D2 addresses only the *other* half of
   that paragraph (the interior-`false` trap). Flag the conflict explicitly instead of
   silently siding with the design: state in D2 (and in D9's spec edit) that §7.1's
   sentence governs *kind/emission decisions*, where an OR would be a false type
   change, and does not govern a **rejection** predicate where over-approximation is
   the safe direction — then scope the spec sentence accordingly. Do not delete or
   weaken the sentence for emission consumers.
3. **BLOCKING — the exit-0 test as specified cannot detect the failure it exists to
   detect.** `EXPECT_SUCCESS` is defined as "require `rc EQUAL 0` and skip the message
   match", which passes just as happily if the plugin exits 0 having emitted nothing.
   The acceptance criterion is "still generates **everything else** normally". Add a
   positive artifact assertion to the success branch — e.g. require
   `${OUT_DIR}/coverage_dictionary.fletcher.pb.h` (and, for `ts`,
   `coverage_dictionary.fletcher.ts`) to exist and be non-empty — and say so in the
   test table.
4. **REQUIRED (doc only) — record the deviation from the story's wording.** The story's
   Scope says "reusing DICT-1's `HasFieldDictionary` reader"; the design deliberately
   never calls it and reads `ir::FieldFacts.dictionary` instead. That is the better
   choice and it honours locked #5/#10 (the fact *is* the reader's output, one
   substrate) — but it is a literal divergence from the story text and step-4a will
   diff against that text. Add one explicit line to Risk 6: "story-wording deviation,
   recorded: `HasFieldDictionary` is not called; `facts.dictionary` is its value as
   landed by `BaseFacts`, which is what makes the superset property hold."
5. **REQUIRED — make the superset proof cover the node builders it does not name, and
   cover locked #9's named shape.** D1's containment argument is written only about the
   `flatten_field` difference. Strengthen it to the actual proof obligation: every node
   that can carry the fact is built by a `BaseFacts(field)` call site, and the walk
   reaches all of them — including `TryBuildWkt` (`ir.cpp:235`, `:251`, `:267`), which
   is the path for locked decision #9's explicitly-valid
   `google.protobuf.StringValue field [(fletcher.dictionary)]` → `dictionary(idx,
   utf8)`. That shape is a **true positive** DICT-3 will emit and the untouched
   accessor will mis-read, and it has no fixture. Add `coverage_dictionary_wkt.proto`
   as its own single-dictionary-field fixture (the guard stops at the first offender,
   so it cannot share `coverage_dictionary.proto`).

### Non-blocking notes (fix while you are in there)

6. **CMake truthiness.** Implement the two new optional variables as
   `if(DEFINED EXPECT_FIELD AND NOT EXPECT_FIELD STREQUAL "")` /
   `if(EXPECT_SUCCESS)` rather than bare `if(NOT ${_req})`-style probes; a value of
   `0`/`OFF`/`N`/`*-NOTFOUND` would otherwise be silently ignored. Also print
   stdout+stderr in the inverted branch's FATAL message, as the existing branch does.
7. **Fixture conventions.** Match the GIR-8 fixtures exactly
   (`proto/coverage_unsupported.proto`): SPDX + copyright header (CI runs whole-tree
   header scans), `syntax = "proto3";`, a distinct `package integration.coverage.<x>;`,
   and a header comment saying "COMMITTED BUT NOT WIRED, invoked only by ctest
   `<name>`". Spell the syntax/package in the D8 table so the implementer does not
   guess; Risk 5's substring argument then holds by construction.
8. **Consider suggesting the shape of DICT-2's walk in D9's spec edit.** Since this
   item disproves "enforcement must be a *descriptor* walk" for the accessor guard, the
   sentence you add should say "a walk rooted at each message's own **declared** fields
   (descriptor or `ir::BuildFieldIr`) rather than a check on the projection's output" —
   otherwise DICT-2 reads the surviving text as a mandate to write a second bespoke
   descriptor walker, which is the drift locked #10 warns against. Leave the decision
   to DICT-2; just do not leave it a false mandate.
9. **Mirror `FindUnsupportedIr`'s forward-compat note** in `FindDictionaryField`'s
   comment ("any future child-bearing node kind MUST be added here",
   `generator.cpp:1614-1617`) — the two predicates plus this one are now three walks
   that must be extended together.
10. Two inline fixes already applied by this review: the D1 cross-reference to the spec
    edit now says **D9** (was D8), and D6's cost paragraph now records the
    `ByteSizeLong() == 0` fast path so the per-field re-parse cost is not overstated.

## Step-2 review, cycle 2 (2026-08-28)

**Verdict: APPROVE.** All five required items are **discharged**, not acknowledged. I
re-checked each against the tree rather than against the rework log; the two items I
was told to check hardest (B1, R5) are now genuinely load-bearing. Residual items
below are nits — none blocks implementation. Two accuracy fixes applied inline (see
7–8).

### Discharge verified

- **B1 — the field-flatten fixture is now genuinely red-if-broken. CONFIRMED.** With
  the dictionary on `w` and none inside `DictFfWrap`: both emit walks `continue` past
  `w` under the identical predicate `TYPE_MESSAGE && !is_repeated &&
  HasFieldFlatten(fd)` (`generator.cpp:606-610`, `cpp_backend_schema_visitor.cpp:78-90`),
  so no projection-level route exists. `DictFfWrap` has no message-level `flatten`, so
  the guard *does* visit its two fields directly — and they carry nothing, which is what
  makes the fixture discriminating rather than green-regardless. The single surviving
  route is `BuildFieldIr(w)` → `BuildSingularMessage` (not WKT, `HasMessageFlatten`
  false, not recursive) → `node.facts = BaseFacts(field)` at `ir.cpp:545` → root hit,
  naming `…DictFfGuard.w`. Break `BaseFacts(w)` and *no node in the file* carries the
  fact → exit 0 → `if(rc EQUAL 0)` FATAL. Also confirmed nothing intercepts this shape
  earlier: `ValidateNoUnsupportedIr` sees only two `SCALAR` children, and the third
  `HasFieldFlatten` call site (`generator.cpp:129-149`) is include collection, not
  validation. Two options on one field co-exist cleanly — `#50000` (bool) and `#50001`
  (message) are both known to the pool and both re-parse in the same
  `ReparseOptionsWithPool` round-trip.
- **R5 — the closed-set table is genuinely closed. CONFIRMED by exhaustive grep.**
  In production code `facts.dictionary` is written in exactly two places, `ir.cpp:62`
  (`BaseFacts`) and `ir.cpp:295` (`ApplyDictionaryFacts`) — no other source or header
  writes it. The `BaseFacts(` call sites in `ir.cpp` are exactly {77, 235, 251, 267,
  314, 368, 398, 402, 416, 420, 436, 457, 468, 471, 482, 495, 506, 520, 545, 556}; all
  twenty appear in D1's table, plus `ApplyDictionaryFacts` and the
  `BuildStructVariant:571` edge row. Nothing missing, nothing invented.
- **B3 — `EXPECT_ARTIFACTS` cannot pass vacuously, and the three names are the right
  three. CONFIRMED.** `<stem>.fletcher.pb.h` is unconditional (`generator.cpp:1853-1858`);
  `<stem>.fletcher.arrow.pb.h` is written when `!schema_only` **and** the view content is
  non-empty, and `GenerateViewFile`'s `has_views` is true as soon as one non-skipped
  message exists (`:1476-1482`) — `DictScalarGuard` qualifies, so this is not a
  conditionally-absent artifact that would red the test for the wrong reason;
  `<stem>.fletcher.ts` is unconditional under `ts` (`:1872-1878`). Basenames land
  directly in `OUT_DIR` because `-I ${PROTO_DIR}` canonicalises `file->name()` to the
  bare fixture name. `EXISTS` + non-zero `file(SIZE)` closes the "exit 0, emitted
  nothing" hole, and the `EXPECT_MESSAGE`/`EXPECT_ARTIFACTS` required-arg split means
  neither mode can run with no assertion at all.
- **B2 — the scoping is honest.** The conflict is stated, the position is argued, the
  conforming alternative and its cost are named, and D9 edit 3 scopes rather than
  deletes: the rule stays binding for emitters. **One hole in the scoping, fixed
  inline** — see 7.
- **R4 — recorded, and the citation is stronger than the doc claims.**
  `option_reader.hpp:76-80` does say the descriptor-based overload "cannot see a
  dictionary that reached a node through flatten propagation". That makes the story's
  `HasFieldDictionary` wording not merely less tidy but *insufficient*: a descriptor
  walk over each non-skipped message's declared fields would miss
  `coverage_dictionary_flatten.proto`'s `DictFlatWrap.kind` outright, because
  `DictFlatWrap` is skipped by the message loop. Worth one clause in Risk 6 so 4a sees
  the deviation as forced, not preferred.

### Residual items (non-blocking)

1. **No ctest exercises `FindDictionaryField`'s `STRUCT`-child edge.** Every fixture
   hits the fact on the *root* node (scalar, WKT, propagated-flatten, wrapper-field) or
   expects the `Any` error. Delete the `STRUCT` branch and all seven tests stay green.
   Every *in-file* declaring message is visited directly by the message loop, so the
   only shape that isolates the edge is a **cross-file** one: `Outer { Inner i = 1; }`
   importing `Inner { string k = 1 [(fletcher.dictionary) = {}]; }`, with only
   `Outer`'s file passed to protoc — `OrderedMessages` drops `Inner` at
   `generator.cpp:175` (`msg->file() != file`), so the struct-field edge is the sole
   route, while the nanoarrow schema/IPC for `Outer` *does* carry the child's type
   inline from that same IR node. Exposure in a normal build is narrow (the imported
   file is usually generated with the same opts and trips the guard itself), and the
   edge is copied verbatim from two tested neighbours — so: either add the two-file
   fixture, or add one sentence to D8 disclosing that no ctest covers this edge and
   why that is accepted. Do not leave it undisclosed.
2. Consider adding `coverage_dictionary.DictScalarGuard.ipc` to `EXPECT_ARTIFACTS` —
   `ipc` is one of the two tokens under test and its output is currently unasserted.
3. `EXPECT_ARTIFACTS` is a `|`-joined string: split with
   `string(REPLACE "|" ";" …)` and remember CMake's own list separator is `;`, so a
   bare `-DEXPECT_ARTIFACTS=a;b` would arrive pre-split — the `|` convention is right,
   just document it in the script header as `run_unsupported_generation_check` does not.
4. The forcing tests' `EXPECT_MESSAGE` (`dictionary.*RecordBatch accessor / Rust
   backend.*round RIR`) cannot be satisfied by GIR-10's text (no `dictionary` token) and
   vice versa — re-confirmed. Keep it that way when editing D5's wording.
5. D2's empty-name fallback is still the right defensive shape; with the table now
   closed I can add that no *reachable* production path produces `dictionary == true`
   with an empty `proto_full_name` (the only default-facts nodes are
   `MakeListOf`'s intermediate levels, and `ApplyDictionaryFacts` reaches them only
   after setting the non-empty root). Keep the fallback, drop any temptation to
   `assert(!e->empty())`.
6. `coverage_dictionary_unsupported.proto` should keep the `Any` field **after** the
   dictionary field (as tabled) — that ordering is what makes the test prove
   pass-ordering rather than field-ordering.
7. **Applied inline:** D9 edit 3 now *bounds* the rejection-predicate exception to a
   backend-availability guard and explicitly denies it to DICT-2's legality gate
   (which locked #9 requires to gate on the field's own mapped `FieldKind`). Without
   that clause the scoping edit would trade a silent-miss hazard for a licence to
   over-reject a legal struct-nested dictionary.
8. **Applied inline:** D2's flagged-conflict bullet said top-level-only gating "would
   silently miss every propagated and struct-nested declaration". Propagated
   declarations land on the *root* (`ApplyDictionaryFacts`, `ir.cpp:293-304`), so they
   would **not** be missed; the miss set is the struct-child edge. Corrected, because a
   flagged conflict that rests on an overstated example invites 4a to reject the flag
   along with the example.
