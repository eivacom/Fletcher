# D7O-1 code review (step 4b): nonsensical options are refused

Diff: `555d899..9abb0c7`. Production change: `RecordBatchBatcher`'s constructor throws
`PubSubError(kInvalidArgument)` for `max_rows < 1` or a negative timeout, replacing the clamp.

**Verdict: no blocking findings, no should-fix findings, 4 nits.**

## What was checked and holds

- **Exception safety of the throw inside `make_shared`.** The throw comes from the constructor body,
  after every member is built, so `cb_` (the only member that owns anything) is destroyed during
  unwinding. `~RecordBatchBatcher` does not run, and nothing needs it to: `ended_` is still true
  and there is no thread. `make_shared` frees its block. The by-value `callback` parameter in
  `Subscribe` is moved-from and destroyed before the handler runs. `Start` and the provider
  `Subscribe` are never reached. `StopOnThrow` is declared after the throw point, so it never
  engages. Callback released, no thread, no provider subscription: confirmed.
- **Re-entrancy.** The option check runs before `subscriber_->Subscribe`, so a re-entrant call with
  bad options gets `kInvalidArgument` and not `kReentrantCall`. Both are loud and typed, and
  checking arguments first agrees with "refused before anything is subscribed". This is not a
  finding.
- **Design economy.** The check sits in the constructor, which is the only way to build a batcher.
  An invalid batcher cannot be represented, and no later code tolerates one: `max_rows_` and
  `timeout_` stay as given. This is the right direction.
- **Zero-timeout timing assertion.** It is one-sided (`arrived - t0 >= 1 ms`, with `t0` taken before
  `Publish`). `Loop` closes on `kTimeout` only after re-checking `now >= deadline_`, so an early
  `wait_until` wakeup cannot produce a false red. The 2 s upper wait is generous.
- **Huge values.** Two cases are currently wrong:
  - `milliseconds::max()` wraps to a deadline about 1 ms out.
  - `max_rows = INT64_MAX` fails `Reserve` in `SetSchema`, which drops the decoder for good.

  The plan already assigns both to D7O-2 and D7O-3 (ladder rungs 3 and 4), and d7 L15 marks them
  `[designed-not-built]`, so they are not findings here. See RECORD for the header text.

## Nits

- N1 (high) `InRangeBatchOptionsAreUsedAsGiven`: in both arms, and for `good` in the positive
  control, the sink or captured locals are declared after `sub`. If a `WaitFor` or `wait_for` ASSERT
  fails, they are destroyed before `~SubscriberArrow`'s `Stop()`, which can still deliver into them.
  That is undefined behaviour on the failure path only. The first test's own "declared first"
  pattern fixes it.
- N2 (medium) The `token.expired()` check depends on the moved-from `cb`, which lives until the end
  of the iteration, being empty. That is unspecified. libc++ copies small-buffer functors on move.
  It is safe here only because the lambda is heap-sized. Add `cb = nullptr;` after the call, or pass
  a temporary.
- N3 (high) On the new tree, the 200 ms sleep after `Publish` is dead time. Nothing is subscribed,
  and `subscribe_with_options_count == 0` already proves it. It only serves the red on the old tree.
  Keep it if that red is wanted, but say so in a comment.
- N4 (low) The test's `std::cerr` "observed count" line is diagnostic noise in a passing run.

## RECORD

- RECORD: hpp's "Any other value is used as given." is stated unconditionally, but it is false for
  `milliseconds::max()` and `max_rows = INT64_MAX` until D7O-2 and D7O-3. d7 L15 correctly marks
  that clause `[designed-not-built]`. Qualify the header or leave it as is, at the PM's discretion.
- RECORD: d7 L15 "Holds at: `cpp:33-45`". The constructor is at `cpp:34-47`.
