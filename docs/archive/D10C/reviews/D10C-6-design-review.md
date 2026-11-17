# D10C-6 — design review, cycle 1 of 2

**Verdict: APPROVE-WITH-DEBT(4).** No BLOCKER, no STOP-AND-ASK. **The narrowing stands** as the mechanism (DEBT-1).

**Proof of read:** `plans/RIX-rulings.md`, 649 lines (Read; :649 blank), 50 `## ` entries, read top to bottom; last
heading :643 "## 2026-11-16 — D10C-5: the stage-size overrun". Also read: d10 L3 (:149-181), §4 :447-449, §5 D1-D34
and §8; ladder rungs 6-7; audits 0a and 0b; FWD-1/1b (`design-debt.md:902-904`). Tree checked: `SA:173-451`,
`Subscriber.cs:117-163, 382-471, 555-619`, `pubsub/src/subscriber.cpp:318-327, 584-751`, `in_process_provider.cpp:299-334`,
`h:1066-1127`.

## Checked, clean
- **Ruling :575 (6) and L3's scope.** The scope is one object: `_claims` is an instance field, and the guard is
  `TwoArrowSubscribers…`. The refusal applies in either order (a theory over `rowFirst`). The claim is released after
  `Unsubscribe` and after `Subscription.Dispose`, which are the control's two arms. The same kind twice, a schema watch and
  other topics are not refused. :448 (1) stays for C++/C. :203 (3)/D3 holds, because `Batcher` is untouched. There is no
  principle-vs-detail conflict: L3's ruled-refusal bullet puts this check in managed code by name. At rung 7 fastdds does
  answer the mix natively (d9 (b2), `FL_INVALID_ARGUMENT`), but the managed check runs first, so nothing is forwarded.
- **Exception type.** `InvalidOperationException` is right: the object's own state forbids the call, as at `SA:428-434`
  and `Subscriber.cs:404-411`. A `FletcherException` would assert a C status and origin that never occurred (L2). Neither
  :575 (6) nor the R-6 option text names a type. New public surface is 0 (two `<exception>` docs only).
- **P2 is sound.** Batched ids set bit 63, and row ids come from a counter starting at 1 (`h:1071-1072`, `binding.cpp:115`).
  Rung 7 keeps batched `Subscription`s owned by `_subscriber`, so `BelongsTo` (`SA:373`) still runs before the id is used.
- **Budget.** The design is 149/150 lines. Net production lines: built +101, narrowed about +70; both fit the 250 ceiling.
  The ejection was from the light lane's 60, not the stage ceiling. Files-to-delete "None" is real (0a CLEAR). No bridge.
- **Forcing test.** It is red for the right reason, and the release-then-accept arm is a real control.

## Mechanism: the narrowing stands
Lifetime first. If `Record`, the wiring or the `_batchers` insert throws after the inner subscribe succeeded, the built
`try` frees the topic while the native subscription lives. Those lines throw only on out-of-memory. The narrowing does not
close the class either: `Subscriber.Subscribe`'s own tail (`Subscriber.cs:456-471`) can throw after the native call
succeeds. So neither version is reachable short of out-of-memory, and this is not a BLOCKER. Still, the narrowing is
correct on every path `SubscriberArrow` controls. It errs towards refusing (the ruling's direction), re-indents nothing,
and is about 30 lines smaller.

## DEBT (appended to `design-debt.md`)
- **DEBT-1 (step 3, this stage): apply the narrowing.** Construct `RowDelivery`/`Batcher` **before** `Claim`. Their
  constructors only assign fields (`SA:506-510`, `:625-632`), so nothing between `Claim` and the `try` can throw and leave
  a claim with no subscription. Wrap only the inner `_subscriber.Subscribe` in the `try`, and do `Record` and the wiring
  after it. Key the claims by `TopicPath` (ordinal equality, `TopicPath.cs:206`): `<default>` is a valid segment
  (`:144-178`), so `ToString()` makes `default(TopicPath)` and a real topic `<default>` the same key.
- **DEBT-2 (step 3, this stage): the refusal text steers users into silent loss.** "...or use another SubscriberArrow"
  (`SA:320-321`): a second `SubscriberArrow` on the **same provider** is the cross-subscriber collision d9 (b2) registers
  for domains 3/4. On `inprocess` the later silently replaces the earlier (`in_process_provider.cpp:307-315`). Drop the
  clause, or say "over another provider".
- **DEBT-3 (step 3, this stage): the kind assertion is vacuous.** Every message names both "per-row" and "batched",
  because the attempted kind and the held kind always differ. So `SubscriberArrowTests.cs:394` passes whichever kind the
  message calls held. Assert on "holds a per-row" / "holds a batched".
- **DEBT-4 (rung 7, FWD-1c): P1 is false for the in-delivery carve-out.** A close from inside any delivery on the provider
  leaves the transport subscription "open and quiet … until this Subscriber is destroyed, or the same topic is subscribed
  again" (`subscriber.hpp:185-191`, `subscriber.cpp:720-745`). That is harmless at rung 6, where one fan-out serves both
  kinds. At rung 7, batched runs on a second provider subscriber (d9 (b2)). After a per-row close inside a delivery
  releases the claim, a batched subscribe on `inprocess` replaces the quiet row subscription. A later per-row subscribe
  then takes the `provider_subscribed` fast path (`:324-327`) and gets no rows, silently. The mirror case holds for a
  batched close made from a row delivery. FWD-1 and P3 cover only the batched in-handler close. Rung 7 must keep the claim
  after any close made inside a delivery, for both kinds, until `Dispose` or a same-kind re-subscribe. P1 is reworded at
  close. "Survives rung 7 unchanged" holds for claim and record, but not for this release.
