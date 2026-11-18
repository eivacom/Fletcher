# D7F — Batch assembly fixes — Plan and tracker

**Purpose (owner's terms):** every batch a subscriber receives arrives one at a time and in order,
and no lost row ever goes unreported, so that rows a caller publishes in sequence reach the consumer
in sequence. This is a prerequisite for RIX, where "it is the caller's responsibility that the rows
are indexed compactly and in sequence" and Surge refuses any batch that does not continue exactly
where the last one ended.

- **Domain:** 7, Batch assembly ([spec](../docs/domains/d7-batch-assembly.md)), within one existing domain.
- **Locked decisions:** [D7F-locked-decisions.md](D7F-locked-decisions.md).
- **Owner rulings:** [RIX-rulings.md](RIX-rulings.md). The ledger is shared with RIX.
- **Test ladder:** [D7F-test-ladder.md](D7F-test-ladder.md). The stages below are generated from it,
  one stage per rung except stage 3, which carries rungs 3–6 for the reasons the ladder gives.
- **Pre-round audit and census:** [D7F-pre-round-audit.md](D7F-pre-round-audit.md).

This file is both the `plan_path` (the tracker) and the `user_stories_path`.

## Tracker

Status: ⚪ not started · 🔴 in progress · 🟢 done (forcing test green, reviewed, logged).

| Item | Subject (what becomes true) | Inherits | Est. net prod. lines | Status |
|---|---|---|---|---|
| D7F-1 | Rows that reach a subscription with no usable schema are reported when it closes: one null `kClosing` batch whose `rows_dropped` equals the rows sent. The header and README state D-D7F-3's wording. | The existing pipeline; census C2 (one count entry), C4 (delete the dead branch `cpp:69-71`) | 30–50 | 🟢 |
| D7F-2 | The same loss is reported at each deadline, without waiting for a schema; a schema that arrives later gives normal batches, and rows plus drops still add up across the change. | D7F-1's null-batch flush and single count entry; census C3 (retire `ready_` as a reporting gate) | 0–20, likely negative | 🟢 |
| D7F-3 | Every batch callback runs on the subscription's batch thread, one at a time, in window order; `Publish` never runs or waits on one. Closing from another thread delivers every queued window, then `kClosing`, before it returns. The over-budget row around an in-callback `Unsubscribe` is counted (test labelled `heavy`). `Unsubscribe` from inside a callback never delivers a nested batch: queued windows, then `kClosing`, follow once the callback returns. | D7F-1 and D7F-2; census C1 (single delivery site), C2 (delete the duplicate retry block `cpp:96-104`), C5 (lock order); the re-anchoring of the 10 tests listed in the ladder, rung 3 | 40–80 | 🟢 |

Each item's forcing tests are named by the architect at design, from the subjects above and the
ladder's external paths. The ladder's guards (no deadlock, conservation property, states × endings,
order stress) belong to the stages it names.

## Items

### D7F-1 — Loss before a schema is reported at close

**User story:** as a subscriber to a topic whose schema never arrives or cannot be read, I am told
how many rows I lost when I unsubscribe, instead of the count vanishing.
**Acceptance:** ladder rung 1; L1 (violation 1), L6, L7, L9; D-D7F-3.

### D7F-2 — Loss before a schema is reported at each deadline

**User story:** as a subscriber, I learn about rows lost before the schema arrives while the
subscription is still open, at the same cadence as any other window.
**Acceptance:** ladder rung 2; L1 (violation 1), L5, L6, L13.

### D7F-3 — One batch thread, in order, nothing lost at close

**User story:** as a subscriber, my callback is never run twice at once or out of order, never runs
on the transport's thread, and every row published before I unsubscribe is either delivered or
counted, whether I unsubscribe from inside my callback or from elsewhere.
**Acceptance:** ladder rungs 3–6; L1 (violation 2), L2, L4, L9 as revised, L11, L12; D-D7F-2,
D-D7F-4.

## Definition of done (round)

- All three items 🟢.
- The default run of `pubsub-arrow` tests and `ctest -L heavy` are both green.
- The `pubsub-arrow-fastdds` integration suite is green.
- Domain 7's L1, L9 and L12 build-state marks read `[built]`, with the tests that prove them.
- §4 "Who calls, and on which thread" is rewritten for D10.
