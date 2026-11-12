# D7O-2 — No timeout (design)

Round D7O, domain 7, branch `feature/d7o-batch-options`, tree `9cd04de`. Story: `plans/D7O-plan.md`
§ D7O-2. Ladder: `plans/D7O-test-ladder.md` rungs 2 and 3. Locked: D-D7O-1, -2, -4, -5, -6. Step 0a:
`plans/reviews/D7O-2-audit-0a.md` (CLEAR). Rulings ledger read in full: 285 lines, 23 entries, last
heading "2026-11-12 — D7O hand-over: huge timeouts, and a huge max_rows".
Citations: `cpp` = `pubsub-arrow/src/subscriber_arrow.cpp`, `hpp` = its header, `test` =
`pubsub-arrow/tests/test_pubsub_arrow.cpp`, `d7` = `docs/domains/d7-batch-assembly.md`, `reg` =
`docs/domains/bounded-context-register.md`. Line numbers are those of `9cd04de`.

## Summary

"No timeout" is spelled as the top of the timeout's own range: a named constant
`BatchOptions::kNoTimeout = std::chrono::milliseconds::max()` (INT64_MAX ms, a plain `int64_t` for a C
caller). It is not a special case. L5 already says a timeout too long for the clock means no
deadline, and `kNoTimeout` is the longest timeout there is. So one change does the work of both
rungs: `ArmTimer` (`cpp:193-200`) arms a deadline only when `last_flush_ + timeout_` can be
represented, and otherwise arms none. That removes the overflow (D-D7O-2), and leaves the window to
close only when full, on the offset budget, or at closing. D7O-1's range check is not touched.

## Design

**The spelling: decided here, not put to the owner.** Three candidates were weighed against D-D7O-1,
L15 and D-D7O-3:

| Spelling | Verdict |
|---|---|
| Negative (C#'s −1 ms) | Forbidden. L15 refuses every negative timeout. |
| Zero | Forbidden. D-D7O-3: zero is accepted and means the 1 ms floor. |
| A separate flag (`bool`, or `std::optional<milliseconds>`) | Rejected. It creates a state the purpose forbids: "no timeout" *and* 5 s. That pair would need either a second refusal or a silent winner, which is "silently getting something else". `optional` also changes the existing field's type, and a C caller cannot pass it as a plain value. |
| **`milliseconds::max()`, named `kNoTimeout`** | **Chosen.** It is one field with no contradictory state. BC-8's predicate `timeout < 0` already accepts it. It is INT64_MAX as an `int64_t` across the C ABI (D-D7O-1). Its meaning is the meaning L5 already gives it, so there is one idle path, not two. |

Nothing a user can observe differs between "the flag" and "the constant", apart from the flag's
contradictory state. So this is a choice between mechanisms, and the simpler one wins. The brief
records it as made without asking.

**New public surface (1 of 1).** In `hpp`'s `BatchOptions` (`hpp:77-83`):
```cpp
/// No timeout: windows close only at max_rows, on the offset budget, or at closing; never on a clock.
static constexpr std::chrono::milliseconds kNoTimeout = std::chrono::milliseconds::max();
```
A static data member leaves `BatchOptions` an aggregate, so designated initialisers
(`{.max_rows = …, .timeout = …}`, README:91) still compile (C++20, `pubsub-arrow/CMakeLists.txt:18`).
The default stays 1 min (D-D7O-6).

**The one mechanism: `ArmTimer` arms only a deadline it can represent** (BC-4's single site). The
sketch below shows the intent. Its exact shape is the implementer's.
```cpp
void ArmTimer() {
    if (has_deadline_ || !HasPending()) return;
    using Clock = std::chrono::steady_clock;
    // L5: a deadline is never earlier than its timeout says. A timeout the clock cannot reach from the
    // previous flush (kNoTimeout is the longest) arms no deadline: the window closes only when full,
    // on the offset budget, or at closing (L4).
    static_assert(std::ratio_less_equal_v<Clock::period, std::milli>);  // premise 1
    constexpr auto kReach = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::duration::max());
    if (timeout_ > kReach) return;                                       // the conversion would overflow
    const auto window = std::chrono::duration_cast<Clock::duration>(timeout_);   // exact, cannot overflow
    if (last_flush_.time_since_epoch() > Clock::duration::max() - window) return;  // the sum would
    deadline_ = std::max(Clock::now() + kMinWindow, last_flush_ + window);
    has_deadline_ = true;
    cv_.notify_all();
}
```
- **Why both comparisons.** `milliseconds::max()` cannot even be converted to the clock's
  nanoseconds. Today the conversion wraps to −1 ms (ladder rung 3). The value
  `duration_cast<milliseconds>(steady_clock::duration::max())` converts exactly, but adding
  `last_flush_` to it overflows. Neither comparison can overflow: `timeout_ ≥ 0` is guaranteed by
  BC-8, so `max − window` is safe, and the left side is a plain read.
- **"No deadline" is not a new state.** It is today's `has_deadline_ == false`, now reached while
  rows are pending. Every reader of that flag already does the right thing:
  - `Loop` (`cpp:258-259`) waits on `cv_` alone. Only a queued batch (`Close` at row limit or
    budget) or `Stop` wakes it.
  - `AddRow`'s deadline check (`cpp:101`) never fires.
  - `Close` (`cpp:208-209`) resets the flag and `last_flush_`, so each later window asks again.
  No other code reads `timeout_` except the D7O-1 refusal message.
- **The closing path is unchanged** (`Stop` → `Close(kClosing)` → queue → `Loop`, `cpp:125-145`).
  So a loss-only window (no usable schema yet, an unusable schema, malformed rows) is delivered at
  closing as a null or zero-row `kClosing` batch, with `rows_dropped` counting it (L1, L6, D-D7O-5).
  Lost rows never count toward `max_rows` (`cpp:117` reads `decoder_->num_rows()`), which is why
  such a window waits for closing. This is audit F2, disclosed in the header and README.
- **Concurrency.** `ArmTimer` runs under `mu_` (from `AddRow`) as today. It reads `timeout_`
  (immutable) and `last_flush_` (under `mu_`), and adds no shared state, lock or thread.
- **Error paths.** None are added. The predicate cannot throw.

**BC-8 (D-D7O-4): the range check is unchanged.** `timeout_ < milliseconds::zero()` (`cpp:41`)
accepts `kNoTimeout` as it stands. D7O-1's design foresaw that "stage 2 extends this check", but the
chosen spelling needs no extension, and adding nothing is the strongest way to keep one check.
Rung 2's test proves that the check accepts the spelling: its `Subscribe` must not throw.

**Where "what the timeout means" is answered.** "Acceptable?" is answered at BC-8 (the
constructor). "Deadline or none?" is answered at BC-4 (`ArmTimer`). The second answer is
necessarily per window, because it depends on `last_flush_`. The register's BC-4 row names
`ArmTimer` as its site.

**Docs (not counted).**
- `hpp:80-82`: the `timeout` member comment gains the `kNoTimeout` sentence, plus: "any timeout too
  long for the clock to reach means the same; a long timeout the clock can reach gives a deadline at
  its true time". This closes F3.
- `hpp:102-106`: the batched `Subscribe` doc adds that with `kNoTimeout`, a window closes only when
  full, on the offset budget, or at `Unsubscribe`/destruction, and that a window of only lost rows is
  then reported only in the `kClosing` batch (F2). This closes F4. The existing sentence "Any other
  value is used as given" (`hpp:110`) becomes true for every timeout (4a N1). It stays untrue for
  `max_rows` until D7O-3.
- `pubsub-arrow/README.md:62-77`: the same, in user terms, with one example line (F5).
- `d7`:
  - L4's and L5's no-timeout and unrepresentable cases become `[built]`, with the tests below.
  - L5's "Until D7O … overflows" note (`d7:124-125`) is deleted.
  - L5's "Holds at" names `ArmTimer`.
  - L15's "used as given" clause becomes `[built]` for timeouts.
  - L1 and L6 gain one line on F2.
  - §4's `BatchOptions` bullet names `kNoTimeout`.
  - §(c)'s last bullet is resolved for timeouts.
  - **§2 Deadline (F1)** is amended only with the owner's approval (brief, Decision 1). Proposed
    text: "When a pending window times out: `max(now + 1 ms, last_flush + timeout)`, for a timeout
    the clock can reach from the previous flush; otherwise the window has no deadline (L5:
    `kNoTimeout`, or a timeout too long for the clock)."
- `reg` BC-4: the site gains "`ArmTimer`: the deadline, or none when the timeout cannot be reached
  from the previous flush (L5)". The test column gains rungs 2 and 3.

## Corner cases forbidden

Rung 1 (unrepresentable):
- **A subscription that has "no timeout" and a finite timeout at once.** There is one field, so
  this cannot be written.
- **A second idle mechanism** ("no timeout" path beside "too long" path). There is one predicate,
  and `kNoTimeout` is just its largest input.
- **An early or wrapped deadline.** The sum is never formed unless it can be represented, so
  D-D7O-2's overflow is unreachable rather than detected after the fact.
- **"No timeout" spelled negative or zero.** Negative is refused (L15). Zero keeps its meaning, the
  1 ms floor (D-D7O-3). The spelling is the opposite end of the range.
- **A clock coarser than 1 ms**, where `duration_cast<milliseconds>(Clock::duration::max())` would
  itself overflow. The `static_assert` refuses such a build.
- **A second range check.** None is added, and BC-8 is untouched.

Rung 2 (refused at the door): nothing new. Negative timeouts stay refused by D7O-1.

Handled residue:
- **A timeout the clock can reach from one flush but not from a later one.** This happens near
  `kReach`, as `last_flush_` advances. The first such window has a deadline at its true time; later
  ones have none. *Why not forbidden:* the bound depends on where the clock is now, not on the
  value. Refusing it at subscribe would refuse values L15 says are used as given, and it would be a
  second check. Each window answers it with the same predicate, so no extra branch exists. Nothing
  observable separates the two outcomes, because either way the deadline lies centuries away.

## Premises and stop conditions

1. **`steady_clock::period` is at most 1 ms** (nanoseconds on MSVC, libstdc++ and libc++). The
   `static_assert` enforces this. **STOP-AND-ASK** if a supported platform fails it. Do not weaken
   the assert.
2. **`steady_clock::now()` is far from `time_point::max()`**, so `now + kMinWindow` never overflows.
   This holds for since-boot clocks. It is not guarded. If a platform's epoch puts `now()` within
   1 ms of the maximum, STOP and report.
3. **A C ABI caller can carry the timeout as an `int64_t` count of milliseconds**, so INT64_MAX is
   `kNoTimeout` with no translation (D-D7O-1). If the shim round picks a narrower field, it maps
   its own maximum onto `kNoTimeout`. That is a binding concern and does not reach `c-abi/` in D7O.
   **STOP-AND-ASK** only if a reviewer reads D-D7O-1 as requiring a value other than an `int64_t`
   millisecond count.
4. **The pre-stage red run of rung 3 executes the signed overflow this stage removes** (undefined
   behaviour). On this project's compilers without UBSan it wraps as the ladder computes. The red is
   the defect itself, and cannot be shown without running it. If the red run shows anything other
   than an early `kTimeout` (a crash also counts as red), record what was observed. If an arm
   *passes* on the pre-stage tree, STOP and report. Do not re-declare it a guard silently.
5. **The Mock delivers synchronously** (`test:36`), so `AddRow` has run when `Publish` returns. If a
   body uses `InProcessPubSubProvider` instead, read only after `WaitFor` or after the ending.
6. **No edit under `c-abi/` or `dotnet/`.** STOP-AND-ASK if the build forces one.

## Forcing-test mapping

All tests go in `test`, suite `SubscriberArrowBatchTest`, using `MockProvider`, `BatchSink` and the
existing helpers (`MakeRow`, `PublishDirect` with `AppendByte(0x00)`, as in
`OnlyDroppedRowsStillDeliversEmptyBatch`). They are driven from outside only. **The observation
window is 200 ms and one-sided.** Every sink is declared before its `SubscriberArrow` (4b nit).

A shared file-local helper runs one **body** with a given timeout. The bodies are:
- **(a)** declared schema, `max_rows` 3, 7 `Publish` (rows 0-6);
- **(b)** topic never declared, `max_rows` 2, 5 `PublishDirect` rows;
- **(c)** declared schema, `max_rows` 2, 5 malformed rows.

**Rung 2, forcing: `NoTimeoutWindowsCloseOnlyWhenFullOrAtClosing`.** Every arm uses `kNoTimeout`.
- *(a), ended by `Unsubscribe`:*
  1. `WaitFor(2, 2 s)` gives two `kRowLimit` batches of 3 rows (rows 0-2 and 3-5), with
     `rows_dropped` 0.
  2. After the observation window, there are still exactly 2.
  3. `Unsubscribe` gives exactly one more: `kClosing`, 1 row (row 6), `rows_dropped` 0.
  4. After a second observation window, there are still exactly 3 (L9).
- *(a), ended by `~SubscriberArrow`:* the same, with the destructor as the ending.
- *(b):* nothing within the window. `Unsubscribe` gives exactly one batch: null, `kClosing`,
  `rows_dropped` 5, no attachments (L1, L6, L7).
- *(c):* nothing within the window. `Unsubscribe` gives exactly one batch: zero rows, `kClosing`,
  `rows_dropped` 5.
- *Turned green by:* `kNoTimeout` exists and BC-8 accepts it, and `ArmTimer` arms no deadline for
  it.
- *Red for the right reason on `9cd04de`:* **it does not compile**, because `kNoTimeout` does not
  exist (the ladder's stated red). That this test can also fail *behaviourally* is shown by the
  control.

**Control (green both ways, declared): `ShortTimeoutControlSeesTheDeadlineInEveryBody`.** These are
bodies (a), (b) and (c) with a 10 ms timeout. Each gets a `kTimeout` delivery within `WaitFor`'s
2 s: in (a), row 6; in (b) and (c), the lost rows. This proves that the observation window and the
helper see a deadline that exists. So rung 2's "nothing within the window" is not true by
construction. Lawful under L4 and L5 (a finite timeout gives a deadline).

**Rung 3, forcing: `TimeoutTooLongForTheClockBehavesAsNoTimeout`.** Body (a), ended by
`Unsubscribe`, with the same assertions as rung 2's (a) arm. The arms are:
- *`milliseconds::max()` (written as the literal, not `kNoTimeout`):* red on `9cd04de`. The
  conversion wraps to −1 ms, so a `kTimeout` arrives about 1 ms after row 0 (`cpp:196`), and the
  "exactly two `kRowLimit`" assertion fails.
- *`duration_cast<milliseconds>(steady_clock::duration::max())`:* red on `9cd04de`. The value
  converts, but `last_flush_ + timeout_` overflows to a deadline in the past, so `kTimeout` arrives
  after the 1 ms floor.
- *100 years (3 155 760 000 000 ms), a declared GUARD arm:* green both ways. The sum is
  representable today and after the change, so its deadline is at its true time, never within the
  test. It guards against a "fix" that refuses huge values, or one that misjudges where the clock's
  reach ends and fires early.

*Turned green by:* the two comparisons in `ArmTimer`. The implementer confirms each arm's state on
the pre-stage tree (premise 4).

**Order of the red work.** The implementer adds rung 3's test and the control first and runs them on
`9cd04de`: rung 3 is red on two arms, its guard arm is green, and the control is green. Then the
implementer adds rung 2's test and shows the compile failure. Each rung's red is shown separately.

**Existing tests: none retired or re-anchored** (step 0a, CLEAR). The finite stand-ins
(`NoTriggerBatchOptions`, `RowLimitOptions`, the 10-minute literals) stay on finite timeouts. They
keep L5's finite-timeout coverage, and moving them would only be churn.

## Risks / Unknowns

- **The ladder's "rung 3 may be partly green with rung 2".** Rung 3's `milliseconds::max()` arm is
  literally `kNoTimeout`'s value. Both rungs are built in one stage, so the arm is red on the
  pre-stage tree and green after. It does not need to be declared a guard. Only the 100-year arm is
  green both ways.
- **The red run relies on signed overflow** (premise 4). The exposure is limited to the pre-stage
  tree, which is the defect under repair.
- **A no-timeout subscription that never gets a usable schema reports nothing until it closes.**
  This is ruled (D-D7O-5, F2) and disclosed in the header and README.
- **C# (the switch round, not D7O).** C#'s `InfiniteTimeSpan` is −1 ms, which C++ refuses (L15). The
  binding maps it to INT64_MAX. Record this for the shim and C# switch rounds. It does not reach
  `dotnet/` here.
- **No coexistence window.** The overflowing sum is replaced in place. No later stage deletes
  anything this stage adds. D7O-3's sweep uses `kNoTimeout` as one of its values.
- **STOP-AND-ASK: none.** L4, L5, L1, L6, L9, L15, D14 and the 2026-11-12 rulings agree. The one
  tier-2 gap, §2's Deadline definition (F1), is silent rather than contrary. It goes to the owner as
  the plan's DoD requires.

## Files-to-touch

- `pubsub-arrow/src/subscriber_arrow.cpp`: `ArmTimer` (`:193-200`) and its comment (`:188-192`).
- `pubsub-arrow/include/fletcher/pubsub_arrow/subscriber_arrow.hpp`: `kNoTimeout`; the `BatchOptions`
  and batched `Subscribe` comments (F3, F4, F2).
- `pubsub-arrow/README.md`: § Batched RecordBatch subscribe (F5, F2).
- `pubsub-arrow/tests/test_pubsub_arrow.cpp`: the body helper, the two forcing tests and the control.
- `docs/domains/d7-batch-assembly.md`: L1, L4, L5, L6, L15, §4, §(c); §2 only if Decision 1 is
  approved.
- `docs/domains/bounded-context-register.md`: the BC-4 row.

**Net lines (production: `cpp` and the one `hpp` declaration): +10 / −2.** Tests are about +170.
Docs are about +25 / −6 and are not counted.

## Files-to-delete

- **The overflowing sum `last_flush_ + timeout_` and the early or wrapped deadline it produced**
  (`cpp:196`). It is replaced by the checked sum in `ArmTimer`. The behaviour is gone, and that is
  disclosed.
- **d7 L5's note "Until D7O, `last_flush_ + timeout_` overflows …" (`d7:124-125`).** It is replaced by
  the `[built]` mark and the "Holds at" text.
- **No file, type or test is deleted.** This stage adds a capability (one constant) and repairs one
  expression in place. No construct was retired to make room: the round budgeted this slot of
  public surface (D-D7O-1). The finite stand-ins in the tests are lawful (step 0a) and stay.
