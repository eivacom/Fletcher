# PDA-DEC-A4 — architecture review, cycle 1 of 2

Design: `plans/PDA-DEC-A4-lifetime-tier.md` (240 lines, `d2acfb9`).
Brief: `plans/PDA-DEC-A4-brief.md`.
Rulings ledger read in full first: `plans/PDA-DEC-rulings.md`, **34 entries**
(`grep -c '^## 2026'` → 34).

**Verdict: NEEDS-REWORK — 2 BLOCKERs, 5 DEBT, 1 STOP-AND-ASK ruled below.**

Findings only. Everything not named here I checked and had nothing to say about;
in particular the gate mechanism *does* forbid rather than document (retirement
and invocation take the same lock, so "retired but being invoked" is not a
representable interleaving), the three primary forcing cases are red today for
the reasons stated, `Files-to-delete` is real, there is no coexistence bridge,
and the doc is inside budget (240 ≤ 300 lines, 0 new public surface, net lines
declared).

---

## Tree claims spot-checked (all true)

- `ProviderConformance` never touches `Publisher`/`Subscriber` — `grep -c
  'Subscriber|Publisher' integration-tests/pubsub-conformance/src/clauses.cpp` →
  **0**. The design's central claim about why the class is invisible holds.
- `provider.hpp:156-160` says an unknown topic is *"a no-op, not an error, so it
  is safe to call unconditionally on teardown"* — verbatim, at those lines.
- `subscriber.cpp:177-181` throws `kInvalidArgument`; `:84-88` is the snapshot
  fan-out; `subscriber.hpp:64-72` is the "intentional and by design" paragraph;
  `test_publisher_subscriber.cpp:339` and `:406` are the two defect-pinning
  cases. All as cited.
- P4 holds: `next_id` is `std::atomic<uint64_t>{1}` with `fetch_add`
  (`subscriber.cpp:56,143`) and erasure returns nothing to a pool.
- The fifth-suite precedent is real: `conformance_copy_accounting`,
  `conformance_seam_vocabulary` and `conformance_registry` are each their own
  binary with `gtest_main` and no transport SDK
  (`integration-tests/pubsub-conformance/CMakeLists.txt:167-213`), each with a
  declared `TIMEOUT`.
- No hidden cross-cutting change: the only above-seam callers of
  `Subscriber::Unsubscribe` are `pubsub-arrow/src/subscriber_arrow.cpp:392` and
  `gateway/src/ws_session.cpp:246,334`. Neither calls it from inside a delivery
  callback, neither depends on the `kInvalidArgument` throw, and the gateway's
  delivery callback is `net::post` (`ws_session.cpp:205`), so the new blocking
  `Unsubscribe` cannot wedge the session strand. `Files-to-touch` is correct as
  it stands.

The design carries no hand-composed post-change ledger, no survival table and no
caller count. Nothing to delete on that head.

---

## BLOCKER 1 — the load-bearing ordering rule is proved over two resources; the design introduces three

§1's rule is: *"`impl_->mu` is never held while a gate is acquired; a gate may be
held while `impl_->mu` is acquired … One direction only, therefore no cycle."*
That is true of the pair {`mu`, gate}, and false as a statement about the
design's whole lock graph, because the design adds two edges it does not name:

**Edge A — gate → gate.** The delivery loop holds `*entry.gate` *for the whole of
one invocation*, and the design promotes cross-subscription unsubscribe from
inside a callback to a feature (*"unsubscribing a **different** subscription from
inside a callback takes effect within that same delivery"*). So a callback
running under gate(E1) can call `Unsubscribe(E2)`, which now **blocks** on
gate(E2). Two concurrent deliveries on two topics of the same `Subscriber` — Fast
DDS runs a listener per reader, so this is ordinary — whose callbacks cancel each
other's subscriptions is a plain ABBA deadlock. Before this change `Unsubscribe`
never blocked, so the cycle did not exist; the design creates it and then asserts
in the *forbidden* table (row 5, rung 1) that lock-order inversion cannot occur.
Neither `SelfUnsubscribeInsideItsOwnCallbackReturns` nor
`ReentrantSubscribeFromInsideDeliveryDoesNotDeadlock` can go red on it: both are
single-delivery cases about the `mu`↔gate edge.

**Edge B — gate → the provider's own lock.** `Unsubscribe` still calls
`provider->Unsubscribe(segments_to_unsub)` on the last-subscription path
(`subscriber.cpp:205-206`), and `~Subscriber` still calls it per topic
(`:125-130`). The design says where `mu` is released relative to the gate and
says nothing about where the gate is released relative to the provider call. The
natural reading of *"takes the gate and sets `retired = true`"* as a
function-scoped `lock_guard` holds gate(E) across `provider->Unsubscribe`, and
Fast DDS's `Unsubscribe` *"waits for any in-flight schema delivery to finish"*
(verification A3, `fast_dds_pubsub_provider.cpp:570-574`) — a delivery which is
at that moment blocked on gate(E). Deadlock, from a scoping choice the design
leaves to the implementer.

Consequence in both cases is a hang with no typed signal — the failure class
§12.4 records as costing two hours of runner time on this very branch.

**Acceptable fix (cheapest first).** State the ordering rule as a *total* order
over all three resources — `impl_->mu` < gate < provider — and forbid the two new
edges rather than handle them: no gate is held while another gate is acquired,
and no gate is held while the provider is entered. Edge B is a one-line scoping
statement in §1 plus a `Files-to-touch` note. Edge A is cheapest to *forbid* with
the alternative §1 rejected: a `thread_local` delivery marker, under which a
thread already inside a delivery never blocks on any gate — it sets `retired` and
returns, and the per-entry check at invocation already makes the guarantee hold on
that thread. That also lets the gate be a plain `std::mutex` (see DEBT-2 and
DEBT-5). If Edge A is kept instead, it must move out of the forbidden table into
*handled residue* with its reachability argument written out, and the README must
disclose it — but a "forbidden, rung 1" row that a control cannot redden may not
ship as it stands.

## BLOCKER 2 — unstated premise: the last-subscription path re-enters the provider from inside its own delivery

`Subscriber::Unsubscribe` calls `provider->Unsubscribe` when the topic's last
entry goes. Issued from inside that subscription's own callback, that is a
re-entrant provider `Unsubscribe` from inside a provider delivery — and
verification A3 established what the tree does with it:

- **loopback: deliberate deadlock.** `in_process_provider.cpp:248` — *"mu_ is
  held across the callback: one delivery at a time, so a callback must not
  re-enter"*; `:270-275` — *"mu_ is non-recursive, so a re-entering callback
  deadlocks rather than corrupting — which is what the contract forbids."*
- **Fast DDS: undocumented self-wait** (`fast_dds_pubsub_provider.cpp:570-574`).
- **XRCE: safe** (`recursive_mutex`).

The design says nothing about this path. Its forbidden table asserts
*"Self-unsubscribe deadlocking on its own frame | rung 1 | recursive gate — owner
re-entry always succeeds"*, and its control
(`CallerTier.SelfUnsubscribeInsideItsOwnCallbackReturns`) runs against **a probe
provider defined in-file**, which by construction cannot exhibit the hang. So a
rung-1 forbidden claim would ship that is false against two of the three shipped
providers, proved by the one subject that cannot see it. The brief states it to
the owner more broadly still: *"Forbidden (cannot occur, by construction): … a
cancellation deadlocking against the handler that issued it."*

This is also where the owner's 2026-09-03 note bites — *"A4's design touches the
same callback path and must not quietly pre-empt"* `kReentrantCall = 10`. The
provider-tier answer is **A3's**, and A4 must neither fix it nor claim it away.

**Acceptable fix — scoping, not handling; forbidding here is not cheaper because
the case is not A4's to forbid.** Add premise **P5**: *the provider tier's answer
to a re-entrant `Unsubscribe` is unresolved and is A3's (`kReentrantCall`); on the
loopback and Fast DDS the last-subscription path deadlocks today, unchanged by
this item.* Give it a stop condition (do not fix it here; if the implementer is
tempted, that is a stop-and-ask). Scope forbidden row 4 and the amended clause-6
sentence explicitly to **this tier's own gate** — the sentence *"does not wait for
the frame it is in"* is fine, a liveness promise about the whole call is not.
Disclose the residue in the harness README beside the two limits already listed,
and correct the brief's Forbidden line so the owner is not told this cannot occur.

---

## STOP-AND-ASK — ruled: the Unsubscribe-idempotence change needs its own owner authorisation

The design's **P3** asks for this ruling explicitly and refuses to land either
half without it. That is the right behaviour and is not itself a finding. My
ruling, on the merits:

**Yes, it needs its own authorisation, and a brief default-on-silence is not
enough.** Three reasons, in order of weight:

1. The 2026-09-03 ruling authorises *"all eight amendments"*. Idempotence is
   verification **finding #5**, which says of itself: *"Not among the nine, and it
   compounds A4."* It reaches A4 only through `plans/PDA-decouple-interface.md:109`
   — a **PM scoping line**, not an owner ruling. The PM cannot enlarge an
   authorisation over frozen text.
2. The change writes a new normative sentence into **§7's clauses**, which
   §12.1 names `frozen`, and whose *who may act* is **"nobody alone"**. A
   recommendation with a stated default is exactly acting alone if the owner never
   answers.
3. It is a **decision, not a correction**. Verification says plainly: *"Neither §7
   nor §9 says which tier is right."* Aligning `subscriber.cpp` to
   `provider.hpp:156-160` looks like repair only if you assume the provider tier is
   the reference; nothing in the spec makes it so. The alternative — make the
   provider tier refuse too — is equally available and equally consistent with the
   frozen text.

For the PM: brief decision 1 already puts the question well. It needs an
**affirmative** answer, and it should be carried together with the DEBT-3 line
below, so the owner sees the one exception to the memory-safety guarantee he is
being asked to approve. This is one question, not two round trips.

**Ruled *not* a stop-and-ask, so the PM need not carry it:** §12.3. The design
declines to amend it and offers a stop-and-ask if review disagrees. I do not. The
new caller-tier coverage limit is a blind spot of the kind the 2026-09-01
copy-accounting-scope and 2026-09-03 isolation-scope rulings both put in
`integration-tests/pubsub-conformance/README.md`, and §12.3's own standing policy
(*"a guard may ship with a recorded blind spot"*) covers it. README is the
established home. Do not spend an owner question on this.

---

## DEBT (5) — handed to the implementer, does not loop the design

**DEBT-1 — `StaleSnapshotProbeIsDetected` is a control on nothing unless the
instrument is shared.** The design says a hand-built copy-then-release-then-call
fan-out *"**must** be flagged by the same detector"*, but there is no detector
object in this design: the three primary cases assert directly on `Subscriber`
with latches. If the probe case hand-writes its own fan-out *and* its own
assertions, it proves only that a broken fan-out fails an assertion written to
fail on it — the shape this round has already logged three times. Owed: factor the
observation apparatus (latch protocol + the "was this entered after the
unsubscribe returned" predicate) into one helper used by both the primary case and
the probe, or drop the case and say in the README that the guard's falsification
is the three primaries being red today.

**DEBT-2 — the recursive gate makes premise P2 unfalsifiable.** P2 ("one callback
at a time per subscription") carries a stop-and-ask if false, but with a
`recursive_mutex` a provider that re-enters delivery for one subscription on one
thread acquires the gate again and proceeds **silently**; the P2 stop condition can
never fire. A non-recursive gate makes that violation deadlock loudly, which is
detection rather than masking, and is what leaves the *typed* refusal to A3's
`kReentrantCall` where the owner put it. Rides with BLOCKER 1's fix.

**DEBT-3 — the brief does not name the carve-out in the memory-safety guarantee.**
The spec amendment says an unsubscribe from inside its own callback *"does not wait
for the frame it is in"* — i.e. the one shape where a caller may **not** free or
unpin its callback state on return. That is the exception a BIND author must know
and the one the brief's Forbidden list currently reads as covered. Owed: one line
in brief decision 2, added before the owner answers the stop-and-ask above.

**DEBT-4 — `Files-to-delete` names `:434-436` but the same defect assertion recurs
at `:438-440`.** `EXPECT_EQ(second_calls, 1)` appears twice in
`UnsubscribeFromInsideCallbackIsSafe`; under the fix the second publish's
expectations change too (`first_calls == 2`, `second_calls == 0`). The wholesale
rewrite covers it, so nothing is at risk — the ledger is just incomplete as
written.

**DEBT-5 — the hot path's lock-free property is reversed without naming what it
reverses.** `subscriber.cpp:31-34` records the fan-out being deliberately made
lock-free (*"an atomic load replaces a mutex round trip on every sample"*), and the
delivery-cost budget this seam works to is `provider.hpp:109-113`'s measured
**1.4 ns per call**. One uncontended `recursive_mutex` acquire per entry per sample
is a real fraction of that, and `recursive_mutex` is materially dearer than
`std::mutex` on MSVC. The design names the cost in the abstract (*"one uncontended
lock"*) but not the decision it reverses, and `CopyAccounting` cannot see it —
it counts copies, not locks. Owed: name the reversal in Risks, and either measure
it or state plainly that it is accepted unmeasured. (Choosing the non-recursive
gate under DEBT-2 shrinks this.)

---

## Answers to the questions raised with the review

- **Is the fifth suite scope growth?** No. The authorised A4 includes §9's
  *Inherits as its oracle* row (verification: *"A4 (§7 clauses + §9's oracle
  row)"*), and BIND inherits `integration-tests/pubsub-conformance`, not
  `pubsub/tests`. A caller-tier oracle that BIND inherits has to live there. The
  cheaper instrument — more cases in `test_publisher_subscriber.cpp` — would leave
  §9's row still blind, which is the third part of the class the design correctly
  identifies as the one that stops it recurring. +410/−45 is large for an
  "amendment" but it is not over budget on any declared measure, and the remedy for
  size here would be to cut the oracle, which is the wrong thing to cut.
- **Does it forbid or merely document?** It forbids, at this tier. Sharing one
  lock between retirement and invocation is a genuine rung-1 move, not a test.
  §7's amended wording is the record of the forbid, not the mechanism. The two
  places it stops forbidding and starts asserting are BLOCKERs 1 and 2.
- **Blocking cancel.** Safe for the self case at this tier, and covered by a case
  that really would hang and redden under `TIMEOUT` if the recursion were dropped.
  Not safe as claimed once a second gate or the provider is in the picture —
  BLOCKER 1 and BLOCKER 2.
- **Controls.** Three primaries are genuinely red today; `ALiveSubscriptionStill
  Receives` and `AReleasedIdIsNeverReused` are real if thin; the two deadlock
  controls redden on the mutation they name but on no other edge (BLOCKER 1);
  `StaleSnapshotProbeIsDetected` is the unfalsifiable one (DEBT-1).

## NIT (noted, not returned)

The brief is 61 lines against the 60-line cap — one line over, and the overflow is
the trailing *as-landed* stub the PM fills at close.

---
---

# Cycle 2 of 2 — revision `9b354cb` (cycle 1 was `d2acfb9`)

Rulings ledger re-read in full: **36 entries** (`grep -c '^## 2026'` → 36),
including the two of 2026-09-04 that discharge my cycle-1 stop-and-ask.

**Verdict: NEEDS-REWORK — 1 BLOCKER, which *is* the stop-and-ask, plus 4 DEBT.**
There is no third design cycle and none is needed: the single BLOCKER is settled
by the owner's answer to one question plus a one-line reconciliation that lands in
implementation.

## Cycle-1 BLOCKERs: both closed, and closed properly

**BLOCKER 1 (lock order) — closed.** The order is now stated as total
(`impl_->mu` < gate < provider) with both edges *forbidden* rather than handled:
edge A by a delivery-depth counter, edge B by a scoping mandate. The gate is now
`std::mutex` + `atomic<bool>`, and the store-then-barrier order makes the two
interleavings total — I checked the four cases (self, non-delivery thread racing
an in-flight invocation, later entry in the same loop, and unsubscriber acquiring a
free gate ahead of the loop) and they are correct as described. `CallerTier.Cross
CancellingDeliveriesDoNotDeadlock` is a real control: it hangs on any build that
lets a delivery thread block on a gate. The mechanism also folded A4-DEBT-2 in —
§1.2's argument for the non-recursive gate is the right one and is the argument I
made, arrived at independently rather than copied.

**BLOCKER 2 (provider re-entrancy premise) — closed.** P5 states it, names A3 and
`kReentrantCall = 10` as the owner of the answer, forbids the implementer from
touching it *including by using `kReentrantCall`*, scopes forbidden row 4 and the
clause-6 sentence to this tier's gate, and disclosed it in the README. The brief's
Forbidden line was corrected and the residue moved into Risks. Nothing left here.

**A4-DEBT-1 — closed by deletion, and that is the stronger answer.** Dropping
`StaleSnapshotProbeIsDetected` and resting falsification on the three primaries
(red today) plus the two deadlock controls is not compliance-by-deletion: a case
that could only fail an assertion written to fail on it was carrying no evidence,
and what replaces it is a claim a reader can check. The remaining set does redden
on revert or half-landing — the three primaries on the behaviour, `SelfUnsubscribe…`
on dropping the depth counter (the non-recursive gate then self-deadlocks under
`TIMEOUT`), `CrossCancelling…` on edge A, `ReentrantSubscribe…` on the `mu`↔gate
edge. **A4-DEBT-4 closed** (both assertion sites now listed, with the changed
expectations spelled out).

## BLOCKER (cycle 2) — the sentence going into frozen §7 is narrower than the mechanism, in the unsafe direction

§1.3 publishes, and §3 puts into frozen §7 clause 6:

> An `Unsubscribe` **issued from inside a delivery callback on that subscriber**
> does not wait…

§1.1 implements it with a **file-local** `thread_local` depth counter, which is one
counter for every `Subscriber` in the process. So a handler running on subscriber
**X** that cancels a subscription on subscriber **Y** also skips **Y**'s barrier —
`Y.Unsubscribe` returns while a Y delivery may be inside its callback on another
thread. The published sentence tells that caller the wait happened (the callback
was not "on that subscriber"), so it frees or unpins the handler state: a
use-after-free, of exactly the class this item exists to remove, licensed by the
seam's own frozen text. §1.1's supporting claim — *"it is invisible across
`Subscriber` instances"* — is true of the storage and false of the effect.

This is not a separate question from P6; it is the same question asked at the
scope the mechanism actually has. Whichever way the owner rules, the text and the
mechanism must be made to agree before either lands.

**Acceptable fix (one line, and the owner's answer picks which):** either scope the
depth counter to the `Subscriber::Impl` it belongs to — the published sentence then
becomes true, the within-instance ABBA stays forbidden, and the residual
*cross-instance* mutual cancel is named as handled residue (and the brief's
"forbidden" line for two handlers cancelling each other narrowed to match) — or
publish the carve-out at its real width, *"issued from inside any delivery callback
on this thread"*, and say so in the stop-and-ask, because that widens the owner's
carve-out by two steps rather than one.

## STOP-AND-ASK — P6, ruled in three parts as asked

**1. Is the widening necessary to close edge A?** For the *within-subscriber* cycle,
**yes** — I checked the narrow form and it does not close it. Skipping the barrier
only when the target gate is the one this thread already holds leaves thread A
(holding gate E1, cancelling E2) and thread B (holding gate E2, cancelling E1)
blocking on each other. So the widening from "its own subscription" to "any
subscription on this subscriber" is load-bearing, not a convenience, and the design
is right that narrowing it back locally reinstates the deadlock. **The second
step** — from "on this subscriber" to "any subscriber in the process" — is *not*
necessary for that cycle; it is a consequence of where the counter was put. It does
buy closure of the rarer cross-instance mutual cancel. That is the choice in the
BLOCKER above, and it is the owner's to make because it is his carve-out being
widened.

**2. Is it a deviation you must carry?** **Yes, and it is worse than one step.** The
2026-09-04 ruling does not merely permit the self case, it asserts uniqueness —
*"that is **the one shape** where a caller may not free callback state on return"* —
and the owner was shown that carve-out precisely so he could weigh it before
agreeing (*"Carve-out you should see before agreeing"*). A second shape contradicts
the ruling's own words, so it is a ruling deviation, not an inference. It is the
fifth consecutive narrow-claim-stated-honestly ruling; the 2026-09-03 licence to
*infer* that preference licenses narrowing without asking, never widening. And the
true width is two steps, not the one the design's Risks bullet and the brief
describe. Carry it as a **decision**, not a Risks line — with the per-thread scope
on the table, since the design as written implements a width neither the brief nor
the frozen sentence states.

**3. What does an application lose — one sentence, product vocabulary?**

> A handler that cancels **any** subscription — not only its own, and as currently
> built not even only on its own subscriber object — gets a cancel that returns
> straight away instead of waiting, so in every one of those cases the application
> must keep that handler's state alive rather than freeing it when the cancel
> returns.

For the framed question, the two options and their costs are: **(a) keep it to one
subscriber object** — the published sentence becomes true as written, cost is that
two handlers on *different* subscriber objects cancelling each other can still hang
one another (disclosed, not forbidden); **(b) any handler, anywhere in the process**
— nothing can hang, cost is that the "you may free your handler state" promise is
off in a second, wider set of cases and the contract must say so.

## Also pressure-tested, and clean

- **Does closing edge A reopen the forcing test's guarantee?** No, on the half that
  matters. A handler can never be *entered* after its cancellation returned: the
  `retired` store precedes the return and every entry is re-checked under its own
  gate before invocation, so no new invocation can begin. What the skip gives up is
  only "not currently running on another thread" — which is the carve-out, and must
  be published rather than implied. The two halves of clause 6 come apart cleanly
  here and the design's wording keeps them apart.
- **Thread pools.** Fine. The counter is incremented on whichever thread runs the
  invocation, and the same thread takes and releases that gate under a `lock_guard`,
  so no hand-off between the two is representable.
- **Per-thread is not per-logical-flow** — see A4-DEBT-7.
- **Budget.** Design 299/300, brief 60/60, public surface 0. I compared against
  rev 1 for compression: what left the document is the discharged P3, the deleted
  probe case, and the contention bullet folded into handled residue. Nothing
  load-bearing was squeezed to fit; the reduction from +410 to +360 is the dropped
  test case, which is a scope cut and not a compression.
- **Tree claims re-checked:** the two of 2026-09-04 rulings are quoted accurately;
  `subscriber.cpp:205-206`, `in_process_provider.cpp:248,270-275`,
  `fast_dds_pubsub_provider.cpp:570-574` and `provider.hpp:109-113,130-132` are all
  as cited. One claim is false — see A4-DEBT-6.

## DEBT (4, cycle 2)

**A4-DEBT-6 — must land: edge B's named live check cannot fire.** The design offers
`ctest -R 'ProviderConformance\.'` against Fast DDS as edge B's live check. That
suite constructs no `Subscriber` at all — which is the design's *own* premise, and I
re-verified it: the only `Subscriber` anywhere in the harness is
`integration-tests/pubsub-conformance/src/copy_accounting.cpp:247,266`, i.e. the
`CopyAccounting` suite, whose subjects are in-process by construction and so have no
provider that waits on its own in-flight delivery either. Edge B therefore has **no
live check in the harness**, and the sentence that makes its uncontrolled status
acceptable is the one sentence that is not true. Owed: delete the claim and say edge
B rests on the scoping mandate with no live check — the same honesty §12.2 applied
when it brought labels down to the evidence — or name a harness that really drives
`Subscriber` over Fast DDS (`integration-tests/pubsub-arrow-fastdds/tests/
test_roundtrip.cpp:119,171,250,336`, via `SubscriberArrow`) and say plainly that it
is opportunistic, since nothing there guarantees a delivery is in flight at the
`Unsubscribe`.

**A4-DEBT-7 — the depth counter is per-thread, not per-logical-flow.** A handler
that hands the cancellation to a helper thread and then waits for it deadlocks: the
helper has depth 0, so it takes the barrier on the gate the handler is still
holding. One line of handled residue in §"Corner cases" and one in the README; it is
adjacent to, but not covered by, the existing "a callback that never returns" entry.

**A4-DEBT-8 — a probe is owed before the fan-out loop is written.** Answering the
question directly: **yes**, and it is cheap. The design does not merely accept an
unmeasured cost, it *defers a live design choice to a number it declines to take* —
§1.2 rejects the atomic in-flight counter while calling it "cheaper per sample", and
Risks says the alternative is there "if the number ever matters". A ~20-line
throwaway on this machine answering (i) uncontended `std::mutex` lock+unlock per
entry per sample against the 1.4 ns call at `provider.hpp:109-113`, and (ii) the
same for an `atomic` fetch_add, settles the choice before the loop is written rather
than after. The design's reason for declining ("a probe would be measuring the
standard library") is exactly why it is cheap, not why it is uninformative.

**A4-DEBT-9 — §1.1's *"invisible across `Subscriber` instances"* is false as
written.** True of the storage, false of the effect. Correct it whichever way the
owner rules on P6 — under fix (a) it becomes true; under fix (b) it must be replaced
by a statement of the cross-instance effect.
