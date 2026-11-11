# D7F — Evidence ledger

| Item | Forcing tests | Inner loop | Full suite | Recorded |
|---|---|---|---|---|
| D7F-1 | `SubscriberArrowBatchTest.NoUsableSchemaDropsReportedAt{Unsubscribe,Destruction}`, `.SchemaDeclaredAfterSubscribeDropsReportedAt{Unsubscribe,Destruction}` — green; red before the fix (0 deliveries vs 1), reported by the implementer at phase A | pubsub-arrow 39/39 passed (PM run, forced rebuild, 2026-11-10) | pubsub-arrow-fastdds 4/4 (implementer, 2026-11-10) | `a2c8b97` |
| D7F-2 | `SubscriberArrowBatchTest.NoUsableSchemaDropsReportedAtEachDeadlineThenLateSchemaAssembles`, `.SchemaDeclaredAfterSubscribeDropsReportedAtEachDeadline` — green; red before the fix (implementer, phase A) | pubsub-arrow 43/43 passed (PM run, forced rebuild, 2026-11-11, at 7170ca9) | pubsub-arrow-fastdds 4/4 (PM run, 2026-11-11) | `dbaa2b6`, `7170ca9` |
| 2026-11-10 | D7F-1 | `NoUsableSchemaDropsReportedAtUnsubscribe` | a2c8b97 |
| 2026-11-11 | D7F-2 | `NoUsableSchemaDropsReportedAtEachDeadlineThenLateSchemaAssembles` | 7170ca9 |
| 2026-11-11 | D7F-2 | `NoUsableSchemaDropsReportedAtEachDeadlineThenLateSchemaAssembles` | df5aa0c |

## D7F-3

### 6(e) red on base

Leg (e) of `SubscriberArrowBatchTest.UnsubscribeFromInsideACallbackNeverDeliversNested`
(`~SubscriberArrow` inside a held `kTimeout` callback, `max_rows` 2, timeout 200 ms; while held, rows
x = 1, 2 queue a full window and x = 3 is pending), run **alone** against `7080670`'s production
`pubsub-arrow/src` and `include`, with the tests (and test CMake/conanfile) of `7ce7266`. Method: a
temporary detached worktree at `7080670`; the four test-infrastructure files copied from `7ce7266`;
the rung-6 TEST body cut to leg (e) only; `conan build` with tests skipped, then the gtest binary run
with `--gtest_filter=*UnsubscribeFromInsideACallbackNeverDeliversNested`. Implementer, 2026-11-11,
three runs: **red each time, the same 5 failed assertions**, about 0.2 s each (no bounded wait
timed out):

1. `at_unsubscribe_return == 1` failed, actual **3**: when the in-callback `~SubscriberArrow`
   returned, three deliveries were recorded. The `kRowLimit` had already run on the publishing thread,
   and the destructor itself delivered `kClosing` before returning ("a batch was delivered before the
   closing call returned").
2. `max_in_flight <= 1` failed, actual **2**: the queued window's `kRowLimit` callback ran on the
   publishing thread while the `kTimeout` callback was still held (L12).
3. `max_depth <= 1` failed, actual **2**: the `kClosing` batch (x = 3) was delivered nested inside the
   held callback, by the in-callback destructor's `Stop`/`Flush(kClosing)` on the timer thread (L12,
   ruling 2026-11-11 (1)).
4. Delivery 1 (`kRowLimit`, first x 1) was not on the thread of delivery 0 (L12).
5. Delivery 1 was on the publishing thread (D10).

The recorded sequence matched the expected reasons and rows (`kTimeout` x0 (1), `kRowLimit` x1 (2),
`kClosing` x3 (1)), so the red is about the thread, overlap and nesting, not about content. **"Then
nothing" is read only up to the snapshot**: on the base the timer thread is detached by the
in-callback `Stop`, so nothing waits for it, and the test reads after every recorded callback has
returned.

### `FailedBatchedSubscribeReleasesItsCallback` red on base

The guard added in fix cycle 1 (code review 4b S1), run with the full light suite against
`7080670`'s production `pubsub-arrow/src` and `include` and the tests of fix cycle 1 (temporary
worktree, implementer, 2026-11-11): **red**, 1.05 s. The one failed assertion is
`captured.expired()`: "the failed Subscribe's batch thread still holds the callback it was given".
The batch thread started before the refused `Subscribe` was never stopped, so it kept the callback
for the life of the process. Green on the fix; red again with the fix disabled (a local control).
