# RBA-4 C++ Composite Accessor Parity

## Summary

RBA-4 extends the existing C++ RecordBatch accessor from scalar columns and
metadata to full `<Class>View` type parity: `STRUCT`, `REPEATED_SCALAR`,
`REPEATED_STRUCT`, `MAP`, and `NESTED_LIST` at depth 2-3. The generated accessor
stays column-oriented: all Arrow casts happen once in `Make`, the accessor owns
the cached Arrow handles, and getters index those handles directly.

Key decisions:

- Add a header-only span library at
  `arrow-bridge/include/fletcher/arrow_bridge/recordbatch_spans.hpp`, beside
  `arrow_row_view.hpp`. Generated accessor headers include it when any
  collection field is emitted.
- Every generated `<Class>Accessor` gets `RowView`, `at(row)`, and `is_null(row)`
  unconditionally, even scalar-only accessors. `RowView` is a borrowed two-word
  forwarder: `{const <Class>Accessor* a; int64_t row;}`.
- `STRUCT` fields compose by building the nested `<Inner>Accessor` once from the
  `StructArray`; getters return `RowView` or `std::optional<RowView>`.
- Lists and maps return small span objects. Spans borrow the accessor's cached
  arrays and inner accessors; they must not outlive the accessor.
- Validation remains positional and type-only. Leaf types use
  `DataType::Equals(..., false)`. Composite types validate shape and recurse into
  child types, avoiding whole-composite `DataType::Equals`.
- Non-nullable proto fields are gated by actual runtime `null_count() == 0`,
  recursively for non-nullable composite children.
- Cross-file nested accessors reuse `CollectCrossFileIncludes`, mapping
  `.fletcher.pb.h` to `.fletcher.accessor.pb.h`.

**Carry-forward note (resolved):** `field(i)` direct use, no re-`Slice`.
RBA-4 follows the carry-forward invariant from RBA-2: cache `struct_array->field(i)`
directly, no re-`Slice`. Arrow C++ `StructArray::field(i)` already windows children to
the struct's `[offset, len)`, so both the retained whole-`StructArray` validity
(`struct_validity_->IsNull(row)`) and the cached `field(i)` children share one
struct-logical coordinate origin — row indices line up. The oracle's `Slice` text in §7 / D-RBA-7 is shape-not-final-text and is scheduled for correction in RBA-7.

Recommended split: split RBA-4 into RBA-4a and RBA-4b.

- RBA-4a: `RowView`, `at(row)`, `is_null(row)`, `STRUCT`,
  `REPEATED_SCALAR`, `REPEATED_STRUCT`, and C++ cross-file includes.
- RBA-4b: `MAP` and `NESTED_LIST` depth 2-3.

This split is preferable because RBA-4a proves recursive struct composition and
the borrow/null-safety model before adding the more complex offset trees for maps
and nested lists.

## Design

### Runtime Span Helpers

Create:

```text
arrow-bridge/include/fletcher/arrow_bridge/recordbatch_spans.hpp
```

The existing `arrow_row_view.hpp` helpers are per-row scalar based and use
`GetScalar` in some collection paths. The RBA-4 accessor needs column spans over
cached arrays, so the new header should be independent and explicit about the
borrowed lifetime contract.

Generated accessor headers include:

```cpp
#include <fletcher/arrow_bridge/recordbatch_spans.hpp>
```

The header lives in namespace `fletcher`. It is header-only, so
`arrow-bridge/CMakeLists.txt` does not need a new source entry, but package
install/export rules must continue installing `include/`.

Borrowing contract:

- A span stores raw pointers or references to arrays and nested accessors owned
  by the parent `<Class>Accessor`.
- A `RowView` stores a raw pointer to the nested accessor owned by the parent
  accessor.
- Neither spans nor row views may outlive the accessor they borrow from.
- Accessor copies are fine because cached Arrow handles and inner accessors are
  value members/shared owners. A span remains tied to the specific accessor
  instance that produced it.

Representative helper API:

```cpp
namespace fletcher {

template <class V, class ArrayT>
struct ScalarSpan {
  const ArrayT* values = nullptr;
  int64_t base = 0;
  int64_t len = 0;

  int64_t size() const { return len; }
  bool empty() const { return len == 0; }
  bool is_null(int64_t i) const { return values->IsNull(base + i); }
  V operator[](int64_t i) const { return values->Value(base + i); }
};

template <class AccT>
struct StructSpan {
  const AccT* values = nullptr;
  int64_t base = 0;
  int64_t len = 0;

  int64_t size() const { return len; }
  bool empty() const { return len == 0; }
  bool is_null(int64_t i) const { return values->is_null(base + i); }
  std::optional<typename AccT::RowView> operator[](int64_t i) const {
    const int64_t r = base + i;
    if (values->is_null(r)) return std::nullopt;
    return values->at(r);
  }
};

template <class KV, class KA, class VV, class VA>
struct ScalarMapSpan {
  const KA* keys = nullptr;
  const VA* values = nullptr;
  int64_t base = 0;
  int64_t len = 0;

  int64_t size() const { return len; }
  bool empty() const { return len == 0; }
  KV key(int64_t i) const { return keys->Value(base + i); }
  bool value_is_null(int64_t i) const { return values->IsNull(base + i); }
  VV value(int64_t i) const { return values->Value(base + i); }
};

template <class KV, class KA, class AccT>
struct StructMapSpan {
  const KA* keys = nullptr;
  const AccT* values = nullptr;
  int64_t base = 0;
  int64_t len = 0;

  int64_t size() const { return len; }
  bool empty() const { return len == 0; }
  KV key(int64_t i) const { return keys->Value(base + i); }
  std::optional<typename AccT::RowView> value(int64_t i) const {
    const int64_t r = base + i;
    if (values->is_null(r)) return std::nullopt;
    return values->at(r);
  }
};

template <class AccT, int Depth = 2>
struct NestedStructSpan;

template <class AccT>
struct NestedStructSpan<AccT, 2> {
  const arrow::ListArray* inner_lists = nullptr;
  const AccT* leaf_values = nullptr;
  int64_t base = 0;
  int64_t len = 0;

  int64_t size() const { return len; }
  bool empty() const { return len == 0; }
  bool is_null(int64_t i) const { return inner_lists->IsNull(base + i); }
  std::optional<StructSpan<AccT>> operator[](int64_t i) const {
    const int64_t r = base + i;
    if (inner_lists->IsNull(r)) return std::nullopt;
    return StructSpan<AccT>{
        leaf_values, inner_lists->value_offset(r), inner_lists->value_length(r)};
  }
};

template <class AccT>
struct NestedStructSpan<AccT, 3> {
  const arrow::ListArray* mid_lists = nullptr;
  const arrow::ListArray* inner_lists = nullptr;
  const AccT* leaf_values = nullptr;
  int64_t base = 0;
  int64_t len = 0;

  int64_t size() const { return len; }
  bool empty() const { return len == 0; }
  bool is_null(int64_t i) const { return mid_lists->IsNull(base + i); }
  std::optional<NestedStructSpan<AccT, 2>> operator[](int64_t i) const {
    const int64_t r = base + i;
    if (mid_lists->IsNull(r)) return std::nullopt;
    return NestedStructSpan<AccT, 2>{
        inner_lists, leaf_values, mid_lists->value_offset(r),
        mid_lists->value_length(r)};
  }
};

}  // namespace fletcher
```

For string/binary scalar spans, the generated template value type should be
`std::string_view`, and `operator[]` must use `GetView(base + i)` instead of
`Value(base + i)`. Implement this either with a generated `use_get_view` span
variant or a small span traits hook; do not call `GetScalar`.

### RowView and `at(row)`

Every accessor emits these public members unconditionally:

```cpp
bool is_null(int64_t row) const {
  return struct_validity_ != nullptr && struct_validity_->IsNull(row);
}

struct RowView {
  const FooAccessor* a = nullptr;
  int64_t row = 0;

  int32_t id() const { return a->id(row); }
  std::optional<std::string_view> name() const { return a->name(row); }
  BarAccessor::RowView bar() const { return a->bar(row); }
};

RowView at(int64_t row) const { return RowView{this, row}; }
```

Storage:

```cpp
std::shared_ptr<arrow::StructArray> struct_validity_;
```

When built from a `RecordBatch`, `struct_validity_` is null and `is_null(row)` is
always false. When built from a `StructArray`, `struct_validity_` retains the
struct array, so `is_null(row)` reflects the struct element validity bitmap.

`RowView` forwards every generated getter:

- scalar: no-row forwarding to `a->field(row)`;
- nullable scalar: same optional return as accessor getter;
- non-nullable struct: `InnerAccessor::RowView field() const`;
- nullable struct: `std::optional<InnerAccessor::RowView> field() const`;
- collections: span or optional span, matching the accessor getter.

No `RowView` owns arrays. It is valid only while `*a` is alive.

### Factories and `FromColumns_`

Both factories are emitted for every message, unconditionally:

```cpp
static arrow::Result<FooAccessor> Make(
    const std::shared_ptr<arrow::RecordBatch>& batch);

static arrow::Result<FooAccessor> Make(
    const std::shared_ptr<arrow::StructArray>& struct_array);
```

The `RecordBatch` factory stores schema metadata and no struct validity.

The `StructArray` factory:

- checks null input;
- obtains `length` from `struct_array->length()`;
- obtains fields from `StructType`;
- stores `struct_validity_ = struct_array` on the result;
- builds `cols` from `struct_array->field(i)` directly;
- does not call `Slice` again.

Representative shape:

```cpp
static arrow::Result<FooAccessor> Make(
    const std::shared_ptr<arrow::StructArray>& struct_array) {
  if (struct_array == nullptr)
    return arrow::Status::Invalid("Foo: null StructArray");

  const auto& st =
      *std::static_pointer_cast<arrow::StructType>(struct_array->type());
  arrow::ArrayVector cols;
  cols.reserve(struct_array->num_fields());
  for (int i = 0; i < struct_array->num_fields(); ++i) {
    cols.push_back(struct_array->field(i));  // no re-Slice
  }

  ARROW_ASSIGN_OR_RAISE(
      auto self,
      FromColumns_(struct_array->length(), cols, st.fields(),
                   /*schema_metadata=*/nullptr));
  self.struct_validity_ = struct_array;
  return self;
}
```

`FromColumns_` continues to be the single validation/cache path. It must be
extended by adding new code paths rather than altering non-accessor emitters. The
existing RBA-4 fail-fast comment is replaced only inside the accessor emitter.

Validation helpers should be generated or emitted as local private static
functions to keep `FromColumns_` readable:

```cpp
static arrow::Status CheckNonNullable(
    const arrow::Array& array, std::string_view where) {
  if (array.null_count() != 0)
    return arrow::Status::Invalid(where, ": non-nullable, found ",
                                  array.null_count(), " nulls");
  return arrow::Status::OK();
}
```

For composites, validation must recurse into the expected child layout:

- `STRUCT`: check `Type::STRUCT`, check child count, gate struct null count if the
  field is non-nullable, then call `<Inner>Accessor::Make(struct_array)`.
- `REPEATED_SCALAR`: check `Type::LIST`, gate outer list null count if the field
  is non-nullable, check values child leaf type, cache list and typed values.
- `REPEATED_STRUCT`: check `Type::LIST`, gate outer list null count if
  non-nullable, check values child is `STRUCT`, build `<Inner>Accessor` from the
  values `StructArray`.
- `MAP`: check `Type::MAP`, gate outer map null count if non-nullable, check keys
  leaf type, then either check scalar value leaf type or build value accessor from
  the item `StructArray`.
- `NESTED_LIST`: check each list level shape, gate each non-nullable generated
  level by null count, build leaf accessor from the flattened leaf `StructArray`.
  Inner list levels are exposed as nullable in the span API, regardless of schema
  nullable flags.

Do not gate on child names or Arrow nullable flags. The emitted error text should
keep the current precise form: class, column index, generated field name, expected
shape/type, and actual type.

### STRUCT Getter

Generated storage for a struct field:

```cpp
std::shared_ptr<arrow::StructArray> outer_struct_;
std::optional<OuterAccessor> outer_;
```

Cache both: `outer_struct_` is the row-level null check for nullable 1:1 struct
fields, while `outer_` owns child columns and supplies `RowView`.

`FromColumns_`:

```cpp
const auto& col = cols[i];
if (col->type_id() != arrow::Type::STRUCT)
  return arrow::Status::Invalid("Foo column 1 'outer': expected struct, got ",
                                col->type()->ToString());
auto struct_col = std::static_pointer_cast<arrow::StructArray>(col);
if (!field_is_nullable && struct_col->null_count() != 0)
  return arrow::Status::Invalid("Foo column 1 'outer': non-nullable, found ",
                                struct_col->null_count(), " nulls");
ARROW_ASSIGN_OR_RAISE(auto outer, OuterAccessor::Make(struct_col));
self.outer_struct_ = struct_col;
self.outer_ = std::move(outer);
```

Getter (consistent single example):

```cpp
OuterAccessor::RowView outer(int64_t row) const {
  return outer_->at(row);
}

std::optional<OuterAccessor::RowView> maybe_outer(int64_t row) const {
  if (outer_struct_->IsNull(row)) return std::nullopt;
  return outer_->at(row);
}
```

For a nullable struct field, the getter must check the parent struct column's
validity before returning the inner accessor's view. This is the B2 no-read-
through-null rule.

### REPEATED_SCALAR Getter

Storage:

```cpp
std::shared_ptr<arrow::ListArray> readings_;
std::shared_ptr<arrow::DoubleArray> readings_values_;
```

`FromColumns_`:

```cpp
if (col->type_id() != arrow::Type::LIST)
  return arrow::Status::Invalid("Sample column 0 'readings': expected list, got ",
                                col->type()->ToString());
auto list = std::static_pointer_cast<arrow::ListArray>(col);
if (!field_is_nullable && list->null_count() != 0)
  return arrow::Status::Invalid("Sample column 0 'readings': non-nullable, found ",
                                list->null_count(), " nulls");
auto values = list->values();
const auto expected_value_type = arrow::float64();
if (!values->type()->Equals(*expected_value_type, false))
  return arrow::Status::Invalid("Sample column 0 'readings' values: expected ",
                                expected_value_type->ToString(), ", got ",
                                values->type()->ToString());
self.readings_ = list;
self.readings_values_ = std::static_pointer_cast<arrow::DoubleArray>(values);
```

Getter:

```cpp
fletcher::ScalarSpan<double, arrow::DoubleArray> readings(int64_t row) const {
  return {readings_values_.get(), readings_->value_offset(row),
          readings_->value_length(row)};
}

std::optional<fletcher::ScalarSpan<double, arrow::DoubleArray>>
maybe_readings(int64_t row) const {
  if (maybe_readings_->IsNull(row)) return std::nullopt;
  return fletcher::ScalarSpan<double, arrow::DoubleArray>{
      maybe_readings_values_.get(), maybe_readings_->value_offset(row),
      maybe_readings_->value_length(row)};
}
```

Scalar element nulls are not converted to `optional`. Callers use
`span.is_null(j)`.

### REPEATED_STRUCT Getter

Storage:

```cpp
std::shared_ptr<arrow::ListArray> track_;
std::shared_ptr<arrow::StructArray> track_values_;
std::optional<CoordAccessor> track_inner_;
```

`FromColumns_`:

```cpp
auto list = std::static_pointer_cast<arrow::ListArray>(col);
auto values = list->values();
if (values->type_id() != arrow::Type::STRUCT)
  return arrow::Status::Invalid("Sample column 1 'track' values: expected struct, got ",
                                values->type()->ToString());
auto structs = std::static_pointer_cast<arrow::StructArray>(values);
ARROW_ASSIGN_OR_RAISE(auto inner, CoordAccessor::Make(structs));
self.track_ = list;
self.track_values_ = structs;
self.track_inner_ = std::move(inner);
```

Getter:

```cpp
fletcher::StructSpan<CoordAccessor> track(int64_t row) const {
  return {&*track_inner_, track_->value_offset(row), track_->value_length(row)};
}
```

`StructSpan::operator[](j)` checks `track_inner_->is_null(base + j)` and returns
`std::nullopt` for null struct elements.

### MAP Getter

Arrow map columns are `MapArray` with flattened key and item arrays. Cache the map
structural array plus typed flattened children.

Scalar value map storage:

```cpp
std::shared_ptr<arrow::MapArray> counts_;
std::shared_ptr<arrow::StringArray> counts_keys_;
std::shared_ptr<arrow::Int32Array> counts_values_;
```

Getter:

```cpp
fletcher::ScalarMapSpan<std::string_view, arrow::StringArray,
                        int32_t, arrow::Int32Array>
counts(int64_t row) const {
  return {counts_keys_.get(), counts_values_.get(), counts_->value_offset(row),
          counts_->value_length(row)};
}
```

Message value map storage:

```cpp
std::shared_ptr<arrow::MapArray> waypoints_;
std::shared_ptr<arrow::StringArray> waypoints_keys_;
std::shared_ptr<arrow::StructArray> waypoints_values_;
std::optional<CoordAccessor> waypoints_inner_;
```

Getter:

```cpp
fletcher::StructMapSpan<std::string_view, arrow::StringArray, CoordAccessor>
waypoints(int64_t row) const {
  return {waypoints_keys_.get(), &*waypoints_inner_,
          waypoints_->value_offset(row), waypoints_->value_length(row)};
}
```

`StructMapSpan::value(j)` returns `std::nullopt` when the message value struct is
null. Scalar maps expose `value_is_null(j)` for nullable scalar values.

Implementation detail: use the Arrow C++ `MapArray` key/item accessors available
in the project-pinned Arrow version. If the exact API returns the entries struct
instead, unwrap field 0 as keys and field 1 as items after checking `Type::MAP`.

### NESTED_LIST Getter

Nested list storage must cache every structural list level and the flattened leaf
struct accessor.

**Depth 2 support (in-scope per spec §6):** the design handles `List<List<Struct<T>>>`.

Example:

```cpp
std::shared_ptr<arrow::ListArray> rings_;
std::shared_ptr<arrow::ListArray> rings_inner_lists_;
std::shared_ptr<arrow::StructArray> rings_leaf_values_;
std::optional<CoordAccessor> rings_leaf_;
```

Getter:

```cpp
fletcher::NestedStructSpan<CoordAccessor, 2> rings(int64_t row) const {
  return {rings_inner_lists_.get(), &*rings_leaf_, rings_->value_offset(row),
          rings_->value_length(row)};
}
```

**Depth 3 support (in-scope per spec §6):** the design handles `List<List<List<Struct<T>>>>`.
Storage adds one more cached list level:

```cpp
std::shared_ptr<arrow::ListArray> regions_;
std::shared_ptr<arrow::ListArray> regions_mid_lists_;
std::shared_ptr<arrow::ListArray> regions_inner_lists_;
std::shared_ptr<arrow::StructArray> regions_leaf_values_;
std::optional<CoordAccessor> regions_leaf_;
```

Getter:

```cpp
fletcher::NestedStructSpan<CoordAccessor, 3> regions(int64_t row) const {
  return {regions_mid_lists_.get(), regions_inner_lists_.get(), &*regions_leaf_,
          regions_->value_offset(row), regions_->value_length(row)};
}
```

Each inner list level is nullable in the access API. `span[i]` returns
`std::optional<inner span>`, and the leaf struct span returns
`std::optional<RowView>` per element.

### Cross-File Includes

Accessor headers must include imported accessor headers for nested messages and
map message values. Reuse the existing model:

- `type_mapper` populates `FieldInfo::mapping.nested_header` and
  `map_value_header`.
- `generator.cpp` already has `CollectCrossFileIncludes`.
- RBA-4 calls `CollectCrossFileIncludes` read-only (or adds a new sibling
  collector if needed to avoid altering existing-emitter paths) to compute its own
  `#include` set for the accessor output.
- The emitted include path maps suffix `.fletcher.pb.h` to
  `.fletcher.accessor.pb.h`.

Representative emitted include:

```cpp
#include "dep.fletcher.accessor.pb.h"
```

No new naming convention is introduced. Same-file nested messages need no include.

### Unsupported Constructs

D-RBA-6 requires no silent gap. If `GatherFields`/`type_mapper` reports a kind the
accessor still does not support, emit a comment in the generated accessor header
at the getter/storage site. This is a generation-time comment exactly as the
existing path already does — no silent gap, no weaker semantics.

## Forcing-test mapping

`AccessorTest.CompositeColumnsReadColumnOriented` should cover the full design in
one fixture:

- `STRUCT`, cross-file, and depth >= 2: `a.outer(row).inner().leaf()` verifies
  nested accessors are generated for every message, imported accessor headers are
  included, `RowView` forwards recursively, and the struct factory composes.
- Nullable 1:1 struct field: null row returns `std::nullopt`, proving the getter
  checks the parent struct column before returning a row view.
- Non-nullable struct field with runtime null: `Make` returns an error, proving
  actual `null_count` gating on composites.
- `REPEATED_SCALAR`: `span.size()`, empty list, element reads, and
  `span.is_null(j)` verify offset slicing and scalar null handling.
- `REPEATED_STRUCT`: `span[j] == std::nullopt` for a null struct element proves
  element-level no-read-through-null via the inner accessor's `is_null`.
- `MAP` with scalar values: `ScalarMapSpan` verifies keys, values, empty maps, and
  `value_is_null(j)`.
- `MAP` with message values: `StructMapSpan::value(j) == std::nullopt` for a null
  value verifies message map null safety.
- `NESTED_LIST`: null inner list returns `std::nullopt`; non-null inner list
  reaches a leaf `RowView`. Depth 2 is mandatory, depth 3 should have a small
  additional case if the fixture can stay readable.
- Name tolerance: rename nested/cross-file Arrow fields while keeping order and
  types; `Make` must still succeed.
- Unsupported construct comment: a proto fixture with one unsupported mapped
  construct should assert the documented comment remains visible rather than a
  silently generated wrong getter.

Recommended split test slices:

- RBA-4a green slice: cross-file nested struct with nested child, nullable and
  non-nullable struct null-safety, repeated scalar including empty/null element,
  repeated struct including null element, and no map/nested-list fields.
- RBA-4b green slice: add scalar map, struct map, nested list depth 2, nested list
  depth 3 if supported in the same implementation, plus empty map and null inner
  list assertions.

## Risks/Unknowns

- Arrow C++ `MapArray` child accessor names vary across versions. The
  implementation should confirm the pinned API and centralize unwrapping in one
  helper.
- String/binary spans need `GetView`, while numeric spans use `Value`. A traits
  hook is cleaner than duplicating span classes, but it must not introduce per-
  access allocation or `GetScalar`.
- `std::optional<InnerAccessor>` is already the local pattern for nested
  accessor storage, but using plain value members with delayed assignment may be
  possible. Keep `optional` unless it complicates generated code.
- Composite recursive null gates need a precise definition of which child
  nullability is proto-declared versus Arrow item/value nullable by convention.
  Do not gate on Arrow nullable flags.
- Iterator support for the new spans is useful but not required for the forcing
  test. Land `size`, `empty`, direct indexing, and null probes first.
- Existing scalar-only generated output may change once `RowView`, `at(row)`, and
  `is_null(row)` are emitted unconditionally for every accessor. This is allowed
  only for the opt-gated accessor output, not for pre-RBA outputs. Golden no-drift
  must still prove `.fletcher.pb.h`, `.fletcher.arrow.pb.h`, TypeScript, and IPC
  outputs are byte-identical.

## Files-to-touch

- `arrow-bridge/include/fletcher/arrow_bridge/recordbatch_spans.hpp`: new
  header-only span helper library.
- `arrow-bridge/CMakeLists.txt`: likely no source change, but verify install
  packaging includes the new header.
- `protoc/src/recordbatch_accessor_emitter.cpp`: replace the RBA-4 composite
  fail-fast path with composite validation, storage, getters, `RowView`,
  `at(row)`, `is_null(row)`, span include emission, and struct-source validity
  retention.
- `protoc/src/recordbatch_accessor_emitter.hpp`: update comments only if needed
  for the expanded emitter contract.
- `protoc/src/generator.cpp`: reuse `CollectCrossFileIncludes` read-only for
  accessor output include emission; keep behavior for existing emitters unchanged.
- `protoc/include/type_mapper.hpp` and `protoc/src/type_mapper.cpp`: read-only by
  default. Touch only if RBA-4 reveals missing metadata needed to distinguish
  scalar map values, message map values, nested list depth, or cross-file leaf
  accessors.
- `integration-tests/protoc-arrow-bridge`: add composite accessor proto fixtures,
  CMake generation entries, and C++ gtests for the forcing and acceptance cases.
- Existing no-drift tests: extend expectations only for new accessor outputs; do
  not change existing-output baselines.

## Step-2 review (2026-06-24)

**Verdict: APPROVED** (all items resolved).

**Confirmed items (load-bearing, verified):**

- **`field(i)` no-slice invariant is correct and consistently applied.** The
  `Make(StructArray)` path caches `struct_array->field(i)` directly (no re-`Slice`)
  *and* stores `struct_validity_ = struct_array`. Arrow C++ returns `field(i)`
  already windowed to the struct's `[offset, len)`, and `StructArray::IsNull(row)`
  is offset-aware, so children and validity share one struct-logical coordinate
  origin — both index by the same `row`. RBA-2 proved a re-`Slice` here reads the
  wrong row; this design correctly does not re-slice. **Settled — no escalation.**
- **Coordinate-origin justification added.** Retained whole-`StructArray` validity
  (`struct_validity_->IsNull(row)`) and cached `field(i)` children share one
  struct-logical coordinate origin because Arrow `field(i)` is already
  offset-windowed to `[offset, len)` — row indices line up exactly. This is the
  one-line guarantee that "no re-slice + keep whole-StructArray validity" is
  provably consistent.
- **REPEATED_STRUCT / MAP-message / NESTED_LIST flattened addressing is
  coordinate-consistent.** The inner accessor is built over `list->values()` /
  `map->items()` (the full, offset-0 flattened child), and elements are read at
  `value_offset(row)+j`, which is absolute into that same child; `is_null` reads the
  child's validity at the same flattened index. Holds for sliced outer lists too.
- **All five null paths covered:** nullable 1:1 struct (`maybe_*` getter checks the
  parent struct column's `IsNull(row)`); null repeated-struct element
  (`StructSpan::operator[]` → `nullopt`); null map message value (`StructMapSpan::
  value` → `nullopt`); null inner list (`NestedStructSpan::operator[]` → `nullopt`);
  non-nullable struct w/ runtime null → `Make` error, recursed into composite
  children transitively via the nested `Make`'s own `null_count` gate (D-RBA-4).
- **Unsupported-construct handling clarified (D-RBA-6).** Generation-time comment
  only — no silent gap, no construction-time error (if a construct has no getter
  there is no construction path to error from).
- **Depth-3 NESTED_LIST pinned as in-scope support.** Spell out depth-3 storage
  (outer list + mid list + inner list + leaf struct + leaf accessor); support is
  real and mandatory per spec §6, distinct from the test case being optional.
- **Cross-file `CollectCrossFileIncludes` reuse kept D-RBA-1-clean.** Call
  read-only (or add a new sibling collector if needed); no modification of
  existing-emitter behaviour; Files-to-touch confirms "keep behavior for existing
  emitters unchanged".
- **Split is sound.** RBA-4a (struct + repeated-scalar + repeated-struct + RowView +
  cross-file) and RBA-4b (maps + nested lists) each carry an independently
  red-first-able forcing-test slice, and together they reconstitute the full
  `AccessorTest.CompositeColumnsReadColumnOriented` incl. every null path. **Endorsed
  as written** (4a proves recursive struct composition + the borrow/null-safety
  model before the harder map/nested-list offset trees).

**Non-blocking note:** The struct-getter example now uses consistent single field names
(`outer_`/`outer_struct_` and `maybe_outer_`/`outer_struct_`) to avoid implementer
confusion.