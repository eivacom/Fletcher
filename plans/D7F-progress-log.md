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
