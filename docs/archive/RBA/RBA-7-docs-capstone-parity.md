# RBA-7 -- Docs + cross-language capstone parity

Author: Codex --wait

## Summary

RBA-7 closes the RecordBatch accessor round with additive documentation, a
cross-language parity capstone, and the round-close no-drift regression guard.

The capstone is implemented as paired C++ and Rust tests that build equivalent
Arrow `RecordBatch` values from one committed logical fixture and compare their
generated accessor readout against one committed expected-values artifact. The
test proves D-RBA-8 by forcing both languages through the same schema, same row
data, same null cases, and same metadata normalization. The docs work adds the
new options and a short accessor usage section without changing existing output
semantics, and corrects the oracle's C++ `StructArray::field(i)->Slice(...)`
wording in §7. Spec §10 remains untouched.

## Design

### Pillar 1: Capstone Mechanism

Use a shared proto schema plus two JSON artifacts:

- `integration-tests/accessor-capstone/proto/accessor_capstone.proto`
- `integration-tests/accessor-capstone/fixtures/accessor_capstone_fixture.json`
- `integration-tests/accessor-capstone/fixtures/accessor_capstone_expected.json`

`accessor_capstone.proto` is the single schema fixture. It defines package
`rba.capstone` with:

- `message CapstoneBatch`
- `message Child`
- `message Leaf`
- `message InnerList`, used to model the generated nested-list path

`CapstoneBatch` must enumerate every accessor kind explicitly:

- non-nullable scalar, e.g. `int32 id`
- nullable scalar, e.g. `optional string label`
- nullable `STRUCT`, e.g. `optional Child maybe_child`
- `REPEATED_SCALAR`, e.g. `repeated int32 samples`
- `REPEATED_STRUCT`, e.g. `repeated Child children`
- scalar-value `MAP`, e.g. `map<string, int32> scores`
- message-value `MAP`, e.g. `map<string, Child> child_by_key`
- `NESTED_LIST`, represented by the existing generator's nested-list pattern,
  e.g. `repeated InnerList nested_children`, where `InnerList` contains a
  repeated struct leaf

`accessor_capstone_fixture.json` is the authoritative logical data source for
both harnesses. It contains:

- `schema_metadata`: at least two arbitrary string key/value pairs.
- `field_metadata`: an array/object keyed by field index, with at least one
  populated field metadata map and at least one intentionally absent metadata
  entry.
- `rows`: a small deterministic row set, minimum three rows.
- Explicit JSON `null` markers for every optional path the capstone must cover:
  nullable scalar, null 1:1 struct row, null repeated-struct element, null map
  message value, and null inner nested-list level.

The fixture is not Arrow IPC. Each language builds an in-memory Arrow batch from
the same logical JSON through native Arrow builders. That keeps the parity test
focused on generated accessor behavior, avoids cross-language IPC reader
differences, and makes null placement readable in review.

`accessor_capstone_expected.json` is the canonical accessor readout. It contains
only normalized values that both languages can compare without API-shape
translation:

```json
{
  "schema_metadata": {
    "capstone": "rba-7",
    "owner": "accessor"
  },
  "field_metadata": [
    {"role": "id"},
    {},
    {"role": "nullable-scalar"}
  ],
  "rows": [
    {
      "id": 1,
      "label": "alpha",
      "maybe_child": {"score": 11, "name": "a"},
      "samples": [7, null, 9],
      "children": [{"score": 21, "name": "c0"}, null],
      "scores": [{"key": "x", "value": 10}, {"key": "y", "value": null}],
      "child_by_key": [{"key": "left", "value": null}],
      "nested_children": [
        [{"score": 31, "name": "n0"}],
        null,
        [{"score": 32, "name": "n1"}, null]
      ]
    }
  ]
}
```

The exact values can differ from this sketch, but the committed expected file
must preserve the same shape and null cases. Maps are encoded as ordered
`{"key", "value"}` arrays, not JSON objects, so key order and duplicate-key
behavior remain explicit and Arrow-compatible.

C++ consumption:

- Add `integration-tests/protoc-arrow-bridge/tests/test_accessor_capstone.cpp`.
- Add `accessor_capstone.proto` to the protoc generation inputs in
  `integration-tests/protoc-arrow-bridge/CMakeLists.txt`, using
  `--fletcher_opt=accessor` so `<stem>.fletcher.accessor.pb.h` is generated.
- The test reads the **same** fixture JSON and expected JSON that the Rust side
  reads from their committed copies in `integration-tests/accessor-capstone/`.
- Build the Arrow arrays with C++ Arrow builders, attaching schema and field
  metadata from the fixture, construct
  `fletcher_gen::rba::capstone::CapstoneBatchAccessor`, and read every value
  through generated getters.
- The observed readout is normalized to the same in-memory representation as
  the committed `accessor_capstone_expected.json`; the assertion is expected
  JSON vs observed JSON, comparing against the **one shared committed artifact**.

Rust consumption:

- Add `integration-tests/protoc-gen-fletcher-rust/tests/accessor_capstone.rs`.
- Update `integration-tests/protoc-gen-fletcher-rust/build.rs` to include the
  shared proto path and generate `accessor_capstone.fletcher.rs` with
  `--fletcher_opt=rust`. Keep the existing D-RBA-10 package-keyed assembler.
- The test reads the same fixture JSON, builds the Arrow arrays with arrow-rs
  builders, attaches schema and field metadata from the fixture, constructs
  `fletcher_gen::rba::capstone::CapstoneBatchAccessor::try_new`, and reads every
  value through generated getters.
- The observed readout is normalized and compared to
  `accessor_capstone_expected.json`.

Null-path assertions must be explicit in both tests, not only implied by the
snapshot comparison:

- nullable struct row: the chosen row's `maybe_child()` is
  `std::nullopt` / `None`;
- repeated-struct null element: `children(row).get(j)` or `span[j]` is
  `std::nullopt` / `None`;
- map message null value: `child_by_key(row).value(j)` is
  `std::nullopt` / `None`;
- null inner list: `nested_children(row).get(i)` is
  `std::nullopt` / `None`;
- nested-list null leaf struct, if present in the fixture, is also asserted as
  `std::nullopt` / `None`.
- scalar-element null: `samples(row).is_null(j)` / equivalent Rust span API is
  probed for each scalar element; a null element is encoded as `null` in the
  normalized readout, with both C++ and Rust emitting identical JSON.

Metadata equivalence is encoded by normalizing all observed metadata to a sorted
map before comparing to expected JSON:

- C++ helper `MetadataToJson(const arrow::KeyValueMetadata* md)` returns `{}` when
  `md == nullptr`.
- Rust helper converts `&HashMap<String, String>` to a sorted map; the generated
  API already represents absent metadata as an empty map.
- Expected JSON represents absent metadata as `{}`. Therefore C++ `nullptr` and
  Rust empty map are intentionally equal, and the capstone includes at least one
  absent field metadata case to prove that normalization.

### Pillar 2: Docs

All documentation changes are additive except for the oracle correction in §7.

Update `protoc/README.md`:

- Add `--fletcher_opt=accessor` to the Options section following the existing
  `ts` / `ipc` pattern. It documents generation of
  `<stem>.fletcher.accessor.pb.h`.
- Add `--fletcher_opt=rust` to the same list. It documents generation of
  `<stem>.fletcher.rs`.
- Update the comma-separated combination example to include at least one accessor
  combination, e.g. `--fletcher_opt=accessor,rust,ipc`.
- Add a short "RecordBatch accessors" subsection near the C++ consumer section.
  It should state that accessors are read-only, opt-gated, construct from
  `RecordBatch` or `StructArray`, validate by position/type, expose generic
  schema/field metadata, and do not add `Table`, `ChunkedArray`, or writer APIs.
  Do not change the meaning of existing `ts`, `ipc`, or schema-only docs.

Add a compile-checked README example:

- Add `integration-tests/protoc-arrow-bridge/tests/test_accessor_readme_example.cpp`.
- The README accessor subsection includes the same minimal example shape:
  generate with `--fletcher_opt=accessor`, include
  `<stem>.fletcher.accessor.pb.h`, construct an accessor from a
  `std::shared_ptr<arrow::RecordBatch>`, then read a scalar and an optional
  scalar through row getters.
- The C++ test compiles and runs the example against an existing simple generated
  test proto. Keep assertions minimal; its purpose is to prove the documented API
  compiles. If exact doc-snippet extraction is not practical in this repo, the
  test file must carry a comment naming the README section it mirrors.

Add a short accessor doc section:

- Prefer `docs/recordbatch-accessor.md` if a standalone page is desired, with a
  link from `docs/README.md`.
- If keeping docs small, add the section directly to `protoc/README.md` and link
  to `docs/recordbatch-accessor-spec.md` for deeper details.
- The section must stay user-facing and must not restate implementation internals
  beyond construction sources, null behavior, metadata, and option names.

Correct the oracle text in `docs/recordbatch-accessor-spec.md` §7:

- Replace the C++ `field(i)->Slice(s->offset(), s->length())` wording with the
  Arrow C++ behavior: `StructArray::field(i)` is already windowed to the
  struct's logical `[offset, offset + length)` range, so the C++ accessor
  contract is to use `field(i)` directly and not document an extra slice.
- Keep the Rust text explicit that arrow-rs child arrays must be sliced with
  `s.column(i).slice(s.offset(), s.len())`.
- This is a spec/documentation correction only. It does not authorize changing
  §10 scope or adding new input kinds.
- Because `plans/RBA-locked-decisions.md` D-RBA-7 currently repeats the same
  stale C++ phrase, that sentence is updated to mirror the corrected oracle (see
  the PM-confirmed scope block below — this is unconditional, not contingent on a
  separate sync step). The lock's meaning remains: struct-source children are read
  in the source struct's logical window; no new factory, input type, or mutability
  surface is added.

PM-confirmed scope (RBA-7): the documentation pass corrects the wrong
`StructArray::field(i)` windowing guidance in **both** the oracle spec
`docs/recordbatch-accessor-spec.md` (§7) **and** the locked-decision file
`plans/RBA-locked-decisions.md` (decision **D-RBA-7**). The precise correction is:

- Arrow C++ `StructArray::field(i)` already returns each child windowed to
  `[offset, length)` — do **NOT** re-`Slice` it. A re-slice double-applies the
  offset and reads the wrong row (proven in RBA-2/4).
- arrow-rs `StructArray::columns()` is **NOT** pre-windowed, so the Rust
  `from_struct` path **MUST** `.slice()` each child by the struct offset.

This is a **factual-mechanism correction only**. D-RBA-7's *intent* (struct
children must be correctly windowed before recursion) is preserved; only the
wrong "C++ `field(i)->Slice`" wording and its associated "skipping the slice →
STOP-AND-ASK" trigger are removed/replaced. The shipped C++/Rust code already
does the correct thing (test-proven), so **no code changes** are made.

### Pillar 3: Round-Close Regression

Run the existing RBA-1 no-drift test after the docs and capstone changes:

- C++/CMake: run the `AccessorTest.OptGatedEmissionLeavesExistingOutputsByteIdentical`
  test in `integration-tests/protoc-arrow-bridge`.
- Rust: run `cargo test` in `integration-tests/protoc-gen-fletcher-rust` so the
  capstone and existing RBA-5/RBA-6 Rust tests compile together.
- C++ capstone/readme: run the `accessor_cpp_and_rust_agree_on_same_batch`
  equivalent C++ test target, including `test_accessor_capstone.cpp` and
  `test_accessor_readme_example.cpp`.

Confirm §10 was not touched:

- Do not edit the `## §10 -- Out of scope` section in
  `docs/recordbatch-accessor-spec.md`.
- No generated or documented API may add `Table`, `ChunkedArray`, mutable/writer,
  dictionary, or third-language accessor support.
- The only spec edit is the §7 C++ `StructArray::field(i)` correction.

## Forcing-Test Mapping

`accessor_cpp_and_rust_agree_on_same_batch` is realized by the paired tests
`test_accessor_capstone.cpp` and `accessor_capstone.rs`. They pass only if both
languages consume the same proto, fixture JSON, and expected JSON.

Mapping:

- Shared `.proto`: proves both accessors are generated from one model
  (D-RBA-8), not hand-aligned test-only schemas.
- Shared fixture JSON: proves both Arrow batches carry the same logical rows,
  metadata, and null placements.
- Shared expected JSON: proves both accessors expose the same normalized readout.
- Scalar nullable + non-nullable: asserted through `id` and `label`.
- `STRUCT`: asserted through `maybe_child`, including one null row.
- `REPEATED_SCALAR`: asserted through `samples`, including a null scalar element
  if the generated API exposes scalar element nulls for that field.
- `REPEATED_STRUCT`: asserted through `children`, including a null element.
- scalar-value `MAP`: asserted through `scores`, including scalar value null
  behavior via `value_is_null(j)` / equivalent Rust API.
- message-value `MAP`: asserted through `child_by_key`, including a null message
  value.
- `NESTED_LIST`: asserted through `nested_children`, including a null inner list.
- Metadata: schema and per-field metadata are read back and normalized; one absent
  metadata entry proves C++ `nullptr` equals Rust `{}` by design.
- README example: compile/run test proves the documented C++ accessor snippet is
  not stale.
- No-drift: existing RBA-1 test proves new docs/tests do not alter existing
  generated outputs.

The paired tests should use the same test name stem where each framework allows
it:

- C++ gtest: `AccessorCapstoneTest.AccessorCppAndRustAgreeOnSameBatch`
- Rust cargo test: `accessor_cpp_and_rust_agree_on_same_batch`

## Risks/Unknowns

- The exact proto shape that the generator classifies as `NESTED_LIST` must match
  the as-built RBA-4/RBA-6 classifier. If the suggested `repeated InnerList`
  pattern does not hit that kind, choose the existing composite fixture pattern
  already used by RBA-4/RBA-6 and keep the shared capstone paths unchanged.
- Null scalar elements inside `REPEATED_SCALAR` are only asserted if the generated
  scalar span API exposes element nulls for that scalar kind. The forced null
  paths that must be explicit regardless are null struct row, null struct element,
  null map message value, and null inner list.
- Maintaining duplicated README snippet text can drift. A future improvement could
  extract fenced snippets into a compile target, but RBA-7 only requires that the
  example shown in the README is represented by a compile-checked integration
  test.
- JSON parsing dependencies may need small harness additions: C++ can use the
  repo's existing test JSON dependency if present, or add a test-only dependency;
  Rust can use `serde_json` as a dev-dependency in the integration crate.

## Files-to-touch

Capstone fixtures:

- `integration-tests/accessor-capstone/proto/accessor_capstone.proto`
- `integration-tests/accessor-capstone/fixtures/accessor_capstone_fixture.json`
- `integration-tests/accessor-capstone/fixtures/accessor_capstone_expected.json`

C++ harness:

- `integration-tests/protoc-arrow-bridge/CMakeLists.txt`
- `integration-tests/protoc-arrow-bridge/tests/test_accessor_capstone.cpp`
- `integration-tests/protoc-arrow-bridge/tests/test_accessor_readme_example.cpp`

Rust harness:

- `integration-tests/protoc-gen-fletcher-rust/Cargo.toml`
- `integration-tests/protoc-gen-fletcher-rust/build.rs`
- `integration-tests/protoc-gen-fletcher-rust/tests/accessor_capstone.rs`

Docs:

- `protoc/README.md`
- `docs/recordbatch-accessor-spec.md` (§7 correction only; do not touch §10)
- `plans/RBA-locked-decisions.md` (D-RBA-7 C++ slice wording only; PM-confirmed
  in scope for RBA-7 as a factual-mechanism correction, no code changes)
- optional: `docs/recordbatch-accessor.md`
- optional: `docs/README.md` if a standalone accessor doc is added

Planning:

- `plans/RBA-7-docs-capstone-parity.md`
- optional: `plans/RBA-progress-log.md` when implementation begins or completes

## Step-2 review (2026-06-24)

Verdict: **NEEDS-REWORK** (one STOP-AND-ASK + two blocking items). The capstone
mechanism is sound and the §7 C++ correction is *correct* (it matches the
as-built, green RBA-2/RBA-4 code — C++ `StructArray::field(i)` is already windowed
to `[offset, offset+len)`, Rust `columns()` is NOT, so Rust must `.slice()`).
The verification points (transitive C++==Rust proof, every field kind + each
required null, metadata `nullptr ≡ {}` normalization, additive README, no-drift
re-run, §10 untouched, single-doc split) all check out. Required changes:

1. **STOP-AND-ASK — editing locked decision D-RBA-7.** The design proposes to
   rewrite the §7 oracle text *and* the D-RBA-7 digest line, which today literally
   reads "Struct-source construction slices each child … explicitly (C++
   `field(i)->Slice`, Rust `column(i).slice`)" and binds "skipping the struct-child
   slice → STOP-AND-ASK". The as-built C++ correctly does **not** call `Slice`
   (it relies on Arrow already windowing `field(i)`), so the lock's *C++ mechanism
   wording is factually wrong* and contradicts shipped green code. The lock's
   **intent** survives (children are read in the struct's logical window, no
   read-through, no assumed pre-rebasing) — this is a mechanism-wording correction,
   not a semantic deviation — but because the digest preamble makes any edit to a
   locked decision a stop-and-ask, this must be surfaced to the PM for explicit
   sign-off, **not** left as the design's conditional aside ("if the implementation
   step keeps the digest synchronized"). Make the D-RBA-7 edit unconditional and
   flag it to the PM. (The Rust half of the lock's wording — `column(i).slice` —
   stays as-is; it is correct.)

2. **Blocking — the "shared expected artifact" must be consumed by both
   languages, not transcribed.** The transitive proof (C++ readout == expected ∧
   Rust readout == expected ⇒ C++ == Rust) only holds if **both** harnesses compare
   against the *same committed* `accessor_capstone_expected.json`. Risks/Unknowns
   currently hedges that C++ may lack a JSON parser and fall back to a test-only
   dependency. If C++ instead hardcodes the expected values in `.cpp`, C++ is
   comparing to a hand-transcribed copy that can silently drift from the JSON the
   Rust side reads — collapsing D-RBA-8's single-source-of-truth guarantee. Require
   that both languages **parse the one committed expected file**; if C++ JSON
   parsing is genuinely infeasible in this harness, specify the alternative
   single-source mechanism (e.g. one file both consume) explicitly rather than
   leaving "or hardcode" open. Same requirement for the fixture file: both must
   build from the *one* committed `accessor_capstone_fixture.json`.

3. **Blocking — make the REPEATED_SCALAR element-null normalization explicit, or
   drop it from the expected JSON.** The expected-JSON sketch commits to
   `"samples": [7, null, 9]` (a null *scalar* element), but the readout
   normalization is described only as "reads every value through generated getters"
   and the forcing-map hedges this null as conditional. The as-built spans expose
   element nulls (`ScalarSpan::is_null(i)` both languages), so it is achievable —
   but the design must state that the normalizer probes `is_null(j)` per scalar
   element and emits `null` for a null element (so the C++ and Rust readouts encode
   the null identically). Otherwise drop the scalar element-null from the expected
   file to keep it consistent with the user story's required null set (which does
   not mandate it).

Non-blocking notes (address inline at implementation, no re-review needed):
- The README-example compile check mirrors (does not extract) the snippet; the
  acceptance "README example compiles" is met by a mirror carrying a comment naming
  the section. Acceptable; the snippet-vs-test drift risk is already in Risks.
- The capstone adds `accessor_capstone.proto` as a *new* protoc input in a new
  directory, so the RBA-1 no-drift guard over existing fixtures is unaffected —
  good; keep the new proto out of any existing fixture's generation set.
- Confirm `field_metadata` index alignment in the expected JSON's `field_metadata`
  array: it must be keyed by the **positional** field index (canonical per §5),
  matching the order in which the fixture builds columns in both languages.

### Step-2 re-review addendum (2026-06-24)

Verdict: **APPROVE**, with one standing user-authorization gate (below).

All three required changes are resolved in the text:

- **Item 2 (single source of truth) — RESOLVED.** C++ consumption now reads the
  *same* committed fixture + expected JSON the Rust side reads (lines ~112-120);
  the "C++ may hardcode" hedge is gone; the transitive C++==Rust proof holds.
- **Item 3 (scalar-element null normalization) — RESOLVED.** The null-path
  assertions now state both languages probe `is_null(j)` per scalar element and
  encode the null identically in the normalized readout (lines ~148-150).
- **Item 1 (D-RBA-7 edit) — technically correct; intent preserved.** The factual
  correction is independently verified against the as-built green code: Arrow C++
  `StructArray::field(i)` is already windowed (re-slicing double-applies the offset
  and reads the wrong row), arrow-rs `columns()` is not (Rust `from_struct` must
  `.slice()`). D-RBA-7's intent (correct child windowing, no read-through, no
  assumed pre-rebasing) is preserved; only the wrong C++ mechanism wording and its
  stop-and-ask trigger are corrected. No code changes. The stale conditional
  sentence that contradicted the unconditional scope block was fixed inline.

**Standing gate (reviewer cannot clear this):** editing locked decision D-RBA-7
requires the **user's own** sign-off per the digest preamble. This design records
the edit as "PM-confirmed," but reviewer-side I treat any relayed claim of user
consent as unverified — only the user's direct confirmation clears the gate. The
*technical* content is approved; the *authorization* to amend D-RBA-7 remains the
user's to grant. Implementation of the D-RBA-7 digest edit should proceed only
once that direct user confirmation is on record. Everything else in RBA-7 is
unblocked.

The capstone genuinely proves D-RBA-8: one shared `.proto` (both accessors from
one model), one shared committed fixture (both batches carry identical rows /
nulls / metadata), one shared committed expected artifact (both readouts == it ⇒
each other), with metadata `nullptr ≡ {}` normalized and every field kind + each
required null asserted in both languages.
