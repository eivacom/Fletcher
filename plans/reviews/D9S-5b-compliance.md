# D9S-5b compliance review (step 4a)

**Verdict: ISSUES (2 blocking, doc-only). STOP-AND-ASK: none.** Scope `f89644f..60ff057`, read-only, no build.
**Proof of read:** `plans/RIX-rulings.md`: 502 lines by `wc -l` (503 in the reader, because the last line has no
newline), read top to bottom. The highest entry is at :493, "2026-11-14 — D9S-5b: a refused batched subscribe never delivers".
No implicit collision: :493 names :478 and narrows it to accepted subscriptions.

## Blocking
- **B1. d7 L1: the law still contradicts its own refused clause.** The clause landed as a trailing bullet (d7:91-97).
  L1's headline ("always reported, never silent") and its law sentence ("Every row that reaches the batcher before
  closing and is not delivered is counted…", d7:75-76) are unqualified. :493 says these rows are "discarded uncounted",
  and the owner knew it "strains d7 L1". A stated principle outranks a detail, so as written the clause reads as
  subordinate to the law it amends. *Fix:* the law sentence gains ", except rows caught by a refused `Subscribe`
  (below; D19)", and the headline gains the same qualifier. This adds no new law: :493 says "L1 gains a clause".
- **B2. d7 L6's headline still says idleness gets no delivery.** "Loss gets a delivery, and idleness does not" (d7:151)
  sits over a body that, like :478, gives an idle close one delivery. This answers pressure test 4: it is the one law
  text left that says an idle close delivers nothing. *Fix:* "…and idleness does not, except the closing (L9, D18)".

## Non-blocking
- **N1 (tier 2 against tier 1, for the owner to approve).** :478 widens the round to "that one domain-7 law" (L6).
  D-D9S-8, the design and the build also revise L9. I judge L9's new sentence ("Exactly one `kClosing` ends an accepted
  subscription…") a restatement of :478 plus :493's "accepted", not a third law, so this is not a STOP. *Suggested
  fix:* an ingest note under :478: "d7 L6 amended; L9 restates the same sentence".
- **N2.** `binding.h:868-870` says the empty closing is a table "once a row has been decoded". The mechanism, hpp and
  README say "once a row has brought a usable schema". A malformed first row on a declared topic leaves a decoder,
  so the closing has zero rows, not NULL (`RunAssemblyCell` kAssembling proves it). *Fix:* use the hpp's wording.
- **N3.** Two stale tier-3 comments still read the closing as conditional: `test_batches.cpp:271` ("then the partial
  one") and `subscriber_arrow.cpp:133` (`Stop`: "the pending window becomes the kClosing batch"). *Fix:* "then exactly
  one kClosing (empty when idle)".

## Checked and conformant
- **(1) Laws.** L6's closing clause, L9 and L1's clause carry exactly :478 and :493. D19 matches :493. The shape
  (zero-row with a decoder, null without) is existing L6/L7 applied to the closing window, which :478 left to the design
  (0b concurs), so it is not extra law. D1 and Findings (c) are narrowed as the design said.
- **(2) Code scope.** d7 has `MarkRegistered` and its one call site (after `batchers_`, where the throw guard is
  disarmed), the `Close` condition (rule 1), and the `Loop` gate plus discard outside `mu_` (rule 2). Net **+15/−2 code
  lines** (declared +20/−4). Rule 3 and m4 are absent, as DEBT-4 allowed. No shim source changed: `binding.h` changed in
  comments only. After domain 7's `Subscribe`, the shim takes only noexcept steps (`batches_server.cpp:142-145`,
  `binding.cpp:910-913`), so "a refused call never delivers" holds at the ABI. Nothing in `dotnet/`, domain 2 or `core/`.
- **(3) Corpus.** 0a's deletion is done, and nothing else cites the deleted test. All 11 tolerance re-anchors (`f1e8363`)
  only move declarations. All 7 assertion re-anchors add exactly one empty `kClosing`, last, 0 dropped, in the right shape
  per cell. Nothing is over-applied. The LAWFUL tests that close by an explicit outside `Unsubscribe` are untouched.
- **(4) DEBT-5.** Its wording is in `binding.h:1060-1061`, the hpp, the README, the cpp comment, and the design and brief
  risk lines. "No reliable end signal", "must not destroy from inside" and "may arrive before this call returns" are gone.
- **(5) Marks.** d7 L1 and L6 (`[built]`, tests named), L9's tests, d9 L1 (F1's destroy-inside cells), d9 L15, BC-5 and
  rung 6 are each backed by a test in the tree that asserts the claim.
- **Tests against the design.** F1 matches its cells, (d) and the controls, and adds an undecoded state. F2 and its
  control match. D7-1, D7-2, G1 and the stress tightening match. Corner cases 1–6 still hold. The refusal is a discard,
  not a recovery path.

## RECORD
- RECORD: the design (rule 3 at :25-26, corner case 2's "unregistered stop discards", m4 at :81) still carries what
  DEBT-4 deleted. Annotate it "deleted (DEBT-4)".
- RECORD: D18 says "accepted" but cites only :478. The narrowing comes from :493, so cite :493 too.
- RECORD: d9 L15 still opens "`[built]` **in part**" (d9:382), although every clause it lists is now built. Reconcile.
- RECORD: d9 §1 :48 and §2 :85 flip their marks without naming a test. The tests are named at L1 and L15, as before.
- RECORD (close finding): tests came in at **+789/−72** against the declared ~+470/−60, **1.7×**. F1 ~280 against 195,
  F2 ~127 against 70, D7-1 ~174 against 110, D7-2 ~75 against 50. Under the 2× line.
- RECORD: step-3 evidence is not in the tree yet: the red-first runs, the reds for m1–m3, m2's deterministic F2 red and
  F2's measured pre-change red rate. It is owed at close (DEBT-4).
