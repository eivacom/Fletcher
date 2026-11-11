# D7F-3 compliance review (step 4a), cycle 1

**Verdict: ISSUES(3, 1 blocking).** Diff `7080670..5b4e807` (phase A `5e35328`, rulings and 0b `cbe7f34`,
phase B `5b4e807`). Judged against the design (rev 2 plus the 6(d)/6(e) rows), the brief, design
review cycles 1-2, DEBT C2-1/C2-2, D-D7F-1..7, d7 as amended, the register and the runbook census.

**Proof of read:** `plans/RIX-rulings.md` read top to bottom: **196 lines, 16 entries**. The last is
"2026-11-11 — The D7F-3 ladder gains rungs 6(d) and 6(e)". No tier-1 collision: both 2026-11-11 L9
rulings name L9 and widen its exception explicitly, and ruling (2) extends domain 3's 2026-09-04 rule
while leaving 2026-09-05 standing.

## B1 (BLOCKING): rung 6(e) has no red-on-base record and never went through the step-0b audit
The design's "Red-first order" requires every forcing test to be recorded red on the base before §1-§5
land. The ladder ruling requires ruling (1) to be *proven* by 6(e). Leg (e) of
`UnsubscribeFromInsideACallbackNeverDeliversNested` was written in the phase-B commit, together with
the fix (`git diff 5e35328 5b4e807` shows that the test file gains only (e)). Phase A's red window and
the 0b audit (at `5e35328`, scope "6(a-d)") cannot cover it. The TEST was already red on the base
through (a), so its red says nothing about (e). The design row's red reason for (e) is still a claim,
not evidence. No red record for (e) exists in `plans/`.
**Acceptable fix (evidence only, no code):** run leg (e) alone against `7080670`'s `src/`, and record
the failing assertions and their reason. Append a 0b addendum that audits 6(e), and have it note that
"then nothing" can only be read up to the snapshot, because the batch thread is detached.

## N1: L9, d7 §4 and the README overstate the destructor's in-callback exception; the header's destructor doc imports the wrong exception
Ruling (1) treats destruction inside a subscription's own batch callback like an in-callback
`Unsubscribe` *for that subscription*. The code does exactly that, per batcher `Stop`, and still waits
for every *other* batched subscription of the same `SubscriberArrow`, under L9's main clause. Three
texts say the destructor simply "returns at once": L9 ("... from inside the subscription's own batch
callback ... the call returns at once"), d7 §4 ("where they return at once") and the README ("Two
calls return at once instead", which then lists three). The hpp destructor doc is precise ("returns at
once for that callback's subscription"). But it opens with "as `Unsubscribe` does (see there)", which
pulls in the row-delivery-thread exception. Destroying from a row-delivery thread stays forbidden
(2026-09-05).
**Acceptable fix (text only):** in L9, §4 and the README, write "does not wait for that subscription's
batches". In the destructor doc, write "as an `Unsubscribe` from another thread does" and drop "(see
there)". L9 is tier 1, so show the owner the final sentence at close. This stays within D12's scope.

## N2 (note; pre-existing; no D7F-3 change asked): destroying from a row-delivery thread is stopped only while domain 3 has a provider subscription to tear down
C2-1 relies on "the program stops under either option": `~Subscriber` rethrows `kReentrantCall`
(sub:556-578). That fires only for a topic still `provider_subscribed`. Suppose every batched
subscription of X was already unsubscribed in-callback (provider teardown done, batcher still
draining), and X is then destroyed from a delivery thread on the same provider. Then `Stop` takes the
third case (it returns false and detaches), `~Subscriber` has nothing to unsubscribe, and
`~SubscriberArrow` returns with batches still owed. The act is forbidden, yet it is neither stopped nor
waited for. A lawful caller is covered, because the in-callback exception already obliges the callback
to outlive those batches. **PM:** record this under BC-5's residue or as a domain 3 question.

## Flagged by the implementer
**d7 L11 "timer thread" is a stale term, not a change in the law's meaning.** Design §1: the loop
replaces `TimerLoop` "on the same thread, started by Start". The thread is renamed, not replaced.
L11's principle ("the batcher outlives every thread running inside it") is unchanged and still holds,
through `Start`'s self capture. Its operational sentence is now incomplete but not contradicted:
`Stop` also detaches when called from a row-delivery thread on the subscription's transport (C2-2
(ii), ruling 2026-11-11 (2)), and L11 does not say "only". Not edited, as instructed.
**Production size:** I measured +118/−78 code lines (cpp +117/−78, hpp +1/−0; comments and blanks
excluded), net +40, against a declared +98/−64, net +34. That is far under the 250 ceiling. It is not
a conformance defect; it goes in the brief's "As landed" (RECORD).

## Checks that passed (rely on these in cycle 2)
1. **Design shape.**
   - §1: `Close` is the one closer and never releases `mu_`. `Loop` holds the one `cb_` call
     (cpp:240), drains a FIFO `std::deque` and records `loop_id_`. `ended_` starts true and is reset
     if thread creation throws. Every notify is `notify_all`.
   - §2: `AddRow` runs in design order: door, deadline close, no-decoder count, `TryAppend` twice
     around `Close(kRowLimit)`, then the count close.
   - §3: `Stop` is as designed, plus the Decision 2 (b) case `InsideDeliveryOn(provider_)`, which
     moves and detaches `thread_` (C2-2 ii).
   - §4: copy, not erase; the `Ended()` sweep; erase only on `true`; the destructor snapshots without
     clearing.
   - §5: `decided_` is set under `mu_`, `SetSchema` refuses a second decision before doing anything
     else, and `SchemaDecided()` reads lock-free.
   - Two deviations with no effect: `TryAppend` returns `bool` (the design has an enum), and the sweep
     skips `id` itself.
2. **D-D7F-7.** One `cb_` site, one `dropped_` counter (written only where §2 and `DiscardPending`
   place it), one closer (`Close`, called from `AddRow`, `Loop` and `Stop`). The retry block is gone.
3. **D-D7F-2.** The transport thread takes `mu_` only to append, close and queue. It never waits on
   `cv_`, joins, or runs a callback. `Stop` returns at once in both ruled cases (the subscription's own
   batch thread, and a row-delivery thread on its own transport). There is no bound and no blocking
   queue.
4. **D-D7F-5.** No public declaration changed. The diff adds only comments and the private
   `provider_token_` (DEBT C2-2; the design's ABI line discloses the layout change).
5. **No domain 3 edit.** Nothing under `pubsub/`, `core/` or `docs/pubsub-interface-spec.md` changed.
   `delivery_frame.hpp` is only included, and only `InsideDeliveryOn` (read-only) is called.
6. **Texts and flips.**
   - The hpp `Unsubscribe` doc and the README state ruling (2) and the earlier L9 rulings in behaviour
     terms, including "may not free until kClosing" (C2-2 iii). D12 and D13 restate rulings (1) and
     (2) faithfully.
   - No other law's statement changed: L1 and L12 keep their text, and only their marks and bullets
     moved.
   - Each flip is backed by a test: L1 (heavy rung 5, `RowsConservedAtEveryEnding`); L9 (rung 4 a-d,
     rung 6 a-e); L12 (rung 3, `DeliveryOrderStress`); BC-1 (`DeliveryOrderStress`); BC-2 (the
     conservation guard plus the heavy test); BC-5 (rungs 4 and 6,
     `InCallbackUnsubscribeWhilePublishingDoesNotDeadlock`).
   - BC-3 was already `[built]` at the base. Only its decided-site text changed (DEBT-6), and
     correctly.
7. **What survived.** The `if (stopped_) return;` at cpp:89 is `AddRow`'s door, which §2 keeps and
   which was present at the base. The guard inside the retry is gone. A grep of `pubsub-arrow/` finds
   none of these: `ready_` (gone since D7F-2), `once_flag`/`call_once`, `schema_set`, `schema_decided`,
   `TimerLoop`, `timer_`, `Flush`, erase-before-stop and clear-before-stop.
8. **Corner ladder.** Every state the design forbids is unrepresentable as designed. The one door
   (`SetSchema` after a decision) refuses, with no fallback. No refusal became a recovery path.
9. **Tests.** The 10 re-anchors match the design's list, and `ReuseAcrossWindows` is untouched. Phase B
   changed no phase-A assertion; it only added leg (e). D-D7F-4: the test carries the `heavy` label and
   is excluded with `-LE heavy`, not disabled.

## RECORD
- RECORD: the brief's "As landed": production +118/−78 code lines (net +40) against a declared +98/−64 (net +34).
- RECORD: d7 L11 and §1 say "timer thread", and L1 violation 1 says "`TimerLoop` flushes". These are now the batch thread and `Loop`.
- RECORD: the "Holds at" lines in d7 L1, L4, L9, L10 and L11, and reg BC-7 (`cpp:243-246`), cite pre-D7F-3 line numbers. For example, L9's `cpp:428-429` is now the `Ended()` sweep.
- RECORD: ladder rung 6 still cites "`t.get_id() == this_thread` (cpp:143)" and "the `exchange` guard, which becomes dead". It became `EXPECT_FALSE`.
- RECORD: reg BC-3's status column names only D7F-1 and D7F-2, though D7F-3 moved its decided-site. In design-debt, DEBT-6 and C2-2 are not yet marked as landed.
