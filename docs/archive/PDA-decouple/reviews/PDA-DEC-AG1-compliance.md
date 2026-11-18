# PDA-DEC-AG1 — compliance review (independent, adversarial)

**Item:** PDA-DEC-AG1 — the misbehaving callback (A2 + A3 + A8)
**Diff base:** `ff714dc` · working tree, uncommitted (`git diff` + 3 untracked files)
**Judged against:** `plans/PDA-DEC-rulings.md` (52 entries, BINDING) · `plans/PDA-DEC-AG1-brief.md`
· `plans/PDA-DEC-AG1-misbehaving-callback.md` (known stale at ruling 48) ·
`plans/reviews/PDA-DEC-AG1-design-review.md` · `plans/reviews/design-debt.md` (AG1-DEBT-9…19)

**Read receipt:** 52 entries. Last two — *"A subscriber's handler failure is not the
publisher's business"* (2026-09-05) and *"Re-entry is refused on every protocol; the
capability passes to PDA-ABI"* (2026-09-05, supersedes entry 48 as to direction).

**Verdict: PASS-WITH-FINDINGS.** The mechanisms are conformant: twelve doors all precede
their locks, absorption is a type property, rulings 49/50/51/52 are discharged in the spec
and in behaviour, and A8 landed whole with the compile-failure guard intact. **Four blocking
findings, all of the same shape:** the *published contract text* in three public headers and
in the harness's charter record was not brought forward from ruling 48, so the tree now ships
a seam header that says the opposite of the seam spec amended in the same change.

**Charter constraint: HOLDS.** (Verified independently, section 3.)

---

## 1. The highest-risk change — dispatch restored under the loopback mutex

The implementer deleted the ruling-48 delivery gate (−152) and restored dispatch under
`impl_->mu` (`pubsub/src/in_process_provider.cpp:276` → `:295`), arguing the door, not the
non-recursive mutex, is what stands between a handler and a deadlock. **That argument
survives the attack.** I enumerated every path that reaches `impl_->mu`:

- Exactly four acquisition sites exist — `:211` (CreateTopic), `:276` (Publish), `:306`
  (Subscribe), `:347` (Unsubscribe). Each is preceded, in the same lambda, by
  `internal::RefuseIfInsideDeliveryOn(this, …)` at `:187`, `:268`, `:302`, `:343`.
- No other member acquires it. `~InProcessPubSubProvider` is `= default` and takes no lock;
  destruction from inside a delivery is out of contract under §6 clause 5 as widened. The
  class has no other public method, so there is no state-mutating accessor outside the four
  (design P2's stop condition is not tripped).
- The frame is pushed by `DeliveryChannel::Deliver` carrying the provider's own `this`,
  sealed once at `Subscribe` (`:310`), so every door on the instance recognises it.
- The cross-instance cycle is now *narrower* than before: a handler on P1 that publishes into
  P2 whose handler publishes back into P1 is refused at P1's door on the same thread, where
  at base it deadlocked. Cross-*thread* cycles between two loopback instances remain and are
  pre-existing (AG1-DEBT-9 verified them as loud hangs).
- The one genuinely new edge is the depth-0 sweep: `DeliveryScope::~DeliveryScope` now fires
  at `DeliveryChannel::Deliver`'s frame, i.e. **inside** `Publish`'s `impl_->mu` critical
  section, and it blocks on an A4 `Gate`. It is safe here for a reason worth writing down:
  a deferred entry is only created when the handler cancelled on **its own** `Subscriber`,
  whose gates can only be held by a delivery from **this same** loopback instance, and this
  instance serialises deliveries under `mu` — so the gate is free by construction. No other
  holder of a `Gate` blocks on `impl_->mu` (`Impl::RetireAndDrain` releases the barrier
  before any provider call; `subscriber.cpp:152-162` states that rule as this file's own
  invariant). This is also not a regression: at base `ff714dc` the loopback already
  dispatched under `mu` and the sweep already ran there.

**No path found that reaches the lock without passing a refusal.** No finding.

## 2. Twelve doors, re-audited independently

The error shape verification caught once (door placed *after* `lock_guard(impl_->mu)`) does
not recur. Every door line precedes the first lock line in its own function body, and nothing
in any of the twelve functions acquires a lock before entering `TranslateSeamFailure`:

| Provider | CreateTopic | Publish | Subscribe | Unsubscribe |
|---|---|---|---|---|
| loopback (`in_process_provider.cpp`) | door 187 / lock 211 | 268 / 276 | 302 / 306 | 343 / 347 |
| Fast DDS (`fast_dds_pubsub_provider.cpp`) | 300 / 303 | 396 / 406 (`shared_lock`) | 452 / 455 | 564 / 582 |
| XRCE (`xrce_dds_pubsub_provider.cpp`) | 651 / 659 | 799 / 802 | 862 / 865 | 1007 / 1010 |

Checked additionally: no helper called before a door takes a lock; XRCE's other lock sites
(`:213` in `OnTopic`, `:557` in the run-loop) are not entry points; `FastDDSPubSubProvider::
PayloadBytes()` is read-only and lock-free, as design P2 asserted. **No finding.**

## 3. The charter constraint — re-verified, and it holds

Question asked: *can any single mechanism green all of clauses 1/2/3/6?* Answer: **no.**

- **Door only, no absorption.** AG1-3 (`ThrowingCallbackIsAbsorbedWithoutReentering`) asserts
  `DeliveryChannel::AbsorbedTotal()` rose by exactly one and that the publisher's `Reply` is
  `ok()`; with no absorption the count never moves and the loopback charges the publisher
  `kPayloadTooLarge`. RED. AG1-1's second half likewise RED.
- **Absorption only, no door.** AG1-2 records the status its own frame caught: on the
  loopback a re-entrant `Unsubscribe` re-locks a non-recursive `std::mutex`, which
  `TranslateSeamFailure` turns into `kInternal`, not `kReentrantCall`; on Fast DDS it hangs
  under TIMEOUT; on XRCE it is served. RED. AG1-6 asserts `kReentrantCall` by name for
  `Subscribe` on all six subjects and for `CreateTopic`/`Publish` on the four local ones.
  RED.
- **Structural, not conventional.** AG1-2 has no exception anywhere on its path (the refusal
  is caught and compared inside the callback's own frame, before any catch exists), and
  AG1-3 has no re-entry on its path. Absorption lives in `pubsub/src/delivery_channel.cpp`;
  refusal in `core/include/fletcher/core/internal/delivery_frame.hpp` — still two mechanisms
  in two files, as the design required, and neither can substitute for the other.

Clause 6 was **re-aimed, not deleted**, as ruling 52 requires: it is now
`EveryProviderMethodIsRefusedFromInsideADelivery` and pins both failure directions (no silent
hang — enforced by the per-test ctest TIMEOUT; no silent serve — enforced by asserting the
status *by name*, never merely "not ok"). The constraint is intact. **But its published
record is wrong — see finding B4.**

## 4. `SubjectFactory::publishes_into_subject_instance` — structurally right

The flag is a genuine design deviation (a new field on a `Files-to-touch`-external header)
and it is the right one. It is **structural, not a label match**: it defaults `true`, and the
only site that sets it `false` is `MakePeerSubjectFactory` (`peer_subject.cpp:160`), through
which both peer subjects are built. It does not open a hole:

- On a peer subject the else-branch **asserts `ok()`** rather than skipping, so a refusal
  that leaked across the instance boundary is caught, not hidden. The direction is right.
- The refusal of `CreateTopic`/`Publish` is still asserted on **every provider**, because
  each of the three has a local subject (`InProcessLocal`, `InProcessCarrying`,
  `FastDdsLocal`, `XrceLocal`). No provider's method axis goes unasserted.
- `Subscribe` reaches the subject's own instance on all six subjects, so the clause's
  strongest assertion is unconditional.

This discharges AG1-DEBT-12(a) more strongly than the debt asked for. **No finding.**

## 5. Ruling conformance, item by item

- **Ruling 49 (destroy-another-subscription forbidden, *and said so*).** Spec §6 clause 5 is
  widened and names the case (`spec:733-742`); `~Subscriber` rethrows `kReentrantCall` out of
  a noexcept destructor (`subscriber.cpp:402-421`); the refusal message spells the status in
  text (AG1-DEBT-11 discharged). **Partly failed on "said so" — finding B3.**
- **Ruling 50 (residue PUBLISHED, not merely permitted).** Published in three places:
  `subscriber.hpp:120-130` beside A4's carve-out, harness README limit 5 rewritten from
  "untouched and unclaimed" to the stated residue, and spec §6 clause 6 points at the header.
  **Satisfied.**
- **Ruling 51 (a subscriber's handler failure never reaches the publisher).** Made
  *unrepresentable*, not discriminated: `DeliveryChannel::Deliver` is `noexcept` and catches
  inside the dispatch site, so no delivery-callback frame reaches `TranslateSeamFailure` at
  all. Spec §5.3 rewritten to say exactly that, and to say the absorbed count is what the
  suite asserts on. **Satisfied.**
- **Ruling 52 (uniform refusal).** Twelve doors, all four methods, all three providers. Spec
  §6 clause 6 states the **refusal**; the "contract, not luck" wording never landed in the
  spec; §7 clause 6's open pointer redirects to §6 clause 6; AG1-DEBT-19 registers the
  PDA-ABI forward obligation and explicitly does not pre-authorise it. **Satisfied in the
  spec and in the code — contradicted in three public headers and the harness record (B1,
  B2, B4).**
- **A8 landed whole.** `kReentrantCall = 10` + its own `static_assert` + the `core/README.md`
  row + `internal/status_name.hpp` + `test_status_taxonomy.cpp`'s exhaustive `switch` with no
  `default:` label (compiled with `-Werror=switch` / `/we4062`). Confirmed: an un-published
  append **fails to compile** (part 1 of the guard), and a published row with no number is a
  failure rather than a silently skipped line (part 4). `PubSubError::Sanitize` passes
  `kReentrantCall` through unchanged, and `TranslateSeamFailure` rethrows it untouched.
  **Satisfied.**

## 6. Blocking findings

### B1 — the seam's own public header still publishes ruling 48

`pubsub/include/fletcher/pubsub/provider.hpp:161-166` and `:196-204`. Added *in this diff*:

> `- **Re-entrancy: three of the four methods are SERVED** (§6 clause 6). CreateTopic,
> Publish and Subscribe may be issued from inside a delivery on this same instance and this
> same thread, and every provider serves them … Only \`Unsubscribe\` is refused; see below.`

and, on `Unsubscribe`:

> `**The one call this seam refuses from inside a delivery** (§6 clause 6, …)`

Ruling 52 voids that as to direction and withdraws exactly this permission wording. §6
clause 6, amended in the same diff, says the opposite; so does the shipped code, which
refuses all four. This is the header a provider author and a language-binding author read,
and it is the one artefact that would let a future implementer believe three of four are
contract.

**Acceptable fix:** rewrite both blocks to state the uniform refusal of all four
`PubSubProvider` methods, keeping the two narrowness sentences already there (another
*thread* is not re-entrancy; a *second instance* is not re-entrancy) and adding the PDA-ABI
re-permitting pointer. No code change.

### B2 — the loopback header publishes ruling 48 *and* a mechanism that does not exist

`pubsub/include/fletcher/pubsub/in_process_provider.hpp:48-54`. Added *in this diff*:

> `Threading: one delivery at a time, instance-wide, dispatched under a delivery GATE rather
> than under the instance mutex … CreateTopic, Publish and Subscribe are served from inside a
> delivery (spec §6 clause 6); a row published from a handler is delivered after that handler
> returns rather than nested inside it …`

Every clause of this is false against the shipped tree. There is no gate — `Publish` takes
`std::lock_guard lock(impl_->mu)` at `:276` and calls `channel.Deliver` at `:295` with it
held. Nothing is served from inside a delivery; all four are refused at `:187`, `:268`,
`:302`, `:343`. No row is deferred and drained. The loopback has no README, so this comment
is the **only** published description of its threading model, and it is the one provider
whose published contract is entirely ruling 48.

**Acceptable fix:** restate as *one delivery at a time, dispatched under the instance mutex,
which is also §7 clause 6's drain; all four methods refused with `kReentrantCall` at the door
before any lock*, and delete the gate and deferral sentences.

### B3 — `~Subscriber`'s published carve-out is now a process termination

`pubsub/include/fletcher/pubsub/subscriber.hpp:44-46` (unchanged by this diff):

> `It carries the same carve-out as Unsubscribe — a destructor reached from inside one of
> this Subscriber's own callbacks cannot wait for the frame it is in.`

That is no longer what happens. `~Subscriber` (`subscriber.cpp:392-421`) collects every topic
still `provider_subscribed` — which the new self-cancel skip at `:587` *deliberately leaves
true* — and calls `provider->Unsubscribe(segs)`. From inside that provider's own delivery
frame the door refuses with `kReentrantCall`, and the destructor **rethrows out of a noexcept
destructor**, ending the program. So the shape the header advertises as a supported carve-out
is now precisely the §6-clause-5 violation ruling 49 forbade.

Ruling 49's operative half is *"and say so"*. The spec says it (§6 clause 5, widened); the one
header where an application author would look says the opposite. The design's own rung-1
ladder lists *"Destroying a seam object from inside a delivery on that instance"* as
unrepresentable-by-contract — this header is where that contract leaks back into a permission.

**Acceptable fix:** replace the carve-out sentence in `~Subscriber`'s doc comment with §6
clause 5 as widened — destroying a `Subscriber` while a delivery on its provider instance is
in flight on this thread is out of contract and ends the program naming `kReentrantCall` —
and keep the A4 quiescence paragraph above it. No code change.

### B4 — the charter constraint's published record still describes clause 6 under ruling 48

Two places state that clause 6 needs neither mechanism, which was true only while clause 6
asserted the *permitted* set:

- `integration-tests/pubsub-conformance/README.md`, independence table row for
  `EveryProviderMethodIsRefusedFromInsideADelivery` — *"Which mechanism it needs: **neither**"*.
- `integration-tests/pubsub-conformance/src/clauses.cpp:401` — *"AG1-6 needs neither, and only
  the loopback's delivery gate."*

Clause 6 now asserts `kReentrantCall` by name for all four methods; it needs **the door**, and
nothing but the door greens it. The constraint itself is intact (section 3), but this is the
artefact that records *why the grouping was permitted*, and as written it invites a future
reviewer to conclude clause 6 is not a door control and can be dropped. It also names a
mechanism (the gate) that no longer exists.

**Acceptable fix:** both entries say clause 6 needs **the door only** (no absorption on its
path); delete the "delivery gate" reference. Optionally add the one line that makes the
constraint checkable: *narrowing the door back to `Unsubscribe` greens AG1-1/2 and reds
AG1-6; widening it from per-thread to per-instance greens AG1-6 and reds AG1-4.*

## 7. Non-blocking

- **N1.** `fastdds-pubsub-provider/README.md:514` (unchanged bullet) still says a callback
  re-entering `Publish`/`Subscribe`/`CreateTopic` from the intraprocess-replay first delivery
  *"deadlocks"* — directly above the new bullet saying all four are refused before any lock.
  The `DeliveryChannel` is constructed before `create_datareader` (`:489`, `:502`), so the
  replay pushes a frame and the door fires. Two adjacent bullets now disagree.
- **N2.** `integration-tests/pubsub-conformance/CMakeLists.txt:296` names
  `DeclarePublishAndSubscribeAreServedFromInsideADelivery`, a test that no longer exists.
- **N3.** `clauses.cpp:600` attributes the loopback's blocking to *"the loopback's delivery
  gate"*; the parallel README sentence gets it right (*"holds its instance mutex across
  dispatch"*).
- **N4.** `DeliveryChannel::AbsorbedTotal()` is a third public operation on a surface the
  design declared as exactly two (*"Its only operations are `Deliver` … and
  `AbsorbedCount`"*). Earned — a `ProviderConformance` clause can only see a `ProviderSubject`
  and cannot reach a channel a provider built inside itself — but it is public surface the
  brief's "2 of 3" does not count.
- **N5.** Six files outside `Files-to-touch`, each mechanically forced and none scope creep:
  `core/include/fletcher/core/internal/status_name.hpp` (exhaustive switch),
  `pubsub/include/fletcher/pubsub/in_process_provider.hpp` (doc only — and the site of B2),
  `fastdds-pubsub-provider/benchmarks/bench_pub_sub_type.cpp` and
  `xrcedds-pubsub-provider/src/internal/xrce_test_hook.hpp` (signature changes),
  `integration-tests/pubsub-conformance/include/fletcher/conformance/subject.hpp` and
  `src/peer_subject.cpp` (AG1-DEBT-12(a)).
- **N6.** Token identity across static types is an unstated assumption. `DeliveryChannel` is
  constructed with the *derived* `this`; `Subscriber::Unsubscribe:587` asks with
  `impl_->provider.get()`, a `PubSubProvider*`. They are equal only because every provider in
  the tree is single, non-virtual inheritance from a polymorphic base (verified across all
  twelve subclasses). `delivery_frame.hpp`'s token contract states that tokens are addresses
  of *distinct live objects* but not that two static types of the *same* object must agree.
  Guarded by a live control — `CallerTier.CancelFromInsideDeliveryDoesNotEnterTheProvider`
  would go red — so it is a documentation gap, not a defect.

## 8. Scope, deletions, and the converse

**Ordered deletions — all carried out.** `subscriber.cpp`'s file-local `g_delivery_stack`,
`DeliveryScope`, `DeferredRelease` and `g_deferred_releases` are gone (moved, not
duplicated). The three `catch (...) { draining_ = false; throw; }` pairs in
`ordered_delivery.hpp` are gone. `subscriber.cpp`'s bare `catch (...) {}` is now a typed catch
rethrowing `kReentrantCall`, with the catch-all retained for other teardown failures exactly
as ordered. XRCE's UB warning is narrowed to a citation of the enforcing `noexcept`.

**Retired / re-anchored citations — both check out.**
`XrceProviderTest.ReentrantUnsubscribeNoUseAfterFree` is gone and replaced *in place* by
`ReentrantUnsubscribeIsRefusedAndTheFlushSurvives`, which asserts `kReentrantCall` and keeps
the #62 use-after-free assertion (`delivery_count == 2`).
`OrderedDeliveryTest.SteadyStateSurvivesAThrowingCallback` is re-anchored to
`SteadyStateAbsorbsAThrowingCallbackAndKeepsDelivering`, trading `EXPECT_THROW` for
`EXPECT_NO_THROW` plus an absorbed-count assertion — the correct re-anchor under ruling 51.

**The converse — what survived that should not.** Two constructs the design ordered gone are
still present, and both are conformant:

- `data_reader_listener.hpp:46-56` — kept, narrowed, and cited to **AG1-DEBT-16**, which
  explicitly ordered it kept (*"Narrow `Files-to-delete` to the callback rationale and leave
  the guard in place"*). Verified: it is a `Take`-wide guard, and the diff re-words its log
  lines so they no longer claim to be about the callback.
- The loopback's **dispatch-under-lock**, which the design ordered replaced by the gate.
  Ruling 52 removes the gate's reason to exist and expressly rejects spending a further design
  cycle on it, so restoring the lock is conformance to the ledger. What did **not** come
  along is the header text — that is B2.

**Numbers.** Actual **+1399 / −226** (1109/226 tracked plus 290 lines of new untracked
headers) against declared **+1050 / −150**: +33% on adds, +51% on deletes. Both are earned:
ruling 52 turned three doors into twelve, forced clause 6's rewrite, and added the
uniform-refusal publication to all three provider READMEs and two spec clauses — while
removing the ~150-line gate the declaration had budgeted for. The split trigger (>1300 added
*at the core-half checkpoint*) never came close to firing.

---

## RECORD

- `plans/PDA-DEC-AG1-misbehaving-callback.md` still describes the ruling-48 design (permitted
  set, delivery gate, premise P5, clause 6's old name, and `Files-to-delete`'s "the loopback's
  dispatch-under-lock"). Known-stale by the PM's own statement; the as-landed correction is
  owed.
- `plans/reviews/design-debt.md` AG1-DEBT-9, -17 and -18 are gate-specific and are moot now
  that no gate exists; AG1-DEBT-1 was already retired at cycle 2.
- The harness README's "Three of these **eighteen** cases redden by hanging" correctly updated
  the count from seventeen; the "three" is unchanged and still right.

---

# RE-REVIEW after fix cycle 2 (independent, adversarial)

**Diff base:** `ff714dc` · working tree, uncommitted · **+1713 / −235**
(1373/235 tracked + 340 lines of three new untracked source files;
`plans/reviews/*.md` excluded).

**Read receipt:** 52 entries. Last two — *"A subscriber's handler failure is not the
publisher's business"* (2026-09-05) and *"Re-entry is refused on every protocol; the
capability passes to PDA-ABI"* (2026-09-05, **supersedes entry 48 as to direction**).

**Verdict: PASS-WITH-FINDINGS.** All four cycle-1 blockers are closed against the tree, and
all four cycle-1 non-blocking notes (N1–N3, plus the token-identity gap N6) are closed too.
**One new blocking finding, and it is the same class as the four it replaces:** a new
caller-visible refusal landed in `Subscriber::Subscribe` with no published contract text
anywhere.

**Charter constraint: HOLDS.** Re-verified independently, below.

**The +63% overage is EARNED.** Measured, below.

---

## A. The four claimed-closed blockers — verified against the tree

- **B1 — CLOSED.** `provider.hpp:159-171` now reads *"Re-entrancy: ALL FOUR methods are
  REFUSED"* and names `PubSubError(kReentrantCall)` before any lock. `:197-213` restates it
  on `Unsubscribe`. **Both narrowness sentences survive** ("another THREAD … is not
  re-entrancy and is served"; "a callback reaching a SECOND provider instance is not
  re-entrancy either"). The **AG1-DEBT-19** pointer is present and explicitly says PDA-ABI
  re-permitting "is not pre-authorised, and needs a fresh owner ruling" — which is ruling
  52's own wording, not a paraphrase. A §5.3 absorption bullet was added alongside; it
  states the containment as a type property, matching ruling 51.
- **B2 — CLOSED.** `in_process_provider.hpp:48-60` now says *"dispatched with the instance
  mutex HELD"*, ties that lock to **§7 clause 6's drain** ("an `Unsubscribe` on another
  thread blocks on that mutex until the delivery in flight has returned"), and states all
  four refused *"at a door BEFORE any lock"*. The **gate and deferral sentences are gone.**
  Verified against the code: `Publish` takes `lock_guard(impl_->mu)` and calls
  `channel.Deliver` with it held; `Unsubscribe`'s comment now says *"No separate §7.6 drain
  is needed: `mu` above IS the drain"*, which is true of that lock. **Converse checked:**
  `PendingRow`, `DeliverOrDefer`, `gate_owner`, `pending_mu` and the string "delivery gate"
  return **zero hits** anywhere outside `plans/`. The gate is fully gone, not parked.
- **B3 — CLOSED, and correctly.** `subscriber.hpp:44-59` replaces the false carve-out with
  *"It does NOT carry Unsubscribe's carve-out, and destroying a Subscriber from inside a
  delivery on its own provider ENDS THE PROGRAM"*, cites §6 clause 5 as widened, and
  explains the mechanism exactly as the code does it (the self-cancel skip leaves
  `provider_subscribed` true → the destructor reaches `provider->Unsubscribe` → the door
  refuses → rethrow out of a `noexcept` destructor). It states *why* that is the designed
  answer — the alternative is a silent unbounded leak — which is ruling 49's operative half.
  `subscriber.cpp:432-443` carries the same reasoning at the site. **No redesign was
  attempted, and none was owed:** ruling 49 chose the named stop over the silent leak.
- **B4 — CLOSED.** `README.md:69` and `clauses.cpp:401` both now say **the door only**;
  `clauses.cpp:401` reads *"AG1-6 needs THE DOOR, and only the door"* and adds the
  falsification line in both directions (a door on `Unsubscribe` alone leaves AG1-6 red;
  widening per-thread → per-instance reds AG1-3). The "delivery gate" reference is gone from
  both. `clauses.cpp:600` (old N3) now says *"it holds its instance mutex across dispatch"*.
- **Cycle-1 non-blocking, also closed:** N1 (`fastdds-pubsub-provider/README.md:514` now
  says the replay delivery *"no longer reaches it … all four methods are refused at a door
  before any lock"*), N2 (the stale `DeclarePublishAndSubscribe…` name is gone from
  `CMakeLists.txt`, replaced by the four real ones), N6 (token identity, see §D).

## B. Charter constraint — re-verified, and it still holds

No single mechanism greens clauses 1/2/3/6. Re-checked by falsification, not by reading the
table:

- **AG1-2** (`ReentrantCallIsRefusedWithoutAnyThrow`) has no exception on its path — the
  refusal is caught and compared inside `RecordReentrantUnsubscribe`, in the callback's own
  frame, before any catch exists. Absorption cannot green it.
- **AG1-3** has no re-entry on its path, and asserts on `DeliveryChannel::AbsorbedTotal()`
  rising by exactly one. The door cannot green it.
- **AG1-6** now asserts `kReentrantCall` **by name** for `Subscribe` on all six subjects
  (`clauses.cpp:729-733`), and for `CreateTopic`/`Publish` where they are genuinely
  re-entrant, via `Reply::refused()` rather than `!ok()`. A door on `Unsubscribe` alone
  leaves it red everywhere; absorption cannot produce a status.
- **Importantly, the new caller-tier door does NOT green AG1-6.** `ProviderSubject::Subscribe`
  takes a `PubSubProvider::SubscribeCallback` and reaches the provider directly
  (`subject.hpp:116,136-138`), never through `Subscriber`. AG1-6 still measures the twelve
  provider doors and nothing else. Independence is intact.
- Absorption lives in `pubsub/src/delivery_channel.cpp`; refusal in
  `core/include/fletcher/core/internal/delivery_frame.hpp`. Two mechanisms, two files, still
  structural.

**Answer: YES, the charter constraint holds.**

## C. New public surface — `Subscriber::AbsorbedCallbackFailures()` — JUSTIFIED

Item is now **3 of 3**, at the declared cap. Judged on the merits:

- **Justified.** There are two containment sites, not one. `DeliveryChannel::Deliver`
  contains at the *provider* dispatch site; `EnsureProviderSubscription`'s fan-out lambda
  contains at the *caller* tier (`subscriber.cpp:359-373`), and that second catch is the one
  a handler's own escaped exception hits. Ruling 51's words are *"contained and reported
  where it happened"* — a count is the report, and before this the caller-tier containment
  reported nothing at all. The concrete case is real and now pinned:
  `CallerTier.ARefusedReentrantSubscribeIsCountedRatherThanSwallowedSilently` fails without
  it.
- **Right shape.** Process-wide, monotonic, `noexcept`, `[[nodiscard]]`, function-local
  static so there is no init-order coupling. Bracketed-delta is the only sound way to assert
  on it and the test does exactly that. It mirrors `DeliveryChannel::AbsorbedTotal()`
  one-for-one, so the seam now has one observable per containment site and no third concept.
- **Right home.** A static on `Subscriber` is mildly odd for a process-wide count, but the
  alternative — a free function or a new diagnostics header — is *more* surface, not less,
  and the symmetry with `DeliveryChannel::AbsorbedTotal()` is worth more than the purity.
  It does not trip premise **P2**: P2's stop condition is a fifth method or a state-mutating
  accessor on `PubSubProvider`, and this is a read-only static on a different class.

**No finding.** But see B5 — the header documents the counter and still does not document
the refusal that makes it necessary.

## D. Token identity — correct at every site but one

`static_cast<const PubSubProvider*>(this)` is applied at **all 12 doors** (in_process
187/268/302/344, fastdds 300/396/452/566, xrce 651/799/862/1007) and at **all 4 provider
push sites** (in_process 312, fastdds 493/503, xrce 978). The rule is now written down as a
requirement in `delivery_channel.hpp:56-70`, with the reason stated correctly: the
caller-tier door asks with `impl_->provider.get()`, a **base** pointer, and base/derived
`const void*` equality is incidental rather than guaranteed. The choice not to tighten the
constructor signature is documented in the same block and is the right call — the XRCE test
hook legitimately builds a channel over a bare `Impl*` (`xrce_dds_pubsub_provider.cpp:1136`)
and asks with the same `Impl*` (`:1142`), self-consistent and outside the question. The two
free-standing test tokens (`bench_pub_sub_type.cpp:378`, `test_fast_dds_pubsub_provider.cpp:51`)
are likewise self-consistent and no door asks about them.

**One site was missed — see N1.** `ProbeProvider` is a `PubSubProvider` subclass and passes
a bare derived `this`.

## E. Ruling conformance, final pass

- **Ruling 49** — spec §6 clause 5 widened (`:733-742`); `~Subscriber` rethrows
  `kReentrantCall` out of a `noexcept` destructor; the refusal string spells `kReentrantCall`
  in text (AG1-DEBT-11); and the "say so" half is now in `subscriber.hpp` where an
  application author reads it. **Satisfied** (was the substance of B3).
- **Ruling 50 — the residue is PUBLISHED, not merely permitted.** Three places, all
  affirmative rather than by omission: `subscriber.hpp:134-143` ("stays open and **quiet** …
  until this Subscriber is destroyed, or the same topic is subscribed again"); harness
  README limit 5, rewritten from "untouched and unclaimed" to the stated residue with the
  rejected alternative named; spec §6 clause 6's closing paragraph pointing at it.
  **Satisfied.**
- **Ruling 51** — unrepresentable rather than discriminated: `DeliveryChannel::Deliver` is
  `noexcept` and catches inside the dispatch site, so no delivery frame reaches
  `TranslateSeamFailure`. Spec §5.3 rewritten to say exactly that, including that the
  §5.1 `overflow_error → kPayloadTooLarge` mapping "must never be reachable from a *delivery*
  callback's frame". The count is now the published observable at **both** containment
  sites. **Satisfied.**
- **Ruling 52** — twelve doors, all four methods, all three providers, each before its
  first lock. §6 clause 6 states the **refusal**; the "contract, not luck" wording never
  landed. Clause 6 re-aimed, not deleted. AG1-DEBT-19 registered against PDA-ABI with the
  no-pre-authorisation sentence intact. **Satisfied, and now consistent in every published
  artefact** — which is what cycle 2 was for.
- **Premise P1, hardened beyond what the design asked.** `xrcedds-pubsub-provider/CMakeLists.txt`
  is pinned `STATIC` with the reason written into the file: built shared, that component
  would get its own copy of the `inline thread_local` frame stack and would **serve** a call
  every other provider refuses. All seven `fletcher-*` library targets are now STATIC or
  INTERFACE. This converts P1 from a stop-and-ask a `-DBUILD_SHARED_LIBS=ON` would have
  walked past silently into a build-system invariant. Good, and the right direction.

## F. §6 clause 6's cross-instance paragraph — real constraint, over-stated mechanism

The new normative sentence — *"A callback must not enter a provider instance that can,
directly or transitively, deliver back into the instance it is running on"* — **describes a
real constraint and promises nothing.** It is a caller obligation, and it errs toward
over-forbidding, which is the safe direction and the one the 2026-09-03 licence permits
without asking. It does not hand a reader anything that hangs. That part is right.

The *mechanism* sentence beside it is not quite right — see N2.

## G. Blocking finding

### B5 — the caller-tier refusal in `Subscriber::Subscribe` is published nowhere

`pubsub/src/subscriber.cpp:305` adds
`internal::RefuseIfInsideDeliveryOn(provider.get(), "Subscriber::Subscribe (new topic)")`.

The **placement is correct in both directions**, and I attacked it:

- It sits *after* the `ts.provider_subscribed` early return, so the permitted cached-arrival
  shape still works — `CallerTier.ReentrantSubscribeFromInsideDeliveryDoesNotDeadlock`
  re-subscribes to `kT1` from inside a delivery on `kT1`, hits the early return, and never
  reaches the door.
- It sits *before* `ts.provider_subscribe_in_progress = true`, before `lock.unlock()`, and
  before `provider->Subscribe`. No provider work happens first, and the probe's
  `subscribe_calls` counter is asserted unchanged (`caller_tier.cpp:1030-1032`), which no
  catch anywhere can fake.
- The reported cause is confirmed: `ProbeProvider::Subscribe` (`caller_tier.cpp:125-134`)
  has no door and does serve the call. The refusal genuinely had to move to this tier for the
  seam's answer to be uniform rather than inherited from whichever provider is underneath.

**What is wrong is that nothing says so.** This is new, caller-visible, data-dependent
behaviour on a public seam method: `Subscriber::Subscribe` from inside a delivery on its
provider **throws `PubSubError(kReentrantCall)` for a topic this Subscriber has not already
subscribed, and is served for one it has.** I checked every artefact that could publish it:

- `subscriber.hpp`'s `Subscribe` doc comment (`:84-96`) — silent. It documents what happens
  when a callback throws, and says nothing about `Subscribe` itself throwing.
- Spec §6 clause 6's above-the-seam paragraph — names **only** `Subscriber::Unsubscribe`
  ("asks the same question at its own door and skips the transport-level teardown"). A
  reader takes from it that the caller tier absorbs the question, which is true of cancel and
  false of subscribe.
- §7 clause 6's amendment — same, cancel only ("Fletcher's own cancel path never raises that
  status").
- The harness README's caller-tier limits (1–6) — silent.
- `Subscriber::AbsorbedCallbackFailures()`'s own doc comment describes the case as the
  *reason the counter exists* — the one place in the tree that names it, on a diagnostics
  accessor rather than on the method that throws.

This is the identical shape to cycle-1's B1 and B3: the code does one thing and the published
contract says nothing or says otherwise. It lands against ruling 49's operative half
(*"forbid it, and say so"*), ruling 50's (*"states this plainly rather than implying"*),
and the owner's now nine-times-restated preference for a narrow claim stated honestly over a
wide one implied. The asymmetry with `Subscriber::Unsubscribe` — which is *documented* as
never raising this status — makes the silence worse, because a reader who has read that
paragraph will conclude the caller tier does not raise it at all.

**Acceptable fix — prose only, no code change:**
1. `subscriber.hpp`, on `Subscribe`: one paragraph stating that a `Subscribe` issued from
   inside a delivery on this Subscriber's provider is refused with `kReentrantCall` **when it
   must enter the provider** — i.e. for a topic this Subscriber has not already subscribed —
   and is served from the cached arrival when it need not; and that a handler which lets that
   refusal escape has it absorbed by the fan-out and counted in
   `AbsorbedCallbackFailures()`.
2. Spec §6 clause 6's closing paragraph: widen from `Subscriber::Unsubscribe` to both
   caller-tier methods, saying that cancel skips and subscribe refuses, and why the two
   differ (cancel has a safe skip; subscribe has no answer but the provider's).
3. Harness README: one caller-tier limit entry (or a row beside limit 5) recording the same,
   with the two new `CallerTier` cases named.

## H. Non-blocking

- **N1 — the probe provider breaks the rule this diff just published.**
  `integration-tests/pubsub-conformance/src/caller_tier.cpp:131` builds
  `DeliveryChannel(this, std::move(callback))` with a bare derived `this`, while
  `delivery_channel.hpp:56-60` states *"A provider must pass `static_cast<const
  PubSubProvider*>(this)`, not a bare `this`"*. `ProbeProvider` is single, non-virtual,
  sole-base inheritance so the addresses coincide today, and
  `CancelFromInsideDeliveryDoesNotEnterTheProvider` plus the two new cases would go red if
  they ever stopped — but this is the fourth provider in the tree, it is the one whose
  missing door forced the caller-tier change, and `caller_tier.cpp` is already in
  `Files-to-touch`. Fix: add the cast. One line.
- **N2 — §6 clause 6 says Fletcher "does not detect" a case it does detect.** The paragraph
  reads *"a handler on instance A that calls into instance B, while a handler on B calls into
  A, is not refused by either — and on the loopback … that is a genuine ABBA deadlock rather
  than a refusal … Fletcher does not detect this and will not repair it."* True of the
  **two-thread** mutual case, which is the genuine hazard. **False of the same-thread
  transitive case**, which the following normative sentence is actually about: A dispatches
  (token A pushed) → handler enters B → B dispatches (token B pushed) → handler enters A →
  A's door sees token A on this thread's stack and **refuses by name**. Safe direction (the
  text under-promises), so not blocking, but frozen spec text should not describe a loud
  named refusal as an undetected hang. Fix: one clause splitting same-thread (refused at the
  second door) from cross-thread (a real ABBA hang no door can see).
- **N3 — the door is preceded by an unbounded wait.**
  `EnsureProviderSubscription` runs `provider_cv.wait(lock, …)` *before* the early return and
  therefore before the door (`subscriber.cpp:280`). A handler that should get a prompt
  `kReentrantCall` instead blocks while another thread is inside `provider->Subscribe` for the
  same topic — and on the loopback that other thread is itself blocked on `impl_->mu`, held
  by the delivering thread the handler is running on. That is a deadlock, not a wait.
  **Pre-existing** (the wait predates this door and the loopback dispatched under `mu` at
  base) and the placement is forced — moving the door above the wait would break the
  permitted cached-arrival shape, because the wait is what makes `provider_subscribed`
  accurate. But the door's own comment claims the refusal comes "before any provider work",
  and the published hang note 20 lines above it names a *different* shape (a provider that
  delivers synchronously from inside `Subscribe`). Worth one honest sentence rather than a
  code change.
- **N4 — the door comment overstates its own scope.** *"this refusal is raised before any
  state is touched rather than rolled back after"* is true of `EnsureProviderSubscription`
  and false of `Subscriber::Subscribe`, which has already done `topics.try_emplace`,
  `next_id.fetch_add`, `subscription_topic[id] = key` and `RewriteEntries(push_back)` before
  it calls in — and whose `catch (...)` rollback (`:499-533`) still runs, including
  `RetireAndDrain` on the rolled-back gate. I traced that rollback under the door: it is
  safe on both paths (same-Subscriber handler → the deferred sweep; other-Subscriber handler
  → a fresh, never-delivered gate), so this is a comment accuracy issue only.
- **N5 — files outside `Files-to-touch`.** `xrcedds-pubsub-provider/CMakeLists.txt` (the
  STATIC pin, §E), `pubsub/include/fletcher/pubsub/in_process_provider.hpp`,
  `core/include/fletcher/core/internal/status_name.hpp`,
  `fastdds-pubsub-provider/benchmarks/bench_pub_sub_type.cpp`,
  `xrcedds-pubsub-provider/src/internal/xrce_test_hook.hpp`,
  `integration-tests/pubsub-conformance/include/fletcher/conformance/subject.hpp` and
  `src/peer_subject.cpp`. Each is mechanically forced by a change that *is* in scope; none is
  new capability. Not scope creep.

## I. The numbers — the overage is EARNED

Actual **+1713 / −235** against declared **+1050 / −150**: **+63% on adds**, over the
runbook's 50% threshold. My judgement, measured rather than argued:

| Bucket | Added |
|---|---|
| Markdown total | 244 |
| — of which `plans/` process docs (design CORRECTION + debt register) | 68 |
| — of which shipped docs (spec 90, harness README 80, three provider READMEs 6) | 176 |
| C++ / CMake, tracked + the three new files | 1469 |
| — of which **comment or blank** | **890 (61%)** |
| — of which substantive code | **≈557** |

**Substantive executable and declaration lines added: ~557, against a declared budget of
1050.** The item is *under* budget on code. The entire overage is prose — published contract
text and the in-source rationale this round's rulings keep demanding — and every line of it
is traceable to a review finding or a ruling:

- **Ruling 52** turned 3 doors into 12, forced clause 6's full rewrite and re-aiming, forced
  the uniform-refusal publication into three provider READMEs and two spec clauses, and
  forced the AG1-DEBT-19 registration — while *deleting* the ~150-line gate the declaration
  had budgeted adds for. That is the bulk of cycle 1's +33%.
- **Cycle 2's +314** is B1 (+30, all comment), B2 (+16, all comment), B3 (+51, 50 comment /
  1 declaration), the caller-tier door (1 call + ~20 lines of rationale), the counter
  (2 lines + ~20 of rationale and doc), two new `CallerTier` cases (+78), and the
  README / CMake / spec / fastdds-README corrections I asked for. **Nothing in it is new
  feature scope.**

The split trigger (>1300 added *at the core-half checkpoint*) never fired and was never
close. The one caution worth passing on: this item's ratio of prose to code is high even by
this round's standards, but the 2026-09-05 "ceremony is expensive" ruling was aimed at *plan*
documents, and only 68 lines here are that. The rest is the seam's published contract, which
rulings 49, 50 and 52 each made a deliverable in its own right.

**Verdict: earned. Not scope creep.**

---

## RECORD

- `integration-tests/pubsub-conformance/README.md` says *"Three of these eighteen cases
  redden by hanging"* for `conformance_caller_tier`; there are now **twenty**
  `TEST(CallerTier, …)`. The two new AG1 cases
  (`SubscribingToANewTopicFromInsideADeliveryIsRefusedByName`,
  `ARefusedReentrantSubscribeIsCountedRatherThanSwallowedSilently`) appear in neither table.
- `plans/PDA-DEC-AG1-misbehaving-callback.md`'s CORRECTION block predates fix cycle 2 and
  does not mention the caller-tier door, `Subscriber::AbsorbedCallbackFailures()`, or the
  move to **3 of 3** public surface. `## Numbers` still reads *"+1050 / −150 … Public surface
  2 of 3"*.
- The design's forcing-test mapping item 6 still names
  `…DeclarePublishAndSubscribeAreServedFromInsideADelivery`; the shipped clause is
  `EveryProviderMethodIsRefusedFromInsideADelivery`. Covered in spirit by the CORRECTION
  block, not by name.
