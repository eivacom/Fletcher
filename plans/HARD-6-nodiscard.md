# HARD-6: `[[nodiscard]]` on Public API

## Summary

HARD-6 is an additive public-API annotation change only. It must not change return types, parameters, overload sets, wire bytes, generated code, or provider behavior. The implementation should add `[[nodiscard]]` to the selected declarations in core, pubsub, and arrow-bridge, then fix existing legitimate return-value discards with explicit `static_cast<void>(...)` casts before the annotations are relied on.

Current target declarations confirmed:

| API | Current declaration |
|---|---|
| `core/include/fletcher/core/positional_io.hpp:267` | `bool IsNull(int field_index) const` |
| `pubsub/include/fletcher/pubsub/subscriber.hpp:58` | `SubscribeResult Subscribe(const std::vector<std::string>& segments, SubscribeCallback cb)` |
| `pubsub/include/fletcher/pubsub/provider.hpp:103-104` | `virtual SubscriptionResult Subscribe(const std::vector<std::string>& topic_segments, SubscribeCallback callback) = 0` |
| `pubsub/include/fletcher/pubsub/publisher.hpp:42` | `std::vector<std::string> ListTopics() const` |
| `arrow-bridge/include/fletcher/arrow_bridge/codec.hpp:48` | `EncodedRow EncodeRow(const ArrowRow& values) const` |
| `arrow-bridge/include/fletcher/arrow_bridge/codec.hpp:50` | `ArrowRow DecodeRow(const EncodedRow& buf) const` |
| `arrow-bridge/include/fletcher/arrow_bridge/codec.hpp:51` | `ArrowRow DecodeRow(const uint8_t* data, size_t len) const` |
| `core/include/fletcher/core/envelope.hpp:39` | `inline std::vector<uint8_t> SerializeEnvelope(const Envelope& env)` |
| `core/include/fletcher/core/envelope.hpp:74` | `inline Envelope DeserializeEnvelope(const uint8_t* data, size_t size)` |
| `core/include/fletcher/core/envelope.hpp:117` | `inline Envelope DeserializeEnvelope(const std::vector<uint8_t>& buf)` |
| `pubsub/include/fletcher/pubsub/owned_schema.hpp:54` | `static OwnedSchema DeepCopy(const ArrowSchema* src)` |

Build posture check:

| Area | Finding |
|---|---|
| `core/CMakeLists.txt` | No `/WX`, `-Werror`, `-Werror=unused-result`, or warning option found. Tests use `gtest_discover_tests`. |
| `pubsub/CMakeLists.txt` | No warnings-as-errors found. Tests use `gtest_discover_tests`. |
| `arrow-bridge/CMakeLists.txt` | No warnings-as-errors found. Tests use `gtest_discover_tests`. |
| `core/conanfile.py`, `pubsub/conanfile.py`, `arrow-bridge/conanfile.py` | Recipes generate a normal `CMakeToolchain`; no warning-as-error flags injected. |
| Wider scanned CMake/Conan-style metadata | No `/WX`, `-Werror`, `unused-result`, or `C4834` hits. |

Because normal component builds do not currently force warnings-as-errors, HARD-6 needs its own compile-failure target with explicit local warning flags.

## Design

Add `[[nodiscard]]` directly to the confirmed declarations:

```cpp
[[nodiscard]] bool IsNull(int field_index) const;
[[nodiscard]] SubscribeResult Subscribe(...);
[[nodiscard]] virtual SubscriptionResult Subscribe(...) = 0;
[[nodiscard]] std::vector<std::string> ListTopics() const;
[[nodiscard]] EncodedRow EncodeRow(...) const;
[[nodiscard]] ArrowRow DecodeRow(...) const;
[[nodiscard]] inline std::vector<uint8_t> SerializeEnvelope(...);
[[nodiscard]] inline Envelope DeserializeEnvelope(...);
[[nodiscard]] static OwnedSchema DeepCopy(...);
```

Annotate the base `PubSubProvider::Subscribe` declaration only. Do not annotate concrete provider override declarations — the `[[nodiscard]]` attribute does not inherit to overrides; each declaration stands alone. Overrides without the attribute will not warn at concrete-type call sites, so the base annotation in the spec is sufficient and necessary.

Blast-radius audit result (corrected):

| Target | Discarding call sites found | Action |
|---|---:|---|
| `PositionalReader::IsNull` | 0 | All uses are in conditions or assertions. No fix needed. |
| `Publisher::ListTopics` | 0 | All uses inspect the result or assign it. No fix needed. |
| `Codec::EncodeRow` | 1 | One intentional discard in `test_codec.cpp:249` wrapped with `static_cast<void>`. Already fixed. |
| `Codec::DecodeRow` overloads | 5 | Five discards inside `EXPECT_THROW(...)` at `arrow-bridge/tests/test_codec.cpp:609,621,630,638,645` must be wrapped with `static_cast<void>(...)`. |
| `SerializeEnvelope` | 0 | All uses assign. No fix needed. |
| `DeserializeEnvelope` overloads | 3 | Three discards inside `EXPECT_THROW(...)` at `core/tests/test_envelope.cpp:112,119,128` must be wrapped with `static_cast<void>(...)`. |
| `OwnedSchema::DeepCopy` | 1 | One intentional discard in `test_owned_schema.cpp:39` wrapped with `(void)`. Already fixed. |
| `Subscriber::Subscribe` base decl | 6 | Six intentional registration-only discards at `pubsub/tests/test_publisher_subscriber.cpp:189,246,248,271,306,332` must be wrapped with `(void)`. **(cycle-2: line 332 was previously dropped from the fix list below — must be added.)** |

Specific discard sites to fix before relying on `[[nodiscard]]`:

**DecodeRow (5 sites)** — wrap the expression inside the `EXPECT_THROW` with `static_cast<void>(...)`:

| File:line | Exact call shape |
|---|---|
| `arrow-bridge/tests/test_codec.cpp:609` | `EXPECT_THROW(codec.DecodeRow(buf.data(), buf.size()), ...)` → `EXPECT_THROW(static_cast<void>(codec.DecodeRow(...)), ...)` |
| `arrow-bridge/tests/test_codec.cpp:621` | `EXPECT_THROW(codec.DecodeRow(row), ...)` → wrap with `static_cast<void>` |
| `arrow-bridge/tests/test_codec.cpp:630` | `EXPECT_THROW(codec.DecodeRow(row), ...)` → wrap with `static_cast<void>` |
| `arrow-bridge/tests/test_codec.cpp:638` | `EXPECT_THROW(codec.DecodeRow(buf.data(), buf.size()), ...)` → wrap with `static_cast<void>` |
| `arrow-bridge/tests/test_codec.cpp:645` | `EXPECT_THROW(codec.DecodeRow(buf.data(), buf.size()), ...)` → wrap with `static_cast<void>` |

**DeserializeEnvelope (3 sites)** — wrap the expression inside the `EXPECT_THROW` with `static_cast<void>(...)`:

| File:line | Exact call shape |
|---|---|
| `core/tests/test_envelope.cpp:112` | `EXPECT_THROW(DeserializeEnvelope(tiny), ...)` → `EXPECT_THROW(static_cast<void>(DeserializeEnvelope(...)), ...)` |
| `core/tests/test_envelope.cpp:119` | `EXPECT_THROW(DeserializeEnvelope(buf), ...)` → wrap with `static_cast<void>` |
| `core/tests/test_envelope.cpp:128` | `EXPECT_THROW(DeserializeEnvelope(buf), ...)` → wrap with `static_cast<void>` |

**Subscriber::Subscribe (5 sites)** — wrap the call statement with `(void)`:

| File:line | Exact call shape | Classification |
|---|---|---|
| `pubsub/tests/test_publisher_subscriber.cpp:189` | `subscriber.Subscribe(kTopic, ...);` | Registration-only; change to `(void)subscriber.Subscribe(...)` |
| `pubsub/tests/test_publisher_subscriber.cpp:246` | `subscriber.Subscribe(kTopic, ...);` | Registration-only fan-out; add `(void)` |
| `pubsub/tests/test_publisher_subscriber.cpp:248` | `subscriber.Subscribe(kTopic, ...);` | Registration-only fan-out; add `(void)` |
| `pubsub/tests/test_publisher_subscriber.cpp:271` | `subscriber.Subscribe(kTopic, ...);` | Registration-only, id unused; add `(void)` |
| `pubsub/tests/test_publisher_subscriber.cpp:306` | `subscriber.Subscribe(kTopic, ...);` | Registration-only, id unused; add `(void)` |
| `pubsub/tests/test_publisher_subscriber.cpp:332` | `subscriber.Subscribe(kTopic, ...);` | Registration-only (PublishWithAttachmentsFansOutCorrectly); add `(void)` |

**Do NOT add `(void)` casts to `pubsub-arrow/tests/test_pubsub_arrow.cpp` or `fastdds-pubsub-provider/tests/test_fast_dds_pubsub_provider.cpp`:** The `SubscriberArrow::Subscribe` wrapper is not annotated; concrete provider overrides (`FastDDSPubSubProvider::Subscribe`) do not inherit the base `[[nodiscard]]` attribute. Neither will warn at compile time.

## Forcing-test mapping

Use a dedicated compile-failure target wired into CTest, not a runtime GTest. The test must pass only when a TU that intentionally discards every HARD-6 target return triggers a nodiscard diagnostic at compile time.

Recommended location:

| File | Purpose |
|---|---|
| `pubsub-arrow/tests/nodiscard_discard.cpp` | Bad TU that intentionally discards all HARD-6 target returns. `pubsub-arrow` already depends on pubsub and arrow-bridge, and transitively sees core. |
| `pubsub-arrow/tests/CMakeLists.txt` | Add an `EXCLUDE_FROM_ALL` object-library target plus a CTest entry that compiles the TU and gates passage on the nodiscard diagnostic. |

**CMake mechanism (corrected):**

The test must compile the discard TU at CTest time, capture compiler output, and pass only if the nodiscard diagnostic appears. Do NOT use `WILL_FAIL TRUE` — it inverts the verdict after `PASS_REGULAR_EXPRESSION` matches, which flips the sense. Instead, gate solely on `PASS_REGULAR_EXPRESSION` matching the diagnostic:

```cmake
add_library(nodiscard_discard_tu OBJECT EXCLUDE_FROM_ALL
    nodiscard_discard.cpp)

target_link_libraries(nodiscard_discard_tu PRIVATE
    fletcher-core::fletcher-core
    fletcher-pubsub::fletcher-pubsub
    fletcher-arrow-bridge::fletcher-arrow-bridge)

target_compile_features(nodiscard_discard_tu PRIVATE cxx_std_20)

if(MSVC)
    target_compile_options(nodiscard_discard_tu PRIVATE /we4834)
else()
    target_compile_options(nodiscard_discard_tu PRIVATE -Werror=unused-result)
endif()

add_test(
    NAME NodiscardTest.CompileFailsOnDiscard
    COMMAND ${CMAKE_COMMAND}
            --build ${CMAKE_BINARY_DIR}
            --target nodiscard_discard_tu
            --config $<CONFIG>)

set_tests_properties(NodiscardTest.CompileFailsOnDiscard PROPERTIES
    PASS_REGULAR_EXPRESSION "C4834|unused-result|nodiscard")
```

**Red-first verification:**

- **Before `[[nodiscard]]` annotations:** The TU compiles clean; no `C4834`, `unused-result`, or `nodiscard` diagnostic appears; `PASS_REGULAR_EXPRESSION` does not match; CTest reports FAIL (red).
- **After `[[nodiscard]]` annotations:** Discards emit the nodiscard diagnostic; regex matches; CTest reports PASS (green).

**Compile-time mechanism (portable):**

The `--build` command invokes the compiler on the TU as part of the build graph. CMake redirects compiler stderr/stdout to CTest's captured output stream, which `PASS_REGULAR_EXPRESSION` inspects. MSVC emits `C4834` to stderr; gcc/clang emit `unused-result` to stderr. The regex pattern `"C4834|unused-result|nodiscard"` is platform-agnostic and specific to nodiscard violations, avoiding false positives on unrelated compile errors (which would not include these keywords).

The bad TU should keep unrelated compile risks low:

```cpp
#include <fletcher/arrow_bridge/codec.hpp>
#include <fletcher/core/envelope.hpp>
#include <fletcher/core/positional_io.hpp>
#include <fletcher/pubsub/provider.hpp>
#include <fletcher/pubsub/publisher.hpp>
#include <fletcher/pubsub/subscriber.hpp>
#include <fletcher/pubsub/owned_schema.hpp>

void DiscardCore(fletcher::PositionalReader& reader,
                 const fletcher::Envelope& envelope,
                 const std::vector<uint8_t>& bytes) {
    reader.IsNull(0);
    fletcher::SerializeEnvelope(envelope);
    fletcher::DeserializeEnvelope(bytes);
    fletcher::DeserializeEnvelope(bytes.data(), bytes.size());
}

void DiscardPubSub(fletcher::Publisher& publisher,
                   fletcher::Subscriber& subscriber,
                   fletcher::PubSubProvider& provider,
                   const ArrowSchema* schema) {
    publisher.ListTopics();
    subscriber.Subscribe({"nodiscard"}, {});
    provider.Subscribe({"nodiscard"}, {});
    fletcher::OwnedSchema::DeepCopy(schema);
}

void DiscardCodec(fletcher::Codec& codec,
                  const fletcher::ArrowRow& row,
                  const fletcher::EncodedRow& bytes) {
    codec.EncodeRow(row);
    codec.DecodeRow(bytes);
    codec.DecodeRow(bytes.data(), bytes.size());
}
```

## Risks & Unknowns

The base `PubSubProvider::Subscribe` annotation diagnoses discards at polymorphic-reference call sites (tested via the forcing TU's `PubSubProvider& provider.Subscribe(...)`). Concrete provider types whose override declarations lack the annotation will not diagnose at concrete-static-type call sites — this is C++ attribute behavior, not a flaw in the design. The forcing TU correctly exercises the base API and is sufficient to verify the annotation is present and active.

`pubsub-arrow/tests` is the cleanest existing aggregate location because it can see core, pubsub, and arrow-bridge together. If HARD-6 must remain strictly inside only core/pubsub/arrow-bridge test trees, split the forcing test into component-local compile-fail TUs instead: core for positional/envelope, pubsub for publisher/subscriber/provider/owned schema, arrow-bridge for codec.

Generated typed subscriber wrappers discard returns in some integration tests, but they are not the HARD-6 target declarations. Do not edit the generator or generated outputs in this round.

The repository currently contains prior HARD changes: `OwnedSchema::DeepCopy` already checks `ArrowSchemaDeepCopy`, and `Subscriber::Unsubscribe` already has the HARD-5 documentation. HARD-6 should layer annotations on the current headers without reverting those changes.

## Files to touch

| File | Change |
|---|---|
| `core/include/fletcher/core/positional_io.hpp` | Add `[[nodiscard]]` to `PositionalReader::IsNull`. |
| `core/include/fletcher/core/envelope.hpp` | Add `[[nodiscard]]` to `SerializeEnvelope` and both `DeserializeEnvelope` overloads. |
| `core/tests/test_envelope.cpp` | Wrap three `DeserializeEnvelope` discards at lines 112, 119, 128 with `static_cast<void>(...)` inside `EXPECT_THROW`. |
| `pubsub/include/fletcher/pubsub/subscriber.hpp` | Add `[[nodiscard]]` to `Subscriber::Subscribe`. |
| `pubsub/include/fletcher/pubsub/provider.hpp` | Add `[[nodiscard]]` to `PubSubProvider::Subscribe` base declaration. |
| `pubsub/include/fletcher/pubsub/publisher.hpp` | Add `[[nodiscard]]` to `Publisher::ListTopics`. |
| `pubsub/include/fletcher/pubsub/owned_schema.hpp` | Add `[[nodiscard]]` to `OwnedSchema::DeepCopy`. |
| `pubsub/tests/test_publisher_subscriber.cpp` | Wrap six `Subscriber::Subscribe` discards at lines 189, 246, 248, 271, 306, 332 with `(void)`. |
| `arrow-bridge/include/fletcher/arrow_bridge/codec.hpp` | Add `[[nodiscard]]` to `EncodeRow` and both `DecodeRow` overloads. |
| `arrow-bridge/tests/test_codec.cpp` | Wrap five `DecodeRow` discards at lines 609, 621, 630, 638, 645 with `static_cast<void>(...)` inside `EXPECT_THROW`. |
| `pubsub-arrow/tests/nodiscard_discard.cpp` | New negative-compile TU that discards every HARD-6 annotated result. |
| `pubsub-arrow/tests/CMakeLists.txt` | Add `nodiscard_discard_tu` compile-fail target and `NodiscardTest.CompileFailsOnDiscard` CTest entry with `/we4834` and regex gate. |

## Step-2 review (cycle 2, 2026-07-09)

**Verdict: NEEDS-REWORK — 1 blocking item.** Two of the three cycle-1 blockers
are fully resolved; the discard audit is 5/6 complete and still self-contradictory.

Resolved (verified against source):

- **Cycle-1 #2 (negative-compile polarity) — RESOLVED.** `WILL_FAIL` now appears
  only in a "do NOT use" note; the CTest entry gates solely on
  `PASS_REGULAR_EXPRESSION "C4834|unused-result|nodiscard"`. Red-first polarity is
  correct: pre-annotation the TU compiles clean (exit 0) → no diagnostic → no
  regex match → CTest FAIL (red); post-annotation the discards become errors →
  diagnostic present → match → PASS (green). This relies on the CTest rule that
  `PASS_REGULAR_EXPRESSION` ignores the process exit code, so the non-zero
  `--build` failure post-annotation is still reported PASS — correct. The
  `${CMAKE_COMMAND} --build … --target … --config $<CONFIG>` command is portable
  across MSVC (multi-config) and gcc (single-config; `--config` ignored). An
  unrelated compile error false-fails (no keyword in output) rather than
  false-passing — the safe direction.
- **Cycle-1 #3 (MSVC scope) — RESOLVED.** `/we4834` promotes only C4834 (a
  default-level-1 warning, so no `/W4` needed); gcc/clang use
  `-Werror=unused-result`, whose diagnostic text contains both "nodiscard" and
  "unused-result" on both compilers. Not `/W4 /WX`. Surgical.
- **Non-blocking reconciliation — CONFIRMED.** Only the base decls are annotated;
  concrete `Subscribe` overrides (`FastDDSPubSubProvider`, XRCE, gateway mock,
  `SubscriberArrow`) are left un-annotated and the pubsub-arrow/fastdds
  override-call `(void)` edits are correctly dropped (discards through concrete
  static types don't warn — verified: fastdds test `provider` at
  `test_fast_dds_pubsub_provider.cpp:115` is a `FastDDSPubSubProvider`, not a base
  ref). All 11 annotated decl line numbers verified accurate and additive-only
  (positional_io.hpp:267, subscriber.hpp:58, provider.hpp:103-104, publisher.hpp:42,
  codec.hpp:48/50/51, envelope.hpp:39/74/117, owned_schema.hpp:54 [decl line; spec
  cites the :53 comment]). Broad spot-check of IsNull/ListTopics/SerializeEnvelope/
  DeserializeEnvelope/EncodeRow/DeepCopy across src, gateway, and integration-tests
  confirms every other use assigns or consumes the result — the "0 discard" rows
  hold. `gateway/src/ws_session.cpp:188` assigns `Subscriber::Subscribe` to a
  local (not a discard).

Blocking:

1. **Discard audit still incomplete — line 332 dropped.** The
   `Subscriber::Subscribe` audit lists SIX genuine discard sites
   (`test_publisher_subscriber.cpp:189,246,248,271,306,332` — verified by grep,
   all six are bare-statement discards of the `[[nodiscard]]`-annotated base
   `Subscribe`) but the blast-radius row labelled it "Five"/count 5, and both the
   detailed fix table and Files-to-touch omitted line 332
   (`PublishWithAttachmentsFansOutCorrectly`). This is the exact defect class
   (incomplete audit) that cycle-1 flagged. An implementer following the fix
   list would leave a stray `[[nodiscard]]` diagnostic on that line, contradicting
   the doc's own contract (Summary: "fix existing legitimate return-value discards
   … before the annotations are relied on"). *I corrected the count, the fix
   table, and Files-to-touch inline to add line 332 (6 sites); confirm and apply
   the `(void)` wrap there. No other sites missing.*
