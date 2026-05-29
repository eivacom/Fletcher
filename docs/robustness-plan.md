# Robustness Improvements — Execution Plan

Tracking doc for the `feature/robustness_improvements` work-stream. Goal: bring
the generated-code layer and the full encode→decode chain up to the
test-coverage and correctness bar of a mature library (the standard we hold
Apache Arrow to).

## Branch strategy

- This branch is temporarily **based on the PR #79 branch**
  (`feature/pubsub-arrow-batched-subscribe`, the batched-subscribe + dictionary
  work), so Phase 1's codec hardening stacks cleanly on top of #79's
  `scalar_codec.cpp` / `codec.cpp` changes instead of conflicting with them.
- **No PR is opened until PR #79 has merged.** Once #79 lands on `main`
  (which already contains the flatten feature, PR #73), this branch is rebased
  onto the updated `main` and PRs are cut from there.
- Suggested PR split: **PR A** = Phase 1; **PR B** = Phase 3 safety net
  (generator harness + byte-identity); **PR C** = Phase 2 rewrite + remaining
  Phase 3. Keeps each reviewable; lands the low-risk, high-value work first.

## Three phases

1. **Phase 1 (first):** fix runtime correctness issues in the codec / positional
   I/O — everything *except* the proto→Arrow generator (that layer is rewritten
   in Phase 2, so fixing its bugs now would be throwaway work).
2. **Phase 2:** refactor the generated-code layer (the protoc generator) onto a
   recursive type IR + recursive-visitor emitters.
3. **Phase 3 (parallel with 2):** rigorously test the generated-code layer and
   the entire encode→decode chain.

---

## Phase 1 — Codec / positional-I/O correctness hardening

Scope is the runtime wire path: `arrow-bridge/src/codec.cpp`,
`arrow-bridge/src/scalar_codec.cpp`, `arrow-bridge/src/row_reader.hpp`, and
`core/include/fletcher/core/positional_io.hpp`. This explicitly **excludes** the
generator / `type_mapper` (rewritten in Phase 2).

Every fix lands as a small, independently reviewable commit **with at least one
negative / malformed-input test** — the suite today is 100% happy-path.

Order matters: land the reader-bounds hardening (#2) **before** the
full-consumption check (#1).

| # | Fix | Severity | Notes |
|---|-----|----------|-------|
| 2 | **Reader bounds + oversized-length reject.** In both `row_reader.hpp` (`Read<T>`, `ReadBytes`) and the codec, change `pos + n > size` to the overflow-safe `n > size - pos`; reject an oversized var-length `LEN` / list-map `COUNT` against the remaining buffer *before* allocating or looping; guard the `uint32_t`→`int` narrowing into `BitfieldBytes`. | Critical | Faces untrusted wire bytes. Do first. |
| 1 | **Full-buffer-consumption check.** In the **top-level** `Codec::DecodeRow(data, len)` only, throw if `reader.pos != len` after the field loop. **Must not** be added to nested decode — struct/list/map/union share the same `Reader` cursor. | Critical | Rejects truncated / trailing-garbage buffers. |
| 7 | **Harden `core/positional_io.hpp`** (`PositionalReader`): same bounds-overflow class, and a top-level consumption check. This reader backs the **generated edge-tier** decode path (not the codec's reader); it is runtime infra, not the generator, so it belongs in Phase 1. Note: `PositionalReader` *does* spawn sub-readers for nested decode, so the consumption check must be top-level-only by that mechanism. | Critical | Edge-tier (MCU/device) decode path. |
| 3 | **`FixedSizeBinary` encode** — investigated, found non-reachable. Arrow's `FixedSizeBinaryScalar` constructor CHECK-validates `value->size() == byte_width` (aborts on mismatch, even in Release), so an undersized scalar cannot be constructed and the over-read is unreachable via the Arrow API. No guard added; a comment documents the invariant. | — | Resolved as non-issue. |
| 4 | **Map decode type fidelity.** Pass the original `type` argument straight through when building the decoded `MapScalar` (as struct/list/union decode already do) instead of rebuilding `arrow::map(...)` with default child names/nullability. | Low | Only affects hand-built non-default map schemas; the generator never emits them. |
| 6 | **Sparse-union fidelity.** Add the first sparse-union round-trip test + a test asserting decoded inactive children are null; document that only the active variant survives the wire round-trip. | Low | No sparse-union test exists today. |
| — | **Length-narrowing consistency** (the old #5) is folded into #2; document the 4 GiB / 4 G-element limit in `codec.hpp`. | — | |

Sibling note: `positional_io.hpp` and the codec's `Reader` are two
implementations of the same wire format and must stay byte-compatible; keep
their hardening in lockstep.

---

## Phase 2 — Refactor the generated-code layer

Replace the flat `FieldKind` enum + hand-unrolled `NESTED_LIST(depth 2/3)`
generator with a **recursive type IR + recursive-visitor emitters**.

- **2a. Recursive Mapped-Type IR** (`Scalar | List<T> | FixedSizeList<T,n> |
  Struct<fields> | Map<K,V> | Dictionary<V> | Unsupported{reason}`), mirroring
  Arrow's `DataType` tree and the TS `SchemaDescriptor`. `type_mapper` builds it.
  Flatten resolution becomes IR construction (a chained-flatten wrapper just
  yields nested `List(List(Struct))` — the depth special-case disappears and
  arbitrary depth falls out). Unsupported cases become an explicit
  `Unsupported{reason}` node — this fixes the `MapFlattenedRepeated`
  `nullopt`-ambiguity bug (currently live on `main`) by construction.
- **2b. Emitters as recursive visitors** over the IR, each threading a context
  (target buffer var, current value expression, indentation, fresh loop-var
  names): edge C++ class, Arrow view + `ToArrowRow`, nanoarrow schema
  (IR-driven, dropping the brittle `arrow_type_expr` string matching), TS
  interface + descriptor.
- **2c. Decision:** keep emitting bespoke recursive C++ (preserves the
  zero-Arrow-dependency, typed edge classes — recommended) vs. shift the edge
  tier to a generic schema-driven runtime codec (smaller generated code, slight
  runtime indirection, like the TS client already does). Recommend bespoke
  recursive for the rewrite; note runtime-codec as a possible follow-up.
- **Hard invariant:** the wire format stays **byte-identical** (the runtime
  `Codec` and released components are the contract). The rewrite is "same bytes,
  cleaner generator", guarded by Phase 3's byte-identity suite.
- **De-risking:** build the IR + the encode emitter as a vertical slice first,
  behind the existing tests; migrate emitter-by-emitter with the old path kept
  until the new one passes; delete the old switch only at parity.

---

## Phase 3 — Comprehensive test coverage (parallel with Phase 2)

The harness + byte-identity subset must land *before/at the start* of Phase 2 so
the rewrite is test-guarded.

- **3a. Generator compile-and-run harness:** a `coverage.proto` exercising every
  type / WKT / enum / nesting depth / flatten variant / service; run the plugin,
  **actually compile** the generated C++, `tsc --noEmit` the TS, then
  build a row → `Encode()` → reconstruct → assert every field. (Today the
  generated code is only `cat`-ed; nothing is compiled or executed.)
- **3b. Byte-identity + both directions** across all protos
  (`Encode() == EncodeRow()`), incl. nulls set/unset and empty vs non-empty
  containers; test both decode directions; **read back map keys/values and
  struct inner fields** (currently never verified).
- **3c. Codec edge/safety/boundary suite:** all untested type families
  (sparse/dense union, intervals, time32/64, duration, decimals incl. negative,
  large/view, fixed-size-binary); boundaries (`INT*_MIN`, `UINT*_MAX`,
  NaN/±Inf/-0.0 bit-compared, empty string, embedded NULs, multi-byte UTF-8);
  empty containers; wide null-bitfields (≥9 fields/elements, high-index nulls);
  all error/throw paths; encode→decode→encode determinism.
- **3d. Flatten + arbitrary-nesting** unit tests at the IR level, and
  schema-evolution **negative** tests (v1 buffer decoded as v2 is detected, not
  silently corrupted).
- **3e. Property/fuzz testing:** a round-trip property test (random Arrow rows →
  encode → decode → `Equals`) and a `DecodeRow` fuzz harness over random /
  truncated buffers (directly exercises the Phase 1 safety fixes). This is the
  move that closes the gap with Arrow's own randomized testing.
