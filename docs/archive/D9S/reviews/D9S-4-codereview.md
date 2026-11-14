# D9S-4a code review (step 4b)

Diff `5b36a63..e766c3e`. Scope: binding.h, binding.cpp door, handles.hpp host, batches_server.cpp,
batches_edge.cpp, CMake/Conan/CI, test_batches.cpp, test_shim_batches_nothing.cpp.

**Verdict: no blocking findings. 2 should-fix, 5 nits.**

Checked and sound (one line each, so the next cycle does not re-derive them):
- Destruction order: `batches` goes first; `~SubscriberArrow` stops every batcher, then its `Subscriber`.
- In-callback destroy: the thunk captures only `(on_batch, ctx)`; the batcher is co-owned by its thread.
- L11: `batches_mu` covers only get-or-create, never held across `Subscribe` or a callback; no deadlock.
- Guard: declared after the zero-init structs; moved means not released, unmoved means released once.
- `milliseconds(INT64_MAX)` == `kNoTimeout` (64-bit rep); domain 7 guards the deadline sum.
- Bit 63: `fl_subscriber_unsubscribe(tagged)` reaches `Subscriber::Unsubscribe` as unknown: a no-op.
- Only noexcept steps follow `SubscribeBatches`; all conversions run before the subscription goes live.
- C99: `struct ArrowArray;`/`struct ArrowSchema;` forward-declared at binding.h:107-108.
- ODR: one `SubscribeBatches` per artefact; the test's second Arrow meets the shim only via C Data.
- Tests: `inprocess` is synchronous, so the `INT64_MAX` row is deterministic; waits one-sided; no leaks.

## Should-fix

**S1. A tagged id passed to `fl_subscriber_unsubscribe` answers FL_OK and does nothing; refuse it at
the door instead.** Confidence: medium (the design sanctions the no-op, B1/L13).
`binding.cpp:927` forwards the tagged id to the row `Subscriber`'s silent unknown-id path. A binding
that "unsubscribes" a batched id gets FL_OK, frees `ctx`, and keeps receiving batches: a silent
use-after-free. Suggested: throw `kInvalidArgument` on `subscription_id & kBatchedIdTag`.
**Disposition: DECLINED by the PM per d9 L13 (the round may change no laws).** The no-op is
documented in the unsubscribe doc instead (S1', fixed in e57e7af).

**S2. The header does not state the destroy contract the thunk was built for.** Confidence: medium.
`fl_subscriber_destroy` (binding.h:858-864) did not say that (a) it BLOCKS for running batch
callbacks and delivers CLOSING on another thread before returning, nor that (b) from inside a batch
callback it returns without waiting, so `ctx` must outlive the remaining deliveries.
**Fixed in 8383095; closed in cycle 3 together with B1.**

## Nits

- N1. Thunk: on export failure it builds a `PubSubError` only to catch and discard it; `return;`
  says the same. (D9S-6 replaces it anyway.)
- N2. The `catch (...)` also encloses `on_batch`; scope D9S-6's catch to steps 1-4, or a throwing
  callback gets a second delivery for the same window.
- N3. No 4a row hands an UNMOVED batch to the guard; "release exactly once" is untested until rung 5.
- N4. `TheMatcherFindsEveryPatternItIsGivenInItsOwnSource` cannot fail for any reason the guard
  cares about. Harmless, but not a control.
- N5. Linux (not yet run): Arrow recipe defaults `with_mimalloc=True` (initial-exec TLS in a
  dlopen'd `.so`). Confirm on the first Linux run, or set `arrow/*:with_mimalloc=False`.

## RECORD
- RECORD: D9S-4-batches-cross.md test row 1 (finite timeout, wait for 3) differs from the test
  (`INT64_MAX`, wait for 2, CLOSING via destroy). Update the plan to match the test.
- RECORD: The design's H3 says the thunk "throws into domain 7"; the code catches locally (D-D9S-4).
- RECORD: handles.hpp:180-181 credits declaration order with keeping the provider alive; the
  `SubscriberArrow`'s own `Subscriber` co-owns it, so the order only sequences the closes.

## Cycle 2

Diff `609ee23..8383095` (binding.h comments only).

**Verdict: 1 blocking, 1 should-fix, 2 nits. S2 does not close until B1 is fixed.**

True as written:
- "blocks until running batch callbacks finish": `Stop` waits on `ended_`, set after the queue drains.
- "on another thread": only `Loop` (the batch thread) calls `cb_`.
- In-callback return-without-waiting: `Stop` detaches when `this_thread == loop_id_`.
- "only fl_subscriber_destroy ends a batched subscription": unknown id is a no-op (subscriber.cpp:652).

**B1 (blocking, confidence high). "each subscription's CLOSING batch is delivered" and "`ctx` must
stay valid until CLOSING has arrived" are false for an idle last window.** `Close(kClosing)` returns
early when `!HasPending()` (d7 L6). Reachable: destroy inside a ROW_LIMIT callback with no rows
since. A C binding has no other end signal, so freeing on CLOSING leaks and waiting for it hangs.
Fix: "CLOSING *if the last window holds rows or drops*"; for in-callback destroy, refuse it in the
binding (as D-BIND-18 does) or keep `ctx` for the process. **Fixed in e57e7af; closed in cycle 3.**

**S1' (should-fix, confidence high). The S1 disposition sits in the wrong doc block.**
`fl_subscriber_unsubscribe` (binding.h:934-937) still said the context may be freed at once on
return, which is the S1 use-after-free for a batched id. Fix: one sentence there stating a batched
id is not cancelled (FL_OK, batches continue, `ctx` stays live; end it with destroy). Doc line
only, so L13 is untouched. **Fixed in e57e7af; closed in cycle 3.**

Nits:
- N6. Termination applies to a row delivery on ANY subscriber over the same provider, and to a
  batched-only subscriber too, not only "this same subscriber". Closed in cycle 3.
- N7. binding.h:1041 is 133 columns; rewrap missed. Closed in cycle 3.

## Cycle 3

Diff `8383095..e57e7af` (binding.h comments only).

**Verdict: CLEAR. 0 blocking, 0 should-fix, 1 nit. B1, S1', N6 and N7 are closed; S2 closes with B1.**

True as written:
- Outside a callback: `Stop` queues `Close(kClosing)` (returns at 242 when idle), waits on `ended_`.
- "Once it returns nothing runs": `Loop` drops `cb_` (sole holder of `ctx`) before setting `ended_`.
- Inside a callback: `Stop` detaches (136-140); "NOT guaranteed ... no reliable end signal" holds.
- N6: provider refuses `Unsubscribe` per instance (in_process_provider.cpp:343); `~Subscriber`
  rethrows `kReentrantCall` (578); batched-only hits it via `BatchHost`'s `subscriber_.reset()` (357).
- S1': binding.cpp:927 hits the unknown-id return (subscriber.cpp:651-658); `Contain` answers FL_OK.
- N7: IDS paragraph rewrapped; every changed line under 100 columns.

Nit:
- N8. The "process termination" line holds only when the destroyed subscriber still has a live
  transport subscription; one with no topics destroys cleanly (subscriber.cpp:538-558). Errs safe.
