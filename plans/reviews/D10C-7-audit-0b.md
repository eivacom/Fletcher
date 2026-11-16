# D10C-7 — step 0b test-authority audit (new-test), rungs 7+8

**Verdict: CLEAR.** No FORBIDDEN and no FORBIDDEN-BY-TOLERANCE test. Two UNMOORED items (U1, U2); neither blocks building.
No tier-1 disagreement. Base `4542e78` (uncommitted tests plus untracked `ManagedBatcherGoneTests.cs`). **Proof of read:**
`plans/RIX-rulings.md` top to bottom: **667 lines, 52 `## ` entries**, last "## 2026-11-16 — D10C-7: the switch and the deletion
are one stage" (:660). Yardstick: d10 L3, L4, L6, L8, L9, L13, L14 (V1-V4, V9-V12), D24/D29/D32, §8 rule 6; d7 L6, L9, L12;
rulings :140, :484, :500, :536, :551, :575, :605, :650, :660.

**Scope** (SAT = `SubscriberArrowTests.cs`): the D24 probe `ArrowCDataInterfaceTests.ADictionaryColumnCrossesAsADictionary`; the SAT helpers
`EdgeRefuses`/`ServerLeg`/`EdgeShimText`/`ValueAt`; 21 re-anchors, T31 and D10C-6's three; the rewrites T21/T25/T36; the new edge-refusal, idle-close (x3),
one-thread, T32/T41 twins and 10 ms control tests; `AKeptBatchReadsIntactAfterClosing`; `AQuietCloseKeepsTheTopicForItsKind` (x2);
both `ManagedBatcherGoneTests` scans; the Fast DDS `ASubscriberArrowForwardsItsReaderProfileOnBothForms` with `RefusedOnTheEdge`.

## Law (a): all LAWFUL
- **:575 (2)**: `EdgeRefuses` asserts NotSupported/Seam, `StartsWith` the shim text, then exactly one '.'-terminated sentence naming
  `Eiva.Fletcher.Server`, and no handler call. Exact. `EdgeShimText` matches `c-abi/src/binding.cpp:233-236` plus the entry name.
  L3's "verbatim" makes the copy the subject. T7's server arm refuses the edge text and the package on a provider refusal ("the only such
  sentence is the edge refusal's"). The Fast DDS `RefusedOnTheEdge` is weaker (it checks `Contains` the package, not "one sentence"), but lawful.
- **:551 (2) / :575 (5)**: T21, T36 and the probe assert `DictionaryArray` with its nulls and values. `ValueAt` reads through the indices
  and converts nothing. The index width is left to d7. The per-row value-type test (D32) is untouched.
- **:484 / d7 L6, L9**: the idle close expects one zero-row Closing per closer (the topic is declared, so a decoder exists: "zero-row, or null
  if there is no decoder"), then nothing. T25 expects RowLimit, then an empty Closing, then nothing. T26 ignores its Closing. The kept-batch test's Closing carries
  the pending 5th row ("the pending window").
- **L13 / d7 L12 / :551 (4)**: the one-thread test expects one thread, not the publisher's or the closer's, windows 0/2/4 in order, and an empty Closing.
  "Publish never waits on a handler" is :140's accepted cost. No test asserts overlap.
- **:536 (3), V7-V8**: no cap or `ArgumentOutOfRange` assertion remains. T31 uses `MaxTimeout` only as a value ("waits"). It is
  LEGITIMATE-BUT-ADJACENT until rung 10 removes the constant. **V4/V11**: T15/T16 and `ASchemaTheCodecCannotOpen…` expect a null batch with rows counted.
- **FWD-1c**: refusing the other kind after an in-row-delivery close serves :575 (6) and L9 ("nothing … lost … silently"), which was R-6's own
  trigger. A same-kind re-subscribe delivers. Disclosed in the brief (Interfaces row 4).

## Law (b): driven from outside
The leg comes from the server file beside the tests (P5), never from `NativeMethods`. All SAT subjects go through the public API.
**The scans are inspections, and they are legitimate as rung 8's forcing proof.** Ruling :660 names "rung 8's forcing scans", and d10 L4 proposes the
source scan. Dead code behaves the same as none, so no behavioural test can see "gone". L4's behavioural half is the one-thread test.
Both scans have pattern controls and input controls. **`FLETCHER_PACKED_ASSEMBLIES`**: never skipped (a named folder that is absent fails),
but when unset it silently reads the build output. Nothing in `.github` sets it yet, so the "packed" half is proven only by a CI run that
sets it. Close must cite that run.

## Deletions
T20, T30, T34, T38, T39 and T40 are gone, with no replacement. `RowBytes` and `windowByteCeiling` are gone too. Nothing left asserts their subjects. **Ruled restoration
text still in the tree:** `d10:181-182` (T30 "to retire or re-anchor") and `d10:378` (V12 cites T20). Ruling :650 (1) amends both,
and the design's Files-to-touch lists them. They must land in this stage's commit set. GatewayClient's ceiling test is domain 5's batcher and out of scope.

## Implementer flags
1. **The quiet-close per-row arm expects the claim refusal on edge.** Tier 1 does not order the two refusals; :551 (1) and :575 (6) each hold
   for its own case. **U2: the ordering is UNMOORED (tier 3)**. D2/FWD-1b set it, and D10C-6's row-first arm already uses the same order.
   The alternative, asking the variant first, is what L6 forbids. Keep it. (2) The edge text is a correct copy. (3) The env var: see (b).
4. **The guards are red today, and the label is wrong.** `AKeptBatchReadsIntactAfterClosing` is red only through its edge truth-table arm,
   which is D2's forcing arm; its server arm is a real guard. `AQuietCloseKeepsTheTopicForItsKind` is red on both legs (the claim is released
   today), so it is D5's **forcing** test. Relabel both docs. Neither asserts anything forbidden.

## U1: the `new Thread(` arm goes past L4's text (stop-and-ask, or narrow)
L4 forbids a "**batch thread** of its own". L13's "adds no thread" sits inside the batch-delivery law. The scan refuses any `new Thread(`
anywhere in `dotnet/src/Fletcher`, which is a binding-wide prohibition that no ruling states. It forces nothing: there are 0 hits today, and the
batcher used a `Timer`. It also misses `Task.Run`/`ThreadPool`, which are used lawfully at `Subscriber.cs:653,778` and `SchemaArrival.cs:174`.
Choose one: drop the arm; keep it labelled "a proxy for L4's batch thread: a hit needs a ruling"; or ask the owner. The
`ArrayBufferWriter<byte> _field` pattern maps to L4's "window" (its one hit today is `SA:604`, `Batcher`), so it stays.

**Laws vs build marks:** new tests exist for L3 (D29), L6, L8, L13 and L14 (V1-V4, V11, V12), and the scans cover L4. Each stays `[designed-not-built]` until green.
L12's batch site, L9's hand-over and L11's served batch-callback dispose are for rungs 9-11. No false green.
