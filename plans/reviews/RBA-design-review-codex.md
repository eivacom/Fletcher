# RBA — Pre-implementation Design Review (Codex)

Reviewer: Codex (via codex-rescue), adversarial pre-implementation critique.
Scope: `docs/recordbatch-accessor-spec.md`, `plans/RBA-locked-decisions.md`,
`plans/RBA-recordbatch-accessor.md`, grounded against `protoc/src/generator.cpp`,
`protoc/include/type_mapper.hpp`, `arrow-bridge/include/fletcher/arrow_bridge/arrow_row_view.hpp`.
No implementation code reviewed (none exists yet). Saved by the main agent
because the Codex sandbox blocked the direct write.

## BLOCKER (resolve before implementation begins)

1. **Rust `from_struct` offset handling is wrong** — plan lines 176-179, 316-318;
   spec §227. A sliced `StructArray` in arrow-rs does not auto-rebase children to
   row zero. Passing `s.columns().to_vec()` directly to `from_columns` on a sliced
   struct produces silently misaligned row reads.

2. **Nullable struct/list/map rows are unsound** — spec §107-111, §192-196; plan
   lines 352-359, 432-449. The design exposes child getters even when the parent
   row is null, with no guard. Nulls in composite parents need either optional
   return types or checked access that blocks reads through null.

3. **Composite `DataType::Equals` contradicts name/nullability tolerance** —
   spec §127-145; locked decision D-RBA-4 lines 34-44; `generator.cpp` lines
   204-214. Using blanket type equality on list/struct/map columns will reject
   valid runtime batches whose child field names differ from the generated schema.

4. **Rust downcast ownership model is unsound** — plan lines 145-156, 716-721. The
   sample clones concrete arrays into `Arc<T>` rather than preserving the original
   `ArrayRef`, creating slicing and offset hazards.

5. **Cross-file nested accessor layout is not designed** — spec §70-73;
   `type_mapper.hpp` lines 47-59; `generator.cpp` lines 85-105. C++ includes and
   Rust module imports for accessors built from messages in imported `.proto` files
   are undefined.

6. **Flatten-wrapper behavior is contradictory** — spec §70-73; plan lines 361-370;
   `generator.cpp` lines 2207, 2800, 2868. The spec skips flatten-wrapper messages
   but the nested-list fixture depends on a flattened `Ring` wrapper. Getter return
   types and composition are undefined.

## SHOULD-FIX

- **C++ struct child slicing drops parent null propagation** (`StructArray::Flatten`
  not used; plan lines 77-82, 243-247).
- **Map/list offset casting is unchecked** — Rust offsets are `i32`, cast to `usize`
  without overflow check; plan lines 432-449.
- **Rust `ArrayAccessor` span bound is likely wrong** — `ArrayAccessor` is
  implemented on references, not owned arrays; plan lines 483-491.
- **`RecordBatch` vs `StructArray` factory metadata differ silently** — spec
  §166-178, §222-225.
- **`field_number`/`field_id` fallback is too vague for schema evolution** — spec
  §159-162; no precedence or duplicate handling defined.
- **No-drift golden test cannot prove global no-change** — spec §43-53;
  `generator.cpp` lines 2901-2968. Option matrix (`schema_only`, `ts`, `ipc`,
  combined) is not covered.
- **Rust crate plan is not buildable** — no `Cargo.toml` exists in the repo;
  `Cargo.toml`, build script, plugin discovery, and CI command are all absent from
  RBA-5.

## NIT

- Factory signatures are inconsistent (by-value `shared_ptr` vs const-ref vs
  reference).
- `schema_only,accessor` emit policy is unspecified.
- Out-of-scope Arrow layouts (fixed-size list, large list, union, extension, large
  string) are not explicitly excluded.
- `field_metadata("name")` lookup conflicts with name-tolerant construction.

---

## Resolutions (applied to the design, 2026-06-24)

Blockers (B2 + B5 decided by the user; rest applied as recommended):

- **B1 — Rust struct-source offset.** `from_struct` now slices each child to the
  struct's `[offset,len)` window explicitly (Rust `column(i).slice(offset,len)`,
  mirroring C++ `field(i)->Slice`); the "arrow-rs slices children with the struct"
  claim removed. Spec §7 "Struct-source offset handling"; D-RBA-7; plan flat+composite
  Rust samples.
- **B2 — nullable composite rows.** USER CHOSE *uniform row-bound struct getters*.
  Struct getters are now `field(row) → <Inner>Accessor::RowView` (C++) / `Row` (Rust),
  `std::optional<…>`/`Option<…>` when nullable, **None on a null row** (no
  read-through-null). Spec §3 access-shape paragraph, §6 table; D-RBA-7; plan
  composite + collection samples; RBA-4/RBA-6 forcing tests assert the null-row case.
- **B3 — composite `DataType::Equals`.** §4 now scopes `DataType::Equals` to
  leaf/scalar columns; composites are gated by shape + recursive child-type checks.
  D-RBA-4 updated.
- **B4 — Rust downcast ownership.** Switched to arrow-rs `downcast_array` (buffer-
  sharing, offset-preserving) instead of a re-`Arc`'d `downcast_ref` clone. Spec §7;
  plan `downcast_array` helper.
- **B5 — cross-file nested accessors.** USER CHOSE *design it now*. C++ reuses
  `CollectCrossFileIncludes` (`.fletcher.pb.h`→`.fletcher.accessor.pb.h`); Rust uses
  a module convention (`crate::fletcher_gen::<stem>::<Class>Accessor`). New spec
  §8.1; new locked decision **D-RBA-10**; RBA-4/RBA-5/RBA-6 carry the work + a
  two-file import fixture.
- **B6 — flatten-wrapper.** Clarified: flatten-wrappers get no accessor; `Ring` is
  absorbed so `rings` is `NESTED_LIST` with leaf `Coord`. Fixture + prose fixed
  (plan collection example); rule stated in spec §1/§2/§6.

Should-fixes: parent-null propagation folded into B2 (no read-through-null);
unchecked offset cast documented as sound (32-bit non-negative list offsets);
`ArrayAccessor` span bound corrected to the reference type; RecordBatch-vs-StructArray
metadata divergence documented (struct source ⇒ empty schema metadata);
`field_number`/`field_id` fallback explicitly deferred (not a silent fallback);
no-drift test now covers the full opt matrix (RBA-1); Rust crate fully specified
(`Cargo.toml`/`build.rs`/plugin discovery/CI, RBA-5).

Nits: factory signatures standardised (C++ const-ref; Rust owned-`try_new` /
borrowed-`from_struct` is intentional); `schema_only,accessor` policy = orthogonal
(spec §2, D-RBA-2); out-of-scope Arrow layouts listed (spec §10); `field_metadata(i)`
by-index canonical, `field_metadata("name")` best-effort (spec §5).
