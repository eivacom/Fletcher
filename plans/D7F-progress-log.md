# D7F — Progress log

## Kickoff — 2026-11-10

**Directive (owner):** "Run all three items". Start at D7F-1 and continue through D7F-3, stopping
only for a Stage Brief with decisions, a stop-and-ask or a blocker. Commit and push each item to
`origin feature/d7f-batch-assembly` at close.
**Preflight:** three config keys were missing and were added from values already decided
(`domain_status`, `consistency_audit_path`, `progress_log_path`). The plugin source is at 0.10.0
while the marketplace says 0.9.1; the owner chose to proceed.

## D7F-1 — Loss before a schema is reported at close — 🟢 2026-11-10

**Landed:** `a2c8b97`. A subscription that gets rows but never a usable schema now delivers one null
`kClosing` batch with those rows in `rows_dropped`. Header and README carry D9's wording.
**Steps:** 0a CLEAR · design `8f4ffab` · review APPROVE-WITH-DEBT(2) `b2ef5ae` · tests red at phase A
(0 deliveries vs 1) · 0b CLEAR · green · 4a CONFORMS · 4b no blocking (2 should-fix, 3 nits).
**Verification:** pubsub-arrow 39/39 (PM, forced rebuild); pubsub-arrow-fastdds 4/4 (implementer).
**Numbers:** production +25/−37 against declared +10/−25; design cycles 1; fix cycles 0; implementer
launches 1.
**Record corrections (PM):** d7 L1 tests line, findings (c), L1 violation-1 text, L6 tests and interim
note, L7 body, Q3, findings a3 and b2; citation-drift note on the d7 header.
**Carried to D7F-2:** DEBT-3 (codec lookup race), DEBT-4 (drops-then-good-rows test), DEBT-5 (header
timeout sentence) in `plans/reviews/design-debt.md`.
**Process note:** `conan create` reused a cached binary and skipped the tests because `run_tests` is
not part of the package ID. The config's inner loop now forces a rebuild of `fletcher-pubsub-arrow`.

## D7F-2 — paused 2026-11-10 (environment blocker)

**State:** implementation committed in `dbaa2b6`; inner loop 43/43 at that commit. 4a ISSUES(2, 1 blocking:
the unimportable-schema guard cell), 4b no blocking (S1 test timing should-fix; S2 carried to D7F-3 as
DEBT-6). Fix cycle 1 is written in the working tree, **uncommitted and unverified**: the fourth guard
cell, the stale `cpp:~360` comment, and S1's tolerant wait helper. PM record corrections to d7 and the
design are in the working tree too.
**Blocker:** the host has ~0.1–0.3 GB free virtual memory, so MSVC fails with C1060 (out of heap
space) on `pubsub-arrow` and on `integration-tests/pubsub-arrow-fastdds`, even with one build job.
**To resume:** with memory freed, run the inner loop (expect 43/43), re-run 4a on the fix, then the full
suite, then close D7F-2.

## D7F-2 — Loss before a schema is reported at each deadline — 🟢 2026-11-11

**Resumed:** the memory blocker has cleared (10.6 GB virtual free). Owner directive: "Resume D7F-2, then D7F-3".
**Landed:** `dbaa2b6` + fix cycle 1 `7170ca9` + the close commit. The deadline now reports schema-less loss
without waiting for a schema; a late schema gives normal batches; rows plus drops add up across the change.
**Steps:** 0a CLEAR · design `e30f2aa` · review APPROVE-WITH-DEBT(3) `7916105` · red at phase A · 0b CLEAR ·
green · 4a cycle 1 ISSUES(2, 1 blocking) → cycle 2 ISSUES(1, 0 blocking; F3 comment fixed at close) ·
4b no blocking (S1 fixed in cycle 1; S2 → DEBT-6, carried to D7F-3).
**Verification:** pubsub-arrow 43/43 (PM, forced rebuild, `7170ca9`); pubsub-arrow-fastdds 4/4 (PM).
(The earlier "uncommitted and unverified" state was superseded by `7170ca9`.)
**Numbers:** production +64/−76 against declared +20/−40; adds >50% over, disclosed in the brief, mostly
D9 doc wording. Design cycles 1; fix cycles 1; implementer launches 2 (incl. the close-out edits).
**Record corrections:** design "As built" note and Risks line; d7 L8 close-cell order; design-debt
D7F-1/D7F-2 entries struck to where they landed.
**Carried to D7F-3:** DEBT-6 (the decided state lives in three places).

## D7F-3 — One delivery thread, window order, safe in-callback close — 🟢 2026-11-11

**Landed:** `5e35328` (tests red) · `5b4e807` (phase B) · `a06031b`, `5293cab` (fix cycles). Every batch callback
runs on the subscription's batch thread, one at a time, in window order; `Publish` never runs or waits on one.
A close from another thread drains queued windows, then `kClosing`. An in-callback close, or an Unsubscribe from a
row-delivery thread of the same transport, returns at once and delivery continues after.
**Owner touches (3):** Decision 1 (a), Decision 2 (b) on the design brief; rungs 6(d) and 6(e) after 0b.
All are in the ledger (2026-11-11 entries). L9 is amended to match; L1/L11/§1 had a term-only rename, timer thread → batch thread.
**Steps:**
- 0a CLEAR (10 re-anchors).
- Design `b02747f`, then review NEEDS-REWORK(2 blockers: map window B1, malformed Decision 2 B2), revised to `243a3c3`, then cycle 2 APPROVE-WITH-DEBT(2), C2-1 brief fix before the owner saw it.
- Phase A red, then 0b CLEAR (6(d) plan change → owner), then phase B green.
- 4a ISSUES(3, 1 blocking: 6(e) had no red record and no 0b), then CONFORMS. 4b: 3 should-fix, then 1, then 0.
**PM process slip:** I told the implementer 6(e) needed no 0b; 4a caught it. Its red-on-base record was rebuilt against
`7080670` and a 0b addendum audited it. Rule: every new test, even one on an already-ruled subject, gets 0b before green.
**Verification:** pubsub-arrow 50/50, heavy 2/2, pubsub-arrow-fastdds 4/4 (PM, `5293cab`).
**Numbers:** code +137/−81 vs declared +98/−64 (adds +40%, under the 50% line; ceiling 250). Design cycles 2/2;
fix cycles 2; implementer launches 4/5.
**Open, recorded:** mutual-unsubscribe deadlock (pre-existing, documented, BC-5); N2 domain-3 question (BC-5);
L5 cadence and L10 throwing-callback marked built with no test (0a/0b notes); BC-7 out of scope.
