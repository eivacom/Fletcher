# D7O-1 — architecture review, cycle 1

**Verdict: APPROVE-WITH-DEBT(1).** No BLOCKER, no STOP-AND-ASK. Reviewed: `plans/D7O-1-refuse-bad-options.md`
and `plans/D7O-1-brief.md` at `16f3d7e`.
**Ledger proof-of-read:** `plans/RIX-rulings.md` read top to bottom: **285 lines, 23 entries, last heading
"2026-11-12 — D7O hand-over: huge timeouts, and a huge max_rows"**. The design contradicts no ruling.

## Point 1: the check in `RecordBatchBatcher`'s constructor is conformant (rely on this at 4a/4b)
- **Tier 1 is met as written.** L15 and "Refuse both" fix what callers observe: the batched `Subscribe` refuses
  with `kInvalidArgument` before anything is subscribed, and the rule lives in one place in domain 7. The
  constructor is domain 7 and has exactly one construction site (`cpp:371`; grep finds no other). It runs
  before `Start` (`cpp:373`) and before `subscriber_->Subscribe` (`cpp:383`). The throw leaves `Subscribe`
  through `make_shared`. D-D7O-3 is met.
- **D-D7O-4 is met.** Its principle is "one answer". The check runs as part of the batched `Subscribe` and
  nothing else reaches it, so "the batched `Subscribe` is the only place that decides" stays true. The
  constructor serves the principle better than the ladder's literal "top of `Subscribe`". RIX S4's
  `SubscribeIndexed(…, BatchOptions = {}, …)` (`plans/RIX-row-index.md:184`) inherits the check instead of
  needing a second copy. The ladder is tier 3, and departing from it with a stated reason is the design's call.

## What the design requires of the register (BC-8)
- **Nothing at design time.** Line 46 points to Files-to-touch, and lines 89-92 specify an edit that
  **the stage makes in step 3**. So the register being unchanged at `16f3d7e` is consistent with the design.
- **Required in step 3**, under the register's own rule that a round changing a single site says so:
  (a) the single site becomes the batcher constructor, reached only from the batched
  `SubscriberArrow::Subscribe`, before anything is started or subscribed;
  (b) the copy entry "constructor clamp (`cpp:36`)" is marked deleted (D7O-1);
  (c) the status becomes: check `[built]` (D7O-1), sweep owed by D7O-3. The structural-test column is unchanged.
- Today's BC-8 wording is already true of this design. The edit sharpens the site's name; it does not move
  the site. **D-D7O-4's digest wording needs no amendment**, and a close-time reviewer should not read the
  constructor as a second site.

## Point 2: out-of-range *and* re-entrant gets `kInvalidArgument`, which is conformant and not an owner question
- No tier-1 text orders these two refusals. Spec §6 clause 6 (`pubsub-interface-spec.md:901-912`) binds
  **seam** methods (`PubSubProvider`), and it requires only that the door comes before any lock is taken.
  `SubscriberArrow::Subscribe` is not a seam method, and the options check takes no lock and touches no state.
- The argument-first order already exists on this path. `JoinSegments` (`cpp:369`) refuses bad segments with
  `kInvalidArgument` before domain 3's door (`subscriber.cpp:356`) is reached. Domain 3's own
  already-subscribed path also raises `kInvalidArgument` (`subscriber.cpp:288-297, 324-327`) with no door.
- Both orders give a typed refusal that leaves nothing behind, so nothing fails silently. The new order is
  strictly better than the old tree: a bad-options re-entrant call no longer builds and starts a batcher that
  `StopOnThrow` then has to tear down. Disclosing it in the brief's "Decisions for you" is the right weight.

## Ceiling (check 7b): fits, no split
I priced the work the design commissions:
- one member initialiser changed;
- one predicate and one `throw` whose message names both values (about 3-6 lines with `std::to_string`);
- possibly one `#include`: `PubSubError` is at `core/.../status.hpp:136` (citation correct), and
  `subscriber_arrow.cpp` does not name it today.

That comes to **about +6-10 / −1 production lines against a ceiling of 250**, which agrees with the declared
+6/−1. Tests (about +110) and docs are excluded. The design adds no new types, verbs, migrations or fixtures,
and `MockProvider` and `BatchSink` already exist.
Other budgets: the design is 230 of 300 lines; new public surface is 0, so the round's one slot is still
free for stage 2; the brief is 60 of 60 lines.

## Tree claims verified (rely on these; do not re-derive them)
- When the constructor body throws, `~RecordBatchBatcher` and `Stop` do not run. `ended_` defaults to `true`
  (`cpp:290`). `StopOnThrow` is declared after `make_shared` (`cpp:377-382`). The callback is passed by value
  all the way into `cb`/`cb_`, and unwinding destroys every copy before the caller's handler runs. That
  supports "expired straight after `catch`", the same mechanism `FailedBatchedSubscribeReleasesItsCallback`
  relies on.
- Premise 2 holds: `subscriber.cpp:445-447` always calls the options form. `Subscriber` fans out to one
  provider subscription per topic. The Mock counter (`test:94`, `:103`) is **cumulative**: `Unsubscribe` never
  decrements it.
- Premise 5 holds: nothing under `c-abi/` names `SubscriberArrow`, `BatchOptions` or `RecordBatchCallback`.
- Later rungs are not undone by this stage. Any non-negative or separate-field "no timeout" passes
  `timeout < 0`, and stage 2 has to touch the constructor signature anyway to carry it. Rung 4 is unaffected.
- The guard's arithmetic holds. With timeout 0, deadline = `max(now + 1 ms, last_flush_)` = `now + 1 ms`,
  which is at least `t0 + 1 ms` (`cpp:187`). With `max_rows` 1, `cpp:108` closes a window on every row.

## DEBT-1: the forcing test's red-run claim contradicts its own UB guard
Lines 159-162 say every observation is red on `8e25cca` ("the counter reads 1, and the refused sinks receive
the `Publish`"). But lines 163-165 unsubscribe **every** non-throwing arm before the `Publish`. Run on the
old tree, that means:
- (a) the refused sinks stay empty, so "nothing delivered" is **green** there and is never shown able to fail;
- (b) the counter reads ≥ 1 because it is cumulative, not exactly 1;
- (c) an arm that unsubscribes before its `weak_ptr` check has its callback released by `Stop`
  (`cpp:260-264`), so that check would pass on the old tree too.

Only the `milliseconds::min()` arm reaches the overflow at `cpp:187`.
**Fix in step 3:** on the red path, check the `weak_ptr` right after the call, unsubscribe only the
`milliseconds::min()` arm, and keep the other arms live through the 200 ms window. Or declare the sinks
covered by the positive control. Either way, record the counter value actually observed.
