# D7O-2 code review (4b): No timeout

Diff `920b3ec..7a0cc64`. Production: `ArmTimer` (`subscriber_arrow.cpp:199-213`) and
`BatchOptions::kNoTimeout` (hpp). Tests: three new `SubscriberArrowBatchTest` cases (+277).

**Verdict: 0 blocking · 0 should-fix · 3 nit.**

## What was checked and holds

- **Deadline arithmetic.** `kReach = duration_cast<ms>(ns::max())` truncates toward zero, so
  `kReach * 10^6 <= ns::max()` and the `duration_cast<ns>(timeout_)` that follows is exact and
  cannot overflow. `max() - window` cannot overflow because `window >= 0` (BC-8 refuses negative
  values). `last_flush_ + window` is formed only after the guard. `now + kMinWindow` and a negative
  `time_since_epoch` (which no real clock has) cannot overflow either. Timeout 0 still gives
  `max(now + 1 ms, last_flush_)`, so the 1 ms floor is unchanged. A timeout just under the
  reachable limit gives a deadline near `time_point::max()`. `Loop`'s `wait_until` on it reaches
  MSVC's clamped relative wait (`_Abs_time - now` is positive and cannot overflow). The 100-year
  guard arm runs this wait for real.
- **No deadline.** `AddRow:102` short-circuits on `!has_deadline_`. `Loop:271` waits on `cv_`
  alone. Every way a window closes still notifies: `Close` (row limit, offset budget) and `Stop`
  (closing) both call `notify_all`. While pending, `ArmTimer` re-runs on each event, and
  `last_flush_` does not move within a window, so the answer is the same every time.
- **`kNoTimeout`.** A `static constexpr` data member is implicitly inline in C++17/20, so it has
  no ODR problem and needs no out-of-line definition. A static member does not take part in
  aggregate initialisation, so `BatchOptions` stays an aggregate and designated initialisers still
  compile. It adds no instance layout, so the ABI is unchanged.
- **Tests.** Every timing window is one-sided: they assert that nothing arrived in 200 ms, or
  that something arrived within 2 s. The control (10 ms) always has a pending window: row 6, or
  the drops. Its assertions are on totals only, so they do not depend on timing. Lifetime: in
  every test `BatchSink` is declared before `BodyRun`. Inside `BodyRun` the members are destroyed
  in the order `sub_`, `pub_`, `mock_`, so the subscriber dies first on every ASSERT-return path.
  The D7O-1 finding is fixed here. In the control, `lock_guard` is declared after `run`, so it
  is released before `run` is destroyed. Live subscriptions are ended by `End` or by
  `~BodyRun → ~SubscriberArrow`, which joins the batch thread, so no thread leaks.
- The test comments' arithmetic is right: `INT64_MAX` ms converted to ns wraps to exactly −1 ms,
  and `kReach` plus `last_flush_` overflows once uptime exceeds 0.78 ms.

## Nits

- N1 (high): forbidding direction. Half of the "deadline or none" answer does not depend on the
  window: whether `timeout_` converts at all (`timeout_ > kReach`) and the `duration_cast`. Both
  could be settled once in the constructor (store `std::optional<Clock::duration> window_`), so
  `ArmTimer` keeps only the per-window sum guard, and "no timeout" becomes a value instead of being
  worked out again on every event of a no-deadline window. This is cheap either way, so it is not a defect.
- N2 (medium): the second `Observe()` after `End()` in both helpers asserts something `End`'s
  synchronous contract already guarantees, since the batch thread has been joined. It adds about
  1.4 s of sleep across rungs 2 and 3 and cannot fail.
- N3 (low): no-timeout + offset-budget close is not exercised. That path never touches the
  timer, so this is only a coverage note.

RECORD: d7-batch-assembly.md L5 "Holds at: `ArmTimer` …, `cpp:200`, `cpp:285`": `cpp:285` is now
`lk.unlock()` in `Loop`'s tail and `cpp:200` is `ArmTimer`'s first line. The deadline readers are
`cpp:102` and `cpp:271-276`.
