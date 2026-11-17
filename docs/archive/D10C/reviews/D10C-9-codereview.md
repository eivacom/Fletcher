# D10C-9 — step 4b code review (light lane)

Base `86eef85`, working tree. Ledger read in full: `plans/RIX-rulings.md` **667 lines, 52 `## ` entries**, last "## 2026-11-16 — D10C-7: the switch and the deletion are one stage"; :536 (3) and :575 (4) applied. **Blocking: 0 · should-fix: 2 · nit: 4.**

## Findings
**S1 — should-fix (high). The BC-8 scan misses the likeliest regression and over-reaches elsewhere.** `SubscribeBatched` already copies the options into locals (`SubscriberArrow.cs:190-191`: `long maxRows = options.MaxRows; long timeoutMs = ...`). A re-added `if (maxRows < 1)`, `if (timeoutMs < 0)` or `options.MaxRows is < 1` passes `Compared` (`BatchOptionsTests.cs:295`), because the pattern needs `.MaxRows`/`.Timeout` right next to the operator. Meanwhile the tree-wide `.Timeout` arm trips on a lawful arrival-side `x.Timeout <` and on `new X<T>().Timeout`, as audit 0b noted. The narrowing is cheap and fixes both problems. Use the existing `BodyOf` to restrict the relational and clamp check to `SubscribeBatched`'s body and the `BatchOptions` class. Match operators by operand rather than by name (`\s[<>]=?\s|\bis\s+(not\s+)?[<>]`). The body's `delegate*...<nint, ...>` has no spaces around its brackets, so it does not trip. Keep the `Math.Clamp/Min/Max` arm over the whole tree for `.MaxRows` only, since that name is unique to `BatchOptions`. Add `if (maxRows < 1)` as a pattern control.

**S2 — should-fix (medium). About 75 of the 489 lines are padding.** (a) `Delivery`/`Sink` (`:432-488`) copy `SubscriberArrowTests`' own sink line for line. Make that one internal, as was already done for `ServerLeg`. (b) `FletcherCode` (`:278-290`) copies `ManagedBatcherGoneTests`' folder lookup (`:103-121`). Extract it next to `CodeLines`. (c) The grid's edge leg publishes to 42 topics, sleeps 300 ms and asserts empty sinks (`:167-170`, `:184-187`). That checks nothing: every subscribe threw, so no handler was ever registered. Keep the NotSupported check and drop the rest. What remains of the overrun (scan body and call controls about 95 lines, rung-9 harness about 128) is the brief's own underestimate, not padding.

**Verified sound (no finding):**
- Grid literals: `MinValue` floors to −922337203685478, `MaxValue` truncates to 922337203685477, 100 years is 3155760000000.
- Accepted cells: {1, 2, long.Max} × {0, 1 ms, ∞, 100 y, Max} = 15, and 27 are refused.
- Each refused cell's message is built per cell from literals. Each cell also checks the exact type, `Seam`, and that the topic is free again. That probe is real under :575 (6), because it uses the same `SubscriberArrow`.
- The edge-leg NotSupported on out-of-range cells is valid outside proof that C# checks nothing.
- Rung 9 asserts the order, the row values, an empty `Closing`, and nothing after it.
- The 200 ms hardening is sound. With `MaxRows = 2`, windows [2,3] and [4,5] fill at publish time, so only window [1] depends on the deadline.
- `Dispose` sets `_disposed` before the native destroy (`SubscriberArrow.cs:391`), so the `finally`'s second `Dispose` cannot hang the body.

**Nits**
- N1: T31's 200 ms quiet window (`SubscriberArrowTests.cs:~1765`) has no control. Its server arm is fully covered by the grid cells {2, long.Max} × {100 y, `MaxValue`}, which run 2 s with a 10 ms control. Either name that in its remarks or add a sibling 10 ms window.
- N2: `TheDisposeReturnedFlagCanFail` is a harness tautology (`Wait(200 ms)` against `Sleep(1500)`). In a real self-wait the body fails, but fixture teardown (`_provider.Dispose` with the batch thread stuck in destroy) may still hang, so do not claim "never hangs".
- N3: BC-14's `Conversion` misses `TotalSeconds * 1000`-style conversions. Add `\.TotalSeconds\b`.
- N4: `BodyOf` counts braces inside string and char literals. That is harmless today.

## Conformance
It matches the brief. Production is −3 (the brief says −2; the extra line is a blank line): `MaxTimeout` is gone, and no other public change. There are two cases on each of rungs 9 and 10 with their controls, two scans with pattern controls, and T31 re-anchored as two cases on both legs. The docs cover d10 L3, L11, L12, L14, §4 and the BC-8 and BC-14 marks. There is no change to d7 or d9. Nothing the brief said would go survives: the `MaxTimeout` mentions left in d10 are historical. Beyond the brief, `ServerLeg`, `EdgeShimText` and `ServerPackage` became internal, which is test-only. The d10 marks match the code and the test names.

## RECORD
- d10 L11 "Holds at `SA:428-434` (`SubscriberArrow.Dispose`)": `Dispose` is now `SA:376-393`. This went stale at D10C-7, and this stage edits L11.
- Tests net about +625 against the brief's +230 to +300 (about 2x). The evidence row and the As-landed line do not say so.
- Audit 0b cites the scan hits at `SubscriberArrow.cs:193-194`. They are now `:190-191`, shifted by the deletion.
