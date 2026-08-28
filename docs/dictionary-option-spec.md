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

The plugin must reject — with a clear `UnsupportedReason` — any use that the
runtime cannot honor, rather than emit a schema that fails downstream:

The rule is keyed on the **mapped `FieldKind`**, not the raw proto type: the
option is **allowed exactly when `MapField` resolves the field to
`FieldKind::SCALAR`**, and rejected otherwise. This is the robust formulation
because `MapField` already folds singular scalars, enums, **and well-known
wrapper messages** into `FieldKind::SCALAR`.

| Field shape | Result |
|---|---|
| singular scalar (`bool`/int/uint/float/double/`string`/`bytes`/`enum`) | **allowed** (maps to `SCALAR`) |
| well-known wrapper message (`google.protobuf.StringValue`, `Int32Value`, …) | **allowed** — `MapField`→`MapWellKnown` maps it to a *nullable* `SCALAR` of the inner type ([type_mapper.cpp:632-669](../protoc/src/type_mapper.cpp#L632-L669)); yields a nullable `dictionary(<idx>, <inner>)` |
| singular **struct** message field | **rejected** — maps to `STRUCT`; dictionary value type must be primitive/scalar |
| `repeated` field | **rejected** — `list(dictionary(...))` is not supported by the codec re-fold |
| `map<K,V>` field | **rejected** — same reason |
| `oneof` member | already unsupported by the plugin; option is moot |
| `ordered: true` | **rejected in v1** (see §6) |

> **WKT wrappers are deliberately allowed.** A `StringValue` field is already a
> nullable string at the schema/codec/runtime level, so `dictionary(<idx>, utf8)`
> nullable is exactly consistent with how the option behaves on `optional string`.
> Gating on `field->type() == TYPE_MESSAGE` would wrongly reject it; gate on the
> mapped kind instead. Add a forcing test for a wrapper field (DICT-2).

`[(fletcher.flatten_field)]` is a no-op on scalar fields, so a scalar carrying
both options resolves to dictionary; document, do not error.

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

The existing `FindBoolOption`
([type_mapper.cpp:21-29](../protoc/src/type_mapper.cpp#L21-L29)) reads a **varint**
unknown field. A *message-typed* option arrives instead as a
**length-delimited** unknown field (#50001) on `FieldOptions` whose payload is a
serialized `DictionaryOptions`. The reader must walk that payload's inner fields
(field 1 = `index_type` varint, field 2 = `ordered` varint). Per
[locked-decisions](../plans/DICT-locked-decisions.md), this is done with a small
unknown-field walker — preserving the plugin's property of **not** linking
`options.pb.cc` — with linking the generated descriptor recorded as the
alternative if the option set grows.

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
