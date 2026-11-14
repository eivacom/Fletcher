# D9S-5a design review — cycle 1

**Verdict: APPROVE-WITH-DEBT(4).** No BLOCKERs. Nothing deviates from a ruling, law or locked decision; no STOP-AND-ASK.

**Reviewed:** the design and the brief, against d9 §1, L4, L7, L11, L13–L17 and §4; d7 L5, L6, L9 and L15; register BC-5, -8, -13 and -14; locked decisions; ladder rungs 5–7; the 0a audit. Tree `37412a4`.
**Proof of read:** I read `plans/RIX-rulings.md` top to bottom: **491 lines**, highest entry at l.478, **"2026-11-14 — D9S-5: every close
ends with one closing delivery; D9S-5 splits"**. No tier-1 disagreement is open; that ruling settled the 0a one.

## Scope against ruling 2026-11-14
- **No assertion covers an idle or in-callback close.** Every CLOSING a test asserts has rows pending: F1's row 5, F2's one row, G1's W2, and G2's six cells with no deadline in reach. F1's second close hits a dead id, a no-op (P2, verified at `subscriber_arrow.cpp:479-512`). That is not an idle close, and it matches the ruling's "exactly one".
- **5b has nothing to undo in production.** The close forwards to `SubscriberArrow::Unsubscribe`, so 5b's L6 change reaches it with no shim edit. The thunk already carries both of 5b's candidate shapes (zero rows, null batch). The only idle sentence stays in `binding.h:867-877`, which 5b rewrites. The one test-side residue is DEBT-2.

## Rulings asked for
**An untagged id refused with `FL_INVALID_ARGUMENT` is argument validation, not a third L4 answer.**
- L4 forbids answering a *seam's* rule again. Bit 63 is the shim's own C spelling of a batched id (d9 §1 "Owns: the C spelling"; D9S-4 B1 and l.239). Domain 7 has no such rule.
- An untagged value lies outside the parameter's domain, as NULL does, and L4 does not count NULL checks either.
- Both alternatives are worse. Forwarding the number closes the batched subscription that shares it: both counters start at 1 (`subscriber.cpp:267`, verified), so the wrong subscription closes silently. A no-op decided in the shim would answer "is it live?" without asking domain 7, which *is* answering domain 3's unsubscribe rule.
- L12 and L13 force the asymmetry with `fl_subscriber_unsubscribe`, which answers `FL_OK` to a batched id; the header documents it.
- No owner decision is needed. Recording it in d9 §4 keeps L4 at two exceptions (DEBT-1).

**The test-only oracle is lawful under L7 and L8.**
- **L7** governs the shim's header and export table. The oracle lives in `c_abi_tests` and adds no export (`exports.map:48` exports `fl_*` by wildcard).
- **L8** binds the edge build. The link is server-only and the edge graph is unchanged with tests on or off, so the "Narrow the docs" ruling is not touched.
- **Precedent:** T13's arrow-bridge oracle. The test binary already holds its own Fletcher core through `fletcher-c-abi-impl`'s PUBLIC `fletcher-pubsub` link, and the two copies share no object.
- **Ladder ground rule:** rung 7 names the oracle, and every exercise stays `fl_*`.
- **Placement:** inside the existing `#if FLETCHER_EXPECTED_VARIANT == 2` block (`test_batches.cpp:295`), because the edge and probe executables compile that file too.

## Budget
**7b, the implied work: no breach, no split.** Priced from the body: one declaration (+4); `RequireServerBuild` and the entry point (about +24/−5, replacing the inline refusal at `binding.cpp:885-890`); one `handles.hpp` declaration; a ~12-line `UnsubscribeBatches`; a 4-line edge throw. That is about **+55/−5 counted against 250**, with no recipe, production CMake or CI edit hidden. Even doubled, it fits. Five files against 2–3 exceeds the estimate, not the ceiling, which counts lines.

**Tests at +490 do not mean the ladder is wrong.** Tests are outside the ceiling. The volume comes from the three rungs the ladder bundled on purpose (l.79-83) and from BC-8's 42-cell grid, which the register names as the structural test. No test is deleted later, so nothing accumulates. The 4a-style overrun risk sits in G1's wrapper, G2's grid, and F2 copying the subscribe's truth table (DEBT-4).

**7a, the documents.** Both are at their caps, which is a finding (DEBT-3). The design reaches 146 lines with about 3,500 characters in four table rows, roughly 35 lines at its own wrap. I priced the work separately, so the compression hides no work.

## DEBT (appended to `plans/reviews/design-debt.md`)
- **DEBT-1 (step 3, docs): scope the marks to what G1 proves.** BC-13's batch half and d9 L14's unmoved-release clause are `[built]` only for "released exactly once, after the callback, on the batch thread". BC-13's pool-baseline test and L14's "keeps nothing" stay `[designed-not-built]`, owed to the ASan/LSan leg. d9 §4: an untagged id is a malformed argument. Header: the row-delivery clause says "on the same provider".
- **DEBT-2 (step 3, tests): keep 5a's tests valid after 5b.** Give each batched subscription its own sink, and assert counts before teardown. Otherwise 5b's empty CLOSING for an idle subscription torn down (F1's destroy variant, G2) turns a 5a count red. F1's destroy variant needs its own control for the 2 s window, on a second subscriber.
- **DEBT-3 (PM): both documents are at their caps.** The brief is 60/60, and its placeholder takes l.57-60, so the ≤5-line As-landed block will push it past 60 at close. Trim at least 3 lines before the owner reads it. The design (146/150) needs nothing unless a cycle 2 opens; then cut rather than add.
- **DEBT-4 (step 3, close): test volume.** F2's truth-table arms share one body with `TheBuildAnswersABatchedSubscribeAsItAnswersTheQuery` (guard e) rather than copying it. G2 is one table-driven cell runner. Report actual test lines against +490 at close; more than 2× is a close finding.

**NITs, fixed in place:**
- In the design, F1's second-subscription arm now applies to the close variant only.
- In the design, G2's six cells now say "no deadline in reach": a 100-year timeout has a deadline that never fires early (d7 L5).
- The brief now says "a close leaves other subscriptions running"; a destroy ends them all.
