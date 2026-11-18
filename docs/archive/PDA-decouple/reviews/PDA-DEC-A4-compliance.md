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


---
---

# Re-check 2 after fix cycle 2 — revision `a86de50` (cycle 1 `aa72813`, cycle 2 `6d74d53`)

*The two sections above stand as the cycle-1 and re-check-1 records and are not
rewritten. This section reviews `git diff d9a6e8f a86de50` (+361/−58) only.*

Ledger: **38 entries** (`grep -c '^## 2026'` → 38), unchanged. Both fixes narrow,
so none was needed — agreed.

**Verdict: FAIL — 1 blocking, 0 minor.** R1, R2 and R3 are all genuinely closed;
I re-probed R1's exact shape and it is fixed. The blocking item is a **fourth**
path in the same family, found by probe, in-contract, 5/5 reproducible.

**"The frozen promise now has exactly one exception": NO.** The probe that decides
it is **probe 6** — *a handler cancels a **sibling** subscription whose callback is
running on another thread; that cancel defers its release to the end of the
cancelling handler's frame, which ends first; a fresh thread that has never been
inside a delivery then cancels that sibling and returns while its callback is
still running.* 5/5 on `a86de50`.

**Is the item ready to close? No** — one ~3-line fix short of it, and no owner
question is needed because the fix narrows.

---

## R1 — closed for the shape it named, and I re-probed it rather than read it

Probe 3, which failed 3/3 at `6d74d53`, now passes 3/3: *"after a self-cancel,
another thread's cancel of the same id waited: **YES**."* Probes 1 and 2 still pass
3/3. The mechanism is right where it is right: `Retirements` is shared-owned so a
deferred release outlives the `Subscriber`; the sweep at **depth 0** rather than
per frame is the correct choice and I checked the nested case that motivates it
(probe 5c: an inner frame's deferral is not released while an outer frame is still
on the stack, and the retired entry is not re-invoked); publish and lookup happen
under `mu` in the same critical section that moves the live map, so "live",
"retiring" and "gone" stay disjoint; `Retirements`' own lock is always innermost
and never held across anything that blocks.

## BLOCKING — the deferral is scoped to the cancelling FRAME, but the promise needs it scoped to the GATE

The carve-out is deliberately wide: a handler may cancel **any** subscription on
its own subscriber, not only its own. The deferred release is narrow: it fires when
**this thread's** outermost delivery frame returns.

For the *self* shape those two coincide — the fan-out holds the gate across the
handler, so the frame necessarily outlives the callback, which is why R1's fix
works. For the *sibling* shape they come apart: entry E2's callback is running on
another thread (§6 clause 2 expressly permits it, and Fast DDS's listener-per-reader
makes it ordinary), handler E1 cancels E2 from inside its own frame, E1's frame
ends, the sweep erases E2 from `retirements` — and E2's callback is still running.
From that instant E2 is in neither map, so a cancel from any other thread takes the
no-op branch and returns mid-callback.

Probe 6, 5/5 on `a86de50`: *"third-party cancel after a sibling self-cancel waited:
**NO** (returned while E2's handler was still running)."* The third party is a
freshly spawned thread that has never been inside a delivery, so there is no reading
on which it is covered by the carve-out; §7 clause 6 and `subscriber.hpp` both tell
it that nothing is in progress and that it may free. Nothing in the shape is out of
contract: no destruction race, no re-entrant provider call (the probe keeps a spare
entry so the topic never empties), and cancelling a sibling from inside a handler is
behaviour this design promotes.

I state the reachability honestly: this is **narrower** than the two shapes already
fixed — it needs the carve-out exercised on a sibling, that sibling delivering
concurrently, and a later cancel of it. But it is the same class, the frozen text
still asserts the count is one, and ruling 38's own reasoning covers it verbatim —
*"an exception that only bites under a race is the silent use-after-free you already
ruled against."*

**Acceptable fix (~3 lines, narrowing, no new authorisation):** carry the `Gate`
alongside the id in `DeferredRelease`, and at sweep time — which is **depth 0**, so
this thread holds no gate and no cycle is representable — take and release the
barrier before erasing: `{ std::lock_guard<std::mutex> b(gate->mu); }
retirements->Release(id);`. Self shapes pay nothing (the gate is already free by
then); sibling shapes wait exactly as long as that handler takes, which is the cost
clause 6 already charges. *Alternative, if the hot path is preferred over the
delivery thread's return:* have the fan-out loop release the retirement for an entry
whose `retired` flag is set once its callback has returned — the precise moment the
promise becomes true — but that moves ownership of the release, which the current
"only the owner may defer or release" invariant would have to be restated for.
**One control is owed with it**, mirroring probe 6, with the mutation *"sweep
without taking the barrier"*.

## R2 — closed, and the whole control table audited

The Observed cell is brought down and now says outright that the case does **not**
redden if the two branches are collapsed, naming
`ADuplicateCancelWaitsForTheDrainInProgress` as the sole pin. I re-read every row of
both tables: the four rows carrying "—" (`ACancelOfAFullyRetiredId…`,
`ReentrantSubscribe…`, `ALiveSubscriptionStillReceives`, `AReleasedIdIsNeverReused`)
now describe only what they watch, and the six rows carrying a mutation each name
one that is mechanically plausible for that case and no other. **No case is credited
with catching something it cannot.**

## R3 — closed, and §5.3 is genuinely untouched

The new paragraph states all three parts §5.3 asks for — contained at the point of
invocation, the remaining subscribers still receive that sample, and no report
anywhere — and says why containment is not tolerance. Adequate to the requirement
that it be *stated*.

§5.3 unmodified, verified structurally rather than by eye: the cumulative spec diff
`963bde5 → a86de50` contains exactly **two hunks**, at §7 clause 6 and §9's oracle
row. Nothing else frozen was touched across three cycles.

## Limits 7 and 8, judged as asked

- **Neither is an exception to the "you may free on return" promise.** Limit 7's
  failure mode is a topic released at the provider while a live subscriber remains —
  silent non-delivery, not a callback outliving its cancellation. Limit 8's residual
  is a **hang** (a driver delivering synchronously from inside `Subscribe` into a
  handler that subscribes to the same topic), which is the trade the owner's
  2026-09-04 preference does sanction. Neither creates a shape in which a handler
  runs past a cancel that returned.
- **Splitting 7 into 7 and 8 makes the published set more accurate, not less.** The
  old limit 7 described the subscribe side as "a few-instruction window" when it in
  fact lasted the whole provider call — 400/400 duplicate registrations at 50 µs is
  the outcome under contention, not a race. Publishing that correction is the
  honest move, and closing it is squarely inside the 2026-08-31 divergence ruling
  (Fast DDS refusing the loser `kInvalidArgument` on a valid `Subscribe` while the
  loopback reports `kSubscriptionEnded` to a live subscriber is exactly a
  cross-provider divergence, and that ruling forbids pinning one instead of fixing
  it). **Limit 8 conflicts with no ruling.**
- Limit 7's refusal to serialise the teardown side is now argued against *today's*
  providers rather than a hypothetical one, which is stronger than the version I
  passed last cycle, and both legs of the cycle it names are exercised by cases in
  this suite.

## Also pressure-tested this cycle, and clean

- **`Subscribe`'s new `provider_cv.wait` against frozen §7 clause 5 ("`Subscribe`
  never blocks").** Checked and **not** a violation: clause 5's sentence is bound to
  the late-joiner schema ("*Late joiners get the schema asynchronously; `Subscribe`
  never blocks*"), which is what `SchemaArrival` exists for, and `Subscribe` already
  blocks on `mu` and on `provider->Subscribe` itself. The new wait is bounded by a
  provider call this same path would make anyway, and the `InProgressGuard` clears
  the flag and notifies on the throwing path, so a failing `provider->Subscribe`
  cannot wedge the topic.
- **A handler that throws mid-frame** (probe 5a): contained, the fan-out continues,
  the sibling still receives both samples, an outstanding deferral is still swept,
  and a later cancel does not hang.
- **Nested delivery frames on one thread** (probe 5c): the inner frame's deferral
  survives until depth 0 and the retired entry is not re-invoked.
- **A `Subscriber` destroyed with a deferral outstanding** (probe 5b): no crash —
  `Retirements` is kept alive by the deferral's `shared_ptr`, which is exactly why
  it is shared-owned. The destructor *does* return while such a handler runs, but
  that is **out of contract** and therefore not a finding: frozen §6 clause 5 says
  *"Destruction requires quiescence: no call in flight, no callback able to
  re-enter."*
- **Converse check clean, final pass.** No `kInvalidArgument` for an unknown
  subscription id anywhere; no test, header or doc asserting the pre-change
  contract; the spec text amended in cycle 1 still matches the code after two
  mechanism changes underneath it — with the single exception of the sentence the
  blocking finding is about (*"which is what keeps the exception count at exactly
  one"*), which becomes true again the moment that fix lands. Suite 16/16 green as
  built.

## RECORD (PM's, one line each, not blocking)

- `plans/PDA-DEC-A4-lifetime-tier.md` still reads *"**37 entries**"* and carries the
  PM note *"10 cases shipped, not the 8 tabled"*; the ledger is at **38** and the
  suite now ships **16**.
- `subscriber.hpp`'s destructor paragraph promises *"Same wait, same one carve-out
  as Unsubscribe."* After the deferral change that is over-generous — a
  self-cancelled subscription is no longer in the live map the destructor drains —
  but §6 clause 5 puts the whole scenario outside the contract, so this is wording,
  not behaviour.


---
---

# Final check — revision `a96a2a7` (`aa72813` → `6d74d53` → `a86de50` → this)

*The three sections above stand as their own records. This reviews
`git diff 0e263c3 a96a2a7` (+125/−18) only.*

Ledger: **38 entries**, unchanged — correctly, since the fix narrows.

**Verdict: PASS.** No blocking findings, no minor findings.

**"The frozen promise now has exactly one exception": YES.** Decided by **probe 9**,
a randomised sweep over the sibling window with a **real non-vacuity control**: the
pre-fix package for `a86de50` is still on disk, so I built the identical probe
against it. Pre-fix: **266 cancels returned while the callback ran, of 282 that
landed inside the victim window** (600 iterations). Post-fix: **0 of 249** (600
iterations) and **0 of 685** (2,500 iterations). Probe 6 — the shape that was the
blocking finding — flipped from **NO 5/5** to **YES 6/6**.

**Ready to close: yes.**

---

## How I decided it, including the instrument I had to throw away

The fix is the one both reviewers specified, taken verbatim: `DeferredRelease`
carries the `Gate`, and the depth-0 sweep drains before releasing. I checked the
safety argument rather than accepting it — at depth 0 the sweeping thread holds no
gate and no `mu`; a thread inside a delivery never blocks on a gate; every gate the
fan-out took is released at the end of its own loop iteration, *before*
`~DeliveryScope` runs, so a sweep can never wait on a gate its own frame still
holds. Probe 7B drives the ABBA shape with both sweeps draining and completes.

**One thing I want on the record about method.** My first breadth instrument
(probe 8, a uniform subscribe/deliver/cancel fuzz — 52k invocations, 12.7k sibling
cancels from inside a delivery) reported **0 violations against the PRE-FIX
library**. It was vacuous: its cancellers popped each victim, so the third-party
cancel almost never arrived *after* an inside sibling-cancel had already retired
that id. I discarded it as evidence rather than reporting its zero. That is the
same failure the implementer caught in its own stub — a global delivery lock making
the sibling shape unreachable — and it is why probe 9 is built around the window
instead of hoping to hit it, and why it ships with a control.

Evidence, all re-run on `a96a2a7`:

| Probe | What it drives | Pre-fix `a86de50` | Subject `a96a2a7` |
|---|---|---|---|
| 6 | the blocking shape: sibling cancelled from inside a frame that ends first, then a fresh thread cancels | **NO** 5/5 | **YES** 6/6 |
| 9 | the same window, randomised hold and arrival | **266 / 282** landings violated | **0 / 249**, and **0 / 685** at 2,500 iterations |
| 7A | a three-deep chain: E1 cancels busy E2, E2 cancels busy E3, a fourth thread cancels E3 | — | clean |
| 7B | cross-cancelling deliveries, both sweeps draining | — | completes, no deadlock |
| 1, 2, 3 | cross-`Subscriber` wait; duplicate concurrent cancel; cancel after a self-cancel | — | **YES** each |
| 5a, 5c | a handler that throws mid-frame; nested delivery frames | — | contained, fan-out continues, deferral still swept; retired entry not re-invoked |

**And the structural check, which is what makes it a claim rather than a sample.** I
enumerated every path on which `Unsubscribe` can return, and every one either takes
the barrier or is the carve-out: id live → publish, then drain (barrier, or defer
**and stay published until the gate is free**); id in `retirements` → drain (barrier,
or carve-out); id in neither → returns at once, and an id is in neither map only
after a barrier was taken — in `Unsubscribe`, in the sweep, in `~Subscriber`, and on
`Subscribe`'s rollback path alike. The early return therefore has exactly one
antecedent: `InsideDeliveryOn(identity.get())`. That is the published carve-out and
nothing else. Probe 9 measures what the enumeration argues.

## The new residue — a delivery that blocks at its end

**My call: consistent with the rulings, and it does NOT need an owner decision.**
Carry it as information in the close note if you like, but not as a question.

Three reasons, in order of weight. First, it is not a new trade but the existing one
reached one step along: frozen clause 6 already charges an unbounded wait on the
slowest handler, and README limit 1 already publishes that a callback which never
returns blocks a cancel forever — the wait has moved onto the delivery thread, its
bound has not changed. Second, the alternative to this wait is precisely the silent
use-after-free the owner has now ruled against three times; his standing reasoning —
*a loud hang is preferred over a silent use-after-free* — covers a stalled delivery
thread exactly, and a stall here is loud, detectable and corrupts nothing. Third, the
2026-09-03 licence permits **narrowing** without asking, and this narrows the
exception set from two to one; the residue is the price of the narrowing, not a
widening of anything the owner was shown.

I also checked it cannot manufacture a *new* deadlock class. The sweeper holds
nothing, so it blocks no one but the provider thread it is on; the gate it waits for
is held by a fan-out whose handler can never block on a gate; and the only way the
wait becomes unbounded is a handler that never returns, which is limit 1. Placing it
inside limit 1 rather than as a new limit is the honest placement.

*Non-blocking suggestion, not a finding:* the sentence lives in the harness README,
which is where two prior rulings put such limits and is therefore right — but a
delivery thread blocking is a property of the **delivery path**, and a C++ or binding
author reads `subscriber.hpp`. One line beside the `SubscribeCallback` paragraph
would put it where that reader looks. The close does not depend on it.

## `~Subscriber`'s paragraph — confirmed accurate

It no longer claims the destructor is a synchronisation point. It names §6 clause 5
as the precondition, scopes its guarantee explicitly to *inside* the contract, and
keeps the carve-out. I checked the one way it could still be over-generous — a
subscription self-cancelled from inside a handler is no longer in the live map the
destructor drains, so its callback can outlive the destructor — and the new wording
covers it, because that scenario requires a callback in flight at destruction, which
§6 clause 5 puts outside the contract however the destructor behaves. Accurate as
written; my record item from last round is discharged.

## Final conformance sweep — all clean

- **The three 2026-09-04 rulings hold in effect, not merely in text.** Idempotence:
  a no-op at the caller tier (`SubscriberTest.UnsubscribeUnknownIdIsANoOp`,
  `CallerTier.UnsubscribeOfAnUnknownIdIsANoOp`) and at the provider tier
  (`provider.hpp` unchanged). The wait: probes 1–3, 6, 9. The carve-out scoped to
  one subscriber object, with the cross-instance hang published: probe 1 plus
  README limit 3.
- **Ruling 38's discriminator intact.** `retirements->Find` splits "being cancelled
  right now" (waits) from "unknown or fully cancelled" (silent no-op) at
  `subscriber.cpp:508-510`; probe 2 confirms the first, the shipped case the second.
- **Spec: still exactly the two authorised hunks** across all four cycles — §7
  clause 6 and §9's oracle row. Nothing else frozen moved; §5.3, §6 and §12 are
  untouched. `PubSubStatus` untouched; `kReentrantCall` appears nowhere in `pubsub/`.
- **Converse, final.** No `kInvalidArgument` for an unknown subscription id
  anywhere. Nothing asserts or documents the pre-change contract: the only two
  textual hits are a comment naming the retired test as retired, and the README's
  *"what it caught"* quotation of the old error — both records of removal, not
  survivals.
- **The published limit set describes the code and adds no exception to the
  promise.** Limit 1 now carries the delivery-blocking residue; limit 7's failure
  mode is silent non-delivery; limit 8's residual is a loud hang. None of the three
  is a case in which a callback outlives a cancel that returned.
- Suite **17/17** green as built; the new control mirrors probe 6 and its third
  thread is genuinely never inside a delivery.

## RECORD (PM's, one line each, not blocking)

- `plans/PDA-DEC-A4-lifetime-tier.md` still reads *"**37 entries**"* and carries the
  PM note *"10 cases shipped, not the 8 tabled"*; the ledger is **38** and the suite
  now ships **17**.
