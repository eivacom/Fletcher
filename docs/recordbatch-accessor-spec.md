# RecordBatch Accessor — Specification (oracle)

Status: **proposed** (round **RBA**). This is the authoritative spec for the
generated RecordBatch-accessor feature. On any contradiction with the plan or a
per-item design, **this document wins**. Locked-decision digest:
[plans/RBA-locked-decisions.md](../plans/RBA-locked-decisions.md). Plan + tracker:
[plans/RBA-recordbatch-accessor.md](../plans/RBA-recordbatch-accessor.md).

---

## §0 — Context & motivation

`fletcher-protoc` already turns a `.proto` message into an Arrow schema plus a set
of generated artefacts (the nanoarrow `<Class>Schema()`, the row class `<Class>`,
the immutable `<Class>View`, TypeScript descriptors, IPC schema files). The
`<Class>View` can wrap a `RecordBatch` **one row at a time** — but it does so by
calling `column(i)->GetScalar(row)` per cell, which allocates a
`std::shared_ptr<arrow::Scalar>` for every value
([protoc/src/generator.cpp](../protoc/src/generator.cpp) `GenerateViewClass`). That
is convenient but expensive, and it is the *only* generated way to read a batch.

Meanwhile, hand-written consumers across the repo repeatedly do the same
boilerplate before they can touch data:

```cpp
auto ids   = std::static_pointer_cast<arrow::Int32Array>(batch->column(0));
auto temps = std::static_pointer_cast<arrow::DoubleArray>(batch->column(1));
auto lbls  = std::static_pointer_cast<arrow::StringArray>(batch->column(2));
// ... only now can you call ids->Value(row), lbls->GetString(row), ...
```

This is error-prone (positional, untyped, no validation — a wrong cast is
undefined behaviour) and it is duplicated everywhere a function accepts a
`RecordBatch`.

**Goal.** Generate, per message, a *pre-cooked, column-oriented accessor* that
takes a whole `RecordBatch` in its constructor, **cheaply verifies it matches the
message's schema once**, does all the casting once up front, and then exposes
typed, zero-copy getters that index straight into the cached concrete column
arrays — eliminating the boilerplate and the per-cell allocation. The accessor is
emitted for **C++ and Rust**, selected by a plugin argument.

## §1 — Scope & the pure-add-on contract

The existing generator is consumed in several contexts and **must not change
behaviour**. This feature is strictly additive (**D-RBA-1**):

- No existing generated output's bytes change, with or without the new flags.
- No existing emitter function's behaviour changes. Shared helpers
  (`GatherFields`, the `FieldInfo` struct, `type_mapper`) are reused **read-only**;
  the accessor is built by **new** emitter functions.
- Existing `test_package`, integration-test, and CI invocations are untouched.
- A **golden no-drift test** asserts the existing outputs are byte-identical
  before/after the feature and with/without the new flags.

## §2 — Plugin options & output files

Two new comma-separated `--fletcher_opt` tokens, parsed in
`ArrowRowGenerator::Generate` alongside the existing `ts` / `ipc` / `schema_only`
(**D-RBA-2**):

| Option | Emits | Output file |
|---|---|---|
| `accessor` | C++ column accessor classes | `<stem>.fletcher.accessor.pb.h` |
| `rust` | Rust column accessor structs | `<stem>.fletcher.rs` |

Absent ⇒ nothing new is written. The new files are **separate** from the existing
`.fletcher.pb.h` / `.fletcher.arrow.pb.h` / `.fletcher.ts` / `.<Message>.ipc`
outputs; the feature never interleaves content into an existing file.

`accessor` and `rust` are **orthogonal** to the existing tokens: each, when
present, emits its file regardless of `schema_only` / `ts` / `ipc` (those continue
to control only the existing outputs). In particular `schema_only,accessor` still
emits the accessor header — the accessor is a read-side artifact independent of
whether the row class is emitted — and `accessor,rust` emits both new files. The
opt parser adds the two tokens with no change to how the existing tokens behave.

The accessor is generated for the **same message set** the existing emitters use
(`OrderedMessages`, skipping recursive and flatten-wrapper messages), one accessor
type per generated message: `<Class>Accessor` (C++) / `<Class>Accessor` (Rust),
where `<Class>` is the existing `ClassName(msg)` (nested `Outer.Inner` →
`Outer_Inner`).

## §3 — The accessor model: column-oriented, cast-once

The accessor is **column-oriented**, in contrast to the row-oriented
`<Class>View` (**D-RBA-3**):

1. At construction the factory validates the columns (§4) and, for each field,
   down-casts the column to its concrete Arrow array type and **caches the typed
   array handle** (`std::shared_ptr<T>` in C++, `Arc<T>` in Rust). Because each
   cached handle owns its buffers, the accessor needs **no** reference back to the
   source `RecordBatch` / `StructArray`.
2. Per-row getters index the cached array directly — `temperature_->Value(row)`
   (C++) / `self.temperature.value(row)` (Rust). No `GetScalar`, no per-access
   re-cast, no allocation.

**One accessor, two sources — self-composing.** A `RecordBatch` and an
`arrow::StructArray` are structurally identical (top-level columns ↔ struct child
columns), so the **same** generated `<Class>Accessor` is constructible from either:
a top-level factory (`Make(std::shared_ptr<arrow::RecordBatch>)` /
`try_new(RecordBatch)`) and a struct factory
(`Make(std::shared_ptr<arrow::StructArray>)` / `from_struct(&StructArray)`), both
delegating to one shared column-validating helper.

**Both factories are generated for every message, unconditionally.** The generator
does **not** analyse whether a message is used as a nested field and emit the
struct factory only then — it always emits both, for every accessor. A given
message is routinely read both ways in different contexts (a top-level batch here,
a struct column there, possibly in another `.proto` the generator never sees), so
predicting which factory is "needed" would be wrong; both are always present.

**Access shape — column getters + row-bound `RowView` for structs.** A
`<Class>Accessor` is the column holder (it caches the validated casts). Scalar and
collection field getters take the row: `acc.scalar(row)`, `acc.list(row)` →
span. A **struct field** getter is **row-bound**: it returns the inner message's
`<Inner>Accessor::RowView` — a `{accessor*, row}` forwarder whose own getters take
no row — so struct values read uniformly everywhere, whether the struct is a 1:1
field, a list element, or a map value:

- non-nullable struct field → `fix.position(row).lat()` (RowView at `row`);
- nullable struct field → `std::optional<…RowView>` / `Option<…Row>`, **None when
  the struct column is null at `row`** (so you can never read through a null);
- nullable list/map/nested-list field → the span getter returns
  `std::optional<Span>` / `Option<Span>`, None on a null row;
- a **struct element** of a list/map/nested-list (the Arrow `item`/`value` child is
  nullable) is returned as an **optional** too — `span[j]` / `map.value(j)` →
  `std::optional<RowView>` / `Option<Row>`, None on a null element. Scalar elements
  follow the Arrow norm (`span[j]` → value, with `span.is_null(j)` available).

This is the key closure of the read-through-null hazard: **every** path that yields
a struct value — 1:1 field, list element, map value, nested-list leaf — is
optional and `None` on a null, so child fields are never read through a null parent.
It composes **recursively to any depth** — `fix.position(row).lat()`,
`outer.mid(row).inner().v()`, `if (auto e = sample.track(row)[j]) e->x()` — every
struct value being one `<Inner>Accessor` (built once over the flattened struct
column) read at the right index via its `RowView`. There is no second struct type.
The inner accessor is built/cached once per struct field (validating its children
and retaining the struct's null bitmap for `is_null`); the `RowView` **borrows** the
accessor (a two-word value created per access; it must not outlive the accessor).

This makes bulk row access cheap and removes the caller's casting boilerplate. The
existing per-row `<Class>View` is **not** replaced or re-implemented; it remains
for the row-at-a-time / nested-scalar use cases it already serves.

## §4 — Schema validation (the cast-safety invariant)

Because the cached pointers are obtained by an unchecked down-cast, the **only**
invariant the constructor must guarantee for soundness is that each column's
runtime Arrow type is the type the generated cast targets. Validation is therefore
**positional and type-only** (**D-RBA-4**):

- Check `batch->num_columns()` equals the message's generated field count.
- For each field index `i`, check `batch->column(i)->type()` matches the expected
  Arrow type at the granularity that guarantees the concrete array class, then —
  and only then — cache the down-cast pointer. **For a leaf/scalar column** this is
  a direct type comparison (C++ `DataType::Equals`, **metadata ignored**; Rust
  `DataType` equality) — a leaf type has no child fields, so `Equals` compares only
  the type and is exactly right. **For a composite column** (struct/list/map),
  *never* `Equals` the whole composite type (that would also compare child field
  names + nullability — see the recursion paragraph below); instead check the
  composite's *shape* + recurse into child **types**.
- **Do not** gate on the field **name** (a runtime batch may legitimately carry
  different field names — this is the one thing producers are least likely to keep
  identical) or on the **nullable** flag (a column being non-null where the schema
  marks it nullable, or vice-versa, does not change the array class).

Why type-only positional and not `Schema::Equals`? `Schema::Equals` additionally
compares names and nullability and (optionally) metadata, so it would **reject
batches the casts handle correctly** — most importantly when field names differ.
The type-only gate is *sufficient* for cast safety and *tolerant* of the
name/nullability/metadata differences that do not affect it.

**Composite columns recurse the same rule.** A struct / map / list column is
validated by checking its shape (it is a struct/map/list with the expected child
count) and each child's **type**, recursively — **not** by a blanket
`DataType::Equals` on the composite type, which would also compare child field
**names** and **nullability** (the very things the gate tolerates). For example, a
nested-message column is a `StructArray` whose children are the sub-message's
columns; the generated code confirms it is a struct of the expected arity, checks
each child's type, and caches each down-cast child array once — then a per-row view
indexes the cached children directly. Deeper nesting and repeated structs recurse
the same way.

**Error reporting.** A mismatch (wrong column count, or column `i`'s type ≠
expected) returns a failed `Result` with a precise message
(`"<Class> column 3 'temperature': expected double, got int64"`); the constructor
**never throws / never panics**. Construction is via a static factory
(`<Class>Accessor::Make` in C++, `<Class>Accessor::try_new` in Rust) returning
`arrow::Result<…>` / `Result<…, ArrowError>`.

Order is assumed to follow the proto-derived schema (the batch is produced from
the same message definition). **Column order is therefore part of the contract:**
the gate is strictly positional, and a reorder-tolerant match by the
`field_number` / `field_id` metadata Fletcher bakes into the schema is **explicitly
deferred — out of scope for RBA** (it is *not* a silent fallback). If a future
round adds it, it must specify precedence (positional first, metadata only when
*every* generated field is present exactly once in the batch's metadata) and reject
duplicate / missing `field_number`s rather than guessing; until then, a reordered
batch is a validation error, not a best-effort match.

## §5 — Metadata access (generic)

The accessor exposes whatever key/value metadata the **live RecordBatch schema**
carries, generically, with no Fletcher knowledge of the keys (**D-RBA-5**):

- **Schema-level**: `schema_metadata()` → the schema's `KeyValueMetadata`
  (C++ `const arrow::KeyValueMetadata*`) / the schema metadata map (Rust
  `&HashMap<String, String>`). **Source-dependent, by construction:** only a
  `RecordBatch`'s `arrow::Schema` carries schema-level metadata; an
  `arrow::StructArray` (the nested-composition source) has none, so an accessor
  built via the struct factory reports **empty** schema metadata. This divergence
  is expected and documented, not a bug — schema-level metadata is a top-level
  concept. (Per-field metadata is available either way, from the schema's fields or
  the struct type's fields respectively.)
- **Per-field**: `field_metadata(i)` **by index is canonical** and always aligns
  with the positional gate. A convenience `field_metadata("name")` overload looks
  the name up in the **live** schema's field names — which, under name-tolerant
  construction (§4), may differ from the generated proto name, so it is best-effort
  and may not resolve; by-index access is the reliable form and is what generated
  code and tests use.

This is how downstream domain metadata (units, roles, coordinate frames, …) becomes
readable on the accessor without Fletcher referencing any domain vocabulary. The
accessor reads metadata at **runtime** from the batch; the feature does **not**
teach the schema *generator* to bake any new domain metadata (that would couple
Fletcher to a specific data model).

## §6 — Type-system coverage (parity with `<Class>View`)

Coverage equals the existing view (**D-RBA-6**). Each `FieldKind` maps to a cached
concrete array and a getter shape. Nullable scalar getters return
`std::optional<…>` / `Option<…>`; non-nullable scalar getters return the value
type.

| `FieldKind` | Cached array (C++ / Rust) | Getter shape |
|---|---|---|
| `SCALAR` | `Int32Array` / `DoubleArray` / `StringArray` / `TimestampArray` … | `field(row)` → value or `optional`; string/binary → `string_view` / `&str` |
| `REPEATED_SCALAR` | `ListArray` (+ typed values child) | `field(row)` → a list span over the row's slice (`optional<Span>` if the field is row-nullable) |
| `STRUCT` | the nested `<Inner>Accessor` (built once over the `StructArray` column) | `field(row)` → `<Inner>Accessor::RowView` bound to `row` (`optional<RowView>` if nullable — None on null row) |
| `REPEATED_STRUCT` | `ListArray` over `StructArray` | `field(row)` → a struct span; `span[j]` → `optional<RowView>` (None on a null element) |
| `MAP` | `MapArray` (keys + items children) | `field(row)` → a key/value span; scalar `value(j)` (+ `value_is_null(j)`); message `value(j)` → `optional<RowView>` |
| `NESTED_LIST` | nested `ListArray` (depth 2–3) | `field(row)` → nested list spans; leaf `span[j]` → `optional<RowView>` |

A `STRUCT` value reuses the **same accessor type** (§3): the parent caches the
nested `<Inner>Accessor` over the struct column once (validating its children), and
every struct *value* — a 1:1 field, a list element, a map value — is that one
accessor's `RowView` at the right index, so it nests recursively and never reads
through a null. The collection getters
(`REPEATED_SCALAR` / `REPEATED_STRUCT` / `MAP` / `NESTED_LIST`) follow the same
template approach the existing `arrow_row_view.hpp` helpers use (`ArrowScalarList`,
`ArrowRowViewList`, `ArrowScalarMap`, …), bound to **column** child arrays rather
than to per-row scalars — so the cast-once / no-allocation property holds for
nested fields too (a `REPEATED_STRUCT` element view reuses the nested-accessor
composition). An unsupported construct is emitted as a generation-time comment,
exactly as the existing path already does (no silent gap).

The headline win (and the simplest, first-landed slice) is the **flat scalar**
case, which is where the hand-written casting boilerplate overwhelmingly lives.

## §7 — Lifetime & ownership

The accessor is **read-only** and keeps its data alive by **owning its cached
column handles**, each of which owns its buffers — so it needs no reference to the
source `RecordBatch` / `StructArray`, and is identical whichever factory built it
(**D-RBA-7**):

- C++: cached members are `std::shared_ptr<T>` (e.g. `std::shared_ptr<arrow::DoubleArray>`),
  obtained via `std::static_pointer_cast` after the type check. The accessor is
  movable/copyable (copies share buffers). A nested-message field is held as the
  nested accessor by value (`std::optional<<Inner>Accessor>`, populated in the
  factory). The field list (for `*_metadata()`) is stored as an `arrow::FieldVector`.
- Rust: cached members are typed array handles obtained with the buffer-sharing,
  offset-preserving idiom — `arrow::array::downcast_array::<T>()` (or
  `AsArray::as_primitive().clone()`), **not** a re-`Arc`-wrapped `downcast_ref`
  clone — so offset/len are retained; a nested-message field is held as the nested
  `<Inner>Accessor` by value; the `Fields` and schema metadata map are stored for
  `*_metadata()`.

**Struct-source offset handling (both languages, explicit).** Constructing from a
`StructArray` does **not** assume children are pre-rebased: each child is sliced to
the struct's `[offset, offset+len)` window before caching — C++
`field(i)->Slice(s->offset(), s->length())`, Rust
`s.column(i).slice(s.offset(), s.len())`. (A `StructArray`'s logical offset is not
necessarily baked into the arrays returned by `columns()`/`field()`, so the slice
is required for correctness, not just convenience.) Per-row getters then index by
`row` directly. No setter / builder / mutation API exists.

## §8 — C++ and Rust parity; arrow-rs specifics

Both languages are generated from the shared `FieldInfo` / `type_mapper` model
(**D-RBA-8**), so the C++ and Rust accessors expose equivalent APIs and read the
same RecordBatch identically. Rust targets the official **`arrow`** crate
(arrow-rs); the generated `.rs` uses `arrow::array` downcasts
(`as_primitive` / `as_string` / `downcast` helpers), `arrow::datatypes::DataType`
for the type gate, and `arrow::error::ArrowError` for the `Result` error. The exact
arrow-rs API spellings and the pinned crate version are fixed at implementation
time (RBA-5); the cross-language **parity** is proven by the capstone (RBA-7),
which reads one batch through both accessors and asserts identical values.

### §8.1 — Cross-file nested messages

A nested field may reference a message defined in an **imported** `.proto`, whose
accessor lives in that file's generated output. Both languages must resolve it:

- **C++** — reuse the existing cross-file mechanism. The generator already
  collects per-field cross-file headers (`FieldInfo`'s `nested_header` /
  `map_value_header`, gathered by `CollectCrossFileIncludes`); the accessor header
  emits `#include` for each, mapping the existing suffix
  `.fletcher.pb.h` → `.fletcher.accessor.pb.h` (exactly as the view header already
  maps to `.fletcher.arrow.pb.h`). Mechanical; no new convention.

- **Rust** — Rust has no `#include`, and a proto **file stem** is not a usable
  module identifier (stems may contain `-`, `.`, or path separators, and two files
  in different directories can share a stem). So the module convention is keyed on
  the **proto package**, exactly as `prost`/`tonic` generate modules — robust and
  conventional:
  - A message's accessor lives at `crate::fletcher_gen::<pkg-path>::<Class>Accessor`,
    where `<pkg-path>` is the proto `package` split on `.` into nested modules
    (`navisuite.sensors` → `navisuite::sensors`). This matches the C++
    `fletcher_gen::<pkg>` namespacing 1:1. A message with **no** package sits
    directly under `crate::fletcher_gen`.
  - The package — not the file — is the namespace, so two imported files that share
    a stem but differ in package never collide; two files in the **same** package
    contribute to the **same** module (their generated `.rs` are mounted together,
    as `prost` does). Identifiers are sanitized to valid Rust (`raw` idents where a
    segment is a keyword); a package segment that is not a valid ident is a
    generation error, not a silent rename.
  - Mounting: the consuming crate `include!`s/`mod`s the generated files under
    `fletcher_gen`; the generator emits the package `mod` nesting and the `use`
    (or fully-qualified path) for each imported message it references. `fletcher_gen`
    is the default mount point (changing it after RBA-5 → STOP-AND-ASK).
  - The Cargo test crate (RBA-5) includes a genuine **two-file, two-package** import
    so the convention is exercised, not just asserted.

## §9 — Testing & the no-drift guarantee

- **No-drift (the add-on guarantee, RBA-1).** A golden test runs the plugin on the
  fixtures with and without `accessor`/`rust` and asserts every existing output
  (`.fletcher.pb.h`, `.fletcher.arrow.pb.h`, `.fletcher.ts`, `.<Message>.ipc`) is
  byte-identical, and that the new flags add only the new files.
- **C++ accessor (RBA-2..4).** In the `protoc-arrow-bridge` integration harness
  (or a dedicated `protoc-accessor` harness), add fixture `.proto`s and gtest TUs
  that build a real `RecordBatch`, construct the accessor, and assert: correct
  values per row; a type-mismatched batch → error `Result`; a **name-mismatched
  but type-compatible** batch → **success** (proving §4's name tolerance); generic
  metadata read-back; full-type-parity reads.
- **Rust accessor (RBA-5..6).** A new Cargo test crate runs the plugin
  (`--fletcher_opt=rust`), includes the generated `.rs`, and `cargo test` builds a
  RecordBatch with arrow-rs and asserts the same behaviours.
- **Capstone (RBA-7).** One comprehensive fixture read through both the C++ and
  Rust accessors over an equivalent batch, asserting identical results, plus docs.

Each item is gated by **one** red-first forcing test; the remaining tests in the
item are green acceptance tests added in the same item.

## §10 — Out of scope

- Any change to an existing generated output or existing emitter behaviour (§1).
- A mutable / writer accessor (read-only only).
- Re-implementing single-row, scalar-materialising access — that is the existing
  `<Class>View`.
- `arrow::Table` / `ChunkedArray` input. Construction from a `RecordBatch` **and**
  from an `arrow::StructArray` are both first-class, always-generated capabilities
  (§3) — the struct factory is what nested composition uses internally, but it is
  also a public entry point callers may use directly. What is **out of scope** is
  `Table` / `ChunkedArray` input; a Table accessor is a possible follow-up, and the
  existing `<Class>View` keeps its Table constructor meanwhile.
- Dictionary-encoded columns (the proto→Arrow mapping produces none; the DICT round
  owns dictionaries).
- **Arrow layouts the proto→Arrow mapping never emits**, explicitly excluded so the
  type gate need not handle them: `large_list` / `large_utf8` / `large_binary`,
  `fixed_size_list` / `fixed_size_binary` (beyond what the mapping already uses),
  union (dense/sparse — oneofs are unsupported upstream), and Arrow extension types.
  The mapping emits exactly: the scalar types in the README table, `list` (32-bit
  offsets), `map`, and `struct`. The gate validates against those; an unexpected
  layout is a validation error, never silently accepted.
- Languages beyond C++ and Rust (TypeScript already has its descriptor path).
- Teaching the schema generator to bake any new (domain) metadata into schemas.
