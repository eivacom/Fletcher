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
