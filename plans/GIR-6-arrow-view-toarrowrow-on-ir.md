# GIR-6 Arrow View Getters + ToArrowRow IR Visitors

## Summary

GIR-6 moves the generated Arrow C++ **view getters** and `ToArrowRow()` field emission off the temporary `IrNode -> FieldMapping` projection and onto recursive IR visitors.

The live scope is intentionally narrow:

- migrate generated `...View` getters
- migrate `ToArrowRow()`
- keep behavior value-identical to the current generated C++

The following are **not live paths** and must not be migrated:

- `SetFromScalars_`
- `ToScalars`
- `Make*Scalar_`

Those helpers are dead code orphaned by GIR-3/GIR-4. They have no callers, and the generated coverage fixture confirms the live surface: `coverage.fletcher.arrow.pb.h` has 122 `ToArrowRow`/`View` hits and zero `ToScalars`/`SetFromScalars_` hits.

The new backend entry points are:

```cpp
void EmitViewGetterFromIr(std::ostringstream& out,
                          const ir::IrNode& node,
                          const std::string& getter_name,
                          size_t field_index,
                          const google::protobuf::FileDescriptor* context_file);

void EmitToArrowRowFieldFromIr(std::ostringstream& out,
                               const ir::IrNode& node,
                               const std::string& getter_expr,
                               size_t field_index,
                               const google::protobuf::FileDescriptor* context_file);
```

The IR remains language-neutral. Arrow scalar names, Arrow array names, builder names, C++ storage types, generated view class names, and Arrow type expressions are derived in the C++ backend lookup layer, not stored on IR nodes.

Value identity is structurally achievable because the current `FieldMapping` bridge is only a projection of backend type information such as scalar type, storage type, buffer-ness, Arrow type expression, builder type, and scalar constructor template. GIR-6 removes that projection for the live view/row paths and calls the same backend lookup source directly.

## Design

GIR-6 adds a C++ backend visitor pair next to the existing GIR-3/GIR-4/GIR-5 visitors:

```cpp
class EdgeViewGetterVisitor {
public:
    EdgeViewGetterVisitor(std::ostringstream& out,
                          std::string getter_name,
                          size_t field_index,
                          const google::protobuf::FileDescriptor* context_file);

    void EmitField(const ir::IrNode& node);
};

class EdgeToArrowRowVisitor {
public:
    EdgeToArrowRowVisitor(std::ostringstream& out,
                          std::string getter_expr,
                          size_t field_index,
                          const google::protobuf::FileDescriptor* context_file);

    void EmitField(const ir::IrNode& node);
};
```

The visitors use backend-only derivations:

```cpp
const CppScalarInfo& LookupScalar(
    const ir::LogicalType& type,
    const std::optional<ir::EnumIdentity>& enum_identity);

std::string CppClassName(
    const google::protobuf::Descriptor* msg,
    const google::protobuf::FileDescriptor* context_file);
```

`CppScalarInfo` should expose, or be extended to expose, all C++/Arrow spelling needed by the view and row visitors:

```cpp
struct CppScalarInfo {
    std::string scalar_type;
    std::string array_type;
    std::string storage_type;
    std::string getter_type;
    std::string builder_type;
    std::string arrow_type_expr;
    std::string scalar_ctor_template;
    bool value_is_buffer;
};
```

If `array_type` is not already explicit, derive it in the C++ backend lookup layer. Do not add array-type spelling to IR.

`ToArrowRow()` must not use the GIR-3 `ValueAccessMode` model. It reads fields through the public generated getter, which already applies defaults for non-nullable scalar fields. Re-applying default handling inside `ToArrowRow()` would double-default. The `ToArrowRow()` visitor branches only on nullable vs non-nullable shape.

### Scalar

Scalar nodes lower through `LookupScalar(node.logical_type, node.enum_identity)`.

View getter shape:

```cpp
// nullable
std::optional<GetterType> name() const {
    if (!scalars_[idx]->is_valid) return std::nullopt;
    if constexpr (value_is_buffer) {
        const auto& s = static_cast<const ScalarType&>(*scalars_[idx]);
        return std::string_view{
            reinterpret_cast<const char*>(s.value->data()),
            static_cast<size_t>(s.value->size())};
    } else {
        return static_cast<const ScalarType&>(*scalars_[idx]).value;
    }
}

// non-nullable
GetterType name() const {
    if constexpr (value_is_buffer) {
        const auto& s = static_cast<const ScalarType&>(*scalars_[idx]);
        return std::string_view{
            reinterpret_cast<const char*>(s.value->data()),
            static_cast<size_t>(s.value->size())};
    } else {
        return static_cast<const ScalarType&>(*scalars_[idx]).value;
    }
}
```

`GetterType` is `std::string_view` for string/bytes buffers and the backend storage type otherwise. Enum scalars continue to use the existing Arrow physical scalar mapping.

`ToArrowRow()` scalar shape:

```cpp
// nullable
auto value = getter_expr;
row.push_back(value.has_value()
    ? std::shared_ptr<arrow::Scalar>(
          scalar_ctor_template("std::string(*value)" or "*value"))
    : arrow::MakeNullScalar(arrow_type_expr));

// non-nullable
auto value = getter_expr;
row.push_back(scalar_ctor_template("std::string(value)" or "value"));
```

For string/bytes, `ToArrowRow()` must continue constructing Arrow scalars from owned `std::string` values so Arrow owns the bytes.

`google.protobuf.*Value` wrapper WKTs are nullable scalar nodes with `facts.nullable = true`; they use this same generic nullable scalar path. Timestamp and duration WKTs lower through lookup to `arrow::TimestampScalar` and `arrow::DurationScalar` with `arrow::TimeUnit::NANO`.

### Struct

Struct nodes use `CppClassName(struct.identity.descriptor, context_file)`.

View getter shape:

```cpp
// nullable
std::optional<NestedClassView> name() const {
    if (!scalars_[idx]->is_valid) return std::nullopt;
    return NestedClassView(scalars_[idx]);
}

// non-nullable
NestedClassView name() const {
    return NestedClassView(scalars_[idx]);
}
```

`ToArrowRow()` shape:

```cpp
{
    auto type = arrow::struct_(
        detail::ImportSchema(NestedClassSchema())->fields());

    auto value = getter_expr;
    if constexpr (nullable) {
        if (value != nullptr) {
            row.push_back(std::make_shared<arrow::StructScalar>(
                ToArrowRow(*value), type));
        } else {
            row.push_back(arrow::MakeNullScalar(type));
        }
    } else {
        row.push_back(std::make_shared<arrow::StructScalar>(
            ToArrowRow(value), type));
    }
}
```

### List

`ListNode` has no `list_depth` field. Nested-list depth is structural and must be detected by walking nested `ListNode` elements until the leaf node is reached.

The visitor must classify these whole shapes:

- `List<Scalar>`
- `List<Struct>`
- `List<List<Struct>>`
- `List<List<List<Struct>>>`

It must not implement depth-2 or depth-3 nested struct lists as naive composed accessors. Current generated behavior is monolithic.

Depth-2 view getter:

```cpp
fletcher::ArrowNestedList<NestedClassView> name() const {
    const auto& ls =
        static_cast<const arrow::ListScalar&>(*scalars_[idx]);
    return fletcher::ArrowNestedList<NestedClassView>(ls.value);
}
```

Depth-3 view getter:

```cpp
fletcher::ArrowNestedList2<NestedClassView> name() const {
    const auto& ls =
        static_cast<const arrow::ListScalar&>(*scalars_[idx]);
    return fletcher::ArrowNestedList2<NestedClassView>(ls.value);
}
```

The leaf struct identity, not the intermediate list node, determines `NestedClassView`.

Repeated scalar view getter:

```cpp
fletcher::ArrowScalarList<ElementType, ElementArrayType> name() const {
    const auto& ls =
        static_cast<const arrow::ListScalar&>(*scalars_[idx]);
    return fletcher::ArrowScalarList<ElementType, ElementArrayType>(ls.value);
}
```

Repeated scalar `ToArrowRow()` uses a value builder:

```cpp
{
    ElementBuilderType builder;
    for (const auto& v : getter_expr) {
        (void)builder.Append(v);
    }

    row.push_back(std::make_shared<arrow::ListScalar>(
        *builder.Finish(),
        arrow::list(arrow::field("item", element_arrow_type, true))));
}
```

Repeated struct view getter:

```cpp
fletcher::ArrowRowViewList<NestedClassView> name() const {
    const auto& ls =
        static_cast<const arrow::ListScalar&>(*scalars_[idx]);
    return fletcher::ArrowRowViewList<NestedClassView>(ls.value);
}
```

Repeated struct `ToArrowRow()` uses a pointer builder returned by `arrow::MakeBuilder`:

```cpp
{
    auto type = arrow::struct_(
        detail::ImportSchema(NestedClassSchema())->fields());
    auto builder = arrow::MakeBuilder(type).ValueOrDie();

    for (const auto& v : getter_expr) {
        auto s = std::make_shared<arrow::StructScalar>(
            ToArrowRow(v), type);
        (void)builder->AppendScalar(*s);
    }

    row.push_back(std::make_shared<arrow::ListScalar>(
        *builder->Finish(),
        arrow::list(arrow::field("item", type, true))));
}
```

Depth-2 nested struct list `ToArrowRow()` must reproduce the existing monolithic branch shape, including the untyped final `ListScalar` in the non-null path:

```cpp
{
    auto coord_type = arrow::struct_(
        detail::ImportSchema(NestedClassSchema())->fields());
    auto inner_list_type = arrow::list(
        arrow::field("item", coord_type, true));
    auto outer_list_type = arrow::list(
        arrow::field("item", inner_list_type, true));

    if constexpr (nullable) {
        auto value = getter_expr;
        if (value == nullptr) {
            row.push_back(arrow::MakeNullScalar(outer_list_type));
        } else {
            auto outer_builder =
                arrow::MakeBuilder(inner_list_type).ValueOrDie();

            for (const auto& ring : *value) {
                auto inner_builder =
                    arrow::MakeBuilder(coord_type).ValueOrDie();

                for (const auto& v : ring) {
                    auto s = std::make_shared<arrow::StructScalar>(
                        ToArrowRow(v), coord_type);
                    (void)inner_builder->AppendScalar(*s);
                }

                (void)outer_builder->AppendScalar(
                    arrow::ListScalar(*inner_builder->Finish(),
                                      inner_list_type));
            }

            row.push_back(std::make_shared<arrow::ListScalar>(
                *outer_builder->Finish()));
        }
    } else {
        const auto& value = getter_expr;
        auto outer_builder =
            arrow::MakeBuilder(inner_list_type).ValueOrDie();

        for (const auto& ring : value) {
            auto inner_builder =
                arrow::MakeBuilder(coord_type).ValueOrDie();

            for (const auto& v : ring) {
                auto s = std::make_shared<arrow::StructScalar>(
                    ToArrowRow(v), coord_type);
                (void)inner_builder->AppendScalar(*s);
            }

            (void)outer_builder->AppendScalar(
                arrow::ListScalar(*inner_builder->Finish(),
                                  inner_list_type));
        }

        row.push_back(std::make_shared<arrow::ListScalar>(
            *outer_builder->Finish()));
    }
}
```

Depth-3 nested struct list follows the existing `ring_list_type` / `poly_list_type` pattern from `generator.cpp` lines 487-553:

```cpp
{
    auto coord_type = arrow::struct_(
        detail::ImportSchema(NestedClassSchema())->fields());
    auto ring_list_type = arrow::list(
        arrow::field("item", coord_type, true));
    auto poly_list_type = arrow::list(
        arrow::field("item", ring_list_type, true));
    auto outer_list_type = arrow::list(
        arrow::field("item", poly_list_type, true));

    if constexpr (nullable) {
        auto value = getter_expr;
        if (value == nullptr) {
            row.push_back(arrow::MakeNullScalar(outer_list_type));
        } else {
            auto outer_builder =
                arrow::MakeBuilder(poly_list_type).ValueOrDie();

            for (const auto& poly : *value) {
                auto poly_builder =
                    arrow::MakeBuilder(ring_list_type).ValueOrDie();

                for (const auto& ring : poly) {
                    auto ring_builder =
                        arrow::MakeBuilder(coord_type).ValueOrDie();

                    for (const auto& v : ring) {
                        auto s = std::make_shared<arrow::StructScalar>(
                            ToArrowRow(v), coord_type);
                        (void)ring_builder->AppendScalar(*s);
                    }

                    (void)poly_builder->AppendScalar(
                        arrow::ListScalar(*ring_builder->Finish(),
                                          ring_list_type));
                }

                (void)outer_builder->AppendScalar(
                    arrow::ListScalar(*poly_builder->Finish(),
                                      poly_list_type));
            }

            row.push_back(std::make_shared<arrow::ListScalar>(
                *outer_builder->Finish()));
        }
    } else {
        const auto& value = getter_expr;
        auto outer_builder =
            arrow::MakeBuilder(poly_list_type).ValueOrDie();

        for (const auto& poly : value) {
            auto poly_builder =
                arrow::MakeBuilder(ring_list_type).ValueOrDie();

            for (const auto& ring : poly) {
                auto ring_builder =
                    arrow::MakeBuilder(coord_type).ValueOrDie();

                for (const auto& v : ring) {
                    auto s = std::make_shared<arrow::StructScalar>(
                        ToArrowRow(v), coord_type);
                    (void)ring_builder->AppendScalar(*s);
                }

                (void)poly_builder->AppendScalar(
                    arrow::ListScalar(*ring_builder->Finish(),
                                      ring_list_type));
            }

            (void)outer_builder->AppendScalar(
                arrow::ListScalar(*poly_builder->Finish(),
                                  poly_list_type));
        }

        row.push_back(std::make_shared<arrow::ListScalar>(
            *outer_builder->Finish()));
    }
}
```

The nullable branch is controlled by the outer `ListNode.facts.nullable`. GIR-3 populates this fact for flattened-wrapper nested-list shapes; GIR-6 consumes it and should confirm it remains set by `BuildFieldIr`.

Unsupported nested scalar lists remain unsupported unless the current generator already emits them.

### Map

Map keys must be scalar. Values may be scalar or struct.

Scalar-value map view getter:

```cpp
fletcher::ArrowScalarMap<KeyType, KeyArrayType,
                         ValueType, ValueArrayType> name() const {
    const auto& ms =
        static_cast<const arrow::MapScalar&>(*scalars_[idx]);
    return fletcher::ArrowScalarMap<KeyType, KeyArrayType,
                                    ValueType, ValueArrayType>(ms.value);
}
```

Message-value map view getter:

```cpp
fletcher::ArrowRowViewMap<KeyType, KeyArrayType,
                          NestedClassView> name() const {
    const auto& ms =
        static_cast<const arrow::MapScalar&>(*scalars_[idx]);
    return fletcher::ArrowRowViewMap<KeyType, KeyArrayType,
                                     NestedClassView>(ms.value);
}
```

Scalar-value map `ToArrowRow()` uses value builders for both key and value arrays:

```cpp
{
    KeyBuilderType key_builder;
    ValueBuilderType val_builder;

    for (const auto& [k, v] : getter_expr) {
        (void)key_builder.Append(k);
        (void)val_builder.Append(v);
    }

    auto keys = *key_builder.Finish();
    auto vals = *val_builder.Finish();
    auto val_field = arrow::field("value", value_arrow_type, true);

    auto kv = *arrow::StructArray::Make(
        {keys, vals},
        {arrow::field("key", key_arrow_type, false), val_field});

    row.push_back(std::make_shared<arrow::MapScalar>(
        kv, arrow::map(key_arrow_type, val_field)));
}
```

Message-value map `ToArrowRow()` uses a value builder for keys and a pointer builder for struct values:

```cpp
{
    KeyBuilderType key_builder;
    auto val_type = arrow::struct_(
        detail::ImportSchema(MapValueClassSchema())->fields());
    auto val_builder = arrow::MakeBuilder(val_type).ValueOrDie();

    for (const auto& [k, v] : getter_expr) {
        (void)key_builder.Append(k);
        auto s = std::make_shared<arrow::StructScalar>(
            ToArrowRow(v), val_type);
        (void)val_builder->AppendScalar(*s);
    }

    auto keys = *key_builder.Finish();
    auto vals = *val_builder->Finish();
    auto val_field = arrow::field("value", val_type, true);

    auto kv = *arrow::StructArray::Make(
        {keys, vals},
        {arrow::field("key", key_arrow_type, false), val_field});

    row.push_back(std::make_shared<arrow::MapScalar>(
        kv, arrow::map(key_arrow_type, val_field)));
}
```

### Bridge retirement boundary

After GIR-6, these paths consume IR directly:

- encode visitor
- decode visitor
- schema/IPC visitor
- Arrow view getters
- `ToArrowRow()`

The temporary `IrNode -> FieldMapping` bridge remains for consumers not migrated by GIR-6:

- TypeScript generation until GIR-7
- RecordBatch accessor generation until RIR
- compatibility helpers still shared by those paths

GIR-6 must not delete the bridge wholesale.

## Forcing-test mapping

The forcing test belongs in the compile-and-run coverage harness, not in `protoc/tests`.

Use:

```text
integration-tests/protoc-coverage/
```

Do not place the codec round-trip test under:

```text
protoc/tests/
```

The `protoc/tests` suite is for in-process generator/IR unit tests and does not link the generated Arrow C++ plus codec path needed for this forcing test.

Primary forcing test:

```cpp
TEST(ViewVisitor, RoundTripsViaCodec) {
    CoverageMessage original = MakeCoverageMessage();

    auto row = ToArrowRow(original);

    auto encoded = EncodeViaCodec(row, CoverageMessageSchema());
    auto decoded_row = DecodeViaCodec(encoded, CoverageMessageSchema());

    CoverageMessageView view(decoded_row);

    CoverageMessage reconstructed;
    SetFromViewOrAccessors(reconstructed, view);

    EXPECT_TRUE(Equals(original, reconstructed));

    EXPECT_EQ(view.scalar_field(), expected_scalar);
    ASSERT_TRUE(view.nullable_struct().has_value());
    EXPECT_EQ(view.nullable_struct()->inner(), expected_inner);
    EXPECT_EQ(CollectMapKeys(view.scalar_map()), expected_keys);
    EXPECT_EQ(CollectMapValues(view.scalar_map()), expected_values);
    EXPECT_EQ(CollectMapMessageValues(view.message_map()), expected_message_values);
}
```

The fixture should exercise:

- nullable scalar
- non-nullable scalar
- enum scalar
- string/bytes buffer scalar
- wrapper WKT nullable scalar
- timestamp/duration WKT scalar
- nullable struct
- repeated scalar
- repeated struct
- nullable flattened-wrapper nested list depth 2
- nullable flattened-wrapper nested list depth 3, if present
- map with scalar value
- map with message value

The important assertion is that data is read back through generated `...View` accessors after a `ToArrowRow()` plus codec round trip.

Additional pure generator tests may live under `protoc/tests` only if they inspect visitor classification or emitted snippets without linking generated Arrow C++ codec output.

The green gates are:

```text
1. protoc unit suite with forced rebuild:
   --build=fletcher-protoc/*

2. coverage harness:
   integration-tests/protoc-coverage/
   reconstruct + encode/decode + schema .ipc oracles
   including ViewVisitor.RoundTripsViaCodec

3. no-drift gates:
   RBA no-drift
   TypeScript still green through the FieldMapping bridge
```

Generated-source golden churn is acceptable only after review confirms behavior identity. Schema `.ipc` output should remain byte-identical.

## Risks & Unknowns

The highest risk is nested-list classification. The IR has structural nested `ListNode`s, not a `list_depth` field. The visitor must count list depth, find the leaf `StructNode`, and emit the existing monolithic depth-2/depth-3 shapes.

Nullable flattened-wrapper nested lists depend on `ListNode.facts.nullable` being populated on the outer list by GIR-3. GIR-6 should verify this construction fact before relying on it for `MakeNullScalar`.

`ToArrowRow()` must not apply default handling. It reads public getters, so only nullable vs non-nullable branching belongs there.

Map emission is sensitive to value-builder vs pointer-builder mechanics. Repeated scalar and scalar map values use direct builders with `.Append()` and `.Finish()`. Struct list/map values use `arrow::MakeBuilder(...)` pointer builders with `->AppendScalar()` and `->Finish()`.

String and bytes ownership must remain unchanged. View getters may return `std::string_view` into Arrow buffers, but `ToArrowRow()` must pass owned `std::string` into Arrow scalar construction.

Array-type derivation must stay in the C++ backend lookup layer. Do not add Arrow array names or C++ type spellings to IR.

RBA and TypeScript remain on the bridge. GIR-6 must not refactor or retire those consumers.

If migration reveals a real existing behavior bug, stop and handle it as a behavior fix under the locked decision process instead of hiding it inside the IR migration.

## Files-to-touch

```text
protoc/src/cpp_backend_view_visitor.hpp
protoc/src/cpp_backend_view_visitor.cpp
```

Add the IR-driven view getter and `ToArrowRow()` visitors.

```text
protoc/src/cpp_backend_type_table.hpp
protoc/src/cpp_backend_type_table.cpp
```

Expose any missing backend lookup fields needed by the visitors, especially scalar array type and getter type. Keep all Arrow/C++ spelling backend-local.

```text
protoc/src/generator.cpp
```

Replace the live `EmitViewGetters` and `GenerateToArrowRow` internals with IR visitor calls.

Delete or stop referencing live-path bridge helpers only where they become unused by view getters and `ToArrowRow()`. Keep bridge usage for TypeScript and RBA.

```text
integration-tests/protoc-coverage/
```

Add `ViewVisitor.RoundTripsViaCodec` to the compile-and-run coverage harness.

Update generated coverage expectations only where source layout changes without behavior changes.

```text
integration-tests/protoc-coverage/*
```

Use this path for coverage fixture, generated-source, schema, and round-trip updates. Do not use `tests/coverage/*`.

```text
protoc/tests/*
```

Only add tests here for pure IR/visitor shape assertions that do not require generated Arrow C++ codec linkage.

## Dead-code cleanup

These helpers are outside GIR-6 migration scope because they are already dead:

```text
SetFromScalars_
ToScalars
Make*Scalar_
```

Delete the orphaned generator helpers that emit them, including the anonymous-namespace helpers currently responsible for field extraction and scalar helper generation, such as:

```text
EmitFieldExtraction
EmitScalarHelper
ScalarEntry-related ToScalars emission
```

The cleanup rule is simple:

- delete dead `SetFromScalars_` / `ToScalars` / `Make*Scalar_` emitters
- do not migrate them to IR visitors
- do not add tests for them
- confirm no callers remain after deletion
- keep the live `ToArrowRow()` and `...View` generated surfaces covered by `integration-tests/protoc-coverage/`

## Step-2 review (2026-07-10, re-review of full rewrite)

**Verdict: APPROVE.** All four prior blockers are resolved and the four
non-blocking items are folded. No locked-decision deviation; no regression to the
blessed node-by-node faithfulness. Verified against the real generator + generated
header + coverage harness.

- **Blocker 1 (scope / dead code) — RESOLVED.** Design migrates ONLY the live
  view getters + `ToArrowRow()` and DELETES the dead helpers. Confirmed
  `EmitScalarHelper` / `EmitFieldExtraction` / `ScalarEntry` have **zero call
  sites** in `protoc/src`, the generated `coverage.fletcher.arrow.pb.h` has
  **0** `ToScalars` / `SetFromScalars_` / `Make*Scalar_` hits and **122**
  `ToArrowRow`/`View` hits — matching the doc's claim exactly.
- **Blocker 2 (nested-list) — RESOLVED.** `ir.hpp` confirms `ListNode` has no
  `list_depth` (only `unique_ptr<IrNode> element`); nesting is structural. Design
  reproduces whole-shape depth-2 (`ArrowNestedList`) / depth-3
  (`ArrowNestedList2`) with leaf-struct identity, not naive per-level recursion,
  matching live `EmitViewGetters` + `GenerateToArrowRow`.
- **Blocker 3 (no double-default) — RESOLVED.** Non-nullable scalar getters emit
  `value_or(default)` (generator.cpp:372) / empty-buffer (:366-369), so
  `ToArrowRow()` reads the public getter and branches only on nullable vs
  non-nullable. `ValueAccessMode` is correctly not re-threaded.
- **Blocker 4 (test home) — RESOLVED.** `ViewVisitor.RoundTripsViaCodec` is homed
  in `integration-tests/protoc-coverage/` (doc explicitly forbids `protoc/tests`).
  That harness already `#include`s `coverage.fletcher.arrow.pb.h` and calls
  `ToArrowRow`; read-back covers map keys/values + struct inner fields.
- **Non-blocking 5-8 — FOLDED.** value-vs-pointer builder mechanics distinguished
  (value `.Append()/.Finish()` for repeated-scalar + scalar map; `MakeBuilder`
  pointer `->AppendScalar()/->Finish()` for struct list/map values); array-type
  kept in backend, not IR; wrapper-WKT on the generic nullable scalar path
  (`FieldFacts.wkt` = `WRAPPER_*` + `nullable`); `IrNode.facts.nullable` confirmed
  as the outer-list nullability source with a `BuildFieldIr` verification step.

Locked decisions honoured: #1 (IR language-neutral; all Arrow/C++ spelling stays
in the backend lookup layer), #3/#4 (RBA + TS stay on the bridge — RBA until RIR,
TS until GIR-7; view migrated behind the round-trip-via-codec oracle), #7 (enum
lowers to int32 physical, dictionary not modelled as a container), #8 (bespoke
edge path untouched), #2 (schema `.ipc` byte-identical; source golden churn only).

Non-blocking corrections for the implementer (do not gate):

1. **Header paths.** `Files-to-touch` lists `protoc/src/cpp_backend_view_visitor.hpp`
   and `protoc/src/cpp_backend_type_table.hpp`, but this repo homes headers in
   `protoc/include/` (existing `cpp_backend_schema_visitor.hpp` /
   `cpp_backend_decode_visitor.hpp`; `cpp_backend_type_table.hpp` already lives at
   `protoc/include/`). New visitor pair should be
   `protoc/include/cpp_backend_view_visitor.hpp` + `protoc/src/…​.cpp`, and the
   type-table edit targets the existing `protoc/include/cpp_backend_type_table.hpp`.
2. **Depth-3 local names.** The doc's depth-3 `ToArrowRow()` snippet renames the
   intermediate builders (`poly_builder`/`ring_builder`) vs the current generator's
   `mid_builder`/`inner_builder`. Cosmetic only (generated local-variable golden
   churn); behaviour and schema `.ipc` are unaffected. Keeping the current names
   minimises golden churn.
3. **Forcing-test placeholders.** The `RoundTripsViaCodec` snippet uses illustrative
   names (`CoverageMessage`, `EncodeViaCodec`, `SetFromViewOrAccessors`, …); the real
   fixture type is `CompositeCoverage` (namespace
   `fletcher_gen::integration::coverage`) with `CompositeCoverageSchema()`. Map to
   the real fixture when implementing.
