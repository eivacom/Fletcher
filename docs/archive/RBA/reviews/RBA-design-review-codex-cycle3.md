# RBA Design Review - Cycle 3 Spot Check

Reviewer: Codex. Scope: focused spot-check of the B2 and B5 fixes applied in
`dff3e4a` on `feature/recordbatchaccessor`. This is still pre-implementation and
design-doc-only. No design docs or implementation files were modified.

Grounded files read:

- `docs/recordbatch-accessor-spec.md`
- `plans/RBA-locked-decisions.md`
- `plans/RBA-recordbatch-accessor.md`
- `plans/reviews/RBA-design-review-codex-cycle2.md`
- `protoc/src/generator.cpp`
- `protoc/include/type_mapper.hpp`
- `arrow-bridge/include/fletcher/arrow_bridge/arrow_row_view.hpp`

External API spot-checks:

- arrow-rs `MapArray` docs confirm `value_offsets() -> &[i32]`, `value_length() -> i32`, `keys()`, `values()`, and that `MapArray` is its own type, physically list-like but not a `ListArray`: <https://docs.rs/arrow/latest/arrow/array/struct.MapArray.html>
- arrow-rs `ArrayAccessor` docs confirm the reference-type accessor model used by the span examples: <https://docs.rs/arrow/latest/arrow/array/trait.ArrayAccessor.html>

## Verdicts

- **B2 = PARTIALLY.** Collection struct elements are now optional on list/map/nested-list paths, but a non-nullable 1:1 struct field still returns a plain `RowView` even though validation is nullability-tolerant and the accepted runtime column may contain nulls.

- **B5 = PARTIALLY.** Package-keyed Rust paths are the right direction and avoid stem/path collisions, but the design still outputs one `.rs` per proto file while saying same-package files share a module; duplicate inline package modules will collide unless generation is aggregated or emits package fragments.

## BLOCKER

### Non-Nullable 1:1 Struct Fields Can Still Read Through Runtime Nulls
Severity: BLOCKER

Citation: `docs/recordbatch-accessor-spec.md:121-123`, `131-140`, `156-159`, `240-243`; `plans/RBA-locked-decisions.md:74-86`; `plans/RBA-recordbatch-accessor.md:263-270`, `788-797`.

The revised design correctly makes nullable 1:1 struct fields and collection struct elements optional, but it still defines a non-nullable struct field as `fix.position(row).lat()` with a plain `RowView`. The validation model still explicitly does not gate on nullability, so a runtime `StructArray` with a null at that row is accepted even if the proto field is non-nullable. That leaves a direct read-through-null path for 1:1 struct fields, contradicting the new claim that every struct-value path is optional/guarded.

Recommendation: Either validate `null_count == 0` for runtime columns backing non-optional struct getters, or make every struct getter return `optional<RowView>` / `Option<Row>` based on the live struct null bitmap regardless of proto nullability. Also update the stale RBA-4 scope line that still says collection struct elements are plain `RowView` (`plans/RBA-recordbatch-accessor.md:794-797`).

### Same-Package Rust Files Cannot Both Emit the Same Inline Package Module
Severity: BLOCKER

Citation: `docs/recordbatch-accessor-spec.md:323-331`; `plans/RBA-locked-decisions.md:119-125`; output remains per input file per spec `docs/recordbatch-accessor-spec.md:62-68`; current generator output naming is file-based in `protoc/src/generator.cpp:40-52`.

The package-keyed path `crate::fletcher_gen::<pkg-path>::<Class>Accessor` is workable for references, but the mounting story is not. The design says two files in the same package contribute to the same module while each generated `<stem>.fletcher.rs` emits the package `mod` nesting. If two generated files both contain `pub mod navisuite { pub mod sensors { ... } }` and are included under `fletcher_gen`, Rust will reject the duplicate module definition.

Recommendation: Specify one of these concrete strategies before implementation: generate one Rust file per package, generate per-file fragments that assume the package module already exists, or have `build.rs` synthesize an aggregate `fletcher_gen` module that opens each package module once and includes file fragments inside it. The RBA-5 two-file same-package case should be tested, not only two-file two-package imports.

## SHOULD-FIX

### Struct Validity Retention Needs a Constructor Contract
Severity: SHOULD-FIX

Citation: `docs/recordbatch-accessor-spec.md:138-140`, `273-280`; C++ sample `plans/RBA-recordbatch-accessor.md:463-472`, `501-515`; Rust sample `plans/RBA-recordbatch-accessor.md:585-589`.

The fix says the inner accessor retains the struct null bitmap, false for `RecordBatch` sources and windowed for `StructArray` sources. That is the right model, and owning a `shared_ptr<StructArray>` / `Arc<StructArray>` is compatible with the "no source ref needed" claim because it is just another cached Arrow array handle. But the sample `FromColumns_` helper still takes only child columns/fields/meta; it does not show how `Make(StructArray)` passes or installs the struct validity array after slicing children.

Recommendation: Specify the constructor contract explicitly: `Make(StructArray)` must pass the sliced/windowed struct array, or set `struct_validity_` immediately after `FromColumns_` returns, for every accessor built from struct/list/map/nested-list struct values. Add a nonzero-offset sliced struct test that asserts `is_null(0)` reflects the sliced source row.

### Nested List Inner-List Nulls Are Still Not Modeled
Severity: SHOULD-FIX

Citation: `docs/recordbatch-accessor-spec.md:124-128`, `241-243`; C++ `NestedStructSpan::operator[]`, `plans/RBA-recordbatch-accessor.md:447-453`; Rust `NestedStructSpan` comment and usage, `plans/RBA-recordbatch-accessor.md:570-572`, `636-651`; arrow-rs `GenericListArray` docs state list elements may themselves be NULL.

The current B2 fix guards the leaf struct value, but `NestedStructSpan::operator[]` / `get(i)` still returns an inner `StructSpan` for an inner list without checking whether that inner list element is null. For `list<list<struct>>`, the inner `list` item is nullable under the same Arrow child-nullability convention used elsewhere. A null inner list can therefore still produce an apparently usable inner span whose offsets are semantically arbitrary for a null list element.

Recommendation: Make nested-list inner access optional too: `NestedStructSpan::operator[](i)` / `get(i)` should return `optional<StructSpan>` / `Option<StructSpan>` when the inner list is null. The existing usage prose already uses Rust `get(i)?`; align the C++ helper signature and both language samples with that contract.

## NIT

### Package Segment Sanitization Wording Is Internally Inconsistent
Severity: NIT

Citation: `docs/recordbatch-accessor-spec.md:326-328`; `plans/RBA-locked-decisions.md:122-124`.

The spec says identifiers are sanitized to valid Rust using raw identifiers for keywords, then says a non-ident package segment is a generation error, not a silent rename. That is probably intended, but "sanitized" can be read as broader renaming. This is minor but easy to misread during implementation.

Recommendation: Tighten the wording: valid Rust identifiers are emitted directly, Rust keywords use raw identifiers, and all other package segments are rejected.

---

## Resolutions (applied 2026-06-24, after cycle 3)

User decision on the B2 residual: **validate `null_count == 0` at construction** for
proto-non-nullable fields (so non-nullable getters stay plain and are provably
null-free). All cycle-3 items applied:

- **B2 (PARTIALLY → closed).** D-RBA-4 / spec §4 now additionally enforce
  `null_count == 0` for every proto-non-nullable field (recursively for non-nullable
  composite children); a runtime null there is a validation error. Non-nullable
  struct/scalar getters return plain values/`RowView`; nullable fields and all
  collection elements stay `optional`/`Option`. Flat C++/Rust `FromColumns_`/`from_columns`
  show the check. Spec §3 "both ends" paragraph, §4 bullet, D-RBA-4, D-RBA-7.
- **B5 (PARTIALLY → closed).** Generated `.rs` files emit **bare items, no self-`mod`
  wrapper**; the `build.rs` **assembler** declares the `fletcher_gen` tree once
  (one `mod` per package, `include!`-ing all files of that package), so same-package
  multi-file mounting cannot duplicate a `mod`. Spec §8.1, D-RBA-10, RBA-5 build.rs
  scope; test crate adds a same-package two-file case.
- **Should-fix — nested-list intermediate nulls.** Each inner list level is now
  nullable too: C++ `NestedStructSpan::operator[]` and Rust `NestedStructSpan::get`
  return `optional`/`Option` (None on a null inner list); leaf elements remain
  optional. Spec §6 row; both samples + the usage prose.
- **Should-fix — `struct_validity` contract.** Spec §7 now states `is_null` is
  `false` for a `RecordBatch` source and reads the retained (windowed) struct null
  bitmap for a `StructArray` source — the only source-derived state kept.
- **Nit — sanitization wording.** Unified to a single rule (valid ident → as-is;
  keyword → `r#…`; otherwise generation error) stated identically in spec §8.1 and
  D-RBA-10.
