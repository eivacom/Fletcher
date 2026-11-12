# PDA-DEC-AG1 — compliance re-review, CYCLE 3 (independent, adversarial)

**Item:** PDA-DEC-AG1 — the misbehaving callback (A2 + A3 + A8)
**Diff base:** `ff714dc` · working tree, uncommitted
**Scope:** the **cycle-2 increment only** — B5's prose discharge, the
`EnsureProviderSubscription` reorder, the `AbsorbedCallbackFailures()` narrowing, and the
`DeliveryChannel` constructor change — plus their consequences.
**Prior reviews:** `plans/reviews/PDA-DEC-AG1-compliance.md` (§1–8 cycle 1, §A–I cycle 2).
This file does not overwrite it.

**Read receipt — `plans/PDA-DEC-rulings.md`, measured: 52 entries**
(`grep -c '^## 2026'` = 52). Last two:
*"A subscriber's handler failure is not the publisher's business"* (2026-09-05) and
*"Re-entry is refused on every protocol; the capability passes to PDA-ABI"* (2026-09-05,
**supersedes entry 48 as to direction**). Rulings 44–52 treated as live; the ledger is
binding over the design doc and over the code.

**Not re-audited, by instruction and because the increment does not touch them:** the twelve
provider doors, the loopback dispatch-under-mutex argument, and
`SubjectFactory::publishes_into_subject_instance`. Verified twice; unchanged — the only edits
to `in_process_provider.cpp`, `fast_dds_pubsub_provider.cpp` and
`xrce_dds_pubsub_provider.cpp` in this increment are the four `DeliveryChannel(this, …)`
push sites losing their now-redundant casts. The doors keep theirs.

---

## Verdict: PASS-WITH-FINDINGS

**Two blocking findings, both prose-only, both the same shape as B5 itself:** the text added
to discharge B5 states a rule the code does not implement, and the text it was added
*underneath* still states the opposite of the code.

**Charter constraint: HOLDS.** Verified independently, §3.

**Neither finding is an oracle-wins tripwire.** Reasoning at the end of §1/C1 — the offending
sentence is inside the un-merged ruling-52 amendment the implementer wrote in this same
change, and the correction *narrows*, which the 2026-09-03 licence permits without asking.
Flagged so the PM can overrule.

---

## 1. Is B5 discharged? — PARTLY

B5 asked for the caller-tier refusal in `Subscriber::Subscribe` to be published in three
places. All three received text. Judged on content, not on presence:

| Place | Present? | Correct? |
|---|---|---|
| `integration-tests/pubsub-conformance/README.md` limit 5 (`:826-841`) | yes | **yes** — "the answer is data-dependent on the same call: served for an already-subscribed topic, refused by name for a new one", plus all four `CallerTier` cases named. This is the honest one. |
| `docs/pubsub-interface-spec.md` §6 clause 6 closing paragraph (`:793-804`) | yes | **no** — see C1. The asymmetry and its reason are right; the mechanism sentence over-claims. |
| `pubsub/include/fletcher/pubsub/subscriber.hpp` on `Subscribe` (`:96-117`) | yes | **no, twice** — repeats C1's over-claim in a sharper form, and sits directly beneath a paragraph (C2) that says the opposite about reporting. |

**The asymmetry itself is stated right, and I attacked it.** Spec §6 clause 6: *"its **two**
methods answer differently because only one of them has an answer that does not need the
provider. `Subscriber::Unsubscribe` **skips** … `Subscriber::Subscribe` **refuses**."* Both
halves are true of the tree:

- The cancel skip is real and is keyed on the **provider** token, not the Subscriber identity:
  `subscriber.cpp:664` `!internal::InsideDeliveryOn(impl_->provider.get())` guards the
  `provider_subscribed = false` flip *and* the teardown, inside one critical section and
  before the flip — so the residue the skip creates (transport subscription left open) is the
  one ruling 50 ordered published, and it is published in three places.
- The refusal is real and unavoidable: `EnsureProviderSubscription` must reach
  `provider->Subscribe`, and there is no cached answer to give.
- The scope word is right: the header says *"from inside a delivery callback on **this
  Subscriber's provider**"*, not "on this Subscriber". That matches
  `RefuseIfInsideDeliveryOn(provider.get(), …)`, which fires for a handler on a *different*
  `Subscriber` over the same provider. A weaker wording would have been wrong.

### C1 — BLOCKING. The published rule over-claims atomicity and determinism the code does not deliver

Two sentences, one in frozen normative text and one in a public header:

- `docs/pubsub-interface-spec.md` §6 clause 6: *"`Subscriber::Subscribe` **refuses**, with
  this status, **when and only when** it must enter the provider … That refusal is raised at
  the caller tier, **before any wait and before any state is touched**, so it is the same
  answer whichever provider is underneath."*
- `pubsub/include/fletcher/pubsub/subscriber.hpp:105-108`: *"The refusal is raised at this
  tier, **before this Subscriber touches any state** and before it blocks on anything, so the
  answer is the same whatever provider is underneath **and whatever another thread happens to
  be doing**."*

Three claims. The last ("the same answer whichever provider is underneath") is true and is the
point of the tier. The other two are false against the tree.

**(a) "when and only when it must enter the provider" — false in the window the reorder itself
created.** The door's actual predicate is `!ts.provider_subscribed` *as observed at door time*
(`subscriber.cpp:284-316`). When another thread is inside `provider->Subscribe` for that topic
(`provider_subscribe_in_progress == true`, `provider_subscribed == false`), a handler's
`Subscribe` for that topic is now **refused** — even though, had it proceeded, the post-wait
re-check at `:323-325` would have returned the cached arrival **without entering the
provider**. So refusal does not imply "must enter the provider". This is not a defect in the
reorder — refusing there is strictly better than the deadlock that preceded it (§2) — it is a
defect in the sentence. The header's *"whatever another thread happens to be doing"* is the
same claim in its strongest and most clearly false form: what another thread is doing is
precisely what decides the answer in that window.

**(b) "before any state is touched" — false unconditionally.** By the time
`EnsureProviderSubscription` is entered, `Subscriber::Subscribe` (`:506-520`) has already run
`topics.try_emplace(key)` (a node that is **never** erased and is not rolled back),
`next_id.fetch_add(1)` (an id permanently consumed), `subscription_topic[id] = key`, and — the
one that matters — `RewriteEntries(push_back{id, cb, gate})`, which **atomically publishes the
caller's callback into the live fan-out snapshot**. A refused `Subscribe` therefore rolls back
rather than never touching anything, and the callback of a `Subscribe` that threw
`kReentrantCall` is invocable for the duration of that rollback by any delivery iterating that
topic's fan-out — reachable on a provider that begins delivering before `Subscribe` returns,
which is exactly the concurrent-first-Subscribe window of (a).

**Why this is blocking rather than a nit.** This is cycle-2 non-blocking finding **H/N4** ("the
door comment overstates its own scope") *promoted from an in-source comment into frozen
normative spec text and a public header* — the increment made it worse, which is the one
condition under which my brief permits re-raising a finding I did not block on. And the
direction is wrong: cycle-2 N2 was left non-blocking because it *under*-promised; this
*over*-promises. Rulings 49 (*"forbid it, and say so"*), 50 (*"states this plainly rather than
implying"*) and the owner's now ten-times-restated preference for **a narrow claim stated
honestly over a wide one implied** all point the same way. The instruction for this review is
explicit: *a fix that publishes a wrong rule is worse than the silence it replaced.*

**Acceptable fix — prose only, no code change.**

1. Spec §6 clause 6, last sentence of the new paragraph: replace *"when and only when it must
   enter the provider"* with what the code decides on — *"whenever this `Subscriber` has not
   already established a provider-level subscription for that topic"* — and replace *"before
   any wait and before any state is touched"* with *"before this tier waits on anything and
   before it enters the provider; the local record the call had begun is rolled back on the
   way out"*. Keep *"so it is the same answer whichever provider is underneath"*.
2. `subscriber.hpp:105-108`: the same two corrections, and **delete** *"and whatever another
   thread happens to be doing"*.
3. Optionally one honest sentence, in the harness README's register: *a topic another thread is
   at that moment establishing counts as not-yet-subscribed, so a handler may be refused for a
   topic that is served a moment later.*

**Not an oracle-wins tripwire — stated explicitly, because it is a contradiction between the
code and a spec sentence.** The sentence is not pre-existing frozen text the code must obey:
it is part of the §6 clause 6 append that ruling 52 authorised and that this same change is
landing, un-merged. Correcting it *narrows* the claim, which the 2026-09-03 licence permits
without a fresh ruling (and which the round has already done once here — the "contract, not
luck" wording was withdrawn before it ever landed). The remedy is a prose repair, not an owner
stop-and-ask. Recorded so the PM can take the other view if they prefer.

### C2 — BLOCKING. `Subscribe`'s doc comment now publishes both "reported nowhere" and "counted"

`pubsub/include/fletcher/pubsub/subscriber.hpp:84-91` (unchanged by this diff) still reads:

> *"…the exception is **contained at the point of invocation**, every remaining subscriber on
> that topic still receives that sample, and the throw is **not reported anywhere** — no
> status, no return value, no log."*

Twenty-two lines below it, added *by this increment*, `:112-115` reads:

> *"A handler that lets that `PubSubError` escape has it **absorbed by the fan-out** … and
> counted in `AbsorbedCallbackFailures()`; the containment is deliberate, the count is what
> keeps it from being silent…"*

And `:182-183`, also added by this increment, states the principle outright: *"Containment
without a count is a SILENT wrong answer, though, and this counter is the whole difference."*
So one doc comment on one public method publishes the silence the counter exists to remove,
and then publishes the counter. A reader gets two answers.

This is the same defect class as B5 — the published contract not saying what the code does —
arriving through the fix for B5. It is also a ruling-51 conformance point as this change's own
§5.3 amendment reads that ruling (*"contained and reported where it happened"*, discharged by a
count). The spec resolves the tension explicitly (*"the handle **counts** what it absorbed"*,
`spec:706-712`) and `provider.hpp:153-159` is resolvable in context; `subscriber.hpp` is the
only artefact that leaves it standing.

**Acceptable fix — prose only:** amend `:88-89` to *"no status and no return value, and no log
— the only report is the count in `AbsorbedCallbackFailures()` below."*

---

## 2. The reorder, attacked

`internal::RefuseIfInsideDeliveryOn(provider.get(), "Subscriber::Subscribe (new topic)")` now
sits at `subscriber.cpp:316`, between the `provider_subscribed` early return (`:284-286`) and
`provider_cv.wait` (`:319`).

### 2.1 Paths in

**One caller, and only one:** `Subscriber::Subscribe` at `:525`. Nothing else in the tree
references `EnsureProviderSubscription` outside its own definition and one comment. So the
order is `permitted early return → door → wait → re-check → provider->Subscribe`, on every
path, with no alternative entry.

### 2.2 The four things that could have gone wrong

- **The permitted cached-arrival shape still reaches the early return without hitting the
  door.** `ts.provider_subscribed` is read *before* the door.
  `CallerTier.ReentrantSubscribeFromInsideDeliveryDoesNotDeadlock` re-subscribes to `kT1` from
  inside a delivery on `kT1`, where `provider_subscribed` is necessarily true — it returns at
  `:285` and never reaches `:316`. `ASubscribeDuringADrainKeepsItsProviderSubscription`'s
  newcomer likewise: `provider_subscribed` is cleared only *after* the drain, in the same
  critical section as the teardown decision (`:658-668`). Neither existing green case is
  disturbed.
- **The door now precedes every blocking operation in this function.** The only blocks below it
  are `provider_cv.wait` and `provider->Subscribe`. The only lock acquired *above* it is
  `impl_->mu` at `:508`, and no thread ever holds `impl_->mu` across a provider call, a gate,
  or a wait — `EnsureProviderSubscription` unlocks before `provider->Subscribe` (`:334`),
  `Unsubscribe` releases before entering the provider (`:668`), `~Subscriber` the same
  (`:470-477`), and `provider_cv.wait` releases it. So `mu` is bounded and the door is
  reachable.
- **No lost wakeup, no orphaned flag.** The throw leaves `mu` held and
  `provider_subscribe_in_progress` untouched — the door is *above* the only site that sets it
  (`:333`), so there is nothing for `InProgressGuard` to owe and no `notify_all` skipped. The
  cv is broadcast and every waiter re-tests its predicate.
- **No new deadlock on the rollback.** `Subscriber::Subscribe`'s `catch (…)` (`:522-556`)
  reacquires nothing (it already owns `mu`), publishes the retirement under `mu`, **unlocks**,
  then calls `RetireAndDrain` — obeying this file's own stated invariant, "never acquire a gate
  while holding `mu`". On the refusal path the handler is inside a delivery on its own
  `Subscriber`, so `RetireAndDrain` takes the deferral branch and blocks on nothing; a handler
  on a *different* `Subscriber` takes a barrier on a gate created microseconds ago that has
  never been delivered to. Both traced; both free by construction.

**Moving the early return above the wait is also safe.** `provider_subscribed = true` and
`schema_arrival = result.schema` are written in one critical section under `mu` (`:412-415`),
so a pre-wait reader cannot see a torn pair. The only behaviour change is that a caller
arriving in the window where `provider_subscribed` is already true but the in-progress flag has
not yet been cleared returns immediately instead of waiting for a flag that no longer governs
anything. Strictly better.

**And the reorder removes a real deadlock, not a theoretical one.** With the door after the
wait: handler H, inside a delivery the provider is dispatching with its instance mutex held,
subscribes to a topic thread B is mid-`provider->Subscribe` for. H waits on
`provider_subscribe_in_progress`, which only B can clear; B is inside the provider, which
cannot return until H's delivery does. Neither moves. That is the third instance in this item
of one defect class — a refusal placed behind a blocking operation — and the fix is the right
one, better than the "one honest sentence" I offered as the cheaper alternative in cycle-2 N3.

### 2.3 Is `TheCallerTierDoorIsReachedBeforeItCanBlock` a real control?

**Yes.** I did not take the claimed `EXIT=124` on faith; I traced the mechanism.

- The instrument is real, not a stub. `ProbeProvider::Subscribe` gains
  `wait_for_inflight_on_subscribe` (`caller_tier.cpp:131-136`), which sets the
  `subscribe_parked` latch and then `cv_.wait(lock, in_flight_ == 0)`. `in_flight_` is
  incremented by `Deliver` before dispatch and decremented after (`:158-168`). So B is
  genuinely parked inside the provider for the duration of the delivery, which is what the
  loopback and XRCE actually do.
- The interleaving is **made**, not waited for: handler sets `handler_running` → `starter`
  releases `b_may_start` → B enters `provider->Subscribe(kT2)` and sets `subscribe_parked` →
  handler proceeds. No sleeps anywhere on the path.
- Under the pre-fix ordering the deadlock is exact and unbounded (`provider_cv.wait` has no
  timeout), so the case reddens by hanging, and the declared ctest `TIMEOUT 60` on
  `conformance_caller_tier` is what reports it. The README's counts are updated consistently
  ("Four of these twenty-one cases redden by hanging"; 21 `TEST(CallerTier, …)` measured).
- It is **not** greened merely by the door existing:
  `SubscribingToANewTopicFromInsideADeliveryIsRefusedByName` covers that. This case's
  distinguishing power is the hang, and the hang is real.

**One softness, non-blocking (N1 below):** the latch's return value is discarded
(`(void)probe->subscribe_parked.WaitFor(...)`). If the latch never fired, the case would wait
10 s, proceed anyway, and still assert a refusal — silently degenerating into a duplicate of
the case above it. On a build with the door *after* the wait it would still hang, so the red is
preserved; but the instrument should assert its own precondition.

---

## 3. Charter constraint — verified independently, and it HOLDS

The constraint: A3's re-entrancy assertions must be red before A2's catch lands, or the catch
must count and expose what it absorbed. The question for this cycle: **did narrowing
`Subscriber::AbsorbedCallbackFailures()` weaken it?**

**No.** The implementer's claim — that clause AG1-3 reads `AbsorbedTotal()`, which did not
change — checks out, and the independence is stronger than that claim:

- `ProviderConformance.ThrowingCallbackIsAbsorbedWithoutReentering` (`clauses.cpp:561-591`)
  brackets on `DeliveryChannel::AbsorbedTotal()` at `:574`, `:582`, `:586`. That is still the
  file-local process-wide `AbsorbedTotalCounter()` in `pubsub/src/delivery_channel.cpp:16-19`,
  incremented unconditionally at `:69`. Unchanged by the increment.
- The clause **constructs no `Subscriber` at all** — it subscribes through `ScopedSubscription`
  over a `ProviderSubject` — so the narrowed accessor is not merely unread there, it is
  unreachable. The stated reason for leaving `AbsorbedTotal()` process-wide is true as written.
- **No cross-feed in either direction.** The `Subscriber` fan-out's catch increments only
  `absorbed_here` (`subscriber.cpp:397`, a per-`Impl` `shared_ptr<atomic<uint64_t>>`);
  `DeliveryChannel::Deliver`'s catch increments only its own per-channel counter and the
  process total (`delivery_channel.cpp:68-69`). A handler failure contained at the caller tier
  never reaches the channel's catch, and vice versa. So the two tiers' observables cannot
  substitute for one another, which is what the grouping condition requires.
- The other three legs are untouched by this increment and were re-verified by falsification in
  cycle 2: AG1-2 has no exception on its path, AG1-3 has no re-entry on its path, AG1-6 asserts
  `kReentrantCall` by name and is greened by the door alone. The new caller-tier behaviour still
  does not reach AG1-6, which measures `ProviderSubject::Subscribe` directly.

**Answer: the charter constraint holds, and finding 3 did not weaken it.**

---

## 4. The converse — what survived that should not have

Grepped rather than taken on trust.

- **The file-local process-wide counter in `subscriber.cpp` is GONE.** No `static std::atomic`
  remains in that file; the only counter is `Impl::absorbed` (`subscriber.cpp:148`), captured
  by value into the fan-out lambda so it outlives `~Subscriber` exactly as `identity` does.
  `Subscriber::AbsorbedCallbackFailures()` is a non-static `const noexcept` member (`:418-420`),
  and every reference in the tree calls it on an object. No orphan claims it is process-wide:
  `subscriber.hpp:191-193` and `subscriber.cpp:145-147` both say the opposite, and the harness
  README says *"in that `Subscriber`'s"*.
- **The normative base-cast paragraph is GONE, and left no orphan.** No shipped `.md`, `.hpp` or
  `.cpp` still tells a provider to write `static_cast<const PubSubProvider*>(this)` at a *push*
  site; the four real push sites (`in_process_provider.cpp:311`,
  `fast_dds_pubsub_provider.cpp:492,502`, `xrce_dds_pubsub_provider.cpp:978`) and
  `caller_tier.cpp:141` now pass a bare `this` through the typed constructor, which performs the
  adjustment. The only surviving mentions are in `plans/reviews/*`, which are records.
- **The rule really did become a signature, not a second comment.** `DeliveryChannel(const
  PubSubProvider*, SubscribeCallback)` is the only two-argument constructor, and `const void*`
  does not implicitly convert to `const PubSubProvider*` — so the retired shape now fails to
  compile rather than compiling wrongly. The three exempt sites carry the named `RawToken{}` and
  each carries its justification (`bench_pub_sub_type.cpp:378`,
  `test_fast_dds_pubsub_provider.cpp:51`, `xrce_dds_pubsub_provider.cpp:1140`); the XRCE hook
  asks with the same `Impl*` it pushed (`:1147`), so it is self-consistent and outside the
  base/derived question. Count matches the claim exactly: 4 push + 1 probe + 3 raw.
- **The gate is still gone.** `PendingRow`, `DeliverOrDefer`, `gate_owner`, `pending_mu` and the
  string "delivery gate" return zero hits outside `plans/`.
- **The CORRECTION block is an in-place addition, not a rewrite** — the ruling-48 text is intact
  beneath it, per the PDA-DEC-A1 precedent, and a second CORRECTION block records all three
  cycle-2 code-review shapes.

**Nothing survived that should not have.**

---

## 5. Ruling conformance for the increment

- **Ruling 49** (*forbid it, and say so*) — untouched by the increment; `~Subscriber`'s
  published text and the rethrow are as at cycle 2. **Still satisfied.**
- **Ruling 50** (*publish the residue*) — the increment rewrote surrounding sentences in all
  three places and the residue survived each rewrite intact: `subscriber.hpp:157-167`, harness
  README limit 5, spec §6 clause 6. **Still satisfied.**
- **Ruling 51** (*contain and report at the failure site*) — the narrowing improves this: the
  report is now scoped to the party that can act on it, and the accessor's doc says why.
  **Satisfied, except that `subscriber.hpp` also still publishes "not reported anywhere" —
  finding C2.**
- **Ruling 52** (*refuse everywhere*) — the increment does not touch the twelve doors, and the
  caller-tier door composes with them rather than substituting; spec §6 clause 6 still states
  the refusal, and the caller-tier paragraph is additive. **Satisfied as to direction; C1 is
  about the accuracy of the addition, not its direction.**
- **The standing narrow-claim preference** (ten consecutive rulings, most recently 48/50/52) —
  **violated by C1**, the only place in this increment where the tree claims more than it does.
  The harness README, notably, gets it right; the spec and the header do not.

---

## 6. Disposition of my cycle-2 non-blocking findings (§H)

| Cycle-2 | Status |
|---|---|
| **H/N1** — `ProbeProvider` builds a channel with a bare derived `this`, breaking the rule the same diff published | **CLOSED**, and structurally: the rule became a parameter type, so the implicit derived→base conversion does the adjustment at `caller_tier.cpp:141`. Better than the one-line cast I asked for. |
| **H/N2** — §6 clause 6 says Fletcher *"does not detect"* a same-thread transitive case it does detect | **STILL OPEN**, untouched. Not re-raised as blocking: it under-promises, which is the safe direction, and the increment did not make it worse. |
| **H/N3** — the door is preceded by an unbounded wait | **CLOSED**, by the reorder (§2). The implementer chose the code fix over the honest sentence; that is the better choice. |
| **H/N4** — the door comment overstates its own scope (*"before any state is touched"*) | **NOT closed, and made worse.** Promoted from an in-source comment into frozen spec text and a public header. It is now **C1**. |
| **H/N5** — files outside `Files-to-touch` | **Moot.** The increment adds no new out-of-scope file; those it touches (`bench_pub_sub_type.cpp`, `test_fast_dds_pubsub_provider.cpp`, `xrce_test_hook.hpp`) were already on the cycle-1 list and are mechanically forced by the constructor change. |

---

## 7. Non-blocking, this cycle

- **N1 — the new control does not assert its own precondition.** `caller_tier.cpp:1119-1120`
  discards `subscribe_parked.WaitFor(...)`. If the latch never fired, the case would wait 10 s,
  proceed, and pass — silently degenerating into a duplicate of
  `SubscribingToANewTopicFromInsideADeliveryIsRefusedByName` while still claiming to pin the
  ordering. Hang-detection is unaffected, so this is precision, not soundness. Fix:
  `ASSERT_TRUE(...)`, one word.
- **N2 — the same case asserts a status and nothing else.** `caller_tier.cpp`'s own stated
  vacuity rule is *"no case may pass on 'it did not throw' alone. Every case asserts a call
  count, delivered bytes, or an ordering fact."* Its sibling asserts `subscribe_calls`
  unchanged; this one does not. Adding that assertion would also cover N1.
- **N3 — the "one shape that could still hang" note is now stale, in the safe direction.**
  `subscriber.cpp:274-278` and harness README `:886-890` both name the residual hang shape as *a
  provider that delivers synchronously from inside `Subscribe` into a handler that subscribes to
  the same topic*. After the reorder that shape is **refused**, not hung, for any provider that
  dispatches through `DeliveryChannel` — all three, and `provider.hpp:153-159` now says every
  provider does. The note over-states the hazard, which is safe, but it is one of the few places
  the tree is now less accurate than it could be for free.
- **N4 — the base-cast rule survives only on the asking side, unenforced.**
  `RefuseIfInsideDeliveryOn` still takes `const void*`, so the twelve doors'
  `static_cast<const PubSubProvider*>(this)` remains a convention a future provider could forget
  — the same failure mode the push-side change just eliminated. Mitigated:
  `delivery_channel.hpp:56-60` states the invariant (*"Every door asks with a BASE pointer"*) in
  the one place a provider author reads. Not a regression; noted so it is not lost.

---

## 8. Numbers

Measured against `ff714dc`, `plans/reviews/*` excluded: **+1923 / −235** (1565/235 tracked plus
358 lines in three untracked source files) against declared **+1050 / −150** — **+83% on adds**,
up from cycle 2's +63%.

Of 1644 added C++/CMake lines, **1017 (62%) are comment or blank**, so substantive code is
**≈627 against a declared 1050** — still comfortably under budget on code. The remaining 279
added lines are Markdown, of which the shipped share (spec, harness README, three provider
READMEs) is the published contract rulings 49/50/52 each made a deliverable in its own right.
The cycle-3 increment itself is ~90 lines of published prose (B5), the reorder (one moved
statement plus ~30 lines of rationale), the counter narrowing (~4 lines plus doc), the
constructor change (~35 header lines plus 8 call sites), and one ~60-line forcing case. **Every
line traceable to a review finding. Earned; not scope creep.**

---

## RECORD

- `plans/PDA-DEC-AG1-misbehaving-callback.md` `## Numbers`' superseded line reads *"+1713 /
  −235"*; the tree now measures **+1923 / −235** (+83%, not +63%). The "ruled EARNED" judgement
  still holds on the same arithmetic (62% comment, ~627 substantive against 1050).
- The same file's forcing-test mapping item 6 was **renamed** to
  `…EveryProviderMethodIsRefusedFromInsideADelivery` but its body still reads *"all three must
  return `ok()`"* — the ruling-48 assertion under the ruling-52 name. Covered in spirit by the
  first CORRECTION block; not by that line.
- `integration-tests/pubsub-conformance/README.md`'s two `CallerTier` tables enumerate 17 cases
  plus `CancelFromInsideDeliveryDoesNotEnterTheProvider` as "the eighteenth" (correct), and the
  overall count "twenty-one" is correct — but the three PDA-DEC-AG1 caller-tier cases are
  described only under limit 5 and appear in neither table.
- `plans/reviews/design-debt.md`: AG1-DEBT-13 (`ProbeProvider` pushes no provider token) is
  discharged by the tree and is not annotated as such.
