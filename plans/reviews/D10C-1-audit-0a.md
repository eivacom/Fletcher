# D10C-1 — test-authority audit, step 0a (corpus)

**Verdict: MUST-DELETE(1), not blocking D10C-1.** The one forbidden test (T38) is in scope only as the source
of a port. Tier 1 decides when it is deleted: after rung 1 lands green, at rung 7 as the ladder already
schedules it. Deleting it before the port would break d10 L4 ("deleted only after its tests are checked
for cases domain 7's suite lacks") and ruling :203 ("I don't want to just delete the C# batcher if it
contains functionality that should be ported over to the C++ one first").

**Proof of read:** `plans/RIX-rulings.md` read in full, **603 lines**. Last entry (:595): "## 2026-11-15 — D10C
ladder: the domain-7 port tests". d10 §3 L1-L15, §7 Q9 and §8 rule 6; d7 §3 L1-L15; d2 reserved-key rows (:51, :281).

**Scope (tree `8674421`):**
- `pubsub-arrow/tests/test_pubsub_arrow.cpp`: 58 `TEST`s, of which 40 are `SubscriberArrowBatchTest` (:524-:3458).
- `test_pubsub_arrow_heavy.cpp`: 2 tests labelled `heavy`, :52 and :65.
- The `NodiscardTest` probe (`discard_probe.cpp`). It is a compile check, not a batching test.
- `dotnet/tests/Fletcher.Tests/SubscriberArrowTests.cs`: the four Q9 sources, T28 :801, T32 :888, T38 :1132 and T41 :1191.
- No `DISABLED_`, `GTEST_SKIP`, `Skip` or `[Ignore]` anywhere in scope.

## Classification

| Test | Class | Action | Authority |
|---|---|---|---|
| `SubscriberArrowTests.ARowThatWouldPassTheCeilingFlushesTheWindowFirst` (:1132) | **FORBIDDEN** | **DELETE** at rung 7, not before rung 1 is green | d10 L4: "The binding holds no window, row count, deadline, timer or batch thread of its own". It builds `SubscriberArrow(_provider, windowByteCeiling: …)`, which is the C# ceiling (V9, D-BIND-63). |
| `RowsSplitAcrossMessagesAreDroppedNotMisaligned` (:801) | LEGITIMATE-BUT-ADJACENT | RE-ANCHOR (rung 7, as the pre-round audit says) | It runs on the managed batcher, but its subject is lawful under d7 L1-L3. It also reads straight after `Publish`, so it must wait. |
| `ARowWithAnEmptyAttachmentKeyArrives` (:888) | LEGITIMATE-BUT-ADJACENT | RE-ANCHOR (rung 3, per-row) | d7 L3. An empty key is not reserved: d2 :51 defines a reserved key as "An attachment key whose first byte is `0x01`". |
| `AHandlerThatThrowsIsAbsorbedCountedAndRaised` (:1191) | LEGITIMATE-BUT-ADJACENT | RE-ANCHOR (rung 3) | d10 L9: "Every managed handler failure is caught at its thunk, counted and raised". |
| 40 `SubscriberArrowBatchTest` + 2 `heavy` + the other 18 in `test_pubsub_arrow.cpp` | LAWFUL | none | They assert d7 L1-L13 and L15 as built. Spot-checked: `UnsubscribeFromCallbackDuringRowLimitFlushStopsDeliveryCleanly` expects exactly one closing delivery (L9, D18). The heavy pair follows ruling :140 ("It runs only when the heavy label is requested"). |

**Scheduled restoration: none.**
- **The port is not a restoration of T38.** What T38 tests is the C# ceiling, which d10 L4 forbids. Rung 1's case 4 proves a different subject: d7 L4's own clause, "A row that meets the budget opens the next window rather than being lost", on d7's offset budget, with no C# window. Ruling :595 authorises that port: (1) "test files only" and (2) "a `heavy`-labelled test (~128 MiB rows); no test hook in domain 7's code".
- **No C# row restores T38.** No ladder row, comment or manifest entry re-points, re-anchors or restores it in C#. The ladder (rung 7) and d10 §8 rule 6 both say DELETE.
- **The other three ports** (cases 1-3) carry lawful d7 subjects: L1/L2, L3 and L10.

**Tier-1 disagreements: none.**
- :595 (1) revises d10 §8's "by a domain-7 round" (a revision recorded in the ledger), and d7's laws are untouched.
- d7 L10 ("A throw from the callback is swallowed") agrees with d10 V13 and L9: C# keeps its own count.

**Laws `[built]` with no test, which rung 1 closes:**
- **d7 L10:** its own Tests line says "A throwing callback has no test". Case 3 adds one.
- **d7 L4, "opens the next window":** the only test is the heavy `OverBudgetRowAroundInCallbackUnsubscribeIsCounted`, which shows the early close. Its over-budget row does not fit even alone, so landing in the next window is never asserted. Case 4 asserts it.
- **d7 L3:** no test with an empty attachment key. Case 2 adds one.

d7 L14 is `[designed-not-built]` and has no test, which is correct. No false greens. No UNMOORED tests.

**Ledger note (not a finding):** the ladder's "read in full: 593 lines" was written before :595 was appended; the ledger is now 603 lines.
