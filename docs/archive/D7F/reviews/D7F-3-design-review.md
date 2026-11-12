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

§4 takes the batcher out of `batchers_`, calls `Stop()` outside `mu_`, and puts it back only if
`Stop` returned false, so between those steps the entry is in no map. (1) A `~SubscriberArrow` on
another thread inside that window, while A's callback runs `Unsubscribe(A)`, never waits for A. It
resets `subscriber_` and destroys `mu_`/maps while A's thread is still in `Unsubscribe`: the
put-back, `subscriber_->Unsubscribe` and the codec clean-up are a silent use-after-free, and A then
delivers after the destructor returned. (2) A duplicate outside `Unsubscribe(A)` while another is in
`Stop` finds nothing and returns mid-drain. That breaks L9 and the 2026-09-04 rule that a racing
cancel "waits for the same drain" (`pubsub-interface-spec.md:1028-1033`), which §4 claims to mirror.
**Acceptable fix (forbid; it removes lines):** copy the `shared_ptr` without erasing it, `Stop()`
outside `mu_`, and erase only when `Stop` returned true; keep the `Ended()` sweep; no put-back step.

## B2 — Brief Decision 2 / P1: option (a)'s real cost is a hang, and a tier-1 precedent is missing (STOP-AND-ASK)

Under (a), a transport thread inside an InProcess delivery holds the instance mutex (`ipp:48-52`)
while `Stop` waits for the batch thread. Any queued or in-flight batch callback that calls
`Publish`/`Subscribe`/`Unsubscribe` on that provider then hangs both threads with no signal. That
includes the supported in-callback `Unsubscribe` of a topic's last subscription, via
`sub:742-750`. This is P1's own stop condition, fired by the default, and it makes §6's "row
delivery never waits on a callback" false. Also missing is the owner's domain 3 answer to the same
shape: an `Unsubscribe` from inside a delivery does not wait, because waiting "is exactly what lets
two handlers hang one another" (`pubsub-interface-spec.md:1015-1021`, ruling 2026-09-04).
**Acceptable fix (text only):** name the hang in (a) and the precedent in the arch lines,
re-ground the recommendation, have P1 say it fires under (a), and state today's timer-thread hang as fact.

## DEBT (cycle 1), all folded into revision 2 and closed
- ~~DEBT-1: the deadline residue~~ is forbidden by §2's first line. ~~DEBT-2: `notify_all`~~ is in §1.
  ~~DEBT-3: the benchmark and fastdds comments~~ are now in Files-to-touch.

## NIT (fixed in place)
- Brief Decision 1's recommendation: dropped "and it needs no extra code", a cost ground.

## Cycle 2

**Verdict: APPROVE-WITH-DEBT(2). No BLOCKER stands.** Revision 2 cites the same ledger read (169
lines, 14 entries, same last heading). Production: +92/−64, or +98/−64 under (b), far below 250.

**B1 is closed, at the class.** The rule is that an entry leaves `batchers_` only after its loop has
ended (§4). Every remover was checked:
- `Unsubscribe` erases only after `Stop` returned true, which means the loop has ended.
- The sweep takes only `Ended()` entries. `ended_` is false for every inserted entry, because
  `Start` sets it before the insert, and it turns true only after the last callback returns.
- The destructor snapshots without clearing and waits on each `ended_` before `subscriber_.reset()`,
  so an in-callback `Unsubscribe` racing it finishes against live members.
There is no new window where an entry is removed while it still delivers, including under (b),
whose third case returns false and so leaves the entry in place. Rung 4(d) witnesses the racing cancel.

**B2 is closed.** (a) now states the hang, P1 records that it fires under (a), and §6's sentence is
now a pointer. **No domain 3 change, verified:** `InsideDeliveryOn` is a read-only query
(`delivery_frame.hpp:63`) at BC-6's single site. Every in-tree provider pushes its own
`PubSubProvider*` token in `DeliveryChannel::Deliver` (`delivery_channel.cpp:61`; the doors check
the same address, e.g. `in_process_provider.cpp:343`). `InsideDeliveryOn(provider)` therefore
answers correctly for InProcess, Fast DDS and XRCE with nothing added. A tier asking that question
before it waits is a door, which the header permits (`:8-10`). Where the token lives is C2-2.

**Decision shape: both Decisions pass.** Each asks a principle question in behaviour terms. Each
option has a user line and an architecture line naming a law or ruling. Each recommendation is
grounded in the laws, the round purpose or §1 Intent, and each has a default. Decision 2's scope
has a premise defect (C2-1).

**The 300/300 cap was a real finding, fixed as a NIT.** The document was sized by the cap: two
over-long lines were squeezed in to stay at 300, and Decision 2 was stated three times (Risks, P1,
brief). Trimmed in place: Risks keeps the decision, the fact and the build note, and leaves the hang
mechanics to P1. §4's "both orders go" now points to Files-to-delete, and the two long lines were
reflowed. 300 → 296 lines, no substance removed.

### DEBT (cycle 2)
- **C2-1 — PM-facing; must land before the owner reads the brief.** Decision 2's "(or destroys the
  subscriber)" is already ruled. Destroying a seam object over a provider from inside one of that
  provider's deliveries is forbidden (§6 clause 5, ruling 2026-09-05) and stops the program in
  `~Subscriber` (`sub:556-578`) under either option. (b)'s "No hang … you may not free" is false for
  that half. Strike the destructor (brief :43, design Risks) and state the fact. Scope "No hang" to
  the subscription's own provider: another provider's delivery thread still waits, as in domain 3.
- **C2-2 — must land with (b) if it is ruled.** (i) `Subscriber` exposes no provider
  (`subscriber.hpp:32`), so the token is captured in `SubscriberArrow`'s constructor and the member
  lands in `subscriber_arrow.hpp`. Correct "comments only" (§4, Files-to-touch) and the ABI line.
  (ii) The third case moves and detaches `thread_` like the `loop_id_` case, else a joinable thread
  reaches `~std::thread`. (iii) Publish the new "may not free on return" case at hpp:147, README:93
  and d7 L9: spec:1015-1021's own reason is that an unpublished exception is a use-after-free.

### NIT (fixed in place)
- Brief Decision 2: "interface spec §6" → "§7 clause 6" (the cited text is spec:1001-1033).
- Design: the cap trims above.
