# RIR — RBA accessor onto the IR — Placeholder / Round Stub

Dedicated follow-up round that reconciles the RecordBatch-accessor (RBA) emitter
with the recursive language-neutral IR built in GIR. **Stub only** — flesh out
into a full round (tracker + locked decisions + config) at kickoff, the way
GIR/HARD were. Recorded now so the reconciliation deferred by
[GIR-locked-decisions.md](GIR-locked-decisions.md) #3 has a concrete home and
does not float.

## Why this exists

GIR deliberately leaves the RBA accessor emitter read-only (consuming a thin
`FieldKind` projection of the IR) to avoid destabilising a freshly-merged,
heavily-tested, non-wire surface during the wire-critical rewrite. After GIR,
encode + decode + schema/IPC + view + TS are all direct IR emitters, and the
`FieldMapping`/`FieldKind` bridge serves **only two** remaining consumers: the RBA
C++/Rust accessor emitter **and** the edge row-class setters/getters. This round
is the tail that migrates both off the bridge and retires `FieldKind`.

## Position in the roadmap

```
GIR ──▶ DICT ──▶ BIND-C# ──▶ BIND-Rust ──▶ RIR
```

*(DICT inserted 2026-08-28. It runs before the binding rounds so each language
backend implements dictionary support once, natively, rather than being built
dictionary-blind and retrofitted per language.)*

**Hard dependencies:**
- **GIR** — the recursive language-neutral IR + the **C++** backend logical→type
  table exist.
- **BIND-Rust** — the **Rust** backend logical→type table exists. RIR migrates the
  RBA **Rust** accessor by reusing that table (not by building its own — that
  would make RBA, not BIND, the first Rust IR backend, reversing the GIR/BIND
  decision). Hence RIR is gated **after BIND-Rust**, which (with the C#-first BIND
  split) is the *last* binding round.
- Independent of **BIND-C#** (RBA has no C# accessor).

## RIR closes ALL deferred interdependencies

**This is RIR's defining property, and it is deliberate:** every deferral the
preceding rounds made lands here, and nothing is left dangling behind it. As of
2026-08-28 that set is:

| Deferred by | What RIR must close |
|---|---|
| GIR locked #3 | Migrate the RBA **C++** accessor emitter off the `FieldKind` projection onto the IR |
| GIR locked #3 | Migrate the RBA **Rust** accessor emitter onto the IR, killing the `arrow::…()` string-parsing (reusing BIND-Rust's Rust type table) |
| GIR locked #3 | Migrate the remaining **edge row-class setters/getters** off the bridge — the *other* `FieldKind` consumer |
| GIR locked #3 | **Retire `FieldKind`** and `ProjectIrToFieldMapping` once both consumers are off it |
| GIR-10 | Lift the RBA **depth-2/3 hand-unroll** cap (arbitrary depth is free on the IR) |
| GIR-10 | Support **scalar-leaf nested lists** (`List<List<scalar>>`) in the RBA C++/Rust emitters, and **remove** the `FindScalarLeafNestedList` guard |
| **DICT locked #11** | Support **dictionary columns** in the RBA C++/Rust accessors, and **remove** the DICT-1.5 guard |

**Rule for both guards: the support and the guard removal land in the SAME
change.** A guard that outlives its subject is the failure mode GIR-10's own
carry-forward warned about ("retire a leg with its subject"). A green suite with
the guard still rejecting the input it now supports is a false pass.

**And the rule going forward:** any new cross-round deferral must land in RIR, or
it is a stop-and-ask. RIR is the tail; it must not grow a tail of its own.

## Scope (sketch)

- Migrate the RBA **C++** accessor emitter (`recordbatch_accessor_emitter.cpp`,
  `EmitAccessor*`) from the `FieldKind` projection to the IR directly.
- Migrate the RBA **Rust** accessor emitter (`EmitRustAccessor`/`EmitRustRbaHelpers`)
  onto the IR, **eliminating the `arrow::…()` string-parsing** (derive arrow-rs
  types from the IR's abstract logical id via BIND-Rust's Rust table).
- Migrate the remaining **edge row-class setters/getters** off the `FieldMapping`
  bridge too — after GIR they are the *other* consumer of the projection, so
  `FieldKind` cannot retire until both they and RBA are on the IR.
- **Lift the RBA nesting caps** — two distinct caps:
  - (a) the depth-2/3 hand-unroll, where the IR now makes arbitrary depth free.
  - (b) **scalar-leaf nested lists** (`List<List<scalar>>`). GIR-10 enabled these
    on every backend **except** RBA and added a front-end guard
    `ValidateBackendsSupportFields` / `FindScalarLeafNestedList` (generator.cpp)
    that fails the plugin with a clear error if `--fletcher_opt=accessor,rust` is
    requested for such a proto. RIR must extend the RBA C++/Rust emitters to
    scalar-leaf nested lists **and remove that guard**. (Until RIR, such protos
    must omit `accessor,rust`.)
- **Support dictionary columns in the accessor** (carried over from the removed
  DICT-6; full requirement set in
  [DICT-dictionary-option.md](DICT-dictionary-option.md) under "DICT-6 — MOVED"):
  positional type gate expects `dictionary(<index>, <value>)`; cast-once,
  offset-preserving cache of the `DictionaryArray` **plus** the downcast values
  array; null-before-index getter that keeps the **value-typed signature
  unchanged** (utf8 → `std::string_view` / `&str`, int32 → `int32_t` / `i32`) so
  the encoding stays invisible at the API, with borrowed returns tied to the
  cached dictionary's values buffer; index-type→key-array mapping driven by the
  option's `index_type`. Then **remove the DICT-1.5 front-end guard**. The
  dictionary flag is already on the IR as `ScalarFacts.dictionary` (GIR locked
  #7), so this is an emitter change, not an IR change.
- **Carry the `nested_leaf_is_scalar` distinction.** GIR-10 routed scalar-leaf
  nesting to the `FieldKind`-consuming emitters by reusing `FieldKind::NESTED_LIST`
  + an additive `nested_leaf_is_scalar` bool (no new enum value, so the read-only
  RBA switch stayed untouched). Whatever replaces `FieldKind` must preserve this
  scalar-vs-struct-leaf distinction.
- **Retire `FieldKind`** — once its two consumers (the RBA projection and the edge
  row-class emitters) are on the IR, both `FieldKind` and the
  `ProjectIrToFieldMapping` bridge delete.
- Preserve every RBA contract: type-only positional gate, cast-once/offset-
  preserving cache, `null_count()==0` for proto-non-nullable, never-panic
  `Status`/`Result`, C++/Rust parity, and the no-drift golden (re-baselined but
  still additive-gating).

## Forcing-test shape (sketch)

- The existing RBA suites (`test_accessor_*`, capstone, Rust crate) stay green,
  byte-identical accessor **output** re-baselined under review.
- A test proving arbitrary nesting depth beyond the old 2/3 cap now reads.
- A test proving a scalar-leaf `List<List<scalar>>` proto now generates working
  RBA C++/Rust accessors under `--fletcher_opt=accessor,rust` — and that the
  GIR-10 front-end guard (`ValidateBackendsSupportFields`) no longer fires.
- A test proving a **dictionary** field now reads through both the C++ and Rust
  accessors **as its value type**, with the getter signature unchanged — and that
  the DICT-1.5 guard no longer fires for such a proto under
  `--fletcher_opt=accessor,rust`.
- A build/grep assertion that `FieldKind` is gone and the RBA emitters reference
  only the IR.
- A negative assertion that **neither** front-end guard
  (`FindScalarLeafNestedList`, the DICT-1.5 dictionary predicate) remains in
  `generator.cpp` — proving each was retired with its subject rather than left
  rejecting input the emitters now handle.

## Out of scope

- ~~Dictionary-encoded columns in the accessor (owned by the DICT round /
  DICT-6).~~ **Moved IN-SCOPE 2026-08-28.** DICT-6 was removed from round DICT;
  DICT ships the front-end guard (DICT-1.5) and RIR ships the capability. See
  DICT locked decision #11.
- Adding the dictionary *option surface*, mapper validation, schema emission or
  TS descriptor — those are round DICT (DICT-1..5) and must already be on `main`
  before RIR starts.
- Any wire-format change.
- Adding a third accessor language (C# has no RBA accessor).
