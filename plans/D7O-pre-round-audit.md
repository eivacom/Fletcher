# D7O pre-round audit — consistency (Gate 4) and bounded-context census (Gate 4b)

Written by `round-designer` on 2026-11-12, after the owner rulings of that day were recorded
([RIX-rulings.md](RIX-rulings.md), the 2026-11-12 entries) and domain 7 was amended to match them
(L4, L5, new L15, D14–D16, Q5 resolved). Tree: `ff5be3e` plus those document edits.

**Corpus audited:** `docs/domains/d7-batch-assembly.md` (as amended); the batched-subscriber passages
of `pubsub-arrow/include/fletcher/pubsub_arrow/subscriber_arrow.hpp` and `pubsub-arrow/README.md`;
the 2026-11-12 rulings and the earlier rulings they touch (BIND D-BIND-25, D-BIND-59, D-BIND-64,
Decision 6); `.claude/runbook.D7F.config.md` (closed round, for carry-over constraints); the test
corpus that exercises `BatchOptions`.

**Totals:** 28 rows: 20 OK, 8 FINDING (F1–F8), 0 BLOCKER, plus 1 owner confirmation (LC-1). No law-vs-law
conflict. One implicit ruling collision was found and resolved by the owner the same day (R-3).

## A. Clauses against the laws

| # | Clause (source) | Governing law | Verdict |
|---|---|---|---|
| 1 | §1 Owns: "deciding whether a subscription's `BatchOptions` are acceptable" (d7, amended) | L15 | OK |
| 2 | §1 Refuses: "Validating `BatchOptions`" (d7, struck through) | L15 | OK: withdrawn by D15, kept visible as the record |
| 3 | §2 Deadline: "`max(now + 1 ms, last_flush + timeout)`" (d7) | L5 | **FINDING** F1: silent on "no timeout" and on an unrepresentable sum. Suggest: "for a subscription with a timeout; see L5 for none or too long". Apply at D7O close with the owner's approval. |
| 4 | §2 Flush reason: `kRowLimit`, `kTimeout`, `kClosing` (d7) | L4 | OK: no new reason; a no-timeout window simply never gets `kTimeout` |
| 5 | L4 (amended) | L4/L5 | OK |
| 6 | L5 (amended) | L5 | OK; the "too long for the clock" sentence is LC-1 below |
| 7 | L1 "counted in the `rows_dropped` of a later delivered batch" with no timeout | L1, L5, L6 | **FINDING** F2: holds, but a window of only lost rows (no schema yet, malformed rows; drops do not count toward `max_rows`) is reported **only at closing** when there is no timeout. This is the ruled behaviour, not a violation; disclose it in the header and README, and the ladder asserts it. |
| 8 | L6 "a window that contains only drops is still delivered" | L6 | OK: still delivered, at closing (F2) |
| 9 | L13 "Subscribing never waits for the schema" vs L15's refusal | L13, L15 | OK: the refusal depends only on the options |
| 10 | L15 "before anything is subscribed: no thread …" vs `cpp:371-372` (batcher constructed and started before subscribing; `StopOnThrow`) | L15, L11 | OK as a law; tier 3 must comply: the check precedes `Start`. Design note for the architect. |
| 11 | L15 "every value it accepts is used as given" vs `cpp:36` clamp | L15 | OK as a law; tier 3 must comply (the clamp is deleted) |
| 12 | §4 "Applications call `Subscribe` … from any thread, including from inside the batch callback" | L15 | OK: a refusal from inside a callback throws to that callback's code like any other; no new case |
| 13 | §4 `BatchOptions` surface (amended: "how 'no timeout' is spelled is D7O's design") | L5 | OK |
| 14 | §8 rule 1 "Name the bucket" | L2 | OK: no new loss reason |
| 15 | Q4 (`kCapacity`) | L4 | OK: untouched |
| 16 | hpp:76-79 `BatchOptions` member comments ("…or after this long") | L5, L15 | **FINDING** F3: silent on no timeout and on the refusal. D7O rewrites them. |
| 17 | hpp:95-102 batched `Subscribe` doc ("at the window's deadline — `options.timeout` after the previous flush …") | L5, L15 | **FINDING** F4: add the no-timeout case, F2's consequence, and "throws `PubSubError(kInvalidArgument)` for `max_rows < 1` or a negative timeout". |
| 18 | hpp convenience overload "(8000 rows, 1 min)" | L15 | OK: defaults are in range |
| 19 | README:63-70 "a row-count limit is reached **or** the window's deadline passes …" | L5 | **FINDING** F5: as F4. |
| 20 | README:85 example `{.max_rows = 4096, .timeout = std::chrono::seconds(5)}` | L15 | OK |
| 21 | README:133-135 offset budget reported as `kRowLimit` | L4 | OK |
| 22 | `.claude/runbook.D7F.config.md` stop-and-ask "Any change to `BatchOptions` … (D-D7F-5)" | — | OK: D-D7F-5 was scoped to the closed round D7F; D7O's change is ruled (D14, D15) |

## B. Laws against rulings, and rulings against each other (4b, 4c)

| # | Pair | Verdict |
|---|---|---|
| R-1 | 2026-11-12 "Add 'never'" text: "C# keeps its current behaviour" vs "Refuse both": "C# callers passing MaxRows < 1 now get an exception" | OK: different subjects (timeout vs row count); the later names its C# consequence explicitly |
| R-2 | 2026-11-12 "one batcher, C++'s" (D16) vs D-BIND-25 ("Arrow tier managed … batch-first") | **FINDING** F6: an explicit supersession of D-BIND-25's batcher half (the owner was shown D-BIND-25 when ruling). The text of D-BIND-25 is annotated by the C# switch round, not D7O (domain 10's ruling). |
| R-3 | "keep it compliant until removed" vs "no interim fixes" (both 2026-11-12) | Implicit collision, put to the owner: **revised in place**, "No fixes; don't ship it first". Recorded in the ledger. |
| R-4 | "one shim, two variants" vs BIND Decision 6 (shim without Arrow C++, for package size) | **FINDING** F7: the owner was told the cost (C# desktop carries Arrow C++) when ruling, so this is a partial supersession, scoped to the server variant. Annotated by the shim round (domain 9). |
| R-5 | D-BIND-64 (concurrent, out-of-order C# handlers "as in C++") vs domain 7 L12 / L9 | Already false of C++ since D7F; the C# copy stays non-compliant by ruling R-3 until deleted. Annotated by the C# switch round. |
| R-6 | L15 vs C#'s `BatchOptions.MaxTimeout` (refuses a finite timeout above ~49.7 days) | **FINDING** F8: once C# runs on domain 7, that refusal is a second answer to BC-8 that domain 7 does not give. Owed by the C# switch round: drop the cap (a loosening) or bring it to the owner. Not D7O's. |

**LC-1 (owner confirmation owed).** L5's sentence "a deadline is never earlier than its timeout says:
a timeout too long for the clock to represent means no deadline, never an early or wrapped one" was
written by round-designer, not quoted from a ruling. It follows from L15 ("every value it accepts is
used as given": `milliseconds::max()` is not negative, so it is accepted and must mean something) and
fixes today's overflow at `cpp:187`. **Confirmed by the owner 2026-11-12, "Keep it".**

## C. Test corpus (Gate 4 step 6)

Searched for tests that pass `max_rows` ≤ 0, a zero or negative timeout, or assert a clamp, in C++
and C# (`git grep` over `*test*`, 2026-11-12): **none in domain 7's suite.** The C# tests that pass
`Timeout.InfiniteTimeSpan` (`dotnet/tests/Fletcher.GatewayClient.Tests/BatchedSubscriptionTests.cs`,
`Fletcher.AotSmoke`) are lawful under D14 and belong to other domains. No test is deleted or
re-anchored.

## D. Bounded-context census (Gate 4b)

Searched by operation: range checks and clamps on a batch row count or timeout; deadline arithmetic
on a batch window; anything that turns rows into windows. Places found: `subscriber_arrow.cpp:36`,
`:187`; `dotnet/src/Fletcher/SubscriberArrow.cs:228-237`, `:512-523`;
`dotnet/src/Fletcher.GatewayClient/GatewayBatch.cs` (options check, timer, window cut). The C++
gateway server and the C ABI use no batched subscribe.

| Context | Disposition | Site / copies | Structural test | Register |
|---|---|---|---|---|
| Are these batch options acceptable? | **DEFINE** (domain 7) | Batched `SubscriberArrow::Subscribe`, before anything starts. Copies: the C++ ctor clamp, deleted by D7O; C#'s checks, deleted by the C# switch round; the gateway client's, BC-9 | Sweep over option values: refused ⇔ out of range; a refused call leaves nothing behind | BC-8 (new) |
| When does a window close, and why? | **EXTEND** | `RecordBatchBatcher`: `ArmTimer` and the batch-thread loop (no-deadline case, overflow) | The existing `FlushesAt*` tests plus the ladder's no-timeout rung | BC-4 (unchanged single site) |
| Is a row delivered, dropped or neither? | **REUSE** | `TryAppend` (BC-2) | `RowsConservedAtEveryEnding` | BC-2 |
| Batch assembly outside domain 7 | **DEFER-BUT-REGISTER** | C# `Batcher` (planned deletion), C# `GatewayBatcher` (no plan) | none | BC-9 (new) |
