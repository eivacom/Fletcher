# D10C-7 — Stage Brief (2026-11-16)

**In one sentence:** a C# app with the server package gets its batches from the C++ batcher, an app on the small shim gets a clear
"add Eiva.Fletcher.Server" refusal, and under Decision 1 (d) C#'s own batcher is deleted in this stage.
**Forcing test:** batched tests on both shims: server, batches one at a time, in order, on one thread, exactly one closing delivery
(empty when idle), re-folded dictionaries, attachments; small shim, the named refusal and nothing delivered.

## Interfaces
| Surface | Change | Why |
|---|---|---|
| `SubscriberArrow.SubscribeBatched` | CHANGED | Server shim: windows closed by the C++ batcher, handed over as batches the handler owns and may keep. Small shim: named refusal. |
| its options | CHANGED | Passed as given: a row limit below 1 or a negative timeout is now the C++ batcher's "invalid argument" error (not clamped, not `ArgumentOutOfRange`); a timeout past 49.7 days is accepted. |
| `Unsubscribe`, `Dispose`, a subscription's `Dispose` | CHANGED | From outside a handler they return after the closing delivery; from inside the handler they return at once and the closing delivery follows it. |
| same-topic refusal | CHANGED | After an unsubscribe made inside a row delivery, the other kind stays refused on that topic until the subscriber is disposed (the network subscription is still open then). |
| `HandlerFaulted.SubscriptionId` | CHANGED | Batched ids are now the shim's (very large numbers); 0 for a fault in a batch that arrives before the subscribe call returns. No new public surface; per-row, publishing and decoding untouched. |

## Deleted
- Five tests the laws forbid (dictionaries as plain values, the 49.7-day refusal, overlapping batches, two C#-only window sizes;
  D10C-1 ported their cases to C++), and C#'s own option checks. C#'s batcher: under (d) deleted here with a sixth test (its ceiling
  constant); otherwise unreachable (no option, build or fallback leads to it: dead code, not a bridge) until D10C-8 deletes it.

## Corner cases forbidden vs handled
**Forbidden:** a refused subscribe that delivers; a batch freed early, twice, or kept on memory the shim reclaims; two handler calls at
once or out of order; one kind silently replacing the other; a negative timeout as zero; the package sentence on another refusal.
**Handled:** if C# itself fails to take a batch over (out of memory), the handler gets "no batch" with that window's rows counted as lost.

## Decisions for you
1. **The switch is near the size ceiling. Does C#'s own batcher go in the same stage, and if not, where is the switch cut?**
   (d) **Merge D10C-8 in:** switch, CI lane and deleting C#'s batcher in one stage, net about −160 to −310. *Users:* nothing ships
   mid-round; C#'s copy is gone when batches start coming from C++; no red test, no dead code. *Architecture:* the purpose's "C#'s own
   copy is gone" (:566) and L4 (no window, count, timer or batch thread in C#) land with the switch; D10C-8 moves in whole (the sixth
   forbidden test, BC-9's narrowing, its forcing scans of the packed assemblies and the source). Cost: one review of ~300 lines written
   and ~600 deleted, mostly one file; the round drops to 10 stages.
   (a) **Two stages, batcher kept:** all C# first, then the CI run proving a Fast DDS reader profile reaches a batched subscription.
   *Users:* nothing ships; no red test. *Architecture:* the switch lands whole, but C#'s copy stays in source, unreachable, until D10C-8 (L4).
   (b) **The ladder's cut, attachments second.** *Users:* nothing ships; four tests red for one stage. *Architecture:* saves ~10 lines
   (attachments reuse D10C-3's copy), so it does not relieve the ceiling: a red window for no gain.
   (c) **One stage, switch only, about +255 to +305 net.** *Users:* none. *Architecture:* over the 250 ceiling; :643 accepted D10C-5's
   overrun but kept 250 for later stages, so this needs a new exception.
   **Recommendation:** (d): it serves the round purpose better, since "C#'s own copy is gone" becomes true in the stage where batches
   start coming from C++, with no dead-code window. Its only cost is review size, which breaks no tie here. **Default:** (d).

## Risks accepted / debt carried
- Dictionary import checked first: if Apache.Arrow cannot take C++'s column as-is, work stops for you (C# never converts it). The
  preview-bundle consumer's batched path stays red until D10C-11 (:650 (2)).
- **Verification:** dotnet suite, edge and server legs (Windows); binding-abi-conformance, both passes; pubsub-transport-conformance
  with its server pass; Linux devcontainer (`fletcher-build:latest`); under (d) the packed-assembly scan.

## Numbers
Declared net lines: switch (7a) **+215 to +245** (reviewer's pricing, agreed) · CI lane +40 to +60 · under (d) whole stage about −160 to
−310 · new public surface: 0 · design cycles used: 1/2. Counted: code and `//` comments (the `SubscriberArrow` file header's rewrite
counts both ways); `///` doc comments, Markdown and domain-doc marks are listed, not counted (ladder :14, as D10C-3).

---
*As landed (<date>, appended by the PM at close, ≤5 lines):*
