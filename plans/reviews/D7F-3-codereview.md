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
