# D9S-4 — Stage Brief (2026-11-13)

**Lane:** full loop. The design is in `plans/D9S-4-batches-cross.md`.

**In one sentence:** on the server build of the shim, a C program (and later C#) can subscribe to batches and receive every batch Fletcher's one batcher builds: Arrow columns it reads with its own Arrow, the reason the batch closed, a lost-row count and each row's attachments, one at a time and in order. The edge build answers the same call with "not supported in this build" and stays Arrow-free.
**Forcing tests:**
- `Batches.EachClosedWindowCrossesAsAnImportableBatchInWindowOrder`: 5 rows give 2 full batches and 1 timed-out batch, each equal to what was published, attachments included, all on one thread and in order.
- `Batches.AWindowThatCouldNotBeBuiltArrivesAsNoBatchWithItsRowsCounted`: rows with no usable schema arrive as "no batch", every row counted as lost.
- `Batches.TheBuildAnswersABatchedSubscribeAsItAnswersTheQuery`: the edge and test builds refuse, write nothing and deliver nothing; the server build serves.

## Interfaces
| Surface | Change | Why |
|---|---|---|
| Batched subscribe: one call, exported by both builds | NEW | the server serves it; the edge refuses it with not-supported (L12, L14, L15) |
| The batch a callback receives (3 C types) | NEW | Q6; it reuses the row path's attachments |
| Server build's dependencies; CI c-abi lane | CHANGED | Arrow, through the batcher only; the edge build is unchanged (L8) |

## Deleted
Nothing. Nothing in the shim served batches before. The C# batcher this makes redundant belongs to the C# switch round.

## Corner cases forbidden vs handled
**Forbidden:**
- Arrow in the edge build: only the server's recipe and build files name it, and the existing CI check proves it.
- An edge build that half-serves the call: it refuses before reading any argument. Nothing is created, written or called back.
- A batch that is unsafe to keep: the program takes it over by the standard Arrow handover, or the shim frees it once, when the callback returns.
- The shim choosing batch sizes, timeouts or limits: options go to the batcher as given, and its refusal comes back word for word. A source scan proves the shim has no thread, timer or clock.
- Attachments out of row order: there is one set per row, an empty set for a row without any.
- A subscription number on a batch: the first batch can arrive before subscribe returns, so programs match batches to subscriptions by the context they registered.

**Handled:** per-row and batched subscription numbers repeat on one subscriber (Decision 1); a failed conversion is lost silently until stage 6 (Decision 2); a subscribe refused for bad options leaves a hidden, empty host behind, because only the batcher judges options and it is asked through that host.

**Red before the change:** the tests do not compile. Against an edge build without the refusal, the edge arm sees success.

## Decisions for you
1. **Should one subscriber object hold both row-by-row and batched subscriptions?**
   (a) One object. *Users:* nothing new to manage, and destroying the subscriber ends both kinds; but a batched number can equal a row number, so a number passed to the wrong close cancels the wrong subscription, or nothing. *Architecture:* the server build only adds (L13), one new call in both builds (L12).
   (b) A separate batch-subscriber object. *Users:* numbers never mix, but there are two kinds of subscriber to manage. *Architecture:* about four more calls every build must carry and the edge must refuse (L12), over the surface budget.
   **Recommendation:** (a). It serves the round purpose (batches through the one batcher, edge unchanged), and a wrong number is a caller error, as today. **Default:** (a).
2. **Until stage 6, what happens to a batch the shim fails to convert (e.g. out of memory)?**
   (a) Accept the gap in stages 4–5: those rows vanish uncounted. *Users:* no one is exposed; nothing ships mid-round, and C# does not call this before a later round. *Architecture:* L18 holds at round close and keeps its red-first test in stage 6.
   (b) Add the report now, without its test. *Users:* the same end behaviour, earlier. *Architecture:* the code lands two stages before its proof, so stage 6 cannot show it red first.
   **Recommendation:** (a). The ruling "Report it as lost" is met by the round, and each law keeps one test. **Default:** (a).

## Risks accepted / debt carried
- **Public surface 4 vs a budget of 3:** one call and three types. The fourth is the reason's C spelling; a bare number is the cut, which I do not recommend. Reviewer to rule. Stage 5 adds the close, already designed.
- The server tests wait one-sided, with a timeout of at least 1 s. On a badly loaded runner only the reason assertion can flake.
- Dictionary columns arrive in the batcher's form (d9 Q11, open), so the tests avoid them.
- **Premises (stop and ask if false):** Arrow's symbols stay hidden in the server build; `INT64_MAX` ms is exactly "no timeout"; the batcher's refusal keeps its error type. Evidence: Windows by hand. Linux and CI wait for the owner's draft PR.

## Numbers
Declared net lines (raw diff): about **+650 / −35**. Production +262/−8 in 9 files, of which about 209 count toward the 250 ceiling (`binding.h` doc comments are not counted); no 4a/4b split. Tests about +330; docs about +60/−25. New public surface: 4. Design cycles used: 1/2.

---
*As landed (<date>, appended by the PM at close, ≤5 lines):*
<delta vs the above — actual net lines, anything retired or added the brief did not predict, fix cycles used>.
