# GIR-1 Design: Generator Compile-And-Run Harness

## Summary

GIR-1 creates a new greenfield integration harness at `integration-tests/protoc-coverage/`. It is separate from `integration-tests/protoc-arrow-bridge/` because the existing bridge suite already proves targeted byte compatibility across many small fixtures, while GIR-1 needs one broad "coverage proto" that compiles and executes every generated surface as a refactor guard before the IR rewrite begins.

The forcing test is:

```text
CoverageHarness.GeneratedCppCompilesEncodesAndReconstructs
```

GIR-1's concrete scope is:

- Generate from `coverage.proto` using the existing plugin.
- Compile and execute generated C++ edge row code.
- Compile and execute generated Arrow view / `ToArrowRow` code.
- Generate IPC schema output.
- Compile and execute generated RBA C++ accessor code.
- Generate TS and Rust accessor outputs, then compile-check them when `tsc` / Rust tooling is available.
- Skip, not fail, the optional TS/Rust checks when the local toolchain is absent.
- Build a representative row, encode it, reconstruct it through the generated encoded-row path, convert it through Arrow view / `ToArrowRow`, and verify field equality at the C++ value level.

GIR-1 does not introduce the GIR-2 parity oracle. It should not claim byte identity yet. Its job is to make generated outputs real compiled artifacts with one broad fixture so GIR-2 can add wire and decode oracles on top.

## Design

### Home and layout

```text
integration-tests/protoc-coverage/
  CMakeLists.txt
  CMakeUserPresets.json
  conanfile.py
  README.md
  proto/
    coverage.proto
    coverage_future.proto
  tests/
    test_coverage_harness.cpp
    test_coverage_accessor.cpp
  ts/
    package.json
    tsconfig.json
    src/
      compile_check.ts
  rust-accessor/
    Cargo.toml
    src/
      lib.rs
    tests/
      compile_check.rs
  cmake/
    run_tsc_check.cmake
    run_cargo_check.cmake
```

`coverage_future.proto` is committed but unwired initially; it holds any currently non-compiling generated output so `coverage.proto` stays clean (see Risks & Unknowns).

### CMakeUserPresets and conanfile

`CMakeUserPresets.json` matches `protoc-arrow-bridge`:

```json
{
  "version": 4,
  "vendor": {
    "conan": {}
  },
  "include": [
    "build/generators/CMakePresets.json"
  ]
}
```

`conanfile.py` follows `integration-tests/protoc-arrow-bridge/conanfile.py`:

- `settings = "os", "compiler", "build_type", "arch"`
- `generators = "CMakeDeps", "CMakeToolchain"`
- Requires:
  - `fletcher-protoc/[*, include_prerelease]`
  - `fletcher-arrow-bridge/[*, include_prerelease]`
  - `fletcher-pubsub/[*, include_prerelease]`
  - `protobuf/3.21.12`
  - `gtest/1.17.0`
  - `nlohmann_json/3.11.3` only if the C++ test uses JSON fixtures; otherwise omit.
  - `zlib/1.3.1`, `override=True`, matching the existing Arrow conflict workaround.
- `build()` configures, builds, sets `CTEST_OUTPUT_ON_FAILURE=1`, and runs `cmake.test()`.

### CMake wiring

CMake wiring mirrors `protoc-arrow-bridge`:

- `find_package(fletcher-protoc CONFIG REQUIRED)`
- `find_package(fletcher-arrow-bridge CONFIG REQUIRED)`
- `find_package(fletcher-pubsub CONFIG REQUIRED)`
- `find_package(protobuf CONFIG REQUIRED)`
- `find_package(Arrow CONFIG REQUIRED)`
- `find_package(GTest REQUIRED)`
- Hard-fail if `fletcher-protoc::plugin` is missing.
- Derive `PROTOBUF_WKT_INCLUDE_DIR` from `protobuf::protoc`.
- Generate into `${CMAKE_CURRENT_BINARY_DIR}/generated`.
- Use `add_custom_command()` to invoke `protobuf::protoc` with:
  - `--plugin=protoc-gen-fletcher=$<TARGET_FILE:fletcher-protoc::plugin>`
  - `--fletcher_out=${GENERATED_DIR}`
  - `--fletcher_opt=ipc,accessor,ts,rust`
  - `-I ${PROTO_DIR}`
  - `-I ${FLETCHER_PROTO_INCLUDE_DIR}`
  - `-I ${PROTOBUF_WKT_INCLUDE_DIR}`
- Use one generation stem: `coverage` (from `coverage.proto` only; leave `coverage_future.proto` unwired until GIR-10 enables it).

**Plugin invocation contract:** Confirm that a single `protoc` invocation with `--fletcher_opt=ipc,accessor,ts,rust` emits all six primary artifacts (`.pb.h`, `.arrow.pb.h`, `.accessor.pb.h`, `.ts`, `.rs`, `.ipc` files). If the plugin requires separate invocations for different opt values (as `protoc-arrow-bridge` historically did for `ipc` vs `accessor`), split the custom commands and declare outputs from each; do not merge into a single combined invocation.

### Generated outputs

Generated outputs declared by CMake:

```text
generated/coverage.fletcher.pb.h
generated/coverage.fletcher.arrow.pb.h
generated/coverage.fletcher.accessor.pb.h
generated/coverage.fletcher.ts
generated/coverage.fletcher.rs
generated/coverage.ScalarCoverage.ipc
generated/coverage.CompositeCoverage.ipc
generated/coverage.ServiceRequest.ipc
generated/coverage.ServiceReply.ipc
```

If the current plugin names IPC outputs per message exactly as `<stem>.<Message>.ipc`, use those names. The harness should declare every IPC file that the runtime test opens.

### CMake targets and tests

CMake targets and tests:

```text
generate_coverage_outputs
coverage_harness_tests
coverage_accessor_tests
CoverageHarness.GeneratedCppCompilesEncodesAndReconstructs
CoverageHarness.GeneratedAccessorCppCompilesAndReads
CoverageHarness.GeneratedTypescriptCompiles
CoverageHarness.GeneratedRustAccessorCompiles
```

`coverage_harness_tests` includes:

```text
tests/test_coverage_harness.cpp
```

It depends on `generate_coverage_outputs`, includes `${GENERATED_DIR}`, links:

```text
GTest::gtest_main
arrow::arrow
fletcher-arrow-bridge::fletcher-arrow-bridge
fletcher-pubsub::fletcher-pubsub
```

(Service surface is compiled — `fletcher-pubsub` linkage is retained because `--fletcher_opt=ipc` routes service definitions through the pubsub emitter. GIR-1 does not execute a transport, but it verifies the surface compiles.)

`coverage_accessor_tests` includes:

```text
tests/test_coverage_accessor.cpp
```

It depends on `generate_coverage_outputs`, includes `${GENERATED_DIR}`, links:

```text
GTest::gtest_main
arrow::arrow
fletcher-arrow-bridge::fletcher-arrow-bridge
```

Both use `gtest_discover_tests(... DISCOVERY_MODE PRE_TEST)`.

### Optional toolchain checks (graceful skip)

Find tools at configure time:

```cmake
find_program(TSC_EXECUTABLE tsc)
find_program(CARGO_EXECUTABLE cargo)
find_program(RUSTC_EXECUTABLE rustc)
```

Add the CTest tests for TS and Rust, but use a skip marker in the wrapper scripts and `SKIP_REGULAR_EXPRESSION` to report Skipped when tools are absent:

```cmake
add_test(NAME CoverageHarness.GeneratedTypescriptCompiles
    COMMAND "${CMAKE_COMMAND}"
        -DTSC_EXECUTABLE=${TSC_EXECUTABLE}
        -DGENERATED_DIR=${GENERATED_DIR}
        -DTS_DIR=${CMAKE_CURRENT_SOURCE_DIR}/ts
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/run_tsc_check.cmake)
set_tests_properties(CoverageHarness.GeneratedTypescriptCompiles
    PROPERTIES SKIP_REGULAR_EXPRESSION "SKIP_MARKER")

add_test(NAME CoverageHarness.GeneratedRustAccessorCompiles
    COMMAND "${CMAKE_COMMAND}"
        -DCARGO_EXECUTABLE=${CARGO_EXECUTABLE}
        -DRUSTC_EXECUTABLE=${RUSTC_EXECUTABLE}
        -DGENERATED_DIR=${GENERATED_DIR}
        -DRUST_DIR=${CMAKE_CURRENT_SOURCE_DIR}/rust-accessor
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/run_cargo_check.cmake)
set_tests_properties(CoverageHarness.GeneratedRustAccessorCompiles
    PROPERTIES SKIP_REGULAR_EXPRESSION "SKIP_MARKER")
```

When the tool is absent, the wrapper script prints the marker to stdout and exits 0. CTest parses stdout, sees the marker, and reports the test as Skipped (not Failed or Passed). This approach works reliably across all platforms.

`run_tsc_check.cmake` behavior:

- If `TSC_EXECUTABLE` is empty or missing, print `"SKIP_MARKER: tsc not found on PATH"` to stdout and exit cleanly.
- Copy or reference `generated/coverage.fletcher.ts` from the TS check package.
- Run `tsc --noEmit -p ts/tsconfig.json`.
- No network install is required inside CTest. `package.json` documents dependencies, but the CTest check uses whatever `tsc` is already on `PATH`.

`run_cargo_check.cmake` behavior:

- If `cargo` or `rustc` is absent, print `"SKIP_MARKER: rustc/cargo not found on PATH"` to stdout and exit cleanly.
- Set an environment variable such as `FLETCHER_GENERATED_RUST_DIR=${GENERATED_DIR}`.
- Run `cargo check --manifest-path rust-accessor/Cargo.toml --locked`.
- The Rust crate uses `include!(concat!(env!("FLETCHER_GENERATED_RUST_DIR"), "/coverage.fletcher.rs"));`.

### coverage.proto shape

```proto
syntax = "proto3";
package integration.coverage;

import "google/protobuf/timestamp.proto";
import "google/protobuf/duration.proto";
import "google/protobuf/wrappers.proto";
import "google/protobuf/empty.proto";
import "fletcher/options.proto";

enum TopLevelStatus {
  TOP_LEVEL_STATUS_UNSPECIFIED = 0;
  TOP_LEVEL_STATUS_OK = 1;
  TOP_LEVEL_STATUS_WARN = 2;
  TOP_LEVEL_STATUS_ERROR = 3;
}

message NestedEnums {
  enum InnerStatus {
    INNER_STATUS_UNSPECIFIED = 0;
    INNER_STATUS_ACTIVE = 1;
    INNER_STATUS_DISABLED = 2;
  }

  InnerStatus state = 1;
}

message ScalarCoverage {
  bool bool_value = 1;
  int32 int32_value = 2;
  int64 int64_value = 3;
  uint32 uint32_value = 4;
  uint64 uint64_value = 5;
  sint32 sint32_value = 6;
  sint64 sint64_value = 7;
  fixed32 fixed32_value = 8;
  fixed64 fixed64_value = 9;
  sfixed32 sfixed32_value = 10;
  sfixed64 sfixed64_value = 11;
  float float_value = 12;
  double double_value = 13;
  string string_value = 14;
  bytes bytes_value = 15;

  optional bool optional_bool = 16;
  optional int32 optional_int32 = 17;
  optional string optional_string = 18;
  optional bytes optional_bytes = 19;

  google.protobuf.BoolValue wrapped_bool = 20;
  google.protobuf.Int32Value wrapped_int32 = 21;
  google.protobuf.Int64Value wrapped_int64 = 22;
  google.protobuf.UInt32Value wrapped_uint32 = 23;
  google.protobuf.UInt64Value wrapped_uint64 = 24;
  google.protobuf.FloatValue wrapped_float = 25;
  google.protobuf.DoubleValue wrapped_double = 26;
  google.protobuf.StringValue wrapped_string = 27;
  google.protobuf.BytesValue wrapped_bytes = 28;

  google.protobuf.Timestamp timestamp_value = 29;
  google.protobuf.Duration duration_value = 30;

  TopLevelStatus status = 31;
  NestedEnums.InnerStatus nested_status = 32;
}

message Leaf {
  int32 id = 1;
  string label = 2;
  TopLevelStatus status = 3;
}

message Branch {
  Leaf leaf = 1;
  optional Leaf optional_leaf = 2;
  repeated Leaf leaves = 3;
}

message ScalarListWrapper {
  option (fletcher.flatten) = true;
  repeated int32 values = 1;
}

message StructListWrapper {
  option (fletcher.flatten) = true;
  repeated Leaf values = 1;
}

message NestedScalarListWrapper {
  option (fletcher.flatten) = true;
  repeated ScalarListWrapper values = 1;
}

message NestedStructListWrapper {
  option (fletcher.flatten) = true;
  repeated StructListWrapper values = 1;
}

message FlattenedPoint {
  option (fletcher.flatten) = true;
  double x = 1;
  double y = 2;
}

message FieldFlattenedPosition {
  FlattenedPoint point = 1 [(fletcher.flatten_field) = true];
}

message CompositeCoverage {
  ScalarCoverage scalars = 1;
  optional ScalarCoverage optional_scalars = 2;

  Branch branch = 3;
  optional Branch optional_branch = 4;

  repeated int32 repeated_scalar = 5;
  repeated string repeated_string = 6;
  repeated bytes repeated_bytes = 7;
  repeated Leaf repeated_struct = 8;

  map<string, int32> map_scalar = 9;
  map<string, Leaf> map_struct = 10;

  ScalarListWrapper flattened_scalar_list = 11;
  StructListWrapper flattened_struct_list = 12;
  repeated ScalarListWrapper nested_scalar_lists = 13;
  repeated StructListWrapper nested_struct_lists = 14;
  repeated NestedScalarListWrapper depth3_scalar_lists = 15;
  repeated NestedStructListWrapper depth3_struct_lists = 16;

  optional ScalarListWrapper optional_flattened_scalar_list = 17;
  optional StructListWrapper optional_flattened_struct_list = 18;

  FlattenedPoint message_flattened_point = 19;
  FieldFlattenedPosition field_flattened_position = 20;
}

message ServiceRequest {
  CompositeCoverage row = 1;
}

message ServiceReply {
  bool accepted = 1;
  string message = 2;
}

service CoverageService {
  rpc Submit(ServiceRequest) returns (ServiceReply);
}
```

### coverage_future.proto (initially empty or commented)

`coverage_future.proto` is a placeholder for any currently non-compiling or unsupported fixture fields. It is committed to the repo but not wired into the CMake generation or test until a later round (e.g., GIR-10) enables it. This allows us to document the shape of future coverage work without blocking GIR-1 on unsupported generator features. If the current generator cannot emit compilable code for scalar-leaf nested lists today, those messages and the related fields will move here; if it can emit clean stubs or omits them cleanly, they remain in `coverage.proto` and are simply left unused in the test.

**Decision rule for `coverage_future.proto` population:**
- If generation hard-fails (plugin errors) on a field → move field and its type to `coverage_future.proto`, omit from `coverage.proto`.
- If generation emits non-compiling C++ output → move field and its type to `coverage_future.proto`, omit from `coverage.proto`.
- If generation omits or emits a clean stub (test just doesn't use the field) → keep in `coverage.proto`, don't execute it in test.

### Coverage decisions

- `ScalarCoverage` is the canonical scalar/WKT/enum fixture.
- `CompositeCoverage` is the canonical struct/list/map/flatten/nesting fixture.
- Depth 2 and depth 3 are included now because RBA already has depth caps and GIR must not accidentally regress them.
- Depth 2 (nested-scalar-lists, nested-struct-lists) and depth 3 (depth3-scalar-lists, depth3-struct-lists) establish the fixture shape for GIR-10's future `List<List<scalar>>` work. If the current flat generator cannot generate scalar-leaf nested lists today in a way that produces compilable C++, those messages/fields move to `coverage_future.proto` and stay unwired until GIR-10 enables them. The plugin must generate cleanly (either usable code or a clean omission); it must not leave non-compiling stubs in `coverage.proto`.
- Enums are included as both package-scope and nested enum references. GIR-1 only compiles current int32-lowered behavior; GIR-9 later adds typed C++ enum symbols against the same fixture.
- Service is included so service emitters stay visible to generation and future BIND work, but GIR-1 does not execute a pubsub transport.

### What "compiles and runs" means in GIR-1

- **C++ edge header:**
  - Include `coverage.fletcher.pb.h`.
  - Construct a `CompositeCoverage` generated row with non-default values, empty containers, non-empty containers, unset optionals, and set optionals.
  - Call generated `Encode()`.
  - Construct the generated row from `fletcher::EncodedRow` using the generated encoded-row constructor (exact API naming deferred to implementation, but it accepts encoded bytes and re-inflates the row).
  - Assert representative scalar, WKT, enum-as-int32, string, bytes, struct, repeated, map, and flatten values are reconstructed.

- **Arrow view + `ToArrowRow`:**
  - Include `coverage.fletcher.arrow.pb.h`.
  - Build the same generated row.
  - Call generated `ToArrowRow`.
  - Use generated Arrow view accessors and/or `fletcher-arrow-bridge::Codec` to verify representative values can be read from the Arrow side.
  - GIR-1 verifies execution and value reconstruction, not byte identity. `Encode()==EncodeRow()` belongs to GIR-2.

- **IPC schema:**
  - Generate `.ipc` for all top-level message types that the test opens.
  - Runtime C++ test opens the emitted IPC stream and verifies it is readable as an Arrow schema.
  - GIR-1 may assert field names/types exist for `ScalarCoverage` and `CompositeCoverage`.
  - Byte-identical IPC/source-schema parity is left to GIR-5's oracle.

- **RBA C++ accessor:**
  - Include `coverage.fletcher.accessor.pb.h`.
  - Build an Arrow `RecordBatch` matching `CompositeCoverage`.
  - Construct the generated accessor.
  - Read representative scalar, optional, struct, repeated, map, and nested-list values.
  - Do not rewrite or reshape RBA. This is a compile/run guard for the read-only emitter.

- **TS:**
  - Generate `coverage.fletcher.ts`.
  - Run `tsc --noEmit`.
  - No runtime JS execution in GIR-1.
  - Skip if `tsc` is absent.

- **Rust accessor:**
  - Generate `coverage.fletcher.rs`.
  - Compile-check the local `rust-accessor` crate with `cargo check --locked`.
  - No RBA rewrite and no generated Rust row/backend work.
  - Skip if `cargo` or `rustc` is absent.

### Not in GIR-1

- Wire byte identity.
- Decode known golden bytes.
- Descriptor/golden byte baselines.
- TS descriptor byte identity.
- IR unit tests.
- Enum typed C++ symbol emission.
- New behavior for unsupported proto mappings.
- RBA migration onto IR.

### Exact commands (corrected for cross-platform)

Scoped inner-loop command for Windows (VS multi-config generator):

```powershell
cd C:\Users\CTM\source\prototypes\Fletcher\integration-tests\protoc-coverage
conan install . --build=missing -pr:a=../../.conan-profiles/Windows-msvc194-x86_64-Release
cmake --preset conan-default
cmake --build --preset conan-release
ctest --preset conan-release --output-on-failure -R CoverageHarness.GeneratedCppCompilesEncodesAndReconstructs
```

Scoped inner-loop command for Linux (single-config generator):

```bash
cd /workspaces/Fletcher/integration-tests/protoc-coverage
conan install . --build=missing -pr:a=../../.conan-profiles/Linux-gcc13-x86_64-Release
cmake --preset conan-release
cmake --build --preset conan-release
ctest --preset conan-release --output-on-failure -R CoverageHarness.GeneratedCppCompilesEncodesAndReconstructs
```

**Explanation:** On Windows, the VS generator is multi-config and Conan creates the `conan-default` *configuration preset* (which selects the generator at configure time). On Linux, the Unix Makefiles single-config generator creates only the `conan-release` preset. Both then use `conan-release` for build and test. If generator-agnostic behavior is preferred (matching `protoc-arrow-bridge` CI practice), substitute both platforms with:

```bash
conan build .
```

This invokes CMake configure, build, and test via conan's abstraction and works on both Windows and Linux without tracking preset names.

**Full suite addition for Windows:**

```powershell
cd C:\Users\CTM\source\prototypes\Fletcher
conan create core/. --build=missing -pr:a=.conan-profiles/Windows-msvc194-x86_64-Release
conan create arrow-bridge/. --build=missing -pr:a=.conan-profiles/Windows-msvc194-x86_64-Release
conan create pubsub/. --build=missing -pr:a=.conan-profiles/Windows-msvc194-x86_64-Release
conan create protoc/. --build=missing -pr:a=.conan-profiles/Windows-msvc194-x86_64-Release

cd C:\Users\CTM\source\prototypes\Fletcher\integration-tests\protoc-coverage
conan install . --build=missing -pr:a=../../.conan-profiles/Windows-msvc194-x86_64-Release
cmake --preset conan-default
cmake --build --preset conan-release
ctest --preset conan-release --output-on-failure
```

**Full suite addition for Linux:**

```bash
cd /workspaces/Fletcher
conan create core/. --build=missing -pr:a=.conan-profiles/Linux-gcc13-x86_64-Release
conan create arrow-bridge/. --build=missing -pr:a=.conan-profiles/Linux-gcc13-x86_64-Release
conan create pubsub/. --build=missing -pr:a=.conan-profiles/Linux-gcc13-x86_64-Release
conan create protoc/. --build=missing -pr:a=.conan-profiles/Linux-gcc13-x86_64-Release

cd /workspaces/Fletcher/integration-tests/protoc-coverage
conan install . --build=missing -pr:a=../../.conan-profiles/Linux-gcc13-x86_64-Release
cmake --preset conan-release
cmake --build --preset conan-release
ctest --preset conan-release --output-on-failure
```

## Forcing-test mapping

`CoverageHarness.GeneratedCppCompilesEncodesAndReconstructs` is implemented in `tests/test_coverage_harness.cpp`.

It fails red today for the right reason: no `integration-tests/protoc-coverage/` harness exists, no `coverage.proto` is generated, and generated C++ output is not compiled/executed as a single coverage fixture.

The test turns green when:

- `coverage.proto` is run through `protoc-gen-fletcher` with `--fletcher_opt=ipc,accessor,ts,rust` (or the plugin's required invocation sequence if multiple invocations are needed).
- `coverage.fletcher.pb.h` and `coverage.fletcher.arrow.pb.h` are compiled into `coverage_harness_tests`.
- The test builds a `CompositeCoverage` row.
- The test calls generated `Encode()`.
- The test reconstructs the row from encoded bytes with the generated encoded-row constructor.
- The test verifies representative values:
  - scalar primitive
  - optional unset and set
  - WKT timestamp/duration/wrapper
  - enum lowered as current int32 behavior
  - bytes/string
  - nested struct
  - repeated scalar
  - repeated struct
  - map scalar
  - map struct
  - message-level flatten
  - field-level flatten
- The test calls `ToArrowRow` and verifies the Arrow-side representation is usable.
- The emitted IPC schema files are present and readable.

Companion GIR-1 tests:

```text
CoverageHarness.GeneratedAccessorCppCompilesAndReads
CoverageHarness.GeneratedTypescriptCompiles
CoverageHarness.GeneratedRustAccessorCompiles
```

Only `CoverageHarness.GeneratedCppCompilesEncodesAndReconstructs` is the forcing test for the item. The other tests are part of GIR-1 acceptance because the story explicitly lists RBA C++/Rust and TS outputs.

GIR-2 will extend this same fixture with:

```text
ParityOracle.EncodeEqualsEncodeRowAndRoundTrips
```

That oracle must compare generated `Encode()` bytes to `Codec.EncodeRow()` bytes and add decode round-trip value equality plus known-golden-byte decode. GIR-1 leaves enough seams by centralizing row construction helpers in `tests/test_coverage_harness.cpp` or a small local helper header inside `tests/`, so GIR-2 can reuse the exact same populated rows and null/empty variants without re-transcribing fixture data.

## Risks & Unknowns

The largest risk is that current generator support may not cover every desired `coverage.proto` field at once, especially scalar-leaf nested lists. The design handles this via `coverage_future.proto`: if generation hard-fails or produces non-compiling output on a field, that field and its type move to `coverage_future.proto` (committed but unwired until GIR-10 enables it). If current generation omits the field cleanly or produces a clean stub, it remains in `coverage.proto` and is simply not executed in the test.

TS and Rust toolchains are intentionally optional for local CTest. Their absence must report Skipped (via `SKIP_REGULAR_EXPRESSION`), not Failed or Passed. CI can still install them and make those checks active.

The harness must not embed future IR facts as C++ type strings. Test assertions may include current generated API names and Arrow field names, but fixture metadata and helper code must not create a proto-to-IR table using C++ spelling. GIR-3 owns the language-neutral IR model.

RBA remains read-only. GIR-1 can compile and execute generated RBA accessor output, but it must not refactor `recordbatch_accessor_emitter.cpp` or change `FieldKind` lifetime.

Wire bytes are a hard invariant, but GIR-1 is not the byte-identity oracle. Any byte assertions added in GIR-1 should be limited to "non-empty / decodable / reconstructs equivalent values." Exact byte equality belongs in GIR-2.

## Files-to-touch

New harness files:

```text
integration-tests/protoc-coverage/CMakeLists.txt
integration-tests/protoc-coverage/CMakeUserPresets.json
integration-tests/protoc-coverage/conanfile.py
integration-tests/protoc-coverage/README.md
integration-tests/protoc-coverage/proto/coverage.proto
integration-tests/protoc-coverage/proto/coverage_future.proto
integration-tests/protoc-coverage/tests/test_coverage_harness.cpp
integration-tests/protoc-coverage/tests/test_coverage_accessor.cpp
integration-tests/protoc-coverage/ts/package.json
integration-tests/protoc-coverage/ts/tsconfig.json
integration-tests/protoc-coverage/ts/src/compile_check.ts
integration-tests/protoc-coverage/rust-accessor/Cargo.toml
integration-tests/protoc-coverage/rust-accessor/src/lib.rs
integration-tests/protoc-coverage/rust-accessor/tests/compile_check.rs
integration-tests/protoc-coverage/cmake/run_tsc_check.cmake
integration-tests/protoc-coverage/cmake/run_cargo_check.cmake
```

CI/workflow files:

```text
.github/workflows/ci.integration-test.protoc-coverage.yml
.github/workflows/ci.pr.yml (or the existing top-level caller, if it exists)
```

The `ci.integration-test.protoc-coverage.yml` file is a `workflow_call` reusable workflow that:
- Accepts inputs: `devcontainer-image` (passed by the caller)
- Includes `integration-tests/protoc-coverage` in its sparse-checkout list (matching how `protoc-arrow-bridge` includes its dir)
- Runs conan/cmake/ctest for the coverage harness

The `ci.pr.yml` (or equivalent top-level caller) must:
- Invoke `ci.integration-test.protoc-coverage.yml` with `devcontainer-image` input
- Add path filter to trigger on changes under `core/**`, `protoc/**`, `arrow-bridge/**`, `pubsub/**`, `integration-tests/protoc-coverage/**`, or `integration-tests/protoc-arrow-bridge/**`

No production generator files are required for GIR-1 unless the forcing test exposes a genuine generated-code compile break. If that happens, keep the fix minimal and do not start the GIR-3 IR rewrite early.

## Step-2 re-review (2026-07-10)

**Verdict: APPROVE.** The full rewrite resolves all three prior blockers and both should-fix items. Nothing regressed against `docs/robustness-plan.md` §3a or `plans/GIR-locked-decisions.md`.

Prior findings, confirmed resolved:

1. **Blocker 1 (per-OS presets) — RESOLVED.** The "Exact commands" section now gives distinct Windows and Linux blocks. Windows (VS multi-config) configures with `conan-default` and builds/tests with `conan-release`; Linux (single-config) uses `conan-release` throughout. The explanation correctly attributes this to multi-config vs single-config preset generation, and offers the generator-agnostic `conan build .` fallback matching `protoc-arrow-bridge` CI practice. Profile paths (`../../.conan-profiles/...`) resolve correctly from the harness dir on both OSes.
2. **Blocker 2 (graceful skip) — RESOLVED.** The mechanism is now `find_program` at configure time plus `add_test` invoking `run_tsc_check.cmake`/`run_cargo_check.cmake`, with `set_tests_properties(... SKIP_REGULAR_EXPRESSION "SKIP_MARKER")`. Wrapper prints `SKIP_MARKER: ...` and exits 0 when the tool is absent; CTest matches the regex and reports **Skipped** irrespective of exit code — so an absent `tsc`/`rustc` can never surface as Passed, and a present-but-failing toolchain still reports Failed (nonzero, no marker). Correct.
3. **Blocker 3 (CI wiring) — RESOLVED.** Files-to-touch now names the reusable `ci.integration-test.protoc-coverage.yml` (with `devcontainer-image` input and the `integration-tests/protoc-coverage` sparse-checkout entry) and the concrete top-level caller edit in `ci.pr.yml`: invoke with `devcontainer-image` and add the path filter over `core/**`, `protoc/**`, `arrow-bridge/**`, `pubsub/**`, and both integration-test dirs.
4. **Should-fix 4 (`coverage_future.proto`) — FOLDED IN.** The explicit decision rule now routes both hard-fail *and* non-compiling C++ output to `coverage_future.proto` (committed, unwired until GIR-10), keeping `coverage.proto` clean.
5. **Should-fix 5 (single invocation → six artifacts) — FOLDED IN.** The "Plugin invocation contract" note requires confirming one `--fletcher_opt=ipc,accessor,ts,rust` invocation emits all six artifacts, and mandates splitting the custom commands (declaring per-invocation outputs) if the plugin needs separate passes.

No new blockers. Minor forward note (non-blocking, already anticipated by the doc): the depth-3 fixtures sit at the RBA depth-2/3 cap (locked #3), so the accessor test must read no deeper than the cap; the `coverage_future.proto` decision rule already covers any field that exceeds it.
