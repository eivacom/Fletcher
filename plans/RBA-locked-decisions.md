# RBA — Locked Decisions (digest)

Round **RBA** (RecordBatch accessor generator). This is the small digest the
architect / architect-reviewer / compliance steps must honour. Full rationale +
narrative: [docs/recordbatch-accessor-spec.md](../docs/recordbatch-accessor-spec.md)
(the **oracle** — wins on any contradiction). A proposed deviation from any of
these is **STOP-AND-ASK**.

- **D-RBA-1 — Pure add-on; zero behavioural change (oracle §1, §9).** The feature
  adds new, opt-gated generation only. **No existing generated output's bytes may
  change**, and **no existing emitter function's behaviour may change**, when the
  new flags are absent — and the existing outputs stay byte-identical even when
  the new flags *are* passed. Existing `test_package` / integration / CI
  invocations are left untouched. Shared helpers (`GatherFields`, `FieldInfo`,
  `type_mapper`) are reused **read-only**; new code is added in new functions, not
  by editing existing emitters. A golden no-drift test enforces this. Any diff to
  an existing output or any behaviour change to an existing emitter →
  STOP-AND-ASK.

- **D-RBA-2 — Opt-gated, default-off (oracle §2).** `--fletcher_opt=accessor`
  emits the C++ accessor header `<stem>.fletcher.accessor.pb.h`;
  `--fletcher_opt=rust` emits the Rust accessor `<stem>.fletcher.rs`. With neither
  flag, the plugin emits exactly what it emits today. New output **files only** —
  never new content interleaved into an existing output file. The two tokens are
  **orthogonal** to `schema_only` / `ts` / `ipc`: each emits its file when present
  regardless of the others (so `schema_only,accessor` still emits the accessor).

- **D-RBA-3 — Column-oriented, cast-once (oracle §3).** The accessor down-casts
  each RecordBatch column to its concrete Arrow array type **once**, at
  construction, and caches it; per-row getters index the cached typed array
  directly. There is **no per-cell scalar materialisation** in the access path
  (`GetScalar(row)` per access is the existing per-row `<Class>View`, which is
  *not* re-implemented here). A per-access `GetScalar` / re-cast in a getter →
  STOP-AND-ASK.

- **D-RBA-4 — Positional type-check validation → `Result` (oracle §4).** The
  factory validates (a) the column **count** and (b) per position, that each
  column's Arrow **type** is the one the generated cast targets — the sole
  invariant that makes the cached `static_cast` / `downcast` sound. A **leaf/scalar**
  column is gated by a direct `DataType::Equals` (metadata ignored) — a leaf type
  has no child fields, so this compares only the type. A **composite** column
  (struct/map/list) is gated by checking its *shape* + **recursing** into child
  **types** (each child array cached once); it is **never** gated with a blanket
  `DataType::Equals`, which would also compare child names/nullability. It does
  **not** gate on field **name** or on the **nullable** flag. It returns
  `arrow::Result<Accessor>` (C++) / `Result<Accessor, ArrowError>` (Rust) on
  mismatch — it **never throws / never panics**, and never down-casts a column
  before its type is checked. Names and metadata are *exposed* but never *gating*.
  Name-gating, nullability-gating, or a throwing/panicking constructor →
  STOP-AND-ASK.

- **D-RBA-5 — Generic, domain-agnostic metadata (oracle §5).** Schema-level and
  per-field metadata are read from the **live RecordBatch schema** at runtime and
  exposed as opaque key→value maps. Fletcher hardcodes **no** domain-specific key
  (no `navisuite:*`, no units vocabulary, nothing). Hardcoding any domain key, or
  teaching the *schema generator* to bake domain metadata, → STOP-AND-ASK.

- **D-RBA-6 — Full type parity with `<Class>View` (oracle §6).** Coverage equals
  the existing view: scalars, nested structs, repeated scalars, repeated structs,
  maps, and nested lists. No silent type gap; an unsupported construct is a
  generation-time comment exactly as the existing path already does.

- **D-RBA-7 — Read-only, lifetime-safe, self-composing (oracle §3, §7).** The
  accessor is read-only and keeps data alive by **owning its cached column handles**
  (C++ `std::shared_ptr<T>`, Rust `Arc<T>`) — each owns its buffers, so the accessor
  needs **no** reference to the source and is identical whichever factory built it.
  The **same** `<Class>Accessor` is constructible from a `RecordBatch` **or** an
  `arrow::StructArray` (a top-level factory + a struct factory sharing one
  column-validating helper). **Both factories are generated for every message,
  unconditionally** — never gated on whether a message is used as a nested field;
  a message may be read as a top-level batch in one place and as a struct column in
  another, so the generator emits both for every accessor. **Struct-source
  construction slices each child to the struct's `[offset, offset+len)` window
  explicitly** (C++ `field(i)->Slice`, Rust `column(i).slice`) — it never assumes
  children are pre-rebased — and Rust caches typed handles with the offset-preserving
  `downcast_array` idiom, not a re-`Arc`'d `downcast_ref` clone. A struct **value**
  (1:1 field, list element, or map value) is read through the inner accessor's
  **row-bound `RowView`** which **borrows** the accessor (must not outlive it).
  **No struct value is ever read through a null:** a 1:1 nullable struct field
  returns `std::optional<RowView>` / `Option<Row>` (None on a null row), and a
  struct **element** of a list/map/nested-list is likewise returned as an *optional*
  (None when the Arrow `item`/`value` child is null) — so the dangerous read-through
  is impossible everywhere, not just for 1:1 fields. The inner accessor exposes
  `is_null(row)` (false when built from a `RecordBatch`; from the retained struct
  null bitmap when built from a `StructArray`). Composes **recursively to any
  depth**. No setter / builder / mutation API. Emitting only one factory, reading a
  struct value (1:1, element, OR map value) through a null, or skipping the
  struct-child slice → STOP-AND-ASK.

- **D-RBA-8 — C++ and Rust parity from one model (oracle §8).** Both languages are
  generated from the shared `FieldInfo` / `type_mapper` model. Rust targets the
  official `arrow` crate (arrow-rs). The two accessors expose equivalent APIs and
  read the **same** RecordBatch identically (capstone-proven, RBA-7).

- **D-RBA-9 — RecordBatch + StructArray are the supported inputs; Table is not
  (oracle §10 scope).** Construction from a `RecordBatch` and from an
  `arrow::StructArray` are both first-class, always-generated capabilities (D-RBA-7)
  — the struct factory is what nesting uses internally and is also a public entry
  point. Out of scope: `arrow::Table` / `ChunkedArray` input, a mutable/writer
  accessor, dictionary-encoded columns, and any third language target (the existing
  `<Class>View` keeps its Table ctor; the DICT round owns dictionaries). Adding any
  of these → STOP-AND-ASK.

- **D-RBA-10 — Cross-file nested accessors are designed, both languages (oracle
  §8.1).** A nested field whose message comes from an imported `.proto` resolves to
  that file's generated accessor. **C++** reuses the existing cross-file include
  mechanism (`CollectCrossFileIncludes`), mapping `.fletcher.pb.h` →
  `.fletcher.accessor.pb.h`. **Rust** keys its module convention on the proto
  **package** (prost/tonic-style), **not** the file stem (stems can contain
  `-`/`.`/path separators and collide across directories): a message's accessor is
  `crate::fletcher_gen::<pkg-path>::<Class>Accessor` (package `a.b.c` →
  `a::b::c`; no-package → directly under `fletcher_gen`), mirroring the C++
  `fletcher_gen::<pkg>` 1:1. Same-package files share a module; differing packages
  never collide; non-ident package segments are a generation error, not a silent
  rename. The Rust Cargo test crate exercises a genuine two-file, two-package
  import. Changing the mount-point name (`fletcher_gen`) or the package-path scheme
  after RBA-5 lands → STOP-AND-ASK.

**Still-in-force prior locks.** The robustness-hardening invariants (codec /
positional-I/O decode safety) and the DICT-round invariants are unaffected — RBA
touches neither the wire format nor the runtime re-fold. RBA is generator-side and
adds no new wire/runtime surface.
