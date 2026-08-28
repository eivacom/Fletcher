# DICT-1 — step-4a architecture-conformance review (adversarial)

Round **DICT** · Item **DICT-1** · Branch `feature/dictionary-option`
Diff base: `HEAD` = `6fa5085`, staged (`git diff --cached HEAD`)
Design: `plans/DICT-1-option-surface-reader.md` (revised body, cycle-2 APPROVE)
Locked: `plans/DICT-locked-decisions.md` #1 #2 #3 #4 #5 #10 (+ GIR locked #7)
Spec: `docs/dictionary-option-spec.md` §2, §7
Reviewer: independent agent, fresh context. Empirically verified (build + run +
two mutation probes), not taken on the suite's word.

## Verdict: **CONFORMS**

No blocking conformance item. Two accepted deviations from the design's *letter*
(A1, A2), four non-blocking observations (O1–O4). One bookkeeping obligation
attaches to A1.

Evidence base:

* Built `fletcher_proto_plugin_tests` (MSVC Release, existing `protoc/build`).
* Full suite: **92 passed / 93**, 1 pre-existing skip (`SchemaVisitor.CaptureGoldens`).
* `TypeMapperTest.ReadsDictionaryOption` green.
* Two deliberate mutations applied and **restored** (`git checkout --` from the
  index; `git status` re-verified identical, suite re-verified green).
* `clang-format 18.1.3 --dry-run -Werror` clean on every changed/added C++ file.
* SPDX + copyright headers present on all three new files.

---

## 1. Shared-primitive extraction — behaviour genuinely unchanged

The highest-risk part. Read at source level, not inferred from the suite.

**Round-trip is byte-for-byte the same code.** `HEAD`'s
`OptionMetadataResolver::Impl::ParsedOptions` body was:

```cpp
std::unique_ptr<Message> dyn(factory.GetPrototype(pool_opts_desc)->New());
if (!dyn->ParseFromString(opts.SerializeAsString())) return nullptr;
```

`ReparseOptionsWithPool` (`protoc/src/option_reader.cpp`) is the identical two
statements with `factory` reached through a pointer, returning the `unique_ptr`.
The caller's `if (!dyn) return nullptr;` reproduces the old failure path exactly,
including the two behaviours easiest to lose in an extraction:

* **the `cache.find` hit is retained verbatim** and still precedes everything, so
  the shared default-options instance still parses **once per resolver** (a #121
  behaviour, not an optimisation);
* **nothing is cached on parse failure** — as before, so a failing options blob is
  re-attempted rather than memoised as `nullptr`.

The only lifetime change is *where* the failed `dyn` destructs (inside the new
function rather than in `ParsedOptions`); in both cases the caller's `factory`
member is still alive, so this is not observable.

**Factory-before-cache member order — verified by reading the declarations, as
instructed.** Current file:

* `protoc/src/option_metadata.cpp:243` — `mutable google::protobuf::DynamicMessageFactory factory;`
* `protoc/src/option_metadata.cpp:253` — `mutable std::map<const Message*, std::unique_ptr<Message>> cache;`

`factory` is still declared **above** `cache`, so members destruct cache-then-
factory and the cached prototypes still die before the factory that minted them.
The design required this be pinned by a comment; the comment is present and says
the right thing ("Swapping these two lines is a silent use-after-free at resolver
teardown, not a reordering"). The pre-diff line numbers cited in the brief
(`:241` / `:245`) shifted to `:243` / `:253` purely because that comment was
inserted — the *order* is unchanged. **Conforms.**

**Regression gate honoured.** `protoc/tests/test_option_metadata.cpp` is **not in
the diff** and the 33 #121 tests are green: `OptionMetadataTest` 17 +
`EscapeCppStringLiteralTest` 9 + `MetadataRuleParseTest` 4 +
`MetadataRuleCompileTest` 3 = 33. That is the design's stated no-drift proof, met
as stated.

Also checked, no finding: `ReparseOptionsWithPool` is passed
`ext->containing_type()` (the **pool's** `FieldOptions`, not the linked one), so
`ext` is a valid extension descriptor for the produced `DynamicMessage` — the same
invariant `option_metadata` already relied on.

## 2. R1 no-false-positive — holds, and the fail-soft floor was NOT traded away

**Structural proof in the code as built** (`ReadFieldDictionaryOption`):

1. `opts.ByteSizeLong() == 0` → `nullopt` (fast path).
2. `ResolveDictionaryExtension(field->file()->pool())` → on `nullptr`,
   **immediate `nullopt`**, before any factory is constructed and before any
   unknown-field inspection. The gate checks all five conditions the design
   listed (non-null, `!is_repeated`, containing type
   `google.protobuf.FieldOptions`, number `50001`, `CPPTYPE_MESSAGE`) and
   correctly does **not** demand
   `message_type()->full_name() == "fletcher.DictionaryOptions"`.
3. Only after the gate passes is `DynamicMessageFactory factory` declared and the
   re-parse attempted.
4. The presence probe `HasLengthDelimitedDictionaryField(opts)` is reachable from
   **exactly one** site: `if (!dyn) { ... }`. There is no second call.

So a foreign message-typed option at 50001 in a pool without
`fletcher/options.proto` cannot reach the probe. **Conforms to the narrowed D3
step 5.**

**Mutation-verified, not merely read.** I temporarily added the probe to the
`ext == nullptr` branch (the pre-R1 shape). Result: the two no-false-positive
sub-cases fail —

* `test_type_mapper.cpp:1034` "extension NOT in the pool -> nullopt"
* `test_type_mapper.cpp:1047` "FOREIGN message-typed option at #50001 -> nullopt"

— including the realistic case (`extend google.protobuf.FieldOptions { Foreign
foreign = 50001; }` in a pool that never imports Fletcher's options). So R1 is
genuinely pinned by the suite, not incidentally satisfied. Mutation reverted.

**The adjacent acceptance requirement was not sacrificed.** Malformed → defaults
still holds, on both of its shapes:

* zero-length submessage (`= {}` / injected empty payload) → parses, `HasField`
  true, both sub-fields absent → `{INT32, false}` **present**;
* truncated payload (`08`) → whole `ParseFromString` fails → probe fires →
  `DictionaryOption{}` **present**.

**Mutation-verified as probe-dependent:** with `HasLengthDelimitedDictionaryField`
short-circuited to false, *only* the `truncated` sub-case fails
(`test_type_mapper.cpp:1013`), and the `empty` case still passes — i.e. the probe
is load-bearing precisely for the case the design says it exists for, and for
nothing else. That is the exact trade the design demanded, in the exact place.
Mutation reverted.

## 3. `index_type` decoded by enum SYMBOL NAME — conforms, and the claim is now true

`DecodeDictionaryOptions` does
`index_type->enum_type()->FindValueByNumber(refl->GetEnumValue(d, index_type))`
and then switches on `evd->name()` in `IndexKindFromSymbol`. `enum_type()` is the
**pool's** enum descriptor, so a fork that renumbers the enum values is read
correctly end to end: protoc serialises the fork's number, the re-parse reads that
number against the fork's descriptor, the symbol resolves, the name maps. No raw
number ever reaches the mapping. `nullptr` `EnumValueDescriptor` and unrecognised
symbols both fall to `INT32`, as designed. **Conforms (locked #4, D0 reason 2).**

The claim is pinned from both ends by the forcing test:

* the **shipped** numbering by the injected-byte case `08 02` → `INT16`
  (asserts `DICTIONARY_INDEX_INT16 == 2` in the shipped file);
* the **symbol** path by the source-text cases, which compile the *shipped*
  `protoc/include/fletcher/options.proto` (via `FLETCHER_OPTIONS_PROTO_DIR`), so a
  renamed sub-field, renamed enum value or moved extension number fails the test.
  That also makes the forcing test an assertion of spec §2 and locked #2, as the
  design intended.

## 4. Carrier — `ir::FieldFacts` only; no second source of truth, no structural kind

* `protoc/include/ir.hpp` adds `enum class DictionaryIndexKind { INT8, INT16,
  INT32, INT64 }` (placed next to the dead `DictionaryModifier`, which is left
  untouched as designed) and two `FieldFacts` members
  `dictionary_index_kind` / `dictionary_ordered` beside the locked-named
  `bool dictionary`. Comment states the presence/payload split.
* `FieldKind` (`type_mapper.hpp:30-37`) — **unchanged**, six members, no
  dictionary member. Locked #5 honoured.
* `NodeKind` (`ir.hpp:29-36`) — **unchanged**. `Dictionary` is not modelled as a
  container peer of `List`/`Struct`. GIR locked #7 honoured, no stop-and-ask.
* `struct FieldMapping` — **unchanged**; `type_mapper.hpp` and `type_mapper.cpp`
  are not in the diff at all, so the flat projection cannot be a second source of
  truth in this item.
* Population is in `BaseFacts` (one place, all field-built nodes) plus the
  message-level flatten propagation. Test asserts `NodeKind::SCALAR` survives on
  a dictionary-modified scalar, on a WKT wrapper, and that a non-annotated
  sibling stays `false`/`INT32`.

**Conforms.**

## 5. Locked #10 — `options.pb.cc` is not linked

`protoc/CMakeLists.txt` adds exactly one line, `src/option_reader.cpp`, to
`fletcher_plugin_core`. There is **no** `protobuf_generate`, no
`add_custom_command`, and no reference to `options.pb.*` anywhere under `protoc/`
except the explanatory comment in `option_reader.hpp:17`. The extension is
reached only through `pool->FindExtensionByName` + reflection. **Conforms.**

## 6. The flagged deviation — adjudicated

### A1 (ACCEPTED, with a bookkeeping obligation) — the wrapper/leaf conflict is not recorded anywhere

**What the design says.** D4's code snippet requires
`AppendWarning(inner_ir.facts, "(fletcher.dictionary) on flatten wrapper '…' overridden…")`
on a disagreeing pair, and the forcing-test mapping requires the sub-case to
assert "…**and** `inner_ir.facts.warning` mentions the override (an IR-fact
assertion)".

**What was built** (`protoc/src/ir.cpp`, `BuildFlattenedSingular`): the nesting is
inverted —

```cpp
if (!inner_ir.facts.dictionary) {
    if (const std::optional<DictionaryOption> outer = ReadFieldDictionaryOption(field)) { ... }
}
```

— so when the leaf already declares a dictionary the wrapper's option is **never
read**, no conflict is detected, and no warning is written. `AppendWarning` was
never created (it does not exist in the tree).

**Adjudication: correct, and silence is acceptable for DICT-1.**

* **Behaviourally identical on the facts.** Both shapes resolve leaf-wins. The
  design's `else` branch only appended a warning; it changed no fact. Verified by
  reading both, and pinned by the test: `Holder.on_both` (wrapper `INT64`, leaf
  `INT8`) → `INT8`, `dictionary_ordered == false`. That is a *stronger*
  behavioural assertion than the warning-string assertion it replaces, and it is
  exactly the R7 observability the step-2 review asked for.
* **The channel really is dead — independently re-verified, not taken from the
  design.** `facts.warning` is copied into `FieldMapping.warning` at
  `type_mapper.cpp:159,169,184,194,217,248`, and a grep across
  `generator.cpp`, `cpp_backend_*.cpp`, `ts_backend_*.cpp` and
  `recordbatch_accessor_emitter.cpp` finds **no reader**. So writing the warning
  would have been unobservable to the user; not writing it loses nothing the
  design claimed the user would see. The design's own cycle-2 correction states
  this and hands escalation to DICT-2.
* **The deviation is recorded with a reason at the point of change** — the
  `ir.cpp` comment states leaf-wins, why (most-specific declaration; a
  shared-library wrapper must not dictate a consumer's column), and that the
  disagreeing pair is accepted silently because `facts.warning` is no longer
  rendered post-GIR and escalation belongs to the item owning the error channel.
  That meets the bar for a recorded deviation rather than a silent one.

**Residual + obligation (non-blocking):**

1. DICT-2 receives **no IR-level record** that a conflict occurred; if it wants to
   escalate it must re-read the wrapper descriptor. D4b already hands DICT-2 a
   descriptor walk, so this is not a new gap — but it is a real narrowing of the
   design's intent and should not be discovered by DICT-2 the hard way.
2. The **staged design doc now contradicts the staged code** (D4's snippet and the
   forcing-test-mapping row both still mandate the warning). Record the deviation
   in `plans/DICT-progress-log.md` at step-5, or strike those two lines from D4,
   so DICT-2 does not inherit a false premise.

### A2 (ACCEPTED) — sub-field presence is not probed before decode

Design step 4 phrases both sub-field reads as "present, singular, <cpp_type>".
`DecodeDictionaryOptions` checks singular + cpp_type but **not** `HasField`,
relying on `GetEnumValue` / `GetBool` returning the field's declared default when
unset. For the shipped file this is exactly equivalent (`index_type` default
`DICTIONARY_INDEX_UNSPECIFIED = 0` → `INT32`; `ordered` default `false`), and the
code comments the reasoning. The only divergence is hypothetical: a forked
`options.proto` that put a non-zero explicit `default =` on those sub-fields would
be honoured by the code and overridden to `INT32` by the design's letter — where
honouring the pool's declaration is the more defensible answer and is consistent
with D0 reason 2. **Accepted; no action.**

## 7. Scope — clean, and D4b's hole is left detectable

**Files touched** (`git diff --cached HEAD --name-only`) match the design's
Files-to-touch **exactly** — 9 modified (incl. `docs/dictionary-option-spec.md`,
`docs/fletcher-options.md`, `protoc/tests/CMakeLists.txt`), 3 added
(`option_reader.hpp/.cpp`, `proto_text_pool.hpp`), plus the design doc itself.

**Hard scope guard (D6) intact — no stop-and-ask triggered:**
`protoc/src/recordbatch_accessor_emitter.*` — untouched. All Rust accessor
emission — untouched. `protoc/include/type_mapper.hpp`, `protoc/src/type_mapper.cpp`,
`protoc/src/generator.cpp`, `protoc/src/cpp_backend_*`, `arrow-bridge/*`,
`pubsub-arrow/*` — all untouched. `FindBoolOption`/flatten reading — untouched.
The only match for the guard-grep is `protoc/tests/test_type_mapper.cpp`, a test
file the design names.

`test_type_mapper.cpp` includes `cpp_backend_schema_visitor.hpp` — that is a
**public** header (`protoc/include/`, PUBLIC include dir) and the design's D4b
sub-case explicitly directs the test at `cpp_backend::BuildFlattenedFieldList`.
Not a scope breach.

**D4b's wrapper hole is neither silently closed nor silently lost.** It is
converted into a live assertion in the forcing test, on the `Rec` fixture
(`Pair p = 1 [(fletcher.flatten_field) = true, (fletcher.dictionary) = {INT16}]`):

* `HasFieldDictionary(Rec.p) == true` — the declaration **is** readable, so
  DICT-2's descriptor-walk rejection has something to fire on;
* neither inlined record (`a`, `b`) carries `facts.dictionary`;
* `dict_count == 1`, the single dictionary being the sibling scalar `s`
  (which also pins spec §4's "flatten_field on a scalar is a no-op, document,
  do not error").

Any future change that silently propagated the wrapper's option to the N inlined
columns breaks this, and DICT-2 closing the hole must consciously flip it.
Ownership is recorded in the staged design doc (D4b decision + hand-off), and
spec §4 independently mandates the rejection. **Conforms.**

`BuildFlattenedRepeated` correctly received **no** propagation (design: DICT-2
rejects it as non-`SCALAR`). The `field_count() != 1` branch of
`BuildFlattenedSingular` still uses `BaseFacts(field)`, so a wrapper-declared
dictionary lands on a `STRUCT` node there — also a DICT-2 rejection, consistent
with the design.

## 8. Spec §7 rewrite — faithful to what was built

Checked clause by clause against `option_reader.cpp`:

| §7 claim | Code |
|---|---|
| does not link `options.pb.cc`; value sits as length-delimited unknown #50001 | confirmed (CMakeLists, §5 above) |
| serialize `field->options()`, re-parse into a `DynamicMessage` from the request's pool | `ReparseOptionsWithPool` |
| sub-fields located **by name**, `index_type` resolved to its **symbol name** | `FindFieldByName` + `FindValueByNumber(...)->name()` |
| one shared primitive `ReparseOptionsWithPool`, extracted from `option_metadata.cpp` | confirmed, both callers |
| presence is the trigger, read as `HasField` on the re-parsed extension | `if (!refl->HasField(*dyn, ext)) return nullopt;` |
| the **declaration** is the only evidence; a bare #50001 is not enough | `ResolveDictionaryExtension` 5-way gate, checked before anything else |
| fail-soft: declared-but-unreadable → defaults; narrow probe; probe decodes nothing | confirmed + mutation-verified (§2) |
| louder failure belongs to the validation pass | consistent with the D4b/DICT-2 hand-off |
| linking the generated descriptor remains the recorded alternative | retained |

Spec §2's `.proto` block matches the shipped
`protoc/include/fletcher/options.proto` exactly (enum member names and numbers,
`index_type = 1`, `ordered = 2`, `dictionary = 50001`, proto2 `optional`).
`docs/fletcher-options.md` gained the one registry row locked #2 requires.
**Conforms.**

## 9. Story acceptance (`plans/DICT-dictionary-option.md` § DICT-1)

All met: hand-encoded `08 02` → `{INT16,false}`; `10 01` and `08 02 10 01` →
`ordered == true`; absence → no dictionary and `HasFieldDictionary == false`;
malformed/empty payload → defaults; full index round-trip
`{unspecified→int32, int8, int16, int32, int64}` driven from real source text.

The story's "put it in `type_mapper.cpp` / declare in `type_mapper.hpp`" wording
was deliberately deviated by the **approved** design (D3 placement note:
`type_mapper.hpp:13-15` only forward-declares `ir::IrNode` and must not start
including `ir.hpp`). That is pre-existing, reasoned and APPROVEd — not a new
deviation.

## 10. Non-blocking observations

* **O1** — `docs/fletcher-options.md` also changed one prose sentence
  ("Field number 50000 is…" → "Field numbers 50000/50001 are…") beyond the
  registry row the design authorised. Trivial grammatical follow-through of the
  row itself; noted only because the design said "registry row only; the
  user-facing prose section stays DICT-5's".
* **O2** — `proto_text_pool.hpp` was written fresh rather than literally
  "extracted from `test_option_metadata.cpp`'s fixture". This is the right call:
  the design's own regression requirement is that `test_option_metadata.cpp` stay
  green **unchanged**, and it calls the refactor "later optional". Cost is ~25
  duplicated lines of pool seeding. Everything in the header is `inline` /
  in-class as the single-binary trap requires, and the duplicate-50000 pool trap
  is documented at the top and respected (each sub-case builds its own pool).
* **O3** — the `truncated (08)` sub-case asserts `{present, defaults}`, which is
  also what a hypothetically-successful parse would yield, so the assertion does
  not *by itself* uniquely pin step 5. I closed that gap by mutation (§2); if the
  team wants it pinned in-suite, asserting that the *foreign-shaped* corrupt
  payload behaves differently would do it. Not required by the design.
* **O4** — D0 reason 2's forward-compatibility is true for **renumbering**, not
  **renaming**: a fork that renames the enum values degrades silently to `INT32`.
  The design records this ("Unknown name … → INT32"); no test exercises it.
  Acceptable.
* Cost/lifetime contract (D5) implemented as specified: the gate precedes the
  factory, so only fields that carry options **in a pool that declares
  `fletcher.dictionary`** pay for a `DynamicMessage`; the factory is function-local
  and declared **before** the message it creates in both call sites
  (`ReadFieldDictionaryOption` and the `Impl` member order); all decoded values are
  plain scalars copied out before the factory dies. No process-global cache, no
  `static` factory.

## 11. Tree state after review

Both mutations reverted with `git checkout --` from the index. `git status
--short` is byte-identical to the pre-review snapshot (13 entries, same
M/A flags), and the full protoc suite is green again (92/93, 1 pre-existing skip).
No review artefact left in the source tree other than this file.

---

# Re-review — fix round on top of the above (step-4a, cycle 2)

Diff base `6fa5085`, staged. Re-reviewed after the step-4b fix round. Delta vs
the state reviewed above: `ir.cpp` +67/-0 (was +28), `option_reader.cpp` 198 lines
(was 164), `option_reader.hpp` 82 (was 71), `proto_text_pool.hpp` 136 (was 122),
`test_type_mapper.cpp` +606 (was +374), spec +100/-11 (was +56/-11).

## Verdict: **DEVIATES** — 1 blocking item, 1 nit

Everything the coordinator asked to re-confirm holds, the SF-1 deferral is
correctly reasoned and genuinely detectable, the P2-5 skip is right, and the R1
property is now pinned **in-suite** (stronger than the mutation I needed last
round). One item blocks: the **placement rule that SF-2 introduced is stated more
broadly than the code delivers**, and it is stated in the *authoritative spec*
plus the code comment that DICT-2/DICT-3 will read.

Suite: rebuilt, **92/93 green**, 1 pre-existing skip. `clang-format 18.1.3
--dry-run -Werror` clean on all seven changed/added C++ files.

---

### RR-1 (BLOCKING) — the SF-2 placement rule is false for a reachable shape; DICT-3 cannot rely on it as written

**What is asserted.** Two places, in identical terms:

* `docs/dictionary-option-spec.md` §7.1, "Where the fact lands (for consumers)":
  "A dictionary declaration is a *field-level* fact, so **every IR node built from
  that field carries it**".
* `protoc/src/ir.cpp`, `BaseFacts` comment: "a dictionary declaration is a
  FIELD-level fact, so EVERY node built from that field carries it, **never just
  one of them** … Consumers therefore never have to guess which node is
  authoritative".

**Counterexample, confirmed empirically** (temporary probe test, built and run,
then reverted). Shape:

```proto
message WrapRep { option (fletcher.flatten) = true; repeated string values = 1; }
message H { repeated WrapRep xs = 1 [(fletcher.dictionary) = { index_type: DICTIONARY_INDEX_INT16 }]; }
```

Observed `BuildFieldIr(H.xs)`:

| level | kind | `facts.dictionary` |
|---|---|---|
| 1 (outer) | LIST | **true** |
| 2 | LIST | **false** |
| 3 (leaf) | SCALAR | **true** |

**Mechanism** — `ir.cpp`, `BuildFlattenedRepeated`, the repeated-scalar-leaf
branch: `leaf.facts = BaseFacts(field)`, then
`node = MakeListOf(std::move(leaf))`, then
`for (int d = 0; d < depth + 1; ++d) node = MakeListOf(std::move(node));`, then
`node.facts = BaseFacts(field);`. `MakeListOf` (`ir.cpp:170-176`) gives every level
it creates a **default-constructed** `FieldFacts`, and only the *outermost* node
is overwritten. Same pattern in the non-message-leaf branch
(`for (int d = 0; d < depth; ++d)`) and in the final leaf-struct return.

**Why this blocks rather than being a nit.**

1. The coordinator's own question is "confirm the rule as written is what the code
   does, and that DICT-3 can rely on it". It is not, and DICT-3 cannot.
2. The gap is **interior**, which is strictly worse than a uniform miss: a
   consumer descending the node tree finds the fact, loses it, then finds it
   again. That defeats the exact promise the rule makes ("never have to guess
   which node is authoritative").
3. It is **not covered by the SF-1 deferral text**. §7.1 item 2 defers only "a
   dictionary declared on `W`'s single inner field is **not read**", and says the
   outer-declared case "is carried" — unqualified. So the deferral record is
   *incomplete* as well as the rule being *overstated*.
4. The suite cannot catch it: the SF-1 outer-declared sub-case uses
   `repeated WrapOuter rep_outer_declared` whose wrapper holds a **singular**
   `string value`, i.e. depth 0 and exactly one list level — the only depth at
   which the rule happens to hold. The 2-level shape is never built.
5. **Same standard as the first-round finding A1.** I blocked-with-obligation
   there because the design doc asserted a behaviour (`facts.warning` set on a
   conflict) the code did not implement. Here the assertion lives in the
   *authoritative spec*, is addressed *to consumers*, and is what DICT-3's design
   will quote. Weaker treatment would be inconsistent.

**Remedy (cheap; doc + one sub-case).**

* Scope the rule to what the builders actually write: the **top-level** node plus
  the element chain written by `BuildRepeatedScalarOrEnum` / `ApplyDictionaryFacts`
  — and state the exception: *intermediate* list levels synthesised by
  `BuildFlattenedRepeated` for nested-list shapes do **not** carry the fact.
* Add the exception to §7.1 item 2 so the deferral record is complete.
* Pin it. The fixture **already** declares `WrapRepeated` (flatten + `repeated
  string values`), so `repeated WrapRepeated rep_rep = 8 [(fletcher.dictionary) =
  {...}]` plus three assertions is the whole cost, and it lands next to the
  existing SF-1 pair.

No code change is required — the *behaviour* is inside the deliberately-deferred
`BuildFlattenedRepeated` and is harmless today for the same reason SF-1 is
(the node is LIST, which DICT-2 rejects). What must change is the claim.

### RR-2 (nit, non-blocking) — wrong return-site count in the SF-1 deferral rationale

`ir.cpp`'s `BuildFlattenedRepeated` comment says "mirroring the propagation across
this function's **five** return sites". There are **seven**: four that build LIST
nodes (`field_count() != 1`; the repeated-scalar-leaf branch; the non-message-leaf
branch; the final leaf-struct return) and three `MakeUnsupported` returns
("map value type unsupported", "flatten wrapper leaf type unsupported",
recursive). The miscount is in a load-bearing deferral rationale that DICT-2 will
weigh; it happens to *under*state the surface, so it does not weaken the argument,
but it should be right.

---

## Adjudication of the three items the coordinator raised

### (1) SF-1 as a DOCUMENTED DEFERRAL rather than a code fix — **CORRECT**

**Reasoning verified against the tree.** `BuildFlattenedRepeated` has seven return
sites (RR-2); every one yields `NodeKind::LIST` (four) or `NodeKind::UNSUPPORTED`
(three). None can produce a top-level `SCALAR` or `STRUCT`. So the claim "the
resulting node is always LIST/UNSUPPORTED, which DICT-2 rejects as non-SCALAR
anyway" is **true as stated**, and locked #9 makes non-`SCALAR` a codegen error —
so the deferral cannot produce a *wrong schema*, only a *missed loud rejection*.
§7.1 says exactly that and does not dress it up: "the cost of the gap is that the
rejection cannot *fire* for the inner-declared shape, so it stays quiet instead of
loud." Honest, and the honest cost is acceptable at DICT-1.

**Genuinely detectable, not lost.** The sub-case "flattened repeated:
INNER-declared dictionary is DROPPED (SF-1 gap)" does three things, and all three
are needed: it asserts `HasFieldDictionary(WrapInner.field(0)) == true` (the
declaration *is* readable, so DICT-2's future check has something to fire on),
`EXPECT_FALSE` on **both** the LIST and its element, and — the part that makes it
a trap rather than a snapshot — the explicit contrast
`EXPECT_TRUE(BuildFieldIr(Holder.on_inner).facts.dictionary)`, so the divergence
between the singular and repeated spellings of the *same wrapper* is the thing
under test. Closing the gap flips it. Verified green.

**A new spec section is the right home — and this is the same standard I applied
to A1.** Last round I objected that the design doc asserted a behaviour the code
lacked, and required reconciliation in a durable artefact. Here the implementer
went further than a progress-log line: the deferral is recorded (a) in the
**authoritative spec** §7.1, beside the `flatten_field` hole it is a sibling of,
(b) in a comment at the deferral site in `ir.cpp`, and (c) as a pinning test.
Ownership is **DICT-2**, i.e. inside the round, so locked #11 ("a new deferral that
does not land in RIR is a stop-and-ask") is not triggered. The spec is the artefact
DICT-2's design and its review actually read, which a progress-log entry is not.
Correct home, correctly cross-referenced from code and test.

### (2) The new spec §7.1 — faithful, coherent with the §7 rewrite, with one overstatement

Faithful on the substance:

| §7.1 claim | Tree |
|---|---|
| `flatten_field` wrapper inlined away before its IR node exists, by `BuildFlattenedFieldListImpl` / `GatherFieldsImpl` | correct; unchanged from the first-round check |
| intended semantics **reject**; enforcement must be a front-end **descriptor** walk, not a projection check | correct — the projection is never invoked for the wrapper |
| a *scalar* carrying both is a documented no-op and the dictionary applies | correct, and pinned (`Rec.s`, INT8) |
| `BuildFlattenedRepeated` builds every node from the **outer** field's facts | correct for the nodes it writes; see RR-1 for the levels it does not write |
| outer-declared IS carried, inner-declared is NOT read | correct and pinned |
| the singular spelling disagrees with the repeated one | correct and pinned by the contrast assertion |
| resulting node is always a list, so validation rejects it as non-scalar; the cost is a quiet instead of loud rejection | correct (seven return sites, all LIST/UNSUPPORTED) |
| map key/value nodes never carry it (built from the synthetic map entry's own fields) | correct — `BuildMapNode` uses `BaseFacts(key_fd)` / `BaseFacts(val_fd)`; pinned |
| consumers must gate on the **top-level** node's kind | correct; pinned by the repeated / map / oneof / struct sub-cases |
| "every IR node built from that field carries it" | **overstated — RR-1** |

**Coherent with the §7 rewrite.** §7 (mechanism: reflection, presence trigger,
declaration-only evidence, fail-soft) and §7.1 (the reader's *reach*, and where the
fact lands) do not contradict each other, and the walker mandate is gone from both.
The pre-existing §5 / §5.1 talk about `FieldMapping.is_dictionary` — DICT-2's
projection and RIR's accessor — and are untouched, so no duplicate or competing
statement of the carrier was introduced. Locked #5's "projection, not a second
source of truth" survives.

One structural remark, not a finding: the "Where the fact lands" paragraph is a
*positive contract* filed under a heading titled "Known gaps at v1". That is
defensible (the gaps are its exceptions) but it is why RR-1's overstatement is
consequential rather than cosmetic — as filed, it reads as the authoritative
contract a consumer may lean on.

### (3) SF-2 / `ApplyDictionaryFacts` — the code is right; only the stated scope is too broad

`ApplyDictionaryFacts` sets the three facts on `node` when it has none, then
recurses into a LIST's element via `std::get_if<ListNode>`. Checked against the
rule it is meant to uphold:

* **Matches `BuildRepeatedScalarOrEnum`** (`ir.cpp:453-461`), which writes
  `BaseFacts(field)` to *both* the element and the list. So the flatten-propagation
  path and the direct path now agree for the one-level case — which was the actual
  SF-2 defect. Pinned by "flatten wrapper over a repeated inner field: LIST *and*
  element" (`Holder.on_rep_wrapper`, INT16 on both), verified green.
* **Correctly does NOT recurse into STRUCT children or MAP key/value.** Those are
  built from their own field descriptors, so recursing would fabricate a
  declaration the author never wrote. §7.1 states this and the test pins both (map
  key/value `EXPECT_FALSE`; struct member carries its own).
* **Leaf-wins is doubly enforced** — at the call site, which only enters when the
  inner node has no dictionary, and per node inside. The per-node guard is
  defensive-only today: the only way to reach a node-with-fact under a
  top-without-fact is the `BuildFlattenedRepeated` interior-list shape, which this
  function is never called on. Harmless, and cheap insurance.
* Early-return ordering preserved: the `field_count() != 1` struct branch of
  `BuildFlattenedSingular` still returns **before** the propagation block, so a
  flatten-ignored wrapper is untouched.

**Can DICT-3 rely on the rule?** On the rule *scoped to what the builders write* —
yes. On the rule *as written* — no: see RR-1. That is the whole of my objection to
SF-2; the mechanism itself is correct and is an improvement on what I passed last
round.

### (4) The P2-5 skip — **CORRECT**, and the recorded substitute is adequate (in fact better)

Skipping is the conformant answer. D3 step 5 defines the probe as a *presence*
probe that "decodes nothing — so it does not reconstitute the walker D0 rejected",
and §7 now repeats that verbatim ("The probe *decodes nothing* — it is not a
payload walker"). Parsing the #50001 payload after the whole-message parse already
failed would be a bespoke, hand-rolled decode of a length-delimited custom-option
payload on the failure path — precisely the mechanism locked #10 (revisited) and D0
retired, and precisely the contract I mutation-verified as conforming last round.
Implementing P2-5 would have been the deviation.

The substitute is adequate and, unusually, stronger than the thing it replaces:

* **Recorded at the probe site** — the SF-3 comment in `ReadFieldDictionaryOption`
  states the limit exactly: because protobuf refuses two extensions of one extendee
  at one number in a pool, no foreign option can be *declared* at 50001 alongside
  `fletcher.dictionary`, so the no-false-positive property is defended at
  **pool-declaration** granularity, not per field; once the pool declares the
  extension the probe trusts a bare "50001 + LENGTH_DELIMITED" record.
* **Recorded in the spec** — §7's "Granularity of that guarantee" paragraph says the
  same, and does **not** overstate: it explicitly concedes that the bytes are
  interpreted "regardless of who wrote them" and that "the wire format carries no
  type identity, so no reader can do better". That is the honest statement P2-5
  would only have narrowed, never closed.
* **Now pinned in-suite, not by mutation.** The new identical-bytes pair is the
  right test: the *same* foreign declaration and the *same* payload bytes
  (`kI16OrderedBytes`, asserted equal via `UnknownPayloadAt`, not assumed) read as
  `nullopt` at **60200 inside the Fletcher-aware pool** and as `nullopt` at
  **50001 in a pool without Fletcher's options**, while those exact bytes decode to
  `{INT16, ordered}` when they sit at 50001 in a Fletcher-aware pool. Three
  outcomes, one byte string — the reader demonstrably keys off the pool's
  declaration, not the number and not the bytes. Last round I had to establish this
  by mutating the source; it is now a first-class assertion. Net improvement.

### (5) Changes to the SHARED primitive — checked for #121 impact, no drift

Not on the coordinator's list, but they touch #121's path, so I re-derived them:

* **`SerializeToString` (checked) replaces `SerializeAsString` (P2-8).** Failure is
  unreachable for a `FieldOptions`: it has no required fields and its custom
  options live in the `UnknownFieldSet` as opaque bytes, so `IsInitialized()` is
  always true. Where the two *would* differ, the new path is the safer one — the old
  one turned a serialization failure into an empty string, which parses successfully
  into an empty message and reports "option absent"; the new one returns `nullptr`
  and routes the caller to its fail-soft path. Design D2 pinned only
  "serialize / re-parse", so this stays inside the approved shape.
* **Null guards on `pool_options_descriptor` / `factory` (P2-7 / P2-9).**
  `ParsedOptions` passes `rule.steps[0].ext->containing_type()` and the address of
  its own member factory, neither of which can be null, so #121 is unaffected.
* **The documented FLAT-POOL precondition** is a real, previously-unrecorded limit
  of the primitive (a default `DynamicMessageFactory` resolves extensions through
  `pool_options_descriptor->file()->pool()`), and the shared API header is the right
  place for it.
* **`test_option_metadata.cpp` still has ZERO lines of diff** and the 33 #121 tests
  are green (`OptionMetadataTest` 17 + `EscapeCppStringLiteralTest` 9 +
  `MetadataRuleParseTest` 4 + `MetadataRuleCompileTest` 3). The design's no-drift
  proof obligation is met unchanged.

## Re-confirmed unchanged (all hold)

| Invariant | Evidence |
|---|---|
| Locked #10 — `options.pb.cc` not linked | `protoc/CMakeLists.txt` adds exactly `src/option_reader.cpp`; no `protobuf_generate`, no `add_custom_command`; zero `options.pb` references under `protoc/` outside the explanatory comment |
| `factory` before `cache` | `option_metadata.cpp:243` vs `:253`, member-order invariant comment intact |
| `ir::FieldFacts` sole carrier | `ir.hpp` diff is the `DictionaryIndexKind` enum plus two members plus comments; `NodeKind` untouched; `FieldMapping` untouched |
| No `FieldKind` member | `protoc/include/type_mapper.hpp` — **zero diff** |
| No RBA / Rust diff | `recordbatch_accessor_emitter.{hpp,cpp}`, `type_mapper.{hpp,cpp}`, `generator.cpp`, `cpp_backend_*`, all Rust emission — **zero diff** |
| `test_option_metadata.cpp` | **zero lines of diff** |
| Files-to-touch | unchanged from the first-round check; the only additions to the staged set are the two review artefacts under `plans/reviews/` |
| SPDX / clang-format | headers present on all three new files; `clang-format 18.1.3 --dry-run -Werror` clean on all seven changed/added C++ files |

## Tree state after re-review

One temporary probe test was appended to `protoc/tests/test_type_mapper.cpp` to
obtain the RR-1 evidence, then removed with `git checkout --` from the index
(`grep -c ZZZReviewProbe` returns 0). `git status --short` matches the pre-review
snapshot; the suite was rebuilt and re-verified green (92/93, one pre-existing
skip).

---

# Confirmation pass — RR-1 / RR-2 discharge (cycle 3)

Scope: verification only, per the coordinator. Base `6fa5085`.

## Verdict: **CONFORMS** on content — RR-1 and RR-2 are genuinely discharged.
## One delivery blocker outside the code: **the fix is NOT STAGED.**

### D-1 (must fix before commit) — the RR-1/RR-2 fix lives only in the working tree

The coordinator's message says "Staged". It is not. `git diff --cached HEAD` — the
artefact I was asked to review, and what `git commit` would capture — still holds
the **pre-fix** text for all three files:

| File | index (`git show :<path>`) | working tree |
|---|---|---|
| `protoc/src/ir.cpp` | "five return sites"; "every node here is built from BaseFacts(FIELD)" | "seven return sites (four LIST, three MakeUnsupported)"; qualified PLACEMENT RULE |
| `docs/dictionary-option-spec.md` | old unqualified "every IR node built from that field carries it"; no table (`grep -c "Nodes carrying"` → **0**) | placement table + interior-gap paragraph |
| `protoc/tests/test_type_mapper.cpp` | no `rep_nested` (`grep -c` → **0**); `foreign_parseable` = `1a 03 61 62 63` | `rep_nested` fixture + sub-case; `foreign_parseable` = `08 02 1a 03 61 62 63` |

Committing the index as-is would land the *worst* half-state: the authoritative
spec would still carry the invariant RR-1 blocked on, with no guarding sub-case.
Remedy: `git add docs/dictionary-option-spec.md protoc/src/ir.cpp
protoc/tests/test_type_mapper.cpp`. Content-wise nothing else is needed.

**Reviewer error, disclosed.** While mutation-testing the new sub-case I reverted
`protoc/src/ir.cpp` with `git checkout --`, which restores from the **index** and
therefore discarded the (unstaged) fix in that file. I restored it from a
pre-mutation copy and verified: RR-2's wording is back to "seven return sites",
the qualified PLACEMENT RULE is back, `git diff --numstat` → `25 12
protoc/src/ir.cpp`, `clang-format --dry-run -Werror` clean, suite green. No other
file was touched by me except this review document. Flagging it because the
partially-staged tree made a normally-safe revert destructive.

### (a) The narrowed claim matches the code for every row — **YES**

Verified against every `BaseFacts` assignment site in `ir.cpp` (20 of them):

| Table row | Code |
|---|---|
| singular scalar / WKT / struct / oneof-unsupported → the single node | scalar path; `TryBuildWkt`; `BuildSingularMessage` (`MakeStructNode` + `BaseFacts` on the STRUCT only, children from their own fields); `MakeUnsupported` |
| `repeated` scalar / enum → list **and** element | `BuildRepeatedScalarOrEnum`: `elem.facts` *and* `node.facts` |
| map → map node only | `BuildMapNode`: `node.facts = BaseFacts(field)`, key/value from `BaseFacts(key_fd)` / `BaseFacts(val_fd)` |
| flatten (singular), incl. over a `repeated` inner → node + element when list | `ApplyDictionaryFacts` recursing into `ListNode::element` |
| nested list from repeated flatten → outermost + leaf, intermediates default | `BuildFlattenedRepeated`: `leaf.facts` and the post-loop `node.facts`; every level from `MakeListOf` keeps default facts |

The governing sentence — "it lands on every IR node that is itself built from that
field's facts — which is **not** the same as 'every node in the subtree'" — is now
exactly true, and the `BaseFacts` comment states the same rule in the same terms.

*Completeness note (non-blocking):* the table has no row for **non-flatten
`repeated <message>`**. `BuildRepeatedMessage` gives the LIST `BaseFacts(field)`
while the STRUCT element from `MakeStructNode` keeps default facts — so LIST only.
That follows correctly from the governing sentence and contradicts nothing, but a
reader who generalises row 2 (`repeated` scalar → list *and* element) by symmetry
would guess wrong. One row would close it.

### (b) The new sub-case guards both directions — **YES, mutation-verified**

Reasoning alone was insufficient here, so I mutated `BuildFlattenedRepeated` twice
and rebuilt each time:

* **interior gap closed** (`IrNode node = MakeListOf(std::move(leaf)); node.facts =
  BaseFacts(field);`) → fails at `test_type_mapper.cpp:1094` with "intermediate
  list level carries DEFAULT facts".
* **leaf loses the fact** (`leaf.facts.dictionary = false;`) → fails at the same
  sub-case with "the leaf IS built from BaseFacts(field)".

Both mutations reverted. Note also `ASSERT_EQ(outer_list.element->kind,
NodeKind::LIST)` and `EXPECT_EQ(mid_list.element->kind, NodeKind::SCALAR)`, which
pin the *shape*, so a restructuring that removed the intermediate level fails too
rather than passing vacuously.

*Useful detail for whoever maintains this:* the intermediate level is the **first**
`MakeListOf(std::move(leaf))`, not a loop iteration — my first mutation attempt
targeted the `for (int d = 0; d < depth + 1; ++d)` body and the test correctly did
**not** trip, because at `depth == 0` that loop produces the outermost list. The
sub-case therefore pins the depth-0 shape (one intermediate); a depth ≥ 1 shape has
several intermediates and is covered by the rule's wording but not by a test. Fine
as scoped.

### (c) Residual over-broad wording — **THREE spots survive** (non-blocking)

The two authoritative statements are fixed. What remains:

1. `docs/dictionary-option-spec.md` §7.1 item 2 still opens
   "`ir::BuildFlattenedRepeated` builds **every node** from the **outer** field's
   facts" — then corrects itself in the same sentence ("carried on the outermost
   list and the leaf (but *not* on intermediate list levels …)"). Net-correct, but
   the leading clause is the exact phrasing RR-1 objected to; "builds its nodes
   from" would remove the snag.
2. `protoc/tests/test_type_mapper.cpp:981-982`, the SF-4 block header: "Pins the
   PLACEMENT RULE asserted by BaseFacts' comment: **every node built from the
   field** carries the fact". Should be "every node built from `BaseFacts(field)`".
3. `protoc/tests/test_type_mapper.cpp:1073`, the SF-1 block header: "Outer-declared
   is carried (**every node** is built from BaseFacts(outer))" — directly
   contradicted 16 lines later by the RR-1 block's "the placement rule is NOT
   'every node built from the field'".

`ir.cpp`'s two remaining uses of "every" are correctly scoped ("every such node",
where "such" = passes through `BaseFacts`; "EVERY node that is itself built from
BaseFacts(field)"). Not findings.

### RR-2 — confirmed

`BuildFlattenedRepeated` comment now reads "seven return sites (four LIST, three
MakeUnsupported)". Independently counted: 7 `return` statements — four building
LIST nodes (`field_count() != 1`; repeated-scalar-leaf; non-message-leaf; final
leaf-struct) and three `MakeUnsupported` ("map value type unsupported", "flatten
wrapper leaf type unsupported", recursive). The over-broad "every node here is
built from BaseFacts(FIELD)" in that same comment was also replaced with "the
OUTERMOST node and the leaf here are built from BaseFacts(FIELD) … (but NOT on the
intermediate list levels of a nested-list shape — RR-1)". Both parts discharged.

### P2-21 — does not weaken R1

`foreign_parseable` moving to `08 02 1a 03 61 62 63` / INT16 **strengthens** the
suite. It is an *injected* payload in a pool that **does** declare
`fletcher.dictionary`, so it is not on the R1 axis at all; R1 is carried by the
untouched `dict_no_ext` case, the `ForeignSchema(50001)` case and the 60200 half,
all still asserting identical `kI16OrderedBytes`. What the change buys is exactly
the discrimination I recorded as O3 in round 1: previously every corrupt/foreign
row collapsed to defaults, so the probe path and the decode path were
indistinguishable; now `foreign_parseable` = INT16 while `truncated` and
`foreign_corrupt` = defaults, so "the probe was not involved" is observable. The
new `wrong_number` row (valid bytes at #50002 → not read) is a further gain.

### Unchanged, re-confirmed

SF-1 deferral text and its pinning sub-case; P2-5 skip; locked #10 (CMake adds only
`src/option_reader.cpp`, zero `options.pb` references); `factory`@243 before
`cache`@253 with its invariant comment; `ir::FieldFacts` sole carrier; no
`FieldKind` member (`type_mapper.hpp` zero diff); zero diff to RBA / Rust /
`type_mapper.*` / `generator.cpp` / `cpp_backend_*`; `test_option_metadata.cpp`
**zero lines of diff**; `clang-format 18.1.3 --dry-run -Werror` clean.

Suite after restoring `ir.cpp`: **93 tests, 92 pass, 1 pre-existing skip**
(`SchemaVisitor.CaptureGoldens`).
