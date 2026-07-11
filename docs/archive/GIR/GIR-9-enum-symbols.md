# GIR-9 Design: Emit C++ Enum Symbols and Typed Accessors

## Summary

GIR-9 implements #75 by using the existing first-class `EnumIdentity` IR data to emit C++ enum symbols and typed enum field accessors from the GIR-backed C++ generator.

The core behavior is additive C++ API output:

- Emit every proto enum as a scoped C++ enum:

```cpp
enum class Name : int32_t {
  SYMBOL = number,
};
```

- Keep enum field storage, raw getters, raw setters, Arrow schema, IPC schema, positional `Encode()` bytes, decode behavior, and TypeScript wire descriptors `int32_t`/integer based.
- Add typed enum getters and fluent typed setters for singular and nullable enum fields.
- Keep raw `int32_t` setters available.
- Use a shared `CppEnumName` helper in row and view emitters.
- Derive enum identity from descriptors/IR only; do not store C++ type strings on IR nodes.
- Leave RBA read-only.

The fixture design resolves the three integration facts from the Step-2 re-review:

1. `coverage.proto` already declares `TopLevelStatus` and `NestedEnums.InnerStatus`. GIR-9 reuses those existing enums and does not re-declare `TopLevelStatus`. The existing enum fields `ScalarCoverage.status`, `ScalarCoverage.nested_status`, `Leaf.status`, and `NestedEnums.state` receive generated typed C++ accessors additively, with no wire/schema change.
2. The new broad enum fixture is isolated in a separate `enum_coverage.proto` generation unit. This keeps the existing whole-file `coverage.fletcher.ts` golden and existing `coverage.*.ipc` set byte-identical. The new file produces fresh `enum_coverage.*` generated outputs and goldens.
3. The enum-owner ordering edge is tested red-first by declaring the message that owns a nested enum after the message that references that nested enum. Without the new enum-owner dependency in `TopologicalVisit`, generated C++ references `EnumOwner::InnerStatus` before `EnumOwner` is complete and fails to compile.

## Design

### Enum Emission

GIR-9 emits all enum descriptors in a generated file, not only enums referenced by fields.

Top-level enums are emitted at package namespace scope before message classes:

```cpp
namespace fletcher_gen {
namespace integration {
namespace coverage {

enum class TopLevelStatus : int32_t {
  TOP_LEVEL_STATUS_UNSPECIFIED = 0,
  TOP_LEVEL_STATUS_OK = 1,
  TOP_LEVEL_STATUS_WARN = 2,
  TOP_LEVEL_STATUS_ERROR = 3,
};

class ScalarCoverage {
 public:
  ScalarCoverage& set_status(int32_t value);
  ScalarCoverage& set_status(TopLevelStatus value);

  int32_t status() const;
  TopLevelStatus status_typed() const;
};

}  // namespace coverage
}  // namespace integration
}  // namespace fletcher_gen
```

Nested enums are emitted once inside their owning generated message class, in the `public:` section before accessors that may reference them:

```cpp
class NestedEnums {
 public:
  enum class InnerStatus : int32_t {
    INNER_STATUS_UNSPECIFIED = 0,
    INNER_STATUS_ACTIVE = 1,
    INNER_STATUS_DISABLED = 2,
  };

  NestedEnums& set_state(int32_t value);
  NestedEnums& set_state(InnerStatus value);

  int32_t state() const;
  InnerStatus state_typed() const;
};
```

For `coverage.proto`, GIR-9 emits C++ enum classes for the existing declarations:

```proto
enum TopLevelStatus { ... }

message NestedEnums {
  enum InnerStatus { ... }
  InnerStatus state = 1;
}
```

It does not add or re-declare `TopLevelStatus`. It adds typed C++ accessors for existing enum fields:

```proto
message ScalarCoverage {
  TopLevelStatus status = 31;
  NestedEnums.InnerStatus nested_status = 32;
}

message Leaf {
  TopLevelStatus status = 3;
}
```

The generated C++ for `coverage.proto` changes additively, but existing TS, IPC, schema, RBA, and `.v1.bin` goldens remain byte-identical.

### Ordering Edge

`TopologicalVisit` currently emits messages in file order and reorders only for `TYPE_MESSAGE` fields. Enum fields are skipped, which is insufficient once typed C++ accessors name nested enum owner classes.

A field like this:

```proto
message EnumCoverage {
  EnumOwner.InnerStatus nested_status = 5;
}

message EnumOwner {
  enum InnerStatus {
    INNER_STATUS_UNSPECIFIED = 0;
    INNER_STATUS_ACTIVE = 1;
    INNER_STATUS_DISABLED = 2;
  }
}
```

requires `EnumOwner` to be emitted before `EnumCoverage` in C++, even though `EnumOwner` appears after `EnumCoverage` in proto source. A nested enum cannot be forward-declared independently from its owning class.

GIR-9 adds an enum-owner dependency edge:

For message `M`, for each enum-typed field `F`, if `F`'s enum `E` is nested inside owning message `O`, and `O != M`, and `O` belongs to the same generated file, visit `O` before `M`.

Implementation shape in `TopologicalVisit`:

```cpp
for (int i = 0; i < message->field_count(); ++i) {
  const auto* field = message->field(i);
  if (field->type() != google::protobuf::FieldDescriptor::TYPE_ENUM) {
    continue;
  }

  const auto* enum_descriptor = field->enum_type();
  const auto* owner = enum_descriptor ? enum_descriptor->containing_type() : nullptr;
  if (owner != nullptr && owner != message && owner->file() == message->file()) {
    TopologicalVisit(owner, visiting, visited, ordered);
  }
}
```

The real code should match local helper style and diagnostics, but the behavior is fixed: enum-owner dependencies are part of message ordering.

The forcing fixture must declare the owner after the referencing message. Declaring the owner before the referencing message would let current file-order emission pass without the GIR-9 fix and would not be red-first.

### `CppEnumName`

Add a shared C++ enum-name helper, for example in `protoc/include/generator_internal.hpp`:

```cpp
std::string CppEnumName(
    const google::protobuf::EnumDescriptor* enum_descriptor,
    const google::protobuf::FileDescriptor* context_file,
    const google::protobuf::Descriptor* current_message);
```

The exact signature may vary to fit existing generator utilities, but it must be shared by:

- enum declaration emission in `generator.cpp`
- row typed setters in `generator.cpp`
- row typed getters in `generator.cpp`
- view typed getters in `cpp_backend_view_visitor.cpp`

The helper derives C++ spelling from descriptors and scope context:

```cpp
TopLevelStatus
NestedEnums::InnerStatus
EnumOwner::InnerStatus
CurrentMessage::InnerStatus
InnerStatus
```

It must not be stored on `ScalarNode`, `EnumIdentity`, `FieldMapping`, or any IR node. The IR remains language-neutral. C++ spelling is backend-only.

For imported enum references in `enum_coverage.proto`, the generated `enum_coverage.fletcher.pb.h` must include or otherwise make visible the generated declaration for `coverage.proto` so that `TopLevelStatus` can be used in typed accessors. If the generator already emits includes for proto imports, reuse that path. If it does not, GIR-9 must add the minimal include emission needed for generated C++ references to imported enum types.

### Typed Accessors

Raw enum field storage remains `int32_t`.

Singular enum fields keep raw accessors and gain typed overloads:

```cpp
EnumCoverage& set_status(int32_t value) {
  // existing raw setter body
  return *this;
}

EnumCoverage& set_status(TopLevelStatus value) {
  set_status(static_cast<int32_t>(value));
  return *this;
}

int32_t status() const {
  // existing raw getter body
}

TopLevelStatus status_typed() const {
  return static_cast<TopLevelStatus>(status());
}
```

Nullable enum fields keep raw optional accessors and gain typed accessors:

```cpp
EnumCoverage& set_nullable_status(int32_t value) {
  // existing raw setter body
  return *this;
}

EnumCoverage& set_nullable_status(TopLevelStatus value) {
  set_nullable_status(static_cast<int32_t>(value));
  return *this;
}

std::optional<int32_t> nullable_status() const {
  // existing raw getter body
}

std::optional<TopLevelStatus> nullable_status_typed() const {
  auto value = nullable_status();
  if (!value.has_value()) {
    return std::nullopt;
  }
  return static_cast<TopLevelStatus>(*value);
}
```

Repeated enum fields keep raw setters accepting the existing `int32_t` container. GIR-9 may add typed getters, but must not add typed repeated setters in this round:

```cpp
std::vector<TopLevelStatus> statuses_typed() const {
  std::vector<TopLevelStatus> out;
  out.reserve(statuses().size());
  for (int32_t value : statuses()) {
    out.push_back(static_cast<TopLevelStatus>(value));
  }
  return out;
}
```

Map-valued enum fields keep raw map/container APIs with `int32_t` values. GIR-9 may add typed getters that cast only the value side while preserving the existing generated map shape:

```cpp
std::vector<std::pair<std::string, TopLevelStatus>> status_by_name_typed() const {
  std::vector<std::pair<std::string, TopLevelStatus>> out;
  out.reserve(status_by_name().size());
  for (const auto& entry : status_by_name()) {
    out.emplace_back(entry.first, static_cast<TopLevelStatus>(entry.second));
  }
  return out;
}
```

If the actual generated map representation differs, use that representation and cast only enum values.

View typed getters follow the same return types and use the row-header enum declarations. The view header must not re-declare enums.

```cpp
TopLevelStatus status_typed() const {
  return static_cast<TopLevelStatus>(status());
}
```

No validation or normalization is added. Unknown proto3 enum numeric values continue to round-trip as `int32_t`. A typed getter may return an enum-class value that does not correspond to a declared enumerator.

## Forcing-test mapping

Use two fixture inputs.

First, keep `coverage.proto` as-is. It already contains the existing enum declarations and enum fields:

```proto
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
  TopLevelStatus status = 31;
  NestedEnums.InnerStatus nested_status = 32;
}

message Leaf {
  TopLevelStatus status = 3;
}
```

GIR-9 must generate typed accessors for these existing fields:

```cpp
ScalarCoverage scalars;
scalars.set_status(TopLevelStatus::TOP_LEVEL_STATUS_OK);
EXPECT_EQ(scalars.status_typed(), TopLevelStatus::TOP_LEVEL_STATUS_OK);

scalars.set_nested_status(NestedEnums::InnerStatus::INNER_STATUS_ACTIVE);
EXPECT_EQ(scalars.nested_status_typed(),
          NestedEnums::InnerStatus::INNER_STATUS_ACTIVE);

Leaf leaf;
leaf.set_status(TopLevelStatus::TOP_LEVEL_STATUS_WARN);
EXPECT_EQ(leaf.status_typed(), TopLevelStatus::TOP_LEVEL_STATUS_WARN);

NestedEnums nested;
nested.set_state(NestedEnums::InnerStatus::INNER_STATUS_DISABLED);
EXPECT_EQ(nested.state_typed(), NestedEnums::InnerStatus::INNER_STATUS_DISABLED);
```

Second, add a separate proto file for the new forcing fixture:

```text
integration-tests/protoc-coverage/proto/enum_coverage.proto
```

Exact proto structure:

```proto
syntax = "proto3";
package integration.coverage;

import "coverage.proto";

enum StandaloneStatus {
  STANDALONE_STATUS_UNSPECIFIED = 0;
  STANDALONE_STATUS_PRESENT = 1;
}

message EnumCoverage {
  TopLevelStatus status = 1;
  optional TopLevelStatus nullable_status = 2;
  repeated TopLevelStatus statuses = 3;
  map<string, TopLevelStatus> status_by_name = 4;
  EnumOwner.InnerStatus nested_status = 5;
}

message EnumOwner {
  enum InnerStatus {
    INNER_STATUS_UNSPECIFIED = 0;
    INNER_STATUS_ACTIVE = 1;
    INNER_STATUS_DISABLED = 2;
  }

  int32 marker = 1;
}
```

The declaration order is intentional. `EnumCoverage` references `EnumOwner.InnerStatus`, and `EnumOwner` is declared after `EnumCoverage`. There is no `TYPE_MESSAGE` field from `EnumCoverage` to `EnumOwner`, so the existing message-only topological ordering does not rescue this case. Without the enum-owner edge, generated C++ fails red-first by naming `EnumOwner::InnerStatus` before `EnumOwner` is emitted.

`StandaloneStatus` is intentionally unreferenced. The test must still compile a reference to it, proving GIR-9 emits all file-level enums, not just referenced enums.

Forcing test name:

```cpp
TEST(EnumEmit, GeneratedEnumSymbolsRoundTrip) {
  // compile/run assertions below
}
```

Core mutable row assertions:

```cpp
EnumCoverage msg;

msg.set_status(TopLevelStatus::TOP_LEVEL_STATUS_OK);
EXPECT_EQ(msg.status(), static_cast<int32_t>(TopLevelStatus::TOP_LEVEL_STATUS_OK));
EXPECT_EQ(msg.status_typed(), TopLevelStatus::TOP_LEVEL_STATUS_OK);

msg.set_nullable_status(TopLevelStatus::TOP_LEVEL_STATUS_WARN);
ASSERT_TRUE(msg.nullable_status_typed().has_value());
EXPECT_EQ(*msg.nullable_status_typed(), TopLevelStatus::TOP_LEVEL_STATUS_WARN);

msg.set_nested_status(EnumOwner::InnerStatus::INNER_STATUS_ACTIVE);
EXPECT_EQ(msg.nested_status_typed(),
          EnumOwner::InnerStatus::INNER_STATUS_ACTIVE);

msg.set_statuses(std::vector<int32_t>{
    static_cast<int32_t>(TopLevelStatus::TOP_LEVEL_STATUS_OK),
    static_cast<int32_t>(TopLevelStatus::TOP_LEVEL_STATUS_WARN),
});
auto typed_statuses = msg.statuses_typed();
ASSERT_EQ(typed_statuses.size(), 2);
EXPECT_EQ(typed_statuses[0], TopLevelStatus::TOP_LEVEL_STATUS_OK);
EXPECT_EQ(typed_statuses[1], TopLevelStatus::TOP_LEVEL_STATUS_WARN);

msg.set_status_by_name({
    {"ok", static_cast<int32_t>(TopLevelStatus::TOP_LEVEL_STATUS_OK)},
    {"warn", static_cast<int32_t>(TopLevelStatus::TOP_LEVEL_STATUS_WARN)},
});
auto typed_map = msg.status_by_name_typed();
ASSERT_EQ(typed_map.size(), 2);
EXPECT_EQ(typed_map[0].second, TopLevelStatus::TOP_LEVEL_STATUS_OK);
EXPECT_EQ(typed_map[1].second, TopLevelStatus::TOP_LEVEL_STATUS_WARN);

auto standalone = StandaloneStatus::STANDALONE_STATUS_PRESENT;
EXPECT_EQ(static_cast<int32_t>(standalone), 1);
```

If the generated map API is not initializer-list based, keep the assertion intent and use the existing raw map setter shape. Repeated and map raw setters must use `int32_t` literals or explicit `static_cast<int32_t>`. They must not pass scoped enum-class values directly into raw `int32_t` containers.

View assertions:

```cpp
auto encoded = msg.Encode();
EnumCoverageView view(encoded);

EXPECT_EQ(view.status_typed(), TopLevelStatus::TOP_LEVEL_STATUS_OK);

ASSERT_TRUE(view.nullable_status_typed().has_value());
EXPECT_EQ(*view.nullable_status_typed(), TopLevelStatus::TOP_LEVEL_STATUS_WARN);

EXPECT_EQ(view.nested_status_typed(),
          EnumOwner::InnerStatus::INNER_STATUS_ACTIVE);

auto view_statuses = view.statuses_typed();
ASSERT_EQ(view_statuses.size(), 2);
EXPECT_EQ(view_statuses[0], TopLevelStatus::TOP_LEVEL_STATUS_OK);
EXPECT_EQ(view_statuses[1], TopLevelStatus::TOP_LEVEL_STATUS_WARN);

auto view_typed_map = view.status_by_name_typed();
ASSERT_EQ(view_typed_map.size(), 2);
EXPECT_EQ(view_typed_map[0].second, TopLevelStatus::TOP_LEVEL_STATUS_OK);
EXPECT_EQ(view_typed_map[1].second, TopLevelStatus::TOP_LEVEL_STATUS_WARN);
```

Byte identity between raw and typed singular setter paths:

```cpp
EnumCoverage raw_msg;
raw_msg.set_status(1);
raw_msg.set_nullable_status(2);
raw_msg.set_nested_status(1);
raw_msg.set_statuses(std::vector<int32_t>{1, 2});
raw_msg.set_status_by_name({{"ok", 1}, {"warn", 2}});

EnumCoverage typed_msg;
typed_msg.set_status(TopLevelStatus::TOP_LEVEL_STATUS_OK);
typed_msg.set_nullable_status(TopLevelStatus::TOP_LEVEL_STATUS_WARN);
typed_msg.set_nested_status(EnumOwner::InnerStatus::INNER_STATUS_ACTIVE);
typed_msg.set_statuses(std::vector<int32_t>{
    static_cast<int32_t>(TopLevelStatus::TOP_LEVEL_STATUS_OK),
    static_cast<int32_t>(TopLevelStatus::TOP_LEVEL_STATUS_WARN),
});
typed_msg.set_status_by_name({
    {"ok", static_cast<int32_t>(TopLevelStatus::TOP_LEVEL_STATUS_OK)},
    {"warn", static_cast<int32_t>(TopLevelStatus::TOP_LEVEL_STATUS_WARN)},
});

EXPECT_EQ(raw_msg.Encode(), typed_msg.Encode());
```

Red-first expectations before GIR-9:

```cpp
TopLevelStatus::TOP_LEVEL_STATUS_OK;
StandaloneStatus::STANDALONE_STATUS_PRESENT;
EnumOwner::InnerStatus::INNER_STATUS_ACTIVE;

ScalarCoverage().status_typed();
ScalarCoverage().nested_status_typed();
Leaf().status_typed();
NestedEnums().state_typed();

EnumCoverage().status_typed();
EnumCoverage().nullable_status_typed();
EnumCoverage().statuses_typed();
EnumCoverage().status_by_name_typed();
EnumCoverage().nested_status_typed();
```

These references should fail before GIR-9 because enum classes and typed accessors are not emitted. After enum class emission but before the enum-owner ordering fix, the `EnumCoverage` case should still fail because `EnumOwner` is declared after `EnumCoverage` in `enum_coverage.proto`.

Validation commands:

```bash
cmake --build <build-dir> --target generate_coverage_outputs
ctest -R EnumEmit.GeneratedEnumSymbolsRoundTrip
ctest -R coverage
ctest -R TsVisitor.DescriptorByteIdentical
ctest -R GenErrors.NoValueOrDieInIrGeneratedCode
ctest -R CoverageHarness.GeneratedTypescriptCompiles
```

The exact build preset may vary, but the validation intent is fixed:

- regenerate both `coverage.proto` and `enum_coverage.proto`
- compile generated C++ row and view APIs
- run the enum forcing test
- prove existing `coverage.fletcher.ts` remains byte-identical
- prove existing `coverage.*.ipc` set remains byte-identical
- prove RBA remains read-only/no-drift
- add fresh goldens only for `enum_coverage.proto`

## Risks & Unknowns

Typed accessor naming must not collide with existing raw accessors. The proposed getter name is `<field>_typed()`, and the proposed setter is an overload of `set_<field>(EnumType)` for singular and nullable fields. Raw `int32_t` setters remain source-compatible.

Repeated and map enum fields must not gain typed setters in GIR-9. Scoped `enum class` values do not implicitly convert to `int32_t`, but collection overloads can still become confusing or expensive. Keep collection writes raw and add typed read helpers only.

Nested enum qualification is the highest-risk generator detail. The same `CppEnumName` helper must be used in row and view emitters so `NestedEnums::InnerStatus`, `EnumOwner::InnerStatus`, and imported `TopLevelStatus` are spelled consistently.

Imported enum references in `enum_coverage.proto` require generated C++ visibility for `TopLevelStatus` from `coverage.proto`. If generated headers for imports are not already included, GIR-9 must add include emission for imported generated row headers.

The enum-owner dependency must be descriptor-based, not string-based. It should use `FieldDescriptor::enum_type()` and `EnumDescriptor::containing_type()` or equivalent IR descriptor identity. Do not add C++ type names to IR nodes.

Duplicate enum emission must be prevented by emitting enums only in their owning scope: top-level enums in file scope, nested enums inside the owning message class. If tracking is needed, track by descriptor identity or full proto name.

Open enum values remain allowed. Typed getters are casts, not validators.

Existing fixture churn is not allowed. `coverage.proto` is not widened for `EnumCoverage`; it already contains enough enum fields to prove typed accessors on existing messages. The new enum fixture lives in `enum_coverage.proto` so existing whole-file TS and IPC goldens do not change.

RBA is read-only. Do not update RBA emitters or RBA goldens for GIR-9.

## Files-to-touch

Generator internals:

```text
protoc/include/generator_internal.hpp
protoc/src/generator.cpp
protoc/src/cpp_backend_view_visitor.cpp
```

Required `generator.cpp` edits:

1. Add enum-owner dependency handling in `TopologicalVisit` after the existing message-field dependency loop.
2. Emit every file-level enum descriptor before message classes.
3. Emit every nested enum descriptor inside its owning message class before generated accessors.
4. Add typed enum setters for singular and nullable enum fields, delegating to raw `int32_t` setters.
5. Add typed enum getters for singular and nullable enum fields.
6. Add typed repeated/map enum getters if compatible with the existing generated container shapes.
7. Use `CppEnumName` for every emitted enum type spelling.
8. Ensure generated headers include imported generated row headers when typed accessors reference imported enum declarations.

Required `cpp_backend_view_visitor.cpp` edits:

1. In scalar getter emission, detect enum identity.
2. Emit typed view getters using `CppEnumName`.
3. Reference row-header enum declarations only; do not re-declare enums in view classes.

Read/confirm only unless helper plumbing needs adjustment:

```text
protoc/include/ir.hpp
protoc/src/ir.cpp
protoc/include/cpp_backend_type_table.hpp
protoc/src/cpp_backend_type_table.cpp
```

Confirm `EnumIdentity` already carries enum identity and symbols without C++ type strings. Confirm enum storage remains `int32_t`.

Existing fixture proto remains unchanged except for ordinary formatting only if required:

```text
integration-tests/protoc-coverage/proto/coverage.proto
```

Do not add `EnumCoverage` here. Do not re-declare `TopLevelStatus`. Existing generated `coverage.proto` C++ should gain enum classes and typed accessors, but the existing whole-file non-C++ goldens stay byte-identical:

```text
integration-tests/protoc-coverage/golden/coverage.fletcher.ts
integration-tests/protoc-coverage/golden/coverage.Branch.v1.bin
integration-tests/protoc-coverage/golden/coverage.CompositeCoverage.v1.bin
integration-tests/protoc-coverage/golden/coverage.CompositeCoverage.alternate-null-empty.v1.bin
integration-tests/protoc-coverage/golden/coverage.CompositeCoverage.maps-non-sorted.v1.bin
integration-tests/protoc-coverage/golden/coverage.FieldFlattenedPosition.v1.bin
integration-tests/protoc-coverage/golden/coverage.FlattenedPoint.v1.bin
integration-tests/protoc-coverage/golden/coverage.Leaf.v1.bin
integration-tests/protoc-coverage/golden/coverage.NestedEnums.v1.bin
integration-tests/protoc-coverage/golden/coverage.ScalarCoverage.v1.bin
integration-tests/protoc-coverage/golden/coverage.ScalarCoverage.all-set.v1.bin
integration-tests/protoc-coverage/golden/coverage.ServiceReply.v1.bin
integration-tests/protoc-coverage/golden/coverage.ServiceRequest.v1.bin
```

Add the separate enum fixture:

```text
integration-tests/protoc-coverage/proto/enum_coverage.proto
```

Add or extend enum forcing tests:

```text
integration-tests/protoc-coverage/tests/test_enum_emit.cpp
```

If local convention prefers fewer test binaries, this may be folded into an existing coverage test file, but the discovered test name should remain:

```text
EnumEmit.GeneratedEnumSymbolsRoundTrip
```

CMake edits in `integration-tests/protoc-coverage/CMakeLists.txt`:

1. Keep the existing `coverage.proto` generation unit and `_ipc_all_messages` unchanged.
2. Add a second generation unit:

```cmake
set(_enum_stem enum_coverage)
set(_enum_src "${PROTO_DIR}/${_enum_stem}.proto")

set(GENERATED_ENUM_CPP_HEADERS
    "${GENERATED_DIR}/${_enum_stem}.fletcher.pb.h"
    "${GENERATED_DIR}/${_enum_stem}.fletcher.arrow.pb.h"
    "${GENERATED_DIR}/${_enum_stem}.fletcher.accessor.pb.h")
set(GENERATED_ENUM_TS
    "${GENERATED_DIR}/${_enum_stem}.fletcher.ts")
set(GENERATED_ENUM_RUST
    "${GENERATED_DIR}/${_enum_stem}.fletcher.rs")
```

3. Add the expected IPC set for the separate file:

```cmake
set(_enum_ipc_all_messages
    EnumCoverage
    EnumOwner)

set(_enum_ipc_opened_messages
    EnumCoverage)
```

4. Build `GENERATED_ENUM_IPC`, `_enum_expected_ipc_arg`, and an `add_custom_command` mirroring the existing `coverage.proto` command, but using `${_enum_src}` and validating only:

```text
enum_coverage.EnumCoverage.ipc
enum_coverage.EnumOwner.ipc
```

5. Include both `coverage.proto` and `enum_coverage.proto` generated outputs in `generate_coverage_outputs`:

```cmake
add_custom_target(generate_coverage_outputs ALL
    DEPENDS ${GENERATED_OUTPUTS} ${GENERATED_ENUM_OUTPUTS})
```

6. Add the enum forcing test executable:

```cmake
add_executable(coverage_enum_emit_tests tests/test_enum_emit.cpp)
add_dependencies(coverage_enum_emit_tests generate_coverage_outputs)
target_include_directories(coverage_enum_emit_tests PRIVATE "${GENERATED_DIR}")
target_compile_definitions(coverage_enum_emit_tests PRIVATE
    PARITY_GOLDEN_DIR_PATH="${CMAKE_CURRENT_SOURCE_DIR}/golden")
target_link_libraries(coverage_enum_emit_tests PRIVATE
    GTest::gtest_main
    arrow::arrow
    fletcher-arrow-bridge::fletcher-arrow-bridge
    fletcher-pubsub::fletcher-pubsub)

gtest_discover_tests(coverage_enum_emit_tests DISCOVERY_MODE PRE_TEST)
```

7. Add a TS byte-identical test for the new generated TS file if the local harness requires every generated TS file to have a committed golden:

```cmake
target_compile_definitions(<enum-ts-test> PRIVATE
    GENERATED_TS_PATH="${GENERATED_DIR}/enum_coverage.fletcher.ts"
    TS_GOLDEN_PATH="${CMAKE_CURRENT_SOURCE_DIR}/golden/enum_coverage.fletcher.ts")
```

If the existing `coverage_ts_visitor_tests` only supports one pair of paths, either parameterize it for both files or add a second small test target. Do not point the existing `coverage.fletcher.ts` golden at enum output.

New golden files:

```text
integration-tests/protoc-coverage/golden/enum_coverage.fletcher.ts
integration-tests/protoc-coverage/golden/enum_coverage.EnumCoverage.v1.bin
integration-tests/protoc-coverage/golden/enum_coverage.EnumCoverage.all-set.v1.bin
integration-tests/protoc-coverage/golden/enum_coverage.EnumOwner.v1.bin
```

If the parity oracle requires IPC schema goldens, add the matching new IPC goldens under the existing local convention:

```text
integration-tests/protoc-coverage/golden/enum_coverage.EnumCoverage.ipc
integration-tests/protoc-coverage/golden/enum_coverage.EnumOwner.ipc
```

Existing `coverage.proto` golden names that stay identical:

```text
integration-tests/protoc-coverage/golden/coverage.fletcher.ts
integration-tests/protoc-coverage/golden/coverage.Branch.v1.bin
integration-tests/protoc-coverage/golden/coverage.CompositeCoverage.v1.bin
integration-tests/protoc-coverage/golden/coverage.CompositeCoverage.alternate-null-empty.v1.bin
integration-tests/protoc-coverage/golden/coverage.CompositeCoverage.maps-non-sorted.v1.bin
integration-tests/protoc-coverage/golden/coverage.FieldFlattenedPosition.v1.bin
integration-tests/protoc-coverage/golden/coverage.FlattenedPoint.v1.bin
integration-tests/protoc-coverage/golden/coverage.Leaf.v1.bin
integration-tests/protoc-coverage/golden/coverage.NestedEnums.v1.bin
integration-tests/protoc-coverage/golden/coverage.ScalarCoverage.v1.bin
integration-tests/protoc-coverage/golden/coverage.ScalarCoverage.all-set.v1.bin
integration-tests/protoc-coverage/golden/coverage.ServiceReply.v1.bin
integration-tests/protoc-coverage/golden/coverage.ServiceRequest.v1.bin
```

Files not to touch for behavior:

```text
RBA generator sources
*.fletcher.accessor.pb.h goldens
GIR-10/GIR-11 fixture areas
```

RBA remains read-only for GIR-9.

## Step-2 final re-review (2026-07-11)

Verdict: **APPROVE.** Third rework cycle. The three fixture-integration blockers
from the prior re-review are genuinely resolved against the real fixture files,
the sound core is intact, and LOCKED #1/#2/#3/#6/#7/#9 are honored. One
non-design build-wiring correction is required at implementation time (item A);
it does not reopen any of the three blockers.

Blockers — confirmed resolved:

1. **Enums reused, not re-declared — RESOLVED.** Verified against real
   `coverage.proto` (enum `TopLevelStatus` L19-24, `NestedEnums.InnerStatus`
   L26-34, fields `ScalarCoverage.status`=31 / `nested_status`=32 L71-72,
   `Leaf.status`=3 L78, `NestedEnums.state`=1 L33). The design reuses these and
   does not re-declare `TopLevelStatus` (no protoc "already defined"). Existing
   fields gain typed accessors additively. Cross-checked `coverage_fixture.hpp`:
   it drives every enum field through **raw `int32_t` setters** with `int32_t`
   constants (`set_status(kTopLevelStatusWarn)`, `MakeLeaf(...,int32_t status)`),
   and the design keeps those raw setters source-compatible, so the added
   `set_status(TopLevelStatus)` overload is a non-preferred conversion (no
   ambiguity) and the fixture compiles unchanged. C++ source grows; wire/.ipc/TS
   stay identical.

2. **Existing goldens byte-identical via a SEPARATE `enum_coverage.proto` —
   RESOLVED (with correction A).** New `EnumCoverage`/`EnumOwner`/`StandaloneStatus`
   live in a new proto + a second generation unit. Enum lowering stays int32 on
   the Arrow schema / IPC / wire / TS surfaces, so `coverage.fletcher.ts`, every
   `coverage.*.ipc`, and every `coverage.*.v1.bin` stay byte-identical; the
   additive typed accessors live only in the C++ row/view headers, which are
   compiled, not golden-diffed. No existing golden is mutated. The second-unit
   CMake shape (own stem, headers, `_enum_ipc_all_messages`={EnumCoverage,
   EnumOwner}, own custom_command + validate guard, separate TS golden test,
   fresh `enum_coverage.*` goldens) matches the real CMakeLists structure. Same
   `package integration.coverage` means the imported `TopLevelStatus` needs only
   an include, not cross-namespace qualification — sound.

3. **Ordering edge red-first — RESOLVED.** `EnumOwner` is declared AFTER
   `EnumCoverage`, and `EnumCoverage`'s only reference to it is the TYPE_ENUM
   field `nested_status` (`EnumOwner.InnerStatus`). Verified against the real
   `TopologicalVisit` (generator.cpp ~L111-145): its dependency loop recurses
   only on `TYPE_MESSAGE` fields, and for map fields only when the *value* is
   `TYPE_MESSAGE`. `EnumCoverage` has no `TYPE_MESSAGE` field (its map value is
   `TopLevelStatus`, an enum), so today it carries NO ordering dependency and
   stays in file order (first) — generated C++ names `EnumOwner::InnerStatus`
   before the class exists. Genuinely red-first; the new enum-owner edge fixes
   it. The design's `TopologicalVisit` sketch uses placeholder arg names but
   explicitly defers to the real 4-arg signature (`msg,file,emitted,order`); the
   `owner->file()==message->file()` guard is equivalent to the real `==file`
   guard, so behavior is correct.

Sound core — confirmed intact: emits ALL file-level enums including the
unreferenced `StandaloneStatus` (L346-349, forced-referenced at L417); adds typed
getters + fluent typed setters while retaining raw int32 setters; storage stays
int32 / no wire change (LOCKED #2); shared `CppEnumName` used across row + view
emitters; `CppEnumName`/enum spelling kept backend-only, not stored on any IR
node (LOCKED #1, #7); RBA untouched and read-only (LOCKED #3); #75 lands as typed
emission on first-class enum identity (LOCKED #6); red-first fixture (LOCKED #9).

Required correction (build wiring, not design — does not reopen the blockers):

A. **Do NOT request `rust` for the enum generation unit (and drop
   `GENERATED_ENUM_RUST`).** The coverage unit's `--fletcher_opt=...,rust` emits
   the fixed-name shared helper `__rba.fletcher.rs` (declared at real CMakeLists
   L100). "Mirroring the coverage command" for the enum unit would make a SECOND
   protoc invocation write the same `${GENERATED_DIR}/__rba.fletcher.rs` — a
   duplicate producer. Under a parallel generator (Ninja) on Windows this races
   into a sharing violation / build failure; even serialized it is churn, and the
   emitted `enum_coverage.fletcher.rs` is dead output (the Rust cargo harness only
   compiles its fixed entry set, not this new file). The enum forcing test needs
   only the C++ row/view headers plus the `.ipc` guard, so generate
   `ipc,accessor,ts` (or minimally `ipc,ts` + the two C++ headers). This is the
   one spot where the "CMake edits are concrete and correct" claim overreached;
   the fix is mechanical.

Minor (optional, non-blocking): enum_coverage's `accessor` output and its
`.fletcher.ts` will be pulled into the existing accessor-compile and tsc harness
paths — fine as long as the emitter produces valid output (expected), but confirm
the tsc harness type-checks the new `.ts` rather than only `coverage.fletcher.ts`.
