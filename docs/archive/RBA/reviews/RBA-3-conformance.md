# RBA-3 — Architecture-conformance review (Codex, adversarial)

> Record of the `codex-compliance` verdict for RBA-3. The Codex wrapper did not
> persist its own file on the re-review pass; this captures the returned verdict
> verbatim for the audit trail. Diff base: `41ebf04`.

## Final verdict: PASS — ready to merge

### Resolution of the one blocking item
- **Test used a domain-flavored metadata key** (`unit` / `m/s`, a units vocabulary
  string) — flagged under D-RBA-5. **Fixed (test-only):** replaced with
  obviously-arbitrary, domain-neutral strings — schema `producer=x` →
  `schema_key_0=schema_val_0`, field `unit=m/s` → `field_key_0=field_val_0` —
  across all assertions/comments (incl. the struct-source read-back), with a note
  that they are arbitrary opaque strings the generator never references. The
  emitter was **not** touched (it was always generic).

### Emitter conformance (unchanged from the first pass)
- Zero hardcoded/domain metadata keys in the emitter; all three getters
  (`schema_metadata()`, `field_metadata(int)`, `field_metadata(name)`) return the
  live Arrow `KeyValueMetadata` **verbatim** — no key interpretation, filtering,
  generation, or validation. Metadata is never a construction gate.

### Locked decisions
- **D-RBA-1** (pure add-on): PASS — getters only; no change to validation, scalar
  getters, factories, or existing generated files. RBA-1 no-drift green; RBA-2
  scalar green.
- **D-RBA-5** (generic, domain-agnostic): PASS — verbatim pass-through; no domain
  vocabulary in emitter or (now) test.

### Design requirements
- Return type `const arrow::KeyValueMetadata*` (oracle §5). Lifetime contract
  preserved with the `Field::metadata()` co-ownership comment. Null/OOB/unknown →
  `nullptr`, never throw. Schema-level read name-independent. All test paths
  exercised: verbatim read-back, absent metadata, OOB indices, unknown names,
  struct-source nulls, plain-batch nulls.

### Test execution
- Accessor suite 6/6 green; RBA-1 no-drift green; RBA-2 scalar green; new forcing
  test green.

**Recommendation:** OK to merge.
