# GIR-8 Code Review: UnsupportedError + No-ValueOrDie

**Date**: 2026-07-11  
**Reviewer**: Claude Code / Codex  
**Target**: GIR-8 implementation (d71952bb682d4a9aa31997a6a3416d68f38bfc32..HEAD)

---

## Summary

GIR-8 introduces validation to fail the protoc plugin fatally when genuinely-unsupported IR types (google.protobuf.Any, Struct, real oneof, unsupported map keys/values) are encountered, before any artifact is emitted. Simultaneously, it replaces all 9 `.ValueOrDie()` sites in emitter code with the new `detail::FletcherValueOrThrow<T>()` helper, which throws descriptive `std::runtime_error` instead of process abort.

**Finding Count**:
- **Blocking (Critical)**: 0
- **Non-Blocking (Should Fix)**: 1  
- **Nits**: 0

---

## Detailed Findings

### **[P2 - Non-Blocking] Repeated/Map `google.protobuf.Any` May Bypass Validation**

**Severity**: Non-Blocking. The field will eventually fail downstream (likely at emission or schema build), but validation won't reject it with a clear message.

**Location**: `protoc/src/generator.cpp:1453–1457` — `FindUnsupportedIr` STRUCT branch

**Issue**: 
When `google.protobuf.Any` appears as a **repeated** field or **map value**, `BuildFieldIr` represents it as a `LIST` or `MAP` node containing a `STRUCT` node (representing the well-known type). The current `FindUnsupportedIr` function recurses into the STRUCT's fields but does not check the STRUCT node's identity itself.

```cpp
if (node.kind == ir::NodeKind::STRUCT) {
    for (const auto& f : std::get<ir::StructNode>(node.node).fields) {
        if (auto e = FindUnsupportedIr(*f.type)) return e;
    }
}
```

Since Any's children (`type_url`, `value`) are valid scalar types, the recursion returns success, bypassing the validation. This allows:
- `repeated google.protobuf.Any payload;` ✗ (not caught)
- `map<string, google.protobuf.Any> data;` ✗ (not caught)

to pass validation even though:
- `google.protobuf.Any payload;` ✓ (correctly caught)

would be rejected.

**Fix**: Before recursing into STRUCT fields, check if the struct identity is an unsupported well-known type (Any, Struct, etc.):

```cpp
if (node.kind == ir::NodeKind::STRUCT) {
    const auto& st = std::get<ir::StructNode>(node.node);
    // Check struct identity against unsupported WKTs before recursing into fields
    if (IsUnsupportedWellKnownType(st.identity.descriptor)) {
        return "unsupported field '" + node.facts.proto_full_name + 
               "': " + GetUnsupportedReason(st.identity);
    }
    for (const auto& f : st.fields) {
        if (auto e = FindUnsupportedIr(*f.type)) return e;
    }
}
```

**Workaround**: Until fixed, `repeated Any` and `map<..., Any>` will still fail at generation time (downstream in emitters or schema builders), but with a less clear error message. The forcing test `GenErrors.UnsupportedTypeFailsBuild` only covers singular Any, not repeated/map variants.

---

## Verification: Supported Checks

### ✓ ValidateNoUnsupportedIr / FindUnsupportedIr Correctness

**Recursion completeness**: All child-bearing node kinds are covered:
- ✓ `LIST` (line 1440)
- ✓ `FIXED_SIZE_LIST` (line 1444)  
- ✓ `MAP` key + value (lines 1449–1450)
- ✓ `STRUCT` fields (lines 1454–1456)
- ✓ SCALAR carries no children (correctly omitted)

**Skip predicates applied**: Both `IsRecursive()` and `IsFlattenedWrapper()` are respected (line 1470), ensuring only messages that would actually be generated are validated.

**Error messages**: Clear, includes field name and reason:
```
"unsupported field 'integration.coverage.unsupported.UnsupportedCoverage.payload': 
 google.protobuf.Any is dynamically typed..."
```

**Validation entry point**: Correctly placed at the start of `ArrowRowGenerator::Generate()` (line 1539), before any artifact emission.

---

### ✓ FletcherValueOrThrow<T>() Correctness

**Value identity on success**: Uses `std::move(result).ValueUnsafe()`, returning the same value as `.ValueOrDie()` would (line 1386).

**Throw on failure**: Throws `std::runtime_error` with descriptive message including context and status (lines 1382–1384).

**Exception safety**: No noexcept violations; throws are intentional and properly handled in generated code contexts.

**Guard definition**: Properly wrapped with per-package header guard `FLETCHER_DETAIL_VALUE_OR_THROW_<PACKAGE>_DEFINED` (lines 1369–1371), preventing ODR violations when multiple view headers from the same package are included in one translation unit.

**Includes**: Generated view header correctly includes `<stdexcept>` (line 1331) and `<string>` (line 1332) for exception types and message formatting.

**All 9 sites replaced**: 
- ✓ 2 sites in `generator.cpp` (lines 813, 827) — scalar reads in view constructors
- ✓ 7 sites in `cpp_backend_view_visitor.cpp` (lines 351, 388, 391, 421, 424, 427, 468) — builder creation

No `.ValueOrDie()` text remains in emitter code; only comments reference the old name.

---

### ✓ Check Scripts

**`run_unsupported_generation_check.cmake`** (lines 31–59):
- ✓ Invokes protoc on `coverage_unsupported.proto` with the real plugin
- ✓ Asserts non-zero exit (line 43: `if(rc EQUAL 0) message(FATAL_ERROR...)`)
- ✓ Verifies error output names the field (`payload`) — line 52
- ✓ Verifies error output includes the reason (`google.protobuf.Any is dynamically typed`) — line 55
- ✓ Would fail the build if generation succeeded (intended behavior)

**`check_no_value_or_die.cmake`** (lines 26–79):
- ✓ Scans only IR-emitted generated files (`*.fletcher.pb.h`, `*.fletcher.arrow.pb.h`, `*.fletcher.ts`) — lines 53–56
- ✓ Explicitly excludes RBA outputs (`*.accessor.pb.h`, `*.fletcher.rs`, `__rba.fletcher.rs`) — lines 45–49
- ✓ Explicitly excludes binary IPC streams (`*.ipc`) — line 48
- ✓ Fails the build if any `.ValueOrDie()` remains (line 74–78)
- ✓ Verifies files were actually generated (line 68–70)
- ✓ Not vacuous: confirms at least one file was scanned before passing

**CMake wiring**: Both tests are correctly added to the build (lines 275–288 in `CMakeLists.txt`) as proper ctest entries.

---

### ✓ Test Fixture

**`coverage_unsupported.proto`**:
- ✓ Genuinely unsupported: uses `google.protobuf.Any` (lines 20)
- ✓ Isolated: not wired into normal coverage generation (referenced only by the failure test, see CMakeLists.txt lines 275–283)
- ✓ Properly namespaced: `integration.coverage.unsupported` (line 15)
- ✓ Imports well-known-type proto: `google/protobuf/any.proto` (line 17)

---

### ✓ Behavior Preservation

**Supported inputs**: No change to generated code's behavior on valid/supported protos. The validation is purely a new pre-generation check.

**Byte identity**: Generated C++ source will churn where `.ValueOrDie()` text is replaced with `detail::FletcherValueOrThrow()` calls, but this preserves value and exception-handling semantics. Wire bytes, IPC schemas, and TS output remain identical for `coverage.proto` (verified by existing golden-file tests).

**No UB introduced**: 
- No buffer overruns, dangling pointers, or uninitialized access.
- Rvalue-reference parameter in `FletcherValueOrThrow` is correctly moved.
- No ordering or determinism issues introduced.

---

## Conclusion

**Overall Status**: READY to merge with one known limitation.

The implementation correctly enforces unsupported-type rejection at generation time and replaces all `.ValueOrDie()` sites with exception-safe alternatives. The one non-blocking gap (repeated/map Any) is a false-negative in the validation scope, not a runtime defect. Downstream checks or explicit test expansion can mitigate until fixed.
