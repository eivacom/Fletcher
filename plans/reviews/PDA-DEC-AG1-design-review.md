# PDA-DEC-AG1 — design review, cycle 1 of 2

**Read receipt.** `plans/PDA-DEC-rulings.md`, read in full: **47** entries
(`grep -c '^## ' = 47`). Last two: *2026-09-05 — Every protocol driver is C++, so
both sides of the protocol ABI are C++ (scope)* and *2026-09-05 — Ceremony is
expensive; group the remaining work into as few items as makes sense (process)*.

**Verdict: NEEDS-REWORK — 3 BLOCKERs, 8 DEBT.**

Budget is clean: design 269/300 lines, brief 53/60, public surface 2/3, net lines
declared. The charter constraint's structural argument **survives** adversarial
attack (question 1 below) — the grouping stands and I am **not** recommending the
`{A3+A8}` / `{A2}` split. The three BLOCKERs are all about the *breadth and the
edges* of the refusal, not about the grouping.

---

## BLOCKERs

### B1 — STOP-AND-ASK. The refusal is four methods wide, and brief decision 1 states the cost of one

`TranslateSeamFailure` wraps all four `PubSubProvider` methods, so the guard
refuses `CreateTopic`, `Publish`, `Subscribe` **and** `Unsubscribe` when issued
from inside a delivery on the same instance. "Rung 2 — Re-entry itself … no
serve-it-if-safe" makes that deliberate. Measured against the tree, that is a
much larger narrowing than the brief discloses:

| From inside a delivery callback, same instance | loopback today | Fast DDS today | XRCE today | after AG1 |
|---|---|---|---|---|
| `Unsubscribe` | deadlock (`in_process_provider.cpp:258` holds `mu_` across `:279`) | self-wait on reader deletion (`:570-574`) | **served** (recursive `mu`, `:161-168`) | refused |
| `Subscribe` | deadlock | **works** | **works** | refused |
| `Publish` | deadlock | **works** | **works** | refused |
| `CreateTopic` | deadlock | **works** | **works** (`:758`) | refused |

Verified: the Fast DDS data listener holds **no** provider lock during dispatch
(`data_reader_listener.hpp:39-44` — the listener owns only the callback, the
schema and the queue; `fast_dds_pubsub_provider.cpp:570-572` says it is the
*schema* listener that takes the provider mutex), and `Publish` takes only
`std::shared_lock(impl_->mu)` (`:387`). XRCE's `mu` is recursive *precisely so*
that a call issued while `OnTopic` is on the stack relocks on the same thread
(`xrce_dds_pubsub_provider.cpp:161-168`). So publish-from-handler — the
transform-and-republish shape, and the same-protocol half of the bridge the
2026-08-31 ruling says must become "trivially possible" — **works today on two of
the three providers and stops working on all three.**

The only provider on which `Publish` from a handler is actually broken is the
loopback, and only because it holds `mu_` across dispatch — an implementation
choice made for §6 clause 1 serialisation, not a contract requirement. The
2026-08-31 divergence ruling ("fix in-round … so the ABI is defined over
consistent behaviour rather than freezing an inconsistency") points at least as
strongly toward fixing the loopback as toward refusing everywhere.

Brief decision 1's cost line reads, in full: *"Cost: an XRCE application that
cancels from inside its handler stops working — loudly, with a named error."*
Three of the four refusals, and both providers where they are regressions, are
absent. The owner has chosen the honestly-stated narrow claim **nine consecutive
times**; answering (a) on this cost line would be answering on a false premise.

Note also that the design has **no live control in the method dimension**. Clause
4 is the not-too-wide control for *threads*; nothing asserts that anything at all
remains permitted from inside a delivery on the same instance — which is the
dimension in which this guard is widest.

**Acceptable fix (cheapest I would approve):** restate brief decision 1's cost
line to name all four methods and the two providers where `Publish`/`CreateTopic`/
`Subscribe` from a handler work today, and offer the narrower option beside (a) —
refuse only the calls that are unsafe on some provider (`Unsubscribe`, and
`Subscribe` if the loopback's dispatch-under-lock is kept), with the loopback's
own `mu_`-across-dispatch named as the alternative thing to fix. Forbidding is
**not** cheaper here; the axis is breadth, and a per-method predicate at the one
funnel is a few lines either way. Whichever the owner picks, add one clause
asserting what is still permitted from inside a delivery, or the guard has no
anti-widening control on its widest axis.

### B2 — `~Subscriber` is named as a site, given no remedy, and its residue is published untruthfully

The design says *"`subscriber.cpp:588` **and** `~Subscriber` call
`provider->Unsubscribe(...)`"* and then supplies a remedy phrased only for
`Subscriber::Unsubscribe`: skip the teardown, *"leaving `provider_subscribed`
true and the fan-out empty … the transport-level subscription lives until the
`Subscriber` is destroyed or that topic is subscribed again."*

That sentence is false on the destructor path, and the path is reachable and
silent:

- `~Subscriber` (`subscriber.cpp:395-441`) drains, sets `provider_subscribed =
  false` at `:431`, then calls `provider->Unsubscribe(segs)` at `:437` inside a
  bare **`catch (...) {}`** (`:438-439`).
- Reachable: a handler delivering on Subscriber **X** destroys Subscriber **Y**,
  both over the same provider instance. Y is quiescent, so §6 clause 5's
  "destruction requires quiescence" is satisfied for Y — but the *provider-instance*
  predicate the design deliberately chose is true, so the guard fires.
- Silent: the refusal is swallowed at `:438`. `~Subscriber` returns normally. The
  transport subscription stays registered; the provider still holds the fan-out
  callback, which holds `shared_ptr`s to Y's `Impl` (`subscriber.cpp:312-338`), so
  `Impl` and the subscription outlive the object **with no bound at all** — the
  published escape hatch ("until the `Subscriber` is destroyed") has already been
  spent.
- It is also a regression in the owner's stated direction: today that same shape
  *hangs* on the loopback and Fast DDS. The 2026-09-04 rulings say twice, in this
  round, that a loud hang is preferred over a silent leak of the guarantee.

**Acceptable fix — forbidding is cheaper than handling.** The design is already
amending §6; widen §6 clause 5's quiescence from "no callback able to re-enter" to
name the case explicitly — *destroying any seam object over a provider instance
requires that no delivery on that instance is in flight on this thread* — which
makes the case unrepresentable by contract rather than handled, and costs one
sentence. If instead the case is to be handled, the design must (a) say the door
check covers `~Subscriber` too, (b) state that `subscriber.cpp:438`'s bare
`catch (...)` is what makes it silent today, and (c) publish the residue
**correctly** in `subscriber.hpp`: on the destructor path the transport
subscription is unbounded, not bounded by destruction.

### B3 — Unstated premise: two of the three providers do not dispatch from where they store the callback, and `DeliveryChannel` is move-only

*"A provider stores a `DeliveryChannel` where it stores a `SubscribeCallback`
today."* That premise is false at three of the five dispatch sites in the tree,
and it is false for a reason: the sites **copy the callback into a local before
invoking user code**, deliberately, to close a shipped use-after-free.

- `pubsub/src/in_process_provider.cpp:275` — `const SubscribeCallback cb =
  it->second.callback;`, comment at `:270-274` ("HARD-4's pattern: a callback that
  re-enters `Unsubscribe()` would otherwise null the `std::function` being
  invoked").
- `xrcedds-pubsub-provider/src/xrce_dds_pubsub_provider.cpp:296` and `:352` —
  both copy `ts.callback` into a local, with the issue-#62 comment at `:319-323`
  and `:240-244`. `test_xrce_provider.cpp:36`'s
  `ReentrantUnsubscribeNoUseAfterFree` — which this design proposes to delete —
  is the regression test for exactly that copy.

A move-only `DeliveryChannel` cannot be copied to a local, so the literal
conversion recipe does not compile at three sites, and the obvious way out
("dispatch through the live `TopicState`") is the crash HARD-4 fixed, while the
test that would catch it is being retired in the same change. Fast DDS is fine —
`ordered_delivery.hpp` invokes `callback_` in place at `:98`, `:157` and `:208`
(the design lists two of those three; `DrainLocked`'s `:208` is unmentioned).

**Acceptable fix, one word:** make `DeliveryChannel` **copyable**. Nothing in the
rung-1 argument needs move-only — the property that matters is "no accessor to the
wrapped callback", and a copy preserves the token and the `noexcept` catch
identically. Then add one line naming `in_process_provider.cpp:275` and
`xrce_dds_pubsub_provider.cpp:296,352` as the copy-to-locals sites the conversion
must preserve, and `ordered_delivery.hpp:208` as the third Fast DDS dispatch.

---

## The six questions, answered

### 1. The charter constraint — **holds**, structurally. Do not split.

Tracker charter (binding, PM): *"A3's re-entrancy assertions MUST be red before
A2's catch lands, or the catch must count and expose what it absorbed."* The
design takes neither literal branch and substitutes a structural argument. I tried
to break it in both directions and could not:

*Can absorption green an A3 control?* Landing `DeliveryChannel` first, guard
absent: clause 2's callback re-enters and does not throw, so `Deliver`'s catch is
never entered at all. The redness comes from the deadlock on the loopback's
non-recursive `mu_` (`in_process_provider.cpp:258` held across `:279`), from Fast
DDS's self-wait on reader deletion (`:570-574`), and from XRCE serving the call and
recording the wrong status. **None of those three is an exception**, so a catch
cannot convert any of them. Clause 1 is likewise red by hang. If the re-entrant
call throws something *other* than `kReentrantCall`, the callback records that
status and the equality assertion fails inside the callback's own frame, before
any catch exists.

*Can the guard green an A2 control?* Clause 3's callback never re-enters, so
`InsideDeliveryOn` is never consulted on that path. Without absorption the throw
escapes into `Publish`'s `TranslateSeamFailure` (`in_process_provider.cpp:251`)
and the publisher's `Reply` carries `kPayloadTooLarge` — red — or, on XRCE,
unwinds across the C frames `:259-263` documents. A `noexcept` `Deliver` written
*without* a catch terminates the process, which is also red.

*The one case I expected to slip through* is the teardown path: absorption **can**
hide a missing door check, because a `kReentrantCall` raised inside
`Subscriber::Unsubscribe` propagates out of the handler and is swallowed by
`Deliver`. Clause 5 covers it, and covers it correctly — it asserts on the probe
provider's *recorded call count*, which no catch can affect.

So the three controls are order-independent and each is red for a reason the other
mechanism cannot supply. That is a stronger discharge than the charter's branch
(a), which only constrains landing order. **The grouping is sound; I am not
recommending the split.** One observation for the PM: DEBT-2 below (give `Deliver`
an observable absorbed-count, because `pubsub/` has no logger) would *also*
satisfy the charter's literal branch (b) for free.

### 2. Unrepresentable vs discriminated — **claim holds for the delivery path**, with one wording correction and one residue already declared

Enumerated every `SubscribeCallback` invocation in the tree: `in_process_provider.cpp:279`;
`ordered_delivery.hpp:98`, `:157`, `:208`; `xrce_dds_pubsub_provider.cpp:307`,
`:358`. All six are inside the provider packages and all six are replaceable by
`DeliveryChannel::Deliver`. There is **no second kind of user callback at the
seam**: `SchemaArrival` carries no continuation (`schema_arrival.hpp` has no
`std::function`, no `Then`/`OnReady`), so a resolver cannot run foreign code
inside `CreateTopic`'s `TranslateSeamFailure`. Above the seam, `Subscriber`'s
fan-out and `SubscriberArrow` run *inside* the provider's dispatch, so a `noexcept`
`Deliver` is between them and every entry point. No surviving path found.

Two corrections, neither blocking:
- The design says "no callback frame ever reaches `TranslateSeamFailure`". A
  `RowEncoder` frame does, and **must** — §5.1's `std::overflow_error →
  kPayloadTooLarge` rule is stated by type and explicitly covers a `RowEncoder`
  throwing for reasons of its own. I have corrected the wording in place to
  *delivery* callback.
- The claim is "unrepresentable **given** every dispatch goes through `Deliver`",
  and the design already carries that as declared residue (a provider keeping its
  own copy) with an honest why-not-forbidden line. Fine as written.

### 3. `subscriber.cpp:588` — the remedy is right, and A4's lock graph really is untouched. The destructor is not (→ B2)

For `Subscriber::Unsubscribe`, the design's shape is not just adequate, it is the
*correct* one, and the phrase "leaving `provider_subscribed` true" is what makes
it so. The teardown at `:587-589` is the last step, **after** the fan-out entry is
removed under `mu` (`:531-545`) and after `RetireAndDrain` (`:557`). So skipping
`:588` removes nothing A4's promise rests on: no invocation begins (the entry is
gone), none is in progress (the gate was drained or carved out). A4's published
sentence stays true.

"`provider_subscribed` true" also forces the predicate read **inside** the
critical section at `:571-585`, before the flip at `:583` — which is the only
placement that works. Skipping at `:587` after `:583` had already set it false
would leave a live transport subscription that `Subscriber` believes is gone, and
the next `Subscribe` would re-register it (`EnsureProviderSubscription:307`) —
`kInvalidArgument` on Fast DDS (`fast_dds_pubsub_provider.cpp:433-435`), a
silently replaced slot on the loopback. The design gets this right; the
implementer must not "simplify" it to `:587`. Re-subscribe after the skip is also
clean: `EnsureProviderSubscription:307-308` returns the cached
`ts.schema_arrival`, the ordinary fan-out path.

Lock graph (P4): the predicate is a `thread_local` read, taking no lock, inside an
existing critical section. No new lock, no new edge in `lock → provider → gate →
mu` (the cycle `:295-298` warns about). **P4 is genuine and holds.**

The A4-adjacent gap is the destructor, which is B2.

### 4. Rung 1 declined — **legitimate deferral, not charter-fencing**

The runbook's rule is that a stage charter may never forbid the fix to the defect
the stage keeps failing on. Two tests, both passed:

- *Is the NVI split actually forbidden?* Yes, twice over, and not by this item's
  own charter: `stop_and_ask_extra` bullet 1 ("Adding, removing or reordering a
  `PubSubProvider` method (decision 4)") and spec §11/§12.1. Neither was written
  for AG1.
- *Is the chosen mechanism a real fix or a workaround?* Real, for every provider
  that exists. All three are converted in-item, the shared `DeliveryChannel` is
  the only dispatch, and the conformance clauses bind any future one. The gap the
  NVI would close is narrower than the design's own prose suggests: not "a
  provider forgets the guard" (deleting the one-argument `TranslateSeamFailure`
  makes that a **compile error** at every site that exists), but only "a
  third-party provider deliberately keeps its own copy of the callback". That is
  declared residue with a why-not-forbidden line, and PDA-ABI's single host-side
  adapter closes it by construction for loaded drivers — which is where drivers
  will actually come from after PDA-ABI.

This is the item declining to widen a frozen method set it is not authorised to
touch, while shipping a fix that covers the whole present tree. That is deferral,
not fencing.

### 5. The numbers — **+520 is light, but it is not a budget breach**

Bottom-up, at this tree's comment density: `delivery_frame.hpp` ~70;
`delivery_channel.{hpp,cpp}` ~110; `status.hpp` + `core/README.md` +
`test_status_taxonomy.cpp` ~60; 12–14 call-site conversions ~25; loopback ~25;
Fast DDS (`ordered_delivery` ×3 sites, listener, tests, README) ~110; XRCE
(2 sites, the retired test's replacement, README) ~90; `subscriber.{hpp,cpp}` ~30;
spec §5.3/§6/§7 ~60; five conformance clauses with hostile callbacks, a second
thread and TIMEOUT wiring ~280; CMake + harness README + runbook ~50. **≈ 910
added**, i.e. ~1.75× the declared +520 — squarely in the round's measured 1.7–4×
band and at the *good* end of it.

That is a DEBT-grade calibration note, not a breach: the design doc is 269/300,
public surface 2/3, net lines are declared, and the split contingency is already
written with the right cut line ("A2 and A3 must not be split from each other").
What is missing is a *trigger*: name the number at which the split fires (DEBT-7).

### 6. The four stop conditions — **all four are genuine future tripwires; none is already true**

- **P1** — verified by search, not argument: every `add_library` in the tree is
  `STATIC`, `OBJECT` or `INTERFACE`; `pubsub/CMakeLists.txt:11` is exact and
  `core/CMakeLists.txt:7` is `INTERFACE`. No `SHARED` anywhere. Genuine.
- **P2** — `PubSubProvider` has exactly four virtual methods
  (`provider.hpp:111,120,173,181`), so "four sites per provider, twelve in all" is
  a closed count, and the method set is itself a stop-and-ask. Two non-funnelled
  entries exist and should be named rather than left to the count: the
  **destructor** (governed by §6 clause 5) and `FastDDSPubSubProvider::PayloadBytes()`
  (`fast_dds_pubsub_provider.hpp:133`, a read-only accessor). See DEBT-8. Not
  already-true.
- **P3** — verified stronger than the design cites: `subject.hpp:118-119` states
  *"the publisher side may be another process; the subscriber side is always this
  process and always this instance"*, and `peer_subject.cpp:67-71` implements
  `Subscribe`/`Unsubscribe` as direct `provider_->…` calls on the calling thread
  while `PublishRow`/`DeclareTopic` go over the pipe. Genuine, and the peer subject
  really does carry the clause. I have corrected the citation in place.
- **P4** — genuine, and holds; see question 3.

---

## DEBT (register entries; these do not loop the design)

| Id | Owed |
|----|------|
| AG1-DEBT-1 | Two product call sites of the one-argument `TranslateSeamFailure` are outside the twelve and outside `Files-to-touch`: `pubsub/src/provider_registry.cpp:186` and `:211` (factory and path-resolver). Deleting the one-argument form breaks both. Add the file, and say what token a registry call passes — there is no instance yet, and `nullptr` is safe only because the frame stack never holds one. `core/tests/test_write_buffer.cpp:453` (PDA-DEC-A1, in flight) is a fourteenth site and will need rebasing. |
| AG1-DEBT-2 | **`pubsub/src` has no logging facility** — zero hits for `std::cerr`, `printf`, `LOG`, `spdlog`. `Deliver` "logs it" is unimplementable there today, and deleting `data_reader_listener.hpp:46-56` removes the only ERROR line a throwing callback currently produces (`"subscribe callback threw: …"`). Silently discarding an application's exception on all three providers is the wrong shape for a round whose owner has chosen loud-over-silent nine times. Cheapest fix: give `DeliveryChannel` an observable absorbed-count that a clause can assert — which also discharges the charter constraint's literal branch (b) for free. |
| AG1-DEBT-3 | The tree already has this mechanism. `subscriber.cpp:36` is `thread_local std::vector<const void*> g_delivery_stack` with an RAII `DeliveryScope` (`:103-140`) — the exact shape proposed for `core/…/internal/delivery_frame.hpp`. Lift the existing one into the new header and have `subscriber.cpp` use it: tokens are addresses, so provider tokens and `Subscriber` tokens coexist in one stack safely, and the item then shares the code A4 needed four cycles to settle instead of standing a second copy of it beside it. |
| AG1-DEBT-4 | Clause 4's shape must be stated. The loopback holds `mu_` across dispatch (`in_process_provider.cpp:258`→`:279`), so a second thread's seam call **blocks for the whole delivery**. A clause that requires the second call to *complete* while the callback runs deadlocks there — a false red under TIMEOUT. It must require only that the call was *issued* and did not see `kReentrantCall`; its anti-widening force rests on the DDS subjects, where delivery is on a transport thread. |
| AG1-DEBT-5 | No clause asserts what is still **permitted** from inside a delivery on the same instance. Clause 4 guards the thread axis only. Add one once B1's breadth is settled. |
| AG1-DEBT-6 | `subscriber.cpp:298-301` publishes a *hang* — "a provider that delivers synchronously from inside `Subscribe` into a handler that subscribes to the same topic — a loud hang under the suite's TIMEOUT, and published in the harness README". The guard converts that hang into a `kReentrantCall`. The README sentence and the code comment both go stale; `Files-to-touch` already lists the README but the design does not name this edit. |
| AG1-DEBT-7 | Name the split trigger as a number, not a feeling: the design's contingency (i) everything but the DDS providers, (ii) the DDS providers, is the right cut, but "if the two DDS conversions blow the budget" has no threshold. My bottom-up estimate is ≈910 added against a declared 520 (question 5). |
| AG1-DEBT-8 | P2 should name the two entries that do **not** funnel through `TranslateSeamFailure` and say why each is safe: `~PubSubProvider` (covered by §6 clause 5's quiescence rule) and `FastDDSPubSubProvider::PayloadBytes()` (`fast_dds_pubsub_provider.hpp:133`, read-only). Leaving "no thirteenth" resting on the method count alone invites a future accessor to slip in. |

---

## NITs — fixed in place, not returned

Corrected silently in `plans/PDA-DEC-AG1-misbehaving-callback.md`: the rung-1
bullet now says *delivery* callback frame (a `RowEncoder` frame reaches
`TranslateSeamFailure` by §5.1 design), and P3 now cites `subject.hpp:118-119`,
the sentence that actually carries the claim, beside the two lines it cited.

---

## On the Stage Brief's three owner decisions

Decisions 2 and 3 are the right questions and are put fairly. Decision 3 in
particular is the one the C#/Rust binding authors most need answered, and (a) is
plainly right.

Decision **1 is the right question with the wrong cost line** — that is B1. It is
also, on inspection, really *two* questions the brief has fused: "is the answer
uniform across protocols?" (where (a) is obviously right, and is the divergence
ruling's own direction) and "how many of the four calls does it cover?" (where the
answer decides whether transform-and-republish keeps working on two providers).
The owner should be shown the second one separately.

There is a **fourth** decision the brief does not ask and, on B2's evidence,
should: *a handler tears down a different subscriber over the same protocol —
should that be refused, or should the seam simply declare it out of contract?*
The design silently chose "refused, then swallowed by an existing `catch (...)`",
which is neither.
