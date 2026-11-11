# D7F-3 design review — cycle 1 of 2

**Verdict: NEEDS-REWORK — 2 BLOCKERs (B2 is STOP-AND-ASK: an owner question to reframe), 3 DEBT.**
Design `plans/D7F-3-single-delivery-thread.md`, brief `plans/D7F-3-brief.md`, HEAD `b02747f`. The design's
`cpp`/`test` line citations were spot-checked against the tree and are exact.

**Proof of read:** `plans/RIX-rulings.md` read top to bottom: **169 lines, 14 entries**. The last heading is
"2026-11-10 — Queued windows are delivered before the closing batch".

**Checks that passed (rely on these in cycle 2; do not re-derive them):** the queue, single consumer and
`Close`-never-unlocks mechanism give L12, the D10 thread move and L1 violation 2 by construction. Rungs
3–6 are each red on `9ab5d86` for the stated reason. The `loop_id_` answer to BC-6 adds no
`thread_local`. Subscription ids are monotonic (`sub:595`), so a draining entry cannot collide with a
new one. Budget 7b: about 5 private members, 3 private functions and 2 atomic accessors price at
+40..+70 net, far below 250, and the B1 fix lowers that. No split is needed. The design is 282 lines,
adds 0 public surface and has no coexistence bridge. Decision 1's premise holds: with a window pending
or queued, the batches can only arrive nested (L12), after the destructor returns (L9's text) or
never (L1). Decision 1 is well-formed; its one cost clause was removed as a NIT.

## B1 — §4's take-out/put-back window lets a concurrent destructor or duplicate Unsubscribe miss a draining batcher

§4 takes the batcher out of `batchers_` and calls `Stop()` outside `mu_`. It puts the batcher back
only if `Stop` returned false, so between those two steps the entry is in no map. (1) Suppose
`~SubscriberArrow` runs on another thread inside that window while A's callback is calling
`Unsubscribe(A)`. The destructor never waits for A. It resets `subscriber_` and destroys `mu_`/maps
while A's thread is still inside `Unsubscribe`: the put-back, `subscriber_->Unsubscribe` and the
codec clean-up are then a silent use-after-free. A's thread then delivers the queue and `kClosing`
after the destructor has returned. (2) A duplicate outside `Unsubscribe(A)` on T2, while T1 is inside
`Stop` (the true path, never put back), finds nothing and returns mid-drain. That breaks L9 and the
owner's 2026-09-04 rule that a racing cancel "waits for the same drain" (`pubsub-interface-spec.md:1028-1033`),
which §4 says it mirrors. Both cases contradict the design's own forbidden case at :164. D10 widens
today's timer-only gap to every callback, because `kRowLimit` callbacks no longer run inside
domain 3's gate.
**Acceptable fix (forbid; cheaper than handling, and removes lines):** `Unsubscribe` copies the
`shared_ptr` under `mu_` without erasing it. It calls `Stop()` outside `mu_` and erases the entry
under `mu_` only when `Stop` returned true. The `Ended()` sweep stays and there is no put-back step.

## B2 — Brief Decision 2 / P1: option (a)'s real cost is a hang, and a tier-1 precedent is missing (STOP-AND-ASK)

Under (a), a transport thread inside an `InProcessPubSubProvider` delivery holds the instance mutex
(`ipp:48-52`, "Another THREAD … is served") while `Stop` waits for the batch thread. Suppose any
queued or in-flight batch callback of that subscription calls `Publish`, `Subscribe` or `Unsubscribe`
on the same provider. That includes the supported in-callback `Unsubscribe` of a topic's last
subscription, whose `provider->Unsubscribe` takes that mutex (`sub:742-750`). Both threads then hang
with no signal. This is P1's own stop condition, fired by the design's default, and it makes §6's
"row delivery never waits on a callback" false. The option says only "the transport thread pauses".
Also missing: the owner has already answered this shape for domain 3. An `Unsubscribe` issued from
inside a delivery on that subscriber does not wait, because waiting "is exactly what lets two
handlers hang one another" (`pubsub-interface-spec.md:1015-1021`, ruling 2026-09-04). Option (b) is
that carve-out applied to L9, and the recommendation does not weigh it.
**Acceptable fix (text only, no redesign before the ruling):** (a)'s user line names the hang and
its trigger, and its arch line names the clash with spec:1015-1021 and P1. (b)'s arch line reads
"extends domain 3's in-delivery carve-out to L9". Re-ground the recommendation on that. P1 says its
stop condition fires under (a). State as fact, not as authority, that today's tree already hangs
here for a timer-thread callback.

## DEBT (register: `design-debt.md` § D7F-3; does not loop the design)

- **DEBT-1 — the deadline residue's "why not forbidden?" is weak.** "Today a slow kTimeout callback
  already…" and "L5 has no test" are code-as-authority. L4 says a window closes at the first of four
  events. Proposed forbid: at the top of `AddRow`, if `has_deadline_ && now >= deadline_`, call
  `Close(kTimeout)` before `TryAppend`. That is one clock read per row and only queues (D10-clean).
  Otherwise d7 L4/L5 must record the residue rather than stay `[built]`, and the brief's "Handled"
  line must stop calling the fix costly.
- **DEBT-2 — `cv_` is shared by the loop and any number of `Stop` waiters** (several `Unsubscribe`
  calls plus the destructor). Every notify on it (`Close`, `ArmTimer`, the loop's `ended_`) must be
  `notify_all`. A `notify_one` reaches the wrong waiter and hangs.
- **DEBT-3 — `Files-to-touch` misses two items.** First, `arrow-bridge/benchmarks/bench_arrow_codec.cpp`:
  `received` (:511) is declared after `sub` (:507), so under D10 the destructor's drain can call the
  callback on a destroyed atomic. Reorder it and fix the stale "Flush runs" comment at :18. Second,
  the fastdds comments at `test_roundtrip.cpp:209-210` and `:221-222` (audit 0a) are not in §7's list.

## NIT (fixed in place)
- Brief Decision 1's recommendation: dropped "and it needs no extra code", a cost ground.
