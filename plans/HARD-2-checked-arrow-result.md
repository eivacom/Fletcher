# HARD-2: Runtime-Only Checked Arrow Result Access

## Summary

Replace the 13 hand-written runtime `.ValueOrDie()` calls with one checked helper that converts failed Arrow `Result<T>` values into catchable `std::invalid_argument` exceptions.

Scope is only the hand-written runtime half of #53:

- `arrow-bridge/src/codec.cpp`: 9 sites, re-confirmed at current lines 96, 139, 153, 225, 238, 281, 291, 306, 307.
- `arrow-bridge/include/fletcher/arrow_bridge/arrow_row_view.hpp`: 4 sites, re-confirmed at current lines 74, 108, 145, 238.

Out of scope:

- The 25 generator-emitted `.ValueOrDie()` sites. Those remain for the GEN round per H-INV-5 and locked decisions #7/#11.

This changes error signalling only. It must not alter encoded bytes, decoded values, public API signatures, callback behavior, or generated output.

## Design

Add one small Arrow `Result<T>` helper, used by both runtime files:

```cpp
namespace fletcher::detail {

template <typename T>
T ValueOrThrow(arrow::Result<T>&& result, const char* operation) {
    if (!result.ok()) {
        throw std::invalid_argument(std::string(operation) + ": " +
                                    result.status().ToString());
    }
    return std::move(result).ValueUnsafe();
}

}  // namespace fletcher::detail
```

Implementation placement is the installed detail header:

```text
include/fletcher/arrow_bridge/detail/arrow_result.hpp
```

This header is included by the public installed header `arrow-bridge/include/fletcher/arrow_bridge/arrow_row_view.hpp`, so it must be installed or downstream consumers fail to compile. The `arrow-bridge` package uses `conanfile.py` with a recursive wildcard copy of all `*.hpp` files from the source `include/` tree, preserving directory structure (`copy(self, "*.hpp", src=..., keep_path=True)`). The new `detail/` subdirectory and its `arrow_result.hpp` header are automatically included in the installed package; no enumerated file list update or CMakeLists.txt change is required.

The helper header must include:

```cpp
#include <arrow/result.h>
#include <stdexcept>
#include <string>
#include <utility>
```

If the project's Arrow version prefers `MoveValueUnsafe()` over rvalue `ValueUnsafe()`, use that extraction after the explicit `ok()` check. The invariant is that the helper checks first, then performs unchecked extraction only on the success path.

Both `arrow-bridge/src/codec.cpp` and `arrow-bridge/include/fletcher/arrow_bridge/arrow_row_view.hpp` include the helper header and use operation-specific messages at each call site so failures name the runtime operation that failed. In both files, the call sites resolve `detail::ValueOrThrow` as `fletcher::detail::ValueOrThrow`.

For `codec.cpp`, the proposed replacements are:

```cpp
auto elem = detail::ValueOrThrow(arr->GetScalar(i),
                                 "Codec: list GetScalar failed");
auto key = detail::ValueOrThrow(key_arr->GetScalar(i),
                                "Codec: map key GetScalar failed");
auto val = detail::ValueOrThrow(val_arr->GetScalar(i),
                                "Codec: map value GetScalar failed");

auto builder = detail::ValueOrThrow(arrow::MakeBuilder(elem_type),
                                    "Codec: list MakeBuilder failed");
return detail::ValueOrThrow(builder->Finish(),
                            "Codec: list builder Finish failed");

auto key_builder = detail::ValueOrThrow(arrow::MakeBuilder(map_type.key_type()),
                                        "Codec: map key MakeBuilder failed");
auto val_builder = detail::ValueOrThrow(arrow::MakeBuilder(map_type.item_type()),
                                        "Codec: map value MakeBuilder failed");

auto key_arr = detail::ValueOrThrow(key_builder->Finish(),
                                    "Codec: map key builder Finish failed");
auto val_arr = detail::ValueOrThrow(val_builder->Finish(),
                                    "Codec: map value builder Finish failed");
```

For `arrow_row_view.hpp`, include the helper header and replace the four accesses:

```cpp
ViewT operator[](int64_t i) const {
    return ViewT(detail::ValueOrThrow(array_->GetScalar(i),
                                      "ArrowRowViewList: GetScalar failed"));
}
```

Use equivalent operation names for `ArrowNestedList`, `ArrowNestedList2`, and `ArrowRowViewMap`.

Do not change any public method signatures. The existing return types and call syntax remain unchanged; only failed Arrow `Result<T>` handling changes from process abort to `std::invalid_argument`.

Do not catch these exceptions in codec or row-view code. H-INV-3 is not directly involved because these files are not DDS/XRCE callback boundaries. Provider callback code remains responsible for preventing exceptions from escaping middleware callbacks.

## Forcing-test mapping

Add `CodecTest.BadResultThrowsInsteadOfAbort` in `arrow-bridge/tests/test_codec.cpp`.

**Pre-fix behaviour:** The test must demonstrate that before the fix, the process aborts due to `.ValueOrDie()` calling `abort()` on failed `Result`. This is valid red-for-the-right-reason (process termination, not a catchable exception). The test binary crashes when it attempts to access an out-of-range element via the row-view operator.

**Post-fix behaviour:** After the fix, the same failed `Result` is converted to a catchable `std::invalid_argument` exception.

Test shape:

```cpp
#include <fletcher/arrow_bridge/arrow_row_view.hpp>

namespace {

class DummyStructView {
   public:
    explicit DummyStructView(std::shared_ptr<arrow::Scalar> scalar)
        : scalar_(std::move(scalar)) {}

   private:
    std::shared_ptr<arrow::Scalar> scalar_;
};

}  // namespace

TEST(CodecTest, BadResultThrowsInsteadOfAbort) {
    auto stype = arrow::struct_({arrow::field("x", arrow::int32())});

    std::shared_ptr<arrow::Array> array;
    arrow::StructBuilder builder(stype, arrow::default_memory_pool(),
                                 {std::make_shared<arrow::Int32Builder>()});
    ASSERT_TRUE(builder.Append().ok());
    auto* x_builder = static_cast<arrow::Int32Builder*>(builder.field_builder(0));
    ASSERT_TRUE(x_builder->Append(7).ok());
    ASSERT_TRUE(builder.Finish(&array).ok());

    fletcher::ArrowRowViewList<DummyStructView> views(array);

    EXPECT_THROW(static_cast<void>(views[1]), std::invalid_argument);
}
```

The `DummyStructView` class satisfies `ArrowRowViewList<ViewT>`'s contract (documented in `arrow_row_view.hpp` lines 59-60): it is constructible from `std::shared_ptr<arrow::Scalar>`. The test constructs a struct array with one element (index 0 valid) and attempts out-of-range access (index 1), which causes `array_->GetScalar(1)` to return a failed `Result`.

Pre-fix, `views[1]` reaches `array_->GetScalar(1).ValueOrDie()` and the process aborts (test binary crashes).

Post-fix, the same failed `Result` is caught and converted to `std::invalid_argument`, which the test captures with `EXPECT_THROW`.

The test assertion must be `EXPECT_THROW(..., std::invalid_argument)`, not a death test and not `std::runtime_error`.

Add a second assertion that exercises the `codec.cpp`-path by inducing a builder operation failure. For example, add an additional test case that creates a list with an unsupported element type or triggers a `Finish()` failure on a builder, and verify that the helper converts this failure to `std::invalid_argument`. This ensures both the row-view (`arrow_row_view.hpp`) and codec (`codec.cpp`) paths are exercised and not just the row-view path.

Existing codec round-trip and byte-identity tests remain the H-INV-1 regression guard. HARD-2 should not add, remove, or reorder encoded bytes.

## Risks & Unknowns

Arrow extraction API spelling may vary by version. The implementer should use the available post-check unsafe extraction API, normally `ValueUnsafe()` or `MoveValueUnsafe()`, without reintroducing `.ValueOrDie()`.

The row-view helper must live in an installed header because the affected row-view methods are templates. Keeping it under `fletcher/arrow_bridge/detail` avoids making it part of the intentional public API while still satisfying header-only template use. The `detail/` subdirectory automatically ships in the installed package via the conanfile's recursive `*.hpp` copy.

The helper throws `std::invalid_argument` for every failed Arrow `Result<T>` in this HARD-2 scope. That matches H-INV-2 and locked decision #2 because these are runtime data/access/builder failures on recoverable paths. No site in the 13-site scope should be converted to `std::runtime_error` unless a locked decision is changed; otherwise STOP-AND-ASK.

No generator files, generated fixtures, or committed `.fletcher.*` outputs should be touched. Doing so conflicts with H-INV-5 and locked decisions #7/#11; if implementation appears to require it, STOP-AND-ASK.

## Files-to-touch

```text
arrow-bridge/include/fletcher/arrow_bridge/detail/arrow_result.hpp
arrow-bridge/include/fletcher/arrow_bridge/arrow_row_view.hpp
arrow-bridge/src/codec.cpp
arrow-bridge/tests/test_codec.cpp
```

## Step-2 review (cycle 2) — 2026-07-09

**Verdict: APPROVE.**

All three cycle-1 rework items are resolved, and the previously-approved
substance is intact.

1. **Install rule — RESOLVED.** The doc (Design section) now states the header
   ships via the conanfile's recursive `copy(self, "*.hpp", src=.../include,
   dst=..., keep_path=True)`. Independently confirmed against
   `arrow-bridge/conanfile.py:61-65` (`package()`): the wildcard + `keep_path=True`
   copies the new `detail/` subtree automatically. No enumerated-list or
   `CMakeLists.txt` edit is claimed, and Files-to-touch correctly lists only the
   four files (no spurious CMake edit).

2. **Exact path + both includers — RESOLVED.** The doc commits to
   `include/fletcher/arrow_bridge/detail/arrow_result.hpp` (Design + Files-to-touch,
   no hedging) and explicitly states BOTH `src/codec.cpp` and the public
   `arrow_row_view.hpp` include it. The only remaining conditional
   (`ValueUnsafe()` vs `MoveValueUnsafe()`) is legitimate Arrow-version
   portability, not path/includer hedging.

3. **Pre-fix red + codec-path assertion — RESOLVED.** Forcing-test mapping
   documents `.ValueOrDie()` → `abort()` as valid red-for-the-right-reason
   (process termination — matches locked decision #9's named reasons), commits to
   `EXPECT_THROW(std::invalid_argument)` (explicitly not a death test, not
   `std::runtime_error`), verifies the `DummyStructView` fixture against the
   `ArrowRowViewList<ViewT>` contract (`arrow_row_view.hpp:59-60`), and requires a
   second assertion exercising a `codec.cpp`-path `Result<T>` failure
   (builder/Finish/MakeBuilder), not only the row-view path.

**No regression in approved substance.** All 13 sites match SPEC exactly
(codec.cpp 96/139/153/225/238/281/291/306/307; arrow_row_view.hpp
74/108/145/238). `ValueOrThrow<T>` is sound (ok()-check-first, then unchecked
extract on the success path). `std::invalid_argument` matches H-INV-2 / locked
decision #2 and is the type SPEC HARD-2 acceptance names. Generator's 25 sites
left untouched (H-INV-5 / locked #7, STOP-AND-ASK guard present). Happy-path
byte-identity / no-signature-change preserved (H-INV-1 / H-INV-4).

**Non-blocking advisory (implementer):** confirm the codec-path failure is
actually inducible through a public entry point (e.g. an `EncodeRow`/`DecodeRow`
input that forces `MakeBuilder`/`Finish` to fail); if it proves un-inducible from
the public API, exercising `detail::ValueOrThrow` on a deliberately-failed
`arrow::Result<T>` still satisfies H-INV-2 but does not cover the codec.cpp call
sites — surface that rather than dropping the codec-path coverage.
