# D9S-5b code review (step 4b), `f89644f..60ff057`

**Verdict: APPROVE.** 0 blocking · 1 should-fix · 3 nits. Read only, nothing was built.

The gate is sound. `registered_` is written and read only under `mu_`, and every `Close` caller holds `mu_`. `MarkRegistered` sets the flag under the lock and then calls `notify_all`, and `Loop` re-checks state under the lock before both waits, so no wakeup is lost. An unregistered window waits in order (`!has_deadline_`: `cv_.wait`; deadline: `Close(kTimeout)` queues it and clears `has_deadline_`, so it does not spin). A stop while unregistered and waiting wakes the loop, which takes the `stopped_ && !registered_` branch first, swaps the queue out, and frees it after `lk.unlock()` without calling the callback.

Exactly one closing, and it comes last: the `stopped_` guard in `Stop` runs once. After `stopped_` nothing can queue, because `AddRow` returns early and `Loop` never reaches `Close(kTimeout)` once it is stopped.

In-callback close and destroy: `Stop` queues the closing, detaches the thread and returns false. The loop delivers the rest after the callback returns. The thread co-owns the batcher, and the thunk holds only `(on_batch, ctx)`.

Shape: idle with a decoder, `Finish()` gives zero rows of the topic's schema; idle without one, the batch is null; both report 0 dropped.

Shim: nothing after `SubscribeBatches` can throw, and the `SchemaArrival` copy in `return {...}` is a `shared_ptr` copy and does not throw. So "refused means never delivered" holds through the C ABI.

C++ use-after-free when captured state dies first is documented: README :108 and :117-121, hpp :129-136 and :183-189, and the design's Risks section.

Flakiness: the waits are one-sided (`WaitFor(n)` / `WaitForDeliveries(n)`). Each exact-count assert after one of them is safe for one of three reasons:
- An outside close is synchronous.
- In the inside and row-delivery cells, the held closing turns "the close did not wait" from an inference about timing into a direct observation.
- The shared observation window can only miss a late delivery; it cannot fail by mistake.

D7-2 is deterministic. `DeliverThenFailProvider` calls back synchronously and then throws outside any delivery scope, so `StopOnThrow`'s `Stop` waits for `ended_` before `calls` and `token` are read. F2's `next` has no data race, because `inprocess` delivers on the publishing thread.

## Findings

**S1 · should-fix · confidence medium.** An unregistered `Stop` built and queued a `kClosing` (design rule 3 says it discards; m4 could not go red; `bad_alloc` during unwind could terminate). Fixed in f140eb3; closed in cycle 2.

## Nits
- N1 (high): F2's inside half cannot catch the race. `inprocess` holds its instance mutex through the row delivery that makes the attempt, so the flood cannot reach `busy` during it. Only m2 can turn that half red. The test's header comment ("half … from inside") should say so.
- N2 (medium): D7-2's precondition `provider->delivered() == kCaught` counts the provider's own callback calls, not rows reaching `AddRow`. If domain 3 stopped routing rows before publishing the entry, the test would pass with nothing tested. Add a positive control, e.g. the same provider without the throw delivers `kCaught` rows.
- N3 (low): a `Stop` that lands between `batchers_[id] = batcher` and `MarkRegistered()` loses an idle closing silently. It is unreachable, because it needs the id before `Subscribe` returns (P5). Worth a comment at cpp:472.

## RECORD:
- `plans/D9S-5b-idle-close.md` rule 3 and m4 describe a discard in `Stop` that the tree does in `Loop` (moot if S1 lands as proposed).

## Cycle 2

`60ff057..f140eb3`. **Verdict: APPROVE.** 0 blocking · 0 should-fix · 0 nits. S1 is closed. Read only, nothing was built.

**S1 fix (cpp:257).**
- The lock is right: all three `Close` callers hold `mu_`.
- The terminate path is closed: during unwind, `Close(kClosing)` returns before `Finish()` and `queue_.push_back`, so nothing allocates.
- Pending rows are freed with `decoder_` at the end of `Loop`, outside `mu_`; `atts_` goes with the batcher.
- The rewritten idle test (`reason != kClosing`) is equivalent: past the new line, `kClosing` implies `registered_`.
- Loop's discard is unchanged and still frees windows closed earlier (`kRowLimit`, `kTimeout`) outside `mu_`.
- Skipping `has_deadline_ = false` is harmless, because `Loop` takes the discard branch first.

**Two gates: acceptable, keep both.** They do not overlap in purpose.
- `Loop`'s gate is the only one that matters for correctness: it holds back pre-registration windows and discards them on refusal. The `max_rows 1` cell kills m3.
- The `Close` gate only keeps unwind allocation-free, so deleting it is an equivalent mutant for every delivery-observing test. That is the nature of a no-throw guard, not a design flaw. Do not write a test for it.
- The `max_rows 100` cell now guards the pair: it goes red only if both gates are gone. Keep it.
- Step 3 / close should record this mutant as equivalent so nobody expects it to go red.

**Comments.** N3 (cpp:476): accurate for `Unsubscribe`; still unreachable under P5. `Stop` (cpp:134): matches the code: exactly one `kClosing`, behind what is already queued; none when unregistered; idempotent.

## RECORD:
- cpp:476: the destructor also closes, and it does so without an id. It is excluded only because destroying the object during a member call breaks the contract. The comment names only "closing needs the id".
- cpp:136: "gets none" reads as "no kClosing". An unregistered batcher gets nothing at all, because its queued windows are discarded too.
- d7 L1: the "except rows caught by a refused `Subscribe` (below; D19)" clause appears twice, once in the headline and once in the sentence after it.
- Plan: list "delete the `Close` kClosing gate" as an equivalent mutant (only reachable under `bad_alloc`).
