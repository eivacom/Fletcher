# RBA — Progress Log

One section is appended per item by the runbook after it is green, reviewed,
logged, and pushed. See
[RBA-recordbatch-accessor.md](RBA-recordbatch-accessor.md) for the tracker.

<!-- Entries appended below by the round runbook -->

## RBA-1 — Option plumbing + additive no-drift guarantee (2026-06-24)

**Forcing test:** `AccessorTest.OptGatedEmissionLeavesExistingOutputsByteIdentical` → 🟢   (⚪ → 🔴 → 🟢)
**Design:** `plans/RBA-1-opt-plumbing-no-drift.md`  ·  Step-2: APPROVE (after 1 rework cycle — 4 items resolved)
**What landed:** Two default-off `--fletcher_opt` tokens (`accessor`, `rust`) that each emit one new output file (`<stem>.fletcher.accessor.pb.h`, `<stem>.fletcher.rs`) via **new** emitter entry points, unconditionally (one file per token, never content-gated). No existing emitter function touched and no existing output byte changed (D-RBA-1). RBA-1 files are minimal-but-valid skeletons; real accessor content lands RBA-2+.
**Files touched:** `protoc/src/generator.cpp` (opt parse + 2 additive `context->Open` blocks + 2 filename helpers), `protoc/src/recordbatch_accessor_emitter.{hpp,cpp}` (new), `protoc/CMakeLists.txt`, `integration-tests/protoc-arrow-bridge/CMakeLists.txt` (new `accessor_tests` target, 3 `-I` roots), `integration-tests/protoc-arrow-bridge/tests/test_accessor.cpp` (new), `integration-tests/protoc-arrow-bridge/proto/empty_accessor.proto` (new degenerate fixture).
**Reviews:** compliance `CONFORMANT` (initial HIGH on Rust well-formedness silent downgrade → fixed) · code-review `blocking 0 (2 resolved: staging + rustc-proof) / should-fix 0 (majors adjudicated) / nits logged`  (full: `plans/reviews/RBA-1-conformance.md`, `plans/reviews/RBA-1-codereview.md`)
**Verification:** inner-loop `PASS` — plugin unit suite 3/3; integration `ctest` 64/64 (62 pre-existing + 2 new Accessor); `rustc` parse-check RAN on this box (rustc 1.96.0, not skipped). Full round-wide suite (core unit tests + Rust crate) deferred to a later stage-close — the Rust crate does not exist until RBA-5. (accepted residuals: none)
**Commit / push:** `feature/recordbatchaccessor` → `origin feature/recordbatchaccessor`
**Carry-forwards / stop-and-asks:** none. (Won't-fix nits logged: unknown-token errors out of scope — acceptance = unknown ⇒ no new files; `FieldDescriptor*` null guards unneeded in plugin context; docs deferred to RBA-7.)

## RBA-2 — C++ scalar accessor + positional type-check validation (2026-06-24)

**Forcing test:** `AccessorTest.ScalarColumnsReadAndValidatePositionally` → 🟢   (⚪ → 🔴 → 🟢)
**Design:** `plans/RBA-2-cpp-scalar-accessor.md`  ·  Step-2: APPROVE (after 1 rework cycle — 3 items; reviewer also fixed inline the anon-ns→`namespace fletcher` linkage requirement)
**What landed:** First real generated C++ `<Class>Accessor`: dual `Make(RecordBatch)`/`Make(StructArray)` factories sharing one `FromColumns_` (column-count gate + per-column `type()->Equals(*expected,/*check_metadata=*/false)` + `null_count()==0` for proto-non-nullable), caching `std::shared_ptr<ConcreteArray>` columns (buffers owned — no source ref), scalar getters (`Value`/zero-copy `GetView`; nullable → `std::optional`/`std::string_view`), `num_rows()`. Validation is positional + type-only (name- and nullable-flag-tolerant). Composite-containing messages **fail fast** (RBA-4 will add real handling). Metadata getters deferred to RBA-3 (storage laid). Non-behavioural helper relocation: field-walk helpers moved anon-ns → `namespace fletcher` via new shared internal header `protoc/include/generator_internal.hpp`.
**Files touched:** `protoc/src/recordbatch_accessor_emitter.cpp`, `protoc/include/generator_internal.hpp` (new), `protoc/src/generator.cpp` (linkage move + include), `integration-tests/protoc-arrow-bridge/{CMakeLists.txt, proto/accessor_scalar.proto (new), tests/test_accessor_scalar.cpp (new)}`.
**Reviews:** compliance `CONFORMANT` (initial blocking: composite-validation gap → fixed via fail-fast) · code-review `blocking 0 (3 "missing files" = untracked/staging; 2 CRITICAL/MAJOR confirmed FALSE POSITIVES with evidence — Arrow `field(i)` pre-windows; `Status::Invalid` is variadic) / minors addressed (test strengthened)`  (full: `plans/reviews/RBA-2-conformance.md`, `plans/reviews/RBA-2-codereview.md`)
**Verification:** inner-loop `PASS` — integration `ctest` 67/67 (Accessor suite 5/5); RBA-1 no-drift `OptGatedEmissionLeavesExistingOutputsByteIdentical` 🟢 (proves the helper relocation changed no emitted bytes). Full round-wide suite deferred to a later stage-close (Rust crate not built until RBA-5). (accepted residuals: none)
**Commit / push:** `feature/recordbatchaccessor` → `origin feature/recordbatchaccessor`
**Carry-forwards / stop-and-asks:** **[RBA-4/RBA-5/6]** cross-language `StructArray` child-windowing asymmetry — Arrow **C++** `StructArray::field(i)` returns children already sliced to `[offset,len)` (do NOT re-`Slice`), whereas arrow-rs `StructArray::columns()` is **not** pre-windowed (Rust `from_struct` MUST slice). Design doc RBA-2 corrected for the record; RBA-4 (C++ nested) and RBA-5/6 (Rust) must honor the per-language rule.

## RBA-3 — C++ generic metadata access (2026-06-24)

**Forcing test:** `AccessorTest.ExposesSchemaAndFieldMetadataGenerically` → 🟢   (⚪ → 🔴 → 🟢)
**Design:** `plans/RBA-3-cpp-generic-metadata.md`  ·  Step-2: APPROVE (cycle 1; 2 non-blocking doc-only clarifications folded in)
**What landed:** Three public getters on every generated `<Class>Accessor`, returning `const arrow::KeyValueMetadata*` over RBA-2's already-stored `schema_metadata_`/`fields_`: `schema_metadata()`, `field_metadata(int i)` (canonical; null on OOB / null field / absent), `field_metadata(const std::string& name)` (linear live-name scan; null on unknown). Metadata is exposed **verbatim and domain-agnostic** — the generator references no keys (D-RBA-5). Absent/OOB/unknown → `nullptr`, never throw. Pure add-on (D-RBA-1). Emitted comments preserve the `Field::metadata()` by-value-copy/co-ownership lifetime justification + the `schema_metadata()` null contract (struct source / no-metadata batch).
**Files touched:** `protoc/src/recordbatch_accessor_emitter.cpp` (3 getters + `<string>` include), `integration-tests/protoc-arrow-bridge/tests/test_accessor_scalar.cpp` (new `ExposesSchemaAndFieldMetadataGenerically`).
**Reviews:** compliance `PASS` (initial blocking: test used domain-flavored key `unit=m/s` → neutralized to arbitrary `field_key_0=field_val_0` / `schema_key_0=schema_val_0`; emitter was always generic) · code-review `blocking 0 / major 0 / minor 0 / nit 0` (clean — bounds/null/lifetime/name-scan all verified)  (full: `plans/reviews/RBA-3-conformance.md`, `plans/reviews/RBA-3-codereview.md`)
**Verification:** inner-loop `PASS` — Accessor suite 6/6; integration `ctest` 68/68; RBA-1 no-drift + RBA-2 scalar still 🟢. Full round-wide suite deferred to a later stage-close (Rust crate not built until RBA-5). (accepted residuals: none)
**Commit / push:** `feature/recordbatchaccessor` → `origin feature/recordbatchaccessor`
**Carry-forwards / stop-and-asks:** **[RBA-6]** metadata-representation parity — C++ returns `nullptr` for absent metadata, the Rust side returns an empty `HashMap`; the cross-language parity check (RBA-6/RBA-7) must treat C++-`nullptr` ≡ Rust-empty-map, not a literal mismatch.
