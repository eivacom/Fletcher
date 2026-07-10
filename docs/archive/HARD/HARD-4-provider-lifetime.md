# HARD-4 — Provider Lifetime & Callback Re-entrancy

## Summary

HARD-4 fixes two provider-lifecycle defects without changing public API signatures, installed headers, wire format, or XRCE dispatch architecture.

For FastDDS issue #63, destruction is made an explicit quiescence contract: callers must ensure no concurrent API calls and no provider callbacks that may re-enter the instance are in flight during destruction. The destructor must not add a broad `lock_guard` around DDS teardown because `impl_->mu` is non-recursive and DDS entity deletion may wait for callbacks that can re-enter the provider, producing a hold-and-wait deadlock.

For the XRCE #62 residual, `Impl::OnTopic()` must stop depending on live `TopicState` fields after invoking user callbacks. `Unsubscribe()` does not erase the `TopicState`; it resets fields in place:

```cpp
ts.callback = nullptr;
ts.pending.clear();
```

The spec prose saying that `Unsubscribe` "erases the TopicState" is imprecise. The PM should correct that wording when HARD-4 closes. The map node remains live; the real bug is that `OnTopic()` keeps using `ts.callback`, `ts.pending`, and `ts.shared_schema` across re-entrant callback execution while those fields can be reset in place.

The XRCE fix is narrow: copy callback-time data from `TopicState` into locals before invoking callbacks, then invoke through those locals. Dispatch stays under the existing recursive mutex; callbacks are not moved outside the lock. No blanket callback `try/catch` is added. Copy-to-locals removes the `std::bad_function_call` path caused by re-entrant `Unsubscribe()` nulling `ts.callback`; user-thrown exceptions remain a pre-existing behavior outside this point-fix.

## Design

FastDDS #63:

`FastDDSPubSubProvider::~FastDDSPubSubProvider()` currently iterates `impl_->topics` and deletes DDS entities while normal public methods protect the same state with `impl_->mu`. `CreateTopic()`, `Publish()`, `Subscribe()`, and parts of `Unsubscribe()` hold that mutex while resolving topic state and touching DDS entities.

The fix is to document the destruction precondition near the destructor and in any existing public destructor comment:

```cpp
// Destruction precondition: no thread may be executing or about to enter a
// public provider API on this instance, and no provider callback may still be
// in flight if that callback can re-enter this provider. The destructor tears
// down DDS entities and invalidates all TopicState pointers; it is not a
// synchronization boundary for concurrent use of the provider object.
```

Do not add a destructor-wide `std::lock_guard`. `impl_->mu` is a non-recursive `std::mutex`, and DDS deletion calls such as `delete_datareader()` may wait for in-flight listener callbacks to complete. If the destructor holds `impl_->mu` while deletion waits, and the callback re-enters the provider and tries to take `impl_->mu`, the destructor and callback can deadlock. This matches the existing `Unsubscribe()` shape, which extracts state under the provider lock and deletes readers outside that lock.

XRCE #62 residual:

`Impl::OnTopic()` must use local snapshots for every value needed during callback dispatch. The implementation should preserve the current recursive-mutex dispatch model and keep the change focused on lifetime safety.

Data path shape:

```cpp
SubscribeCallback callback;
SharedSchema schema_for_callback;
Envelope envelope;

{
    auto& ts = tit->second;
    if (!ts.callback) {
        return;
    }

    envelope = DeserializeEnvelope(...);

    if (!ts.schema_resolved) {
        ts.pending.push_back(std::move(envelope));
        return;
    }

    callback = ts.callback;
    schema_for_callback = ts.shared_schema;
}

// Still under the existing recursive mutex; only locals are used.
if (callback) {
    callback(envelope.row.data(), envelope.row.size(),
             schema_for_callback, std::move(envelope.attachments));
}
```

Schema-flush path shape:

```cpp
SubscribeCallback callback;
SharedSchema schema_for_callbacks;
std::vector<Envelope> pending;

{
    auto& ts = tit->second;
    if (ts.schema_resolved) {
        return;
    }

    ts.schema = OwnedSchema::DeepCopy(schema.get());
    ts.shared_schema = MakeSharedSchema(std::move(schema));
    ts.schema_resolved = true;

    callback = ts.callback;
    schema_for_callbacks = ts.shared_schema;
    pending = std::move(ts.pending);
    ts.pending.clear();
}

// Still under the existing recursive mutex; do not touch ts here.
if (callback) {
    for (auto& env : pending) {
        callback(env.row.data(), env.row.size(),
                 schema_for_callbacks, std::move(env.attachments));
    }
}
```

The key implementation rule is syntactic and testable: after the first user callback invocation on a path, `OnTopic()` must not read or write `ts` or any field reached through `ts`. In particular, the schema-flush loop must iterate a local `pending` vector and invoke a local `callback`.

No public API changes are allowed. The installed public header `xrcedds-pubsub-provider/include/.../xrce_dds_pubsub_provider.hpp` must remain byte-for-byte untouched for this item.

Test seam:

Add a non-installed internal test header:

```cpp
// xrcedds-pubsub-provider/src/internal/xrce_test_hook.hpp
#pragma once

#ifndef FLETCHER_BUILD_TESTS
#error "xrce_test_hook.hpp is test-only"
#endif

#include <cstddef>
#include <cstdint>

namespace fletcher::xrce::test {

struct ReentrantUnsubscribeResult {
    int delivery_count = 0;
};

ReentrantUnsubscribeResult RunReentrantUnsubscribeSchemaFlushScenario();

}  // namespace fletcher::xrce::test
```

The header declares free hook function(s) only. Its signatures must reference only complete public or standard types. It must not name, dereference, store, or require a complete `XrceDDSPubSubProvider::Impl`.

The hook body lives in `xrcedds-pubsub-provider/src/xrce_dds_pubsub_provider.cpp`, under `FLETCHER_BUILD_TESTS`. Because that translation unit contains the complete `Impl`, `TopicState`, and real `Impl::OnTopic()`, the hook can construct the scenario, populate real internal state, synthesize schema/data samples, and drive the real `OnTopic()` path. The hook must not reimplement delivery logic.

## Forcing-test mapping

XRCE forcing test: `ReentrantUnsubscribeNoUseAfterFree` in `xrcedds-pubsub-provider/tests/test_xrce_provider.cpp`.

The test calls the internal hook that builds this real `Impl::OnTopic()` scenario:

1. A topic has a callback and at least two pending envelopes.
2. A synthesized schema sample enters the schema-flush path.
3. On the first delivery, the callback unsubscribes the same topic.
4. `Unsubscribe()` performs the real in-place reset: `ts.callback = nullptr` and `ts.pending.clear()`.

Expected assertions:

```cpp
EXPECT_NO_THROW({
    auto result = fletcher::xrce::test::RunReentrantUnsubscribeSchemaFlushScenario();
    EXPECT_EQ(result.delivery_count, 2);
});
```

Pre-fix behavior:

The first pending envelope is delivered. The callback unsubscribes and clears `ts.pending`. On loop iteration 2, the old schema-flush loop still advances through `ts.pending`. Evaluating `env.row.data()`, `env.row.size()`, or `std::move(env.attachments)` reads a destroyed `Envelope`, because `clear()` ended the element lifetimes and invalidated the iterator. That is object-lifetime undefined behavior from the same iterator-invalidation defect the fix removes.

In ordinary Release builds without sanitizers, this path is still deterministic for the test: `std::vector::clear()` retains the backing storage, so the destroyed `Envelope` read typically returns the previous bytes reproducibly rather than faulting. After argument evaluation, the loop invokes `ts.callback`, which was nulled by `Unsubscribe()`, so `std::function::operator()` throws `std::bad_function_call`. That exception is the deterministic observable asserted by the red-first test, and the exception itself is defined language-level behavior.

The correct characterization is therefore: the pre-fix test is deterministic in Release and needs no sanitizer; the defined `std::bad_function_call` is the assertion point; the same pre-fix path also exercises the object-lifetime UB caused by iterator invalidation, before `std::function::operator()` runs. The document must not claim the pre-fix path has no UB or no destroyed-object read.

Post-fix behavior:

`OnTopic()` copies `callback`, `shared_schema`, and `pending` into locals before invoking user code. Re-entrant `Unsubscribe()` can reset the live `TopicState`, but the in-flight schema-flush delivery uses only local values. No `std::bad_function_call` is thrown, and both pending envelopes are delivered.

FastDDS forcing test:

Add a quiescent destruction test that exercises destruction under the documented precondition. It is not a race test. The test name and comment should make the contract explicit, for example `DestructAfterQuiescentUseDocumentsContract`.

## Risks & Unknowns

The FastDDS contract is not fully enforceable by unit tests. A race test for destruction during concurrent use would be flaky and would not make that usage supported. The valid acceptance test is destruction after quiescent use plus clear documentation.

The no-destructor-lock decision depends on conservative DDS teardown behavior: deletion may wait for callbacks. If a future FastDDS version guarantees deletion cannot wait on user callbacks, a lock could be reconsidered, but it still would not make concurrent object destruction safe.

The XRCE red-first test intentionally observes `std::bad_function_call`, but the pre-fix execution also includes object-lifetime UB from reading a destroyed `Envelope` after `pending.clear()`. This is acceptable as a characterization test for the exact bug being removed, but the design must be explicit that the path is not fully defined.

If the callback storage changes away from `std::function`, the red observable may need to change. The required behavioral assertion remains: pre-fix incomplete delivery or exception; post-fix `EXPECT_NO_THROW` and `delivery_count == 2`.

## Files-to-touch

`fastdds-pubsub-provider/src/fast_dds_pubsub_provider.cpp`
- Add the destructor quiescence precondition comment.
- Do not add a destructor-wide lock.

`fastdds-pubsub-provider/include/fletcher/fastdds_pubsub_provider/fast_dds_pubsub_provider.hpp`
- If there is an existing destructor doc comment, mirror the quiescence precondition without changing signatures.

`fastdds-pubsub-provider/tests/test_fast_dds_pubsub_provider.cpp`
- Add the quiescent destruction contract test.

`xrcedds-pubsub-provider/src/xrce_dds_pubsub_provider.cpp`
- Copy callback, schema, envelope data, attachments, and pending envelopes to locals before callback invocation.
- Ensure no `TopicState&` or `ts.*` is used after a user callback begins on the same path.
- Keep dispatch under the existing recursive mutex.
- Add `FLETCHER_BUILD_TESTS` hook bodies that construct the real internal scenario and call the real `Impl::OnTopic()`.

`xrcedds-pubsub-provider/src/internal/xrce_test_hook.hpp`
- New non-installed test-only header.
- Declare free hook function(s) using only complete public or standard types.
- Guard with `FLETCHER_BUILD_TESTS`.
- Do not reference incomplete `Impl` in signatures or inline bodies.

`xrcedds-pubsub-provider/tests/test_xrce_provider.cpp`
- Add `ReentrantUnsubscribeNoUseAfterFree`.
- Assert `EXPECT_NO_THROW` and `delivery_count == 2`.

`xrcedds-pubsub-provider/tests/CMakeLists.txt`
- Add the internal `src/internal` include path for tests.
- Define or propagate `FLETCHER_BUILD_TESTS` for the provider test build so the hook declarations and bodies are compiled.
- Do not require sanitizers for this test.

## Step-2 review (cycle 3, 2026-07-09)

**Verdict: APPROVE.** Both cycle-2 open items are resolved. Regression
spot-checks all pass.

Item #1 — red-first characterization (RESOLVED). The doc no longer claims the
pre-fix path is "defined / no UB". §Forcing-test mapping (pre-fix) and §Risks now
state: the schema-flush 2nd iteration reads a destroyed `ts.pending[1]` Envelope
(object-lifetime UB, the same iterator-invalidation as defect #2), then invokes
the `Unsubscribe`-nulled `ts.callback` → `std::bad_function_call`; deterministic
in Release because `clear()` retains backing storage; no sanitizer needed; the
`bad_function_call` throw is the assertion point. Post-fix copy-to-locals →
`EXPECT_NO_THROW` + `delivery_count == 2`. Reconciles with defect #2; overclaim
dropped (line 165 explicitly forbids the "no UB" wording).

Item #2 — test seam compilable + header-safe (RESOLVED). `xrce_test_hook.hpp`
declares only a free function returning a POD (`ReentrantUnsubscribeResult`),
standard-type includes only, and explicitly must not name/dereference/require a
complete `Impl` (line 135) — so no incomplete-`Impl` dereference; it compiles.
Bodies live in the complete-`Impl` TU under `FLETCHER_BUILD_TESTS`, drive the
real `Impl::OnTopic()`, and must not reimplement delivery. Installed public
header untouched (H-INV-4). Header + CMake flag wiring are in Files-to-touch.

Regression spot-checks (all intact): #63 precondition-only, no destructor lock
(LOCKED #4); dispatch stays under the recursive mutex (LOCKED #10); scope limited
to fastdds + xrce providers; no blanket callback `try/catch`; spec's "erases the
TopicState" prose flagged as imprecise (in-place field reset is the real
behavior).

Non-blocking verification notes for the implementer (do NOT re-open the review):

- Build wiring: `FLETCHER_BUILD_TESTS` must reach the compilation of
  `xrce_dds_pubsub_provider.cpp` itself (the provider target as built for the
  test tree), not merely the test executable — otherwise the guarded hook bodies
  are elided and the test fails to *link* (undefined reference), which is the
  intent of "so the hook declarations AND bodies are compiled." Also wrap the
  `#include "internal/xrce_test_hook.hpp"` in the provider `.cpp` inside the same
  `#ifdef FLETCHER_BUILD_TESTS` so the header's `#error` guard never fires in a
  production build.
- Load-bearing premise: the entire pre-fix characterization rests on
  `Unsubscribe` performing an *in-place reset* (`ts.callback = nullptr;
  ts.pending.clear();`) and leaving the map node live. If the real `Unsubscribe`
  actually erases the map entry, the pre-fix UB flavour is a dangling
  `TopicState` (not `clear()`-retained storage) and the "deterministic
  `bad_function_call`" narrative must be revised. The doc correctly flags the
  spec's "erases" wording as imprecise; confirm the code matches before relying
  on the red observable.
