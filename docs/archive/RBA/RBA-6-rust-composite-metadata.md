# RBA-6 Rust Composite Accessor + Generic Metadata

Author: Codex --wait

## Summary

RBA-6 extends the Rust RecordBatch accessor from the RBA-5 scalar baseline to the
same composite and metadata surface already designed for the C++ accessor in
RBA-4. The Rust accessor must read arbitrary schema and field metadata, compose
nested message accessors recursively, expose zero-copy list/map/nested-list spans,
and resolve cross-file nested messages through the D-RBA-10 package-keyed module
tree.

Critical decision: span and row helper types should be emitted once by the Rust
test/build assembler into a shared generated prelude module,
`crate::fletcher_gen::__rba`, and generated accessor files should import from that
module. Do not emit helper definitions into every `.fletcher.rs` file, because
same-package multi-file assembly includes multiple generated files into one module
and duplicate helper definitions would collide. This keeps RBA-5's self-contained
generated-crate model without adding a hand-written runtime crate dependency.

No locked decision is in tension with this design. The Rust path follows the
oracle/D-RBA-7 requirement that `from_struct(&StructArray)` slices children to the
struct window before downcasting.

## Design

### Metadata Getters

Every generated accessor stores:

```rust
fields: Fields,
metadata: HashMap<String, String>,
```

and exposes:

```rust
pub fn schema_metadata(&self) -> &HashMap<String, String> { &self.metadata }

pub fn field_metadata(&self, i: usize) -> &HashMap<String, String> {
    self.fields.get(i).map(|f| f.metadata()).unwrap_or(Self::empty_metadata())
}
```

Return type is `&HashMap<String, String>`. The returned reference is borrowed from
the accessor and must not outlive it. The getter never errors. Missing schema
metadata is stored as an empty map. Missing field metadata returns an empty static
map rather than panicking; if the pinned Rust toolchain makes a static
`HashMap::new()` awkward, generate a private accessor-level `empty_metadata()`
using `OnceLock<HashMap<String, String>>`.

`try_new(RecordBatch)` copies the live batch schema metadata with
`schema.metadata().clone()` and stores the live `Fields`. `from_struct(&StructArray)`
stores the struct type's `Fields` and an empty schema metadata map, because a
`StructArray` has field metadata but no schema-level metadata. Metadata is
generic and domain-agnostic: no Fletcher or domain key is special-cased, and
metadata never participates in validation.

By-index field metadata is the required API and aligns with positional validation.
A by-name convenience getter is not part of the forcing test and should be
deferred unless already present in RBA-5; if added later, it must be best-effort
over the live field names and must not replace by-index access.

### Span and Row Helpers

The assembler emits one shared helper module:

```rust
pub mod fletcher_gen {
    pub mod __rba {
        // RowAccess, ScalarSpan, StructSpan, ScalarMapSpan, StructMapSpan,
        // NestedStructSpan, optional depth-3 nested span.
    }

    pub mod package { include!(...); }
}
```

Generated accessor files remain bare accessor items, matching D-RBA-10. They do
not declare their own package `mod`, and they do not define helper types. They
reference helpers by **fully-qualified path only** (`crate::fletcher_gen::__rba::ScalarSpan`,
etc.) — **not** via any per-file `use` imports — so that two same-package files
co-mounted into one module (via the assembler's per-package `include!`) do not collide on duplicate
imports or definitions (see Step-2 review R1: this is the RBA-5 as-built constraint
proven in code). The generated getters and field storage must emit only
fully-qualified paths when referencing `__rba` items.

Helper shape:

```rust
pub trait RowAccess {
    type Row<'a>
    where
        Self: 'a;

    fn row(&self, row: usize) -> Self::Row<'_>;
    fn is_null(&self, row: usize) -> bool;
}

pub struct ScalarSpan<A: ArrayAccessor> { vals: A, base: usize, len: usize }
pub struct StructSpan<'a, Acc: RowAccess> { inner: &'a Acc, base: usize, len: usize }
pub struct ScalarMapSpan<K: ArrayAccessor, V: ArrayAccessor> { keys: K, vals: V, base: usize, len: usize }
pub struct StructMapSpan<'a, K: ArrayAccessor, Acc: RowAccess> { keys: K, vals: &'a Acc, base: usize, len: usize }
pub struct NestedStructSpan<'a, Acc: RowAccess> { mid: &'a ListArray, leaf: &'a Acc, base: usize, len: usize }
```

Fields should stay private and helpers should expose `pub(crate) fn new(...)`
constructors so generated accessor modules can construct spans without exposing
their internals publicly.

Public helper methods:

- `ScalarSpan::len`, `is_empty`, `is_null(i)`, `value(i) -> A::Item`.
- `StructSpan::len`, `is_empty`, `is_null(i)`, `get(i) -> Option<Acc::Row<'_>>`.
- `ScalarMapSpan::len`, `is_empty`, `key(i)`, `value_is_null(i)`, `value(i)`.
- `StructMapSpan::len`, `is_empty`, `key(i)`, `value(i) -> Option<Acc::Row<'_>>`.
- `NestedStructSpan::len`, `is_empty`, `is_null(i)`, `get(i) -> Option<StructSpan<'a, Acc>>`.

`ArrayAccessor` is held by value, and for generated code the concrete type is a
reference such as `&'a Float64Array` or `&'a StringArray`. That lets one
`ScalarSpan` cover primitive, string, and binary scalar arrays using arrow-rs's
reference implementations.

Every generated accessor emits a row forwarder and `RowAccess` implementation:

```rust
pub struct CoordRow<'a> {
    a: &'a CoordAccessor,
    row: usize,
}

impl CoordRow<'_> {
    pub fn x(&self) -> f64 { self.a.x(self.row) }
    pub fn y(&self) -> f64 { self.a.y(self.row) }
}

impl RowAccess for CoordAccessor {
    type Row<'a> = CoordRow<'a>;

    fn row(&self, row: usize) -> CoordRow<'_> { CoordRow { a: self, row } }

    fn is_null(&self, row: usize) -> bool {
        self.struct_validity.as_ref().map_or(false, |s| s.is_null(row))
    }
}
```

The borrow contract is explicit: rows and spans borrow the accessor and its cached
arrays. They must not outlive the accessor instance that produced them. The
accessor owns all Arrow array handles (`Arc<T>`) and nested accessors by value, so
the source `RecordBatch` or `StructArray` can be dropped after construction.

### Composite Recursion and `from_columns`

Every accessor keeps the RBA-5 dual constructors:

```rust
pub fn try_new(batch: RecordBatch) -> Result<Self, ArrowError>;
pub fn from_struct(s: &StructArray) -> Result<Self, ArrowError>;
fn from_columns(
    num_rows: usize,
    cols: Vec<ArrayRef>,
    fields: Fields,
    metadata: HashMap<String, String>,
    struct_validity: Option<Arc<StructArray>>,
) -> Result<Self, ArrowError>;
```

`from_struct` must:

- verify the input is a struct array by construction and extract its `Fields`;
- slice every child to `s.offset(), s.len()` before downcasting or recursing;
- retain a sliced/windowed struct validity handle for `is_null(row)`;
- pass an empty schema metadata map.

If arrow-rs has no direct cheap "sliced `StructArray` as Arc" helper in the pinned
version, build the retained validity representation in one helper and keep the
same row coordinate origin as the sliced child columns. Do not use unsliced child
columns with sliced validity.

`from_columns` validates column count first, then validates each field
positionally and type-only. Scalar leaf fields keep the RBA-5 exact `DataType`
gate and offset-preserving `downcast_array` cache path. Composite fields validate
shape and recurse; they never compare an entire composite `DataType`, because that
would gate on child names and nullable flags.

Non-nullable proto fields are checked with actual runtime `null_count() == 0`.
This gate recurses through composites exactly as D-RBA-4 requires. Nullable fields
skip the null-count gate and surface nulls through `Option` getters or span null
probes. All validation failures return `Err(ArrowError::SchemaError(...))`; the
generated constructors and getters must not panic as part of validation.

Per-kind construction:

- `STRUCT`: check the column is `StructArray`; if proto non-nullable, require
  `null_count() == 0`; build `<Inner>Accessor::from_struct(struct_col)` once and
  cache both the parent struct array for row-level null checks and the inner
  accessor for row forwarding. Non-nullable getter returns `<Inner>Row<'_>`;
  nullable getter returns `Option<<Inner>Row<'_>>`, `None` when the parent struct
  column is null at that row.
- `REPEATED_SCALAR`: check `ListArray`; if the field is non-nullable, gate the
  outer list `null_count`; check the values child leaf type; cache the list and
  typed values child. Getter returns `ScalarSpan<&TArray>` or `Option<ScalarSpan>`
  for row-nullable lists.
- `REPEATED_STRUCT`: check `ListArray`; gate outer null count when non-nullable;
  check values child is `StructArray`; build the leaf `<Inner>Accessor` from the
  flattened values. Getter returns `StructSpan`; `StructSpan::get(j)` uses
  `inner.is_null(base + j)` and returns `None` for null struct elements.
- `MAP` scalar value: check `MapArray`; gate outer null count when non-nullable;
  check key type and scalar item type; cache the map, typed keys, and typed item
  arrays. Getter uses `MapArray::value_offsets()` for row bounds and exposes
  scalar value nulls through `value_is_null(j)`.
- `MAP` message value: same map checks, but item/value child must be
  `StructArray`; build the message value accessor from the flattened item struct.
  `StructMapSpan::value(j)` returns `None` when the value struct is null.
- `NESTED_LIST`: descend through the generated nested-list depth, caching each
  structural `ListArray` level and the flattened leaf accessor. Each inner list
  level is nullable in the API: `NestedStructSpan::get(i)` returns `None` when the
  inner list is null. Leaf struct elements still return `Option<Row>` through
  `StructSpan::get(j)`.

List and map row bounds are derived from their own structural arrays. Do not treat
`MapArray` as a `ListArray`; generate a separate bounds helper over
`MapArray::value_offsets()`. The helper names should make the coordinate system
clear:

```rust
fn list_bounds(l: &ListArray, row: usize) -> (usize, usize);
fn map_bounds(m: &MapArray, row: usize) -> (usize, usize);
```

Both return the flattened child `[base, len)` for a parent row and are used only
after the structural array has been validated and cached. The generated code must
not compute collection spans from temporary arrays or from a mismatched sliced
child; B1/B4 are preserved by slicing struct children in `from_struct` and caching
typed arrays with the offset-preserving `downcast_array` path.

### Cross-File Package Modules

Rust cross-file nested accessors resolve by package, never by file stem:

```rust
crate::fletcher_gen::<pkg-path>::<Class>Accessor
```

where `pkg-path` is the proto package split on `.` and sanitized with the
D-RBA-10 rule: Rust keyword segments become raw identifiers (`r#type`), valid
proto identifier segments are otherwise unchanged, and an invalid segment is a
generation error.

Generated `.fletcher.rs` files emit bare accessor items only. The assembler groups
all generated files by proto package and emits one module tree under
`fletcher_gen`, including every file of the same package in the same innermost
module. Two files in the same package therefore share one module; two files with
the same stem but different packages cannot collide.

The nested-message type resolver must emit fully qualified accessor paths for
imported messages and can use local unqualified names for same-package messages
only when the generated item is in the same assembled module. The forcing fixture
should include:

- file A, package `rba.main`, defining the top-level message;
- file B, package `rba.child`, defining a child message used by a nested struct;
- a same-package second file contributing another message to prove the assembler
does not duplicate package modules.

The top-level chain should include a nested struct whose child is itself a struct
from the second file/package, so the test calls a row chain equivalent to:

```rust
accessor.outer(row).inner().leaf()
```

with at least one segment resolved through
`crate::fletcher_gen::rba::child::<Class>Accessor`.

### Fixture

The `composite_and_metadata_read` Cargo test should build a real arrow-rs
`RecordBatch` with arbitrary schema metadata and arbitrary per-field metadata.
The fixture must include:

- nested 1:1 struct with a cross-file, second-package child struct;
- `REPEATED_SCALAR`;
- `REPEATED_STRUCT`;
- `NESTED_LIST` as `List<List<Struct<...>>>` at minimum;
- `map<..., scalar>`;
- `map<..., message>`.
**Same-package two-file fixture:** to exercise R1's fully-qualified path + co-mount
collision check, include a second file in the same package (e.g. package `rba.main`)
defining a second message. Both files must emit at least one *composite* accessor
(STRUCT, REPEATED_STRUCT, or list/map getter) so that when co-mounted by the
assembler into one `mod rba::main`, the fully-qualified `crate::fletcher_gen::__rba::`
helper paths in both files are actually exercised and prove no collision.

The same fixture should include positive rows and null rows/elements for each
optional path. A second batch or modified column should inject a runtime null into
a proto non-nullable struct field and assert construction returns `Err`.

## Forcing-Test Mapping

- Arbitrary schema metadata read-back: `schema_metadata()` returns the live
  `RecordBatch` schema map by borrowed reference and returns an empty map for
  struct-sourced accessors.
- Arbitrary field metadata read-back: `field_metadata(i)` returns the live field
  metadata map by index, never by hardcoded key.
- Nested struct with child from a second `.proto` in a second package: package
  assembler emits `crate::fletcher_gen::<pkg-path>::<Class>Accessor`, and
  `STRUCT` recursion builds the imported child accessor through `from_struct`.
- `REPEATED_SCALAR`: list structural array plus typed flattened values are cached;
  row getter returns `ScalarSpan`.
- `REPEATED_STRUCT`: list structural array plus flattened inner accessor are
  cached; `StructSpan::get(j)` returns `Option<Row>`.
- `NESTED_LIST`: each list level is cached; `NestedStructSpan::get(i)` returns
  `None` on a null inner list and otherwise a struct span over the leaf accessor.
- `map<..., scalar>`: `ScalarMapSpan` exposes key/value reads and
  `value_is_null(j)` over flattened map children.
- `map<..., message>`: `StructMapSpan::value(j)` returns `Option<Row>` and checks
  the message value accessor's retained struct validity.
- Nullable 1:1 struct row: getter checks the parent struct column validity and
  returns `None` before any row forwarder is exposed.
- Null struct element: `StructSpan::get(j)` checks `inner.is_null(base + j)` and
  returns `None`.
- Null map message value: `StructMapSpan::value(j)` checks `vals.is_null(base + j)`
  and returns `None`.
- Non-nullable struct field with runtime null: `from_columns` gates actual
  `null_count() == 0` for proto non-nullable struct fields and returns `Err`.
- RBA-5 scalar + no-drift + C++ tests stay green: all code paths are opt-gated to
  Rust accessor generation and the existing emitters remain unchanged.

## Risks/Unknowns

- Cross-file import in Rust: the module assembler must be the single source of the
  `fletcher_gen` tree. If individual generated files start emitting package
  modules again, same-package multi-file fixtures will fail or duplicate items.
- Span lifetime borrowing: helper constructors must make it impossible for spans
  to own temporary downcast references. Generated getters should borrow from
  cached `Arc<T>` fields only, never from temporary `ArrayRef` values.
- `StructArray` validity windowing in arrow-rs: implementation must confirm the
  pinned API for retaining a sliced validity source that shares row coordinates
  with sliced children.
- Static empty metadata map: choose `OnceLock` or an equivalent pinned-toolchain
  pattern; do not make absent metadata an error and do not index with `fields[i]`
  in a way that can panic.
- Map child accessors vary across arrow-rs versions. Centralize map unwrapping in
  generated helper code or one emitter routine so scalar-map and struct-map paths
  cannot drift.

## Split Recommendation

Split RBA-6 if the implementation is heavy, mirroring the successful RBA-4a/4b
shape.

Recommended RBA-6a:

- metadata getters;
- shared `__rba` helper module;
- `RowAccess`, `<Class>Row`, `row()`, and `is_null()`;
- `STRUCT`, `REPEATED_SCALAR`, and `REPEATED_STRUCT`;
- cross-file package module resolution with the two-file/two-package fixture.

Recommended RBA-6b:

- `MAP` scalar values;
- `MAP` message values;
- `NESTED_LIST` depth 2 and any already-supported depth 3;
- remaining null-path assertions for map message values and null inner lists.

This split is preferable because RBA-6a proves the borrow model, struct recursion,
metadata, and package assembler before adding the more complex map and nested-list
offset trees. The full forcing test remains the acceptance target for RBA-6 as a
whole.

## Files-to-touch

- `protoc/src/rust_recordbatch_accessor_emitter.cpp` or the existing Rust
  accessor emitter source: emit the `__rba.fletcher.rs` file with helper
  implementations (RowAccess, ScalarSpan, etc.) **exactly once** per protoc run,
  versioned with the pinned arrow `=59.0.0`; emit per-message accessors in
  separate `.fletcher.rs` files referencing `__rba` by fully-qualified paths
  (no per-file `use`). The plugin owns all `__rba` arrow-API definitions so
  they stay in sync with the per-impl getters that instantiate them.
- `protoc/src/recordbatch_accessor_emitter.cpp` / shared accessor helpers only if
  Rust and C++ share emitter utility code; keep existing C++ accessor behavior
  unchanged.
- `protoc/src/generator.cpp`: keep option parsing opt-gated; if needed, pass the
  package/import context to the Rust accessor emitter without changing existing
  emitter outputs.
- Rust generated-crate `build.rs` assembler from RBA-5: `include!` the
  plugin-emitted `__rba.fletcher.rs` file **exactly once** directly under
  `crate::fletcher_gen::__rba`, group other `.fletcher.rs` files by proto
  package under `crate::fletcher_gen::<pkg>`, and include same-package accessor
  files together in one module (one `include!` call per package). The assembler
  declares `fletcher_gen` **once** and never duplicates per-file or per-package
  `mod` blocks. The `__rba` module is emitted once by the plugin and included
  once by the assembler, ensuring helper and getter definitions stay versioned
  together.
- Rust Cargo test fixture protos: add the two-file/two-package import fixture and
  a same-package two-file fixture.
- Rust Cargo tests: add `composite_and_metadata_read` plus the non-nullable
  runtime-null error case.
- No-drift tests: verify existing generated outputs remain byte-identical and only
  new Rust accessor outputs change under `--fletcher_opt=rust`.

## Step-2 review — round 2 (2026-06-24)

**Verdict: APPROVE.** All 3 round-1 blocking items (R1–R3) are resolved in the
doc; R4–R7 and the RBA-6a/6b split are intact. One non-blocking residual (N1)
to confirm at implementation. No locked-decision deviation; no STOP-AND-ASK.

Verified against the revised text:

- **R1 — RESOLVED.** Span/Row Helpers section (L81–88) now mandates
  **fully-qualified path only** (`crate::fletcher_gen::__rba::ScalarSpan`, etc.),
  **no per-file `use`** — explicitly tied to the RBA-5 as-built co-mount
  constraint (E0252/E0428). The same-package two-file fixture (L291–296) now
  requires **both** files to emit a composite getter so the collision check is
  actually exercised, not asserted away.
- **R2 — RESOLVED.** `__rba` is now `pub mod` (L72), so the `pub fn` getter return
  types (`StructSpan`/`ScalarSpan`/…) are nameable at the public boundary; this
  removes the `private_interfaces` defect. (Span *fields* stay private with
  `pub(crate) fn new` constructors, L109–111 — correct: the types are public, their
  internals are not.)
- **R3 — RESOLVED.** Emission owner pinned: the **plugin** emits
  `__rba.fletcher.rs` exactly once, versioned with arrow `=59.0.0`, owning all
  helper arrow-API definitions so they stay in lock-step with the per-`impl`
  getters that instantiate them (L378–384); the `build.rs` **assembler `include!`s
  it exactly once directly under `crate::fletcher_gen::__rba`** (L391–399), with
  `fletcher_gen` declared once and no per-file/per-package `mod` duplication.

R4 (sliced-children ⇒ sliced validity handle, L171–181), R5 (representation
parity, metadata getters), R6 (map-message null = value-child validity, L320–321),
and R7 (split + deferred two-package fixture in RBA-6a, L356–362) remain intact.

**Non-blocking residual (confirm at implementation, do not hold approval):**

- **N1 — "exactly once per protoc run" vs RBA-5's per-fixture protoc invocation.**
  RBA-5's `build.rs` invokes protoc **once per fixture `.proto`** (RBA-5 §3.2), so a
  plugin that emits `__rba.fletcher.rs` "once per protoc run" will write it **N
  times** (once per fixture) into `OUT_DIR`. This is **safe** only because `__rba`
  has zero per-file/per-message content, so the N writes are byte-identical
  idempotent overwrites and the assembler `include!`s the single resulting file
  exactly once (the design already says "include! ... exactly once"). Confirm at
  implementation that (a) every emitted copy is byte-identical (no message-list or
  file-name interpolation leaks into `__rba`), and (b) the assembler enumerates and
  includes `__rba.fletcher.rs` exactly once even though it appears after multiple
  protoc runs. If the build ever switches to a single multi-proto protoc
  invocation, the "once per run" wording becomes literally true and N1 is moot.
  Net behaviour is correct as written; this is a precision/seam check, not a defect.

---

## Step-2 review — round 1 (2026-06-24)

**Verdict: NEEDS-REWORK** — 3 blocking items (R1–R3); R4–R7 are required
clarifications folded in. No locked-decision deviation found (no STOP-AND-ASK):
the helper-placement issue in R1 is a *consistency-with-as-built* defect, not a
proposed deviation from D-RBA-10's locked text (which fixes the package-path
scheme + the no-self-`mod` + build.rs-assembler model, all honoured here).

Checked against oracle §3/§4/§5/§6/§7/§8/§8.1, D-RBA-1/4/5/6/7/8/10, RBA-4
(C++ composite parity), RBA-3 (C++ metadata), and the **as-built RBA-5** emitter
model (RBA-5 design Implementation-note #2 + RBA-5 code review + progress log).

### Blocking

**R1 — Helper module + import model contradicts the RBA-5 as-built constraint
(load-bearing; the design's own self-described "critical decision").** The design
says the assembler emits `pub(crate) mod __rba { … }` once and that generated
files "import helpers with fully qualified **or local `use` paths**" (original
text). But RBA-5 *as built* (RBA-5 Impl-note #2, confirmed in `RBA-5-codereview.md`
and the progress log) proved that the `build.rs` assembler co-mounts **every
same-package file into ONE module via per-file `include!`**, so the emitter had to
drop **all file-level `use`** and the free `downcast_array` fn — emitting
fully-qualified paths + a per-`impl` private associated `downcast_array` — because
two same-package files each emitting the same `use`/free item collide (E0252
duplicate import / E0428 duplicate definition). The RBA-6 helper proposal
reintroduces exactly this hazard: if a generated accessor emits
`use crate::fletcher_gen::__rba::{ScalarSpan, …};`, two same-package files mounted
into one `mod` produce duplicate `use` imports → won't compile. **Required:** the
design must commit to **fully-qualified path references only** for every `__rba`
helper in generated getters/storage (no per-file `use`), explicitly mirroring the
RBA-5 self-contained-item rule, and the two-file/same-package fixture must compile
with at least one *composite* getter present in **both** same-package files so the
collision is actually exercised, not just asserted away. (Inline wording at the
"Span and Row Helpers" section updated to fully-qualified-only; the fixture
strengthening is still required.)

**R2 — `pub` helper types inside a `pub(crate)` module leak through public getters
(private-in-public).** Getters are `pub fn track(&self, row) -> StructSpan<'_, …>`,
`pub fn readings(&self, row) -> ScalarSpan<&Float64Array>`, etc., and the helper
types are declared `pub trait RowAccess` / `pub struct ScalarSpan` **inside**
`pub(crate) mod __rba`. A `pub fn` whose return type is reachable only at
`pub(crate)` visibility triggers the `private_interfaces` lint (deny-by-default in
some configs; a hard error if the crate ever `#![deny(private_interfaces)]`, and a
real usability defect — downstream callers of the crate cannot name the span types
they receive). **Required:** either (a) make `__rba` a `pub mod` (preferred — the
helpers are part of the public accessor surface, exactly like the C++
`namespace fletcher` span header is public), or (b) re-export the helper types at a
`pub` path the getters' visibility can see. State the chosen visibility explicitly
and confirm it compiles with the getters' `pub` signatures. (Note: D-RBA-10 locks
the `fletcher_gen` **mount point name**, not the `__rba` submodule's visibility, so
this is free to fix.)

**R3 — "The assembler emits `__rba`" needs a concrete source-of-truth + version
pin; helper definitions cannot float between the plugin and `build.rs`.** RBA-5's
`build.rs` is a *test-crate* artifact; the C++ plugin emits the per-file
`.fletcher.rs`. The design hand-waves "the assembler emits one shared helper
module" without saying **who owns the `__rba` text** and how it stays in lock-step
with the per-`impl` arrow API spellings the plugin emits (both must agree on the
pinned arrow `=59.0.0` surface — `ArrayAccessor`, `value_offsets`, `MapArray`,
`downcast_array`). If `build.rs` hard-codes `__rba` while the plugin emits the
getters that instantiate it, an arrow-version bump or an API spelling change
silently desyncs them. **Required:** pin (a) which artifact emits the `__rba`
source text (recommend: the **plugin** emits it as a dedicated generated file,
e.g. `__rba.fletcher.rs`, that the assembler `include!`s once under
`fletcher_gen::__rba` — so plugin and getters share one arrow-API source), and
(b) state that `__rba` is emitted **exactly once** regardless of message/file count
and never per-package (it sits directly under `fletcher_gen`, above the package
tree, so it is reachable by every package module). The current sketch (lines
70–78) shows `__rba` as a sibling of `package` under `fletcher_gen` — keep that,
but make the emission-owner explicit.

### Required clarifications (folded in / to confirm at implementation)

**R4 — Struct-source `is_null` / validity windowing must be coordinate-consistent
with the sliced children (state it as the explicit one-liner RBA-4 has).** The
design slices children to `[offset,len)` AND retains a struct-validity handle, and
says "keep the same row coordinate origin as the sliced child columns." Make the
guarantee precise: because the children are **sliced** (rebased to a 0-based
window), the retained `struct_validity` **must also be the sliced StructArray**
(`s.slice(offset, len)` / windowed), so `struct_validity.is_null(row)` and the
cached child reads share one 0-based origin. This is the *opposite* of the C++
RBA-4 rule (C++ keeps the **whole** StructArray because `field(i)` is already
offset-windowed and `StructArray::IsNull(row)` is offset-aware — RBA-4 review
finding 1). The Rust side slices both, so storing an **unsliced** validity handle
against sliced children would be an off-by-offset null bug. Add this sentence to
the `from_struct` bullet ("retain a sliced/windowed struct validity handle") and
to the `is_null` contract.

**R5 — Metadata representation parity vs C++ (RBA-3 carry-forward).** The design's
`schema_metadata()`/`field_metadata(i)` return `&HashMap` (genuinely empty when
absent), whereas C++ returns `*const KeyValueMetadata` (`nullptr` when absent).
This is **representation parity, not literal parity** — the RBA-7 capstone must
treat C++-`nullptr` ≡ Rust-empty-map (RBA-3 review finding 6 + progress-log
carry-forward). The design is correct (§5 fixes Rust as `&HashMap`); just record
that the parity assertion is representation-level so RBA-7 does not read a false
mismatch. Also: `field_metadata(i)`'s out-of-bounds path returns
`Self::empty_metadata()` (good — total, never panics, mirrors C++ `nullptr`-on-OOB);
keep the `OnceLock` empty-map and confirm it never indexes `self.fields[i]` in a
panicking way (the design already says `self.fields.get(i)…` — correct).

**R6 — Map-message null source must be the value child's validity, not the entry's
(B2 precision).** `StructMapSpan::value(j)` returns `None` "when the value struct
is null." Confirm the null probe is `vals.is_null(base + j)` over the **flattened
map *value* (item.value) child** — i.e. the message-typed `StructArray` built from
the map's value child — exactly as RBA-4 does (`waypoints_inner_->is_null`). The
forcing-test-mapping line (`vals.is_null(base + j)`) is correct; keep the cache
wiring so `vals` is the value-child accessor's retained struct validity, not the
map entries struct. (Arrow map entries are non-null by spec; the nullability that
matters is the value child's.)

**R7 — Split is sound; one tightening.** RBA-6a (metadata + `__rba` + RowAccess/Row
+ STRUCT/REPEATED_SCALAR/REPEATED_STRUCT + cross-file/two-package) and RBA-6b
(MAP scalar + MAP message + NESTED_LIST + remaining null paths) are each
independently green-able and together reconstitute `composite_and_metadata_read`.
**Endorsed** — RBA-6a proves the borrow model, struct recursion, the `__rba`
placement (R1–R3), metadata, and the package assembler before the harder map /
nested-list offset trees, mirroring the proven RBA-4a/4b shape. Tightening: the
**two-file/two-package import fixture deferred from RBA-5** (progress-log
carry-forward (a)) must land in **RBA-6a** (it is the cross-file half of that
slice), and R1's "composite getter in both same-package files" check belongs in
RBA-6a too (both are gated by the struct path, not the map path).

### Confirmed sound (no change required)

- Parity coverage vs RBA-4: every composite kind (STRUCT incl. cross-file ≥2-level,
  REPEATED_SCALAR, REPEATED_STRUCT, NESTED_LIST, map-scalar, map-message) + all
  five null paths are present and mapped 1:1 to the forcing test; metadata getters
  mirror RBA-3 (generic maps, absent→empty, no domain key — D-RBA-5).
- Null-safety (B2): struct elements / nullable struct fields / null map message
  values / null inner lists → `None`; non-nullable struct runtime null → `Err`;
  every null path in the forcing test is covered; no read-through-null.
- `from_struct` slices children to `[offset,len)`; scalar leaves keep the RBA-5
  exact `DataType` gate + offset-preserving `downcast_array`; `null_count==0` gate
  recursed through non-nullable composites (D-RBA-4). MapArray gets its own
  `map_bounds` over `value_offsets()` (not treated as a ListArray) — correct.
- D-RBA-10 cross-file: package-path resolution `crate::fletcher_gen::<pkg-path>::
  <Class>Accessor`, sanitization (keyword→`r#`, invalid→generation error), bare
  items, assembler declares each package `mod` once — all honoured.
- Pure add-on (D-RBA-1): composite handling replaces only the RBA-5 composite
  fail-fast; existing emitters untouched; never panics.
