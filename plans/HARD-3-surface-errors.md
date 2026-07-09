# HARD-3 — Surface discarded / swallowed errors

## Summary

HARD-3 fixes two silent error paths without changing wire format or public runtime/pubsub APIs:

1. `OwnedSchema::DeepCopy` currently ignores `ArrowSchemaDeepCopy` failure at `C:\Users\CTM\source\prototypes\Fletcher\pubsub\include\fletcher\pubsub\owned_schema.hpp:53-57`. It must capture the return code and throw on failure, in place, per locked decisions #2 and #8.
2. `FletcherTopicType::serialize` currently catches everything, clears `payload.length`, and returns `false` at `C:\Users\CTM\source\prototypes\Fletcher\fastdds-pubsub-provider\src\fast_dds_pubsub_provider.cpp:177-180`. It must capture a diagnostic for `std::exception::what()` and still return `false` without rethrowing — and the capture itself must be exception-safe per H-INV-3 — per locked decision #3.

Both changes need red-first negative tests. The tests should fail before the behavioral fix because the current behavior is silent success/empty diagnostic, not because of a brittle crash or a changed public signature.

## Design

For `OwnedSchema::DeepCopy`, keep the implementation in `pubsub/include/fletcher/pubsub/owned_schema.hpp`. Change:

```cpp
ArrowSchemaDeepCopy(src, copy.get());
return copy;
```

to capture the return code:

```cpp
ArrowErrorCode code = ArrowSchemaDeepCopy(src, copy.get());
if (code != NANOARROW_OK) {
    throw std::runtime_error(
        "OwnedSchema::DeepCopy: ArrowSchemaDeepCopy failed with code " +
        std::to_string(code));
}
return copy;
```

Use `std::runtime_error` because `ArrowSchemaDeepCopy` reports only an `ArrowErrorCode`/errno-style integer and can fail for allocation/resource failures as well as malformed source shape. The message must name `OwnedSchema::DeepCopy` and `ArrowSchemaDeepCopy`, and include the code so callers have a diagnostic instead of a null/empty schema. This honors H-INV-2 by throwing on a recoverable error rather than aborting or silently returning an empty schema.

For `FletcherTopicType::serialize`, keep DDS callback safety as the primary constraint. Do not rethrow from `serialize`. The diagnostic capture mechanism must be **exception-safe in itself** — i.e., nothing in the catch handler can throw out of `serialize`.

Define a `noexcept` internal helper that accepts a `const char*` pointer (not a `std::string`, avoiding allocation):

```cpp
void SetLastSerializeError(const char* message) const noexcept {
    try {
        std::lock_guard<std::mutex> guard(diagnostic_mu_);
        last_serialize_error_ = (message != nullptr) ? message : "";
    } catch (...) {
        // Silently suppress any lock/assign failure. We are already in an exception
        // handler, and we must not propagate out of serialize.
    }
}
```

The member storage is:

```cpp
mutable std::mutex diagnostic_mu_;
mutable std::string last_serialize_error_;

std::string LastSerializeError() const;
```

In the catch block, pass `e.what()` directly (a `const char*`):

```cpp
} catch (const std::exception& e) {
    SetLastSerializeError(e.what());
    payload.length = 0;
    return false;
} catch (...) {
    SetLastSerializeError("non-std exception in FletcherTopicType::serialize");
    payload.length = 0;
    return false;
}
```

This design ensures:
- The `const char*` argument does not allocate at the call site.
- The `SetLastSerializeError` function is `noexcept` and wraps the lock + assign in an inner `try { … } catch (…) {}`, so even if `std::mutex::lock()` throws `std::system_error` or `std::string::operator=` throws `std::bad_alloc`, the exception is suppressed and `serialize` continues safely.
- Callers check the diagnostic via `LastSerializeError()`, a public const method on `FletcherTopicType` (guarded by the same `diagnostic_mu_` for thread safety; no diagnostic capture race).
- This is an internal provider type, not a public Fletcher API. Avoid `stderr` as the primary mechanism because it is hard to assert in tests and introduces noisy side effects. A stored diagnostic is deterministic, testable, and sufficient for the acceptance requirement that the error be captured.

To make `serialize` directly testable without constructing a full DDS participant, extract **only** `TransportData` and `FletcherTopicType` from the anonymous namespace in `fast_dds_pubsub_provider.cpp` into an internal provider header:

```text
fastdds-pubsub-provider/src/internal/fletcher_topic_type.hpp
```

**All other anonymous-namespace types** (`RawBytes`, `RawBytesTopicType`, `SubscriptionListener`, `SchemaChannel`, `SchemaListener`) **remain in the `.cpp`.** This constraint preserves linkage and keeps unrelated FastDDS provider logic private.

The new header must carry every dependency that `TransportData` and `FletcherTopicType` reference:
- `CDR_LE` (constant).
- `FixedWriteBuffer` and `WriteBuffer` (base class and template parameter).
- `Attachments` (struct).
- `PubSubProvider::RowEncoder` (function type).
- Required FastDDS includes (`TopicDataType`, `SerializedPayload_t`, etc.).

The moved type bodies (serialize/deserialize) must be **verbatim byte-identical** — no refactoring on the happy path — to honor H-INV-1 (no happy-path bytes change).

Put both types under `namespace fletcher::internal`. The tests already add `fastdds-pubsub-provider/src` to the include path, so the test can include this internal header without exposing it as installed public API. The production provider continues to construct the type as `impl_->type_support.reset(new internal::FletcherTopicType(...))`, preserving TypeSupport registration and lifetime semantics.

This seam is principled because `FletcherTopicType` owns transport serialization behavior and diagnostics; testing it directly avoids a full DDS round trip for an encoder exception that happens wholly inside the `TopicDataType::serialize` body.

## Forcing-test mapping

`#54` forcing test: add `OwnedSchemaTest.DeepCopyFailureThrows` in `pubsub/tests`, preferably a new focused TU:

```text
pubsub/tests/test_owned_schema.cpp
```

and add it to the existing `pubsub_tests` executable in `pubsub/tests/CMakeLists.txt`.

Nanoarrow's vendored implementation at `pubsub/third_party/nanoarrow/nanoarrow.c` shows `ArrowSchemaDeepCopy` fails only when one of its copy/allocation helpers fails. It does not validate an invalid format string; it copies `schema->format` with `ArrowSchemaSetFormat`. A null `children` pointer with a small positive `n_children` is not a safe forcing input because the function allocates output children and then dereferences `schema->children[i]`, which crashes instead of returning an error.

The constructible failing input is an oversized declared child count that forces `ArrowSchemaAllocateChildren(schema_out, schema->n_children)` to return `ENOMEM` before the function reads `schema->children`:

```cpp
TEST(OwnedSchemaTest, DeepCopyFailureThrows) {
    ArrowSchema malformed{};
    malformed.format = "i";
    malformed.name = nullptr;
    malformed.metadata = nullptr;
    malformed.flags = 0;
    malformed.n_children =
        std::numeric_limits<int64_t>::max() / static_cast<int64_t>(sizeof(ArrowSchema*));
    malformed.children = nullptr;
    malformed.dictionary = nullptr;
    malformed.release = nullptr;
    malformed.private_data = nullptr;

    EXPECT_THROW(
        {
            auto copy = OwnedSchema::DeepCopy(&malformed);
            (void)copy;
        },
        std::runtime_error);
}
```

Why this fails in nanoarrow: `ArrowSchemaDeepCopy` initializes the output schema, copies `format`, `flags`, `name`, and `metadata`, then calls `ArrowSchemaAllocateChildren(schema_out, schema->n_children)`. With the huge positive `n_children`, nanoarrow attempts to allocate an impossible child pointer table and returns `ENOMEM`. Current Fletcher ignores that status and returns an invalid/empty `OwnedSchema`, so this test is red before the fix and green after the throw is added.

**Sanitizer note for `#54`:** Checked `.conan-profiles/Windows-msvc194-x86_64-Release`, `.conan-profiles/Linux-gcc13-x86_64-Release`, `pubsub/CMakeLists.txt`, `pubsub/tests/CMakeLists.txt`, and `.github/workflows/ci.pubsub.yml`. **None enable AddressSanitizer, UBSan, or any memory sanitizer.** Both the Windows (MSVC) and Linux (GCC) Release profiles contain no sanitizer options, and the standard test runs use plain Release builds. Therefore, the test remains unguarded — the huge-`n_children` allocation request will return `nullptr` deterministically on both platforms (64-bit address space exhaustion, no OOM abort), and the test will be red for the right reason (silent status discarding, not abort).

`#60` forcing test: add a direct serialization diagnostic test in `fastdds-pubsub-provider/tests/test_fast_dds_pubsub_provider.cpp` after extracting the internal seam.

The test constructs the internal topic type and injects a throwing encoder through `TransportData::encoder`, which is exactly the production seam used by `Publish`:

```cpp
TEST(FletcherTopicTypeTest, SerializeCapturesEncoderExceptionDiagnostic) {
    fletcher::internal::FletcherTopicType type(128);

    Attachments attachments;
    fletcher::internal::TransportData data;
    data.attachments = &attachments;
    data.encoder = [](WriteBuffer&) {
        throw std::runtime_error("encoder boom");
    };

    std::vector<uint8_t> storage(128);
    eprosima::fastdds::rtps::SerializedPayload_t payload;
    payload.data = storage.data();
    payload.max_size = static_cast<uint32_t>(storage.size());
    payload.length = 123;

    EXPECT_FALSE(type.serialize(&data, payload, eprosima::fastdds::dds::DataRepresentationId_t{}));
    EXPECT_EQ(payload.length, 0u);
    EXPECT_THAT(type.LastSerializeError(), testing::HasSubstr("encoder boom"));
}
```

Before the fix, once the extraction/accessor exists, the catch-all still returns `false` and clears `payload.length`, but the diagnostic remains empty. That is the intended red-first failure. After the fix, `catch (const std::exception&)` captures `what()` via the `noexcept` `SetLastSerializeError` and the test passes. The test must also assert that no exception propagates from `serialize`.

## Risks & Unknowns

The `OwnedSchemaTest.DeepCopyFailureThrows` input depends on a very large allocation request returning `nullptr`. That is the strongest genuine `ArrowSchemaDeepCopy` failure path available without modifying nanoarrow or adding allocator hooks; the vendored nanoarrow has plain `ArrowMalloc`/`malloc` and no global schema allocator injection. The huge-`n_children` seam is deterministic on plain MSVC/gcc Release builds (verified via conan profile and CI workflow inspection) and forces `ArrowSchemaAllocateChildren` to return `ENOMEM`, the correct and only non-crashing nanoarrow seam.

Invalid `format` strings are not suitable for this forcing test. `ArrowSchemaDeepCopy` does not parse or validate the format; it copies the string. Semantic schema validation belongs elsewhere and would not prove that `OwnedSchema::DeepCopy` surfaces a discarded status.

The `#60` diagnostic-capture path is **explicitly exception-safe** by design. The `SetLastSerializeError` function accepts a `const char*` (no allocation at call site), is marked `noexcept`, and wraps the mutex lock + string assignment in an inner `try { … } catch (…) {}` block. This ensures that no throw can escape from the catch handler into the DDS `serialize` call, honoring H-INV-3 and locked decision #3 directly. The mutex itself is fine and is not a data race (happy path never touches it; writes are serialized inside `serialize` catch blocks, reads are inside `LastSerializeError()` which acquires the lock).

The FastDDS diagnostic is intentionally internal. Adding a public provider method or changing `Publish`/`serialize` signatures would violate H-INV-4. If reviewers require process-wide logging in addition to storage, that is a STOP-AND-ASK because the current provider does not have an established logging facility and adding one would broaden the design.

The internal extraction of `FletcherTopicType` must preserve behavior and linkage. Keep it under `fastdds-pubsub-provider/src/internal`, not public `include`, and avoid moving unrelated FastDDS provider logic (all other anonymous-namespace types stay in `.cpp`). The moved bodies are verbatim to guarantee byte-identical happy paths per H-INV-1.

## Files to touch

```text
C:\Users\CTM\source\prototypes\Fletcher\pubsub\include\fletcher\pubsub\owned_schema.hpp
```

Implement the in-place `ArrowSchemaDeepCopy` status check and throw. Add `#include <string>` to support `std::to_string` and `std::string` concatenation.

```text
C:\Users\CTM\source\prototypes\Fletcher\pubsub\tests\test_owned_schema.cpp
C:\Users\CTM\source\prototypes\Fletcher\pubsub\tests\CMakeLists.txt
```

Add `OwnedSchemaTest.DeepCopyFailureThrows` and include the new TU in `pubsub_tests`.

```text
C:\Users\CTM\source\prototypes\Fletcher\fastdds-pubsub-provider\src\fast_dds_pubsub_provider.cpp
C:\Users\CTM\source\prototypes\Fletcher\fastdds-pubsub-provider\src\internal\fletcher_topic_type.hpp
```

Extract `TransportData` and `FletcherTopicType` into an internal testable seam (keeping all other anonymous-namespace types in `.cpp`), add exception-safe stored serialize diagnostics with `noexcept` `SetLastSerializeError`, and update provider construction to use `fletcher::internal::FletcherTopicType`.

```text
C:\Users\CTM\source\prototypes\Fletcher\fastdds-pubsub-provider\tests\test_fast_dds_pubsub_provider.cpp
```

Add the serialize diagnostic forcing test using a throwing encoder. Verify that the test target has `fastdds-pubsub-provider/src` on its include path for accessing the internal header; add to CMake if needed.

## Step-2 review (cycle 2, 2026-07-09)

**Verdict: APPROVE.** All three cycle-1 blocking items are resolved; no regression in the approved substance.

Verification against `SPEC` (docs/runtime-hardening-spec.md §HARD-3) and `LOCKED` (HARD-locked-decisions #3, #8; H-INV-1/2/3/4/5):

1. **#60 capture is now effectively `noexcept` — RESOLVED.** `SetLastSerializeError(const char*) const noexcept` (design §Design) wraps the `lock_guard` + `std::string` assign in an inner `try{...}catch(...){}`, so a `std::system_error` from `lock()` or a `std::bad_alloc` from `operator=` is swallowed. The catch handler passes `e.what()` directly (a `const char*`; `std::exception::what()` is itself `noexcept`) — no `std::string` is constructed at the call site, so no `bad_alloc` can originate there. `payload.length = 0` and `return false` cannot throw. The whole handler is exception-tight → H-INV-3 / decision #3 honoured. Confirmed against the live `catch(...)` at `fast_dds_pubsub_provider.cpp:177-180`.

2. **#54 sanitizer determinism — RESOLVED, independently reconfirmed.** I re-checked: both cited profiles exist and are plain Release with no `CFLAGS`/`CXXFLAGS`/sanitizer entries; a case-insensitive scan for `sanitize|asan|ubsan|fsanitize` across `pubsub/`, `fastdds-pubsub-provider/`, and every `.github/workflows/*.yml` (incl. the cited `ci.pubsub.yml`) returns nothing. The huge-`n_children` ENOMEM seam is unchanged and stays unguarded — red-for-the-right-reason (silent status discard, not an OOM abort). The design's §"Sanitizer note" states this correctly.

3. **Extraction constrained (H-INV-1) — RESOLVED.** Only `TransportData` + `FletcherTopicType` move to `src/internal/fletcher_topic_type.hpp`; all five other anon-namespace types (`RawBytes`, `RawBytesTopicType`, `SubscriptionListener`, `SchemaChannel`, `SchemaListener`) stay in the `.cpp` (verified present at :52/:56/:262/:290/:314). Header carries the named deps (`CDR_LE`, `FixedWriteBuffer`/`WriteBuffer`, `Attachments`, `PubSubProvider::RowEncoder`, FastDDS includes); bodies move verbatim on the happy path; construction switches to `internal::FletcherTopicType`. The fastdds test target already has `../src` on its include path (tests/CMakeLists.txt:11-12), so no CMake change is strictly required — the "add if needed" is a no-op. `#include <string>` added to `owned_schema.hpp` (header currently includes `<stdexcept>` but not `<string>`, so this is needed for `std::to_string`/concat).

No-regression checks: #54 fixed in place in pubsub (decision #8); #60 behavioural change scoped to the `:177` catch handler (try-block/deserialize verbatim → happy-path byte-identical, H-INV-1); pubsub + fastdds only, generator untouched (H-INV-5); public signatures unchanged, diagnostic is internal (H-INV-4); DeepCopy throws `std::runtime_error` (spec-permitted alternative).

Non-blocking notes (implementer hygiene, not required for approval):
- N1. The explicit dep bullet list omits the standard-library headers the moved bodies + new members need (`<mutex>`, `<string>`, `<vector>`, `<memory>`, `<cstring>`, `<functional>`). The design's "carry every dependency" clause covers them; add them so the header is self-contained.
- N2. `LastSerializeError() const` is declared without a body; define it inline in the header for a self-contained seam (an out-of-line `.cpp` definition also links, since the test links `fletcher-fastdds-pubsub-provider`).
