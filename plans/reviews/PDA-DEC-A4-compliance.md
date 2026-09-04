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
