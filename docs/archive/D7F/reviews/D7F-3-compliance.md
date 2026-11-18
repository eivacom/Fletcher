# D7F-3 compliance review (step 4a)

*Cycle 1 was compressed at cycle 2 to keep this file within the 120-line cap. Its findings are kept
whole; the passed-checks list is shortened.*

## Cycle 1

**Verdict: ISSUES(3, 1 blocking).** Diff `7080670..5b4e807` (phase A `5e35328`, rulings and 0b
`cbe7f34`, phase B `5b4e807`), checked against the design (rev 2 plus the 6(d)/6(e) rows), the brief,
design review cycles 1-2, DEBT C2-1/C2-2, D-D7F-1..7, d7, the register and the runbook census.
**Proof of read:** `plans/RIX-rulings.md` read top to bottom: **196 lines, 16 entries**; the last is
"2026-11-11 — The D7F-3 ladder gains rungs 6(d) and 6(e)". No tier-1 collision: both 2026-11-11
rulings name L9 and widen it explicitly, and 2026-09-05 still stands.

**B1 (BLOCKING): rung 6(e) had no red-on-base record and no step-0b audit.** The design's red-first
order requires one, and the ladder ruling makes 6(e) the proof of ruling (1). Leg (e) was written in
phase B, together with the fix. Phase A and the 0b audit (scope "6(a-d)") did not cover it, and the
TEST's existing red on (a) says nothing about (e).
*Fix:* run leg (e) alone against `7080670`'s `src/` and record the reasons it fails; audit 6(e) in a
0b addendum.

**N1: the destructor's in-callback exception was overstated.** L9, d7 §4 and the README said the
destructor "returns at once". The code does not wait for that one subscription, but it still waits
for the object's other batched subscriptions. The hpp destructor doc's "as `Unsubscribe` does (see
there)" pulled in the row-delivery-thread exception, which is forbidden for the destructor
(2026-09-05).
*Fix:* text only. Write "does not wait for that subscription's batches", and show the owner the
final L9 text at close.

**N2 (note, pre-existing): a forbidden destruction from a row-delivery thread is not always
stopped.** It is stopped only while domain 3 still has a provider subscription to tear down. If X's
batched subscriptions were all unsubscribed in-callback, `~SubscriberArrow` returns with batches
still owed. *Fix:* record it under BC-5's residue, as a domain 3 question.

**Flagged items.** In d7 L11, "timer thread" is a stale term: the same thread was renamed. The
principle is unchanged. The detach sentence is incomplete (the row-delivery case) but not
contradicted. Production code is +118/−78 against the declared +98/−64 (RECORD).

**Passed:**
- The design shape matches §1-§5 and the Decision 2 (b) `Stop` case (C2-2).
- D-D7F-7: one `cb_` call, one `dropped_` counter, one closer (`Close`); the retry block is gone.
- D-D7F-2: the transport thread appends, closes and queues only; there is no bound and no blocking
  queue.
- D-D7F-5: there are 0 public changes. The private `provider_token_` (C2-2) is the only new member.
- No domain 3 edit.
- Every L1, L9, L12, BC-1, BC-2 and BC-5 flip is backed by a test. BC-3 was already `[built]`.
- The `if (stopped_) return;` that remains is `AddRow`'s door, which §2 keeps. `ready_`,
  `once_flag`, `schema_set` and `schema_decided` are all gone.
- The corner-case ladder holds. The 10 re-anchors match the design, and phase B weakened no phase-A
  assertion.

**RECORD (cycle 1):**
- The brief's "As landed" line.
- d7 "timer thread" / `TimerLoop` terms.
- Stale "Holds at" lines in L1, L4, L9, L10 and L11, and in BC-7.
- Ladder rung 6's `cpp:143` and "exchange guard" text.
- BC-3's status column.
- The design-debt entries DEBT-6 and C2-2 not yet marked landed.

## Cycle 2

**Verdict: CONFORMS. No blocking finding.** Fix cycle 1 is `a06031b` (`git diff 7ce7266 HEAD`).
**Proof of read:** the ledger is unchanged: 196 lines, 16 entries, same last heading.

- **B1 is closed (evidence).** `plans/D7F-evidence.md` § "6(e) red on base" records the setup: leg
  (e) alone, run on `7080670`'s `src`+`include` with the tests of `7ce7266`, red 3 out of 3. It lists
  5 failed assertions: unsubscribe-return 3≠1, in-flight 2, depth 2, and `kRowLimit` delivered off the
  batch thread and on the publishing thread. They match the design row's red reason, and the delivered
  content was correct, so the red is about threading, not content. The audit half rests on the 0b
  addendum now running in parallel (6(e) plus the new guard test). That is a close gate for the PM,
  not a 4a finding.
- **N1 is closed.** L9 now reads "does not wait for that subscription's batches … A destruction still
  waits for the object's other batched subscriptions". d7 §4 and the README match. The hpp destructor
  doc reads "as an `Unsubscribe` from another thread does", says it "still waits for every other",
  and says destroying it from a row-delivery thread "stays forbidden". This is faithful to ruling (1)
  and within D12's scope; L9 adds a consequence of its own main clause, not a new law. *PM:* show the
  owner the final L9 text at close, as asked in cycle 1.
- **N2 and S2 are recorded** in BC-5's residue, (2) and (1). The header and README now state the
  mutual-wait residue in behaviour terms, consistent with the design's Risks.
- **The renames changed no law's meaning.**
  - L11 now says "batch thread" in both of its sentences: the same thread, the same detach rule.
  - L1's change is in the violation-1 history bullet only (`Loop` and "batch thread"). L1's law
    sentence is untouched.
  - §1 Intent is a term change only.
  - L12's history bullet keeps "timer thread", correctly, because it describes the old code.
  - The only law statement whose meaning moved is L9, as ruled.
- **The RECORD lines are applied.** I spot-checked the refreshed "Holds at" lines: L1, L4, L9, L11
  and BC-7 point at the right code at HEAD. The other applied items:
  - the ladder's rung 6 now cites `loop_id_`/`InsideDeliveryOn` and `EXPECT_FALSE`;
  - BC-3 names D7F-3;
  - DEBT-6 and C2-2 are struck as landed.
  The brief's "As landed" line is left to the PM at close.
- **S1 conforms.** The `StopOnThrow` guard is a local struct, so the public surface is unchanged. It
  calls the existing `Stop`, so the closer is still `Close` and there is no new `cb_` call. On a
  same-transport row-delivery thread, `Stop` returns at once and does not wait (D-D7F-2);
  `FailedBatchedSubscribeReleasesItsCallback` exercises exactly that path, through domain 3's existing
  `kReentrantCall` door.
- **S3 conforms.**
  - The end of `Loop` moves `cb_` and `decoder_` out under `mu_`, destroys them with `mu_` released,
    and only then sets `ended_`. A waiting outside close therefore returns after the release.
  - `AddRow` stops at its `stopped_` door, so it never touches the null decoder.
  - `cb_` is still called at one site (cpp:243); line 260 is a move, not a call.
  - Skipping `Reserve` on `kClosing` touches no law.
- **No domain 3 edit.** `pubsub/`, `core/` and the interface spec are unchanged since `7080670`.
- **Test bounds.** Fix cycle 1 shortened bounded waits (`kHold` and `kAwait` to 1 s, `kSeedAwait`
  0.5 s); no assertion changed. The rung-3 wait is now 1 s, which the ladder's "≤ 500 ms" does not
  match but the design's "≤ 500 ms–2 s" does. Red still means red.

**RECORD (cycle 2):**
- RECORD: production is now +136/−80 code lines (cpp +135/−80, hpp +1/−0), net +56, against the
  declared +98/−64 (net +34). This is under the 250 ceiling and belongs in the brief's "As landed".
- RECORD: ladder rung 3 says the wait is "≤ 500 ms"; the test now waits `kHold` (1 s).
