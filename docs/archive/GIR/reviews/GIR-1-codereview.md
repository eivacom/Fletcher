# GIR-1 Code Review: Compile-and-Run Harness
**Date**: 2026-07-10
**Scope**: Static analysis of uncommitted working-tree changes against base commit aa207dd
**Files Reviewed**:
- `.github/workflows/ci.pr.yml` (diff changes)
- `.github/workflows/ci.integration-test.protoc-coverage.yml` (new file)
- `integration-tests/protoc-coverage/CMakeLists.txt`
- `integration-tests/protoc-coverage/conanfile.py`
- `integration-tests/protoc-coverage/proto/coverage.proto`
- `integration-tests/protoc-coverage/proto/coverage_future.proto`
- `integration-tests/protoc-coverage/tests/test_coverage_harness.cpp`
- `integration-tests/protoc-coverage/tests/test_coverage_accessor.cpp`
- `integration-tests/protoc-coverage/tests/coverage_fixture.hpp`
- `integration-tests/protoc-coverage/cmake/run_tsc_check.cmake`
- `integration-tests/protoc-coverage/cmake/run_cargo_check.cmake`

---

## BLOCKING ISSUES

### 1. Graceful-Skip Mechanism Can Mask Missing Dependencies
**File**: `cmake/run_tsc_check.cmake`
**Severity**: BLOCKING

The graceful-skip logic prints SKIP_MARKER when tsc is absent. However, `file(COPY ...)` on lines 28-29 lacks error handling:

```cmake
file(MAKE_DIRECTORY "${TS_DIR}/generated")
file(COPY "${_gen_ts}" DESTINATION "${TS_DIR}/generated")
```

If the copy fails (permissions, disk full), the test proceeds with a missing generated TS file and tsc fails confusingly instead of being marked Skipped.

**Risk**: Broken toolchain or filesystem issues silently appear as Failed instead of Skipped.

**Fix**: Add error checking after file operations.

---

### 2. .ipc Output List Manually Maintained; Incomplete Generation Undetected
**File**: `CMakeLists.txt` lines 76-80
**Severity**: BLOCKING

The hardcoded `.ipc` output list is not auto-discovered:

```cmake
set(GENERATED_IPC
    "${GENERATED_DIR}/${_stem}.ScalarCoverage.ipc"
    "${GENERATED_DIR}/${_stem}.CompositeCoverage.ipc"
    "${GENERATED_DIR}/${_stem}.ServiceRequest.ipc"
    "${GENERATED_DIR}/${_stem}.ServiceReply.ipc")
```

If a new top-level message is added to coverage.proto, this list must be manually updated or CMake will not know generation is incomplete. No build-time validation exists.

**Risk**: Silent truncation of generated outputs; incomplete harness coverage.

**Fix**: Either use `file(GLOB ...)` or document the list must be kept in sync with coverage.proto.

---

### 3. Windows CI Job Uses Relative Paths for Conan Profile
**File**: `.github/workflows/ci.integration-test.protoc-coverage.yml` lines 79-93
**Severity**: BLOCKING

Windows job assumes working directory is repo root but uses relative paths:

```bash
(cd "$COMPONENT" && conan create . --build=missing -pr:a="../$PROFILE")
cd integration-tests/protoc-coverage && conan build . --build=missing -pr:a=../../.conan-profiles/Windows-msvc194-x86_64-Release
```

If `conan create` changes the working directory unexpectedly, the profile path becomes invalid.

**Risk**: Silent profile lookup failure; build uses wrong or default profile; test passes but under wrong conditions.

**Fix**: Use absolute paths: `${{ github.workspace }}/.conan-profiles/Windows-msvc194-x86_64-Release`

---

### 4. Protobuf WKT Include Directory Resolution Not Validated
**File**: `CMakeLists.txt` lines 28-48
**Severity**: BLOCKING

The script resolves the protobuf WKT include path but never validates it exists:

```cmake
set(PROTOBUF_WKT_INCLUDE_DIR "${_protoc_root}/include")
# ... no check if this path is real ...
```

A broken symlink or platform-specific issue could cause a valid CMake configuration but a failing protoc invocation later.

**Risk**: Cryptic protoc errors on non-standard protobuf installations.

**Fix**: Add `if(NOT EXISTS "${PROTOBUF_WKT_INCLUDE_DIR}")` after resolution.

---

## NON-BLOCKING ISSUES

### 5. test_coverage_harness.cpp Uses Static Variable for Memory Lifetime
**File**: `tests/test_coverage_harness.cpp` lines 47-57
**Severity**: NON-BLOCKING

The RoundTrip helper uses a static `EncodedRow` to keep the buffer alive for the Codec:

```cpp
static EncodedRow kept_alive;
kept_alive = std::move(encoded);
```

Not thread-safe, but GTest runs tests sequentially by default. Low risk.

---

### 6. Cargo --locked Fragility Acknowledged
**File**: `cmake/run_cargo_check.cmake` line 43
**Severity**: NON-BLOCKING

Uses `cargo check --locked`, which can fail if Cargo.lock falls out of sync with available crates. Comment (lines 20-23) acknowledges this is a known risk and can be relaxed if needed.

---

### 7. coverage_future.proto Not Wired But No Build-Time Safeguard
**File**: `CMakeLists.txt` and `proto/coverage_future.proto`
**Severity**: NON-BLOCKING

The future coverage shapes are committed but deliberately unwired. No mechanism prevents accidental inclusion in the CMake generation command.

**Fix**: Add a CMake comment or variable to explicitly control future coverage inclusion.

---

## NITS

### 8. Missing README for Integration Test
**File**: `integration-tests/protoc-coverage/`
**Severity**: NIT

No README explaining harness purpose, local usage, or relationship to coverage_future.proto.

---

### 9. Hardcoded Generated File Naming Convention
**File**: `CMakeLists.txt` lines 65-80
**Severity**: NIT

Generated file names hardcoded without documented convention. If generator output naming changes, these will silently be wrong.

**Fix**: Add comment documenting expected naming scheme.

---

## SUMMARY

| Severity | Count |
|----------|-------|
| BLOCKING | 4 |
| NON-BLOCKING | 3 |
| NIT | 2 |

### BLOCKING (must fix):
1. `cmake/run_tsc_check.cmake`: Missing error checking on file operations.
2. `CMakeLists.txt`: .ipc output list manually maintained; new messages silently truncate generation.
3. `.github/workflows/ci.integration-test.protoc-coverage.yml`: Windows uses relative paths for profile.
4. `CMakeLists.txt`: Protobuf WKT path not validated.

### Key Observations:
- Harness logic is sound: encode->decode->Arrow round-trip->IPC readability tests are well-structured.
- Graceful skip mechanism is correctly implemented (SKIP_REGULAR_EXPRESSION usage is right), but file operation errors can mask issues.
- Fixture correctness: coverage.proto split is well-documented; ServiceRequest.row->payload rename is necessary and justified.
- CI wiring: paths, sparse-checkout, and reusable-workflow inputs look correct EXCEPT Windows profile paths.
- No major resource leaks or portability issues detected.

### Test Assertions Quality:
- test_coverage_harness.cpp covers encode/decode, Arrow view, IPC schema readability. Assertions check value equality post-roundtrip. Not vacuous.
- test_coverage_accessor.cpp builds Arrow batch, reads via accessor, covers scalars/optional/nested/maps/lists. Assertions valid.
- Fixture (coverage_fixture.hpp) provides consistent row across both tests; empty + populated containers tested.

