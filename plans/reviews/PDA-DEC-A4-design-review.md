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
