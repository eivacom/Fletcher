# RBA-3 - C++ generic metadata access

## Summary

RBA-3 adds generic metadata getters to each generated C++ `<Class>Accessor`.
The accessor exposes the metadata carried by the live Arrow schema/fields used to
construct it, without interpreting keys and without adding any Fletcher- or
domain-specific vocabulary.

This is a public API addition on top of RBA-2 storage. RBA-2 already stores
`fields_` as `arrow::FieldVector` and `schema_metadata_` as
`std::shared_ptr<const arrow::KeyValueMetadata>`; RBA-3 only adds read-only
accessors over those members. Validation remains positional and type-only, and
metadata is never a construction gate.

## Design

### Generated public methods

Emit these methods in the public section of every generated `<Class>Accessor`:

```cpp
const arrow::KeyValueMetadata* schema_metadata() const;
const arrow::KeyValueMetadata* field_metadata(int i) const;
const arrow::KeyValueMetadata* field_metadata(const std::string& name) const;
```

Exact return spelling is `const arrow::KeyValueMetadata*`, matching oracle
section 5.
The existing `std::shared_ptr<const arrow::KeyValueMetadata>` storage remains an
implementation detail used to keep schema metadata alive; it is not exposed in
the public API.

`<string>` must be included by the generated accessor header because the name
overload accepts `const std::string&`. The existing emitter already includes
`<arrow/api.h>`, which provides `arrow::KeyValueMetadata`.

### Lifetime and borrowing

All three getters return non-owning pointers. A returned pointer is valid only as
long as the accessor object is alive, and only until that accessor object is
destroyed. Callers that need metadata to outlive the accessor must copy the
metadata values they need.

The accessor owns enough state to make this borrowing safe:

- `schema_metadata()` returns `schema_metadata_.get()`. For a `RecordBatch`
  source, `schema_metadata_` is copied from `batch->schema()->metadata()`, so the
  accessor's shared pointer keeps the schema metadata object alive independently
  of the source batch/schema.
- `field_metadata(...)` returns `fields_[i]->metadata().get()` after resolving an
  index. `fields_` stores `std::shared_ptr<arrow::Field>` values copied from the
  live schema or struct type; those field objects keep their own metadata alive.

The returned `arrow::KeyValueMetadata` is logically read-only through the
`const` pointer. RBA-3 adds no mutation, builder, setter, or metadata-normalizing
API.

### Schema metadata behavior

`schema_metadata()` returns the schema-level metadata from the live
`RecordBatch` schema when the accessor was built with `Make(RecordBatch)`.

`Make(StructArray)` continues to pass `nullptr` for `schema_metadata_`, because
an `arrow::StructArray` has no top-level `arrow::Schema`. Therefore an accessor
constructed from a struct source returns `nullptr` from `schema_metadata()`. This
is the C++ representation of empty schema metadata for struct-sourced accessors.

Absent schema metadata is not an error. It is represented by `nullptr`.

### Field metadata by index

`field_metadata(int i)` is the canonical field metadata getter. It resolves
directly against `fields_` by index, so it aligns with the positional validation
contract from D-RBA-4 and does not depend on names matching generated proto field
names.

Behavior:

- If `i < 0` or `i >= fields_.size()`, return `nullptr`; do not throw and do not
  return `arrow::Status`.
- If `fields_[i]` is null, return `nullptr`. The normal factory path supplies
  real Arrow fields, but this keeps the generated method total over its stored
  state.
- If the field exists but has no metadata, return `nullptr`; absent field
  metadata is not an error.
- If metadata exists, return `fields_[i]->metadata().get()` verbatim.

The getter does not inspect field names, generated proto names, metadata keys, or
metadata values.

### Field metadata by name

`field_metadata(const std::string& name)` is a convenience overload over the
live field names stored in `fields_`. It performs a linear scan:

```cpp
for (const auto& field : fields_) {
  if (field != nullptr && field->name() == name) return field->metadata().get();
}
return nullptr;
```

Unknown names return `nullptr`; this is not an error. If a matching field has no
metadata, the result is also `nullptr`.

If duplicate live field names are present, the overload returns the first match
in `fields_` order. This follows the simple linear-scan rule and avoids inventing
name-resolution semantics beyond Arrow's stored field list.

Because RBA construction is name-tolerant, this overload searches the live Arrow
field names, not the generated proto names. It is best-effort only. Generated
code and tests that must be robust to field-name drift should use
`field_metadata(i)`.

### Generic, domain-agnostic handling

The getters expose exactly the `arrow::KeyValueMetadata` objects already present
on the live schema/fields. The emitter must not mention or branch on any metadata
key such as units, producers, Fletcher-specific field ids, or any downstream
domain vocabulary. Metadata is not copied key-by-key, filtered, normalized,
generated, or used for validation.

### No-drift constraint

RBA-3 only changes the opt-gated accessor header emitted by
`--fletcher_opt=accessor`. It must not alter existing generated files
(`.fletcher.pb.h`, `.fletcher.arrow.pb.h`, `.fletcher.ts`, IPC outputs), existing
emitter behavior, option semantics, or shared helper behavior. The RBA-1 no-drift
test must remain green.

## Forcing-test mapping

`AccessorTest.ExposesSchemaAndFieldMetadataGenerically` should build a
`RecordBatch` whose schema contains arbitrary metadata, for example:

- schema metadata: `producer=x`
- field metadata at index 0: `unit=m/s`

After `auto acc = <Class>Accessor::Make(batch).ValueOrDie();`:

- `acc.schema_metadata()` is non-null because `Make(RecordBatch)` stores the live
  schema metadata shared pointer.
- Looking up `producer` on `acc.schema_metadata()` returns `x` verbatim.
- `acc.field_metadata(0)` is non-null because `fields_` stores the live schema's
  field objects.
- Looking up `unit` on `acc.field_metadata(0)` returns `m/s` verbatim.
- `acc.field_metadata("live_field_name")` returns the same field metadata when
  the supplied name matches the live Arrow field name.

The test should also cover absent metadata as a non-error path:

- a field without metadata returns `nullptr` from `field_metadata(i)`;
- an unknown name returns `nullptr` from `field_metadata(name)`;
- a struct-sourced accessor, if tested here, returns `nullptr` from
  `schema_metadata()`.

The forcing test's keys are arbitrary strings. The generated accessor never
references those strings; it only returns Arrow's stored metadata object.

## Risks/Unknowns

No locked decision is in tension. The only API choice that needs care is avoiding
`std::shared_ptr<const arrow::KeyValueMetadata>` in the public getter signatures:
RBA-2's shared pointer storage is internal ownership, while oracle section 5
fixes the C++ public return type as `const arrow::KeyValueMetadata*`.

`field_metadata(name)` is necessarily best-effort because D-RBA-4 permits
runtime field names to differ from generated proto names. This is not a
correctness risk as long as by-index access remains canonical and the name
overload is documented as live-name lookup.

## Files-to-touch

- `protoc/src/recordbatch_accessor_emitter.cpp`: emit the three public metadata
  getters for every C++ accessor; add `<string>` to generated accessor headers;
  reuse existing `fields_` and `schema_metadata_` storage without changing
  validation or scalar getter behavior.
- `integration-tests/protoc-arrow-bridge/tests/test_accessor_scalar.cpp`: add
  `AccessorTest.ExposesSchemaAndFieldMetadataGenerically`, building a batch with
  arbitrary schema/field metadata and asserting verbatim read-back plus null
  results for absent/unknown metadata.
- `integration-tests/protoc-arrow-bridge/CMakeLists.txt`: touch only if the new
  test needs additional fixture generation or registration; otherwise leave it
  unchanged.

## Step-2 review (2026-06-24)

**Verdict: APPROVE** (with 2 non-blocking clarifications folded in below; no
locked-decision deviation, no STOP-AND-ASK).

Checked against oracle §5, D-RBA-5, D-RBA-1, and the RBA-3 user story/acceptance.

Findings:

1. **Generic / domain-agnostic (D-RBA-5) — PASS.** All three getters return the
   live Arrow `KeyValueMetadata` objects verbatim; the emitter is forbidden from
   mentioning or branching on any key (design "Generic, domain-agnostic
   handling"). No hardcoded domain vocabulary, no schema-generator change. Honoured.

2. **Pure add-on (D-RBA-1) — PASS.** Getters-only addition over RBA-2 storage;
   no change to validation, scalar getters, existing emitters, option semantics,
   or other outputs. RBA-1 no-drift stays green. Honoured.

3. **Lifetime/borrowing — PASS, with one precision note (folded in).** The claim
   that `field_metadata(i)`'s returned pointer is "valid as long as the accessor
   is alive" is correct, but it is correct for a non-obvious reason worth stating
   in the generated doc-comment: `arrow::Field::metadata()` returns the metadata
   **by value** (a *copy* of the field's member `shared_ptr`), so
   `fields_[i]->metadata().get()` dereferences a temporary `shared_ptr` that is
   destroyed at end-of-expression. The pointed-to `KeyValueMetadata` survives only
   because `fields_[i]` (the `shared_ptr<Field>` kept by the accessor) co-owns it
   via the field's own member — **not** because the temporary survives. The
   design's "those field objects keep their own metadata alive" (Lifetime section)
   already names this; keep that exact justification in the emitted code so a
   future edit does not "simplify" by storing the temporary's `.get()` past the
   full expression. No code change required; the design is sound as written.

4. **Out-of-bounds / unknown-name / absent metadata — PASS.** Index `<0` or
   `>= size()` → `nullptr`; null `fields_[i]` → `nullptr`; field-without-metadata
   → `nullptr`; unknown name → `nullptr`; duplicate live names → first match. No
   throw, no `Status`, no UB. Totality over stored state is explicit. Matches the
   acceptance ("absent metadata → empty/null, not an error").

5. **Schema getter independent of field names — PASS.** `schema_metadata()`
   returns `schema_metadata_.get()` directly; no name matching involved.

6. **StructArray → `nullptr` schema metadata — ACCEPTABLE per §5, clarification
   folded in.** Spec §5 says a struct-sourced accessor "reports **empty** schema
   metadata"; this design realises "empty" as a **`nullptr` return**, not an
   empty-but-non-null `KeyValueMetadata`. That is a legitimate reading of §5/"
   empty/null", and the design + forcing test both document the `nullptr` form, so
   it is internally consistent and disclosed — **APPROVE**. Required clarification
   (doc-only): the generated getter's doc-comment and/or the RBA-7 docs must state
   that `schema_metadata()` may return `nullptr` (struct source, or a
   `RecordBatch` whose schema carries no metadata), so callers null-check before
   `->Contains(...)`/`->Get(...)` rather than dereferencing. This shifts a
   null-guard onto the caller; make it explicit. (Note for the eventual Rust side
   in RBA-6: §5 fixes Rust's `schema_metadata()` as `&HashMap` — i.e. genuinely
   empty, not optional — so C++ `nullptr` and Rust empty-map are *representation*
   parity, not literal parity. Not an RBA-3 blocker; flagged so RBA-6 does not
   try to force a nullable Rust return for symmetry.)

7. **Return type/spelling vs §5 and RBA-2 shape — PASS.** All three return
   `const arrow::KeyValueMetadata*`, matching §5; internal
   `shared_ptr<const KeyValueMetadata>` / `arrow::FieldVector` storage stays
   private, consistent with the RBA-2 sketch.

8. **Forcing-test → design mapping — PASS.** The named test exercises verbatim
   read-back of arbitrary schema + field metadata, plus the absent/unknown null
   paths and the struct-source `nullptr`. It does the class, not just one key: the
   keys are arbitrary strings the generator never references.

9. **Files-to-touch — plausible.** Single emitter file + one test TU; CMake only
   if a new fixture is needed. No hidden cross-cutting change (the `<string>`
   include is local to the generated header). Buildable.

Required (non-blocking) follow-through:
- (a) Keep the field-metadata lifetime justification (finding 3) in the emitted
  doc-comment verbatim, so the `metadata().get()` idiom is not "simplified" into a
  dangling read.
- (b) Document the `schema_metadata()` `nullptr` contract on the generated getter
  (finding 6) and carry the C++-nullptr / Rust-empty-map representation note into
  RBA-6 so parity is not mis-read as a literal-return mismatch.
