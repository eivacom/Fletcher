# RBA Design Review - Cycle 2

Reviewer: Codex. Scope: verification re-review of the revised RBA design docs
committed as `ebcff05` on `feature/recordbatchaccessor`. This remains a
pre-implementation review; no design docs or implementation files were modified.

Grounded files read:

- `docs/recordbatch-accessor-spec.md`
- `plans/RBA-locked-decisions.md`
- `plans/RBA-recordbatch-accessor.md`
- `plans/reviews/RBA-design-review-codex.md`
- `protoc/src/generator.cpp`
- `protoc/include/type_mapper.hpp`
- `arrow-bridge/include/fletcher/arrow_bridge/arrow_row_view.hpp`

External API checks used primary docs:

- arrow-rs `downcast_array`: <https://docs.rs/arrow/latest/arrow/array/fn.downcast_array.html>
- arrow-rs `ArrayAccessor`: <https://docs.rs/arrow/latest/arrow/array/trait.ArrayAccessor.html>
- arrow-rs `StructArray`: <https://docs.rs/arrow/latest/arrow/array/struct.StructArray.html>
- arrow-rs `GenericListArray`: <https://docs.rs/arrow/latest/arrow/array/struct.GenericListArray.html>
- arrow-rs `MapArray`: <https://docs.rs/arrow/latest/arrow/array/struct.MapArray.html>
- Arrow C++ arrays: <https://arrow.apache.org/docs/cpp/api/array.html>

## Prior Finding Verdicts

- **B1 Rust struct-offset: CLOSED.** `from_struct` now slices every struct child to the struct window before `from_columns` (`docs/recordbatch-accessor-spec.md:273-280`, `plans/RBA-recordbatch-accessor.md:179-184`, `330-336`), and arrow-rs `StructArray::columns()` / `slice()` docs support this correction.

- **B2 nullable composites: NOT-CLOSED.** The 1:1 nullable struct getter shape is improved (`docs/recordbatch-accessor-spec.md:121-126`), but list/map struct elements still return unconditional `RowView`s (`plans/RBA-recordbatch-accessor.md:414-429`, `521-530`) and the broader nullability-tolerant validation model still allows nulls behind non-optional getters.

- **B3 leaf-vs-composite type gate: CLOSED.** The spec now clearly limits `DataType::Equals` to leaf/scalar columns and requires shape plus recursive child-type checks for composites (`docs/recordbatch-accessor-spec.md:146-176`; `plans/RBA-locked-decisions.md:37-48`).

- **B4 Rust downcast/span bounds: PARTIALLY.** The sample helper now uses `arrow::array::downcast_array::<T>` with the documented `T: From<ArrayData>` bound (`plans/RBA-recordbatch-accessor.md:149-158`), and the `ArrayAccessor` reference-type span shape is conceptually aligned with arrow-rs. However RBA-5 still says the Rust emitter uses `as_any().downcast_ref` (`plans/RBA-recordbatch-accessor.md:755-758`), contradicting the revised spec and sample.

- **B5 cross-file: PARTIALLY.** C++ include remapping is now grounded in the existing include mechanism (`docs/recordbatch-accessor-spec.md:300-304`; `protoc/src/generator.cpp:95-111`, `2819-2836`). The Rust module convention is still not workable as specified for real proto paths/stems (`docs/recordbatch-accessor-spec.md:306-318`).

- **B6 flatten-wrapper: CLOSED.** The plan now states that `Ring` is absorbed, no `RingAccessor` is generated, and `rings` is a `NESTED_LIST` with leaf `Coord` (`plans/RBA-recordbatch-accessor.md:384-399`), matching the type mapper's flatten-wrapper handling (`protoc/src/type_mapper.cpp:486-585`; `protoc/include/type_mapper.hpp:75-80`).

## BLOCKER

### Nullability-Tolerant Validation Still Allows Read-Through-Null
Severity: BLOCKER

Citation: `docs/recordbatch-accessor-spec.md:121-126`, `156-159`, `229-236`; `plans/RBA-recordbatch-accessor.md:408-429`, `455-475`, `515-530`, `564-582`; arrow-rs `ArrayAccessor` validity docs state values at null indexes are unspecified.

The revised design fixes the obvious 1:1 nullable struct getter by returning `optional<RowView>` / `Option<Row>`, but it does not close the null-read problem. Validation explicitly does not gate on nullability, so a runtime batch can be accepted with nulls in a field whose generated getter returns a plain value or a plain span. The collection span examples also return struct element `RowView`s unconditionally, so a null list item or null map message value still exposes child getters through `track(row)[j].x()` / `waypoints(row).value(j).x()`.

Recommendation: Decide a single null contract that covers runtime data, not just proto-declared optionality. Either validate `null_count == 0` for every non-optional generated getter, or make getters/spans expose optional values whenever the live array can contain nulls; for struct spans and map message values, add `is_null(j)` plus `optional<RowView>`/`Option<Row>` element access, not unconditional `RowView`.

### Rust Map Spans Use `ListArray` APIs on `MapArray`
Severity: BLOCKER

Citation: `plans/RBA-recordbatch-accessor.md:557-562`, `572-578`; arrow-rs `MapArray` docs list `value_offsets`, `value_length`, `keys`, `values`, but `MapArray` is a distinct struct, not a `ListArray`.

The Rust helper `span_bounds(list: &ListArray, row: usize)` is called with `&self.counts` and `&self.waypoints`, whose type is `Arc<MapArray>`. The comment says "MapArray is a ListArray", but arrow-rs exposes `MapArray` as its own type with its own methods. The generated Rust sample will not compile as written, and the design still has no generic bounds strategy for list-like offset providers.

Recommendation: Split bounds helpers: `list_span_bounds(&ListArray, row)` and `map_span_bounds(&MapArray, row)`, or introduce a local trait implemented for the exact pinned arrow-rs version. Add a Rust compile test that exercises scalar maps and message maps before claiming RBA-6 parity.

### Rust Cross-File Module Convention Does Not Handle Real Proto Stems
Severity: BLOCKER

Citation: `docs/recordbatch-accessor-spec.md:306-318`; `plans/RBA-locked-decisions.md:100-107`; current filename/stem behavior in `protoc/src/generator.cpp:40-52`; Rust module identifiers must be valid Rust identifiers.

The spec says each generated `<stem>.fletcher.rs` is mounted as `crate::fletcher_gen::<stem>`, but the repo's stem convention preserves proto paths before stripping `.proto`. A proto such as `foo/bar.proto`, `my-schema.proto`, or two different directories containing `common.proto` cannot be represented safely by a raw `<stem>` Rust module. The "package namespace below stem" claim does not fix path separators, invalid identifier characters, Rust keywords, or basename collisions.

Recommendation: Define a deterministic Rust module-name sanitizer and collision strategy. Better: mount generated files by sanitized proto path components under `fletcher_gen`, then put package modules below that; require RBA-5's two-file fixture to include a subdirectory or otherwise collision-prone stem.

## SHOULD-FIX

### RBA-5 Still Contradicts the Downcast Decision
Severity: SHOULD-FIX

Citation: spec `docs/recordbatch-accessor-spec.md:266-270`; plan sample `plans/RBA-recordbatch-accessor.md:149-158`; RBA-5 scope `plans/RBA-recordbatch-accessor.md:755-758`; arrow-rs `downcast_array` docs show `pub fn downcast_array<T>(array: &dyn Array) -> T where T: From<ArrayData>`.

The revised sample uses the right arrow-rs primitive for offset-preserving, buffer-sharing concrete arrays. But the RBA-5 scope still instructs the emitter to use `as_any().downcast_ref`, which was the old rejected model. That contradiction is likely to reintroduce B4 during implementation.

Recommendation: Update RBA-5 scope to name `downcast_array` / `AsArray` only, and explicitly forbid re-wrapping a `downcast_ref` clone.

### Span Bounds Need to Explicitly Account for Sliced List Semantics
Severity: SHOULD-FIX

Citation: `plans/RBA-recordbatch-accessor.md:431-437`, `557-562`; arrow-rs `GenericListArray` docs explain that slicing does not slice values and offsets may not start at 0.

The current list span formulas are probably correct when `value_offsets()` / `value_offset()` are used as absolute indexes into the unsliced values array, but the design does not state this invariant. That matters because `from_struct` now intentionally slices list/map child arrays, making sliced-list behavior mandatory, not an edge case. The implementation must not normalize offsets to zero unless it also slices the values child consistently.

Recommendation: Add an explicit invariant to the span helper design: `base` is an absolute index into the cached values/entries child, not a row-relative offset. Add tests where a `StructArray` containing list/map fields is sliced with nonzero offset before accessor construction.

### RowView Lifetime Is Not Documented as Borrowed
Severity: SHOULD-FIX

Citation: `docs/recordbatch-accessor-spec.md:116-133`; C++ sample `plans/RBA-recordbatch-accessor.md:441-448`; Rust sample `plans/RBA-recordbatch-accessor.md:533-538`.

Rust encodes the row-view borrow in `CoordRow<'a>`, but the C++ `RowView` is just `{const CoordAccessor* a; int64_t row}` and can outlive the accessor if a caller stores it. That is not memory-safe by construction; it is a borrowed view with an unenforced lifetime. The design currently calls the forwarder "sound" without stating this limitation.

Recommendation: Document `RowView` as a non-owning borrowed view whose lifetime must not exceed the accessor, or make the C++ view hold a `std::shared_ptr<const Impl>` / owning handle if independent lifetime is required. Add a short generated comment in the C++ API.

## NIT

### Collection Samples Still Omit Optional Return Forms
Severity: NIT

Citation: spec `docs/recordbatch-accessor-spec.md:124-126`, `232-236`; C++ sample `plans/RBA-recordbatch-accessor.md:455-475`; Rust sample `plans/RBA-recordbatch-accessor.md:564-582`.

The spec says nullable list/map/nested-list fields return `optional<Span>` / `Option<Span>`, but the illustrative samples show only plain spans and do not show where the null row check lives. Given B2, this omission is no longer harmless: implementers need exact examples for null-row collection behavior.

Recommendation: Add one nullable collection example in each language, including the `IsNull(row)` / `is_null(row)` check and the returned optional span type.

### C++ `ScalarSpan` String Return Type Is Under-Specified
Severity: NIT

Citation: C++ helper sample `plans/RBA-recordbatch-accessor.md:408-413`; spec string getter note `docs/recordbatch-accessor-spec.md:231`.

The helper uses `vals->Value(base + i)` for every scalar `ArrayT`, but the spec wants string/binary getters to return `std::string_view`. Arrow C++ string arrays commonly distinguish owning/string-returning APIs from view-returning APIs. This is a small API-shape ambiguity, but it can cause either copies or the wrong return type.

Recommendation: Specify a scalar accessor traits table/function for C++ spans, so string/binary use the same view-returning API as scalar getters.

---

## Resolutions (applied 2026-06-24, after cycle 2)

- **B2 (NOT-CLOSED → closed).** Read-through-null is now blocked on **every** struct
  path: 1:1 fields, list elements, map values, and nested-list leaves all return
  `std::optional<RowView>` / `Option<Row>`, `None` on a null. The inner accessor
  exposes `is_null(row)` (false for a RecordBatch source; from the retained struct
  null bitmap for a StructArray source); spans check it. Spec §3 (access-shape +
  "key closure" paragraph), §6 table; D-RBA-7; C++ `StructSpan`/`StructMapSpan` and
  Rust `RowAccess`-bounded spans return optionals.
- **Rust map-span bug (new blocker → fixed).** Added a separate `map_bounds(&MapArray)`
  over `MapArray::value_offsets()`; `counts`/`waypoints` no longer call the
  `ListArray` `list_bounds` helper.
- **B4 (PARTIALLY → closed).** RBA-5 scope text now says `downcast_array` (offset-
  preserving), matching the spec/sample; `downcast_ref` is noted as used only to
  *test* a column is a `StructArray` before recursing.
- **B5 (PARTIALLY → closed).** Rust module convention re-keyed on the proto
  **package** (prost/tonic-style) — `crate::fletcher_gen::<pkg-path>::<Class>Accessor`
  — never the file stem (stems may carry `-`/`.`/path separators and collide).
  Same-package files share a module; non-ident segments are a generation error.
  Spec §8.1; D-RBA-10; RBA-6 scope; test crate uses a two-file, two-package import.
- **Should-fixes.** Sliced list/map span invariant documented (C++ span-helper
  INVARIANT comment + spec §7); `RowView`/span lifetime documented as borrowing the
  accessor (C++ + Rust comments, D-RBA-7, spec §3).
- **Nits (carried, not blocking).** The two cycle-2 nits — a nullable *collection*
  example showing the optional-span / `is_null(row)` check, and a C++ scalar-traits
  note so string/binary spans return `string_view` — are accepted as small
  clarifications to fold in during RBA-2/RBA-4 implementation (the spec already
  states both intents in §3/§6/§231); not gating.
