# RBA-2 — C++ scalar accessor + positional type-check validation

## Summary

RBA-2 emits the first real C++ `<Class>Accessor` implementation in the accessor
header: flat, scalar-only, column-oriented, and cast-once.
Every generated accessor has both `RecordBatch` and `StructArray` factories, both
delegating to one `FromColumns_` validator/populator.
Validation is positional and type-only, with an additional actual-data
`null_count()==0` gate for proto-non-nullable fields.
The accessor owns only cached typed Arrow array handles plus live fields/metadata;
it never keeps the source batch or struct array alive directly.

## Design

### Class structure

`EmitAccessorHeader` emits one `<Class>Accessor` for each message in
`OrderedMessages(file)`, using `GatherFields` / `FieldInfo` / `ScalarTypeInfo` as
the read-only schema model. RBA-2 only emits scalar-field accessors; unsupported
non-scalar fields keep the existing generated-comment pattern and are not silently
misrepresented.

Generated header dependencies:

- `<arrow/api.h>`
- `<cstdint>`
- `<memory>`
- `<optional>`
- `<string_view>`
- `<utility>`
- `<vector>`

Generated public shape:

```cpp
class TelemetryAccessor {
 public:
  static arrow::Result<TelemetryAccessor> Make(
      const std::shared_ptr<arrow::RecordBatch>& batch);
  static arrow::Result<TelemetryAccessor> Make(
      const std::shared_ptr<arrow::StructArray>& struct_array);

  int64_t num_rows() const;

  // One generated getter per scalar field.
  double temperature(int64_t row) const;
  std::optional<std::string_view> label(int64_t row) const;

 private:
  TelemetryAccessor() = default;

  static arrow::Result<TelemetryAccessor> FromColumns_(
      int64_t num_rows, const arrow::ArrayVector& cols, arrow::FieldVector fields,
      std::shared_ptr<const arrow::KeyValueMetadata> schema_metadata);

  int64_t num_rows_ = 0;
  arrow::FieldVector fields_;
  std::shared_ptr<const arrow::KeyValueMetadata> schema_metadata_;
  std::shared_ptr<arrow::DoubleArray> temperature_;
  std::shared_ptr<arrow::StringArray> label_;
};
```

`Make(RecordBatch)` extracts `num_rows()`, `columns()`, `schema()->fields()`, and
`schema()->metadata()`, then calls `FromColumns_`.

`Make(StructArray)` first checks the source pointer is non-null and returns
`arrow::Status::Invalid("<Class>: null StructArray")` if so. Then it builds an
`arrow::ArrayVector` from the children with `struct_array->field(i)` directly —
**no explicit `Slice`**: Arrow C++ `StructArray::field(i)` already returns each
child sliced to the struct's `[offset, offset+length)` window, so an additional
`field(i)->Slice(offset, length)` would *double-apply* the offset (verified at
implementation: a double-slice read the wrong row; corrected per the Step-2 /
Step-4 reviews against Arrow C++ 23.0.1). It then passes `struct_array->length()`,
`struct_type()->fields()`, and `nullptr` schema metadata to `FromColumns_`. The
correct windowing is inherited by RBA-4. (Cross-language note for RBA-5/6: this
is C++-specific — arrow-rs `StructArray::columns()` is **not** pre-windowed, so
the Rust `from_struct` path **must** slice each child by the struct offset.) The
null-source-pointer guard precedes any `s->offset()` / `s->num_fields()`
dereference.

The `Make(RecordBatch)` factory should also check for null source pointers and return
`arrow::Status::Invalid("<Class>: null RecordBatch")` if so; this is consistent with
"never throws" and avoids dereferencing bad caller input.

### Validation logic

`FromColumns_` performs all gates before any unchecked down-cast:

1. Check `cols.size() == expected_field_count`.
2. For each scalar field at index `i`, check `cols[i] != nullptr`.
3. Check `cols[i]->type()->Equals(*expected_type, false)`.
4. If the proto field is non-nullable, check `cols[i]->null_count() == 0`.
5. Populate the accessor with `num_rows`, fields, schema metadata, and
   `std::static_pointer_cast<ConcreteArray>(cols[i])`.

Names, `arrow::Field::nullable()`, and metadata are not validation gates.
Generated error text should include class, column index, proto field name,
expected Arrow type, and actual Arrow type, for example:

```text
Telemetry column 1 'id': expected int32, got double
Telemetry column 0 'temperature': non-nullable, found 1 nulls
Telemetry: expected 3 columns, got 2
```

The expected type expression comes from `ScalarTypeInfo.arrow_type_expr`; the
message/field names come from `FieldInfo` / descriptor data only for diagnostics.
Type comparison uses metadata-ignore mode: `Equals(*expected, false)`.

For `Make(StructArray)`, the `FromColumns_` count + per-position type gate is what
rejects a `StructArray` whose child types or arity don't match. The StructArray
path passes `nullptr` schema metadata (per SPEC §5) and has no name or metadata to
fall back on — only the positional type check is used.

### Type coverage

RBA-2 needs an accessor-emitter mapping from `ScalarTypeInfo` to concrete Arrow
array/getter forms. The mapper remains the source of truth for the Arrow type
expression and C++ value type; the accessor emitter derives the concrete array
class needed for cached handles.

| Arrow type expression | Cached C++ array | Getter value |
|---|---|---|
| `arrow::boolean()` | `arrow::BooleanArray` | `bool` |
| `arrow::int8()` | `arrow::Int8Array` | `int8_t` |
| `arrow::int16()` | `arrow::Int16Array` | `int16_t` |
| `arrow::int32()` | `arrow::Int32Array` | `int32_t` |
| `arrow::int64()` | `arrow::Int64Array` | `int64_t` |
| `arrow::uint8()` | `arrow::UInt8Array` | `uint8_t` |
| `arrow::uint16()` | `arrow::UInt16Array` | `uint16_t` |
| `arrow::uint32()` | `arrow::UInt32Array` | `uint32_t` |
| `arrow::uint64()` | `arrow::UInt64Array` | `uint64_t` |
| `arrow::float32()` | `arrow::FloatArray` | `float` |
| `arrow::float64()` | `arrow::DoubleArray` | `double` |
| `arrow::utf8()` | `arrow::StringArray` | `std::string_view` |
| `arrow::binary()` | `arrow::BinaryArray` | `std::string_view` |
| `arrow::timestamp(<unit>)` | `arrow::TimestampArray` | `int64_t` |
| `arrow::duration(<unit>)` | `arrow::DurationArray` | `int64_t` |

Timestamp/duration unit variants are emitted from the exact
`ScalarTypeInfo.arrow_type_expr` (`SECOND`, `MILLI`, `MICRO`, `NANO` where the
mapper can produce them). The current WKT path produces nanoseconds; the emitter
should not hard-code only nano if `ScalarTypeInfo` later carries another unit.

If the current `ScalarTypeInfo` model lacks a direct array-class field, keep the
mapping local to `recordbatch_accessor_emitter.cpp` and keyed by the Arrow type
expression family. Do not change mapper behavior for existing emitters.

### Getter shapes

For non-nullable scalar fields:

```cpp
int32_t id(int64_t row) const { return id_->Value(row); }
double temperature(int64_t row) const { return temperature_->Value(row); }
std::string_view label(int64_t row) const { return label_->GetView(row); }
```

For nullable scalar fields:

```cpp
std::optional<int32_t> id(int64_t row) const {
  if (id_->IsNull(row)) return std::nullopt;
  return id_->Value(row);
}

std::optional<std::string_view> label(int64_t row) const {
  if (label_->IsNull(row)) return std::nullopt;
  return label_->GetView(row);
}
```

String and binary getters use `GetView(row)` so the accessor returns
`std::string_view` without copying. Numeric, boolean, timestamp, and duration
getters use `Value(row)`.

Row bounds are not checked by generated getters; this matches Arrow array access
conventions and keeps RBA-2 focused on schema/data validation at construction.

### Memory model

The accessor stores `std::shared_ptr<ConcreteArray>` for every scalar column.
Those arrays retain their Arrow buffers, so callers may drop all references to the
source `RecordBatch` or `StructArray` after a successful `Make`.

The accessor stores:

- `int64_t num_rows_`
- `arrow::FieldVector fields_`
- `std::shared_ptr<const arrow::KeyValueMetadata> schema_metadata_`
- per-field cached typed array handles

It does not store the source `RecordBatch`, the source `StructArray`, or any
per-row scalar materialization. `StructArray` construction keeps data alive via
the sliced child arrays returned by `Slice`; those sliced arrays share buffers and
carry the correct offset/length window.

## Forcing-test mapping

- Build RecordBatch for scalar fixture and assert getters: generated getters read
  cached arrays directly with `Value` / `GetView`, so each row should match the
  fixture data.
- Nullable field with null returns `nullopt`: nullable getters guard with
  `IsNull(row)` before reading.
- Type-mismatched batch fails with precise message: `FromColumns_` checks
  `type()->Equals(*expected, false)` per index before down-cast and formats class,
  column, field, expected, and actual.
- Name-mismatched but type-compatible batch succeeds: validation never inspects
  runtime field names.
- Proto-non-nullable scalar column with runtime null fails: `FromColumns_` checks
  `null_count()==0` for proto-non-nullable fields.
- Wrong column count fails: `cols.size()` is the first validation gate.
- `Make` never throws: factories return `arrow::Result`, guard null source
  pointers, and use Arrow status errors instead of exceptions.
- Accessor keeps data alive after caller drops batch: cached `shared_ptr<T>`
  columns own buffers independently of the source object.
- `Make(StructArray)` reads equivalent data identically: struct factory slices
  child arrays to the struct window, then uses the same `FromColumns_` as
  `RecordBatch`.
- Both factories present and working on flat message: every accessor emits both
  public overloads unconditionally.
- Non-nullable column with no validity buffer accepted: validation is
  `null_count()==0`, not a validity-buffer presence check.


## Risks / Unknowns

- `ScalarTypeInfo` does not currently expose a concrete Arrow array class. RBA-2
  should add accessor-local derivation keyed by `arrow_type_expr`; changing the
  shared mapper contract for existing emitters is out of scope unless explicitly
  approved.
- Arrow C++ API differences by version may affect exact string/binary view calls
  or timestamp/duration array aliases. The implementation should compile-check
  the generated header in the integration harness and adjust only the generated
  accessor shapes, not the locked semantics.
- RBA-2 is scalar-only. If the scalar fixture includes repeated, map, or nested
  message fields, either change the fixture or emit an explicit unsupported
  comment for those fields; do not partially validate composites in RBA-2.

## Files-to-touch

- `protoc/src/generator.cpp` — add `#include "generator_internal.hpp"` at the top
  to include declarations of `OrderedMessages`, `GatherFields`, `FieldInfo`, and
  `ArrowTypeExpr` (definitions remain in place, unchanged).
- `protoc/include/generator_internal.hpp` — new shared internal header containing
  **declarations only** of `OrderedMessages`, `FieldInfo`, `ArrowTypeExpr`, and
  `GatherFields` (`GatherFieldsImpl` may stay file-local — it is not needed by the
  accessor TU). Declarations live in `namespace fletcher` (NOT anonymous). The
  corresponding **definitions in `generator.cpp` must move out of the anonymous
  namespace into `namespace fletcher`** so they gain external linkage — a header
  declaration cannot resolve to an internal-linkage definition in another TU. This
  is the only edit to those definitions (a linkage change, no logic change); it
  emits identical bytes, so it is **zero behavioural change** and the RBA-1
  no-drift test guards byte-identity. Reachable from both TUs because CMake puts
  both `include/` and `src/` on the `fletcher_plugin_core` include path
  (`target_include_directories ... PUBLIC include PRIVATE src`). See revision note.
- `protoc/src/recordbatch_accessor_emitter.cpp` — implement scalar accessor
  generation, validation emission, accessor-local scalar array mapping, generated
  includes, factories, getters, and storage. Include `generator_internal.hpp`.
- `protoc/src/recordbatch_accessor_emitter.hpp` — only if the public emitter
  declaration/comment needs wording updates; no behavior belongs here.
- `protoc/include/type_mapper.hpp` — only with STOP-AND-ASK approval if helper or
  `ScalarTypeInfo` visibility proves impossible to reuse read-only from the
  accessor emitter.
- `integration-tests/protoc-arrow-bridge/proto/accessor_scalar.proto` — add a
  flat scalar fixture covering bool, signed/unsigned ints, float/double,
  utf8/binary, timestamp, duration, nullable and non-nullable cases.
- `integration-tests/protoc-arrow-bridge/tests/test_accessor.cpp` — add
  `AccessorTest.ScalarColumnsReadAndValidatePositionally`, generated-header
  include, fixture builders, validation-failure cases, lifetime check, and
  `StructArray` factory check.
- `integration-tests/protoc-arrow-bridge/CMakeLists.txt` — generate the accessor
  fixture with `--fletcher_opt=accessor`, add generated accessor header to the
  accessor test target include path/dependencies, and link Arrow for the new
  runtime test code.

## Step-2 review (2026-06-24)

**Verdict: APPROVE (READY FOR IMPLEMENTATION).** The accessor shape, validation
rule, null-count gate, cast-once memory model, dual-factory contract, and
forcing-test mapping are correct and faithfully match SPEC §3/§4/§7 and
D-RBA-3/4/7. All three blocking items from the initial review are resolved.

**Re-review note (2026-06-24, round 2):** verified all three fixes against the
revised text. One accuracy correction applied inline to Fix 1: the original
"definitions remain in place, unchanged" was technically wrong — the helpers are
in `generator.cpp`'s anonymous namespace (internal linkage), so a header
declaration cannot link to them across TUs; the definitions must move into
`namespace fletcher` for external linkage. That move is non-behavioural (same
emitted bytes, guarded by the RBA-1 no-drift test) and is **not** a D-RBA-1
deviation, so it does not block. Header placement under `protoc/include/` is
correct: CMake puts both `include/` and `src/` on the `fletcher_plugin_core`
include path, so both `generator.cpp` and `recordbatch_accessor_emitter.cpp`
reach it. No residual blockers.

**Revisions applied (2026-06-24 rework):**

1. **Helper visibility resolved via shared internal header.** `OrderedMessages`,
   `FieldInfo`, `GatherFields`, and `ArrowTypeExpr` are in `generator.cpp`'s
   anonymous namespace (lines 25–2896), internal linkage, unreachable today from
   `recordbatch_accessor_emitter.cpp`. **Solution: path (a) — pure relocation.**
   Create a new `protoc/include/generator_internal.hpp` (in `namespace fletcher`)
   declaring the required helpers + struct, and add `#include "generator_internal.hpp"`
   to `generator.cpp`. **The definitions must move from the anonymous namespace into
   `namespace fletcher` to gain external linkage** (a header declaration cannot link
   to an internal-linkage definition in another TU) — this linkage move is the only
   edit; the logic is unchanged and the emitted bytes are identical, so it is **zero
   behavioural change** and **not a D-RBA-1 violation** (D-RBA-1 forbids output-byte
   and emitter-behaviour changes; a linkage relocation is neither). The RBA-1
   no-drift test confirms existing outputs stay byte-identical. Both TUs can include
   the header because CMake exposes both `include/` and `src/` on the
   `fletcher_plugin_core` include path. Files-to-touch updated accordingly.

2. **Metadata getters deferred to RBA-3.** Removed `schema_metadata()` and
   `field_metadata(int i)` public methods from the generated public shape. RBA-2
   now stores `fields_` and `schema_metadata_` **only**; the accessor owns the
   storage for later metadata access, but the public accessor methods land in
   RBA-3. This keeps the RBA-2 item focused (schema validation + scalar-field
   access) and preserves the honest contract of RBA-3's `ExposesSchemaAndFieldMetadataGenerically`
   test (it will prove *first* that metadata is accessible). SPEC §5 and the plan
   (line 801 onwards) confirm metadata access is RBA-3 scope.

3. **Two under-specified points tightened.** Added explicit note that
   `FromColumns_`'s count + per-position type gate rejects a mismatched
   `StructArray` — there is no name/metadata fallback. Also clarified that the
   null-source-pointer guard in `Make(StructArray)` precedes any
   `s->offset()` / `s->num_fields()` dereference, with a one-line emphasis in
   the description of `Make(StructArray)`.

**Non-blocking / confirmed-correct (no action required):**

- Positional + type-only validation via `type()->Equals(*expected, false)`,
  with name- and nullable-*flag*-tolerance, matches D-RBA-4 and SPEC §4 exactly.
  The name-mismatch-succeeds and wrong-count-fails forcing cases map correctly.
- `null_count() == 0` is gated on **actual** nulls (not validity-buffer presence)
  and only for proto-non-nullable fields; the "no-validity-buffer accepted" case
  is covered. Correct per D-RBA-4. `null_count()` is O(1) when there is no
  validity buffer — the design's claim holds for Arrow C++.
- Getters index the **cached** typed array (`Value` / `GetView`) — no per-cell
  `GetScalar`/re-cast; string/binary use zero-copy `GetView` → `string_view`.
  Honours D-RBA-3.
- Cached `std::shared_ptr<ConcreteArray>` keep buffers alive with no source ref;
  `StructArray` children sliced to `[offset,len)` before caching; factories
  return `arrow::Result` and never throw. Honours D-RBA-7 and the lifetime test.
- Array-class table is accurate: `FloatArray` (float32) vs `DoubleArray`
  (float64), `TimestampArray`, `DurationArray`, `StringArray`/`BinaryArray` are
  the correct Arrow C++ concrete classes. `ScalarTypeInfo` indeed exposes
  `arrow_type_expr` and `storage_type` but **no** concrete-array field, so the
  accessor-local derivation keyed by `arrow_type_expr` is the right call and
  changes no shared-mapper contract.
- Scalar-only scope is respected; non-scalar fields keep the existing
  unsupported-comment pattern (D-RBA-6 partial; full parity is RBA-4). No
  composite validation leaks in.
