# GIR-8: UnsupportedError + No-ValueOrDie

## Summary

GIR-8 makes IR `Unsupported{reason}` nodes fatal at protoc generation time, before any artifact is written. Today `GatherFieldsImpl()` builds IR with `ir::BuildFieldIr(fd)`, projects it through `ProjectIrToFieldMapping()`, and silently skips `nullopt`. That hides `NodeKind::UNSUPPORTED` alongside non-flat-bridge-representable-but-not-fatal shapes.

The `.ValueOrDie()` cleanup applies to generated IR-emitted Arrow/view code, not RBA. The emitter code contains exactly 9 `.ValueOrDie()` sites: 7 `arrow::MakeBuilder(...).ValueOrDie()` calls in `cpp_backend_view_visitor.cpp` (lines 352, 389, 392, 422, 425, 428, 469) and 2 generated view-class constructor scalar reads in `generator.cpp` (lines 812, 826). When these emitter sites are replaced, they eliminate all ~26 generated `.ValueOrDie()` occurrences in the active `coverage.proto` generated view classes by construction. RBA is read-only and excluded from the zero-grep assertion.

## Design

Add a generation-front validation pass in `protoc/src/generator.cpp`, called at the start of `ArrowRowGenerator::Generate()` before `GenerateFile()`, `GenerateViewFile()`, TS, IPC, or RBA emission.

The pass walks the file's message set and recursively inspects field IR:

```cpp
std::optional<std::string> FindUnsupportedIr(const ir::IrNode& node) {
    if (node.kind == ir::NodeKind::UNSUPPORTED) {
        const auto& u = std::get<ir::UnsupportedNode>(node.node);
        return "unsupported field '" + node.facts.proto_full_name + "': " + u.reason;
    }

    if (node.kind == ir::NodeKind::LIST) {
        return FindUnsupportedIr(*std::get<ir::ListNode>(node.node).element);
    }

    if (node.kind == ir::NodeKind::FIXED_SIZE_LIST) {
        return FindUnsupportedIr(*std::get<ir::FixedSizeListNode>(node.node).element);
    }

    if (node.kind == ir::NodeKind::MAP) {
        const auto& m = std::get<ir::MapNode>(node.node);
        if (auto e = FindUnsupportedIr(*m.key)) return e;
        return FindUnsupportedIr(*m.value);
    }

    if (node.kind == ir::NodeKind::STRUCT) {
        for (const auto& f : std::get<ir::StructNode>(node.node).fields) {
            if (auto e = FindUnsupportedIr(*f.type)) return e;
        }
    }

    return std::nullopt;
}
```

Then:

```cpp
bool ValidateNoUnsupportedIr(const google::protobuf::FileDescriptor* file,
                             std::string* error) {
    for (const auto* msg : OrderedMessages(file)) {
        // Mirror the skip predicate from GenerateFile, GenerateViewFile,
        // and IPC emission loops: skip recursive messages and flattened wrappers.
        if (IsRecursive(msg) || IsFlattenedWrapper(msg)) continue;
        
        for (int i = 0; i < msg->field_count(); ++i) {
            auto node = ir::BuildFieldIr(msg->field(i));
            if (auto e = FindUnsupportedIr(node)) {
                *error = *e;
                return false;
            }
        }
    }
    return true;
}
```

`Generate()` becomes:

```cpp
if (!ValidateNoUnsupportedIr(file, error)) return false;
```

This uses the existing protoc plugin contract: `Generate()` returns `false` and fills `std::string* error`; protoc surfaces that string on stderr. No C++-specific strings are added to IR. The IR still carries only `UnsupportedNode{reason}` as an abstract error reason.

Fatal shapes are exactly `BuildFieldIr()` cases that produce `NodeKind::UNSUPPORTED`, including `google.protobuf.Any`, `google.protobuf.Struct`, real `oneof`, unsupported map key/value cases, and unsupported flatten-wrapper leaves. Proto2 groups are NOT among these - a group field has `type() == TYPE_GROUP`, which `BuildFieldIr` routes to scalar-or-enum handling, silently mapping it to `INT32` via `PrimitiveKind(TYPE_GROUP)`. Groups do not produce `UNSUPPORTED` IR.

Non-fatal parked shapes remain non-fatal unless `BuildFieldIr()` itself returns `UNSUPPORTED`. In particular, `coverage_future.proto` scalar-leaf nested-list shapes must not become this fixture: they are parked because the current flat projection or emitters are incomplete, not because every such IR node is a canonical unsupported protoArrow mapping.

For `.ValueOrDie()`, emit one checked helper in generated `.fletcher.arrow.pb.h` view headers under the existing `detail` namespace:

```cpp
template <typename T>
T FletcherValueOrThrow(arrow::Result<T>&& result, const char* context) {
    if (!result.ok()) {
        throw std::runtime_error(
            std::string(context) + ": " + result.status().ToString());
    }
    return std::move(result).ValueUnsafe();
}
```

The view header needs `<stdexcept>` and `<string>` if not already present.

Replace the 7 generated builder sites in `cpp_backend_view_visitor.cpp`:

```cpp
auto builder = arrow::MakeBuilder(type).ValueOrDie();
```

with:

```cpp
auto builder = detail::FletcherValueOrThrow(
    arrow::MakeBuilder(type), "arrow::MakeBuilder");
```

Replace the 2 generated scalar extraction sites in `generator.cpp` view constructors:

```cpp
batch.column(i)->GetScalar(row).ValueOrDie()
chunk->GetScalar(offset).ValueOrDie()
```

with:

```cpp
detail::FletcherValueOrThrow(
    batch.column(i)->GetScalar(row), "RecordBatch::GetScalar")

detail::FletcherValueOrThrow(
    chunk->GetScalar(offset), "Array::GetScalar")
```

This preserves valid-input behavior: successful `Result<T>` values produce the same builders and scalars as before. On failure, generated code throws a descriptive `std::runtime_error` instead of aborting through `.ValueOrDie()`.

Emitter decisions:

```text
MIGRATE: generator.cpp view-class constructors
MIGRATE: cpp_backend_view_visitor.cpp ToArrowRow builder creation
LEAVE: cpp_backend_schema_visitor.cpp unsupported visitor throws, but central validation prevents reaching it
LEAVE: ts_backend_visitor.cpp unsupported currently renders placeholders only if reached, but central validation prevents reaching it
LEAVE: cpp_backend_decode_visitor.cpp no ValueOrDie sites found in this scope
LEAVE: cpp_backend_type_table.cpp no ValueOrDie sites found in this scope
LEAVE: IPC in-process schema path; central validation runs before IPC generation
LEAVE: recordbatch_accessor_emitter.cpp / generated .accessor.pb.h / generated .rs; RBA is read-only and excluded
```

Supported-input byte and behavior identity is preserved. The generated C++ source goldens will churn where `.ValueOrDie()` text is replaced by helper calls, but wire bytes, decoded values, schema `.ipc`, view round-trip, and TS output must remain identical for active `coverage.proto`.

## Forcing-test mapping

Add integration negative tests in `integration-tests/protoc-coverage`, not to active coverage generation. 

Test 1: `GenErrors.UnsupportedTypeFailsBuild`

Add fixture:

```proto
// integration-tests/protoc-coverage/proto/coverage_unsupported.proto
syntax = "proto3";
package integration.coverage.unsupported;

import "google/protobuf/any.proto";

message UnsupportedCoverage {
  google.protobuf.Any payload = 1;
}
```

Wire a CMake script test, for example `cmake/run_unsupported_generation_check.cmake`, using `execute_process()` to invoke the real protoc/plugin:

```cmake
execute_process(
  COMMAND "${PROTOC}"
    "--plugin=protoc-gen-fletcher=${FLETCHER_PLUGIN}"
    "--fletcher_out=${OUT_DIR}"
    "-I" "${PROTO_DIR}"
    "-I" "${FLETCHER_PROTO_INCLUDE_DIR}"
    "-I" "${PROTOBUF_WKT_INCLUDE_DIR}"
    "${PROTO_DIR}/coverage_unsupported.proto"
  RESULT_VARIABLE rc
  OUTPUT_VARIABLE out
  ERROR_VARIABLE err)

if(rc EQUAL 0)
  message(FATAL_ERROR "unsupported fixture unexpectedly generated successfully")
endif()

string(CONCAT combined "${out}\n${err}")
if(NOT combined MATCHES "payload")
  message(FATAL_ERROR "error did not name unsupported field: ${combined}")
endif()
if(NOT combined MATCHES "google.protobuf.Any is dynamically typed")
  message(FATAL_ERROR "error did not include UnsupportedNode reason: ${combined}")
endif()
```

Add the test with explicit dependency on the plugin target:

```cmake
add_test(NAME GenErrors.UnsupportedTypeFailsBuild
  COMMAND "${CMAKE_COMMAND}"
    -DPROTOC=$<TARGET_FILE:protobuf::protoc>
    -DFLETCHER_PLUGIN=$<TARGET_FILE:fletcher-protoc::plugin>
    -DPROTO_DIR=${PROTO_DIR}
    -DOUT_DIR=${CURRENT_BINARY_DIR}/unsupported-generated
    -DFLETCHER_PROTO_INCLUDE_DIR=${FLETCHER_PROTO_INCLUDE_DIR}
    -DPROTOBUF_WKT_INCLUDE_DIR=${PROTOBUF_WKT_INCLUDE_DIR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/run_unsupported_generation_check.cmake)
```

Test 2: `GenErrors.NoValueOrDieInIrGeneratedCode`

Add a zero-`.ValueOrDie()` assertion scoped to IR-emitted generated files. The assertion must exclude RBA outputs and run over the full integration suite (all integration harnesses that generate code from IR):

```cmake
add_test(NAME GenErrors.NoValueOrDieInIrGeneratedCode
  COMMAND "${CMAKE_COMMAND}"
    -DGENERATED_DIR=${GENERATED_DIR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_no_value_or_die.cmake)
```

The script `check_no_value_or_die.cmake` checks coverage and all other integration test generated outputs, including:

```text
coverage.fletcher.pb.h
coverage.fletcher.arrow.pb.h
coverage.fletcher.ts
coverage.*.ipc is binary and skipped
[all IR-emitted files across protoc-arrow-bridge, protoc-gen-fletcher-rust, accessor-capstone, gateway, npm, cli, protoc/test_package]
```

It excludes:

```text
coverage.fletcher.accessor.pb.h
coverage.fletcher.rs
__rba.fletcher.rs
[all RBA-emitted files]
```

Red-first behavior:

```text
GenErrors.UnsupportedTypeFailsBuild fails today because coverage_unsupported.proto generates successfully and silently skips payload.
GenErrors.NoValueOrDieInIrGeneratedCode fails today because coverage.fletcher.arrow.pb.h contains generated .ValueOrDie() calls from view constructors and ToArrowRow builder creation.
```

Inner-loop commands:

```text
protoc unit rebuild: --build=fletcher-protoc/*
coverage harness rebuild: integration-tests/protoc-coverage all targets
full integration rebuild: all integration harnesses (protoc-arrow-bridge, protoc-gen-fletcher-rust, accessor-capstone, gateway, npm, cli, protoc/test_package)
ctest: GenErrors.UnsupportedTypeFailsBuild, GenErrors.NoValueOrDieInIrGeneratedCode, coverage parity/schema/view/TS tests, all integration test suites
RBA no-drift: existing accessor tests stay green, with RBA generated files excluded from the grep assertion
```

## Risks & Unknowns

The main compatibility risk is over-broad validation. The fatal condition must be `NodeKind::UNSUPPORTED`, not `ProjectIrToFieldMapping() == nullopt`, or GIR-8 will incorrectly pull parked `coverage_future.proto`-style projection gaps into the hard-error set.

Recursion handling is unchanged.

The checked helper changes generated C++ source and may require golden updates. Any change in wire bytes, decoded values, schema IPC bytes, view round-trip values, or TS output for supported inputs is out of scope and should stop the change.

Constructor error handling cannot return `arrow::Status` without an API change. Throwing a descriptive exception is the least invasive replacement for `.ValueOrDie()` in constructors. If the project wants status-returning factories instead, that is a wider API decision.

The validation traversal must be exhaustive over all node kinds with children. The fix adds `FIXED_SIZE_LIST`; any future node kind that carries nested IR must be added here.

## Files-to-touch

```text
protoc/src/generator.cpp
  Add ValidateNoUnsupportedIr() (with correct skip predicate: IsRecursive(msg) || IsFlattenedWrapper(msg))
  Add FindUnsupportedIr() with FIXED_SIZE_LIST recursion
  Call ValidateNoUnsupportedIr() at the top of ArrowRowGenerator::Generate()
  Replace generated view constructor .ValueOrDie() emissions with detail::FletcherValueOrThrow()

protoc/src/cpp_backend_view_visitor.cpp
  Replace all 7 arrow::MakeBuilder(...).ValueOrDie() emissions with detail::FletcherValueOrThrow(...)

protoc/src/cpp_backend_view_visitor.hpp or generator.cpp view-header emission
  Ensure the generated view header defines the checked helper exactly once per package/header guard
  Add #include <stdexcept> and #include <string> if not already present

integration-tests/protoc-coverage/proto/coverage_unsupported.proto
  New negative fixture with google.protobuf.Any

integration-tests/protoc-coverage/CMakeLists.txt
  Add GenErrors.UnsupportedTypeFailsBuild
  Add GenErrors.NoValueOrDieInIrGeneratedCode

integration-tests/protoc-coverage/cmake/run_unsupported_generation_check.cmake
  Invoke protoc with coverage_unsupported.proto, require nonzero return, assert field name and UnsupportedNode reason

integration-tests/protoc-coverage/cmake/check_no_value_or_die.cmake
  Grep all IR-emitted generated files across full integration suite for ValueOrDie, exclude RBA outputs

integration-tests/protoc-coverage/golden/*
  Update only generated-source goldens if the repository stores them for reviewed source churn
  Do not update wire-byte, schema IPC, decoded-value, or TS goldens unless a separate approved change explains why
```

## Step-2 re-review (2026-07-11)

**Verdict: NEEDS-REWORK - 1 blocker (internal contradiction on recursion).** The
prior traversal blocker and all three corrections are resolved; the ValueOrDie
half is good. Verified against generator.cpp / ir.cpp / type_mapper.cpp / ir.hpp.

Resolved / confirmed:
- **Blocker 2 (traversal) RESOLVED.** `FindUnsupportedIr` recurses `LIST.element`,
  `FIXED_SIZE_LIST.element` (now added), `MAP.key`+`MAP.value`, and
  `STRUCT.fields[].type`, and returns on the `UNSUPPORTED` leaf. Per `ir.hpp`
  (`NodeKind {SCALAR,LIST,FIXED_SIZE_LIST,STRUCT,MAP,UNSUPPORTED}`) those are ALL
  child-bearing kinds; `SCALAR` carries only `enum_identity`, no IR children.
  Exhaustive. (Note: `BuildFieldIr` never currently emits `FIXED_SIZE_LIST`, so
  that arm is defensive/future-proofing - fine, and matches the Risks note.)
- **Blocker 1 (skip predicate) RESOLVED.** `ValidateNoUnsupportedIr` iterates
  `OrderedMessages(file)` and skips `IsRecursive(msg) || IsFlattenedWrapper(msg)`,
  correctly mirroring the four emit loops (generator.cpp:1166, 1292, 1360,
  1483-1484).
- **Corrections folded.** GroupsINT32 confirmed: `BuildFieldIr` routes
  `TYPE_GROUP` through `BuildSingularScalarOrEnum`/`BuildRepeatedScalarOrEnum` 
  `ScalarVariantFor`  `PrimitiveKind(TYPE_GROUP)` default  `INT32`, never
  `UNSUPPORTED`. (The `UnsupportedReason` string "proto2 groups are not
  supported" in type_mapper.cpp:279 is NOT on the classification path.) Exactly 9
  emitter sites confirmed (view-visitor 352/389/392/422/425/428/469;
  generator.cpp 812/826) - the only `.ValueOrDie()` in `protoc/src`. Full-suite
  RBA-excluded grep described.
- **ValueOrDie half good.** `FletcherValueOrThrow` returns
  `std::move(result).ValueUnsafe()` on `ok()`  value-identical to `.ValueOrDie()`;
  throws otherwise. The `detail` namespace already exists in the generated
  `*.arrow.pb.h` (per-header guard); `<string>` present, `<stdexcept>` needs
  adding (doc notes this); both generator.cpp view-constructor sites emit into the
  same header, so `detail::` is in scope. RBA emitter has zero `.ValueOrDie()` and
  is excluded. Buildable.

**BLOCKER (item 2 - recursion contradiction, must fix before build):**
The doc takes TWO incompatible positions. The CODE (Design , lines 48-65) does
NOT make recursion fatal, for two independent reasons: (1) `OrderedMessages` 
`TopologicalVisit` (generator.cpp:119) EXCLUDES recursive messages from the
iteration set entirely; (2) the skip predicate (line 54) skips `IsRecursive(msg)`
anyway. Furthermore `IsRecursive` (type_mapper.cpp:74-93) flags ANY message that
can transitively REACH a cycle (its `stack.count` returns true for any ancestor on
the DFS path), so a top-level message with a field referencing a recursive message
is ALSO `IsRecursive`  excluded/skipped. There is therefore NO case in which a
recursive-derived `UNSUPPORTED` node reaches `FindUnsupportedIr`. The code's actual
behaviour = today's: recursion stays skipped, file still generates, regression-free.

But the PROSE claims the opposite: line 76 ("a top-level recursive message causes
the entire file to fail generation . intentional") and line 257 ("The behaviour
change to fail recursive top-level messages is intentional"). This is factually
wrong given the code and internally contradictory. It is dangerous: an implementer
following the prose could remove the skip to "own" recursionfatal, regressing
every currently-skipped recursive message - and every message referencing one -
into build failures.

Required change (single coherent, regression-free position = what the code does):
1. Commit to: **recursion remains skipped / non-fatal, exactly as today.** GIR-8
   hard-errors ONLY on genuinely-unsupported TYPES reached inside non-skipped
   messages: `google.protobuf.Any`, `google.protobuf.Struct`, real `oneof`,
   unsupported map key/value, and unsupported flatten-wrapper leaves. Delete the
   "recursivefatal / behaviour change" claims at lines 76 and 257, and reframe the
   Summary (line 5) accordingly. Drop or restate "Verified: no active in-repo proto
   has top-level recursive messages" - it is only meaningful under the (rejected)
   fatal interpretation; the correct statement is "recursion handling is unchanged."

Minor (non-blocking, fold if convenient):
- Given `OrderedMessages` already excludes recursive messages, the `IsRecursive(msg)`
  term in the validation skip is redundant (harmless/defensive); `IsFlattenedWrapper`
  IS needed (wrappers appear in `OrderedMessages` and are skipped at emit).
- `ValidateNoUnsupportedIr` calls `BuildFieldIr(msg->field(i))` directly and does
  not replicate `GatherFieldsImpl`'s field-level-flatten inlining
  (generator.cpp:445-448); for such a field `BuildFieldIr` yields a `STRUCT` that
  `FindUnsupportedIr` recurses into, so the same descendant fields are still checked
  for `UNSUPPORTED` - equivalent detection. Worth a one-line note.
