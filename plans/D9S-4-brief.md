# D9S-4 — Stage Brief (2026-11-13)

**Lane:** full loop, design cycle 2. The design is in `plans/D9S-4-batches-cross.md`. **The stage splits:** 4a ships everything except attachments; 4b, a small stage, adds them.

**In one sentence:** on the server build of the shim, a C program (and later C#) can subscribe to batches and receive every batch Fletcher's one batcher builds: Arrow columns it reads with its own Arrow, the reason the batch closed and a lost-row count, one at a time and in order (each row's attachments arrive in 4b). The edge build answers the same call with "not supported in this build" and stays Arrow-free.
**Forcing tests:**
- `Batches.EachClosedWindowCrossesAsAnImportableBatchInWindowOrder` (4a): 5 rows give 2 full batches and 1 timed-out batch, each equal to what was published, all on one thread and in order.
- `Batches.EachBatchRowCarriesItsOwnAttachments` (4b): each row's attachments arrive beside that row.
- `Batches.AWindowThatCouldNotBeBuiltArrivesAsNoBatchWithItsRowsCounted`: rows with no usable schema arrive as "no batch", every row counted as lost.
- `Batches.TheBuildAnswersABatchedSubscribeAsItAnswersTheQuery`: the edge and test builds refuse, write nothing and deliver nothing; the server build serves.

## Interfaces
| Surface | Change | Why |
|---|---|---|
| Batched subscribe: one call, exported by both builds | NEW | the server serves it; the edge refuses it with not-supported (L12, L14, L15) |
| The batch a callback receives, and the callback (2 C types; the reason is named numbers) | NEW | Q6; 4b reuses the row path's attachments |
| Server build's dependencies; CI c-abi lane | CHANGED | Arrow, through the batcher only; the edge build is unchanged (L8) |
| Two Fletcher package recipes gain the Linux "position-independent" switch | CHANGED | the server shim cannot link on Linux without it. **Pending your ruling, asked by the PM.** |

## Deleted
Nothing. Nothing in the shim served batches before. The C# batcher this makes redundant belongs to the C# switch round.

## Corner cases forbidden vs handled
**Forbidden:**
- Arrow in the edge build: only the server's recipe and build files name it, and the existing CI check proves it.
- An edge build that half-serves the call: it refuses before reading any argument. Nothing is created, written or called back.
- A batch that is unsafe to keep: the program takes it over by the standard Arrow handover, or the shim frees it once, when the callback returns.
- The shim choosing batch sizes, timeouts or limits: options go to the batcher as given, and its refusal comes back word for word. A source scan proves the shim has no thread, timer or clock.
- Attachments out of row order (4b): there is one set per row, an empty set for a row without any.
- A row and a batched subscription sharing a number: batched numbers carry a mark row numbers never have, and the batched close (stage 5) refuses an unmarked number. A batch carries no number, because it may arrive before subscribe returns; programs match batches to subscriptions by the context they registered.

**Handled:** a row and a batched subscription to the same topic through one provider collide (Decision 1); a failed conversion is lost silently until stage 6 (Decision 2); a subscribe refused for bad options leaves a hidden, empty host behind (only the batcher judges options, and it is asked through that host).

**Red before the change:** the tests do not compile. Against an edge build without the refusal, the edge arm sees success.

## Decisions for you
1. **Should one subscriber object hold both row-by-row and batched subscriptions, given that a row subscription and a batched one to the *same topic* through one provider collide?** On the in-process transport the later one silently replaces the earlier, which stops receiving. On Fast DDS the later one is refused, "already subscribed".
   (a) One object, collision documented. *Users:* nothing new to manage, and destroying the subscriber ends both kinds. Same-topic row + batched on it collides as above, which a user may not expect inside one object. *Architecture:* the server build only adds one call (L13, L12), and the shim forwards the transport's rule rather than answering it (L4).
   (b) A separate batch-subscriber object. *Users:* the same collision, now between two objects, where it already exists today for any two subscribers on one provider; there are two kinds of subscriber to manage. *Architecture:* about four more calls every build must carry and the edge must refuse (L12), over the surface budget.
   (c) One object, and the shim refuses a same-topic mix on it with a clear error. *Users:* one predictable answer on every transport for that object; another object on the same provider still collides. *Architecture:* a new rule the shim answers itself (an L4 exception), with per-topic bookkeeping.
   **Recommendation:** (a). It serves the round purpose (batches through the one batcher, edge unchanged), keeps the collision the transport's existing rule, now documented, and registers the provider difference for domains 3/4. **Default:** (a).
2. **Until stage 6, what happens to a batch the shim fails to convert (e.g. out of memory)?**
   (a) Accept the gap in stages 4–5: those rows vanish uncounted. *Users:* no one is exposed; nothing ships mid-round, and C# does not call this before a later round. *Architecture:* L18 holds at round close and keeps its red-first test in stage 6.
   (b) Add the report now, without its test. *Users:* the same end behaviour, earlier. *Architecture:* the code lands two stages before its proof, so stage 6 cannot show it red first.
   **Recommendation:** (a). The ruling "Report it as lost" is met by the round, and each law keeps one test. **Default:** (a).

## Risks accepted / debt carried
- **Linux link (pending your ruling, asked by the PM):** the design assumes the two batcher-side recipes gain the same "position-independent" switch Fletcher's pub/sub recipe has. If you refuse, the fallback sets it in the Linux build profile only, which is weaker: any other Linux profile fails to link the server shim.
- Between 4a and 4b, batches arrive without their rows' attachments; nothing ships in between. The server tests wait one-sided (timeout of at least 1 s), so a loaded runner can flake only the reason assertion. Dictionary columns arrive in the batcher's form (d9 Q11, open), so the tests avoid them.
- **Premises (stop and ask if false):** Arrow's symbols stay hidden in the server build; `INT64_MAX` ms is exactly "no timeout"; the batcher's refusal keeps its error type. Evidence: Windows by hand. Linux and CI wait for the owner's draft PR.

## Numbers
**4a:** declared net lines (raw diff) about **+635 / −40**. Production is +298/−12 in 11 files, of which about 236 count toward the 250 ceiling (`binding.h` doc comments are not counted). Tests about +270; docs about +65/−25. **4b:** production about +20 in 2 files and tests about +60, a light-lane candidate. Unsplit, the stage would count about 254, so the split is taken now. New public surface: 3. Design cycles used: 2/2.

---
*As landed (<date>, appended by the PM at close, ≤5 lines):*
<delta vs the above — actual net lines, anything retired or added the brief did not predict, fix cycles used>.
