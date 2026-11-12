# D7O-2 — architecture review, cycle 1

**Verdict: APPROVE-WITH-DEBT(2).** No BLOCKER, no STOP-AND-ASK. Reviewed: `plans/D7O-2-no-timeout.md` and
`plans/D7O-2-brief.md` at `d60e8cd`.
**Ledger proof-of-read:** `plans/RIX-rulings.md` read top to bottom: **285 lines, 23 entries, last heading
"2026-11-12 — D7O hand-over: huge timeouts, and a huge max_rows"**. The design contradicts no ruling.

## Point 1: `kNoTimeout = milliseconds::max()` is a faithful reading, not a principle-vs-detail collision
- The principle (purpose, D14, D-D7O-1): the subscriber can ask for windows that never close on a clock and
  never get `kTimeout`, and can say so as a plain C value. The detail (L5, confirmed as law by LC-1): a timeout
  too long for the clock means no deadline. Both are tier 1, and they prescribe **the same observable result**
  for the same input. Naming the top of the range does not follow a detail *instead of* the principle. It
  satisfies the principle through a law the owner confirmed.
- `kNoTimeout` (INT64_MAX ms, about 292 million years) is always more than `kReach` (about 9.2e12 ms) on a
  nanosecond clock. So it never arms a deadline. That rests on the predicate, not on timing luck.
- The rejected alternatives are argued correctly. A negative value is refused by L15. Zero is lawfully the
  1 ms floor. A flag adds a contradictory state. The `int64_t` C form meets D-D7O-1, so premise 3's
  STOP-AND-ASK does not fire. D-D7O-1 leaves the spelling to the architect, so not asking was right.
- **D-D7O-4 holds. Rely on this at 4a/4b.** `timeout_ > kReach` in `ArmTimer` does not answer "acceptable?". It
  answers BC-4's per-window "deadline or none?", and it runs after BC-8 has accepted the value. It is not a
  second range check.

## Point 2: the arithmetic is overflow-free. Verified, rely on this
- `duration_cast<milliseconds>(nanoseconds::max())` has conversion factor 1/10^6, so it divides and cannot
  overflow. The result is `kReach` = 9 223 372 036 854 ms.
- For `timeout_ ≤ kReach`, `kReach × 10^6` = 9 223 372 036 854 000 000, which is ≤ INT64_MAX. So
  `window` is exact.
- `max − window ≥ 0` because BC-8 guarantees `timeout_ ≥ 0`. The final sum is formed only when it is
  representable.
- For finite timeouts in range, `deadline_` is the same value as today's (`cpp:196`). This changes no
  existing behaviour.
- Any clock period whose millisecond conversion would overflow already fails to compile, because `kReach` is
  `constexpr`. The `static_assert` is extra protection on top of that. MSVC, libstdc++ and libc++ all use
  `nano`. Optional tightening: DEBT-2.
- **The rung-3 red value is confirmed.** `(2^63−1)·10^6 mod 2^64 = −10^6 ns`, which is −1 ms.

## Point 3: the UB red run is sound evidence
- The defect under repair *is* the signed overflow. Both red arms execute it: the conversion overflows for
  `milliseconds::max()`, and the sum overflows for `duration::max()` in ms. A red that never runs the overflow
  cannot be shown.
- Premise 4 covers every outcome:
  - early `kTimeout` → red;
  - crash or UBSan abort → red;
  - hang → ctest timeout → red;
  - **pass → STOP.**
- No path through UB can produce a false green unnoticed. The tree has no sanitizer preset (grepped).
- The UB is confined to the pre-stage run. After the change, no arm forms the sum: the 100-year guard's
  `3.156e18 ns` fits both before and after.

## Point 4: the brief's Decision 1 is well-formed and is properly the owner's
- It has two options. Each has a Users line and an Architecture line that names L5, F1 and the DoD. The
  recommendation rests on L5. The default keeps the status quo and reports F1 open.
- It **is** an owner question. §2 is tier 2 and L5 is tier 1, and a tier-2 correction goes to the owner,
  never applied silently. F1 ("with the owner's approval") and the plan's DoD (`D7O-plan.md:58`) both say so.
  F1 records that approval is owed; it does not record that it was given.
- **"Made without asking"** (the spelling) is correctly not a question, by D-D7O-1.

## Ceiling (check 7b): fits, no split
- The work is priced as follows:
  - `ArmTimer` grows from 8 lines to about 13–15, plus its comment;
  - two `hpp` lines;
  - possibly one `#include <ratio>`.
- That is about **+12–15 / −3 production lines against 250**. The declared +10/−2 is the right order: no new
  type, verb, thread, lock or fixture. Tests (about +170) use only existing helpers (`BatchSink` `test:490`,
  `MockProvider` `test:40`, `RowLimitOptions` `test:1707`, `OnlyDroppedRowsStillDeliversEmptyBatch`
  `test:666`). Docs are not counted.
- Design: 271 of 300 lines. Public surface: 1 of 1 for the round, since D7O-1 added 0. The ladder's two-rung
  stage is the flagged plan shape, not an exception.

## Tree claims verified (rely on these; do not re-derive them)
- `timeout_` is read only at `cpp:41`, `cpp:45` (D7O-1's check and its message) and `cpp:196`.
- `ArmTimer` is at `cpp:193-200`.
- No-deadline readers: `Loop` `cpp:258-259`, `AddRow` `cpp:101`, `Close` resets `cpp:208-209`.
- Lost rows never reach `max_rows`: `cpp:117` reads `decoder_->num_rows()`.
- `BatchOptions` is at `hpp:77-83`, and "used as given" is at `hpp:110`.
- C++20: `pubsub-arrow/CMakeLists.txt:18`. Designated initialisers: `README.md:91`. A static data member
  keeps `BatchOptions` an aggregate and leaves its layout unchanged.
- **Rung 4 / D7O-3: nothing here is undone there.** `SetSchema` and `Reserve` are untouched. The sweep's
  "no timeout" and `milliseconds::max()` columns are now the same value, which is harmless.

## DEBT-1: the control can go flaky-red if it asserts *which* rows its `kTimeout` holds
- `last_flush_` is set when the batcher is constructed (`cpp:294`). On a slow run, a 10 ms deadline can close
  row 0's window early, or spread body (b)/(c)'s five lost rows over several `kTimeout` batches.
- "In (a), row 6" (design :200) is therefore true only under timing, and this control is declared green both
  ways.
- **Fix in step 3:**
  - assert only that some `kTimeout` delivery arrives within 2 s. That is the ladder's one-sided rule;
  - if you want content, assert conservation after `Unsubscribe`: Σ`num_rows` + Σ`rows_dropped` = 7 (a), or
    Σ`rows_dropped` = 5 (b, c).

## DEBT-2: step-3 notes (no design change)
- (a) **BC-4.** The row's single site still names `RecordBatchBatcher::Flush`, which D7F-3 replaced with
  `Close`. Design :93-94 says in the present tense that the row names `ArmTimer`. When BC-4 is edited, name
  both `Close` and `ArmTimer`, under the register's own rule.
- (b) **Rung-3 red, as observed.** Seven synchronous publishes probably finish inside 1 ms. If so, the red
  shows as a `kTimeout` holding row 6 that fails "still exactly 2", not the predicted "about 1 ms after row
  0". That is still an early `kTimeout` under premise 4: record it, do not stop.
- (c) **Optional.** `std::ratio_less_v` (strictly finer than 1 ms) would make `kNoTimeout > kReach` hold for
  every accepted build.
