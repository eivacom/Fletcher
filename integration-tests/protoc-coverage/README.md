# protoc-coverage — generator compile-and-run harness (GIR-1)

A greenfield integration harness that runs one broad `coverage.proto` through the
`protoc-gen-fletcher` plugin and **compiles and executes every generated
surface** as a refactor guard before the IR rewrite (GIR-3+):

- **C++ edge row** (`coverage.fletcher.pb.h`) — build a populated
  `CompositeCoverage`, `Encode()`, reconstruct via the generated encoded-row
  constructor, and verify representative values.
- **Arrow view / `ToArrowRow`** (`coverage.fletcher.arrow.pb.h`).
- **IPC schema** (`coverage.<Message>.ipc`) — opened at runtime and parsed back
  as an Arrow schema.
- **RBA C++ accessor** (`coverage.fletcher.accessor.pb.h`) — read-only guard.
- **TypeScript** (`coverage.fletcher.ts`) and **Rust accessor**
  (`coverage.fletcher.rs`) — compile-checked when `tsc` / `cargo`+`rustc` are
  available, **Skipped otherwise** (never Failed/Passed).

This is a companion to `integration-tests/protoc-arrow-bridge` (which proves
targeted byte compatibility across many small fixtures). GIR-1 is **not** the
byte-identity oracle — `Encode()==EncodeRow()`, decode golden bytes and IPC
byte parity belong to GIR-2/GIR-5.

## Tests

| CTest name                                              | What it guards                         |
|---------------------------------------------------------|----------------------------------------|
| `CoverageHarness.GeneratedCppCompilesEncodesAndReconstructs` | **forcing test** — edge row + Arrow view + IPC |
| `CoverageHarness.GeneratedAccessorCppCompilesAndReads`  | generated RBA C++ accessor (read-only) |
| `CoverageHarness.GeneratedTypescriptCompiles`           | `tsc --noEmit` (skips if no `tsc`)     |
| `CoverageHarness.GeneratedRustAccessorCompiles`         | `cargo check` (skips if no `cargo`)    |

## Run it

Components must be in the local Conan cache first
(`conan create core/. ...` then `arrow-bridge`, `pubsub`, `protoc`).

**Windows (VS multi-config):**

```powershell
cd integration-tests/protoc-coverage
conan install . --build=missing -pr:a=../../.conan-profiles/Windows-msvc194-x86_64-Release
cmake --preset conan-default
cmake --build --preset conan-release
ctest --preset conan-release --output-on-failure
```

**Linux (single-config):**

```bash
cd integration-tests/protoc-coverage
conan install . --build=missing -pr:a=../../.conan-profiles/Linux-gcc13-x86_64-Release
cmake --preset conan-release
cmake --build --preset conan-release
ctest --preset conan-release --output-on-failure
```

Or, generator-agnostic on both platforms: `conan build .`.

## `coverage_future.proto`

`proto/coverage_future.proto` is **committed but unwired**. It parks the
scalar-leaf flatten family (`ScalarListWrapper` and its nestings) that the
current generator cannot emit faithfully as compilable output on every surface
(non-compiling C++ for the depth-3 wrapper; a silent depth-2 collapse; a TS
syntax error for the singular scalar wrapper). GIR-10 (which owns
`List<List<scalar>>`) will move those shapes back into `coverage.proto` and wire
them. The struct-leaf equivalents (`StructListWrapper` and its nestings)
generate cleanly and stay wired today.
