# GIR — Locked Decisions

Firm choices for the generator IR rewrite (robustness Phase 2/3). The architect,
architecture reviewer, implementer, and compliance reviewer must honor these; a
proposed deviation is a **stop-and-ask**. Full rationale in
[docs/robustness-plan.md](../docs/robustness-plan.md).

1. **The IR is LANGUAGE-NEUTRAL.** Each `Scalar` node carries an **abstract
   logical-type identity** (a `ScalarKind` enum: BOOL/INT*/UINT*/FLOAT*/UTF8/
   BINARY/FIXED_SIZE_BINARY(n)/DATE*/TIMESTAMP{unit,tz}/TIME*{unit}/DURATION{unit}/
   DECIMAL{p,s}/INTERVAL{...}), **never** a C++ type string. The existing
   C++-string tables (`arrow_type_expr`, `scalar_ctor`, `builder_type`) become a
   **C++-backend** lookup keyed on the logical id; Rust/C#/TS backends each own
   their own table. Embedding C++ strings in the IR is the exact anti-pattern the
   RBA Rust emitter proves harmful — it is a stop-and-ask.

2. **The WIRE format stays byte-identical (hard invariant).** No change to the
   encode→decode bytes for any input the flat generator already supported.
   Generated **source** bytes *will* change (goldens, incl. the RBA no-drift
   golden, are re-baselined under review — purpose survives, baseline moves);
   **wire** bytes do not. `Encode()==EncodeRow()` (encode) and a decode
   round-trip value-equality oracle (decode) land **before** the rewrite (GIR-1/2)
   and guard every migrated emitter.

3. **The RBA accessor emitter stays read-only this round.** It is not rewritten
   onto the IR (freshly merged, heavily tested, non-wire); it consumes a **thin
   `FieldKind` projection** of the IR. Consequences: RBA keeps its depth-2/3 cap,
   and DICT-6 still patches the flat accessor. **`FieldKind` is NOT deleted at
   Phase-2 parity** — it survives as a thin IR projection through a long
   transition. Rewriting the RBA emitter is out of scope / stop-and-ask.
   **Its concrete home:** the reconciliation (migrate the RBA C++/Rust accessor
   onto the IR, kill the Rust emitter's `arrow::…()` string-parsing, and retire
   `FieldKind`) is a dedicated follow-up round — **RIR (RBA↔IR)**, see
   [RIR-rba-onto-ir.md](RIR-rba-onto-ir.md) — sequenced **after BIND-Rust**,
   which builds the Rust logical-type table RIR reuses (GIR builds the C++ one).
   Downstream chain: **GIR → BIND-C# → BIND-Rust → RIR**.

4. **Migrate emitter-by-emitter behind per-emitter oracles.** The old path is kept
   until each new emitter passes its oracle (encode → Encode==EncodeRow; decode →
   round-trip; schema+IPC → `test_schema_builder` + `.ipc` golden; TS → `tsc` +
   descriptor golden; view → round-trip via codec). A temporary IR→`FieldMapping`
   adapter is a migration bridge only; RBA/IPC must not consume it permanently.

5. **The schema emitter and the IPC builder are UNIFIED onto one IR
   schema-visitor.** `BuildMessageSchemaInto`/`BuildMessageSchema` and the
   nanoarrow schema emitter are two copies today (hand-kept in lockstep); Phase 2
   renders both from one visitor (C++ source on one path, in-process nanoarrow on
   the other). Migrating one without the other is a stop-and-ask.

6. **Fold generator-behaviour fixes into the rewrite; do not do them on the flat
   model first.** #55 becomes the `Unsupported{reason}` node → build error;
   #53-generated becomes a checked-result helper in the new emitter conventions;
   #75 becomes the `Enum` IR node + typed emission; #59 becomes IR **metadata**
   (implementation deferrable). Any GEN fix that corrupts **wire bytes shipping
   today** lands **with the GIR-2 baseline** (so the oracle captures correct
   bytes), not after.

7. **`Enum` is a first-class IR identity; `Dictionary` is a scalar MODIFIER.**
   The IR preserves the enum descriptor/symbols even though the C++ backend still
   lowers enums to int32 (needed by #75 + BIND). A dictionary field stays
   `SCALAR` with a modifier (per the DICT spec) — it is **not** a structural
   container peer of `List`/`Struct`. Modeling it as a container is a stop-and-ask.

8. **Edge codec strategy (2c) — emit both, decided with BIND-2 in view.** Keep
   bespoke recursive C++ for the **edge** tier (zero-Arrow-dependency, MCU/device).
   The IR schema-visitor may **additionally** emit a descriptor-driven codec for
   the ABI surface BIND-2 wraps. Do not silently foreclose the descriptor-driven
   path; do not remove the bespoke edge path.

9. **Every behavioural item ships a red-first test; refactor items ship a green
   oracle.** Feature/fix items (GIR-8/9/12) fail before the fix for the right
   reason. Migration items (GIR-3..7) are guarded by the byte-identity /
   round-trip oracles (a refactor that changes wire bytes is red). Coverage items
   (GIR-10/11) may surface real bugs (red-first) or stand as regression guards.

10. **Scope = the protoc generator + generator tests + the codec test harness.**
    In: `protoc/` (generator, type_mapper, emitters), the new compile-and-run
    harness, protoc unit tests, codec edge/fuzz tests. Out: runtime codec
    correctness (Phase 1, done #98), pubsub/providers, the RBA accessor rewrite
    (decision #3), BIND emitters (BIND-5/6 gated after this round), and any wire
    format change (decision #2). Adding out-of-scope work is a stop-and-ask.

11. **Base after HARD; do not revive the stale branch.** The round bases on `main`
    after the HARD PRs (#109–#116) merge; the `feature/robustness_improvements`
    branch is not reused (Phase-1 landed via #98). Confirm the base at kickoff.
