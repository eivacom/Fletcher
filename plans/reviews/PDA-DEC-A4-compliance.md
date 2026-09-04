# PDA-DEC-A4 — step-4a compliance review

Diff base `963bde5` → `aa72813` (8 files, +940/−29; 25 of the adds are the PM's
ledger entry). Rulings ledger read in full first: `plans/PDA-DEC-rulings.md`,
**37 entries** (`grep -c '^## 2026'` → 37).

**Verdict: PASS-WITH-FINDINGS(3)** — 2 must-land before close, 1 minor. Neither
must-land item touches the mechanism: the owner's three rulings are honoured *in
effect*, which I verified independently rather than by reading. What is missing is
a control on the mechanism the owner ruled on, and one unpublished shape in which
the frozen sentence is false.

Findings only. Everything not named here I checked and had nothing to say about.

---

## Verified independently (not taken from the implementer)

Two throwaway probes built against the packaged `fletcher-pubsub` (the same
artefact the suite links) rather than against the test file, so the harness cannot
be the thing that makes the answer come out right.

- **Per-`Subscriber::Impl` scope is true of the EFFECT, not only the storage.** A
  handler on subscriber X cancelling a subscription on subscriber Y **does** take
  Y's barrier and wait for Y's in-flight handler to return (probe: `YES`,
  deterministic, 300 ms hold window). This is what cycle 2 found false of the
  previous attempt. The token stack — not a counter — is also what makes the
  *nested* case right: a handler reached through a delivery on Y skips Y's barrier
  even when the innermost frame belongs to X, so the published sentence stays true
  for that path too.
- `CallerTier` 10/10 green as built; the edge-B control genuinely drives edge B (a
  function-scoped barrier makes main hold `gate(last)` across
  `provider->Unsubscribe` while the parked delivery walks into that same gate —
  real ABBA, and it reddens on no other mutation, since dropping the barrier
  entirely makes this case pass).
- Converse check clean: no surviving `kInvalidArgument` for an unknown
  subscription id at either tier; no surviving test or doc asserting the late
  callback as intentional; `subscriber.hpp:64-72` gone with no replacement; both
  named legacy tests retired, not edited to pass (`UnsubscribeUnknownIdThrows`
  deleted, `…IsSafe` rewritten wholesale with **both** `EXPECT_EQ(second_calls,
  1)` sites inverted); `CallerTier.StaleSnapshotProbeIsDetected` never written, as
  ordered. `core/include/fletcher/core/status.hpp` untouched; `kReentrantCall`
  appears nowhere in `pubsub/` or in the new suite — A3's allocation is not
  pre-empted. Frozen text touched: **§7 clause 6 and §9's oracle row only**, which
  is exactly the authorised set; §12.3 correctly left alone.

---

## FINDING 1 (must land) — the one mechanism the owner ruled on is the one mechanism with no control

The change ships nine mechanisms and eight controls. The README's own standard is
stated in its control table — *"each was made to go red by mutating the thing it
controls rather than asserted to be one"* — and §8.1's standing requirement, which
§9 restates verbatim in the very row this diff amends, is *"a live negative control
ships with it — a guard nobody has made go red is a guard nobody has measured."*

**Nothing in the tree reddens if the delivery-depth predicate is widened from
`InsideDeliveryOn(identity.get())` back to `!g_delivery_stack.empty()`.** That
mutation is the process-wide scope the owner rejected on 2026-09-04, it reinstates
the exact use-after-free cycle 2 raised as a BLOCKER, and every one of the ten
`CallerTier` cases stays green under it — because no case in the suite constructs
two `Subscriber` objects. The ruling's whole content is a *relationship between two
subscribers*, and the suite cannot express one.

This is not hypothetical and not expensive: my probe is ~40 lines, needs no
transport, passes on the shipped build, and flips to `NO` under that mutation by
construction (a thread inside any delivery would skip Y's barrier and return before
the 300 ms hold expires).

**Acceptable fix:** add `CallerTier.CancellingOnAnotherSubscriberWaitsForItsDelivery`
— two `Subscriber`s on one probe, X's handler cancels Y's subscription while Y's
delivery is parked, assert Y's handler had exited when the cancel returned — and
one row in the README's control table naming the mutation ("widen the depth
predicate process-wide").

## FINDING 2 (must land) — the frozen sentence is false in a second, unpublished shape: a concurrent duplicate cancel

Amended §7 clause 6, and `subscriber.hpp` after it, publish: *"no invocation of that
subscription's callback begins after `Unsubscribe` returns, and none is in progress
when it does … on return the caller may free or unpin whatever the callback was
using"*, with **one** published exception (issued from inside a delivery callback on
that subscriber).

Two threads cancelling the **same id** is a second exception, and it is not
published. The loser of the `subscription_topic` race takes the new no-op branch at
`subscriber.cpp:302-310` and **returns immediately while that subscription's
callback is still running**. Probe evidence, 3/3 runs: *"duplicate concurrent cancel
waited: NO (returned while the handler was still running)"*.

This is the cycle-2 BLOCKER's defect class in a second place, and the idempotence
ruling is what creates the shape: the owner authorised the no-op precisely so a
foreign-runtime finaliser could cancel **unconditionally** — on a GC'd runtime that
finaliser runs on a different thread from the application's own teardown, which is
this race exactly. The owner's reasoning is normative here: *"a too-wide 'you may
free' promise is a silent use-after-free — the exact defect class this item was
opened to remove."*

I record plainly that the approved design has the same hole — §2 says only *"unknown
id returns silently"* and does not separate "never live" from "being cancelled right
now" — so this is an inherited gap, not an implementer invention. It still cannot
ship inside frozen text that promises the opposite.

**Acceptable fix (cheapest, and the one the owner's five prior narrow-claim rulings
point at):** one sentence in `subscriber.hpp` and one numbered limit in the harness
README — *a cancel of an id another thread is already cancelling returns without
waiting; only the thread that wins the race gets the barrier* — plus a `CallerTier`
case pinning it. Making it true instead (keep the gate reachable by id until the
barrier completes, and have the duplicate wait on it) is the stronger answer but is
a mechanism change and needs the owner.

## FINDING 3 (minor, non-blocking) — an undeclared build-system change, carrying a false rationale

`integration-tests/pubsub-conformance/CMakeLists.txt` swaps `enable_testing()` for
`include(CTest)`. It is not in the design's `Files-to-touch` for that file (which
orders the target and `gtest_discover_tests … TIMEOUT 60`, nothing else), and the
justification written into the file and into the commit message —
*"so every entry below carries a declared TIMEOUT"* / *"run under TIMEOUT 60 via
include(CTest)"* — is contradicted by the same file's own history:
`gtest_discover_tests(conformance_registry … PROPERTIES TIMEOUT 60)` predates this
diff and works under bare `enable_testing()`. What `include(CTest)` does add is a
`BUILD_TESTING` option (referenced nowhere else in the repo) that, set OFF, silently
registers no tests for the round's central deliverable.

**Acceptable fix:** revert those four lines.

---

## RECORD (PM's to correct in place; not blocking, no fix cycle)

- `plans/PDA-DEC-A4-lifetime-tier.md` §1.1 still specifies a **file-local**
  `thread_local` depth counter and still says *"it is invisible across `Subscriber`
  instances"* (A4-DEBT-9). The owner overruled that on 2026-09-04 and the tree
  implements per-`Impl` scope. Leaving it standing is **not** acceptable as a
  record: the design doc is what PDA-ABI and BIND will read, and it currently
  describes a mechanism the owner rejected. One-line fix, PM's.
- Same document, header: *"**36 entries**"*; the ledger has 37. Forcing-test table
  lists 8 cases; 10 shipped.
- `plans/PDA-DEC-A4-brief.md` (rev 2) lists under **Forbidden**: *"two handlers
  cancelling each other's subscriptions hanging one another."* Ruling 3 permits
  exactly that across `Subscriber` objects, and the README now publishes it as
  residue. The brief's qualifier *"at this layer"* does not carry it. One line.
- `docs/archive/HARD/HARD-progress-log.md:58` still records the old "one final
  in-flight message … by design" behaviour. Correctly left: it is a past-tense
  record of what that round landed, not a live contract. Named here so it is not
  mistaken for a survivor later.

---

## The two declared deviations — explicit calls

**1. `+915 / −29` actual against `+≈360 / −≈45` declared (2.5× the adds) —
ORDERED WORK, not scope growth**, with the single exception of Finding 3. Per file:
`caller_tier.cpp` +537 is eight design-table cases plus two debt-closing ones, all
latch-driven and multithreaded, in a file the design ordered; README +98 is
ruling-mandated publication plus the four limits and the falsification sentence the
design's `Files-to-touch` names; spec +36/−4, header +46/−12, `subscriber.cpp`
+156/−13 and tests +40/−17 are each the ordered change for that path. Nothing here
is a feature the design did not order. The honest reading is that the design's
`+≈360` under-estimated its own deliverable — the estimate was wrong, not the work.
Only the four-line `include(CTest)` swap is genuinely undeclared, and it is 0.4% of
the overrun.

**2. Two cases beyond the forcing-test table — LEGITIMATE DEBT CLOSURE, not
unreviewed scope.** `UnsubscribeDoesNotHoldAGateWhileEnteringTheProvider` closes
**A4-DEBT-6**, which the register marks *"Must land"* and which cycle 2 established
had **no live check anywhere**; I confirmed it drives edge B for real and reddens on
that mutation and no other. `DestructorDrainsAnInFlightDelivery` gives a control to a
row the design listed as **forbidden, rung 1** (*"Callback running after
`~Subscriber` returns"*) with nothing to redden it — the same defect A4-DEBT-6 was
raised about, one row down. Adding controls to uncontrolled rung-1 rows is this
round's standard, not a breach of it.


---
---

# Re-check after fix cycle 1 — revision `6d74d53` (cycle 1 was `aa72813`)

*The `PASS-WITH-FINDINGS(3)` above stands as the cycle-1 record and is not
rewritten. This section reviews `git diff 10ad452 6d74d53` (+386/−71) only.*

Rulings ledger re-read in full: **38 entries** (`grep -c '^## 2026'` → 38),
including the new 2026-09-04 entry that my cycle-1 finding 2 produced.

**Verdict: FAIL — 1 blocking, 2 minor.** The blocking item is narrow and the fix
is about ten lines; everything else in this cycle came back clean, and two of the
three cycle-1 findings are closed by probe rather than by reading. But the item
cannot close while a **frozen** sentence says something the tree does not do.

**"The frozen promise now has exactly one exception": NO.** There is a second, and
it is R1 below. Reported immediately, as asked.

---

## Closed, and verified by probe rather than by reading

Three throwaway probes rebuilt against the **new** package
(`fletcb13015e00d85d`, 2026-09-04 08:23), not against the test file.

- **Cycle-1 finding 1 — closed.** `CancellingOnAnotherSubscriberWaitsForItsDelivery`
  is a real control on the newest ruling's mechanism, and my independent probe
  agrees 3/3: a handler on X cancelling on Y waits out Y's in-flight handler. The
  "reddens on exactly one case" claim holds structurally as well as by assertion —
  every other case constructs one `Subscriber`, and with one `Subscriber`
  `InsideDeliveryOn(identity.get())` and `!g_delivery_stack.empty()` are the same
  predicate, so no other case *can* distinguish the mutation. Claim confirmed.
- **Cycle-1 finding 2 — closed for the shape it named.** My probe 2, which failed
  3/3 in cycle 1, now passes 3/3: the duplicate concurrent cancel blocks on the
  winner's drain. The `retiring` map is the right discriminator, published and
  cleared under `mu` in the same critical sections that move `subscription_topic`,
  so "live", "being cancelled now" and "gone" are three states rather than two
  with no window between the first two. I also checked the awkward interleaving in
  the other order — an outside thread winning and blocking on the gate while the
  handler *then* self-cancels — and it is correct: the self-cancel finds
  `retiring`, hits the carve-out skip, and returns instead of deadlocking.
- **Cycle-1 finding 3 — closed.** `enable_testing()` restored, `TIMEOUT 60` kept.
- **Converse check clean.** No `kInvalidArgument` path for an unknown subscription
  id at either tier; nothing in the tree asserts or documents the pre-change
  contract; the earlier spec amendment still matches the code after the mechanism
  changed underneath it (§7 clause 6's "fully cancelled" wording was updated in
  step with the three-state split, and the §9 row is untouched). `PubSubStatus` is
  untouched and `kReentrantCall` still appears nowhere in `pubsub/`. Suite 14/14
  green as built.

## R1 (BLOCKING) — the carve-out un-publishes the drain, so the promise still has two exceptions

The self-cancel carve-out does not only return early for the caller inside the
handler. It also takes the id **out of the `retiring` map while that handler is
still on the stack**: `RetireAndDrain` skips the barrier and returns, and the
winner's second critical section then erases `retiring[id]` at once. From that
instant the id is in neither map, so a cancel of the same id from **any other
thread** takes the no-op branch and returns while the callback is still running.

Probe evidence, 3/3 reproducible on `6d74d53`: *"after a self-cancel, another
thread's cancel of the same id waited: **NO** (returned while that handler was
still running)."* The probe keeps a sibling subscription so the provider is never
re-entered (P5) — nothing in the shape is exotic.

That second caller is **not** in the published exception: it did not issue from
inside a delivery callback, so §7 clause 6 and `subscriber.hpp` both tell it that
"none is in progress when it returns" and that it may free handler state. It may
not. This is the same defect class as cycle-1 finding 2 — a race-only exception to
a promise the frozen text says has one — and the reachability argument the owner
accepted there applies unchanged: a handler that cancels itself once it has what
it wanted is ordinary, and the unconditional teardown cancel is precisely what the
idempotence ruling exists to serve. Worth stating in its general form: **whenever
the carve-out is exercised — on any subscription of that subscriber, not only the
handler's own — the drain stops being visible to everyone else.**

Ruling 38 is explicit about what it is buying: *"the 'you may free on return'
promise has **exactly one** exception, the self-cancel carve-out."* It has two.

**Acceptable fix (~10 lines, and it needs no new authorisation — it narrows, which
the 2026-09-03 licence permits without asking):** when `RetireAndDrain` skips the
barrier, leave the id **published in `retiring`** until the invocation it is inside
actually returns, and have the delivery frame clear the ids retired during it (a
thread-local list swept by `DeliveryScope`'s destructor costs the hot path nothing
when it is empty). A cancel from a non-delivery thread then finds the gate, blocks
on it, and waits the handler out like any other. *Alternative, if the implementer
prefers not to touch the mechanism again:* carry one line to the owner asking
whether a cancel arriving in the carve-out's shadow is the same exception or a
second one — but do not close the item on text that asserts the answer.

## R2 (minor) — `ACancelOfAFullyRetiredIdReturnsWithoutWaiting` does not redden on the collapse it is credited with

The README credits it with showing *"that the two no-op branches were not
collapsed."* It cannot. Collapse them the natural way — drop
`impl_->retiring.erase(subscription_id)` so a finished id stays "retiring" — and
the case still passes: the gate it would then find has a free barrier, so the call
returns at once and the unrelated delivery it watches is undisturbed either way.
The two branches are distinguishable only while a drain is actually in progress,
which is its sibling's job. Its Mutation cell is honestly "—"; the Observed cell
overclaims. *The implementation does discriminate* — that is probe-verified — so
this is a claim defect, not a mechanism one.

**Acceptable fix:** bring the Observed cell down to what the case shows — a fully
cancelled or never-issued id neither throws nor waits — and leave
`ADuplicateCancelWaitsForTheDrainInProgress` as the sole pin on the distinction.

## R3 (minor) — a newly swallowed exception, in the one place §5.3 requires the behaviour be stated

The fan-out now wraps `entry.callback(...)` in `catch (...) {}`. It came from the
step-4b reviewer's S3 and it is an improvement on unwinding into a transport
thread. My angle is only this: locked decision 10 and frozen §5.3 make a throwing
callback **forbidden**, and §5.3's own words require the seam to *"say what a
provider does if one does anyway"* — while §7 clause 6, as amended by this item,
now binds the clauses *"at every tier this seam publishes."* So the caller tier has
just acquired a silent tolerance for a forbidden state, stated in a source comment
and nowhere a reader of the seam would look. Nothing to redesign; the behaviour is
the right one.

**Acceptable fix:** one sentence in `subscriber.hpp` — a callback that throws is
contained, the remaining subscribers still receive that sample, and the exception
is not reported anywhere. Do not touch frozen §5.3 to say it.

## Judged as asked

- **B1's residual window (README limit 7) — consistent with the rulings, and NOT a
  third exception to the promise.** I checked what it actually claims: it is about
  the *provider-level* subscribe/unsubscribe transition, whose failure mode is a
  subscriber that silently receives nothing, not a callback running after a cancel
  returned. It neither touches "you may free on return" nor creates a shape in
  which a handler outlives its cancellation, so the owner's exactly-one-exception
  count is unaffected by it. Declining the serialising lock is also right under the
  rulings rather than merely cheaper: gate→lock via a callback re-entering
  `Subscribe` against lock→gate via a teardown holding it across a synchronously
  delivering provider is a genuine new cycle, and the 2026-09-04 preference for a
  loud hang over a silent use-after-free does not license *manufacturing* a hang to
  close a race that cannot corrupt memory.
- **S2 declined — I agree.** The 2026-09-04 carve-out ruling says, in the owner's
  own selected words, that a handler cancelling *any* subscription on its own
  subscriber gets an immediate return. Narrowing the skip to the frames this thread
  holds would make the published sentence false in the safe direction, but it would
  still make it false, and a reviewer's licence to narrow does not extend to
  contradicting the sentence the owner was shown. No disagreement.

## RECORD (PM's, one line each, not blocking)

- All four cycle-1 RECORD items are fixed in place; I re-checked each.
- `plans/PDA-DEC-A4-lifetime-tier.md` now reads *"**37 entries**"* and carries a PM
  note *"10 cases shipped, not the 8 tabled"*; the ledger is at **38** and the
  suite ships **14**.
