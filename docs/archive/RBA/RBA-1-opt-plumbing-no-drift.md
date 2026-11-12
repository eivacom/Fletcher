# RBA-1 - Option Plumbing + Additive No-Drift Design

## Summary

RBA-1 adds two default-off `--fletcher_opt` tokens, `accessor` and `rust`, to
`ArrowRowGenerator::Generate`. When present, they write only new files:

- `accessor` -> `<stem>.fletcher.accessor.pb.h`
- `rust` -> `<stem>.fletcher.rs`

No existing output path changes. Existing files (`.fletcher.pb.h`,
`.fletcher.arrow.pb.h`, `.fletcher.ts`, and `.<Message>.ipc`) must stay
byte-identical whether or not the new options are present. The initial emitters
produce minimal valid artifacts only: an empty compilable C++ header and a Rust
module banner/comment that parses as a Rust source file. RBA-2+ owns the accessor
class/struct content.

This design follows D-RBA-1 and D-RBA-2. If implementation discovers that any
existing generated output changes, or that an existing emitter function must
change behavior to support this, stop and ask.

## Design

### Option parsing in `ArrowRowGenerator::Generate`

Extend the existing comma-separated parser in
`protoc/src/generator.cpp::ArrowRowGenerator::Generate` next to the current
`schema_only`, `ts`, and `ipc` booleans:

```cpp
bool schema_only = false;
bool emit_ts = false;
bool emit_ipc = false;
bool emit_accessor = false;
bool emit_rust = false;
```

The token loop adds:

```cpp
else if (token == "accessor")
    emit_accessor = true;
else if (token == "rust")
    emit_rust = true;
```

Unknown tokens keep today's behavior: they are ignored and do not create output.
Absent options leave both new booleans false. The parser remains orthogonal:
`schema_only,accessor`, `ts,accessor`, `ipc,rust`, and any combination with the
existing tokens are legal and independently gated.

### New output file plumbing

Add filename helpers in `generator.cpp` alongside the existing output-name helpers:

```cpp
std::string AccessorOutputFilename(const std::string& proto_name);
std::string RustAccessorOutputFilename(const std::string& proto_name);
```

They use the same stem derivation as `OutputFilename`, `ViewOutputFilename`, and
`TsOutputFilename`, producing:

- `<stem>.fletcher.accessor.pb.h`
- `<stem>.fletcher.rs`

In `Generate`, after the existing outputs have been written, add independent
blocks:

```cpp
if (emit_accessor) {
    const std::string content = EmitAccessorHeader(file);
    const std::string out_name = AccessorOutputFilename(file->name());
    auto stream = std::unique_ptr<google::protobuf::io::ZeroCopyOutputStream>(
        context->Open(out_name));
    if (!WriteToStream(stream.get(), content, error)) return false;
}

if (emit_rust) {
    const std::string content = EmitRustAccessor(file);
    const std::string out_name = RustAccessorOutputFilename(file->name());
    auto stream = std::unique_ptr<google::protobuf::io::ZeroCopyOutputStream>(
        context->Open(out_name));
    if (!WriteToStream(stream.get(), content, error)) return false;
}
```

The new blocks do not wrap or modify the existing `.fletcher.pb.h`,
`.fletcher.arrow.pb.h`, `.fletcher.ts`, or IPC generation blocks. They also do not
depend on `schema_only`; accessor and rust files are read-side/additional artifacts
and are emitted whenever their token is present.

### New emitter functions

Add new emitter entry points, without editing existing emitter functions:

```cpp
// protoc/src/recordbatch_accessor_emitter.hpp
namespace fletcher {
std::string EmitAccessorHeader(const google::protobuf::FileDescriptor* file);
std::string EmitRustAccessor(const google::protobuf::FileDescriptor* file);
}
```

Implement them in `protoc/src/recordbatch_accessor_emitter.cpp` and add that file
to `fletcher_plugin_core` in `protoc/CMakeLists.txt`.

RBA-1 content is deliberately minimal. Each emitter emits **exactly one file per
proto invocation, unconditionally** (the `context->Open`/`WriteToStream` block runs
whenever the token is set, never suppressed on message content) â€” unlike the view
block, which is skipped when `GenerateViewFile` returns empty. This is what makes
the forcing test's "exactly +2 files" assertion true for the whole class, not just
the chosen fixtures.

- `EmitAccessorHeader` emits generated-file comments, `#pragma once`, and a valid
  empty namespace skeleton matching `fletcher_gen::<proto package>`. No includes
  beyond what the minimal header needs. No accessor classes yet.
- `EmitRustAccessor` emits generated-file comments and a Rust module/banner body
  that parses as a standalone Rust source file. It must not emit package wrapper
  modules; the RBA-5 assembler owns package mounting per D-RBA-10.

The call sites are only the two new `Generate` blocks. Existing functions such as
`GenerateFile`, `GenerateViewFile`, `GenerateTypeScriptFile`, schema IPC helpers,
and their subordinate emitters are not edited for behavior.

### Reuse of shared helpers

For RBA-1 minimal output, the new emitters do not need field-level content. The
implementation should still preserve the RBA model boundary:

- The message set for future content is `OrderedMessages(file)`, with the same
  recursive and flattened-wrapper skips already used by existing emitters.
- `GatherFields` and `FieldInfo` remain the shared model for RBA-2+ accessor
  content.
- These helpers are read-only: do not change their ordering, filtering, mapping,
  or emitted behavior as part of RBA-1.

Current implementation note: `OrderedMessages`, `GatherFields`, and `FieldInfo`
are local to `generator.cpp`. If a separate accessor emitter source needs direct
access to them before RBA-2, the allowed move is a declaration/visibility
extraction with no semantic change. If that extraction would alter an existing
emitter or generated bytes, STOP-AND-ASK. For the RBA-1 minimal emitters, avoid
that risk by keeping the new emitter independent of field traversal until content
work begins.

### Unknown/absent opt handling

No option, empty option, or unknown option writes no new accessor files. Unknown
tokens continue to be ignored exactly as today, so `--fletcher_opt=unknown` emits
only the existing default outputs. `accessor` and `rust` are individually gated:
passing one does not imply the other.

## Forcing-Test Mapping

Add the forcing test to the existing
`integration-tests/protoc-arrow-bridge` harness, not a new
`integration-tests/protoc-accessor` harness.

Decision: use `protoc-arrow-bridge` for RBA-1 because it already locates
`fletcher-protoc::plugin`, `protobuf::protoc`, proto include roots, generated
output directories, Arrow dependencies, and GTest. RBA-1 is a generator no-drift
test over the existing option matrix, so colocating it with the current generator
integration harness gives maximum coverage with minimum new infrastructure. A
dedicated `protoc-accessor` harness can be introduced later if RBA-2+ accessor
runtime tests become large enough to justify separation.

Add `tests/test_accessor.cpp` with:

```cpp
TEST(AccessorTest, OptGatedEmissionLeavesExistingOutputsByteIdentical)
```

**D-RBA-1 verification (no-drift contract).** The forcing test verifies the
"with flags == without flags" half: for each baseline option set, run protoc
twice (baseline vs. baseline + `accessor,rust`) and assert byte-identical
existing outputs + exactly 2 new files. However, this **within-build test does
NOT prove the before/after-feature half** — that post-feature plugin bytes match
pre-feature plugin bytes. That half is guaranteed by: **(i) the untouched-emitter
discipline** — existing emitter functions and their outputs are never modified
(enforced by code review + this test), and **(ii) the existing integration test
suite**: the per-fixture TUs in `integration-tests/protoc-arrow-bridge/tests/`
(`test_telemetry.cpp`, `test_nested.cpp`, `test_arrow_view.cpp`,
`test_nullability_consistency.cpp`, …) plus `test_ipc_parity.cpp` already
compile and consume the existing `.fletcher.pb.h`, `.fletcher.arrow.pb.h`, and
`.ipc` outputs; these tests serve as regression guards and must stay green.
(NOTE: there is no `test_generator.cpp` or `test_view_class.cpp` in this harness —
the guard is the per-fixture suite named above.) A change to an existing emitter shows up
immediately as a test failure in that suite, pinning the before/after invariant
without a separate golden. This two-level guard (discipline + existing suite)
enforces D-RBA-1 in full.

### Matrix and fixture requirements


The test should run the plugin itself into temporary per-case directories rather
than reuse the harness's normal `generated` directory. For each baseline option
set:

- empty
- `ts`
- `ipc`
- `schema_only`
- `ts,ipc`
- `schema_only,ts,ipc`

run protoc twice against the same fixture proto:

1. baseline options
2. baseline options plus `accessor,rust`

Then assert:

- Every file produced by the baseline run exists in the accessor/rust run.
- Every baseline file is byte-identical in both runs.
- The accessor/rust run's file set equals the baseline set plus exactly:
  `<stem>.fletcher.accessor.pb.h` and `<stem>.fletcher.rs`.
- The C++ accessor header can compile as an empty header in the existing test
  target or via a tiny generated include TU.
- The Rust file is checked for well-formedness: either (a) `rustc` is available
  and the test runs `rustc --crate-type lib --emit metadata` to parse it
  (preferred), and that check is counted/required, **or** (b) if `rustc` is not
  available in the test environment, the test is marked `GTEST_SKIP()` with a
  logged, *counted* skip message and the Rust parse moved to the RBA-5 Rust crate
  (where the toolchain is pinned). Do **not** silently downgrade the Rust check to
  "file exists" without visible failure/skip reporting; the harness maintainer
  must see the decision made.

The test is run through the existing CTest discovery:

```powershell
ctest -R "Accessor" --output-on-failure
```

- `integration-tests/protoc-arrow-bridge/CMakeLists.txt`
  - Add the test TU and all three `-I` proto search roots as compile definitions.
  - Wire all three roots into the test's runtime protoc invocation.
- `integration-tests/fixtures/empty.proto` (or equivalent degenerate fixture)
  - If one does not exist, create a minimal proto with no messages (or only
    recursive/flatten) for the edge-case matrix test.

The no-drift test should use at least one fixture that exercises all existing
output classes under the matrix. `nested.proto` is a good default because it
declares multiple messages for IPC; if TypeScript descriptor coverage needs richer
fields, use `telemetry.proto` or run the matrix over both. Keep the assertion about
file sets exact for each selected stem.

## Risks/Unknowns

- Helper visibility: the current helper model is in `generator.cpp`. RBA-1 can
  avoid touching it because minimal files do not require message content. RBA-2+
  will likely need a narrow private shared model header or a caller-built model
  passed into the new emitter. Any semantic helper change is disallowed.
- Rust parse availability: the C++ integration harness may not always have
  `rustc`. The design prefers a real parse check when available; if CI lacks
  Rust for this harness, either add Rust as a harness prerequisite or split that
  parse assertion into the later Rust accessor harness. Do not weaken the file-set
  and no-drift checks.
- Option matrix completeness: the forcing test must cover all six baseline sets and
  **at least two fixture classes** (typical + degenerate). A single baseline or a
  single fixture class is insufficient because drift could be conditional on
  `schema_only`, `ts`, `ipc`, or message content.
- D-RBA-1 two-level enforcement: the new forcing test (with flags vs. without) is
  NOT alone sufficient to prove D-RBA-1. The existing per-fixture integration TUs
  (`test_telemetry.cpp` / `test_nested.cpp` / `test_arrow_view.cpp` / …) and
  `test_ipc_parity.cpp` provide the before/after-feature regression guard. This
  design must acknowledge both halves.
- Golden drift sensitivity: line endings, output ordering, and temp paths must not
  enter generated content. The new emitter comments should include the source proto
  name, matching existing generated files, but not absolute paths.

## Implementation Notes (RBA-1 minimal phase)

1. The unconditional one-file-per-token rule is the key to the "+2 files"
   assertion; do not add any `if (has_messages)` guard to the two new blocks in
   `Generate`. The emitters themselves are free to be sparse (empty namespace for
   the C++ header, module banner for Rust), but the file write must always happen.
2. Assert both typical and degenerate fixtures across the matrix; typical fixtures
   exercise view/IPC/TS; degenerate ones prove unconditional emission.
3. Rust well-formedness check must be visible (GTEST_SKIP() with logged skip, not
   silent pass). Decide in implementation whether to require `rustc` or defer to
   RBA-5.
4. All three `-I` roots must be in the test's runtime protoc command line. Verify
   `nested.proto` can be generated (it imports `fletcher/options.proto`).
5. Byte-identity assertion for baseline files catches nondeterminism early.

## Files-to-Touch

- `protoc/src/generator.cpp`
  - Add `emit_accessor` / `emit_rust` option booleans.
  - Add output filename helpers.
  - Add two independent `context->Open`/`WriteToStream` blocks for the new files.
  - Do not edit existing emitter functions' behavior.
- `protoc/src/recordbatch_accessor_emitter.hpp`
  - Declare `EmitAccessorHeader` and `EmitRustAccessor`.
- `protoc/src/recordbatch_accessor_emitter.cpp`
  - Implement minimal valid C++ and Rust outputs.
- `protoc/CMakeLists.txt`
  - Add the new emitter source to `fletcher_plugin_core`.
- `integration-tests/protoc-arrow-bridge/tests/test_accessor.cpp`
  - Add `AccessorTest.OptGatedEmissionLeavesExistingOutputsByteIdentical`.

## Step-2 re-review (2026-06-24, round 2)

**Verdict: APPROVE.** All four round-1 items are resolved:
1. D-RBA-1 two-level guard articulated (within-build forcing test + existing
   suite as before/after guard). The architect's draft named two non-existent
   TUs (`test_generator.cpp`, `test_view_class.cpp`); corrected inline to the real
   per-fixture consumers (`test_telemetry.cpp` / `test_nested.cpp` /
   `test_arrow_view.cpp` / …) + `test_ipc_parity.cpp` (verified present in the
   harness CMakeLists). The argument now rests on tests that exist.
2. Unconditional one-file-per-token emission stated inline + degenerate fixture
   (recursive/flatten-only) added to the matrix; Impl-note 1 forbids any
   `if (has_messages)` guard. "+2 exactly" now holds for the class.
3. Rust well-formedness: `GTEST_SKIP()` with a counted/visible skip, no silent
   "file exists" downgrade; defer-to-RBA-5 is the documented fallback.
4. All three `-I` roots wired into the runtime protoc command line (Files-to-touch
   + Impl-note 4), with `nested.proto`'s `fletcher/options.proto` import called out.

Inline cleanups applied: de-duplicated the Rust bullet (was triplicated) and
un-mashed the Implementation Notes block (was a single run-on line). No
locked-decision deviations; no STOP-AND-ASK. Ready for implementation.
