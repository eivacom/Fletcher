# HARD-7 — Consolidate Duplicated Helpers

## Summary

HARD-7 is a behaviour-preserving refactor that gives `JoinSegments`, `AppendFixed<T>`, and `BitfieldBytes` one source of truth each. The design deletes duplicated helper bodies, re-points call sites to shared definitions, and keeps all emitted bytes and wire bytes unchanged per H-INV-1 and locked decision #6.

Three helpers, three homes:
1. **`JoinSegments`** — consolidated to existing shared definition in `pubsub/include/fletcher/pubsub/internal/segments.hpp`.
2. **`AppendFixed<T>`** — consolidated to existing private source header `arrow-bridge/src/scalar_codec.hpp` (both call sites already include it).
3. **`BitfieldBytes`** — consolidated to new public `core/include/fletcher/core/detail/bitfield.hpp` (required by both codec and positional I/O paths).

The only signedness-sensitive part is `BitfieldBytes`: the shared definition must keep the codec path's `int64_t` count arithmetic to preserve overflow-safe intent (locked decision #6).

## Design

### `JoinSegments`

RE-GREP confirmed current locations:

- Existing shared definition: `pubsub/include/fletcher/pubsub/internal/segments.hpp:17`
- Duplicate definition: `xrcedds-pubsub-provider/src/xrce_dds_pubsub_provider.cpp:59`
- Duplicate definition: `pubsub-arrow/src/publisher_arrow.cpp:80`
- Duplicate definition: `pubsub-arrow/src/subscriber_arrow.cpp:444`
- Mock join helpers:
  - `pubsub/tests/test_publisher_subscriber.cpp:75`
  - `pubsub-arrow/tests/test_pubsub_arrow.cpp:76`
  - `gateway/src/main.cpp:127`

Single-source home stays unchanged:

```cpp
namespace fletcher {
namespace internal {

inline std::string JoinSegments(const std::vector<std::string>& segs);

}  // namespace internal
}  // namespace fletcher
```

Edits:

- Add `#include <fletcher/pubsub/internal/segments.hpp>` to:
  - `xrcedds-pubsub-provider/src/xrce_dds_pubsub_provider.cpp`
  - `pubsub-arrow/src/publisher_arrow.cpp`
  - `pubsub-arrow/src/subscriber_arrow.cpp`
  - `pubsub/tests/test_publisher_subscriber.cpp`
  - `pubsub-arrow/tests/test_pubsub_arrow.cpp`
  - `gateway/src/main.cpp`
- Delete the anonymous-namespace `JoinSegments` body from `xrcedds-pubsub-provider/src/xrce_dds_pubsub_provider.cpp`.
- Delete `PublisherArrow::JoinSegments` and `SubscriberArrow::JoinSegments` definitions.
- Delete their private declarations from:
  - `pubsub-arrow/include/fletcher/pubsub_arrow/publisher_arrow.hpp`
  - `pubsub-arrow/include/fletcher/pubsub_arrow/subscriber_arrow.hpp`
- Replace call sites with `internal::JoinSegments(...)`.
- In mocks/gateway, delete the local `Join` loop and replace call sites directly with `fletcher::internal::JoinSegments(...)`. No join logic remains outside the shared header.

Dependency direction is valid:

- `xrcedds-pubsub-provider` links `fletcher-pubsub` publicly.
- `pubsub-arrow` links `fletcher-pubsub` publicly.
- `pubsub` tests link `fletcher-pubsub`.
- `pubsub-arrow` tests link `fletcher-pubsub-arrow`, which links `fletcher-pubsub`.
- `gateway` links `fletcher-pubsub`.

### `AppendFixed<T>`

RE-GREP confirmed current locations:

- `arrow-bridge/src/codec.cpp:27`
- `arrow-bridge/src/scalar_codec.cpp:19`

Both definitions are byte-identical:

```cpp
template <typename T>
void AppendFixed(std::vector<uint8_t>& buf, T value) {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&value);
    buf.insert(buf.end(), bytes, bytes + sizeof(T));
}
```

Single-source home:

- Define in the existing private source header `arrow-bridge/src/scalar_codec.hpp`.

Chosen home rationale:

- Both call sites (`codec.cpp` and `scalar_codec.cpp`) already include `scalar_codec.hpp`, so zero new includes required.
- Keeps the helper private — no public API surface expansion (respects H-INV-4).
- Honours locked decision #6: prefer deleting duplicates and including an existing source over creating a new header.
- The stated objection ("pulls Arrow deps") is moot because `codec.cpp:16` already includes `scalar_codec.hpp`.

Signature in `arrow-bridge/src/scalar_codec.hpp`:

```cpp
namespace fletcher {

template <typename T>
inline void AppendFixed(std::vector<uint8_t>& buf, T value) {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&value);
    buf.insert(buf.end(), bytes, bytes + sizeof(T));
}

}  // namespace fletcher
```

Edits:

- Define the template once in `arrow-bridge/src/scalar_codec.hpp` (include guards ensure no multi-definition hazard in `.cpp` translation units).
- Delete both anonymous-namespace `AppendFixed<T>` definitions from `codec.cpp` and `scalar_codec.cpp`.
- Replace call sites with unqualified `AppendFixed(...)` (both `.cpp` files include `scalar_codec.hpp` transitively).

No emitted bytes change: the implementation is exactly the same `reinterpret_cast` plus `insert` sequence.

### `BitfieldBytes`

RE-GREP confirmed current locations:

- `arrow-bridge/src/codec.cpp:35` takes `int64_t`.
- `core/include/fletcher/core/positional_io.hpp:140` takes `size_t` in `PositionalWriter`.
- `core/include/fletcher/core/positional_io.hpp:418` takes `size_t` in `PositionalReader`.

The `core` copies are identical to each other but divergent from the codec copy. Locked decision #6 requires preserving the codec's `int64_t` overflow-safe intent, so the shared helper must not take only `size_t`.

Single-source home:

- Add `core/include/fletcher/core/detail/bitfield.hpp` (a public header installed alongside `positional_io.hpp`).

Rationale for public placement:

- The public `core/include/fletcher/core/positional_io.hpp` must include it, so it must be installed (same as `arrow_result.hpp`).
- No existing free-function home in `core` can serve both codec and positional I/O without exposing unrelated machinery.

Signature:

```cpp
namespace fletcher::detail {

// Number of bytes needed for a null bitfield covering n items.
// Takes int64_t so wire-supplied uint32_t counts do not narrow through int.
inline std::size_t BitfieldBytes(std::int64_t n) {
    return static_cast<std::size_t>((n + 7) / 8);
}

}  // namespace fletcher::detail
```

Design notes:

- The formula is intentionally unchanged.
- No new negative-count guard is added because this item is a refactor, not a behavioural fix.
- The codec path keeps passing signed counts to `detail::BitfieldBytes(...)`.
- Positional I/O call sites convert only already-bounded values:
  - validated `int num_fields`
  - stored `int num_fields_`
  - `uint32_t count`
- Avoid generic `size_t -> int64_t` implicit narrowing. Where a positional call currently says `BitfieldBytes(static_cast<size_t>(num_fields))`, change it to use the validated signed value directly or an explicit safe cast from `uint32_t`.

Edits:

- Add `#include <fletcher/core/detail/bitfield.hpp>` to:
  - `arrow-bridge/src/codec.cpp`
  - `core/include/fletcher/core/positional_io.hpp`
- Delete `BitfieldBytes(int64_t)` from `arrow-bridge/src/codec.cpp`.
- Delete both private static `BitfieldBytes(size_t)` definitions from `positional_io.hpp`.
- Replace codec call sites with `detail::BitfieldBytes(...)`.
- Replace positional I/O call sites with `detail::BitfieldBytes(...)`.
- In `PositionalWriter::MapContext::BeginValues`, replace the local `(count + 7) / 8` expression with the shared helper too, so the bitfield-size calculation has one implementation.
- Call sites in `positional_io.hpp` (approximately 7-8 locations including `:45, :112, :260, :331, :333, :348, :389` and the inline `:126` in `MapContext::BeginValues`) must all move to the shared helper.

Dependency direction is valid:

- `arrow-bridge` links `fletcher-core` publicly.
- `core` is header-only and can own the shared detail helper.
- `positional_io.hpp` remains in `core` and includes a sibling core detail header.

### Verification mechanism

Acceptance gate:
- The primary verification is the existing component + integration test suites staying green (byte-identity assertions included).
- Each helper must have exactly one definition, verified manually or via documented grep command at implementation time.

Local developer convenience:
- Optional: create `tools/verify_hard7_helpers.ps1` as a local-only development aid (not a CI gate).
- This script is a **Windows-only convenience only**; it must NOT be wired into ctest/CMake or the CI pipeline (which runs on Linux).
- If created, document explicitly as "local manual verification only; CI gate is the test suites staying green."

Documented one-time verification (at implementation):
- Run `grep` or `findstr` to confirm each helper appears exactly once in its home:
  - `JoinSegments` in `pubsub/include/fletcher/pubsub/internal/segments.hpp`
  - `AppendFixed` in `arrow-bridge/src/scalar_codec.hpp`
  - `BitfieldBytes` in `core/include/fletcher/core/detail/bitfield.hpp`
- Confirm no lingering anonymous-namespace copies or duplicate definitions.

## Forcing-test mapping

Existing suites stay green:

- `core` tests cover `positional_io.hpp` users after the two private `BitfieldBytes` copies are removed and all call sites move to the shared helper.
- `arrow-bridge` tests cover `codec.cpp` and `scalar_codec.cpp` byte identity after `AppendFixed<T>` and `BitfieldBytes` consolidation.
- `pubsub` tests cover the mock provider after its local `Join` logic is removed.
- `pubsub-arrow` tests cover publisher/subscriber topic-keying after both wrapper `JoinSegments` methods are removed.
- Gateway integration tests cover the gateway in-process provider after its mock join logic points at the shared helper.
- Existing byte-identity and integration suites remain the behavioural forcing test; no new behavioural test is added because this is a refactor.

Single-definition acceptance:
- All component + integration suites pass (byte-identity included).
- Manual/grep confirmation that each helper has exactly one definition in its designated home; no lingering duplicates.
- No call-site orphaning (all references to consolidated helpers redirect to the shared definition).

## Risks & Unknowns

- The workspace does not have `rg`; current locations were confirmed with `findstr` and targeted file reads.
- `JoinSegments` mock helpers are output-equivalent to the shared helper but not all are text-identical. The design removes the duplicated logic rather than preserving adapter bodies.
- `AppendFixed` moves to an existing private source header rather than a public detail header (per architecture review blocking item #1). This is the correct placement: honours H-INV-4 (no unwanted public API surface), honours locked #6 (prefer existing source), and requires zero new includes (both consumers already include the target header).
- `BitfieldBytes` public placement in `core/include/.../detail/bitfield.hpp` is correct: it must be installed because `positional_io.hpp` includes it. This differs from `AppendFixed` because `positional_io.hpp` is itself public, whereas `scalar_codec.hpp` is private.
- Verification is local-only or documented as manual; no Windows-only PowerShell gate is wired into CI (per architecture review blocking item #2). Real acceptance gate is the test suites staying green.
- No STOP-AND-ASK tension found.

## Files-to-touch

- `pubsub/include/fletcher/pubsub/internal/segments.hpp`
- `xrcedds-pubsub-provider/src/xrce_dds_pubsub_provider.cpp`
- `pubsub-arrow/src/publisher_arrow.cpp`
- `pubsub-arrow/src/subscriber_arrow.cpp`
- `pubsub-arrow/include/fletcher/pubsub_arrow/publisher_arrow.hpp`
- `pubsub-arrow/include/fletcher/pubsub_arrow/subscriber_arrow.hpp`
- `pubsub/tests/test_publisher_subscriber.cpp`
- `pubsub-arrow/tests/test_pubsub_arrow.cpp`
- `gateway/src/main.cpp`
- `arrow-bridge/src/scalar_codec.hpp`
- `arrow-bridge/src/codec.cpp`
- `arrow-bridge/src/scalar_codec.cpp`
- `core/include/fletcher/core/detail/bitfield.hpp`
- `core/include/fletcher/core/positional_io.hpp`

## Step-2 review (cycle 2 — 2026-07-10)

**Verdict: APPROVE.**

Both cycle-1 blocking items are resolved and the previously-approved parts show
no regression.

1. **AppendFixed home — RESOLVED.** The single `inline template AppendFixed<T>`
   now lives in the existing private header `arrow-bridge/src/scalar_codec.hpp`
   (§AppendFixed, lines 83-114); both consumers already `#include` it, so no new
   includes are added; both anonymous-namespace copies are deleted; the public
   `buffer_append.hpp` is fully gone (absent from the design body and from
   Files-to-touch). Marked `inline` → header-safe, no ODR hazard across the two
   TUs. Honours locked #6 (prefer existing source over a new header) and H-INV-4
   (no public-API surface).
2. **Verification portability — RESOLVED.** The single-definition check is now an
   explicitly local-only developer convenience / manual grep, documented as
   **not** wired into ctest/CMake or the CI pipeline (§Verification mechanism,
   lines 178-194, and Risks line 218). Stated acceptance gate = all component +
   integration suites green (byte-identity included) + manual/grep
   single-definition confirmation.

No-regression spot-check (all confirmed intact): `BitfieldBytes` →
`core/include/fletcher/core/detail/bitfield.hpp`,
`inline std::size_t BitfieldBytes(std::int64_t n)` (public placement justified,
`int64_t` overflow-safe intent preserved per locked #6, both `positional_io`
copies collapsed); `JoinSegments` → existing `segments.hpp` (copies + private
decls deleted, call sites re-pointed); scope confined to
arrow-bridge/core/pubsub-arrow/xrce/gateway + test mocks; byte-identical, no
public-API change. Byte-identity across the `int64_t`-widening of the core
`BitfieldBytes`/`MapContext::BeginValues` call sites holds for all realistic
counts; any divergence is only at absurd overflow inputs and is the intended
safe behaviour (locked #6).

Advisory (non-blocking, compile-time-caught): the consolidated `AppendFixed`
sits in `namespace fletcher` and is called unqualified (line 112). That resolves
only if each call site is inside `namespace fletcher`; if any call site is not,
qualify it as `fletcher::AppendFixed(...)`. No silent-failure risk — a mismatch
fails the build.
