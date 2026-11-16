# D10C-6 — step 4a compliance review, cycle 1

**Verdict: ISSUES (2 blocking, 1 to rule).** Diff `928bb7d` → working tree (`dotnet`, `docs`, `plans/D10C-evidence.md`).

**Proof of read:** `plans/RIX-rulings.md` read top to bottom: **648 lines** (`wc -l` 648; the file ends in one newline,
so "649" counts the empty line an editor shows after it), **50 `## ` entries**, last heading :643 "## 2026-11-16 —
D10C-5: the stage-size overrun". No tier-1 collision: :575 (6) names :448 (1) and revises it for C# only (explicit),
and :448 (1) stands for C++/C. Also read: the design, the brief, the design review (DEBT-1 to DEBT-4), `design-debt.md:902-910`,
audits 0a/0b, d10 L3 (:149-185) and §4 (:436-453), ladder rung 6, and `TopicPath.cs:206-220`.

## Checked, conformant
- **:575 (6), both orders, one-object scope.** `_claims` and `_claimed` are instance fields. A theory over `rowFirst`
  covers both orders, and the `TwoArrowSubscribers…` guard pins the scope. The same kind twice, other topics and the
  schema watch are not refused (L3: no rule without a ruling). `Batcher` is untouched (:203 (3)).
- **Release after Unsubscribe returns:** `_claimed.Remove` and `Release` run only after `_subscriber.Unsubscribe`
  returns normally (`SA:387-400`). `BelongsTo` still runs first. A throw keeps the claim. The id-keyed release happens once.
- **DEBT-1 (partly; see F1).** `RowDelivery`/`Batcher` are built before `Claim`. Nothing that can throw sits between
  `Claim` and the `try`. Claims are keyed by `TopicPath` (ordinal `Equals`/`GetHashCode`, default→null key).
- **DEBT-2:** "or use another SubscriberArrow" is gone. **DEBT-3:** the test asserts "holds a per-row" / "holds a
  batched", and the evidence row shows that control red. **DEBT-4:** left for rung 7 as FWD-1c (`design-debt.md:910`).
- **No new public surface:** `Claim`, `Release` and `Record` are private. The only additions are two `<exception>` docs.
- **Refusal at the door:** the refusal throws under the lock before any native call. Nothing recovers from it.
- **Nothing survived that should not:** Files-to-delete is "None" (0a CLEAR). The `ToString()` key and the
  DEBT-2 clause are gone (grepped).
- **No d7/d9 edit:** the only docs file touched since `928bb7d` is `d10-csharp-binding.md`.
- **Marks vs code:** the L3 header, the ruled-refusal bullet `[built]` and the §4 :448-450 `[built]` match the code (but see F2, F3).

## Findings
**F1 (blocking): DEBT-1 is only partly applied, because `Record` is still inside the `try`.** DEBT-1 (review, and
`design-debt.md:907`) said to "put the `try` around the inner `_subscriber.Subscribe` only; do `Record` and the wiring
after it". In the code, `Record(result.Subscription.Id, topic)` sits inside both `try` blocks (`SA:233`, `SA:286`). If
`Record` throws after the native subscribe succeeded, the `catch` releases the claim while the subscription is still
live. That is the exact gap the narrowing was chosen to close. Only out-of-memory can trigger it, but the shape the
review ordered is not the shape that was built.
*Fix:* in both entry points, move `Record(...)` out of the `try`, to the line after the `catch` block, ahead of the wiring.

**F2 (blocking, doc-only): a Files-to-touch item was missed, so L3's Tests line was never updated.** The design's
Files-to-touch lists "L3's Tests line". The diff instead cites the forcing test inside the ruled-refusal bullet and
leaves the Tests line (`d10:180-182`) unchanged. The two guards, `TheSameKindTwiceAndDifferentTopicsAreNotRefused` and
`TwoArrowSubscribersOnOneProviderDoNotRefuseEachOther`, are cited nowhere in d10.
*Fix:* add all three tests to L3's Tests line, with the guards marked as guards. The bullet may keep its pointer.

**F3 (owner/PM to rule): the server leg was not run.** Ladder rung 6 requires "both orders, on both legs", and the brief's
Verify section lists the server leg. The evidence row says "Server leg not run". Its stated reason ("the batched-first
arms use the managed batcher, so the server leg is not shown by this row") is a description, not a waiver. Meanwhile
the L3 mark and the §4 mark say `[built]` with no qualifier, whereas D10C-3 marked edge-only evidence `[built on the edge shim]`.
*Fix:* run the server leg with the brief's recipe and add its counts to the row. Otherwise qualify both marks
"(edge leg; server leg unrun)" and record a waiver.

## RECORD
- RECORD: The ledger count is cited as 649 (design :95, brief, audits). `wc -l` gives 648. The audits' "no trailing newline" is wrong: the file ends in `\n`.
- RECORD: Design :98 gives the test range `SubscriberArrowTests.cs:357-454`. It is now 357-456, after DEBT-3 added 2 lines.
- RECORD: Design §Design step 3 and the deviations table still describe the pre-DEBT-1 shape (a wide `try`, `ToString()` key). They need a close annotation.
- RECORD: Production lines are +104 / −2. The review's narrowed estimate was about +70. The brief's Numbers and As-landed sections are owed at close.
- RECORD: d10's `SA:N` citations below `SA:215` shift by about +100 (e.g. L3 BC-8 `SA:228-237`, L4 `SA:353-827`). Several were already stale at the base commit.
- RECORD: d10 §4 :448-450 is `[built]` without the `~~…~~` strike that the other built bullets in that list use.

## Cycle 2 — PASS (F1-F3 closed; the d10 citations below are RECORD, not blocking)
**F1 closed.** `Record` now runs after the `catch` in `SubscribeClaimed` (`SA:289`), the objects are built before `Claim`, and only the inner subscribe is inside the `try`. The new `topic.ToKey()` refuses with the same exception and message as `Subscriber.cs:395`, just earlier, so it adds no rule (L3). **F2 closed:** L3's Tests line now lists all three tests, with the guards marked, and the §4 bullet is struck. **F3 closed:** the server leg passed 465/465 on each TFM, before and after the fix. The design now matches the code (+103/−2, ledger 648, tests :357-457). Still true: no d7/d9 edit, no new public surface.
- RECORD: The "unmapped" two do map by content. A +239 offset fits only the old Batcher-era citations; these were current at base, so they move +101. Fixes: d10 :272 `SA:591-606`→`SA:453-468` (`ReportAbsorbed`) and `SA:681-688`→`SA:543-550` (per-row catch); :273 `SA:867-874`→`SA:968-975` (batched catch); :307 (L11) `SA:445-451`→`SA:428-434` (the in-delivery `Dispose` refusal). All other re-anchored `SA:N` citations check out by content. Also: the evidence row's lane (4) says "first build only, as (3)", but that reason does not hold because lane (4) runs `SubscriberArrow` (the brief did not require the lane); the design's Files-to-touch omits the re-anchoring; and the cycle-2 brief's d10 :270/:271 are :272/:273.
