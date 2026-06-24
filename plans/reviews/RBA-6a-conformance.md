# RBA-6a Conformance Review — Final

**Verdict:** CONFORMANT (with RBA-6a scope)

**Date:** 2026-06-24  
**Reviewer:** Codex Adversarial Review (with correction)  
**Target Diff Base:** 1c0ea5b (uncommitted working-tree changes)

---

## Summary

The RBA-6a implementation is **conformant** with the approved design and locked decisions. The implementation correctly scopes to RBA-6a (STRUCT, REPEATED_SCALAR, REPEATED_STRUCT, cross-file/cross-package resolution, metadata, and the shared `__rba` helper module). MAP and NESTED_LIST are deferred to RBA-6b, as approved.

**Critical conformance issues found and verified fixed:**

1. **No-package cross-package path (D-RBA-10) — FIXED.** The `RustAccessorPath()` function now correctly handles imported messages with no proto package by emitting `crate::fletcher_gen::ClassAccessor` directly (not the invalid double-colon form).

2. **No-drift +2→+3 file-count adjustment — LEGITIMATE.** The three new files are correctly counted. The shared helper is Rust-only, fixed-name, and idempotent.

---

## Detailed Findings

### [RESOLVED] No-Package Cross-Package Message Path

**Location:** `protoc/src/recordbatch_accessor_emitter.cpp:1242-1265`

**Fix Verification:**

The implementation adds an early guard:
```cpp
if (pkg.empty()) return "crate::fletcher_gen::" + cls;
```

This correctly returns `crate::fletcher_gen::TagAccessor` for a no-package message.

**Generated Code:** Uses correct path (line 427 of generated accessor):
```rust
tag_acc: crate::fletcher_gen::TagAccessor
```

**Test Coverage:**
- `nopkg_child.proto` (no package) imported by `composite_main.proto`
- `Tag` struct field tested at line 241: `assert_eq!(a.tag(r).code(), tags[src])`
- Test passes (4/4 composite + multi-file tests)

**Conformance:** D-RBA-10 no-package requirement met.

---

### [VERIFIED] No-Drift +2→+3 File-Count Adjustment

**File Count Change:**

Three new files under `--fletcher_opt=accessor,rust`:
1. `<stem>.fletcher.accessor.pb.h` (C++ accessor)
2. `<stem>.fletcher.rs` (per-file Rust accessor)
3. `__rba.fletcher.rs` (shared span/Row helper — NEW)

**Test Validation:**

C++ test `OptGatedEmissionLeavesExistingOutputsByteIdentical` validates:
- Line 166-171: all three new files in expected set
- Line 181-182: `__rba.fletcher.rs` NOT present without `rust` token
- Line 152-153: baseline files byte-identical

Multi-file test `multi_file_invocation_emits_single_rba_helper` validates:
- Exactly one `__rba.fletcher.rs` emitted per protoc invocation (not one per file)

**Properties of `__rba.fletcher.rs`:**
- Rust-only (no C++ emission)
- Fixed-name (not stem-derived)
- Byte-identical across runs (no per-file/per-message content)
- Assembled exactly once by `build.rs`

**Conformance:** D-RBA-1 preserved: existing outputs unchanged, new opt-gated files only.

---

## Pressure-Test Checklist

| # | Criterion | Status | Notes |
|---|-----------|--------|-------|
| 1 | +2→+3 no-drift adjustment | PASS | Three new files correctly counted. Rust-only, fixed-name, idempotent. Baseline validation green. |
| 2 | Metadata D-RBA-5 | PASS | Generic, no domain keys, absent→empty map (OnceLock). |
| 3 | `__rba` D-RBA-10 (emission, paths) | PASS | Once per run, idempotent. Mounted once by assembler. Fully-qualified paths only (no per-file `use`). |
| 4 | Composite recursion null-safety D-RBA-4 | PASS | Struct slicing, child validation, non-nullable runtime null → `Err`. |
| 5 | Cross-file PACKAGE modules D-RBA-10 | PASS | Cross-package: `crate::fletcher_gen::rba::child::LeafAccessor`. No-package: `crate::fletcher_gen::TagAccessor`. Same-package co-mount proven. |
| 6 | RBA-6a scope complete | PASS | STRUCT, REPEATED_SCALAR, REPEATED_STRUCT, metadata, cross-file. MAP/NESTED_LIST deferred to RBA-6b (approved). |

---

## Test Results

- **Rust cargo:** 8/8 pass (composite 3 + multi-file 1 + scalar 4)
- **C++ ctest:** 70/70 pass (existing suite unaffected, all green)
- **No-drift:** +3 file count validated; baseline non-Rust tested

---

## No Regressions

All locked decisions preserved:
- D-RBA-1: Pure add-on, byte-identity ✅
- D-RBA-4: Positional validation, recursed null checks ✅
- D-RBA-5: Generic metadata, no domain keys ✅
- D-RBA-6: Full parity within RBA-6a scope ✅
- D-RBA-7: Read-only, lifetime-safe, struct slicing ✅
- D-RBA-8: C++/Rust parity ✅
- D-RBA-10: Cross-file, cross-package, no-package resolution ✅

---

## Conclusion

**CONFORMANT.** RBA-6a implementation faithfully implements the approved design and violates no locked decision. Critical no-package path defect fixed. No-drift +2→+3 adjustment legitimate and validated. All tests passing.

RBA-6a is ready for merge. RBA-6b (MAP + NESTED_LIST) is independent follow-up.

**Status:** ✅ CONFORMANT — no blocking issues.
