# DICT-3 — Schema emission (ONE IR schema-visitor)

Item: **DICT-3** · Round: **DICT** · Branch: `feature/dictionary-option` (base `832bff4`)
Story: `plans/DICT-dictionary-option.md` § "DICT-3 — Schema emission (ONE IR schema-visitor)"
Spec: `docs/dictionary-option-spec.md` §3, §5, §7.1, §7.1.1
Locked: `plans/DICT-locked-decisions.md` (#3, #4, #5, #6, #7, #8, #9, #11)
Forcing test: `DictionaryTest.SchemaIsDictionaryType`

---

## Summary

Teach the **one** `cpp_backend::SchemaVisitor` to emit `dictionary(<declared index>,
<value>)` for a top-level field whose IR node is `SCALAR` **and** carries
`facts.dictionary`, via a single new sink operation so the generated
`<Cls>Schema()` C++ source and the in-process nanoarrow schema agree by
construction. Every row-oriented path, the codec, the wire and the runtime
re-fold are untouched.

DICT-3 also closes a hole it *creates*: the vendored nanoarrow IPC encoder
rejects dictionary types outright, so `--fletcher_opt=ipc` must become a
front-end backend-availability rejection instead of a late, partial-output
failure — and DICT-1.5's guard text, which currently claims the IPC backend
accepts dictionary fields, becomes false and is corrected here.

---

## Design

### D1 — The gate sits at exactly ONE position: the top-level field loop

Per the DICT-2 carry-forward and spec §7.1.1, the branch is

```
node.kind == ir::NodeKind::SCALAR  &&  node.facts.dictionary
```

and it is evaluated **only** in `SchemaVisitor::Visit()`'s per-field loop
(`cpp_backend_schema_visitor.cpp:523-534`), on `*f.node` — the *top-level* node
of a `SchemaFieldRecord`. `EmitNodeType()` and `EmitScalarType()` must **never**
read `facts.dictionary`.

This placement is load-bearing twice over, not stylistic:

* **The kind half** is what DICT-2 mandated. A DICT-2-accepted proto can reach
  emission with the fact live on a `LIST` node (spec §7.1.1's `map<string,W>` /
  `W{flatten; repeated string vals [(dictionary)={INT8}]}` repro, rc 0 under
  `ipc`, `accessor` naming `xf.ns.W.vals`). Gating on the fact alone emits
  `dictionary(idx, <list>)`.
* **The position half** is independently necessary and is *not* covered by the
  kind half. Per the spec §7.1.1 placement table, a `repeated` scalar/enum
  carrying the option puts the fact on the list node **and its element**, and the
  element is a `SCALAR`. If the gate were moved inside `EmitNodeType`'s `SCALAR`
  case — the obvious-looking place — the element would satisfy both terms and the
  visitor would emit `list<dictionary(idx, utf8)>`: precisely the shape spec §8
  puts out of scope and `EnsureDictionaryValueSupported`
  (`arrow-bridge/src/scalar_codec.cpp:32-52`) rejects at runtime. Keeping the
  gate at the top-level position makes `list<dictionary(...)>` and
  `map<K, dictionary(...)>` **structurally unreachable** rather than merely
  filtered, which is the general (class-level) guarantee this item owes.

Consequences that follow for free, and are the reason no second gate is needed
anywhere:

* **Nested positions are handled by recursion into a full `Visit()`, not by a
  second branch.** A scalar dictionary field inside a struct child / list element
  / map value is emitted by *that message's own* `<Child>Schema()`, whose
  `Visit()` applies the gate at its own top level; the parent then deep-copies it.
  `ArrowSchemaDeepCopy` copies `schema->dictionary`
  (`protoc/third_party/nanoarrow/nanoarrow.c:1194-1206`), so the C++-source sink's
  `ArrowSchemaDeepCopy(<Child>Schema().get(), dst)` and the nanoarrow sink's
  `BuildMessageSchemaIntoFromIr(child, …) + ArrowSchemaDeepCopy` both preserve it.
  Verified in the vendored source, not assumed.
* **Map key/value nodes never trigger it**: they are built from the synthetic
  map-entry fields and carry `dictionary == false` (spec §7.1.1 placement table),
  and they are reached only through `EmitNodeType`, which does not read the fact.
* **Dictionary-ness stays IR-derived.** No descriptor read (`HasFieldDictionary`)
  enters emission — this is DICT-1.5's binding carry-forward and the reason its
  superset property survives.

Suggested shape (names indicative, structure normative):

```cpp
// SchemaVisitor private
void EmitTopLevelFieldType(const ir::IrNode& node, SchemaRef schema) {
    if (node.kind == NodeKind::SCALAR && node.facts.dictionary) {
        // DICT-3: dictionary encoding is emitted ONLY here, at the top-level
        // field position. Do NOT move this into EmitNodeType — see D1.
        SchemaRef value =
            sink_.SetTypeDictionary(schema, DictionaryIndexArrowType(
                                                node.facts.dictionary_index_kind));
        EmitScalarType(std::get<ir::ScalarNode>(node.node), value);
        return;
    }
    EmitNodeType(node, schema);   // byte-for-byte today's path
}
```
and `Visit()`'s loop calls `EmitTopLevelFieldType(*f.node, child)` in place of
`EmitNodeType(*f.node, child)`. The overlay lines that follow
(`SetName` / `SetNullable` / `SetMetadata`) are **unchanged and stay after**.

### D2 — One new sink operation, returning the value slot

```cpp
// SchemaSink (pure virtual; both sinks implement)
//
// Make `schema` a dictionary-encoded field: `schema` itself becomes the INDEX
// array of type `index_type`, and the returned handle is the value ("dictionary")
// slot into which the caller emits the VALUE type. Emitting the index type before
// allocating the dictionary is part of the contract.
virtual SchemaRef SetTypeDictionary(SchemaRef schema, ArrowType index_type) = 0;
```

Why one method that returns a handle, rather than passing the value `ArrowType`
in or exposing `AllocateDictionary` + a navigator:

* the value type is then emitted by **`EmitScalarType`, the same code that emits
  a plain scalar** — so locked #3 ("value type derived from the field") holds for
  every `LogicalKind`, including `TIMESTAMP`/`DURATION`, with no second mapping
  to keep in step;
* the "index first, then allocate" ordering cannot be got wrong by a caller;
* both sinks receive the identical call, so the two outputs cannot drift — this
  is the "agree by construction, not by lockstep" property the story requires.

**Normative operation sequence** (both sinks, in this order):

1. set the **index** type on `schema` (its `format` becomes an integer);
2. `ArrowSchemaAllocateDictionary(schema)`;
3. `ArrowSchemaInit(schema->dictionary)`;
4. (caller) set the **value** type on `schema->dictionary`.

Step 3 is **required, not defensive**: `ArrowSchemaAllocateDictionary`
(`nanoarrow.c:1142-1154`) sets only `release = NULL`, while
`ArrowSchemaSetFormat` (`nanoarrow.c:1050`) dereferences `schema->private_data`
— uninitialised without the `Init`. nanoarrow's own `ArrowSchemaDeepCopy` does
exactly Allocate-then-Init (`nanoarrow.c:1194-1201`). Step 1 before step 2 is
safe because `ArrowSchemaSetType` deliberately does not touch `->dictionary`
(`nanoarrow.c:758-760`, with the comment saying so); the order is pinned anyway
so the trace-parity test has a fixed expectation.

`NanoarrowSchemaSink` wraps the allocate in the existing `CheckNa(...,
"allocate dictionary")`. `CppSchemaSink` interns `Expr(schema) + "->dictionary"`
as the returned ref and renders the three calls with the existing `indent_`,
discarding return codes exactly as the surrounding generated lines already do:

```cpp
ArrowSchemaSetType(schema.get()->children[0], NANOARROW_TYPE_INT16);
ArrowSchemaAllocateDictionary(schema.get()->children[0]);
ArrowSchemaInit(schema.get()->children[0]->dictionary);
ArrowSchemaSetType(schema.get()->children[0]->dictionary, NANOARROW_TYPE_STRING);
```

### D3 — `DictionaryIndexKind -> ArrowType` lives in the visitor TU

A file-local `ArrowType DictionaryIndexArrowType(ir::DictionaryIndexKind)` in
`cpp_backend_schema_visitor.cpp`'s anonymous namespace, an exhaustive 4-case
switch with **no `default`** (mirroring `DictionaryIndexArrowTypeExpr`'s
rationale in `cpp_backend_type_table.cpp:135-153`).

Not in `cpp_backend_type_table.hpp`, deliberately: `ArrowType` is a nanoarrow
enum, not a C++/Arrow type *string*, the table header does not include nanoarrow,
and the existing `LogicalKind -> ArrowType` mapping already lives in this TU by
design (`cpp_backend_schema_visitor.hpp:18-20`: "the LogicalKind -> nanoarrow
physical type mapping lives in the C++ sink, never on an IR node"). GIR locked #1
is satisfied — no type token reaches an IR node.

Do **not** reuse DICT-2's `cpp_backend::DictionaryIndexArrowTypeExpr`: that
returns `"arrow::int8()"`-style Arrow-C++ text, whose named consumer is round
RIR's accessor type gate (spec §5.1). Two spellings of the same fact, each in the
one place its consumer needs it.

### D4 — `ArrowTypeName` must gain `INT8` and `INT16` (the one asymmetry the sink abstraction does not cover)

`ArrowTypeName` (`cpp_backend_schema_visitor.cpp:124-155`) has no `INT8`/`INT16`
case and its `default:` returns `"NANOARROW_TYPE_UNINITIALIZED"`. An `INT8` or
`INT16` index would therefore render as an **uninitialised type in the generated
C++ source while the nanoarrow sink is correct** — the sinks share the *visitor*,
not this rendering table, so this is the single place where the two paths can
still diverge. Add both cases.

Test consequence, and it is mandatory: **the forcing test must exercise a
non-`int32` index through the generated-source path.** An `int32`-only fixture
leaves this defect green.

### D5 — Nullability, `ordered`, name and metadata

* Nullability, name and field metadata continue to be applied by `Visit()` to the
  **outer (index)** schema after the type, unchanged. That is where Arrow reads a
  field's nullable flag and where `field_number`/`field_id` belong. Spec §4's
  "nullability is preserved" therefore needs no new code — it falls out of
  `sink_.SetNullable(child, f.node->facts.nullable)`.
* The value ("dictionary") schema receives a **type and nothing else** — no name,
  no nullable overlay, no metadata.
* `ARROW_FLAG_DICTIONARY_ORDERED` is **never emitted**, and
  `facts.dictionary_ordered` is **never read by emission**. Locked #8 rejects
  `ordered: true` at the front end (R3); the runtime re-fold
  (`BuildDictionaryColumn`) produces a first-seen-order, unordered dictionary, so
  emitting the flag would declare something the runtime cannot produce. This is
  also what makes the `ordered`-leaking holes in D8 harmless rather than wrong.

### D6 — Everything else stays value-typed (locked #6, #7)

Audited writers, all **unchanged**:

| Path | Why untouched |
|---|---|
| edge encode/decode visitor (`cpp_backend_type_table.cpp`) | reads `scalar.*`; wire is the value type, one per row (locked #6) |
| server-tier Arrow view (`cpp_backend_view_visitor.cpp`) | emits per-**row** scalar/list types (`arrow::MakeNullScalar(sc.arrow_type_expr)`, `arrow::list(arrow::field("item", …))`). Row-oriented by definition; the codec accepts a value scalar for a dictionary field on encode (`codec.cpp:389-395`) and unwraps on decode (`scalar_codec.cpp:305-311`). Spec §5 requires value-typed here |
| TypeScript descriptor | value-type `WireTypeId` (locked #7; DICT-5 verifies) |
| RBA C++/Rust accessor emitters | zero diff (locked #11, GIR locked #3); DICT-1.5's guard covers the gap |
| `ArrowTypeExpr` (`generator.cpp:260`) | **audit result: dead.** Repo-wide grep finds only its definition and its `generator_internal.hpp:66` declaration — no call site. Not a third schema-type site. Leave it; deleting it is nobody's task this round (spec §5 note) |
| `arrow-bridge`, `pubsub`, `pubsub-arrow`, vendored nanoarrow | zero diff (locked #6) |

### D7 — The IPC backend cannot encode a dictionary type: a front-end rejection, not a late throw

**Finding, in the vendored source** —
`protoc/third_party/nanoarrow/nanoarrow_ipc.c:28246-28248`:

```c
case NANOARROW_TYPE_DICTIONARY:
  ArrowErrorSet(error, "IPC encoding of dictionary types unsupported");
  return ENOTSUP;
```

`ArrowIpcEncodeField` (`:28298`) also never encodes `schema->dictionary`. So
`SerializeSchemaIpc` throws for any schema containing a dictionary column. Three
consequences, all of which DICT-3 must handle:

**(a) `--fletcher_opt=ipc` fails, and today it would fail badly.** Without a
guard the failure surfaces at `generator.cpp:2018-2024` as
`"failed to build IPC schema for 'X': SerializeSchemaIpc: write schema: IPC
encoding of dictionary types unsupported"` — *after* the C++ header, the view
header and the TS file have already been written (they precede the IPC loop),
i.e. a partial-output failure carrying a nanoarrow-internal message with no
remedy. Fix: reject at the front end, before any artifact.

**Design:** extend the existing backend-availability pass — do **not** add a
parallel mechanism (DICT-1.5 scope rule):

```cpp
bool ValidateBackendsSupportFields(const FileDescriptor* file, bool emit_accessor,
                                   bool emit_rust, bool emit_ipc, std::string* error) {
    if (!emit_accessor && !emit_rust && !emit_ipc) return true;
    for (msg : OrderedMessages(file)) {
        if (IsRecursive(msg) || IsFlattenedWrapper(msg)) continue;
        for (field : msg->fields) {
            auto node = ir::BuildFieldIr(field);
            // GIR-10: RBA-ONLY. The IPC backend DOES support nested lists —
            // widening this to emit_ipc would reject working protos.
            if (emit_accessor || emit_rust)
                if (auto e = FindScalarLeafNestedList(node)) { *error = …; return false; }
            if (auto e = FindDictionaryField(node)) {
                // name EVERY requested-and-unsupported backend in ONE message
                *error = …; return false;
            }
        }
    }
    return true;
}
```

Three details that a naive edit gets wrong, so they are normative:

1. **The nested-list check must stay conditioned on `(emit_accessor ||
   emit_rust)`.** Its own message says the IPC backend supports nested lists, and
   the `protoc-arrow-bridge` harness compiles nested-list protos with
   `--fletcher_opt=ipc` today. Widening it reds existing tests.
2. **The dictionary complaint must fire for `ipc` alone**, and its text must name
   which backend(s) are at fault — RBA/Rust ("tracked for round RIR"), IPC
   ("nanoarrow's IPC encoder rejects dictionary types"), or both — in a single
   error rather than firing twice.
3. **The remedy differs per backend** and must be stated: for `ipc`, "regenerate
   without `--fletcher_opt=ipc`; the schema is still available from the generated
   `<Cls>Schema()`".

**Over-approximation is correct here, and this is the licensed exception.**
`FindDictionaryField` ORs over the whole reachable subtree (list elements at
every level, map key/value, struct fields — `generator.cpp:1734-1748`). Spec
§7.1.1's closing paragraph explicitly licenses that shape for a
**backend-availability** predicate and forbids it for kind/emission decisions.
Here it is not merely licensed but *right*: a dictionary declared on a field of a
struct-typed (including imported) child genuinely does reach the `.ipc` through
the deep copy, so the subtree reach matches the failure set. It also means the
P1-C hole (D8, row 4) is **closed for `--fletcher_opt=ipc`**: the walk descends
into `StructNode.fields`, so `Top {repeated NestWrap xs}` over an `Inner`
carrying the declaration is rejected from the importing file too.

**(b) DICT-1.5's guard message is now factually wrong.** `generator.cpp:1836-1837`
reads "…(the edge, Arrow view, IPC schema and TS backends accept dictionary
fields)". Correct it in the same change, along with the two block comments that
repeat the claim (`generator.cpp:1808-1809`, `:1976-1977`). A guard that states a
false fact is worse than no comment.

**(c) The story's build-wiring bullet is wrong and is corrected here.**
`integration-tests/protoc-arrow-bridge/CMakeLists.txt:60-107` passes
`--fletcher_opt=ipc` to **every** stem in `PROTO_STEMS`. Adding `dictionary`
there would fail the harness configure/build. **`dictionary.proto` gets its own
`add_custom_command` without the `ipc` token**, following the existing
`option_metadata` (`:141`) and `_ACCESSOR_STEMS` (`:178`) precedent in the same
file, plus its own `add_custom_target` and an `add_dependencies` edge on
`integration_tests`. It also must omit `accessor` / `rust` (DICT-1.5).

### D8 — THE TIMING CONSTRAINT: resolved, with the chain-following follow-up NOT a prerequisite

DICT-2 handed forward: "the three disclosed holes stop being harmless at DICT-3 …
the chain-following follow-up should land before or with DICT-3." That was
written when nothing read `facts.dictionary`. Re-assessed against what DICT-3
actually emits, hole by hole:

| Hole (spec §7.1.1) | Where the fact sits when emission runs | What DICT-3 emits | Verdict |
|---|---|---|---|
| **S2**, repeated inner field — `M{map<string,W> m}` over `W{flatten; repeated string vals [(dictionary)={INT8}]}` | top-level node for `W.vals` is a **LIST** | kind gate false → `list<utf8>`, **byte-identical to pre-DICT-3** | benign; the mandated kind gate *is* the mitigation |
| **S2**, singular inner field with `ordered: true` | `SCALAR` + fact | `dictionary(int32, utf8)`, unordered | well-formed, and identical to what `BuildDictionaryColumn` produces. The loss is the `ordered` **diagnostic**; v1 honours `ordered` nowhere (D5) |
| **gap-2's sibling** — `M{W w}` / `W{flatten; repeated V vs}`, declaration on `V`'s field | the fact is **never read into the IR** (`BuildFlattenedRepeated` reads `BaseFacts(W.vs)` and no deeper) | nothing to gate — today's bytes | benign by gap 2's own construction argument, unchanged by DICT-3 |
| **P1-C** — struct leaf behind a `(fletcher.flatten)` wrapper hop, `Inner.k` illegal | `SCALAR` + fact, inside `InnerSchema()` | `dictionary(idx, value)` in `InnerSchema()`; `TopSchema()` deep-copies it | the diagnostic cannot be escaped by a build that compiles: `Inner` is judged **directly** as a member of `OrderedMessages(<its own file>)`, and `TopSchema()`'s `ArrowSchemaDeepCopy(InnerSchema()…)` only compiles if that file was also generated. Residual cost = *which* invocation reports it. **Additionally closed for `ipc`** by D7 |

**Conclusion.** There is **no shape in which the emission gate is true and the
emitted column type is malformed.** Each hole resolves to one of: (i) the fact
sits on a non-`SCALAR` top-level node → the kind gate reproduces today's bytes
exactly; (ii) the fact never entered the IR → nothing to emit; (iii) the fact
sits on a genuine scalar, where `dictionary(idx, value)` is the *correct* column
type and the only unmet declaration is `ordered`, which nothing in v1 honours.
The holes therefore keep costing exactly what they cost today — a missing
diagnostic — and do **not** become silently wrong column types.

**So DICT-3 proceeds without the chain-following follow-up.** That follow-up
remains worth landing (it is a diagnostic-quality item, and round **RIR**'s
accessor genuinely *reads* the column type, so it should land before RIR), but it
is not a prerequisite here. What DICT-3 *does* take on, because it is the one
place where a silence really does become a hard failure the day this lands, is
the `ipc` guard (D7).

**Two conditions make that conclusion binding, and both must be pinned by a
mutation-sensitive test — not left as prose:**

1. **The gate stays at the top-level field position (D1).** Move it into
   `EmitNodeType` and S2's LIST shape emits `list<dictionary(idx, utf8)>`
   immediately. This cannot be pinned from the integration harness, because
   DICT-2's R2 rejects such a proto at the plugin front end. It must be a **unit
   test that calls `BuildMessageSchemaIntoFromIr` directly**, bypassing the
   validation pass (see Tests, U3).
2. **`facts.dictionary_ordered` is read by no emitter (D5).** Same reasoning:
   unit-level, bypassing R3 (Tests, U4).

### D9 — Byte-identity of the no-rules path

The gate's `else` branch is literally today's `EmitNodeType(*f.node, child)`, and
`facts.dictionary` is `false` on every node of a proto with no dictionary rules.
The emitted operation stream for every existing fixture is therefore unchanged
call-for-call, which is what keeps the 10 committed `protoc/tests/golden/*.ipc`
files byte-identical. Pinned by: the existing `SchemaVisitor` golden cases
(`test_schema_visitor.cpp:252-266`, 10 byte comparisons),
`integration-tests/protoc-arrow-bridge/tests/test_ipc_parity.cpp`, and the RBA
`AccessorTest` no-drift suite. A diff in any of them means the option leaked into
the no-rules path.

**Do NOT add a dictionary case to `test_schema_visitor.cpp`'s `Cases()`** — that
path runs through `SerializeSchemaIpc` and would fail with the ENOTSUP of D7,
which is expected behaviour, not a bug to chase.

### Concurrency / safety

No new state. `SchemaVisitor` and both sinks remain per-`Generate()` stack
locals; the `OptionMetadataResolver` memoisation contract
(`cpp_backend_schema_visitor.hpp:182-187`, single-threaded, per-`Generate()`) is
untouched. `CppSchemaSink::exprs_` stays a `std::deque` so the new
`"…->dictionary"` interned string keeps a stable address like every other
`SchemaRef`. Error paths: the nanoarrow sink throws via the existing `CheckNa`
(caught at `generator.cpp:2021`); the C++-source sink cannot fail. Memory: the
allocated `schema->dictionary` is released by nanoarrow's own
`ArrowSchemaReleaseInternal`, which the `Init` in step 3 installs — which is a
second reason step 3 is not optional.

---

## Forcing-test mapping

### `DictionaryTest.SchemaIsDictionaryType` (new, `integration-tests/protoc-arrow-bridge/tests/test_dictionary.cpp`)

New fixture `integration-tests/protoc-arrow-bridge/proto/dictionary.proto`
(`syntax = "proto3"; package integration; import "fletcher/options.proto";`),
shaped to cover the *class*, not one instance:

```proto
message DictionaryEvent {
  string category = 1 [(fletcher.dictionary) = {}];                                // int32 idx, non-nullable
  optional string region = 2 [(fletcher.dictionary) = {}];                         // nullable
  string code = 3 [(fletcher.dictionary) = { index_type: DICTIONARY_INDEX_INT16 }];// non-int32 idx -> pins D4
  int32 kind = 4 [(fletcher.dictionary) = { index_type: DICTIONARY_INDEX_INT8 }];  // non-utf8 value + int8 idx
  google.protobuf.StringValue tag = 5 [(fletcher.dictionary) = {}];                // WKT wrapper -> nullable dict (locked #9)
  string plain = 6;                                                                // control: no option
}
message DictionaryHolder { DictionaryEvent ev = 1; }                               // deep-copy preserves ->dictionary
```

The test uses `test_flatten.cpp`'s `ImportNano` helper shape
(`arrow::ImportSchema` over the generated `OwnedSchema`) and asserts, on
`DictionaryEventSchema()`:

| Assertion | Design element it turns green |
|---|---|
| `category`: `type()->id() == arrow::Type::DICTIONARY`, index `INT32`, value `STRING`, `nullable() == false` | D1's gate + D2's op sequence + D5's outer-schema overlay |
| `region`: same but `nullable() == true` | D5 (nullability preserved on the index schema) |
| `code`: index `INT16` | **D4** — fails on the un-extended `ArrowTypeName` (generated source renders `NANOARROW_TYPE_UNINITIALIZED`), so this row is what makes the C++-source sink honest |
| `kind`: index `INT8`, value `INT32` | D3's exhaustive index mapping + locked #3 (value derived from the field) |
| `tag`: nullable `dictionary(int32, utf8)` | locked #9 / spec §4 WKT-wrapper acceptance, reached through `TryBuildWkt`'s nullable `SCALAR` |
| `plain`: `STRING`, **not** dictionary | D9 (no leak into the no-rules path) |
| `DictionaryHolderSchema()`'s `ev` is a struct whose child `category` is still `DICTIONARY` | D1's deep-copy claim, on both sinks (`nanoarrow.c:1194-1206`) |
| `#include "dictionary.fletcher.arrow.pb.h"` and assert `ToArrowRow`'s `category` scalar is `arrow::utf8()`-typed | locked #7 / D6 — row-oriented output stays value-typed |

Build wiring per **D7(c)**: a dedicated `add_custom_command` for
`dictionary.proto` **without** `--fletcher_opt=ipc` (and without
`accessor`/`rust`), a `generate_dictionary_headers` target, an
`add_dependencies(integration_tests generate_dictionary_headers)` edge, and
`tests/test_dictionary.cpp` in the `integration_tests` sources. No new
`find_package` / link (the test needs only `fletcher-arrow-bridge` +
`fletcher-pubsub`, both already linked; `pubsub-arrow` is DICT-4's wiring).

Red-first: the test fails behaviourally before the change
(`type()->id()` is `STRING`, not `DICTIONARY`) once the fixture exists.

### Supporting tests (required — the forcing test alone under-covers)

Unit, `protoc/tests/test_schema_visitor.cpp` (uses the existing proto-text
`DescriptorPool` fixture; calls `BuildMessageSchemaIntoFromIr` / the `CppSchemaSink`
directly, so it can express shapes the front end rejects):

* **U1 `DictionaryScalarEmitsIndexOverValue`** — `children[0]->format == "i"`,
  `children[0]->dictionary != nullptr`, `->dictionary->format == "u"`,
  `(flags & ARROW_FLAG_NULLABLE)` per proto.
* **U2 `DictionaryIndexKindsCoverAllFour`** — formats `"c"`, `"s"`, `"i"`, `"l"`
  for INT8/16/32/64. Reds if any `DictionaryIndexArrowType` arm is wrong.
* **U3 `DictionaryFactOnNonScalarNodeIsNotEncoded`** — the **kind-gate and
  position pin** (D8 condition 1). `repeated string vals = 1
  [(fletcher.dictionary) = {index_type: DICTIONARY_INDEX_INT8}]` →
  `children[0]->format == "+l"`, `children[0]->dictionary == nullptr`, **and**
  `children[0]->children[0]->dictionary == nullptr`. Deleting the `kind == SCALAR`
  term reds the first; moving the gate into `EmitNodeType` reds the second. This
  is the S2 pin, and it is the one test whose absence would make D8's conclusion
  unverified prose.
* **U4 `DictionaryOrderedIsNeverEmitted`** — `ordered: true` on a scalar →
  `(children[0]->flags & ARROW_FLAG_DICTIONARY_ORDERED) == 0` (D8 condition 2).
* **U5 two-sink parity** — add a dictionary fixture to the existing
  `RecordingSink` trace comparison so the C++-source and nanoarrow sinks are shown
  to issue the identical op stream. **Note the limitation the existing comment at
  `test_schema_visitor.cpp:343-356` already states**: that test compares two
  traces from the *same* visitor and so cannot catch a visitor-side mutation. It
  therefore does **not** substitute for D4's pin — add a separate assertion that
  the generated source text for the INT16 case contains `NANOARROW_TYPE_INT16`
  and never `NANOARROW_TYPE_UNINITIALIZED`.
* `RecordingSink` must implement the new pure virtual — a deliberate compile-red
  step ahead of the behavioural red.

Plugin level, `integration-tests/protoc-coverage` (D7):

* **`GenErrors.DictionaryRejectedBy_ipc`** — `coverage_dictionary.proto` +
  `--fletcher_opt=ipc` → non-zero exit, message names the field and the nanoarrow
  IPC limitation. Red-first: today it exits 0 (and after the visitor change but
  before the guard it fails late with the raw nanoarrow text — check both stages).
* **`GenErrors.DictionaryRejectedBy_ipc_structChild`** — reuse
  `coverage_dictionary_struct_child.proto` with `ipc`, pinning the subtree reach
  (and hence P1-C's closure for `ipc`).
* **`GenErrors.NestedListAcceptedByIpc`** — GIR-10's scalar-leaf-nested-list
  fixture with `--fletcher_opt=ipc` **alone** must still **succeed**. This is the
  no-false-positive guard for D7 detail 1, and it reds on the naive edit.
* **Check DICT-1.5's existing `GenErrors.DictionaryAcceptedWithoutRbaBackends`**:
  if its invocation includes the `ipc` token it will now fail. Drop the token from
  that invocation (keeping its `EXPECT_ARTIFACTS` intent) and let the new negative
  cover `ipc`. Do not "fix" it by weakening the guard.

---

## Risks / Unknowns

1. **STOP-AND-ASK — a dictionary schema cannot be announced over any IPC-based
   provider.** Spec §5 says the generated schema "is what `CreateTopic`
   publishes", and the wire providers announce it via `SerializeSchemaIpc`
   (`fastdds-pubsub-provider/src/fast_dds_pubsub_provider.cpp:367`,
   `xrcedds-pubsub-provider/src/xrce_dds_pubsub_provider.cpp:515`,
   `gateway/src/ws_session.cpp:237`) — which throws for a dictionary column
   (D7). So after DICT-3 a dictionary column is usable with in-process / mock
   providers (which is what DICT-4's forcing test and the existing
   `SubscriberArrowBatchTest.DictionaryColumn*` tests use) but **not over FastDDS
   / XRCE-DDS / the WebSocket gateway.** This is not fixable inside locked #6:
   the fix is a change to the vendored nanoarrow IPC encoder, in **two** copies
   that must stay byte-compatible (`protoc/third_party/nanoarrow/` and the
   runtime's). Note that `pubsub/src/publisher.cpp:48-51` already carries a
   comment accepting exactly this limitation, so the codebase has met it before.
   **Ask:** confirm the round documents this as a v1 limitation (my
   recommendation — DICT-5's docs deliverable, plus a line in spec §5/§8) rather
   than taking on the nanoarrow encoder. DICT-3's forcing test does not depend on
   the answer; DICT-4's transport choice does.

2. **`arrow::ImportSchema` and an unnamed dictionary value schema.** D5 leaves the
   value schema's `name` at nanoarrow's default (`NULL`). Arrow's C-bridge
   importer uses the dictionary child for its *type* only, and tolerates a null
   name for fields generally — but this was not verified against the pinned Arrow
   version (headers are a Conan dependency, not in-tree). If the forcing test's
   `ImportSchema` errors, the fix is one line in `SetTypeDictionary`: also emit
   `SetName(value, "")`. Both sinks get it from the one sink method, so the
   fix cannot desync them.

3. **`ArrowSchemaAllocateDictionary` availability in consumers.** It is public
   nanoarrow API (`nanoarrow.h:1562`) and macro-namespaced identically to
   `ArrowSchemaInit` / `ArrowSchemaSetType`, which the generated header already
   calls — so a consumer that links today links this too. Low risk; if a
   consumer's vendored nanoarrow predates it, that is a pre-existing version-skew
   problem, not a DICT-3 one.

4. **Pre-existing, dictionary-independent finding — do not fix here.** A
   `(fletcher.flatten)` wrapper used as a **map value or struct child** appears to
   produce a non-compiling header today: `IsFlattenedWrapper(msg) =
   HasMessageFlatten && field_count() == 1` (`type_mapper.cpp:651`), no schema
   function is generated for a wrapper (`generator.cpp:1360`, and the comment at
   `:1772` states it), yet `BuildMapNode` → `MakeStructNode(W)` makes the parent
   emit `ArrowSchemaDeepCopy(WSchema().get(), …)`. That likely explains why the S2
   repro was only ever observed as "rc 0" and never as a usable artifact. **The
   design does not depend on this**: D8 row 1's argument (kind gate → byte-identical
   `list<utf8>`) stands on its own. Flagged for the round's backlog; it is a GIR-level
   issue and out of DICT-3's scope.

5. **Assumption:** `FindDictionaryField`'s descent (`generator.cpp:1734-1748`) is
   unchanged by this item and still ORs over LIST / FIXED_SIZE_LIST / MAP /
   STRUCT. D7's reach argument depends on it; if a reviewer finds an edge missing,
   the `ipc` guard under-approximates in the same way DICT-1.5's does, and the
   remedy is the same (add the edge to all three walks together, per the
   forward-compat note at `generator.cpp:1727-1730`).

6. **Not verified, low consequence:** whether the `protoc-coverage` harness's
   guard-check driver (`cmake/run_backend_guard_check.cmake`) needs a new
   parameter to pass the `ipc` token. If it hardcodes the option string, it needs
   a small parameterisation; the design does not otherwise depend on its shape.

**No locked decision is deviated from.** #6 (wire byte-identical), #7 (only
schema emission branches) and #5 (`SCALAR` + modifier, no new `FieldKind`) are all
honoured; #3/#4 are honoured by deriving the value type through `EmitScalarType`
and the index through `facts.dictionary_index_kind`; #8 by never emitting the
ordered flag; #9/#11 are untouched. No public-API change: `SchemaSink` is an
internal protoc header, and the only new pure virtual lands on a class whose
implementors are the two sinks plus the test `RecordingSink`.

---

## Files-to-touch

**Modify**

* `protoc/include/cpp_backend_schema_visitor.hpp` — `SchemaSink::SetTypeDictionary`
  pure virtual (+ contract comment); the two sink declarations;
  `SchemaVisitor::EmitTopLevelFieldType` private declaration.
* `protoc/src/cpp_backend_schema_visitor.cpp` — `ArrowTypeName` `INT8`/`INT16`
  cases (D4); file-local `DictionaryIndexArrowType` (D3);
  `CppSchemaSink::SetTypeDictionary` and `NanoarrowSchemaSink::SetTypeDictionary`
  (D2); `EmitTopLevelFieldType` and the one-line change in `Visit()`'s loop (D1).
* `protoc/src/generator.cpp` — `ValidateBackendsSupportFields` gains `emit_ipc`
  (D7), the dictionary message names the faulting backend(s) and the correct
  remedy, the nested-list branch is scoped to `accessor || rust`, the call site at
  `:1978` passes `emit_ipc`, and the three now-false "IPC schema … accept
  dictionary fields" claims (`:1808-1809`, `:1836-1837`, `:1976-1977`) are
  corrected.
* `protoc/tests/test_schema_visitor.cpp` — `RecordingSink` override; U1–U5.
* `integration-tests/protoc-arrow-bridge/CMakeLists.txt` — dedicated
  `dictionary.proto` generation command **outside** `PROTO_STEMS` and without
  `ipc`; `generate_dictionary_headers` target; `add_dependencies`;
  `tests/test_dictionary.cpp` in `integration_tests`.
* `integration-tests/protoc-coverage/CMakeLists.txt` (and
  `cmake/run_backend_guard_check.cmake` if it hardcodes the option token) — the
  three new `GenErrors` ctests; adjust `DictionaryAcceptedWithoutRbaBackends`'s
  option list if it carries `ipc`.
* `docs/dictionary-option-spec.md` — §5: the actual emission mechanics (index on
  the field, value on `schema->dictionary`, ordered flag never set) and the new
  **`--fletcher_opt=ipc` incompatibility** with its nanoarrow evidence; §7.1.1
  "Consequence for DICT-3": record the resolution in D8 (holes stay
  missing-diagnostic, not wrong-type) and the two tests that pin it; §8: the
  IPC-announcement limitation from Risk 1.
* `docs/fletcher-options.md` — `(fletcher.dictionary)` is incompatible with
  `--fletcher_opt=ipc` as well as `accessor`/`rust`.
* `plans/DICT-dictionary-option.md` — DICT-3 scope: correct the "add `dictionary`
  to `PROTO_STEMS`" build-wiring bullet, record the `ipc` guard, mark the
  `ArrowTypeExpr` audit closed (dead, no call sites).

**Add**

* `integration-tests/protoc-arrow-bridge/proto/dictionary.proto`
* `integration-tests/protoc-arrow-bridge/tests/test_dictionary.cpp`
* (possibly) one `integration-tests/protoc-coverage/proto/` fixture if the
  existing `coverage_dictionary*.proto` set does not cover the `ipc` negatives —
  prefer reuse.

**Must have ZERO diff** (a diff is a stop-and-ask)

`protoc/src/recordbatch_accessor_emitter.*`, all Rust accessor emission,
`protoc/src/ir.cpp` + `protoc/include/ir.hpp`, `protoc/src/option_reader.cpp`,
`protoc/src/type_mapper.{hpp,cpp}`, `protoc/include/fletcher/options.proto`,
`protoc/src/cpp_backend_view_visitor.cpp`, `protoc/src/cpp_backend_type_table.*`,
`protoc/third_party/nanoarrow/**`, `protoc/tests/golden/*.ipc`, `arrow-bridge/**`,
`pubsub/**`, `pubsub-arrow/**`.
