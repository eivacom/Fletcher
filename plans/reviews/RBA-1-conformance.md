# RBA-1 Conformance Review

**Date:** 2026-06-24  
**Reviewer:** Codex (adversarial review)  
**Target:** branch diff against 9adf8b8dd3ae4fd02ca0779e3e71b930eca1cb74  
**Status:** CONFORMANT ✓

## Executive Summary

The RBA-1 implementation conforms to the approved design and all locked decisions. All conformance checks pass, including the Rust well-formedness requirement that was initially flagged and subsequently corrected by the implementer.

## Conformance Verification

### D-RBA-1 (Pure add-on): ✅ PASS

- No existing emitter functions edited.
- Option parsing added cleanly in `generator.cpp` (booleans `emit_accessor` / `emit_rust`) without changing existing boolean flow.
- New output blocks are genuinely new: `context->Open` calls outside all existing guards, unconditional on message content.
- Existing outputs (`.fletcher.pb.h`, `.fletcher.arrow.pb.h`, `.fletcher.ts`, `.ipc`) remain byte-identical per test assertion across all six baseline option combinations.
- Two-level no-drift enforcement: within-build forcing test (`OptGatedEmissionLeavesExistingOutputsByteIdentical`) proves "with flags == without flags"; existing per-fixture integration suite (`test_telemetry.cpp` / `test_nested.cpp` / `test_arrow_view.cpp` / `test_ipc_parity.cpp`) guards before/after-feature invariant.

### D-RBA-2 (Opt names accessor/rust, orthogonal gates): ✅ PASS

- `emit_accessor` and `emit_rust` booleans correctly parsed and gated independently.
- Both files emit **unconditionally** when their token is present (no `if (has_messages)` guard on the `context->Open` blocks).
- Degenerate fixture (`empty_accessor.proto` — recursive-only, no view/IPC output) correctly validates the "+2 always" contract: both files emitted even when the proto has no Arrow-mappable messages.
- Tokens are orthogonal to `schema_only`, `ts`, `ipc`: `schema_only,accessor` correctly emits the accessor header.

### D-RBA-10 (Rust: bare items, no self-`mod` wrapper): ✅ PASS

- Generated `.rs` emits only generated-file comments and a module banner (lines 52–62 in `recordbatch_accessor_emitter.cpp`).
- No `mod <package>` wrapper of its own; the RBA-5 assembler owns package mounting via build.rs.
- Comments correctly document that package mounting is deferred to RBA-5.

### Rust well-formedness check (Design Step-2, item-3): ✅ PASS (FIXED)

**Original issue:** Test only asserted non-empty file (lines 154–160 in initial read).

**Corrected implementation** (lines 212–224, 258–286):
1. **When `rustc` is available (line 249 probes once):**
   - `CheckNoDriftForCase()` runs `RustcCheck()` on each generated `.rs` (lines 219–223).
   - `RustcCheck()` invokes `rustc --crate-type lib --edition 2021 --emit metadata` with explicit `--crate-name` to work around dotted-filename crate-name invalidity.
   - Assert parse success (Rust well-formedness required, counted).

2. **When `rustc` is absent:**
   - Dedicated test `GeneratedRustFileParsesWithRustc` calls `GTEST_SKIP()` (lines 263–268) with explicit, logged message:
     > "rustc not available on this machine; Rust well-formedness parse is deferred to the RBA-5 Rust crate (pinned toolchain)."
   - Skip is **visible and counted** in ctest output — maintainer sees the decision, no silent downgrade.
   - Inline no-drift test still asserts `.rs` is non-empty (line 218 — minimal guard, not the sole check).

**Verdict:** Conformant. The design's mandatory choice (rustc-or-GTEST_SKIP, no silent pass) is fully implemented. The skeleton (comment-only) correctly parses with rustc (on this box 1.96.0: all 64 test cases pass).

### File coverage and scope: ✅ PASS

- Two new emitter files: `recordbatch_accessor_emitter.hpp` and `recordbatch_accessor_emitter.cpp`.
- Minimal C++ header: generated banner, `#pragma once`, empty `fletcher_gen::<package>` namespace — no accessor classes or field iteration.
- Minimal Rust skeleton: generated banner that parses as valid Rust (no package wrapper modules per D-RBA-10).
- No scope creep into RBA-2+ (real accessor content). Implementation respects the boundary: RBA-2+ owns content, RBA-1 owns plumbing + minimal skeleton.

### CMakeLists integration and proto roots: ✅ PASS

- New source file correctly added to `fletcher_plugin_core` target in `protoc/CMakeLists.txt`.
- Test correctly integrated into `protoc-arrow-bridge` harness.
- All three proto search roots (`PROTO_DIR`, `FLETCHER_PROTO_INCLUDE_DIR`, `PROTOBUF_WKT_INCLUDE_DIR`) are wired as CMake compile definitions and passed to runtime protoc invocation (lines 82–100).
- `empty_accessor.proto` correctly imports and resolves `fletcher/options.proto`.

### Test matrix completeness: ✅ PASS

- Six baseline option combinations: `{}`, `ts`, `ipc`, `schema_only`, `ts,ipc`, `schema_only,ts,ipc`.
- Two fixture classes: typical (`nested.proto` — multiple messages, view, IPC) and degenerate (`empty_accessor.proto` — no Arrow-mappable output).
- Total: 12 no-drift cases per test run, plus dedicated Rust parse test (13 test cases).
- All 64 assertions pass on this box (rustc 1.96.0 present; dedicated test runs, not skipped).

## Conclusion

The RBA-1 implementation correctly implements the option plumbing, additive emission, and minimal skeleton emitters. The Rust well-formedness check now conforms to the design: it runs `rustc` when available (required, counted), visibly defers to RBA-5 when absent (GTEST_SKIP with logged reason), and never silently downgrades to "file-exists-only".

**Status: READY FOR MERGE** ✓
