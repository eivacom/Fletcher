# D10C-1 — test-authority audit, step 0b (new-test)

**Verdict: CLEAR.** No FORBIDDEN or FORBIDDEN-BY-TOLERANCE test. No new value subject: stop-and-ask not triggered.

**Proof of read:** `plans/RIX-rulings.md` read in full: **603 lines, 45 entries**; last ":595 — ## 2026-11-15 — D10C ladder: the domain-7 port tests". Also d7 §3 L1-L15, d2 L13/L18 and the reserved-key row, d10 §7 Q9 and §8 rule 6, ladder rung 1 and its ground rules, brief, 0a audit.

**Scope:** `git diff 6b3f5c7 -- pubsub-arrow/tests` holds 5 tests, 3 helpers (`BlobBytes`, `OneAttachment`, `WriteHalfBudgetRow`/`BoolListSchema`), no `DISABLED_` and no skips.

| Test | Class | Subject and authority |
|---|---|---|
| `RowsSplitAcrossMessagesAreDroppedNotMisaligned` (test_pubsub_arrow.cpp) | LAWFUL | d7 L1 "Every row … not delivered is counted", L2 (1 + 2 = 3), L3 "A dropped row takes its attachment with it". This is the Q9 case (§8 r6). |
| `AnEmptyAttachmentKeyArrivesWithItsRow` (same) | LAWFUL | d7 L3. An empty key is lawful: d2 L13 refuses only "a zero byte", and a reserved key's "first byte is `0x01`". |
| `AThrowingCallbackDoesNotStopTheNextBatch` (same) | LAWFUL | d7 L10 "A throw from the callback is swallowed"; L12 in order; L9 "Exactly one `kClosing`". It asserts no failure count (d10 L9 keeps that in C#). |
| `ARowThatMeetsTheBudgetClosesTheWindowAndOpensTheNext` (heavy) | LAWFUL | d7 L4 "the offset budget is reported as `kRowLimit`. A row that meets the budget opens the next window rather than being lost". Real size, no hook (:595 (2)). |
| control `TwoHalfBudgetRowsFitAloneButNotTogether` (heavy) | LAWFUL (premise control) | This is d2's `BatchDecoder` refusal for the budget, already tested by `BatchDecoderTest.CapacityExceededLeavesBuildersUntouched`. It is a premise, not proof of the rung. Brief §How: "d2's public `Codec`/`BatchDecoder` only build bytes or check a premise". |

**Law (b), public API:** cases 1-4 are driven only through `PublisherArrow::Publish`/`PublishDirect` (public, `publisher_arrow.hpp:43-49`) and the batched `SubscriberArrow::Subscribe`/`Unsubscribe`. They observe results through the callback, `Attachments::Find`/`KeyAt` and Arrow arrays. No d7 internal is called or observed. This meets rung 1's declared exception. The control calls d2's public class, not d7's.

**Tier-1 disagreements:** none. d10 §8 r6 says "a small-size offset-budget split", but :595 (2) revises it to heavy at about 128 MiB. That is a recorded ruling revision, not a conflict.

**Scheduled restoration:** none. The heavy case's subject is d7 L4's budget, not the C# window ceiling, which d10 L4 forbids (T38, deleted at rung 7 as 0a found).

**Laws [built] with no test:** this stage closes the 0a gaps (L10 throw, L4 "opens the next window", L3 empty key). d7's Tests lines remain stale until its next round (:595 (1)).

**Note, not a finding:** case 4's `ASSERT_EQ(recs.size(), 1u)` is checked when the first delivery wakes the waiter. It is one-sided and lawful.
