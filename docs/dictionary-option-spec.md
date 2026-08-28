# Dictionary Column Option — Specification

Authoritative spec for the `(fletcher.dictionary)` protoc option, which lets a
`.proto` field declare that its generated Arrow column is **dictionary-encoded**.

This is the `spec_path` for the `DICT` runbook round. Per-item design docs
(`plans/DICT-*.md`) and the implementation derive from this document; the
firm choices it depends on are recorded in
[plans/DICT-locked-decisions.md](../plans/DICT-locked-decisions.md).

> **Post-#96 sync (2026-06-12).** This spec was drafted against the pre-#96
> tree. PR #96 (`--fletcher_opt=ipc`) is now merged to `main` and pulled into
> this branch. It **relocated** the options file to
> `protoc/include/fletcher/options.proto`, **rewrote** `protoc/src/generator.cpp`
> (+273/−54), **vendored** nanoarrow into `protoc/third_party/`, and added
> `protoc/tests/test_schema_builder.cpp`. Extension **50001 is still free**.
> The `file:line` references below predate #96 and are *indicative only* —
> re-confirm them at design time; in particular DICT-3's schema-emission hook
> must be re-derived (the IPC feature shares that code path).

---

## 1. Motivation

The runtime already supports dictionary columns end-to-end: the batched
`PubSubArrow::Subscribe` path re-folds per-row value scalars into a real
`arrow::DictionaryArray` in
[subscriber_arrow.cpp:198-229](../pubsub-arrow/src/subscriber_arrow.cpp#L198-L229),
and the codec accepts a dictionary field supplied as either a `DictionaryScalar`
or a plain value scalar
([codec.cpp:378-389](../arrow-bridge/src/codec.cpp#L378-L389)). What is missing
is a way to **declare** a dictionary column from a `.proto`: today the only way
to get one is to hand-author an `arrow::schema(...)` in C++.

This feature closes that gap. It is purely a **generator** change plus a small
options addition — the wire format and the runtime re-fold logic are unchanged.

## 2. Surface syntax

A new field-level option, defined in
[protoc/include/fletcher/options.proto](../protoc/include/fletcher/options.proto):

```proto
enum DictionaryIndexType {
    DICTIONARY_INDEX_UNSPECIFIED = 0;   // resolves to int32
    DICTIONARY_INDEX_INT8        = 1;
    DICTIONARY_INDEX_INT16       = 2;
    DICTIONARY_INDEX_INT32       = 3;
    DICTIONARY_INDEX_INT64       = 4;
}

message DictionaryOptions {
    optional DictionaryIndexType index_type = 1;   // default → int32
    optional bool               ordered     = 2;   // v1: must be false/unset (see §6)
}

extend google.protobuf.FieldOptions {
    // 50000 is taken by flatten_field on FieldOptions — use 50001.
    optional DictionaryOptions dictionary = 50001;
}
```

Usage:

```proto
message Event {
  string category = 1 [(fletcher.dictionary) = {}];                              // int32 index
  string code     = 2 [(fletcher.dictionary) = { index_type: DICTIONARY_INDEX_INT16 }];
  optional string region = 3 [(fletcher.dictionary) = {}];                        // nullable dictionary
}
```

The empty-message form `= {}` is the canonical "make this a dictionary, defaults"
trigger; presence is detected with `HasExtension`, mirroring how `flatten` uses
presence rather than a truthy value.

## 3. Value type and index type

- **Value type is derived from the field**, never specified in the option. A
  `string` field → `dictionary(<index>, utf8())`; an `int32` field →
  `dictionary(<index>, int32())`; an `enum` field → `dictionary(<index>, int32())`.
  This keeps the option from contradicting the field's own type.
- **Index type** comes from `index_type`, defaulting to `int32` (which is also
  what `arrow::compute::DictionaryEncode` produces, so the common case needs no
  cast at re-fold time). `INT8`/`INT16`/`INT64` are honored by the runtime's
  declared-type `Cast` at
  [subscriber_arrow.cpp:215-227](../pubsub-arrow/src/subscriber_arrow.cpp#L215-L227).

## 4. Validation rules (enforced at codegen)

The plugin must reject any use that the runtime cannot honor, rather than emit a
schema that fails downstream.

> **DICT-2 (landed): enforcement is ONE FATAL FRONT-END PASS, not a projection
> `nullopt`.** `ValidateDictionaryDeclarations` (`generator.cpp`, rules in
> `type_mapper.cpp`: `DictionaryUnsupportedReason` / `FindIllegalDictionaryField`)
> sets protoc's `*error` and fails the plugin before any artifact is written, for
> **every** option set including `schema_only`. It runs after
> `ValidateNoUnsupportedIr` (#55) and **before** `ValidateBackendsSupportFields`
> (DICT-1.5), whose "regenerate without `--fletcher_opt=accessor,rust`" remedy is
> misleading advice for an illegal declaration.
> Rejection is deliberately **not** `MapField -> nullopt`: `nullopt` becomes a
> `skipped_comment` and generation continues at exit 0 (a silent dropped column),
> and the schema walk does not call the projection at all — it uses
> `IsSchemaRepresentable`, a by-hand mirror — so a projection-level rejection
> would drift the generated row header against the emitted schema. `MapField`
> stays total. Read locked decision #9's "with a clear `UnsupportedReason`" as
> "with a clear reason string", not as "routed through the literal
> `UnsupportedReason()` function".

The rule is keyed on the **mapped `FieldKind`**, not the raw proto type: the
option is **allowed exactly when `MapField` resolves the field to
`FieldKind::SCALAR`**, and rejected otherwise. This is the robust formulation
because `MapField` already folds singular scalars, enums, **and well-known
wrapper messages** into `FieldKind::SCALAR`.

| Field shape | Result |
|---|---|
| singular scalar (`bool`/int/uint/float/double/`string`/`bytes`/`enum`) | **allowed** (maps to `SCALAR`) |
| well-known wrapper message (`google.protobuf.StringValue`, `Int32Value`, …) | **allowed** — `MapField` → `ir::BuildFieldIr`/`TryBuildWkt` → `ProjectIrToFieldMapping` maps it to a *nullable* `SCALAR` of the inner type (post-GIR-3 there is no `MapWellKnown`); yields a nullable `dictionary(<idx>, <inner>)` |
| singular **struct** message field | **rejected** — maps to `STRUCT`; dictionary value type must be primitive/scalar |
| `repeated` field | **rejected** — `list(dictionary(...))` is not supported by the codec re-fold |
| `map<K,V>` field | **rejected** — same reason |
| `oneof` member | **rejected** with its own wording — the field has no Arrow mapping at all (in practice #55 reports the unsupported type first) |
| `ordered: true` | **rejected in v1** (see §6) |
| `(fletcher.flatten_field)` **wrapper field** carrying the option | **rejected** — the wrapper's fields are inlined as separate columns, so there is no single column to dictionary-encode (§7.1 gap 1). **Includes a WKT wrapper field** carrying `flatten_field`: see the note below |
| two declarations on one singular `(fletcher.flatten)` chain that **disagree** | **rejected** — leaf-wins would discard the author's own annotation without a trace. *Agreeing* declarations (including spelling differences: `= {}`, `UNSPECIFIED` and `INT32` are equal) are silent and legal |
| a declaration on the **inner** field of a **single-field** `(fletcher.flatten)` wrapper reached through a `repeated` field | **rejected** — the resulting column is a list (§7.1 gap 2) |

> **WKT wrappers are deliberately allowed.** A `StringValue` field is already a
> nullable string at the schema/codec/runtime level, so `dictionary(<idx>, utf8)`
> nullable is exactly consistent with how the option behaves on `optional string`.
> Gating on `field->type() == TYPE_MESSAGE` would wrongly reject it; gate on the
> mapped kind instead. Add a forcing test for a wrapper field (DICT-2).

`[(fletcher.flatten_field)]` is a no-op on **scalar** fields, so a scalar carrying
both options resolves to dictionary; document, do not error. The rejection above
therefore carries the **full three-term** predicate the two inlining walks use
(`TYPE_MESSAGE && !is_repeated && HasFieldFlatten`), never `HasFieldFlatten`
alone.

> **Do NOT exclude WKT wrappers from the `flatten_field` rejection.** It looks
> like the acceptance case above, and it is not: `ir::BuildFieldIr` routes a
> `StringValue` field through `TryBuildWkt` to a nullable `SCALAR` **carrying**
> the dictionary, while both inlining walks gate only on
> `TYPE_MESSAGE && !is_repeated && HasFieldFlatten` and so inline
> `StringValue.value` as a plain non-nullable `utf8` column with no dictionary.
> That is the same "accepted by the kind gate / dropped by emission" class as the
> `flatten_field`-over-`flatten`-wrapper shape. A WKT wrapper **without**
> `flatten_field` is the acceptance case and stays legal.

Nullability is preserved: `optional string region = 3 [(fletcher.dictionary)=...]`
yields a **nullable** dictionary field (the runtime preserves nulls through the
re-fold — see `DictionaryColumnPreservesNulls`).

## 5. Wire / schema contract (the core of the design)

A dictionary is a *columnar* optimisation; per-row it is just the value. The
change is therefore almost entirely localized to **schema emission** — every
row-oriented path keeps treating the field as its value type:

| Concern | Behaviour for a dictionary field |
|---|---|
| `FieldKind` | stays `SCALAR` — the field's `ScalarTypeInfo` (storage, setter, getter, builder, `scalar_ctor`) is the **value type**, unchanged |
| Wire bytes | the **value type**, one value per row — byte-identical to the same field without the option (`CppWireTypeIdHex` = value type's id) |
| Generated schema (`<Cls>Schema()` C++ source **and** the `--fletcher_opt=ipc` runtime builder) | the field becomes `dictionary(<index>, <value>)`, nullability preserved — this is what `CreateTopic` publishes and what drives the batched re-fold. **Two emitters** carry this: `EmitNanoarrowTypeSetup` (generated source) and `BuildMessageSchemaInto` (IPC runtime builder); both must branch on `is_dictionary` (see the implementation hook below) |
| Server Arrow schema / `ToArrowRow` per-row scalars | **value-type** scalars (the codec accepts value scalars for dictionary fields on encode — [codec.cpp:389-395](../arrow-bridge/src/codec.cpp#L389-L395) — and unwraps the dictionary type to its value type on decode — [scalar_codec.cpp:305-311](../arrow-bridge/src/scalar_codec.cpp#L305-L311)) |
| Batched `PubSubArrow::Subscribe` | re-folds value scalars into a real `DictionaryArray` of the declared type — **existing** runtime, no change |
| TypeScript descriptor | the field's **value-type** `WireTypeId` — TS clients receive plain values; there is no client-side re-fold |

> **SUPERSEDED (DICT-2 finding, grounded on this tree).** The "**two** schema
> emitters" claim below is **stale post-GIR-5**. `GenerateSchemaFunction` ignores
> its `fields` argument and delegates to
> `cpp_backend::GenerateSchemaFunctionFromIr`, and `BuildMessageSchemaInto`
> delegates to `cpp_backend::BuildMessageSchemaIntoFromIr` — **one visitor
> (`cpp_backend::SchemaVisitor`), two sinks**. `EmitNanoarrowTypeSetup`,
> `SetScalarSchemaType`, `SetMetadataPairs`, `RequireNestedMsg` and
> `ArrowTypeExpr` in `generator.cpp` are **definition-only** (dead) post-GIR-5.
> DICT-3 must branch **inside the visitor**, on `ir::FieldFacts.dictionary`
> **AND `kind == NodeKind::SCALAR`** (the kind gate is load-bearing — an accepted
> proto can carry the fact on a LIST node; see §7.1.1), and
> must **not** "fix" those dead functions; deleting them is nobody's task in this
> round. The `FieldMapping` hook in the paragraph below **did** land (DICT-2,
> `is_dictionary` + `dict_index_type_expr`), but its named consumer is round RIR's
> IR-based RecordBatch accessor (§5.1), not schema emission.

**Implementation hook:** add dictionary metadata to `FieldMapping` (e.g.
`bool is_dictionary` + `std::string dict_index_type_expr`) in
[type_mapper.hpp](../protoc/include/type_mapper.hpp); leave the `scalar`
`ScalarTypeInfo` as the value type. Among the **writers**, only the Arrow
**schema** emitters branch on `is_dictionary`; the encode/decode/getter/setter/TS
emitters are untouched because they read `scalar.*`. Post-#96 there are **two**
schema emitters and both must branch: `EmitNanoarrowTypeSetup`
([generator.cpp:900](../protoc/src/generator.cpp#L900), emits the generated
`<Cls>Schema()` C++ source) **and** `BuildMessageSchemaInto`
([generator.cpp:1225](../protoc/src/generator.cpp#L1225), the `--fletcher_opt=ipc`
runtime schema builder — scalar branch at
[generator.cpp:1241](../protoc/src/generator.cpp#L1241)). Updating only the first
leaves IPC schemas value-typed (a silent miss). The one **reader** that must also
branch on it is the generated RecordBatch accessor (§5.1). This shape also
anticipates the Phase-2 `Dictionary<V>` IR node in
[robustness-plan.md](robustness-plan.md).

### 5.1 RecordBatch accessor (RBA) reading dictionary columns

The generated `<Class>Accessor` (C++ `.fletcher.accessor.pb.h` and the arrow-rs
`.fletcher.rs`, produced by the RBA round) is the **one reader that branches on
`is_dictionary`**. It consumes the *columnar* batch — including the re-folded
`DictionaryArray` the batched `Subscribe` hands back (the §5 "Batched
`PubSubArrow::Subscribe`" row) — so, unlike the row-oriented paths above, it
cannot treat a dictionary field as a bare value column:

- **Type gate.** For an `is_dictionary` field the accessor's positional,
  type-only gate expects `dictionary(<index>, <value>)` (C++
  `arrow::dictionary(idx, val)`, Rust `DataType::Dictionary(idx, val)`) — the
  declared `dict_index_type_expr` index over the field's value type — instead of
  the bare value type. A value-typed column where a dictionary is declared (or a
  wrong index/value type) is a validation error, never silently accepted
  (matches the accessor's existing "unexpected layout is an error" rule).
- **Getter.** The getter keeps its **value-typed signature** unchanged (e.g.
  `std::string_view` / `&str` for a utf8 dictionary, `int32_t` / `i32` for an
  int32 one) and resolves index → dictionary value per row. Consumers see the
  value type, consistent with the rest of this spec; the dictionary encoding is
  invisible at the API surface.
- **Caching / nulls / no-panic.** The dictionary array is cast-once and cached
  offset-preserving like any other column; proto-non-nullable dictionary fields
  keep the `null_count()==0` enforcement and proto-optional ones surface nulls
  via `optional`/`Option`; every failure path is a `Status`/`Result` error,
  never a panic. C++ and Rust read the same batch identically (RBA parity).

**This reuses the RBA _discipline_, not its scalar _code_.** The existing accessor
is hard-wired to concrete value arrays (`LookupScalarArray`), value-array storage,
exact value-`DataType` gates, and direct `value(row)`/`GetView(row)` getters — none
of which handle a dictionary column. DICT-6 adds **new** dictionary-specific emission
(an index-type→key-array mapping, a `dictionary(idx,val)` expected-type, a cached
`DictionaryArray` + downcast values array, and a null-then-index-resolve getter). What
carries over is the *contract* — type-only gate, cast-once cache, null guard, never
panic, cross-language parity — not the scalar helper tables. This is additive to the
RBA accessor. The RBA round explicitly defers dictionaries to DICT (RBA spec
out-of-scope; locked decision **D-RBA-9**).

> **Amendment (2026-08-28) — which ROUND delivers this section.** The requirement
> above is unchanged and still binding. What changed is its home: it is **no
> longer the final stage of round DICT**. Round GIR left the RBA accessor
> read-only, and round **RIR** migrates that emitter onto the IR — so writing
> dictionary support against the flat accessor in DICT would mean writing it
> twice, against two substrates. Therefore:
>
> * **Round DICT** ships **DICT-1.5**, a front-end guard that fails the plugin
>   when `--fletcher_opt=accessor` or `rust` is requested for a proto containing a
>   dictionary field. Nothing invalid is generated; the limitation is explicit.
> * **Round RIR** delivers this section's capability against the IR-based
>   accessor, and removes the DICT-1.5 guard in the same change.
>
> Until RIR lands, a dictionary proto must omit `--fletcher_opt=accessor,rust`.
> See DICT locked decision #11 and `plans/RIR-rba-onto-ir.md`.

## 6. `ordered` handling (v1 limitation)

`arrow::compute::DictionaryEncode` produces an **unordered**, first-seen-order
dictionary, and honoring `ordered: true` would require sorting the value array
in `BuildDictionaryColumn` (a `pubsub-arrow` runtime change, out of scope here).
To avoid emitting a schema the runtime cannot faithfully produce, v1 **rejects**
`ordered: true` at codegen. The field is kept in `DictionaryOptions` so the
surface stays forward-compatible; honoring it is a documented follow-up.

## 7. Reading the option (implementation note)

The plugin still does **not** link a generated `fletcher/options.pb.cc` (locked
decision #10), so `(fletcher.dictionary)` never arrives as a known field: the
linked-in `google::protobuf::FieldOptions` C++ class does not know the extension,
and the value therefore sits as a **length-delimited** unknown field (#50001) in
that options message's `UnknownFieldSet`.

**Mechanism: reflection over a `DynamicMessage`, not an unknown-field walker.**
The reader (`protoc/src/option_reader.cpp`) serializes `field->options()` and
re-parses those bytes into a `DynamicMessage` whose descriptor comes from the
`DescriptorPool` protoc populated from the `CodeGeneratorRequest` — which *does*
know `fletcher.dictionary`. The extension then reads as a real message field by
reflection, and its sub-fields (`index_type`, `ordered`) are located **by name**,
with `index_type` resolved to its enum **symbol name**
(`DICTIONARY_INDEX_INT16` → int16), never to a raw field/enum number. That
round-trip is the one shared primitive `ReparseOptionsWithPool`, extracted from
the identical trick `OptionMetadataResolver` already used for third-party options
(`option_metadata.cpp`) — one implementation, two typed consumers, no second
bespoke option parser. It is also why a consumer pinning an older or newer
`fletcher/options.proto` still reads correctly, and why the reader keeps working
if someone later *does* link the generated descriptor.

**Presence is the trigger** (`= {}` is a dictionary with defaults), read as
`HasField` on the re-parsed extension.

**The extension declaration is the only evidence acted on.** The pool must
resolve `fletcher.dictionary` to a singular, message-typed `FieldOptions`
extension numbered 50001; otherwise the field has no dictionary. A bare
length-delimited field at #50001 is deliberately *not* enough — 50000/50001 sit
in the collision-prone internal range, so trusting the number alone would let a
foreign option fabricate a dictionary column nobody declared.

*Granularity of that guarantee:* it is defended at **pool-declaration**
granularity, not per field. Protobuf refuses two extensions of the same extendee
at the same number in one pool, so a foreign option can never be *declared* at
50001 in a pool that also declares `fletcher.dictionary` — but once the pool does
declare it, the bytes at #50001 are interpreted as a `DictionaryOptions`
regardless of who wrote them, and the presence probe below trusts a bare
"#50001 + length-delimited" record. The wire format carries no type identity, so
no reader can do better; what makes the answer safe is that the *declaration*
comes from Fletcher's own `options.proto`.

**Fail-soft contract.** A **declared** but unreadable option resolves to the
**defaults** (int32, unordered) — never to "absent", and never to a hard error:
the reader is called from `ir::BuildFieldIr`, which has no error channel, and
dropping a declared dictionary would emit a *value-typed* column for a field the
author declared dictionary. Concretely, when the extension resolved but its blob
does not re-parse (a truncated or garbage payload), a narrow **presence probe**
looks for a length-delimited field #50001 in the untouched `UnknownFieldSet` and
answers "declared, defaults". The probe *decodes nothing* — it is not a payload
walker. These paths are unreachable from protoc-compiled input (protoc either
serializes a valid `DictionaryOptions` or fails the compile), so this is a
robustness floor rather than a routine path; a louder failure for a corrupt
payload belongs to the validation pass, which has an error channel.

Linking the generated descriptor remains the recorded alternative if Fletcher's
own option set ever outgrows reflection.

### 7.1 Known gaps at v1 — wrapper shapes where the declaration is dropped

> **STATUS (DICT-2, landed).** Both gaps are now **hard errors at the plugin's
> front end**; the **IR-level drops described below are UNCHANGED** and are still
> pinned by `TypeMapperTest.ReadsDictionaryOption`. What changed is only the
> plugin's *verdict* on such a proto:
> * **gap 1** — rejected by rule **R1**, a descriptor-level rule inside
>   `FindIllegalDictionaryField`'s raw walk (`type_mapper.cpp`), carrying the full
>   three-term wrapper predicate and **including WKT wrapper fields** (§4);
> * **gap 2** — rejected by rule **R5**, but **only for a SINGLE-FIELD wrapper
>   reached through a `repeated` field**, mirroring
>   `ir::BuildFlattenedRepeated`'s chain loop **including its
>   `field_count() == 1` term**. Both boundaries are normative — see the two
>   bullets under gap 2.
>
> The pass reaches these rules from a generated message's own fields, through
> `(fletcher.flatten_field)` wrappers, **and** through struct / list-element /
> map-value children (so an **imported** message's illegal declaration is reported
> from an importing file too). It never enters a `(fletcher.flatten)` wrapper
> message, which is the one remaining hole — §7.1.1 states the exact boundary and
> why that exclusion is required.

Both were **silent** before DICT-2, both were owned by the validation item that
owns the error channel (DICT-2), and both are pinned by sub-cases of
`TypeMapperTest.ReadsDictionaryOption`:

1. **`(fletcher.flatten_field)` wrapper field.** A wrapper field carrying both
   `flatten_field` and `dictionary` is inlined away by the field walks
   (`cpp_backend_schema_visitor.cpp`'s `BuildFlattenedFieldListImpl`,
   `generator.cpp`'s `GatherFieldsImpl`) *before* its IR node is ever built, so
   the *projection* (`ProjectIrToFieldMapping`'s output / `FieldMapping`) never
   carries the fact for this shape. Intended semantics: **reject** — a wrapper
   that inlines N columns has no single column to dictionary-encode. Enforcement
   for DICT-2's mapped-kind rejection must therefore be **a walk rooted at each
   message's own declared fields (descriptor or `ir::BuildFieldIr`) rather than a
   check on the projection's output** — the projection is never invoked for the
   wrapper. This is scoped to DICT-2; the choice between a descriptor walk and an
   `ir::BuildFieldIr` walk stays DICT-2's to make. (A *scalar* field carrying both
   is fine: `flatten_field` requires a message type, so it is a documented no-op
   and the dictionary applies.)
   **DICT-1.5's backend-availability guard is unaffected by this gap.** It calls
   `ir::BuildFieldIr` on each message's own declared fields directly (not via
   `GatherFieldsImpl`/`BuildFlattenedFieldListImpl`'s projection), so
   `BaseFacts(w)` for the wrapper field itself is read and the guard fires; see
   `plans/DICT-1.5-backend-support-guard.md` D1.
2. **Inner declaration under a `repeated` message-level-flatten wrapper.**
   `ir::BuildFlattenedRepeated` builds each of its seven return nodes from the
   **outer** field's
   facts, so `repeated W xs [(fletcher.dictionary) = {...}]` is carried on the
   outermost list and the leaf (but *not* on intermediate list levels — see the
   placement table below), while a
   dictionary declared on `W`'s single inner field is **not read** — the singular
   spelling `W w` (which resolves through `BuildFlattenedSingular` and does
   propagate) therefore disagrees with the repeated one. Not closed in v1 because
   the resulting node is always a list, which the validation item rejects as
   non-scalar anyway; the cost of the gap is that the rejection cannot *fire* for
   the inner-declared shape, so it stays quiet instead of loud.
   **DICT-2 pays that cost (R5) — narrowly.** Two boundaries are normative:
   * **A MULTI-FIELD flatten wrapper behind a `repeated` field is LEGAL and must
     stay legal.** For `repeated W xs` where `W` is
     `{option (fletcher.flatten) = true; string k = 1 [(fletcher.dictionary)]; int32 n = 2;}`,
     `BuildFlattenedRepeated` returns `List<Struct(W)>` **without entering its
     chain loop** (`msg->field_count() != 1`), `W` is not `IsFlattenedWrapper` so
     the validation pass judges `W` on its own, and `W.k`'s scalar dictionary is
     built by `BuildStructVariant` with `ir::BuildFieldIr` and therefore **is
     honoured by emission**. R5 carries `field_count() == 1` for exactly this
     reason; dropping the term would permanently outlaw a working proto.
   * **Gap 2 has a SIBLING that R5 does NOT close, and that is deliberate.** R5
     keys on the **outer field** being `repeated`; the `repeated` hop can instead
     sit **inside a singular chain** — `W w = 1;` where
     `W {flatten; repeated V vs = 1;}` and the declaration lives on `V`'s field.
     `ir::BuildFieldIr(W.vs)` routes to `BuildFlattenedRepeated`, which reads
     `BaseFacts(W.vs)` and no deeper field, so the declaration is dropped exactly
     as in gap 2; `W` is `IsFlattenedWrapper`, so the pass never judges `W.vs`
     directly and R5 cannot see it from `w`. **Safe by gap 2's own construction
     argument** (schema emission consumes the identical node and drops it too —
     the column is a plain list, DICT-1.5's guard and the accessor agree, no
     mis-read exists); the cost is the same as gap 2's original cost, i.e. the
     rejection stays quiet. **`ordered: true` is among the declarations dropped
     here**, so locked #8's "rejected at codegen in v1" is **not absolute** —
     probed: `M{W w}` / `W{flatten; repeated V vs}` /
     `V{flatten; string s [(dictionary) = {INT8, ordered: true}]}` generates at
     exit 0 with no diagnostic. Closing it would need R5 generalised to walk
     singular chains looking for repeated hops: a scope increase with **no safety
     gain**. Do not generalise R5 for this.
   **DICT-1.5's backend-availability guard also cannot see this declaration, and
   that is safe by construction rather than by luck:** schema emission consumes
   the identical node (`GatherFieldsImpl`'s inline branch requires
   `!fd->is_repeated()`), so it drops the declaration too — the emitted column is
   a plain `list<...>`, the accessor reads a `list<...>` as a `list<...>`, and no
   mis-read exists. See `plans/DICT-1.5-backend-support-guard.md` D1 ("The one
   declaration the IR cannot see, and why that is safe").

#### 7.1.1 The validation pass's detection boundary (DICT-2)

`ValidateDictionaryDeclarations` judges, for each message `M` in
`OrderedMessages(file)` that is neither `IsRecursive` nor `IsFlattenedWrapper`:

* every field `M` declares;
* every field reachable by descending `(fletcher.flatten_field)` wrappers (the
  same three-term predicate the two inlining walks use); and
* every field of a **singular-message child**, a **list element**, or a **map
  value** — recursively, but **never** entering an `IsFlattenedWrapper` or an
  `IsRecursive` message.

Those three positions are where emission deep-copies a child message's **own**
schema function — but they are **not all** of the positions where it does so, and
the difference is a disclosed hole rather than an absent one. Stated precisely:

`cpp_backend_schema_visitor` calls `DeepCopyMessageStruct` from **two** call sites
(`case NodeKind::STRUCT`, `cpp_backend_schema_visitor.cpp:449`, and
`case NodeKind::MAP`, `:468` — the `LIST` case *recurses* into `EmitNodeType`
rather than calling), reaching **four** emission positions:

| # | Emission position | Judged by the pass? |
|---|---|---|
| 1 | a singular nested struct field | **yes** |
| 2 | the element of `List<Struct>` | **yes** |
| 3 | the struct **leaf** of `List<List<...<Struct>>>` | **no** — see below |
| 4 | a map value | **yes** |

**The invariant, accurately:** the pass judges a deep-copy position when the child
message is named **directly by a field of a judged message**. It does **not** judge
a position whose child message is reached **through a `(fletcher.flatten)` wrapper
hop**, because the `!IsFlattenedWrapper(child)` term cuts the walk at the wrapper.
That excludes position 3 outright (a nested list is only constructible through a
wrapper hop, via `ir::BuildFlattenedRepeated`) and excludes positions 1/2/4
whenever a wrapper sits in between. Preserve *this* statement, not the
field-shape list — and note it is the wrapper exclusion, not the choice of
positions, that draws the line.

##### Why the wrapper exclusion is the load-bearing term

`(fletcher.flatten_field)` inlining exists in exactly two places —
`GatherFieldsImpl` and `BuildFlattenedFieldListImpl` — and both build a
**generated message's own top-level column list** (both recurse, so nested
chains are inlined too). So "is rule R1 sound at this position?" reduces to
"does some generated schema function inline this message's fields?":

| Position | Inlined by a generated schema function? | R1 sound? |
|---|---|---|
| a generated message's own field | yes, by its own `<Cls>Schema()` | **yes** |
| a field of a struct / list-element / map-value **child** | yes — the child's schema is `ArrowSchemaDeepCopy(<Child>Schema())`, built by the child's **own** inlining walk | **yes** |
| a field of an `IsFlattenedWrapper` message | **no** — no schema function is generated for a wrapper, so nothing inlines its fields | **NO** |

Evidence for row 2, on generated output: for
`FfLeaf {flatten; string s = 1;}` / `M {FfLeaf p = 1 [(flatten_field)]; int32 q = 2;}`
/ `Top {M m = 1;}`, `MSchema()` emits children `s`, `q` — `flatten_field` **is**
inlined inside `M` — and `TopSchema()` is `ArrowSchemaDeepCopy(MSchema())`.
Corroborating structural fact: the only readers of `ir::StructNode.fields`
repo-wide are the three validation walks; **no emitter reads them**, so the IR view
in which a struct child keeps such a field as a scalar carrying the dictionary
never reaches an artifact.

For row 3 the declaration is genuinely **resolved and honoured**, so applying R1
there is a **false positive**. Pinned end-to-end by
`GenErrors.DictionaryLiveInsideFlattenWrapperAccepted`
(`coverage_dictionary_wrapper_live.proto`, `EXPECT_SUCCESS`): deleting either the
top-level `IsFlattenedWrapper(msg)` skip or the descent's
`!IsFlattenedWrapper(child)` term reds it. The unit suite cannot cover this — the
pass is file-local and no unit test calls it — so that ctest is the only pin; the
unit test pins the *premise* (`Chains.ff_inside_wrapper` maps to a live `int16`
dictionary) instead.

##### What is still accepted silently

| Shape | Why nothing judges it |
|---|---|
| `map<string, W> m = 1;` (or a struct/list child of type `W`) where `W {flatten; string v = 1 [(dictionary) = {ordered: true}]}` | a `map` value / struct child does **not** flatten (`BuildMapNode` → `MakeStructNode(val_msg)`), so `W`'s fields are judged only through a field that actually flattens `W` — and the descent must not enter a wrapper (row 3 above) |
| gap 2's sibling: a `repeated` hop inside a **singular** flatten chain | R5 keys on the *outer* field being `repeated` (see the bullet under gap 2) |
| a struct **leaf reached through a `(fletcher.flatten)` wrapper hop** — `Top {repeated NestWrap xs = 1;}` over `NestWrap {flatten; repeated Inner ms = 1;}`, or the singular `Top {W w = 1;}` over `W {flatten; Inner m = 1;}`, where `Inner` carries the illegal declaration | the child *is* deep-copied (emission really emits `ArrowSchemaDeepCopy(InnerSchema(), …)`), but the walk stops at the wrapper: `!IsFlattenedWrapper(child)` is **required for R1's soundness** (row 3 above), so this is *not* fixable by descending into the wrapper. **The file-choice inconsistency S3 closed therefore survives one wrapper hop**: `protoc leaf_inner.proto` rejects `xf.li.Inner.k`, while `protoc leaf_nest.proto` / `leaf_sing.proto` exit 0 |

All three silently drop `ordered: true` as well as an index type, so locked #8 is
enforced as a **diagnostic** on every shape the pass reaches, but **not
absolutely**.

Rows 1 and 3 are both the **wrapper exclusion** doing its job, so they share one
fix shape — and it is *not* the obvious descent:

* row 1 needs **R1 to become context-dependent** (`is_top_level`-gated), so the
  other rules can be applied inside a wrapper without R1 firing on a declaration
  that is genuinely honoured there;
* row 3 needs the wrapper **chain followed to its leaf message**, judging the
  **leaf** (which *is* deep-copied) and never the wrapper's own fields. That is a
  strictly smaller change than gating R1 and could land first.

Row 2 keeps the resolution approved for it at step 2: it stays open and **R5 must
not be generalised** to reach it. (It sits downstream of the same exclusion — the
declaration lives below a wrapper the walk cannot enter — so the row-3 fix shape
may subsume it; that is for the follow-up item to establish, not something this
document asserts.)

Until one of those lands, **descending into a wrapper is the one fix that must not
be attempted**: it reintroduces the R1 false positive that
`GenErrors.DictionaryLiveInsideFlattenWrapperAccepted` exists to catch.

##### Consequence for DICT-3 (load-bearing)

Because those shapes are accepted, DICT-2 does **not** establish "only `SCALAR`
nodes carrying `facts.dictionary` reach emission". **DICT-3 must branch on
`facts.dictionary` AND `kind == NodeKind::SCALAR`.** Verified accepted at exit 0
today, with the fact live on a **LIST** node:

```proto
message W { option (fletcher.flatten) = true;
            repeated string vals = 1 [(fletcher.dictionary) = {index_type: DICTIONARY_INDEX_INT8}]; }
message M { map<string, W> m = 1; }
```

(`--fletcher_opt=accessor` on that proto makes DICT-1.5's guard name
`xf.ns.W.vals`, which is what proves the fact is on the LIST.) Without the kind
gate the schema visitor would emit `dictionary(<idx>, <list>)`.

**Where the fact lands (for consumers).** A dictionary declaration is a
*field-level* fact, and it lands on every IR node that is itself built from that
field's facts — which is **not** the same as "every node in the subtree".
Precisely:

| Shape | Nodes carrying `dictionary` |
|---|---|
| singular scalar, WKT wrapper, struct, oneof member (unsupported node) | the single node built from the field |
| `repeated` scalar / enum | the list node **and** its element |
| map | the **map node only** — key and value nodes are built from the synthetic map-entry fields and carry nothing |
| message-level-flatten wrapper (singular), incl. a wrapper over a `repeated` inner field | the resolved node, and its element when that node is a list |
| **nested list** from a repeated flatten wrapper (`repeated W` where `W` wraps a `repeated` scalar) | the **outermost** list and the **leaf** — the **intermediate** list levels keep default facts (`dictionary == false`) |

The last row is a real interior gap: `ir::BuildFlattenedRepeated` builds its extra
list levels with `MakeListOf`, which produces default facts, so a consumer walking
down the tree *finds → loses → refinds* the fact. Do not treat an interior
`dictionary == false` as authoritative. It is harmless in v1 (the validation item
rejects the whole non-scalar shape) and is pinned by the
`"flattened repeated nested list"` sub-case of
`TypeMapperTest.ReadsDictionaryOption`.

Consumers must in all cases gate on the **top-level** node's kind rather than on
"some node has `dictionary = true`" — for **kind / emission** decisions, where an
OR across a subtree would silently change a column's declared Arrow type on
evidence that does not belong to that column. This rule stays binding for
emitters (and, per locked decision #9, for DICT-2's legality gate: a scalar
dictionary declared inside a struct child stays legal, and DICT-2 must gate on
the field's own mapped `FieldKind`, not on an ancestor's — over-rejecting there
would permanently reject a legal proto).

The **one** exception is a **backend-availability rejection** predicate, where
the error asymmetry is the opposite: over-approximating costs a loud, fixable
error (regenerate without `--fletcher_opt=accessor,rust`) and no wrong output,
while under-approximating costs a silent mis-read. DICT-1.5's
`FindDictionaryField` (`plans/DICT-1.5-backend-support-guard.md` D2) is exactly
this predicate and deliberately ORs over every reachable node (list elements at
every level, fixed-size-list elements, map key/value, struct fields) rather than
gating on the top-level node alone — gating on the top-level node alone would
silently miss a dictionary declared on a field of a struct-typed (including
imported) child, breaking the safety property the guard exists to provide. This
exception is bounded to backend-availability guards; it is not licence for any
kind/emission consumer, or for DICT-2's legality gate, to OR over a subtree.

## 8. Out of scope

- Any change to the wire/byte format or to the runtime re-fold logic.
- `list(dictionary(...))`, `map` value dictionaries, struct/list/map/union
  dictionary value types.
- The Phase-2 generator IR rewrite (this feature is built against the current
  flat-`FieldKind` generator, shaped to fold cleanly into the future `Dictionary<V>`
  IR node).
- Sorting for `ordered: true` (deferred follow-up, §6).

## 9. Documentation deliverable

[docs/fletcher-options.md](fletcher-options.md) gains a `(fletcher.dictionary)`
section and the extension-number registry adds the `50001 / FieldOptions` row.
