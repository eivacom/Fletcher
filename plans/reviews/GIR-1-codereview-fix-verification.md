# GIR-1 Code Review: Fix Verification (Targeted Re-Review)
**Date**: 2026-07-10
**Scope**: Verification of fixes for 4 BLOCKING findings from GIR-1 original review
**Verification Mode**: Scoped re-review of only the blockers; no full harness re-review, no test re-run

---

## FINDING 1: Graceful-Skip Mechanism Can Mask Missing Dependencies
**File**: `integration-tests/protoc-coverage/cmake/run_tsc_check.cmake`
**Status**: ✅ RESOLVED

### Original Issue
File operations (`file(MAKE_DIRECTORY)` and `file(COPY)`) on lines 28-29 lacked error handling. If copy failed (permissions, disk full), test would proceed with missing generated TS file, producing confusing downstream tsc errors instead of graceful skip.

### Fix Applied
- **Lines 33–38**: Added post-condition check after `file(MAKE_DIRECTORY)`: `if(NOT IS_DIRECTORY "${TS_DIR}/generated")` → `FATAL_ERROR` with clear message about filesystem/permission fault.
- **Lines 39–46**: Added post-condition check after `file(COPY)`: `if(NOT EXISTS "${TS_DIR}/generated/${_gen_ts_name}")` → `FATAL_ERROR` about broken/partial copy.
- **Lines 15–18**: Graceful-skip (SKIP_MARKER) is now the ONLY path when tsc is absent; all other problems are hard failures.

### Verification
✅ A genuinely broken toolchain or failed copy will now FAIL immediately with actionable error, not be masked as Skipped.
✅ Absent tsc remains the only graceful skip path.
✅ No new blocking issues introduced.

---

## FINDING 2: .ipc Output List Manually Maintained; Incomplete Generation Undetected
**File**: `integration-tests/protoc-coverage/CMakeLists.txt` + NEW `integration-tests/protoc-coverage/cmake/validate_generated_ipc.cmake`
**Status**: ✅ RESOLVED

### Original Issue
Hardcoded `GENERATED_IPC` list (prior lines 76–80) was not auto-discovered. If a new top-level message was added to coverage.proto, the manual list had to be updated or CMake would silently not know generation was incomplete. No build-time validation existed.

### Fixes Applied

#### 1. **CMakeLists.txt** — Single-Source-of-Truth List
- **Lines 101–119**: Introduced `_ipc_all_messages` as the authoritative list of every message for which protoc-gen-fletcher emits an .ipc schema stream. Includes detailed comment (lines 102–109) explaining why flatten *list* wrappers (StructListWrapper, NestedStructListWrapper) are inlined and emit no standalone .ipc, but normal messages and message-level flatten structs do.
- **Lines 124–128**: Separate `_ipc_opened_messages` list for the 4 messages the runtime C++ test actually reads (ScalarCoverage, CompositeCoverage, ServiceRequest, ServiceReply).
- **Lines 130–133**: `GENERATED_IPC` now built from `_ipc_opened_messages` (subset), not hardcoded.
- **Lines 135–141**: Pass `_expected_ipc_arg` (pipe-joined list of all messages' basenames) to post-generation guard.

#### 2. **validate_generated_ipc.cmake** (NEW)
- **Lines 26–31**: Glob actual .ipc files from `GENERATED_DIR`, extract basenames into `_actual` list.
- **Lines 33–35**: Normalize both lists (remove duplicates, sort).
- **Lines 37–55**: Compare `_expected` vs `_actual`:
  - If not equal, compute `_missing` (expected but not emitted) and `_unexpected` (emitted but not expected).
  - Emit `FATAL_ERROR` with full diagnostics: expected list, actual list, missing, and unexpected.
  - Actionable message points to updating `_ipc_all_messages` and/or `_ipc_opened_messages` in CMakeLists.txt.

#### 3. **CMakeLists.txt** — Integration
- **Lines 156–162**: Post-generation `COMMAND` in custom command invokes `cmake ... validate_generated_ipc.cmake`. Guard runs immediately after protoc, before tests; non-zero exit fails the custom command and build.

### Verification
✅ A new/removed/renamed top-level message WILL be caught (both under- and over-generation).
✅ Error message is explicit: lists expected vs actual, shows missing and unexpected .ipc files, and actionable (points to which lists to update).
✅ The expected list (not a glob) is authoritative, so under-generation cannot be silently accepted.
✅ No new blocking issues introduced.

---

## FINDING 3: Windows CI Job Uses Relative Paths for Conan Profile
**File**: `.github/workflows/ci.integration-test.protoc-coverage.yml`
**Status**: ✅ RESOLVED

### Original Issue
Windows job (lines 79–93) used relative paths like `../../.conan-profiles/Windows-msvc194-x86_64-Release`. If `conan create` changed the working directory unexpectedly, the profile path would become invalid.

### Fix Applied
- **Line 86**: Define `PROFILE` variable with absolute path: `PROFILE="${{ github.workspace }}/.conan-profiles/Windows-msvc194-x86_64-Release"`.
- **Line 89**: Use `"$PROFILE"` in all `conan create` invocations for components.
- **Line 96**: Use absolute path directly in conan build step: `${{ github.workspace }}/.conan-profiles/Windows-msvc194-x86_64-Release`.

### Verification
✅ Windows Conan-profile paths are now absolute via `${{ github.workspace }}`, immune to cwd changes.
✅ Paths are well-formed and consistent.
✅ No new blocking issues introduced.

---

## FINDING 4: Protobuf WKT Include Directory Resolution Not Validated
**File**: `integration-tests/protoc-coverage/CMakeLists.txt`
**Status**: ✅ RESOLVED

### Original Issue
Script resolved `PROTOBUF_WKT_INCLUDE_DIR` (lines 28–48) but never validated it existed. A broken symlink or non-standard protobuf package layout could produce valid CMake configuration but cryptic protoc "File not found" errors during generation.

### Fix Applied
- **Lines 51–63**: Added comprehensive validation block after resolution:
  - **Line 57**: `if(NOT IS_DIRECTORY "${PROTOBUF_WKT_INCLUDE_DIR}")` → `FATAL_ERROR` with explanation that the package layout may differ or symlink is broken.
  - **Lines 64–72**: Loop through each required WKT proto (timestamp, duration, wrappers, empty) and `if(NOT EXISTS ...)` → `FATAL_ERROR` with actionable message about install breakage or empty symlink.

### Verification
✅ Resolved WKT include-dir is now validated to exist as a directory.
✅ Each required WKT proto file is checked individually (Timestamp, Duration, Wrappers, Empty).
✅ Error messages distinguish between: path doesn't exist, path is not a directory, or specific proto is missing.
✅ Catches broken symlinks, non-standard layouts, and empty symlinks before protoc is invoked.
✅ No new blocking issues introduced.

---

## NEW BLOCKING ISSUES
**Status**: ✅ NONE DETECTED

All four fixes are narrowly scoped to their respective findings. No new blocking issues introduced:
- Error handling in run_tsc_check.cmake is conservative and clear.
- The .ipc guard is well-integrated into the custom command and has comprehensive diagnostics.
- Absolute paths in CI are well-formed and idiomatic for GitHub Actions.
- WKT validation is thorough and fails with clear messages.

---

## MINOR OBSERVATIONS (Nits)
1. **validate_generated_ipc.cmake**: Error message (line 54) mentions updating both `_ipc_all_messages` and (conditionally) `_ipc_opened_messages`, but could be slightly clearer that `_ipc_opened_messages` is a *subset* for the runtime test. Not blocking; documentation or comments in CMakeLists.txt suffice.

---

## SUMMARY

| Finding | Severity | Status |
|---------|----------|--------|
| 1. Missing error checking on file operations (tsc) | BLOCKING | ✅ RESOLVED |
| 2. .ipc output list manually maintained | BLOCKING | ✅ RESOLVED |
| 3. Windows CI relative paths for Conan | BLOCKING | ✅ RESOLVED |
| 4. WKT include-dir not validated | BLOCKING | ✅ RESOLVED |

**No new blocking issues introduced.**

**Fixes verified as complete and correct.**
