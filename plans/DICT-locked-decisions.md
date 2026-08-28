# DICT — Locked Decisions

Firm choices for the dictionary-option round. The architect, architecture
reviewer, and compliance reviewer must honor these; a proposed deviation is a
stop-and-ask. Full rationale in
[docs/dictionary-option-spec.md](../docs/dictionary-option-spec.md).

1. **Trigger is a field-level message option.** `(fletcher.dictionary)`, a
   `DictionaryOptions` message extending `google.protobuf.FieldOptions` —
   **not** a bare bool, message-level option, naming convention, or enum
   auto-detection. Presence (`HasExtension` / `{}`) is the trigger.

2. **Extension number is `50001` on `FieldOptions`.** `50000` is already used by
   `flatten` (MessageOptions) and `flatten_field` (FieldOptions); do not reuse or
   renumber it. Update the registry table in `docs/fletcher-options.md`.

3. **Value type is derived from the field, never specified in the option.** The
   option carries only `index_type` and `ordered`. A `string` field →
   `dictionary(<idx>, utf8())`, etc.

4. **Index type defaults to `int32`.** Selectable int8/16/32/64;
   `DICTIONARY_INDEX_UNSPECIFIED` → int32. (Matches the runtime
   `DictionaryEncode` output and its declared-type cast.)

5. **Dictionary-ness is a scalar MODIFIER, never a structural kind.**
   *(Restated 2026-08-28 for the post-GIR tree; the intent is unchanged.)* The
   canonical carrier is **`ScalarFacts.dictionary`** on the IR
   ([`protoc/include/ir.hpp`](../protoc/include/ir.hpp)), which GIR already
   provides per **GIR locked decision #7**. `FieldKind` stays `SCALAR` and the
   flat `FieldMapping` projection carries whatever it needs derived from the IR —
   it is a projection, not a second source of truth. The field's value type
   remains the **value type**. No new `FieldKind` enum member, and modelling
   `Dictionary` as a container peer of `List`/`Struct` is a stop-and-ask
   (GIR locked #7).

6. **Wire format is byte-identical.** A dictionary field encodes/decodes exactly
   as the same field without the option (value type, one per row). Only the
   emitted **Arrow/nanoarrow schema type** changes. No change to the codec, the
   positional I/O, or `pubsub-arrow`'s `BuildDictionaryColumn`.

7. **Only schema emission branches on the option.** Encode, decode, getters,
   setters, `ToArrowRow` per-row scalars, and the TypeScript descriptor all keep
   treating the field as its value type.

8. **`ordered: true` is rejected at codegen in v1.** The field stays in the
   option message for forward compatibility; honoring it (sorting in the runtime
   re-fold) is a deferred follow-up, explicitly out of scope.

9. **Accept the option iff the field maps to `FieldKind::SCALAR`; reject all
   other kinds.** The gate is on the **mapped `FieldKind`**, not the raw proto
   type. `MapField` folds singular scalars, enums, **and well-known wrapper
   messages** (`StringValue`, `Int32Value`, …) into `FieldKind::SCALAR`, so a
   wrapper carrying `(fletcher.dictionary)` is a **valid nullable dictionary**
   (`dictionary(<idx>, <inner>)`) — gating on `field->type()==TYPE_MESSAGE` would
   wrongly reject it. Struct messages (`STRUCT`), `repeated`, `map`, and
   `NESTED_LIST` map to non-`SCALAR` kinds and are a codegen error with a clear
   `UnsupportedReason`. Dictionary value type must be primitive/scalar.

10. **Read the option without linking `options.pb.cc`** — the constraint is the
    mechanism-independent part, and it holds. *(Revisited 2026-08-28.)* The
    original mechanism was a hand-rolled unknown-field walker over the
    length-delimited field, consistent with `FindBoolOption`. Since then **PR
    #121** landed a general option reader that satisfies the same constraint more
    robustly, by reflection over a `DynamicMessage` built from the
    `DescriptorPool` protoc populates from the `CodeGeneratorRequest`
    ([`protoc/src/option_metadata.cpp`](../protoc/src/option_metadata.cpp)) — so
    enum-value names and nested option fields are reachable without linking the
    declaring `.proto`.

    **DICT-1 must evaluate reusing that reader before writing a walker.** If it
    can read `(fletcher.dictionary)`, reuse it and this decision retires; a
    second bespoke parser for the same job would be the drift GIR spent a whole
    round removing. Choosing the walker anyway is fine, but it must be a recorded
    decision with a reason, not a default.

11. **The generated accessor does NOT learn dictionary columns in this round.**
    *(Changed 2026-08-28. Previously: "learns dictionary columns in this round
    (DICT-6, final stage)".)*

    DICT-6 is **removed**. Extending the RBA `<Class>Accessor` (C++
    `.fletcher.accessor.pb.h` + arrow-rs `.fletcher.rs`) to read
    `dictionary(<index>, <value>)` columns moves to round **RIR**, which migrates
    that same emitter onto the IR. Written here it would patch the flat accessor
    that RIR then retires — the same work against two substrates.

    In this round the gap is instead made loud and safe by **DICT-1.5**: a
    front-end guard that fails the plugin when `--fletcher_opt=accessor` or
    `rust` is requested for a proto containing a dictionary field, extending
    GIR-10's existing `ValidateBackendsSupportFields` pass rather than adding a
    parallel mechanism. **The RBA emitter is untouched by DICT** (GIR locked #3:
    it stays read-only until RIR).

    The full requirement set — `dictionary(idx,val)` positional gate, cast-once
    offset-preserving `DictionaryArray` cache plus downcast values,
    null-before-index getter with the **value-typed signature unchanged**,
    index-type→key-array mapping, `null_count()==0` for proto-non-nullable,
    never-panics, C++/Rust parity — is carried into RIR verbatim in intent, and
    **RIR removes the DICT-1.5 guard in the same change that adds the support**
    so the guard cannot outlive its subject.

    Consequence, deliberately: **RIR is now the single round that closes every
    deferred interdependency** (RBA onto the IR, `FieldKind` retirement,
    scalar-leaf nested lists, dictionary columns, both front-end guards). Adding
    a new deferral that does not land in RIR is a stop-and-ask.

