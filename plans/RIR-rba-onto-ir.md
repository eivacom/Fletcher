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
heavily-tested, non-wire surface during the wire-critical rewrite. That leaves
one tail: migrate RBA fully onto the IR. This round is that tail.

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
- Lift the depth-2/3 nesting cap where the IR now makes arbitrary depth free.
- **Retire `FieldKind`** — its last consumer (the RBA projection) is gone.
- Preserve every RBA contract: type-only positional gate, cast-once/offset-
  preserving cache, `null_count()==0` for proto-non-nullable, never-panic
  `Status`/`Result`, C++/Rust parity, and the no-drift golden (re-baselined but
  still additive-gating).

## Forcing-test shape (sketch)

- The existing RBA suites (`test_accessor_*`, capstone, Rust crate) stay green,
  byte-identical accessor **output** re-baselined under review.
- A test proving arbitrary nesting depth beyond the old 2/3 cap now reads.
- A build/grep assertion that `FieldKind` is gone and the RBA emitters reference
  only the IR.

## Out of scope

- Dictionary-encoded columns in the accessor (owned by the DICT round / DICT-6).
- Any wire-format change.
- Adding a third accessor language (C# has no RBA accessor).
