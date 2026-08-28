# DICT-1 — step-4b independent code review

- **Branch:** `feature/dictionary-option`
- **Diff base:** `HEAD` = `6fa5085`, reviewed via `git diff --cached HEAD` (staged)
- **Scope reviewed:** `protoc/include/fletcher/options.proto`, `protoc/include/option_reader.hpp`,
  `protoc/src/option_reader.cpp`, `protoc/src/option_metadata.cpp`, `protoc/include/ir.hpp`,
  `protoc/src/ir.cpp`, `protoc/tests/proto_text_pool.hpp`, `protoc/tests/test_type_mapper.cpp`,
  `protoc/CMakeLists.txt`, `protoc/tests/CMakeLists.txt`, docs.
- **Not built/run:** no configured build tree in the repo and no vcpkg/protobuf toolchain located
  within a reasonable budget, so this is a static review. Everything below is reasoned from source.

## Verdict

**No blocking findings.** The core mechanism is sound: the extraction from #121 is faithful, the
lifetime discipline around `DynamicMessage` is correct, the presence probe cannot over-read, and the
enum-by-symbol decode cannot produce an out-of-range kind. The findings are concentrated in two
places: **IR propagation asymmetries on the flattened-repeated path** (real behavioural holes,
undocumented and untested) and **two test sub-cases that do not discriminate the behaviours they
claim to pin**.

Counts: **blocking 0 · should-fix 4 · P2 6 · nit 9**.

---

## Directed answers to the five review questions

### 1. The extraction from #121's internals — CLEAN

`option_metadata.cpp:257-268` vs the original: verified line by line.

- The `cache.find(&opts)` hit is **preserved** and still returns before any work
  (`option_metadata.cpp:257-258`).
- Failure semantics are **identical**: the original returned `nullptr` on parse failure without
  caching; `ReparseOptionsWithPool` returns `nullptr` and the caller returns `nullptr` without
  caching (`:265`). No new negative-caching, no new positive-caching.
- On success, `raw = dyn.get()` is taken *before* the `std::move` into the cache (`:266-268`) —
  correct order preserved.
- **Member declaration order is correct and now documented.** `mutable DynamicMessageFactory factory`
  at `option_metadata.cpp:244`, `mutable std::map<..., std::unique_ptr<Message>> cache` at `:254`
  after the inserted comment block. Reverse-declaration-order destruction therefore runs
  cache-then-factory. The added `MEMBER-ORDER INVARIANT` comment is exactly the right mitigation for
  a hazard a green suite cannot reveal.
- `factory` is `mutable`, so taking `&factory` from the `const` `ParsedOptions` compiles as before.

Nothing to fix here.

### 2. Lifetime / ownership around `DynamicMessage` — CORRECT

`ReadFieldDictionaryOption` (`option_reader.cpp:124-158`):

- `DynamicMessageFactory factory;` (`:143`) is declared **before** `std::unique_ptr<Message> dyn`
  (`:144`), so `dyn` is destroyed first. Correct.
- `refl->GetMessage(*dyn, ext, &factory)` (`:156`) returns a reference owned by `dyn` (or, if unset,
  by `factory`) — both alive across the `DecodeDictionaryOptions` call.
- `DecodeDictionaryOptions` copies only an `enum class` and a `bool` into `*out`; the only borrowed
  object is `evd->name()` (a `const std::string&` into the descriptor pool, which outlives
  everything) and it is consumed inside the call. **Nothing borrowed escapes.** No `Descriptor*`,
  `string_view` or reference is returned.
- Null-deref safety: `field == nullptr` guarded (`:126`); `ext->containing_type()` is never null for
  an extension `FieldDescriptor`; `index_type->enum_type()` is guarded by the
  `cpp_type() == CPPTYPE_ENUM` test; `FindValueByNumber` may return null and **is** checked
  (`:80-82`), leaving the default kind intact.
- A genuinely good detail worth calling out: building `dyn` from `ext->containing_type()` (rather
  than from any other `FieldOptions` descriptor) is what makes `Reflection::HasField(*dyn, ext)`
  safe — protobuf `CHECK`s `field->containing_type() == descriptor_`, and here they are the same
  object by construction. Worth keeping a comment on, because a "cleaner-looking" refactor that
  fetched the descriptor from the pool by name could break it into an abort.

### 3. The presence probe — cannot over-read; foreign reachability is narrow but real

`HasLengthDelimitedDictionaryField` (`option_reader.cpp:98-107`) walks an **already-parsed**
`UnknownFieldSet`, indexes `field(i)` strictly under `field_count()`, and never touches
`uf.length_delimited()`. **No over-read or mis-parse of a truncated buffer is possible** — the
truncation was already absorbed by protobuf's own unknown-field parser, which is why the payload is
there as a well-formed length-delimited *record* even though its *contents* are garbage.

Can a foreign option at 50001 reach it? Only through a narrow door, and it is not fully closed:

- The probe is reached only after `ResolveDictionaryExtension` succeeded, i.e. the pool declares
  `fletcher.dictionary` as a singular message-typed `FieldOptions` extension at 50001. Protobuf
  refuses two extensions of the same extendee at the same number in one pool, so a *protoc-compiled*
  foreign option at 50001 cannot coexist with `fletcher.dictionary` in that pool. Good.
- But the probe only matches on `number == 50001 && type == LENGTH_DELIMITED` in the **untouched**
  UnknownFieldSet, so any hand-built / injected descriptor, or any options blob whose whole-message
  reparse fails for an unrelated reason, can drive it. See P2-5 and SF-3 below.

### 4. Enum decode by symbol name — safe, silent

`IndexKindFromSymbol` (`option_reader.cpp:58-65`) is total over `std::string` and returns a valid
`ir::DictionaryIndexKind` for every input, so **no out-of-range value is reachable**. Absent →
`GetEnumValue` returns the field default `0` = `DICTIONARY_INDEX_UNSPECIFIED` → INT32 (correct per
spec). Undeclared *number*: `fletcher/options.proto` is `syntax = "proto2"` (confirmed at
`options.proto:6`), so the enum is **closed** and an undeclared number is pushed into the
submessage's `UnknownFieldSet` before it ever reaches `GetEnumValue` — the comment at `:77-79` is
accurate. Even if it did arrive (open enum in a forked pool), `FindValueByNumber` returns null and
the guard holds. Unknown *symbol* → INT32, silently; see N-18.

The forward-compatibility claim in the header ("a consumer pinning a version that renumbered the
enum values is still read correctly") is genuinely delivered, because number→symbol resolution goes
through the **pool's** enum descriptor, not the plugin's.

### 5. Ordinary correctness / error propagation

The reader never throws by construction: no `throw`, no `.at()`, no `ValueOrThrow`, and
`ReadFieldDictionaryOption` is documented and implemented as total (`nullopt` or a value). It is
called from `ir::BaseFacts`/`BuildFlattenedSingular`, which have no error channel — consistent.
`std::bad_alloc` from the `SerializeAsString` temporary is the only theoretical escape and is not
something the plugin can meaningfully report anyway.

`ByteSizeLong`/`SerializeAsString` failure paths: see P2-8 and N-17.

---

## Findings

### should-fix

**SF-1 — Inner-declared `(fletcher.dictionary)` is silently dropped on the flattened-*repeated*
path.** *(confidence: high)*
`BuildFlattenedRepeated` builds every one of its nodes from `BaseFacts(field)` where `field` is the
**outer** field (`ir.cpp:346`, `:350`, `:364`, `:368`, `:384`), and there is **no mirror** of the
`BuildFlattenedSingular` propagation added at `ir.cpp:296-302`. The inner field's own option is never
read on this path. So:

```proto
message W { option (fletcher.flatten) = true;
            string v = 1 [(fletcher.dictionary) = { index_type: DICTIONARY_INDEX_INT8 }]; }
message M { W w = 1;             // -> dictionary INT8 (BuildFlattenedSingular -> BuildFieldIr(inner))
            repeated W xs = 2; } // -> NO dictionary at all
```

The singular and repeated spellings of the same wrapper disagree. Unlike the D4b `flatten_field`
hole, this is neither documented in the spec nor pinned by a test, so DICT-2 can be built on the
wrong assumption. Fix by propagating, or record it next to D4b in the spec **and** pin it with a
test.

**SF-2 — Which node carries the fact is inconsistent between flatten and non-flatten repeated
shapes.** *(confidence: high)*
`BuildFlattenedSingular` writes the propagated fact only onto the node it returns (`ir.cpp:298-300`).
If the wrapper's single inner field is `repeated`, `BuildFieldIr(inner)` returns a LIST, so the
outer field's dictionary lands on the **LIST node only** — whereas `BuildRepeatedScalarOrEnum`
writes `BaseFacts(field)` onto **both** the LIST (`ir.cpp:419`) and its SCALAR element (`:416`). A
DICT-2 consumer that reads the element's facts (a natural choice — the element is what gets
dictionary-encoded) sees the option in one shape and not the other. `BaseFacts`'s new comment
("consumers must gate on the TOP-LEVEL node's kind") states a convention but the propagation code
does not follow it uniformly. At minimum document which node is authoritative and make the
propagation match.

**SF-3 — The foreign-option-at-50001 case and the malformed-payload case do not discriminate each
other.** *(confidence: high)*
The `"FOREIGN message-typed option at #50001 -> nullopt"` sub-case builds `kForeignSchema` in a pool
that does **not** contain `fletcher/options.proto`. `ResolveDictionaryExtension` therefore fails at
its very first line (`FindExtensionByName("fletcher.dictionary")` → null) — **byte-for-byte the same
code path** as the immediately preceding `"extension NOT in the pool -> nullopt"` sub-case. The test
proves the pool-declaration gate twice and never reaches the probe. The interesting shape — a pool
that *does* know `fletcher.dictionary`, plus a field whose 50001 blob is foreign/unparseable, i.e.
exactly where the probe fires and where a false positive would fabricate a column — is untested.
Because the `truncated` case (which *does* fire the probe) lives in a different pool from the foreign
case, **neither sub-case constrains the probe's discrimination**, which was the stated purpose of
having both.

Suggested: add a case to `ipool` (which has the extension) with a blob that fails the whole-message
reparse and is *not* a Fletcher payload, and assert the chosen behaviour explicitly. Also record in
the spec that R1 ("a foreign option cannot fabricate a dictionary") is defended at
**pool-declaration** granularity, not per-field: once the pool declares the extension, the probe
trusts a bare `50001 + LENGTH_DELIMITED`.

**SF-4 — No test coverage for `repeated`, map, oneof or struct-member fields carrying the option.**
*(confidence: high)*
`kDictSchema` has only singular scalar / wrapper / flatten shapes. Yet `BaseFacts`'s new comment
(`ir.cpp:39-42`) makes a load-bearing claim about the repeated case — "BaseFacts runs for both the
LIST node and its element node with the SAME descriptor, so both carry it" — that **nothing
asserts**. Given DICT-2 will gate on precisely this, and given SF-1/SF-2, this is the highest-value
missing coverage in the change. Add at least: `repeated string` with the option (assert LIST +
element facts), a `map<string,string>` field with the option (assert where it lands), a struct member
with the option (via `BuildStructVariant` recursion), and the SF-1 flattened-repeated shape.

### P2

**P2-5 — The probe downgrades a perfectly valid dictionary payload when an *unrelated* option in the
same blob is corrupt.** *(confidence: high mechanism, low reachability)*
`ReparseOptionsWithPool` parses the **whole** `FieldOptions`. Any failure anywhere in that blob —
including in some third-party option that has nothing to do with 50001 — routes to
`HasLengthDelimitedDictionaryField`, which returns `DictionaryOption{}`, discarding a fully valid
`index_type`/`ordered` that is sitting right there. Cheap improvement: on whole-message failure,
pull the 50001 length-delimited payload out of the `UnknownFieldSet` and parse *it* as
`ext->message_type()` through the same factory, falling back to bare defaults only if that fails
too. That turns "declared → defaults" into "declared → correctly decoded" for the one case where the
information is available, without reintroducing a payload walker. Spec §7 says these paths are
unreachable from protoc-compiled input, which is why this is P2 and not higher.

**P2-6 — A fresh `DynamicMessageFactory` + `GetPrototype(FieldOptions)` per option-carrying field,
several times per field.** *(confidence: high that the cost exists, medium that it matters)*
`GetPrototype` on `google.protobuf.FieldOptions` builds a dynamic layout for it and its transitive
message-typed fields; that is not a trivial per-field cost. It runs once per `BaseFacts`, so **twice
for every repeated field** (element at `ir.cpp:416` + list node at `:419`; likewise `:346`/`:350`,
`:364`/`:368`), once more for the outer field in `BuildFlattenedSingular` (`:297`), and again on
every `HasFieldDictionary` call. The `ByteSizeLong() == 0` fast path (`option_reader.cpp:134`) bounds
the blast radius to fields that carry *some* option, which is the right call and keeps the common
case free. If this ever shows up, the fix is a cache keyed by `(pool, options-message address)` whose
lifetime is scoped to one IR build — not `static` — which would preserve the cross-pool
address-reuse safety the comment at `:139-142` is protecting.

**P2-7 — The pool the gate consults is not the pool the reparse resolves extensions through.**
*(confidence: high mechanism, low reachability)*
`ResolveDictionaryExtension` looks the extension up in `field->file()->pool()`
(`option_reader.cpp:136`), but the default-constructed `DynamicMessageFactory` makes the dynamic
message's reflection resolve extensions through `pool_options_descriptor->file()->pool()` — i.e. the
pool that owns the **`FieldOptions` descriptor**. These are the same object only for a *flat* pool.
In a layered pool (`descriptor.proto` in an underlay, `fletcher/options.proto` on top) the gate would
pass, the reparse would leave the blob in unknowns, `HasField` would be false, and the reader would
return **absent** — the outcome the design itself calls the worst one — with the probe never firing.
Verified not reachable today: the plugin uses protoc's own flat request pool (no `DescriptorPool`
construction anywhere in `protoc/src/`), and `ProtoTextPool` seeds `descriptor.proto` into the same
flat pool. Worth (a) stating the flat-pool precondition on `ReparseOptionsWithPool` in the header,
and/or (b) hardening: if the whole parse *succeeded* but `dyn`'s own `UnknownFieldSet` still holds a
length-delimited field at 50001, the extension was not resolved — treat that as "declared".

**P2-8 — `SerializeAsString()` swallows serialization failure.** *(confidence: high mechanism,
negligible reachability; pre-existing from #121)*
`option_reader.cpp:120` uses `opts.SerializeAsString()`, which on failure returns a partial or empty
string with no signal. An empty string then **parses successfully** into an empty message, so
`HasField` is false and the caller reports "no dictionary" — bypassing the fail-soft probe entirely.
`SerializeToString(&s)` with a checked return (→ `nullptr`, which routes DICT-1 to the probe) is a
two-line change and strictly better. Since the review brief explicitly asked about this path: it is
currently unchecked in both consumers.

**P2-9 — `ReparseOptionsWithPool` documents lifetime but not nullability.** *(confidence: high)*
A null `pool_options_descriptor` or `factory` is a hard crash inside protobuf, not a `nullptr`
return. It is now a public cross-module API with two callers in different TUs. Both currently pass
`ext->containing_type()`, which cannot be null — but the contract should say so, or assert it.

**P2-10 — Strictness asymmetry with the neighbouring flatten reader.** *(confidence: high)*
`option_reader.cpp:31-34` justifies the name-based gate with "trusting the number alone would let a
foreign option fabricate a dictionary column nobody declared" — but `type_mapper.cpp:44-52`
(`FindBoolOption`) still matches `(fletcher.flatten)` / `(fletcher.flatten_field)` by **bare number
50000**, and `test_schema_visitor.cpp:398` documents that as deliberate. So the exact threat model
the new comment cites remains unmitigated one function away: a foreign varint at 50000 can still
fabricate a flatten. Not introduced by DICT-1 and not DICT-1's job to fix, but the comment's
rationale is only half-true tree-wide; add a cross-reference or file an issue so the inconsistency is
a decision rather than an accident.

### nit

**N-11 — Layering inversion in the new header.** `option_reader.hpp:30` includes `ir.hpp` solely for
`ir::DictionaryIndexKind`, so the low-level option reader now depends on the IR layer, and
`option_metadata.cpp` — which wants only `ReparseOptionsWithPool` — transitively pulls in `ir.hpp`.
Consider splitting the reparse primitive into its own small header, or hosting
`DictionaryIndexKind` somewhere both can see cheaply.

**N-12 — `proto_text_pool.hpp` is the third near-verbatim copy of this helper.**
`test_option_metadata.cpp:155-195` (`CollectErrors`/`FixturePool`) and
`test_schema_visitor.cpp:404-435` (`OptCollectErrors`/`OptFixturePool`) are the same code and were
not migrated. The header presents itself as the shared one; either migrate the two existing copies or
say explicitly that migration is deferred, so the next reader does not add a fourth.

**N-13 — The duplicate-extension claim checks out, but the seed uses `EXPECT_NE`.** The
"POOL TRAP" reasoning in `proto_text_pool.hpp:17-22` is correct: `DescriptorPool pool_` is a
per-instance member, and protobuf's duplicate-extension-number check is per-pool, so separate
`ProtoTextPool` instances declaring `flatten_field = 50000` do not collide. The all-`inline`
requirement is satisfied (only `AddFletcherOptions` is a free function and it is `inline`; the two
classes' member functions are implicitly inline). Minor: the constructor's `EXPECT_NE` for the
`descriptor.proto` seed lets a failed seed continue into a cascade of confusing `BuildFile` failures.
`ASSERT_*` is not usable in a constructor, so consider a `bool ok_` accessor callers can
`ASSERT_TRUE` on.

**N-14 — `ir.hpp`'s pre-existing `DictionaryModifier` enum (line 103) is dead code**, and DICT-1 now
adds a second, parallel dictionary representation (`DictionaryIndexKind` +
`FieldFacts::dictionary_index_kind`) directly below it. Nothing in `protoc/` references
`DictionaryModifier`. Delete it so DICT-2 cannot pick the wrong carrier.

**N-15 — The `bad_enum` sub-case is non-discriminating on its own.** `08 09` asserts INT32, which is
also exactly what you would get if the entire `index_type` branch of `DecodeDictionaryOptions` were
deleted. It is only meaningful because `i16`/`i16_ordered` cover the positive path in the same loop —
worth a comment noting the dependency, since a future refactor could delete the positive cases and
leave a green but vacuous `bad_enum`.

**N-16 — `HasFieldDictionary` is a dead API today.** It performs a full serialize/reparse/factory
build to return a bool, its own doc-comment tells callers to prefer `facts.dictionary`, and its only
in-tree callers are the tests. Consider deferring it to the item that actually needs a
raw-descriptor validation walk.

**N-17 — `ByteSizeLong()` on the shared default instance.** The fast path
(`option_reader.cpp:134`) mutates protobuf's shared `FieldOptions` default instance's cached-size
field. Benign as the comment says (relaxed atomic, and the plugin is single-threaded), and the
obvious alternative (`&field->options() == &FieldOptions::default_instance()`) is arguably more
fragile. Recording only so a future reviewer does not re-derive it. Also confirmed correct on the
substantive point: `ByteSizeLong()` **does** count unknown fields, so the fast path cannot skip a
field that carries only an un-linked custom option, and `= {}` costs ≥4 bytes so it cannot be
mistaken for absence.

**N-18 — Unknown enum symbols degrade silently.** `IndexKindFromSymbol` maps both
`DICTIONARY_INDEX_UNSPECIFIED` and any unrecognised symbol to INT32, so a future
`DICTIONARY_INDEX_UINT8` becomes int32 with no diagnostic anywhere. Documented and consistent with
the fail-soft contract; flag it for the validation item that owns the error channel.

**N-19 — Housekeeping, outside the diff.** An empty untracked directory literally named
`C:UsersCTMsourceprototypesFletcherplansreviews` sits at the repo root (a mangled path from an
earlier run). Worth deleting before something lands inside it.

---

## Things checked and found correct (recorded so they are not re-litigated)

- `options.proto`: 50001 does not collide with the two existing 50000 extensions (different
  extendee for `flatten`, and `flatten_field`/`dictionary` differ in number); `proto2` syntax is
  required for `extend` and is present; the enum is closed, which the reader's comments correctly
  rely on. `docs/fletcher-options.md`'s registry table is updated in the same change.
- The `= {}` / zero-length-submessage case: presence-not-truthiness is implemented via
  `Reflection::HasField` on the re-parsed extension (`option_reader.cpp:153`), and the injected
  `empty` sub-case (zero-length payload, `present = true`) genuinely pins it.
- `ordered: true` **does** exercise the parse. `fletcher/options.proto` is proto2 and the sub-field is
  `optional bool`, so explicit-presence applies; and the injected `ordered` case (`10 01`) plus
  `i16_ordered` (`08 02 10 01`) pin the byte-level decode independently of the source-text route. The
  brief's proto3-omits-defaulted-false worry does not bite, and the test comment's reasoning is
  sound.
- The `varint` sub-case correctly pins that a wire-type mismatch on a *known* field is skipped into
  unknowns, so the parse succeeds, `HasField` is false, and the probe is never reached → absent. This
  is the one sub-case that meaningfully separates "probe reached" from "probe not reached".
- The `truncated` sub-case genuinely reaches the probe: a length-1 payload of `08` fails the
  submessage varint parse, which propagates out of `ParseFromString`.
- Leaf-wins on `Holder.on_both` is genuinely discriminating (INT64 outer vs INT8 inner → asserts
  INT8), and `on_wrapper` genuinely exercises the new propagation (INT64 + `ordered`).
- The D4b `Rec.p` sub-case is genuinely pinning: `dict_count == 1` plus `EXPECT_FALSE` on `a`/`b`
  would both fail if the hole were closed, so a future fix cannot pass silently.
- `BuildStructVariant` (`ir.cpp:510-523`) recurses through `BuildFieldIr`, so struct members do pick
  up their own dictionary facts — no gap there (though it is untested; see SF-4).
- The `ProtoTextPool` import graph resolves: `fletcher/options.proto` imports
  `google/protobuf/descriptor.proto`, seeded by the constructor under its real name;
  `kDictSchema` additionally imports `wrappers.proto`, added via `AddLinked`; `kForeignSchema` imports
  only `descriptor.proto`.
- `io::ErrorCollector::AddError(int, int, const std::string&)` and `Parser::RecordErrorsTo` match the
  signatures already used by two existing test files, so the protobuf version in use supports them —
  no build break from the new header.

---

# Round 2 — delta re-review (2026-08-28)

Same base (`6fa5085`), re-read `git diff --cached HEAD`. Build treated as verified per the
coordinator (94/94 green, `test_option_metadata.cpp` at zero lines of diff — confirmed: that file is
absent from the diffstat, so the shared-primitive extraction is regression-proved by an unmodified
33-test consumer).

## Verdict on the delta

**Nothing blocking. Nothing should-fix.** All four SF items and all six P2s are addressed, three of
them by a *documented deferral with a pinning test* rather than a code change — which is the right
call in each case and is materially better than the silent gaps I found in round 1. What remains is
**2 P2 and 5 nit**, none of which should hold DICT-1. **Close it.**

Counts (delta only): **blocking 0 · should-fix 0 · P2 2 · nit 5**.

## 1. `ApplyDictionaryFacts` (SF-2) — correct

`ir.cpp:283-294`. Verified against the coordinator's specific questions.

- **Terminates.** The recursion follows only the `LIST -> element` chain of a `unique_ptr`-owned tree
  built bottom-up; there are no back-edges, and recursive proto messages are rejected upstream by
  `IsRecursive`. Depth is bounded by proto nesting depth.
- **Every node kind it can meet is handled.** SCALAR / STRUCT / MAP / UNSUPPORTED: writes and stops —
  correct, because a STRUCT's children and a MAP's key/value are built from *different*
  `FieldDescriptor`s (the map entry's own fields), so the fact must **not** be pushed there, and the
  new map sub-case asserts exactly that. LIST: writes and recurses. LIST-of-LIST: recurses per level.
  The `std::get_if` guard also survives a hypothetical kind/variant mismatch instead of throwing.
- **No double-write.** The `if (!node.facts.dictionary)` guard is per node, so leaf-wins holds at
  every level, and the caller's outer guard (`ir.cpp:322`) makes the top-level check redundant but
  harmless — it is genuinely needed for the recursive calls.
- **The recursion is reached on a path that matters, not just on a synthetic one.**
  `Holder.on_rep_wrapper` (`WrapRepeated` = flatten wrapper over `repeated string values`) drives
  `BuildFlattenedSingular -> BuildFieldIr(inner) -> BuildRepeatedScalarOrEnum` -> `LIST(SCALAR)` with
  both facts false -> propagation -> `ApplyDictionaryFacts` sets the LIST *and* recurses into the
  element. Deleting the `if (node.kind == NodeKind::LIST)` block fails precisely
  `list.element->facts.dictionary` in that sub-case and nothing else. The implementer's mutation
  claim checks out, and the mutated behaviour is exactly the SF-2 defect.
- **Nothing is written where DICT-3 will not look**, with one exception recorded as P2-20 below.

## 2. The SF-3 test restructure — the byte-identity assertion is real and load-bearing

This is the change I was most sceptical of, and it holds up.

- **The bytes really are identical, and it is asserted rather than assumed.**
  `zz.Foreign{ x: 2, y: true }` with `Foreign { int32 x = 1; bool y = 2; }` serializes to
  `08 02 10 01`; `DictionaryOptions{ index_type: DICTIONARY_INDEX_INT16 (=2), ordered: true }`
  serializes to the same four bytes. `UnknownPayloadAt(fd, N)` pulls the actual payload off the
  descriptor and `EXPECT_EQ`s it against `kI16OrderedBytes` — **the same constant used to inject the
  `i16_ordered` row that decodes to `{INT16, ordered}`**. So the fixture is self-checking: if
  protobuf ever ordered or encoded those sub-fields differently, the byte-identity assertion fails
  loudly instead of the pair quietly ceasing to mean anything. That was my main worry and it is
  closed. (Hex-escape greediness is not a hazard here — every escape is immediately followed by a
  backslash — and no byte is `0x00`, so `std::string(ptr, 4)` is exact.)
- **The two halves are now genuinely different scenarios**, not the same first-gate exit twice:
  - *half 1* — `ForeignSchema(60200)` compiled **into `ipool`, which also declares
    `fletcher.dictionary` at 50001**. Here `ResolveDictionaryExtension` **succeeds**, the reparse
    **succeeds** (the pool knows `zz.foreign`, so it parses as a real extension), and the answer is
    absent solely because `HasField(*dyn, ext@50001)` is false. This is the strongest available form
    of the no-false-positive property and did not exist before.
  - *half 2* — `ForeignSchema(50001)` in a pool without `fletcher/options.proto`, the original
    first-gate exit, now with the byte-identity assertion attached and with an explanatory comment
    stating *why* it can only be tested in a separate pool (protobuf refuses two extensions of one
    extendee at one number).
- **Probe-reached and probe-not-reached now sit side by side in one pool, and the boundary is
  observable in both directions.** `truncated` (`08`) and `foreign_corrupt` (`1a 05 01` — field 3,
  length 5, one byte available -> submessage parse fails -> whole reparse fails) both flip
  `present: true -> false` if the probe is removed. `varint` and `wrong_number` both flip
  `false -> true` if the probe were widened to varints or to any number. So the probe's behaviour
  boundary is pinned from both sides inside the Fletcher-aware pool. See P2-21 for the one claim in
  this block that is *not* observable.
- **The residual false positive is now explicit rather than hidden.** `foreign_corrupt` documents and
  pins that foreign, unparseable bytes at exactly 50001 in a Fletcher-aware pool **do** yield
  "declared, defaults". That is the pool-declaration-granularity limit, and it is recorded in three
  places (`option_reader.cpp:170-177`, spec section 7 "no reader can do better", and this test row).
  Round 1 asked for exactly this and got it.
- `wrong_number` is slightly over-determined (absent for two independent reasons: `HasField` false
  *and* the probe's number check), but it is correct and cheap. Not worth changing.

## 3. SF-4's new coverage — asserts the real shape, not the emitted shape

Checked each against the builder that produces it:

- `Shapes.tags` (`repeated string` + option) -> `BuildRepeatedScalarOrEnum` writes `BaseFacts(field)`
  to both the LIST (`ir.cpp:419`) and the element (`:416`). Test asserts LIST kind, both facts, and
  both index kinds. **Real shape.**
- `Shapes.labels` (`map<string,string>` + option) -> `BuildMapNode` writes `BaseFacts(field)` to the
  MAP node and `BaseFacts(key_fd)` / `BaseFacts(val_fd)` to key/value, which carry no option. Test
  asserts MAP true, key/value **false**. **Real shape**, and it usefully pins the negative so a
  future consumer cannot start looking for the fact on the value node.
- `Shapes.oc` (oneof member) -> `BuildFieldIr` returns `MakeUnsupported` for any
  `real_containing_oneof()` field (`ir.cpp:571-575`), and `MakeUnsupported` calls `BaseFacts`, so the
  fact is carried on an UNSUPPORTED node. Test asserts `UNSUPPORTED` + `in_real_oneof` + the fact.
  **Real, pre-existing shape** — not "whatever the code emits"; the UNSUPPORTED-for-oneof rule is
  independent of DICT-1 and long-standing.
- `MemberHolder.m` -> `BuildStructVariant` recurses through `BuildFieldIr` per member, so each member
  carries its own declaration and the holder field carries none. Test asserts all three. **Real
  shape.** (Positional indexing nit at N-23.)
- `Holder.rep_outer_declared` / `rep_inner_declared` (SF-1 pin) -> both go through
  `BuildFlattenedRepeated`'s `inner->type() != TYPE_MESSAGE` branch, where leaf and outer both take
  `BaseFacts(outer field)`. Outer-declared -> both true; inner-declared -> both false. Test asserts
  exactly that **and** contrasts it against the singular spelling in the same block. This is a good
  pin: closing the gap flips two `EXPECT_FALSE`s, so it cannot be closed accidentally.

## 4. P2 fixes — all three correct

- **P2-8** `option_reader.cpp:161-164`: `SerializeToString(&bytes)` checked -> `nullptr`, with the
  reasoning (empty string parses successfully and would report "absent" instead of routing to the
  fail-soft path) written down. Correct, and it now feeds the probe rather than bypassing it.
- **P2-9** `option_reader.cpp:155`: explicit null guard on both pointers returning `nullptr`, plus
  the contract in the header. Correct.
- **P2-7** `option_reader.hpp:38-49`: the flat-pool precondition is stated precisely, including the
  failure mode ("the extension would silently stay in the dynamic message's unknown fields and read
  as absent"). Documentation-only, which is the right weight for a limitation that is unreachable
  from protoc's own request pool.
- Bonus: P2-10 (the `FindBoolOption`-by-bare-number asymmetry) is recorded at
  `option_reader.cpp:36-45` with an explicit out-of-scope rationale, and N-18 at `:70-73`. Both are
  now decisions rather than accidents, which is all I asked for.

## 5. The deliberate skips — I agree all four are safely deferrable

- **P2-5 (decode the 50001 payload after a whole-message parse failure): agreed, defer.** One
  correction to the stated rationale, for the record: my suggestion was to re-parse the extracted
  payload as `ext->message_type()` *through the same DynamicMessage mechanism*, so it would **not**
  reconstitute the retired unknown-field walker — the letter of "the probe decodes nothing" would
  break, the spirit would not. The reason this is safely deferrable is the other one you give: no
  protoc-compiled input can reach it, so the change would add a second parse path with no
  test-reachable user, and the granularity limit is now recorded in code and in spec section 7/7.1.
  Keep the skip; consider trimming the "would contradict the contract" framing to "no reachable input
  needs it" if the note is ever revisited.
- **N-11 (header split): agreed.** Purely cosmetic layering; `option_metadata.cpp` transitively
  including `ir.hpp` costs nothing observable.
- **N-14 (dead `ir::DictionaryModifier`): agreed, but keep it on DICT-2's list explicitly.** It is
  zero-risk today and genuinely a DICT-2/RIR concern, but it is the one leftover that could actively
  mislead: a DICT-2 implementer looking for "the dictionary carrier" finds two candidates one screen
  apart, and only one is live. Deleting it is a one-line change whenever DICT-2 opens.
- **N-16 (`HasFieldDictionary`): agreed, and this one is now properly justified.** Spec section 7.1
  item 1 states the `flatten_field` rejection "must be a front-end **descriptor** walk, not a
  projection-level check" — that is a named, documented consumer, so the function is pre-provisioned
  rather than dead. Withdrawn.

## Remaining findings

### P2

**P2-20 — Intermediate LIST levels of the GIR-10 nested-list shapes do not carry the fact, so the
"every node built from that field carries it" invariant is not literally true.** *(confidence: high;
no user-visible consequence today)*
`BuildFlattenedRepeated` writes `BaseFacts(field)` to the **leaf** and to the **outermost** list node
only (`ir.cpp:346`/`:350` and `:364`/`:368`); the list levels created by the intervening
`for (...) node = MakeListOf(std::move(node));` keep default facts. So
`repeated ScalarListWrapper xs = 1 [(fletcher.dictionary) = {}]` yields
`List(fact) -> List(NO fact) -> Scalar(fact)`. Two consequences:

1. The placement rule asserted by `BaseFacts`' new comment (`ir.cpp:39-51`) and by spec section 7.1's
   "**every IR node built from that field carries it**" is false for those middle levels. The PM is
   closing partly on the strength of that invariant, so it should either be qualified or made true.
2. It produces the *mirror* asymmetry to the one SF-2 just fixed: `ApplyDictionaryFacts` sets **every**
   level (it recurses), while the direct `BaseFacts` path skips the middles — so the flatten-propagated
   and directly-built nested lists now disagree in the opposite direction from round 1. Also, because
   the caller's guard at `ir.cpp:322` tests only the top node, a nested list whose outermost level
   already carries the fact short-circuits propagation entirely and the middle level stays false.

Neither is user-visible: the top-level node is a LIST, which DICT-2 rejects as non-SCALAR. Cheapest
resolution is one qualifying clause in spec section 7.1 ("...every node except the intermediate list
levels of the nested-list shapes, which DICT-2 rejects as non-scalar anyway"); the alternative is
setting facts inside the `MakeListOf` loops, which touches the GIR-10 shapes and is not worth it now.

**P2-21 — `foreign_parseable`'s "probe not reached" claim is not observable from the assertions.**
*(confidence: high)*
`foreign_corrupt` and `foreign_parseable` both assert `{present: true, INT32, ordered: false}`, so the
two rows are observationally identical: if the probe fired for `foreign_parseable` too, nothing in the
test would change. The comment at the injection site ("parses fine ... present with defaults, WITHOUT
the probe") is therefore a claim about an internal path the test cannot see. The probe's *boundary* is
still pinned in both directions (see section 2 above), so this is not a coverage hole so much as an
over-claiming comment. One-line fix that makes it real: inject `08 02 1a 03 61 62 63` instead — the
success path then decodes `{INT16}` while the probe path would yield `{INT32}`, making the two
mechanisms observationally distinct with the same "foreign junk present" shape. Worth doing whenever
this fixture is next touched; not worth reopening DICT-1.

### nit

**N-22 — `ApplyDictionaryFacts` mutates through a `const`-declared pointer.** `ir.cpp:290` binds
`const ListNode* list`, then calls `ApplyDictionaryFacts(*list->element, d)`. This compiles and
mutates because `unique_ptr::operator*` is const-qualified and returns a non-const `T&` — the
pointer's constness does not reach the pointee. It is well-defined (the root `IrNode&` is non-const),
but it reads as const-correct when it is deliberately not. Drop the `const` so the mutation is
visible at the call site.

**N-23 — The struct-member sub-case indexes `st.fields[0]` / `st.fields[1]` positionally.**
`BuildStructVariant` iterates `msg->field(i)` in declaration order so this holds, but the assertion
would silently start testing the wrong member if `Member`'s fields were reordered. One
`EXPECT_EQ(st.fields[0].name, "dict")` would make the mis-target loud.

**N-24 — `ApplyDictionaryFacts`' doc comment says "today that is a LIST's element" but the function
recurses arbitrarily deep.** Accurate about today's reachable shapes, slightly under-describes the
code. Trivial.

**N-25 — Two round-1 nits remain open by design, both now with rationale in-tree:** N-12 (third copy
of the pool fixture) is documented as a deliberate deferral at `proto_text_pool.hpp:23-29` with the
regression-proof argument, which is a *better* answer than migrating would have been for this item.
N-17 (`ByteSizeLong` on the shared default instance) is unchanged and remains benign. No action.

**N-26 — Housekeeping (repeat of N-19):** the empty untracked directory
`C:UsersCTMsourceprototypesFletcherplansreviews` is still at the repo root.

## Round-1 items closed by this delta

| Round 1 | Disposition | Verified |
|---|---|---|
| SF-1 | Deferred to DICT-2, documented (`ir.cpp:335-348`, spec 7.1 item 2) + pinned by two sub-cases | yes |
| SF-2 | Fixed — `ApplyDictionaryFacts` recursion + placement rule documented + pinned | yes |
| SF-3 | Fixed — parameterised foreign fixture at 60200/50001, byte-identity asserted, probe boundary pinned in one pool | yes |
| SF-4 | Fixed — repeated / map / oneof / struct-member sub-cases, all asserting real shapes | yes |
| P2-5 | Skipped with recorded rationale | agreed |
| P2-6 | Not addressed (per-call factory cost); no note added. Still acceptable — the option-less fast path bounds it | acceptable |
| P2-7 | Documented precondition on `ReparseOptionsWithPool` | yes |
| P2-8 | Fixed — checked `SerializeToString` | yes |
| P2-9 | Fixed — null guard + contract | yes |
| P2-10 | Recorded as a known decision (`option_reader.cpp:36-45`) | yes |
| N-11, N-14, N-16 | Deferred / withdrawn — see section 5 | agreed |
| N-12, N-13, N-15, N-18 | Documented or fixed (`seeded()`, "Do not delete them" note, N-18 comment) | yes |
| N-17, N-19 | Open, benign | acceptable |
