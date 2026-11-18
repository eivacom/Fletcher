# D9S-5b compliance review (step 4a)

## Cycle 1: ISSUES (2 blocking, doc-only), at `f89644f..60ff057`. Ledger 502 lines, highest entry :493
- **B1** (blocking): d7 L1's headline and law sentence lacked the refused-subscribe exception. Closed in cycle 2.
- **B2** (blocking): d7 L6's headline ("idleness does not") lacked the closing exception. Closed in cycle 2.
- **N1** (tier 2 against tier 1, for the owner): L9 was also revised; judged a restatement; an ingest note under :478 was suggested.
- **N2**: binding.h said "once a row has been decoded", not "usable schema". Closed in cycle 2.
- **N3**: stale closing comments at `test_batches.cpp:271` and `subscriber_arrow.cpp:133`. Closed in cycle 2.
- **Conformant:** (1) laws carry exactly :478/:493; (2) code scope; (3) corpus; (4) DEBT-5; (5) marks; tests; corner cases 1–6.
- **RECORD x6:** R1 design rule 3, corner case 2, m4; R2 D18 cites :493; R3 d9 L15 "in part"; R4 d9 §1/§2 name tests;
  R5 tests +789/−72 (1.7x); R6 step-3 evidence owed at close.

## Cycle 2: ISSUES (1 blocking, doc-only or a one-line revert). STOP-AND-ASK: none.
Scope `60ff057..f140eb3`, read-only, no build.
**Proof of read:** `plans/RIX-rulings.md`: 502 lines by `wc -l`, read top to bottom, unchanged since cycle 1. The highest
entry is at :493, "2026-11-14 — D9S-5b: a refused batched subscribe never delivers". No new entry and no collision.

### Blocking
- **B3. The S1 guard re-creates rule 3, which the design (same commit) records as deleted. This makes DEBT-4 worse.**
  `cpp:257` (`if (reason == kClosing && !registered_) return;`) is rule 3's substance: "queues no `kClosing`". The
  design reads "3. *Deleted (DEBT-4)*" and "m4 *deleted*" (:25, :79), i.e. DEBT-4 branch (a), one mechanism. 4b offered
  the guard *or* "amend rule 3 and drop m4", and both landed. Effect on D7-2's `max_rows` 100 cell (rows pending):
  deleting the guard is masked by Loop's discard (m4 is equivalent again), and m3 no longer turns it red, because the
  guard queues nothing. Branch (a)'s stated payoff, "m3 kills both D7-2 cells", is false; only m2 reddens that path.
  DEBT-4 allows two mechanisms only under branch (b), with m4 declared equivalent.
  *Fix (keep the guard):* rule 3 := "An unregistered `Close(kClosing)` returns at once (4b S1): no closing is built, so
  an unwinding `StopOnThrow` allocates nothing." m4 := "delete that guard: equivalent, masked by rule 2 (DEBT-4 (b))".
  m3 adds "(`max_rows` 100 stays green: the guard)". *Or* revert `cpp:257` to `!(kClosing && registered_)`, i.e. branch (a).

### Checked and conformant
- **B1 closed.** At d7:63-66, the headline and the law sentence each end "except rows caught by a refused `Subscribe`
  (below; D19)", which is :493's "L1 gains a clause", carried into both.
- **B2 closed.** d7:153 reads "Loss gets a delivery, and idleness does not, except the closing (L9, D18)."
- **S1 stays inside the two authorised clauses.** The guard acts only on `kClosing` of a batcher that was never
  registered, which is the L1 refused clause (:493). Accepted closings (L6/L9, :478) are unchanged: once the guard has
  passed, `!HasPending() && reason != kClosing` is rule 1. Non-closing reasons are unchanged. The skipped
  `has_deadline_`/`last_flush_` reset cannot be observed: Loop's `stopped_ && !registered_` branch exits first, and
  `AddRow` returns on `stopped_`. Caught rows stay discarded uncounted, so it is a refusal, not a recovery path. No new law.
- **The P5 comment** at `cpp:476-477` is accurate and adds no code.
- **RECORD fixes.** R2: D18 cites :478, with "accepted" from :493. R3: d9 L15 is `[built]` (D9S-4a..5b, every clause).
  R4: d9 §1 :48 and §2 :85 name `Batches.EveryCloseEndsWithExactlyOneClosingDelivery` and
  `Batches.ARefusedBatchedSubscribeNeverDelivers`, which exist (`test_batches.cpp:1157`, `:1320`). N2: `binding.h:870`
  now says "once a row has brought a usable schema". N3: `test_batches.cpp:271-272` and `cpp:134-136` are fixed, and a
  grep finds no other stale wording. R1: rule 3 and m4 are marked deleted. The text is true, but the code now
  contradicts it (B3).
- The shim source is untouched (a `binding.h` comment only). Code net: +1 line in domain 7.

### Non-blocking
- **N1 is still open.** The log says "No owner question is needed", but that decision is the owner's, not the PM's
  (tier 2 against tier 1). At close, take the ingest note under :478 to the owner: "d7 L6 amended; L9 restates the same sentence".

### RECORD
- RECORD: d7 L1's refused clause "Holds at" (d7:95-97) names `MarkRegistered` and `Loop`'s gate, but not `Close`'s guard.
- RECORD: design corner case 2 (:44), "unregistered stop discards", is true again with the guard. Reconcile it with B3.
- RECORD: R5 (test volume) and R6 (step-3 evidence, including m4 as B3 settles it) are still owed at close.
