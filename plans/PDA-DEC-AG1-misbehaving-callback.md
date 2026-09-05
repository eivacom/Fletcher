# PDA-DEC-AG1 — The misbehaving callback (A2 + A3 + A8)

## Summary

The seam gains one answer, identical on all three providers, to *"what happens when a
subscriber's delivery callback misbehaves?"* An exception it lets escape is absorbed at the
dispatch site before any transport frame. `CreateTopic`, `Publish` and `Subscribe` **keep
working** from inside a delivery everywhere — the loopback is fixed to make that true; only
`Unsubscribe` on the **same instance on the same thread** is refused, with the newly
allocated `kReentrantCall = 10`; destroying a seam object in that frame becomes out of
contract. Both mechanisms live once, in shared code. §5.3 is discharged, §6 gains a sixth
clause, and §6 clause 5 widens.

> ### CORRECTION — superseded direction (fix cycle 2, 2026-09-05)
>
> **The Summary above, and every part of this design that describes a *permitted* set, is
> superseded by owner ruling 52.** The original text is left intact: the record should show what
> was designed and what replaced it.
>
> **What changed and why.** This design was written under ruling 48 ("refuse only what is unsafe;
> the rest converges up"), which rested on the cycle-1 architecture review's finding that
> `CreateTopic`/`Publish`/`Subscribe` from a handler *work today on Fast DDS and XRCE*. That was a
> lock-graph claim about **Fletcher's** locks. Implementation tested it **by probe rather than by
> reading** and falsified it: a Fast DDS listener callback runs with the **RTPS reader mutex**
> held, so both re-entrant paths hang independently. The capability works on **XRCE only** — one
> protocol of three. Premise **P2** fired as designed and the implementer stopped rather than
> narrowing the permitted set to make its own tests pass.
>
> **As landed instead:** all four methods are refused from inside a delivery on all three
> providers — twelve doors, each **before any lock**. §6 clause 6 states the **refusal**; the
> "read a row and publish a derived one … is contract, not luck" wording was withdrawn before it
> ever shipped. Re-permitting is registered as **AG1-DEBT-19** against PDA-ABI, where the
> loaned-sample receive path lands; ruling 52 does **not** pre-authorise it.
>
> **Retired with the ruling:** the loopback delivery gate (the `PendingRow` / `pending_mu` /
> `gate_owner` / `DeliverOrDefer` deferral) existed *solely* to make re-entrant `Publish` work and
> is deleted — the base already dispatched under the instance mutex, so the doors are purely
> additive and strictly remove deadlock edges. Premise **P5** retires with it, as do
> **AG1-DEBT-9/-17/-18**, whose subject no longer exists. The **charter constraint survives**:
> clause 6 was re-aimed at the refusal being uniform and loud, not deleted, and both step-4
> reviews re-verified its independence by falsification.

> **Added after this block was first written (fix cycle 2, same day).** A caller-tier door in
> `EnsureProviderSubscription`: the probe provider has **no door of its own and was serving**
> re-entrant subscribes, so refusing per-provider was not uniform. With it, `Subscriber::Subscribe`
> to a **new** topic from inside a delivery is refused by name, while an already-subscribed topic
> still returns its cached arrival. Also `Subscriber::AbsorbedCallbackFailures()` — ruling 51
> requires containment, and containment without a count is indistinguishable from swallowing.
> New public surface is therefore **3 of 3**, not the 2 the Numbers section states.

> ### CORRECTION — shapes changed by fix cycle 2's review findings (2026-09-05)
>
> Three details above are no longer true as written; the text stays, the corrections
> are here.
>
> 1. **The caller-tier door precedes `provider_cv.wait`, not follows it.** As first
>    landed it sat after the wait, so a handler subscribing to a topic another thread
>    was mid-`provider->Subscribe` for blocked on a flag only that thread could clear,
>    and that thread was waiting on the delivery the handler was inside — a deadlock
>    where the clause promises a refusal (code review cycle 2, should-fix A). The
>    ordering is now the contract, with `CallerTier.TheCallerTierDoorIsReachedBefore
>    ItCanBlock` as its control; it reddens by hanging.
> 2. **`Subscriber::AbsorbedCallbackFailures()` is a non-static member over a
>    per-`Impl` counter**, not the process-wide static the block above landed. Every
>    observer holds the `Subscriber` whose handler failed, so a process-wide total
>    cannot answer the question the accessor was added for (code review cycle 2,
>    should-fix B; narrowed now on PM ruling, published surface being expensive to
>    narrow later and cheap to widen). `DeliveryChannel::AbsorbedTotal()` stays
>    process-wide — the clause that reads it sees only a `ProviderSubject`.
> 3. **`DeliveryChannel`'s ordinary constructor takes a `const PubSubProvider*`.**
>    The base-cast rule was a normative paragraph of comment across nineteen sites and
>    one of them already broke it (`caller_tier.cpp`, compliance N1); it is now a
>    signature, with the `const void*` form behind `DeliveryChannel::RawToken{}` for
>    the three sites that have no provider object — the XRCE test hook, and the Fast
>    DDS unit-test and benchmark helpers (code review cycle 2, should-fix C). The
>    paragraph the signature replaces is deleted. Public surface is unchanged at
>    **3 of 3**: one type gained an overload and a tag, none was added.

## Design

Two mechanisms over one thread-local — absorption and refusal — deliberately in different
files so neither can substitute for the other, plus the two places the tree must change
for the permitted set to be true everywhere (parts 4 and 5).

**1. The delivery frame** (`core/include/fletcher/core/internal/delivery_frame.hpp`, new;
`internal/` is not public surface — `segments.hpp` precedent). This is
`subscriber.cpp:36`'s `thread_local std::vector<const void*> g_delivery_stack` and its
RAII `DeliveryScope` (`:103-140`) **lifted verbatim** into a shared header, plus
`bool InsideDeliveryOn(const void*)`; `subscriber.cpp` then uses the lifted one rather than
a second copy (AG1-DEBT-3). Tokens are compared by address and **never dereferenced**, so
provider and `Subscriber::Impl` tokens coexist in one stack safely.

**2. `DeliveryChannel`** (`pubsub/include/fletcher/pubsub/delivery_channel.hpp`, new;
public surface item 2). **Copyable** — value-semantic over a `SubscribeCallback` plus the
owning provider's identity token — because three dispatch sites deliberately copy the
callback into a local before invoking user code, closing the HARD-4 / issue-#62
use-after-free: `in_process_provider.cpp:275`, `xrce_dds_pubsub_provider.cpp:296`, `:352`.
Those copies are **preserved as copies of the channel**; nothing in the rung-1 argument
needs move-only. Its only operations are
`void Deliver(const uint8_t*, size_t, const SharedSchema&, const Attachments&) noexcept`
and `uint64_t AbsorbedCount() const noexcept` (shared across copies). `Deliver` pushes the
frame, invokes the callback inside `try` / `catch (...)`, absorbs what escapes, counts it,
pops; there is no accessor to the wrapped callback. `pubsub/src` has no logging facility at
all (AG1-DEBT-2), so the count — in a `shared_ptr`, so every copy reports the same number —
is the observable, asserted by a clause. `noexcept` is load-bearing: it makes an unwind
across XRCE's C frames (`:259-263` calls that path UB) a type property, not a comment. It
replaces six invocation sites: `in_process_provider.cpp:279`; `ordered_delivery.hpp:98`,
`:157`, `:208` (all three Fast DDS dispatches, `DrainLocked`'s included);
`xrce_dds_pubsub_provider.cpp:307`, `:358`.

**3. The door check, on `Unsubscribe` only.** Owner ruling 2026-09-05 ("refuse only what's
unsafe"): the other three calls work today on Fast DDS and XRCE and must keep working, so
the guard is **not** put on `TranslateSeamFailure`, whose one-argument form is **unchanged**
(retiring AG1-DEBT-1 — the two `provider_registry.cpp` sites need no edit). Instead each
provider's `Unsubscribe` opens with `internal::RefuseIfInsideDeliveryOn(this,
"Unsubscribe")`, inside its existing `TranslateSeamFailure` and **before any lock**,
throwing `PubSubError(kReentrantCall,…)`. Three sites, one per provider; `const void*`
keeps `core` independent of `pubsub`. The refused set is exactly: same thread **and** same
instance **and** `Unsubscribe`. Another thread calling during a delivery is not re-entrancy
and still works (A4's drain); a callback reaching a **second** instance is not either
(PDA-DEC-8); the other three methods stay permitted — §6 clause 6 says so positively and
forcing clause 6 asserts it.

**4. The loopback stops dispatching under its lock** — what makes the permitted set true
on the third provider. Today `Publish` holds the non-recursive `impl_->mu` across the
callback (`in_process_provider.cpp:258` → `:279`), so any re-entry deadlocks. Instead, in
the shape `OrderedDelivery` already uses and A4's `Gate` already proves out, the instance
gains one **delivery gate**: a `std::mutex`, the id of the thread holding it, and a
`std::deque` of pending rows. `Publish` does its state work under `mu_`, moves the
finished `std::vector<uint8_t>` out of the lock, then — gate free → take it, `Deliver`,
drain the deque, release; gate held **by this thread** (re-entry) → move the row onto the
deque and return, so the outer frame delivers it after the current handler returns, never
nested; gate held by another thread → block, as today. Delivery stays serialized
instance-wide (§6 clause 1 unchanged — the gateway sees no new concurrency), per-writer
order holds (§7 clause 2), and the deferral moves bytes it owns, copying nothing.

**The lock rule is the whole safety argument:** `mu_` is never held while the gate is
acquired, by anyone. A publisher drops `mu_`, takes the gate, then re-reads the slot's
callback under `mu_` before delivering. `Unsubscribe` clears the callback under `mu_`,
drops `mu_`, then takes and releases the gate — its §7 clause 6 drain, which cannot
self-deadlock because re-entrant `Unsubscribe` is refused at the door above it. That
re-read is what makes "no callback begins after `Unsubscribe` returns" hold with no
`mu_ → gate` edge; the gate is held by `shared_ptr` from the topic slot, so no map
operation can invalidate it.

**5. Fletcher's own `Unsubscribe` never triggers the refusal; its destructor no longer
hides it.** `subscriber.cpp:588` calls `provider->Unsubscribe(...)`, which a handler
cancelling its own last subscription reaches from inside the delivery frame. Rather than
catch the refusal, `Subscriber::Unsubscribe` **checks the same predicate at the door**
and skips the transport-level teardown, leaving `provider_subscribed` true and the
fan-out empty. No callback runs (A4's gates are already retired), so A4's promise is
untouched; the cost — the transport subscription lives until the `Subscriber` is destroyed
or that topic is subscribed again — is published in `subscriber.hpp` beside A4's carve-out.
The predicate is the **provider-instance** one, and the check sits in the critical section
at `:571-585` **before** the flip at `:583`: skipping after it would re-register on the
next `Subscribe` (review §3).

`~Subscriber` is the other path, and ruling 2026-09-05 makes it **out of contract** rather
than remedied — destroying a seam object while a delivery on that instance is in flight on
this thread violates §6 clause 5 as widened. One line changes: the bare `catch (...) {}`
at `subscriber.cpp:438` becomes
`catch (const PubSubError& e) { if (e.status() == kReentrantCall) throw; }`. A throw from
a (noexcept) destructor terminates, printing status and message — the ruling's "stated,
named error instead of a silent one". Today that shape hangs loudly on two providers; the
alternative (skip, leaking the transport subscription and the other `Subscriber`'s `Impl`
with no bound) is the silent leak the owner rejected. Other teardown failures stay
swallowed.

**Behaviour that changes, per provider.** Loopback: the permitted three start working and
re-entrant `Unsubscribe` is refused, both instead of deadlocking; a throwing callback no
longer reaches `TranslateSeamFailure` at `:251`, so `std::overflow_error →
kPayloadTooLarge` can no longer charge a subscriber's bug to a publisher. XRCE: no unwind
across the session pump's C frames, and re-entrant `Unsubscribe` — served today — is
refused. Fast DDS: re-entrant `Unsubscribe` is refused rather than self-waiting on reader
deletion (`:570-574`). On both DDS providers the other three calls are **unchanged**.
**Not touched:** XRCE's `std::recursive_mutex` (`:168`), now **load-bearing** — it is how
`Publish`/`CreateTopic` from a handler keep working there; `OrderedDelivery::draining_`,
which also claims the drain slot against a *second* thread (§6 clause 2), of which only
the `catch (...) { …; throw; }` pairs go; and `subscriber.cpp:298-301`'s published hang,
which stays true — an above-seam lock cycle in `Subscriber`, on which the narrowed guard
never fires (AG1-DEBT-6).

**Spec amendments** (inside the eight of ruling 2026-09-03; the clause 5 widening is
authorised by ruling 2026-09-05). §5.3's task sentence becomes the rule. **§6 clause 5**
widens from "no call in flight, no callback able to re-enter" to name the case:
*destroying any seam object over a provider instance requires that no delivery on that
instance is in flight on this thread*. **§6 gains clause 6** — the refusal, its exact
scope, **and what remains permitted**: `CreateTopic`, `Publish` and `Subscribe` may be
issued from inside a delivery on the same instance and are served, a publish so issued
being ordered after the delivery in progress rather than nested. §7 clause 6's *"that
question is open and is owned elsewhere"* pointer (`spec:795-799`) redirects there.
`kReentrantCall` lands in `core/README.md` in the same change, which
`Taxonomy.PublishedNumbersMatchTheEnum`'s exhaustive `switch` enforces by compile
failure.

## Corner cases forbidden

Rung 1 — unrepresentable:
- **A subscriber's exception reaching a publisher's status.** `Deliver` is `noexcept` and
  catches inside the dispatch site, so no **delivery** callback frame reaches
  `TranslateSeamFailure` (a `RowEncoder` frame does, and must — §5.1's rule is by type).
  No "whose frame threw" discrimination is added: no frame arrives to discriminate.
- **An unwind across a transport's C frames** (XRCE's documented UB) — same mechanism,
  enforced by a type instead of a comment.
- **An unguarded dispatch inside a provider** — a `DeliveryChannel` has no accessor for
  the callback it wrapped.
- **Three providers, three behaviours** — one absorption and one refusal in shared code, and
  the permitted set made true on all three rather than refused everywhere; there is nowhere
  for a provider to write a local policy.
- **A nested delivery on the loopback** — a re-entered `Publish` is enqueued behind the gate
  its own thread holds, so "a callback invoked from inside a callback here" is unreachable.
- **A refusal escaping `Subscriber::Unsubscribe`** — it checks the predicate before
  entering the provider, so Fletcher's own cancel path never raises it.
- **Destroying a seam object from inside a delivery on that instance** — removed by
  contract, §6 clause 5 as widened: no skip-and-leak path, no unbounded residue.

Rung 2 — refused typed at the door, no recovery path, no partial mode:
- **Re-entrant `Unsubscribe`** — one refusal, `kReentrantCall`, before any lock. No
  queue-it, no serve-it-if-safe, no recursive-lock licence, no per-provider subset: the
  *method* set is narrow, but within it every provider answers identically.
- **A §6-clause-5 violation arriving as silence** — `~Subscriber` no longer swallows
  `kReentrantCall`; the program stops, naming status and message.

Handled residue — each with *why it could not be forbidden*:
- **An application may swallow the `kReentrantCall` its own `Unsubscribe` was thrown.** The
  seam cannot compel anyone to look at an exception, and this call — unlike the destructor
  path — leaves no state behind, so terminating would be disproportionate.
- **A callback that never returns still blocks its delivering thread.** §7 clause 6 already
  publishes that the seam cannot bound foreign callback duration (`spec:773`).
- **A provider could copy the callback before wrapping it.** Closing this needs an NVI split
  of `PubSubProvider`, a method-set stop-and-ask; the clauses bind any provider, and
  PDA-ABI's one host-side adapter closes it for loaded drivers by construction.

## Premises and stop conditions

- **P1 — one frame stack per process.** Every component is a STATIC library
  (`pubsub/CMakeLists.txt:11`; `fletcher-core` is INTERFACE), so the inline `thread_local`
  has exactly one instance. **STOP-AND-ASK** if any component becomes a shared library, or
  if PDA-ABI places the dispatch adapter inside the driver DLL rather than host-side — do
  not paper over a forked thread-local with a registration handshake.
- **P2 — `Unsubscribe` is the only entry point that must carry the door check, one site
  per provider.** `PubSubProvider` has four virtual methods (`provider.hpp:111,120,173,181`)
  and ruling 2026-09-05 permits three from inside a delivery. The two entries that do not
  funnel through `TranslateSeamFailure` are named rather than left to a count
  (AG1-DEBT-8): `~PubSubProvider`, covered by §6 clause 5 as widened, and
  `FastDDSPubSubProvider::PayloadBytes()` (`…hpp:133`), read-only. **STOP-AND-ASK** if a
  fifth method or a state-mutating accessor appears, or if a permitted call proves unsafe
  on some provider after all — widening the refused set is an owner decision.
- **P3 — the harness's `Unsubscribe` is a real re-entry.** `ProviderSubject::Unsubscribe`
  reaches the provider directly, on the calling thread, on local *and* peer subjects
  (`subject.hpp:118-119,120,140`). **STOP-AND-ASK** if a subject ever routes it through a
  queue or another thread: the clause would then test nothing.
- **P4 — A4's lock graph is settled and stays settled.** Above the seam this adds one
  `thread_local` read inside the existing critical section at `subscriber.cpp:571-585`:
  no new lock, no new edge in `lock → provider → gate → mu` (review §3). **STOP-AND-ASK**
  if it needs a new edge in `subscriber.cpp` — an owner scope question, not a local fix
  (ruling 2026-09-04, fix cycle 3).
- **P5 — the loopback's new gate adds exactly one edge, pointing one way.** The only rule
  is *never acquire the gate while holding `mu_`*; every path drops `mu_` first.
  **STOP-AND-ASK** if a path is found that must hold `mu_` across the gate: that is a
  `mu_ → gate` cycle, and the answer is an owner question about the loopback's
  serialization guarantee, not a recursive mutex.

## Forcing-test mapping

Clauses 1–4 and 6 live in `ProviderConformance` (`clauses.cpp`), so they run against every
subject — both cross-process ones included — with no per-provider assertions. Per-clause
ctest entries carry a **`TIMEOUT`**: an uncapped hang is not a red (A4 precedent). Runbook
scope becomes the whole suite plus `|conformance_xrce`.

1. **`…HostileCallbackNeitherEscapesNorIsChargedElsewhere`** *(forcing test)*. The callback
   re-enters `Unsubscribe` on this instance, records the status it catches, then throws
   `std::overflow_error`. Asserts recorded status `kReentrantCall`, the publishing `Reply`
   `ok()`, and a following unrelated `DeclareTopic`/`PublishRow` `ok()`. *Red for the right
   reason today:* loopback and Fast DDS **hang** (TIMEOUT); XRCE records no status because
   it serves the call; the loopback charges the publisher `kPayloadTooLarge` for a
   subscriber's throw. It cannot compile before `kReentrantCall` exists.
2. **`…ReentrantCallIsRefusedWithoutAnyThrow`** *(live negative control for A3)*. The
   callback re-enters `Unsubscribe` and does **not** throw: no exception anywhere on this
   path, so absorption is not on it and cannot green it. *Red today:* hang on two
   providers, wrong/absent status on the third.
3. **`…ThrowingCallbackIsAbsorbedWithoutReentering`** *(live negative control for A2)*.
   The callback throws and never re-enters, so the door check is not on this path. Also
   asserts `AbsorbedCount()` rose by one — absorbed, not merely unobserved (AG1-DEBT-2),
   which is the charter's literal branch (b) for free. *Red today:* the loopback charges
   the publisher; XRCE unwinds across C frames.
4. **`…AnotherThreadIsNotRefusedDuringADelivery`** *(not-too-wide control, thread axis)*.
   A second thread issues a seam call on the same instance during a delivery, asserting
   only that it was **issued and did not see `kReentrantCall`**, joining after the handler
   returns — not that it *completed* during the delivery, which the loopback's gate makes
   impossible and would be a false red under TIMEOUT (AG1-DEBT-4); the anti-widening force
   rests on the DDS subjects. *Red if* the guard is widened from per-thread to per-instance.
5. **`CallerTier.CancelFromInsideDeliveryDoesNotEnterTheProvider`**. The probe provider
   records its `Unsubscribe` calls; a handler cancelling its own last subscription must
   produce none while its frame is live. *Red today:* `subscriber.cpp:588` calls through.
6. **`…EveryProviderMethodIsRefusedFromInsideADelivery`** *(renamed from `DeclarePublishAndSubscribeAreServed…`, ruling 52)* *(not-too-wide control,
   method axis — AG1-DEBT-5, and the clause ruling 2026-09-05 requires)*. From inside a
   delivery the callback calls `DeclareTopic`, `PublishRow` on a second topic and
   `Subscribe` on a third; all three must return `ok()` and the derived row must
   **arrive**. *Red today:* the loopback deadlocks (TIMEOUT). *Red after* if the refusal
   is widened beyond `Unsubscribe`, or the deferred row is dropped rather than drained.

   > **CORRECTION (PM, 2026-09-05, cycle-3 close).** The two sentences above are the
   > pre-ruling-52 body and were not brought forward when the case was renamed. Under
   > ruling 52 re-entry is refused on **every** protocol, so the case asserts the
   > opposite of what it says here: all three calls must be **refused with
   > `kReentrantCall`**, and the case is a not-too-wide control in the sense that each
   > refusal must be charged to the re-entrant caller and to no other party. The
   > "derived row must arrive" clause does not survive ruling 52 at all. Raised by the
   > cycle-3 compliance re-review; the shipped test is correct, this prose was not.

## Risks / Unknowns

- **Charter constraint — re-verified under the narrowed refusal, and it holds.** Clauses
  2, 3 and 6 are mutually independent, in any landing order: narrowing changed only
  *which* call clause 2's callback makes, so there is still no exception on clause 2's
  path (the refusal is caught and compared inside the callback's own frame, before any
  catch exists), no re-entry on clause 3's, and clause 6 is green only because the
  loopback stopped dispatching under its lock. No absorption ledger is invented —
  `AbsorbedCount()` is one integer a clause reads.
- **Size — the revision's real risk.** Revision 1 declared +520; review measured ≈910
  bottom-up, and ruling 2026-09-05 added the loopback fix and the permitted-set clause.
  Declared at +1050. **Split trigger, as a number** (AG1-DEBT-7): at the point the core
  half is green — enum, spec, `delivery_frame`, `DeliveryChannel`, loopback gate,
  `Subscriber`, clauses 1–6 against the in-process subject — measure; **if added lines
  exceed 1300, split** into (i) that half and (ii) Fast DDS + XRCE. Pre-authorised by
  this trigger, needing no further design cycle. **A2 and A3 must not be split from each
  other** — that is the masking the charter forbids.
- **The loopback rewrite re-enters delivery code.** The gate is new lock structure.
  Mitigations: the shape is copied from `OrderedDelivery`, the ordering rule is one
  sentence (P5), and clauses 1–6 plus the existing loopback suite run against it. A
  *second* lock-order defect there after the first fix is the owner's stop condition
  (2026-09-04, fix cycle 3) — stop, do not patch again.
- **No coexistence window anywhere** — the dispatch catches are deleted, the loopback path
  replaced, nothing flagged or overloaded.
- **Behaviour narrowed, deliberately:** an XRCE application that cancels from inside its
  handler stops working (loudly); a transport subscription outlives a self-cancelling
  handler (brief decision 1); destroying a seam object from inside a delivery now
  terminates with a named status where it previously hung or leaked.
- **Diagnostics lost, deliberately:** deleting `data_reader_listener.hpp:46-56` removes the
  only ERROR line a throwing callback produces and `pubsub/src` has no logger;
  `AbsorbedCount()` replaces it, asserted rather than read (AG1-DEBT-2).

## Files-to-touch

- `core/`: `include/fletcher/core/internal/delivery_frame.hpp` **(new)** · `include/fletcher/core/status.hpp` · `README.md` · `tests/test_status_taxonomy.cpp`
- `pubsub/`: `include/fletcher/pubsub/delivery_channel.hpp` **(new)** · `src/delivery_channel.cpp` **(new)** · `CMakeLists.txt` · `include/fletcher/pubsub/{provider,subscriber}.hpp` · `src/in_process_provider.cpp` · `src/subscriber.cpp`
- `fastdds-pubsub-provider/`: `src/fast_dds_pubsub_provider.cpp` · `src/internal/{data_reader_listener,ordered_delivery}.hpp` · `tests/test_fast_dds_pubsub_provider.cpp` · `README.md`
- `xrcedds-pubsub-provider/`: `src/xrce_dds_pubsub_provider.cpp` · `tests/test_xrce_provider.cpp` · `README.md`
- `docs/pubsub-interface-spec.md` (§5.3, §6 clauses 5 and 6, §7 clause 6 pointer) · `.claude/runbook.PDA-DEC.config.md` (scope + rebaselined counts; local-only)
- `integration-tests/pubsub-conformance/`: `src/clauses.cpp` · `src/caller_tier.cpp` · `CMakeLists.txt` (per-clause entries + `TIMEOUT`) · `README.md`

## Files-to-delete

- **The dispatch-site catches** — `data_reader_listener.hpp:46-56` and
  `ordered_delivery.hpp:97-103`, `:156-161`, `:209-213` — replaced by `DeliveryChannel::Deliver`.
- **The loopback's dispatch-under-lock** and the deadlock-by-design comments documenting
  it (`in_process_provider.cpp:248`, `:270-275`) — replaced by the delivery gate; the
  comments have no replacement, because the statement stops being true.
- **`subscriber.cpp:36`'s file-local `g_delivery_stack` + `DeliveryScope`** — moved, not
  duplicated, into `internal/delivery_frame.hpp` (AG1-DEBT-3).
- **`subscriber.cpp:438`'s bare `catch (...) {}`** — replaced by a typed catch rethrowing
  `kReentrantCall`; other teardown failures stay swallowed.
- **`XrceProviderTest.ReentrantUnsubscribeNoUseAfterFree`** (`test_xrce_provider.cpp:36`) —
  retired as written, behaviour deliberately changed; replaced **in place** by a case
  asserting `kReentrantCall`, plus clauses 1–2. Its use-after-free is covered instead by the
  copy-to-local of the *channel*, which is why the channel is copyable.
- **XRCE's UB warning** (`:259-263`) — narrows to a citation of the enforcing `noexcept`.

## Numbers

Declared net lines **+1050 / −150** (rev 1 said +520; review measured ≈910, and ruling

> **SUPERSEDED — as landed 2026-09-05:** **+1713 / −235**, new public surface **3 of 3**.
> +63% over declared adds, past the runbook's 50% threshold, and **ruled EARNED** by
> architecture-conformance re-review: of 1469 added C++/CMake lines **890 (61%) are comment
> or blank**, so substantive code is **~557 against the 1050 declared** — under budget. The
> growth is published-contract prose the rulings demanded (spec ~90, harness README ~80,
> in-source rationale), not scope creep. Design cycles 2/2 · implementer launches 5.
2026-09-05 added the loopback fix and the permitted-set clause); split trigger >1300 at the
core-half checkpoint. Public surface **2 of 3**: `kReentrantCall`, `DeliveryChannel`.
