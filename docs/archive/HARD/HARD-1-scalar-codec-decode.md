# HARD-1: Fix DecodeScalarFromReader Defects

## Summary

HARD-1 fixes two scoped defects in `arrow-bridge/src/scalar_codec.cpp` inside `fletcher::detail::DecodeScalarFromReader` without changing the wire format or the function signature.

The first defect is memory safety: the `FIXED_SIZE_BINARY` decode case at `scalar_codec.cpp:272-278` reads bytes from `Reader` and constructs `std::make_shared<arrow::Buffer>(ptr, byte_width)` at `:276`. That Arrow buffer aliases the reader's transient source memory instead of owning a copy, so the returned `arrow::FixedSizeBinaryScalar` can read dangling bytes after the encoded row/source buffer is freed or overwritten.

The second defect is code shape / dead code: the string/binary block at `scalar_codec.cpp:223-250` has an inner switch whose six valid cases all return, but the inner `default: break` and outer `break` fall through to the function-tail duplicate throw at `:317`. That tail throw duplicates the reachable outer `default` throw at `:313-315` and is not a meaningful runtime path for the narrowed string/binary case.

The design preserves H-INV-1, H-INV-2, H-INV-4, and locked decisions #1, #2, #9, and #11. No generator or public API work is in scope.

## Design

For #52, replace the non-owning fixed-size-binary buffer construction with an owned copy, mirroring the existing string/binary decode branch.

Current string/binary branch pattern at `scalar_codec.cpp:229-232`:

```cpp
uint32_t len = r.Read<uint32_t>();
const uint8_t* ptr = r.ReadBytes(len);
auto ibuf =
    arrow::Buffer::FromString(std::string(reinterpret_cast<const char*>(ptr), len));
```

Current fixed-size-binary branch at `scalar_codec.cpp:272-278`:

```cpp
case T::FIXED_SIZE_BINARY: {
    const int32_t byte_width =
        static_cast<const arrow::FixedSizeBinaryType&>(*type).byte_width();
    const uint8_t* ptr = r.ReadBytes(byte_width);
    auto ibuf = std::make_shared<arrow::Buffer>(ptr, byte_width);
    return std::make_shared<arrow::FixedSizeBinaryScalar>(ibuf, type);
}
```

Change only the ownership step:

```cpp
auto ibuf = arrow::Buffer::FromString(
    std::string(reinterpret_cast<const char*>(ptr), byte_width));
```

Then keep the existing scalar construction:

```cpp
return std::make_shared<arrow::FixedSizeBinaryScalar>(ibuf, type);
```

This changes only the lifetime of decoded bytes. The reader still consumes exactly `byte_width` bytes, and the scalar still exposes the same bytes. H-INV-1 is preserved because no encoded or decoded byte layout changes.

For #58, make the string/binary block self-terminating by replacing the impossible inner `default: break` / outer `break` fall-through with a local throw or by removing the inner default and ensuring every path in the block returns or throws. The cleanest implementation is:

```cpp
            switch (type->id()) {
                case T::STRING:
                    return std::make_shared<arrow::StringScalar>(ibuf);
                case T::LARGE_STRING:
                    return std::make_shared<arrow::LargeStringScalar>(ibuf);
                case T::BINARY:
                    return std::make_shared<arrow::BinaryScalar>(ibuf);
                case T::LARGE_BINARY:
                    return std::make_shared<arrow::LargeBinaryScalar>(ibuf);
                case T::STRING_VIEW:
                    return std::make_shared<arrow::StringViewScalar>(ibuf);
                case T::BINARY_VIEW:
                    return std::make_shared<arrow::BinaryViewScalar>(ibuf);
                default:
                    throw std::invalid_argument("DecodeScalar: unsupported Arrow type: " +
                                                type->ToString());
            }
```

Then delete the function-tail duplicate throw at `scalar_codec.cpp:317`, keeping the reachable outer `default` at `:313-315`.

Deleting `:317` is safe because every outer switch case either returns or throws. The only previous non-returning path inside the string/binary group was the impossible inner default, and the revised block makes that path explicit. The reachable unsupported-type behavior remains the outer `default`, which throws `std::invalid_argument`.

The signature remains unchanged:

```cpp
std::shared_ptr<arrow::Scalar> DecodeScalarFromReader(
    Reader& r, const std::shared_ptr<arrow::DataType>& type)
```

No public API, generator, or wire-format changes are part of this item.

## Forcing-test mapping

Tests should live in the existing `arrow-bridge/tests/test_codec.cpp`. That file already contains scalar codec round-trip coverage, is already the only source in `arrow_bridge_tests`, and adding these focused codec cases there avoids a CMake target split for a narrow fix. A later cleanup can split scalar-specific tests into `test_scalar_codec.cpp`, but that is not needed for HARD-1.

Add a fixed-size-binary ownership regression test for #52. It should:
1. Construct a test-owned mutable buffer (e.g. `std::vector<uint8_t>` or `std::string`) containing known bytes
2. Create a `Reader` that reads from that buffer
3. Decode the `arrow::FixedSizeBinaryScalar` using `DecodeScalarFromReader`
4. After decode, **overwrite the exact backing bytes** in the original buffer with a known-different byte pattern (e.g. 0xAA or a different byte sequence)
5. Assert the decoded scalar still exposes the original byte sequence, not the overwritten bytes

Red-first behavior: Before the fix, the decoded scalar's buffer aliases the test-owned source memory through `std::make_shared<arrow::Buffer>(ptr, byte_width)`. When the test overwrites the backing buffer region post-decode, the scalar's read will expose the new overwritten bytes, causing the assertion to fail. After the fix, the scalar owns a copy via `arrow::Buffer::FromString(std::string(...))`, so overwriting the test's buffer does not affect the scalar's bytes, and the assertion passes. Overwriting (not freeing) is critical: freeing then reading the alias is undefined behavior and commonly returns the stale-correct bytes, giving a false-green before the fix that defeats locked decision #9's intent.

Add round-trip coverage for every string/binary/fixed-size-binary variant:

```cpp
arrow::utf8()
arrow::large_utf8()
arrow::binary()
arrow::large_binary()
arrow::utf8_view()
arrow::binary_view()
arrow::fixed_size_binary(n)
```

This exercises #52 through fixed-size binary and #58 through all six string/binary inner-switch return arms. It also confirms that H-INV-1 remains true for the affected scalar families.

Add an unsupported-type negative test that decodes a concrete Arrow type unsupported by `DecodeScalarFromReader` and expects `std::invalid_argument`, not abort. A practical candidate is `arrow::list(arrow::int32())`, which does not match any outer switch case in `DecodeScalarFromReader` and will reach the outer `default` throw at `scalar_codec.cpp:313-315`. This test is a **green regression guard**: it confirms the reachable outer `default` (unsupported-type throw) is preserved after deleting the duplicate tail throw at `:317`. The outer `default` already throws pre-fix, so this is not a red-first test for locked decision #9; it is a structural regression guard ensuring no case falls off the function without a return or throw.

## Risks & Unknowns

`arrow::Buffer::FromString` is the right local pattern for owned copies because it is already used in the string/binary decode branch at `scalar_codec.cpp:231-232`. The fixed-size-binary branch should mirror that ownership behavior.

The inner switch edge case in #58 is that the `default` is unreachable under normal C++ control flow because the outer switch has already selected only `STRING`, `LARGE_STRING`, `BINARY`, `LARGE_BINARY`, `STRING_VIEW`, or `BINARY_VIEW`. Still, making the inner default throw keeps the block total and avoids relying on the function-tail throw.

Deleting the tail throw at `scalar_codec.cpp:317` should not expose a fall-off path if every outer case is audited during implementation. The implementation should build with compiler warnings cleanly after removal, specifically with no `-Wreturn-type` warnings (MSVC C4715) signalling that control can reach the end of a non-void function.

## #9 / #58 ruling — EXEMPT, non-behavioural (settled)

Locked decision #9 requires a red-first test for every *behavioural* fix. #58 removes provably-unreachable dead code: the outer switch narrows `type->id()` to the six string ids before the inner switch (lines 223-228), so the inner `default` and the tail throw at `:317` can never execute in any program path. Removal of unreachable code produces zero observable change; #58 is therefore not a behavioural fix and is exempt from the red-first requirement.

This is the same class of exemption #9 already grants HARD-6's annotation half — a compile-time proof (e.g. discard-on-compile-fails) stands in for a runtime red-first test. The validation for #58 is: (i) the unreachability argument above, and (ii) a **clean build with compiler warnings-as-error enabled after deleting `:317`**, specifically no `-Wreturn-type` / MSVC C4715 warnings ("control reaches end of non-void function"), which is the structural proof that no outer case falls off the end. The unsupported-type regression test is a green structural guard, not a #9 red-first test.

## Files to touch

`arrow-bridge/src/scalar_codec.cpp`

Implement the owned-copy fixed-size-binary decode and remove the duplicate function-tail throw after making the string/binary block self-terminating.

`arrow-bridge/tests/test_codec.cpp`

Add the fixed-size-binary ownership regression (overwrite-based), expanded string/binary/fixed-size-binary variant round-trips, and unsupported-type `std::invalid_argument` test with a concrete type (`arrow::list(arrow::int32())`).

## Step-2 review (cycle 2, 2026-07-09)

**Verdict: APPROVE.**

Cycle-1 substance stays intact; the three cycle-1 rework items are correctly resolved.

1. **#52 test is overwrite-based (resolved).** The forcing test (lines 92-99) decodes
   from a test-owned mutable buffer, overwrites the exact backing region post-decode,
   and asserts the scalar still reads the originals. The rationale is stated explicitly:
   overwriting (not freeing) avoids the false-green where freeing-then-reading returns
   stale-correct bytes as UB. This matches the spec acceptance and honours locked
   decision #9's intent. (Premise that `ReadBytes` yields a pointer aliasing the source
   buffer is the spec-asserted bug definition — §HARD-1 #52.)
2. **Unsupported-type test names a provably-`default`-reaching type, labelled a green
   guard (resolved).** Verified against `arrow-bridge/src/scalar_codec.cpp`: the
   `DecodeScalarFromReader` switch (lines 199-316) has **no `T::LIST` arm**, so
   `arrow::list(arrow::int32())` (type id `LIST`) falls to the outer `default` throw at
   :313-315. The doc correctly frames this as a structural green regression guard (not a
   #9 red-first test), since the outer `default` already throws pre-fix and the guard only
   proves it survives deleting the duplicate tail throw at :317.
3. **#58 is EXEMPT/settled, no stop-and-ask (resolved).** Section at lines 125-129 states
   #58 as a non-behavioural dead-code removal, exempt from red-first, validated by (i) the
   unreachability argument and (ii) a warnings-as-error build clean of `-Wreturn-type` /
   MSVC C4715. No "stop-and-ask" framing remains anywhere in the doc.

Substance not regressed: #52 owned-copy via `arrow::Buffer::FromString` mirrors the string
branch (H-INV-1 preserved, bytes unchanged); #58 makes the inner block self-terminating and
deletes the unreachable tail throw while keeping the reachable outer `default` (H-INV-2:
unsupported type throws `std::invalid_argument`, never aborts); signature unchanged (H-INV-4);
scope is arrow-bridge-only (H-INV-5, locked #7/#11) with a plausible two-file `Files-to-touch`.

Non-blocking note (implementation, not design): when the inner switch becomes total
(`default: throw`), remove the now-dead outer `break;` after the inner switch so the
warnings-as-error build does not trip MSVC C4702 (unreachable code) — the doc already
implies this by omitting the trailing `break` from the shown block.
