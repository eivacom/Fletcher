# D7F-3 code review (step 4b): `git diff 7080670 5b4e807`

Verdict: **nothing blocking**. The lock order holds: `mu_` is never held across `cb_` or `Subscriber::Unsubscribe`, and only the batcher's own `mu_` sits under any lock. No joinable `std::thread` is destroyed. The detached thread touches only its self-owned batcher, never `SubscriberArrow`. Every notify is `notify_all`, and every wait re-checks its state. `kClosing` is last by construction (the `stopped_` door plus the loop's queue-before-stopped order).

## should-fix

1. **A failed Subscribe leaves its batch thread waiting forever.** Confidence: high. `subscriber_arrow.cpp:360-383`. `Start` runs before `subscriber_->Subscribe`. That call can throw, for example kReentrantCall from a delivery callback (`subscriber.cpp:356`) or a transport error. When it throws, the local `batcher` goes away. The thread's `self` keeps the batcher alive, `stopped_` is never set, and `Loop` waits on `cv_` with no way to wake. `cb_` and its captures leak with it, and no error reports this. This existed before, but this item owns `Start`'s exception path. Fix: wrap the call, `catch (...) { static_cast<void>(batcher->Stop()); throw; }`. Stop returns false inside a delivery frame, and the thread then ends by itself.

2. **A batch callback that closes a *different* subscription can deadlock.** Confidence: medium. `subscriber_arrow.cpp:125-131`. Stop returns at once only for its *own* `loop_id_`. In every other case it blocks in `cv_.wait` until that batcher has drained. Two ways to hang:
   - A's callback calls `Unsubscribe(B)` while B's callback calls `Unsubscribe(A)`. Each waits for the other's drain.
   - `~SubscriberArrow` runs inside A's callback and waits for B, while B's callback calls `Unsubscribe(A)`.

   The old join-based Stop already had this deadlock. Forbidding-direction fix: one `thread_local` "inside a batch callback" flag, set around the `cb_` call. With it, Stop returns at once from *any* batch thread, and the flag replaces `loop_id_`. If that is outside the rulings, at least state it in the header next to the two "returns at once" cases.

3. **The detached thread finishes its teardown after the closing call has returned, and ended batchers stay in memory.** Confidence: medium. `subscriber_arrow.cpp:254-258, 126-131, 211-217, 438-441`.
   - On the detach paths, a later `Stop` from outside waits on `ended_`, then finds `thread_` empty and does not join. So `~RecordBatchBatcher` may run on the detached thread *after* `Unsubscribe`/`~SubscriberArrow` returns. It destroys `cb_` (and with it the user's captured objects) and the decoder on a thread nobody waits for, possibly during static teardown at exit.
   - Separately, a batcher whose Stop returned false stays in `batchers_`. It only leaves when a later `Unsubscribe` of another id prunes it, or at destruction. Meanwhile it holds builders that `Close(kClosing)` sized with `Reserve(max_rows_)` for a window that will never come.

   Fix: at the end of `Loop`, before `ended_ = true`, move `cb_` and `decoder_` out and destroy them with `mu_` released, and skip `Reserve` when the reason is `kClosing`. The tail then touches only the batcher's own small allocation, and keeping it in `batchers_` is cheap.

## nit

- `subscriber_arrow.cpp:222`: if `queue_.push_back` throws bad_alloc, it escapes `Close`. That terminates the program from the batch thread or from `~SubscriberArrow`, which breaks the "nothing may escape" invariant stated at :168.
- `test_pubsub_arrow.cpp:1717`: rung 3 is the only sub-second bound (500 ms). Use `kHold`.
- `test_pubsub_arrow.cpp:1989`: in rung 6 (a)/(d), a stall of more than 200 ms between rows 1-3 lets AddRow's L4 check close a kTimeout, so the run is red but the product is correct. A wider timeout costs nothing, because the held callback blocks the loop.
- `subscriber_arrow.cpp:125`: the row-thread detection only works for providers that dispatch through `DeliveryChannel`. A provider that pushes no token gets a blocking wait from a per-row callback.
- `subscriber_arrow.cpp:425-433`: `Unsubscribe` prunes other subscriptions' ended entries, an O(n) scan on every call. Once fix 3 makes those entries cheap to keep, the prune can go.

## RECORD:
- `tests/CMakeLists.txt` says "every wait … bounded (2-3 s), so a red run fails well inside the cap". On a red tree, rung 6's five sub-cases can each wait up to kHold + 2×kAwait (~8 s), about 40 s in total, which is over the 30 s TIMEOUT. Rung 3's bound is 0.5 s.
- The header's `~SubscriberArrow` doc says it "returns at once for that callback's subscription". It does not say that it still waits for every *other* subscription's drain (see should-fix 2).

## Cycle 2 (`git diff 7ce7266 HEAD -- pubsub-arrow`, HEAD a06031b)

Verdict: **nothing blocking**. S1 and S3 are closed. S2 is closed as documented.

- **S1, the guard: correct on every exit path.** Both locals are declared in order, so the guard is destroyed before `batcher`, and its raw pointer is still valid when it runs. On a throw from inside a delivery (the kReentrantCall case), `Stop` takes the frame branch and detaches. The loop then finds `stopped_` with an empty queue and ends by itself. On a throw from any other thread, `Stop` waits for a loop that has nothing to drain, so it returns at once. On a throw from another subscription's batch callback, `loop_id_` differs, and the wait is equally short. The new test covers the in-delivery case.
- **S3, the ordering: sound.** `cb_` and `decoder_` are released with `mu_` dropped and before `ended_ = true`. So neither the destructor's wait, `Stop`'s wait, nor the `Ended()` sweep can see the batcher as ended while either one still exists. Dropping `mu_` also lets a captured object's destructor call `Unsubscribe` or `~SubscriberArrow` safely: `Stop` takes the `loop_id_` branch instead of deadlocking. No `Close` or `TryAppend` can follow, because `stopped_` gates `AddRow` and `Stop` is idempotent.

### should-fix
1. **The seeded guards' 0.5 s waits now measure publisher speed, not product behaviour.** Confidence: medium. `test_pubsub_arrow.cpp:2194, 2196`. `reached`/`closed` open only once the publisher reaches `close_at`, which can be row 39. On Windows' default ~15.6 ms timer, each `sleep_for(3 ms)` (about one row in three) and each 1 ms batch timeout stretches to one tick. That puts a seed at about 0.2-0.25 s, which matches the measured 2.38 s / 16 seeds, so the margin is about 2x on a stalled runner. The cap was lowered to bound a *red* row (16 seeds × 3 waits). Bound it by stopping at the first failed seed instead (check the wait and `return`/`ASSERT`), which lets `kSeedAwait` go back to the 2-3 s range. `RowsConservedAtEveryEnding` (:2279) is less exposed: its wait starts after the last Publish.

### nit
- `subscriber_arrow.cpp:405-411`: the guard is disarmed before `batchers_`/`sub_topic_` are inserted. A bad_alloc there leaks the thread, and also a live transport subscription. OOM-only. Disarming after the insert would close it.
- `subscriber_arrow.cpp:65-68`: `SetSchema` does not check `stopped_`. A row whose schema has not been decided yet, arriving between `Stop` and `subscriber_->Unsubscribe`, rebuilds `decoder_` after the loop released it. That decoder is then freed by whoever drops the batcher last. Adding `if (stopped_) return;` refuses it.
- The rung 4/6 waits (`kHold`/`kAwait` = 1 s) against a 100-200 ms trigger leave about 5x margin. That is acceptable; I am noting it only because it is the same trade as should-fix 1.

## Cycle 3 (`git diff f55c525 HEAD -- pubsub-arrow`, HEAD 5293cab)

Verdict: **nothing blocking, nothing to fix.** The cycle-2 should-fix and both nits are closed.
- The guard is now disarmed after the inserts (`subscriber_arrow.cpp:405-411`), and the batcher is copied in rather than moved, so the guard's pointer stays valid. If `sub_topic_` throws, the `batchers_` entry is left stopped; its loop ends at once, and a later sweep or the destructor removes it. That is OOM-only.
- `SetSchema` now refuses once stopped (`:67`) and leaves `decided_` false. Late rows then call `AcquireCodec` until the transport unsubscribes, which is harmless because `AddRow` ignores them.
- The seeded guards' early stop (`if (HasFailure()) return;` at the end of each seed) is correct, and 2 s gives about 8x margin over a ~0.25 s coarse-timer seed. nit: on a red inside seed, a late hook can still call `sub->Unsubscribe` while `sub.reset()` runs. That is a test-only race that only exists once the test has already failed.
