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
GIR ──▶ BIND-C# ──▶ BIND-Rust ──▶ RIR
```

**Hard dependencies:**
- **GIR** — the recursive language-neutral IR + the **C++** backend logical→type
  table exist.
- **BIND-Rust** — the **Rust** backend logical→type table exists. RIR migrates the
  RBA **Rust** accessor by reusing that table (not by building its own — that
  would make RBA, not BIND, the first Rust IR backend, reversing the GIR/BIND
  decision). Hence RIR is gated **after BIND-Rust**, which (with the C#-first BIND
  split) is the *last* binding round.
- Independent of **BIND-C#** (RBA has no C# accessor).

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
- A build/grep assertion that `FieldKind` is gone and the RBA emitters reference
  only the IR.

## Out of scope

- Dictionary-encoded columns in the accessor (owned by the DICT round / DICT-6).
- Any wire-format change.
- Adding a third accessor language (C# has no RBA accessor).
